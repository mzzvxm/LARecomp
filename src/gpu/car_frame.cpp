// MCLA GPU renderer -- the player car's draws of a frame. See car_frame.h.

#include "car_frame.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_car_frame_log, false, "MCLA/GPU",
    "Debug: logs one line under [car frame] whenever the player's car changes in the draw list the car effects share (its "
    "stencil ID, how many draws it has, how many are rigid parts and how many skinned): what was found, how it was found and what "
    "was left out. With this on the list is collected in every frame, whether a car effect (the car shadow map, the car ray tracing) "
    "asks for it or not, so the line shows by itself. Never saved. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[car frame]";
// Draws held before the player's ID is known (the ones of every ID): the player's car is the first one drawn in nearly every frame,
// so this is for the rare frame it is not (and for a scene where no draw is near the vehicle: the garage, the pause view).
constexpr size_t kPendingCap = 64;
// Meshes read for their model-space box in one frame (a read is the whole mesh's indices and vertices: the car's ~200 draws are read
// over a few frames after it appears, then never again while their keys are seen).
constexpr uint32_t kBoxBudget = 32;
// Frames a box is kept after its key was last seen.
constexpr uint64_t kBoxKeepFrames = 900;
// How often the box cache is swept.
constexpr uint64_t kBoxSweepFrames = 128;
// Frames before a vertex shader whose microcode could not be verified is tried again.
constexpr uint32_t kRecipeRetryFrames = 300;
// Draws a frame may make a camera from before giving up on it (a vertex bank whose c12-c15 are no view inverse).
constexpr uint32_t kCameraAttempts = 8;
// Meshes whose failure was logged (once each).
constexpr size_t kUnreadableLogged = 64;
// A cache of palettes of skinned draws kept for reuse.
constexpr size_t kPalettePool = 64;

void NormalizeAll(std::vector<float>* v) {
  for (size_t i = 0; i + 2 < v->size(); i += 3) {
    float* n = v->data() + i;
    const double length = std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
    if (std::isfinite(length) && length > 1e-12) {
      const double inverse = 1.0 / length;
      n[0] = float(n[0] * inverse);
      n[1] = float(n[1] * inverse);
      n[2] = float(n[2] * inverse);
    }
  }
}

}  // namespace

CarFrame::~CarFrame() = default;

void CarFrame::Initialize(GuestMemory& memory) {
  memory_ = &memory;
  reader_.owner = this;
  requested_ = false;
  collecting_ = false;
  guest_frame_ = 0;
  previous_id_ = 0;
  logged_any_ = false;
  StartFrame();
}

void CarFrame::Shutdown() {
  ReleaseDraws(frame_.draws);
  ReleaseDraws(pending_);
  frame_ = car_frame::Frame{};
  listeners_.clear();
  recipes_.clear();
  layouts_.clear();
  boxes_.clear();
  reads_.Clear();
  unreadable_logged_.clear();
  palette_pool_.clear();
  palette_cache_.clear();
  palette_cache_banks_ = nullptr;
  ctx_ = Context{};
  reader_.owner = nullptr;
  memory_ = nullptr;
  requested_ = false;
  collecting_ = false;
}

void CarFrame::AddListener(car_frame::Listener* listener) {
  if (listener && std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end()) {
    listeners_.push_back(listener);
  }
}

// ---------------------------------------------------------------------------
// the frame's boundaries
// ---------------------------------------------------------------------------

void CarFrame::BeginFrame() {
  // A request made since the last present (any time in the frame before) starts the collecting here at the latest: the frame's first
  // draw has not been issued yet.
  if (!collecting_ && REXCVAR_GET(mcla_gpu_car_frame_log)) {
    requested_ = true;  // the debug line asks for the list itself
  }
  if (requested_ && memory_ && !collecting_) {
    collecting_ = true;
    requested_ = false;
    StartFrame();
  }
}

void CarFrame::OnPresent() {
  const bool was_collecting = collecting_;
  if (was_collecting) {
    FinishFrame();
  }
  ++guest_frame_;
  if (REXCVAR_GET(mcla_gpu_car_frame_log)) {
    requested_ = true;  // the debug line asks for the list itself
  }
  collecting_ = requested_ && memory_ != nullptr;
  requested_ = false;
  if (collecting_ || was_collecting) {
    StartFrame();  // the Frame is cleared at the guest present, also when nothing asks for the next one
  }
  if (guest_frame_ % kBoxSweepFrames == 0) {
    for (auto it = boxes_.begin(); it != boxes_.end();) {
      if (car_frame_rules::Stale(it->second.seen, guest_frame_, kBoxKeepFrames)) {
        it = boxes_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void CarFrame::StartFrame() {
  ReleaseDraws(frame_.draws);
  ReleaseDraws(pending_);
  frame_.valid = false;
  frame_.guest_frame = guest_frame_;
  frame_.player_id = 0;
  frame_.player_world = temporal::Mat4{};
  frame_.view = temporal::Mat4{};
  frame_.view_projection = temporal::Mat4{};
  for (int c = 0; c < 3; ++c) {
    frame_.to_sun[c] = 0.0;
    frame_.bounds_min[c] = 0.0;
    frame_.bounds_max[c] = 0.0;
  }
  frame_.bounds_valid = false;
  tracker_.BeginFrame(previous_id_);
  player_read_ = false;
  player_.valid = false;
  main_draws_ = 0;
  world_draws_ = 0;
  camera_done_ = false;
  camera_attempts_ = 0;
  boxes_read_ = 0;
  bounds_missing_ = 0;
  world_box_ = car_frame_rules::Box3{};
  counts_ = Counts{};
  log_ = REXCVAR_GET(mcla_gpu_car_frame_log);
  ctx_ = Context{};
}

void CarFrame::FinishFrame() {
  const uint32_t before = tracker_.previous();  // the ID the frame started from
  previous_id_ = tracker_.confirmed();
  if (!log_) {
    return;
  }
  Summary now;
  now.valid = frame_.valid;
  now.id = frame_.player_id;
  now.draws = uint32_t(frame_.draws.size());
  uint32_t after_world = 0;
  for (const car_frame::Draw& d : frame_.draws) {
    now.body += d.part == car_frame::Part::kBody;
    now.skinned += d.part == car_frame::Part::kSkinned;
    after_world += d.after_world;
  }
  if (logged_any_ && now == logged_) {
    return;
  }
  logged_any_ = true;
  logged_ = now;
  if (!now.valid) {
    REXLOG_INFO("{} frame {}: no player's car in the draw list ({} vehicle draws: {} with no player's vehicle to match, {} wheel proxies, "
                "{} of other cars, {} held back and not matched); player's vehicle: {}",
                kPrefix, guest_frame_, counts_.vehicle_draws, counts_.no_player, counts_.proxies, counts_.other_cars, pending_.size(),
                player_.note.empty() ? std::string("not read") : player_.note);
    return;
  }
  uint32_t with_key = 0, with_box = 0, deferred = 0;
  for (const car_frame::Draw& d : frame_.draws) {
    with_key += d.mesh_key != 0;
    with_box += d.has_box;
    deferred += d.box_deferred;
  }
  const double* t = frame_.player_world.m[3];
  REXLOG_INFO("{} frame {}: the player's car is stencil ID {} (the frame before: {}; its gWorld is {:.2f} m from the vehicle the game "
              "says the player drives, at ({:.2f}, {:.2f}, {:.2f})): {} draws ({} body, {} skinned, {} after the first world draw), "
              "{} with a mesh key, {} with a model box ({} to come; world box {}), camera {}, sun {}; left out: {} wheel proxies, {} with "
              "no triangles, {} with a matrix that is not a number, {} of other cars; reads: {} meshes for their box, {} for listeners; "
              "player's vehicle: {}",
              kPrefix, guest_frame_, now.id, before, tracker_.best(now.id), t[0], t[1], t[2], now.draws, now.body, now.skinned,
              after_world, with_key, with_box, deferred,
              frame_.bounds_valid
                  ? std::format("({:.2f}, {:.2f}, {:.2f}) to ({:.2f}, {:.2f}, {:.2f})", frame_.bounds_min[0], frame_.bounds_min[1],
                                frame_.bounds_min[2], frame_.bounds_max[0], frame_.bounds_max[1], frame_.bounds_max[2])
                  : std::string("not valid"),
              camera_done_ ? "read" : "NOT read (no vertex bank held a view inverse)",
              (frame_.to_sun[0] != 0.0 || frame_.to_sun[1] != 0.0 || frame_.to_sun[2] != 0.0)
                  ? std::format("({:.3f}, {:.3f}, {:.3f})", frame_.to_sun[0], frame_.to_sun[1], frame_.to_sun[2])
                  : std::string("none"),
              counts_.proxies, counts_.no_triangles, counts_.not_finite, counts_.other_cars, counts_.boxes_read, counts_.listener_reads,
              player_.note.empty() ? std::string("not read") : player_.note);
}

// ---------------------------------------------------------------------------
// the draws
// ---------------------------------------------------------------------------

// CHECK: the pause view's world (4 guest samples) and the garage draw the car once per tile, and a rear-view mirror may draw the vehicles into
// the main keys: the Frame would then list the car's draws more than once (the pause view) or twice with another camera (the mirror). Not
// told apart here; the log line's draw count (about 210 for the parked R34 of the car dump, without the proxies) shows it.
void CarFrame::OnMainDraw(const MainDraw& d) {
  if (!collecting_ || !d.view || !d.banks || !d.state) {
    return;
  }
  const uint32_t order = main_draws_++;
  const uint32_t mask = d.stencil_ids & ~1u & ((1u << car_dump::kIdSlots) - 1u);
  if (mask == 0) {
    ++world_draws_;
    return;
  }
  OnVehicleDraw(d, order, uint32_t(std::countr_zero(mask)));
}

void CarFrame::OnVehicleDraw(const MainDraw& d, uint32_t order, uint32_t id) {
  HandleVehicleDraw(d, order, id);
  ctx_ = Context{};
  SyncPlayer();
}

// Whether the draw has triangles that car_mesh::ReadTriangles can be expected to read: no computed fetch, a primitive that is drawn as
// triangles. (The ones that only guest memory tells, an unreadable range or a stream with no vertices, show in ReadTriangles.)
bool CarFrame::HasTriangles(const MainDraw& d) const {
  const DrawRecord& r = d.view->record();
  if ((r.flags & kDrawVsComputedFetchIndex) != 0 || (d.vs && (d.vs->flags & ShaderStore::kComputedFetchIndex) != 0)) {
    return false;
  }
  if (d.primitive.conversion == PrimitiveConversion::kUnsupported || d.primitive.conversion == PrimitiveConversion::kRectList) {
    return false;
  }
  return d.primitive.topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST ||
         d.primitive.topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
}

void CarFrame::HandleVehicleDraw(const MainDraw& d, uint32_t order, uint32_t id) {
  ++counts_.vehicle_draws;
  if (!player_read_) {
    player_read_ = true;  // the guest is held back inside this call: the player's vehicle is read here, once a frame
    car_mesh::ReadPlayerVehicle(&player_, log_);
  }
  if (!player_.valid) {
    ++counts_.no_player;
    return;
  }
  const DrawRecord& r = d.view->record();
  // The wheel proxies: a stencil-only draw per wheel with an ID of its own, at the wheel: never the car, and never a match.
  uint32_t colour_mask = 0;
  for (uint32_t t = 0; t < kColorTargetCount; ++t) {
    if (d.state->color[t].bound) {
      colour_mask |= d.state->color[t].write_mask & 0xFu;
    }
  }
  if (car_dump::WheelProxyOf(r.vs_identity, r.ps_identity, colour_mask) != car_dump::ProxyWhy::kNo) {
    ++counts_.proxies;
    return;
  }

  // Whose draw is it: gWorld's translation against the player's vehicle (a few constant reads).
  const uint8_t* bank = d.banks->vs.data();
  double translation[3];
  const bool finite = car_frame_rules::WorldTranslation(bank, translation);
  const car_frame_rules::IdentityTracker::Verdict verdict = tracker_.OnDraw(id, finite ? translation : nullptr, player_.position);
  if (verdict.improved) {
    best_world_[id] = car_dump::BankMatrix(bank, car_dump::kWorldRegister);
  }
  if (verdict.confirmed_now || verdict.switched) {
    OnConfirmed(id, verdict.switched);
  }
  const bool assumed = verdict.standing != car_frame_rules::IdentityTracker::Standing::kOther;
  const bool confirmed = tracker_.confirmed() != 0;
  if (!assumed) {
    if (confirmed) {
      ++counts_.other_cars;  // another car's: nothing more is done with it
      return;
    }
    if (pending_.size() >= kPendingCap) {
      ++counts_.pending_full;
      return;
    }
  }

  if (!d.input_layout || !d.geometry || !HasTriangles(d)) {
    ++counts_.no_triangles;
    return;
  }
  const LayoutInfo& layout = LayoutOf(*d.input_layout);
  if (!layout.position) {
    ++counts_.no_triangles;
    return;
  }
  const RecipeEntry& recipe = RecipeOf(d);
  const car_frame_rules::ModelDecode model = car_frame_rules::DecodeModel(bank, recipe.choice.recipe);
  if (!model.finite) {
    ++counts_.not_finite;
    return;
  }
  if (!camera_done_ && camera_attempts_ < kCameraAttempts && recipe.choice.recipe != car_dump::Recipe::kSkinned) {
    ++camera_attempts_;
    NoteCamera(bank, model.world);
  }

  // The draw joins the Frame when the ID is confirmed, else the held-back list.
  std::vector<car_frame::Draw>& target = confirmed ? frame_.draws : pending_;
  target.emplace_back();
  car_frame::Draw& out = target.back();
  FillDraw(d, order, id, recipe, layout, model, &out);
  if (confirmed) {
    AddToBounds(out);
  }
  if (assumed) {
    CallListeners();
  }
}

// The frame's first confirmed ID (or a nearer one than the confirmed): the held-back draws of that ID are the player's, in order.
void CarFrame::OnConfirmed(uint32_t id, bool switched) {
  if (switched) {
    // What was listed belonged to the other ID. (Its own earlier draws, skipped when the other one was confirmed, are lost: a frame
    // with two cars nearer than 5 m to the vehicle and the wrong one first is rare, and the next frame starts from this ID.)
    ReleaseDraws(frame_.draws);
    world_box_ = car_frame_rules::Box3{};
    bounds_missing_ = 0;
  }
  for (car_frame::Draw& held : pending_) {
    if (held.stencil_id == id) {
      frame_.draws.push_back(std::move(held));
      AddToBounds(frame_.draws.back());
    } else if (!held.palette.empty() && palette_pool_.size() < kPalettePool) {
      held.palette.clear();
      palette_pool_.push_back(std::move(held.palette));
    }
  }
  pending_.clear();
}

// The Frame's own fields that follow from what is known so far, kept current after every vehicle draw (the trigger reads them
// without a call of its own).
void CarFrame::SyncPlayer() {
  const uint32_t id = tracker_.confirmed();
  frame_.player_id = id;
  frame_.valid = id != 0 && !frame_.draws.empty();
  if (id != 0) {
    frame_.player_world = best_world_[id];
  }
  const bool bounds = bounds_missing_ == 0 && !world_box_.Empty();
  frame_.bounds_valid = bounds;
  if (bounds) {
    for (int c = 0; c < 3; ++c) {
      frame_.bounds_min[c] = world_box_.lo[c];
      frame_.bounds_max[c] = world_box_.hi[c];
    }
  }
}

void CarFrame::ReleaseDraws(std::vector<car_frame::Draw>& draws) {
  for (car_frame::Draw& d : draws) {
    if (d.palette.capacity() != 0 && palette_pool_.size() < kPalettePool) {
      d.palette.clear();
      palette_pool_.push_back(std::move(d.palette));
    }
  }
  draws.clear();
}

std::vector<float> CarFrame::TakePalette() {
  if (palette_pool_.empty()) {
    return {};
  }
  std::vector<float> palette = std::move(palette_pool_.back());
  palette_pool_.pop_back();
  return palette;
}

// ---------------------------------------------------------------------------
// the caches: layouts and recipes
// ---------------------------------------------------------------------------

const CarFrame::LayoutInfo& CarFrame::LayoutOf(const InputLayout& layout) {
  const auto found = layouts_.find(layout.id);
  if (found != layouts_.end()) {
    return found->second;
  }
  LayoutInfo info;
  const uint32_t real = uint32_t(layout.streams.size());  // the slots after these read the zero buffer
  bool have_indices = false, have_weights = false;
  for (const plume::RenderInputElement& e : layout.elements) {
    if (!e.semanticName || e.semanticIndex != 0 || e.slotIndex >= real || e.slotIndex >= layout.slots.size()) {
      continue;
    }
    const uint32_t stride = layout.slots[e.slotIndex].stride;
    const uint32_t stream = layout.streams[e.slotIndex].stream;
    if (std::strcmp(e.semanticName, "POSITION") == 0) {
      uint32_t bytes = 0;
      switch (e.format) {
        case plume::RenderFormat::R32G32B32_FLOAT: bytes = 12; break;
        case plume::RenderFormat::R32G32B32A32_FLOAT: bytes = 16; break;
        case plume::RenderFormat::R16G16B16A16_FLOAT: bytes = 8; break;
        default: break;
      }
      if (!info.position && bytes != 0 && e.alignedByteOffset + bytes <= stride) {
        info.position = true;
        info.position_stream = stream;
        info.position_slot = e.slotIndex;
        info.position_offset = e.alignedByteOffset;
        info.position_stride = stride;
        info.position_format = e.format;
      }
    } else if (std::strcmp(e.semanticName, "NORMAL") == 0) {
      if (!info.normal) {
        info.normal = true;
        info.normal_stream = stream;
        info.normal_offset = e.alignedByteOffset;
        info.normal_format = e.format;
      }
    } else if (std::strcmp(e.semanticName, "BLENDINDICES") == 0) {
      if (!have_indices) {
        have_indices = true;
        info.blend_indices_slot = e.slotIndex;
        info.blend_indices_offset = e.alignedByteOffset;
        info.blend_indices_format = e.format;
      }
    } else if (std::strcmp(e.semanticName, "BLENDWEIGHT") == 0) {
      if (!have_weights) {
        have_weights = true;
        info.blend_weights_slot = e.slotIndex;
        info.blend_weights_offset = e.alignedByteOffset;
        info.blend_weights_format = e.format;
      }
    }
  }
  info.blend = have_indices && have_weights;
  // CHECK: a position that is not float3, float4 or half4 (a float2 or half2 position has no z) leaves the draw out; the car dump saw only
  // float3/float4 (the note's section 3): a run of the log shows whether any listed draw of the car is left out for this.
  return layouts_.emplace(layout.id, info).first->second;
}

const CarFrame::RecipeEntry& CarFrame::RecipeOf(const MainDraw& d) {
  const DrawRecord& r = d.view->record();
  const auto found = recipes_.find(r.vs_identity);
  if (found != recipes_.end() && (found->second.verified || guest_frame_ - found->second.checked < kRecipeRetryFrames)) {
    return found->second;
  }
  RecipeEntry entry;
  const car_mesh::ShaderReads& reads = reads_.Of(r, guest_frame_, kRecipeRetryFrames);
  entry.verified = reads.verified;
  if (d.shaders && d.vs) {
    d.shaders->GetVertexElements(*d.vs, elements_);
    for (const ShaderStore::VertexElement& e : elements_) {
      entry.skinned_declared |= e.usage == car_mesh::kUsageBlendIndices;
    }
  }
  entry.choice = car_dump::ChooseRecipe(reads.reads, entry.skinned_declared, r.vs_identity);
  entry.checked = guest_frame_;
  return recipes_.insert_or_assign(r.vs_identity, entry).first->second;
}

// ---------------------------------------------------------------------------
// one draw
// ---------------------------------------------------------------------------

void CarFrame::FillDraw(const MainDraw& d, uint32_t order, uint32_t id, const RecipeEntry& recipe, const LayoutInfo& layout,
                        const car_frame_rules::ModelDecode& model, car_frame::Draw* out) {
  const DrawRecord& r = d.view->record();
  const bool skinned = recipe.choice.recipe == car_dump::Recipe::kSkinned;
  out->part = skinned ? car_frame::Part::kSkinned : car_frame::Part::kBody;
  out->stencil_id = id;
  out->vs = r.vs_identity;
  out->ps = r.ps_identity;
  out->order = order;
  out->after_world = world_draws_ != 0;
  out->model_to_world = model.model;  // a skinned draw: gWorld alone (DecodeModel)
  out->geometry = *d.geometry;
  out->position_slot = layout.position_slot;
  out->position_offset = layout.position_offset;
  out->position_stride = layout.position_stride;
  out->position_format = layout.position_format;
  if (skinned) {
    if (layout.blend) {
      out->blend_indices_slot = layout.blend_indices_slot;
      out->blend_indices_offset = layout.blend_indices_offset;
      out->blend_indices_format = layout.blend_indices_format;
      out->blend_weights_slot = layout.blend_weights_slot;
      out->blend_weights_offset = layout.blend_weights_offset;
      out->blend_weights_format = layout.blend_weights_format;
      out->palette = TakePalette();
      FillPalette(*d.banks, &out->palette);
    }
  }

  // The triangles are read from here on (the box, the listeners): this draw's call.
  ctx_.draw_in = &d;
  ctx_.draw = out;
  ctx_.active = true;
  mesh_loaded_ = false;
  if (!skinned) {
    out->mesh_key = ComputeMeshKey(d, layout);
    NoteBox(out);
  }
}

// The palette as the vertex shader reads it: c64..c255, 4 floats a register. A skinned draw's bank is often the one of the draw before
// (the driver's draws share a palette): the copy then comes from the last one, by the bank's version.
void CarFrame::FillPalette(const ConstantBanks::Banks& banks, std::vector<float>* out) {
  if (palette_cache_banks_ == &banks && palette_cache_version_ == banks.vs_version && !palette_cache_.empty()) {
    *out = palette_cache_;
    return;
  }
  constexpr uint32_t kRegisters = car_dump::kRegisterCount - car_dump::kPaletteRegister;
  out->resize(size_t(kRegisters) * 4);
  const uint8_t* bank = banks.vs.data();
  for (uint32_t reg = 0; reg < kRegisters; ++reg) {
    for (uint32_t c = 0; c < 4; ++c) {
      (*out)[size_t(reg) * 4 + c] = car_dump::BankFloat(bank, car_dump::kPaletteRegister + reg, c);
    }
  }
  palette_cache_ = *out;
  palette_cache_banks_ = &banks;
  palette_cache_version_ = banks.vs_version;
}

// The mesh key of a rigid draw: where its position stream, its index range and its declaration are, what it draws from them, and the
// geometry cache's stamp of the two guest ranges (car_frame_rules.h MeshKeyOf).
uint64_t CarFrame::ComputeMeshKey(const MainDraw& d, const LayoutInfo& layout) const {
  const DrawRecord& r = d.view->record();
  if (r.kind == DrawKind::kInline) {
    return 0;  // the vertices are in the draw's own record
  }
  const car_mesh::StreamRef position = car_mesh::ResolveStream(*d.view, r, layout.position_stream);
  if (!position.ok) {
    return 0;
  }
  const auto stamp = [&](uint32_t address, uint32_t size) -> uint64_t { return d.cache ? d.cache->RangeStamp(address, size) : 0; };
  car_frame_rules::MeshKeyParts parts;
  parts.position = car_frame_rules::StreamKey{position.address, position.size, position.stride, layout.position_offset,
                                              uint32_t(layout.position_format), stamp(position.address, position.size)};
  if (layout.normal) {
    if (layout.normal_stream != layout.position_stream) {
      const car_mesh::StreamRef normal = car_mesh::ResolveStream(*d.view, r, layout.normal_stream);
      if (!normal.ok) {
        return 0;
      }
      parts.normal = car_frame_rules::StreamKey{normal.address, normal.size, normal.stride, layout.normal_offset,
                                                uint32_t(layout.normal_format), stamp(normal.address, normal.size)};
    } else {
      parts.normal.offset = layout.normal_offset;
      parts.normal.format = uint32_t(layout.normal_format);
    }
  }
  parts.indexed = r.kind == DrawKind::kIndexed ? 1u : 0u;
  parts.primitive = r.primitive;
  parts.count = r.count;
  parts.start = r.start;
  parts.base_vertex = r.base_vertex;
  if (r.kind == DrawKind::kIndexed) {
    const bool index32 = (r.index_dword0 & 0x80000000u) != 0;
    const uint32_t size = index32 ? 4u : 2u;
    parts.index_address = GuestMemory::ToPhysical(r.index_address) + r.start * size;
    parts.index_bytes = r.count * size;
    parts.index32 = index32 ? 1u : 0u;
    parts.endian = (r.index_dword0 >> 29) & 3u;
    if (d.primitive.conversion == PrimitiveConversion::kNone) {
      parts.index_stamp = stamp(parts.index_address, parts.index_bytes);  // the cache holds a region of the range only then
    }
    if (d.primitive.topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP) {
      parts.reset = d.state->sc_mode.multi_prim_ib_ena != 0 ? 1u : 0u;
      parts.reset_index = d.state->reset_index & (index32 ? 0xFFFFFFFFu : 0xFFFFu);
    }
  }
  return car_frame_rules::MeshKeyOf(parts);
}

// ---------------------------------------------------------------------------
// the camera and the bounds
// ---------------------------------------------------------------------------

void CarFrame::NoteCamera(const uint8_t* vs_bank, const temporal::Mat4& world) {
  const temporal::Mat4 wvp = car_dump::BankMatrix(vs_bank, car_dump::kWvpRegister);
  const car_frame_rules::BankCamera camera = car_frame_rules::CameraFromBank(vs_bank, world, wvp);
  if (!camera.valid) {
    return;
  }
  camera_done_ = true;
  frame_.view = camera.view;
  frame_.view_projection = camera.view_projection;
  for (int c = 0; c < 3; ++c) {
    frame_.to_sun[c] = camera.sun_valid ? camera.to_sun[c] : 0.0;
  }
}

// The model-space box of a rigid draw's mesh, from the cache by key; a key not seen yet is read (the whole mesh: the draw's call is
// the only time the guest's bytes are held still), at most a few a frame.
void CarFrame::NoteBox(car_frame::Draw* out) {
  if (out->mesh_key == 0) {
    return;
  }
  const auto found = boxes_.find(out->mesh_key);
  if (found != boxes_.end()) {
    found->second.seen = guest_frame_;
    if (found->second.valid) {
      out->has_box = true;
      std::memcpy(out->box_min, found->second.lo, sizeof(out->box_min));
      std::memcpy(out->box_max, found->second.hi, sizeof(out->box_max));
    }
    return;
  }
  if (boxes_read_ >= kBoxBudget) {
    out->box_deferred = true;
    return;
  }
  ++boxes_read_;
  ++counts_.boxes_read;
  MeshBox box;
  box.seen = guest_frame_;
  if (LoadMesh()) {
    box.valid = car_frame_rules::BoxOfMesh(mesh_.local.data(), mesh_.local.size() / 3, mesh_.tri.data(), mesh_.tri.size(), box.lo, box.hi);
  }
  boxes_.emplace(out->mesh_key, box);
  if (box.valid) {
    out->has_box = true;
    std::memcpy(out->box_min, box.lo, sizeof(out->box_min));
    std::memcpy(out->box_max, box.hi, sizeof(out->box_max));
  }
}

// A listed rigid draw's box joins the car's world box; a draw whose box is only deferred keeps the box invalid for this frame (a draw
// that cannot be read has nothing to bound).
void CarFrame::AddToBounds(const car_frame::Draw& draw) {
  if (draw.part != car_frame::Part::kBody) {
    return;
  }
  if (draw.box_deferred) {
    ++bounds_missing_;
    return;
  }
  if (draw.has_box) {
    car_frame_rules::AddTransformedBox(&world_box_, draw.model_to_world, draw.box_min, draw.box_max);
  }
}

// ---------------------------------------------------------------------------
// the listeners and the triangles
// ---------------------------------------------------------------------------

void CarFrame::CallListeners() {
  for (car_frame::Listener* listener : listeners_) {
    listener->OnPlayerDraw(*ctx_.draw, reader_);
  }
}

bool CarFrame::Reader::ReadTriangles(std::vector<float>* positions, std::vector<uint32_t>* indices, std::vector<float>* normals) const {
  if (!owner) {
    if (positions) {
      positions->clear();
    }
    if (indices) {
      indices->clear();
    }
    if (normals) {
      normals->clear();
    }
    return false;
  }
  return owner->ReadForListener(positions, indices, normals);
}

// The current draw's triangles, once per draw (the box and every listener share the read).
bool CarFrame::LoadMesh() {
  if (mesh_loaded_) {
    return mesh_.problem == car_mesh::Problem::kNone;
  }
  mesh_loaded_ = true;
  if (!ctx_.active || !ctx_.draw_in || !memory_) {
    mesh_.Reset();
    mesh_.problem = car_mesh::Problem::kUnreadable;
    return false;
  }
  car_mesh::DrawInputs inputs;
  inputs.view = ctx_.draw_in->view;
  inputs.state = ctx_.draw_in->state;
  inputs.vs = ctx_.draw_in->vs;
  inputs.primitive = ctx_.draw_in->primitive;
  car_mesh::ReadTriangles(*memory_, inputs, scratch_, &mesh_);
  if (mesh_.problem != car_mesh::Problem::kNone) {
    LogUnreadable(car_mesh::ProblemName(mesh_.problem));
    return false;
  }
  return true;
}

void CarFrame::LogUnreadable(const char* what) {
  if (!ctx_.draw || unreadable_logged_.size() >= kUnreadableLogged) {
    return;
  }
  const uint64_t tag = ctx_.draw->mesh_key != 0 ? ctx_.draw->mesh_key : (ctx_.draw->vs * 31u + ctx_.draw->ps);
  if (!unreadable_logged_.insert(tag).second) {
    return;
  }
  REXLOG_WARN("{} the triangles of a draw could not be read: {} (vs {:016x}, ps {:016x}, mesh key {:016x}, stencil ID {}); logged once "
              "per mesh",
              kPrefix, what, ctx_.draw->vs, ctx_.draw->ps, ctx_.draw->mesh_key, ctx_.draw->stencil_id);
}

bool CarFrame::ReadForListener(std::vector<float>* positions, std::vector<uint32_t>* indices, std::vector<float>* normals) {
  if (positions) {
    positions->clear();
  }
  if (indices) {
    indices->clear();
  }
  if (normals) {
    normals->clear();
  }
  if (!ctx_.active || !ctx_.draw || !ctx_.draw_in || !memory_ || !positions || !indices) {
    return false;
  }
  ++counts_.listener_reads;
  if (!LoadMesh()) {
    return false;
  }
  const car_frame::Draw& draw = *ctx_.draw;
  const uint32_t first = mesh_.first_vertex;
  const uint32_t span = uint32_t(mesh_.local.size() / 3);
  indices->assign(mesh_.tri.begin(), mesh_.tri.end());
  if (draw.part == car_frame::Part::kBody) {
    positions->assign(mesh_.local.begin(), mesh_.local.end());
    if (normals) {
      if (car_mesh::ReadNormals(*memory_, *ctx_.draw_in->view, first, span, normals)) {
        NormalizeAll(normals);
      }
    }
    return true;
  }

  // The driver: the bones blended as the vertex shader does, then gWorld: world space.
  car_dump::SkinStats stats;
  std::string note;
  const std::vector<float>* directions_in = nullptr;
  if (normals && car_mesh::ReadNormals(*memory_, *ctx_.draw_in->view, first, span, &skin_normals_in_)) {
    directions_in = &skin_normals_in_;
  }
  if (!car_mesh::SkinPositions(*memory_, *ctx_.draw_in->view, ctx_.draw_in->banks->vs.data(), first, span, mesh_.local, &skin_blended_,
                               &stats, &note, directions_in, directions_in ? &skin_normals_out_ : nullptr)) {
    LogUnreadable(note.c_str());
    positions->clear();
    indices->clear();
    return false;
  }
  const temporal::Mat4& world = draw.model_to_world;  // gWorld alone for a skinned draw
  positions->resize(size_t(span) * 3);
  for (uint32_t v = 0; v < span; ++v) {
    const double p[3] = {double(skin_blended_[size_t(v) * 3]), double(skin_blended_[size_t(v) * 3 + 1]),
                         double(skin_blended_[size_t(v) * 3 + 2])};
    double w[4];
    car_dump::TransformPoint(world, p, w);
    (*positions)[size_t(v) * 3] = float(w[0]);
    (*positions)[size_t(v) * 3 + 1] = float(w[1]);
    (*positions)[size_t(v) * 3 + 2] = float(w[2]);
  }
  if (directions_in) {
    normals->resize(size_t(span) * 3);
    for (uint32_t v = 0; v < span; ++v) {
      const double n[3] = {double(skin_normals_out_[size_t(v) * 3]), double(skin_normals_out_[size_t(v) * 3 + 1]),
                           double(skin_normals_out_[size_t(v) * 3 + 2])};
      for (int c = 0; c < 3; ++c) {
        (*normals)[size_t(v) * 3 + c] = float(n[0] * world.m[0][c] + n[1] * world.m[1][c] + n[2] * world.m[2][c]);
      }
    }
    NormalizeAll(normals);
  }
  return true;
}

}  // namespace mcla::gpu

// MCLA GPU renderer -- the player car's draws of a frame. See car_frame.h.

#include "car_frame.h"

#include "cpu_timing.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include "settings/larecomp_settings.h"
#include "xxhash_inline.h"
#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "guest_memory.h"
#include "textures.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[car frame]";
// Draws held before the player's ID is known (the ones of every ID): the player's car is the first one drawn in nearly every frame,
// so this is for the rare frame it is not (and for a scene where no draw is near the vehicle: the garage, the pause view).
constexpr size_t kPendingCap = 64;
// Meshes read for their model-space box in one frame (a read is the whole mesh's indices and vertices: the car's ~200 draws are read
// over a few frames after it appears, then never again while their keys are seen).
constexpr uint32_t kBoxBudget = 32;
// Frames a box is kept after its key was last seen (the fingerprints behind the keys are kept as long).
constexpr uint64_t kBoxKeepFrames = 900;
// Guest frames between two 'keys' log lines.
constexpr uint64_t kKeyLogFrames = 300;
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
  walk_.Shutdown();
  ReleaseDraws(frame_.draws);
  ReleaseDraws(pending_);
  frame_ = car_frame::Frame{};
  listeners_.clear();
  recipes_.clear();
  layouts_.clear();
  boxes_.clear();
  contents_.Clear();
  key_window_ = KeyWindow{};
  others_requested_ = false;
  others_on_ = false;
  other_limits_ = car_rt_instances::Limits{};
  other_limits_next_ = car_rt_instances::Limits{};
  selector_ = car_rt_instances::CarSelector{};
  scratch_draw_ = car_frame::Draw{};
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
  const bool walk = walk_.Wanted();  // also tells the guest-thread tap whether to note vehicles
  if (!collecting_ && (larecomp::settings::mcla_gpu_car_frame_log.Get() || walk)) {
    requested_ = true;  // the debug line (and the model walk's comparison) ask for the list themselves
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
  if (larecomp::settings::mcla_gpu_car_frame_log.Get() || walk_.Wanted()) {
    requested_ = true;  // the debug line (and the model walk's comparison) ask for the list themselves
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
    contents_.Sweep(guest_frame_, kBoxKeepFrames);
  }
}

void CarFrame::StartFrame() {
  ReleaseDraws(frame_.draws);
  ReleaseDraws(pending_);
  frame_.valid = false;
  frame_.guest_frame = guest_frame_;
  frame_.player_id = 0;
  frame_.player_world = temporal::Mat4{};
  frame_.vehicle_to_world = temporal::Mat4{};
  frame_.view = temporal::Mat4{};
  frame_.view_projection = temporal::Mat4{};
  for (int c = 0; c < 3; ++c) {
    frame_.to_sun[c] = 0.0;
    frame_.bounds_min[c] = 0.0;
    frame_.bounds_max[c] = 0.0;
  }
  frame_.bounds_valid = false;
  frame_.other_cars.clear();
  frame_.other_draws.clear();
  frame_.light = car_rt_material::FrameLight{};
  frame_.vehicles.clear();
  frame_.seen_cars.clear();
  frame_.vehicles_valid = false;
  frame_.player_vehicle_known = false;
  vehicles_read_ = false;
  vehicle_scan_ = car_mesh::VehicleScan{};
  // The other cars asked for in the frame before are collected in this one (a request is for the next frame, as the player's draws are).
  stencil_centres_on_ = std::exchange(stencil_centres_requested_, false) && collecting_;
  others_on_ = collecting_ && others_requested_ && other_limits_next_.On();
  other_limits_ = others_on_ ? other_limits_next_ : car_rt_instances::Limits{};
  others_requested_ = false;
  selector_.Begin(other_limits_);
  content_limit_ = 0;
  content_spent_ = 0;
  content_blocked_ = false;
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
  log_ = larecomp::settings::mcla_gpu_car_frame_log.Get();
  ctx_ = Context{};
  // The model walk's production for this frame: the other cars' limits of this frame (the scene asked in the frame before) and the mode it asked for.
  {
    car_model_walk::Production production;
    production.on = others_on_ && model_mode_next_ != car_model_rt::Mode::kOff;
    production.limits = other_limits_;
    production.mode = model_mode_next_;
    production.traffic_cars = traffic_cars_next_;
    production.wheels = wheels_next_;
    walk_.SetProduction(production);
    model_mode_next_ = car_model_rt::Mode::kOff;
    traffic_cars_next_ = 0;
    wheels_next_ = true;
  }
  walk_.StartFrame(guest_frame_, collecting_);
}

void CarFrame::FinishFrame() {
  const uint32_t before = tracker_.previous();  // the ID the frame started from
  previous_id_ = tracker_.confirmed();
  walk_.FinishFrame();  // the debug model walk's comparison with this frame's draws (a walk frame only)
  if (!log_) {
    return;
  }
  LogKeys();
  Summary now;
  now.valid = frame_.valid;
  now.id = frame_.player_id;
  now.draws = uint32_t(frame_.draws.size());
  now.other_cars = uint32_t(frame_.other_cars.size());
  now.other_draws = uint32_t(frame_.other_draws.size());
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
              "player's vehicle: {}; {}",
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
              player_.note.empty() ? std::string("not read") : player_.note, OthersText());
}

// The other cars of the frame for the log: the cars kept with their ids, distances and draws, and what became of the rest of their draws.
std::string CarFrame::OthersText() const {
  if (!others_on_) {
    return "other cars: not asked for";
  }
  std::string cars;
  for (const car_frame::OtherCar& car : frame_.other_cars) {
    cars += std::format("{}ID {} at {:.1f} m ({} draws)", cars.empty() ? "" : ", ", car.stencil_id, car.distance, car.draws);
  }
  const std::string vehicles = vehicles_read_
                                   ? std::format("the game's vehicles near the player: {} ({} of {} opponents read, {} traffic and {} parked cars live in all, {} not a car's pose, "
                                                 "{} taken for the player's own){}{}",
                                                 frame_.vehicles.size(), vehicle_scan_.opponents_read, vehicle_scan_.opponents_active, vehicle_scan_.traffic_seen,
                                                 vehicle_scan_.parked_seen, vehicle_scan_.rejected, vehicle_scan_.own, vehicle_scan_.valid ? "" : " (lists NOT readable)",
                                                 vehicle_scan_.note.empty() ? std::string() : " [" + vehicle_scan_.note + "]")
                                   : std::string("the game's vehicles: not read");
  return std::format("other cars ({}): {} kept [{}]; of {} draws of other cars {} listed, {} of cars not kept, {} under {} indices, {} skinned, "
                     "{} with no triangles or no matrix, {} waiting for their fingerprints (budget {} KiB a frame), {} with no key; {} cars given up "
                     "for a nearer one; {}",
                     car_rt_instances::Describe(other_limits_), frame_.other_cars.size(), cars, counts_.others_seen, counts_.others_listed,
                     counts_.others_far, counts_.others_small, other_limits_.min_indices, counts_.others_skinned,
                     counts_.others_no_triangles + counts_.others_not_finite, counts_.others_deferred, other_limits_.content_bytes / 1024u,
                     counts_.others_no_key, counts_.others_evicted, vehicles);
}

// ---------------------------------------------------------------------------
// the draws
// ---------------------------------------------------------------------------

// CHECK: the pause view's world (4 guest samples) and the garage draw the car once per tile, and a rear-view mirror may draw the vehicles into
// the main keys: the Frame would then list the car's draws more than once (the pause view) or twice with another camera (the mirror). Not
// told apart here; the log line's draw count (about 210 for the parked R34 of the car dump, without the proxies) shows it.
void CarFrame::OnMainDraw(const MainDraw& d) {
  const cpu_timing::Scope cpu_scope(cpu_timing::Section::kCarCollect);
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
  if (walk_.active()) {
    // The draw's own model-to-world, for the check of the walk's placements (a rigid draw only: the recipe the draw path uses).
    temporal::Mat4 world;
    bool have_world = false;
    if (walk_.producing() && d.banks) {
      const RecipeEntry& recipe = RecipeOf(d);
      if (recipe.choice.recipe != car_dump::Recipe::kSkinned) {
        const car_frame_rules::ModelDecode model = car_frame_rules::DecodeModel(d.banks->vs.data(), recipe.choice.recipe);
        if (model.finite) {
          world = model.model;
          have_world = true;
        }
      }
    }
    // the model walk: the first call of a walk frame walks the cars, every call notes the draw's buffers
    walk_.OnVehicleDraw(*d.view, *d.state, id, have_world ? &world : nullptr);
    if (walk_.TakeFresh()) {
      for (car_frame::Listener* listener : listeners_) {
        listener->OnWalkedCars(walk_.walked(), walk_.mesh_source());  // inside the draw's call: the guest is held back
      }
    }
  }
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
    if (player_.valid) frame_.vehicle_to_world = player_.vehicle_to_world;
    if (others_on_ && player_.valid && !vehicles_read_) {
      // The game's other vehicles near it, from the same call (car_rt_memory_rules.h): what places a car the camera does not see.
      vehicles_read_ = true;
      car_mesh::ReadOtherVehicles(player_, car_rt_memory::ScanRadius(other_limits_.radius), &frame_.vehicles, &vehicle_scan_, log_);
      frame_.vehicles_valid = vehicle_scan_.valid;
      frame_.player_vehicle_known = true;
      for (int c = 0; c < 3; ++c) {
        frame_.player_vehicle[c] = player_.position[c];
      }
    }
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
    if ((others_on_ || stencil_centres_on_) && finite) {
      car_rt_memory::NoteSeen(&frame_.seen_cars, id, translation);
    }
    if (others_on_) {
      // Another car's draw (the previous frame's ID is the player's until a draw near the vehicle says otherwise, and a draw of another ID that is not
      // that near the vehicle takes no match: IdentityTracker::kStrictMatchMetres): to the selection of the cars of the ray traced scene.
      HandleOtherDraw(d, order, id, verdict.distance, bank);
      return;
    }
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

// A draw of another car (the ray traced scene's other cars, car_rt_instances_rules.h): the selection by distance from the player's vehicle, then
// what the player's draws get: the recipe, the matrix, the mesh key. Left out: the draws of cars that are not kept, the smallest parts, the skinned
// ones (the other cars' drivers), and a draw whose mesh has no fingerprint yet while the frame's budget for them is spent (it joins in a later
// frame: a car that is new appears in parts over a few frames, and the frame that sees it does not stall on it).
// CHECK: the cost of a kept car's draw in a steady frame (no new fingerprints) is the layout and the recipe lookups, the matrix, the key's two
// stamps and two fingerprint lookups, a few hundred nanoseconds; times about 140 draws a car and 2 to 4 cars. The first run's render thread
// time with and without mcla_gpu_car_rt_cars 0 says; the log line's draw counts say how many draws were handled.
void CarFrame::HandleOtherDraw(const MainDraw& d, uint32_t order, uint32_t id, double distance, const uint8_t* bank) {
  ++counts_.others_seen;
  const car_rt_instances::CarSelector::Verdict verdict = selector_.OnDraw(id, distance);
  if (verdict.evicted != 0) {
    DropOtherCar(verdict.evicted);
  }
  if (!verdict.keep) {
    ++counts_.others_far;
    return;
  }
  const DrawRecord& r = d.view->record();
  if (r.count < other_limits_.min_indices) {
    ++counts_.others_small;
    return;
  }
  if (!d.input_layout || !d.geometry || !HasTriangles(d)) {
    ++counts_.others_no_triangles;
    return;
  }
  const LayoutInfo& layout = LayoutOf(*d.input_layout);
  if (!layout.position) {
    ++counts_.others_no_triangles;
    return;
  }
  const RecipeEntry& recipe = RecipeOf(d);
  if (recipe.choice.recipe == car_dump::Recipe::kSkinned) {
    ++counts_.others_skinned;
    return;
  }
  const car_frame_rules::ModelDecode model = car_frame_rules::DecodeModel(bank, recipe.choice.recipe);
  if (!model.finite) {
    ++counts_.others_not_finite;
    return;
  }
  content_limit_ = other_limits_.content_bytes;
  content_blocked_ = false;
  const uint64_t key = ComputeMeshKey(d, layout);
  content_limit_ = 0;
  if (key == 0) {
    ++(content_blocked_ ? counts_.others_deferred : counts_.others_no_key);
    content_blocked_ = false;
    return;
  }

  car_frame::OtherDraw out;
  out.stencil_id = id;
  out.order = order;
  out.after_world = world_draws_ != 0;
  out.vs = r.vs_identity;
  out.ps = r.ps_identity;
  out.mesh_key = key;
  out.indices = r.count;
  out.model_to_world = model.model;
  out.material = MaterialOf(d);  // RT-HIT-SHADING: what the draw's surface is made of (a hit on this car's part the camera does not see is shaded from it)
  NoteLight(d, out.material);
  frame_.other_draws.push_back(out);
  car_frame::OtherCar* car = nullptr;
  for (car_frame::OtherCar& c : frame_.other_cars) {
    if (c.stencil_id == id) {
      car = &c;
      break;
    }
  }
  if (!car) {
    frame_.other_cars.emplace_back();
    car = &frame_.other_cars.back();
    car->stencil_id = id;
  }
  car->Add(model.model, distance);
  ++counts_.others_listed;

  // The listeners read the triangles of a mesh they have not seen, here, in the draw's own call.
  scratch_draw_.part = car_frame::Part::kBody;
  scratch_draw_.stencil_id = id;
  scratch_draw_.vs = r.vs_identity;
  scratch_draw_.ps = r.ps_identity;
  scratch_draw_.order = order;
  scratch_draw_.mesh_key = key;
  scratch_draw_.model_to_world = model.model;
  ctx_.draw_in = &d;
  ctx_.draw = &scratch_draw_;
  ctx_.active = true;
  mesh_loaded_ = false;
  for (car_frame::Listener* listener : listeners_) {
    listener->OnOtherCarDraw(out, reader_);
  }
}

// A car the selection gave up for a nearer one: its listed draws go.
void CarFrame::DropOtherCar(uint32_t id) {
  std::erase_if(frame_.other_draws, [id](const car_frame::OtherDraw& o) { return o.stencil_id == id; });
  std::erase_if(frame_.other_cars, [id](const car_frame::OtherCar& c) { return c.stencil_id == id; });
  ++counts_.others_evicted;
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
  if (d.viewport) {
    out->viewport = *d.viewport;
    out->depth_word = d.depth_word;
    out->viewport_known = true;
  }
  if (others_on_ && !skinned) {
    out->material = MaterialOf(d);  // RT-HIT-SHADING: the shader's class, colours and diffuse fetch (the opponents' reflections of this car's hits)
    NoteLight(d, out->material);
  }
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

// The mesh key of a rigid draw: where its position stream, its index range and its declaration are, what it draws from them, and a
// fingerprint of the guest bytes it draws (car_frame_rules.h MeshKeyOf).
//
// The key was once made from the geometry cache's stamp of the two guest ranges. That stamp only says "may have changed" (it moves when
// anything in a page the range touches was written, when the region is watched or hashed, when it is made again), and in a parked
// car's frames about a dozen keys moved every frame (the box cache read those meshes again, the ray tracing built them again). The
// stamp now only decides WHEN the bytes are looked at again: the indices are scanned once, the vertices the indices name are hashed
// once, both again only when the stamp has moved, and the key follows what they say. Per draw in a steady frame: the stamps (an
// exact-range lookup each), the fingerprint cache's lookups, the key's mixing; no heap allocation, no guest read.
// CHECK: the cost is estimated, not measured (about 5 microseconds for a mesh of ~1.4k indices and ~7 KB of vertices when its stamp
// moved; one such pass over all of a car's ~195 draws, ~1 ms, in the frame the car appears): the 'keys' line's KiB figure and the
// render thread's time in a run say. What the guest writes to make the stamps move (writes beside the meshes, the same bytes
// again, or other bytes) is not known from the logs; the 'keys' line splits it: "again with the same bytes" against "other bytes".
uint64_t CarFrame::ComputeMeshKey(const MainDraw& d, const LayoutInfo& layout) {
  const DrawRecord& r = d.view->record();
  if (r.kind == DrawKind::kInline) {
    return 0;  // the vertices are in the draw's own record
  }
  const car_mesh::StreamRef position = car_mesh::ResolveStream(*d.view, r, layout.position_stream);
  if (!position.ok) {
    return 0;
  }
  car_mesh::StreamRef normal;  // the NORMAL element's own stream, when it is another one
  const bool own_normal = layout.normal && layout.normal_stream != layout.position_stream;
  if (own_normal) {
    normal = car_mesh::ResolveStream(*d.view, r, layout.normal_stream);
    if (!normal.ok) {
      return 0;
    }
  }
  car_frame_rules::MeshKeyParts parts;
  parts.position = car_frame_rules::StreamKey{position.address, position.size, position.stride, layout.position_offset,
                                              uint32_t(layout.position_format), 0};
  if (own_normal) {
    parts.normal = car_frame_rules::StreamKey{normal.address, normal.size, normal.stride, layout.normal_offset,
                                              uint32_t(layout.normal_format), 0};
  } else if (layout.normal) {
    parts.normal.offset = layout.normal_offset;
    parts.normal.format = uint32_t(layout.normal_format);
  }
  const bool indexed = r.kind == DrawKind::kIndexed;
  parts.indexed = indexed ? 1u : 0u;
  parts.primitive = r.primitive;
  parts.count = r.count;
  parts.start = r.start;
  parts.base_vertex = r.base_vertex;
  // A draw with more indices than a car has is not fingerprinted (car_mesh::ReadTriangles does not read it either).
  const bool fingerprint = d.cache != nullptr && r.count <= car_mesh::kMaxIndices;
  IndexInfo index;
  if (indexed) {
    const bool index32 = (r.index_dword0 & 0x80000000u) != 0;
    const uint32_t size = index32 ? 4u : 2u;
    parts.index_address = GuestMemory::ToPhysical(r.index_address) + r.start * size;
    parts.index_bytes = r.count * size;
    parts.index32 = index32 ? 1u : 0u;
    parts.endian = (r.index_dword0 >> 29) & 3u;
    bool order_free = false, skip_reset = false;
    if (d.primitive.topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP) {
      parts.reset = d.state->sc_mode.multi_prim_ib_ena != 0 ? 1u : 0u;
      parts.reset_index = d.state->reset_index & (index32 ? 0xFFFFFFFFu : 0xFFFFu);
      skip_reset = parts.reset != 0;
    } else {
      order_free = d.primitive.topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    }
    // The geometry cache holds a region of the index range only when the draw's indices went to the host as they are; a draw that
    // converts them (quads, fans) has no stamp to tell a change by, and its indices stay out of the key.
    if (fingerprint && d.primitive.conversion == PrimitiveConversion::kNone) {
      index = IndexContent(d, parts.index_address, parts.index_bytes, r.count, index32, parts.endian, order_free, skip_reset,
                           parts.reset_index);
      parts.index_content = index.content;
    }
  }
  if (fingerprint) {
    // The vertices the draw uses: those its indices name (plus the base vertex), or its own run of an auto draw. The rest of the fetch
    // window is other meshes' (or other data) and does not belong in the key.
    const auto own = [&](const car_mesh::StreamRef& s) {
      return car_frame_rules::OwnVertices(indexed, r.start, r.count, r.base_vertex, index.have_range, index.lo, index.hi,
                                          s.stride != 0 ? s.size / s.stride : 0u);
    };
    parts.position.content = StreamContent(d, position, own(position));
    if (own_normal) {
      parts.normal.content = StreamContent(d, normal, own(normal));
    }
  }
  if (content_blocked_) {
    return 0;  // another car's mesh whose fingerprints the frame's budget did not allow: no key yet (what was made stays cached)
  }
  return car_frame_rules::MeshKeyOf(parts);
}

// CHECK: a range the geometry cache holds no region of (a draw that has not been through Prepare, a vertex stream built from the resolve
// memory) has no stamp: its bytes stay out of the key, as the stamp did before. Not seen in a run: the log's 'keys' line would show a
// mesh whose key never moves while its bytes do, if any.
CarFrame::IndexInfo CarFrame::IndexContent(const MainDraw& d, uint32_t address, uint32_t bytes, uint32_t count, bool index32, uint32_t endian,
                                           bool order_free, bool skip_reset, uint32_t reset_index) {
  IndexInfo out;
  const uint64_t stamp = d.cache->RangeStamp(address, bytes);
  if (stamp == 0 || bytes == 0) {
    return out;
  }
  const uint64_t slot = car_frame_rules::IndexSlot(address, bytes, index32, endian, order_free, skip_reset, reset_index);
  if (content_limit_ != 0 && !contents_.Fresh(slot, stamp) && !car_rt_instances::MayFingerprint(content_spent_, bytes, content_limit_)) {
    content_blocked_ = true;  // another car's new mesh, and this frame's budget for those is spent
    return out;
  }
  car_frame_rules::ContentCache::Outcome outcome = car_frame_rules::ContentCache::Outcome::kHit;
  const car_frame_rules::ContentEntry entry = contents_.Get(
      slot, stamp, guest_frame_,
      [&](car_frame_rules::ContentEntry* made) {
        counts_.content_bytes += bytes;
        if (content_limit_ != 0) {
          content_spent_ += bytes;
        }
        const uint8_t* raw = memory_->Read(address, bytes);
        if (!raw) {
          return;  // not committed guest memory: no fingerprint (0), nothing is read from it
        }
        const car_frame_rules::IndexSummary summary =
            car_frame_rules::SummarizeIndices(raw, count, index32, endian, order_free, skip_reset, reset_index);
        made->content = summary.fingerprint;
        made->has_range = summary.any;
        made->lo = summary.lo;
        made->hi = summary.hi;
      },
      &outcome);
  NoteOutcome(outcome);
  out.content = entry.content;
  out.have_range = entry.has_range;
  out.lo = entry.lo;
  out.hi = entry.hi;
  return out;
}

// The fingerprint of the bytes of the vertices of one stream that the draw uses (`span`), made again when the geometry cache's stamp of
// the stream's whole fetch window has moved. 0: not known (no region of that window in the geometry cache, or unreadable memory).
uint64_t CarFrame::StreamContent(const MainDraw& d, const car_mesh::StreamRef& stream, const car_frame_rules::VertexSpan& span) {
  if (stream.size == 0 || stream.inline_data) {
    return 0;
  }
  const uint64_t stamp = d.cache->RangeStamp(stream.address, stream.size);
  if (stamp == 0) {
    return 0;
  }
  uint32_t offset = 0, length = stream.size;
  if (!span.whole && stream.stride != 0) {
    offset = span.first * stream.stride;
    length = span.count * stream.stride;
  }
  const uint64_t slot = car_frame_rules::VertexSlot(stream.address, stream.size, stream.stride, span);
  if (content_limit_ != 0 && !contents_.Fresh(slot, stamp) && !car_rt_instances::MayFingerprint(content_spent_, length, content_limit_)) {
    content_blocked_ = true;
    return 0;
  }
  car_frame_rules::ContentCache::Outcome outcome = car_frame_rules::ContentCache::Outcome::kHit;
  const car_frame_rules::ContentEntry entry = contents_.Get(
      slot, stamp, guest_frame_,
      [&](car_frame_rules::ContentEntry* made) {
        counts_.content_bytes += length;
        if (content_limit_ != 0) {
          content_spent_ += length;
        }
        if (length == 0) {
          made->content = car_frame_rules::VertexContent(0, span);  // the draw names no vertex the window holds
          return;
        }
        const uint8_t* bytes = memory_->Read(stream.address + offset, length);
        if (bytes) {
          made->content = car_frame_rules::VertexContent(uint64_t(XXH3_64bits(bytes, length)), span);
        }
      },
      &outcome);
  NoteOutcome(outcome);
  return entry.content;
}

void CarFrame::NoteOutcome(car_frame_rules::ContentCache::Outcome outcome) {
  using Outcome = car_frame_rules::ContentCache::Outcome;
  switch (outcome) {
    case Outcome::kNew: ++counts_.content_new; break;
    case Outcome::kChanged: ++counts_.content_changed; break;
    case Outcome::kSame: ++counts_.content_same; break;
    case Outcome::kHit: break;
  }
}

// What the meshes' keys did, as one line at most every kKeyLogFrames guest frames (and only when something happened: a new key, a
// fingerprint made again). In a parked car, a few seconds after it appears, "new mesh keys" must be 0 and the line must stop; "again
// with the same bytes" is the write watch's stamp moving over a mesh that did not change (the key stayed: that is the fix at work).
void CarFrame::LogKeys() {
  KeyWindow& w = key_window_;
  ++w.frames;
  w.frames_with_new += counts_.new_keys != 0;
  w.max_new = std::max(w.max_new, counts_.new_keys);
  w.new_keys += counts_.new_keys;
  w.content_new += counts_.content_new;
  w.content_changed += counts_.content_changed;
  w.content_same += counts_.content_same;
  w.content_bytes += counts_.content_bytes;
  if (!w.Active() || (w.since != 0 && guest_frame_ - w.since < kKeyLogFrames)) {
    return;
  }
  uint32_t body = 0;
  for (const car_frame::Draw& draw : frame_.draws) {
    body += draw.part == car_frame::Part::kBody;
  }
  REXLOG_INFO("{} keys: frame {} (the last of {} collected since the last 'keys' line): {} new mesh keys ({} of those frames had some, at most {} in "
              "one; this frame lists {} body draws); behind the keys {} meshes' ranges fingerprinted for the first time, {} again with other bytes "
              "(another key), {} again with the same bytes (the write watch's stamp moved, the key stayed); {:.1f} KiB of guest bytes read for "
              "them; {} fingerprints held",
              kPrefix, guest_frame_, w.frames, w.new_keys, w.frames_with_new, w.max_new, body, w.content_new, w.content_changed, w.content_same,
              double(w.content_bytes) / 1024.0, contents_.size());
  w = KeyWindow{};
  w.since = guest_frame_;
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

// The model-space box of a rigid draw's mesh, from the cache by key; a key not seen yet is a NEW key (counted) and is read (the whole
// mesh: the draw's call is the only time the guest's bytes are held still), at most a few a frame, the rest in the frames after.
void CarFrame::NoteBox(car_frame::Draw* out) {
  if (out->mesh_key == 0) {
    return;
  }
  auto found = boxes_.find(out->mesh_key);
  if (found == boxes_.end()) {
    MeshBox fresh;
    fresh.pending = true;
    found = boxes_.emplace(out->mesh_key, fresh).first;
    ++counts_.new_keys;
  }
  MeshBox& box = found->second;
  box.seen = guest_frame_;
  if (box.pending) {
    if (boxes_read_ >= kBoxBudget) {
      out->box_deferred = true;
      return;
    }
    ++boxes_read_;
    ++counts_.boxes_read;
    box.pending = false;
    if (LoadMesh()) {
      box.valid = car_frame_rules::BoxOfMesh(mesh_.local.data(), mesh_.local.size() / 3, mesh_.tri.data(), mesh_.tri.size(), box.lo, box.hi);
    }
  }
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

bool CarFrame::Reader::ReadTexcoords(std::vector<float>* uvs) const {
  if (!owner) {
    if (uvs) {
      uvs->clear();
    }
    return false;
  }
  return owner->ReadTexcoordsForListener(uvs);
}

// The current draw's TEXCOORD0 for the vertices of the triangles ReadForListener returned (the mesh read of the same draw is shared). A rigid draw only: the driver's vertices are
// blended and are not in the scene.
bool CarFrame::ReadTexcoordsForListener(std::vector<float>* uvs) {
  if (uvs) {
    uvs->clear();
  }
  if (!uvs || !ctx_.active || !ctx_.draw || !ctx_.draw_in || !memory_ || ctx_.draw->part != car_frame::Part::kBody || !LoadMesh()) {
    return false;
  }
  return car_mesh::ReadTexcoords(*memory_, *ctx_.draw_in->view, mesh_.first_vertex, uint32_t(mesh_.local.size() / 3), uvs);
}

// RT-HIT-SHADING (car_rt_material_rules.h): the class of the draw's pixel shader, its base colours from the pixel constants, and the fetch constant of its diffuse texture.
car_rt_material::Material CarFrame::MaterialOf(const MainDraw& d) const {
  const DrawRecord& r = d.view->record();
  const auto fetch = [&](uint32_t slot) {
    car_rt_material::FetchInfo info;
    uint32_t dwords[6];
    d.view->FetchGroup(slot, dwords);
    const TextureFetch texture = ReadTextureFetch(dwords);
    if (texture.is_texture && texture.key.dimension == uint8_t(xe::DataDimension::k2DOrStacked) && !texture.key.stacked && texture.key.width != 0 && texture.key.height != 0) {
      info.texture = true;
      std::memcpy(info.dwords, dwords, sizeof(info.dwords));
      info.width = texture.key.width;
      info.height = texture.key.height;
    }
    return info;
  };
  return car_rt_material::Capture(r.ps_identity, d.banks ? d.banks->ps.data() : nullptr, fetch);
}

// The frame's light: the pixel constants of the first vehicle draw (c26 ambient, c39 sun colour, c57 sky), and the environment map's slots from the first draw of a shader that
// reads it (fetch slots 0 and 1; the slot the scene bound for the draw, valid for this frame).
// CHECK: that c26, c39 and c57 hold the ambient, the sun's colour and the sky colour for the CARS' shaders as they do for the ao's reads of the world (the translated paint shader
// names them gLightAmbient, gLightColor[0] and ambYPos at those registers); the "[car rt] ... the cars' light" line prints them at the first frame.
void CarFrame::NoteLight(const MainDraw& d, const car_rt_material::Material& material) {
  car_rt_material::FrameLight& light = frame_.light;
  if (!light.valid && d.banks) {
    const uint8_t* ps = d.banks->ps.data();
    float sun[4], ambient[4], sky[4];
    if (car_rt_material::ReadRegister(ps, 39, sun) && car_rt_material::ReadRegister(ps, 26, ambient)) {
      for (int c = 0; c < 3; ++c) {
        light.sun[c] = std::max(sun[c], 0.0f);
        light.ambient[c] = std::max(ambient[c], 0.0f);
      }
      if (car_rt_material::ReadRegister(ps, 57, sky)) {
        for (int c = 0; c < 3; ++c) {
          light.sky[c] = std::max(sky[c], 0.0f);
        }
      }
      light.valid = true;
    }
  }
  if (light.env_sharp == 0 && material.env != 0 && d.shared) {
    const uint32_t sharp = d.shared->texture_2d[0];
    if ((sharp & ~kTextureGammaBit) != kNullTexture2DSlot) {
      light.env_sharp = sharp;
      light.env_blur = material.env == 1 ? d.shared->texture_2d[1] : 0u;
    }
  }
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

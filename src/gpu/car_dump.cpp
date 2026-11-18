// MCLA GPU renderer -- the car dump (a debug prototype). See car_dump.h.

#include "car_dump.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "device.h"
#include "frame.h"
#include "guest_memory.h"
#include "host_passes.h"
#include "native_gfx/guest/guest_resources.h"
#include "native_gfx/shader_identity.h"
#include "native_gfx/vertex_declaration.h"
#include "vehicle_motion.h"

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

namespace {

constexpr const char* kPrefix = "[car dump]";
// How far a draw's pixels may be from the image's distance, relative, to count as the same surface (the half float image is
// within 0.05% of the distance; 1% is 5 cm at 5 m).
constexpr double kFitTolerance = 0.01;
static_assert(car_dump::kIdSlots == temporal::kVehicleIds, "the dump's ID slots are the renderer's");

// The key of one group of the census.
using GroupKey = std::tuple<uint32_t, uint64_t, uint64_t>;  // ID, vertex shader, pixel shader

std::string PrimitiveText(uint32_t mask) {
  std::string text;
  for (uint32_t p = 0; p < 32; ++p) {
    if (mask & (1u << p)) {
      text += std::format("{}{}", text.empty() ? "" : "/", PrimitiveName(p));
    }
  }
  return text.empty() ? "none" : text;
}

std::string FormatText(uint32_t mask) {
  std::string text;
  for (uint32_t f = 0; f < 8; ++f) {
    if (mask & (1u << f)) {
      text += std::format("{}{}", text.empty() ? "" : "/", car_dump::PositionFormatName(car_dump::PositionFormat(f)));
    }
  }
  return text.empty() ? "none" : text;
}

double Distance3(const double a[3], const double b[3]) {
  const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

double Relative(double a, double b) { return std::fabs(a - b) / std::max(1.0, std::fabs(b)); }

}  // namespace

const char* CarDump::ProblemName(Problem problem) {
  return car_mesh::ProblemName(problem);
}

CarDump::~CarDump() = default;

void CarDump::Initialize(Device& device, FrameRing& frames, GuestMemory& memory) {
  device_ = &device;
  frames_ = &frames;
  memory_ = &memory;
  Refresh();
}

void CarDump::Shutdown() {
  readback_.reset();
  readback_bytes_ = 0;
  Release();
  wants_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  memory_ = nullptr;
}

void CarDump::Refresh() {
  const uint64_t target = larecomp::settings::mcla_gpu_car_dump.Get();
  if (target != target_) {
    target_ = target;
    done_ = false;  // a new request dumps again
  }
  wants_ = memory_ != nullptr && target != 0 && !done_ && frame_ >= target;
}

void CarDump::Release() {
  draws_.clear();
  draws_.shrink_to_fit();
  history_ = History{};
  finished_ = false;
  camera_valid_ = false;
}

// ---------------------------------------------------------------------------
// the frame's draws
// ---------------------------------------------------------------------------

void CarDump::Open() {
  Release();
  open_ = true;
  dump_frame_ = frame_;
  chosen_ = 0;
  car_mesh::ReadPlayerVehicle(&player_, true);
  REXLOG_INFO("{} frame {}: the first draw that writes a vehicle ID comes after {} main-view draws that write none; dumping this "
              "frame; player's vehicle: {}",
              kPrefix, dump_frame_, world_draws_, player_.note);
}

void CarDump::OnMainDraw(const MainDraw& draw) {
  const uint32_t mask = draw.stencil_ids & ~1u & ((1u << temporal::kVehicleIds) - 1u);
  if (mask == 0) {
    ++world_draws_;
    return;
  }
  if (!open_) {
    Open();
  }
  draws_.emplace_back();
  Draw& out = draws_.back();
  const DrawRecord& r = draw.view->record();
  out.id_mask = mask;
  out.id = uint32_t(std::countr_zero(mask));
  out.vs = r.vs_identity;
  out.ps = r.ps_identity;
  out.world_before = world_draws_;
  Expand(draw, &out);
}

void CarDump::Expand(const MainDraw& d, Draw* out) {
  const DrawView& view = *d.view;
  const DrawRecord& r = view.record();
  out->primitive = r.primitive;
  out->kind = r.kind;
  out->count = r.count;
  out->matrices = car_dump::DecodeCarMatrices(d.banks->vs.data());
  out->damage = (d.state->bool_words[0] & 1u) != 0;
  if (d.shaders && d.vs) {
    d.shaders->GetVertexElements(*d.vs, elements_);
    for (const ShaderStore::VertexElement& e : elements_) {
      out->skinned |= e.usage == car_mesh::kUsageBlendIndices;
    }
  }

  // What the draw is and how its vertices reach the world: from what its vertex shader reads (car_dump_rules.h).
  for (uint32_t t = 0; t < kColorTargetCount; ++t) {
    if (d.state->color[t].bound) {
      out->colour_mask |= d.state->color[t].write_mask & 0xFu;
    }
  }
  out->proxy = car_dump::WheelProxyOf(r.vs_identity, r.ps_identity, out->colour_mask);
  const ShaderReads& reads = reads_.Of(r);
  out->ucode_read = reads.verified;
  const car_dump::RecipeChoice choice = car_dump::ChooseRecipe(reads.reads, out->skinned, r.vs_identity);
  out->recipe = choice.recipe;
  out->recipe_source = choice.source;
  out->role = car_dump::RoleOf(out->proxy, out->recipe);
  out->model = car_dump::ModelMatrix(out->matrices, out->recipe);

  // The draw's triangles out of guest memory (car_mesh_read.h: the same code the car frame reads a draw with).
  car_mesh::DrawInputs inputs;
  inputs.view = d.view;
  inputs.state = d.state;
  inputs.vs = d.vs;
  inputs.primitive = d.primitive;
  car_mesh::ReadTriangles(*memory_, inputs, scratch_, out);
  if (out->problem == Problem::kNone && out->recipe == car_dump::Recipe::kSkinned) {
    SkinDraw(d, out->first_vertex, uint32_t(out->local.size() / 3), out);
  }
}

// The blended positions of a skinned draw (car_dump_rules.h SkinBlend): the BLENDINDICES and BLENDWEIGHT elements of its declaration over
// the span of vertices `first`.. of their streams, the palette of the draw's bank. False, with out->skin_note saying why, when it cannot be
// done (the draw then has no placed positions).
bool CarDump::SkinDraw(const MainDraw& d, uint32_t first, uint32_t span, Draw* out) {
  return car_mesh::SkinPositions(*memory_, *d.view, d.banks->vs.data(), first, span, out->local, &out->blended, &out->skin,
                                 &out->skin_note);
}

void CarDump::OnAnchor(const ConstantBanks::Banks& banks) {
  if (!open_) {
    return;
  }
  // The frame's camera, as car_selfrefl.cpp OnAnchor reads it (the frame size in c62, the view, view-projection and view inverse in
  // c4-c15 of the anchor's pixel bank).
  const uint8_t* ps = banks.ps.data();
  const float width = car_dump::BankFloat(ps, 62, 0);
  const float height = car_dump::BankFloat(ps, 62, 1);
  camera_valid_ = false;
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    return;
  }
  camera_valid_ = temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), dump_frame_, &camera_) ==
                  temporal::Reject::kNone;
}

// ---------------------------------------------------------------------------
// the history image, read back
// ---------------------------------------------------------------------------

void CarDump::OnHistoryImage(plume::RenderCommandList* list, HostImage& image, uint32_t divisor, const temporal::Mat4& lens) {
  if (!open_ || history_.requested || !list || !device_ || !frames_ || !image.texture) {
    return;
  }
  if (image.format != plume::RenderFormat::R16G16B16A16_FLOAT || image.width == 0 || image.height == 0) {
    return;
  }
  const uint32_t width = image.width;
  const uint32_t height = image.height;
  const uint32_t row_bytes = (width * 8 + 255) / 256 * 256;  // rows 256-aligned
  const uint64_t bytes = uint64_t(row_bytes) * height;
  if (!readback_ || readback_bytes_ < bytes) {
    readback_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes), "car dump readback");
    readback_bytes_ = readback_ ? bytes : 0;
    if (!readback_) {
      REXLOG_ERROR("{} the history image's readback buffer could not be made: no check", kPrefix);
      return;
    }
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::COPY_SOURCE));
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(readback_.get(), plume::RenderFormat::R16G16B16A16_FLOAT, width, height, 1,
                                                        row_bytes / 8, 0),
      plume::RenderTextureCopyLocation::Subresource(image.texture.get()));
  // The next frame's paint draws read the image as a texture: back to where the history pass left it.
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
  history_.requested = true;
  history_.width = width;
  history_.height = height;
  history_.divisor = divisor;
  history_.lens = lens;
  const uint64_t frame = dump_frame_;
  frames_->RetireAction([this, frame, width, height, row_bytes] { ProcessHistory(frame, width, height, row_bytes); });
}

void CarDump::ProcessHistory(uint64_t frame, uint32_t width, uint32_t height, uint32_t row_bytes) {
  if (frame != dump_frame_ || !history_.requested || history_.ready || !readback_) {
    return;
  }
  const void* mapped = readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} the history image of frame {} could not be mapped: no check", kPrefix, frame);
    return;
  }
  const uint8_t* source = static_cast<const uint8_t*>(mapped);
  history_.distance.assign(size_t(width) * height, 0.0f);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = source + size_t(y) * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      uint16_t half;
      std::memcpy(&half, row + size_t(x) * 8 + 6, 2);  // the alpha: the vehicle's view distance
      const float value = car_dump::HalfToFloat(half);
      history_.distance[size_t(y) * width + x] = (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
    }
  }
  readback_->unmap();
  history_.ready = true;
  TryCheck();
}

void CarDump::TryCheck() {
  if (!finished_ || !history_.ready) {
    return;
  }
  RunCheck();
  Release();
}

// ---------------------------------------------------------------------------
// the end of the frame: the census and the OBJ
// ---------------------------------------------------------------------------

void CarDump::OnPresent() {
  if (open_) {
    Finish();
    open_ = false;
    done_ = true;
  }
  world_draws_ = 0;
  ++frame_;
  Refresh();
}

// What each stencil ID holds, by part (car_dump_rules.h IdSummary), for the choice of the player's car and the census.
void CarDump::Summarise(car_dump::IdSummary (&ids)[car_dump::kIdSlots]) const {
  for (const Draw& d : draws_) {
    car_dump::IdSummary& s = ids[d.id];
    if (d.role == car_dump::Role::kProxy) {
      ++s.proxy_draws;
      continue;
    }
    if (d.role == car_dump::Role::kSkinned) {
      ++s.skinned_draws;
      continue;
    }
    ++s.body_draws;
    s.paint_draws += d.ps == car_dump::kCarPaintPs;
    if (!d.matrices.finite) {
      continue;
    }
    const double t[3] = {d.matrices.world.m[3][0], d.matrices.world.m[3][1], d.matrices.world.m[3][2]};
    if (camera_valid_) {
      s.camera_distance = std::min(s.camera_distance, Distance3(t, camera_.position));
    }
    if (player_.valid) {
      s.player_distance = std::min(s.player_distance, Distance3(t, player_.position));
    }
  }
}

car_dump::Pick CarDump::ChoosePlayer(const car_dump::IdSummary (&ids)[car_dump::kIdSlots]) const {
  const int32_t wanted = larecomp::settings::mcla_gpu_car_dump_id.Get();
  const car_dump::Pick pick = car_dump::ChoosePlayerId(ids, wanted, player_.valid);
  if (wanted > 0 && pick.reason != car_dump::PickReason::kRequested) {
    REXLOG_WARN("{} mcla_gpu_car_dump_id = {} has no car draw in this frame: choosing by the rules instead", kPrefix, wanted);
  }
  return pick;
}

void CarDump::Finish() {
  car_dump::IdSummary summary[car_dump::kIdSlots];
  Summarise(summary);
  const car_dump::Pick pick = ChoosePlayer(summary);
  chosen_ = pick.id;
  REXLOG_INFO("{} the player's car is {}: {}. The vehicle the game says the player drives: {}. {}", kPrefix,
              pick.id != 0 ? std::format("stencil ID {}", pick.id) : std::string("not found"), car_dump::PickReasonName(pick.reason),
              player_.note,
              player_.valid && pick.reason != car_dump::PickReason::kPlayerVehicle
                  ? std::format("No ID's gWorld is within {:.0f} m of it (or one was asked for): CHECK whether the matrix at entity + 0x90 is "
                                "in the draws' world coordinates",
                                car_dump::kPlayerMatchMetres)
                  : std::string());
  Census(chosen_, summary);
  WriteObj(chosen_, car_dump::Role::kBody);
  WriteObj(chosen_, car_dump::Role::kSkinned);
  finished_ = true;
  if (!history_.requested) {
    REXLOG_WARN("{} frame {}: the car history image was not made in this frame, so there is no check against it (the car "
                "self-reflections must be on: --mcla_gpu_car_selfrefl_test=1; they run in the game's view with a camera for the "
                "frame before and a paint draw through the variant in this frame)",
                kPrefix, dump_frame_);
    Release();
  } else {
    TryCheck();  // (the image may have been read before the present)
  }
}

void CarDump::Census(uint32_t chosen, const car_dump::IdSummary (&summary)[car_dump::kIdSlots]) {
  struct Group {
    uint32_t draws = 0;
    uint32_t primitives = 0;   // bit per xenos primitive type seen
    uint32_t formats = 0;      // bit per PositionFormat seen
    uint32_t indexed = 0, index16 = 0, index32 = 0, autos = 0, inlines = 0;
    uint32_t stride = 0, offset = 0, stream = 0;
    bool stride_varies = false;
    uint32_t bone_min = 0xFFFFFFFFu, bone_max = 0, no_bone = 0;
    uint32_t damage = 0, skinned = 0;
    uint32_t before = 0, after = 0;
    uint32_t triangles = 0, vertices = 0, degenerate = 0, out_of_range = 0;
    uint32_t problems[16] = {};
    bool have_t = false;
    double t[3] = {};
    double dist_min = 1e300, dist_max = -1.0;
    uint32_t wvp_ok = 0, wvp_checked = 0;
    double wvp_worst = 0.0;
    car_dump::Recipe recipe = car_dump::Recipe::kPalette;
    car_dump::RecipeSource source = car_dump::RecipeSource::kDefault;
    car_dump::Role role = car_dump::Role::kBody;
    car_dump::ProxyWhy proxy = car_dump::ProxyWhy::kNo;
    uint32_t ucode_draws = 0;       // draws whose recipe was read from verified microcode
    uint32_t colour_mask = 0;       // the draws' colour masks, or-ed
    uint32_t colour_mask_zero = 0;  // draws with colour mask 0
    uint32_t skin_placed = 0;       // skinned draws that have blended positions
    uint32_t skin_vertices = 0, skin_bad_bone = 0, skin_bad_weight = 0;
    double skin_weight_error = 0.0;
    std::string skin_note;
  };
  struct IdTotals {
    uint32_t draws = 0, vertices = 0, triangles = 0, degenerate = 0, out_of_range = 0, no_triangles = 0;
    double nearest = 1e300;
  };
  std::map<GroupKey, Group> groups;
  IdTotals ids[temporal::kVehicleIds];
  uint32_t before = 0, after = 0, multi = 0;
  for (const Draw& d : draws_) {
    Group& g = groups[GroupKey{d.id, d.vs, d.ps}];
    IdTotals& total = ids[d.id];
    ++g.draws;
    ++total.draws;
    if (g.draws == 1) {
      g.recipe = d.recipe;
      g.source = d.recipe_source;
      g.role = d.role;
      g.proxy = d.proxy;
    }
    g.ucode_draws += d.ucode_read;
    g.colour_mask |= d.colour_mask;
    g.colour_mask_zero += d.colour_mask == 0;
    if (d.role == car_dump::Role::kSkinned) {
      g.skin_placed += !d.blended.empty();
      g.skin_vertices += d.skin.vertices;
      g.skin_bad_bone += d.skin.bad_bone;
      g.skin_bad_weight += d.skin.bad_weight;
      g.skin_weight_error = std::max(g.skin_weight_error, d.skin.worst_weight_sum_error);
      if (g.skin_note.empty()) {
        g.skin_note = d.skin_note;
      }
    }
    before += d.world_before == 0;
    after += d.world_before != 0;
    multi += (d.id_mask & (d.id_mask - 1)) != 0;
    g.primitives |= 1u << (d.primitive & 31u);
    g.formats |= 1u << uint32_t(d.format);
    g.indexed += d.kind == DrawKind::kIndexed;
    g.index16 += d.kind == DrawKind::kIndexed && !d.index32;
    g.index32 += d.kind == DrawKind::kIndexed && d.index32;
    g.autos += d.kind == DrawKind::kAuto;
    g.inlines += d.kind == DrawKind::kInline;
    if (g.draws == 1) {
      g.stride = d.stride;
      g.offset = d.offset;
      g.stream = d.stream;
    } else if (g.stride != d.stride || g.offset != d.offset || g.stream != d.stream) {
      g.stride_varies = true;
    }
    if (d.matrices.bone_valid) {
      g.bone_min = std::min(g.bone_min, d.matrices.bone);
      g.bone_max = std::max(g.bone_max, d.matrices.bone);
    } else {
      ++g.no_bone;
    }
    g.damage += d.damage;
    g.skinned += d.skinned;
    (d.world_before == 0 ? g.before : g.after) += 1;
    g.triangles += d.triangles;
    g.vertices += d.vertices;
    g.degenerate += d.degenerate;
    g.out_of_range += d.out_of_range;
    ++g.problems[uint32_t(d.problem)];
    total.vertices += d.vertices;
    total.triangles += d.triangles;
    total.degenerate += d.degenerate;
    total.out_of_range += d.out_of_range;
    total.no_triangles += d.problem != Problem::kNone;
    // gWorld's translation (c3), its distance to the camera and whether gWorld times the camera's view-projection is the draw's
    // gWorldViewProj.
    const double t[3] = {d.matrices.world.m[3][0], d.matrices.world.m[3][1], d.matrices.world.m[3][2]};
    if (!g.have_t) {
      g.have_t = true;
      std::memcpy(g.t, t, sizeof(t));
    }
    if (camera_valid_ && d.matrices.finite) {
      const double dist = Distance3(t, camera_.position);
      g.dist_min = std::min(g.dist_min, dist);
      g.dist_max = std::max(g.dist_max, dist);
      total.nearest = std::min(total.nearest, dist);
      const double error = car_dump::WvpError(d.matrices.world, camera_.view_projection, d.matrices.wvp);
      ++g.wvp_checked;
      g.wvp_ok += error <= car_dump::kWvpTolerance;
      g.wvp_worst = std::max(g.wvp_worst, error);
    }
  }
  REXLOG_INFO("{} frame {}: {} main-view draws write a vehicle ID ({} before the first world draw, {} after it; {} write more "
              "than one ID), the frame's {} main-view draws before the first vehicle draw wrote none; camera {}",
              kPrefix, dump_frame_, draws_.size(), before, after, multi, draws_.empty() ? 0u : draws_.front().world_before,
              camera_valid_ ? std::format("at ({:.2f}, {:.2f}, {:.2f})", camera_.position[0], camera_.position[1], camera_.position[2])
                            : std::string("not read (the anchor draw was not seen, or its constants were not a camera): no distances, no gWorld * VP check"));
  for (uint32_t id = 1; id < temporal::kVehicleIds; ++id) {
    const IdTotals& total = ids[id];
    if (total.draws == 0) {
      continue;
    }
    const car_dump::IdSummary& parts = summary[id];
    REXLOG_INFO("{} id {}{}: {} draws ({} body, {} skinned, {} wheel proxy; {} of the body with the car paint), {} vertices, {} triangles "
                "(+ {} degenerate, {} out of range), {} draws with no triangles, nearest gWorld {}, {}",
                kPrefix, id, id == chosen ? " (the player's car)" : "", total.draws, parts.body_draws, parts.skinned_draws,
                parts.proxy_draws, parts.paint_draws, total.vertices, total.triangles, total.degenerate, total.out_of_range,
                total.no_triangles,
                total.nearest < 1e299 ? std::format("{:.2f} m from the camera", total.nearest) : std::string("n/a"),
                parts.player_distance < std::numeric_limits<double>::infinity()
                    ? std::format("{:.2f} m from the player's vehicle (its body draws)", parts.player_distance)
                    : std::string("no distance to the player's vehicle"));
  }
  for (const auto& [key, g] : groups) {
    std::string bones;
    if (g.bone_max >= g.bone_min && g.bone_min != 0xFFFFFFFFu) {
      bones = g.bone_min == g.bone_max ? std::format("bone {}", g.bone_min) : std::format("bones {}-{}", g.bone_min, g.bone_max);
    } else {
      bones = "no valid bone";
    }
    if (g.no_bone != 0 && g.no_bone != g.draws) {
      bones += std::format(" ({} draws with an invalid c204.x)", g.no_bone);
    } else if (g.no_bone == g.draws && g.draws != 0 && g.bone_min != 0xFFFFFFFFu) {
      bones += " (invalid)";
    }
    std::string problems;
    for (uint32_t p = 1; p < 16; ++p) {
      if (g.problems[p]) {
        problems += std::format("{}{} x {}", problems.empty() ? "" : ", ", ProblemName(Problem(p)), g.problems[p]);
      }
    }
    std::string wvp = "gWorld * VP vs gWorldViewProj: no camera";
    std::string where = std::format("gWorld t = ({:.2f}, {:.2f}, {:.2f})", g.t[0], g.t[1], g.t[2]);
    if (g.wvp_checked != 0) {
      wvp = std::format("gWorld * VP == gWorldViewProj in {}/{} draws (worst relative error {:.1e})", g.wvp_ok, g.wvp_checked,
                        g.wvp_worst);
      where += std::format(", {:.2f}-{:.2f} m from the camera", g.dist_min, g.dist_max);
    }
    std::string part = car_dump::RoleName(g.role);
    if (g.role == car_dump::Role::kProxy) {
      part += std::format(" (excluded from the car; because {})", car_dump::ProxyWhyName(g.proxy));
    } else if (g.role == car_dump::Role::kSkinned) {
      part += " (left out of the car's body; written to the skinned OBJ)";
    }
    std::string skin;
    if (g.role == car_dump::Role::kSkinned) {
      skin = std::format("; skin: blended positions for {}/{} draws, {} vertices, {} with a bone outside the palette, {} whose weights do not "
                         "add up to 1 (worst error {:.3f}){}",
                         g.skin_placed, g.draws, g.skin_vertices, g.skin_bad_bone, g.skin_bad_weight, g.skin_weight_error,
                         g.skin_note.empty() ? std::string() : std::format(" [{}]", g.skin_note));
    }
    REXLOG_INFO("{} id {} vs {:016x} ps {:016x}: part {}, recipe {} (from the {}; microcode read in {}/{} draws), colour mask {:#x} "
                "(0 in {} draws), {} draws ({} indexed: {} 16-bit, {} 32-bit; {} auto; {} inline), {}, position {} at offset {} of stream "
                "{}, stride {}{}, {}, bool b0 (damage) on in {}, BLENDINDICES in the declaration of {}, {} before / {} after the first "
                "world draw, {} triangles over {} vertices (+ {} degenerate, {} out of range), {}; {}{}; problems: {}",
                kPrefix, std::get<0>(key), std::get<1>(key), std::get<2>(key), part, car_dump::RecipeName(g.recipe),
                car_dump::RecipeSourceName(g.source), g.ucode_draws, g.draws, g.colour_mask, g.colour_mask_zero, g.draws, g.indexed,
                g.index16, g.index32, g.autos, g.inlines, PrimitiveText(g.primitives), FormatText(g.formats), g.offset, g.stream,
                g.stride, g.stride_varies ? " (varies)" : "", bones, g.damage, g.skinned, g.before, g.after, g.triangles, g.vertices,
                g.degenerate, g.out_of_range, where, wvp, skin, problems.empty() ? "none" : problems);
  }
}

// The OBJ of one part of the chosen car: its body (the rigid parts and the tyres, car_dump_<frame>.obj) or its skinned draws (the
// driver, with the bones blended, car_dump_<frame>_skinned.obj). The wheel proxies are in neither.
void CarDump::WriteObj(uint32_t chosen, car_dump::Role role) const {
  if (chosen == 0) {
    if (role == car_dump::Role::kBody) {
      REXLOG_WARN("{} no car to write", kPrefix);
    }
    return;
  }
  const auto written = [&](const Draw& d) {
    return d.id == chosen && d.role == role && d.triangles != 0 && d.PositionCount() != 0;
  };
  if (std::none_of(draws_.begin(), draws_.end(), written)) {
    return;
  }
  const bool body = role == car_dump::Role::kBody;
  const std::filesystem::path path =
      rex::filesystem::GetExecutableFolder() / std::format("car_dump_{}{}.obj", dump_frame_, body ? "" : "_skinned");
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    REXLOG_ERROR("{} could not open {}", kPrefix, path.string());
    return;
  }
  std::string buffer;
  const auto flush = [&] {
    file.write(buffer.data(), std::streamsize(buffer.size()));
    buffer.clear();
  };
  if (body) {
    std::format_to(std::back_inserter(buffer),
                   "# car dump of guest frame {}: stencil ID {}, the car's body, world space, one group per draw (id_vsHash_psHash_bone). "
                   "Every draw is taken through the recipe its vertex shader's microcode says (the palette's bone c204.x then gWorld for the "
                   "rigid parts, gWorld alone for the tyres); the skinned draws (the driver) are in the _skinned file, the wheel proxies "
                   "in none; the damage branch is not done.\n",
                   dump_frame_, chosen);
  } else {
    std::format_to(std::back_inserter(buffer),
                   "# car dump of guest frame {}: stencil ID {}, the skinned draws (the driver), world space: each vertex is the blend of "
                   "its bones of the palette (BLENDINDICES / BLENDWEIGHT; an unverified decode, see the log's skin counts) taken through "
                   "gWorld.\n",
                   dump_frame_, chosen);
  }
  uint64_t vertex_base = 0;
  uint64_t vertices = 0;
  uint64_t triangles = 0;
  uint32_t groups = 0;
  uint32_t ordinal = 0;
  for (const Draw& d : draws_) {
    ++ordinal;
    if (!written(d)) {
      continue;
    }
    std::format_to(std::back_inserter(buffer), "# draw {}{}{}\n", ordinal - 1, d.skinned ? " skinned" : "", d.damage ? " damage" : "");
    std::format_to(std::back_inserter(buffer), "g {}_{:016x}_{:016x}_{}\n", d.id, d.vs, d.ps,
                   d.matrices.bone_valid && !d.skinned ? std::to_string(d.matrices.bone) : std::string("x"));
    const size_t count = d.PositionCount();
    const float* positions = d.Positions();
    for (size_t v = 0; v < count; ++v) {
      const double p[3] = {double(positions[v * 3]), double(positions[v * 3 + 1]), double(positions[v * 3 + 2])};
      double w[4];
      car_dump::TransformPoint(d.model, p, w);
      const double inverse = std::fabs(w[3]) > 1e-12 ? 1.0 / w[3] : 1.0;
      std::format_to(std::back_inserter(buffer), "v {:.5f} {:.5f} {:.5f}\n", w[0] * inverse, w[1] * inverse, w[2] * inverse);
      if (buffer.size() > (1u << 20)) {
        flush();
      }
    }
    for (size_t t = 0; t + 2 < d.tri.size(); t += 3) {
      std::format_to(std::back_inserter(buffer), "f {} {} {}\n", vertex_base + d.tri[t] + 1, vertex_base + d.tri[t + 1] + 1,
                     vertex_base + d.tri[t + 2] + 1);
      if (buffer.size() > (1u << 20)) {
        flush();
      }
    }
    vertex_base += count;
    vertices += count;
    triangles += d.triangles;
    ++groups;
  }
  flush();
  file.close();
  REXLOG_INFO("{} wrote {}: stencil ID {}, the {}, {} draws as groups, {} vertices, {} triangles", kPrefix, path.string(), chosen,
              body ? "body" : "skinned draws", groups, vertices, triangles);
}

// ---------------------------------------------------------------------------
// the check against the history image
// ---------------------------------------------------------------------------

void CarDump::RunCheck() {
  const std::vector<float>& image = history_.distance;
  if (!camera_valid_) {
    REXLOG_WARN("{} check of frame {}: no camera for the frame (the anchor draw's constants), so the triangles cannot be projected",
                kPrefix, dump_frame_);
    return;
  }
  const uint32_t width = history_.width;
  const uint32_t height = history_.height;
  if (image.size() != size_t(width) * height || width == 0 || height == 0) {
    return;
  }
  uint64_t vehicle_pixels = 0;
  for (float v : image) {
    vehicle_pixels += v > 0.0f;
  }
  // The distance of the image is the lens' linearisation of the stored depth with the PREVIOUS frame's projection; the projection's
  // clip w is that distance only for P23 = -1 and P33 = 0.
  const temporal::Mat4& p = camera_.projection;
  const double lens_difference = std::max({Relative(history_.lens.m[2][2], p.m[2][2]), Relative(history_.lens.m[2][3], p.m[2][3]),
                              Relative(history_.lens.m[3][2], p.m[3][2]), Relative(history_.lens.m[3][3], p.m[3][3])});
  REXLOG_INFO("{} check of frame {}: history image {}x{} (the render size over {}), {} vehicle pixels ({:.2f}% of the image); the "
              "image was linearised with a lens {:.2f}% different from this frame's (largest relative difference of P22, P23, P32, P33); "
              "this frame's P23 = {:.4f}, P33 = {:.4f} (w is the view distance for -1 and 0)",
              kPrefix, dump_frame_, width, height, history_.divisor, vehicle_pixels,
              100.0 * double(vehicle_pixels) / double(size_t(width) * height), 100.0 * lens_difference, p.m[2][3], p.m[3][3]);
  if (vehicle_pixels == 0) {
    REXLOG_WARN("{} check of frame {}: the image has no vehicle pixel (no car in the view, or the history is for another frame): "
                "nothing to compare", kPrefix, dump_frame_);
    return;
  }

  car_dump::Image im;
  im.width = width;
  im.height = height;
  im.sample = car_dump::HistorySample(history_.divisor);
  car_dump::DepthGrid all, body, with_skin;
  all.Reset(width, height);
  body.Reset(width, height);
  with_skin.Reset(width, height);
  // Per (ID, VS, PS): how the draws' own pixels sit against the image under the palette recipe, under gWorld alone and, for a skinned
  // group, under the blend (the recipe the shader's microcode chose is the one drawn into the grids).
  struct GroupFit {
    car_dump::FitCount palette, world, placed;
    car_dump::Recipe recipe = car_dump::Recipe::kPalette;
    car_dump::RecipeSource source = car_dump::RecipeSource::kDefault;
    car_dump::Role role = car_dump::Role::kBody;
    bool seen = false;
  };
  std::map<GroupKey, GroupFit> fits;
  struct Tally {
    uint64_t triangles = 0, off = 0, behind = 0;
    void Add(bool off_image, bool behind_camera) {
      ++triangles;
      off += off_image;
      behind += behind_camera;
    }
  };
  Tally all_tally, body_tally, skin_tally;
  std::vector<car_dump::Vec4> clip_a, clip_b, clip_c;
  for (const Draw& d : draws_) {
    if (d.triangles == 0 || !d.matrices.finite) {
      continue;
    }
    const size_t vertex_count = d.local.size() / 3;
    const bool skinned = d.recipe == car_dump::Recipe::kSkinned;
    // A skinned draw whose bones could not be blended has no placed triangles (the log's skin note says why).
    const bool placed = vertex_count != 0 && d.PositionCount() == vertex_count;
    const bool shown = placed && d.role != car_dump::Role::kProxy;  // the wheel proxies are no geometry of any car
    const bool is_chosen = d.id == chosen_;
    const bool to_body = shown && is_chosen && d.role == car_dump::Role::kBody;
    const bool to_skin = shown && is_chosen;  // the body and its skinned parts
    const temporal::Mat4 to_clip_a = temporal::Mul(d.matrices.model, camera_.view_projection);
    const temporal::Mat4 to_clip_b = temporal::Mul(d.matrices.world, camera_.view_projection);
    clip_a.resize(vertex_count);
    clip_b.resize(vertex_count);
    for (size_t v = 0; v < vertex_count; ++v) {
      const double position[3] = {double(d.local[v * 3]), double(d.local[v * 3 + 1]), double(d.local[v * 3 + 2])};
      clip_a[v] = car_dump::Project(to_clip_a, position);
      clip_b[v] = car_dump::Project(to_clip_b, position);
    }
    if (skinned && placed) {
      // The blended local positions through gWorld (d.model for a skinned draw).
      const temporal::Mat4 to_clip_c = temporal::Mul(d.model, camera_.view_projection);
      clip_c.resize(vertex_count);
      for (size_t v = 0; v < vertex_count; ++v) {
        const double position[3] = {double(d.blended[v * 3]), double(d.blended[v * 3 + 1]), double(d.blended[v * 3 + 2])};
        clip_c[v] = car_dump::Project(to_clip_c, position);
      }
    }
    GroupFit& fit = fits[GroupKey{d.id, d.vs, d.ps}];
    if (!fit.seen) {
      fit.seen = true;
      fit.recipe = d.recipe;
      fit.source = d.recipe_source;
      fit.role = d.role;
    }
    // The pixels of the placed triangles go to the grids of the parts they belong to.
    const auto feed = [&](uint32_t x, uint32_t y, double w) {
      if (shown) {
        all.Splat(x, y, w);
      }
      if (to_body) {
        body.Splat(x, y, w);
      }
      if (to_skin) {
        with_skin.Splat(x, y, w);
      }
    };
    for (size_t t = 0; t + 2 < d.tri.size(); t += 3) {
      const car_dump::Vec4 a[3] = {clip_a[d.tri[t]], clip_a[d.tri[t + 1]], clip_a[d.tri[t + 2]]};
      const car_dump::Vec4 b[3] = {clip_b[d.tri[t]], clip_b[d.tri[t + 1]], clip_b[d.tri[t + 2]]};
      car_dump::Vec4 c[3] = {};
      if (skinned && placed) {
        c[0] = clip_c[d.tri[t]];
        c[1] = clip_c[d.tri[t + 1]];
        c[2] = clip_c[d.tri[t + 2]];
      }
      const car_dump::Vec4* placed_clip = skinned ? c : (d.recipe == car_dump::Recipe::kPalette ? a : b);
      if (shown) {
        const bool off = car_dump::OffImage(placed_clip, im);
        const bool behind = car_dump::BehindCamera(placed_clip);
        all_tally.Add(off, behind);
        if (to_body) {
          body_tally.Add(off, behind);
        }
        if (to_skin) {
          skin_tally.Add(off, behind);
        }
      }
      car_dump::RasterizeClip(im, a, [&](uint32_t x, uint32_t y, double w) {
        fit.palette.Add(w, image[size_t(y) * width + x], kFitTolerance);
        if (d.recipe == car_dump::Recipe::kPalette) {
          feed(x, y, w);
        }
      });
      car_dump::RasterizeClip(im, b, [&](uint32_t x, uint32_t y, double w) {
        fit.world.Add(w, image[size_t(y) * width + x], kFitTolerance);
        if (d.recipe == car_dump::Recipe::kWorld) {
          feed(x, y, w);
        }
      });
      if (skinned && placed) {
        car_dump::RasterizeClip(im, c, [&](uint32_t x, uint32_t y, double w) {
          fit.placed.Add(w, image[size_t(y) * width + x], kFitTolerance);
          feed(x, y, w);
        });
      }
    }
  }
  const auto report = [&](const std::string& what, const car_dump::DepthGrid& grid, const Tally& tally) {
    const car_dump::CheckStats s = car_dump::CompareToDistance(image, grid, kFitTolerance);
    REXLOG_INFO("{} check ({}): {:.2f}% of the {} vehicle pixels are covered by a triangle; depth error over the covered pixels: "
                "mean {:.3f}% / {:.4f} m, median {:.3f}%, 95th percentile {:.3f}% / {:.4f} m, worst {:.1f}%; {:.1f}% within 1%, {:.1f}% "
                "within 2%; the triangle is nearer than the image by more than {:.0f}% on {:.2f}% of them and farther on {:.2f}%; "
                "{} pixels have a triangle and no vehicle (hidden behind the world, or not the car); triangles: {} projected, {} with "
                "all three vertices outside the image ({} of them behind the camera)",
                kPrefix, what, 100.0 * s.coverage, s.vehicle, 100.0 * s.mean_rel, s.mean_abs, 100.0 * s.p50_rel,
                100.0 * s.p95_rel, s.p95_abs, 100.0 * s.max_rel,
                s.covered ? 100.0 * double(s.within_1pct) / double(s.covered) : 0.0,
                s.covered ? 100.0 * double(s.within_2pct) / double(s.covered) : 0.0, 100.0 * kFitTolerance,
                s.covered ? 100.0 * double(s.triangle_nearer) / double(s.covered) : 0.0,
                s.covered ? 100.0 * double(s.triangle_farther) / double(s.covered) : 0.0, s.raster_only, tally.triangles, tally.off,
                tally.behind);
  };
  report(std::format("ID {} body alone: the car's rigid parts and tyres, no skinned part, no wheel proxy", chosen_), body, body_tally);
  if (skin_tally.triangles != body_tally.triangles) {
    report(std::format("ID {} body and its skinned parts (the driver, blended bones; the vehicle pixels include the driver's)", chosen_),
           with_skin, skin_tally);
  }
  report("every ID, no wheel proxies (skinned parts blended)", all, all_tally);
  for (const auto& [key, fit] : fits) {
    const car_dump::FitCount& a = fit.palette;
    const car_dump::FitCount& b = fit.world;
    if (a.samples + a.off + b.samples + b.off == 0) {
      continue;
    }
    const auto text = [](const car_dump::FitCount& f) {
      return std::format("{} / {} (match {:.1f}%, in front {:.1f}%, behind {:.1f}%)", f.samples, f.off, 100.0 * f.Share(f.match),
                         100.0 * f.Share(f.front), 100.0 * f.Share(f.behind));
    };
    REXLOG_INFO("{} fit id {} vs {:016x} ps {:016x}: part {}, recipe chosen {} (from the {}); pixels of the draws' own triangles on a "
                "vehicle pixel / off one: palette then gWorld {}, gWorld alone {}{}; a right recipe matches where the draw is the "
                "visible surface and is next to never in front",
                kPrefix, std::get<0>(key), std::get<1>(key), std::get<2>(key), car_dump::RoleName(fit.role),
                car_dump::RecipeName(fit.recipe), car_dump::RecipeSourceName(fit.source), text(a), text(b),
                fit.role == car_dump::Role::kSkinned ? std::format(", blended bones {}", text(fit.placed)) : std::string());
  }
}

}  // namespace mcla::gpu

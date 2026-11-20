// MCLA GPU renderer -- car headlights as lights. See headlight_rig.h.

#include "headlight_rig.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "car_frame_rules.h"
#include "gpu_class.h"
#include "guest_memory.h"
#include "live_text.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[headlights]";
// Meshes read for their box in one frame (a lens mesh is a few hundred triangles; a car has a handful of lenses, read once and kept).
constexpr uint32_t kBoxBudget = 4;
// Frames a lens mesh's box is kept after it was last seen.
constexpr uint64_t kBoxKeepFrames = 900;
// How many lens draws of one car a frame are kept (the rest are duplicates: the pause menu's and the garage's views draw the car again).
constexpr size_t kLensesPerCar = 32;
// Guest frames between two census lines.
constexpr uint64_t kLogFrames = 300;
// A car whose entity is this near the player's vehicle is the player's.
constexpr double kPlayerRadius = 3.0;
// Lens draws whose car frames (gWorld) are this near are one car's.
constexpr double kSameCar = 0.3;
// Cars a frame keeps (the lens draws of more are dropped).
constexpr size_t kMaxCars = 32;
// The look-at-the-sun frame in the day (one in this many).
constexpr uint64_t kDayProbeFrames = 30;
// How old a headlight point of the game's may be and still count (ms): the hook notes a point every frame it is submitted.
constexpr int kPointAgeMs = 150;
// How far outside the screen a lamp may be and still count as on it (a share of the clip box).
constexpr double kScreenMargin = 1.3;
// Frames a vertex shader that could not be read is left alone before it is tried again (car_frame.cpp's number).
constexpr uint32_t kRecipeRetryFrames = 300;
// Frames the world's sun given by the Scene is trusted without a new one (a time of day does not change in ten seconds; a menu frame gives none).
constexpr uint64_t kWorldSunKeepFrames = 600;

// The frame being built, as the render thread last published it.
HeadlightSet g_set;
std::atomic<bool> g_selected{false};
// The shadow views of the producer (SetLocalShadows), as a copy (the producer's frame changes in place), with the car key of each light of the set they were
// made for: the set that reads them is the next frame's and can be another (headlight_rig_rules.h RemapShadowViews).
larecomp::gpu::LocalShadowFrame g_shadow_frame;
std::array<uint32_t, headlight_rules::kMaxLights> g_shadow_keys{};
uint32_t g_shadow_key_count = 0;
uint32_t g_atlas_texture = headlight_rules::kNoSlot;
uint32_t g_atlas_sampler = headlight_rules::kNoSlot;
uint64_t g_shadow_serial = 0;

LiveText& Setting() {
  static LiveText text("mcla_gpu_headlights");
  return text;
}

// mcla_gpu_headlights with `auto` resolved for this graphics card (gpu_class.h: off on Low, on on Mid and High); a word that is no value counts as `auto`.
bool SettingOn() {
  std::string mode = gpu_class::Resolve(gpu_class::Effect::kHeadlights, Setting().Get());
  if (mode != "on" && mode != "off") {
    mode = std::string(gpu_class::Cell(gpu_class::Effect::kHeadlights, gpu_class::Current()));
  }
  return mode == "on";
}

double Distance(const double a[3], const double b[3]) {
  const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

}  // namespace

const HeadlightSet& CurrentHeadlights() { return g_set; }

namespace headlight_rig {

bool VariantSelected() { return g_selected.load(std::memory_order_relaxed); }

void SetLocalShadows(const larecomp::gpu::LocalShadowFrame* frame, uint32_t atlas_texture_slot, uint32_t atlas_sampler_slot) {
  g_shadow_frame = larecomp::gpu::LocalShadowFrame{};
  g_shadow_key_count = 0;
  g_atlas_texture = headlight_rules::kNoSlot;
  g_atlas_sampler = headlight_rules::kNoSlot;
  if (frame && frame->count != 0 && atlas_texture_slot != headlight_rules::kNoSlot) {
    // Made at the producer's trigger, in the frame whose set is the current one: the lights the views name are those, by place.
    g_shadow_frame = *frame;
    g_shadow_key_count = std::min<uint32_t>(g_set.count, uint32_t(g_shadow_keys.size()));
    for (uint32_t i = 0; i < g_shadow_key_count; ++i) {
      g_shadow_keys[i] = g_set.lights[i].car_key;
    }
    g_atlas_texture = atlas_texture_slot;
    g_atlas_sampler = atlas_sampler_slot;
  }
  ++g_shadow_serial;
}

}  // namespace headlight_rig

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void HeadlightRig::Initialize(GuestMemory& memory) {
  memory_ = &memory;
  enabled_ = false;
  observing_ = false;
  state_logged_ = false;
  frame_ = 0;
  last_night_ = 0;
  world_sun_ = false;
  world_sun_frame_ = 0;
  tracker_.Clear();
  boxes_.clear();
  reads_.Clear();
  ClearPublished();
}

void HeadlightRig::Shutdown() {
  headlight_points::SetWanted(false);
  ClearPublished();
  tracker_.Clear();
  boxes_.clear();
  reads_.Clear();
  world_sun_ = false;
  memory_ = nullptr;
  enabled_ = false;
  observing_ = false;
}

void HeadlightRig::ClearPublished() {
  g_set = HeadlightSet{};
  g_selected.store(false, std::memory_order_relaxed);
  const float origin[3] = {};
  block_ = headlight_rules::MakeConstants(nullptr, 0, origin, nullptr, headlight_rules::kNoSlot, headlight_rules::kNoSlot);
  lamp_count_ = 0;
}

void HeadlightRig::ReadSettings() {
  namespace s = larecomp::settings;
  const bool was_enabled = enabled_;
  enabled_ = memory_ != nullptr && SettingOn();
  headlight_points::SetWanted(enabled_);
  strength_ = float(std::clamp(double(s::mcla_gpu_headlight_strength.Get()), 0.0, 8.0));
  range_ = float(std::clamp(double(s::mcla_gpu_headlight_range.Get()), 10.0, 200.0));
  outer_degrees_ = float(std::clamp(double(s::mcla_gpu_headlight_cone.Get()), 10.0, 70.0));
  night_override_ = int(s::mcla_gpu_headlight_night.Get());
  log_ = s::mcla_gpu_headlight_log.Get();
  if (was_enabled != enabled_ || !state_logged_) {
    state_logged_ = true;
    REXLOG_INFO("{} {} (mcla_gpu_headlights = {}, GPU class {}; live)", kPrefix, enabled_ ? "on" : "off", Setting().Get(),
                gpu_class::ClassName(gpu_class::Current()));
    if (!enabled_) {
      tracker_.Clear();
      ClearPublished();
      night_ = 0.0;
    }
  }
}

void HeadlightRig::BeginFrame() {
  ReadSettings();
  if (!enabled_) {
    observing_ = false;
    return;
  }
  // The one frame in 30 that looks at the sun through the lens draws is only for when the Scene gives no world sun (the occlusion is off, or no frame of it yet).
  const bool world_sun = world_sun_ && frame_ - world_sun_frame_ <= kWorldSunKeepFrames;
  const bool probe = !world_sun && frame_ % kDayProbeFrames == 0;
  observing_ = night_override_ == 1 || (night_override_ != 0 && (night_ > 0.0 || points_on_ || probe));
}

void HeadlightRig::ApplyNightOverride() {
  if (night_override_ == 1) {
    night_ = 1.0;
  } else if (night_override_ == 0) {
    night_ = 0.0;
  }
  if (night_ > 0.0) {
    last_night_ = frame_;
  }
}

void HeadlightRig::SetWorldSun(const double* to_sun) {
  if (!enabled_ || !to_sun) {
    return;
  }
  const double length = std::sqrt(to_sun[0] * to_sun[0] + to_sun[1] * to_sun[1] + to_sun[2] * to_sun[2]);
  if (!std::isfinite(length) || length < 0.5) {
    return;
  }
  const double sine = std::clamp(to_sun[1] / length, -1.0, 1.0);  // the direction TO the sun: its y is the sine of the elevation
  world_sun_ = true;
  world_sun_frame_ = frame_;
  sun_seen_ = true;
  sun_degrees_ = std::asin(sine) * 180.0 / headlight_rules::kPi;
  night_ = headlight_rules::NightFactor(sine);
  ApplyNightOverride();
}

// ---------------------------------------------------------------------------
// the draws
// ---------------------------------------------------------------------------

void HeadlightRig::ReadFrameFacts(const uint8_t* vs_bank, const car_dump::CarMatrices& m) {
  facts_read_ = true;
  // The camera and its view-projection, as car_frame.cpp reads them from a car draw's vertex constants (c12-c15 the view inverse, gWorld^-1 * gWorldViewProj).
  const car_frame_rules::BankCamera camera = car_frame_rules::CameraFromBank(vs_bank, m.world, m.wvp);
  projection_ok_ = camera.valid;
  if (camera.valid) {
    view_projection_ = camera.view_projection;
  }
  const temporal::Mat4 view_inverse = car_dump::BankMatrix(vs_bank, car_frame_rules::kViewInverseRegister);
  camera_ok_ = car_frame_rules::LooksLikeViewInverse(view_inverse);
  if (camera_ok_) {
    for (int c = 0; c < 3; ++c) {
      camera_[c] = view_inverse.m[3][c];
    }
  }
  // The sun: the Scene's world sun when it gave one (SetWorldSun). Else the lens draw's own c16 (the direction its light travels), which is a car's light, not
  // the world's: the headlights run of 2026-11-20 read -5 degrees from it at noon. A fallback only.
  if (!(world_sun_ && frame_ - world_sun_frame_ <= kWorldSunKeepFrames)) {
    const double travel[3] = {double(car_dump::BankFloat(vs_bank, car_frame_rules::kSunRegister, 0)),
                              double(car_dump::BankFloat(vs_bank, car_frame_rules::kSunRegister, 1)),
                              double(car_dump::BankFloat(vs_bank, car_frame_rules::kSunRegister, 2))};
    const double sine = headlight_rules::SunSine(travel);
    sun_seen_ = std::isfinite(sine);
    sun_degrees_ = std::asin(std::clamp(sine, -1.0, 1.0)) * 180.0 / headlight_rules::kPi;
    night_ = sun_seen_ ? headlight_rules::NightFactor(sine) : 0.0;
    ApplyNightOverride();
  }
  // The player's vehicle, once a frame (the guest is held back inside this call).
  car_mesh::ReadPlayerVehicle(&player_, false);
}

// The world position of a lens draw's mesh: the centre of its model-space box (read once from guest memory and kept by the mesh's address) through the
// draw's own bone matrix and gWorld. The bone's origin where the box cannot be had (an inline mesh, an unreadable one: best effort). False when the box is
// still to be read (the frame's budget of reads is used up): the lens is left out of this frame, a wrong centre would put the car's lamp at its root.
bool HeadlightRig::LensCentre(const MainDraw& d, const car_dump::CarMatrices& m, double out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = m.model.m[3][c];
  }
  if (!memory_ || !d.view || !d.vs || !d.state) {
    return true;
  }
  const DrawRecord& r = d.view->record();
  if (r.kind == DrawKind::kInline) {
    return true;
  }
  native_gfx::DeclarationElement element;
  if (!car_mesh::FindElement(*d.view, r, car_mesh::kUsagePosition, &element)) {
    return true;
  }
  const car_mesh::StreamRef stream = car_mesh::ResolveStream(*d.view, r, element.stream);
  if (!stream.ok) {
    return true;
  }
  uint64_t key = car_frame_rules::Mix(uint64_t(stream.address), uint64_t(stream.size));
  key = car_frame_rules::Mix(key, uint64_t(r.index_address));
  key = car_frame_rules::Mix(key, (uint64_t(r.start) << 32) | uint64_t(r.count));
  key = car_frame_rules::Mix(key, uint64_t(uint32_t(r.base_vertex)));
  auto found = boxes_.find(key);
  if (found == boxes_.end()) {
    if (boxes_read_ >= kBoxBudget) {
      return false;
    }
    ++boxes_read_;
    car_mesh::DrawInputs inputs;
    inputs.view = d.view;
    inputs.state = d.state;
    inputs.vs = d.vs;
    inputs.primitive = d.primitive;
    car_mesh::ReadTriangles(*memory_, inputs, scratch_, &mesh_);
    LensBox box;
    if (mesh_.problem == car_mesh::Problem::kNone) {
      box.valid = car_frame_rules::BoxOfMesh(mesh_.local.data(), mesh_.local.size() / 3, mesh_.tri.data(), mesh_.tri.size(), box.lo, box.hi);
    }
    found = boxes_.emplace(key, box).first;
  }
  LensBox& box = found->second;
  box.seen = frame_;
  if (!box.valid) {
    return true;
  }
  const double centre[3] = {0.5 * (double(box.lo[0]) + double(box.hi[0])), 0.5 * (double(box.lo[1]) + double(box.hi[1])),
                            0.5 * (double(box.lo[2]) + double(box.hi[2]))};
  double world[4];
  car_dump::TransformPoint(m.model, centre, world);
  if (!std::isfinite(world[0]) || !std::isfinite(world[1]) || !std::isfinite(world[2])) {
    return true;
  }
  for (int c = 0; c < 3; ++c) {
    out[c] = world[c];
  }
  return true;
}

void HeadlightRig::OnMainDraw(const MainDraw& d) {
  if (!enabled_ || !observing_ || !d.view || !d.banks) {
    return;
  }
  const DrawRecord& r = d.view->record();
  if (r.ps_identity != headlight_rig::kLensPs && r.ps_identity != headlight_rig::kTrafficLensPs) {
    return;
  }
  ++lens_draws_;
  (r.ps_identity == headlight_rig::kLensPs ? lens_player_ps_ : lens_traffic_ps_) += 1;
  if ((d.stencil_ids & ~1u & ((1u << car_dump::kIdSlots) - 1u)) == 0) {
    ++lens_no_id_;  // counted: a lens that writes no vehicle ID (the traffic's, or a menu's view); it is taken all the same
  }
  const uint8_t* bank = d.banks->vs.data();
  const car_dump::CarMatrices m = car_dump::DecodeCarMatrices(bank);
  if (!m.finite) {
    return;
  }
  if (!facts_read_) {
    ReadFrameFacts(bank, m);
  }
  // A lens mesh on a car is placed by its bone: the vertex shader reads the bone palette and gWorld is only the car's root translation, so the car's turn (the
  // direction its lamps face) is in the bone's rotation and gWorld's rotation is the identity for every car (reading it made every lamp face -z). A draw whose shader
  // reads no palette, or whose bone index names none, is a sprite with a matrix of its own: no car frame, no lamp.
  const car_dump::RecipeChoice recipe = car_dump::ChooseRecipe(reads_.Of(r, frame_, kRecipeRetryFrames).reads, false, r.vs_identity);
  if (recipe.recipe != car_dump::Recipe::kPalette || !m.bone_valid) {
    ++lens_no_bone_;
    return;
  }
  const double root[3] = {m.world.m[3][0], m.world.m[3][1], m.world.m[3][2]};
  headlight_rules::Pose pose;
  if (!headlight_rules::PoseFromBone(m.palette.m, root, &pose)) {
    ++lens_no_pose_;
    return;
  }
  CarObservation* car = nullptr;
  for (CarObservation& seen : cars_) {
    if (Distance(seen.pose.position, pose.position) < kSameCar) {
      car = &seen;
      break;
    }
  }
  if (!car) {
    if (cars_.size() >= kMaxCars) {
      return;
    }
    cars_.emplace_back();
    car = &cars_.back();
    car->pose = pose;
  }
  if (car->lenses.size() >= kLensesPerCar) {
    return;
  }
  std::array<double, 3> centre{};
  if (LensCentre(d, m, centre.data())) {
    if (!headlight_rules::LensOffRoot(pose.position, centre.data())) {
      ++lens_at_root_;  // a lamp is at one of the car's ends: this is a sprite's centre, or a mesh whose box was unreadable (the bone's origin)
      return;
    }
    car->lenses.push_back(centre);
  }
}

// ---------------------------------------------------------------------------
// the frame's end
// ---------------------------------------------------------------------------

void HeadlightRig::OnPresent() {
  ++frame_;
  ++frames_;
  if (!enabled_) {
    cars_.clear();
    facts_read_ = false;
    boxes_read_ = 0;
    return;
  }
  // The game's own headlight points of the last moments (the hook is told only while the headlights are on): the lights' state and their exact place.
  headlight_points::Global().Recent(&points_, std::chrono::milliseconds(kPointAgeMs));
  if (night_override_ >= 0) {
    points_.clear();  // a test that forces the lights on or off (mcla_gpu_headlight_night) goes by the sun's rule alone
  }
  points_on_ = false;
  for (const headlight_points::Point& point : points_) {
    points_on_ = points_on_ || point.intensity > headlight_points::kOffIntensity;
  }
  pts_recent_ = uint32_t(points_.size());
  pts_matched_lamps_ = pts_only_lamps_ = pts_disagree_ = 0;
  uint32_t pts_on = 0;
  for (const headlight_points::Point& point : points_) {
    pts_on += point.intensity > headlight_points::kOffIntensity ? 1u : 0u;
  }
  pts_on_ = pts_on;

  // The cars the lens draws showed: the lamp of each, from its front lenses.
  struct LensLamp {
    headlight_rules::Pose pose;
    headlight_rules::CarLamp lamp;
  };
  std::vector<LensLamp> lens_lamps;
  uint32_t cars = 0;
  for (const CarObservation& car : cars_) {
    ++cars;
    const headlight_rules::Pose& pose = car.pose;
    const size_t lens_count = car.lenses.size();
    headlight_rules::CarLamp lamp;
    const bool front = lens_count != 0 && headlight_rules::FrontLamp(pose, reinterpret_cast<const double(*)[3]>(car.lenses.data()), lens_count, &lamp);
    if (log_ && cars <= 8 && frame_ - last_log_ >= kLogFrames) {
      REXLOG_INFO("{}   car at ({:.1f}, {:.1f}, {:.1f}) facing ({:.2f}, {:.2f}): {} lens draws, {}", kPrefix, pose.position[0], pose.position[1], pose.position[2],
                  -pose.back[0], -pose.back[2], lens_count,
                  front ? std::format("{} on the front half (lens spread {:.2f} m along the car, lamp at ({:.1f}, {:.1f}, {:.1f}))", lamp.front, lamp.spread,
                                      lamp.position[0], lamp.position[1], lamp.position[2])
                        : std::string("no front lens"));
    }
    if (front) {
      lens_lamps.push_back({pose, lamp});
    }
  }
  // The points go to the nearest lens lamp within 3 m; what none took and has a direction (a racer's or the player's) is a lamp of its own.
  std::vector<std::array<double, 3>> lamp_places;
  for (const LensLamp& l : lens_lamps) {
    lamp_places.push_back({l.lamp.position[0], l.lamp.position[1], l.lamp.position[2]});
  }
  std::vector<bool> taken;
  const std::vector<headlight_rules::PointMatch> matches = headlight_rules::AssignPoints(
      points_, reinterpret_cast<const double(*)[3]>(lamp_places.data()), lamp_places.size(), &taken);
  uint32_t lit = 0;
  const auto observe = [&](headlight_rules::Candidate candidate) {
    if (candidate.guest && !(candidate.on > 0.0)) {
      return;  // the game says this car's lights are off: no light, and no place among the four (a track that was on fades out)
    }
    candidate.camera_distance = camera_ok_ ? Distance(candidate.lamp.position, camera_) : 50.0;
    candidate.on_screen = true;
    if (projection_ok_) {
      double clip[4];
      car_dump::TransformPoint(view_projection_, candidate.lamp.position, clip);
      candidate.on_screen = clip[3] > 0.0 && std::fabs(clip[0]) <= kScreenMargin * clip[3] && std::fabs(clip[1]) <= kScreenMargin * clip[3];
    }
    tracker_.Observe(candidate);
    ++lit;
  };
  for (size_t i = 0; i < lens_lamps.size(); ++i) {
    headlight_rules::Candidate candidate;
    candidate.lamp = lens_lamps[i].lamp;
    candidate.player = player_.valid && facts_read_ && Distance(lens_lamps[i].pose.position, player_.position) < kPlayerRadius;
    if (matches[i].count != 0) {
      bool disagrees = false;
      headlight_rules::ApplyPoints(matches[i], &candidate.lamp, &candidate.on, &disagrees);
      candidate.guest = true;
      candidate.player = candidate.player || matches[i].player;
      ++pts_matched_lamps_;
      pts_disagree_ += disagrees ? 1u : 0u;
    }
    observe(candidate);
  }
  for (const headlight_rules::PointLamp& point_lamp : headlight_rules::LampsFromPoints(points_, taken)) {
    headlight_rules::Candidate candidate;
    candidate.lamp = point_lamp.lamp;
    candidate.player = point_lamp.player;
    candidate.guest = true;
    candidate.on = point_lamp.on;
    ++pts_only_lamps_;
    observe(candidate);
  }
  cars_.clear();
  last_cars_ = cars;
  last_cars_lit_ = lit;
  std::array<headlight_rules::Tracked, headlight_rules::kMaxLights> tracked{};
  const size_t count = tracker_.EndFrame(frame_, &tracked);
  Publish(tracked, count);
  if (night_ > 0.0) {
    ++night_frames_;
  }
  if (log_) {
    Log(g_set.count);
  }
  facts_read_ = false;
  boxes_read_ = 0;
  if (frame_ % 128 == 0) {
    for (auto it = boxes_.begin(); it != boxes_.end();) {
      it = frame_ - it->second.seen > kBoxKeepFrames ? boxes_.erase(it) : std::next(it);
    }
  }
}

void HeadlightRig::Publish(const std::array<headlight_rules::Tracked, headlight_rules::kMaxLights>& tracked, size_t count) {
  namespace rules = headlight_rules;
  HeadlightSet set{};
  lamp_count_ = 0;
  const float cos_outer = rules::CosOfDegrees(outer_degrees_);
  const float cos_inner = rules::CosOfDegrees(outer_degrees_ * (rules::kInnerDegrees / rules::kOuterDegrees));
  const float night = float(night_);
  for (size_t i = 0; i < count && set.count < set.lights.size(); ++i) {
    const rules::Tracked& t = tracked[i];
    const float scale = strength_ * t.fade * (t.guest ? float(t.on) : night);  // the game's own light points say whether they are on; else the sun does
    if (!(scale > 0.002f)) {
      continue;
    }
    Headlight& h = set.lights[set.count];
    rules::CarLamp lamp;
    for (int c = 0; c < 3; ++c) {
      lamp.forward[c] = t.forward[c];
      lamp.up[c] = t.up[c];
    }
    rules::BeamAxis(lamp, h.dir);
    // The car's two headlamps, for the shadow views (local_shadow.h): half the pair's span across the car, from the game's two points or the lenses, else the default.
    rules::LampOffset(t.forward, t.up, t.half_span, h.lamp_offset);
    for (int c = 0; c < 3; ++c) {
      h.pos[c] = float(t.position[c]);
      h.color[c] = rules::kColor[c] * scale;
    }
    h.cos_outer = cos_outer;
    h.cos_inner = cos_inner;
    h.range = range_;
    h.car_key = t.key;
    h.is_player = t.player;
    rules::Lamp& l = lamps_[lamp_count_++];
    for (int c = 0; c < 3; ++c) {
      l.position[c] = h.pos[c];
      l.direction[c] = h.dir[c];
      l.color[c] = h.color[c];
    }
    l.range = h.range;
    l.cos_outer = h.cos_outer;
    l.cos_inner = h.cos_inner;
    ++set.count;
  }
  g_set = set;
  g_selected.store(set.count > 0, std::memory_order_relaxed);
  RebuildBlock();
}

void HeadlightRig::RebuildBlock() {
  const float camera[3] = {float(camera_[0]), float(camera_[1]), float(camera_[2])};
  larecomp::gpu::LocalShadowFrame views;  // the producer's, moved to the lights of this set by their car
  const larecomp::gpu::LocalShadowFrame* shadows = nullptr;
  if (g_shadow_frame.count != 0) {
    std::array<uint32_t, headlight_rules::kMaxLights> keys{};
    const uint32_t count = std::min<uint32_t>(g_set.count, uint32_t(keys.size()));
    for (uint32_t i = 0; i < count; ++i) {
      keys[i] = g_set.lights[i].car_key;
    }
    views = headlight_rules::RemapShadowViews(g_shadow_frame, g_shadow_keys.data(), g_shadow_key_count, keys.data(), count);
    shadows = views.count != 0 ? &views : nullptr;
  }
  block_ = headlight_rules::MakeConstants(lamps_.data(), lamp_count_, camera, shadows, g_atlas_texture, g_atlas_sampler);
  shadow_serial_ = g_shadow_serial;
}

const headlight_rules::HeadlightConstants& HeadlightRig::Block() {
  if (shadow_serial_ != g_shadow_serial) {
    RebuildBlock();  // a producer set its views since the block was made (the frame's, set after the present)
  }
  return block_;
}

void HeadlightRig::Log(size_t picked) {
  if (frame_ - last_log_ < kLogFrames) {
    return;
  }
  last_log_ = frame_;
  std::string lights;
  for (uint32_t i = 0; i < g_set.count; ++i) {
    const Headlight& h = g_set.lights[i];
    lights += std::format("{}{} key {} at ({:.1f}, {:.1f}, {:.1f}) axis ({:.2f}, {:.2f}, {:.2f}) colour {:.2f}", i ? "; " : "", h.is_player ? "player" : "car",
                          h.car_key, h.pos[0], h.pos[1], h.pos[2], h.dir[0], h.dir[1], h.dir[2], h.color[0]);
  }
  REXLOG_INFO("{} frame {}: night {:.2f} (the {} sun {} at {:+.1f} degrees{}), {} lens draws in {} frames' windows (player's lens {}, traffic's {}, no vehicle ID {}, "
              "no pose {}, left out: no bone palette {}, at the car's root {}), game's light points {} recent ({} lit, {} notes since the start), lens lamps with points {}, lamps from points alone {}, forward turned by the points {}, {} cars seen, {} with a front lens, {} lights ({}), {} variant draws ({} with the alpha test, {} more drew the game's own shader while the variant's "
              "pipeline was being made), {} lens boxes kept",
              kPrefix, frame_, night_, world_sun_ && frame_ - world_sun_frame_ <= kWorldSunKeepFrames ? "world's" : "lens draw's", sun_seen_ ? "seen" : "NOT seen",
              sun_degrees_, night_override_ >= 0 ? ", overridden" : "", lens_draws_, kLogFrames,
              lens_player_ps_, lens_traffic_ps_, lens_no_id_, lens_no_pose_, lens_no_bone_, lens_at_root_, pts_recent_, pts_on_, headlight_points::Global().notes(), pts_matched_lamps_,
              pts_only_lamps_, pts_disagree_, last_cars_, last_cars_lit_, picked,
              lights.empty() ? std::string("none") : lights, variant_draws_, variant_alpha_draws_, fallback_draws_, boxes_.size());
  lens_draws_ = lens_player_ps_ = lens_traffic_ps_ = lens_no_pose_ = lens_no_id_ = lens_no_bone_ = lens_at_root_ = 0;
  variant_draws_ = variant_alpha_draws_ = fallback_draws_ = 0;
}

}  // namespace mcla::gpu

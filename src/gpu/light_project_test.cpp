// Check of the consumers' shapes as projections of the LightFrame (light_project.h, LIGHT-UNIFY U4 = package LU-4): for every flip, the bytes a consumer receives from the frame are the
// bytes its own producer made, over recorded frames. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_light_project_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_light_project_test [<path>.rig.txt]   (exit 0 = passed)
// or by hand: clang++ -std=c++23 -I src/gpu src/gpu/light_project_test.cpp -o light_project_test
//
// THE RECORDED FRAMES. A drive of 1500 guest frames at 60 FPS: a player's car and eleven others (traffic ahead and oncoming, cars that cross, that vanish, whose lights go off for a
// stretch), a camera behind the player. The headlight rig's REAL tracker (headlight_rules::Tracker, the unified picker) turns what the lens draws showed into the rig's set of each frame
// (the same arithmetic as HeadlightRig::Publish); the shadow replay's REAL rules (local_shadow::ViewPicker, PlanViews, and the publication loop of LocalShadow::RecordMaps) make the maps
// of each frame at each of the three levels; and every frame goes through the manager's own steps (the Collector at the frame's start, the last trigger's maps moved to this set by the
// car, the trigger's own, the street lamps picked by the volume's real LampPicker) in the order the Scene calls them. Given a file written by the game's recorder
// (mcla_gpu_light_pick_record = <path>, <path>.rig.txt) the same chain runs over THOSE frames too (the tracker's inputs of a real night drive).
//   4a  the headlight block: ProjectHeadlightBlock of the frame against HeadlightRig's block (MakeConstants over the rig's lamps and the remapped maps), 1072 bytes, before the trigger and
//       after it, at every level, over every frame; the deliberate differences of every part of the block are found.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "headlight_rig_rules.h"
#include "headlight_types.h"
#include "light_budget.h"
#include "light_convert.h"
#include "light_frame.h"
#include "light_manager_rules.h"
#include "light_pick_record.h"
#include "light_project.h"
#include "local_shadow_rules.h"
#include "local_shadow_types.h"

namespace lg = larecomp::gpu;
namespace lm = larecomp::gpu::light_manager;
namespace lp = mcla::gpu::light_project;
namespace hr = mcla::gpu::headlight_rules;
namespace ls = mcla::gpu::local_shadow;
namespace lb = mcla::gpu::light_budget;
namespace rec = mcla::gpu::light_pick::record;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool SameBytes(const void* a, const void* b, size_t bytes) { return std::memcmp(a, b, bytes) == 0; }

// ---------------------------------------------------------------------------
// the drive: what the lens draws showed, frame by frame
// ---------------------------------------------------------------------------

struct SimCar {
  double start[3] = {};
  double velocity[3] = {};
  double forward[3] = {1, 0, 0};
  bool player = false;
  int seen_from = 0, seen_to = 1 << 30;  // the frames in which the game draws it (and the rig sees its lamps)
  int off_from = -1, off_to = -1;        // the frames its lights are off (the game's own light points say so)
  double half_span = 0.55;
};

std::vector<SimCar> MakeDrive() {
  std::vector<SimCar> cars;
  const auto add = [&](double x, double z, double vx, double vz, double fx, double fz) {
    SimCar c;
    c.start[0] = x;
    c.start[1] = 0.65;
    c.start[2] = z;
    c.velocity[0] = vx;
    c.velocity[2] = vz;
    c.forward[0] = fx;
    c.forward[2] = fz;
    c.half_span = 0.5 + 0.02 * double(cars.size() % 6);
    cars.push_back(c);
    return &cars.back();
  };
  add(0, 0, 18, 0, 1, 0)->player = true;
  for (int k = 0; k < 4; ++k) {  // traffic ahead, two lanes
    add(30.0 + 24.0 * k, k % 2 ? 3.6 : -3.6, 11.0 + 1.5 * (k % 3), 0, 1, 0);
  }
  for (int k = 0; k < 3; ++k) {  // oncoming
    SimCar* c = add(160.0 + 50.0 * k, k % 2 ? 8.0 : 11.5, -15.0 - k, 0, -1, 0);
    c->seen_from = 20 * k;
  }
  SimCar* crossing = add(70.0, -40.0, 0.0, 9.0, 0, 1);  // crosses the road
  crossing->seen_from = 100;
  crossing->seen_to = 700;
  SimCar* vanish = add(95.0, -3.6, 13.0, 0, 1, 0);  // vanishes in the middle of the picture
  vanish->seen_to = 520;
  SimCar* off = add(40.0, 7.0, 10.0, 0, 1, 0);  // lights off for a stretch
  off->off_from = 300;
  off->off_to = 520;
  SimCar* late = add(10.0, -7.5, 24.0, 0, 1, 0);  // comes in late and passes the player
  late->seen_from = 700;
  return cars;
}

constexpr double kFps = 60.0;

// The rig's set of a frame from what the tracker made (HeadlightRig::Publish's arithmetic, strength 1, night 1).
mcla::gpu::HeadlightSet PublishSet(const std::array<hr::Tracked, hr::kMaxLights>& tracked, size_t count) {
  mcla::gpu::HeadlightSet set;
  set.count = 0;
  const float cos_outer = hr::CosOfDegrees(hr::kOuterDegrees);
  const float cos_inner = hr::CosOfDegrees(hr::kOuterDegrees * (hr::kInnerDegrees / hr::kOuterDegrees));
  for (size_t i = 0; i < count && set.count < set.lights.size(); ++i) {
    const hr::Tracked& t = tracked[i];
    const float scale = t.fade * (t.guest ? float(t.on) : 1.0f);
    if (!(scale > 0.002f)) {
      continue;
    }
    mcla::gpu::Headlight& h = set.lights[set.count];
    h = mcla::gpu::Headlight{};
    hr::CarLamp lamp;
    for (int c = 0; c < 3; ++c) {
      lamp.forward[c] = t.forward[c];
      lamp.up[c] = t.up[c];
    }
    hr::BeamAxis(lamp, h.dir);
    hr::LampOffset(t.forward, t.up, t.half_span, h.lamp_offset);
    for (int c = 0; c < 3; ++c) {
      h.pos[c] = float(t.position[c]);
      h.color[c] = hr::kColor[c] * scale;
    }
    h.cos_outer = cos_outer;
    h.cos_inner = cos_inner;
    h.range = hr::kRange;
    h.car_key = t.key;
    h.is_player = t.player;
    ++set.count;
  }
  return set;
}

struct Camera {
  double eye[3] = {};
};

// One frame of the drive: the set the rig made, the camera.
struct DriveFrame {
  uint64_t frame = 0;
  mcla::gpu::HeadlightSet set;
  Camera camera;
};

std::vector<DriveFrame> RunDrive(const std::vector<SimCar>& drive, int frames) {
  std::vector<DriveFrame> out;
  hr::Tracker tracker;
  std::array<hr::Tracked, hr::kMaxLights> tracked{};
  for (int f = 1; f <= frames; ++f) {
    const double t = double(f) / kFps;
    Camera cam;
    cam.eye[0] = drive[0].start[0] + drive[0].velocity[0] * t - 6.0;
    cam.eye[1] = 2.4;
    cam.eye[2] = 0.0;
    // two stretches in which the rig sees no car at all (a tunnel, the garage): the lights fade out and the set is empty
    const bool blackout = (f >= 200 && f < 230) || f >= 1320;
    for (size_t i = 0; i < drive.size() && !blackout; ++i) {
      const SimCar& car = drive[i];
      if (f < car.seen_from || f >= car.seen_to) {
        continue;
      }
      hr::Candidate c;
      for (int k = 0; k < 3; ++k) {
        c.lamp.position[k] = car.start[k] + car.velocity[k] * t;
        c.lamp.forward[k] = car.forward[k];
      }
      c.lamp.up[1] = 1.0;
      c.lamp.half_span = car.half_span;
      c.player = car.player;
      const double d[3] = {c.lamp.position[0] - cam.eye[0], c.lamp.position[1] - cam.eye[1], c.lamp.position[2] - cam.eye[2]};
      c.camera_distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      c.on_screen = std::fabs(d[2]) < 0.5 * std::fabs(d[0]) + 8.0 && c.camera_distance < 200.0;
      c.guest = true;
      c.on = (car.off_from >= 0 && f >= car.off_from && f < car.off_to) ? 0.0 : 1.0;
      if (c.camera_distance < 190.0) {
        tracker.Observe(c);
      }
    }
    tracker.SetBeam(double(hr::kRange), double(hr::kOuterDegrees));
    const size_t n = tracker.EndFrame(uint64_t(f), &tracked, nullptr, t);
    DriveFrame df;
    df.frame = uint64_t(f);
    df.set = PublishSet(tracked, n);
    df.camera = cam;
    out.push_back(df);
  }
  return out;
}

// The recorded frames of the game's own recorder (mcla_gpu_light_pick_record): the tracker's inputs, replayed through the real tracker into sets.
std::vector<DriveFrame> RunRecording(const std::vector<rec::RigFrame>& frames) {
  std::vector<DriveFrame> out;
  hr::Tracker tracker;
  std::array<hr::Tracked, hr::kMaxLights> tracked{};
  for (const rec::RigFrame& f : frames) {
    for (const hr::Candidate& c : f.cars) {
      tracker.Observe(c);
    }
    tracker.SetBeam(f.beam_range, f.beam_outer_degrees);
    const size_t n = tracker.EndFrame(f.frame, &tracked, f.has_view ? &f.frustum : nullptr, f.now_s);
    DriveFrame df;
    df.frame = f.frame;
    df.set = PublishSet(tracked, n);
    out.push_back(df);
  }
  return out;
}

// ---------------------------------------------------------------------------
// the shadow replay's maps, as LocalShadow makes them
// ---------------------------------------------------------------------------

struct Replay {
  ls::ViewPicker picker;
  lb::Level level = lb::Level::kHigh;
  int views_override = 0;
  bool grouped = true;  // every frame it published had each light's views one after the other
};

lg::LocalShadowView MakeView(uint32_t light_index, uint64_t frame, uint32_t view, uint32_t views) {
  lg::LocalShadowView v{};
  for (int i = 0; i < 16; ++i) {
    v.world_to_shadow[i] = 0.01f * float((frame * 7 + view * 13 + uint64_t(i) * 3) % 997);
  }
  const float w = 1.0f / float(std::max(views, 1u));
  v.atlas_rect[0] = w * float(view);
  v.atlas_rect[1] = 0.0f;
  v.atlas_rect[2] = w * float(view + 1);
  v.atlas_rect[3] = 0.5f;
  v.depth_bias = 0.002f;
  v.texel_size = 1.0f / 512.0f;
  v.light_index = light_index;
  v.valid = 1;
  return v;
}

// LocalShadow::PickViews' lights from the rig's set, the pick and the plan, and RecordMaps' publication of one entry for each light each planned view covers.
lg::LocalShadowFrame PublishViews(Replay* r, const mcla::gpu::HeadlightSet& set, const Camera& cam, uint64_t frame) {
  ls::Light lights[4];
  uint32_t count = 0;
  for (uint32_t i = 0; i < set.count && count < 4; ++i) {
    const mcla::gpu::Headlight& h = set.lights[i];
    if (ls::MakeLight(h.pos, h.dir, h.cos_outer, h.range, h.car_key, h.is_player, i, &lights[count], h.lamp_offset)) {
      ++count;
    }
  }
  ls::PickContext context;
  context.have_reference = true;
  for (int k = 0; k < 3; ++k) {
    context.reference[k] = cam.eye[k];
  }
  const lb::LightBudget budget = lb::BudgetFor(true, r->level, r->views_override, lb::Level::kOff, ls::kLevelTable);
  ls::Group groups[4];
  const uint32_t cars = r->picker.Pick(lights, count, budget.shadow_cars, context, groups);
  ls::ViewPlan plan;
  ls::PlanViews(lights, groups, cars, budget.shadow_lamp_cars, budget.shadow_views, &plan);
  lg::LocalShadowFrame out;
  for (uint32_t v = 0; v < plan.count; ++v) {
    const ls::ViewSlot& view = plan.views[v];
    for (uint32_t m = 0; m < view.group.count && out.count < out.views.size(); ++m) {
      out.views[out.count++] = MakeView(view.lights[view.group.members[m]].index, frame, v, plan.count);
    }
  }
  // each light's views one after the other?
  std::array<bool, 8> closed{};
  uint32_t last = ~0u;
  for (uint32_t e = 0; e < out.count; ++e) {
    const uint32_t li = out.views[e].light_index;
    if (li != last) {
      if (li < closed.size() && closed[li]) {
        r->grouped = false;
      }
      if (last < closed.size()) {
        closed[last] = true;
      }
      last = li;
    }
  }
  return out;
}

lg::ShadowAtlasInfo MakeAtlas(uint32_t slot = 41) {
  lg::ShadowAtlasInfo a;
  a.valid = true;
  a.slot = slot;
  a.sampler = 2;  // descriptors.h kSamplerPointClamp
  a.width = 1024;
  a.height = 1024;
  a.tile_small = 512;
  return a;
}

std::array<uint32_t, 4> KeysOf(const mcla::gpu::HeadlightSet& set) {
  std::array<uint32_t, 4> k{};
  for (uint32_t i = 0; i < set.count && i < 4; ++i) {
    k[i] = set.lights[i].car_key;
  }
  return k;
}

// The rig's block exactly as HeadlightRig::Publish / RebuildBlock make it (the same as light_manager_test.cpp's RigBlock).
hr::HeadlightConstants RigBlock(const mcla::gpu::HeadlightSet& set, const lg::LocalShadowFrame* remapped, uint32_t atlas_slot, uint32_t sampler, const float camera[3]) {
  std::array<hr::Lamp, hr::kMaxLights> lamps{};
  for (uint32_t i = 0; i < set.count; ++i) {
    for (int c = 0; c < 3; ++c) {
      lamps[i].position[c] = set.lights[i].pos[c];
      lamps[i].direction[c] = set.lights[i].dir[c];
      lamps[i].color[c] = set.lights[i].color[c];
    }
    lamps[i].range = set.lights[i].range;
    lamps[i].cos_outer = set.lights[i].cos_outer;
    lamps[i].cos_inner = set.lights[i].cos_inner;
  }
  return hr::MakeConstants(lamps.data(), set.count, camera, remapped != nullptr && remapped->count != 0 ? remapped : nullptr, atlas_slot, sampler);
}

const auto kRemap = [](const lg::LocalShadowFrame& f, const uint32_t* ok, uint32_t oc, const uint32_t* nk, uint32_t nc) { return hr::RemapShadowViews(f, ok, oc, nk, nc); };

// ---------------------------------------------------------------------------
// the manager's frame, step by step as the Scene calls it, with every consumer's legacy data beside it
// ---------------------------------------------------------------------------

struct Totals {
  uint64_t frames = 0;
  uint64_t lit_frames = 0;           // frames with at least one light
  uint64_t shadow_frames = 0;        // frames with at least one view read by a light
  uint64_t two_view_frames = 0;      // frames with a light that reads two views
  uint64_t block_checks = 0, block_bad = 0;
  uint64_t block_before_with_views = 0;
  uint64_t not_grouped = 0;
  std::string first;
};

// Drives `frames` through the Collector as the manager does and compares the headlight block of every frame, at its start and after the trigger, byte for byte.
Totals RunBlocks(const std::vector<DriveFrame>& frames, lb::Level level, int views_override, uint32_t atlas_slot) {
  Totals t;
  Replay replay;
  replay.level = level;
  replay.views_override = views_override;
  lm::Collector c;
  lm::ShadowCarry<lg::LocalShadowFrame> carry;
  c.Reset();
  carry.Clear();
  const lg::ShadowAtlasInfo atlas = MakeAtlas(atlas_slot);
  bool had_trigger = false;
  for (const DriveFrame& f : frames) {
    ++t.frames;
    const mcla::gpu::HeadlightSet& set = f.set;
    const float camera[3] = {float(f.camera.eye[0]), float(f.camera.eye[1]), float(f.camera.eye[2])};
    // Scene::BeginFrame: the rig's lights (BeginLights), the replay (it gives its maps up when the frame before had no trigger), then the last trigger's maps (BeginFrame).
    c.BeginFrame(uint32_t(f.frame), double(f.frame) / kFps, set);
    if (!had_trigger) {
      carry.Clear();
    }
    const std::array<uint32_t, 4> keys = KeysOf(set);
    const lg::LocalShadowFrame moved = carry.ForSet(keys.data(), set.count, kRemap);
    c.SetShadows(moved.count != 0 ? &moved : nullptr, carry.has() ? carry.atlas() : lg::ShadowAtlasInfo{});
    const auto compare = [&](const lg::LocalShadowFrame* maps, uint32_t slot, const char* when) {
      const hr::HeadlightConstants legacy = RigBlock(set, maps, slot, atlas.sampler, camera);
      const hr::HeadlightConstants projected = lp::ProjectHeadlightBlock(c.frame(), camera);
      lm::SelfCheck sc;
      lp::CheckBlockBytes(c.frame(), legacy, lm::Check::kBlockBytes, &sc);
      const bool same = SameBytes(&legacy, &projected, sizeof(legacy));
      ++t.block_checks;
      if (!same || sc.total_bad() != 0) {
        ++t.block_bad;
        if (t.first.empty()) {
          t.first = std::string(when) + " of frame " + std::to_string(f.frame) + ": " + sc.TakeWindow().first;
        }
      }
      Check(same == (sc.total_bad() == 0), "the check agrees with a byte compare of the two blocks");
      return legacy;
    };
    const hr::HeadlightConstants before = compare(moved.count != 0 ? &moved : nullptr, carry.has() ? atlas.slot : hr::kNoSlot, "start");
    t.block_before_with_views += before.shadow_of[0] != hr::kNoView ? 1u : 0u;
    // the trigger: this frame's maps, except in the frames the replay makes none (a menu, a frame of a day: every 37th)
    const bool trigger = set.count > 0 && f.frame % 37 != 0;
    had_trigger = trigger;
    hr::HeadlightConstants after = before;
    if (trigger) {
      const lg::LocalShadowFrame made = PublishViews(&replay, set, f.camera, f.frame);
      carry.Set(made.count != 0 ? &made : nullptr, atlas, keys.data(), set.count);
      c.SetShadows(made.count != 0 ? &made : nullptr, made.count != 0 ? atlas : lg::ShadowAtlasInfo{});
      after = compare(made.count != 0 ? &made : nullptr, made.count != 0 ? atlas.slot : hr::kNoSlot, "trigger");
    } else if (set.count == 0) {
      carry.Clear();
    }
    t.lit_frames += set.count > 0 ? 1u : 0u;
    bool any = false, two = false;
    for (uint32_t i = 0; i < 4; ++i) {
      any = any || after.shadow_of[i] != hr::kNoView;
      two = two || after.shadow_of_b[i] != hr::kNoView;
    }
    t.shadow_frames += any ? 1u : 0u;
    t.two_view_frames += two ? 1u : 0u;
    lm::SelfCheck inv;
    lm::CheckFrameInvariants(c.frame(), c.has_previous() ? &c.previous() : nullptr, &inv);
    Check(inv.total_bad() == 0, "the frame's own invariants hold in every frame of the drive");
    c.EndFrame();
  }
  t.not_grouped = replay.grouped ? 0u : 1u;
  return t;
}

const char* LevelName(lb::Level l) {
  switch (l) {
    case lb::Level::kMedium: return "medium";
    case lb::Level::kHigh: return "high";
    case lb::Level::kUltra: return "ultra";
    default: return "off";
  }
}

void TestBlockOverTheDrive(const std::vector<DriveFrame>& frames, const std::string& name) {
  for (const lb::Level level : {lb::Level::kMedium, lb::Level::kHigh, lb::Level::kUltra}) {
    const Totals t = RunBlocks(frames, level, 0, 41);
    const std::string at = " (" + name + ", " + LevelName(level) + ")";
    std::printf("INFO: %s, %s: %llu frames, %llu lit, %llu with a light reading a view, %llu with a light reading two, %llu compares of the 1072 byte block\n", name.c_str(), LevelName(level),
                (unsigned long long)t.frames, (unsigned long long)t.lit_frames, (unsigned long long)t.shadow_frames, (unsigned long long)t.two_view_frames,
                (unsigned long long)t.block_checks);
    Check(t.lit_frames > frames.size() / 3 && (t.lit_frames < frames.size() || name != "the made-up drive"), "block bytes: the drive has lit frames to compare" + at + ": " + std::to_string(t.lit_frames) + " of " +
                                                                                               std::to_string(t.frames));
    Check(t.block_bad == 0, "block bytes: the projection of the frame is the rig's 1072 bytes in each of " + std::to_string(t.block_checks) + " compares" + at + ", " +
                                std::to_string(t.block_bad) + " differed: " + t.first);
    Check(t.shadow_frames > t.lit_frames / 2, "block bytes: most lit frames have a light reading a view" + at + ": " + std::to_string(t.shadow_frames));
    if (level != lb::Level::kMedium) {
      Check(t.two_view_frames > 0, "block bytes: a light that reads two views (a view for each headlamp) was seen" + at);
    }
    Check(t.not_grouped == 0, "block bytes: the replay's rules publish a light's views one after the other at every level (the frame's grouping keeps their order)" + at);
  }
  // an atlas slot that is another number, and the debug setting that gives every car a view for each lamp or one view
  for (const int override_views : {1, 2}) {
    const Totals t = RunBlocks(frames, lb::Level::kHigh, override_views, 7);
    Check(t.block_bad == 0 && t.block_checks > 0, "block bytes: the same with " + std::to_string(override_views) + " view(s) a car forced and another atlas slot (" + t.first + ")");
  }
}

// ---------------------------------------------------------------------------
// the pieces
// ---------------------------------------------------------------------------

mcla::gpu::HeadlightSet MakeSet(uint32_t n, uint32_t first_key = 10) {
  mcla::gpu::HeadlightSet set;
  for (uint32_t i = 0; i < n && i < 4; ++i) {
    mcla::gpu::Headlight h{};
    h.pos[0] = 10.0f * float(i) + 0.25f;
    h.pos[1] = 0.7f;
    h.pos[2] = 3.0f - float(i);
    h.dir[0] = 0.0f;
    h.dir[1] = -0.0349f;
    h.dir[2] = 0.9994f;
    h.cos_outer = hr::CosOfDegrees(hr::kOuterDegrees);
    h.cos_inner = hr::CosOfDegrees(hr::kInnerDegrees);
    h.range = hr::kRange;
    h.color[0] = 1.0f * (0.5f + 0.1f * float(i));
    h.color[1] = 0.93f;
    h.color[2] = 0.78f;
    h.car_key = first_key + i;
    h.is_player = i == 0;
    h.lamp_offset[0] = 0.55f;
    set.lights[set.count++] = h;
  }
  return set;
}

void TestRigLightsOf() {
  lm::Collector c;
  c.Reset();
  c.BeginFrame(1, 1.0, MakeSet(3));
  lp::RigLights rig = lp::RigLightsOf(c.frame());
  Check(rig.count == 3, "rig lights: three headlights in the frame");
  bool in_order = rig.count == 3;
  for (uint32_t i = 0; i < rig.count; ++i) {
    in_order = in_order && rig.slot[i] == c.rig().slot[i];
  }
  Check(in_order, "rig lights: in the rig's order, the slots the collector gave");
  // street lamps in the volume's list are not rig lights; the rig's lights come after nothing else
  lg::Light lamp;
  lamp.id = lg::light_id::LampId(lamp.pos);
  lamp.source = lg::LightSource::kLamp;
  lamp.roles = lg::kRoleMaskGameLit;
  c.mutable_frame_for_tests().lights[20] = lamp;
  c.mutable_frame_for_tests().count = 21;
  rig = lp::RigLightsOf(c.frame());
  Check(rig.count == 3, "rig lights: a street lamp in the frame is not one");
  // the order of the surface ranking is what counts: reversed, the block's lights follow it
  lg::LightFrame f = c.frame();
  std::swap(f.order[uint32_t(lg::LightRole::kSurface)][0], f.order[uint32_t(lg::LightRole::kSurface)][2]);
  const lp::RigLights swapped = lp::RigLightsOf(f);
  Check(swapped.count == 3 && swapped.slot[0] == rig.slot[2] && swapped.slot[2] == rig.slot[0], "rig lights: the surface ranking is the order");
  // at most four
  lm::Collector five;
  five.Reset();
  five.BeginFrame(1, 1.0, MakeSet(4));
  Check(lp::RigLightsOf(five.frame()).count == 4, "rig lights: four at the most");
  lm::Collector none;
  none.Reset();
  none.BeginFrame(1, 1.0, MakeSet(0));
  Check(lp::RigLightsOf(none.frame()).count == 0, "rig lights: none in an empty frame");
}

void TestViewsOfRig() {
  lm::Collector c;
  c.Reset();
  const mcla::gpu::HeadlightSet set = MakeSet(3);
  c.BeginFrame(1, 1.0, set);
  lg::LocalShadowFrame made;
  made.views[made.count++] = MakeView(0, 1, 0, 6);  // light 0: two views
  made.views[made.count++] = MakeView(0, 1, 1, 6);
  made.views[made.count++] = MakeView(2, 1, 2, 6);  // light 2: one
  lg::LocalShadowView invalid = MakeView(1, 1, 3, 6);
  invalid.valid = 0;
  made.views[made.count++] = invalid;
  made.views[made.count++] = MakeView(5, 1, 4, 6);  // a light that is not in the set
  c.SetShadows(&made, MakeAtlas());
  const lp::RigLights rig = lp::RigLightsOf(c.frame());
  const lg::LocalShadowFrame v = lp::ShadowViewsOfRig(c.frame(), rig);
  Check(v.count == 3, "views of the rig: the valid views of the rig's lights (" + std::to_string(v.count) + ")");
  Check(v.views[0].light_index == 0 && v.views[1].light_index == 0 && v.views[2].light_index == 2, "views of the rig: each named for its light's place in the rig");
  Check(SameBytes(&v.views[0].world_to_shadow, &made.views[0].world_to_shadow, sizeof(made.views[0].world_to_shadow)) &&
            SameBytes(&v.views[2].world_to_shadow, &made.views[2].world_to_shadow, sizeof(made.views[2].world_to_shadow)),
        "views of the rig: the matrices as they were made");
  // a block for a rig whose slot order is not the slot number order names the right light
  lm::Collector d;
  d.Reset();
  mcla::gpu::HeadlightSet a = MakeSet(3, 40);
  d.BeginFrame(1, 1.0, a);
  mcla::gpu::HeadlightSet reordered = a;
  std::swap(reordered.lights[0], reordered.lights[2]);  // the same cars, the rig ranks them another way next frame
  d.BeginFrame(2, 1.0 + 1.0 / 60.0, reordered);
  d.SetShadows<lg::LocalShadowFrame>(nullptr, lg::ShadowAtlasInfo{});
  const lp::RigLights rr = lp::RigLightsOf(d.frame());
  Check(rr.count == 3 && d.frame().lights[rr.slot[0]].car_key == reordered.lights[0].car_key && d.frame().lights[rr.slot[2]].car_key == reordered.lights[2].car_key,
        "rig lights: after the rig re-ranks its cars the order is the rig's, not the slots'");
}

// A block that is not the rig's is found, whichever part of it differs.
void TestBlockMutations() {
  lm::Collector c;
  c.Reset();
  const mcla::gpu::HeadlightSet set = MakeSet(3);
  c.BeginFrame(1, 1.0, set);
  lg::LocalShadowFrame made;
  made.views[made.count++] = MakeView(0, 1, 0, 6);
  made.views[made.count++] = MakeView(0, 1, 1, 6);
  made.views[made.count++] = MakeView(1, 1, 2, 6);
  const lg::ShadowAtlasInfo atlas = MakeAtlas(77);
  c.SetShadows(&made, atlas);
  const float camera[3] = {5.0f, 6.0f, 7.0f};
  const hr::HeadlightConstants good = RigBlock(set, &made, atlas.slot, atlas.sampler, camera);
  const auto bad = [&](const hr::HeadlightConstants& b) {
    lm::SelfCheck sc;
    lp::CheckBlockBytes(c.frame(), b, lm::Check::kBlockBytes, &sc);
    return sc.total_bad() != 0;
  };
  Check(!bad(good), "block mutations: the rig's own block passes");
  const auto mutate = [&](const char* what, const char* part, auto&& edit) {
    hr::HeadlightConstants b = good;
    edit(b);
    lm::SelfCheck sc;
    lp::CheckBlockBytes(c.frame(), b, lm::Check::kBlockBytes, &sc);
    const std::string first = sc.TakeWindow().first;
    Check(!first.empty(), std::string("block mutations: a difference in ") + what + " is found");
    Check(first.find(part) != std::string::npos, std::string("block mutations: ") + what + " is reported as " + part + " (" + first + ")");
  };
  mutate("the light count", "the light count", [](hr::HeadlightConstants& b) { b.header[0] = 2.0f; });
  mutate("the header's other words", "the light count", [](hr::HeadlightConstants& b) { b.header[3] = 1.0f; });
  mutate("the reach box", "the reach box", [](hr::HeadlightConstants& b) { b.bounds_lo[2] -= 1.0f; });
  mutate("a position", "a light's rows", [](hr::HeadlightConstants& b) { b.lights[3][1] = std::nextafter(b.lights[3][1], 1000.0f); });
  mutate("a colour by one unit in the last place", "a light's rows", [](hr::HeadlightConstants& b) { b.lights[8][0] = std::nextafter(b.lights[8][0], 0.0f); });
  mutate("a row of a light that is not there", "a light's rows", [](hr::HeadlightConstants& b) { b.lights[10][1] = 1.0f; });
  mutate("which view a light reads", "which view a light reads", [](hr::HeadlightConstants& b) { b.shadow_of[0] = hr::kNoView; });
  mutate("which second view a light reads", "which second view a light reads", [](hr::HeadlightConstants& b) { b.shadow_of_b[0] = 2; });
  mutate("the atlas's slot", "the atlas", [](hr::HeadlightConstants& b) { b.atlas[0] += 1; });
  mutate("the atlas's sampler", "the atlas", [](hr::HeadlightConstants& b) { b.atlas[1] += 1; });
  mutate("the atlas's spare words", "the atlas", [](hr::HeadlightConstants& b) { b.atlas[2] = 1; });
  mutate("a view's matrix", "a shadow view", [](hr::HeadlightConstants& b) { b.shadow[1].world_to_shadow[5] += 0.5f; });
  mutate("a view's light index", "a shadow view", [](hr::HeadlightConstants& b) { b.shadow[2].light_index = 2; });
  mutate("a view that is not there", "a shadow view", [](hr::HeadlightConstants& b) { b.shadow[7].valid = 1; });
  // the camera is the one the block carries: a projection with another camera is not the block, and the check takes the block's own
  hr::HeadlightConstants other = good;
  other.camera[0] = 99.0f;
  Check(!bad(other), "block mutations: the camera is the block's own (the Scene overwrites it per draw), so another camera is not a difference");
  // a frame whose light moved by a ulp
  lm::Collector moved;
  moved.Reset();
  mcla::gpu::HeadlightSet shifted = set;
  shifted.lights[1].pos[0] = std::nextafter(shifted.lights[1].pos[0], 1000.0f);
  moved.BeginFrame(1, 1.0, shifted);
  moved.SetShadows(&made, atlas);
  lm::SelfCheck sc;
  lp::CheckBlockBytes(moved.frame(), good, lm::Check::kBlockBytes, &sc);
  Check(sc.total_bad() == 1, "block mutations: a frame whose light is one unit in the last place elsewhere is not the block");
  // an empty frame is the block of no light
  lm::Collector empty;
  empty.Reset();
  empty.BeginFrame(1, 1.0, MakeSet(0));
  empty.SetShadows<lg::LocalShadowFrame>(nullptr, lg::ShadowAtlasInfo{});
  const hr::HeadlightConstants nothing = RigBlock(MakeSet(0), nullptr, hr::kNoSlot, atlas.sampler, camera);
  lm::SelfCheck se;
  lp::CheckBlockBytes(empty.frame(), nothing, lm::Check::kBlockBytes, &se);
  Check(se.total_bad() == 0 && se.total_checks() == 1, "block: an empty frame is the rig's empty block (a box with lo above hi, no atlas)");
  // maps whose atlas the frame does not have: no light reads one
  lm::Collector noatlas;
  noatlas.Reset();
  noatlas.BeginFrame(1, 1.0, set);
  noatlas.SetShadows<lg::LocalShadowFrame>(nullptr, lg::ShadowAtlasInfo{});
  const hr::HeadlightConstants lit = RigBlock(set, nullptr, hr::kNoSlot, atlas.sampler, camera);
  lm::SelfCheck sn;
  lp::CheckBlockBytes(noatlas.frame(), lit, lm::Check::kBlockBytes, &sn);
  Check(sn.total_bad() == 0, "block: lights with no maps are the block with no views and no atlas slot");
}

// The assumption the projection rests on: the frame groups a light's views, so a table whose views of one light are not one after the other would be told apart. (The replay's
// publication is grouped at every level: RunBlocks checks it over the drive.)
void TestUngroupedTableIsFound() {
  lm::Collector c;
  c.Reset();
  const mcla::gpu::HeadlightSet set = MakeSet(2);
  c.BeginFrame(1, 1.0, set);
  lg::LocalShadowFrame made;
  made.views[made.count++] = MakeView(0, 1, 0, 3);
  made.views[made.count++] = MakeView(1, 1, 1, 3);
  made.views[made.count++] = MakeView(0, 1, 2, 3);  // light 0's second view after light 1's
  const lg::ShadowAtlasInfo atlas = MakeAtlas();
  c.SetShadows(&made, atlas);
  const float camera[3] = {};
  const hr::HeadlightConstants legacy = RigBlock(set, &made, atlas.slot, atlas.sampler, camera);
  lm::SelfCheck sc;
  lp::CheckBlockBytes(c.frame(), legacy, lm::Check::kBlockBytes, &sc);
  Check(sc.total_bad() == 1, "ungrouped: maps the replay published apart are found by the byte check (the table's order is the replay's)");
}

}  // namespace

int main(int argc, char** argv) {
  TestRigLightsOf();
  TestViewsOfRig();
  TestBlockMutations();
  TestUngroupedTableIsFound();

  const std::vector<SimCar> drive = MakeDrive();
  const std::vector<DriveFrame> frames = RunDrive(drive, 1500);
  Check(frames.size() == 1500, "a drive of 1500 frames");
  uint32_t max_lights = 0, light_changes = 0;
  std::array<uint32_t, 4> last{};
  uint32_t last_n = 0;
  for (const DriveFrame& f : frames) {
    max_lights = std::max(max_lights, f.set.count);
    bool changed = f.set.count != last_n;
    for (uint32_t i = 0; i < std::min(f.set.count, last_n) && !changed; ++i) {
      changed = f.set.lights[i].car_key != last[i];
    }
    light_changes += changed ? 1u : 0u;
    last_n = f.set.count;
    last = KeysOf(f.set);
  }
  Check(max_lights == 4 && light_changes > 10, "the drive fills the rig's four lights and changes them (" + std::to_string(light_changes) + " changes of the set)");
  TestBlockOverTheDrive(frames, "the made-up drive");

  // the game's own recording, when given
  for (int a = 1; a < argc; ++a) {
    const std::string path = argv[a];
    if (path.size() > 8 && path.substr(path.size() - 8) == ".rig.txt") {
      std::ifstream in(path, std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      std::vector<rec::RigFrame> rig;
      std::vector<rec::ViewFrame> views;
      std::string error;
      Check(in.good() || in.eof(), "the recording " + path + " is readable");
      if (rec::Parse(text.str(), &rig, &views, &error) && !rig.empty()) {
        const std::vector<DriveFrame> recorded = RunRecording(rig);
        std::printf("INFO: %zu frames of %s\n", recorded.size(), path.c_str());
        TestBlockOverTheDrive(recorded, "the recording");
      } else {
        Check(false, "the recording " + path + " is read: " + error);
      }
    }
  }
  std::printf("light_project_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

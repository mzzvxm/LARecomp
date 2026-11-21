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
//   4c  the shadow replay's lights: the lights ProjectShadowLights makes from the frame against the ones PickViews makes from the rig's set (bit for bit), and then the real view machinery
//       (ViewPicker, PlanViews, FitCar, the publication of the maps) run on each over the drive: the same cars, plans, view specs and published maps, at all three levels.
//   U5  the shadow atlas (shadow_atlas_rules.h): the frame's shadow views read from the atlas's published form BY LIGHT ID (ShadowFrameBuilder at the trigger, RebindByIds at the next frame) against
//       the rig's carry and key remap (Collector::SetShadows): the same ShadowFrame, the same lights' refs and rankings, and a headlight block projected from it byte for byte the rig's, at every
//       level over every frame; the one lookup (light_shadow_lookup.h LightLit) of each light against the block's own views and the variant's kernel, bit for bit.
//   4b  the volume's light inputs: the lamps (the volume's real LampPicker through VolumeLampPick, the Collector's SetPickedLamps and ProjectVolumeLamps) against FillLamps' (the same
//       picker run as VolumetricsInputs::FillLamps runs it), the headlights and shadow views (the frame's against FillHeadlights + ApplyHeadlightUnits + FillShadows), every part of VolInputs
//       byte for byte over every frame, with the core's test lamp, a cut, a gap, level changes and a limit.

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
#include "light_shadow_lookup.h"
#include "local_shadow_rules.h"
#include "local_shadow_types.h"
#include "shadow_atlas_rules.h"
#include "vol_lamp_list.h"
#include "volumetrics_headlights.h"
#include "volumetrics_lamp_pick.h"
#include "volumetrics_lamps.h"
#include "volumetrics_types.h"

namespace lg = larecomp::gpu;
namespace lm = larecomp::gpu::light_manager;
namespace lp = mcla::gpu::light_project;
namespace hr = mcla::gpu::headlight_rules;
namespace ls = mcla::gpu::local_shadow;
namespace lb = mcla::gpu::light_budget;
namespace rec = mcla::gpu::light_pick::record;
namespace vol = mcla::gpu::volumetrics;
namespace vl = mcla::gpu::vol_lamps;
namespace sa = mcla::gpu::shadow_atlas;
namespace sl = larecomp::gpu::shadow_lookup;

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
uint32_t LegacyShadowLights(const mcla::gpu::HeadlightSet& set, ls::Light out[4]) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < set.count && count < 4; ++i) {  // LocalShadow::PickViews' loop (LightsOfRigSet)
    const mcla::gpu::Headlight& h = set.lights[i];
    if (ls::MakeLight(h.pos, h.dir, h.cos_outer, h.range, h.car_key, h.is_player, i, &out[count], h.lamp_offset)) {
      ++count;
    }
  }
  return count;
}

// What the replay made of one frame's lights: the cars that hold views, the plan, each car's cone-cull view and the maps it published.
struct ReplayFrame {
  uint32_t cars = 0;
  ls::Group groups[4];
  ls::ViewPlan plan;
  std::array<ls::ViewSpec, 4> early{};
  std::array<bool, 4> early_ok{};
  lg::LocalShadowFrame published;
};

ReplayFrame PublishFromLights(Replay* r, const ls::Light* lights, uint32_t count, const Camera& cam, uint64_t frame) {
  ReplayFrame out;
  ls::PickContext context;
  context.have_reference = true;
  for (int k = 0; k < 3; ++k) {
    context.reference[k] = cam.eye[k];
  }
  const lb::LightBudget budget = lb::BudgetFor(true, r->level, r->views_override, lb::Level::kOff, ls::kLevelTable);
  out.cars = r->picker.Pick(lights, count, budget.shadow_cars, context, out.groups);
  ls::PlanViews(lights, out.groups, out.cars, budget.shadow_lamp_cars, budget.shadow_views, &out.plan);
  for (uint32_t c = 0; c < out.plan.cars && c < 4; ++c) {
    out.early_ok[c] = ls::FitCar(out.plan, c, &out.early[c]);
  }
  const ls::ViewPlan& plan = out.plan;
  for (uint32_t v = 0; v < plan.count; ++v) {
    const ls::ViewSlot& view = plan.views[v];
    for (uint32_t m = 0; m < view.group.count && out.published.count < out.published.views.size(); ++m) {
      out.published.views[out.published.count++] = MakeView(view.lights[view.group.members[m]].index, frame, v, plan.count);
    }
  }
  // each light's views one after the other?
  std::array<bool, 8> closed{};
  uint32_t last = ~0u;
  for (uint32_t e = 0; e < out.published.count; ++e) {
    const uint32_t li = out.published.views[e].light_index;
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

lg::LocalShadowFrame PublishViews(Replay* r, const mcla::gpu::HeadlightSet& set, const Camera& cam, uint64_t frame) {
  ls::Light lights[4];
  const uint32_t count = LegacyShadowLights(set, lights);
  return PublishFromLights(r, lights, count, cam, frame).published;
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

// ---------------------------------------------------------------------------
// 4b: the volume's lamps
// ---------------------------------------------------------------------------

std::vector<vl::GameLamp> MakeStreet(uint32_t n, float seed) {
  std::vector<vl::GameLamp> list;
  for (uint32_t i = 0; i < n; ++i) {
    vl::GameLamp g;
    g.pos[0] = 12.0f + 0.37f * seed + 6.5f * float(i);
    g.pos[1] = 9.2f;
    g.pos[2] = (i % 2 == 0 ? 7.0f : -7.0f) + 0.01f * float(i);
    g.colour[0] = 1.0f;
    g.colour[1] = 0.84f - 0.002f * float(i);
    g.colour[2] = 0.6f;
    g.dir[0] = 0.0f;
    g.dir[1] = -1.0f;
    g.dir[2] = 0.0f;
    g.intensity = 55.0f + float((i * 7) % 46);
    g.cone_deg = 125.0f;
    g.spread_deg = -50.5f;
    g.scale = i % 5 == 0 ? 0.8f : (i % 7 == 0 ? 0.37f : 1.0f);  // the prop pass's fade: a lamp at 80 and at 37 per cent
    g.type = 1;
    g.on = 1;
    list.push_back(g);
  }
  vl::GameLamp omni = list[3];  // an omni lamp (a window), and one of intensity 0.4 (a record the pick finds no use for)
  omni.pos[0] += 1.1f;
  omni.type = 0;
  omni.cone_deg = 360.0f;
  list.push_back(omni);
  vl::GameLamp dim = list[5];
  dim.pos[2] += 2.0f;
  dim.intensity = 0.4f;
  list.push_back(dim);
  vl::GameLamp twin = list[8];  // two records at one place: the pick keeps the better scoring one
  twin.intensity += 30.0f;
  list.push_back(twin);
  return list;
}

vol::VolCamera CameraAt(float x, float yaw) {
  vol::VolCamera cam;
  cam.pos[0] = x;
  cam.pos[1] = 1.6f;
  cam.pos[2] = 0.0f;
  cam.forward[0] = std::cos(yaw);
  cam.forward[1] = 0.0f;
  cam.forward[2] = std::sin(yaw);
  cam.right[0] = -std::sin(yaw);
  cam.right[1] = 0.0f;
  cam.right[2] = std::cos(yaw);
  cam.up[0] = 0.0f;
  cam.up[1] = 1.0f;
  cam.up[2] = 0.0f;
  cam.tan_x = 1.0f;
  cam.tan_y = 0.5625f;
  return cam;
}

// VolumetricsInputs::FillLamps' own lines (volumetrics_inputs.cpp), on the same types: the legacy producer of the lamps.
class LegacyLamps {
 public:
  void Fill(const std::vector<vl::GameLamp>& list, const vol::VolFrameContext& frame, vol::VolInputs* out, uint32_t limit) {
    bool reset = false;
    if (have_camera_) {
      float step2 = 0.0f;
      for (int i = 0; i < 3; ++i) {
        const float d = frame.camera.pos[i] - last_camera_[i];
        step2 += d * d;
      }
      reset = step2 > vol::kCutMetres * vol::kCutMetres;
    }
    reset = reset || frame.elapsed_s > 0.5f;
    for (int i = 0; i < 3; ++i) {
      last_camera_[i] = frame.camera.pos[i];
    }
    have_camera_ = true;
    const uint32_t room_all = std::min(frame.grid.lamps, vol::kMaxLamps);
    const uint32_t have = out->lamp_count;
    const uint32_t room = std::min(limit, room_all > have ? room_all - have : 0u);
    vol::PickParams params;
    params.budget = room;
    params.dt = frame.elapsed_s;
    params.near_m = frame.grid.near_m;
    params.far_m = frame.grid.far_m;
    params.reset = reset;
    picker_.Update(list, frame.camera, params, &picked_);
    uint32_t n = have;
    for (uint32_t i = 0; i < picked_.count && n < vol::kMaxLamps; ++i) {
      out->lamps[n++] = picked_.lamps[i];
    }
    out->lamp_count = n;
  }
  void Reset() {
    picker_.Reset();
    picked_ = vol::PickResult{};
  }
  const vol::PickResult& picked() const { return picked_; }

 private:
  vol::LampPicker picker_;
  vol::PickResult picked_;
  float last_camera_[3] = {};
  bool have_camera_ = false;
};

// The volume's entry of a frame: the core's defaults, and a test lamp on some frames.
vol::VolInputs Entry(bool test_lamp) {
  vol::VolInputs in;
  in.valid = true;
  if (test_lamp) {
    vol::VolLamp t{};
    t.pos[0] = 3.0f;
    t.pos[1] = 5.0f;
    t.pos[2] = 3.0f;
    t.range = 30.0f;
    t.dir[1] = -1.0f;
    t.cos_outer = 0.5f;
    t.cos_inner = 0.8f;
    t.colour[0] = t.colour[1] = t.colour[2] = 40.0f;
    t.shadow_view = -1;
    t.shadow_view_b = -1;
    t.pad[0] = 30.0f;
    in.lamps[in.lamp_count++] = t;
  }
  return in;
}

struct LampRun {
  uint64_t frames = 0, compares = 0, bad = 0, with_lamps = 0, fading = 0, resets = 0, test_lamp_frames = 0, max_lamps = 0;
  std::string first;
};

// One camera drive over a street of 70 lamps, the volume's level going 12 -> 32 -> 48 lamps, a cut at frame 200, a gap at frame 280, the debug limit in a stretch.
LampRun RunLamps(float seed) {
  LampRun run;
  const std::vector<vl::GameLamp> list = MakeStreet(140, seed);
  LegacyLamps legacy;
  vol::VolumeLampPick pick;
  lm::Collector c;
  c.Reset();
  mcla::gpu::HeadlightSet none;
  double clock = 50.0;
  float x = 20.0f;
  for (uint32_t frame = 0; frame < 420; ++frame) {
    clock += 1.0 / 60.0;
    x += frame == 200 ? 400.0f : 0.35f;  // a cut
    c.BeginFrame(frame, clock, none);
    c.SetShadows<lg::LocalShadowFrame>(nullptr, lg::ShadowAtlasInfo{});
    vol::VolFrameContext ctx;
    ctx.frame = frame;
    ctx.camera = CameraAt(x, 0.3f * std::sin(0.02f * float(frame)));
    ctx.grid.lamps = frame < 120 ? 12u : (frame < 240 ? 32u : 48u);
    ctx.grid.near_m = 0.5f;
    ctx.grid.far_m = 160.0f;
    ctx.elapsed_s = frame == 0 ? 0.0f : (frame == 280 ? 0.9f : 1.0f / 60.0f);  // a long gap at 280 (a menu)
    const uint32_t limit = (frame >= 330 && frame < 380) ? 7u : vol::kMaxLamps;  // mcla_gpu_vol_lamp_limit in a stretch
    const bool test_lamp = frame % 3 == 0;
    const bool lamps_on = frame < 380 || frame >= 400;  // the setting's off for 20 frames (mcla_gpu_vol_inputs)
    vol::VolInputs a = Entry(test_lamp), b = Entry(test_lamp);
    if (lamps_on) {
      legacy.Fill(list, ctx, &a, limit);
      const vol::PickResult& picked = pick.Update(list, ctx, b.lamp_count, limit);
      c.SetPickedLamps(picked.records.data(), picked.weights.data(), picked.count, picked.listed);
    } else {
      legacy.Reset();
      pick.Reset();
      c.SetPickedLamps<vl::GameLamp>(nullptr, nullptr, 0, 0);
    }
    lp::ProjectVolumeLamps(c.frame(), &b);
    lm::SelfCheck sc;
    lp::CheckVolumeBytes(b, a, &sc);
    const bool same = SameBytes(&a, &b, sizeof(vol::VolInputs)) || (SameBytes(a.lamps.data(), b.lamps.data(), sizeof(a.lamps)) && a.lamp_count == b.lamp_count);
    ++run.frames;
    ++run.compares;
    if (sc.total_bad() != 0 || !same) {
      ++run.bad;
      if (run.first.empty()) {
        run.first = "frame " + std::to_string(frame) + ": " + sc.TakeWindow().first;
      }
    }
    run.with_lamps += a.lamp_count > (test_lamp ? 1u : 0u) ? 1u : 0u;
    run.test_lamp_frames += test_lamp ? 1u : 0u;
    run.max_lamps = std::max<uint64_t>(run.max_lamps, a.lamp_count);
    for (uint32_t k = 0; k < c.lamp_count(); ++k) {
      run.fading += c.frame().lights[c.lamp_slot(k)].vol_weight < 0.999f ? 1u : 0u;
    }
    lm::SelfCheck inv;
    lm::CheckFrameInvariants(c.frame(), c.has_previous() ? &c.previous() : nullptr, &inv);
    Check(inv.total_bad() == 0, "volume lamps: the frame's own invariants hold with the manager's lamps (frame " + std::to_string(frame) + ")");
    c.EndFrame();
  }
  return run;
}

void TestVolumeLamps() {
  for (const float seed : {0.0f, 3.1f, 11.7f}) {
    const LampRun run = RunLamps(seed);
    const std::string at = " (street seed " + std::to_string(seed) + ")";
    Check(run.bad == 0, "volume lamps: the lamps made from the frame are the volume's own lamps byte for byte in each of " + std::to_string(run.compares) + " frames" + at + ", " +
                            std::to_string(run.bad) + " differed: " + run.first);
    Check(run.with_lamps > 350 && run.max_lamps >= 30 && run.fading > 100, "volume lamps: the drive has lamps, fades and the larger levels" + at + " (" + std::to_string(run.with_lamps) + " frames with lamps, at most " +
                                                                           std::to_string(run.max_lamps) + ", " + std::to_string(run.fading) + " lamp-frames fading)");
    std::printf("INFO: volume lamps%s: %llu frames compared, %llu with lamps, at most %llu in the inputs, %llu lamp-frames with a weight under 1\n", at.c_str(), (unsigned long long)run.frames,
                (unsigned long long)run.with_lamps, (unsigned long long)run.max_lamps, (unsigned long long)run.fading);
  }
  // The pick class is the picker's driver: a cut and a gap start the lamps again at full weight, and the room is the level's less what is in.
  const std::vector<vl::GameLamp> list = MakeStreet(140, 0.0f);
  vol::VolumeLampPick pick;
  vol::VolFrameContext ctx;
  ctx.camera = CameraAt(20.0f, 0.0f);
  ctx.grid.lamps = 12;
  ctx.elapsed_s = 0.0f;
  vol::PickResult first = pick.Update(list, ctx, 1, 0);
  Check(pick.room() == 11 && first.count <= 11 && first.count > 0, "pick: the room is the level's twelve less the test lamp (" + std::to_string(pick.room()) + ")");
  bool all_full = true;
  for (uint32_t k = 0; k < first.count; ++k) {
    all_full = all_full && first.weights[k] == 1.0f;
  }
  Check(all_full, "pick: the first pick is at full weight");
  ctx.elapsed_s = 1.0f / 60.0f;
  ctx.camera = CameraAt(25.0f, 0.0f);
  ctx.grid.lamps = 32;
  const vol::PickResult second = pick.Update(list, ctx, 0, 0);
  bool some_fading_in = false;
  for (uint32_t k = 0; k < second.count; ++k) {
    some_fading_in = some_fading_in || second.weights[k] < 1.0f;
  }
  Check(some_fading_in && second.count > first.count, "pick: lamps that join with a bigger budget fade in");
  ctx.camera = CameraAt(500.0f, 0.0f);  // a cut
  const vol::PickResult cut = pick.Update(list, ctx, 0, 0);
  bool cut_full = cut.count > 0;
  for (uint32_t k = 0; k < cut.count; ++k) {
    cut_full = cut_full && cut.weights[k] == 1.0f;
  }
  Check(cut_full, "pick: after a camera cut every lamp is at full weight");
  Check(pick.Update(list, ctx, 0, 5).count <= 5 && pick.room() == 5, "pick: the debug limit caps the room");
  Check(pick.Update(list, ctx, 28, 0).count <= 4 && pick.room() == 4, "pick: and so do the lamps already in");
  Check(pick.Update(list, ctx, 40, 0).count == 0 && pick.room() == 0, "pick: more lamps in than the level reads leaves no room");
  // a record's weight and the Light's: the volume's colour is the Light's colour x units x weight, bit for bit
  vl::GameLamp g = list[10];
  lg::Light light = lg::LightFromGameLamp(g);
  light.vol_weight = 0.3721f;
  vol::VolLamp made = vol::MakeVolLamp(g);
  for (int i = 0; i < 3; ++i) {
    made.colour[i] *= 0.3721f;
  }
  const vol::VolLamp projected = lg::ToVolLamp<vol::VolLamp>(light);
  Check(SameBytes(&made, &projected, sizeof(made)), "pick: a lamp's volume lamp from its Light is the picker's lamp byte for byte (the near radius, the cone, the views, the weight)");
  Check(projected.shadow_view == -1 && projected.shadow_view_b == -1, "pick: a street lamp reads no shadow view (both -1)");
}

// ---------------------------------------------------------------------------
// 4b: the volume's headlights and the shadow views they read
// ---------------------------------------------------------------------------

// VolumetricsInputs::FillHeadlightInputs' lines on the same types.
void LegacyHeadlights(const mcla::gpu::HeadlightSet& set, const lg::LocalShadowFrame* local, const lg::ShadowAtlasInfo& atlas, vol::VolInputs* out) {
  if (set.count == 0) {
    return;
  }
  vol::FillHeadlights(set, out);
  for (uint32_t i = 0; i < out->headlight_count; ++i) {
    vol::ApplyHeadlightUnits(&out->headlights[i]);
  }
  const vol::ShadowAtlasState state{atlas.valid, atlas.slot, atlas.width, atlas.height};
  vol::FillShadows(local, state, out);
}

struct HeadRun {
  uint64_t frames = 0, bad = 0, with_views = 0, two_views = 0, dropped_views = 0;
  std::string first;
};

// Over the drive: the volume's fill after the trigger of each frame, the lamps of the street beside them.
HeadRun RunHeadlights(const std::vector<DriveFrame>& frames, lb::Level level, bool spoil_views) {
  HeadRun run;
  Replay replay;
  replay.level = level;
  lm::Collector c;
  lm::ShadowCarry<lg::LocalShadowFrame> carry;
  c.Reset();
  carry.Clear();
  const lg::ShadowAtlasInfo atlas = MakeAtlas(52);
  const std::vector<vl::GameLamp> list = MakeStreet(70, 1.5f);
  LegacyLamps legacy_lamps;
  vol::VolumeLampPick pick;
  bool had_trigger = false;
  for (const DriveFrame& f : frames) {
    const mcla::gpu::HeadlightSet& set = f.set;
    c.BeginFrame(uint32_t(f.frame), double(f.frame) / kFps, set);
    if (!had_trigger) {
      carry.Clear();
    }
    const std::array<uint32_t, 4> keys = KeysOf(set);
    const lg::LocalShadowFrame moved = carry.ForSet(keys.data(), set.count, kRemap);
    c.SetShadows(moved.count != 0 ? &moved : nullptr, carry.has() ? carry.atlas() : lg::ShadowAtlasInfo{});
    const bool trigger = set.count > 0 && f.frame % 37 != 0;
    had_trigger = trigger;
    lg::LocalShadowFrame made;
    if (set.count > 0 && !trigger) {
      c.EndFrame();  // the replay made no map in this frame: the volume reads the last trigger's maps moved to this set (TestVolumeWithoutTrigger)
      continue;
    }
    if (trigger) {
      made = PublishViews(&replay, set, f.camera, f.frame);
      if (spoil_views && made.count > 1 && f.frame % 5 == 0) {
        made.views[made.count - 1].atlas_rect[2] = 3.0f;  // a tile outside the atlas: the volume cannot read it
      }
      carry.Set(made.count != 0 ? &made : nullptr, atlas, keys.data(), set.count);
      c.SetShadows(made.count != 0 ? &made : nullptr, made.count != 0 ? atlas : lg::ShadowAtlasInfo{});
    }
    // The volume's fill (after the trigger): the legacy producer's lamps and headlights, and the frame's.
    vol::VolFrameContext ctx;
    ctx.frame = f.frame;
    ctx.camera = CameraAt(float(f.camera.eye[0]), 0.1f);
    ctx.grid.lamps = level == lb::Level::kMedium ? 12u : 32u;
    ctx.grid.headlights = level == lb::Level::kMedium ? 2u : 4u;
    ctx.elapsed_s = f.frame == 1 ? 0.0f : 1.0f / 60.0f;
    const bool test_lamp = f.frame % 4 == 0;
    vol::VolInputs a = Entry(test_lamp), b = Entry(test_lamp);
    legacy_lamps.Fill(list, ctx, &a, vol::kMaxLamps);
    LegacyHeadlights(set, trigger && made.count != 0 ? &made : nullptr, trigger && made.count != 0 ? atlas : lg::ShadowAtlasInfo{}, &a);
    const vol::PickResult& picked = pick.Update(list, ctx, b.lamp_count, 0);
    c.SetPickedLamps(picked.records.data(), picked.weights.data(), picked.count, picked.listed);
    lp::ProjectVolumeLamps(c.frame(), &b);
    lp::ProjectVolumeHeadlights(c.frame(), &b);
    lm::SelfCheck sc;
    lp::CheckVolumeBytes(b, a, &sc);
    const bool same = SameBytes(&a.lamps, &b.lamps, sizeof(a.lamps)) && SameBytes(&a.headlights, &b.headlights, sizeof(a.headlights)) &&
                      SameBytes(&a.shadow_views, &b.shadow_views, sizeof(a.shadow_views)) && a.lamp_count == b.lamp_count && a.headlight_count == b.headlight_count &&
                      a.shadow_view_count == b.shadow_view_count && a.shadow_atlas_slot == b.shadow_atlas_slot && SameBytes(a.shadow_atlas_size, b.shadow_atlas_size, sizeof(a.shadow_atlas_size));
    ++run.frames;
    if (sc.total_bad() != 0 || !same) {
      ++run.bad;
      if (run.first.empty()) {
        run.first = "frame " + std::to_string(f.frame) + ": " + sc.TakeWindow().first;
      }
    }
    Check(same == (sc.total_bad() == 0), "volume headlights: the check agrees with a byte compare of the two inputs");
    run.with_views += a.shadow_view_count != 0 ? 1u : 0u;
    for (uint32_t i = 0; i < a.headlight_count; ++i) {
      run.two_views += a.headlights[i].shadow_view_b >= 0 ? 1u : 0u;
    }
    for (uint32_t i = 0; i < a.shadow_view_count; ++i) {
      run.dropped_views += a.shadow_views[i].valid == 0 ? 1u : 0u;
    }
    lm::SelfCheck inv;
    lm::CheckFrameInvariants(c.frame(), c.has_previous() ? &c.previous() : nullptr, &inv);
    Check(inv.total_bad() == 0, "volume headlights: the frame's own invariants hold");
    c.EndFrame();
  }
  return run;
}

void TestVolumeHeadlightsOverTheDrive(const std::vector<DriveFrame>& frames, const std::string& name) {
  for (const lb::Level level : {lb::Level::kMedium, lb::Level::kHigh, lb::Level::kUltra}) {
    for (const bool spoil : {false, true}) {
      const HeadRun run = RunHeadlights(frames, level, spoil);
      const std::string at = " (" + name + ", " + LevelName(level) + (spoil ? ", some views unusable" : "") + ")";
      std::printf("INFO: volume inputs%s: %llu frames compared, %llu with shadow views, %llu headlight-frames reading two views, %llu views dropped as unusable\n", at.c_str(),
                  (unsigned long long)run.frames, (unsigned long long)run.with_views, (unsigned long long)run.two_views, (unsigned long long)run.dropped_views);
      Check(run.bad == 0, "volume headlights: the lamps, headlights and shadow views made from the frame are the volume's own byte for byte in each of " + std::to_string(run.frames) + " frames" + at +
                              ", " + std::to_string(run.bad) + " differed: " + run.first);
      Check(run.with_views > run.frames / 2, "volume headlights: most frames have shadow views in the inputs" + at);
      if (level != lb::Level::kMedium) {
        Check(run.two_views > 0, "volume headlights: a headlight reading two views was seen" + at);
      }
      if (spoil) {
        Check(run.dropped_views > 0, "volume headlights: views the volume cannot read were dropped by both" + at);
      }
    }
  }
}

// A frame in which the volume fills but the replay made no map of its own: the frame holds the last trigger's maps moved to this frame's set (by the car), the legacy volume reads the
// replay's stale ones with the light indices of the frame they were made for. The same bytes while the rig's order did not change; when it did, the frame's lights read the views of
// THEIR cars (the legacy ones read another car's), so the frame is the right one and the proof run will say so (CHECK).
void TestVolumeWithoutTrigger() {
  const lg::ShadowAtlasInfo atlas = MakeAtlas(31);
  const mcla::gpu::HeadlightSet before = MakeSet(3, 20);
  lm::Collector c;
  lm::ShadowCarry<lg::LocalShadowFrame> carry;
  c.Reset();
  c.BeginFrame(1, 1.0, before);
  lg::LocalShadowFrame made;
  made.views[made.count++] = MakeView(0, 1, 0, 4);
  made.views[made.count++] = MakeView(0, 1, 1, 4);
  made.views[made.count++] = MakeView(2, 1, 2, 4);
  carry.Set(&made, atlas, KeysOf(before).data(), before.count);
  c.SetShadows(&made, atlas);
  c.EndFrame();
  const auto compare = [&](const mcla::gpu::HeadlightSet& now, uint32_t frame) {
    c.BeginFrame(frame, 1.0 + double(frame) / 60.0, now);
    const std::array<uint32_t, 4> keys = KeysOf(now);
    const lg::LocalShadowFrame moved = carry.ForSet(keys.data(), now.count, kRemap);
    c.SetShadows(moved.count != 0 ? &moved : nullptr, atlas);
    vol::VolInputs a = Entry(false), b = Entry(false);
    LegacyHeadlights(now, &made, atlas, &a);  // the replay's published frame is still the last trigger's
    lp::ProjectVolumeHeadlights(c.frame(), &b);
    lm::SelfCheck sc;
    lp::CheckVolumeBytes(b, a, &sc);
    c.EndFrame();
    return sc.total_bad() == 0;
  };
  mcla::gpu::HeadlightSet moved_on = before;
  for (uint32_t i = 0; i < moved_on.count; ++i) {
    moved_on.lights[i].pos[0] += 0.3f;  // the cars moved, the rig's order did not change
  }
  Check(compare(moved_on, 2), "without a trigger: the rig's order is the same, so the frame and the legacy volume have the same bytes");
  mcla::gpu::HeadlightSet swapped = moved_on;
  std::swap(swapped.lights[0], swapped.lights[2]);  // the rig now ranks the third car first
  Check(!compare(swapped, 3), "without a trigger: the rig ranked its cars another way: the frame's views follow their cars, the legacy volume's follow the old places (a difference the proof run reports)");
  // and in that frame the frame's lights read the right views
  lm::Collector d;
  d.Reset();
  d.BeginFrame(4, 1.0, swapped);
  const std::array<uint32_t, 4> keys = KeysOf(swapped);
  const lg::LocalShadowFrame moved = carry.ForSet(keys.data(), swapped.count, kRemap);
  d.SetShadows(&moved, atlas);
  vol::VolInputs b = Entry(false);
  lp::ProjectVolumeHeadlights(d.frame(), &b);
  Check(b.headlight_count == 3 && b.headlights[0].shadow_view >= 0 && b.headlights[0].shadow_view_b < 0 && b.headlights[2].shadow_view >= 0 && b.headlights[2].shadow_view_b >= 0 &&
            b.headlights[1].shadow_view < 0,
        "without a trigger: the car that was third (one view) is first now and reads one; the one that was first (two views) is third and reads two");
}

// Every part of the volume's inputs that is not the legacy producer's is found.
void TestVolumeMutations() {
  const std::vector<vl::GameLamp> list = MakeStreet(70, 0.0f);
  const mcla::gpu::HeadlightSet set = MakeSet(3);
  lm::Collector c;
  c.Reset();
  c.BeginFrame(1, 1.0, set);
  lg::LocalShadowFrame made;
  made.views[made.count++] = MakeView(0, 1, 0, 6);
  made.views[made.count++] = MakeView(0, 1, 1, 6);
  made.views[made.count++] = MakeView(1, 1, 2, 6);
  const lg::ShadowAtlasInfo atlas = MakeAtlas(91);
  c.SetShadows(&made, atlas);
  vol::VolFrameContext ctx;
  ctx.camera = CameraAt(40.0f, 0.2f);
  ctx.grid.lamps = 12;
  vol::VolumeLampPick pick;
  const vol::PickResult& picked = pick.Update(list, ctx, 0, 0);
  c.SetPickedLamps(picked.records.data(), picked.weights.data(), picked.count, picked.listed);
  vol::VolInputs good = Entry(false), mine = Entry(false);
  LegacyLamps legacy;
  legacy.Fill(list, ctx, &good, vol::kMaxLamps);
  LegacyHeadlights(set, &made, atlas, &good);
  lp::ProjectVolumeLamps(c.frame(), &mine);
  lp::ProjectVolumeHeadlights(c.frame(), &mine);
  const auto bad = [&](const vol::VolInputs& b) {
    lm::SelfCheck sc;
    lp::CheckVolumeBytes(mine, b, &sc);
    return sc.TakeWindow().first;
  };
  Check(bad(good).empty(), "volume mutations: the legacy inputs pass");
  Check(good.lamp_count >= 8 && good.headlight_count == 3 && good.shadow_view_count == 3, "volume mutations: the inputs have lamps, three headlights and three views to change");
  const auto mutate = [&](const char* what, const char* part, auto&& edit) {
    vol::VolInputs b = good;
    edit(b);
    const std::string first = bad(b);
    Check(!first.empty(), std::string("volume mutations: a difference in ") + what + " is found");
    Check(first.find(part) != std::string::npos, std::string("volume mutations: ") + what + " is reported with '" + part + "' (" + first + ")");
  };
  mutate("the lamp count", "lamps", [](vol::VolInputs& b) { b.lamp_count -= 1; });
  mutate("a lamp's position", "its position", [](vol::VolInputs& b) { b.lamps[2].pos[1] = std::nextafter(b.lamps[2].pos[1], 100.0f); });
  mutate("a lamp's range", "its range", [](vol::VolInputs& b) { b.lamps[1].range += 0.5f; });
  mutate("a lamp's colour by one unit in the last place", "its colour", [](vol::VolInputs& b) { b.lamps[3].colour[2] = std::nextafter(b.lamps[3].colour[2], 0.0f); });
  mutate("a lamp's near radius", "its near radius", [](vol::VolInputs& b) { b.lamps[0].pad[0] += 0.01f; });
  mutate("a lamp's second view word", "which second view it reads", [](vol::VolInputs& b) { b.lamps[0].shadow_view_b = 0; });
  mutate("a lamp's first view word", "which view it reads", [](vol::VolInputs& b) { b.lamps[0].shadow_view = 0; });
  mutate("a lamp past the count", "lamp", [](vol::VolInputs& b) { b.lamps[b.lamp_count + 1].range = 1.0f; });
  mutate("a headlight's colour", "headlight 1", [](vol::VolInputs& b) { b.headlights[1].colour[0] += 1.0f; });
  mutate("a headlight's near radius", "headlight 2", [](vol::VolInputs& b) { b.headlights[2].pad[0] = 9.0f; });
  mutate("which view a headlight reads", "which view it reads", [](vol::VolInputs& b) { b.headlights[0].shadow_view = 2; });
  mutate("the headlight count", "headlights", [](vol::VolInputs& b) { b.headlight_count = 2; });
  mutate("a view's matrix", "shadow view 1", [](vol::VolInputs& b) { b.shadow_views[1].world_to_shadow[7] += 0.25f; });
  mutate("a view's tile", "shadow view 2", [](vol::VolInputs& b) { b.shadow_views[2].atlas_rect[3] = 0.9f; });
  mutate("a view that is not there", "shadow view 5", [](vol::VolInputs& b) { b.shadow_views[5].valid = 1; });
  mutate("the view count", "shadow views", [](vol::VolInputs& b) { b.shadow_view_count = 2; });
  mutate("the atlas's slot", "atlas slot", [](vol::VolInputs& b) { b.shadow_atlas_slot += 1; });
  mutate("the atlas's size", "atlas size", [](vol::VolInputs& b) { b.shadow_atlas_size[1] = 2048.0f; });
  // an empty rig leaves the entry alone; no atlas gives the volume none; a frame whose lamp is another record is found
  lm::Collector e;
  e.Reset();
  e.BeginFrame(1, 1.0, MakeSet(0));
  e.SetShadows<lg::LocalShadowFrame>(nullptr, lg::ShadowAtlasInfo{});
  vol::VolInputs entry = Entry(true), projected = Entry(true);
  lp::ProjectVolumeHeadlights(e.frame(), &projected);
  lm::SelfCheck sempty;
  lp::CheckVolumeBytes(projected, entry, &sempty);
  Check(sempty.total_bad() == 0 && projected.headlight_count == 0 && projected.shadow_atlas_slot == vol::kNoSlot, "volume headlights: an empty rig leaves the volume's inputs as they were");
  lm::Collector n;
  n.Reset();
  n.BeginFrame(1, 1.0, set);
  n.SetShadows<lg::LocalShadowFrame>(nullptr, lg::ShadowAtlasInfo{});
  vol::VolInputs a = Entry(false), b = Entry(false);
  LegacyHeadlights(set, nullptr, lg::ShadowAtlasInfo{}, &a);
  lp::ProjectVolumeHeadlights(n.frame(), &b);
  lm::SelfCheck sn;
  lp::CheckVolumeBytes(b, a, &sn);
  Check(sn.total_bad() == 0 && b.shadow_atlas_slot == vol::kNoSlot && b.headlights[0].shadow_view == -1 && b.headlight_count == 3,
        "volume headlights: lights with no maps are the volume's lights with no views and no atlas slot");
  // views only the second of which the volume can read: the second is the light's first (DropUnusableViews, shared with FillShadows)
  lg::LocalShadowFrame two;
  two.views[two.count++] = MakeView(0, 1, 0, 3);
  two.views[two.count++] = MakeView(0, 1, 1, 3);
  two.views[0].atlas_rect[2] = 5.0f;  // the first view's tile is outside the atlas
  lm::Collector d;
  d.Reset();
  d.BeginFrame(1, 1.0, set);
  d.SetShadows(&two, atlas);
  vol::VolInputs la = Entry(false), lb2 = Entry(false);
  LegacyHeadlights(set, &two, atlas, &la);
  lp::ProjectVolumeHeadlights(d.frame(), &lb2);
  lm::SelfCheck sd;
  lp::CheckVolumeBytes(lb2, la, &sd);
  Check(sd.total_bad() == 0 && lb2.headlights[0].shadow_view == 1 && lb2.headlights[0].shadow_view_b == -1, "volume headlights: only the second view readable: it becomes the light's first, as the legacy producer does");
  // when no light reads a readable view the volume has no table and no atlas slot
  lg::LocalShadowFrame bad_only;
  bad_only.views[bad_only.count++] = MakeView(0, 1, 0, 3);
  bad_only.views[0].atlas_rect[2] = 5.0f;
  lm::Collector f;
  f.Reset();
  f.BeginFrame(1, 1.0, set);
  f.SetShadows(&bad_only, atlas);
  vol::VolInputs fa = Entry(false), fb = Entry(false);
  LegacyHeadlights(set, &bad_only, atlas, &fa);
  lp::ProjectVolumeHeadlights(f.frame(), &fb);
  lm::SelfCheck sf;
  lp::CheckVolumeBytes(fb, fa, &sf);
  Check(sf.total_bad() == 0 && fb.shadow_view_count == 0 && fb.shadow_atlas_slot == vol::kNoSlot, "volume headlights: no readable view: no table, no atlas slot, the same as the legacy producer");
}

// ---------------------------------------------------------------------------
// 4c: the shadow replay's lights
// ---------------------------------------------------------------------------

bool SameLight(const ls::Light& a, const ls::Light& b) {
  return SameBytes(a.pos, b.pos, sizeof(a.pos)) && SameBytes(a.dir, b.dir, sizeof(a.dir)) && SameBytes(&a.cos_outer, &b.cos_outer, sizeof(double)) && SameBytes(&a.range, &b.range, sizeof(double)) &&
         a.car_key == b.car_key && a.is_player == b.is_player && a.index == b.index && SameBytes(a.lamp_offset, b.lamp_offset, sizeof(a.lamp_offset));
}

bool SameViewSpec(const ls::ViewSpec& a, const ls::ViewSpec& b) {
  return SameBytes(a.eye, b.eye, sizeof(a.eye)) && SameBytes(a.dir, b.dir, sizeof(a.dir)) && SameBytes(&a.fov, &b.fov, sizeof(double)) && SameBytes(&a.near_m, &b.near_m, sizeof(double)) &&
         SameBytes(&a.far_m, &b.far_m, sizeof(double)) && SameBytes(&a.lamps_m, &b.lamps_m, sizeof(double));
}

struct ShadowRun {
  uint64_t frames = 0, lit = 0, bad_lights = 0, bad_replay = 0, with_views = 0, refused = 0, cars_changes = 0;
  std::string first;
};

// Over the drive: the lights of each frame made from the rig's set and from the frame, compared; then two replays (two ViewPickers with the hysteresis they keep across frames, one fed each)
// are run on them and what they make is compared: the cars that hold views, their groups, the plan, the cone cull's view of each car and the published maps.
ShadowRun RunShadowLights(const std::vector<DriveFrame>& frames, lb::Level level, bool corrupt_some) {
  ShadowRun run;
  Replay from_set, from_frame;
  from_set.level = from_frame.level = level;
  lm::Collector c;
  c.Reset();
  uint64_t last_signature = ~0ull;
  for (const DriveFrame& f : frames) {
    mcla::gpu::HeadlightSet set = f.set;
    if (corrupt_some && set.count > 1 && f.frame % 11 == 0) {
      set.lights[1].pos[1] = std::numeric_limits<float>::quiet_NaN();  // a light the rules refuse (the rig never makes one: the guard is what is tested)
    }
    if (corrupt_some && set.count > 2 && f.frame % 13 == 0) {
      set.lights[2].lamp_offset[0] = 5.0f;  // a lamp offset longer than a car is wide: not believed, the light has one view
    }
    c.BeginFrame(uint32_t(f.frame), double(f.frame) / kFps, set);
    c.SetShadows<lg::LocalShadowFrame>(nullptr, lg::ShadowAtlasInfo{});
    ls::Light a[4], b[4];
    const uint32_t na = LegacyShadowLights(set, a);
    const uint32_t nb = lp::ProjectShadowLights(c.frame(), b);
    lm::SelfCheck sc;
    lp::CheckShadowLights(a, na, b, nb, &sc);
    bool same = na == nb;
    for (uint32_t i = 0; i < std::min(na, nb); ++i) {
      same = same && SameLight(a[i], b[i]);
    }
    ++run.frames;
    run.lit += na > 0 ? 1u : 0u;
    run.refused += na < set.count ? 1u : 0u;
    Check(same == (sc.total_bad() == 0), "shadow lights: the check agrees with a field by field compare");
    if (!same || sc.total_bad() != 0) {
      ++run.bad_lights;
      if (run.first.empty()) {
        run.first = "frame " + std::to_string(f.frame) + ": " + sc.TakeWindow().first;
      }
    }
    Check(lp::RigLightsOf(c.frame()).count == set.count, "shadow lights: the frame holds as many rig lights as the set (the replay's 'a headlight is lit')");
    const ReplayFrame ra = PublishFromLights(&from_set, a, na, f.camera, f.frame);
    const ReplayFrame rb = PublishFromLights(&from_frame, b, nb, f.camera, f.frame);
    bool replay_same = ra.cars == rb.cars && ra.plan.count == rb.plan.count && ra.plan.cars == rb.plan.cars && ra.published.count == rb.published.count &&
                       SameBytes(&ra.published, &rb.published, sizeof(ra.published.views)) ;
    for (uint32_t g = 0; g < ra.cars && replay_same; ++g) {
      replay_same = ra.groups[g].car_key == rb.groups[g].car_key && ra.groups[g].count == rb.groups[g].count && ra.groups[g].is_player == rb.groups[g].is_player &&
                    SameBytes(ra.groups[g].eye, rb.groups[g].eye, sizeof(ra.groups[g].eye)) && ra.groups[g].members == rb.groups[g].members;
    }
    for (uint32_t v = 0; v < ra.plan.count && replay_same; ++v) {
      replay_same = ra.plan.views[v].car == rb.plan.views[v].car && ra.plan.views[v].side == rb.plan.views[v].side &&
                    SameBytes(ra.plan.views[v].group.eye, rb.plan.views[v].group.eye, sizeof(ra.plan.views[v].group.eye)) && SameLight(ra.plan.views[v].lights[0], rb.plan.views[v].lights[0]);
    }
    for (uint32_t k = 0; k < 4 && replay_same; ++k) {
      replay_same = ra.early_ok[k] == rb.early_ok[k] && (!ra.early_ok[k] || SameViewSpec(ra.early[k], rb.early[k]));
    }
    if (!replay_same) {
      ++run.bad_replay;
      if (run.first.empty()) {
        run.first = "the views of frame " + std::to_string(f.frame) + " differ";
      }
    }
    run.with_views += ra.published.count != 0 ? 1u : 0u;
    uint64_t signature = ra.cars;  // which cars hold views: a change of the count or of any car
    for (uint32_t g = 0; g < ra.cars; ++g) {
      signature = signature * 1000003u + ra.groups[g].car_key;
    }
    run.cars_changes += signature != last_signature ? 1u : 0u;
    last_signature = signature;
    c.EndFrame();
  }
  return run;
}

void TestShadowLightsOverTheDrive(const std::vector<DriveFrame>& frames, const std::string& name) {
  for (const lb::Level level : {lb::Level::kMedium, lb::Level::kHigh, lb::Level::kUltra}) {
    for (const bool corrupt : {false, true}) {
      const ShadowRun run = RunShadowLights(frames, level, corrupt);
      const std::string at = " (" + name + ", " + LevelName(level) + (corrupt ? ", with lights the rules refuse and a lamp offset that is not believed" : "") + ")";
      std::printf("INFO: shadow lights%s: %llu frames, %llu lit, %llu with maps published, %llu changes of the cars that hold views, %llu frames with a refused light\n", at.c_str(),
                  (unsigned long long)run.frames, (unsigned long long)run.lit, (unsigned long long)run.with_views, (unsigned long long)run.cars_changes, (unsigned long long)run.refused);
      Check(run.bad_lights == 0, "shadow lights: the lights made from the frame are the ones made from the rig's set bit for bit in each of " + std::to_string(run.frames) + " frames" + at + ", " +
                                     std::to_string(run.bad_lights) + " differed: " + run.first);
      Check(run.bad_replay == 0, "shadow lights: the cars, plans, cone cull views and published maps of two replays, one fed each, are the same in each frame" + at + ", " + std::to_string(run.bad_replay) +
                                     " differed: " + run.first);
      Check(run.with_views > run.lit / 2 && run.cars_changes > 5, "shadow lights: the drive has maps and changes of the cars that hold them" + at);
      if (corrupt) {
        Check(run.refused > 0, "shadow lights: the rules refused some light" + at);
      }
    }
  }
}

// Every field of a replay light that is not the rig's is found.
void TestShadowLightMutations() {
  const mcla::gpu::HeadlightSet set = MakeSet(3);
  lm::Collector c;
  c.Reset();
  c.BeginFrame(1, 1.0, set);
  ls::Light legacy[4], mine[4];
  const uint32_t n = LegacyShadowLights(set, legacy);
  const uint32_t nm = lp::ProjectShadowLights(c.frame(), mine);
  Check(n == 3 && nm == 3, "shadow light mutations: three lights each way");
  const auto first = [&](const ls::Light* other, uint32_t count) {
    lm::SelfCheck sc;
    lp::CheckShadowLights(other, count, mine, nm, &sc);
    return sc.TakeWindow().first;
  };
  Check(first(legacy, n).empty(), "shadow light mutations: the rig's lights pass");
  const auto mutate = [&](const char* what, const char* part, auto&& edit) {
    ls::Light l[4] = {legacy[0], legacy[1], legacy[2], legacy[3]};
    edit(l);
    const std::string f = first(l, n);
    Check(!f.empty() && f.find(part) != std::string::npos, std::string("shadow light mutations: a difference in ") + what + " is found as '" + part + "' (" + f + ")");
  };
  mutate("a position by one unit in the last place", "its position", [](ls::Light* l) { l[1].pos[2] = std::nextafter(l[1].pos[2], 1.0e9); });
  mutate("an axis", "its axis", [](ls::Light* l) { l[0].dir[1] += 1.0e-12; });
  mutate("the cone", "its cone", [](ls::Light* l) { l[2].cos_outer -= 1.0e-9; });
  mutate("the range", "its range", [](ls::Light* l) { l[0].range += 0.5; });
  mutate("the car", "its car", [](ls::Light* l) { l[1].car_key += 1; });
  mutate("the player flag", "player", [](ls::Light* l) { l[2].is_player = true; });
  mutate("the place in the rig", "its place in the rig", [](ls::Light* l) { l[2].index = 1; });
  mutate("the lamp offset", "its lamp offset", [](ls::Light* l) { l[0].lamp_offset[1] = 0.01; });
  lm::SelfCheck sc;
  lp::CheckShadowLights(legacy, 2, mine, nm, &sc);
  Check(!sc.TakeWindow().first.empty(), "shadow light mutations: a light too few is found");
  // a refused light in the middle keeps the places of the others
  mcla::gpu::HeadlightSet holey = set;
  holey.lights[1].dir[0] = holey.lights[1].dir[1] = holey.lights[1].dir[2] = 0.0f;  // no direction: refused
  lm::Collector d;
  d.Reset();
  d.BeginFrame(1, 1.0, holey);
  ls::Light a[4], b[4];
  const uint32_t na = LegacyShadowLights(holey, a);
  const uint32_t nb = lp::ProjectShadowLights(d.frame(), b);
  Check(na == 2 && nb == 2 && a[1].index == 2 && b[1].index == 2 && SameLight(a[1], b[1]), "shadow lights: a refused light leaves the next one its own place in the rig (index 2), from the frame too");
}

}  // namespace


// ---------------------------------------------------------------------------
// U5: the shadow views from the atlas's published form, by light id
// ---------------------------------------------------------------------------

// The replay's views made lookup-friendly (the identity as the matrix: the world point IS the atlas uv and the depth; a tile of a third of the atlas): both paths read the same views, the
// identity test is about which view goes to which light, in which order.
void ReshapeForLookup(lg::LocalShadowFrame* f) {
  for (uint32_t v = 0; v < f->count; ++v) {
    lg::LocalShadowView& x = f->views[v];
    std::memset(x.world_to_shadow, 0, sizeof(x.world_to_shadow));
    x.world_to_shadow[0] = x.world_to_shadow[5] = x.world_to_shadow[10] = x.world_to_shadow[15] = 1.0f;
    x.atlas_rect[0] = float(v % 3) / 3.0f;
    x.atlas_rect[1] = 0.0f;
    x.atlas_rect[2] = float(v % 3 + 1) / 3.0f;
    x.atlas_rect[3] = 1.0f;
    x.depth_bias = 0.002f;
    x.texel_size = 1.0f / 512.0f;
  }
}

// A depth for texel (ix, iy) of the test atlas (1536 x 512): a quarter nothing drawn, the rest in [0, 1), the index clamped as a sampler's is.
float StoredDepth(int ix, int iy) {
  ix = std::clamp(ix, 0, 1535);
  iy = std::clamp(iy, 0, 511);
  uint32_t h = uint32_t(ix) * 73856093u ^ uint32_t(iy) * 19349663u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return (h & 3u) == 0 ? 1.0f : float(h >> 8 & 0xFFFFu) / 65536.0f;
}

struct AtlasTotals {
  uint64_t frames = 0, compares = 0, bad = 0;
  uint64_t block_compares = 0, block_bad = 0;
  uint64_t lookups = 0, lookups_bad = 0, lookups_shadowed = 0;
  uint64_t with_views = 0, two_views = 0, kept_across = 0;
  std::string first;
};

// The atlas as LocalShadow mirrors the legacy AtlasOf: valid, the texture's slot, the point sampler, the size, one tile size.
lg::ShadowAtlasInfo LegacyAtlasInfo(const sa::Layout& layout, uint32_t slot, uint32_t sampler) {
  lg::ShadowAtlasInfo a;
  a.valid = true;
  a.slot = slot;
  a.sampler = sampler;
  a.width = layout.width;
  a.height = layout.height;
  a.tile_small = layout.pools[0].request.tile;
  return a;
}

bool SameRefs(const lg::LightFrame& a, const lg::LightFrame& b, std::string* why) {
  if (a.count != b.count) {
    *why = "the slots in use differ";
    return false;
  }
  for (uint32_t i = 0; i < a.count; ++i) {
    const lg::ShadowRef& x = a.lights[i].shadow;
    const lg::ShadowRef& y = b.lights[i].shadow;
    if (x.kind != y.kind || x.first_view != y.first_view || x.view_count != y.view_count || x.mask_channel != y.mask_channel || x.weight != y.weight || x.reach != y.reach) {
      *why = "the shadow ref of slot " + std::to_string(i) + " differs";
      return false;
    }
  }
  for (uint32_t r = 0; r < lg::kRoleCount; ++r) {
    if (a.role_count[r] != b.role_count[r] || std::memcmp(a.order[r].data(), b.order[r].data(), a.order[r].size()) != 0) {
      *why = "the ranking of role " + std::to_string(r) + " differs";
      return false;
    }
  }
  return true;
}

AtlasTotals RunAtlas(const std::vector<DriveFrame>& frames, lb::Level level, int views_override) {
  AtlasTotals t;
  Replay replay;
  replay.level = level;
  replay.views_override = views_override;
  lm::Collector legacy_c, atlas_c;  // the rig's carry and key remap; the atlas's published form by id
  lm::ShadowCarry<lg::LocalShadowFrame> carry;
  legacy_c.Reset();
  atlas_c.Reset();
  carry.Clear();
  const sa::Layout layout = sa::LayoutForLevel(level, views_override);
  const uint32_t slot = 41, sampler = 2;
  const lg::ShadowAtlasInfo atlas = LegacyAtlasInfo(layout, slot, sampler);
  sa::Published kept;  // what the shadow atlas holds: the last trigger's views with their lights' ids
  bool had_trigger = false;
  uint64_t seed = 12345;
  const auto next = [&seed]() {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return float(seed >> 40) / float(1u << 24);
  };
  for (const DriveFrame& f : frames) {
    ++t.frames;
    const mcla::gpu::HeadlightSet& set = f.set;
    const float camera[3] = {float(f.camera.eye[0]), float(f.camera.eye[1]), float(f.camera.eye[2])};
    legacy_c.BeginFrame(uint32_t(f.frame), double(f.frame) / kFps, set);
    atlas_c.BeginFrame(uint32_t(f.frame), double(f.frame) / kFps, set);
    if (!had_trigger) {  // the replay gives its maps up when the frame before had no trigger: the carry and the atlas's published form with them
      carry.Clear();
      kept = sa::Published{};
    }
    const std::array<uint32_t, 4> keys = KeysOf(set);
    const lg::LocalShadowFrame moved = carry.ForSet(keys.data(), set.count, kRemap);
    const lg::ShadowAtlasInfo carried = carry.has() ? carry.atlas() : lg::ShadowAtlasInfo{};
    legacy_c.SetShadows(moved.count != 0 ? &moved : nullptr, carried);
    std::array<uint64_t, 4> ids{};
    const uint32_t id_count = atlas_c.RigIds(ids.data());
    const lg::ShadowFrame rebound = sa::RebindByIds(kept.frame, ids.data(), id_count);
    atlas_c.AdoptShadows(rebound);
    t.kept_across += kept.frame.count != 0 && rebound.count != 0 ? 1u : 0u;
    const auto compare = [&](const lg::LocalShadowFrame* maps, uint32_t atlas_slot, const char* when) {
      std::string why = sa::DiffShadowFrames(legacy_c.frame().shadow, atlas_c.frame().shadow);
      if (why.empty()) {
        SameRefs(legacy_c.frame(), atlas_c.frame(), &why);
      }
      ++t.compares;
      if (!why.empty()) {
        ++t.bad;
        if (t.first.empty()) {
          t.first = std::string(when) + " of frame " + std::to_string(f.frame) + ": " + why;
        }
      }
      // the headlight block projected from the atlas-sourced frame is the rig's, byte for byte
      const hr::HeadlightConstants legacy_block = RigBlock(set, maps, atlas_slot, sampler, camera);
      const hr::HeadlightConstants projected = lp::ProjectHeadlightBlock(atlas_c.frame(), camera);
      ++t.block_compares;
      if (!SameBytes(&legacy_block, &projected, sizeof(legacy_block))) {
        ++t.block_bad;
        if (t.first.empty()) {
          t.first = std::string(when) + " of frame " + std::to_string(f.frame) + ": the block projected from the atlas's frame is not the rig's";
        }
      }
      // THE ONE LOOKUP: each rig light's visibility at points in and around its views, the block's own views (the variant's kernel: the mean of up to two views, seen / views) against LightLit
      // of the light's ShadowRef in the frame (weight 1: the mean itself)
      const lp::RigLights rig = lp::RigLightsOf(atlas_c.frame());
      for (uint32_t i = 0; i < rig.count && i < set.count; ++i) {
        const uint32_t views[2] = {legacy_block.shadow_of[i], legacy_block.shadow_of_b[i]};
        for (int n = 0; n < 6; ++n) {
          float p[3] = {next() * 1.1f - 0.05f, next(), 0.05f + 0.9f * next()};
          const uint32_t pick = views[0] != hr::kNoView ? (next() < 0.5f || views[1] == hr::kNoView ? views[0] : views[1]) : hr::kNoView;
          if (pick != hr::kNoView && pick < lg::kMaxShadowViews) {
            const lg::LocalShadowView& v = legacy_block.shadow[pick];
            p[0] = v.atlas_rect[0] + (v.atlas_rect[2] - v.atlas_rect[0]) * (0.02f + 0.96f * next());
          }
          const auto depth = [](int x, int y) { return StoredDepth(x, y); };
          float sum = 0.0f;
          float count = 0.0f;
          for (const uint32_t k : views) {
            if (k != hr::kNoView && k < lg::kMaxShadowViews) {
              sum += sl::ViewLit(legacy_block.shadow[k], p, 1536, 512, sl::Mode::kSurface, depth);
              count += 1.0f;
            }
          }
          const float want = count > 0.0f ? sum / count : 1.0f;
          lg::ShadowFrame view_frame = atlas_c.frame().shadow;
          view_frame.atlas.width = 1536;
          view_frame.atlas.height = 512;
          const float got = sl::LightLit(view_frame, atlas_c.frame().lights[rig.slot[i]].shadow, p, sl::Mode::kSurface, depth);
          ++t.lookups;
          t.lookups_shadowed += want < 1.0f ? 1u : 0u;
          if (std::memcmp(&want, &got, sizeof(float)) != 0) {
            ++t.lookups_bad;
            if (t.first.empty()) {
              t.first = std::string(when) + " of frame " + std::to_string(f.frame) + ": the lookup of rig light " + std::to_string(i) + " differs: " + std::to_string(want) + " against " + std::to_string(got);
            }
          }
        }
      }
      return legacy_block;
    };
    const hr::HeadlightConstants before = compare(moved.count != 0 ? &moved : nullptr, carry.has() ? slot : hr::kNoSlot, "start");
    t.with_views += before.shadow_of[0] != hr::kNoView ? 1u : 0u;
    const bool trigger = set.count > 0 && f.frame % 37 != 0;
    had_trigger = trigger;
    if (trigger) {
      lg::LocalShadowFrame made = PublishViews(&replay, set, f.camera, f.frame);
      ReshapeForLookup(&made);
      // the legacy: the rig's carry, the manager's conversion with this set's ids
      carry.Set(made.count != 0 ? &made : nullptr, atlas, keys.data(), set.count);
      legacy_c.SetShadows(made.count != 0 ? &made : nullptr, made.count != 0 ? atlas : lg::ShadowAtlasInfo{});
      // the atlas: the replay hands its views with the id of each rig light (a car's lamp id from the car's key: LocalShadow::CollectViews), the atlas builds the published form
      ls::Light lights[4];
      const uint32_t light_count = LegacyShadowLights(set, lights);
      std::array<uint64_t, 4> published_ids{};
      for (uint32_t i = 0; i < light_count; ++i) {
        published_ids[lights[i].index] = lg::light_id::CarLampId(lights[i].car_key);
      }
      sa::ShadowFrameBuilder builder;
      for (uint32_t v = 0; v < made.count; ++v) {
        builder.AddView(made.views[v], made.views[v].light_index < 4 ? published_ids[made.views[v].light_index] : lg::kNoLightId);
      }
      kept = made.count != 0 ? builder.Build(sa::InfoOf(layout, slot, sampler, true)) : sa::Published{};
      atlas_c.AdoptShadows(kept.frame);
      const hr::HeadlightConstants after = compare(made.count != 0 ? &made : nullptr, made.count != 0 ? slot : hr::kNoSlot, "trigger");
      bool two = false;
      for (uint32_t i = 0; i < 4; ++i) {
        two = two || after.shadow_of_b[i] != hr::kNoView;
      }
      t.two_views += two ? 1u : 0u;
    } else if (set.count == 0) {
      carry.Clear();
      kept = sa::Published{};
    }
    legacy_c.EndFrame();
    atlas_c.EndFrame();
  }
  return t;
}

void TestAtlasOverTheDrive(const std::vector<DriveFrame>& frames, const std::string& name) {
  for (const lb::Level level : {lb::Level::kMedium, lb::Level::kHigh, lb::Level::kUltra}) {
    const AtlasTotals t = RunAtlas(frames, level, 0);
    const std::string at = " (" + name + ", " + LevelName(level) + ")";
    std::printf("INFO: %s, %s, atlas: %llu frames, %llu compares of the frame's shadow views, %llu of the projected block, %llu lookups (%llu in shadow), views kept across frames %llu, %llu with two\n", name.c_str(),
                LevelName(level), (unsigned long long)t.frames, (unsigned long long)t.compares, (unsigned long long)t.block_compares, (unsigned long long)t.lookups,
                (unsigned long long)t.lookups_shadowed, (unsigned long long)t.kept_across, (unsigned long long)t.two_views);
    Check(t.bad == 0, "the shadow views read from the atlas by id are the carry's, with the same refs and rankings" + at + ": " + t.first);
    Check(t.block_bad == 0, "the headlight block projected from the atlas-sourced frame is the rig's block, byte for byte" + at + ": " + t.first);
    Check(t.lookups_bad == 0, "every light's one lookup is the variant's kernel over the block's own views, bit for bit" + at + ": " + t.first);
    Check(t.compares > frames.size() && t.with_views > frames.size() / 10 && t.kept_across > frames.size() / 10 && t.lookups > 1000 && t.lookups_shadowed > 100,
          "the drive has frames with views kept across frames and lookups in shadow to compare" + at + " (" + std::to_string(t.with_views) + ", " + std::to_string(t.kept_across) + ", " +
              std::to_string(t.lookups_shadowed) + ")");
  }
  // the debug setting's one view for the car / a view for each lamp: other pool sizes, the same identity
  for (const int per_car : {1, 2}) {
    const AtlasTotals t = RunAtlas(frames, lb::Level::kUltra, per_car);
    Check(t.bad == 0 && t.block_bad == 0 && t.lookups_bad == 0, "the debug views setting " + std::to_string(per_car) + " (" + name + "): " + t.first);
  }
}

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
  TestVolumeLamps();
  TestVolumeMutations();
  TestVolumeWithoutTrigger();
  TestVolumeHeadlightsOverTheDrive(frames, "the made-up drive");
  TestShadowLightMutations();
  TestShadowLightsOverTheDrive(frames, "the made-up drive");
  TestAtlasOverTheDrive(frames, "the made-up drive");

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
        TestVolumeHeadlightsOverTheDrive(recorded, "the recording");
        TestShadowLightsOverTheDrive(recorded, "the recording");
        TestAtlasOverTheDrive(recorded, "the recording");
      } else {
        Check(false, "the recording " + path + " is read: " + error);
      }
    }
  }
  std::printf("light_project_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

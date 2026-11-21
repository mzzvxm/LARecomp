// Check of the passive LightManager's rules (light_manager_rules.h; perf/LIGHT-UNIFY.md package LU-2 = step U2): the slot table (a light keeps its slot while it lives), the
// collector (the rig's headlights with their ids, slots, velocity and order; the volume's street lamps found in the game's list with the picker's weight), the shadow bookkeeping
// (the replay's maps as the rig keeps them), the GPU buffer, and the self-checks against the REAL structs of the headlight block, the volume's inputs and the lamp picker: a frame
// the manager made says what they say (bit for bit but for the picker's weight), and a deliberate difference in any one field is found. A program of its own: no renderer, no
// ReXGlue, no GPU, no game; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_light_manager_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_light_manager_test

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "headlight_rig_rules.h"
#include "headlight_types.h"
#include "light_manager_rules.h"
#include "local_shadow_types.h"
#include "vol_lamp_list.h"
#include "volumetrics_headlights.h"
#include "volumetrics_lamps.h"
#include "volumetrics_rules.h"
#include "volumetrics_types.h"

namespace lg = larecomp::gpu;
namespace lm = larecomp::gpu::light_manager;
namespace hr = mcla::gpu::headlight_rules;
namespace vol = mcla::gpu::volumetrics;
namespace vl = mcla::gpu::vol_lamps;

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

// ---------------------------------------------------------------------------
// the producers, as small as they can be and made with the real types
// ---------------------------------------------------------------------------

struct Car {
  uint32_t key;
  float pos[3];
  bool player = false;
  float colour = 1.0f;
};

mcla::gpu::HeadlightSet MakeSet(const std::vector<Car>& cars) {
  mcla::gpu::HeadlightSet set;
  set.count = 0;
  for (const Car& c : cars) {
    mcla::gpu::Headlight h{};
    for (int i = 0; i < 3; ++i) {
      h.pos[i] = c.pos[i];
      h.color[i] = hr::kColor[i] * c.colour;
    }
    h.dir[0] = 0.0f;
    h.dir[1] = -0.0349f;
    h.dir[2] = 0.9994f;
    h.cos_outer = hr::CosOfDegrees(hr::kOuterDegrees);
    h.cos_inner = hr::CosOfDegrees(hr::kInnerDegrees);
    h.range = hr::kRange;
    h.car_key = c.key;
    h.is_player = c.player;
    if (set.count < set.lights.size()) {
      set.lights[set.count++] = h;
    }
  }
  return set;
}

std::array<uint32_t, 4> KeysOf(const mcla::gpu::HeadlightSet& set) {
  std::array<uint32_t, 4> k{};
  for (uint32_t i = 0; i < set.count; ++i) {
    k[i] = set.lights[i].car_key;
  }
  return k;
}

// The rig's block exactly as HeadlightRig::Publish / RebuildBlock make it: the lamps of the set, the camera, the replay's views moved to this set by the car, the atlas.
hr::HeadlightConstants RigBlock(const mcla::gpu::HeadlightSet& set, const lg::LocalShadowFrame* remapped, uint32_t atlas_slot, uint32_t sampler) {
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
  const float camera[3] = {1.0f, 2.0f, 3.0f};
  return hr::MakeConstants(lamps.data(), set.count, camera, remapped != nullptr && remapped->count != 0 ? remapped : nullptr, atlas_slot, sampler);
}

lg::LocalShadowView MakeView(uint32_t light_index, float seed, bool valid = true) {
  lg::LocalShadowView v{};
  for (int i = 0; i < 16; ++i) {
    v.world_to_shadow[i] = seed + 0.125f * float(i);
  }
  v.atlas_rect[0] = 0.0f;
  v.atlas_rect[1] = 0.0f;
  v.atlas_rect[2] = 0.5f;
  v.atlas_rect[3] = 0.5f;
  v.depth_bias = 0.002f;
  v.texel_size = 1.0f / 512.0f;
  v.light_index = light_index;
  v.valid = valid ? 1u : 0u;
  return v;
}

lg::ShadowAtlasInfo MakeAtlas(uint32_t slot = 37) {
  lg::ShadowAtlasInfo a;
  a.valid = true;
  a.slot = slot;
  a.sampler = 2;  // descriptors.h kSamplerPointClamp
  a.width = 1024;
  a.height = 512;
  a.tile_small = 512;
  return a;
}

// headlight_rules::RemapShadowViews, as the glue hands it in.
const auto kRemap = [](const lg::LocalShadowFrame& f, const uint32_t* ok, uint32_t oc, const uint32_t* nk, uint32_t nc) { return hr::RemapShadowViews(f, ok, oc, nk, nc); };

// ---------------------------------------------------------------------------
// the slot table
// ---------------------------------------------------------------------------

void TestSlotTable() {
  lm::SlotTable t;
  t.Clear();
  t.Begin();
  const uint32_t a = t.Slot(100), b = t.Slot(200), c = t.Slot(300);
  Check(a == 0 && b == 1 && c == 2, "slot table: the lowest free slots in order");
  Check(t.added() == 3 && t.kept() == 0, "slot table: three new ids");
  Check(t.Slot(200) == b, "slot table: the same id in the same frame is the same slot");
  Check(t.Slot(lg::kNoLightId) == lg::kNoLightSlot, "slot table: id 0 has no slot");
  t.End();
  t.Begin();
  // 100 and 300 again, 200 not: its slot is held until End (a new id does not take it in this frame), 400 is new
  Check(t.Slot(300) == c && t.Slot(100) == a, "slot table: a light that lives keeps its slot");
  const uint32_t d = t.Slot(400);
  Check(d == 3, "slot table: a new id takes a free slot, not the one of a light about to be freed");
  t.End();
  Check(t.gone() == 1 && t.kept() == 2 && t.added() == 1, "slot table: one gone, two kept, one added");
  t.Begin();
  Check(t.Slot(500) == 1, "slot table: a freed slot is taken again (the lowest)");
  t.End();
  // full: 64 ids, then a 65th takes the slot of an id not seen in this frame
  t.Clear();
  t.Begin();
  for (uint64_t i = 1; i <= lg::kMaxLights; ++i) {
    t.Slot(i);
  }
  t.End();
  t.Begin();
  for (uint64_t i = 2; i <= lg::kMaxLights; ++i) {
    t.Slot(i);  // id 1 is not seen
  }
  Check(t.Slot(9999) == 0, "slot table: a full table gives a new id the slot of one not seen in this frame");
  Check(t.Slot(10000) == lg::kNoLightSlot && t.overflow() == 1, "slot table: and none when every slot was seen in this frame");
}

// ---------------------------------------------------------------------------
// the headlights
// ---------------------------------------------------------------------------

void TestHeadlights() {
  lm::Collector c;
  c.Reset();
  std::vector<Car> cars = {{7, {10, 1, 5}, true}, {9, {20, 1, 8}}, {3, {-4, 1, 30}}};
  c.BeginFrame(100, 1.0, MakeSet(cars));
  const lg::LightFrame& f = c.frame();
  Check(c.rig().count == 3 && f.count == 3, "headlights: three lights in the frame");
  Check(c.stats().headlights == 3 && c.stats().player == 1, "headlights: the census counts three, one the player's");
  for (uint32_t i = 0; i < 3; ++i) {
    const lg::Light& l = f.lights[c.rig().slot[i]];
    Check(l.source == lg::LightSource::kHeadlight && l.law == lg::LightLaw::kHeadlight && l.car_key == cars[i].key, "headlights: the light is the car's");
    Check(l.id == lg::light_id::CarLampId(cars[i].key, lg::light_id::CarLamp::kPair), "headlights: the id is the car's pair id");
    Check(f.Find(l.id) == int32_t(c.rig().slot[i]), "headlights: Find gives the slot");
    Check(l.roles == lg::kRoleMaskAll && l.vol_weight == 1.0f && l.state == lg::FadeState::kSteady, "headlights: every role, weight 1");
  }
  Check((f.lights[c.rig().slot[0]].flags & lg::kLightFlagPlayer) != 0 && (f.lights[c.rig().slot[1]].flags & lg::kLightFlagPlayer) == 0, "headlights: the player flag follows the rig's");
  Check(f.role_count[uint32_t(lg::LightRole::kSurface)] == 3 && f.role_count[uint32_t(lg::LightRole::kVolume)] == 3 && f.role_count[uint32_t(lg::LightRole::kShadow)] == 0,
        "headlights: surface and volume rank the three, the shadow role none");
  for (uint32_t r = 0; r < 3; ++r) {
    Check(f.order[uint32_t(lg::LightRole::kSurface)][r] == c.rig().slot[r], "headlights: the surface ranking is the rig's order");
  }
  lm::SelfCheck sc;
  lm::CheckFrameInvariants(f, nullptr, &sc);
  Check(sc.total_bad() == 0 && sc.total_checks() == 1, "headlights: the frame's invariants hold");
  c.EndFrame();

  // The next frame: the rig's order changes (the player's car and another swap), a new car comes in, car 9 leaves: ids and slots stay with the cars, velocity is the step over the clock.
  std::vector<Car> next = {{9, {20, 1, 8}}, {7, {11, 1, 5}, true}, {3, {-4, 1, 30}}, {12, {0, 1, 0}}};
  next.erase(next.begin());  // 9 is gone
  next.push_back({15, {1, 1, 1}});
  c.BeginFrame(101, 1.0 + 1.0 / 60.0, MakeSet(next));
  const lg::LightFrame& g = c.frame();
  Check(c.rig().count == 4, "headlights: four lights in the second frame");
  const int32_t s7_before = c.previous().Find(lg::light_id::CarLampId(7));
  const int32_t s7_after = g.Find(lg::light_id::CarLampId(7));
  Check(s7_before >= 0 && s7_before == s7_after, "headlights: a car's light keeps its slot");
  const int32_t s3_before = c.previous().Find(lg::light_id::CarLampId(3));
  Check(s3_before == g.Find(lg::light_id::CarLampId(3)), "headlights: another car's keeps its slot too");
  Check(g.Find(lg::light_id::CarLampId(9)) < 0, "headlights: a car that left has no light");
  const lg::Light& l7 = g.lights[uint32_t(s7_after)];
  Check(std::fabs(l7.vel[0] - 60.0f) < 0.01f && l7.vel[1] == 0.0f && l7.vel[2] == 0.0f, "headlights: velocity is the step over the scene clock (1 m in 1/60 s = 60 m/s)");
  const lg::Light& l3 = g.lights[uint32_t(s3_before)];
  Check(l3.vel[0] == 0.0f && l3.vel[2] == 0.0f, "headlights: a car that did not move has no velocity");
  const lg::Light& l15 = g.lights[uint32_t(g.Find(lg::light_id::CarLampId(15)))];
  Check(l15.vel[0] == 0.0f, "headlights: a new light has no velocity");
  Check(c.stats().slots_kept == 0, "headlights: the census of kept slots is made at the end of the frame");
  lm::SelfCheck sc2;
  lm::CheckFrameInvariants(g, &c.previous(), &sc2);
  Check(sc2.total_bad() == 0, "headlights: the second frame's invariants hold, slots kept");
  c.EndFrame();
  Check(c.stats().slots_kept == 2 && c.stats().slots_added == 2 && c.stats().slots_gone == 1, "headlights: kept 2 (7 and 3), added 2 (12 and 15), gone 1 (9)");

  // A gap or a jump: no velocity.
  std::vector<Car> jump = {{7, {400, 1, 5}, true}};
  c.BeginFrame(102, 1.0 + 2.0 / 60.0, MakeSet(jump));
  Check(c.frame().lights[uint32_t(c.frame().Find(lg::light_id::CarLampId(7)))].vel[0] == 0.0f, "headlights: a step of 389 m is a jump, not a speed");
  c.EndFrame();
  std::vector<Car> gap = {{7, {401, 1, 5}, true}};
  c.BeginFrame(103, 9.0, MakeSet(gap));
  Check(c.frame().lights[uint32_t(c.frame().Find(lg::light_id::CarLampId(7)))].vel[0] == 0.0f, "headlights: after a gap of seconds there is no velocity");
  c.EndFrame();

  // Two cars with one key: the ids stay unique.
  lm::Collector d;
  d.Reset();
  std::vector<Car> twins = {{5, {0, 0, 0}}, {5, {3, 0, 0}}};
  d.BeginFrame(1, 0.0, MakeSet(twins));
  Check(d.rig().count == 2 && d.frame().lights[d.rig().slot[0]].id != d.frame().lights[d.rig().slot[1]].id && d.stats().duplicate_ids == 1, "headlights: a repeated key gets another id");
  lm::SelfCheck sc3;
  lm::CheckFrameInvariants(d.frame(), nullptr, &sc3);
  Check(sc3.total_bad() == 0, "headlights: and the frame is still sound");

  // An empty set, and a set of more than four (the rig never gives more, the collector does not take them).
  lm::Collector e;
  e.Reset();
  e.BeginFrame(1, 0.0, MakeSet({}));
  Check(e.frame().count == 0 && e.rig().count == 0, "headlights: no light, no frame");
}

// ---------------------------------------------------------------------------
// the headlight block, through the rig's own sequence of frames
// ---------------------------------------------------------------------------

template <class BlockT>
bool BlockPasses(const lm::Collector& c, const BlockT& block, lm::Check kind) {
  lm::SelfCheck sc;
  lm::CheckHeadlightBlock(c.frame(), c.rig(), block, kind, &sc);
  return sc.total_bad() == 0;
}

void TestBlock() {
  lm::Collector c;
  lm::ShadowCarry<lg::LocalShadowFrame> carry;
  c.Reset();
  carry.Clear();
  const lg::ShadowAtlasInfo atlas = MakeAtlas();
  uint32_t all = 0, bad = 0;
  // Frame 1: three cars, no maps yet. Frame 2: the maps of frame 1's trigger moved to a set in another order with a car gone and one new. The trigger: this frame's maps.
  const std::vector<std::vector<Car>> sets = {
      {{7, {10, 1, 5}, true}, {9, {20, 1, 8}}, {3, {-4, 1, 30}}},
      {{9, {21, 1, 8}}, {7, {11, 1, 5}, true}, {15, {1, 1, 1}}},
      {{7, {12, 1, 5}, true}, {15, {1, 2, 1}}, {9, {22, 1, 8}}, {3, {-4, 1, 29}}},
      {{15, {1, 3, 1}}},
      {},
  };
  double clock = 10.0;
  for (size_t n = 0; n < sets.size(); ++n) {
    const mcla::gpu::HeadlightSet set = MakeSet(sets[n]);
    const std::array<uint32_t, 4> keys = KeysOf(set);
    // BeginFrame: the lights of the set, the last trigger's maps moved to it.
    c.BeginFrame(uint32_t(200 + n), clock += 1.0 / 60.0, set);
    const lg::LocalShadowFrame moved = carry.ForSet(keys.data(), set.count, kRemap);
    c.SetShadows(moved.count != 0 ? &moved : nullptr, carry.has() ? carry.atlas() : lg::ShadowAtlasInfo{});
    const hr::HeadlightConstants before = RigBlock(set, moved.count != 0 ? &moved : nullptr, carry.has() ? atlas.slot : hr::kNoSlot, atlas.sampler);
    ++all;
    bad += BlockPasses(c, before, lm::Check::kBlockBefore) ? 0u : 1u;
    // The trigger: this frame's maps, one for each light of the set (a view for the first two lights only, one of them not valid on frame 3).
    lg::LocalShadowFrame made;
    for (uint32_t i = 0; i < set.count && i < 2; ++i) {
      made.views[made.count++] = MakeView(i, float(10 * n + i), !(n == 2 && i == 1));
    }
    carry.Set(made.count != 0 ? &made : nullptr, atlas, keys.data(), set.count);
    c.SetShadows(made.count != 0 ? &made : nullptr, made.count != 0 ? atlas : lg::ShadowAtlasInfo{});
    const hr::HeadlightConstants after = RigBlock(set, made.count != 0 ? &made : nullptr, made.count != 0 ? atlas.slot : hr::kNoSlot, atlas.sampler);
    ++all;
    bad += BlockPasses(c, after, lm::Check::kBlockAfter) ? 0u : 1u;
    lm::SelfCheck sc;
    lm::CheckFrameInvariants(c.frame(), c.has_previous() ? &c.previous() : nullptr, &sc);
    Check(sc.total_bad() == 0, "block: the frame's invariants hold with views");
    if (n == 0) {
      Check(c.frame().role_count[uint32_t(lg::LightRole::kShadow)] == 2, "block: two lights have views");
      Check(c.frame().shadow.count == 2 && c.frame().lights[c.rig().slot[0]].shadow.kind == lg::ShadowKind::kAtlas, "block: the views are the frame's");
    }
    c.EndFrame();
  }
  Check(bad == 0, "block: the frame equals the rig's block at the start and after the trigger in each of " + std::to_string(all) + " checks (" + std::to_string(bad) + " differed)");

  // Mutations: each one field of the block, one by one, is found.
  const mcla::gpu::HeadlightSet set = MakeSet(sets[0]);
  const std::array<uint32_t, 4> keys = KeysOf(set);
  lm::Collector m;
  m.Reset();
  m.BeginFrame(1, 1.0, set);
  lg::LocalShadowFrame made;
  made.views[made.count++] = MakeView(0, 1.0f);
  made.views[made.count++] = MakeView(2, 2.0f);
  m.SetShadows(&made, atlas);
  const hr::HeadlightConstants good = RigBlock(set, &made, atlas.slot, atlas.sampler);
  Check(BlockPasses(m, good, lm::Check::kBlockAfter), "block mutations: the unmutated block passes");
  const auto mutate = [&](const char* what, auto&& edit) {
    hr::HeadlightConstants b = good;
    edit(b);
    Check(!BlockPasses(m, b, lm::Check::kBlockAfter), std::string("block mutations: a difference in ") + what + " is found");
  };
  mutate("the light count", [](hr::HeadlightConstants& b) { b.header[0] = 2.0f; });
  mutate("a position", [](hr::HeadlightConstants& b) { b.lights[3][1] = std::nextafter(b.lights[3][1], 1000.0f); });
  mutate("a reach", [](hr::HeadlightConstants& b) { b.lights[0][3] += 1.0f; });
  mutate("an axis", [](hr::HeadlightConstants& b) { b.lights[4][2] = -b.lights[4][2]; });
  mutate("a cos outer", [](hr::HeadlightConstants& b) { b.lights[7][3] *= 0.99f; });
  mutate("a colour", [](hr::HeadlightConstants& b) { b.lights[8][0] = std::nextafter(b.lights[8][0], 0.0f); });
  mutate("a cos inner", [](hr::HeadlightConstants& b) { b.lights[2][3] += 0.01f; });
  mutate("the reach box", [](hr::HeadlightConstants& b) { b.bounds_hi[1] += 1.0f; });
  mutate("a shadow view's matrix", [](hr::HeadlightConstants& b) { b.shadow[0].world_to_shadow[5] += 0.5f; });
  mutate("a shadow view's bias", [](hr::HeadlightConstants& b) { b.shadow[0].depth_bias *= 2.0f; });
  mutate("which view a light reads", [](hr::HeadlightConstants& b) { b.shadow_of[0] = hr::kNoView; });
  mutate("the atlas's slot", [](hr::HeadlightConstants& b) { b.atlas[0] += 1; });
  mutate("the atlas's sampler", [](hr::HeadlightConstants& b) { b.atlas[1] += 1; });
  mutate("a view the frame has not", [](hr::HeadlightConstants& b) { b.shadow_of[1] = 0; });
  (void)keys;
}

// ---------------------------------------------------------------------------
// the volume's lamps, with the real picker
// ---------------------------------------------------------------------------

std::vector<vl::GameLamp> MakeStreet(uint32_t n, float seed) {
  std::vector<vl::GameLamp> list;
  for (uint32_t i = 0; i < n; ++i) {
    vl::GameLamp g;
    g.pos[0] = 12.0f + 0.37f * seed + 23.5f * float(i);
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
    g.scale = i % 5 == 0 ? 0.8f : 1.0f;
    g.type = 1;
    g.on = 1;
    list.push_back(g);
  }
  // an omni lamp (a window) and a lamp that is off: both are records of the list too
  vl::GameLamp omni = list[3];
  omni.pos[0] += 1.1f;
  omni.type = 0;
  omni.cone_deg = 360.0f;
  list.push_back(omni);
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

bool UsableView(const vol::VolShadowView& v) { return vol::UsableView(v); }

void TestVolumeLamps() {
  const std::vector<vl::GameLamp> list = MakeStreet(70, 0.0f);
  vol::LampPicker picker;
  lm::Collector c;
  c.Reset();
  lm::SelfCheck sc;
  uint32_t fading_seen = 0, frames_with_lamps = 0;
  std::array<int32_t, 64> slot_of_key{};
  slot_of_key.fill(-1);
  std::vector<std::pair<uint64_t, uint32_t>> slots_seen;  // id -> slot, to see that one id never changes slot while it lives
  uint32_t slot_changes = 0;
  double clock = 100.0;
  for (uint32_t frame = 0; frame < 240; ++frame) {
    clock += 1.0 / 60.0;
    c.BeginFrame(frame, clock, MakeSet({}));
    const vol::VolCamera cam = CameraAt(20.0f + 0.35f * float(frame), 0.3f * std::sin(0.02f * float(frame)));
    vol::PickParams params;
    params.budget = frame < 120 ? 12u : 32u;  // a level change: more lamps come in with fades
    params.dt = 1.0f / 60.0f;
    params.near_m = 0.5f;
    params.far_m = 160.0f;
    params.reset = frame == 0;
    vol::PickResult picked;
    picker.Update(list, cam, params, &picked);
    for (uint32_t k = 0; k < picked.count; ++k) {
      fading_seen += picked.lamps[k].colour[1] > 0.0f && picked.lamps[k].colour[1] < 30.0f ? 0u : 0u;
    }
    // the volume's inputs of the frame: the core's test lamp first (not in the list), then the picker's lamps
    vol::VolInputs in;
    const uint32_t test_lamps = frame % 3 == 0 ? 1u : 0u;
    if (test_lamps != 0) {
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
      t.pad[0] = 30.0f;
      in.lamps[in.lamp_count++] = t;
    }
    for (uint32_t k = 0; k < picked.count && in.lamp_count < vol::kMaxLamps; ++k) {
      in.lamps[in.lamp_count++] = picked.lamps[k];
    }
    c.SetLamps(list.data(), uint32_t(list.size()), frame == 0, in.lamps.data() + test_lamps, in.lamp_count - test_lamps);
    lm::VolumeCheckSpec spec;
    spec.heads_on = false;
    spec.first_lamp = test_lamps;
    lm::CheckVolumeInputs(c.frame(), c, in, spec, UsableView, &sc);
    lm::CheckFrameInvariants(c.frame(), c.has_previous() ? &c.previous() : nullptr, &sc);
    frames_with_lamps += picked.count != 0 ? 1u : 0u;
    for (uint32_t k = 0; k < c.lamp_count(); ++k) {
      const lg::Light& l = c.frame().lights[c.lamp_slot(k)];
      fading_seen += l.vol_weight < 0.999f ? 1u : 0u;
      Check(c.lamp_source(k) == lm::LampSource::kList, "lamps: every picked lamp is found in the list with its record's shape");
      Check(l.flags & lg::kLightFlagGameLit, "lamps: a lamp is lit by the game's own shaders");
      Check((l.roles & lg::kRoleMaskSurface) == 0 && (l.roles & lg::kRoleMaskVolume) != 0, "lamps: no surface role, the volume's");
      bool seen = false;
      for (auto& p : slots_seen) {
        if (p.first == l.id) {
          seen = true;
          slot_changes += p.second != c.lamp_slot(k) && c.previous().Find(l.id) >= 0 ? 1u : 0u;
          p.second = c.lamp_slot(k);
        }
      }
      if (!seen) {
        slots_seen.push_back({l.id, c.lamp_slot(k)});
      }
    }
    if (frame == 100) {
      Check(c.stats().lamps == picked.count && c.stats().from_list == picked.count, "lamps: the census counts the picked lamps, all from the list");
      Check(c.stats().listed == list.size(), "lamps: and the list's records");
      Check(c.frame().role_count[uint32_t(lg::LightRole::kVolume)] == picked.count, "lamps: the volume's ranking holds the lamps");
    }
    if (test_lamps != 0 && frame == 3) {
      Check(c.lamp_count() == picked.count, "lamps: the test lamp is not in the frame's lamps (it is the core's)");
    }
    c.EndFrame();
  }
  Check(sc.total_bad() == 0, "lamps: over 240 frames of a moving camera the frame equals the volume's lamps in every check (" + std::to_string(sc.total_checks()) + " checks, " +
                                 std::to_string(sc.total_bad()) + " differed: " + sc.TakeWindow().first + ")");
  Check(frames_with_lamps > 200, "lamps: the picker had lamps in most frames");
  Check(fading_seen > 20, "lamps: lamps with a weight under 1 (fading in or out) were seen and matched (" + std::to_string(fading_seen) + ")");
  Check(slot_changes == 0, "lamps: a lamp never changed slot while it lived");

  // The ids: the same place gives the same id in a fresh collector and in a frame a minute later.
  lm::Collector a, b;
  a.Reset();
  b.Reset();
  vol::VolInputs in;
  vl::GameLamp g = list[10];
  in.lamps[0] = vol::MakeVolLamp(g);
  in.lamp_count = 1;
  a.BeginFrame(1, 0.0, MakeSet({}));
  a.SetLamps(list.data(), uint32_t(list.size()), true, in.lamps.data(), 1);
  b.BeginFrame(90000, 1500.0, MakeSet({}));
  b.SetLamps(list.data(), uint32_t(list.size()), true, in.lamps.data(), 1);
  Check(a.frame().lights[a.lamp_slot(0)].id == b.frame().lights[b.lamp_slot(0)].id && a.frame().lights[a.lamp_slot(0)].id == lg::light_id::LampId(g.pos),
        "ids: a lamp's id is its place's, the same in every frame and every run");

  // A lamp the list has no record of (a lamp fading out whose record is gone, the core's test lamp): read back from the volume's lamp, counted, not compared.
  lm::Collector u;
  u.Reset();
  u.BeginFrame(1, 0.0, MakeSet({}));
  vol::VolInputs in2;
  vl::GameLamp ghost = list[5];
  ghost.pos[0] += 0.5f;
  in2.lamps[0] = vol::MakeVolLamp(ghost);
  for (int i = 0; i < 3; ++i) {
    in2.lamps[0].colour[i] *= 0.4f;  // 40 per cent: fading
  }
  in2.lamp_count = 1;
  u.SetLamps(list.data(), uint32_t(list.size()), true, in2.lamps.data(), 1);
  Check(u.stats().unlisted == 1 && u.lamp_source(0) == lm::LampSource::kUnlisted, "lamps: a lamp with no record is unlisted");
  lm::SelfCheck su;
  lm::VolumeCheckSpec spec2;
  spec2.heads_on = false;
  lm::CheckVolumeInputs(u.frame(), u, in2, spec2, UsableView, &su);
  Check(su.total_bad() == 0, "lamps: an unlisted lamp is counted, not compared");
  const lg::Light& ul = u.frame().lights[u.lamp_slot(0)];
  const vol::VolLamp back = lg::ToVolLamp<vol::VolLamp>(ul);
  Check(std::memcmp(back.colour, in2.lamps[0].colour, sizeof(back.colour)) == 0 && ul.vol_weight == 1.0f, "lamps: and its Light projects back to the volume's own colour");

  // Mutations: the volume's lamp, one field at a time, is found; so is a record that is not the picker's and a lamp twice.
  lm::Collector m;
  m.Reset();
  m.BeginFrame(1, 0.0, MakeSet({}));
  vol::VolInputs in3;
  in3.lamps[0] = vol::MakeVolLamp(list[10]);
  in3.lamps[1] = vol::MakeVolLamp(list[11]);
  in3.lamp_count = 2;
  for (int i = 0; i < 3; ++i) {
    in3.lamps[1].colour[i] *= 0.6f;
  }
  m.SetLamps(list.data(), uint32_t(list.size()), true, in3.lamps.data(), 2);
  const auto run = [&](const vol::VolInputs& v) {
    lm::SelfCheck s;
    lm::VolumeCheckSpec sp;
    sp.heads_on = false;
    lm::CheckVolumeInputs(m.frame(), m, v, sp, UsableView, &s);
    return s.total_bad();
  };
  Check(run(in3) == 0, "lamp mutations: the unmutated inputs pass (weight 0.6 on the second)");
  const auto mutate = [&](const char* what, auto&& edit) {
    vol::VolInputs v = in3;
    edit(v);
    Check(run(v) != 0, std::string("lamp mutations: a difference in ") + what + " is found");
  };
  mutate("a position", [](vol::VolInputs& v) { v.lamps[0].pos[0] += 0.001f; });
  mutate("a range", [](vol::VolInputs& v) { v.lamps[1].range *= 1.01f; });
  mutate("a cos outer", [](vol::VolInputs& v) { v.lamps[0].cos_outer += 0.001f; });
  mutate("a cos inner", [](vol::VolInputs& v) { v.lamps[0].cos_inner += 0.001f; });
  mutate("a direction", [](vol::VolInputs& v) { v.lamps[1].dir[0] = 0.01f; });
  mutate("the near radius", [](vol::VolInputs& v) { v.lamps[0].pad[0] *= 1.01f; });
  mutate("one channel of the colour", [](vol::VolInputs& v) { v.lamps[1].colour[2] *= 1.001f; });
  mutate("the weight (colour of one channel only)", [](vol::VolInputs& v) { v.lamps[1].colour[0] *= 0.5f; });
  mutate("a lamp of another place", [](vol::VolInputs& v) { v.lamps[1].pos[2] += 1.0f; });
  // a list whose record at the place is not the picker's: the volume made its lamp from a record that has since changed
  std::vector<vl::GameLamp> other = list;
  other[10].intensity += 10.0f;
  lm::Collector d;
  d.Reset();
  d.BeginFrame(1, 0.0, MakeSet({}));
  d.SetLamps(other.data(), uint32_t(other.size()), true, in3.lamps.data(), 2);
  lm::SelfCheck sd;
  lm::VolumeCheckSpec spd;
  spd.heads_on = false;
  lm::CheckVolumeInputs(d.frame(), d, in3, spd, UsableView, &sd);
  Check(d.lamp_source(0) == lm::LampSource::kListDiffers && sd.total_bad() == 1, "lamp mutations: a list record that is not the picker's lamp is found");
  // a duplicate record at one place: the picker's is found among them
  std::vector<vl::GameLamp> dup = list;
  vl::GameLamp second = list[10];
  second.intensity += 20.0f;
  dup.insert(dup.begin() + 2, second);  // before the picker's record in the list's order
  lm::Collector e;
  e.Reset();
  e.BeginFrame(1, 0.0, MakeSet({}));
  e.SetLamps(dup.data(), uint32_t(dup.size()), true, in3.lamps.data(), 2);
  Check(e.lamp_source(0) == lm::LampSource::kList && e.stats().differs == 0, "lamps: with two records at one place the one the picker used is found");
}

// ---------------------------------------------------------------------------
// the volume's headlights and their shadow views
// ---------------------------------------------------------------------------

void TestVolumeHeadlights() {
  lm::Collector c;
  lm::ShadowCarry<lg::LocalShadowFrame> carry;
  c.Reset();
  carry.Clear();
  const lg::ShadowAtlasInfo atlas = MakeAtlas(91);
  const mcla::gpu::HeadlightSet set = MakeSet({{7, {10, 1, 5}, true}, {9, {20, 1, 8}}, {3, {-4, 1, 30}}});
  c.BeginFrame(1, 1.0, set);
  lg::LocalShadowFrame made;
  made.views[made.count++] = MakeView(0, 1.0f);
  made.views[made.count++] = MakeView(1, 2.0f);
  c.SetShadows(&made, atlas);
  // what the volume's producer does with the same inputs (VolumetricsInputs::FillHeadlightInputs)
  const auto fill = [&](const lg::LocalShadowFrame* local) {
    vol::VolInputs in;
    vol::FillHeadlights(set, &in);
    for (uint32_t i = 0; i < in.headlight_count; ++i) {
      vol::ApplyHeadlightUnits(&in.headlights[i]);
    }
    const vol::ShadowAtlasState state{atlas.valid, atlas.slot, atlas.width, atlas.height};
    vol::FillShadows(local, state, &in);
    return in;
  };
  vol::VolInputs in = fill(&made);
  lm::VolumeCheckSpec spec;
  spec.lamps_on = false;
  const auto run = [&](const vol::VolInputs& v, const lm::Collector& col) {
    lm::SelfCheck s;
    lm::CheckVolumeInputs(col.frame(), col, v, spec, UsableView, &s);
    return s.total_bad();
  };
  Check(in.headlight_count == 3 && in.headlights[0].shadow_view >= 0 && in.headlights[2].shadow_view < 0, "volume headlights: the producer's inputs read two views");
  Check(run(in, c) == 0, "volume headlights: the frame equals the volume's headlights and views bit for bit");
  // a view the volume finds unusable (a bad tile) is dropped by it and not expected by the check
  lg::LocalShadowFrame badtile = made;
  badtile.views[1].atlas_rect[2] = 2.0f;  // outside 0..1
  c.SetShadows(&badtile, atlas);
  Check(run(fill(&badtile), c) == 0, "volume headlights: a view the volume cannot use is dropped by both");
  // an atlas that is not valid: no shadows at all
  c.SetShadows(&made, atlas);
  const auto mutate = [&](const char* what, auto&& edit) {
    vol::VolInputs v = in;
    edit(v);
    Check(run(v, c) != 0, std::string("volume headlight mutations: a difference in ") + what + " is found");
  };
  mutate("the light count", [](vol::VolInputs& v) { v.headlight_count = 2; });
  mutate("a position", [](vol::VolInputs& v) { v.headlights[1].pos[1] += 0.001f; });
  mutate("a colour by one unit in the last place", [](vol::VolInputs& v) { v.headlights[0].colour[0] = std::nextafter(v.headlights[0].colour[0], 1e9f); });
  mutate("the units (near radius)", [](vol::VolInputs& v) { v.headlights[2].pad[0] = 9.0f; });
  mutate("the cone", [](vol::VolInputs& v) { v.headlights[0].cos_inner -= 0.01f; });
  mutate("which view a light reads", [](vol::VolInputs& v) { v.headlights[0].shadow_view = 1; });
  mutate("a light that lost its view", [](vol::VolInputs& v) { v.headlights[1].shadow_view = -1; });
  mutate("a light that gained a view", [](vol::VolInputs& v) { v.headlights[2].shadow_view = 0; });
  mutate("a view's matrix", [](vol::VolInputs& v) { v.shadow_views[0].world_to_shadow[3] += 0.25f; });
  mutate("a view's tile", [](vol::VolInputs& v) { v.shadow_views[1].atlas_rect[3] = 0.75f; });
  mutate("the atlas's slot", [](vol::VolInputs& v) { v.shadow_atlas_slot = 5; });
  mutate("the atlas's size", [](vol::VolInputs& v) { v.shadow_atlas_size[0] = 2048.0f; });

  // no views at all: the volume reads none and has no atlas slot
  lm::Collector n;
  n.Reset();
  n.BeginFrame(1, 1.0, set);
  n.SetShadows<lg::LocalShadowFrame>(nullptr, lg::ShadowAtlasInfo{});
  vol::VolInputs none = fill(nullptr);
  Check(none.shadow_atlas_slot == vol::kNoSlot && run(none, n) == 0, "volume headlights: no views, no atlas slot, and the frame says so too");
  // the rig has headlights and the volume has none because its setting says so: not asked
  lm::VolumeCheckSpec off;
  off.lamps_on = false;
  off.heads_on = false;
  lm::SelfCheck so;
  lm::CheckVolumeInputs(n.frame(), n, vol::VolInputs{}, off, UsableView, &so);
  Check(so.total_checks() == 0, "volume headlights: a part the volume does not read is not checked");
}

// ---------------------------------------------------------------------------
// a light with two views (a car's two headlamps: feat/hl-per-lamp; the block has shadow_of_b and eight views): only where the tree has them
// ---------------------------------------------------------------------------

template <class BlockT>
void TestTwoViewsOf() {
  if constexpr (requires(BlockT b) { b.shadow_of_b[0]; }) {
    const lg::ShadowAtlasInfo atlas = MakeAtlas(12);
    const mcla::gpu::HeadlightSet set = MakeSet({{7, {10, 1, 5}, true}, {9, {20, 1, 8}}, {3, {-4, 1, 30}}});
    lm::Collector c;
    c.Reset();
    c.BeginFrame(1, 1.0, set);
    // the replay's frame in the order it makes them: the player's two lamps, the second car's one, the first car's lamp again is not valid, a third car's one
    lg::LocalShadowFrame made;
    made.views[made.count++] = MakeView(0, 1.0f);
    made.views[made.count++] = MakeView(1, 2.0f);
    made.views[made.count++] = MakeView(0, 3.0f);
    made.views[made.count++] = MakeView(2, 4.0f);
    c.SetShadows(&made, atlas);
    const lg::Light& l0 = c.frame().lights[c.rig().slot[0]];
    Check(l0.shadow.kind == lg::ShadowKind::kAtlas && l0.shadow.view_count == 2 && c.frame().role_count[uint32_t(lg::LightRole::kShadow)] == 3, "two views: the first light has two, three lights read views");
    const BlockT block = RigBlock(set, &made, atlas.slot, atlas.sampler);
    Check(BlockPasses(c, block, lm::Check::kBlockAfter), "two views: the frame equals the rig's block with a second view for a light (shadow_of_b)");
    vol::VolInputs in;
    vol::FillHeadlights(set, &in);
    for (uint32_t i = 0; i < in.headlight_count; ++i) {
      vol::ApplyHeadlightUnits(&in.headlights[i]);
    }
    vol::FillShadows(&made, vol::ShadowAtlasState{atlas.valid, atlas.slot, atlas.width, atlas.height}, &in);
    lm::VolumeCheckSpec spec;
    spec.lamps_on = false;
    lm::SelfCheck sc;
    lm::CheckVolumeInputs(c.frame(), c, in, spec, UsableView, &sc);
    Check(sc.total_bad() == 0 && sc.total_checks() == 2, "two views: and the volume's two views of a headlight (shadow_view_b) (" + sc.TakeWindow().first + ")");
    BlockT bad = block;
    bad.shadow_of_b[0] = hr::kNoView;
    Check(!BlockPasses(c, bad, lm::Check::kBlockAfter), "two views: a second view the block lost is found");
    BlockT swapped = block;
    std::swap(swapped.shadow_of[0], swapped.shadow_of_b[0]);
    Check(!BlockPasses(c, swapped, lm::Check::kBlockAfter), "two views: the two views in the other order are found");
    lg::LightFrame f = c.frame();
    lm::SelfCheck si;
    lm::CheckFrameInvariants(f, nullptr, &si);
    Check(si.total_bad() == 0, "two views: the frame's invariants hold with two views for a light");
  } else {
    std::printf("INFO: this tree's block has one view a light: the two-view cases are not compiled\n");
  }
}

// ---------------------------------------------------------------------------
// the frame's invariants, the GPU buffer, the self-check's window
// ---------------------------------------------------------------------------

void TestFrameAndBuffer() {
  lm::Collector c;
  c.Reset();
  const mcla::gpu::HeadlightSet set = MakeSet({{7, {10, 1, 5}, true}, {9, {20, 1, 8}}});
  c.BeginFrame(77, 5.0, set);
  const std::vector<vl::GameLamp> list = MakeStreet(10, 1.0f);
  vol::VolInputs in;
  for (uint32_t i = 0; i < 6; ++i) {
    in.lamps[in.lamp_count++] = vol::MakeVolLamp(list[i]);
  }
  c.SetLamps(list.data(), uint32_t(list.size()), true, in.lamps.data(), in.lamp_count);
  lg::GpuLightBuffer buf;
  lm::BuildGpuLightBuffer(c.frame(), &buf);
  Check(buf.header.count == c.frame().count && buf.header.frame == 77 && buf.header.generation == c.frame().generation, "buffer: the header says the frame");
  bool rows = true;
  for (uint32_t i = 0; i < lg::kMaxLights; ++i) {
    const lg::Light& l = c.frame().lights[i];
    const lg::GpuLight want = l.id != lg::kNoLightId ? lg::PackGpuLight(l) : lg::GpuLight{};
    rows = rows && std::memcmp(&buf.lights[i], &want, sizeof(want)) == 0;
  }
  Check(rows, "buffer: every row is the light's pack, the free slots zero");
  Check(c.frame().count == 8 && c.frame().lights[7].id == lg::kNoLightId ? false : true, "buffer: the frame counts the highest slot used");
  Check(c.frame().count == 8, "buffer: two headlights and six lamps are eight slots");

  lm::SelfCheck sc;
  lm::CheckFrameInvariants(c.frame(), nullptr, &sc);
  Check(sc.total_bad() == 0, "frame: the invariants hold");
  const lg::LightFrame good = c.frame();
  const auto bad = [&](const char* what, auto&& edit, const lg::LightFrame* prev = nullptr) {
    lg::LightFrame f = good;
    edit(f);
    lm::SelfCheck s;
    lm::CheckFrameInvariants(f, prev, &s);
    Check(s.total_bad() == 1, std::string("frame mutations: ") + what + " is found");
  };
  bad("a repeated id", [](lg::LightFrame& f) { f.lights[1].id = f.lights[0].id; });
  bad("a lamp whose id is not its place's", [](lg::LightFrame& f) { f.lights[4].id ^= 1; });
  bad("a headlight whose id is not its car's", [](lg::LightFrame& f) { f.lights[0].id ^= 1; });
  bad("a light that moved slot", [&](lg::LightFrame& f) { std::swap(f.lights[2], f.lights[3]); }, &good);
  bad("a number that is not a number", [](lg::LightFrame& f) { f.lights[2].range = std::nanf(""); });
  bad("a weight over 1", [](lg::LightFrame& f) { f.lights[3].vol_weight = 1.5f; });
  bad("a ranking that names a free slot", [](lg::LightFrame& f) { f.order[uint32_t(lg::LightRole::kVolume)][0] = 40; });
  bad("a ranking of lights without the role", [](lg::LightFrame& f) { f.order[uint32_t(lg::LightRole::kSurface)][0] = 5; f.role_count[uint32_t(lg::LightRole::kSurface)] = 1; });
  bad("a view that is not the light's", [](lg::LightFrame& f) { f.lights[0].shadow.kind = lg::ShadowKind::kAtlas; f.lights[0].shadow.view_count = 1; f.lights[0].shadow.first_view = 0; });
  bad("a count over the capacity", [](lg::LightFrame& f) { f.count = 70; });

  // the window
  lm::SelfCheck w;
  w.Note(lm::Check::kFrame, "");
  w.Note(lm::Check::kFrame, "a thing");
  w.Note(lm::Check::kBlockBefore, "");
  const lm::SelfCheck::Window win = w.TakeWindow();
  Check(win.Checks() == 3 && win.Bad() == 1 && win.first == "frame: a thing", "self-check: the window counts three, one differed, and says which");
  Check(w.TakeWindow().Checks() == 0 && w.total_checks() == 3 && w.total_bad() == 1, "self-check: the window is taken, the totals stay");
}

// ---------------------------------------------------------------------------
// passivity: the producers' own data is not changed by collecting and checking
// ---------------------------------------------------------------------------

void TestPassive() {
  const std::vector<Car> cars = {{7, {10, 1, 5}, true}, {9, {20, 1, 8}}};
  const mcla::gpu::HeadlightSet set = MakeSet(cars);
  const std::vector<vl::GameLamp> list = MakeStreet(30, 2.0f);
  lg::LocalShadowFrame made;
  made.views[made.count++] = MakeView(0, 1.0f);
  made.views[made.count++] = MakeView(1, 2.0f);
  const lg::ShadowAtlasInfo atlas = MakeAtlas();
  const hr::HeadlightConstants block = RigBlock(set, &made, atlas.slot, atlas.sampler);
  vol::LampPicker picker;
  vol::PickParams params;
  params.budget = 12;
  params.reset = true;
  vol::PickResult picked;
  picker.Update(list, CameraAt(30.0f, 0.0f), params, &picked);
  auto in = std::make_unique<vol::VolInputs>();
  for (uint32_t k = 0; k < picked.count; ++k) {
    in->lamps[in->lamp_count++] = picked.lamps[k];
  }
  vol::FillHeadlights(set, in.get());
  for (uint32_t i = 0; i < in->headlight_count; ++i) {
    vol::ApplyHeadlightUnits(&in->headlights[i]);
  }
  vol::FillShadows(&made, vol::ShadowAtlasState{true, atlas.slot, atlas.width, atlas.height}, in.get());

  const mcla::gpu::HeadlightSet set0 = set;
  const hr::HeadlightConstants block0 = block;
  const vol::VolInputs in0 = *in;
  const std::vector<vl::GameLamp> list0 = list;
  const lg::LocalShadowFrame made0 = made;

  lm::Collector c;
  lm::SelfCheck sc;
  c.Reset();
  c.BeginFrame(1, 1.0, set);
  c.SetShadows(&made, atlas);
  c.SetLamps(list.data(), uint32_t(list.size()), true, in->lamps.data(), picked.count);
  lm::CheckHeadlightBlock(c.frame(), c.rig(), block, lm::Check::kBlockAfter, &sc);
  lm::VolumeCheckSpec spec;
  lm::CheckVolumeInputs(c.frame(), c, *in, spec, UsableView, &sc);
  lm::CheckFrameInvariants(c.frame(), nullptr, &sc);
  c.EndFrame();
  Check(sc.total_bad() == 0 && sc.total_checks() == 5, "passive: block, volume lamps, headlights, views and the frame all agree (" + sc.TakeWindow().first + ")");
  Check(std::memcmp(&set0, &set, sizeof(set)) == 0 && std::memcmp(&block0, &block, sizeof(block)) == 0 && std::memcmp(&in0, in.get(), sizeof(in0)) == 0 &&
            std::memcmp(list0.data(), list.data(), list.size() * sizeof(vl::GameLamp)) == 0 && std::memcmp(&made0, &made, sizeof(made)) == 0,
        "passive: the rig's set, its block, the volume's inputs, the lamp list and the replay's maps are byte for byte what they were");
}

// ---------------------------------------------------------------------------
// the shadow bookkeeping
// ---------------------------------------------------------------------------

void TestShadowCarry() {
  lm::ShadowCarry<lg::LocalShadowFrame> carry;
  carry.Clear();
  Check(!carry.has() && carry.ForSet(nullptr, 0, kRemap).count == 0, "shadow carry: nothing carried, nothing for any set");
  lg::LocalShadowFrame f;
  f.views[f.count++] = MakeView(0, 1.0f);
  f.views[f.count++] = MakeView(1, 2.0f);
  const std::array<uint32_t, 4> old_keys = {7, 9, 0, 0};
  carry.Set(&f, MakeAtlas(), old_keys.data(), 2);
  Check(carry.has() && carry.atlas().slot == 37, "shadow carry: the maps and the atlas are kept");
  const std::array<uint32_t, 4> new_keys = {9, 3, 7, 0};
  const lg::LocalShadowFrame moved = carry.ForSet(new_keys.data(), 3, kRemap);
  const lg::LocalShadowFrame want = hr::RemapShadowViews(f, old_keys.data(), 2, new_keys.data(), 3);
  Check(moved.count == want.count && std::memcmp(&moved, &want, sizeof(moved)) == 0, "shadow carry: the views go to the new set as the rig's own remap says");
  Check(moved.count == 2 && moved.views[0].light_index == 2 && moved.views[1].light_index == 0, "shadow carry: the car 7's view is now light 2, the car 9's light 0");
  carry.Set(&f, lg::ShadowAtlasInfo{}, old_keys.data(), 2);  // an atlas without a slot: the rig keeps nothing
  Check(!carry.has(), "shadow carry: an atlas with no slot carries nothing (SetLocalShadows' rule)");
  carry.Set(nullptr, MakeAtlas(), old_keys.data(), 2);
  Check(!carry.has(), "shadow carry: no frame, nothing");
  lg::LocalShadowFrame empty;
  carry.Set(&empty, MakeAtlas(), old_keys.data(), 2);
  Check(!carry.has(), "shadow carry: a frame with no view, nothing");

  // What the replay publishes against what is carried: the trigger's detection.
  using Carry = lm::ShadowCarry<lg::LocalShadowFrame>;
  const lg::ShadowAtlasInfo atlas = MakeAtlas();
  lg::ShadowAtlasInfo invalid = atlas;
  invalid.valid = false;
  Check(Carry::Published(f, atlas) && !Carry::Published(f, invalid) && !Carry::Published(empty, atlas), "shadow carry: maps are published with a valid atlas and a view, else not");
  carry.Clear();
  Check(!carry.Differs(empty, invalid) && !carry.Differs(empty, atlas), "shadow carry: nothing carried and nothing published: no difference (a day)");
  Check(carry.Differs(f, atlas), "shadow carry: maps made where there were none: a difference (the trigger)");
  carry.Set(&f, atlas, old_keys.data(), 2);
  Check(!carry.Differs(f, atlas), "shadow carry: the same maps and atlas: no difference (a resolve that is not the trigger)");
  lg::LocalShadowFrame other = f;
  other.views[1].world_to_shadow[0] += 1.0f;
  Check(carry.Differs(other, atlas), "shadow carry: another matrix: a difference");
  other = f;
  other.count = 1;
  Check(carry.Differs(other, atlas), "shadow carry: one view fewer: a difference");
  lg::ShadowAtlasInfo moved_atlas = atlas;
  moved_atlas.slot += 1;
  Check(carry.Differs(f, moved_atlas), "shadow carry: another atlas slot: a difference");
  Check(carry.Differs(empty, atlas) && carry.Differs(f, invalid), "shadow carry: maps given up: a difference");
}

void TestRefill() {
  // The volume fills twice in one frame (a second list): the second fill replaces the first's lamps, none is dropped as a repeat, the slots are the same.
  const std::vector<vl::GameLamp> list = MakeStreet(20, 3.0f);
  lm::Collector c;
  c.Reset();
  c.BeginFrame(1, 0.0, MakeSet({{7, {10, 1, 5}, true}}));
  vol::VolInputs in;
  for (uint32_t i = 0; i < 5; ++i) {
    in.lamps[in.lamp_count++] = vol::MakeVolLamp(list[i]);
  }
  c.SetLamps(list.data(), uint32_t(list.size()), true, in.lamps.data(), in.lamp_count);
  const uint32_t slot0 = c.lamp_slot(0);
  const uint32_t count = c.frame().count;
  c.SetLamps(list.data(), uint32_t(list.size()), false, in.lamps.data(), in.lamp_count);
  Check(c.lamp_count() == 5 && c.stats().lamps_dropped == 0 && c.lamp_slot(0) == slot0 && c.frame().count == count, "refill: a second fill in a frame replaces the first's lamps in the same slots");
  // and with fewer lamps the slots of the others are free again
  c.SetLamps(list.data(), uint32_t(list.size()), false, in.lamps.data(), 2);
  Check(c.lamp_count() == 2 && c.frame().role_count[uint32_t(lg::LightRole::kVolume)] == 3 && c.frame().count == 3, "refill: fewer lamps in the second fill leave the frame with fewer (a headlight and two lamps)");
  lm::SelfCheck sc;
  lm::CheckFrameInvariants(c.frame(), nullptr, &sc);
  Check(sc.total_bad() == 0, "refill: the frame is sound");
}

void TestBudgets() {
  lm::Collector c;
  c.Reset();
  c.BeginFrame(1, 0.0, MakeSet({}));
  c.NoteBudgets(5, 1024, 512, 32, 4);
  const uint32_t g0 = c.frame().generation;
  c.NoteBudgets(5, 1024, 512, 32, 4);
  Check(c.frame().generation == g0, "generation: the same atlas and budgets, the same generation");
  c.NoteBudgets(5, 2048, 512, 32, 4);
  Check(c.frame().generation == g0 + 1, "generation: a bigger atlas is another generation");
  c.NoteBudgets(5, 2048, 512, 48, 4);
  Check(c.frame().generation == g0 + 2, "generation: another lamp budget too");
  c.EndFrame();
  c.BeginFrame(2, 0.1, MakeSet({}));
  Check(c.frame().generation == g0 + 2, "generation: a new frame starts with the last");
}

}  // namespace

int main() {
  TestSlotTable();
  TestHeadlights();
  TestBlock();
  TestVolumeLamps();
  TestVolumeHeadlights();
  TestTwoViewsOf<hr::HeadlightConstants>();
  TestFrameAndBuffer();
  TestPassive();
  TestShadowCarry();
  TestRefill();
  TestBudgets();
  std::printf("INFO: the rig's block is %zu bytes, the replay's frame holds %zu views, the volume's lamp is %zu bytes\n", sizeof(hr::HeadlightConstants),
              lg::LocalShadowFrame{}.views.size(), sizeof(vol::VolLamp));
  std::printf("light_manager_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

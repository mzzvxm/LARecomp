// Check of the emitters of the ray traced bounce light (GI-BOUNCE-RT; src/gpu/rt_emitters_rules.h and hlsl/rt_emitters.hlsli). A program of its own: no renderer, no game files, no GPU.
//
//   clang++ -std=c++23 -I src -I src/gpu src/gpu/rt_emitters_rules_test.cpp -o rt_emitters_rules_test        (no citypack sources: nothing here reads the pack)
//   rt_emitters_rules_test [<src dir>]          (without the folder the shader text checks are skipped)
//
// What is checked, in order:
//   groups, tiers  which group a light is of (by its source), the budget tier a level and a rung take
//   the motion     a light seen for the first time is moving; one that stays within kStillSpeed for kStillSamples samples is static; one over kMovingSpeed is moving at once; the band between keeps the state;
//                  a gap, a clock that stands still or a light with no id is moving; a light that leaves the list is forgotten
//   the list       Build over the light pass's own sources (the rig's headlight in the frame, a cop car's lamps, a neon tube, a mod's lamps): the groups' switches (a group off is not gathered), the night factor
//                  (the game lamps' laws, not the headlight's), the gains, the lights too far taken out BEFORE the budgets, the lights with no colour or a number that is not one taken out, static first and
//                  the census, deterministic, the words (GpuLight rows) round trip
//   the shader     hlsl/rt_emitters.hlsli compiled as C++ over the world rows' emitter records: RtEmitterRadiance against light_laws.h's LightTerm and RtEmitterTo against the geometry, on random lights and
//                  surfaces under random world axes (a mirrored set among them)
//   the text       the shader file's use of the law and of the first load's range test

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "hlsl_cpp_shim.h"
#include "rt_emitters_rules.h"

// ---------------------------------------------------------------------------
// the shader file as C++
// ---------------------------------------------------------------------------
namespace hlsl_shim {
StructuredBuffer<uint4> gRtWorld;
StructuredBuffer<uint4> gRtLamps;
}  // namespace hlsl_shim

#define RT_WORLD_NO_RESOURCES
#define RT_WORLD_LOOP
#define RT_HIT_FRAME_ROW(i) hlsl_shim::gRtWorld[i]

namespace hlsl_shim {
#include "hlsl/rt_world_frame.hlsli"
#include "hlsl/light_laws.hlsli"
#include "hlsl/rt_emitters.hlsli"
}  // namespace hlsl_shim

namespace re = mcla::gpu::rt_emitters;
namespace lg = larecomp::gpu;
namespace ll = larecomp::gpu::local_lights;
namespace pts = larecomp::gpu::light_points;
namespace cl = larecomp::gpu::car_lights;
namespace nl = larecomp::gpu::neon_light;
namespace mls = larecomp::gpu::mod_light_source;
namespace lw = larecomp::gpu::light_laws;

namespace {

int g_checks = 0;
int g_failed = 0;
void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    if (g_failed <= 60) std::printf("FAILED: %s\n", what.c_str());
  }
}
#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #cond)
bool Near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }
std::string Num(const char* format, double a) {
  char text[256];
  std::snprintf(text, sizeof(text), format, a);
  return text;
}
std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  std::string text = s.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

struct Lcg {
  uint64_t state = 0x9E3779B97F4A7C15ull;
  uint32_t U32() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(state >> 33);
  }
  float Next() { return float(U32() & 0xFFFFFF) / float(0x1000000); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return U32() % n; }
};

// ---------------------------------------------------------------------------
// the sources, made as the light pass's own tests make them
// ---------------------------------------------------------------------------

lg::Light RigHeadlight(const float pos[3], uint64_t id) {
  lg::Light l;
  for (int i = 0; i < 3; ++i) l.pos[i] = pos[i];
  l.dir[1] = 0.0f;
  l.dir[2] = 1.0f;
  l.cos_outer = 0.788f;
  l.cos_inner = 0.978f;
  l.range = 70.0f;
  l.color[0] = 1.0f;
  l.color[1] = 0.93f;
  l.color[2] = 0.78f;
  l.law_a = 26.0f;
  l.kind = lg::LightKind::kSpot;
  l.source = lg::LightSource::kHeadlight;
  l.law = lg::LightLaw::kHeadlight;
  l.roles = lg::kRoleMaskAll;
  l.id = id;
  return l;
}
// A frame of lights, the surface ranking in their order.
lg::LightFrame FrameOf(const std::vector<lg::Light>& lights) {
  lg::LightFrame f;
  for (size_t i = 0; i < lights.size(); ++i) {
    f.lights[i] = lights[i];
    f.order[uint32_t(lg::LightRole::kSurface)][i] = uint8_t(i);
  }
  f.count = uint32_t(lights.size());
  f.role_count[uint32_t(lg::LightRole::kSurface)] = uint8_t(lights.size());
  return f;
}

pts::Point Pt(uint32_t object, uint32_t kind, uint32_t index, float intensity, float x, float y, float z, const float dir[3] = nullptr, uint32_t vehicle = 0x1000u) {
  pts::Point p;
  p.kind = kind;
  p.index = index;
  p.object = object;
  p.vehicle = vehicle;
  p.owned = true;
  p.pos[0] = x;
  p.pos[1] = y;
  p.pos[2] = z;
  if (dir != nullptr) {
    for (int i = 0; i < 3; ++i) p.dir[i] = dir[i];
    p.has_dir = true;
  }
  p.intensity = p.peak = intensity;
  return p;
}
// One car's lamps: brake lamps (rear), headlights (head lamps), a police roof bar (police), at (ox, oz).
std::vector<pts::Point> CarPoints(uint32_t object, float ox, float oz, bool police) {
  static const float back[3] = {0.0f, 0.0f, 1.0f};
  static const float forward[3] = {0.0f, 0.0f, -1.0f};
  std::vector<pts::Point> v;
  v.push_back(Pt(object, pts::kKindBrake, 0, 0.9f, ox - 0.7f, 0.8f, oz + 2.0f, forward, 0x1000u + object));
  v.push_back(Pt(object, pts::kKindBrake, 1, 0.9f, ox + 0.7f, 0.8f, oz + 2.0f, forward, 0x1000u + object));
  v.push_back(Pt(object, pts::kKindHeadlight, 0, 2.0f, ox - 0.7f, 0.7f, oz - 2.0f, back, 0x1000u + object));
  v.push_back(Pt(object, pts::kKindHeadlight, 1, 2.0f, ox + 0.7f, 0.7f, oz - 2.0f, back, 0x1000u + object));
  if (police) {
    v.push_back(Pt(object, pts::kKindPolice0, 0, 1.0f, ox - 0.5f, 1.5f, oz, forward, 0x1000u + object));
    v.push_back(Pt(object, pts::kKindPolice0, 1, 1.0f, ox - 0.2f, 1.5f, oz, forward, 0x1000u + object));
  }
  return v;
}
cl::CarLightsFrame CarsOf(const std::vector<pts::Point>& points, const double camera[3]) {
  cl::CarLightsFrame f;
  cl::Build(points, {}, camera, &f);
  return f;
}

// A neon car at (x, z) in world space, a 3.6 m tube along z under it.
nl::NeonCar NeonAt(uint32_t object, float x, float z, float level = 1.0f) {
  nl::NeonCar c;
  c.object = object;
  c.record = object + 100;
  c.on = true;
  c.colour[0] = 0.9f * level;
  c.colour[1] = 0.1f * level;
  c.colour[2] = 0.4f * level;
  c.pulse = 0.8f;
  c.twin_ok = true;
  for (int m : {0, 5, 10, 15}) {
    c.car_matrix[m] = 1.0f;
    c.matrix[m] = 1.0f;
  }
  c.car_matrix[12] = x;
  c.car_matrix[13] = 0.5f;
  c.car_matrix[14] = z;
  for (uint32_t p = 0; p < nl::kTubePoints; ++p) {
    c.points[p][0] = x;
    c.points[p][1] = 0.25f;
    c.points[p][2] = z - 1.8f + 3.6f * float(p) / 3.0f;
    c.point_w[p] = 2.5f;
  }
  return c;
}

mls::Record ModLamp(float x, float z, uint32_t id) {
  mls::Record r;
  r.pos[0] = x;
  r.pos[1] = 6.0f;
  r.pos[2] = z;
  r.colour[0] = 1.0f;
  r.colour[1] = 0.8f;
  r.colour[2] = 0.6f;
  r.dir[1] = -1.0f;
  r.intensity = 55.0f;
  r.cone_deg = 125.0f;
  r.spread_deg = -50.5f;
  r.id = id;
  r.type = 1;
  r.on = 1;
  return r;
}
mls::SetPtr ModSet(const std::string& name, std::vector<mls::Record> lamps) {
  auto set = std::make_shared<mls::Set>();
  set->mod = name;
  set->mod_key = larecomp::gpu::light_id::ModKey(name);
  set->lamps = std::move(lamps);
  return set;
}

uint32_t CountGroup(const re::BuildResult& r, uint32_t group) {
  uint32_t n = 0;
  for (const re::Emitter& e : r.list) n += e.group == group ? 1u : 0u;
  return n;
}

// ---------------------------------------------------------------------------
// groups, tiers, the motion
// ---------------------------------------------------------------------------

void GroupChecks() {
  CHECK(re::kGroupAll == 15 && re::kGroupCount == 4 && re::kMaxEmitters == 32 && re::kRowsEach == 4 && re::kWordsEach == 16 && re::kMaxEmitterRows == 128);
  CHECK(re::kMaxEmitters == ll::kMaxPassLights && re::kMaxDistance == 100.0f);
  // the motion's numbers are values (the tests below follow the constants: these pin them): walking pace, half of it to come back, six samples, half a second
  CHECK(re::kMovingSpeed == 1.5f && re::kStillSpeed == 0.75f && re::kStillSamples == 6 && re::kMaxSampleGap == 0.5 && re::kMinColour == 0.003f);
  lg::Light l;
  l.source = lg::LightSource::kHeadlight;
  CHECK(re::GroupOf(l) == re::kGroupHeadlights);
  l.source = lg::LightSource::kNeon;
  CHECK(re::GroupOf(l) == re::kGroupNeon);
  l.source = lg::LightSource::kPolice;
  CHECK(re::GroupOf(l) == re::kGroupOther);
  l.source = lg::LightSource::kMod;
  CHECK(re::GroupOf(l) == re::kGroupOther);
  l.source = lg::LightSource::kOther;
  l.car_key = 77;
  CHECK(re::GroupOf(l) == re::kGroupRear);   // a car's lamp that is no headlight, neon or police: the rear lamps (brake, tail, turn, reverse)
  l.car_key = 0;
  CHECK(re::GroupOf(l) == re::kGroupOther);  // a debug or handed over light
  re::Switches s;
  CHECK(re::MaskOf(s) == re::kGroupAll);
  s.neon = false;
  CHECK(re::MaskOf(s) == (re::kGroupHeadlights | re::kGroupRear | re::kGroupOther));
  s = {false, false, false, false};
  CHECK(re::MaskOf(s) == 0);
  CHECK(std::string(re::GroupName(re::kGroupNeon)) == "neon" && std::string(re::GroupName(re::kGroupRear)) == "rear lamps");
  // the light pass's sources the groups stand for
  CHECK(re::SourcesOf(re::kGroupHeadlights) == (ll::kSourceFrame | ll::kSourceHeadLamps) && re::SourcesOf(re::kGroupNeon) == ll::kSourceNeon && re::SourcesOf(re::kGroupRear) == ll::kSourceRearLamps &&
        re::SourcesOf(re::kGroupOther) == (ll::kSourcePolice | ll::kSourceMod) && re::SourcesOf(0) == 0);
  // the tier: ULTRA the ultra row, a strong card at HIGH the high row, a weak one the smallest
  CHECK(re::TierFor(true, false) == ll::Tier::kUltra && re::TierFor(true, true) == ll::Tier::kUltra && re::TierFor(false, false) == ll::Tier::kHigh && re::TierFor(false, true) == ll::Tier::kHigh);
}

void MotionChecks() {
  re::MotionTracker t;
  const float p0[3] = {10.0f, 0.5f, 20.0f};
  double now = 100.0;
  const double dt = 1.0 / 60.0;
  t.Begin();
  CHECK(t.Note(5, p0, now));  // first sight: moving
  t.End();
  // parked: moving until kStillSamples still samples have been seen
  for (uint32_t i = 1; i <= re::kStillSamples; ++i) {
    now += dt;
    t.Begin();
    const bool moving = t.Note(5, p0, now);
    t.End();
    CHECK(moving == (i < re::kStillSamples));
  }
  // a still light whose position jitters by a millimetre stays static
  {
    float p[3] = {p0[0] + 0.001f, p0[1], p0[2]};
    now += dt;
    t.Begin();
    CHECK(!t.Note(5, p, now));
    t.End();
  }
  // a step at 30 m/s: moving at once, and static again only after the still samples
  {
    float p[3] = {p0[0] + 0.5f, p0[1], p0[2]};
    now += dt;
    t.Begin();
    CHECK(t.Note(5, p, now));
    t.End();
    for (uint32_t i = 1; i <= re::kStillSamples; ++i) {
      now += dt;
      t.Begin();
      const bool moving = t.Note(5, p, now);
      t.End();
      CHECK(moving == (i < re::kStillSamples));
    }
  }
  // the band between the two speeds keeps the state (and restarts the count of still samples): 1.0 m/s
  {
    float p[3] = {p0[0] + 0.5f, p0[1], p0[2]};
    CHECK(re::kStillSpeed < 1.0f && 1.0f < re::kMovingSpeed);
    for (int i = 0; i < 20; ++i) {
      p[0] += float(1.0 * dt);
      now += dt;
      t.Begin();
      CHECK(!t.Note(5, p, now));  // it was static: 1 m/s does not move it
      t.End();
    }
    // ... and a light that was moving at 1 m/s stays moving
    re::MotionTracker u;
    float q[3] = {0.0f, 0.0f, 0.0f};
    double tu = 5.0;
    u.Begin();
    u.Note(9, q, tu);
    u.End();
    for (int i = 0; i < 20; ++i) {
      q[0] += float(1.0 * dt);
      tu += dt;
      u.Begin();
      CHECK(u.Note(9, q, tu));
      u.End();
    }
  }
  // a gap, a clock that did not advance and a light with no id are moving
  {
    re::MotionTracker u;
    u.Begin();
    u.Note(3, p0, 1.0);
    u.End();
    for (uint32_t i = 1; i <= re::kStillSamples + 2; ++i) {
      u.Begin();
      u.Note(3, p0, 1.0 + dt * i);
      u.End();
    }
    u.Begin();
    CHECK(!u.Note(3, p0, 1.0 + dt * (re::kStillSamples + 3)));
    u.End();
    u.Begin();
    CHECK(u.Note(3, p0, 1.0 + dt * (re::kStillSamples + 3) + re::kMaxSampleGap + 0.1));  // a gap: no speed to read
    u.End();
    u.Begin();
    CHECK(u.Note(3, p0, 1.0 + dt * (re::kStillSamples + 3) + re::kMaxSampleGap + 0.1));  // the clock stood still
    u.End();
    u.Begin();
    CHECK(u.Note(lg::kNoLightId, p0, 5.0));
    const float bad[3] = {std::nanf(""), 0.0f, 0.0f};
    CHECK(u.Note(4, bad, 5.0));
    u.End();
  }
  // a light that is not noted in a frame is forgotten (it is moving again when it comes back)
  {
    re::MotionTracker u;
    for (int i = 0; i < 20; ++i) {
      u.Begin();
      u.Note(8, p0, 10.0 + dt * i);
      u.End();
    }
    CHECK(u.size() == 1);
    u.Begin();
    u.End();
    CHECK(u.size() == 0);
    u.Begin();
    CHECK(u.Note(8, p0, 10.0 + dt * 21));
    u.End();
    u.Clear();
    CHECK(u.size() == 0);
  }
}

// ---------------------------------------------------------------------------
// the list
// ---------------------------------------------------------------------------

re::BuildInputs BaseInputs(const lg::LightFrame* frame, const cl::CarLightsFrame* cars, const std::vector<nl::NeonCar>* neon) {
  re::BuildInputs in;
  in.frame = frame;
  in.cars = cars;
  in.neon = neon;
  in.camera[0] = 0.0f;
  in.camera[1] = 1.0f;
  in.camera[2] = 10.0f;
  in.night = 1.0f;
  in.tier = ll::Tier::kHigh;
  return in;
}

void BuildChecks() {
  const double camera[3] = {0.0, 1.0, 10.0};
  const float head_pos[3] = {2.0f, 0.8f, 8.0f};
  const lg::LightFrame frame = FrameOf({RigHeadlight(head_pos, 11)});
  const cl::CarLightsFrame cars = CarsOf(CarPoints(7, -6.0f, 12.0f, true), camera);
  const std::vector<nl::NeonCar> neon = {NeonAt(21, 4.0f, 14.0f)};
  nl::Tube tube;
  CHECK(nl::ResolveTube(neon[0], &tube) && tube.space != nl::Space::kNone);   // the test's neon car is one the pass would place
  CHECK(cars.count == 1);

  // nothing in, nothing out
  {
    re::BuildResult r;
    re::Build(BaseInputs(nullptr, nullptr, nullptr), nullptr, &r);
    CHECK(r.list.empty() && r.census.selected == 0 && r.census.still == 0 && r.census.moving == 0);
  }
  // everything: the rig's headlight (the headlights group), the car's head lamps are COVERED by it (the rig's light is within 3 m of ... no: the cop car is 14 m away, so its own head lamps are another light), its
  // brake lamps (rear), its roof bar (other), the neon tube (neon)
  re::BuildResult all;
  {
    re::BuildInputs in = BaseInputs(&frame, &cars, &neon);
    re::Build(in, nullptr, &all);
    CHECK(CountGroup(all, re::kGroupHeadlights) >= 2 && CountGroup(all, re::kGroupNeon) == 3 && CountGroup(all, re::kGroupRear) >= 1 && CountGroup(all, re::kGroupOther) >= 1);
    CHECK(all.list.size() <= re::kMaxEmitters && all.census.selected == all.list.size() && all.census.dropped_far == 0 && all.census.dropped_dark == 0);
    CHECK(all.census.groups == re::kGroupAll && all.census.by_group[0] + all.census.by_group[1] + all.census.by_group[2] + all.census.by_group[3] == all.list.size());
    // no tracker: everything is moving
    CHECK(all.census.still == 0 && all.census.moving == all.list.size() && all.static_count() == 0);
    for (const re::Emitter& e : all.list) {
      CHECK(e.moving && e.group == re::GroupOf(e.light) && std::isfinite(e.light.color[0]) && e.light.range > 0.0f);
    }
  }
  // the switches: a group that is off is not in the list, the others are as they were
  for (const uint32_t off : {re::kGroupHeadlights, re::kGroupNeon, re::kGroupRear, re::kGroupOther}) {
    re::BuildInputs in = BaseInputs(&frame, &cars, &neon);
    in.groups = re::kGroupAll & ~off;
    re::BuildResult r;
    re::Build(in, nullptr, &r);
    CHECK(CountGroup(r, off) == 0);
    for (const uint32_t other : {re::kGroupHeadlights, re::kGroupNeon, re::kGroupRear, re::kGroupOther}) {
      if (other != off) CHECK(CountGroup(r, other) == CountGroup(all, other));
    }
    CHECK(r.census.groups == (re::kGroupAll & ~off));
    // a group that is off is not GATHERED (not gathered and dropped): the candidates the gathers made are fewer
    if (off != re::kGroupHeadlights) CHECK(r.census.candidates < all.census.candidates);
  }
  {
    re::BuildInputs in = BaseInputs(&frame, &cars, &neon);
    in.groups = 0;
    re::BuildResult r;
    re::Build(in, nullptr, &r);
    CHECK(r.list.empty() && r.census.candidates == 0 && r.census.selected == 0);
  }
  // the night factor: the game lamps' laws (neon, rear, police) are times it; the headlight law's colour is its own
  {
    re::BuildInputs half = BaseInputs(&frame, &cars, &neon);
    half.night = 0.5f;
    re::BuildResult r;
    re::Build(half, nullptr, &r);
    CHECK(r.list.size() == all.list.size());
    for (size_t i = 0; i < r.list.size(); ++i) {
      const float scale = r.list[i].light.law == lg::LightLaw::kHeadlight ? 1.0f : 0.5f;
      for (int k = 0; k < 3; ++k) CHECK(Near(r.list[i].light.color[k], all.list[i].light.color[k] * scale, 1e-6));
    }
    re::BuildInputs day = BaseInputs(&frame, &cars, &neon);
    day.night = 0.0f;
    re::BuildResult d;
    re::Build(day, nullptr, &d);
    CHECK(CountGroup(d, re::kGroupNeon) == 0 && CountGroup(d, re::kGroupRear) == 0 && CountGroup(d, re::kGroupOther) == 0);   // by day the game lamps give nothing: no slot
    CHECK(CountGroup(d, re::kGroupHeadlights) >= 1 && d.census.dropped_dark >= 5 && d.census.selected == d.list.size());        // the headlights' colour holds its own night factor; the others went BEFORE the budgets
    for (const re::Emitter& e : d.list) CHECK(e.light.law == lg::LightLaw::kHeadlight);
    // a night factor that is not a number counts as 0, above 1 as 1
    re::BuildInputs bad = BaseInputs(&frame, &cars, &neon);
    bad.night = std::nanf("");
    re::BuildResult b;
    re::Build(bad, nullptr, &b);
    CHECK(b.list.size() == d.list.size());
    bad.night = 7.0f;
    re::Build(bad, nullptr, &b);
    CHECK(b.list.size() == all.list.size() && Near(b.list[0].light.color[0], all.list[0].light.color[0], 1e-6));
  }
  // the gains: the pass's own (every light), the cars' look (the cars' lamps), the neon's
  {
    re::BuildInputs g = BaseInputs(&frame, &cars, &neon);
    g.gain = 0.5f;
    re::BuildResult r;
    re::Build(g, nullptr, &r);
    for (size_t i = 0; i < r.list.size(); ++i) {
      for (int k = 0; k < 3; ++k) CHECK(Near(r.list[i].light.color[k], all.list[i].light.color[k] * 0.5f, 1e-6));
    }
    g = BaseInputs(&frame, &cars, &neon);
    g.neon_gain = 2.0f;
    re::Build(g, nullptr, &r);
    uint32_t checked = 0;
    for (size_t i = 0; i < r.list.size(); ++i) {
      const float want = r.list[i].group == re::kGroupNeon ? 2.0f : 1.0f;
      for (int k = 0; k < 3; ++k) CHECK(Near(r.list[i].light.color[k], all.list[i].light.color[k] * want, 1e-6));
      checked += r.list[i].group == re::kGroupNeon ? 1u : 0u;
    }
    CHECK(checked == 3);
    g = BaseInputs(&frame, &cars, &neon);
    g.car_gain = 0.0f;
    re::Build(g, nullptr, &r);
    CHECK(CountGroup(r, re::kGroupRear) == 0 && CountGroup(r, re::kGroupOther) == 0 && CountGroup(r, re::kGroupNeon) == 3 && CountGroup(r, re::kGroupHeadlights) == 1);
    g.gain = -3.0f;   // a gain under 0 is 0
    re::Build(g, nullptr, &r);
    CHECK(CountGroup(r, re::kGroupNeon) == 0);
  }
  // too far: a rig light 150 m away is left out (counted), cars' lamps 150 m away are left out BEFORE the budgets take them (a car out of reach takes no slot)
  {
    const float far_pos[3] = {2.0f, 0.8f, 160.0f};
    const lg::LightFrame far_frame = FrameOf({RigHeadlight(far_pos, 12), RigHeadlight(head_pos, 11)});
    std::vector<pts::Point> points = CarPoints(30, 0.0f, 165.0f, true);
    for (uint32_t c = 31; c < 37; ++c) {
      const std::vector<pts::Point> more = CarPoints(c, float(c) * 3.0f, 170.0f, true);
      points.insert(points.end(), more.begin(), more.end());
    }
    const std::vector<pts::Point> near_car = CarPoints(40, -6.0f, 12.0f, false);
    points.insert(points.end(), near_car.begin(), near_car.end());
    const cl::CarLightsFrame many = CarsOf(points, camera);
    re::BuildInputs in = BaseInputs(&far_frame, &many, nullptr);
    re::BuildResult r;
    re::Build(in, nullptr, &r);
    CHECK(r.census.dropped_far >= 2 + 7 * 2);   // the far rig light, and each far car's head, rear and police lights
    for (const re::Emitter& e : r.list) CHECK(re::detail::Dist2(e.light.pos, in.camera) <= re::kMaxDistance * re::kMaxDistance);
    CHECK(CountGroup(r, re::kGroupHeadlights) >= 2 && CountGroup(r, re::kGroupRear) >= 1);   // the near rig light and the near car's lamps are there
  }
  // a light with a number that is not one, or no colour, takes no slot and does not spoil the others
  {
    lg::Light nan_dir = RigHeadlight(head_pos, 50);
    nan_dir.dir[0] = std::nanf("");
    lg::Light inf_colour = RigHeadlight(head_pos, 51);
    inf_colour.color[1] = INFINITY;
    lg::Light nan_pos = RigHeadlight(head_pos, 52);
    nan_pos.pos[2] = std::nanf("");
    lg::Light dark = RigHeadlight(head_pos, 53);
    dark.color[0] = dark.color[1] = dark.color[2] = 1.0e-6f;
    lg::Light good = RigHeadlight(head_pos, 54);
    const lg::LightFrame odd = FrameOf({nan_dir, inf_colour, nan_pos, dark, good});
    re::BuildInputs in = BaseInputs(&odd, nullptr, nullptr);
    re::BuildResult r;
    re::Build(in, nullptr, &r);
    CHECK(r.list.size() == 1 && r.list[0].light.id == 54);
    for (const re::Emitter& e : r.list) {
      for (int k = 0; k < 3; ++k) CHECK(std::isfinite(e.light.color[k]) && std::isfinite(e.light.dir[k]) && std::isfinite(e.light.pos[k]));
    }
  }
  // the budgets: at most the pass's 32 in all, each class at most its tier's cap
  {
    std::vector<pts::Point> points;
    for (uint32_t c = 0; c < 8; ++c) {
      const std::vector<pts::Point> car = CarPoints(60 + c, -8.0f + 2.0f * float(c), 14.0f, true);
      points.insert(points.end(), car.begin(), car.end());
    }
    const cl::CarLightsFrame crowd = CarsOf(points, camera);
    std::vector<nl::NeonCar> neons;
    for (uint32_t c = 0; c < 8; ++c) neons.push_back(NeonAt(70 + c, -8.0f + 3.0f * float(c), 16.0f));
    for (const ll::Tier tier : {ll::Tier::kMedium, ll::Tier::kHigh, ll::Tier::kUltra}) {
      re::BuildInputs in = BaseInputs(&frame, &crowd, &neons);
      in.tier = tier;
      re::BuildResult r;
      re::Build(in, nullptr, &r);
      const ll::ClassBudget b = ll::BudgetOf(tier);
      CHECK(r.list.size() <= re::kMaxEmitters);
      CHECK(CountGroup(r, re::kGroupNeon) <= b.neon && CountGroup(r, re::kGroupRear) <= b.rear && CountGroup(r, re::kGroupHeadlights) <= b.frame + b.head && CountGroup(r, re::kGroupOther) <= b.police + b.mod);
      CHECK(CountGroup(r, re::kGroupNeon) == b.neon_cars * b.neon_points);   // whole cars' tubes
    }
  }
  // a mod's lamps outside its grid are the `other` group; the set the camera is over is the grid's
  {
    const mls::SetPtr a = ModSet("mod_a", {ModLamp(10.0f, 6.0f, 1)});
    const mls::SetPtr b = ModSet("mod_b", {ModLamp(30.0f, 40.0f, 1), ModLamp(32.0f, 44.0f, 2)});
    re::BuildInputs in = BaseInputs(nullptr, nullptr, nullptr);
    in.built = std::make_shared<const std::vector<mls::SetPtr>>(std::vector<mls::SetPtr>{a, b});
    in.active_mod = a.get();
    re::BuildResult r;
    re::Build(in, nullptr, &r);
    CHECK(r.list.size() == 2 && CountGroup(r, re::kGroupOther) == 2);
    for (const re::Emitter& e : r.list) CHECK(e.light.source == lg::LightSource::kMod && e.light.law == lg::LightLaw::kGameLamp);
    in.groups = re::kGroupAll & ~re::kGroupOther;
    re::Build(in, nullptr, &r);
    CHECK(r.list.empty());
  }
  // static first: two neon cars, one parked and one moving 0.5 m a frame; after the still samples the parked car's tube leads the list, the census counts each, and the first frame is all moving
  {
    re::MotionTracker tracker;
    double now = 50.0;
    re::BuildResult r;
    for (int frame_no = 0; frame_no < int(re::kStillSamples) + 3; ++frame_no) {
      const std::vector<nl::NeonCar> two = {NeonAt(81, 3.0f, 14.0f), NeonAt(82, -5.0f + 0.5f * float(frame_no), 20.0f)};
      re::BuildInputs in = BaseInputs(nullptr, nullptr, &two);
      in.now_s = now;
      re::Build(in, &tracker, &r);
      now += 1.0 / 60.0;
      if (frame_no == 0) CHECK(r.census.still == 0 && r.census.moving == 6 && r.static_count() == 0);
    }
    CHECK(r.list.size() == 6 && r.census.still == 3 && r.census.moving == 3 && r.static_count() == 3);
    for (size_t i = 0; i < r.list.size(); ++i) {
      CHECK(r.list[i].moving == (i >= 3));
      CHECK(Near(r.list[i].light.car_key, r.list[0].light.car_key) == (i < 3));   // the parked car's three lights are the first three
    }
    CHECK(tracker.size() == 6);
    // the tracker follows the list: a frame with no neon forgets them
    re::BuildInputs empty = BaseInputs(nullptr, nullptr, nullptr);
    empty.now_s = now;
    re::Build(empty, &tracker, &r);
    CHECK(tracker.size() == 0 && r.list.empty());
  }
  // deterministic: the same inputs give the same words; and the words are the GpuLight rows of the list
  {
    re::BuildInputs in = BaseInputs(&frame, &cars, &neon);
    re::BuildResult a, b;
    re::Build(in, nullptr, &a);
    re::Build(in, nullptr, &b);
    std::vector<uint32_t> wa, wb;
    re::PackWords(a.list, &wa);
    re::PackWords(b.list, &wb);
    CHECK(wa == wb && wa.size() == a.list.size() * re::kWordsEach && !wa.empty());
    for (size_t i = 0; i < a.list.size(); ++i) {
      lg::GpuLight g;
      std::memcpy(&g, wa.data() + i * re::kWordsEach, sizeof(g));
      const lg::Light back = lg::UnpackGpuLight(g);
      const lg::Light& l = a.list[i].light;
      CHECK(back.range == l.range && back.cos_outer == l.cos_outer && back.cos_inner == l.cos_inner && back.law_a == l.law_a && back.law == l.law && back.source == l.source && back.kind == l.kind);
      for (int k = 0; k < 3; ++k) CHECK(back.pos[k] == l.pos[k] && back.dir[k] == l.dir[k] && back.color[k] == l.color[k]);
    }
    const std::string text = re::CensusText(a.census);
    CHECK(Has(text, "emitters " + std::to_string(a.list.size())) && Has(text, "headlights") && Has(text, "neon") && Has(text, "rear lamps") && Has(text, "groups 1111"));
  }
}

// ---------------------------------------------------------------------------
// the shader's two functions over the world rows' emitter records
// ---------------------------------------------------------------------------

struct Basis {
  float ax[3], ay[3], az[3];
};
Basis RandomBasis(Lcg& rng, bool mirror) {
  auto unit = [&](float v[3]) {
    float l = 0.0f;
    for (int i = 0; i < 3; ++i) {
      v[i] = rng.Range(-1.0f, 1.0f);
      l += v[i] * v[i];
    }
    l = std::sqrt(std::max(l, 1e-6f));
    for (int i = 0; i < 3; ++i) v[i] /= l;
  };
  Basis b{};
  unit(b.ax);
  float t[3];
  unit(t);
  float d = b.ax[0] * t[0] + b.ax[1] * t[1] + b.ax[2] * t[2];
  float l = 0.0f;
  for (int i = 0; i < 3; ++i) {
    b.ay[i] = t[i] - d * b.ax[i];
    l += b.ay[i] * b.ay[i];
  }
  l = std::sqrt(std::max(l, 1e-6f));
  for (int i = 0; i < 3; ++i) b.ay[i] /= l;
  b.az[0] = b.ax[1] * b.ay[2] - b.ax[2] * b.ay[1];
  b.az[1] = b.ax[2] * b.ay[0] - b.ax[0] * b.ay[2];
  b.az[2] = b.ax[0] * b.ay[1] - b.ax[1] * b.ay[0];
  if (mirror) {
    for (int i = 0; i < 3; ++i) b.az[i] = -b.az[i];
  }
  return b;
}

lg::Light RandomLight(Lcg& rng, const float around[3]) {
  lg::Light l;
  const uint32_t kind = rng.Below(3);
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = around[i] + rng.Range(-30.0f, 30.0f);
    l.color[i] = rng.Range(0.1f, 2.0f);
  }
  l.pos[1] = around[1] + rng.Range(0.3f, 4.0f);
  float d[3];
  float len = 0.0f;
  for (int i = 0; i < 3; ++i) {
    d[i] = rng.Range(-1.0f, 1.0f);
    len += d[i] * d[i];
  }
  len = std::sqrt(std::max(len, 1e-6f));
  for (int i = 0; i < 3; ++i) l.dir[i] = d[i] / len;
  if (kind == 0) {
    l.law = lg::LightLaw::kHeadlight;
    l.source = lg::LightSource::kHeadlight;
    l.cos_outer = 0.788f;
    l.cos_inner = 0.978f;
    l.range = rng.Range(20.0f, 70.0f);
    l.law_a = 26.0f;
  } else if (kind == 1) {
    l.law = lg::LightLaw::kGameLamp;
    l.source = lg::LightSource::kMod;
    l.cos_outer = rng.Range(-0.2f, 0.6f);
    l.cos_inner = l.cos_outer + rng.Range(0.05f, 0.3f);
    l.range = rng.Range(10.0f, 60.0f);
    l.law_a = rng.Range(5.0f, 90.0f);
  } else {
    l.law = lg::LightLaw::kOmni;
    l.source = lg::LightSource::kNeon;
    l.cos_outer = -1.0f;
    l.cos_inner = -1.0f;
    l.range = rng.Range(4.0f, 20.0f);
    l.law_a = rng.Range(0.3f, 6.0f);
  }
  l.roles = lg::kRoleMaskSurface;
  return l;
}

void ShaderChecks() {
  Lcg rng;
  int cases = 0, lit = 0, wrong_radiance = 0, wrong_to = 0, wrong_out_of_range = 0;
  double worst = 0.0;
  for (int round = 0; round < 400; ++round) {
    const Basis b = RandomBasis(rng, round % 3 == 0);
    const float camera[3] = {rng.Range(-4000.0f, 4000.0f), rng.Range(0.0f, 60.0f), rng.Range(-4000.0f, 4000.0f)};
    const uint32_t count = 1 + rng.Below(32);
    const uint32_t base_row = 7 + rng.Below(500);
    std::vector<lg::Light> lights;
    std::vector<hlsl_shim::uint4> storage(size_t(base_row + count * 4 + 3));
    for (uint32_t i = 0; i < count; ++i) {
      lg::Light l = RandomLight(rng, camera);
      lights.push_back(l);
      const lg::GpuLight g = lg::PackGpuLight(l);
      uint32_t words[16];
      std::memcpy(words, &g, sizeof(g));
      for (uint32_t k = 0; k < 4; ++k) storage[base_row + i * 4 + k] = hlsl_shim::uint4(words[4 * k], words[4 * k + 1], words[4 * k + 2], words[4 * k + 3]);
    }
    hlsl_shim::StructuredBuffer<hlsl_shim::uint4> lamps;
    lamps.data = storage.data();
    lamps.count = storage.size();
    hlsl_shim::gRtLamps = lamps;
    hlsl_shim::RtWorld w;
    w.camera = hlsl_shim::float3(camera[0], camera[1], camera[2]);
    w.axisX = hlsl_shim::float3(b.ax[0], b.ax[1], b.ax[2]);
    w.axisY = hlsl_shim::float3(b.ay[0], b.ay[1], b.ay[2]);
    w.axisZ = hlsl_shim::float3(b.az[0], b.az[1], b.az[2]);
    w.emitterBase = base_row;
    w.emitterCount = count;
    w.emitterStatic = count / 2;
    for (uint32_t i = 0; i < count; ++i) {
      // a surface point near the light and a unit normal, in the WORLD; the view space ones are the same through the axes (a rotation, a mirror among them)
      float pw[3], nw[3], len = 0.0f;
      for (int k = 0; k < 3; ++k) {
        pw[k] = lights[i].pos[k] + rng.Range(-18.0f, 18.0f);
        nw[k] = rng.Range(-1.0f, 1.0f);
        len += nw[k] * nw[k];
      }
      len = std::sqrt(std::max(len, 1e-6f));
      for (int k = 0; k < 3; ++k) nw[k] /= len;
      float rel[3];
      for (int k = 0; k < 3; ++k) rel[k] = pw[k] - camera[k];
      const hlsl_shim::float3 view_point(b.ax[0] * rel[0] + b.ay[0] * rel[1] + b.az[0] * rel[2], b.ax[1] * rel[0] + b.ay[1] * rel[1] + b.az[1] * rel[2], b.ax[2] * rel[0] + b.ay[2] * rel[1] + b.az[2] * rel[2]);
      const hlsl_shim::float3 view_normal(b.ax[0] * nw[0] + b.ay[0] * nw[1] + b.az[0] * nw[2], b.ax[1] * nw[0] + b.ay[1] * nw[1] + b.az[1] * nw[2], b.ax[2] * nw[0] + b.ay[2] * nw[1] + b.az[2] * nw[2]);
      lw::LightSurface s;
      for (int k = 0; k < 3; ++k) {
        s.position[k] = pw[k];
        s.normal[k] = nw[k];
      }
      float want[3];
      lw::LightTerm(lights[i], s, 1.0f, want);
      const hlsl_shim::float3 got = hlsl_shim::RtEmitterRadiance(w, i, view_point, view_normal);
      ++cases;
      const bool any = want[0] > 0.0f || want[1] > 0.0f || want[2] > 0.0f;
      lit += any ? 1 : 0;
      const double e = std::max({std::fabs(got.x - want[0]), std::fabs(got.y - want[1]), std::fabs(got.z - want[2])}) / std::max(1.0, double(std::max({want[0], want[1], want[2]})));
      worst = std::max(worst, e);
      if (!(e < 5e-4)) ++wrong_radiance;
      // out of the range: exactly nothing
      const float d2 = (lights[i].pos[0] - pw[0]) * (lights[i].pos[0] - pw[0]) + (lights[i].pos[1] - pw[1]) * (lights[i].pos[1] - pw[1]) + (lights[i].pos[2] - pw[2]) * (lights[i].pos[2] - pw[2]);
      if (d2 > lights[i].range * lights[i].range * 1.001f && (got.x != 0.0f || got.y != 0.0f || got.z != 0.0f)) ++wrong_out_of_range;
      // the vector to the emitter, in view space
      const float to_world[3] = {lights[i].pos[0] - pw[0], lights[i].pos[1] - pw[1], lights[i].pos[2] - pw[2]};
      const hlsl_shim::float3 to_view(b.ax[0] * to_world[0] + b.ay[0] * to_world[1] + b.az[0] * to_world[2], b.ax[1] * to_world[0] + b.ay[1] * to_world[1] + b.az[1] * to_world[2],
                                      b.ax[2] * to_world[0] + b.ay[2] * to_world[1] + b.az[2] * to_world[2]);
      const hlsl_shim::float3 to = hlsl_shim::RtEmitterTo(w, i, view_point);
      if (!(Near(to.x, to_view.x, 2e-4) && Near(to.y, to_view.y, 2e-4) && Near(to.z, to_view.z, 2e-4))) ++wrong_to;
    }
    CHECK(hlsl_shim::gRtLamps.errors == 0);
  }
  Check(wrong_radiance == 0, Num("RtEmitterRadiance is light_laws.h's LightTerm at the world point, through any axes (%.0f of the cases differ)", wrong_radiance) + Num(", the worst %.1e", worst));
  Check(wrong_out_of_range == 0, Num("an emitter out of its range gives exactly nothing (%.0f do not)", wrong_out_of_range));
  Check(wrong_to == 0, Num("RtEmitterTo is the vector to the emitter in view space (%.0f of the cases differ)", wrong_to));
  Check(cases > 6000 && lit > 500, Num("the random lights and surfaces covered lit and unlit cases (%.0f lit)", lit));
}

void TextChecks(const std::string& src) {
  const std::string glue = ReadFile(src + "/gpu/hlsl/rt_emitters.hlsli");
  const std::string rules = ReadFile(src + "/gpu/rt_emitters_rules.h");
  const std::string frame = ReadFile(src + "/gpu/light_manager.h");
  if (glue.empty() || rules.empty() || frame.empty()) {
    std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    return;
  }
  // the shader reads row 0 alone first, takes the law through the light pass's own function, and nothing here has a name of the game's registers
  CHECK(Has(glue, "if (!(dot(d, d) < range * range))") && Has(glue, "LlLightTerm(LlUnpack(RtEmitterGpuLight(w, i)), pw, nw, 1.0)"));
  CHECK(Has(glue, "const float3 nw = float3(dot(viewNormal, w.axisX), dot(viewNormal, w.axisY), dot(viewNormal, w.axisZ));") && Has(glue, "return w.axisX * d.x + w.axisY * d.y + w.axisZ * d.z;"));
  // the list: the light pass's own sources, the far lights before the budgets, the night factor off the headlight law
  CHECK(Has(rules, "ll::GatherNeonLights(neon_in, &cand.neon);") && Has(rules, "ll::GatherCarLights(cars_in, &cand);") && Has(rules, "ll::GatherModLamps(in.built, in.active_mod, ll::Ab::kGrid, in.camera, &cand.mod);") &&
        Has(rules, "ll::SelectPassLights(filtered, sources, nullptr, 0, cand, in.tier);"));
  // THE BUDGET IS COUNTED AFTER THE FILTERS: every class's candidates and the frame's lights pass the far / number / colour filter (which folds the night factor and the gain in) before the pass's selection counts them
  const size_t filter_at = rules.find("filter.Keep(&cand.police);"), frame_at = rules.find("filtered.lights[i].id = lg::kNoLightId;"), select_at = rules.find("ll::SelectPassLights(");
  CHECK(filter_at != std::string::npos && frame_at != std::string::npos && select_at != std::string::npos && filter_at < frame_at && frame_at < select_at);
  CHECK(Has(rules, "const float scale = gain * (l->law == lg::LightLaw::kHeadlight ? 1.0f : night);") && Has(rules, "if (Far(*l, camera)) {") && Has(rules, "if (!Finite(*l)) {"));
  // the manager's second flag
  {
    size_t both = 0;   // `wanted_ = want_pass_ || want_rt_;` in each of the two setters: one flag cannot take the other's away
    for (size_t at = frame.find("wanted_ = want_pass_ || want_rt_;"); at != std::string::npos; at = frame.find("wanted_ = want_pass_ || want_rt_;", at + 1)) ++both;
    CHECK(Has(frame, "void WantRt(bool wanted) {") && both == 2 && Has(frame, "want_pass_ = wanted;") && Has(frame, "want_rt_ = wanted;"));
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string src = argc > 1 ? argv[1] : "";
  GroupChecks();
  MotionChecks();
  BuildChecks();
  ShaderChecks();
  if (!src.empty()) TextChecks(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

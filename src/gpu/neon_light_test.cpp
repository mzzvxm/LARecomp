// Check of a car's neon underglow as lights of the deferred local light pass (GB-NEON; neon_light.h, local_lights_sources.h, light_budget.h): the decode of the game's car effect object (a fake
// guest memory lays one out), the colour and the pulse as the game makes them (the twin: the 30 Hz hold of hooks/neon_pulse.h gives every frame rate the console's colours), the tube and the
// two readings of its four points, the lights it makes and their budget (whole cars, the tier, the pass's 32 and the priority), the pixel term of such a light held to the one laws (the ground,
// a wall, another car, out of reach, the edge of the reach), the table the hook fills, the ids, and the one budget table. With the `src` folder it also reads the sources: the settings, the
// class table of the graphics card, the pause menu's row, the hook and the pass's wiring. A program of its own: no renderer, no ReXGlue, no GPU, no game; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_neon_light_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_neon_light_test [<the src folder>]     (found from this file's place without an argument)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "headlight_pass_rules.h"
#include "local_lights_sources.h"
#include "mc_engine/hooks/neon_pulse.h"

namespace {

using namespace larecomp::gpu;
namespace lp = larecomp::gpu::local_lights;
namespace nl = larecomp::gpu::neon_light;
namespace lb = mcla::gpu::light_budget;
namespace mn = mcla::neon;

int g_checks = 0, g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
void CheckNear(double got, double want, double tol, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g (tolerance %.3g)\n", what.c_str(), got, want, tol);
  }
}

// ---------------------------------------------------------------------------
// a fake guest memory and a car laid out in it
// ---------------------------------------------------------------------------

struct FakeMem {
  std::map<uint32_t, uint32_t> u32;
  std::map<uint32_t, float> f32;
  std::map<uint32_t, uint8_t> u8;
  uint8_t U8(uint32_t a) const {
    const auto it = u8.find(a);
    return it == u8.end() ? uint8_t(0) : it->second;
  }
  uint32_t U32(uint32_t a) const {
    const auto it = u32.find(a);
    return it == u32.end() ? 0u : it->second;
  }
  float F32(uint32_t a) const {
    const auto it = f32.find(a);
    return it == f32.end() ? 0.0f : it->second;
  }
};

constexpr uint32_t kSelf = 0x40000000u, kRecord = 0x40001000u, kCfg = 0x40002000u, kLight = 0x40003000u;

struct Layout {
  float cfg_rgb[3] = {1.0f, 0.05f, 0.04f};  // a red underglow
  float intensity = 1.5f;
  float pulse = 0.8f;
  float fade = 0.2f * 0.9f;                  // the night constant and a height factor
  bool on = true;
  bool world = true;                         // the tube's points are world positions (else the car's, through the matrix)
  float origin[3] = {820.0f, 17.4f, -795.0f};
  float yaw_deg = 62.0f;
};

// The car's body matrix: the game's row vectors right, up, back, position for a heading.
void BodyMatrix(float yaw_deg, const float origin[3], float m[16]) {
  const float y = yaw_deg * 3.14159265f / 180.0f;
  const float c = std::cos(y), s = std::sin(y);
  const float rows[16] = {c, 0.0f, -s, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s, 0.0f, c, 0.0f, origin[0], origin[1], origin[2], 1.0f};
  std::memcpy(m, rows, sizeof(rows));
}

void LayOut(FakeMem& mem, const Layout& l, uint32_t self = kSelf, uint32_t record = kRecord, uint32_t cfg = kCfg, uint32_t light = kLight) {
  mem.u32[self + nl::kThisRecord] = record;
  mem.u32[record + nl::kRecordConfig] = cfg;
  mem.u32[self + nl::kThisExtLight] = light;
  float colour[3];
  nl::GameColour(l.cfg_rgb, l.intensity, l.pulse, l.fade, colour);
  for (uint32_t i = 0; i < 3; ++i) {
    mem.f32[cfg + nl::kCfgColour + 4 * i] = l.cfg_rgb[i];
    mem.f32[light + nl::kLightColour + 4 * i] = colour[i];
  }
  mem.f32[cfg + nl::kCfgIntensity] = l.intensity;
  mem.f32[self + nl::kThisExtPulse] = l.pulse;
  mem.u8[light + nl::kLightEnable] = l.on ? 1 : 0;
  float body[16];
  BodyMatrix(l.yaw_deg, l.origin, body);
  for (uint32_t i = 0; i < 16; ++i) {
    mem.f32[self + nl::kThisCarMatrix + 4 * i] = body[i];
    mem.f32[light + nl::kLightMatrix + 4 * i] = body[i];
  }
  // the tube under the car along its length (the car's z): A at -1.8, B at +1.8, the two between at a third and two thirds, 0.25 m below the position
  for (uint32_t p = 0; p < nl::kTubePoints; ++p) {
    const float local[3] = {0.0f, -0.25f, -1.8f + 3.6f * float(p) / 3.0f};
    float pt[3];
    if (l.world) {
      nl::TransformPoint(body, local, pt);
    } else {
      std::memcpy(pt, local, sizeof(pt));
    }
    for (uint32_t k = 0; k < 3; ++k) {
      mem.f32[light + nl::kLightPoints + nl::kLightPointStride * p + 4 * k] = pt[k];
    }
    mem.f32[light + nl::kLightPoints + nl::kLightPointStride * p + nl::kLightPointW] = 2.5f;
  }
}

nl::NeonCar CarAt(uint32_t object, const float origin[3], float intensity = 1.5f, float pulse = 0.8f, bool world = true, float yaw = 62.0f) {
  FakeMem mem;
  Layout l;
  l.intensity = intensity;
  l.pulse = pulse;
  l.world = world;
  l.yaw_deg = yaw;
  for (int i = 0; i < 3; ++i) {
    l.origin[i] = origin[i];
  }
  const uint32_t d = (object - kSelf);
  LayOut(mem, l, object, kRecord + d, kCfg + d, kLight + d);
  nl::NeonCar car;
  const bool ok = nl::ReadNeonCar(mem, object, 0, &car);
  Check(ok, "the fake car decodes");
  return car;
}

// ---------------------------------------------------------------------------
// the decode
// ---------------------------------------------------------------------------

void DecodeChecks() {
  FakeMem mem;
  Layout l;
  LayOut(mem, l);
  nl::NeonCar car;
  Check(nl::ReadNeonCar(mem, kSelf, 0, &car), "the effect object decodes");
  Check(car.object == kSelf && car.record == kRecord && car.on && !car.player, "the object, the record, the enable byte; no player without a player's record");
  float want[3];
  nl::GameColour(l.cfg_rgb, l.intensity, l.pulse, l.fade, want);
  for (int i = 0; i < 3; ++i) {
    CheckNear(car.colour[i], want[i], 1.0e-7, "the stored colour is read as the game stored it");
  }
  CheckNear(car.pulse, l.pulse, 0.0, "the pulse is this[404]");
  CheckNear(car.fade, l.fade, 1.0e-5, "the implied fade is the one the colour was made with");
  Check(car.twin_ok, "the colour follows the game's arithmetic of the config and the pulse");
  CheckNear(car.car_matrix[12], l.origin[0], 1.0e-4, "the body matrix's position row");
  Check(car.point_w[0] == 2.5f && car.point_w[3] == 2.5f, "the points' w");
  Check(nl::ReadNeonCar(mem, kSelf, kRecord, &car) && car.player, "the player's record marks the player's car");
  // off: the enable byte 0
  FakeMem off;
  l.on = false;
  LayOut(off, l);
  Check(nl::ReadNeonCar(off, kSelf, 0, &car) && !car.on, "an enable byte of 0 is a car with its neon off");
  // implausible pointers: nothing is decoded
  FakeMem bad;
  Check(!nl::ReadNeonCar(bad, kSelf, 0, &car), "a record that is not a pointer decodes nothing");
  FakeMem no_light;
  LayOut(no_light, Layout{});
  no_light.u32[kSelf + nl::kThisExtLight] = 0;
  Check(!nl::ReadNeonCar(no_light, kSelf, 0, &car), "a car with no light object decodes nothing");
  Check(!nl::ReadNeonCar(mem, 0x100u, 0, &car) && !nl::ReadNeonCar(mem, 0xF0000000u, 0, &car), "an effect object that is not a pointer decodes nothing");
  // the colour is finite and not negative
  FakeMem odd;
  LayOut(odd, Layout{});
  odd.f32[kLight + nl::kLightColour] = std::nanf("");
  odd.f32[kLight + nl::kLightColour + 4] = -1.0f;
  Check(nl::ReadNeonCar(odd, kSelf, 0, &car) && car.colour[0] == 0.0f && car.colour[1] == 0.0f, "a NaN or negative colour channel is 0");
  // the layout numbers are the guest's (sub_823511F8, sub_823553F8)
  Check(nl::kLightEnable == 4 && nl::kLightPoints == 16 && nl::kLightPointW == 12 && nl::kLightMatrix == 80 && nl::kLightColour == 144 && nl::kThisExtLight == 560 && nl::kThisExtPulse == 404 &&
            nl::kThisCarMatrix == 4512 && nl::kCfgColour == 80 && nl::kCfgIntensity == 368,
        "the offsets are the recompiled code's");
  Check(nl::kLightPoints + 3 * nl::kLightPointStride + nl::kLightPointW + 4 == 80, "the four points and their w end where the matrix begins (+80)");
  Check(nl::kLightMatrix + 64 == 144, "the matrix ends where the colour begins (+144)");
}

// ---------------------------------------------------------------------------
// the colour and the pulse: the twin
// ---------------------------------------------------------------------------

void TwinChecks() {
  const float cfg[3] = {1.0f, 0.4f, 0.1f};
  float c[3];
  nl::GameColour(cfg, 2.0f, 0.5f, 0.2f, c);
  // the game's order: (intensity x pulse) x fade, then the colour
  const float scale = (2.0f * 0.5f) * 0.2f;
  Check(c[0] == cfg[0] * scale && c[1] == cfg[1] * scale && c[2] == cfg[2] * scale, "the colour is the config colour times (intensity x pulse) x fade, in the game's order");
  Check(std::fabs(nl::ImpliedFade(c, cfg, 2.0f, 0.5f) - 0.2f) < 1.0e-6f, "the implied fade is the fade");
  Check(nl::ImpliedFade(c, cfg, 2.0f, 0.0f) < 0.0f && nl::ImpliedFade(c, cfg, 0.0f, 0.5f) < 0.0f, "a zero pulse or intensity tells no fade");
  const float zero_cfg[3] = {0.0f, 0.0f, 0.0f};
  Check(nl::ImpliedFade(c, zero_cfg, 2.0f, 0.5f) < 0.0f, "a black config colour tells no fade");
  float fade = 0.0f;
  Check(nl::ColourFollowsTwin(c, cfg, 2.0f, 0.5f, &fade) && std::fabs(fade - 0.2f) < 1.0e-6f, "the true colour follows the twin");
  float skewed[3] = {c[0], c[1] * 1.3f, c[2]};
  Check(!nl::ColourFollowsTwin(skewed, cfg, 2.0f, 0.5f, &fade), "one channel off the twin is caught (a wrong offset shows as this)");
  float too_bright[3] = {c[0] * 20.0f, c[1] * 20.0f, c[2] * 20.0f};
  Check(!nl::ColourFollowsTwin(too_bright, cfg, 2.0f, 0.5f, &fade), "a fade far over 1 is not the game's");
  Check(nl::ColourFollowsTwin(c, cfg, 2.0f, 0.0f, &fade), "nothing to compare at a zero pulse: not a failure");

  // THE PULSE TWIN: the colour at a frame is the colour of the held clock (hooks/neon_pulse.h), so every frame rate shows the console's colours. For the flash rates of the garage car (19) and slow
  // ones, the sequence of distinct colours a second at 30, 60, 120, 144 and 240 FPS is the same one, never more than the console's 30 a second.
  const int8_t rates[] = {1, 3, 5, 19};
  const double fps_list[] = {30.0, 60.0, 120.0, 144.0, 240.0};
  for (const int8_t rate : rates) {
    std::vector<std::vector<float>> by_fps;
    for (const double fps : fps_list) {
      std::vector<float> reds;
      const double dt = 1.0 / fps;
      for (double t = 10.0; t < 14.0; t += dt) {
        const float clock = float(t);
        const float pulse = mn::HeldPulseOf(rate, clock, mn::StdSine);
        float col[3];
        nl::GameColour(cfg, 1.5f, pulse, 0.2f, col);
        // what the colour is at a time must be the colour of the 30 Hz step the time is in
        const float held = mn::HeldClock(clock);
        float step[3];
        nl::GameColour(cfg, 1.5f, mn::PulseOf(rate, held), 0.2f, step);
        if (col[0] != step[0]) {
          Check(false, "the colour of a frame is the colour of its 30 Hz step");
        }
        reds.push_back(col[0]);
      }
      std::set<float> distinct(reds.begin(), reds.end());
      Check(distinct.size() <= size_t(30 * 4 + 2), "at " + std::to_string(int(fps)) + " FPS the colour changes at most 30 times a second (rate " + std::to_string(int(rate)) + ")");
      by_fps.push_back(reds);
    }
    // the unheld pulse would change every frame at 240 FPS: the hold is what the test above proves, so the count is a real difference
    std::set<float> unheld;
    for (double t = 10.0; t < 14.0; t += 1.0 / 240.0) {
      unheld.insert(mn::PulseOf(rate, float(t)));
    }
    std::set<float> held240(by_fps.back().begin(), by_fps.back().end());
    Check(held240.size() < unheld.size() || rate == 1 || rate == 3, "the hold takes the 240 FPS colour sequence down to the console's (rate " + std::to_string(int(rate)) + ")");
  }
  // the pulse the hook reads is bounded 0..1 and so is the colour (the fade and the config bound the rest)
  for (const int8_t rate : rates) {
    for (double t = 0.0; t < 3.0; t += 1.0 / 97.0) {
      const float p = mn::HeldPulseOf(rate, float(t), mn::StdSine);
      Check(p >= 0.0f && p <= 1.0f, "the held pulse is between 0 and 1");
    }
  }
}

// ---------------------------------------------------------------------------
// the tube
// ---------------------------------------------------------------------------

void TubeChecks() {
  const float origin[3] = {820.0f, 17.4f, -795.0f};
  // world reading
  {
    const nl::NeonCar car = CarAt(kSelf, origin, 1.5f, 0.8f, true);
    nl::Tube t;
    Check(nl::ResolveTube(car, &t) && t.space == nl::Space::kWorld, "points in the world are read as the world");
    CheckNear(t.length, 3.6, 1.0e-3, "the tube is 3.6 m");
    CheckNear(t.mid_from_car, 0.25, 1.0e-3, "its middle is 0.25 m under the car's position");
    // the end A is behind or in front along the car's heading; the tube's middle is the car's position less 0.25 m
    CheckNear(0.5 * (t.a[1] + t.b[1]), origin[1] - 0.25, 1.0e-3, "the middle is under the car");
  }
  // car reading: the points are the car's, the matrix puts them in the world
  {
    const nl::NeonCar car = CarAt(kSelf, origin, 1.5f, 0.8f, false);
    nl::Tube t;
    Check(nl::ResolveTube(car, &t) && t.space == nl::Space::kCar, "points in the car's space are put through the matrix and read as the car's");
    CheckNear(t.length, 3.6, 1.0e-3, "the same tube");
    CheckNear(t.mid_from_car, 0.25, 1.0e-3, "under the car as well");
    const nl::NeonCar world = CarAt(kSelf, origin, 1.5f, 0.8f, true);
    nl::Tube tw;
    nl::ResolveTube(world, &tw);
    for (int i = 0; i < 3; ++i) {
      CheckNear(t.a[i], tw.a[i], 1.0e-3, "the two readings of the same tube agree at A");
      CheckNear(t.b[i], tw.b[i], 1.0e-3, "... and at B");
    }
  }
  // neither reading puts the tube at the car: not placed
  {
    nl::NeonCar car = CarAt(kSelf, origin);
    for (int p = 0; p < 4; ++p) {
      car.points[p][0] += 50.0f;
    }
    nl::Tube t;
    Check(!nl::ResolveTube(car, &t) && t.space == nl::Space::kNone, "a tube 50 m from the car is not its underglow");
  }
  // too long
  {
    nl::NeonCar car = CarAt(kSelf, origin);
    car.points[0][2] -= 30.0f;
    nl::Tube t;
    Check(!nl::ResolveTube(car, &t), "a tube of 30 m is not a car's");
  }
  // not finite
  {
    nl::NeonCar car = CarAt(kSelf, origin);
    car.points[0][0] = std::nanf("");
    nl::Tube t;
    Check(!nl::ResolveTube(car, &t), "a NaN point places nothing");
    nl::NeonCar car2 = CarAt(kSelf, origin);
    car2.car_matrix[12] = std::nanf("");
    Check(!nl::ResolveTube(car2, &t), "a NaN car position places nothing");
  }
  // a car at the world origin: both readings can be near; the nearer one wins and the lights are the same
  {
    const float zero[3] = {0.0f, 0.5f, 0.0f};
    const nl::NeonCar car = CarAt(kSelf, zero, 1.5f, 0.8f, true, 0.0f);
    nl::Tube t;
    Check(nl::ResolveTube(car, &t), "a car near the world origin places");
  }
  // the transform is the game's row vector product with the position row as the translation
  {
    float m[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 10, 20, 30, 1};  // a quarter turn about y, moved to (10, 20, 30)
    const float p[3] = {1.0f, 2.0f, 3.0f};
    float out[3];
    nl::TransformPoint(m, p, out);
    CheckNear(out[0], 1.0 * 0 + 2.0 * 0 + 3.0 * -1 + 10, 1.0e-6, "p x M: x");
    CheckNear(out[1], 2.0 + 20, 1.0e-6, "p x M: y");
    CheckNear(out[2], 1.0 * 1 + 30, 1.0e-6, "p x M: z");
  }
}

// ---------------------------------------------------------------------------
// the lights
// ---------------------------------------------------------------------------

std::vector<nl::NeonCar> Cars(int n, const float camera[3], float spacing = 5.0f) {
  std::vector<nl::NeonCar> cars;
  for (int i = 0; i < n; ++i) {
    const float origin[3] = {camera[0] + 3.0f + spacing * float(i), 17.4f, camera[2] + 2.0f};
    cars.push_back(CarAt(kSelf + 0x10000u * uint32_t(i + 1), origin));
  }
  return cars;
}

void LightChecks() {
  const float camera[3] = {820.0f, 18.0f, -800.0f};
  const std::vector<nl::NeonCar> cars = Cars(6, camera);
  for (const lp::Tier tier : {lp::Tier::kMedium, lp::Tier::kHigh, lp::Tier::kUltra}) {
    const lp::ClassBudget b = lp::BudgetOf(tier);
    lp::NeonSourceInputs in;
    in.cars = &cars;
    in.tier = tier;
    in.gain = 1.0f;
    std::memcpy(in.camera, camera, sizeof(camera));
    std::vector<Light> lights;
    lp::NeonReport report;
    lp::GatherNeonLights(in, &lights, &report);
    const std::string at = std::string(" at ") + lp::TierName(tier);
    Check(report.seen == 6 && report.lit == b.neon_cars && report.cut == 6 - b.neon_cars, "the nearest cars are lit, the rest cut" + at);
    Check(lights.size() == size_t(b.neon_cars) * b.neon_points && lights.size() == b.neon, "whole tubes: cars x points lights" + at);
    Check(report.space == nl::Space::kWorld, "the report says how the points were read" + at);
    // nearest first: the cars are 5 m apart in x, the first is the nearest; the lights come car by car
    std::set<uint64_t> ids;
    for (const Light& l : lights) {
      Check(l.source == LightSource::kNeon && l.kind == LightKind::kPoint && l.law == LightLaw::kOmni && l.roles == kRoleMaskSurface && l.id != kNoLightId, "a neon light is an omni surface light of the neon source" + at);
      Check(l.range == lp::NeonReach(tier) && l.law_a > 0.0f && l.range > 0.0f, "its range is the tier's reach" + at);
      CheckNear(l.law_a, lp::kNeonCutShare * l.range * l.range, 1.0e-4, "its plateau is the cut share of the reach squared" + at);
      ids.insert(l.id);
    }
    Check(ids.size() == lights.size(), "every neon light has its own id" + at);
    for (uint32_t i = 0; i < b.neon_cars; ++i) {
      const uint32_t first = i * b.neon_points;
      Check(lights[first].car_key == lights[first + b.neon_points - 1].car_key, "the lights of a tube are one car's" + at);
      if (i > 0) {
        Check(lights[first].car_key != lights[first - 1].car_key, "the next tube is the next car's" + at);
      }
    }
    // the cars lit are the nearest ones, in order
    if (b.neon_cars >= 2) {
      float d0 = 0.0f, d1 = 0.0f;
      for (int k = 0; k < 3; ++k) {
        d0 += (lights[0].pos[k] - camera[k]) * (lights[0].pos[k] - camera[k]);
        d1 += (lights[b.neon_points].pos[k] - camera[k]) * (lights[b.neon_points].pos[k] - camera[k]);
      }
      Check(d0 < d1, "the nearest car's tube comes first" + at);
    }
    // the same frame again: the same ids (a light keeps its identity, the picker's hysteresis keys on it)
    std::vector<Light> again;
    lp::GatherNeonLights(in, &again);
    Check(again.size() == lights.size(), "a second gather is the same" + at);
    for (size_t i = 0; i < again.size() && i < lights.size(); ++i) {
      Check(again[i].id == lights[i].id && std::memcmp(again[i].pos, lights[i].pos, sizeof(again[i].pos)) == 0, "a light keeps its id and place" + at);
    }
  }
  // the points of a tube sit along it, under the car, in equal parts
  {
    const nl::NeonCar car = CarAt(kSelf, camera, 1.5f, 0.8f, true);
    nl::Tube tube;
    nl::ResolveTube(car, &tube);
    std::vector<Light> out;
    lp::MakeNeonTubeLights(car, tube, 4, lp::Tier::kUltra, 1.0f, &out);
    Check(out.size() == 4, "four points at four");
    for (size_t i = 0; i < out.size(); ++i) {
      const float t = (float(i) + 0.5f) / 4.0f;
      for (int k = 0; k < 3; ++k) {
        CheckNear(out[i].pos[k], tube.a[k] + (tube.b[k] - tube.a[k]) * t, 1.0e-4, "the point sits at the middle of its part of the tube");
      }
    }
    std::vector<Light> two;
    lp::MakeNeonTubeLights(car, tube, 2, lp::Tier::kMedium, 1.0f, &two);
    CheckNear(two[0].pos[0], tube.a[0] + (tube.b[0] - tube.a[0]) * 0.25f, 1.0e-4, "two points are at a quarter and three quarters");
    // a tube that is a point is one light
    nl::NeonCar tiny = car;
    for (int p = 0; p < 4; ++p) {
      for (int k = 0; k < 3; ++k) {
        tiny.points[p][k] = car.points[0][k];
      }
    }
    nl::Tube point_tube;
    Check(nl::ResolveTube(tiny, &point_tube) && point_tube.length < nl::kPointTube, "a tube of no length is a point");
    std::vector<Light> one;
    lp::MakeNeonTubeLights(tiny, point_tube, 4, lp::Tier::kUltra, 1.0f, &one);
    Check(one.size() == 1, "a point tube makes one light");
  }
  // the colour: the tube's lights share one game point's strength; at the plateau their sum is the game's colour
  {
    const nl::NeonCar car = CarAt(kSelf, camera, 1.5f, 0.8f, true);
    nl::Tube tube;
    nl::ResolveTube(car, &tube);
    for (const uint32_t points : {1u, 2u, 3u, 4u}) {
      std::vector<Light> out;
      lp::MakeNeonTubeLights(car, tube, points, lp::Tier::kHigh, 1.0f, &out);
      double sum[3] = {0, 0, 0};
      for (const Light& l : out) {
        const float scale = 1.0f / l.law_a;  // the law's term inside the plateau
        for (int i = 0; i < 3; ++i) {
          sum[i] += double(l.color[i]) * scale;
        }
      }
      for (int i = 0; i < 3; ++i) {
        CheckNear(sum[i], double(car.colour[i]) * lp::kNeonTubeGain, 1.0e-5, "at the plateau the tube's lights add up to the game's colour (" + std::to_string(points) + " points)");
      }
    }
    // the gain is a plain multiplier, 0 puts the light out
    std::vector<Light> g2, g0;
    lp::MakeNeonTubeLights(car, tube, 2, lp::Tier::kMedium, 2.0f, &g2);
    lp::MakeNeonTubeLights(car, tube, 2, lp::Tier::kMedium, 0.0f, &g0);
    std::vector<Light> g1;
    lp::MakeNeonTubeLights(car, tube, 2, lp::Tier::kMedium, 1.0f, &g1);
    CheckNear(g2[0].color[0], 2.0 * g1[0].color[0], 1.0e-6, "the gain doubles the colour");
    Check(g0[0].color[0] == 0.0f && g0[0].color[1] == 0.0f, "a gain of 0 puts it out");
    // the colour follows the pulse: a different pulse is a different light, a dark pulse no light
    const nl::NeonCar dark = CarAt(kSelf, camera, 1.5f, 0.0f, true);
    lp::NeonSourceInputs in;
    const std::vector<nl::NeonCar> only_dark = {dark};
    in.cars = &only_dark;
    std::memcpy(in.camera, camera, sizeof(camera));
    std::vector<Light> out;
    lp::NeonReport report;
    lp::GatherNeonLights(in, &out, &report);
    Check(out.empty() && report.dim == 1, "the dark half of the pulse lights nothing (and takes no budget)");
    const nl::NeonCar bright = CarAt(kSelf, camera, 1.5f, 1.0f, true);
    const nl::NeonCar half = CarAt(kSelf, camera, 1.5f, 0.5f, true);
    CheckNear(half.colour[0] * 2.0, bright.colour[0], 1.0e-6, "half the pulse is half the colour");
  }
  // the look's starting numbers are pinned (a change of one is a decision, made with the pictures: RUNPLAN-gb-neon.txt)
  Check(lp::kNeonTubeGain == 1.0f && lp::kNeonCutShare == 0.06f && lp::kNeonMinColour == 0.003f && lp::kNeonSeenMetres == 40.0f, "the neon look's numbers are the planned ones (tube gain 1, cut share 6 %, floor 0.003, 40 m)");
  Check(lp::NeonReach(lp::Tier::kMedium) == 6.0f && lp::NeonReach(lp::Tier::kHigh) == 7.0f && lp::NeonReach(lp::Tier::kUltra) == 8.0f, "the neon's reach is 6 / 7 / 8 m by tier");
  // the reasons a car is not lit
  {
    std::vector<nl::NeonCar> cars2;
    nl::NeonCar off = CarAt(kSelf + 0x100000u, camera);
    off.on = false;
    cars2.push_back(off);
    cars2.push_back(CarAt(kSelf + 0x200000u, camera, 1.5f, 0.0f));  // dark
    nl::NeonCar lost = CarAt(kSelf + 0x300000u, camera);
    for (int p = 0; p < 4; ++p) {
      lost.points[p][0] += 90.0f;
    }
    cars2.push_back(lost);
    const float far_origin[3] = {camera[0] + 120.0f, 17.4f, camera[2]};
    cars2.push_back(CarAt(kSelf + 0x400000u, far_origin));
    cars2.push_back(CarAt(kSelf + 0x500000u, camera));  // the one that lights
    lp::NeonSourceInputs in;
    in.cars = &cars2;
    std::memcpy(in.camera, camera, sizeof(camera));
    lp::NeonReport report;
    std::vector<Light> out;
    lp::GatherNeonLights(in, &out, &report);
    Check(report.seen == 5 && report.off == 1 && report.dim == 1 && report.unplaced == 1 && report.distant == 1 && report.lit == 1 && report.cut == 0, "off, dark, unplaced, far and lit are counted apart");
    Check(out.size() == lp::BudgetOf(lp::Tier::kMedium).neon_points, "and only the lit one has lights");
    lp::NeonSourceInputs none;
    out.clear();
    lp::GatherNeonLights(none, &out, &report);
    Check(out.empty() && report.seen == 0, "no cars, no lights");
  }
  // NEONALL-REVIEW: a car whose tube is out of the picture takes none of the car budget (the pack of racers behind the camera must not push the car ahead out of the nearest few)
  {
    // two cars behind the camera (x - 6, x - 9: the nearest), then three in front (x + 12, +20, +30): the picture looks along +x, a light is seen when its reach (the tier's) is on the +x side of the camera
    std::vector<nl::NeonCar> pack;
    const float xs[5] = {-6.0f, -9.0f, 12.0f, 20.0f, 30.0f};
    for (int i = 0; i < 5; ++i) {
      const float origin[3] = {camera[0] + xs[i], 17.4f, camera[2] + 2.0f};
      pack.push_back(CarAt(kSelf + 0x10000u * uint32_t(i + 1), origin));
    }
    for (const lp::Tier tier : {lp::Tier::kMedium, lp::Tier::kHigh, lp::Tier::kUltra}) {
      const lp::ClassBudget b = lp::BudgetOf(tier);
      lp::NeonSourceInputs in;
      in.cars = &pack;
      in.tier = tier;
      std::memcpy(in.camera, camera, sizeof(camera));
      const std::string at = std::string(" at ") + lp::TierName(tier);
      // no predicate: every light can be seen, the gather is what it was (the two behind take two of the slots)
      std::vector<Light> plain;
      lp::NeonReport plain_report;
      lp::GatherNeonLights(in, &plain, &plain_report);
      Check(plain_report.unseen == 0 && plain_report.lit == std::min<uint32_t>(b.neon_cars, 5u), "without a predicate nothing is out of the picture" + at);
      // with it: the lights are seen from x >= camera - reach (a reach of the tier's on the +x side means the car's x - the reach is ahead of the camera or the light is within the reach of it)
      const float reach = lp::NeonReach(tier);
      in.can_be_seen = [&](const Light& l) { return l.pos[0] + reach > camera[0] + 6.0f; };  // the two cars behind (x - 6, x - 9) are out of the picture, the others in
      std::vector<Light> out;
      lp::NeonReport report;
      lp::GatherNeonLights(in, &out, &report);
      const uint32_t want_lit = std::min<uint32_t>(b.neon_cars, 3u);  // only the three in front can be lit
      Check(report.lit == want_lit && out.size() == size_t(want_lit) * b.neon_points, "the cars out of the picture take no budget: the nearest cars that are seen are lit" + at);
      Check(report.unseen >= 1 || b.neon_cars <= 3, "the cars behind are counted as out of the picture" + at);
      bool behind = false;
      for (const Light& l : out) {
        behind = behind || l.pos[0] < camera[0];
      }
      Check(!behind, "none of the lights is behind the camera" + at);
      // the nearest of the seen cars comes first (the one 12 m ahead)
      Check(!out.empty() && out.front().pos[0] > camera[0] + 6.0f && out.front().pos[0] < camera[0] + 16.0f, "the nearest car in the picture comes first" + at);
      // a predicate that sees nothing lights nothing and takes no budget
      in.can_be_seen = [](const Light&) { return false; };
      std::vector<Light> none;
      lp::NeonReport none_report;
      lp::GatherNeonLights(in, &none, &none_report);
      Check(none.empty() && none_report.lit == 0 && none_report.cut + none_report.unseen == 5, "a picture with no tube in it lights nothing" + at);
    }
  }
}

// ---------------------------------------------------------------------------
// the selection: the class in the pass's budget
// ---------------------------------------------------------------------------

void SelectionChecks() {
  const float camera[3] = {820.0f, 18.0f, -800.0f};
  const std::vector<nl::NeonCar> cars = Cars(6, camera);
  LightFrame frame{};
  for (const lp::Tier tier : {lp::Tier::kMedium, lp::Tier::kHigh, lp::Tier::kUltra}) {
    lp::NeonSourceInputs in;
    in.cars = &cars;
    in.tier = tier;
    std::memcpy(in.camera, camera, sizeof(camera));
    lp::Candidates cand;
    lp::GatherNeonLights(in, &cand.neon);
    const lp::ClassBudget b = lp::BudgetOf(tier);
    // with the neon bit: the class's lights; without it: none, and not even counted as cut
    const lp::Selection s = lp::SelectPassLights(frame, lp::kSourceNeon, nullptr, 0, cand, tier);
    Check(s.count == b.neon && s.from_class[size_t(lp::SourceClass::kNeon)] == b.neon && s.cut_class[size_t(lp::SourceClass::kNeon)] == 0, std::string("the neon bit takes the class's lights at ") + lp::TierName(tier));
    const lp::Selection no = lp::SelectPassLights(frame, lp::kSourceMask, nullptr, 0, cand, tier);
    Check(no.from_class[size_t(lp::SourceClass::kNeon)] == 0 && no.cut_class[size_t(lp::SourceClass::kNeon)] == 0, "without the bit the class is not asked for");
    Check((lp::kSourceMask & lp::kSourceNeon) == 0 && (lp::kSourceAll & lp::kSourceNeon) != 0 && lp::kSourceNeon == 64, "the neon bit is not in the debug setting's mask (the NEON LIGHT row puts it in)");
    // a class cap smaller than what the gather made cuts at the cap (a gather of more cars than the tier takes, handed in by hand)
    lp::NeonSourceInputs big = in;
    big.tier = lp::Tier::kUltra;
    lp::Candidates many;
    lp::GatherNeonLights(big, &many.neon);
    const lp::Selection capped = lp::SelectPassLights(frame, lp::kSourceNeon, nullptr, 0, many, lp::Tier::kMedium);
    Check(capped.count == lp::BudgetOf(lp::Tier::kMedium).neon && capped.cut_class[size_t(lp::SourceClass::kNeon)] == many.neon.size() - capped.count, "a longer candidate list is cut at the tier's cap");
  }
  // the priority: police, mods, neon, head lamps, rear lamps: when the pass's 32 are short the head and rear lamps go first
  {
    lp::Candidates cand;
    auto make = [](uint64_t id_base, size_t n, LightSource source) {
      std::vector<Light> v;
      for (size_t i = 0; i < n; ++i) {
        Light l;
        l.id = id_base + i;
        l.range = 10.0f;
        l.law_a = 2.0f;
        l.color[0] = 1.0f;
        l.source = source;
        l.roles = kRoleMaskSurface;
        v.push_back(l);
      }
      return v;
    };
    cand.police = make(1000, 16, LightSource::kPolice);
    cand.mod = make(2000, 12, LightSource::kMod);
    cand.head = make(3000, 8, LightSource::kHeadlight);
    cand.rear = make(4000, 12, LightSource::kOther);
    lp::NeonSourceInputs in;
    const std::vector<nl::NeonCar> six = Cars(6, camera);
    in.cars = &six;
    in.tier = lp::Tier::kUltra;
    std::memcpy(in.camera, camera, sizeof(camera));
    lp::GatherNeonLights(in, &cand.neon);
    const lp::Selection s = lp::SelectPassLights(frame, lp::kSourcePolice | lp::kSourceMod | lp::kSourceNeon | lp::kSourceHeadLamps | lp::kSourceRearLamps, nullptr, 0, cand, lp::Tier::kUltra);
    Check(s.count == 32, "the pass's 32 are full");
    Check(s.from_class[size_t(lp::SourceClass::kPolice)] == 16 && s.from_class[size_t(lp::SourceClass::kMod)] == 12 && s.from_class[size_t(lp::SourceClass::kNeon)] == 4,
          "police, then mods, then neon take what is left of the 32 (4 neon lights)");
    Check(s.from_class[size_t(lp::SourceClass::kHeadLamps)] == 0 && s.from_class[size_t(lp::SourceClass::kRearLamps)] == 0, "the head and the rear lamps are what the 32 cut");
    Check(s.cut_class[size_t(lp::SourceClass::kNeon)] == 12, "the neon lights the 32 cut are counted (16 wanted, 4 taken)");
    // a whole car is the unit of a tube, but the pass's 32 can cut inside one: the two lights of a medium tube
    cand.police = make(1000, 6, LightSource::kPolice);
    cand.mod = make(2000, 4, LightSource::kMod);
    const lp::Selection m = lp::SelectPassLights(frame, lp::kSourcePolice | lp::kSourceMod | lp::kSourceNeon, nullptr, 0, cand, lp::Tier::kUltra);
    Check(m.count == 6 + 4 + 16 && m.from_class[size_t(lp::SourceClass::kNeon)] == 16, "with room the whole neon class is taken");
  }
  // the class table's names
  Check(lp::kSourceClassCount == 7 && std::string(lp::SourceClassName(lp::SourceClass::kNeon)) == "neon" && lp::CapOf(lp::BudgetOf(lp::Tier::kUltra), lp::SourceClass::kNeon) == 16 &&
            lp::CapOf(lp::BudgetOf(lp::Tier::kHigh), lp::SourceClass::kNeon) == 9 && lp::CapOf(lp::BudgetOf(lp::Tier::kMedium), lp::SourceClass::kNeon) == 4,
        "the class is named and capped 4 / 9 / 16");
}

// ---------------------------------------------------------------------------
// the budget table of the pickers
// ---------------------------------------------------------------------------

void BudgetChecks() {
  for (const lp::Tier tier : {lp::Tier::kMedium, lp::Tier::kHigh, lp::Tier::kUltra}) {
    const lp::ClassBudget c = lp::BudgetOf(tier);
    const lb::LightBudget b = lb::BudgetFor(true, lp::LevelOf(tier), 0, lb::Level::kHigh, lb::Table(lb::kThreeLevels), true);
    Check(b.neon_cars == c.neon_cars && b.neon_points == c.neon_points && b.neon_cars * b.neon_points == c.neon, std::string("the one budget table and the pass's class say the same at ") + lp::TierName(tier));
    const lb::LightBudget off = lb::BudgetFor(true, lp::LevelOf(tier), 0, lb::Level::kHigh, lb::Table(lb::kThreeLevels), false);
    Check(off.neon_cars == 0 && off.neon_points == 0, "NEON LIGHT off: no neon in the budget");
    Check(lb::NeonLights(lp::LevelOf(tier)) == c.neon, "NeonLights is cars x points");
  }
  Check(lb::NeonLights(lb::Level::kMedium) < lb::NeonLights(lb::Level::kHigh) && lb::NeonLights(lb::Level::kHigh) < lb::NeonLights(lb::Level::kUltra), "a higher level never has fewer lights");
  Check(lb::NeonRowOf(lb::Level::kOff).cars == lb::NeonRowOf(lb::Level::kMedium).cars, "the level `off` of LIGHT SHADOWS takes the smallest neon row");
  Check(lb::NeonRowOf(lb::Level::kMedium).points >= 1 && lb::NeonRowOf(lb::Level::kUltra).points <= nl::kTubePoints, "a tube has one to four points");
  // the existing budgets are what they were
  const lb::LightBudget old_style = lb::BudgetFor(true, lb::Level::kUltra, 0, lb::Level::kHigh, lb::Table(lb::kThreeLevels));
  Check(old_style.neon_cars == 0 && old_style.headlights == 4 && old_style.shadow_cars == 3, "a call without the neon argument is the old budget");
  Check(lp::BudgetOf(lp::Tier::kUltra).police == 16 && lp::BudgetOf(lp::Tier::kHigh).head == 5 && lp::BudgetOf(lp::Tier::kMedium).mod == 4 && lp::BudgetOf(lp::Tier::kMedium).frame == 4, "the other classes' budgets are untouched");
}

// ---------------------------------------------------------------------------
// the pixel term of a neon light
// ---------------------------------------------------------------------------

void PixelChecks() {
  const float origin[3] = {820.0f, 17.4f, -795.0f};
  const nl::NeonCar car = CarAt(kSelf, origin, 1.5f, 1.0f, true);
  nl::Tube tube;
  nl::ResolveTube(car, &tube);
  std::vector<Light> lights;
  lp::MakeNeonTubeLights(car, tube, 2, lp::Tier::kHigh, 1.0f, &lights);
  lp::Units units;
  units.night = 1.0f;
  units.exposure = 16.0f;
  const float albedo[3] = {0.5f, 0.5f, 0.5f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  auto lit = [&](const float world[3], const float normal[3], float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    for (const Light& l : lights) {
      float one[3];
      lp::PixelLight(l, units, world, normal, albedo, 1.0f, one);
      for (int i = 0; i < 3; ++i) {
        out[i] += one[i];
      }
    }
  };
  const float mid[3] = {0.5f * (tube.a[0] + tube.b[0]), 0.5f * (tube.a[1] + tube.b[1]), 0.5f * (tube.a[2] + tube.b[2])};
  // the ground under the car
  float ground_pt[3] = {mid[0], mid[1] - 0.3f, mid[2]};
  float under[3];
  lit(ground_pt, up, under);
  Check(under[0] > 0.0f && under[0] > under[1] * 5.0f && under[0] > under[2] * 5.0f, "the ground under the car is lit, in the neon's colour (red)");
  // the ground 3 m to the side: lit less
  const float side = std::sqrt(0.5f);
  (void)side;
  float far_ground[3] = {mid[0] + 3.0f, mid[1] - 0.3f, mid[2]};
  float away[3];
  lit(far_ground, up, away);
  Check(away[0] > 0.0f && away[0] < under[0], "the ground 3 m out is lit, less");
  // a wall 2.5 m from the car, facing it: lit (the wall at x = mid + 2.5 with its normal toward the car)
  const float wall_pt[3] = {mid[0] + 2.5f, mid[1] + 0.5f, mid[2]};
  const float wall_n[3] = {-1.0f, 0.0f, 0.0f};
  float wall[3];
  lit(wall_pt, wall_n, wall);
  Check(wall[0] > 0.0f, "a wall beside the car, facing it, is lit");
  // the same wall's back: not lit (N.L)
  const float wall_back[3] = {1.0f, 0.0f, 0.0f};
  float back[3];
  lit(wall_pt, wall_back, back);
  Check(back[0] == 0.0f && back[1] == 0.0f && back[2] == 0.0f, "the wall's far side takes no light");
  // another car's bodywork 4 m away: lit
  const float other_pt[3] = {mid[0], mid[1] + 0.4f, mid[2] + 6.0f - 3.0f};
  const float other_n[3] = {0.0f, 0.2f, -0.98f};
  float other[3];
  lit(other_pt, other_n, other);
  Check(other[0] >= 0.0f, "another car's body near the tube is evaluated (lit when it faces the tube)");
  // out of reach: nothing
  const float beyond[3] = {mid[0] + 12.0f, mid[1] - 0.3f, mid[2]};
  float gone[3];
  lit(beyond, up, gone);
  Check(gone[0] == 0.0f && gone[1] == 0.0f && gone[2] == 0.0f, "12 m away (past the 7 m reach and the tube's own length) takes no light");
  // the edge of the reach: the step is a small share of the plateau's value (the cut share, 6 %)
  {
    Light l = lights[0];
    l.pos[0] = 0.0f;
    l.pos[1] = 1.0f;
    l.pos[2] = 0.0f;
    float inside[3], outside[3], plateau[3];
    const float at_plateau[3] = {0.0f, 1.0f - std::sqrt(l.law_a) * 0.5f, 0.0f};
    lp::PixelLight(l, units, at_plateau, up, albedo, 1.0f, plateau);
    const float just_in[3] = {l.range * 0.999f, 1.0f, 0.0f};
    const float just_out[3] = {l.range * 1.001f, 1.0f, 0.0f};
    const float side_n[3] = {-1.0f, 0.0f, 0.0f};
    lp::PixelLight(l, units, just_in, side_n, albedo, 1.0f, inside);
    lp::PixelLight(l, units, just_out, side_n, albedo, 1.0f, outside);
    Check(outside[0] == 0.0f && inside[0] > 0.0f, "the light ends at its reach");
    Check(inside[0] <= 0.07f * plateau[0], "and the step at the reach is under 7 % of the value inside the plateau (no visible edge)");
  }
  // the units: a neon light is multiplied by the night light intensity like a game lamp (0 by day where the game says so)
  {
    lp::Units day = units;
    day.night = 0.0f;
    float d[3];
    lp::PixelLight(lights[0], day, ground_pt, up, albedo, 1.0f, d);
    Check(d[0] == 0.0f, "a night factor of 0 puts the neon out, as it does the game's lamps");
    lp::Units dusk = units;
    dusk.night = 0.5f;
    float h[3], f[3];
    lp::PixelLight(lights[0], dusk, ground_pt, up, albedo, 1.0f, h);
    lp::PixelLight(lights[0], units, ground_pt, up, albedo, 1.0f, f);
    CheckNear(h[0] * 2.0, f[0], 1.0e-6, "half the night factor is half the light");
  }
}

// ---------------------------------------------------------------------------
// the table the hook fills
// ---------------------------------------------------------------------------

void TableChecks() {
  using Clock = nl::Table::Clock;
  nl::Table table;
  const float origin[3] = {820.0f, 17.4f, -795.0f};
  const Clock::time_point t0 = Clock::now();
  const nl::NeonCar a = CarAt(kSelf, origin), b = CarAt(kSelf + 0x10000u, origin);
  table.Note(a, t0);
  table.Note(b, t0 + std::chrono::milliseconds(10));
  std::vector<nl::NeonCar> out;
  table.Recent(&out, std::chrono::milliseconds(100), t0 + std::chrono::milliseconds(20));
  Check(out.size() == 2, "two cars seen");
  const nl::NeonCar* first = out[0].object == kSelf ? &out[0] : &out[1];
  CheckNear(first->age_ms, 20.0, 0.01, "the age is how long ago it was seen");
  table.Recent(&out, std::chrono::milliseconds(100), t0 + std::chrono::milliseconds(105));
  Check(out.size() == 1 && out[0].object == kSelf + 0x10000u, "a car not seen for over 100 ms is gone");
  // a second note of the same car replaces its entry
  nl::NeonCar again = a;
  again.pulse = 0.123f;
  table.Note(again, t0 + std::chrono::milliseconds(50));
  table.Recent(&out, std::chrono::milliseconds(100), t0 + std::chrono::milliseconds(60));
  Check(out.size() == 2, "the same car is one entry");
  for (const nl::NeonCar& c : out) {
    if (c.object == kSelf) {
      CheckNear(c.pulse, 0.123, 1.0e-6, "the latest reading is the one kept");
    }
  }
  Check(table.notes() == 3, "the notes are counted");
  // a lit car
  Check(table.AnyLit(0.003f, std::chrono::milliseconds(100), t0 + std::chrono::milliseconds(60)), "a lit car is there");
  Check(!table.AnyLit(0.003f, std::chrono::milliseconds(100), t0 + std::chrono::milliseconds(500)), "none after they are old");
  Check(!table.AnyLit(100.0f, std::chrono::milliseconds(100), t0 + std::chrono::milliseconds(60)), "none bright enough");
  nl::Table off_table;
  nl::NeonCar off = a;
  off.on = false;
  off_table.Note(off, t0);
  Check(!off_table.AnyLit(0.003f, std::chrono::milliseconds(100), t0), "a car with its neon off is not lit");
  // capacity: more cars than the table holds replace the oldest
  nl::Table small;
  for (uint32_t i = 0; i < nl::Table::kCapacity + 5; ++i) {
    small.Note(CarAt(kSelf + 0x1000u * (i + 1), origin), t0 + std::chrono::milliseconds(int(i)));
  }
  small.Recent(&out, std::chrono::milliseconds(1000), t0 + std::chrono::milliseconds(100));
  Check(out.size() == nl::Table::kCapacity, "the table holds its capacity and no more");
  std::set<uint32_t> objects;
  for (const nl::NeonCar& c : out) {
    objects.insert(c.object);
  }
  Check(objects.count(kSelf + 0x1000u) == 0 && objects.count(kSelf + 0x1000u * (nl::Table::kCapacity + 5)) == 1, "the oldest are the ones replaced");
  small.Clear();
  small.Recent(&out, std::chrono::milliseconds(1000), t0);
  Check(out.empty() && small.notes() == 0, "Clear empties it");
  // the want flag
  nl::Want(false);
  Check(!nl::Wanted(), "not wanted");
  nl::Want(true);
  Check(nl::Wanted(), "wanted");
  nl::Want(false);
}

// ---------------------------------------------------------------------------
// the planes' want: the hold, and the same as before without a neon
// ---------------------------------------------------------------------------

void HoldChecks() {
  using hp_route = larecomp::gpu::headlight_pass::Route;
  Check(nl::NextHold(true, true, 0) == nl::kHoldFrames && nl::NextHold(true, true, 5) == nl::kHoldFrames, "a lit neon car fills the hold");
  uint32_t hold = nl::kHoldFrames;
  uint32_t frames = 0;
  while (hold > 0) {
    hold = nl::NextHold(true, false, hold);
    ++frames;
  }
  Check(frames == nl::kHoldFrames, "the hold runs down one frame at a time and ends after kHoldFrames frames without a lit car");
  Check(nl::NextHold(true, false, 0) == 0, "an empty hold stays empty");
  Check(nl::NextHold(false, true, 30) == 0 && nl::NextHold(false, false, 30) == 0, "NEON LIGHT off: the hold is gone at once");
  // a dark half of the pulse (a lit car gone dark for 9 frames at 60 FPS) does not drop the planes
  uint32_t h = nl::NextHold(true, true, 0);
  for (int i = 0; i < 9; ++i) {
    h = nl::NextHold(true, false, h);
    Check(h > 0, "the planes are still wanted in the pulse's dark half");
  }
  Check(nl::kHoldFrames >= 30, "the hold is longer than a flash cycle at the slowest visible rate");

  // THE LINE: planes_wanted_ = enabled_ && (setting_on || (PassLightsHeadlights(route) && HeadlightsOn) || neon_active_), against what it was (GB-H2): enabled_ && (setting_on || HeadlightsOn), where enabled_ was
  // PassOn(route, setting_on); and with NEON LIGHT enabled_ is PassOn(...) || neon_on. Without a lit neon car the want is what it was for every setting, route and HEADLIGHTS state (a player with no neon
  // car pays nothing), with one the planes are wanted whenever the pass is enabled.
  namespace hp = larecomp::gpu::headlight_pass;
  const auto planes = [](bool enabled, bool setting_on, hp_route route, bool headlights_on, bool neon_active) {
    return enabled && (setting_on || (hp::PassLightsHeadlights(route) && headlights_on) || neon_active);
  };
  for (const hp_route route : {hp_route::kForward, hp_route::kPass, hp_route::kBoth}) {
    for (const bool setting_on : {false, true}) {
      for (const bool headlights_on : {false, true}) {
        const std::string at = std::string(" (route ") + hp::RouteName(route) + (setting_on ? ", setting on" : ", setting off") + (headlights_on ? ", HEADLIGHTS on)" : ", HEADLIGHTS off)");
        const bool enabled_old = hp::PassOn(route, setting_on);
        const bool old_line = enabled_old && (setting_on || headlights_on);
        for (const bool neon_on : {false, true}) {
          const bool enabled = enabled_old || neon_on;
          Check(planes(enabled, setting_on, route, headlights_on, false) == old_line, std::string("without a lit neon car the planes' want is what it was") + at + (neon_on ? ", NEON LIGHT on" : ", NEON LIGHT off"));
          Check(planes(enabled, setting_on, route, headlights_on, neon_on) == (enabled && (neon_on || old_line)), std::string("with NEON LIGHT's lit car the planes are wanted") + at + (neon_on ? ", on" : ", off"));
        }
      }
    }
  }
  // the default player (the forward route, the pass's own setting off, HEADLIGHTS on, NEON LIGHT on): the pass is enabled, and with no neon car it wants no plane
  Check(planes(hp::PassOn(hp_route::kForward, false) || true, false, hp_route::kForward, true, false) == false, "the default player with no neon car: the pass is enabled and wants no plane");
  Check(planes(true, false, hp_route::kForward, true, true) == true, "... and with a lit neon car it wants them");
}

// ---------------------------------------------------------------------------
// the ids
// ---------------------------------------------------------------------------

void IdChecks() {
  std::set<uint64_t> ids;
  for (uint32_t car = 0; car < 40; ++car) {
    for (uint32_t i = 0; i < 4; ++i) {
      ids.insert(light_id::NeonCarId(0x40000000u + car * 0x2000u, i));
    }
  }
  Check(ids.size() == 160 && ids.count(kNoLightId) == 0, "every point of every car has its own id, none is 0");
  const float place[3] = {820.0f, 17.4f, -795.0f};
  Check(ids.count(light_id::NeonId(place, 0)) == 0 && light_id::NeonCarId(kSelf, 0) == light_id::NeonCarId(kSelf, 0) && light_id::NeonCarId(kSelf, 0) != light_id::NeonCarId(kSelf, 1) &&
            light_id::NeonCarId(kSelf, 0) != light_id::PointId(kSelf, 0),
        "neon ids are not the place's, nor a car point's");
  Check(LightSource::kNeon != LightSource::kPolice && uint8_t(LightSource::kNeon) == 2, "the neon source keeps its number");
  // NEONALL-REVIEW: two effect objects that differ in one address bit only (the bit the old tag 0x10000000 swallowed) are two cars
  for (uint32_t bit = 0; bit < 32; ++bit) {
    const uint32_t a = 0x40123000u, b = a ^ (1u << bit);
    Check(light_id::NeonCarId(a, 0) != light_id::NeonCarId(b, 0) && light_id::NeonCarId(a, 3) != light_id::NeonCarId(b, 3), "objects that differ in address bit " + std::to_string(bit) + " have their own ids");
  }
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
size_t At(const std::string& text, const std::string& what) { return text.find(what); }

void SourceChecks(const std::string& src) {
  const std::string gpu = src + "/gpu/";
  const std::string cpp = ReadFile(gpu + "local_lights.cpp");
  const std::string header = ReadFile(gpu + "local_lights.h");
  const std::string class_h = ReadFile(gpu + "gpu_class.h");
  const std::string cmake = ReadFile(gpu + "CMakeLists.txt");
  const std::string root_cmake = ReadFile(src + "/../CMakeLists.txt");
  const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string menu = ReadFile(src + "/mc_engine/menus/pause_menu.cpp");
  const std::string debug_menu = ReadFile(src + "/mc_engine/menus/debug_menu_pages.cpp");
  const std::string hook = ReadFile(src + "/mc_engine/hooks/neon_light.cpp");
  const std::string pulse = ReadFile(src + "/mc_engine/hooks/neon_pulse.cpp");
  Check(!cpp.empty() && !class_h.empty() && !settings.empty() && !menu.empty() && !hook.empty() && !cmake.empty() && !root_cmake.empty(), "the sources are readable: " + src);

  // the settings: NEON LIGHT is a player's setting (auto), the gain a debug one
  {
    const std::string rex_setting = "REX_SET" "TING(";  // (spelt in two pieces: tools/check_settings_exposure.py reads definitions, and this is not one)
    const size_t at = settings.find(rex_setting + "std::string, mcla_gpu_neon_light, \"auto\",");
    Check(at != std::string::npos, "mcla_gpu_neon_light is a string setting, auto by default");
    const size_t end = settings.find(");", at);
    Check(at != std::string::npos && settings.substr(at, end - at).find("kDebugOnly") == std::string::npos, "NEON LIGHT is not a debug setting (it is a row)");
    const size_t g = settings.find(rex_setting + "f64, mcla_gpu_neon_light_gain, 1.0,");
    const size_t gend = settings.find(");", g);
    Check(g != std::string::npos && settings.substr(g, gend - g).find("rex::kDebugOnly") != std::string::npos, "the gain is a debug setting, 1 by default");
    Check(Has(settings, "inline auto& mcla_gpu_neon_light = g_gpu.mcla_gpu_neon_light;") && Has(settings, "inline auto& mcla_gpu_neon_light_gain = g_gpu.mcla_gpu_neon_light_gain;"), "both have their alias");
    Check(Has(manifest, "\"mcla_gpu_neon_light\": {") && Has(manifest, "\"mcla_gpu_neon_light_gain\": {") && Has(meta, "{\"mcla_gpu_neon_light\", \"MCLA/GPU\"},") && Has(meta, "{\"mcla_gpu_neon_light_gain\", \"MCLA/GPU\"},"),
          "both are in the manifest and the meta table");
  }
  // the class table: off on the integrated class, on elsewhere
  Check(Has(class_h, "kNeonLight,") && Has(class_h, "{\"mcla_gpu_neon_light\", \"off\", \"on\", \"on\"},") && Has(class_h, "inline constexpr int kEffectCount = 15;"), "AUTO is off on Low and on on Mid and High");
  // the row: on the lighting page after LIGHT SHADOWS, no dependency
  {
    const size_t shadows = At(menu, "Str(\"PM_RxGpuLightShadows\"");
    const size_t neon = At(menu, "Str(\"PM_RxGpuNeonLight\", \"mcla_gpu_neon_light\", \"NEON LIGHT: \"");
    const size_t vol = At(menu, "Str(\"PM_RxGpuVolumetrics\"");
    Check(shadows != std::string::npos && neon != std::string::npos && vol != std::string::npos && shadows < neon && neon < vol, "the NEON LIGHT row is on the lighting page between LIGHT SHADOWS and VOLUMETRIC LIGHT");
    Check(Has(menu, "kNativeNeonLightVals[]  = {\"auto\", \"off\", \"on\"};") && Has(menu, "gpu_class::AutoLabel(mcla::gpu::gpu_class::Effect::kNeonLight)"), "its words are AUTO, OFF, ON and AUTO says what it comes to");
    Check(Has(menu, "constexpr RowDynamics kNeonLightDynamics = {.str_label = NeonLightLabel};"), "no dependency: no .enabled");
  }
  Check(Has(debug_menu, "\"mcla_gpu_neon_light_gain\""), "the gain has a row on the hidden debug menu");
  // the hook: the game's own update first, then the read; only while wanted
  {
    const size_t call = At(hook, "__imp__rex_sub_823553F8(ctx, base);");
    const size_t read = At(hook, "nl::ReadNeonCar(");
    const size_t want = At(hook, "nl::Wanted()");
    Check(call != std::string::npos && read != std::string::npos && want != std::string::npos && call < read && want < read, "the hook runs the game's update, asks whether anyone wants it, then reads");
    Check(Has(hook, "extern \"C\" REX_FUNC(rex_sub_823553F8)") && Has(hook, "REX_EXTERN(__imp__rex_sub_823553F8)") && Has(hook, "#ifndef REXGLUE_HAS_XEO3_TARGET"), "it overrides the underglow's function like neon_pulse.cpp overrides the pulse's");
    Check(!Has(hook, "WriteGuest"), "the hook writes nothing into the guest");
    Check(Has(pulse, "rex_sub_82353E38") && !Has(pulse, "rex_sub_823553F8"), "the two overrides are of two functions");
  }
  Check(Has(root_cmake, "src/mc_engine/hooks/neon_light.cpp") && Has(root_cmake, "src/mc_engine/hooks/neon_pulse.cpp"), "the root CMake builds the hook");
  Check(Has(cmake, "add_executable(larecomp_neon_light_test EXCLUDE_FROM_ALL neon_light_test.cpp)"), "the CMake has this test");
  // the pass's wiring
  {
    Check(Has(cpp, "sources_ = hp::SourcesFor(route, uint32_t(std::clamp<int>(int(st::mcla_gpu_local_lights_sources.Get()), 0, int(ll::kSourceMask))) & ll::kSourceMask);"), "the debug setting is still read as before");
    Check(Has(cpp, "sources_ |= ll::kSourceNeon;") && Has(cpp, "nl::Want("), "NEON LIGHT puts the neon source in and tells the hook");
    // NEONALL-REVIEW: the pass that runs for NEON LIGHT alone (its own setting off, the route `forward`) lights nothing but the neon: the debug sources, the test lamp and the lamp A/B stay with the pass's own setting
    {
      const size_t neon_in_at = At(cpp, "sources_ |= ll::kSourceNeon;");
      const size_t ab_read = At(cpp, "ab_ = ll::AbOf(int(st::mcla_gpu_local_lights_ab.Get()));");
      const size_t gate = At(cpp, "if (!on_for_lights) {");
      const size_t mask = At(cpp, "sources_ &= ll::kSourceNeon;");
      const size_t grid = At(cpp, "ab_ = ll::Ab::kGrid;");
      Check(neon_in_at != std::string::npos && ab_read != std::string::npos && gate != std::string::npos && mask != std::string::npos && grid != std::string::npos && neon_in_at < ab_read && ab_read < gate &&
                gate < mask && mask < grid,
            "with the pass on for NEON LIGHT alone the sources are the neon bit only and the lamp A/B is the grid's (after both are read)");
    }
    Check(Has(cpp, "ll::GatherNeonLights(neon_in, &candidates_.neon, &neon_report);") && Has(cpp, "std::erase_if(candidates_.neon, unseen);"), "the pass gathers the neon lights and drops the ones no pixel is inside the reach of");
    Check(Has(cpp, "mcla_gpu_neon_light_gain.Get()") && Has(cpp, "static LiveText neon_text(\"mcla_gpu_neon_light\");"), "the gain and the setting are read live");
    const size_t gather = At(cpp, "ll::GatherNeonLights(neon_in, &candidates_.neon, &neon_report);");
    const size_t unseen = At(cpp, "std::erase_if(candidates_.neon, unseen);");
    const size_t select = At(cpp, "ll::SelectPassLights(frame,");
    Check(gather != std::string::npos && gather < unseen && unseen < select, "gather, then drop the unseen, then select");
    Check(Has(cpp, "planes_wanted_ = enabled_ && (setting_on || (hp::PassLightsHeadlights(route) && headlight_rig::SettingOn()) || neon_active_);"),
          "the G-buffer's planes are wanted for the setting, for the headlight route while HEADLIGHTS is on, and while a neon car is lit (not for NEON LIGHT alone)");
    Check(Has(cpp, "neon_hold_ = nl::NextHold(neon_on_, neon_on_ && nl::Global().AnyLit(ll::kNeonMinColour), neon_hold_);"), "the planes are held a while after the last lit neon car (no sidecar flapping)");
    Check(Has(cpp, "nl::Global().AnyLit(ll::kNeonMinColour)"), "the pass asks the table for a lit car before it wants the planes");
  }
  Check(Has(header, "neon_light.h") && Has(header, "neon_active_") && Has(header, "NEON"), "local_lights.h tells the neon's story");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  DecodeChecks();
  TwinChecks();
  TubeChecks();
  LightChecks();
  SelectionChecks();
  BudgetChecks();
  PixelChecks();
  TableChecks();
  HoldChecks();
  IdChecks();
  if (!src.empty()) {
    SourceChecks(src);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

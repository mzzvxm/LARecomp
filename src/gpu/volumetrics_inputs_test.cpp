// Check of the volumetric light's light inputs: the game's lamp record and the table the guest hook fills (vol_lamp_list.h), the game's lamp law, its units in the
// volume and the calibration against the lit road (volumetrics_lamps.h), the pick with its hysteresis and fades, the headlights' units and the shadow atlas's
// state (volumetrics_headlights.h, with the real headers of feat/headlights when they are in the tree and their field-for-field mirrors when not), the
// tile-clamped shadow lookup's twin (volumetrics_rules.h ShadowShare), and, given the src/gpu folder, that the hook, its config entry and the shader's offsets
// say what the headers say. A program of its own: no renderer, no ReXGlue, no GPU; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_volumetrics_inputs_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_volumetrics_inputs_test [<src/gpu dir>]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "vol_lamp_list.h"
#include "volumetrics_headlights.h"
#include "volumetrics_lamps.h"
#include "volumetrics_rules.h"
#include "volumetrics_types.h"

// The rig's and the replay's own headers when the tree has them (feat/headlights), else mirrors with the same fields.
#if __has_include("headlight_types.h") && __has_include("local_shadow_types.h")
#include "headlight_types.h"
#include "local_shadow_types.h"
#define TEST_REAL_HEADERS 1
using TestHeadlight = mcla::gpu::Headlight;
using TestHeadlightSet = mcla::gpu::HeadlightSet;
using TestView = larecomp::gpu::LocalShadowView;
using TestFrame = larecomp::gpu::LocalShadowFrame;
#else
#define TEST_REAL_HEADERS 0
struct TestHeadlight {
  float pos[3];
  float dir[3];
  float cos_outer;
  float cos_inner;
  float range;
  float color[3];
  uint32_t car_key;
  bool is_player;
};
struct TestHeadlightSet {
  std::array<TestHeadlight, 4> lights;
  uint32_t count = 0;
};
struct TestView {
  float world_to_shadow[16];
  float atlas_rect[4];
  float depth_bias;
  float texel_size;
  uint32_t light_index;
  uint32_t valid;
};
struct TestFrame {
  std::array<TestView, 8> views{};
  uint32_t count = 0;
};
#endif
#if __has_include("headlight_rig_rules.h")
#include "headlight_rig_rules.h"
#define TEST_RULES_HEADER 1
#else
#define TEST_RULES_HEADER 0
#endif

using namespace mcla::gpu::volumetrics;
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
void CheckNear(double got, double want, double eps, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= eps)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.6f, want %.6f\n", what.c_str(), got, want);
  }
}

constexpr double kDeg = 3.14159265358979323846 / 180.0;

// --- a record as the guest holds it: 128 big-endian bytes -------------------------------------------------------------------------------------------

void PutF(uint8_t* p, float v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  p[0] = uint8_t(b >> 24);
  p[1] = uint8_t(b >> 16);
  p[2] = uint8_t(b >> 8);
  p[3] = uint8_t(b);
}

// The first record of the night probe's log (a Los Angeles street lamp: intensity 100, cone 125, spread -50.5, warm, pointing down).
void MakeRecord(uint8_t* rec, const float pos[3], const float col[3], const float dir[3], float intensity, float cone, float spread, uint8_t type, uint8_t on, float scale) {
  std::memset(rec, 0, vl::kRecordBytes);
  PutF(rec + 0, 9.6f);  // the distance to the eye
  uint8_t* light = rec + vl::kRecordLight;
  for (int i = 0; i < 3; ++i) {
    PutF(light + vl::kLightPosition + 4 * i, pos[i]);
    PutF(light + vl::kLightColour + 4 * i, col[i]);
    PutF(light + vl::kLightDirection + 4 * i, dir[i]);
  }
  PutF(light + vl::kLightIntensity, intensity);
  PutF(light + vl::kLightCone, cone);
  PutF(light + vl::kLightSpread, spread);
  light[vl::kLightType] = type;
  light[vl::kLightOn] = on;
  PutF(rec + vl::kRecordScale, scale);
}

void CheckRecord() {
  const float pos[3] = {-2932.67f, -13.32f, 377.97f};
  const float col[3] = {0.80f, 0.60f, 0.40f};
  const float dir[3] = {0.13f, -0.99f, -0.07f};
  uint8_t rec[vl::kRecordBytes];
  MakeRecord(rec, pos, col, dir, 100.0f, 125.0f, -50.5f, 1, 1, 1.0f);
  const vl::GameLamp l = vl::DecodeRecord(rec);
  CheckNear(l.pos[0], -2932.67, 1e-3, "the record's position x");
  CheckNear(l.pos[1], -13.32, 1e-3, "the record's position y");
  CheckNear(l.pos[2], 377.97, 1e-3, "the record's position z");
  CheckNear(l.colour[1], 0.60, 1e-6, "the record's colour");
  CheckNear(l.dir[1], -0.99, 1e-6, "the record's axis points down");
  CheckNear(l.intensity, 100.0, 1e-6, "the record's intensity");
  CheckNear(l.cone_deg, 125.0, 1e-6, "the record's cone angle");
  CheckNear(l.spread_deg, -50.5, 1e-6, "the record's spread");
  Check(l.type == 1 && l.on == 1, "spot, on");
  CheckNear(l.scale, 1.0, 1e-6, "the record's scale");
  Check(vl::Plausible(l), "a real record is plausible");
  // The layout the header names: a record is 128 bytes, the light starts at 16 and the scale at 112.
  Check(vl::kRecordBytes == 128 && vl::kRecordLight == 16 && vl::kRecordScale == 112, "the record's layout");
  Check(vl::kLightIntensity == 64 && vl::kLightCone == 68 && vl::kLightSpread == 72 && vl::kLightType == 88 && vl::kLightOn == 89, "the light's layout");
  // Garbage is left out.
  vl::GameLamp bad = l;
  bad.pos[1] = std::nanf("");
  Check(!vl::Plausible(bad), "a NaN is not plausible");
  bad = l;
  bad.intensity = 1.0e9f;
  Check(!vl::Plausible(bad), "an absurd intensity is not plausible");
}

void CheckTable() {
  using Clock = vl::Table::Clock;
  vl::Table table;
  std::vector<vl::GameLamp> out;
  uint64_t seen = 0;
  bool stale = false;
  const auto t0 = Clock::now();
  Check(!table.Latest(&out, &seen, &stale, std::chrono::milliseconds(500), t0) && stale, "nothing published: stale");
  vl::GameLamp a;
  a.intensity = 55.0f;
  std::vector<vl::GameLamp> three(3, a);
  table.Publish(three.data(), 3, t0);
  Check(table.Latest(&out, &seen, &stale, std::chrono::milliseconds(500), t0 + std::chrono::milliseconds(100)) && !stale && out.size() == 3 && seen == 1,
        "a fresh list is taken once");
  Check(!table.Latest(&out, &seen, &stale, std::chrono::milliseconds(500), t0 + std::chrono::milliseconds(120)) && !stale, "the same list is not taken twice and is not stale");
  Check(!table.Latest(&out, &seen, &stale, std::chrono::milliseconds(500), t0 + std::chrono::milliseconds(900)) && stale, "a list older than the limit is stale");
  table.Publish(nullptr, 0, t0 + std::chrono::milliseconds(1000));  // an empty list is a list (a day, a menu)
  Check(table.Latest(&out, &seen, &stale, std::chrono::milliseconds(500), t0 + std::chrono::milliseconds(1010)) && !stale && out.empty() && seen == 2,
        "an empty list is taken and is not stale");
  std::vector<vl::GameLamp> many(400, a);
  table.Publish(many.data(), 400, t0 + std::chrono::milliseconds(1100));
  table.Latest(&out, &seen, &stale, std::chrono::milliseconds(500), t0 + std::chrono::milliseconds(1110));
  Check(out.size() == vl::kMaxRecords, "the list is cut at the table's capacity");
  Check(table.published() == 3, "three lists were published");
  bool garage = false;
  table.Publish(three.data(), 3, t0 + std::chrono::milliseconds(1200), true);
  Check(table.Latest(&out, &seen, &stale, std::chrono::milliseconds(500), t0 + std::chrono::milliseconds(1210), &garage) &&
            garage && !stale && out.size() == 3, "the list and garage tag arrive in the same snapshot");
  table.Publish(nullptr, 0, t0 + std::chrono::milliseconds(1300));
  Check(table.Latest(&out, &seen, &stale, std::chrono::milliseconds(500), t0 + std::chrono::milliseconds(1310), &garage) &&
            !garage && !stale && out.empty(), "the next city snapshot replaces the garage tag even with no lamps");
  // the hook's switch
  vl::SetWanted(false);
  Check(!vl::Wanted(), "the hook is off until somebody wants it");
  vl::SetWanted(true);
  Check(vl::Wanted(), "and on when somebody does");
  vl::SetWanted(false);
}

// --- the law ---------------------------------------------------------------------------------------------------------------------------------

vl::GameLamp StreetLamp(float x, float z, float intensity = 55.0f, float h = 6.0f) {
  vl::GameLamp l;
  l.pos[0] = x;
  l.pos[1] = h;
  l.pos[2] = z;
  l.colour[0] = 1.0f;
  l.colour[1] = 0.80f;
  l.colour[2] = 0.55f;
  l.dir[0] = 0.0f;
  l.dir[1] = -1.0f;
  l.dir[2] = 0.0f;
  l.intensity = intensity;
  l.cone_deg = 125.0f;
  l.spread_deg = -50.5f;
  l.scale = 1.0f;
  l.type = 1;
  l.on = 1;
  return l;
}

void CheckLaw() {
  // The game's term: full inside sqrt(I), I / d^2 beyond, 1 per cent at the reach sqrt(100 I) (the mod loader's "20 reaches 44.7 m").
  CheckNear(GameLampTerm(55.0f, 0.0f), 1.0, 1e-9, "the game's term is 1 at the lamp");
  CheckNear(GameLampTerm(55.0f, 7.0f), 1.0, 1e-9, "and inside the plateau (sqrt(55) = 7.4 m)");
  CheckNear(GameLampTerm(55.0f, 14.83f), 0.25, 2e-3, "twice the plateau's radius: a quarter");
  CheckNear(GameReach(20.0f), 44.72, 0.01, "a lamp of 20 reaches 44.7 m (mod_lights.cpp)");
  CheckNear(GameLampTerm(20.0f, GameReach(20.0f)), 0.01, 1e-5, "at the reach the game's term is 1 per cent");
  CheckNear(GameLampTerm(0.0f, 3.0f), 0.0, 1e-9, "no intensity, no light");

  // The cone (sub_822FD2C8): half angles of the angle and of the spread; a negative spread puts the soft edge inside the angle.
  float co, ci;
  ConeCosines(125.0f, -50.5f, &co, &ci);
  CheckNear(std::acos(ci) / kDeg, 37.25, 1e-3, "a street lamp (125, -50.5) is full inside 37.25 degrees");
  CheckNear(std::acos(co) / kDeg, 62.5, 1e-3, "and gone at 62.5");
  ConeCosines(125.0f, 72.0f, &co, &ci);
  CheckNear(std::acos(ci) / kDeg, 62.5, 1e-3, "a positive spread of 72 starts at the angle's half, 62.5");
  CheckNear(std::acos(co) / kDeg, 98.5, 1e-3, "and ends 36 degrees later");
  ConeCosines(90.0f, 0.0f, &co, &ci);
  CheckNear(std::acos(co) / kDeg - std::acos(ci) / kDeg, 0.5, 1e-3, "a spread of 0 counts as 1: a half degree each side");
  Check(ci > co, "full is nearer the axis than gone");

  // MakeVolLamp: units.
  const vl::GameLamp g = StreetLamp(10.0f, 20.0f, 55.0f);
  const VolLamp v = MakeVolLamp(g);
  CheckNear(v.colour[0], 55.0, 1e-4, "the colour carries the intensity: 1.0 x 55");
  CheckNear(v.colour[1], 44.0, 1e-4, "0.8 x 55");
  CheckNear(v.colour[2], 30.25, 1e-4, "0.55 x 55");
  CheckNear(v.range, std::sqrt(5500.0), 1e-3, "the range is the reach sqrt(100 I)");
  CheckNear(LampNearSq(v), kLampNearShare * 55.0, 1e-4, "the near radius squared is the share of the intensity");
  CheckNear(v.dir[1], -1.0, 1e-6, "the axis is the game's, down");
  Check(v.shadow_view == -1, "a lamp has no shadow view");
  CheckNear(v.pos[0], 10.0, 1e-6, "the position is the game's");
  vl::GameLamp faded = g;
  faded.scale = 0.5f;
  CheckNear(MakeVolLamp(faded).colour[0], 27.5, 1e-4, "the prop pass's fade scales the colour");
  CheckNear(MakeVolLamp(g, 2.0f).colour[0], 110.0, 1e-4, "units scale it");
  vl::GameLamp weak = g;
  weak.intensity = 0.3f;
  CheckNear(MakeVolLamp(weak).range, kMinReach, 1e-6, "the reach has a floor");
  vl::GameLamp strong = g;
  strong.intensity = 400.0f;
  CheckNear(MakeVolLamp(strong).range, kMaxReach, 1e-6, "and a ceiling");
  vl::GameLamp omni = g;
  omni.type = 0;
  Check(MakeVolLamp(omni).cos_outer <= -0.999f, "an omni lamp is a point light");
  vl::GameLamp full = g;
  full.cone_deg = 360.0f;
  Check(MakeVolLamp(full).cos_outer <= -0.999f, "an angle of 360 is a point light");
  vl::GameLamp lopsided = g;
  lopsided.dir[0] = 3.0f;
  lopsided.dir[1] = -4.0f;
  const VolLamp lv = MakeVolLamp(lopsided);
  CheckNear(lv.dir[0] * lv.dir[0] + lv.dir[1] * lv.dir[1] + lv.dir[2] * lv.dir[2], 1.0, 1e-5, "the axis is normalised");
  vl::GameLamp none = g;
  none.dir[0] = none.dir[1] = none.dir[2] = 0.0f;
  CheckNear(MakeVolLamp(none).dir[1], -1.0, 1e-6, "a lamp with no axis points down");
  // The padding the near radius uses is the shader's: the fourth row's second float.
  Check(offsetof(VolLamp, shadow_view) == 48 && offsetof(VolLamp, pad) == 52 && offsetof(VolLamp, shadow_view_b) == 56 && sizeof(VolLamp) == 64,
        "VolLamp: shadow_view at 48, the near radius squared (pad[0]) at 52, the second shadow view at 56");
}

// The calibration: the volume's lamp term against the game's, for every intensity the city has.
void CheckCalibration() {
  const float intensities[] = {20.0f, 25.0f, 30.0f, 40.0f, 55.0f, 60.0f, 65.0f, 70.0f, 100.0f};
  for (const float intensity : intensities) {
    vl::GameLamp g = StreetLamp(0.0f, 0.0f, intensity);
    g.colour[0] = g.colour[1] = g.colour[2] = 1.0f;
    const VolLamp v = MakeVolLamp(g);
    // In the units of the lamp's colour: the volume's term is I x falloff, the game's is min(1, I / d^2).
    double lo = 1e9;
    double hi = 0.0;
    const double reach_part = 0.35 * double(v.range);  // the window is still above 0.88 there
    for (double d = 0.0; d <= reach_part; d += 0.25) {
      const double vol = double(intensity) * double(LampFalloff(float(d), v.range, LampNearSq(v)));
      const double game = double(GameLampTerm(intensity, float(d)));
      const double ratio = vol / game;
      lo = std::min(lo, ratio);
      hi = std::max(hi, ratio);
    }
    // The largest ratio is at the core (1 / share), the smallest at the plateau's edge (1 / (1 + share)); both inside the window's few per cent.
    CheckNear(hi, 1.0 / double(kLampNearShare), 0.01, "the volume's core is 1 / share times the game's pool (I " + std::to_string(intensity) + ")");
    CheckNear(lo, 1.0 / (1.0 + double(kLampNearShare)), 0.03, "and its dimmest point (the plateau's edge) 1 / (1 + share) of it (I " + std::to_string(intensity) + ")");
    // Beyond the plateau the volume IS the game's inverse square law, up to the near radius: the ratio of two distances is the game's.
    const double d1 = 3.0 * std::sqrt(double(intensity));
    const double d2 = 4.5 * std::sqrt(double(intensity));
    if (d2 < 0.35 * double(v.range)) {
      const double vol_ratio = double(LampFalloff(float(d1), v.range, LampNearSq(v))) / double(LampFalloff(float(d2), v.range, LampNearSq(v)));
      const double game_ratio = double(GameLampTerm(intensity, float(d1))) / double(GameLampTerm(intensity, float(d2)));
      CheckNear(vol_ratio / game_ratio, 1.0, 0.1, "beyond the plateau two distances dim in the game's ratio (I " + std::to_string(intensity) + ")");
    }
    // Two lamps: the brighter one is brighter by the game's ratio of intensities at the same distance beyond both plateaux.
  }
  // The share is the one that balances the largest and the smallest ratio: a grid of shares has its best at 0.618.
  double best_share = 0.0;
  double best_error = 1e9;
  for (double share = 0.2; share <= 1.2; share += 0.002) {
    const double error = std::max(1.0 / share, 1.0 + share);  // the core's ratio against the edge's inverse
    if (error < best_error) {
      best_error = error;
      best_share = share;
    }
  }
  CheckNear(best_share, double(kLampNearShare), 0.01, "0.618 minimises the largest ratio between the volume's term and the game's");
  CheckNear(best_error, 1.618, 0.01, "which is 1.618");

  // The gain: pi from the game's units to the scattered light, and the shader's 60 is 19.1 times thicker than the haze says.
  CheckNear(kPhysicalGain, 3.14159, 1e-4, "the physical gain is pi");
  CheckNear(kVisibilityBoost, 60.0 / 3.14159265, 1e-3, "the visibility boost is the shader's gain over pi");
  CheckNear(kLightGain, 60.0, 1e-9, "the shader's gain is still 60 (volumetrics_rules.h)");
  // What it comes to: a 10 m column of a clear night's air (sigma 0.0012) at the plateau's edge of a 55 lamp, seen with an even phase function (1 / 4 pi), over a road of
  // albedo 0.15 in the same light.
  vl::GameLamp g = StreetLamp(0.0f, 0.0f, 55.0f);
  g.colour[0] = g.colour[1] = g.colour[2] = 1.0f;
  const VolLamp v = MakeVolLamp(g);
  const float edge = std::sqrt(55.0f);
  const float even = 1.0f / (4.0f * kPi);
  const double boosted = HaloToPoolRatio(v, 55.0f, edge, 10.0f, 0.0012f, even, 0.15f);
  const double physical = HaloToPoolRatio(v, 55.0f, edge, 10.0f, 0.0012f, even, 0.15f, kPhysicalGain / kLightGain);
  Check(boosted > 0.1 && boosted < 0.5, "the shader's gain makes a lamp's air column a quarter of the road's brightness (the look: visible, not a lamp)");
  Check(physical > 0.005 && physical < 0.02, "the physical gain makes it a hundredth: nobody sees a lamp in clear air");
  CheckNear(boosted / physical, double(kVisibilityBoost), 1e-3, "and they differ by the visibility boost");
  Check(HaloToPoolRatio(v, 55.0f, edge, 10.0f, 0.0024f, even, 0.15f) > 1.99 * boosted, "twice the haze, twice the halo (rain)");
  Check(HaloToPoolRatio(v, 55.0f, edge, 10.0f, 0.0012f, even, 0.15f, 2.0f) > 1.99 * boosted, "the debug strength is a plain multiplier");
}

// --- the pick --------------------------------------------------------------------------------------------------------------------------------

VolCamera Street() {
  VolCamera c;  // at the origin, looking along -z (the default), 90 degrees wide
  c.pos[1] = 1.4f;
  c.tan_x = 1.5f;
  c.tan_y = 0.84f;
  return c;
}

float WeightOf(const PickResult& r, const vl::GameLamp& lamp) {
  const VolLamp full = MakeVolLamp(lamp);
  for (uint32_t i = 0; i < r.count; ++i) {
    if (std::fabs(r.lamps[i].pos[0] - lamp.pos[0]) < 0.01f && std::fabs(r.lamps[i].pos[2] - lamp.pos[2]) < 0.01f) {
      return full.colour[0] > 0.0f ? r.lamps[i].colour[0] / full.colour[0] : 0.0f;
    }
  }
  return -1.0f;
}

void CheckFrustum() {
  const VolCamera c = Street();
  const float ahead[3] = {0.0f, 6.0f, -20.0f};
  const float behind[3] = {0.0f, 6.0f, 40.0f};
  const float behind_20[3] = {0.0f, 6.0f, 20.0f};
  const float behind_close[3] = {0.0f, 6.0f, 3.0f};
  const float far_side[3] = {200.0f, 6.0f, -20.0f};
  CheckNear(FrustumFactor(c, ahead, 30.0f, 0.5f, 160.0f), 1.0, 1e-6, "a lamp ahead is in");
  CheckNear(FrustumFactor(c, behind, 30.0f, 0.5f, 160.0f), 0.0, 1e-6, "a lamp 40 m behind is out (its influence does not touch the frustum)");
  const float middle = FrustumFactor(c, behind_20, 30.0f, 0.5f, 160.0f);
  Check(middle > 0.2f && middle < 0.5f, "a lamp 20 m behind still lights a little of the air ahead: " + std::to_string(middle));
  const float partial = FrustumFactor(c, behind_close, 30.0f, 0.5f, 160.0f);
  Check(partial > 0.5f && partial < 1.0f, "a lamp just behind still lights the air ahead: partly in");
  CheckNear(FrustumFactor(c, far_side, 30.0f, 0.5f, 160.0f), 0.0, 1e-6, "a lamp far to the side is out");
  const float beyond[3] = {0.0f, 6.0f, -190.0f};
  CheckNear(FrustumFactor(c, beyond, 30.0f, 0.5f, 160.0f), 0.0, 1e-6, "a lamp past the far plane and its radius is out");
  // Score: nearer is brighter, off is nothing, outside is nothing, a stronger lamp is brighter.
  const vl::GameLamp near_lamp = StreetLamp(0.0f, -10.0f);
  const vl::GameLamp far_lamp = StreetLamp(0.0f, -60.0f);
  Check(LampScore(near_lamp, c, 0.5f, 160.0f) > LampScore(far_lamp, c, 0.5f, 160.0f), "a nearer lamp scores more");
  vl::GameLamp off = near_lamp;
  off.scale = 0.0f;
  CheckNear(LampScore(off, c, 0.5f, 160.0f), 0.0, 1e-9, "a faded out lamp scores nothing");
  vl::GameLamp dark = near_lamp;
  dark.intensity = 0.0f;
  CheckNear(LampScore(dark, c, 0.5f, 160.0f), 0.0, 1e-9, "no intensity, no score");
  CheckNear(LampScore(StreetLamp(0.0f, 60.0f), c, 0.5f, 160.0f), 0.0, 1e-9, "a lamp behind the camera and out of reach scores nothing");
  Check(LampScore(StreetLamp(0.0f, -10.0f, 100.0f), c, 0.5f, 160.0f) > LampScore(StreetLamp(0.0f, -10.0f, 30.0f), c, 0.5f, 160.0f), "a stronger lamp scores more");
  // A lamp's key is its place.
  Check(LampKey(StreetLamp(1.0f, 2.0f)) == LampKey(StreetLamp(1.02f, 2.02f)), "the same lamp to an eighth of a metre has the same key");
  Check(LampKey(StreetLamp(1.0f, 2.0f)) != LampKey(StreetLamp(1.0f, 12.0f)), "and another lamp another");
}

std::vector<vl::GameLamp> Row(int count, float spacing, float first_z = -5.0f) {
  std::vector<vl::GameLamp> row;
  for (int i = 0; i < count; ++i) {
    row.push_back(StreetLamp(((i & 1) != 0 ? 6.0f : -6.0f), first_z - spacing * float(i)));  // both kerbs, along the street ahead
  }
  return row;
}

void CheckPick() {
  Check(LampPicker::SpareSlots(12) == 2 && LampPicker::SpareSlots(32) == 5 && LampPicker::SpareSlots(48) == 8, "a sixth of the budget is kept for the lamps that fade");
  Check(LampPicker::SpareSlots(1) == 0 && LampPicker::SpareSlots(2) == 1 && LampPicker::SpareSlots(0) == 0, "tiny budgets");

  const VolCamera cam = Street();
  const std::vector<vl::GameLamp> row = Row(60, 12.0f);  // 60 lamps, 720 m of street ahead
  for (const uint32_t budget : {12u, 32u, 48u}) {
    LampPicker picker;
    PickParams p;
    p.budget = budget;
    p.dt = 0.0f;
    PickResult r;
    picker.Update(row, cam, p, &r);
    Check(r.count <= budget, "the pick keeps to the budget " + std::to_string(budget));
    Check(r.wanted == budget - LampPicker::SpareSlots(budget) || r.wanted == r.usable, "it picks the budget less the spare slots " + std::to_string(budget));
    // The picked ones are the nearest: every lamp nearer than the farthest picked is picked.
    float farthest = 0.0f;
    for (uint32_t i = 0; i < r.count; ++i) {
      farthest = std::max(farthest, -r.lamps[i].pos[2]);
    }
    uint32_t nearer = 0;
    for (const vl::GameLamp& l : row) {
      if (-l.pos[2] < farthest - 0.01f) {
        ++nearer;
        Check(WeightOf(r, l) >= 0.0f, "a lamp nearer than the farthest picked is picked");
      }
    }
    Check(nearer + 1 >= r.count || r.count == r.usable, "the picks are the contiguous nearest");
    // The first pick has no fade: every picked lamp is at full weight.
    for (const vl::GameLamp& l : row) {
      const float w = WeightOf(r, l);
      Check(w < 0.0f || std::fabs(w - 1.0f) < 1e-5f, "the first pick starts at full weight");
    }
  }
  {
    LampPicker picker;
    PickParams p;
    p.budget = 0;
    PickResult r;
    picker.Update(row, cam, p, &r);
    Check(r.count == 0, "no budget, no lamp");
    p.budget = 12;
    std::vector<vl::GameLamp> empty;
    picker.Update(empty, cam, p, &r);
    Check(r.count == 0 && r.listed == 0, "an empty list (a day) is no lamp");
    picker.Update(row, cam, p, &r);
    Check(r.count > 0, "and a list gives lamps again");
    // Two records at one place are one lamp.
    std::vector<vl::GameLamp> twice = {StreetLamp(0.0f, -10.0f), StreetLamp(0.0f, -10.0f)};
    picker.Reset();
    picker.Update(twice, cam, p, &r);
    Check(r.count == 1, "a lamp listed twice is one lamp");
    // Records that light nothing are not candidates.
    vl::GameLamp off = StreetLamp(0.0f, -10.0f);
    off.scale = 0.0f;
    vl::GameLamp nan = StreetLamp(0.0f, -20.0f);
    nan.pos[0] = std::nanf("");
    picker.Reset();
    picker.Update({off, nan}, cam, p, &r);
    Check(r.count == 0, "a faded out lamp and a garbage record are no candidate");
  }
}

void CheckHysteresis() {
  // Four lamps, a budget of four (3 picks + 1 spare slot). A, B, C close, D a little farther than C.
  const VolCamera cam = Street();
  auto lamp_at = [](float z, float intensity = 55.0f) { return StreetLamp(0.0f, z, intensity); };
  vl::GameLamp a = lamp_at(-10.0f), b = lamp_at(-14.0f), c = lamp_at(-18.0f), d = lamp_at(-19.0f);
  PickParams p;
  p.budget = 4;
  p.dt = 1.0f / 60.0f;
  LampPicker picker;
  PickResult r;
  picker.Update({a, b, c, d}, cam, p, &r);  // the first frame: instant
  Check(WeightOf(r, a) > 0.0f && WeightOf(r, b) > 0.0f && WeightOf(r, c) > 0.0f && WeightOf(r, d) < 0.0f, "A, B and C are picked, D is not");
  // D gets a little brighter than C (40 per cent of the intensity, a score 1.2 times as much): C keeps its place.
  d.intensity = 55.0f * 1.4f;
  c = lamp_at(-18.0f);
  const float score_c = LampScore(c, cam, 0.5f, 160.0f);
  const float score_d = LampScore(d, cam, 0.5f, 160.0f);
  Check(score_d > score_c && score_d < kKeepBonus * score_c, "D scores more than C, but less than the hysteresis allows");
  for (int i = 0; i < 5; ++i) {
    picker.Update({a, b, c, d}, cam, p, &r);
  }
  Check(WeightOf(r, c) > 0.99f && WeightOf(r, d) < 0.0f, "C keeps its place against a rival within the hysteresis");
  // D gets much brighter: it takes C's place, and C fades in the spare slot.
  d.intensity = 100.0f;
  Check(LampScore(d, cam, 0.5f, 160.0f) > kKeepBonus * score_c, "D now scores beyond the hysteresis");
  picker.Update({a, b, c, d}, cam, p, &r);
  const float wc = WeightOf(r, c);
  const float wd = WeightOf(r, d);
  Check(wd > 0.0f && wd < 0.1f, "D comes in at the start of its fade");
  Check(wc > 0.9f && wc < 1.0f, "C starts to fade out");
  Check(r.count == 4 && r.wanted == 3 && r.fading == 1, "three picked and one fading in the spare slot");
  // The fade takes about kFadeSeconds and is smooth: the weights move by one step a frame and C ends gone, D at full weight.
  float last_c = wc;
  float last_d = wd;
  float max_jump = 0.0f;
  int frames = 0;
  while (frames < 200 && (WeightOf(r, c) >= 0.0f || WeightOf(r, d) < 0.999f)) {
    picker.Update({a, b, c, d}, cam, p, &r);
    ++frames;
    const float nc = std::max(WeightOf(r, c), 0.0f);
    const float nd = WeightOf(r, d);
    max_jump = std::max({max_jump, std::fabs(nc - last_c), std::fabs(nd - last_d)});
    last_c = nc;
    last_d = nd;
  }
  Check(frames > 15 && frames < 30, "the fade takes " + std::to_string(kFadeSeconds) + " s: " + std::to_string(frames) + " frames of 1/60 s");
  Check(max_jump < 0.06f, "no weight jumps by more than a step (no pop)");
  Check(WeightOf(r, c) < 0.0f && WeightOf(r, d) > 0.99f, "C is gone and D is at full weight");
  // More lamps leaving than spare slots: the weakest of them go at once, the pick never exceeds the budget.
  LampPicker crowd;
  PickParams pc;
  pc.budget = 12;
  pc.dt = 1.0f / 60.0f;
  PickResult rc;
  std::vector<vl::GameLamp> first = Row(10, 8.0f, -5.0f);
  crowd.Update(first, cam, pc, &rc);
  Check(rc.wanted == 10 && rc.fading == 0, "ten lamps picked");
  std::vector<vl::GameLamp> second = Row(10, 8.0f, -9.0f);  // ten other lamps, as near: every first lamp is replaced
  crowd.Update(second, cam, pc, &rc);
  Check(rc.count == 12, "the pick keeps to its budget when everything changes");
  Check(rc.wanted == 10 && rc.fading == LampPicker::SpareSlots(12), "ten picked and the two spare slots hold the strongest of the leavers");
  Check(rc.dropped == 8, "and the other eight go at once");
}

void CheckResetAndFades() {
  const VolCamera cam = Street();
  const std::vector<vl::GameLamp> row = Row(20, 10.0f);
  PickParams p;
  p.budget = 12;
  p.dt = 1.0f / 60.0f;
  LampPicker picker;
  PickResult r;
  picker.Update(row, cam, p, &r);  // instant: the first pick
  // A new lamp appears ahead: it fades in.
  std::vector<vl::GameLamp> next = row;
  const vl::GameLamp fresh = StreetLamp(0.0f, -3.0f, 100.0f);
  next.push_back(fresh);
  picker.Update(next, cam, p, &r);
  const float w1 = WeightOf(r, fresh);
  picker.Update(next, cam, p, &r);
  const float w2 = WeightOf(r, fresh);
  Check(w1 > 0.0f && w1 < 0.2f && w2 > w1, "a new lamp fades in: " + std::to_string(w1) + " then " + std::to_string(w2));
  // A cut: the picked lamps all start at full weight.
  PickParams cut = p;
  cut.reset = true;
  picker.Update(next, cam, cut, &r);
  Check(std::fabs(WeightOf(r, fresh) - 1.0f) < 1e-5f, "after a cut a picked lamp is at full weight at once");
  Check(r.fading == 0, "and nothing fades after a cut");
  // A lamp that leaves the list fades out from its last place (a day coming): the list goes empty, the lamps go in kFadeSeconds.
  std::vector<vl::GameLamp> empty;
  int frames = 0;
  picker.Update(empty, cam, p, &r);
  Check(r.count > 0 && r.fading == r.count && r.wanted == 0, "lamps that left the list are still there, fading");
  while (r.count > 0 && frames < 100) {
    picker.Update(empty, cam, p, &r);
    ++frames;
  }
  Check(frames > 15 && frames < 30, "an empty list takes the lamps out over the fade: " + std::to_string(frames) + " frames");
  // dt 0 (the first Fill after a switch) is instant; a long gap is the caller's reset.
  LampPicker again;
  PickParams zero;
  zero.budget = 12;
  zero.dt = 0.0f;
  again.Update(row, cam, zero, &r);
  Check(std::fabs(WeightOf(r, row[0]) - 1.0f) < 1e-5f, "dt 0 starts at full weight");
  // The colour is the weight times the lamp's: a half weight is half the colour.
  LampPicker slow;
  slow.Update({StreetLamp(0.0f, -10.0f)}, cam, zero, &r);
  const vl::GameLamp lamp = StreetLamp(0.0f, -10.0f);
  const VolLamp full = MakeVolLamp(lamp);
  Check(std::fabs(r.lamps[0].colour[1] - full.colour[1]) < 1e-4f, "at full weight the colour is the lamp's");
}

// A long random walk of the camera along a street of lamps with the list changing: the pick's invariants hold every frame, and a still camera settles.
void CheckStress() {
  uint32_t state = 12345u;
  auto rnd = [&]() {
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  };
  std::vector<vl::GameLamp> city;
  for (int i = 0; i < 400; ++i) {
    city.push_back(StreetLamp(((i & 1) != 0 ? 6.0f : -6.0f) + rnd() - 0.5f, -float(i) * 9.0f + 200.0f, 40.0f + 60.0f * rnd(), 5.0f + 2.0f * rnd()));
  }
  for (const uint32_t budget : {1u, 2u, 6u, 12u, 32u, 48u}) {
    LampPicker picker;
    VolCamera cam = Street();
    PickParams p;
    p.budget = budget;
    p.dt = 1.0f / 60.0f;
    PickResult r;
    uint32_t max_count = 0;
    bool finite = true;
    for (int frame = 0; frame < 1500; ++frame) {
      cam.pos[2] -= 0.4f;  // 24 m/s down the street
      if (frame % 300 == 299) {
        cam.pos[2] += 900.0f;  // a cut
        p.reset = true;
      } else {
        p.reset = false;
      }
      // the game's list: the lamps within 300 m of the eye, each with the prop pass's fade
      std::vector<vl::GameLamp> list;
      for (const vl::GameLamp& l : city) {
        const float dz = l.pos[2] - cam.pos[2];
        if (std::fabs(dz) < 300.0f) {
          vl::GameLamp g = l;
          g.scale = std::clamp(1.0f - (std::fabs(dz) - 190.0f) / 30.0f, 0.0f, 1.0f);
          list.push_back(g);
        }
      }
      picker.Update(list, cam, p, &r);
      max_count = std::max(max_count, r.count);
      for (uint32_t i = 0; i < r.count; ++i) {
        for (int k = 0; k < 3; ++k) {
          finite = finite && std::isfinite(r.lamps[i].colour[k]) && r.lamps[i].colour[k] >= 0.0f;
        }
        finite = finite && std::isfinite(r.lamps[i].pos[0]) && std::isfinite(LampNearSq(r.lamps[i]));
      }
      if (r.count > budget || r.wanted + r.fading != r.count) {
        Check(false, "the pick keeps to the budget and counts its lamps (budget " + std::to_string(budget) + ", frame " + std::to_string(frame) + ")");
        break;
      }
    }
    Check(finite, "every colour of the walk is finite and not negative (budget " + std::to_string(budget) + ")");
    Check(max_count <= budget, "the walk never exceeds the budget " + std::to_string(budget));
    // A still camera settles: after a second the same list gives the same lamps at full weight and nothing fades.
    std::vector<vl::GameLamp> list;
    for (const vl::GameLamp& l : city) {
      if (std::fabs(l.pos[2] - cam.pos[2]) < 150.0f) {
        list.push_back(l);
      }
    }
    p.reset = false;
    for (int i = 0; i < 90; ++i) {
      picker.Update(list, cam, p, &r);
    }
    PickResult again;
    picker.Update(list, cam, p, &again);
    Check(again.fading == 0 && again.count == r.count && again.wanted == r.wanted, "a still camera settles: nothing fades (budget " + std::to_string(budget) + ")");
    bool full = true;
    for (uint32_t i = 0; i < again.count; ++i) {
      full = full && std::fabs(again.lamps[i].colour[0] - r.lamps[i].colour[0]) < 1e-3f;
    }
    Check(full, "and the same lamps come at the same weights (budget " + std::to_string(budget) + ")");
    Check(again.count <= budget && again.count == again.wanted, "all of them picked (budget " + std::to_string(budget) + ")");
  }
}

// --- the headlights and the shadow atlas ----------------------------------------------------------------------------------------------------------

TestHeadlight MakeHeadlight(float x, uint32_t car, bool player) {
  TestHeadlight h{};
  h.pos[0] = x;
  h.pos[1] = 0.7f;
  h.pos[2] = 0.0f;
  h.dir[2] = -1.0f;
  h.cos_outer = std::cos(38.0f * float(kDeg));
  h.cos_inner = std::cos(12.0f * float(kDeg));
  h.range = 70.0f;
  h.color[0] = 1.0f;
  h.color[1] = 0.93f;
  h.color[2] = 0.78f;
  h.car_key = car;
  h.is_player = player;
  return h;
}

TestView MakeView(uint32_t light, float u0, float v0, float u1, float v1) {
  TestView v{};
  for (int i = 0; i < 16; ++i) {
    v.world_to_shadow[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  }
  v.atlas_rect[0] = u0;
  v.atlas_rect[1] = v0;
  v.atlas_rect[2] = u1;
  v.atlas_rect[3] = v1;
  v.depth_bias = 2e-4f;
  v.texel_size = 1.0f / 512.0f;
  v.light_index = light;
  v.valid = 1;
  return v;
}

void CheckHeadlights() {
  TestHeadlightSet set;
  set.lights[0] = MakeHeadlight(-0.6f, 7, true);
  set.lights[1] = MakeHeadlight(0.6f, 7, true);
  set.lights[2] = MakeHeadlight(30.0f, 9, false);
  set.count = 3;
  VolInputs in;
  FillHeadlights(set, &in);
  Check(in.headlight_count == 3, "the set's three lights");
  for (uint32_t i = 0; i < in.headlight_count; ++i) {
    ApplyHeadlightUnits(&in.headlights[i]);
  }
  // Units: colour x kPeak x D^2, near radius squared D^2 (the lit shaders' kPeak / (1 + (d / D)^2); the rig's own values are checked at the end).
  constexpr double kD2 = double(kHeadlightFalloffDistance) * double(kHeadlightFalloffDistance);
  CheckNear(in.headlights[0].colour[0], 1.0 * double(kHeadlightPeak) * kD2, 1e-3, "the headlight's colour is the rig's times kPeak x D^2");
  CheckNear(in.headlights[0].colour[1], 0.93 * double(kHeadlightPeak) * kD2, 1e-3, "all three channels");
  CheckNear(LampNearSq(in.headlights[0]), kD2, 1e-6, "its near radius squared is D^2");
  CheckNear(in.headlights[0].range, 70.0, 1e-6, "its range is the rig's");
  // The volume's term at the lamp: the lit shaders' (peak x 1 / (1 + (d/D)^2) x colour): the same at every distance (the window aside).
  const VolLamp& hl = in.headlights[0];
  for (const double d : {0.0, 3.0, 9.0, 20.0, 40.0}) {
    const double vol = double(hl.colour[0]) * double(LampFalloff(float(d), hl.range, LampNearSq(hl)));
    const double shader = double(kHeadlightPeak) / (1.0 + d * d / kD2);
    const double window = std::pow(std::max(1.0 - std::pow(d / 70.0, 4.0), 0.0), 2.0);
    CheckNear(vol, shader * window, 1e-4 * shader + 1e-6, "the headlight's term in the air is the lit shaders' at " + std::to_string(d) + " m");
  }
  // With no frame and with no atlas: lit, no views.
  ShadowAtlasState none;
  FillShadows(static_cast<const TestFrame*>(nullptr), none, &in);
  Check(in.shadow_view_count == 0 && in.shadow_atlas_slot == kNoSlot && in.headlights[0].shadow_view == -1, "no frame: no shadow");
  TestFrame frame;
  frame.views[0] = MakeView(0, 0.0f, 0.0f, 0.5f, 1.0f);
  frame.views[1] = MakeView(1, 0.0f, 0.0f, 0.5f, 1.0f);  // the car's second lamp: the same tile
  frame.views[2] = MakeView(2, 0.5f, 0.0f, 1.0f, 1.0f);
  frame.count = 3;
  FillShadows(&frame, none, &in);
  Check(in.shadow_view_count == 0 && in.shadow_atlas_slot == kNoSlot, "an atlas that is not valid: no shadow");
  ShadowAtlasState atlas;
  atlas.valid = true;
  atlas.slot = 123;
  atlas.width = 1024;
  atlas.height = 512;
  FillShadows(&frame, atlas, &in);
  Check(in.shadow_view_count == 3 && in.shadow_atlas_slot == 123, "the three views and the atlas's slot");
  CheckNear(in.shadow_atlas_size[0], 1024.0, 1e-6, "the atlas's width");
  CheckNear(in.shadow_atlas_size[1], 512.0, 1e-6, "and height");
  Check(in.headlights[0].shadow_view == 0 && in.headlights[1].shadow_view == 1 && in.headlights[2].shadow_view == 2, "each light reads its view (a car's two lamps, two entries)");
  Check(in.headlights[0].shadow_view_b == -1 && in.headlights[1].shadow_view_b == -1 && in.headlights[2].shadow_view_b == -1, "a light with one view has no second");
  CheckNear(in.shadow_views[2].atlas_rect[0], 0.5, 1e-9, "the tile's corner is carried as published (u0)");
  CheckNear(in.shadow_views[2].atlas_rect[2], 1.0, 1e-9, "and its far corner (u1, not a size)");
  // A slot that is not a slot, or a zero size, gives nothing.
  ShadowAtlasState bad = atlas;
  bad.slot = kNoSlot;
  FillShadows(&frame, bad, &in);
  Check(in.shadow_view_count == 0 && in.headlights[0].shadow_view == -1, "no slot, no shadow");
  bad = atlas;
  bad.width = 0;
  FillShadows(&frame, bad, &in);
  Check(in.shadow_view_count == 0, "a zero size, no shadow");
  // HL-PER-LAMP: a car's light has a view for each headlamp (two views name one light): the light reads both, the share is the mean of the two.
  {
    VolInputs lamps_in;
    TestHeadlightSet one_car;
    one_car.lights[0] = MakeHeadlight(0.0f, 7, true);
    one_car.lights[1] = MakeHeadlight(30.0f, 9, false);
    one_car.count = 2;
    FillHeadlights(one_car, &lamps_in);
    TestFrame pair;
    pair.views[0] = MakeView(0, 0.0f, 0.0f, 0.25f, 0.5f);
    pair.views[1] = MakeView(0, 0.25f, 0.0f, 0.5f, 0.5f);  // the same light, the other lamp: the next tile
    pair.views[2] = MakeView(1, 0.5f, 0.0f, 0.75f, 0.5f);
    pair.views[3] = MakeView(1, 0.75f, 0.0f, 1.0f, 0.5f);
    pair.count = 4;
    FillShadows(&pair, atlas, &lamps_in);
    Check(lamps_in.shadow_view_count == 4 && lamps_in.headlights[0].shadow_view == 0 && lamps_in.headlights[0].shadow_view_b == 1 && lamps_in.headlights[1].shadow_view == 2 &&
              lamps_in.headlights[1].shadow_view_b == 3,
          "each light reads the two views of its lamps");
    // One of the two cannot be read: the light keeps the other as its one.
    TestFrame half = pair;
    half.views[0].atlas_rect[2] = half.views[0].atlas_rect[0];  // an empty tile
    FillHeadlights(one_car, &lamps_in);
    FillShadows(&half, atlas, &lamps_in);
    Check(lamps_in.headlights[0].shadow_view == 1 && lamps_in.headlights[0].shadow_view_b == -1, "a lamp whose view cannot be read: the light keeps the other lamp's view as its one");
    Check(lamps_in.headlights[1].shadow_view == 2 && lamps_in.headlights[1].shadow_view_b == 3, "the other car's views stay");
    // Both cannot: the light is lit.
    half.views[1].world_to_shadow[3] = std::nanf("");
    FillHeadlights(one_car, &lamps_in);
    FillShadows(&half, atlas, &lamps_in);
    Check(lamps_in.headlights[0].shadow_view == -1 && lamps_in.headlights[0].shadow_view_b == -1 && lamps_in.headlights[1].shadow_view == 2, "both unreadable: the light is lit, the other car's views stay");
    // Eight views: four cars, two lamps each.
    TestHeadlightSet four;
    for (uint32_t i = 0; i < 4; ++i) {
      four.lights[i] = MakeHeadlight(10.0f * float(i), 20 + i, i == 0);
    }
    four.count = 4;
    FillHeadlights(four, &lamps_in);
    TestFrame all;
    all.count = 8;
    for (uint32_t v = 0; v < 8; ++v) {
      all.views[v] = MakeView(v / 2, float(v % 4) * 0.25f, float(v / 4) * 0.5f, float(v % 4 + 1) * 0.25f, float(v / 4 + 1) * 0.5f);
    }
    FillShadows(&all, atlas, &lamps_in);
    bool ok = lamps_in.shadow_view_count == 8;
    for (uint32_t i = 0; i < 4; ++i) {
      ok = ok && lamps_in.headlights[i].shadow_view == int32_t(2 * i) && lamps_in.headlights[i].shadow_view_b == int32_t(2 * i + 1);
    }
    Check(ok, "four cars, eight views: every light reads its two");
  }
  // A view that cannot be read is dropped and its light is lit, the others stay.
  TestFrame broken = frame;
  broken.views[0].atlas_rect[2] = broken.views[0].atlas_rect[0];  // an empty tile
  broken.views[1].world_to_shadow[3] = std::nanf("");
  FillShadows(&broken, atlas, &in);
  Check(in.headlights[0].shadow_view == -1 && in.headlights[1].shadow_view == -1, "an empty tile and a NaN matrix are dropped");
  Check(in.headlights[2].shadow_view == 2 && in.shadow_views[0].valid == 0 && in.shadow_view_count == 3, "the third view still shadows its light");
  TestFrame invalid = frame;
  for (uint32_t i = 0; i < 3; ++i) {
    invalid.views[i].valid = 0;
  }
  FillShadows(&invalid, atlas, &in);
  Check(in.shadow_view_count == 0 && in.shadow_atlas_slot == kNoSlot, "when no light reads a view nothing is bound");
  TestFrame empty;
  FillShadows(&empty, atlas, &in);
  Check(in.shadow_view_count == 0, "no views, no shadow");
  // A view of a light that is not in the set is ignored.
  TestFrame stray = frame;
  stray.views[2].light_index = 3;
  FillShadows(&stray, atlas, &in);
  Check(in.headlights[2].shadow_view == -1, "a view of a light that is not in the set is ignored");
  // The constants the compute pass reads: a level reads as many headlights as it has, and a light's view beyond the views is dropped.
  FillShadows(&frame, atlas, &in);
  in.valid = true;
  VolGpuConstants k;
  const GridDesc medium = GridFor(Level::kMedium);  // two headlights
  VolCamera cam;
  HistoryUse h;
  MakeGpuConstants(medium, in, cam, h, 1, &k);
  Check(k.counts[1] == 2, "medium reads two headlights");
  Check(k.headlights[0].shadow_view == 0 && k.headlights[1].shadow_view == 1, "with their views");
  Check(k.counts[2] == 3 && k.counts[3] == 123, "and the views and the atlas's slot");
  CheckNear(LampNearSq(k.headlights[0]), kD2, 1e-6, "the near radius squared reaches the constants");
  // The rig's constants, when the tree has them.
#if TEST_RULES_HEADER
  CheckNear(kHeadlightPeak, mcla::gpu::headlight_rules::kPeak, 1e-9, "kHeadlightPeak is the rig's kPeak");
  CheckNear(kHeadlightFalloffDistance, mcla::gpu::headlight_rules::kFalloffDistance, 1e-9, "kHeadlightFalloffDistance is the rig's");
  // the low beam's shape the volume gives the headlights (volumetrics_rules.h HeadlightCutoff / HeadlightNearFade; the inject's HeadlightShape) is the lit shaders'
  CheckNear(kHeadCutLow, mcla::gpu::headlight_rules::kCutLow, 1e-9, "the volume's low beam cut-off starts where the rig's does");
  CheckNear(kHeadCutHigh, mcla::gpu::headlight_rules::kCutHigh, 1e-9, "and ends where the rig's does");
  CheckNear(kHeadNearStart, mcla::gpu::headlight_rules::kNearStart, 1e-9, "the near fade starts where the rig's does");
  CheckNear(kHeadNearSpan, mcla::gpu::headlight_rules::kNearSpan, 1e-9, "and spans what the rig's does");
#endif
  std::printf("headlight and shadow types: %s\n", TEST_REAL_HEADERS ? "the real headers of feat/headlights" : "mirrors (feat/headlights is not in this tree)");
}

// The lookup's twin: two tiles side by side, the left one with a wall.
void CheckShadowShare() {
  const int w = 64, h = 32;  // the atlas, two tiles of 32 x 32
  std::vector<float> atlas(size_t(w) * size_t(h), 1.0f);
  // The left tile: a wall at depth 0.3 over x in [8, 16); the right tile: everything at depth 0 (a full shadow if it were read).
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < 32; ++x) {
      atlas[size_t(y) * w + x] = (x >= 8 && x < 16) ? 0.3f : 1.0f;
    }
    for (int x = 32; x < 64; ++x) {
      atlas[size_t(y) * w + x] = 0.0f;
    }
  }
  auto fetch = [&](int x, int y) { return atlas[size_t(y) * w + x]; };
  // A view whose matrix maps the world point (x, y, z) to u = x, v = y, depth = z (w = 1): the first tile is u in [0, 0.5].
  VolShadowView view{};
  for (int i = 0; i < 16; ++i) {
    view.world_to_shadow[i] = 0.0f;
  }
  view.world_to_shadow[0] = 1.0f;   // o.x = x
  view.world_to_shadow[5] = 1.0f;   // o.y = y
  view.world_to_shadow[10] = 1.0f;  // o.z = z
  view.world_to_shadow[15] = 1.0f;  // o.w = 1
  view.atlas_rect[0] = 0.0f;
  view.atlas_rect[1] = 0.0f;
  view.atlas_rect[2] = 0.5f;
  view.atlas_rect[3] = 1.0f;
  view.depth_bias = 0.0f;
  view.texel_size = 1.0f / 32.0f;
  view.valid = 1;
  const float behind_wall[3] = {12.5f / 64.0f, 0.5f, 0.5f};   // the texel centre of the wall, depth 0.5 behind a wall at 0.3
  const float in_front[3] = {12.5f / 64.0f, 0.5f, 0.2f};      // nearer than the wall
  const float no_wall[3] = {3.5f / 64.0f, 0.5f, 0.5f};
  CheckNear(ShadowShare(view, behind_wall, w, h, fetch), 0.0, 1e-5, "a point behind the wall is in its shadow");
  CheckNear(ShadowShare(view, in_front, w, h, fetch), 1.0, 1e-5, "a point in front of the wall is lit");
  CheckNear(ShadowShare(view, no_wall, w, h, fetch), 1.0, 1e-5, "a point where nothing was drawn is lit");
  // The edge of the wall is smooth: halfway between a wall texel and a free one is half.
  const float edge[3] = {16.0f / 64.0f, 0.5f, 0.5f};
  CheckNear(ShadowShare(view, edge, w, h, fetch), 0.5, 1e-5, "on the wall's edge the compare is half (a bilinear weighting)");
  // Outside the tile (the other view's half), outside the depth range, behind the light: lit.
  const float other_tile[3] = {40.0f / 64.0f, 0.5f, 0.5f};
  CheckNear(ShadowShare(view, other_tile, w, h, fetch), 1.0, 1e-9, "a point mapped into the neighbour tile is lit (its texels are another view's)");
  const float deep[3] = {12.5f / 64.0f, 0.5f, 1.5f};
  CheckNear(ShadowShare(view, deep, w, h, fetch), 1.0, 1e-9, "beyond the map's far plane: lit");
  const float shallow[3] = {12.5f / 64.0f, 0.5f, -0.2f};
  CheckNear(ShadowShare(view, shallow, w, h, fetch), 1.0, 1e-9, "before its near plane: lit");
  // The tile's last texel column: the taps must stay in the tile and never read the right tile's zeros.
  const float tile_edge[3] = {31.9f / 64.0f, 0.5f, 0.5f};
  CheckNear(ShadowShare(view, tile_edge, w, h, fetch), 1.0, 1e-5, "at the tile's edge the taps are clamped into the tile (the neighbour's zeros are never read)");
  const float left_edge[3] = {0.1f / 64.0f, 0.5f, 0.5f};
  CheckNear(ShadowShare(view, left_edge, w, h, fetch), 1.0, 1e-5, "and at the atlas's left edge");
  // An invalid view is lit.
  VolShadowView off = view;
  off.valid = 0;
  CheckNear(ShadowShare(off, behind_wall, w, h, fetch), 1.0, 1e-9, "an invalid view lights everything");
  // The bias lifts the shadow.
  VolShadowView biased = view;
  biased.depth_bias = 0.3f;
  CheckNear(ShadowShare(biased, behind_wall, w, h, fetch), 1.0, 1e-5, "a bias of 0.3 puts the receiver (0.5 - 0.3) in front of the wall (0.3)");
  // The right tile read with its own rectangle does see its zeros: the clamp only keeps a view in its own tile.
  VolShadowView right = view;
  right.atlas_rect[0] = 0.5f;
  right.atlas_rect[2] = 1.0f;
  const float in_right[3] = {40.5f / 64.0f, 0.5f, 0.5f};
  CheckNear(ShadowShare(right, in_right, w, h, fetch), 0.0, 1e-5, "the right tile's own view reads the right tile");
}

// --- the files ---------------------------------------------------------------------------------------------------------------------------------

bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream s;
  s << f.rdbuf();
  *out = s.str();
  return true;
}

void CheckFiles(const std::string& folder) {
  std::string toml;
  if (ReadFile(folder + "/../../larecomp_config.toml", &toml)) {
    const size_t at = toml.find("name = \"MCLAVolLampList\"");
    Check(at != std::string::npos, "larecomp_config.toml has the lamp list's hook");
    if (at != std::string::npos) {
      const size_t start = toml.rfind("[[midasm_hook]]", at);
      const size_t end = toml.find("after_instruction", at);
      const std::string block = toml.substr(start, end - start);
      Check(block.find("address = 0x82302814") != std::string::npos, "the hook is at 0x82302814, one instruction after the cone drawer's entry");
      Check(block.find("registers = []") != std::string::npos, "it reads no register");
    }
  } else {
    Check(false, "larecomp_config.toml is readable from " + folder);
  }
  std::string hook;
  if (ReadFile(folder + "/../mc_engine/hooks/vol_lamps.cpp", &hook)) {
    Check(hook.find("void MCLAVolLampList()") != std::string::npos, "the hook file defines MCLAVolLampList");
    Check(hook.find("kListHeaderAddress") != std::string::npos && hook.find("Wanted()") != std::string::npos, "and reads the list's header while wanted");
    Check(hook.find("WriteGuest") == std::string::npos && hook.find("REX_STORE") == std::string::npos, "and writes nothing into the guest");
  } else {
    Check(false, "src/mc_engine/hooks/vol_lamps.cpp is readable");
  }
  std::string cmake;
  if (ReadFile(folder + "/../../CMakeLists.txt", &cmake)) {
    Check(cmake.find("src/mc_engine/hooks/vol_lamps.cpp") != std::string::npos, "the top CMakeLists builds the hook file");
  }
  std::string gpu_cmake;
  if (ReadFile(folder + "/CMakeLists.txt", &gpu_cmake)) {
    Check(gpu_cmake.find("volumetrics_inputs.cpp") != std::string::npos, "src/gpu/CMakeLists builds the producer");
  }
  // The shader reads the lamp's fourth row where the header puts it: the shadow view at +48 and the near radius squared at +52.
  std::string inject;
  if (ReadFile(folder + "/hlsl/volumetrics_inject_cs.hlsl", &inject)) {
    Check(inject.find("g_vol.Load(offset + " + std::to_string(offsetof(VolLamp, shadow_view)) + "u)") != std::string::npos, "the inject reads the shadow view where VolLamp has it");
    Check(inject.find("g_vol.Load(offset + " + std::to_string(offsetof(VolLamp, shadow_view_b)) + "u)") != std::string::npos && inject.find("HeadlightLit2(") != std::string::npos &&
              inject.find("0.5 * (HeadlightLit(p, view_a) + HeadlightLit(p, view_b))") != std::string::npos,
          "the inject reads the second view too and takes the mean of the two lamps' lookups");
    Check(inject.find("g_vol.Load(offset + " + std::to_string(offsetof(VolLamp, pad)) + "u)") != std::string::npos, "and the near radius squared in pad[0]");
    Check(inject.find("LoadDepthIn") != std::string::npos, "and clamps the shadow taps into the view's tile");
    Check(inject.find("rect.zw") != std::string::npos, "with the tile's far corner (u1 v1)");
  } else {
    Check(false, "hlsl/volumetrics_inject_cs.hlsl is readable");
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckRecord();
  CheckTable();
  CheckLaw();
  CheckCalibration();
  CheckFrustum();
  CheckPick();
  CheckHysteresis();
  CheckResetAndFades();
  CheckStress();
  CheckHeadlights();
  CheckShadowShare();
  if (argc > 1) {
    CheckFiles(argv[1]);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

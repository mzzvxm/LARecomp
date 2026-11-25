// Check of the headlight rig's rules (headlight_rig_rules.h): the CPU twin of the shader's term, the cone math, the night factor, where a
// car's headlights are (its lens draws and its own axes), the tracker's picks, hysteresis and fades, the shadow views' table and projection.
// A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_headlight_rig_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_headlight_rig_test

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "headlight_rig_rules.h"

namespace hr = mcla::gpu::headlight_rules;

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

void CheckNear(double got, double want, double tolerance, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tolerance)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.6f, want %.6f (+- %g)\n", what.c_str(), got, want, tolerance);
  }
}

bool Near2(double got, double want, double tolerance) { return std::fabs(got - want) <= tolerance; }

// A lamp at the origin looking along +x (horizontal), the default beam.
hr::Lamp BeamAlongX() {
  hr::Lamp lamp;
  lamp.position[0] = lamp.position[1] = lamp.position[2] = 0.0f;
  lamp.direction[0] = 1.0f;
  lamp.direction[1] = 0.0f;
  lamp.direction[2] = 0.0f;
  lamp.cos_outer = hr::CosOfDegrees(hr::kOuterDegrees);
  lamp.cos_inner = hr::CosOfDegrees(hr::kInnerDegrees);
  lamp.range = hr::kRange;
  return lamp;
}

hr::Surface WallFacingLamp(float distance, float lateral = 0.0f, float height = 0.0f) {
  hr::Surface s;
  s.position[0] = distance;
  s.position[1] = height;
  s.position[2] = lateral;
  s.normal[0] = -1.0f;
  s.normal[1] = 0.0f;
  s.normal[2] = 0.0f;
  return s;
}

// ---------------------------------------------------------------------------
// the term
// ---------------------------------------------------------------------------

// The calibration of the look (headlight_rig_rules.h, HEADLIGHT-FIX-2): the road's term ahead of a low beam, in the game's light units where a street lamp is 1 and the
// night's ambient about 0.1. The first look (peak 2.4, half distance 9 m) gave 0.96 / 0.50 / 0.28 / 0.16 / 0.06 at 5 / 10 / 15 / 20 / 30 m: lost beyond 15 m.
void CheckCalibration() {
  hr::Lamp lamp = BeamAlongX();
  const float dip = hr::kDipDegrees * float(hr::kPi) / 180.0f;
  lamp.position[1] = 0.75f;
  lamp.direction[0] = std::cos(dip);
  lamp.direction[1] = -std::sin(dip);  // the axis points a little below the car's forward
  const auto road = [&](float d) {
    hr::Surface s;
    s.position[0] = d;
    s.position[1] = 0.0f;
    s.position[2] = 0.0f;
    s.normal[0] = 0.0f;
    s.normal[1] = 1.0f;
    s.normal[2] = 0.0f;
    return hr::LampScale(lamp, s);
  };
  struct Want {
    float d, lo, hi;
  };
  const Want table[] = {{5.0f, 0.85f, 1.20f}, {10.0f, 0.70f, 0.95f}, {15.0f, 0.55f, 0.78f}, {20.0f, 0.40f, 0.62f},
                        {25.0f, 0.30f, 0.45f}, {30.0f, 0.20f, 0.33f}, {40.0f, 0.08f, 0.17f}, {50.0f, 0.02f, 0.08f}};
  for (const Want& w : table) {
    const float got = road(w.d);
    Check(got >= w.lo && got <= w.hi, "the road " + std::to_string(int(w.d)) + " m ahead of a low beam: " + std::to_string(got) + " in [" + std::to_string(w.lo) + ", " + std::to_string(w.hi) + "]");
  }
  Check(road(20.0f) > 2.5f * 0.16f && road(30.0f) > 3.0f * 0.06f, "the road at 20 and 30 m is far brighter than the first look's (0.16, 0.06)");
  Check(road(20.0f) > 3.0f * 0.1f && road(30.0f) > 2.0f * 0.1f, "the road 20 and 30 m ahead stands out of the night's ambient (about 0.1)");
  Check(road(5.0f) < 1.25f && road(3.0f) < 1.4f, "a road is never blown out near the car (a lamp's level is 1)");
  bool monotone = true;
  for (float d = 4.0f; d < 70.0f; d += 1.0f) {
    if (road(d + 1.0f) > road(d) + 1e-6f) monotone = false;
  }
  Check(monotone, "the road's light falls with the distance from 4 m on");
  // a wall facing the lamp is brighter than the road at the same distance, and a pale wall at 5 m clips at most as a car's lamps do
  hr::Surface wall = WallFacingLamp(5.0f, 0.0f, 0.75f);
  const float wall5 = hr::LampScale(lamp, wall);
  Check(wall5 > road(5.0f) && wall5 < 2.1f, "a wall facing the lamp at 5 m is lit more than the road, under 2.1");
  // the strength knob scales the colour, not the term: the rig multiplies the colour by it (headlight_rig.cpp); the term itself has no knob
  Check(hr::kPeak > 0.0f && hr::kFalloffDistance > hr::kNearStart, "the calibration's constants are sane");
}

void CheckTerm() {
  const hr::Lamp lamp = BeamAlongX();
  // On the axis, at 3 m, on a wall that faces the lamp: peak * falloff * window, the surface term 1, no cut-off, the near fade complete.
  {
    const float d = 3.0f;
    const float want = hr::kPeak * (1.0f / (1.0f + d * d / (hr::kFalloffDistance * hr::kFalloffDistance))) *
                       std::pow(1.0f - d * d / (hr::kRange * hr::kRange), 2.0f);
    CheckNear(hr::LampScale(lamp, WallFacingLamp(d)), want, 1e-4, "on the axis at 3 m: peak, fall-off and window");
  }
  // The fall-off is a half of the peak at kFalloffDistance (the window is near 1 there).
  {
    const float d = hr::kFalloffDistance;
    const float scale = hr::LampScale(lamp, WallFacingLamp(d));
    const float window = std::pow(1.0f - d * d / (hr::kRange * hr::kRange), 2.0f);
    CheckNear(scale, hr::kPeak * 0.5 * window, 1e-4, "at D the fall-off is a half");
  }
  // Monotone down along the axis from the near fade's end, and zero at and beyond the reach.
  {
    float last = 1e9f;
    bool monotone = true;
    for (float d = 3.0f; d < hr::kRange; d += 1.0f) {
      const float s = hr::LampScale(lamp, WallFacingLamp(d));
      if (s > last + 1e-6f) monotone = false;
      last = s;
    }
    Check(monotone, "the term falls with the distance along the axis");
    CheckNear(hr::LampScale(lamp, WallFacingLamp(hr::kRange)), 0.0, 1e-9, "zero at the reach");
    CheckNear(hr::LampScale(lamp, WallFacingLamp(hr::kRange + 5.0f)), 0.0, 1e-9, "zero beyond the reach");
    Check(hr::LampScale(lamp, WallFacingLamp(hr::kRange - 1.0f)) > 0.0f, "not zero just inside the reach");
  }
  // The cone: full inside the inner angle, 0 at the outer, smooth between, 0 outside, behind the lamp 0.
  {
    const float d = 10.0f;
    const auto at_angle = [&](float degrees) {
      const float rad = degrees * float(hr::kPi) / 180.0f;
      hr::Surface s = WallFacingLamp(0.0f);
      s.position[0] = d * std::cos(rad);
      s.position[2] = d * std::sin(rad);  // sideways, at height 0: the cut-off does not enter
      s.normal[0] = -std::cos(rad);
      s.normal[2] = -std::sin(rad);       // faces the lamp
      return hr::LampScale(lamp, s);
    };
    const float centre = at_angle(0.0f);
    CheckNear(at_angle(hr::kInnerDegrees - 0.5f) / centre * 1.0, 1.0, 0.05, "inside the inner angle the cone is full (the fall-off is the same at a constant distance)");
    CheckNear(at_angle(hr::kOuterDegrees), 0.0, 1e-9, "zero at the outer angle");
    CheckNear(at_angle(hr::kOuterDegrees + 5.0f), 0.0, 1e-9, "zero outside the outer angle");
    const float mid = at_angle(0.5f * (hr::kInnerDegrees + hr::kOuterDegrees));
    Check(mid > 0.0f && mid < centre, "between the angles the cone is partial");
    float last = centre + 1.0f;
    bool monotone = true;
    for (float a = 0.0f; a < hr::kOuterDegrees; a += 1.0f) {
      const float s = at_angle(a);
      if (s > last + 1e-6f) monotone = false;
      last = s;
    }
    Check(monotone, "the term does not grow with the angle off the axis");
    hr::Surface behind = WallFacingLamp(-10.0f);
    behind.normal[0] = 1.0f;
    CheckNear(hr::LampScale(lamp, behind), 0.0, 1e-9, "behind the lamp: nothing");
  }
  // The near fade: nothing nearer than kNearStart, complete from kNearStart + kNearSpan.
  {
    CheckNear(hr::LampScale(lamp, WallFacingLamp(hr::kNearStart - 0.1f)), 0.0, 1e-9, "nothing very near the lamp");
    const float full_at = hr::kNearStart + hr::kNearSpan;
    const float partial = hr::LampScale(lamp, WallFacingLamp(hr::kNearStart + 0.5f * hr::kNearSpan));
    const float complete = hr::LampScale(lamp, WallFacingLamp(full_at));
    Check(partial > 0.0f && partial < complete, "the near fade is partial in its span");
  }
  // The surface term: a road (normal up) seen from a lamp 0.65 m above it, 10 m ahead, keeps the wrap's share; a surface turned away gets
  // nothing.
  {
    hr::Lamp car = BeamAlongX();
    car.position[1] = 0.65f;
    hr::Surface road;
    road.position[0] = 10.0f;
    road.position[1] = 0.0f;
    road.position[2] = 0.0f;
    road.normal[0] = 0.0f;
    road.normal[1] = 1.0f;
    road.normal[2] = 0.0f;
    hr::Surface wall = road;
    wall.normal[0] = -1.0f;
    wall.normal[1] = 0.0f;
    const float on_road = hr::LampScale(car, road);
    const float on_wall = hr::LampScale(car, wall);
    Check(on_road > 0.35f * on_wall && on_road < on_wall, "a road at a grazing angle keeps a share of what a wall facing the lamp gets");
    hr::Surface away = wall;
    away.normal[0] = 1.0f;  // faces away from the lamp
    CheckNear(hr::LampScale(car, away), 0.0, 1e-3, "a surface facing away gets (almost) nothing");
  }
  // The low beam's cut-off: a point high above the lamp gets nothing, one at the lamp's height all.
  {
    hr::Lamp low = BeamAlongX();
    // 20 m ahead, 10 m up: the point is 26.6 degrees above the lamp (sine 0.447 > kCutHigh), inside the cone's outer angle (38 degrees).
    hr::Surface high = WallFacingLamp(20.0f, 0.0f, 10.0f);
    CheckNear(hr::LampScale(low, high), 0.0, 1e-9, "a point well above the lamp is cut off");
    hr::Surface level = WallFacingLamp(20.0f, 0.0f, 0.5f);
    Check(hr::LampScale(low, level) > 0.0f, "a point near the lamp's height is lit");
  }
  // The visibility multiplies.
  {
    const hr::Surface s = WallFacingLamp(10.0f);
    CheckNear(hr::LampScale(lamp, s, 0.25f), 0.25 * hr::LampScale(lamp, s, 1.0f), 1e-6, "the shadow visibility scales the term");
  }
  // The sum over lamps and the colour.
  {
    hr::Lamp a = BeamAlongX();
    a.color[0] = 1.0f;
    a.color[1] = 0.5f;
    a.color[2] = 0.25f;
    hr::Lamp b = a;
    b.position[2] = 3.0f;  // a second lamp 3 m to the side
    const hr::Lamp lamps[2] = {a, b};
    const hr::Surface s = WallFacingLamp(10.0f);
    float sum[3];
    hr::LampSum(lamps, 2, s, nullptr, sum);
    const float one = hr::LampScale(a, s) + hr::LampScale(b, s);
    CheckNear(sum[0], one * 1.0, 1e-5, "the sum: red");
    CheckNear(sum[1], one * 0.5, 1e-5, "the sum: green");
    CheckNear(sum[2], one * 0.25, 1e-5, "the sum: blue");
    const float vis[2] = {1.0f, 0.0f};
    hr::LampSum(lamps, 2, s, vis, sum);
    CheckNear(sum[0], hr::LampScale(a, s), 1e-5, "the sum with the second lamp in shadow");
    hr::LampSum(lamps, 0, s, nullptr, sum);
    CheckNear(sum[0] + sum[1] + sum[2], 0.0, 1e-12, "no lamps: nothing");
  }
  // The early-out box.
  {
    hr::Lamp a = BeamAlongX();
    hr::Lamp b = BeamAlongX();
    b.position[0] = 100.0f;
    b.range = 30.0f;
    const hr::Lamp lamps[2] = {a, b};
    float lo[3], hi[3];
    Check(hr::ReachBox(lamps, 2, lo, hi), "the reach box exists with lamps");
    CheckNear(lo[0], -hr::kRange, 1e-4, "box low x");
    CheckNear(hi[0], 130.0, 1e-4, "box high x");
    CheckNear(hi[1], hr::kRange, 1e-4, "box high y");
    Check(!hr::ReachBox(lamps, 0, lo, hi), "no lamps, no box");
    // Every point a lamp lights is inside the box (the early-out never takes light away).
    bool inside = true;
    for (float d = 1.0f; d < hr::kRange; d += 3.0f) {
      const hr::Surface s = WallFacingLamp(d);
      if (hr::LampScale(a, s) > 0.0f && (s.position[0] < lo[0] || s.position[0] > hi[0])) inside = false;
    }
    Check(inside, "everything a lamp lights is inside the box");
  }
}

// ---------------------------------------------------------------------------
// night
// ---------------------------------------------------------------------------

void CheckNight() {
  const auto sine_of = [](double degrees) { return std::sin(degrees * hr::kPi / 180.0); };
  CheckNear(hr::NightFactor(sine_of(-30.0)), 1.0, 1e-9, "night below the horizon");
  CheckNear(hr::NightFactor(sine_of(hr::kNightFull)), 1.0, 1e-9, "full night at the near edge");
  CheckNear(hr::NightFactor(sine_of(hr::kNightNone)), 0.0, 1e-9, "no night at the far edge");
  CheckNear(hr::NightFactor(sine_of(60.0)), 0.0, 1e-9, "day");
  const double mid = hr::NightFactor(sine_of(0.5 * (hr::kNightFull + hr::kNightNone)));
  CheckNear(mid, 0.5, 1e-6, "half way, half night");
  double last = 2.0;
  bool monotone = true;
  for (double a = -10.0; a <= 20.0; a += 0.5) {
    const double n = hr::NightFactor(sine_of(a));
    if (n > last + 1e-12) monotone = false;
    last = n;
  }
  Check(monotone, "the night factor falls as the sun rises");
  // The direction the light TRAVELS: a noon sun travels down (y -0.87), the sine of its elevation is +0.87.
  const double noon[3] = {0.3, -0.87, 0.4};
  CheckNear(hr::SunSine(noon), 0.87 / std::sqrt(0.09 + 0.87 * 0.87 + 0.16), 1e-9, "a light travelling down is a high sun");
  const double moon[3] = {0.2, 0.9, 0.1};
  Check(hr::NightFactor(hr::SunSine(moon)) == 1.0, "a light travelling up is night");
  const double none[3] = {0, 0, 0};
  Check(hr::NightFactor(hr::SunSine(none)) == 0.0, "no light constants: day");
}

// ---------------------------------------------------------------------------
// the car
// ---------------------------------------------------------------------------

// A car's world matrix as the game's (row vectors: right, up, back, position) facing `heading` degrees (0 along +z, 90 along +x).
void CarMatrix(double heading_degrees, const double position[3], double m[4][4]) {
  const double h = heading_degrees * hr::kPi / 180.0;
  const double forward[3] = {std::sin(h), 0.0, std::cos(h)};
  const double up[3] = {0.0, 1.0, 0.0};
  // right = forward x up for a right-handed world looking along forward with y up (the game's: the right is forward x up)
  const double right[3] = {forward[1] * up[2] - forward[2] * up[1], forward[2] * up[0] - forward[0] * up[2], forward[0] * up[1] - forward[1] * up[0]};
  for (int c = 0; c < 3; ++c) {
    m[0][c] = right[c];
    m[1][c] = up[c];
    m[2][c] = -forward[c];
    m[3][c] = position[c];
  }
  for (int r = 0; r < 4; ++r) {
    m[r][3] = r == 3 ? 1.0 : 0.0;
  }
}

void LensesOf(const hr::Pose& pose, double forward_offset, double side, double height, double out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = pose.position[c] - pose.back[c] * forward_offset + pose.right[c] * side + pose.up[c] * height;
  }
}

void CheckCar() {
  const double position[3] = {100.0, 2.0, 50.0};
  double m[4][4];
  CarMatrix(62.0, position, m);
  hr::Pose pose;
  Check(hr::PoseFromWorld(m, &pose), "a car's matrix is a pose");
  CheckNear(pose.position[0], 100.0, 1e-12, "pose position");
  // Not poses: a NaN, a shrunk axis, two axes that are not at right angles.
  {
    double bad[4][4];
    std::copy(&m[0][0], &m[0][0] + 16, &bad[0][0]);
    bad[3][1] = std::nan("");
    hr::Pose p;
    Check(!hr::PoseFromWorld(bad, &p), "a position that is not a number is no pose");
    std::copy(&m[0][0], &m[0][0] + 16, &bad[0][0]);
    for (int c = 0; c < 3; ++c) bad[2][c] *= 0.1;
    Check(!hr::PoseFromWorld(bad, &p), "an axis of a tenth is no pose");
    std::copy(&m[0][0], &m[0][0] + 16, &bad[0][0]);
    bad[2][0] = bad[1][0];
    bad[2][1] = bad[1][1];
    bad[2][2] = bad[1][2];
    Check(!hr::PoseFromWorld(bad, &p), "two equal axes are no pose");
  }

  // The lenses: head lamps 2.0 m ahead of the root, +-0.65 m, 0.65 m up; tail lamps 2.1 m behind.
  std::vector<std::array<double, 3>> lenses;
  const double spots[][3] = {{2.0, 0.65, 0.65}, {2.0, -0.65, 0.65}, {-2.1, 0.7, 0.8}, {-2.1, -0.7, 0.8}};
  for (const auto& s : spots) {
    std::array<double, 3> l{};
    LensesOf(pose, s[0], s[1], s[2], l.data());
    lenses.push_back(l);
  }
  hr::CarLamp lamp;
  {
    std::vector<double> flat;
    for (const auto& l : lenses) flat.insert(flat.end(), l.begin(), l.end());
    Check(hr::FrontLamp(pose, reinterpret_cast<const double(*)[3]>(flat.data()), lenses.size(), &lamp), "a car with lenses at both ends has headlights");
    Check(lamp.front == 2 && lamp.total == 4, "the two front lenses are the headlights");
    double expected[3];
    LensesOf(pose, 2.0 + hr::kNudge, 0.0, 0.65, expected);
    for (int c = 0; c < 3; ++c) CheckNear(lamp.position[c], expected[c], 1e-9, "the lamp is the mean of the front lenses, nudged forward");
    CheckNear(hr::Dot3d(lamp.forward, pose.back), -1.0, 1e-9, "forward is minus the third row");
    CheckNear(lamp.spread, 4.1, 1e-9, "the lenses spread over 4.1 m");
    CheckNear(lamp.half_span, 0.65, 1e-9, "the two front lenses are 1.3 m apart across the car: half the span is 0.65 m");
  }
  // The beam's axis dips below the forward.
  {
    float axis[3];
    hr::BeamAxis(lamp, axis);
    CheckNear(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2], 1.0, 1e-6, "the beam's axis is a unit vector");
    CheckNear(axis[1], -std::sin(hr::kDipDegrees * hr::kPi / 180.0), 1e-6, "it dips");
    const double h = 62.0 * hr::kPi / 180.0;
    CheckNear(axis[0] / std::cos(hr::kDipDegrees * hr::kPi / 180.0), std::sin(h), 1e-6, "its horizontal part is the car's forward (x)");
    CheckNear(axis[2] / std::cos(hr::kDipDegrees * hr::kPi / 180.0), std::cos(h), 1e-6, "... and z");
  }
  // Only the front lenses seen (the rear ones off the list): a single cluster ahead of the root is the front; behind it is not a headlight.
  {
    std::vector<double> flat;
    for (size_t i = 0; i < 2; ++i) flat.insert(flat.end(), lenses[i].begin(), lenses[i].end());
    hr::CarLamp one;
    Check(hr::FrontLamp(pose, reinterpret_cast<const double(*)[3]>(flat.data()), 2, &one) && one.front == 2, "a single cluster ahead of the root is the front");
    CheckNear(one.half_span, 0.65, 1e-9, "... and the same pair's half span");
    Check(hr::FrontLamp(pose, reinterpret_cast<const double(*)[3]>(flat.data()), 1, &one) && one.front == 1 && one.half_span == 0.0, "one front lens says nothing of the pair: no span");
    flat.clear();
    for (size_t i = 2; i < 4; ++i) flat.insert(flat.end(), lenses[i].begin(), lenses[i].end());
    Check(!hr::FrontLamp(pose, reinterpret_cast<const double(*)[3]>(flat.data()), 2, &one), "a single cluster behind the root is not a headlight");
    Check(!hr::FrontLamp(pose, reinterpret_cast<const double(*)[3]>(flat.data()), 0, &one), "no lenses, no lamp");
  }
  // A car that faces the other way (heading + 180): the same lenses relative to its own axes give the same answer.
  {
    double m2[4][4];
    CarMatrix(62.0 + 180.0, position, m2);
    hr::Pose pose2;
    Check(hr::PoseFromWorld(m2, &pose2), "the turned car is a pose");
    std::vector<double> flat;
    for (const auto& s : spots) {
      double l[3];
      LensesOf(pose2, s[0], s[1], s[2], l);
      flat.insert(flat.end(), l, l + 3);
    }
    hr::CarLamp turned;
    Check(hr::FrontLamp(pose2, reinterpret_cast<const double(*)[3]>(flat.data()), 4, &turned), "a turned car has headlights");
    CheckNear(hr::Dot3d(turned.forward, lamp.forward), -1.0, 1e-9, "and they point the other way");
  }
}

// ---------------------------------------------------------------------------
// the car's two headlamps (the shadow views are made from each lamp: local_shadow.h)
// ---------------------------------------------------------------------------

void CheckLampSpan() {
  // The offset to the right lamp: forward x up (the frame the poses have: right x up = back), half the span, a unit direction times it.
  const double forward_x[3] = {1, 0, 0}, up[3] = {0, 1, 0};
  float offset[3];
  Check(hr::LampOffset(forward_x, up, 0.6, offset), "an offset for a car facing +x");
  CheckNear(offset[0], 0.0, 1e-6, "no offset along the car");
  CheckNear(offset[1], 0.0, 1e-6, "none up");
  CheckNear(offset[2], 0.6, 1e-6, "the right lamp of a car facing +x is at +z (forward x up)");
  const double forward_z[3] = {0, 0, 1};
  Check(hr::LampOffset(forward_z, up, 0.6, offset), "an offset for a car facing +z");
  CheckNear(offset[0], -0.6, 1e-6, "facing +z, forward x up is -x");
  // The axes need not be unit or at right angles (a tracked car's): the offset is still the half span long, across the car.
  const double slanted[3] = {3.0, 0.4, 0.0};
  Check(hr::LampOffset(slanted, up, 0.5, offset), "a slanted forward");
  CheckNear(std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]), 0.5, 1e-6, "the offset is the half span long");
  CheckNear(offset[0] * slanted[0] + offset[1] * slanted[1] + offset[2] * slanted[2], 0.0, 1e-6, "... and at right angles to forward");
  const double parallel[3] = {0, 2, 0};
  Check(!hr::LampOffset(parallel, up, 0.6, offset) && offset[0] == 0.0f && offset[2] == 0.0f, "a forward along up makes no right axis: no offset");
  // A span that is not believed is the default: unknown (0), a lens mesh (0.05), a strip across the grille (2.0), not a number.
  for (const double measured : {0.0, 0.05, 2.0, std::nan("")}) {
    Check(hr::LampOffset(forward_x, up, measured, offset), "an offset");
    CheckNear(offset[2], hr::kDefaultHalfSpan, 1e-6, "a half span that is not believed is the default");
  }
  CheckNear(hr::BelievedHalfSpan(0.8), 0.8, 0.0, "a believable half span is kept");
  Check(hr::kMinHalfSpan < hr::kDefaultHalfSpan && hr::kDefaultHalfSpan < hr::kMaxHalfSpan, "the default is between the limits");
  // The game's own points: a pair across the car (1.2 m apart, as the player's) gives half the distance; one point gives none; the strongest of the group counts.
  {
    hr::GuestPoint a, b, c;
    a.pos[0] = 10.0f; a.pos[1] = 0.7f; a.pos[2] = 5.0f;
    b = a;
    b.pos[2] = 6.2f;
    c = a;
    c.pos[0] = 20.0f;  // another lamp, far away
    const double place[1][3] = {{10.0, 0.7, 5.6}};
    std::vector<bool> taken;
    auto matches = hr::AssignPoints({a, b, c}, place, 1, &taken);
    Check(matches[0].count == 2 && Near2(matches[0].span, 1.2, 1e-5), "two points near a lamp are its pair: 1.2 m apart");
    hr::CarLamp lamp;
    double on = 0.0;
    bool disagrees = false;
    hr::ApplyPoints(matches[0], &lamp, &on, &disagrees);
    Check(Near2(lamp.half_span, 0.6, 1e-5), "the lamp's half span is half of that");
    matches = hr::AssignPoints({a}, place, 1, &taken);
    hr::CarLamp lone;
    lone.half_span = 0.7;  // the lens lamp's own guess
    hr::ApplyPoints(matches[0], &lone, &on, &disagrees);
    Check(lone.half_span == 0.7, "one point says nothing of the pair: the lens lamp's own half span stays");
    // Points that no lens lamp took (a racer's, with a direction) make a lamp whose span is their own.
    a.vehicle = b.vehicle = 0x40;
    a.has_dir = b.has_dir = true;
    a.dir[0] = b.dir[0] = -1.0f;
    const auto made = hr::LampsFromPoints({a, b}, std::vector<bool>(2, false));
    Check(made.size() == 1 && Near2(made[0].lamp.half_span, 0.6, 1e-5), "a lamp made from a pair of points has their half span");
    const auto single = hr::LampsFromPoints({a}, std::vector<bool>(1, false));
    Check(single.size() == 1 && single[0].lamp.half_span == 0.0, "... and none from a single point");
  }
  // The tracker smooths the span over the sightings that knew it and keeps the last when one does not.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out;
    hr::Candidate car;
    car.lamp.position[0] = 0.0;
    car.lamp.position[1] = 0.65;
    car.lamp.position[2] = 10.0;
    car.lamp.forward[0] = 1.0;
    car.lamp.forward[1] = 0.0;
    car.lamp.forward[2] = 0.0;
    car.player = true;
    car.camera_distance = 10.0;
    car.lamp.half_span = 0.6;
    tracker.Observe(car);
    Check(tracker.EndFrame(1, &out) == 1 && Near2(out[0].half_span, 0.6, 1e-12), "the first sighting's span");
    car.lamp.half_span = 0.8;
    tracker.Observe(car);
    tracker.EndFrame(2, &out);
    Check(Near2(out[0].half_span, 0.8 * 0.6 + 0.2 * 0.8 - 0.0, 1e-12) || Near2(out[0].half_span, 0.64, 1e-12), "the next is smoothed: 0.8 of the old, 0.2 of the new");
    car.lamp.half_span = 0.0;
    tracker.Observe(car);
    tracker.EndFrame(3, &out);
    Check(Near2(out[0].half_span, 0.64, 1e-12), "a sighting with no span keeps the last");
  }
}

// ---------------------------------------------------------------------------
// the tracker
// ---------------------------------------------------------------------------

hr::Candidate Car(double x, double z, double distance, bool player = false, bool on_screen = true) {
  hr::Candidate c;
  c.lamp.position[0] = x;
  c.lamp.position[1] = 0.65;
  c.lamp.position[2] = z;
  c.lamp.forward[0] = 1.0;
  c.lamp.forward[1] = 0.0;
  c.lamp.forward[2] = 0.0;
  c.player = player;
  c.camera_distance = distance;
  c.on_screen = on_screen;
  return c;
}

void CheckTracker() {
  // Keys stay with a car that moves, and the output is one frame ahead by its velocity.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    uint32_t key = 0;
    for (uint64_t frame = 1; frame <= 12; ++frame) {
      tracker.Observe(Car(10.0 + double(frame), 0.0, 30.0));
      const size_t n = tracker.EndFrame(frame, &out);
      Check(n == 1, "one car, one light (frame " + std::to_string(frame) + ")");
      if (frame == 1) key = out[0].key;
      Check(out[0].key == key, "the key stays with the car (frame " + std::to_string(frame) + ")");
    }
    // After 12 frames at one metre a frame, the velocity is 1 m/frame and the position is one frame ahead.
    CheckNear(out[0].position[0], 10.0 + 12.0 + 1.0, 0.05, "the light is one frame ahead of the car");
    CheckNear(out[0].fade, 1.0, 1e-6, "the fade is complete after a dozen frames");
  }
  // The fade comes up over several frames, not at once.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    tracker.Observe(Car(0.0, 0.0, 20.0));
    tracker.EndFrame(1, &out);
    CheckNear(out[0].fade, 0.12, 1e-6, "the first frame fades in by a step");
    tracker.Observe(Car(0.0, 0.0, 20.0));
    tracker.EndFrame(2, &out);
    CheckNear(out[0].fade, 0.24, 1e-6, "the second by two");
  }
  // A car that is gone fades out, keeps moving on, and is forgotten.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    uint64_t frame = 1;
    for (; frame <= 12; ++frame) {
      tracker.Observe(Car(2.0 * double(frame), 0.0, 20.0));
      tracker.EndFrame(frame, &out);
    }
    float last = 2.0f;
    bool falls = true;
    size_t shown = 0;
    uint32_t left_at = 0;
    for (uint32_t i = 0; i < 40; ++i, ++frame) {
      const size_t n = tracker.EndFrame(frame, &out);
      if (n > 0) {
        shown = n;
        if (out[0].fade > last + 1e-6f) falls = false;
        last = out[0].fade;
        left_at = i;
      }
    }
    Check(shown == 1 && falls, "a car that is gone fades out");
    Check(left_at >= 6 && left_at <= 12, "the fade out takes about 8 frames (it was " + std::to_string(left_at) + ")");
    CheckNear(double(tracker.track_count()), 0.0, 0.0, "a car gone for good is forgotten");
  }
  // At most four lights, the player's first, whatever the number of cars.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    size_t n = 0;
    for (uint64_t frame = 1; frame <= 30; ++frame) {
      tracker.Observe(Car(0.0, 0.0, 10.0, true));
      for (int k = 1; k <= 9; ++k) tracker.Observe(Car(20.0 * k, 5.0, 20.0 + 10.0 * k));
      n = tracker.EndFrame(frame, &out);
      Check(n <= hr::kMaxLights, "never more than four lights");
    }
    Check(n == hr::kMaxLights, "ten cars give four lights");
    Check(out[0].player, "the player's light is first");
    // The nearest three others are the ones picked.
    uint32_t keys_distinct = 0;
    for (size_t i = 0; i < n; ++i) {
      for (size_t j = i + 1; j < n; ++j) keys_distinct += out[i].key != out[j].key;
    }
    Check(keys_distinct == 6, "four different lights");
    double farthest = 0.0;
    for (size_t i = 1; i < n; ++i) farthest = std::max(farthest, out[i].position[0]);
    Check(farthest <= 60.0 + 1.0, "the others are the three nearest (x 20, 40, 60)");
  }
  // The player is first even when the player's car is far and another is near.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    for (uint64_t frame = 1; frame <= 3; ++frame) {
      tracker.Observe(Car(0.0, 0.0, 90.0, true));
      tracker.Observe(Car(30.0, 0.0, 5.0));
      tracker.EndFrame(frame, &out);
    }
    Check(out[0].player, "the player first though far");
  }
  // Hysteresis: two cars compete for the last place; the one that holds it keeps it against a challenger that is somewhat nearer, and loses it to one
  // that is much nearer.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    uint64_t frame = 1;
    // Three fixed cars at 10, 12, 14 m and the holder of the last place (a car at 30 m); a challenger appears at 28 m.
    const auto run = [&](double challenger_distance, int frames) {
      for (int i = 0; i < frames; ++i, ++frame) {
        tracker.Observe(Car(100.0, 0.0, 10.0));
        tracker.Observe(Car(200.0, 0.0, 12.0));
        tracker.Observe(Car(300.0, 0.0, 14.0));
        tracker.Observe(Car(400.0, 0.0, 30.0));  // the holder
        if (challenger_distance > 0.0) tracker.Observe(Car(500.0, 0.0, challenger_distance));
        tracker.EndFrame(frame, &out);
      }
    };
    run(0.0, 20);
    const auto holds = [&](double x) {
      for (const hr::Tracked& t : out) {
        if (t.fade > 0.5f && std::fabs(t.position[0] - x) < 5.0) return true;
      }
      return false;
    };
    Check(holds(400.0), "the holder has the last place");
    run(28.0, 20);  // a challenger slightly nearer: not enough to take the place (score ratio about 1.1 against the bonus 1.5)
    Check(holds(400.0) && !holds(500.0), "a slightly nearer challenger does not take the place (hysteresis)");
    run(8.0, 30);   // a much nearer one: it does
    Check(holds(500.0) && !holds(400.0), "a much nearer challenger takes it");
  }
  // A car not on the screen scores lower than one that is, at the same distance.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    size_t n = 0;
    for (uint64_t frame = 1; frame <= 20; ++frame) {
      for (int k = 0; k < 4; ++k) tracker.Observe(Car(100.0 * (k + 1), 0.0, 40.0 + k, false, false));
      tracker.Observe(Car(900.0, 0.0, 45.0, false, true));
      n = tracker.EndFrame(frame, &out);
    }
    bool has_visible = false;
    for (size_t i = 0; i < n; ++i) has_visible |= std::fabs(out[i].position[0] - 900.0) < 5.0;
    Check(has_visible, "the car on the screen is picked over cars off it at about the same distance");
  }
  // The player and another car do not swap keys when close together: the player's track matches a player's candidate only.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    uint32_t player_key = 0, other_key = 0;
    for (uint64_t frame = 1; frame <= 10; ++frame) {
      tracker.Observe(Car(0.0, 0.0, 5.0, true));
      tracker.Observe(Car(3.0, 0.0, 8.0));
      tracker.EndFrame(frame, &out);
      for (const hr::Tracked& t : out) {
        if (t.fade <= 0.0f) continue;
        if (t.player) {
          if (frame == 1) player_key = t.key;
          Check(t.key == player_key, "the player's key stays");
        } else {
          if (frame == 1) other_key = t.key;
          Check(t.key == other_key, "the other car's key stays");
        }
      }
    }
    Check(player_key != other_key, "two cars, two keys");
  }
}

// ---------------------------------------------------------------------------
// the view's frustum, a beam's reach into it, and the tracker's rules at the frame's edge (HL-EDGE-FADE)
// ---------------------------------------------------------------------------

namespace li = mcla::gpu::light_influence;

// A world -> clip matrix, row vectors (clip = (p, 1) * M) as the game's, for a camera at `eye` looking along `forward` (world up is +y), a vertical field of view and an
// aspect: x and y in [-w, w], w the depth along the view. z is a made up finite column (the rules never read it).
struct TestCamera {
  double eye[3] = {};
  double forward[3] = {0, 0, 1};
  double fov_y_degrees = 50.0;
  double aspect = 16.0 / 9.0;
  double m[4][4] = {};
  li::ViewFrustum frustum;
  double tan_x = 0.0;
  double tan_y = 0.0;

  void Build() {
    double f[3] = {forward[0], forward[1], forward[2]};
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (double& v : f) v /= fl;
    const double up_world[3] = {0, 1, 0};
    double r[3] = {up_world[1] * f[2] - up_world[2] * f[1], up_world[2] * f[0] - up_world[0] * f[2], up_world[0] * f[1] - up_world[1] * f[0]};
    const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (double& v : r) v /= rl;
    const double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    tan_y = std::tan(fov_y_degrees * hr::kPi / 180.0 * 0.5);
    tan_x = tan_y * aspect;
    const double er = eye[0] * r[0] + eye[1] * r[1] + eye[2] * r[2];
    const double eu = eye[0] * u[0] + eye[1] * u[1] + eye[2] * u[2];
    const double ef = eye[0] * f[0] + eye[1] * f[1] + eye[2] * f[2];
    for (int i = 0; i < 3; ++i) {
      m[i][0] = r[i] / tan_x;
      m[i][1] = u[i] / tan_y;
      m[i][2] = f[i] * 0.001;
      m[i][3] = f[i];
    }
    m[3][0] = -er / tan_x;
    m[3][1] = -eu / tan_y;
    m[3][2] = -ef * 0.001;
    m[3][3] = -ef;
    li::MakeViewFrustum(m, eye, &frustum);
  }
};

void CheckFrustum() {
  TestCamera cam;
  cam.Build();
  Check(cam.frustum.valid, "a camera's matrix makes a frustum");
  for (int p = 0; p < 5; ++p) {
    const double* pl = cam.frustum.planes[p];
    CheckNear(std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]), 1.0, 1e-9, "plane " + std::to_string(p) + " is a unit plane");
  }
  const double axis10[3] = {0, 0, 10};
  const double left_edge_10[3] = {-10.0 * cam.tan_x, 0, 10};        // on the left plane, 10 m ahead
  const double right_out_10[3] = {10.0 * cam.tan_x + 3.0, 0, 10};   // 3 m beyond the right plane (not the nearest distance: the plane is tilted)
  const double behind[3] = {0, 0, -5};
  const double above_10[3] = {0, 10.0 * cam.tan_y + 2.0, 10};
  Check(li::InsideDistance(cam.frustum, axis10) > 3.0, "a point on the axis 10 m ahead is well inside");
  CheckNear(li::InsideDistance(cam.frustum, left_edge_10), 0.0, 1e-9, "a point on the left edge's plane is on the plane");
  Check(li::InsideDistance(cam.frustum, right_out_10) < -1.0 && li::InsideDistance(cam.frustum, behind) < -4.0 && li::InsideDistance(cam.frustum, above_10) < -1.0,
        "points beyond the right edge, behind the camera and above the top are outside");
  // The signed distance is metres: a point 2 m (measured along the plane's normal) beyond the left plane.
  {
    const double nx = cam.frustum.planes[0][0], nz = cam.frustum.planes[0][2];
    const double p[3] = {left_edge_10[0] - 2.0 * nx, 0, 10.0 - 2.0 * nz};
    CheckNear(li::InsideDistance(cam.frustum, p), -2.0, 1e-9, "a point 2 m out along the plane's normal is 2 m outside");
  }
  // NaN in the matrix or the eye: no frustum.
  {
    li::ViewFrustum bad;
    double m[4][4];
    std::memcpy(m, cam.m, sizeof(m));
    m[1][2] = std::nan("");
    Check(!li::MakeViewFrustum(m, cam.eye, &bad) && !bad.valid, "a matrix with a NaN makes no frustum");
    const double eye_bad[3] = {0, std::numeric_limits<double>::infinity(), 0};
    Check(!li::MakeViewFrustum(cam.m, eye_bad, &bad) && !bad.valid, "an eye that is not finite makes no frustum");
    double zero[4][4] = {};
    Check(!li::MakeViewFrustum(zero, cam.eye, &bad), "a matrix of zeros (a plane with no direction) makes no frustum");
  }
  // NdcExtent: 0 on the axis, 1 on the edge, more outside, infinity behind.
  CheckNear(li::NdcExtent(cam.frustum, axis10), 0.0, 1e-9, "the middle of the picture is 0");
  CheckNear(li::NdcExtent(cam.frustum, left_edge_10), 1.0, 1e-9, "the left edge is 1");
  {
    const double half[3] = {0.5 * 10.0 * cam.tan_x, 0, 10};
    CheckNear(li::NdcExtent(cam.frustum, half), 0.5, 1e-9, "half way to the edge is 0.5");
  }
  Check(li::NdcExtent(cam.frustum, right_out_10) > 1.0 && std::isinf(li::NdcExtent(cam.frustum, behind)), "outside the edge is more than 1, behind the camera infinite");
  // SphereFactor: 1 with the centre inside, 0 when the sphere only touches from outside, a straight line between.
  {
    CheckNear(li::SphereFactor(cam.frustum, axis10, 1.5), 1.0, 1e-12, "a sphere with its centre inside is 1");
    const double nx = cam.frustum.planes[0][0], nz = cam.frustum.planes[0][2];
    const double touching[3] = {left_edge_10[0] - 1.5 * nx, 0, 10.0 - 1.5 * nz};
    const double half_in[3] = {left_edge_10[0] - 0.75 * nx, 0, 10.0 - 0.75 * nz};
    CheckNear(li::SphereFactor(cam.frustum, touching, 1.5), 0.0, 1e-9, "a sphere that only touches the frustum from outside is 0");
    CheckNear(li::SphereFactor(cam.frustum, half_in, 1.5), 0.5, 1e-9, "a sphere with its centre half a radius out is 0.5");
    CheckNear(li::SphereFactor(cam.frustum, left_edge_10, 1.5), 1.0, 1e-9, "a sphere with its centre on the plane is 1");
  }
}

void CheckBeamReach() {
  TestCamera cam;
  cam.Build();
  const double outer = hr::kOuterDegrees * hr::kPi / 180.0;
  const double ahead[3] = {0, 0, 1};
  // The beam's strength is the shader's: the same fall-off and window (headlight_rig_rules.h LampScale on the axis, over the peak).
  {
    Check(li::kBeamFalloffDistance == double(hr::kFalloffDistance), "the beam's fall-off distance is the rig's");
    const hr::Lamp lamp = BeamAlongX();
    for (const double t : {3.0, 10.0, 30.0, 60.0}) {
      const double shader = double(hr::LampScale(lamp, WallFacingLamp(float(t)))) / double(hr::kPeak);
      CheckNear(li::BeamStrengthAt(t, double(hr::kRange)), shader, 0.01 * std::max(shader, 0.05), "the beam's strength at " + std::to_string(t) + " m is the shader's");
    }
    CheckNear(li::BeamStrengthAt(0.0, 70.0), 1.0, 1e-12, "the strength at the lamp is 1");
    CheckNear(li::BeamStrengthAt(70.0, 70.0), 0.0, 1e-12, "and 0 at the reach");
  }
  // A lamp in the view is 1 whatever way it points.
  {
    const double pos[3] = {0, 0.7, 20};
    const double away[3] = {0, 0, 1}, toward[3] = {0, 0, -1}, side[3] = {1, 0, 0};
    CheckNear(li::BeamReach(cam.frustum, pos, away, 70.0, outer), 1.0, 1e-9, "a lamp in the view pointing away is 1");
    CheckNear(li::BeamReach(cam.frustum, pos, toward, 70.0, outer), 1.0, 1e-9, "... toward the camera is 1");
    CheckNear(li::BeamReach(cam.frustum, pos, side, 70.0, outer), 1.0, 1e-9, "... across the view is 1");
  }
  // A lamp 6 m beyond the left edge at 20 m depth. Its beam across the view (toward +x) lights the picture; pointing out of it, it does not.
  {
    const double x_edge = -20.0 * cam.tan_x;
    const double pos[3] = {x_edge - 6.0, 0.7, 20};
    const double across[3] = {1, 0, 0}, outward[3] = {-1, 0, 0}, along[3] = {0, 0, 1}, back[3] = {0, 0, -1};
    const double r_across = li::BeamReach(cam.frustum, pos, across, 70.0, outer);
    const double r_out = li::BeamReach(cam.frustum, pos, outward, 70.0, outer);
    const double r_along = li::BeamReach(cam.frustum, pos, along, 70.0, outer);
    const double r_back = li::BeamReach(cam.frustum, pos, back, 70.0, outer);
    Check(r_across > 0.5, "a lamp off the left edge whose beam crosses the picture reaches it (" + std::to_string(r_across) + ")");
    Check(r_out < 0.05, "the same lamp pointing out of the picture reaches almost nothing (" + std::to_string(r_out) + ")");
    Check(r_along > 0.5 && r_along > 4.0 * r_out, "pointing along the road, which goes into the picture, it reaches it (" + std::to_string(r_along) + ")");
    Check(r_back < r_along, "pointing back toward the camera's side, less than along the road");
    // The reach falls as the lamp goes farther out (a smooth fall: no step above a tenth per 0.2 m).
    double previous = -1.0, largest = 0.0;
    for (double out = 0.0; out <= 30.0; out += 0.2) {
      const double p[3] = {x_edge - out, 0.7, 20};
      const double r = li::BeamReach(cam.frustum, p, outward, 70.0, outer);
      if (previous >= 0.0) {
        Check(r <= previous + 1e-12, "the reach of an outward beam only falls as the lamp goes out (" + std::to_string(out) + ")");
        largest = std::max(largest, previous - r);
      }
      previous = r;
    }
    Check(largest <= 0.15, "the reach has no step above 0.15 per 0.2 m (largest " + std::to_string(largest) + ")");
  }
  // A lamp behind the camera lights the air in front of it: pointing forward through the view it reaches it; pointing away it does not.
  {
    const double pos[3] = {0, 0.7, -10};
    const double r_fwd = li::BeamReach(cam.frustum, pos, ahead, 70.0, outer);
    const double back[3] = {0, 0, -1};
    const double r_back = li::BeamReach(cam.frustum, pos, back, 70.0, outer);
    Check(r_fwd > 0.5, "a lamp 10 m behind the camera, pointing forward, reaches the view (" + std::to_string(r_fwd) + ")");
    CheckNear(r_back, 0.0, 1e-9, "pointing away from the view it reaches nothing");
  }
  // The far end only: a lamp 60 m to the side pointing across: only the far end of the beam grazes the view.
  {
    const double pos[3] = {-70.0, 0.7, 10};
    const double across[3] = {1, 0, 0};
    const double r = li::BeamReach(cam.frustum, pos, across, 70.0, outer);
    Check(r > 0.0 && r < 0.3, "a lamp whose far beam only grazes the picture reaches it a little (" + std::to_string(r) + ")");
  }
  // Not a view: nothing. A beam with no reach: nothing.
  {
    li::ViewFrustum none;
    const double pos[3] = {0, 0, 5};
    CheckNear(li::BeamReach(none, pos, ahead, 70.0, outer), 0.0, 0.0, "no frustum, no reach");
    CheckNear(li::BeamReach(cam.frustum, pos, ahead, 0.0, outer), 0.0, 0.0, "a beam with no range reaches nothing");
  }
  // The importance's numbers and the distance fade.
  CheckNear(li::ReachWeight(0.0), 0.35, 1e-12, "a beam that lights nothing in the picture weighs 0.35 (the first version's off-screen share)");
  CheckNear(li::ReachWeight(li::kReachFull), 1.0, 1e-12, "a reach of 0.45 weighs 1");
  CheckNear(li::ReachWeight(1.0), 1.0, 1e-12, "a reach of 1 weighs 1");
  {
    bool monotone = true;
    double previous = 0.0;
    for (double r = 0.0; r <= 1.0; r += 0.01) {
      const double w = li::ReachWeight(r);
      monotone &= w >= previous - 1e-12;
      previous = w;
    }
    Check(monotone, "the weight never falls as the reach grows");
  }
  CheckNear(li::DistanceFade(50.0), 1.0, 1e-12, "a car 50 m away is lit in full");
  CheckNear(li::DistanceFade(90.0), 1.0, 1e-12, "... 90 m too");
  CheckNear(li::DistanceFade(115.0), 0.5, 1e-9, "... half at 115 m");
  CheckNear(li::DistanceFade(140.0), 0.0, 1e-12, "... none at 140 m");
  CheckNear(li::DistanceFade(300.0), 0.0, 1e-12, "... and none beyond");
}

// A car that drives along +z at `speed` metres a frame in the lane x = lane_x while the camera drives along +z at `camera_speed`: in the camera's frame the car comes
// alongside and falls behind. Its lamp is 2 m ahead of its origin at 0.65 m. `seen` is what the game draws: the car's lamp is seen while its place on the picture is
// within `seen_limit` of the middle (a car is drawn while its bounds are in the view: the lamp itself 0.2 beyond the edge is not).
struct PassScene {
  double lane_x = -4.0;
  double start_z = 40.0;
  double speed = 0.25;          // m a frame (15 m/s at 60 FPS)
  double camera_speed = 0.6;    // m a frame (36 m/s)
  double seen_limit = 1.1;      // the game draws the car while its lamp's NDC extent is under this (a car's bounds are larger than its lamp)
  bool heading_away = true;     // true: the car drives the camera's way (its beam lights the road ahead of it, which the camera also looks at)
};

void CheckTrackerView() {
  // A car being passed in the next lane. The camera drives along +z, the car too, slower: it falls behind and goes out through the picture's left edge with its
  // beam pointing ahead (+z): the road ahead of it is in the picture. The first version dropped its light within 8 frames of the game's last draw.
  {
    PassScene sc;
    double mean_fade_after = 0.0;
    int coasted_frames = 0;
    for (const bool use_view : {false, true}) {
      hr::Tracker tracker;
      std::array<hr::Tracked, hr::kMaxLights> out{};
      uint64_t frame = 0;
      uint64_t last_seen = 0;
      std::vector<float> fades;     // the car's light, every frame
      std::vector<bool> coasting;
      double largest_step = 0.0;
      for (frame = 1; frame <= 260; ++frame) {
        TestCamera cam;
        cam.eye[2] = sc.camera_speed * double(frame);
        cam.eye[1] = 1.3;
        cam.Build();
        const double car_z = sc.start_z + sc.speed * double(frame) + 2.0;  // the lamp
        const double lamp[3] = {sc.lane_x, 0.65, car_z};
        const double extent = li::NdcExtent(cam.frustum, lamp);
        const bool seen = extent < sc.seen_limit;
        if (seen) {
          last_seen = frame;
          hr::Candidate c;
          c.lamp.position[0] = lamp[0];
          c.lamp.position[1] = lamp[1];
          c.lamp.position[2] = lamp[2];
          c.lamp.forward[0] = 0.0;
          c.lamp.forward[1] = 0.0;
          c.lamp.forward[2] = 1.0;
          c.camera_distance = std::sqrt((lamp[0] - cam.eye[0]) * (lamp[0] - cam.eye[0]) + (lamp[2] - cam.eye[2]) * (lamp[2] - cam.eye[2]));
          c.on_screen = extent <= 1.3;
          c.guest = true;
          c.on = 1.0;
          tracker.Observe(c);
        }
        const size_t n = tracker.EndFrame(frame, &out, use_view ? &cam.frustum : nullptr);
        float fade = 0.0f;
        bool coast = false;
        for (size_t i = 0; i < n; ++i) {
          if (!out[i].player) {
            fade = out[i].fade;
            coast = out[i].coasting;
          }
        }
        if (!fades.empty()) largest_step = std::max(largest_step, double(std::fabs(fade - fades.back())));
        fades.push_back(fade);
        coasting.push_back(coast);
      }
      // frames after the game's last draw of the car: how long the light stays above one half
      size_t held = 0;
      for (size_t k = size_t(last_seen); k < fades.size() && fades[k] > 0.5f; ++k) {
        ++held;
      }
      const std::string which = use_view ? "with the view: " : "without the view (the first version): ";
      std::printf("  a car passed in the next lane, %slast drawn at frame %llu, its light above half for %zu frames after it, largest step %.3f\n", which.c_str(),
                  (unsigned long long)last_seen, held, largest_step);
      Check(last_seen > 20 && last_seen < 250, which + "the car is drawn for a while and then not (last at " + std::to_string(last_seen) + ")");
      Check(largest_step <= 0.12 + 1e-6, which + "the light's fade never steps by more than 0.12 a frame (" + std::to_string(largest_step) + ")");
      if (!use_view) {
        Check(held <= 8, which + "the light is gone within 8 frames of the car's last draw (" + std::to_string(held) + ")");
      } else {
        Check(held >= 25, which + "the light goes on for a second after the car's last draw while its beam is in the picture (" + std::to_string(held) + " frames)");
        for (size_t k = size_t(last_seen); k < fades.size(); ++k) {
          coasted_frames += coasting[k] ? 1 : 0;
        }
        Check(coasted_frames > 0, which + "the light is marked as coasting after the last draw");
        Check(fades.back() == 0.0f, which + "and it is gone at the end of the drive");
        mean_fade_after = 0.0;
        for (size_t k = size_t(last_seen); k < size_t(last_seen) + 20 && k < fades.size(); ++k) mean_fade_after += fades[k];
        Check(mean_fade_after / 20.0 > 0.9, which + "the first 20 frames after the last draw are in full (" + std::to_string(mean_fade_after / 20.0) + ")");
      }
    }
  }
  // The coast stops at coast_frames whatever the beam does: a car that stands just beyond the edge pointing along the road keeps its light for 90 frames, then the light goes.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    TestCamera cam;
    cam.eye[1] = 1.3;
    cam.Build();
    const double x_edge = -25.0 * cam.tan_x;
    uint64_t frame = 1;
    for (; frame <= 20; ++frame) {  // seen near the edge (extent 0.95) for 20 frames
      hr::Candidate c;
      c.lamp.position[0] = x_edge * 0.95;
      c.lamp.position[1] = 0.65;
      c.lamp.position[2] = 25.0;
      c.camera_distance = 25.0;
      c.guest = true;
      c.on = 1.0;
      tracker.Observe(c);
      tracker.EndFrame(frame, &out, &cam.frustum);
    }
    uint64_t gone_at = 0;
    for (; frame <= 160; ++frame) {
      const size_t n = tracker.EndFrame(frame, &out, &cam.frustum);
      if (n == 0 && gone_at == 0) gone_at = frame - 20;
    }
    std::printf("  a car that stood at the edge and was no longer drawn: its light was gone %llu frames later\n", (unsigned long long)gone_at);
    Check(gone_at >= 90 && gone_at <= 105, "a car that is not drawn any more keeps its light for coast_frames (90) and a few frames of fade (" + std::to_string(gone_at) + ")");
  }
  // A car that vanishes in the middle of the picture (not at the edge: a despawn, a hiding) does not coast: it fades out as before.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    TestCamera cam;
    cam.eye[1] = 1.3;
    cam.Build();
    uint64_t frame = 1;
    for (; frame <= 20; ++frame) {
      hr::Candidate c;
      c.lamp.position[0] = 1.0;
      c.lamp.position[1] = 0.65;
      c.lamp.position[2] = 25.0;
      c.camera_distance = 25.0;
      c.guest = true;
      c.on = 1.0;
      tracker.Observe(c);
      tracker.EndFrame(frame, &out, &cam.frustum);
    }
    uint64_t gone_at = 0;
    for (; frame <= 60; ++frame) {
      const size_t n = tracker.EndFrame(frame, &out, &cam.frustum);
      if (n == 0 && gone_at == 0) gone_at = frame - 20;
    }
    Check(gone_at >= 6 && gone_at <= 12, "a car that vanishes in the middle of the picture fades out in about 8 frames (" + std::to_string(gone_at) + ")");
  }
  // A car that left through the edge with its beam pointing OUT of the picture goes out with the beam: the reach falls to nothing and so does the light, by steps of at most the fade's.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    TestCamera cam;
    cam.eye[1] = 1.3;
    cam.Build();
    const double x_edge = -25.0 * cam.tan_x;
    uint64_t frame = 1;
    double lamp_x = x_edge * 0.9;
    for (; frame <= 20; ++frame, lamp_x -= 0.2) {  // drives out to the left at 0.2 m a frame, pointing left
      hr::Candidate c;
      c.lamp.position[0] = lamp_x;
      c.lamp.position[1] = 0.65;
      c.lamp.position[2] = 25.0;
      c.lamp.forward[0] = -1.0;
      c.lamp.forward[2] = 0.0;
      c.camera_distance = 25.0;
      c.guest = true;
      c.on = 1.0;
      tracker.Observe(c);
      tracker.EndFrame(frame, &out, &cam.frustum);
    }
    uint64_t gone_at = 0;
    float previous = out[0].fade;
    double largest = 0.0;
    for (; frame <= 160; ++frame) {
      const size_t n = tracker.EndFrame(frame, &out, &cam.frustum);
      if (n > 0) {
        largest = std::max(largest, double(std::fabs(out[0].fade - previous)));
        previous = out[0].fade;
      }
      if (n == 0 && gone_at == 0) gone_at = frame - 20;
    }
    std::printf("  a car driving out of the picture with its beam pointing out of it: gone %llu frames after its last draw\n", (unsigned long long)gone_at);
    Check(gone_at > 0 && gone_at < 60, "a car whose beam leaves the picture with it goes out (" + std::to_string(gone_at) + " frames)");
    Check(largest <= 0.12 + 1e-6, "... by steps no larger than the fade's (" + std::to_string(largest) + ")");
  }
  // The ranking at the edge: six cars for the player's place and three more. X and Y are in the middle of the picture at 30 m. E stands off the RIGHT edge at 40 m with its beam
  // across the picture, F off the LEFT edge at 40 m with its beam pointing out of it: the same distance, and both lamps off the screen. The first version ranked them alike
  // (0.35 each: the lamp is off the screen) and took F, the lower key, for the last place: a car at the edge whose beam lights the picture lost to one that lights nothing in it.
  // With the view E is worth its beam and F is not.
  for (const bool use_view : {false, true}) {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    TestCamera cam;
    cam.eye[1] = 1.3;
    cam.Build();
    const double x_edge = 40.0 * cam.tan_x;  // the right edge's x at 40 m
    size_t n = 0;
    for (uint64_t frame = 1; frame <= 30; ++frame) {
      const auto make = [&](double x, double z, double fx, double fz, bool on_screen) {
        hr::Candidate c;
        c.lamp.position[0] = x;
        c.lamp.position[1] = 0.65;
        c.lamp.position[2] = z;
        c.lamp.forward[0] = fx;
        c.lamp.forward[2] = fz;
        c.camera_distance = std::sqrt(x * x + z * z);
        c.on_screen = on_screen;
        return c;
      };
      hr::Candidate player = make(0.0, 2.0, 0.0, 1.0, true);
      player.player = true;
      tracker.Observe(player);
      tracker.Observe(make(-x_edge - 8.0, 40.0, -1.0, 0.0, false));  // F (the lower key): off the left edge, pointing out of the picture
      tracker.Observe(make(x_edge + 8.0, 40.0, -1.0, 0.0, false));   // E: off the right edge, pointing across the picture
      tracker.Observe(make(2.0, 30.0, 0.0, 1.0, true));              // X
      tracker.Observe(make(-3.0, 31.0, 0.0, 1.0, true));             // Y
      n = tracker.EndFrame(frame, &out, use_view ? &cam.frustum : nullptr);
    }
    bool has_e = false, has_f = false;
    for (size_t i = 0; i < n; ++i) {
      has_e |= out[i].position[0] > 20.0 && out[i].fade > 0.5f;
      has_f |= out[i].position[0] < -20.0 && out[i].fade > 0.5f;
    }
    const std::string which = use_view ? "with the view: " : "without the view (the first version): ";
    Check(n == hr::kMaxLights, which + "four lights");
    if (use_view) {
      Check(has_e && !has_f, which + "the car at the edge whose beam crosses the picture is lit, the one whose beam points out of it is not");
    } else {
      Check(has_f && !has_e, which + "the two cars off the screen tie and the lower key wins: the car that lights the picture loses");
    }
  }
  // A car that coasts gives its place to cars that are in the picture: the player's and three cars in view take the four places, and the car that left the picture (its beam still
  // in the view) is dropped from the set, by fade steps; with only two other cars in view it keeps its place.
  for (const int others : {2, 3}) {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    TestCamera cam;
    cam.eye[1] = 1.3;
    cam.Build();
    const double x_edge = -25.0 * cam.tan_x;
    const auto make = [&](double x, double z, bool player) {
      hr::Candidate c;
      c.lamp.position[0] = x;
      c.lamp.position[1] = 0.65;
      c.lamp.position[2] = z;
      c.camera_distance = std::sqrt(x * x + z * z);
      c.player = player;
      c.guest = true;
      c.on = 1.0;
      return c;
    };
    uint64_t frame = 1;
    for (; frame <= 20; ++frame) {  // the car at the left edge is seen with the others
      tracker.Observe(make(0.0, 3.0, true));
      tracker.Observe(make(x_edge * 0.95, 25.0, false));
      for (int k = 0; k < others; ++k) tracker.Observe(make(2.0 + 3.0 * k, 20.0 + 4.0 * k, false));
      tracker.EndFrame(frame, &out, &cam.frustum);
    }
    bool coasting_listed = false;
    for (int i = 0; i < 40; ++i, ++frame) {  // then it is not drawn any more
      tracker.Observe(make(0.0, 3.0, true));
      for (int k = 0; k < others; ++k) tracker.Observe(make(2.0 + 3.0 * k, 20.0 + 4.0 * k, false));
      const size_t n = tracker.EndFrame(frame, &out, &cam.frustum);
      if (i >= 25) {
        coasting_listed = false;
        for (size_t j = 0; j < n; ++j) coasting_listed |= out[j].coasting && out[j].fade > 0.05f;
      }
    }
    if (others == 2) {
      Check(coasting_listed, "with room (the player and two cars in view) the car that left the picture keeps its place");
    } else {
      Check(!coasting_listed, "with the player and three cars in view the car that left the picture gives its place up");
    }
  }
  // Without a view the first version's rule is intact: the car off the screen scores 0.35 (the existing checks above) and nothing coasts.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    for (uint64_t frame = 1; frame <= 20; ++frame) {
      tracker.Observe(Car(10.0, 0.0, 20.0));
      tracker.EndFrame(frame, &out);
    }
    uint64_t gone_at = 0;
    for (uint64_t frame = 21; frame <= 80; ++frame) {
      const size_t n = tracker.EndFrame(frame, &out);
      if (n == 0 && gone_at == 0) gone_at = frame - 20;
      Check(tracker.coasting_count() == 0, "without a view nothing coasts");
    }
    Check(gone_at >= 6 && gone_at <= 12, "without a view a car that is not drawn fades out in about 8 frames (" + std::to_string(gone_at) + ")");
  }
  // The distance fade: a lone car at 115 m settles at half (the target is the fade of its distance), at 150 m it is not lit, at 60 m in full; continuous.
  {
    for (const double distance : {60.0, 115.0, 150.0}) {
      hr::Tracker tracker;
      std::array<hr::Tracked, hr::kMaxLights> out{};
      TestCamera cam;
      cam.Build();
      size_t n = 0;
      for (uint64_t frame = 1; frame <= 40; ++frame) {
        hr::Candidate c;
        c.lamp.position[0] = 0.0;
        c.lamp.position[1] = 0.65;
        c.lamp.position[2] = distance;
        c.camera_distance = distance;
        tracker.Observe(c);
        n = tracker.EndFrame(frame, &out, &cam.frustum);
      }
      const double want = li::DistanceFade(distance);
      const double got = n > 0 ? double(out[0].fade) : 0.0;
      CheckNear(got, want, 2e-4, "a lone car " + std::to_string(int(distance)) + " m away settles at the distance fade (" + std::to_string(want) + ")");
    }
  }
  // The first-version path still agrees with the second for a car in the middle of the picture: the same output as the view's.
  {
    hr::Tracker a, b;
    std::array<hr::Tracked, hr::kMaxLights> oa{}, ob{};
    TestCamera cam;
    cam.eye[1] = 1.3;
    cam.Build();
    for (uint64_t frame = 1; frame <= 20; ++frame) {
      hr::Candidate c;
      c.lamp.position[0] = 1.0;
      c.lamp.position[1] = 0.65;
      c.lamp.position[2] = 20.0;
      c.camera_distance = std::sqrt(1.0 + 18.7 * 18.7);
      a.Observe(c);
      b.Observe(c);
      a.EndFrame(frame, &oa);
      b.EndFrame(frame, &ob, &cam.frustum);
    }
    CheckNear(double(oa[0].fade), double(ob[0].fade), 1e-6, "a car in the middle of the picture is lit the same with or without the view");
  }
}

// ---------------------------------------------------------------------------
// the shadow hook
// ---------------------------------------------------------------------------

void CheckShadow() {
  larecomp::gpu::LocalShadowFrame frame;
  frame.count = 3;
  frame.views[0].valid = 0;
  frame.views[0].light_index = 0;
  frame.views[1].valid = 1;
  frame.views[1].light_index = 2;
  frame.views[2].valid = 1;
  frame.views[2].light_index = 100;  // a lamp's, later
  const auto of = hr::ShadowViewOfLight(&frame, 3);
  Check(of[0] == hr::kNoView && of[1] == hr::kNoView && of[2] == 1 && of[3] == hr::kNoView, "a valid view names its light; an invalid one and a lamp's do not");
  const auto none = hr::ShadowViewOfLight(nullptr, 3);
  Check(none[0] == hr::kNoView && none[3] == hr::kNoView, "no frame, no views");
  const auto beyond = hr::ShadowViewOfLight(&frame, 2);
  Check(beyond[2] == hr::kNoView, "a view of a light that is not in the set (index 2 of 2 lights) is ignored");
  // A light is a car's and has up to two views, one for each of its headlamps: the first valid view that names it, then the second; a third is not read.
  {
    larecomp::gpu::LocalShadowFrame lamps;
    lamps.count = 5;
    const uint32_t names[5] = {1, 1, 0, 1, 0};
    const uint32_t valid[5] = {1, 1, 1, 1, 0};
    for (uint32_t v = 0; v < 5; ++v) {
      lamps.views[v].light_index = names[v];
      lamps.views[v].valid = valid[v];
    }
    const hr::LightViews of2 = hr::ShadowViewsOfLight(&lamps, 3);
    Check(of2.first[1] == 0 && of2.second[1] == 1, "light 1 has its first two views, 0 and 1 (a third is not read)");
    Check(of2.first[0] == 2 && of2.second[0] == hr::kNoView, "light 0 has one valid view (the invalid one is skipped)");
    Check(of2.first[2] == hr::kNoView && of2.second[2] == hr::kNoView, "a light that no view names has none");
    const auto first_only = hr::ShadowViewOfLight(&lamps, 3);
    Check(first_only[1] == 0 && first_only[0] == 2, "the one-view table is the first view of each light");
    Check(hr::ShadowViewsOfLight(nullptr, 3).first[0] == hr::kNoView && hr::ShadowViewsOfLight(nullptr, 3).second[0] == hr::kNoView, "no frame, no views");
  }

  // A matrix that maps x in [0, 10] to u in [0, 1], y in [0, 10] to v in [0, 1] and z in [0, 20] to depth [0, 1], w = 1 (row-major, out = M p).
  larecomp::gpu::LocalShadowView view{};
  view.world_to_shadow[0] = 0.1f;
  view.world_to_shadow[5] = 0.1f;
  view.world_to_shadow[10] = 0.05f;
  view.world_to_shadow[15] = 1.0f;
  view.atlas_rect[0] = 0.0f;
  view.atlas_rect[1] = 0.0f;
  view.atlas_rect[2] = 1.0f;
  view.atlas_rect[3] = 1.0f;
  float uv[2];
  float depth = -1.0f;
  const float p[3] = {5.0f, 2.5f, 10.0f};
  Check(hr::ProjectToShadow(view, p, uv, &depth), "a point inside the view projects");
  CheckNear(uv[0], 0.5, 1e-6, "u");
  CheckNear(uv[1], 0.25, 1e-6, "v");
  CheckNear(depth, 0.5, 1e-6, "depth");
  const float outside[3] = {15.0f, 2.5f, 10.0f};
  Check(!hr::ProjectToShadow(view, outside, uv, &depth), "a point outside the rectangle is not covered (lit)");
  const float too_deep[3] = {5.0f, 2.5f, 25.0f};
  Check(!hr::ProjectToShadow(view, too_deep, uv, &depth), "a point beyond the depth range is not covered");
  // A perspective: w = z (row 3 = 0 0 1 0); u = x / z.
  larecomp::gpu::LocalShadowView persp{};
  persp.world_to_shadow[0] = 1.0f;
  persp.world_to_shadow[5] = 1.0f;
  persp.world_to_shadow[10] = 0.5f;  // z' = 0.5 z, depth = 0.5 z / z = 0.5
  persp.world_to_shadow[14] = 1.0f;  // w = z
  persp.atlas_rect[2] = 1.0f;
  persp.atlas_rect[3] = 1.0f;
  const float q[3] = {2.0f, 1.0f, 4.0f};
  Check(hr::ProjectToShadow(persp, q, uv, &depth), "a perspective view projects");
  CheckNear(uv[0], 0.5, 1e-6, "perspective u = x / w");
  CheckNear(uv[1], 0.25, 1e-6, "perspective v = y / w");
  const float behind[3] = {2.0f, 1.0f, -4.0f};
  Check(!hr::ProjectToShadow(persp, behind, uv, &depth), "a point behind the view is not covered");
  // The compare: lit when the stored depth is not nearer than the receiver's, with the bias moving the receiver toward the lamp.
  Check(hr::ShadowTap(0.5f, 0.5f, 0.0f) == 1.0f, "equal depths are lit (the game's >=)");
  Check(hr::ShadowTap(0.4f, 0.5f, 0.0f) == 0.0f, "a nearer caster shadows");
  Check(hr::ShadowTap(0.49f, 0.5f, 0.02f) == 1.0f, "the bias lets the receiver's own surface through");
}


// ---------------------------------------------------------------------------
// the block the shaders read
// ---------------------------------------------------------------------------

// Four lamps like `a`, in a row.
const hr::Lamp* many4(const hr::Lamp& a) {
  static hr::Lamp four[4];
  for (int i = 0; i < 4; ++i) {
    four[i] = a;
    four[i].position[0] = a.position[0] + 10.0f * float(i);
  }
  return four;
}

void CheckBlock() {
  Check(sizeof(hr::HeadlightConstants) == 1072, "the block is 1072 bytes (672 before the lamps' views: eight views and a second view table)");
  Check(offsetof(hr::HeadlightConstants, shadow_of) == 256 && offsetof(hr::HeadlightConstants, shadow_of_b) == 272 && offsetof(hr::HeadlightConstants, atlas) == 288 &&
            offsetof(hr::HeadlightConstants, shadow) == 304 && sizeof(hr::HeadlightConstants::shadow) == 768,
        "the views follow the two view tables and the atlas slots: c42 shadow_of, c43 shadow_of_b, c44 atlas, c45 the first view");
  hr::Lamp a = BeamAlongX();
  a.position[0] = 5.0f;
  a.position[1] = 0.65f;
  a.position[2] = 1.0f;
  a.color[0] = 0.9f;
  a.color[1] = 0.8f;
  a.color[2] = 0.7f;
  hr::Lamp b = a;
  b.position[0] = 80.0f;
  b.range = 30.0f;
  const hr::Lamp lamps[2] = {a, b};
  const float camera[3] = {1.0f, 2.0f, 3.0f};
  const hr::HeadlightConstants none = hr::MakeConstants(lamps, 2, camera, nullptr, 7, 8);
  CheckNear(none.header[0], 2.0, 0.0, "the count");
  CheckNear(none.camera[2], 3.0, 0.0, "the camera");
  CheckNear(none.lights[0][0], 5.0, 0.0, "light 0 position x");
  CheckNear(none.lights[0][3], hr::kRange, 0.0, "light 0 reach");
  CheckNear(none.lights[1][0], 1.0, 0.0, "light 0 axis x");
  CheckNear(none.lights[1][3], a.cos_outer, 0.0, "light 0 cos outer");
  CheckNear(none.lights[2][1], 0.8, 1e-6, "light 0 colour g");
  CheckNear(none.lights[2][3], a.cos_inner, 0.0, "light 0 cos inner");
  CheckNear(none.lights[3][0], 80.0, 0.0, "light 1 position x");
  CheckNear(none.lights[3][3], 30.0, 0.0, "light 1 reach");
  CheckNear(none.bounds_lo[0], 5.0 - hr::kRange, 1e-4, "the box's low x is the first light's");
  CheckNear(none.bounds_hi[0], 110.0, 1e-4, "the box's high x is the second light's");
  Check(none.shadow_of[0] == hr::kNoView && none.atlas[0] == hr::kNoSlot, "no shadows: no views and no atlas");
  const hr::HeadlightConstants zero = hr::MakeConstants(lamps, 0, camera, nullptr, 7, 8);
  Check(zero.bounds_lo[0] > zero.bounds_hi[0], "no lights: a box that holds no point");
  // The shadow views are copied with their atlas when one names a light of the set.
  larecomp::gpu::LocalShadowFrame frame;
  frame.count = 1;
  frame.views[0].valid = 1;
  frame.views[0].light_index = 1;
  frame.views[0].depth_bias = 0.01f;
  frame.views[0].world_to_shadow[15] = 1.0f;
  const hr::HeadlightConstants lit = hr::MakeConstants(lamps, 2, camera, &frame, 7, 8);
  Check(lit.shadow_of[1] == 0 && lit.shadow_of[0] == hr::kNoView, "light 1 has view 0");
  Check(lit.atlas[0] == 7 && lit.atlas[1] == 8, "the atlas slots");
  CheckNear(lit.shadow[0].depth_bias, 0.01, 1e-9, "the view is copied");
  frame.views[0].valid = 0;
  const hr::HeadlightConstants invalid = hr::MakeConstants(lamps, 2, camera, &frame, 7, 8);
  Check(invalid.atlas[0] == hr::kNoSlot, "an invalid view binds no atlas");
  // A light with TWO views (a car's two headlamps): the first is shadow_of, the second shadow_of_b, and the views are copied up to eight.
  {
    larecomp::gpu::LocalShadowFrame two;
    two.count = 4;
    for (uint32_t v = 0; v < 4; ++v) {
      two.views[v].valid = 1;
      two.views[v].light_index = v / 2;  // lights 0 and 1 have two views each
      two.views[v].world_to_shadow[0] = float(v + 1);
      two.views[v].atlas_rect[2] = 1.0f;
    }
    const hr::HeadlightConstants both = hr::MakeConstants(lamps, 2, camera, &two, 7, 8);
    Check(both.shadow_of[0] == 0 && both.shadow_of_b[0] == 1 && both.shadow_of[1] == 2 && both.shadow_of_b[1] == 3, "each light has its two views, in the order the producer gave them");
    Check(both.shadow_of[2] == hr::kNoView && both.shadow_of_b[2] == hr::kNoView && both.shadow_of_b[3] == hr::kNoView, "a light that is not in the set has none");
    CheckNear(both.shadow[3].world_to_shadow[0], 4.0, 0.0, "the fourth view is copied (the block held four before)");
    larecomp::gpu::LocalShadowFrame eight;
    eight.count = 8;
    for (uint32_t v = 0; v < 8; ++v) {
      eight.views[v].valid = 1;
      eight.views[v].light_index = v / 2;
      eight.views[v].world_to_shadow[0] = float(v + 1);
    }
    const hr::HeadlightConstants all = hr::MakeConstants(many4(a), 4, camera, &eight, 7, 8);
    bool ok = true;
    for (uint32_t light = 0; light < 4; ++light) {
      ok = ok && all.shadow_of[light] == 2 * light && all.shadow_of_b[light] == 2 * light + 1;
    }
    for (uint32_t v = 0; v < 8; ++v) {
      ok = ok && all.shadow[v].world_to_shadow[0] == float(v + 1);
    }
    Check(ok, "four cars with two views each: the eight views are in the block, each light names its two");
    larecomp::gpu::LocalShadowFrame one;
    one.count = 1;
    one.views[0].valid = 1;
    one.views[0].light_index = 0;
    const hr::HeadlightConstants single = hr::MakeConstants(lamps, 2, camera, &one, 7, 8);
    Check(single.shadow_of[0] == 0 && single.shadow_of_b[0] == hr::kNoView, "a light with one view (the car's, not its lamps') has no second");
  }
  // More than four lamps are cut to four.
  hr::Lamp many[6];
  for (auto& lamp : many) lamp = a;
  CheckNear(hr::MakeConstants(many, 6, camera, nullptr, 0, 0).header[0], 4.0, 0.0, "at most four lights");
  // HSQ-PCSS: the shadow lookup's two settings ride in the block's header (y = the lamp's radius in metres, z = the gathers a side; 0 in both: the shader's own defaults)
  {
    namespace sf = larecomp::gpu::shadow_filter;
    hr::HeadlightConstants block = hr::MakeConstants(lamps, 2, camera, nullptr, 7, 8);
    Check(block.header[1] == 0.0f && block.header[2] == 0.0f, "a block made without the settings has the lookup's defaults (header y and z are 0)");
    hr::SetShadowKernel(&block, sf::Params{});
    Check(block.header[1] == 0.0f && block.header[2] == 0.0f, "the default Params leave the header at 0: the block's bytes are what they were before the settings");
    hr::SetShadowKernel(&block, sf::MakeParams(0.12, 3));
    CheckNear(block.header[1], 0.12, 1e-6, "header y is the lamp's radius in metres");
    CheckNear(block.header[2], 3.0, 0.0, "header z is the gathers a side");
    CheckNear(block.header[0], 2.0, 0.0, "and the light count is where it was");
    hr::SetShadowKernel(&block, sf::Params{9.0f, 9});
    CheckNear(block.header[1], double(sf::kMaxLampRadius), 1e-6, "a radius out of range is held to the settings' largest");
    CheckNear(block.header[2], double(sf::kMaxGathers), 0.0, "and the gathers too");
    hr::SetShadowKernel(&block, sf::Params{0.07f, 2});
    Check(block.header[1] == 0.0f && block.header[2] == 0.0f, "the default again: zero");
    hr::SetShadowKernel(&block, sf::Params{0.07f, 3});
    CheckNear(block.header[1], 0.07, 1e-6, "the default radius with a larger footprint is carried (the radius is not 0)");
  }
}


// ---------------------------------------------------------------------------
// which way a car faces: the convention, pinned (the headlights run of 2026-11-20: the light landed BEHIND the player's car)
// ---------------------------------------------------------------------------
// A car's DRAW does not carry the car's turn in gWorld: gWorld (c0-c3) is the root translation only, the rotation is the identity for every car, and the turn is in the
// bone palette's rotation (rows = the images of the model's x, y, z axes; the model's +z is the car's back). The rig read gWorld's rows as the car's frame, so every car
// "faced" -z and the lamps of a car that faced +x shone behind it. These checks build a car the way the game draws it.

// The bone of a car facing `forward` (a horizontal unit vector) on the world's y up: the row-vector palette `palette` (rows right, up, back; row 3 the bone's offset
// from the root) and the draw's gWorld `world` (identity rotation, the root's translation).
void DrawMatrices(const double forward[3], const double root[3], double palette[4][4], double world[4][4]) {
  const double up[3] = {0.0, 1.0, 0.0};
  // right = forward x up (a right-handed world: looking along -z with y up, the right is +x)
  const double right[3] = {forward[1] * up[2] - forward[2] * up[1], forward[2] * up[0] - forward[0] * up[2], forward[0] * up[1] - forward[1] * up[0]};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      palette[r][c] = 0.0;
      world[r][c] = r == c ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 3; ++c) {
    palette[0][c] = right[c];
    palette[1][c] = up[c];
    palette[2][c] = -forward[c];
    world[3][c] = root[c];
  }
}

// A model-space point (x right, y up, z back) through the bone, then gWorld: [p 1] * palette * world.
void ModelToWorld(const double palette[4][4], const double world[4][4], const double p[3], double out[3]) {
  for (int c = 0; c < 3; ++c) {
    const double bone = p[0] * palette[0][c] + p[1] * palette[1][c] + p[2] * palette[2][c] + palette[3][c];
    out[c] = bone + world[3][c];  // gWorld is a translation
  }
}

void CheckFacing() {
  const double root[3] = {100.0, 2.0, 50.0};
  const double plus_x[3] = {1.0, 0.0, 0.0};
  double palette[4][4], world[4][4];
  DrawMatrices(plus_x, root, palette, world);

  // THE BUG, pinned: the draw's gWorld is a pose that faces -z whatever the car does, so it must not be the source of the facing.
  {
    hr::Pose from_world;
    Check(hr::PoseFromWorld(world, &from_world), "the draw's gWorld is a valid frame (the identity)");
    CheckNear(-from_world.back[2], -1.0, 1e-12, "gWorld's rows say the car faces -z, for a car that faces +x (this is why they are not read)");
    CheckNear(-from_world.back[0], 0.0, 1e-12, "... and nothing along x");
  }

  // The bone's frame says +x: a car facing +x lights +x.
  hr::Pose pose;
  Check(hr::PoseFromBone(palette, root, &pose), "the bone of a car is a pose");
  CheckNear(pose.position[0], root[0], 1e-12, "the pose's place is the root (gWorld's translation)");
  CheckNear(-pose.back[0], 1.0, 1e-12, "a car facing +x: its forward (minus the back row) is +x");
  CheckNear(-pose.back[2], 0.0, 1e-12, "... and nothing along z");
  // Not a pose: a bone with no rotation, a NaN.
  {
    double flat[4][4] = {};
    hr::Pose p;
    Check(!hr::PoseFromBone(flat, root, &p), "an empty bone is no pose");
    double bad[4][4];
    std::copy(&palette[0][0], &palette[0][0] + 16, &bad[0][0]);
    bad[2][1] = std::nan("");
    Check(!hr::PoseFromBone(bad, root, &p), "a bone with a NaN is no pose");
  }

  // The lenses as the game's lamp meshes put them (model space, +z back): head lamps 2.0 m ahead (model z -2.0), tail lamps 2.1 m behind (model z +2.1).
  std::vector<double> flat;
  const double model_spots[][3] = {{0.65, 0.65, -2.0}, {-0.65, 0.65, -2.0}, {0.7, 0.8, 2.1}, {-0.7, 0.8, 2.1}};
  for (const auto& spot : model_spots) {
    double w[3];
    ModelToWorld(palette, world, spot, w);
    Check(hr::LensOffRoot(root, w), "a lamp mesh at a car's end is off its root");
    flat.insert(flat.end(), w, w + 3);
  }
  // The head lamps are at +x of the root, the tail lamps at -x.
  Check(flat[0] > root[0] + 1.9 && flat[3] > root[0] + 1.9 && flat[6] < root[0] - 2.0 && flat[9] < root[0] - 2.0, "the model's -z end is the front: +x for this car");
  hr::CarLamp lamp;
  Check(hr::FrontLamp(pose, reinterpret_cast<const double(*)[3]>(flat.data()), 4, &lamp), "the car has headlights");
  Check(lamp.front == 2 && lamp.position[0] > root[0] + 2.0, "the headlights are the two lenses ahead, in front of the root along +x");
  CheckNear(lamp.forward[0], 1.0, 1e-12, "the lamp's forward is +x");
  float axis[3];
  hr::BeamAxis(lamp, axis);
  CheckNear(axis[0], std::cos(hr::kDipDegrees * hr::kPi / 180.0), 1e-6, "the beam goes along +x (dipped)");
  CheckNear(axis[2], 0.0, 1e-6, "... not along z");

  // The tracker keeps it (what Publish makes the beam from), and the shader's term lights the road AHEAD of the car and not behind it.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    hr::Candidate c;
    c.lamp = lamp;
    c.player = true;
    c.camera_distance = 6.0;
    tracker.Observe(c);
    Check(tracker.EndFrame(1, &out) == 1, "one light");
    CheckNear(out[0].forward[0], 1.0, 1e-12, "the tracked light faces +x");
    hr::CarLamp made;
    for (int i = 0; i < 3; ++i) {
      made.forward[i] = out[0].forward[i];
      made.up[i] = out[0].up[i];
    }
    float beam[3];
    hr::BeamAxis(made, beam);
    hr::Lamp light;
    for (int i = 0; i < 3; ++i) {
      light.position[i] = float(out[0].position[i]);
      light.direction[i] = beam[i];
    }
    light.cos_outer = hr::CosOfDegrees(hr::kOuterDegrees);
    light.cos_inner = hr::CosOfDegrees(hr::kInnerDegrees);
    hr::Surface ahead;  // the road 8 m ahead of the lamp
    ahead.position[0] = light.position[0] + 8.0f;
    ahead.position[1] = float(root[1]);
    ahead.position[2] = light.position[2];
    hr::Surface behind = ahead;  // the road 8 m behind it
    behind.position[0] = light.position[0] - 8.0f;
    Check(hr::LampScale(light, ahead) > 0.2f, "the road ahead of a car that faces +x is lit");
    CheckNear(hr::LampScale(light, behind), 0.0, 0.0, "the road behind it is not");
  }

  // The same car facing +z and -x and at 62 degrees: the lamp is always ahead of the root along the facing.
  for (const double heading : {0.0, 62.0, 90.0, 180.0, 270.0}) {
    const double h = heading * hr::kPi / 180.0;
    const double facing[3] = {std::sin(h), 0.0, std::cos(h)};
    double pal[4][4], wor[4][4];
    DrawMatrices(facing, root, pal, wor);
    hr::Pose p;
    Check(hr::PoseFromBone(pal, root, &p), "a turned car is a pose");
    std::vector<double> lens;
    for (const auto& spot : model_spots) {
      double w[3];
      ModelToWorld(pal, wor, spot, w);
      lens.insert(lens.end(), w, w + 3);
    }
    hr::CarLamp l;
    Check(hr::FrontLamp(p, reinterpret_cast<const double(*)[3]>(lens.data()), 4, &l), "a turned car has headlights");
    const double ahead[3] = {l.position[0] - root[0], l.position[1] - root[1], l.position[2] - root[2]};
    CheckNear(hr::Dot3d(ahead, facing), 2.0 + hr::kNudge, 1e-9, "the lamp is 2 m (and the nudge) ahead of the root along the facing");
    CheckNear(hr::Dot3d(l.forward, facing), 1.0, 1e-12, "and faces the way the car does");
  }

  // The game's own points agree: the player's Headlight points, the car's back axis in `dir`, make a lamp that faces +x for a car facing +x.
  {
    hr::GuestPoint a, b;
    a.pos[0] = float(root[0] + 1.95);
    a.pos[1] = float(root[1] + 0.5);
    a.pos[2] = float(root[2] - 0.6);
    b = a;
    b.pos[2] = float(root[2] + 0.6);
    for (hr::GuestPoint* g : {&a, &b}) {
      g->dir[0] = float(pose.back[0]);  // the car's BACK axis: -x
      g->dir[1] = float(pose.back[1]);
      g->dir[2] = float(pose.back[2]);
      g->has_dir = true;
      g->intensity = 2.0f;
      g->player = true;
      g->vehicle = 0x1234;
    }
    std::vector<hr::GuestPoint> points = {a, b};
    const auto made = hr::LampsFromPoints(points, std::vector<bool>(2, false));
    Check(made.size() == 1, "a lamp from the pair");
    CheckNear(made[0].lamp.forward[0], 1.0, 1e-6, "the points' lamp faces +x (minus the back axis)");
    // And the lens lamp of the bone's pose agrees with them (nothing to turn round); the lamp from gWorld's pose would be told to turn.
    std::vector<bool> taken;
    const double place[1][3] = {{lamp.position[0], lamp.position[1], lamp.position[2]}};
    const auto matches = hr::AssignPoints(points, place, 1, &taken);
    Check(matches[0].count == 2, "the front lens lamp takes the pair (they are within 3 m)");
    hr::CarLamp l = lamp;
    double on = 0.0;
    bool disagrees = true;
    hr::ApplyPoints(matches[0], &l, &on, &disagrees);
    Check(!disagrees, "the bone's lamp and the points agree on the facing");
    hr::Pose from_world;
    Check(hr::PoseFromWorld(world, &from_world), "gWorld's pose");
    hr::CarLamp wrong = lamp;
    for (int c = 0; c < 3; ++c) {
      wrong.forward[c] = -from_world.back[c];  // what the old rig made of it
    }
    CheckNear(hr::Dot3d(wrong.forward, lamp.forward), 0.0, 1e-9, "the lamp the old rig made faces -z: a quarter turn off for a car facing +x");
    hr::ApplyPoints(matches[0], &wrong, &on, &disagrees);
    Check(!disagrees, "(a quarter turn is not more than 90 degrees: the points turn it without saying so)");
    // The player of the run (heading 331.7, the points' back axis (0.47, 0.04, -0.88)): the old lamp faced -z, 152 degrees off, and is told to turn round
    // (the census's `forward turned by the points`).
    hr::GuestPoint p;
    p.pos[0] = 818.7f;
    p.pos[1] = 18.1f;
    p.pos[2] = -806.7f;
    p.dir[0] = 0.47f;
    p.dir[1] = 0.04f;
    p.dir[2] = -0.88f;
    p.has_dir = true;
    p.intensity = 2.0f;
    std::vector<hr::GuestPoint> player_points = {p};
    const double player_lamp[1][3] = {{818.7, 18.1, -806.7}};
    const auto player_match = hr::AssignPoints(player_points, player_lamp, 1, &taken);
    hr::CarLamp gworld_lamp;
    gworld_lamp.forward[0] = 0.0;
    gworld_lamp.forward[1] = 0.0;
    gworld_lamp.forward[2] = -1.0;  // gWorld's identity rotation
    disagrees = false;
    hr::ApplyPoints(player_match[0], &gworld_lamp, &on, &disagrees);
    Check(disagrees, "a lamp that faces -z for the player of the run is told to turn round by the points");
    CheckNear(gworld_lamp.forward[2], 0.88 / std::sqrt(0.47 * 0.47 + 0.04 * 0.04 + 0.88 * 0.88), 1e-6, "... and faces the way the player drives (+z)");
  }

  // A lens draw at the car's root (a sprite, or a mesh whose box was unreadable) is no lamp; a lens a metre to the side of the root, level with it, is not either.
  {
    const double at_root[3] = {root[0], root[1] + 0.7, root[2]};
    const double near_root[3] = {root[0] + 0.5, root[1], root[2] - 0.6};
    const double at_end[3] = {root[0] + 1.9, root[1] + 0.6, root[2] + 0.6};
    Check(!hr::LensOffRoot(root, at_root), "a centre above the root is on the root");
    Check(!hr::LensOffRoot(root, near_root), "a centre 0.8 m from the root is no lamp");
    Check(hr::LensOffRoot(root, at_end), "a centre 2 m from the root is a lamp's");
  }
}

// ---------------------------------------------------------------------------
// the game's own light points (the night probe's numbers: perf/out/NIGHT-PROBE.txt C12)
// ---------------------------------------------------------------------------

hr::GuestPoint ProbePoint(const double root[3], double dx, double dy, double dz, float intensity, bool player, uint32_t vehicle, uint32_t index) {
  hr::GuestPoint p;
  p.pos[0] = float(root[0] + dx);
  p.pos[1] = float(root[1] + dy);
  p.pos[2] = float(root[2] + dz);
  p.dir[0] = 0.47f;
  p.dir[1] = 0.04f;
  p.dir[2] = -0.88f;
  p.has_dir = true;
  p.intensity = intensity;
  p.player = player;
  p.vehicle = vehicle;
  p.index = index;
  return p;
}

void CheckPoints() {
  namespace hp = mcla::gpu::headlight_points;
  // The table: the latest of a (vehicle, index), expiry, ownerless points by place, a full table drops the oldest.
  {
    hp::Table table;
    const auto t0 = hp::Table::Clock::now();
    const double zero[3] = {0.0, 0.0, 0.0};
    hr::GuestPoint a = ProbePoint(zero, 1, 0, 0, 2.0f, true, 0x1000, 0);
    table.Note(a, t0);
    a.intensity = 0.0f;
    table.Note(a, t0 + std::chrono::milliseconds(16));
    std::vector<hr::GuestPoint> out;
    table.Recent(&out, std::chrono::milliseconds(150), t0 + std::chrono::milliseconds(20));
    Check(out.size() == 1 && out[0].intensity == 0.0f, "the table keeps the latest point of a (vehicle, index)");
    table.Recent(&out, std::chrono::milliseconds(150), t0 + std::chrono::milliseconds(400));
    Check(out.empty(), "a point not seen for 150 ms is gone");
    hr::GuestPoint other = a;
    other.index = 1;
    table.Note(other, t0);
    table.Recent(&out, std::chrono::milliseconds(150), t0 + std::chrono::milliseconds(20));
    Check(out.size() == 2, "another index is another point");
    hr::GuestPoint free_a;
    free_a.pos[0] = 10.0f;
    free_a.intensity = 3.4f;
    hr::GuestPoint free_b = free_a;
    free_b.pos[0] = 10.1f;  // the same quarter metre: the same point
    hr::GuestPoint free_c = free_a;
    free_c.pos[0] = 12.0f;
    table.Clear();
    table.Note(free_a, t0);
    table.Note(free_b, t0);
    table.Note(free_c, t0);
    table.Recent(&out, std::chrono::milliseconds(150), t0);
    Check(out.size() == 2, "ownerless points are told by their place (a quarter of a metre)");
    table.Clear();
    for (uint32_t i = 0; i < hp::Table::kCapacity + 10; ++i) {
      hr::GuestPoint p;
      p.vehicle = 0x2000 + i;
      p.intensity = 1.0f;
      table.Note(p, t0 + std::chrono::milliseconds(i));
    }
    table.Recent(&out, std::chrono::milliseconds(100000), t0 + std::chrono::milliseconds(1000));
    Check(out.size() == hp::Table::kCapacity, "a full table stays full");
    bool newest = false, oldest = false;
    for (const auto& p : out) {
      newest |= p.vehicle == 0x2000 + hp::Table::kCapacity + 9;
      oldest |= p.vehicle == 0x2000;
    }
    Check(newest && !oldest, "... and the oldest went");
    hp::SetWanted(true);
    Check(hp::Wanted(), "the hook's switch");
    hp::SetWanted(false);
    Check(!hp::Wanted(), "... off again");
  }
  // The player's lamps as the probe read them at night: root (819.61, 17.56, -808.41), the pair at +(-0.38 0.53 2.01) and +(-1.42 0.58 1.45), the car's
  // back axis (0.47 0.04 -0.88), intensity 2.00.
  const double root[3] = {819.61, 17.56, -808.41};
  std::vector<hr::GuestPoint> points;
  points.push_back(ProbePoint(root, -0.38, 0.53, 2.01, 2.0f, true, 0xBF4E7380u, 0));
  points.push_back(ProbePoint(root, -1.42, 0.58, 1.45, 2.0f, true, 0xBF4E7380u, 1));
  // A lens lamp 0.3 m off where the points are (the lens centre of a mesh), pointing the wrong way round (the pose's convention may be wrong).
  hr::CarLamp lens;
  lens.position[0] = root[0] - 0.9 + 0.3;
  lens.position[1] = root[1] + 0.2;
  lens.position[2] = root[2] + 1.73;
  lens.forward[0] = 0.47;
  lens.forward[1] = 0.04;
  lens.forward[2] = -0.88;  // minus the points' forward
  std::vector<std::array<double, 3>> places = {{lens.position[0], lens.position[1], lens.position[2]}, {900.0, 17.0, -800.0}};
  std::vector<bool> taken;
  const auto matches = hr::AssignPoints(points, reinterpret_cast<const double(*)[3]>(places.data()), places.size(), &taken);
  Check(matches[0].count == 2 && matches[1].count == 0 && taken[0] && taken[1], "both points go to the lamp near them, none to the far one");
  CheckNear(matches[0].position[0], root[0] - 0.9, 1e-4, "the mean place x");
  CheckNear(matches[0].position[2], root[2] + 1.73, 1e-4, "the mean place z");
  Check(matches[0].has_dir && matches[0].player && matches[0].vehicle == 0xBF4E7380u, "direction, player and vehicle carry over");
  CheckNear(hr::Length3(matches[0].back), 1.0, 1e-9, "the mean back axis is a unit vector");
  hr::CarLamp lamp = lens;
  double on = -1.0;
  bool disagrees = false;
  hr::ApplyPoints(matches[0], &lamp, &on, &disagrees);
  Check(disagrees, "the points turn the lens lamp's forward round: said");
  CheckNear(lamp.forward[0], -0.47 / std::sqrt(0.47 * 0.47 + 0.04 * 0.04 + 0.88 * 0.88), 1e-6, "the forward is minus the back axis (x)");
  CheckNear(lamp.forward[2], 0.88 / std::sqrt(0.47 * 0.47 + 0.04 * 0.04 + 0.88 * 0.88), 1e-6, "... (z)");
  CheckNear(lamp.position[0], root[0] - 0.9 + hr::kNudge * lamp.forward[0], 1e-4, "the lamp is the points' mean, nudged along the points' forward");
  CheckNear(on, 1.0, 1e-9, "intensity 2.00 is full light");
  // The pair is 1.2 m apart across the car and about 1.95 m ahead of the root along that forward (the probe's numbers are consistent with it).
  {
    const double across[3] = {double(points[0].pos[0] - points[1].pos[0]), double(points[0].pos[1] - points[1].pos[1]), double(points[0].pos[2] - points[1].pos[2])};
    CheckNear(std::fabs(hr::Dot3d(across, lamp.forward)) / hr::Length3(across), 0.0, 0.02, "the lamps' pair is across the forward");
    const double ahead[3] = {double(points[0].pos[0]) - root[0], double(points[0].pos[1]) - root[1], double(points[0].pos[2]) - root[2]};
    CheckNear(hr::Dot3d(ahead, lamp.forward), 1.95, 0.05, "the lamp is about 1.95 m ahead of the root along the forward");
  }
  // On and off: 0 off, 2 full, 1 half; a point at or under the off level is off.
  CheckNear(hr::OnOf(0.0f), 0.0, 0.0, "intensity 0 is off");
  CheckNear(hr::OnOf(0.05f), 0.0, 0.0, "at the off level: off");
  CheckNear(hr::OnOf(1.0f), 0.5, 1e-9, "intensity 1 is half");
  CheckNear(hr::OnOf(3.4f), 1.0, 0.0, "the traffic's 3.4 is full (capped)");
  // Another car's pair with intensity 0 (the probe's second vehicle): its lamp is off.
  {
    const double other_root[3] = {834.47, 18.01, -843.32};
    std::vector<hr::GuestPoint> off;
    off.push_back(ProbePoint(other_root, 0.85, 0.59, 1.85, 0.0f, false, 0xBF5BF600u, 0));
    off.push_back(ProbePoint(other_root, -0.37, 0.63, 1.99, 0.0f, false, 0xBF5BF600u, 1));
    std::vector<std::array<double, 3>> place = {{other_root[0] + 0.24, other_root[1] + 0.6, other_root[2] + 1.92}};
    std::vector<bool> t2;
    const auto m2 = hr::AssignPoints(off, reinterpret_cast<const double(*)[3]>(place.data()), 1, &t2);
    hr::CarLamp l2;
    double on2 = 1.0;
    bool d2 = false;
    hr::ApplyPoints(m2[0], &l2, &on2, &d2);
    CheckNear(on2, 0.0, 0.0, "a car whose Headlight points read 0 has its lights off");
  }
  // A point with no lens lamp near it, an owned one, makes a lamp of its own; an ownerless one with no lamp makes none.
  {
    std::vector<hr::GuestPoint> lone = points;  // the player's pair, no lens lamp
    hr::GuestPoint ownerless;
    ownerless.pos[0] = 50.0f;
    ownerless.intensity = 3.4f;
    lone.push_back(ownerless);
    std::vector<bool> none(lone.size(), false);
    const auto lamps = hr::LampsFromPoints(lone, none);
    Check(lamps.size() == 1 && lamps[0].player && lamps[0].lamp.front == 2, "the owned pair makes one lamp, the ownerless point none");
    CheckNear(lamps[0].lamp.up[1], 1.0, 0.0, "its up is the world's");
    CheckNear(lamps[0].on, 1.0, 0.0, "and it is on");
    std::vector<bool> taken_all(lone.size(), true);
    Check(hr::LampsFromPoints(lone, taken_all).empty(), "points a lens lamp took make no lamp of their own");
  }
  // The tracker carries the game's state: a guest candidate with on 0.5 comes out with it, and `guest` says it is not the sun's.
  {
    hr::Tracker tracker;
    std::array<hr::Tracked, hr::kMaxLights> out{};
    hr::Candidate c = Car(0.0, 0.0, 10.0, true);
    c.guest = true;
    c.on = 0.5;
    tracker.Observe(c);
    const size_t n = tracker.EndFrame(1, &out);
    Check(n == 1 && out[0].guest && std::fabs(out[0].on - 0.5) < 1e-12, "the tracker passes the game's on state through");
    tracker.Observe(Car(0.0, 0.0, 10.0, true));
    tracker.EndFrame(2, &out);
    Check(!out[0].guest, "a car seen without the game's word is the sun's again");
  }
}

}  // namespace

// A frame of views made for one set of lights, read for another: by car, not by place.
void CheckShadowRemap() {
  larecomp::gpu::LocalShadowFrame frame;
  frame.count = 3;
  for (uint32_t v = 0; v < 3; ++v) {
    frame.views[v].valid = 1;
    frame.views[v].world_to_shadow[0] = float(v + 1);  // tells the views apart
  }
  frame.views[0].light_index = 0;  // the player's left lamp
  frame.views[1].light_index = 1;  // the player's right lamp (the same matrix in the producer, here told apart by the number)
  frame.views[2].light_index = 2;  // another car
  const uint32_t old_keys[3] = {10, 10, 20};
  // The same set: nothing moves.
  {
    const uint32_t keys[3] = {10, 10, 20};
    const auto out = hr::RemapShadowViews(frame, old_keys, 3, keys, 3);
    Check(out.count == 3 && out.views[0].light_index == 0 && out.views[1].light_index == 1 && out.views[2].light_index == 2, "the same set: the views stay");
  }
  // The other car came first (a car came into the four and the order changed): its view follows it.
  {
    const uint32_t keys[4] = {30, 20, 10, 10};
    const auto out = hr::RemapShadowViews(frame, old_keys, 3, keys, 4);
    Check(out.count == 3, "all three views find their cars");
    Check(out.views[0].light_index == 2 && out.views[1].light_index == 3 && out.views[2].light_index == 1, "each view goes to a lamp of its own car, the two lamps to two");
    Check(out.views[0].world_to_shadow[0] == 1.0f && out.views[2].world_to_shadow[0] == 3.0f, "the matrices go with them");
    const auto of = hr::ShadowViewOfLight(&out, 4);
    Check(of[0] == hr::kNoView && of[1] == 2 && of[2] == 0 && of[3] == 1, "and the host's table has the new place of every view; the new car has none");
  }
  // A car that is gone loses its view; a car with one lamp now has one view.
  {
    const uint32_t keys[2] = {10, 40};
    const auto out = hr::RemapShadowViews(frame, old_keys, 3, keys, 2);
    Check(out.count == 2 && out.views[0].light_index == 0 && out.views[1].light_index == 0 && out.views[0].world_to_shadow[0] == 1.0f && out.views[1].world_to_shadow[0] == 2.0f,
          "a car that left takes its views with it; a car that is one light now has both views on it (the mean of the two lamps')");
  }
  // Bad input: an index past the producer's set, an invalid view, no new lights.
  {
    larecomp::gpu::LocalShadowFrame odd = frame;
    odd.views[2].light_index = 9;
    odd.views[1].valid = 0;
    const uint32_t keys[3] = {10, 10, 20};
    const auto out = hr::RemapShadowViews(odd, old_keys, 3, keys, 3);
    Check(out.count == 1 && out.views[0].light_index == 0, "a view that names no light of its set, or is not valid, is dropped");
    Check(hr::RemapShadowViews(frame, old_keys, 3, keys, 0).count == 0, "no lights, no views");
  }
}

// A light is a car's, and the views are the car's lamps': two views name one light. The next frame's set is in another order; the views go by the car, both to its light.
void CheckLampViewsRemap() {
  larecomp::gpu::LocalShadowFrame frame;
  frame.count = 6;
  const uint32_t lights[6] = {0, 0, 1, 1, 2, 2};  // the player's two lamps, another car's, a third car's
  for (uint32_t v = 0; v < 6; ++v) {
    frame.views[v].valid = 1;
    frame.views[v].light_index = lights[v];
    frame.views[v].world_to_shadow[0] = float(v + 1);
  }
  const uint32_t old_keys[3] = {10, 20, 30};
  {
    const uint32_t keys[4] = {10, 30, 20, 40};  // the two other cars traded places, a fourth came in
    const auto out = hr::RemapShadowViews(frame, old_keys, 3, keys, 4);
    Check(out.count == 6, "all six views find their cars");
    Check(out.views[0].light_index == 0 && out.views[1].light_index == 0 && out.views[2].light_index == 2 && out.views[3].light_index == 2 && out.views[4].light_index == 1 &&
              out.views[5].light_index == 1,
          "each car's two views go to its one light, wherever the light is now");
    const hr::LightViews of = hr::ShadowViewsOfLight(&out, 4);
    Check(of.first[0] == 0 && of.second[0] == 1 && of.first[1] == 4 && of.second[1] == 5 && of.first[2] == 2 && of.second[2] == 3 && of.first[3] == hr::kNoView,
          "the table names the two views of each light; the new car has none");
    Check(out.views[4].world_to_shadow[0] == 5.0f && out.views[2].world_to_shadow[0] == 3.0f, "the matrices go with them");
  }
  // A car that is gone loses both; a car that has one light and three views (not made) keeps two.
  {
    const uint32_t keys[2] = {30, 10};
    const auto out = hr::RemapShadowViews(frame, old_keys, 3, keys, 2);
    Check(out.count == 4 && out.views[0].light_index == 1 && out.views[1].light_index == 1 && out.views[2].light_index == 0 && out.views[3].light_index == 0,
          "a car that left takes its two views with it");
    larecomp::gpu::LocalShadowFrame many = frame;
    many.count = 3;
    many.views[0].light_index = many.views[1].light_index = many.views[2].light_index = 0;
    const uint32_t keys1[1] = {10};
    Check(hr::RemapShadowViews(many, old_keys, 3, keys1, 1).count == 2, "a light takes two views at most");
  }
  // Two lights of one car (the design before the lamps' views: a light for each lamp) still go one view each.
  {
    larecomp::gpu::LocalShadowFrame two;
    two.count = 2;
    two.views[0].valid = two.views[1].valid = 1;
    two.views[0].light_index = 0;
    two.views[1].light_index = 1;
    const uint32_t keys_old[2] = {10, 10}, keys_new[3] = {20, 10, 10};
    const auto out = hr::RemapShadowViews(two, keys_old, 2, keys_new, 3);
    Check(out.count == 2 && out.views[0].light_index == 1 && out.views[1].light_index == 2, "a car with two lights has a view on each");
  }
}

// The levels of LIGHT SHADOWS (owner, 2026-11-20): high shadows two cars, ultra three, of the four the rig lights, each with a view for each lamp: up to six views, and the
// block holds eight. A car the rig lights but that has no view is lit without a shadow.
void CheckThreeShadowViews() {
  Check(hr::kMaxLights >= 3 && sizeof(hr::HeadlightConstants::shadow) / sizeof(larecomp::gpu::LocalShadowView) >= 6,
        "the rig lights three cars and the block holds the six views ultra makes (it holds eight)");
  hr::Lamp lamps[4];
  for (int i = 0; i < 4; ++i) {
    lamps[i].position[0] = 10.0f * float(i);
  }
  const float camera[3] = {0.0f, 1.5f, 0.0f};
  const uint32_t made_keys[4] = {11, 12, 13, 14};  // the producer's set: the player's car (11), then the cars by importance
  for (uint32_t views = 1; views <= 4; ++views) {
    larecomp::gpu::LocalShadowFrame frame;
    for (uint32_t v = 0; v < views; ++v) {
      larecomp::gpu::LocalShadowView& view = frame.views[frame.count++];
      view.valid = 1;
      view.light_index = v;
      view.world_to_shadow[0] = float(v + 1);
      view.atlas_rect[0] = float(v) / 4.0f;  // each view has its own tile in the atlas
      view.atlas_rect[2] = float(v + 1) / 4.0f;
    }
    // The next frame's set is in another order (the tracker swapped the first two other cars and a car came in at the end).
    const uint32_t next_keys[4] = {11, 13, 12, 14};
    const larecomp::gpu::LocalShadowFrame moved = hr::RemapShadowViews(frame, made_keys, 4, next_keys, 4);
    Check(moved.count == views, std::to_string(views) + " view(s) survive the new order");
    const hr::HeadlightConstants block = hr::MakeConstants(lamps, 4, camera, &moved, 5, 6);
    Check(block.header[0] == 4.0f, "all four cars stay lit");
    Check(block.atlas[0] == 5 && block.atlas[1] == 6, "the atlas is bound while a view is");
    const uint32_t shadowed[4] = {11, 13, 12, 14};  // by the new places: the key of each light
    for (uint32_t light = 0; light < 4; ++light) {
      const uint32_t key = shadowed[light];
      const uint32_t made_at = key - 11;  // the producer's place of this car = its view's number
      if (made_at < views) {
        Check(block.shadow_of[light] != hr::kNoView && block.shadow[block.shadow_of[light]].world_to_shadow[0] == float(made_at + 1) &&
                  block.shadow[block.shadow_of[light]].atlas_rect[0] == float(made_at) / 4.0f,
              "car " + std::to_string(key) + " reads its own view and tile of " + std::to_string(views));
      } else {
        Check(block.shadow_of[light] == hr::kNoView, "car " + std::to_string(key) + " is lit with no shadow when only " + std::to_string(views) + " get a view");
      }
    }
  }
}

// The soft lookup: a map of texels (1 lit, 0 in shadow) read as the shader reads it.
void CheckSoftShadow() {
  // Nothing in shadow: 1. All of it: 0.
  for (float x : {10.0f, 10.37f, 55.9f}) {
    for (float y : {3.0f, 3.5f, 90.25f}) {
      CheckNear(hr::SoftShadow(x, y, [](int, int) { return 1.0f; }), 1.0, 1e-6, "a lit map is lit");
      CheckNear(hr::SoftShadow(x, y, [](int, int) { return 0.0f; }), 0.0, 1e-6, "a map in shadow is dark");
    }
  }
  // A straight vertical edge between texel E - 1 (shadow) and texel E (lit): the share is the part of the box on the lit side, a ramp exactly kShadowKernel texels
  // wide, from 0 at E - 1.5 to 1 at E + 1.5, whatever the row and the fraction of the receiver's place.
  constexpr int E = 40;
  const auto edge = [](int ix, int) { return ix >= E ? 1.0f : 0.0f; };
  const float half = 0.5f * hr::kShadowKernel;
  Check(hr::kShadowKernel == 3.0f, "the first version's box is 3 texels wide (the shader's lookup is light_shadow_filter.h now; this twin is kept for the tests that compare)");
  float last = -1.0f;
  int levels = 0;
  bool monotone = true, ramp = true;
  float first_zero_end = -1.0f, first_one = -1.0f;
  float previous_value = -2.0f;
  for (int k = -400; k <= 400; ++k) {
    const float tx = float(E) + 0.01f * float(k);
    const float got = hr::SoftShadow(tx, 17.3f, edge);
    const float want = std::fmin(1.0f, std::fmax(0.0f, (tx + half - float(E)) / hr::kShadowKernel));
    ramp = ramp && std::fabs(got - want) < 1e-4f;
    monotone = monotone && got >= last - 1e-6f;
    if (got != previous_value) {
      ++levels;
      previous_value = got;
    }
    if (got <= 1e-6f) {
      first_zero_end = tx;
    }
    if (first_one < 0.0f && got >= 1.0f - 1e-6f) {
      first_one = tx;
    }
    last = got;
  }
  Check(ramp, "a straight edge is a linear ramp over the box's width");
  Check(monotone, "... and never steps back");
  Check(levels > 250, "... with a level for every step of the receiver, not five (" + std::to_string(levels) + " levels in 801 steps)");
  CheckNear(first_zero_end, float(E) - half, 0.02, "the shadow's full dark ends 1.5 texels before the edge");
  CheckNear(first_one, float(E) + half, 0.02, "the full light starts 1.5 texels after it");
  // The same edge seen along y and from the other side: the answers mirror.
  {
    const auto flipped = [](int ix, int) { return ix >= E ? 0.0f : 1.0f; };
    bool mirror = true;
    for (int k = -200; k <= 200; ++k) {
      const float tx = float(E) + 0.013f * float(k);
      mirror = mirror && std::fabs(hr::SoftShadow(tx, 5.0f, edge) + hr::SoftShadow(tx, 5.0f, flipped) - 1.0f) < 1e-4f;
    }
    Check(mirror, "light and shadow swapped give the complement");
    const auto horizontal = [](int, int iy) { return iy >= E ? 1.0f : 0.0f; };
    bool same = true;
    for (int k = -200; k <= 200; ++k) {
      const float t = float(E) + 0.013f * float(k);
      same = same && std::fabs(hr::SoftShadow(21.4f, t, horizontal) - hr::SoftShadow(t, 21.4f, edge)) < 1e-6f;
    }
    Check(same, "an edge along the other axis gives the same ramp");
  }
  // A block of shadow 7 x 5 texels (a car's shadow is wider than the box): sliding the receiver 0.02 texel changes the answer by at most 0.02 / 3 (+ rounding), so there
  // is no step anywhere along the way, and inside the block, a box's width from its sides, it is dark.
  {
    const auto block = [](int ix, int iy) { return (ix >= 100 && ix < 107 && iy >= 50 && iy < 55) ? 0.0f : 1.0f; };
    float worst = 0.0f, was = hr::SoftShadow(90.0f, 52.5f, block);
    for (int k = 1; k <= 3000; ++k) {
      const float got = hr::SoftShadow(90.0f + 0.01f * float(k), 52.5f, block);
      worst = std::fmax(worst, std::fabs(got - was));
      was = got;
    }
    Check(worst <= 0.01f / 3.0f + 1e-4f, "no step across a shadow's both edges: the largest change in a 0.01 texel step is " + std::to_string(worst));
    CheckNear(hr::SoftShadow(103.5f, 52.5f, block), 0.0, 1e-6, "the middle of the block is dark");
    Check(hr::SoftShadow(103.5f, 52.5f, [](int ix, int iy) { return (ix >= 100 && ix < 107 && iy >= 50 && iy < 55) ? 0.0f : 1.0f; }) == 0.0f,
          "(the outer texels agree there: four fetches)");
  }
  // The shortcut: when the box's four outer texels agree the answer is theirs, however the middle is (a feature thinner than the box is not seen). One texel wide
  // line of shadow, wholly inside the box: not seen; the same line at the box's edge, where an outer texel is in it: a third of the box.
  {
    int fetches = 0;
    const auto line = [&fetches](int ix, int) {
      ++fetches;
      return ix == 41 ? 0.0f : 1.0f;
    };
    // The box covers texels 39 to 42 (the receiver at 40.5 + 1.5 - ... low corner 39.0): the outer columns are 39 and 42, the line is in column 41: not seen, 4 fetches.
    const float inside = hr::SoftShadow(40.5f, 8.0f, line);
    Check(inside == 1.0f && fetches == 4, "a line inside the box, between lit outer texels, is not seen and costs four fetches (" + std::to_string(fetches) + ")");
    fetches = 0;
    const float at_edge = hr::SoftShadow(42.5f, 8.0f, line);  // low corner 41.0: the outer columns are 41 and 44: the line is an outer one
    CheckNear(at_edge, 2.0 / 3.0, 1e-5, "a line at the box's edge darkens a third of it (" + std::to_string(at_edge) + ")");
    Check(fetches == 4 + 16, "... and takes all sixteen fetches (" + std::to_string(fetches) + " in all)");
  }
  // The receiver's place is a continuous value: texel k is covered by the box centred at k + 0.5 only in its middle three, and a receiver at a texel's centre on a flat
  // edge gets the ramp's middle value, 0.5, only at the edge itself.
  CheckNear(hr::SoftShadow(float(E), 9.0f, edge), 0.5, 1e-6, "a receiver on the edge is half lit");
}

int main() {
  const hr::LensBoxStamps stamps{7, 11};
  Check(stamps.Matches({7, 11}), "unchanged vertex and index ranges reuse the lens box");
  Check(!stamps.Matches({8, 11}), "a vertex write invalidates the lens box");
  Check(!stamps.Matches({7, 12}), "an index write invalidates the lens box");
  Check(!hr::LensBoxStamps{0, 11}.Matches({0, 11}), "an untracked vertex range cannot reuse a lens box");
  Check(!hr::LensBoxStamps{7, 0}.Matches({7, 0}), "an untracked index range cannot reuse a lens box");
  // LU-3: every check that goes through a Tracker runs twice, once with the unified picker (light_pick.h, the default) and once with the legacy code of the old tracker (mcla_gpu_light_pick =
  // legacy): the same suite, the same numbers, through both.
  for (const mcla::gpu::light_pick::PickMode mode : {mcla::gpu::light_pick::PickMode::kUnified, mcla::gpu::light_pick::PickMode::kLegacy}) {
    mcla::gpu::light_pick::DefaultMode() = mode;
    std::printf("-- the tracker's picks through the %s picker\n", std::string(mcla::gpu::light_pick::ModeName(mode)).c_str());
    CheckTerm();
    CheckCalibration();
    CheckNight();
    CheckCar();
    CheckFacing();
    CheckTracker();
    CheckFrustum();
    CheckBeamReach();
    CheckTrackerView();
    CheckShadow();
    CheckSoftShadow();
    CheckShadowRemap();
    CheckLampViewsRemap();
    CheckLampSpan();
    CheckThreeShadowViews();
    CheckBlock();
    CheckPoints();
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// Check of the car self-reflection's rules (car_selfrefl_rules.h): the modes, the clamps, the matrix the paint variant projects
// with, the constants' layout and the depth linearisation the history pass and the paint agree on. A program of its own: it
// needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_selfrefl_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_selfrefl_rules_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "car_selfrefl_rules.h"

namespace cr = mcla::gpu::car_selfrefl;
namespace tp = mcla::gpu::temporal;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// A reference march of the paint variant's ray over a slice of the picture
// ---------------------------------------------------------------------------------------------------------------------
// The history along the ray's screen path is `stored(x)` (x in texels: the view distance of the vehicle pixel there, 0 where there is
// none); the ray point's view distance is d0 + dwdt t and its x is x0 + vx t. NewRay does what the variant's helper does with the
// functions of car_selfrefl_rules.h (version 3); OldRay is the first version's march (version 2: a per-pixel noise on the points,
// a binary test, three halvings), kept here to show what the new one changes. Both return the weight of the hit before the
// self-test and the screen edge, which are the same in both.
using Stored = std::function<float(float)>;

float Frac(float x) { return x - std::floor(x); }

float NewRay(const Stored& stored, float d0, float dwdt, float x0, float vx, int steps, float length, float thickness_setting,
             float slack) {
  float best = 0.0f;
  float sum = 0.0f;
  float previous_t = 0.0f;
  float previous_diff = -1.0f;
  for (int i = 0; i < steps; ++i) {
    const float t = cr::RayDistance(i, steps, length);
    const float depth = d0 + dwdt * t;
    const float s = stored(x0 + vx * t);
    const bool on_vehicle = s > 0.0f;
    const float diff = depth - s;
    const float thickness = std::max(thickness_setting, 1.1f * (t - previous_t));
    const float window = on_vehicle ? cr::WindowWeight(diff, depth, thickness, slack) : 0.0f;
    const float face = 1.0f - cr::Approach(previous_diff, diff, thickness);
    sum += cr::PointWeight(window, t / length, 1.0f);
    best = std::max(best, cr::PointWeight(window, t / length, face));
    previous_diff = on_vehicle ? diff : -1.0f;
    previous_t = t;
    if (sum >= 2.0f) break;
  }
  return best * cr::ThinFactor(sum);
}

float OldRay(const Stored& stored, float d0, float dwdt, float x0, float vx, int steps, float length, float thickness_setting,
             float pixel_x, float pixel_y) {
  const float step = length / float(steps);
  const float thickness = std::max(thickness_setting, 1.2f * step);
  const float jitter = Frac(52.9829189f * Frac(std::floor(pixel_x) * 0.06711056f + std::floor(pixel_y) * 0.00583715f));
  auto hit = [&](float t) {
    const float depth = d0 + dwdt * t;
    const float s = stored(x0 + vx * t);
    const float diff = depth - s;
    return s > 0.0f && diff > 0.01f + 0.004f * depth && diff < thickness;
  };
  float hit_t = -1.0f;
  for (int i = 0; i < steps; ++i) {
    const float t = (float(i) + 0.25f + 0.75f * jitter) * step;
    if (hit(t)) {
      hit_t = t;
      break;
    }
  }
  if (hit_t < 0.0f) return 0.0f;
  float lo = std::max(hit_t - step, 0.0f);
  float hi = hit_t;
  for (int k = 0; k < 3; ++k) {
    const float mid = 0.5f * (lo + hi);
    (hit(mid) ? hi : lo) = mid;
  }
  return 1.0f - cr::SmoothStep(0.55f, 1.0f, hi / length);
}

// How many pixels of a row differ from the next one by more than 0.2, as a share of the row.
double JumpShare(const std::function<float(int)>& weight, int first, int count) {
  int jumps = 0;
  for (int p = first; p + 1 < first + count; ++p) {
    if (std::fabs(weight(p + 1) - weight(p)) > 0.2f) ++jumps;
  }
  return double(jumps) / double(count);
}

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool Near(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

// clip = [p 1] M as the paint variant does it: p.x m[0..3] + p.y m[4..7] + p.z m[8..11] + m[12..15].
void Project(const float m[16], const double p[3], double clip[4]) {
  for (int c = 0; c < 4; ++c) {
    clip[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
  }
}

// The game's projection, row vectors, the camera looking down -z, ndc z 0 at the near plane and 1 at the far one (the stored
// depth is 1 - ndc: 1 near, 0 far).
tp::Mat4 Perspective(double fov_y, double aspect, double n, double f) {
  tp::Mat4 p;
  const double h = 1.0 / std::tan(fov_y * 0.5);
  p.m[0][0] = h / aspect;
  p.m[1][1] = h;
  p.m[2][2] = f / (n - f);
  p.m[2][3] = -1.0;
  p.m[3][2] = n * f / (n - f);
  return p;
}

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 t = tp::Identity();
  t.m[3][0] = x;
  t.m[3][1] = y;
  t.m[3][2] = z;
  return t;
}

}  // namespace

int main() {
  // the modes
  Check(cr::ModeOfTest(-3) == cr::Mode::kOff && cr::ModeOfTest(0) == cr::Mode::kOff, "0 and below is off");
  Check(cr::ModeOfTest(1) == cr::Mode::kOcclusion, "1 is the occlusion");
  Check(cr::ModeOfTest(2) == cr::Mode::kReflection && cr::ModeOfTest(9) == cr::Mode::kReflection, "2 and above is the reflection");

  // the clamps
  cr::Tuning wild;
  wild.length = 100.0f;
  wild.thickness = -1.0f;
  wild.strength = 3.0f;
  wild.steps = 1000;
  wild.view = -2;
  wild.gain = 1000.0f;
  const cr::Tuning clamped = cr::Clamped(wild);
  Check(clamped.length == cr::kMaxLength && clamped.thickness == cr::kMinThickness && clamped.strength == 1.0f &&
            clamped.steps == cr::kMaxSteps && clamped.view == 0 && clamped.gain == cr::kMaxGain,
        "the tuning is clamped");
  const cr::Tuning defaults;
  Check(cr::Clamped(defaults).steps == 12 && cr::Clamped(defaults).length == 2.0f, "the defaults pass the clamps unchanged");

  // the exponent bias
  Check(cr::ExpBiasGain(4) == 0.0625f && cr::ExpBiasGain(0) == 1.0f && cr::ExpBiasGain(-1) == 2.0f,
        "2^-bias: the target's bias 4 is a gain of 1/16");
  Check(cr::ExpBiasGain(1000) == std::ldexp(1.0f, -24), "the bias is limited");

  // the matrix: no motion is the view-projection itself; a motion is applied first
  tp::Mat4 vp = Perspective(0.45, 16.0 / 9.0, 0.5, 1000.0);
  // the history camera one unit to the right of the origin looking down -z: world x 1 is its view x 0
  vp = tp::Mul(Translation(-1.0, 0.0, 0.0), vp);
  float m[16];
  cr::HistoryMatrix(vp, nullptr, m);
  double clip[4];
  const double p[3] = {1.0, 0.0, -10.0};
  Project(m, p, clip);
  Check(Near(clip[0], 0.0, 1e-5) && Near(clip[3], 10.0, 1e-5), "a point straight ahead of the history camera projects to the middle at w 10");
  // a car that was 0.5 behind in world x one frame ago: this frame's x 1.5 is the history's x 1
  const tp::Mat4 motion = Translation(-0.5, 0.0, 0.0);
  cr::HistoryMatrix(vp, &motion, m);
  const double q[3] = {1.5, 0.0, -10.0};
  Project(m, q, clip);
  Check(Near(clip[0], 0.0, 1e-5) && Near(clip[3], 10.0, 1e-5), "the vehicle's motion is applied before the view-projection");

  // the constants' layout
  const cr::Tuning tuning;
  cr::CarSelfReflConstants c = cr::MakeConstants(cr::Mode::kReflection, tuning, m, 77u, 0u, 0.0625f, 5.0f);
  Check(c.a[0] == 2.0f && c.a[1] == 2.0f && c.a[2] == 0.3f && c.a[3] == 1.0f, "A: mode, length, thickness, strength");
  Check(c.b[0] == 12.0f && c.b[1] == 0.0f && c.b[2] == 0.0625f && c.b[3] == 5.0f, "B: steps, view, gain, noise frame");
  Check(c.c[0] == 77u && c.c[1] == 0u && c.c[2] == 0u && c.c[3] == 0u, "C: the slots");
  Check(std::memcmp(c.m, m, sizeof(m)) == 0, "M: the matrix rows");
  const cr::CarSelfReflConstants off = cr::OffConstants();
  Check(off.a[0] == 0.0f && off.c[0] == 0u, "the constants of a draw without a history are mode 0");
  cr::Tuning viewed = tuning;
  viewed.view = 3;
  viewed.gain = 2.0f;
  c = cr::MakeConstants(cr::Mode::kOcclusion, viewed, m, 1u, 0u, 0.0625f, 0.0f);
  Check(c.a[0] == 1.0f && c.b[1] == 3.0f && c.b[2] == 0.125f, "the debug view and the gain multiplier reach the block");

  // the linearisation the history pass makes and the w the paint divides by are the same distance
  const tp::Mat4 proj = Perspective(0.45, 16.0 / 9.0, 0.5, 1000.0);
  bool depth_ok = true;
  for (double distance : {0.6, 1.0, 3.0, 12.5, 40.0, 300.0, 900.0}) {
    // stored depth of a point `distance` in front of the camera, as the game stores it (1 - ndc z)
    const double clip_z = -distance * proj.m[2][2] + proj.m[3][2];
    const double clip_w = distance;  // p[2][3] = -1, p[3][3] = 0
    const double stored = 1.0 - clip_z / clip_w;
    const double t = cr::LinearDepth(stored, proj);
    if (!Near(t, distance, 1e-6)) {
      depth_ok = false;
      std::printf("  distance %g: stored %.9g gives %.9g\n", distance, stored, t);
    }
  }
  Check(depth_ok, "LinearDepth gives back the distance, which is the clip w of the paint's projection");
  Check(Near(cr::LinearDepth(0.0, proj), 1000.0, 1e-6), "stored 0 is the far plane");
  Check(Near(cr::LinearDepth(1.0, proj), 0.5), "stored 1 is the near plane");

  // the history image's size: half for the low setting and for 2560 pixels across and more, the debug override wins
  Check(cr::HistoryDivisor(cr::Mode::kOcclusion, 1280, 0) == 2, "low: half size at any render size");
  Check(cr::HistoryDivisor(cr::Mode::kOcclusion, 3840, 0) == 2, "low: half size at 4K");
  Check(cr::HistoryDivisor(cr::Mode::kReflection, 1280, 0) == 1, "high at 1280 across: full size");
  Check(cr::HistoryDivisor(cr::Mode::kReflection, 1920, 0) == 1, "high at 1920 across: full size");
  Check(cr::HistoryDivisor(cr::Mode::kReflection, 2559, 0) == 1, "high at 2559 across: full size");
  Check(cr::HistoryDivisor(cr::Mode::kReflection, 2560, 0) == 2, "high at 2560 across: half size");
  Check(cr::HistoryDivisor(cr::Mode::kReflection, 3840, 0) == 2, "high at 3840 across: half size");
  Check(cr::HistoryDivisor(cr::Mode::kReflection, 3840, 1) == 1 && cr::HistoryDivisor(cr::Mode::kOcclusion, 3840, 1) == 1,
        "the override 1 gives the full size");
  Check(cr::HistoryDivisor(cr::Mode::kReflection, 1280, 2) == 2 && cr::HistoryDivisor(cr::Mode::kOcclusion, 1280, 2) == 2,
        "the override 2 gives half");
  Check(cr::HistoryDivisor(cr::Mode::kReflection, 3840, 7) == 2 && cr::HistoryDivisor(cr::Mode::kReflection, 1280, -1) == 1,
        "an override that is not 1 or 2 is the rule");
  Check(cr::HistoryEdge(2160, 1) == 2160 && cr::HistoryEdge(2160, 2) == 1080 && cr::HistoryEdge(2161, 2) == 1081 &&
            cr::HistoryEdge(1, 2) == 1 && cr::HistoryEdge(720, 0) == 720,
        "the image's edge is the render edge over the divisor, rounded up");
  // the memory at 4K (3840x2160, RGBA16F, 8 bytes a texel): 63.3 MiB at full size, 15.8 MiB at half
  const double full_mib = double(cr::HistoryEdge(3840, 1)) * cr::HistoryEdge(2160, 1) * 8.0 / (1024.0 * 1024.0);
  const double half_mib = double(cr::HistoryEdge(3840, 2)) * cr::HistoryEdge(2160, 2) * 8.0 / (1024.0 * 1024.0);
  Check(Near(full_mib, 63.28125, 1e-9) && Near(half_mib, 15.8203125, 1e-9), "the image is 63.3 MiB at full size and 15.8 MiB at half at 4K");

  // -------------------------------------------------------------------------------------------------------------------
  // the ray (the paint variant's helper, version 3)
  // -------------------------------------------------------------------------------------------------------------------
  // the points: the same for every pixel (no argument but the index), growing apart, the last at the ray's length, none nearer than
  // the minimum, and the first quarter of the ray holding 4 of 12 points where evenly spaced points would hold 3
  for (int steps : {4, 12, 24}) {
    bool ordered = true;
    for (int i = 0; i < steps; ++i) {
      ordered = ordered && cr::RayDistance(i, steps, 2.0f) >= cr::kRayStartMin &&
                (i == 0 || cr::RayDistance(i, steps, 2.0f) > cr::RayDistance(i - 1, steps, 2.0f));
    }
    Check(ordered && Near(cr::RayDistance(steps - 1, steps, 2.0f), 2.0, 1e-6), "the ray's points grow apart and end at the length");
  }
  Check(Near(cr::RayDistance(0, 12, 2.0f), 2.0 * std::pow(1.0 / 12.0, 1.5), 1e-6), "the first point of 12 on 2 m: 0.048");
  int near_points = 0;
  for (int i = 0; i < 12; ++i) {
    if (cr::RayDistance(i, 12, 2.0f) <= 0.5f) ++near_points;
  }
  Check(near_points == 4, "12 points on 2 m: 4 in the first half metre (evenly spaced: 3)");
  Check(cr::RayDistance(0, 24, 0.2f) == cr::kRayStartMin, "a short ray never puts a point nearer than the minimum");

  // the window of a point: 0 in front of the surface and up to the bias, rises over a ramp, is 1 up to half the thickness, falls
  // to 0 at the thickness, and has no jump in between (a weight that jumps from pixel to pixel is a dotted patch)
  {
    const float depth = 3.0f;
    const float thickness = 0.3f;
    const float slack = 0.0f;
    const float bias = 0.01f + 0.004f * depth;
    Check(cr::WindowWeight(-0.5f, depth, thickness, slack) == 0.0f && cr::WindowWeight(bias, depth, thickness, slack) == 0.0f,
          "a point in front of the surface, or within the bias, has no weight");
    Check(cr::WindowWeight(0.1f, depth, thickness, slack) == 1.0f && cr::WindowWeight(0.5f * thickness, depth, thickness, slack) == 1.0f,
          "just behind the surface the weight is 1, up to half the thickness");
    Check(cr::WindowWeight(thickness, depth, thickness, slack) == 0.0f && cr::WindowWeight(2.0f, depth, thickness, slack) == 0.0f,
          "at the thickness and beyond the weight is 0");
    float biggest_step = 0.0f;
    bool rises_then_falls = true;
    float previous = 0.0f;
    bool seen_plateau = false;
    for (int i = 1; i <= 400; ++i) {
      const float diff = float(i) * 0.001f;
      const float w = cr::WindowWeight(diff, depth, thickness, slack);
      biggest_step = std::max(biggest_step, std::fabs(w - previous));
      if (!seen_plateau && w < previous - 1e-6f) rises_then_falls = false;
      if (seen_plateau && w > previous + 1e-6f) rises_then_falls = false;
      if (w >= 1.0f) seen_plateau = true;
      previous = w;
    }
    Check(biggest_step < 0.06f && rises_then_falls, "the window is continuous (a millimetre moves it under 0.06) and has one hump");
    Check(cr::WindowWeight(0.06f, depth, thickness, 0.0f) > cr::WindowWeight(0.06f, depth, thickness, 0.03f) &&
              cr::WindowWeight(0.06f, depth, thickness, 0.03f) > 0.0f,
          "the slack of a slanted surface moves the bias up and the weight down");
  }

  // the slack: more for a slanted surface, for a coarse image and for a far car, never more than 5 cm
  {
    const float grazing = cr::SurfaceSlack(3.0f, 1080.0f, 1.7f, 0.15f);
    const float facing = cr::SurfaceSlack(3.0f, 1080.0f, 1.7f, 0.9f);
    Check(grazing > facing && facing > 0.0f && grazing < 0.05f, "a surface seen at a slant has more slack than one seen square on");
    Check(cr::SurfaceSlack(3.0f, 540.0f, 1.7f, 0.5f) > cr::SurfaceSlack(3.0f, 1080.0f, 1.7f, 0.5f),
          "a coarser history has more slack");
    Check(cr::SurfaceSlack(30.0f, 360.0f, 1.0f, 0.05f) == 0.05f, "the slack is capped at 5 cm");
  }

  // how sure a hit is: a lone sample is a thin part, a sum of two and a half is a wide one
  Check(Near(cr::ThinFactor(0.0f), cr::kThinFloor, 1e-6) && Near(cr::ThinFactor(2.0f), 1.0, 1e-6) && Near(cr::ThinFactor(9.0f), 1.0, 1e-6),
        "thin factor: the floor with nothing, 1 from a sum of two and a half");
  Check(cr::ThinFactor(1.0f) > cr::kThinFloor && cr::ThinFactor(1.0f) < 0.8f, "a lone full sample is between: 0.73");
  Check(cr::Approach(-1.0f, 0.0f, 0.3f) == 0.0f && cr::Approach(0.1f, 0.2f, 0.3f) == 0.0f && cr::Approach(0.2f, 0.19f, 0.3f) > 0.0f &&
            cr::Approach(0.2f, 0.1f, 0.3f) == 1.0f,
        "approach: none from in front or going deeper, full once the ray comes a tenth of the thickness nearer from behind");
  Check(cr::PointWeight(1.0f, 0.1f, 0.0f) == cr::kBackFloor && cr::PointWeight(1.0f, 0.1f, 1.0f) == 1.0f,
        "a hit reached from behind keeps half of its weight at most, one from the front all of it");
  Check(cr::PointWeight(1.0f, 1.0f, 1.0f) == 0.0f && cr::PointWeight(1.0f, 0.3f, 1.0f) == 1.0f,
        "the weight fades to nothing at the ray's end and not before 0.55 of it");

  // what the pixels show: the darkest the occlusion makes the reflection, and the colour of a hidden face
  Check(Near(cr::Darkened(1.0f, 1.0f), 0.15, 1e-6) && cr::Darkened(1.0f, 0.0f) == 1.0f && cr::Darkened(1.0f, 7.0f) == cr::Darkened(1.0f, 1.0f),
        "a certain, wide hit leaves 15% of the reflection, no weight none changed");
  Check(cr::Darkened(1.0f, cr::kBackFloor) > 0.55f, "a hit that is probably the hidden face leaves more than half of the reflection");
  Check(Near(cr::SeenColour(0.5f, 0.05f, 1.0f), 0.05, 1e-6) && Near(cr::SeenColour(0.5f, 0.05f, 0.0f), 0.5 * cr::kShade, 1e-6),
        "mode 2: the hit's colour from the front, the paint's own colour in shade from behind");

  // a thin part (a mirror's arm: 24 texels wide, 0.15 m in front of the ray's start, the ray crossing the picture at 300 texels a
  // metre and going away from the camera): a row of 240 pixels. The first version's per-pixel noise made neighbours disagree
  // (a dotted patch); the points of the new one are the same for every pixel, so the hits are blocks that follow the part's outline
  // and the row jumps a few times. The weights stay under 0.8 (one sample is a thin part).
  const Stored arm = [](float x) { return (x >= 200.0f && x < 224.0f) ? 3.15f : 0.0f; };
  const float arm_d0 = 3.0f;
  const float arm_dwdt = 0.5f;
  const float arm_vx = -300.0f;
  auto new_arm = [&](int p) { return NewRay(arm, arm_d0, arm_dwdt, float(p), arm_vx, 12, 2.0f, 0.3f, 0.0f); };
  auto old_arm = [&](int p) { return OldRay(arm, arm_d0, arm_dwdt, float(p), arm_vx, 12, 2.0f, 0.25f, float(p), 100.0f); };
  const double new_jumps = JumpShare(new_arm, 300, 240);
  const double old_jumps = JumpShare(old_arm, 300, 240);
  float new_peak = 0.0f;
  float old_mean = 0.0f;
  float new_mean = 0.0f;
  for (int p = 300; p < 540; ++p) {
    new_peak = std::max(new_peak, new_arm(p));
    new_mean += new_arm(p) / 240.0f;
    old_mean += old_arm(p) / 240.0f;
  }
  std::printf("  thin part: jumps per pixel new %.3f old %.3f, peak %.2f, mean new %.3f old %.3f\n", new_jumps, old_jumps,
              new_peak, new_mean, old_mean);
  Check(old_jumps > 0.25, "the first version's row of a thin part is a speckle (a quarter of the neighbours or more differ by 0.2)");
  Check(new_jumps < 0.1 && new_jumps * 3.0 < old_jumps, "the new row jumps at the outline of the blocks, not from pixel to pixel");
  Check(new_peak > 0.4f && new_peak < 0.8f, "the thin part is seen but takes less than 0.8 of the reflection");

  // a wide part (a mirror housing: the whole slice): the ray goes behind it from the front, so the hit is the same as before
  const Stored shell = [](float) { return 3.15f; };
  float shell_min = 1.0f;
  for (int p = 300; p < 540; ++p) {
    shell_min = std::min(shell_min, NewRay(shell, arm_d0, arm_dwdt, float(p), arm_vx, 12, 2.0f, 0.3f, 0.0f));
  }
  Check(shell_min > 0.9f, "a wide part the ray goes behind from the front keeps its full weight (the wing in the trunk lid)");

  // the hidden face: the ray comes from far behind the stored surface towards it (an underside the picture does not hold)
  const Stored plane = [](float) { return 3.0f; };
  const float hidden = NewRay(plane, 3.6f, -0.9f, 0.0f, 0.0f, 12, 2.0f, 0.3f, 0.0f);
  const float front = NewRay(plane, 2.8f, 0.5f, 0.0f, 0.0f, 12, 2.0f, 0.3f, 0.0f);
  std::printf("  hidden face weight %.3f, front face %.3f\n", hidden, front);
  Check(hidden > 0.2f && hidden < 0.55f && front > 0.9f, "a ray that reaches the surface from behind takes at most about half, one from the front all");
  Check(cr::Darkened(1.0f, hidden) > 0.5f, "...so the reflection of a thin part keeps more than half of its light");

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

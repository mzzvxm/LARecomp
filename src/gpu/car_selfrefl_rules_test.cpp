// Check of the car self-reflection's rules (car_selfrefl_rules.h): the modes, the clamps, the matrix the paint variant projects
// with, the constants' layout and the depth linearisation the history pass and the paint agree on, and the ray of the paint variant
// (the window, the cover of a segment, the outline of a part found by halving, the hit's sum), with a reference march of it over a
// slice of the picture that shows what a part seen at several widths and offsets does from one pixel to the next. A program of its
// own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
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
// The history along the ray's screen path is `history(x)` (x in texels: what the history image holds at that texel, with no vehicle
// pixel where `on` is false); the ray point's view distance is d0 + dwdt t and its x is x0 + vx t. March does what the variant's helper
// does with the functions of car_selfrefl_rules.h (version 4: the chain of segments, the outline of a part found by halving). VersionThree is the previous version's march (the heaviest of the points, each a window of its own, a thin part
// floor), kept here to show what the new one changes. Both return the weight of the hit before the self-test and the screen edge,
// which are the same in both.
struct Probe {
  bool on = false;
  float stored = 0.0f;  // the vehicle pixel's view distance
  float colour = 0.0f;  // and its colour (one channel)
};
using History = std::function<Probe(float)>;

struct Ray {
  float d0 = 3.0f;
  float dwdt = 0.5f;
  float x0 = 0.0f;
  float vx = -300.0f;
};

struct RayResult {
  float weight = 0.0f;
  float face = 1.0f;
  float distance = 0.0f;
  float colour = 0.0f;
  int taps = 0;  // the reads of the history (the shaded point's own is not counted): one a point and three for a segment with an outline in it
};

// The shaded point's own colour, as the history holds it, for the first end of the first segment.
constexpr float kOwnColour = 0.25f;

RayResult March(const History& history, const Ray& ray, int steps, float length, float thickness, float slack, float full_share = 0.25f) {
  RayResult result;
  cr::HitSum sum;
  struct Point {
    float t = 0.0f;
    cr::SegmentEnd end;
    float colour = 0.0f;
  };
  // one window for the whole ray, made with the shaded point's view distance
  const cr::WindowShape shape = cr::MakeWindowShape(ray.d0, thickness, slack);
  auto sample = [&](float t) {
    Point p;
    p.t = t;
    const float depth = ray.d0 + ray.dwdt * t;
    const Probe probe = history(std::floor(ray.x0 + ray.vx * t));
    ++result.taps;
    p.end.on = probe.on;
    p.end.stored = probe.on ? probe.stored : 0.0f;
    p.end.diff = depth - probe.stored;
    p.end.t = t;
    p.colour = probe.colour;
    return p;
  };
  // What the segment from `a` to `b` (a before b) takes: as the shader does, a segment with an outline in it is halved three times to
  // find where, and takes the share of its length that is on the window's side.
  auto add = [&](const Point& a, const Point& b) {
    if (cr::HasOutline(a.end, b.end, shape)) {
      const bool a_in = cr::InWindow(a.end, shape);
      const Point in_end = a_in ? a : b;
      const Point other_end = a_in ? b : a;
      Point inner = in_end;
      Point outer = other_end;
      for (int k = 0; k < cr::kOutlineSteps; ++k) {
        const Point m = sample(0.5f * (inner.t + outer.t));
        if (cr::InWindow(m.end, shape)) {
          inner = m;
        } else {
          outer = m;
        }
      }
      const float outline = 0.5f * (inner.t + outer.t);
      const bool in_first = in_end.t < inner.t;
      const Point& first = in_first ? in_end : inner;
      const Point& second = in_first ? inner : in_end;
      cr::Segment s = cr::SegmentOf(first.end, second.end, shape);
      s.cover *= cr::OutlineShare(in_end.t, inner.t, outer.t, other_end.t);
      sum.Add(s, std::min(in_end.t, outline), std::max(in_end.t, outline), length, 1.0f, first.colour, second.colour);
    } else if (cr::MayCover(a.end, b.end, shape)) {
      sum.Add(cr::SegmentOf(a.end, b.end, shape), a.t, b.t, length, 1.0f, a.colour, b.colour);
    }
  };
  Point previous;
  previous.end.on = true;  // the shaded point itself: on the vehicle, diff 0
  previous.end.stored = ray.d0;
  previous.colour = kOwnColour;
  const float full = cr::FullPoints(full_share, steps);
  for (int i = 0; i < steps; ++i) {
    const Point point = sample(cr::RayDistance(i, steps, length));
    add(previous, point);
    previous = point;
    if (sum.Occupied() >= full) break;
  }
  result.weight = sum.Weight(full_share, steps);
  result.face = sum.Face();
  result.distance = sum.Distance(length);
  result.colour = sum.Colour();
  return result;
}

// Version 3, as it was (car_selfrefl_rules.h of a0d2da4d): each point has a window of its own, the weight of the hit is the heaviest
// point's, the sum of the windows says how thin the part is.
float VersionThree(const History& history, const Ray& ray, int steps, float length, float thickness_setting, float slack) {
  auto window = [](float diff, float depth, float thickness, float slack_) {
    const float bias = 0.01f + 0.004f * depth + slack_;
    const float ramp = std::min(0.02f + 0.004f * depth, 0.25f * thickness);
    return cr::SmoothStep(bias, bias + ramp, diff) * (1.0f - cr::SmoothStep(0.5f * thickness, thickness, diff));
  };
  float best = 0.0f;
  float sum = 0.0f;
  float previous_t = 0.0f;
  float previous_diff = -1.0f;
  for (int i = 0; i < steps; ++i) {
    const float t = cr::RayDistance(i, steps, length);
    const float depth = ray.d0 + ray.dwdt * t;
    const Probe probe = history(std::floor(ray.x0 + ray.vx * t));
    const float diff = depth - probe.stored;
    const float thickness = std::max(thickness_setting, 1.1f * (t - previous_t));
    const float w = probe.on ? window(diff, depth, thickness, slack) : 0.0f;
    const float face = 1.0f - (previous_diff > 0.0f ? cr::SmoothStep(0.0f, 0.1f * thickness, previous_diff - diff) : 0.0f);
    const float fade = 1.0f - cr::SmoothStep(0.55f, 1.0f, t / length);
    sum += w * fade;
    best = std::max(best, w * fade * (0.5f + 0.5f * face));
    previous_diff = probe.on ? diff : -1.0f;
    previous_t = t;
    if (sum >= 2.0f) break;
  }
  return best * (0.6f + 0.4f * std::clamp((sum - 0.5f) / 1.5f, 0.0f, 1.0f));
}

// The largest difference between two neighbouring pixels' weights over a row of `count` pixels from `first` (a pixel is half a texel:
// the history of a 4K picture is half the size), and the row's largest weight. A pixel the weight function says is no pixel of the
// surface being shaded (a negative weight: the arm itself is there) is skipped.
struct RowStats {
  float jump = 0.0f;
  float peak = 0.0f;
};
RowStats Row(const std::function<float(float)>& weight_at_texel, int first, int count, float pixel_texels = 0.5f) {
  RowStats r;
  float previous = -1.0f;
  for (int p = first; p < first + count; ++p) {
    const float w = weight_at_texel(float(p) * pixel_texels);
    if (w >= 0.0f) {
      r.peak = std::max(r.peak, w);
      if (previous >= 0.0f) r.jump = std::max(r.jump, std::fabs(w - previous));
    }
    previous = w;
  }
  return r;
}

// The most two neighbouring pixels' weights may differ by in a row over a thin part.
constexpr float kContinuityBound = 0.35f;

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
  wild.full = 100.0f;
  const cr::Tuning clamped = cr::Clamped(wild);
  Check(clamped.length == cr::kMaxLength && clamped.thickness == cr::kMinThickness && clamped.strength == 1.0f &&
            clamped.steps == cr::kMaxSteps && clamped.view == 0 && clamped.gain == cr::kMaxGain && clamped.full == cr::kMaxFull,
        "the tuning is clamped");
  const cr::Tuning defaults;
  Check(cr::Clamped(defaults).steps == 12 && cr::Clamped(defaults).length == 2.0f && cr::Clamped(defaults).full == 0.25f,
        "the defaults pass the clamps unchanged");

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
  cr::CarSelfReflConstants c = cr::MakeConstants(cr::Mode::kReflection, tuning, m, 77u, 0u, 0.0625f);
  Check(c.a[0] == 2.0f && c.a[1] == 2.0f && c.a[2] == 0.3f && c.a[3] == 1.0f, "A: mode, length, thickness, strength");
  Check(c.b[0] == 12.0f && c.b[1] == 0.0f && c.b[2] == 0.0625f && c.b[3] == 0.25f, "B: steps, view, gain, the hit's length share");
  Check(c.c[0] == 77u && c.c[1] == 0u && c.c[2] == 0u && c.c[3] == 0u, "C: the slots");
  Check(std::memcmp(c.m, m, sizeof(m)) == 0, "M: the matrix rows");
  const cr::CarSelfReflConstants off = cr::OffConstants();
  Check(off.a[0] == 0.0f && off.c[0] == 0u, "the constants of a draw without a history are mode 0");
  cr::Tuning viewed = tuning;
  viewed.view = 3;
  viewed.gain = 2.0f;
  c = cr::MakeConstants(cr::Mode::kOcclusion, viewed, m, 1u, 0u, 0.0625f);
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
  // the ray (the paint variant's helper, version 4)
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
  // to 0 at the thickness, and has no jump in between (a weight that jumps from pixel to pixel is a band)
  {
    const float depth = 3.0f;
    const float thickness = 0.3f;
    const cr::WindowShape w = cr::MakeWindowShape(depth, thickness, 0.0f);
    const float bias = 0.01f + 0.004f * depth;
    Check(cr::WindowWeight(-0.5f, w) == 0.0f && cr::WindowWeight(bias, w) == 0.0f,
          "a point in front of the surface, or within the bias, has no weight");
    Check(cr::WindowWeight(0.1f, w) == 1.0f && cr::WindowWeight(0.5f * thickness, w) == 1.0f,
          "just behind the surface the weight is 1, up to half the thickness");
    Check(cr::WindowWeight(thickness, w) == 0.0f && cr::WindowWeight(2.0f, w) == 0.0f, "at the thickness and beyond the weight is 0");
    float biggest_step = 0.0f;
    bool rises_then_falls = true;
    float previous = 0.0f;
    bool seen_plateau = false;
    for (int i = 1; i <= 400; ++i) {
      const float diff = float(i) * 0.001f;
      const float v = cr::WindowWeight(diff, w);
      biggest_step = std::max(biggest_step, std::fabs(v - previous));
      if (!seen_plateau && v < previous - 1e-6f) rises_then_falls = false;
      if (seen_plateau && v > previous + 1e-6f) rises_then_falls = false;
      if (v >= 1.0f) seen_plateau = true;
      previous = v;
    }
    Check(biggest_step < 0.06f && rises_then_falls, "the window is continuous (a millimetre moves it under 0.06) and has one hump");
    Check(cr::WindowWeight(0.06f, w) > cr::WindowWeight(0.06f, cr::MakeWindowShape(depth, thickness, 0.03f)) &&
              cr::WindowWeight(0.06f, cr::MakeWindowShape(depth, thickness, 0.03f)) > 0.0f,
          "the slack of a slanted surface moves the bias up and the weight down");
    // the same product the previous version wrote: the two ramps never overlap, so the difference of two smoothsteps is it
    bool same_as_product = true;
    for (float d : {0.5f, 3.0f, 12.0f}) {
      for (float th : {0.05f, 0.3f, 1.0f}) {
        for (float slack : {0.0f, 0.02f, 0.05f}) {
          const cr::WindowShape s = cr::MakeWindowShape(d, th, slack);
          for (int i = -50; i <= 1200; ++i) {
            const float diff = float(i) * 0.001f * th;
            const float product = cr::SmoothStep(s.bias, s.bias + s.ramp, diff) * (1.0f - cr::SmoothStep(s.fall, s.thickness, diff));
            same_as_product = same_as_product && std::fabs(product - cr::WindowWeight(diff, s)) < 1e-5f;
          }
          same_as_product = same_as_product && s.bias + s.ramp <= s.fall + 1e-6f;
        }
      }
    }
    Check(same_as_product, "the window is the previous version's product of a rise and a fall, whatever the depth, thickness and slack");
  }

  // the integral of the window is exact: the mean along a segment is the integral's difference over the diff's difference, and a
  // fine sum of the window along the line agrees (this is what lets a thin shell between two points count for the part it takes)
  {
    uint32_t seed = 12345u;
    auto rnd = [&seed]() {
      seed = seed * 1664525u + 1013904223u;
      return float(seed >> 8) / float(1u << 24);
    };
    float worst = 0.0f;
    for (int n = 0; n < 400; ++n) {
      const float depth = 0.5f + 8.0f * rnd();
      const float th = 0.05f + 0.6f * rnd();
      const cr::WindowShape s = cr::MakeWindowShape(depth, th, 0.05f * rnd());
      const float d0 = -0.6f + 1.4f * rnd();
      const float d1 = -0.6f + 1.4f * rnd();
      double fine = 0.0;
      const int count = 4000;
      for (int i = 0; i < count; ++i) {
        fine += cr::WindowWeight(d0 + (d1 - d0) * (float(i) + 0.5f) / float(count), s);
      }
      worst = std::max(worst, std::fabs(float(fine / count) - cr::SegmentCover(d0, d1, s)));
    }
    std::printf("  segment cover against a fine sum: worst difference %.5f\n", worst);
    Check(worst < 0.003f, "the cover of a segment is the mean of the window along it");
    const cr::WindowShape s = cr::MakeWindowShape(3.0f, 0.3f, 0.0f);
    Check(cr::SegmentCover(-0.4f, -0.1f, s) == 0.0f && cr::SegmentCover(0.5f, 0.9f, s) == 0.0f, "a segment wholly in front of the window or past it has no cover");
    Check(cr::SegmentCover(0.1f, 0.12f, s) == 1.0f, "a segment wholly on the plateau has the whole cover");
    Check(cr::SegmentCover(-0.2f, 0.7f, s) > 0.1f && cr::SegmentCover(-0.2f, 0.7f, s) < 0.4f,
          "a segment that steps over the whole window (a thin shell between two points) covers the share of it the window takes");
    Check(Near(cr::SegmentCover(0.1f, 0.1f, s), 1.0) && Near(cr::SegmentCover(0.1f, 0.1f + 5e-5f, s), 1.0, 1e-4),
          "a segment with the same diff at both ends is the window there");
  }

  // the slack: more for a slanted surface, for a coarse image and for a far car, never more than 5 cm
  {
    const float grazing = cr::SurfaceSlack(3.0f, 1080.0f, 1.7f, 0.15f);
    const float facing = cr::SurfaceSlack(3.0f, 1080.0f, 1.7f, 0.9f);
    Check(grazing > facing && facing > 0.0f && grazing < 0.05f, "a surface seen at a slant has more slack than one seen square on");
    Check(cr::SurfaceSlack(3.0f, 540.0f, 1.7f, 0.5f) > cr::SurfaceSlack(3.0f, 1080.0f, 1.7f, 0.5f), "a coarser history has more slack");
    Check(cr::SurfaceSlack(30.0f, 360.0f, 1.0f, 0.05f) == 0.05f, "the slack is capped at 5 cm");
  }

  // the segments: what two ends take, the hidden face, the outline of a part, the fade, what the sum makes of it
  {
    const cr::WindowShape s = cr::MakeWindowShape(3.0f, 0.3f, 0.0f);
    // ends of segments: on, diff, the pixel's view distance, the distance along the ray
    const cr::SegmentEnd in_front{true, -0.3f, 2.85f, 0.25f};
    const cr::SegmentEnd inside{true, 0.1f, 2.9f, 0.1f};
    const cr::SegmentEnd inside_later{true, 0.12f, 2.9f, 0.25f};
    const cr::SegmentEnd none{false, 4.0f, 0.0f, 0.25f};
    const cr::SegmentEnd behind_door{true, -0.3f, 3.3f, 0.25f};  // a ray point in front of the door layer (3.3); the part (inside) is 0.4 nearer
    Check(cr::SegmentOf(none, none, s).cover == 0.0f && cr::SegmentOf(none, inside, s).cover == 0.0f && cr::SegmentOf(inside, none, s).cover == 0.0f,
          "a segment with an end on no vehicle takes nothing by itself (its outline is looked for by the halvings)");
    Check(cr::InWindow(inside, s) && !cr::InWindow(in_front, s) && !cr::InWindow(none, s) && !cr::InWindow({true, s.bias, 2.9f, 0.1f}, s) &&
              cr::InWindow({true, s.bias + 1e-3f, 2.9f, 0.1f}, s) && !cr::InWindow({true, 0.3f, 2.9f, 0.1f}, s) &&
              cr::InWindow({true, 0.299f, 2.9f, 0.1f}, s),
          "in the window: on a vehicle, past the bias and short of the thickness");
    Check(cr::HasOutline(none, inside, s) && cr::HasOutline(inside, none, s) && cr::HasOutline(behind_door, inside, s) &&
              !cr::HasOutline(none, none, s) && !cr::HasOutline(inside, inside_later, s) && !cr::HasOutline(none, in_front, s) &&
              !cr::HasOutline(in_front, inside, s),
          "an outline is in a segment where one end is in a window and the other is on no vehicle or on a layer 0.15 behind; not where the "
          "surface is continuous (a wall the ray goes into is the integral's)");
    Check(cr::MayCover(inside, inside_later, s) && cr::MayCover(in_front, inside, s) && !cr::MayCover(in_front, {true, -0.1f, 2.9f, 0.4f}, s) &&
              !cr::MayCover({true, 0.31f, 2.9f, 0.1f}, {true, 0.5f, 2.9f, 0.4f}, s) &&
              cr::MayCover({true, -0.2f, 2.9f, 0.1f}, {true, 0.5f, 2.9f, 0.4f}, s) && !cr::MayCover(none, inside, s),
          "a segment can take something where the line in diff passes through the open window (a shell stepped over included), never off a vehicle");
    bool shortcut_exact = true;
    for (float d0 = -0.5f; d0 <= 0.7f; d0 += 0.013f) {
      for (float d1 = -0.5f; d1 <= 0.7f; d1 += 0.017f) {
        const cr::SegmentEnd e0{true, d0, 2.9f, 0.1f};
        const cr::SegmentEnd e1{true, d1, 2.9f, 0.2f};
        shortcut_exact = shortcut_exact && (cr::MayCover(e0, e1, s) || cr::SegmentOf(e0, e1, s).cover < 1e-4f);
      }
    }
    Check(shortcut_exact, "where a segment cannot take anything its cover is under 0.0001 (float rounding): skipping it changes nothing");
    Check(Near(cr::OutlineShare(0.0f, 0.4f, 0.6f, 1.0f), 0.5) && Near(cr::OutlineShare(1.0f, 0.4f, 0.6f, 0.0f), 0.5) &&
              Near(cr::OutlineShare(0.0f, 0.0f, 0.125f, 1.0f), 0.0625) && cr::OutlineShare(0.5f, 0.5f, 0.5f, 0.5f) == 0.0f,
          "the share of the segment on the window's side of the outline, from either end");
    Check(cr::SegmentOf(inside, inside_later, s).cover == 1.0f && cr::SegmentOf(inside, inside_later, s).face == 1.0f,
          "both ends inside: the whole segment, the visible face");
    Check(cr::SegmentOf(in_front, inside_later, s).cover > 0.0f && cr::SegmentOf(in_front, inside_later, s).cover < 1.0f &&
              cr::SegmentOf(in_front, inside_later, s).face == 1.0f,
          "a ray that goes in at the front covers the part of the segment that is inside, the visible face");
    const cr::SegmentEnd out_back{true, 0.28f, 2.72f, 0.1f};
    Check(cr::SegmentOf(out_back, in_front, s).face == 1.0f - cr::Approach(0.28f, -0.3f, 0.15f, 0.3f) &&
              cr::SegmentOf(out_back, in_front, s).face == 0.0f,
          "a ray that comes back out in front of the surface from behind it reaches the hidden face");
    Check(Near(cr::SegmentOf(inside, inside_later, s).colour_share, 0.5, 1e-3), "two ends with equal windows share the colour evenly");
    Check(cr::Approach(-1.0f, 0.0f, 0.15f, 0.3f) == 0.0f && cr::Approach(0.1f, 0.2f, 0.15f, 0.3f) == 0.0f &&
              cr::Approach(0.2f, 0.19f, 0.15f, 0.3f) > 0.0f && cr::Approach(0.2f, 0.1f, 0.15f, 0.3f) == 1.0f,
          "approach: none from in front or going deeper, full once the ray comes a tenth of the thickness nearer from behind");
    Check(Near(cr::Approach(0.2f, 0.185f, 0.15f, 0.3f), cr::Approach(0.2f, 0.1925f, 0.075f, 0.3f), 1e-5),
          "approach goes by the slope: half the segment, half the rise, the same face");
    Check(cr::RayFade(0.0f) == 1.0f && cr::RayFade(cr::kFadeFrom) == 1.0f && cr::RayFade(cr::kFadeTo) == 0.0f && cr::RayFade(1.0f) == 0.0f &&
              cr::RayFade(0.6f) > 0.0f && cr::RayFade(0.6f) < 1.0f,
          "the fade is 1 up to 0.3 of the ray, falls smoothly, and is 0 from 0.9");
    Check(Near(cr::FullPoints(0.3f, 12), 3.6, 1e-6) && Near(cr::FullPoints(0.3f, 24), 7.2, 1e-6) && cr::FullPoints(0.05f, 4) == 1.0f,
          "the covers that take the whole hit: 0.3 of the ray's points (3.6 of 12, 7.2 of 24), never less than one");

    // `segments` whole segments (cover 1) near the shaded point, `face` and the screen edge's `edge` on all of them
    auto whole = [](float face, float edge, int segments) {
      cr::HitSum h;
      for (int i = 0; i < segments; ++i) {
        h.Add({1.0f, face, 1.0f}, 0.1f + 0.1f * float(i), 0.2f + 0.1f * float(i), 2.0f, edge, 0.0f, 1.0f);
      }
      return h;
    };
    const cr::HitSum one = whole(1.0f, 1.0f, 1);
    Check(Near(one.Weight(0.3f, 12), 1.0 / 3.6, 1e-5) && Near(one.Colour(), 1.0) && Near(one.Distance(2.0f), 0.075, 1e-5),
          "one whole segment: a point's share (1 of 3.6) of the hit, the second end's colour, the middle's distance");
    Check(Near(whole(1.0f, 1.0f, 4).Weight(0.3f, 12), 1.0) && Near(whole(1.0f, 1.0f, 3).Weight(0.3f, 12), 3.0 / 3.6, 1e-5) &&
              Near(whole(0.0f, 1.0f, 4).Weight(0.3f, 12), 2.0 / 3.6, 1e-5) && Near(whole(0.0f, 1.0f, 4).Face(), 0.0) &&
              Near(whole(1.0f, 1.0f, 4).Face(), 1.0),
          "four whole segments on the visible face take the whole hit; the same from behind take half of it");
    Check(Near(whole(1.0f, 1.0f, 1).Weight(0.3f, 24), 1.0 / 7.2, 1e-5),
          "with 24 points a point's step is half as big: the whole hit takes 7.2 covers");
    cr::HitSum thin;
    thin.Add({0.4f, 1.0f, 1.0f}, 0.7f, 0.8f, 2.0f, 1.0f, 0.0f, 1.0f);
    Check(thin.Weight(0.3f, 12) < 0.25f && thin.Weight(0.3f, 12) > 0.1f, "a segment of which the outline of a part takes 0.4: a faint hit");
    cr::HitSum faded;
    faded.Add({1.0f, 1.0f, 1.0f}, 1.6f, 1.9f, 2.0f, 1.0f, 0.0f, 1.0f);
    Check(faded.Weight(0.3f, 12) < 0.05f, "a whole segment at 0.9 of the ray takes almost nothing");
    Check(Near(whole(1.0f, 0.5f, 4).Weight(0.3f, 12), 2.0 / 3.6, 1e-5) && Near(whole(1.0f, 0.0f, 4).Weight(0.3f, 12), 0.0),
          "the screen edge's fade weighs the segments");
    Check(Near(whole(1.0f, 1.0f, 1).Weight(0.6f, 12), whole(1.0f, 1.0f, 1).Weight(0.3f, 12) * 0.5, 1e-5), "a larger share for the whole hit makes it lighter in proportion");
  }

  // what the pixels show: the darkest the occlusion makes the reflection, and the colour of a hidden face
  Check(Near(cr::Darkened(1.0f, 1.0f), 0.15, 1e-6) && cr::Darkened(1.0f, 0.0f) == 1.0f && cr::Darkened(1.0f, 7.0f) == cr::Darkened(1.0f, 1.0f),
        "a certain, wide hit leaves 15% of the reflection, no weight none changed");
  Check(cr::Darkened(1.0f, cr::kBackFloor) > 0.55f, "a hit that is probably the hidden face leaves more than half of the reflection");
  Check(Near(cr::SeenColour(0.5f, 0.05f, 1.0f), 0.05, 1e-6) && Near(cr::SeenColour(0.5f, 0.05f, 0.0f), 0.5 * cr::kShade, 1e-6),
        "mode 2: the hit's colour from the front, the paint's own colour in shade from behind");

  // -------------------------------------------------------------------------------------------------------------------
  // a part seen at several widths and offsets: what the picture does from one pixel to the next
  // -------------------------------------------------------------------------------------------------------------------
  // A thin arm 0.15 m in front of the ray's start (view distance 3.15 against the ray's 3.0, going 0.5 m deeper per metre), the ray
  // crossing the picture at 250 texels a metre (the steps of the ray, 12 over 2 m, are 12 to 63 texels apart on the screen). A row of
  // 1100 pixels, each half a texel from the last (the history of a 4K picture is half the size): the arm is seen at every offset of
  // its width and of the points. Version 3 caught a part narrower than the distance between two points by one pixel and not by the
  // next, in bands that follow its outline, each a whole sample wide: the weight stepped by up to 0.4. Version 4 counts the length of
  // the ray that is inside the part, so a pixel's weight follows the pixel.
  auto arm_in_the_void = [](float first, float width) -> History {
    return [first, width](float x) {
      Probe p;
      p.on = x >= first && x < first + width;
      p.stored = p.on ? 3.15f : 0.0f;
      p.colour = 1.0f;
      return p;
    };
  };
  // the same arm in front of a door the shaded points are on (view distance 3.45, a layer behind: the history holds it everywhere)
  auto arm_over_a_door = [](float first, float width) -> History {
    return [first, width](float x) {
      Probe p;
      p.on = true;
      const bool arm = x >= first && x < first + width;
      p.stored = arm ? 3.15f : 3.45f;
      p.colour = arm ? 1.0f : 0.25f;
      return p;
    };
  };
  Ray ray;
  ray.d0 = 3.0f;
  ray.dwdt = 0.5f;
  ray.vx = -250.0f;
  Ray door_ray = ray;
  door_ray.d0 = 3.3f;
  door_ray.dwdt = -0.3f;  // the rays of a door leave it towards the camera: the door's own layer is never behind them
  float worst_new = 0.0f;
  float worst_old = 0.0f;
  float weakest_old = 1.0f;
  float weakest_peak = 1.0f;
  bool bounded = true;
  long taps_total = 0;
  long taps_rays = 0;
  int taps_most = 0;
  int widths_seen = 0;
  for (const auto& scene : {0, 1}) {
    for (float width : {6.0f, 12.0f, 24.0f, 48.0f}) {
      const History history = scene == 0 ? arm_in_the_void(200.0f, width) : arm_over_a_door(200.0f, width);
      const Ray r = scene == 0 ? ray : door_ray;
      // a pixel whose own texel is the arm is a pixel of the arm, not of the door: it is not in the row
      auto shaded = [&](float x0) { return scene == 0 || !(std::floor(x0) >= 200.0f && std::floor(x0) < 200.0f + width); };
      const RowStats now = Row([&](float x0) { Ray q = r; q.x0 = x0; return shaded(x0) ? March(history, q, 12, 2.0f, 0.3f, 0.0f).weight : -1.0f; }, 0, 1100);
      const RowStats old = Row([&](float x0) { Ray q = r; q.x0 = x0; return shaded(x0) ? VersionThree(history, q, 12, 2.0f, 0.3f, 0.0f) : -1.0f; }, 0, 1100);
      std::printf("  %s, arm %2.0f texels: neighbouring pixels differ by up to %.3f (version 3: %.3f), peak %.2f (version 3: %.2f)\n",
                  scene == 0 ? "in the void " : "over a door", width, now.jump, old.jump, now.peak, old.peak);
      worst_new = std::max(worst_new, now.jump);
      worst_old = std::max(worst_old, old.jump);
      weakest_old = std::min(weakest_old, old.peak);
      weakest_peak = std::min(weakest_peak, now.peak);
      bounded = bounded && now.peak <= 1.0f && now.jump < old.jump;
      for (int px = 0; px < 1100; px += 7) {
        Ray rq = r;
        rq.x0 = 0.5f * float(px);
        const int taps = March(history, rq, 12, 2.0f, 0.3f, 0.0f).taps;
        taps_total += taps;
        ++taps_rays;
        taps_most = std::max(taps_most, taps);
      }
      ++widths_seen;
    }
  }
  Check(widths_seen == 8 && worst_new < kContinuityBound,
        "a thin part seen at several widths and offsets: neighbouring pixels differ by less than " + std::to_string(kContinuityBound));
  Check(worst_old > 0.7f, "(the previous version's rows step by more than 0.7 somewhere: the bands)");
  Check(bounded && weakest_peak > 0.05f, "every row steps by less than version 3's, and the arm is still seen (a peak over 0.05)");
  std::printf("  reads of the history a ray: %.2f on average over these rows (they all cross the part: the worst case), %d at most\n", double(taps_total) / double(taps_rays), taps_most);
  Check(taps_most <= 12 + cr::kOutlineSteps * 12 && double(taps_total) / double(taps_rays) < 14.0,
        "the reads of the history: 12 a ray and three more for each segment that holds a part's outline (under 14 on average even where every ray meets a part)");
  {
    const History empty = [](float) { return Probe(); };
    Check(March(empty, ray, 12, 2.0f, 0.3f, 0.0f).taps == 12 && March(empty, ray, 12, 2.0f, 0.3f, 0.0f).weight == 0.0f,
          "a ray that meets no vehicle reads the history 12 times and takes nothing");
  }

  // a wide part (a mirror housing, a wing's blade: the whole slice, 3.15 against the ray's 3.0 going deeper by 0.5 a metre): the ray
  // is inside it for half a metre, and the hit is whole at every pixel; a part with the same outline and the ray going deeper
  // the other way round (from behind) takes less
  {
    const History shell = [](float) { Probe p; p.on = true; p.stored = 3.15f; p.colour = 1.0f; return p; };
    float shell_min = 1.0f;
    for (int px = 0; px < 600; ++px) {
      Ray rq = ray;
      rq.x0 = float(px);
      shell_min = std::min(shell_min, March(shell, rq, 12, 2.0f, 0.3f, 0.0f).weight);
    }
    std::printf("  a wide part the ray goes through: weakest weight %.3f\n", shell_min);
    Check(shell_min > 0.55f, "a wide part the ray goes behind from the front keeps over half of the hit at every pixel (the wing in the trunk lid)");

    // the hidden face: the ray comes from far behind the stored surface towards it (an underside the picture does not hold)
    const History plane = [](float) { Probe p; p.on = true; p.stored = 3.0f; p.colour = 1.0f; return p; };
    Ray from_behind;
    from_behind.d0 = 3.6f;
    from_behind.dwdt = -0.9f;
    from_behind.vx = 0.0f;
    Ray from_front;
    from_front.d0 = 2.8f;
    from_front.dwdt = 0.9f;
    from_front.vx = 0.0f;
    const RayResult hidden = March(plane, from_behind, 12, 2.0f, 0.3f, 0.0f);
    const RayResult front = March(plane, from_front, 12, 2.0f, 0.3f, 0.0f);
    std::printf("  hidden face weight %.3f (face %.2f), front face %.3f (face %.2f)\n", hidden.weight, hidden.face, front.weight, front.face);
    Check(front.weight > 0.4f && front.face == 1.0f && hidden.face < 0.3f && hidden.weight < 0.75f * front.weight && hidden.weight > 0.15f,
          "a ray that reaches the surface from behind takes less than three quarters of what the same ray takes from the front");
    Check(cr::Darkened(1.0f, hidden.weight) > 0.6f, "...so the reflection of a thin part keeps over half of its light");
  }

  // a thin shell and the ray's points: the shell's depth moves under the ray 1 mm at a time, a steep ray (1 m deeper per metre) has
  // its points land anywhere in or out of the shell's window, and the weight has no step bigger than 0.05 (version 3 had a thickness
  // that grew with the step to catch it, and a point that stepped over it one pixel and not the next)
  {
    float biggest = 0.0f;
    float previous = -1.0f;
    float lowest = 1.0f;
    for (int i = 0; i <= 600; ++i) {
      const float stored = 3.0f + 0.001f * float(i);
      const History shell = [stored](float) { Probe p; p.on = true; p.stored = stored; p.colour = 1.0f; return p; };
      Ray rq;
      rq.d0 = 2.0f;
      rq.dwdt = 1.0f;
      rq.vx = 0.0f;
      const float w = March(shell, rq, 12, 2.0f, 0.3f, 0.0f).weight;
      if (previous >= 0.0f) biggest = std::max(biggest, std::fabs(w - previous));
      lowest = std::min(lowest, w);
      previous = w;
    }
    std::printf("  shell moving under a steep ray: biggest step %.4f, lowest weight %.3f\n", biggest, lowest);
    Check(biggest < 0.05f, "the weight of a shell follows its depth: no step over 0.05 per millimetre wherever the points land");
  }

  // a part met only far along the ray (a 30 texel arm whose window is open for 1.1 to 1.4 m, 0.55 to 0.7 of the length: a lone catch far
  // from the shaded point, the stray blob of the pictures of version 3): a faint hit; a part met in the first half metre takes much more
  {
    float far_peak = 0.0f;
    float far_old = 0.0f;
    float near_peak = 0.0f;
    const History far_arm = [](float x) { Probe a; a.on = x >= 100.0f && x < 130.0f; a.stored = 3.1f; a.colour = 1.0f; return a; };
    const History near_arm = [](float x) { Probe a; a.on = x >= 100.0f && x < 130.0f; a.stored = 2.2f; a.colour = 1.0f; return a; };
    for (int px = 0; px < 1400; ++px) {
      Ray rq = ray;
      rq.x0 = 0.5f * float(px);
      rq.d0 = 2.0f;
      rq.dwdt = 1.0f;  // deeper by a metre a metre: the window of the far arm is open from 1.1 to 1.4 m along the ray, the near arm's from 0.2 to 0.5
      far_peak = std::max(far_peak, March(far_arm, rq, 12, 2.0f, 0.3f, 0.0f).weight);
      far_old = std::max(far_old, VersionThree(far_arm, rq, 12, 2.0f, 0.3f, 0.0f));
      near_peak = std::max(near_peak, March(near_arm, rq, 12, 2.0f, 0.3f, 0.0f).weight);
    }
    std::printf("  a part met far along the ray: peak %.3f (version 3: %.3f), near: %.3f\n", far_peak, far_old, near_peak);
    Check(far_old > 0.3f && far_peak < 0.15f && near_peak > 0.25f,
          "a lone catch far along the ray is a faint hit (under 0.15; version 3 took over 0.3: the stray blob), the same part near the shaded point is not");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

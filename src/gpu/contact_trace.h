#pragma once
// ===========================================================================
// MCLA GPU renderer -- contact shadows: the rules of the trace (pure maths, no GPU)
// ===========================================================================
// hlsl/ao_contact_ps.hlsl marches a ray toward the sun over the picture's linear depth, ao.cpp decides
// what it is given, and the numbers that make a contact shadow crisp or soft live HERE, with a program
// of their own that checks them (contact_trace_test.cpp: it needs no renderer, no ReXGlue and no GPU).
// The shader repeats the constants (marked "contact_trace.h" in its comments: change them here first)
// and this file holds a C++ twin of its march, `March`, which the check runs over synthetic scenes: a
// plane seen at every angle must never shadow itself, a kerb must shadow what is behind it, and the
// shadow's edge must be as narrow as the pixels allow at every render scale.
//
// Why the first version was soft (owner, 2026-11-13: "look way too soft right now"). It was not the
// size of the images: at render scale 3 the passes already ran at 3840x2160 (the first-frame line
// "images 3840x2160 for a 3840x2160 picture"), and at the high setting they run at the render size at
// every scale. It was the look of the trace, which was tuned at 1280x720 and kept in world units:
//   1. the ray's end fade 1 - along^2 made every shadow a gradient (darkest at its caster, a quarter
//      lighter half way along the ray, 44% at three quarters): EndFade holds full darkness over the
//      first 60% of the ray and fades only the last part;
//   2. the depth bias 0.02 + 0.004 z m and a ramp as wide as the bias (6 cm each at 10 m) are 10 pixels
//      at 1280x720 and 30 at 3840x2160, and the edge of a shadow is about as wide as the ramp over
//      the slope of the ray: HitEpsilon and HitRamp are a few millimetres plus a few footprints (the
//      world size of one pixel at that distance), so the edge is a few pixels at every scale;
//   3. two plane-only bilateral passes (cost 0.27 ms of the 0.50 ms the contact shadows took at
//      3840x2160 on the RTX 5080, more than the march itself) blurred every edge across the surface:
//      one pass with a weight on the difference of the visibility keeps edges and removes noise;
//   4. equally spaced steps of 7.75 cm (16 over 1.24 m) with a start offset of up to a whole step:
//      the distance now grows as the square of the step (4 cm at the contact, 12 cm at the far end).
// Cost: the march is unchanged (16 or 8 steps at the same pixels), the filter is one pass instead of two.
// The cheaper path (mcla_gpu_contact_shadows_fast: an adaptive step count and the filter inside the apply pass) is
// described where its numbers are, below ("the adaptive step count"), with its cost model (CostModelFast).
//
// Used by ao.cpp: LengthGrow (the ray's length for a low sun), the default fade, CostModel (the log line);
// the rest is mirrored in the shader and run by the check.
// ===========================================================================

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::contact_trace {

// ---------------------------------------------------------------------------
// the ray
// ---------------------------------------------------------------------------

// The ray grows with a low sun (its shadows are long): kLengthTan / tan(elevation), kept between 1 and
// kLengthMaxScale times the setting's length. At 18.7 degrees: 0.6 m x 2.07 = 1.24 m.
inline constexpr double kLengthTan = 0.7;
inline constexpr double kLengthMaxScale = 3.0;
// The share of the ray over which the shadow stays full before it fades out (mcla_gpu_contact_shadows_fade).
inline constexpr float kDefaultFadeStart = 0.6f;
inline constexpr float kFadeStartMin = 0.2f;
inline constexpr float kFadeStartMax = 0.98f;
// The steps at the low and the high setting.
inline constexpr uint32_t kStepsLow = 8;
inline constexpr uint32_t kStepsHigh = 16;

inline double LengthGrow(double elevation_degrees) {
  const double pi = 3.14159265358979323846;
  const double tan_elevation = std::tan(elevation_degrees * pi / 180.0);
  return tan_elevation > 1e-3 ? std::clamp(kLengthTan / tan_elevation, 1.0, kLengthMaxScale)
                              : kLengthMaxScale;
}

inline float Saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
inline float Smoothstep(float edge0, float edge1, float x) {
  const float t = Saturate((x - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}

// The share of the ray's length (0 to 1) at step `index` of `steps`, the start offset `noise` (0 to 1)
// inside the first step: the square of the step's share, so the steps are fine at the contact.
inline float Along(uint32_t index, float noise, uint32_t steps) {
  const float t = (float(index) + noise) / float(steps);
  return t * t;
}
// The world distance between that step and the next: 2 t length / steps.
inline float StepLength(uint32_t index, float noise, uint32_t steps, float length) {
  const float t = (float(index) + noise) / float(steps);
  return 2.0f * t * length / float(steps);
}
// The shadow's weight at a share `along` of the ray: 1 up to fade_start, a smoothstep to 0 at the end.
inline float EndFade(float along, float fade_start) {
  return 1.0f - Smoothstep(std::min(Saturate(fade_start), kFadeStartMax), 1.0f, along);
}
// The first version's: darkest at the caster, a quarter lighter half way along the ray.
inline float ClassicEndFade(float along) { return 1.0f - along * along; }

// ---------------------------------------------------------------------------
// the depth tolerance, in footprints
// ---------------------------------------------------------------------------

// A footprint is the world size of one pixel of the depth image at depth z: 2 z tan(half fov) / height.
inline float FootprintUnit(float tan_half_fov_y, float height_pixels) {
  return 2.0f * tan_half_fov_y / height_pixels;
}
inline float Footprint(float z, float tan_half_fov_y, float height_pixels) {
  return FootprintUnit(tan_half_fov_y, height_pixels) * z;
}
inline constexpr float kEpsFloor = 0.004f;      // m
inline constexpr float kEpsPixels = 1.5f;       // footprints
inline constexpr float kRampFloor = 0.004f;     // m
inline constexpr float kRampPixels = 2.0f;      // footprints
inline constexpr float kRampStepShare = 0.5f;   // of the depth a step changes by
inline constexpr float kLiftFloor = 0.006f;     // m
inline constexpr float kLiftPixels = 4.0f;      // footprints
inline constexpr float kSunFacingLow = 0.03f;
inline constexpr float kSunFacingHigh = 0.15f;
inline constexpr float kNearZ = 0.05f;

// A step is hidden once the surface at its pixel is nearer than it by more than this ...
inline float HitEpsilon(float footprint) { return kEpsFloor + kEpsPixels * footprint; }
// ... and fully so after this much more. step_length * |sun.z| is the depth a step changes by.
inline float HitRamp(float footprint, float step_length, float sun_z) {
  return kRampFloor + kRampPixels * footprint + kRampStepShare * step_length * std::fabs(sun_z);
}
// How far the ray's start is lifted off its surface, along the normal.
inline float RayLift(float footprint) { return kLiftFloor + kLiftPixels * footprint; }
// The first version's bias and ramp (both): 6 cm at 10 m at any scale.
inline float ClassicBias(float z) { return 0.02f + 0.004f * z; }

// ---------------------------------------------------------------------------
// the filter (ao_common.hlsli AoBilateralEdge)
// ---------------------------------------------------------------------------

inline constexpr float kEdgeRangeSigma = 0.2f;
inline constexpr float kEdgeSpatialSigma = 0.9f;
inline constexpr float kEdgePlanePixels = 4.0f;
inline constexpr float kEdgePlaneFloor = 0.01f;
// The weight of a neighbour whose visibility differs by `difference` from the pixel's own.
inline float EdgeRangeWeight(float difference) {
  const float x = difference / kEdgeRangeSigma;
  return std::exp(-0.5f * x * x);
}
// The apply pass does the filter itself with mcla_gpu_contact_shadows_fast and leaves out a pixel the contact shadows
// change the picture's factor by less than this (hlsl/ao_apply_ps.hlsl kFoldedFilterSkip): the factor is
// 1 - weight (1 - cs) with cs in 0 to 1, so it moves by at most the weight.
inline constexpr float kFoldedFilterSkip = 0.002f;

// ---------------------------------------------------------------------------
// the cost
// ---------------------------------------------------------------------------

// Measured on the RTX 5080 at 3840x2160 (render scale 3, K-DAY 12 h, pass timing, perf\out\GFXW1.txt:
// gk-w1-cost-day-s3-c1/c2): the march at 16 steps 0.232 ms, the two filter passes together 0.271 ms,
// the occlusion's own 0.82 ms next to them. The pass costs scale with the pixels and the steps.
inline constexpr double kMeasuredMegapixels = 3840.0 * 2160.0 / 1e6;
inline constexpr double kMeasuredMarchMs = 0.232;
inline constexpr double kMeasuredFilterPassMs = 0.2705 / 2.0;
inline constexpr double kMsPerMegaStep = kMeasuredMarchMs / (kMeasuredMegapixels * double(kStepsHigh));
inline constexpr double kMsPerMegapixelPass = kMeasuredFilterPassMs / kMeasuredMegapixels;
// The new march does a little more arithmetic per step (the footprint tolerance, the squared step) and
// no more loads; the march is bound by its depth reads. CHECK: not measured, assumed from the extra ALU.
inline constexpr double kMarchAluFactor = 1.2;

struct Cost {
  double megapixels = 0.0;        // traced
  double mega_steps = 0.0;        // depth reads of the march
  double filter_passes = 0.0;
  double march_ms = 0.0;
  double filter_ms = 0.0;
  double total_ms = 0.0;          // an RTX 5080's; other GPUs scale with their throughput
};
inline Cost CostModel(uint32_t width, uint32_t height, uint32_t steps, uint32_t filter_passes,
                      bool new_march) {
  Cost c;
  c.megapixels = double(width) * double(height) / 1e6;
  c.mega_steps = c.megapixels * double(steps);
  c.filter_passes = double(filter_passes);
  c.march_ms = c.mega_steps * kMsPerMegaStep * (new_march ? kMarchAluFactor : 1.0);
  c.filter_ms = c.megapixels * kMsPerMegapixelPass * double(filter_passes);
  c.total_ms = c.march_ms + c.filter_ms;
  return c;
}

// The cheaper contact shadows (mcla_gpu_contact_shadows_fast). NOT MEASURED: both parts are modelled from what the
// passes read, and the first measurement is the off/on pair of the run plan (the numbers here are the thing to check).
//   The march: of the 24 depth reads a pixel makes at 16 steps (the pixel's own, 4 for its normal, 3 of the
//   ground-under-a-vehicle term, 16 steps) only the steps shrink, so a third of the march does not scale with the
//   step count. The average share of the setting's steps the adaptive count keeps is the twin's, for a street
//   (contact_trace_test.cpp: AdaptiveStepShare is checked against it).
//   The filter in the apply: the filter pass reads 22 values a pixel (its depth and the normal's 4, 8 neighbours'
//   depths, 9 visibilities) and writes one; inside the apply the four neighbours' depths and the pixel's own are
//   there already, so it reads 13 (4 diagonal depths, 9 visibilities) and writes nothing, and a pixel the contact
//   shadows cannot darken (the game's own shadow, a surface turned from the sun) skips it. 13 of 22 reads is 59%;
//   55% is taken for the pass and the target write that are gone. The apply may need more registers and cost the
//   plain apply a little (not in the model).
inline constexpr double kMarchFixedShare = 8.0 / 24.0;
inline constexpr double kFoldedFilterShare = 0.55;
// The share of the setting's steps the adaptive count keeps on average, by the picture's height in pixels: the twin's
// street scene at 720p, 1080p and 2160p (the check compares them), between them linearly, flat outside.
inline constexpr double kAdaptShare720 = 0.88;
inline constexpr double kAdaptShare1080 = 0.94;
inline constexpr double kAdaptShare2160 = 0.99;
inline double AdaptiveStepShare(uint32_t height) {
  const double h = double(height);
  if (h <= 720.0) {
    return kAdaptShare720;
  }
  if (h <= 1080.0) {
    return kAdaptShare720 + (kAdaptShare1080 - kAdaptShare720) * (h - 720.0) / 360.0;
  }
  if (h <= 2160.0) {
    return kAdaptShare1080 + (kAdaptShare2160 - kAdaptShare1080) * (h - 1080.0) / 1080.0;
  }
  return kAdaptShare2160;
}
// `folded`: the filter is inside the apply (false at half size, where the filter pass stays: one pass).
inline Cost CostModelFast(uint32_t width, uint32_t height, uint32_t steps, bool folded) {
  Cost c;
  const double share = AdaptiveStepShare(height);
  c.megapixels = double(width) * double(height) / 1e6;
  c.mega_steps = c.megapixels * double(steps) * share;
  c.filter_passes = folded ? kFoldedFilterShare : 1.0;
  c.march_ms = c.megapixels * double(steps) * kMsPerMegaStep * kMarchAluFactor *
               (kMarchFixedShare + (1.0 - kMarchFixedShare) * share);
  c.filter_ms = c.megapixels * kMsPerMegapixelPass * c.filter_passes;
  c.total_ms = c.march_ms + c.filter_ms;
  return c;
}

// ---------------------------------------------------------------------------
// the reference march (the twin of hlsl/ao_contact_ps.hlsl, for the check)
// ---------------------------------------------------------------------------

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// The depth image's lens: tan of the half field of view and the size in pixels. View space: x right,
// y up, z forward; pixel rows go down.
struct Camera {
  float tan_x = 0.394f;
  float tan_y = 0.2217f;
  int width = 1280;
  int height = 720;
};
// The view position of the pixel position (px, py) (pixel units, centres at +0.5) at distance t.
inline Vec3 ViewPos(const Camera& c, float px, float py, float t) {
  const float u = px / float(c.width);
  const float v = py / float(c.height);
  return {(u * 2.0f - 1.0f) * c.tan_x * t, (1.0f - v * 2.0f) * c.tan_y * t, t};
}

// The view-space normal from the linear depth at (x, y), as ao_common.hlsli AoNormalFromImage: from the
// neighbour on each axis whose depth is nearer to the pixel's, facing the camera. depth_at(x, y).
template <class DepthAt>
Vec3 NormalFromDepth(const Camera& c, DepthAt&& depth_at, int x, int y, float tc, Vec3 pc) {
  const int xl = std::max(x - 1, 0), xr = std::min(x + 1, c.width - 1);
  const int yt = std::max(y - 1, 0), yb = std::min(y + 1, c.height - 1);
  const float tl = depth_at(xl, y), tr = depth_at(xr, y), tt = depth_at(x, yt), tb = depth_at(x, yb);
  const Vec3 h = std::fabs(tl - tc) < std::fabs(tr - tc)
                     ? pc - ViewPos(c, float(xl) + 0.5f, float(y) + 0.5f, tl)
                     : ViewPos(c, float(xr) + 0.5f, float(y) + 0.5f, tr) - pc;
  const Vec3 v = std::fabs(tt - tc) < std::fabs(tb - tc)
                     ? ViewPos(c, float(x) + 0.5f, float(yt) + 0.5f, tt) - pc
                     : pc - ViewPos(c, float(x) + 0.5f, float(yb) + 0.5f, tb);
  const Vec3 n = Cross(v, h);
  const float len = std::sqrt(Dot(n, n));
  return len > 1e-20f ? n * (1.0f / len) : Vec3{0.0f, 0.0f, -1.0f};
}

// The filter's twin: the denoise pass's AoBilateralEdge, and, with `cross` (the four neighbours' depths left, right,
// top, bottom, each at the position clamped into the image, as the apply pass reads them for its normal), the apply's
// AoBilateralEdgeFolded, which reads only the diagonal neighbours' depths itself. Both give the same value.
template <class DepthAt, class VisAt>
float EdgeFilter(const Camera& c, DepthAt&& depth_at, VisAt&& vis_at, int x, int y, float tc, Vec3 pc, Vec3 normal,
                 const float* cross = nullptr) {
  const float footprint = FootprintUnit(c.tan_y, float(c.height)) * tc;
  const float plane_sigma = kEdgePlaneFloor + kEdgePlanePixels * footprint;
  const float centre = vis_at(x, y);
  float sum = centre;
  float weights = 1.0f;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      const int qx = std::clamp(x + dx, 0, c.width - 1), qy = std::clamp(y + dy, 0, c.height - 1);
      float tq;
      if (cross && dy == 0) {
        tq = dx < 0 ? cross[0] : cross[1];
      } else if (cross && dx == 0) {
        tq = dy < 0 ? cross[2] : cross[3];
      } else {
        tq = depth_at(qx, qy);
      }
      const Vec3 pq = ViewPos(c, float(qx) + 0.5f, float(qy) + 0.5f, tq);
      const float off_plane = Dot(normal, pq - pc) / plane_sigma;
      const float w_depth = std::exp(-0.5f * off_plane * off_plane);
      const float w_space = std::exp(-0.5f * float(dx * dx + dy * dy) / (kEdgeSpatialSigma * kEdgeSpatialSigma));
      const float value = vis_at(qx, qy);
      const float w = w_depth * w_space * EdgeRangeWeight(value - centre);
      sum += value * w;
      weights += w;
    }
  }
  return sum / weights;
}

// ---------------------------------------------------------------------------
// the ground under a vehicle
// ---------------------------------------------------------------------------
//
// Owner, 2026-11-13: the ground under TRAFFIC cars is bright and, with the contact shadows on, only the
// wheels and the underside's edges darken. Why [I: from the recompiled guest code, not seen in a run; CHECK
// in a drive]: the game's sun shadow map holds the player's car and no other. Vehicle::Draw (guest
// 0x82360518) draws a vehicle into the cascade phases only when its byte +594 is set, and the vehicle's
// constructor (0x8235AF90) sets that byte when its sixth argument is 0 (the same case registers the vehicle
// as the game's one "own" vehicle, the global at 0x8288DDF8): a traffic car casts no sun shadow and the
// ground under it is as lit as an open road. The materials do have the cascade technique (every xAmbient*
// car group has a VS_ShadowDepthLocal), so it is the draw's gate that leaves them out, not the data and not
// the host (nothing in the renderer filters the cascade draws). What traffic has is the game's soft quad
// decal (xAmbientOcclusionShadows PS_CalcShadowsQuad, one a car, a darkening of the colour after the world
// is drawn); the player's car has that and four wheel patches (PS_CalcShadowsSingle) as well as its cascade
// shadow.
//
// The contact march finds a car only within `thickness` (0.3 m) of its ray: a ground pixel seen through
// the gap under a car has the car's rear face in front of it by up to the car's length, so every ray that
// starts under the middle of the car ends at a surface "far in front" of it, which hides nothing by design
// (a wall in front of a ray only overlaps it on the screen). Only the strip along the edges and the wheels
// are within reach: the owner's patchy picture.
//
// For a vehicle that rule is wrong: the surface in front is a car, a solid, and a point behind its visible
// shell by less than the car's length is inside it. So a ground pixel looks UP the column above it (the
// world's up, in view space: UpInView) a little way (kUnderHeight): when the pixel a step falls on is a
// vehicle's (the depth pass's vehicle bit) and the step is behind that shell by more than a tolerance and
// less than kUnderReach, the ground is under the car. Under a car the sun is blocked, as by its own shadow,
// so the pixel gets the full contact shadow: the sun's share of the light goes, the ambient stays. That is
// what the cascade gives the player's car; the apply multiplies by the sun's share AFTER the game's mask,
// so where the game already shadows the ground (the player's car) the term adds nothing and that car's
// picture is unchanged. It does not add the decal's extra darkening: the ground under a traffic car is a
// shadow (ambient only, then the occlusion's own darkening), not a decal's black.
//
// What it does not do, by construction: ground in front of a car (the step is nearer than the shell:
// dz < 0), ground beside it (the column is outside the shell's pixels), ground beyond it (hidden by the
// car or above its roof line, the column goes away from it), a surface that is not roughly horizontal
// (kUnderUpLow), a vehicle's own pixel, anything that is not a vehicle (a table: the contact march's own
// job). Three steps up the column, jittered by the pixel's noise, heights as shares of kUnderHeight.
inline constexpr float kUnderHeight = 0.9f;       // m: how far up the column the steps reach
inline constexpr float kUnderStep0 = 1.0f;        // the heights of the three steps, shares of kUnderHeight
inline constexpr float kUnderStep1 = 0.6f;
inline constexpr float kUnderStep2 = 0.35f;
inline constexpr float kUnderJitterLow = 0.85f;   // the steps' heights are multiplied by 0.85 + 0.3 noise
inline constexpr float kUnderJitterSpan = 0.3f;
inline constexpr float kUnderReach = 6.0f;        // m: how far behind a vehicle's shell a step is still inside it
inline constexpr float kUnderEpsFloor = 0.05f;    // m: the shell must be nearer than the step by this ...
inline constexpr float kUnderEpsPixels = 3.0f;    // ... plus this many footprints (and is fully so at twice that)
inline constexpr float kUnderUpLow = 0.75f;       // the surface's normal against the world's up: none below this ...
inline constexpr float kUnderUpHigh = 0.9f;       // ... all from this
// A car the game now draws into the sun shadow map (TRAFFIC SHADOWS, traffic_shadow.h) has its shadow already: the
// term leaves it out. The hook's distance is to the car's origin (its middle) and the shell the step lands on is
// up to a car's half length nearer or farther, so the term fades in over twice this many metres around that distance.
inline constexpr float kUnderCastBand = 3.0f;

inline float UnderStep(int k) { return k == 0 ? kUnderStep0 : (k == 1 ? kUnderStep1 : kUnderStep2); }

// The world's up in this renderer's view space (x right, y up, z forward), from the game's view matrix
// `m` (row vectors, v' = v M, the camera looking down -z; the upper 3x3's row 1 is the world's Y in view
// space): the same arrangement as the sun's direction (ao.cpp FillPush). Zero when the matrix is not usable.
template <class Matrix>
inline Vec3 UpInView(const Matrix& m) {
  const double x = m[1][0], y = m[1][1], z = -m[1][2];
  const double length = std::sqrt(x * x + y * y + z * z);
  if (!std::isfinite(length) || length < 1e-6) {
    return {};
  }
  return {float(x / length), float(y / length), float(z / length)};
}

// The vehicle bit of a level-0 depth (the lowest bit of the float, ao_common.hlsli AoIsVehicle).
inline bool DepthIsVehicle(float level0_depth) { return (std::bit_cast<uint32_t>(level0_depth) & 1u) != 0u; }
inline float DepthWithVehicleBit(float depth, bool vehicle) {
  return std::bit_cast<float>((std::bit_cast<uint32_t>(depth) & ~1u) | (vehicle ? 1u : 0u));
}
inline float UnderTolerance(float footprint) { return kUnderEpsFloor + kUnderEpsPixels * footprint; }
// The weight of a step whose pixel is a vehicle's: dz is the step's distance minus the shell's (> 0: the
// shell is nearer to the camera). 0 up to the tolerance, 1 from twice it to kUnderReach, then fading out
// over a quarter of the reach.
inline float UnderHitWeight(float dz, float footprint) {
  const float eps = UnderTolerance(footprint);
  return Saturate((dz - eps) / eps) * (1.0f - Saturate((dz - kUnderReach) / (0.25f * kUnderReach)));
}
// How horizontal a surface is: its normal (facing the camera) against the world's up.
inline float UnderUpness(float normal_dot_up) { return Smoothstep(kUnderUpLow, kUnderUpHigh, normal_dot_up); }
// How much of the term a car whose shell is `shell_distance` metres from the camera keeps, when the game draws the
// cars within `cast_distance` of the camera into the sun shadow map (0: none does, the term is as it was).
inline float UnderCastWeight(float shell_distance, float cast_distance) {
  if (!(cast_distance > 0.0f)) {
    return 1.0f;
  }
  return Smoothstep(cast_distance - kUnderCastBand, cast_distance + kUnderCastBand, shell_distance);
}

// ---------------------------------------------------------------------------
// the adaptive step count (mcla_gpu_contact_shadows_fast)
// ---------------------------------------------------------------------------

// The cheaper contact shadows (the debug cvar mcla_gpu_contact_shadows_fast, on by default; off is the march and
// the passes as they were) have two parts: the march takes as many steps as the ray's length on the screen needs
// (this section), and the 3x3 filter is done inside the apply pass (hlsl/ao_apply_ps.hlsl, AoBilateralEdgeFolded).
//
// THE NUMBERS. The squared spacing puts the last two steps 2 L / n pixels apart for a ray of L pixels on the screen
// and n steps. kAdaptEndPixels is the gap the count aims at: 8 pixels is what 16 steps already have at the end of a
// 64 pixel ray, so a ray that the full march samples at a gap of 8 pixels or less is sampled at 8 here, and one it
// samples more coarsely keeps all its steps: the adaptive march is never coarser at the end than the full one is
// on a 64 pixel ray. kAdaptMinSteps is a floor for rays of a few pixels. Tuned on the twin (contact_trace_test.cpp
// AdaptiveStepsMatchTheFullMarch, which states the tolerance): over a kerb and walls, a bollard grid, a row of cars and
// a street, at 720p, 1080p and 2160p, with the sun at four heights and six directions (planes at every angle, which
// must never shadow themselves, are PlanesNeverShadowThemselves), 8 pixels changes the contact visibility by 0.0008 or
// less on average, by more than 0.2 in at most 0.12% of the marched pixels, and in at most 0.016% of them (one in six
// thousand, at 720p) without the full march having that value within 2 pixels (a shadow edge that only moved).
// 12 pixels gives about 2.5 times as many of those for 10%, 6% and 2% fewer steps again at 720p, 1080p and 2160p, 24
// pixels 15 to 20 times as many: 8 is the value that keeps the picture and saves what there is to save.
// WHAT IT SAVES. Only rays that are short on the screen: a ray of 0.6 to 1.8 m is 40 to 300 pixels long at 2160p
// anywhere in the 80 m the contact shadows reach, so at 3840x2160 the count stays at 16 for nearly all of them
// (the check's street: 15.84 of 16 steps on average), at 1080p it is 15.07 and at 720p 14.08 (all scenes: 15.76,
// 14.89, 13.90). That is not the saving the idea promised for the 4K picture: there the cheaper path's gain is the
// filter (below). The cost model takes the street's averages.
inline constexpr float kAdaptEndPixels = 8.0f;
inline constexpr uint32_t kAdaptMinSteps = 4;

// The length in depth image pixels of the segment from `a` to `b` (view space points): each end's pixel
// position is its x and y over z through the lens. Negative when an end is not in front of the near plane
// (the ray passes the camera: the caller marches every step).
inline float RayPixelLength(const Camera& c, Vec3 a, Vec3 b) {
  if (a.z <= kNearZ || b.z <= kNearZ) {
    return -1.0f;
  }
  const float ax = 0.5f * float(c.width) * a.x / (a.z * c.tan_x);
  const float ay = 0.5f * float(c.height) * a.y / (a.z * c.tan_y);
  const float bx = 0.5f * float(c.width) * b.x / (b.z * c.tan_x);
  const float by = 0.5f * float(c.height) * b.y / (b.z * c.tan_y);
  return std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
}
// The number of steps a ray of `length_pixels` on the screen needs: the squared spacing puts the last two steps
// 2 length / n pixels apart, and the count aims at that gap being `end_pixels` (a gap under a pixel only reads the
// same pixel again), never fewer than kAdaptMinSteps and never more than `max_steps` (the setting's: 8 or 16).
// `end_pixels` 0 (the lever is off) or a length that is not known (negative): all of them.
inline uint32_t AdaptiveSteps(float length_pixels, uint32_t max_steps, float end_pixels,
                              uint32_t min_steps = kAdaptMinSteps) {
  if (!(end_pixels > 0.0f) || !(length_pixels >= 0.0f)) {
    return max_steps;
  }
  const float want = std::ceil(2.0f * length_pixels / end_pixels);
  const uint32_t low = std::min(min_steps, max_steps);
  return want >= float(max_steps) ? max_steps : std::max(uint32_t(want), low);
}

struct Trace {
  Camera camera;
  Vec3 to_sun;            // unit, view space
  float length = 0.6f;    // m
  float thickness = 0.3f; // m
  float fade_start = kDefaultFadeStart;
  uint32_t steps = kStepsHigh;
  float adapt_pixels = 0.0f;   // the adaptive step count's end gap in pixels (AdaptiveSteps); 0: every step, as before
  uint32_t adapt_min_steps = kAdaptMinSteps;
  mutable uint32_t steps_used = 0;  // out: the number of steps the last March took (0: it returned before the loop)
  float noise = 0.5f;     // the pixel's start offset, 0 to 1
  float sky = 60000.0f;   // depth values at or above sky * 0.5 are the sky
  float max_distance = 80.0f;
  bool classic = false;   // the first version's march (equal steps, bias and ramp 2 cm + 0.4% of z, 1 - along^2)
  Vec3 up;                // the world's up in view space (UpInView); zero: no ground-under-a-vehicle term
  float under = 0.0f;     // how strongly the ground under a vehicle is shadowed, 0 (off) to 1
  float cast_distance = 0.0f;  // cars within this many metres of the camera cast into the sun shadow map: the term leaves them out (0: none)
  mutable int under_loads = 0;  // out: the depth reads of the under term, summed over the calls (for the check)
};

// The ground-under-a-vehicle term of the pixel (x, y), 0 to 1 (the shader's Under): `normal` is the pixel's
// view-space normal (facing the camera), `pc` its position, `tc` its depth (with the vehicle bit).
// depth_at(x, y) is the level-0 depth with the vehicle bit.
template <class DepthAt>
float UnderVehicle(const Trace& t, DepthAt&& depth_at, float tc, Vec3 pc, Vec3 normal) {
  if (!(t.under > 0.0f) || Dot(t.up, t.up) < 0.5f || DepthIsVehicle(tc)) {
    return 0.0f;
  }
  const float upness = UnderUpness(Dot(normal, t.up));
  if (!(upness > 0.0f)) {
    return 0.0f;
  }
  const Camera& cam = t.camera;
  const float footprint_unit = FootprintUnit(cam.tan_y, float(cam.height));
  const float jitter = kUnderJitterLow + kUnderJitterSpan * t.noise;
  float hit = 0.0f;
  for (int k = 0; k < 3; ++k) {
    const Vec3 p = pc + t.up * (kUnderHeight * UnderStep(k) * jitter);
    if (p.z <= kNearZ) {
      continue;
    }
    const float u = 0.5f + 0.5f * p.x / (p.z * cam.tan_x);
    const float v = 0.5f - 0.5f * p.y / (p.z * cam.tan_y);
    if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) {
      continue;
    }
    const int qx = std::min(int(u * float(cam.width)), cam.width - 1);
    const int qy = std::min(int(v * float(cam.height)), cam.height - 1);
    ++t.under_loads;
    const float td = depth_at(qx, qy);
    if (!DepthIsVehicle(td)) {
      continue;
    }
    float weight = UnderHitWeight(p.z - td, footprint_unit * p.z);
    if (t.cast_distance > 0.0f) {
      // The shell's distance from the camera along this pixel's ray (td is its depth, the ray's z).
      const float sx = p.x / p.z, sy = p.y / p.z;
      weight *= UnderCastWeight(td * std::sqrt(1.0f + sx * sx + sy * sy), t.cast_distance);
    }
    hit = std::max(hit, weight);
  }
  return hit * upness * Saturate(t.under);
}

// The contact visibility of the pixel (x, y), 1 open to 0 hidden: what one invocation of the shader
// writes (with `classic`, what the first version's did). depth_at(x, y) is the linear depth image.
template <class DepthAt>
float March(const Trace& t, DepthAt&& depth_at, int x, int y) {
  const Camera& cam = t.camera;
  t.steps_used = 0;
  const float tc = depth_at(x, y);
  if (tc >= t.sky * 0.5f || tc >= t.max_distance) {
    return 1.0f;
  }
  const Vec3 pc = ViewPos(cam, float(x) + 0.5f, float(y) + 0.5f, tc);
  Vec3 normal = NormalFromDepth(cam, depth_at, x, y, tc, pc);
  if (Dot(normal, pc) > 0.0f) {
    normal = normal * -1.0f;
  }
  const float facing = Dot(normal, t.to_sun);
  if (facing <= 0.0f || Dot(t.to_sun, t.to_sun) < 0.5f) {
    return 1.0f;
  }
  const float footprint_unit = FootprintUnit(cam.tan_y, float(cam.height));
  const float lift = t.classic ? 0.015f + 0.003f * tc : RayLift(footprint_unit * tc);
  const Vec3 start = pc + normal * lift;
  const float thickness = std::max(t.thickness, 1e-3f);
  // The ground under a vehicle starts the shadow (0 when the term is off or the pixel is not that).
  float shadow = t.classic ? 0.0f : UnderVehicle(t, depth_at, tc, pc, normal);
  // The steps: the setting's, or (the lever on) as many as the ray's length on the screen needs.
  uint32_t steps = t.steps;
  if (!t.classic && t.adapt_pixels > 0.0f) {
    steps = AdaptiveSteps(RayPixelLength(cam, start, start + t.to_sun * t.length), t.steps, t.adapt_pixels,
                          t.adapt_min_steps);
  }
  t.steps_used = steps;
  for (uint32_t i = 0; i < steps; ++i) {
    if (shadow >= 0.99f) {
      break;
    }
    const float along = t.classic ? (float(i) + t.noise) / float(steps) : Along(i, t.noise, steps);
    const Vec3 p = start + t.to_sun * (along * t.length);
    if (p.z <= kNearZ) {
      break;
    }
    const float u = 0.5f + 0.5f * p.x / (p.z * cam.tan_x);
    const float v = 0.5f - 0.5f * p.y / (p.z * cam.tan_y);
    if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) {
      break;
    }
    const int qx = std::min(int(u * float(cam.width)), cam.width - 1);
    const int qy = std::min(int(v * float(cam.height)), cam.height - 1);
    const float dz = p.z - depth_at(qx, qy);
    float hit;
    if (t.classic) {
      const float bias = ClassicBias(p.z);
      hit = Saturate((dz - bias) / bias);
    } else {
      const float footprint = footprint_unit * p.z;
      hit = Saturate((dz - HitEpsilon(footprint)) /
                     HitRamp(footprint, StepLength(i, t.noise, steps, t.length), t.to_sun.z));
    }
    hit *= 1.0f - Saturate((dz - thickness) / thickness);
    hit *= t.classic ? ClassicEndFade(along) : EndFade(along, t.fade_start);
    shadow = std::max(shadow, hit);
    if (shadow >= 0.99f) {
      break;
    }
  }
  if (!t.classic) {
    shadow *= Smoothstep(kSunFacingLow, kSunFacingHigh, facing);
  }
  return 1.0f - shadow;
}

}  // namespace mcla::gpu::contact_trace

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
// "images 3840x2160 for a 3840x2160 picture"), and at the ultra setting they run at the render size at
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
// Used by ao.cpp: LengthGrow (the ray's length for a low sun), the default fade, CostModel and CostModelFast (the log lines),
// MaskSkipOn and kSkipFlag (the march's skip under the game's own shadow), UnderCastDistance (the player's car and the cars the
// game's sun shadow map holds are left out of the ground-under-a-vehicle term); the rest is mirrored in the shader and run by the check.
// ===========================================================================

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

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
// The steps at the high and the ultra setting.
inline constexpr uint32_t kStepsHigh = 8;
inline constexpr uint32_t kStepsUltra = 16;
// A pixel of a vehicle (the depth pass's vehicle bit) marches a short, thin ray: see "a car's own pixels" below.
inline constexpr float kVehicleReceiverLength = 0.30f;     // m: the ray's length, whatever the sun's height (no growth with a low sun)
inline constexpr float kVehicleReceiverThickness = 0.06f;  // m: the thickness (the tail that fades out is as long again: hidden up to 0.12 m)
inline constexpr uint32_t kVehicleReceiverSteps = 12;      // at most this many steps (the squared spacing is kept)

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
inline constexpr double kMsPerMegaStep = kMeasuredMarchMs / (kMeasuredMegapixels * double(kStepsUltra));
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
  double block_ms = 0.0;          // CostModelFast: the skip's block pass (0 without the skip)
  double total_ms = 0.0;          // an RTX 5080's; other GPUs scale with their throughput
  double skipped_share = 0.0;     // CostModelFast: the share of the marched pixels the skip under the game's shadow avoids
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

// The cheaper contact shadows (mcla_gpu_contact_shadows_fast). Measured once in the game (PLAY4, 2026-11-14, 3840x2160, K-DAY 12 h,
// a view about half in the game's shade): plain 'ao contact' 0.285 ms + 'ao contact denoise' 0.107 + 'ao apply' 0.115; the first
// cheaper path 0.306 + none + 0.128: the filter's fold is a gain, the march a loss. The model below is of the second version
// (this one): the march is bound by its arithmetic, so its cost is the DXIL instructions a pixel executes, counted by block from
// dxc's listing of hlsl/ao_contact_ps.hlsl (the paths below; recount when the shader changes), scaled to the plain march's own cost
// (CostModel: the measured 0.232 ms x kMarchAluFactor = 0.278 ms at 4K; PLAY4's 0.285). NOT MEASURED for this version.
//   A pixel by what it does, with the instructions it executes (the street at 2160p, contact_trace_test.cpp StreetMix, has the shares):
//     the sky or past the contact fade (kMixFar)                 31
//     turned from the sun (kMixBack): the depth, 4 neighbours, the normal, the facing test        161
//     a march (kMixMarch): 339 before the steps (the normal, the lift, the ground under a car on a horizontal surface: two probes of the
//       vertical column; 369 with the three of the first version, +144 for a pixel whose first column found a car), 79 a step; a vehicle's
//       own pixel (no under term, at most kVehicleReceiverSteps steps) 222 before them: 1187 in all against 1490
//   The cheaper path adds, to a pixel that is not sky: the skip test, 17 instructions (the block the pixel's centre is in is 1:
//   marched) and, where its blocks are all 0, ends the pixel at 106 in all (the block of its centre, one gather, the return); the
//   adaptive count 38 instructions where it is on (heights up to kAdaptMaxHeight) against the steps it saves. The first version
//   had the skip test at 92 (lit) and, in shade at 4K where the mask is the picture's size, 356: the loss measured.
//   The filter in the apply: PLAY4's apply rose by 0.013 ms against the 0.107 ms of the filter pass that is gone.
inline constexpr double kPlay4FilterPassMs = 0.107;
inline constexpr double kPlay4FoldedFilterMs = 0.013;
inline constexpr double kFoldedFilterShare = kPlay4FoldedFilterMs / kPlay4FilterPassMs;
inline constexpr double kInstrFar = 31.0;
inline constexpr double kInstrBack = 161.0;
inline constexpr double kInstrMarchFixed = 339.0;
inline constexpr double kInstrStep = 79.0;
inline constexpr double kInstrAdaptive = 38.0;
inline constexpr double kInstrTestLit = 17.0;
inline constexpr double kInstrSkipped = 106.0;
// What the first version executed, for the log's and the check's comparison.
inline constexpr double kInstrTestLitFirst = 92.0;
inline constexpr double kInstrSkippedFirst = 356.0;   // 25 texel reads (a mask the picture's size); 212 for 9 (a mask a third of it)
// The share of all pixels that are far (sky, past 80 m), turned from the sun, and march, in the check's street at 2160p (contact_trace_test.cpp StreetMix).
inline constexpr double kMixFar = 0.27;
inline constexpr double kMixBack = 0.15;
inline constexpr double kMixMarch = 0.58;
// The block pass: a draw over a sixteenth of the mask's pixels (4 gathers each) and its fixed cost; at 4K.
inline constexpr double kBlockPassMs4k = 0.006;
// The share of the setting's steps the adaptive count keeps on average, by the picture's height in pixels: the twin's
// street scene at 720p, 1080p and 2160p (the check compares them), between them linearly, flat outside.
inline constexpr double kAdaptShare720 = 0.88;
inline constexpr double kAdaptShare1080 = 0.94;
inline constexpr double kAdaptShare2160 = 0.99;
// The adaptive count's arithmetic (38 instructions) against a step (79): it pays where it saves more than half a step, which is
// at 1080 lines (about 1 of 16) and below, not at 1440 (about 0.5) or 2160 (0.16). The host leaves it off above this height.
inline constexpr uint32_t kAdaptMaxHeight = 1080;
inline bool AdaptiveOnAt(uint32_t height) { return height <= kAdaptMaxHeight; }
inline double AdaptiveStepShare(uint32_t height) {
  if (!AdaptiveOnAt(height)) {
    return 1.0;  // not used: every step
  }
  const double h = double(height);
  if (h <= 720.0) {
    return kAdaptShare720;
  }
  return kAdaptShare720 + (kAdaptShare1080 - kAdaptShare720) * (h - 720.0) / 360.0;
}
// THE SKIP UNDER THE GAME'S OWN SHADOW (the section "the skip under the game's own shadow" below). NOT MEASURED either.
//   The skip avoids the pixels in the game's shadow minus the band along each shadow edge (the dilation by the filter's pixel, the
//   mask's bilinear texel, the margin and the block pass's blocks) and minus the warps that mix lit and shadowed pixels: kSkipCatch is
//   the share of the shadowed pixels the check's scenes see skipped, whole 8 x 4 pixel tiles only (contact_trace_test.cpp
//   SkipUnderTheGamesShadow prints and compares it), by the picture's height as the step share is. The share of the screen that is
//   in the game's shadow is the model's input: a frame without the mask (6 h, 18 h, rain, night) skips nothing and costs the march as
//   before (shadowed_share < 0).
inline constexpr double kSkipCatch720 = 0.85;
inline constexpr double kSkipCatch1080 = 0.85;
inline constexpr double kSkipCatch2160 = 0.85;
inline double SkipCatch(uint32_t height) {
  const double h = double(height);
  if (h <= 720.0) {
    return kSkipCatch720;
  }
  if (h <= 1080.0) {
    return kSkipCatch720 + (kSkipCatch1080 - kSkipCatch720) * (h - 720.0) / 360.0;
  }
  if (h <= 2160.0) {
    return kSkipCatch1080 + (kSkipCatch2160 - kSkipCatch1080) * (h - 1080.0) / 1080.0;
  }
  return kSkipCatch2160;
}
// The instructions a pixel of the street executes on average: with the cheaper path (`fast`: the adaptive count where it is on, and
// the skip test and the skip with a `shadowed_share` of 0 to 1 of the pixels that are not sky skipped) or without. A plain march at
// `steps` steps is the base the cost is scaled by.
inline double AverageInstructions(uint32_t height, uint32_t steps, bool fast, double skipped_share, bool skip_on) {
  const bool adaptive = fast && AdaptiveOnAt(height);
  const double march = kInstrMarchFixed + double(steps) * (adaptive ? AdaptiveStepShare(height) : 1.0) * kInstrStep +
                       (adaptive ? kInstrAdaptive : 0.0);
  const double test = skip_on ? kInstrTestLit : 0.0;
  const double skipped = skip_on ? std::clamp(skipped_share, 0.0, 1.0) : 0.0;
  return kMixFar * kInstrFar + (kMixBack + kMixMarch) * skipped * kInstrSkipped +
         (1.0 - skipped) * (kMixBack * (kInstrBack + test) + kMixMarch * (march + test));
}
// `folded`: the filter is inside the apply (false at half size, where the filter pass stays: one pass, and the skip is
// not used). `shadowed_share`: the share of the pixels that are not sky or past the fade that are in the game's own sun shadow (0 to 1) in a frame
// that has the mask; negative: no mask in the frame, or the skip is off (the march as it was).
inline Cost CostModelFast(uint32_t width, uint32_t height, uint32_t steps, bool folded, double shadowed_share = -1.0) {
  Cost c;
  const bool skipping = folded && shadowed_share >= 0.0;
  c.skipped_share = skipping ? std::clamp(shadowed_share, 0.0, 1.0) * SkipCatch(height) : 0.0;
  c.megapixels = double(width) * double(height) / 1e6;
  const double plain = AverageInstructions(height, steps, false, 0.0, false);
  const double fast = AverageInstructions(height, steps, true, c.skipped_share, skipping);
  const double share = AdaptiveStepShare(height);
  c.mega_steps = c.megapixels * double(steps) * share * (1.0 - c.skipped_share);
  c.filter_passes = (folded ? kFoldedFilterShare : 1.0);
  const double plain_march_ms = c.megapixels * double(steps) * kMsPerMegaStep * kMarchAluFactor;
  c.march_ms = plain_march_ms * fast / plain;
  c.block_ms = skipping ? kBlockPassMs4k * c.megapixels / kMeasuredMegapixels : 0.0;
  c.filter_ms = c.megapixels * kMsPerMegapixelPass * c.filter_passes;
  c.total_ms = c.march_ms + c.block_ms + c.filter_ms;
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
// a car's own pixels (the thin parts of a car: a wing, its posts, a mirror)
// ---------------------------------------------------------------------------
//
// Owner, 2026-11-18, the player's Skyline with its tall rear wing, the sun at 17.7 h (18 degrees up) and the contact shadows on:
//   1. the sun behind the camera, the chase camera behind the car: a broad black band across the trunk lid between the uprights, sharp at
//      the wing's lower edge and soft below. The game's shadows leave that area lit, and with the sun low behind the camera the wing's
//      shadow falls forward onto the rear window, not on the lid. Not the under-a-car term (it stays with it off) and not the thickness
//      (it stays at 0.1): the march.
//   2. the camera at the side of the car (the pause menu's): dashed streaks along the wing's edge, dark blobs at the bases of the uprights;
//      right from some angles and completely broken from others.
// The mechanism (contact_trace_test.cpp WingBandOnTheTrunk, WingFromTheSide, a rig with a pitched camera and the exact truth). A step is
// hidden when the surface at its pixel is nearer to the camera than the step by more than the tolerance and less than the thickness, which
// takes that surface for a slab extruded behind it ALONG THE VIEW RAY. That is right for a kerb or a bollard (0.3 m deep) and wrong for a
// part that is much thinner than the thickness: the 4 cm plate of a wing is seen from above at 7 to 10 degrees, where 0.3 m along the view
// ray is 4 cm of height, so a step just below the plate's level and just beyond its far edge (the ray from the trunk climbs on the screen
// onto the plate's pixels at 0.7 to 1.1 m along its 1.3 m) has the plate in front of it by 0.02 to 0.5 m, "inside" it; a 6 cm post that stands
// 0.1 to 0.3 m nearer to the camera than the ray, seen from the side, hides the ray's steps the same way. Seen from the other side of the
// car (a ray that runs away from the camera, or the camera above the plate) the same plate hides nothing, which is why it looked right from some
// angles. The thickness cannot be made smaller for everything (a tyre or a bollard is 0.3 m), and a depth image has no back faces to give a
// part's real thickness.
// WHAT CHANGES: the pixels that belong to a car (the vehicle bit of their own depth) march a short, thin ray: kVehicleReceiverLength
// (0.3 m, not grown by a low sun), kVehicleReceiverThickness (6 cm, hidden up to 12 cm) and at most kVehicleReceiverSteps steps. A car's
// own surfaces are where thin parts overlap their receivers (a wing and its posts on the trunk, a mirror on the door, a spoiler on the
// deck), the contact shadows that matter there are the small ones (a mirror on its door is 14 cm proud), and for the player's car the game's
// own sun shadow map holds the large ones, with 1 to 25 cm texels, so nothing is lost there that the game does not draw; a traffic car's
// big shadows were never right from a depth image. A car's shadow ON THE ROAD (the sills, the tyres, the foot of a car on a kerb) is a
// vehicle occluder on a receiver that is not a car's pixel: those pixels march exactly as they did (the check compares them to the bit).
// Measured on the twin at 720p (the check prints them), the sun at 17.7 degrees, the thickness 0.3 and the 1.3 m ray as in the game:
//   - the trunk lid under the wing from behind, three cameras: 797, 1030 and 1716 pixels darkened where nothing blocks the sun, now 0;
//   - the wing from the side, 8 sun directions, two distances: 63213 and 16434 pixels, now about 5000 and 1000 (what is left sits at the posts'
//     bases and the lid's edges, a few pixels wide, where there are small shadows too);
//   - the mirror on a door, 4 suns: the shadow within 15 cm of the mirror keeps 102% of its dark pixels (14361 against 14112);
//   - a car's tyres and sills on the road, 8 suns: every road pixel is as it was, and 98% of the pixels shadowed within 15 cm are dark.
// COST. A vehicle's pixel is a fixed share of the picture (5 to 15% in a chase view) and gets 12 steps instead of 16: 4 x 79 DXIL instructions
// fewer on 1490 (-20%); the others pay the vehicle test and two minimums (about 9 instructions). The adaptive step count sees the short ray too.

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
// job). Two steps up the column, jittered by the pixel's noise, heights as shares of kUnderHeight.
//
// THE FALSE SLAB (owner, 2026-11-18). The photo of an Audi R8 (the camera in front of the car on the left and low, the sun behind the car) and
// the player's R34 in the same set-up: a dark slab rises from the sunlit road right behind the rear tyre up to the bumper. It is gone with
// contact shadows off and with only this term off (mcla_gpu_contact_shadows_vehicles 0); the march does not make it (nor does the thickness).
// The mechanism (contact_trace_test.cpp GroundBeyondACar): a road pixel that the camera sees PAST the car's end, or through the gap under
// it from a low camera, is behind the car's shell as much as a pixel under the car is, and the column above it falls on car pixels all the
// same; so does the ground beyond the car seen from above its roof. A depth image has no underside, so the column alone cannot tell
// "under the car" from "beyond the car"; the sun can, because the ground is shadowed by the car only where the sun's ray from it is blocked,
// and a ray that runs away from the car (the sun behind the car, the road behind it) is not. Two changes:
//   1. a second column, along the sun's ray (the same heights up it: a point h above the ground is h over the sun's height, at least
//      kUnderSunSinMin, along it): the ground is under the car when BOTH columns find the car, and the second is read only where the first
//      found one. On the twin's scenes, against the exact truth (the sun's ray meets a car within 12 m): from 4 m above and behind the car, the sun
//      at 30 and 18 degrees, the ground called under the car with the sun free was 9199 and 11719 pixels, now 0; in the front-left low set-up
//      with the sun at 18 degrees, 30 and 60 degrees off the car's axis, 5204 and 8212, now 82 and 3, while 76% and 60% of the ground that
//      the column alone put under the car stays there. The price is the strip along the sun's edge of the car where a low sun really does
//      reach under it (0.25 m of clearance over the tangent of the sun's height: 14 cm at 60 degrees, 77 cm at 18): 93% of the ground seen under
//      the check's car at 60 degrees, 97% before, and a ray that has the car's body in front of it by less than the sun's step is not seen.
//   2. the PLAYER's car is always in the game's sun shadow map (traffic_shadow.h: it is the one vehicle the guest draws into the cascades),
//      with or without TRAFFIC SHADOWS: its ground is shadowed or lit by the game's own mask and the term has nothing to add and a slab to
//      take away. The host passes at least kPlayerCarCastDistance (UnderCastDistance), which UnderCastWeight turns into "nothing" for a
//      shell nearer than 9 m (the chase camera, the pause menu and the photo mode are 4 to 9 m from the car) and "all" from 15 m. It also
//      leaves out traffic that near, when TRAFFIC SHADOWS is off: the owner's earlier want, the dark ground under traffic, starts at 9 to 15 m.
//   What is left, by construction: with the sun straight behind a car, the strip of road past the car's far end that a low camera sees
//   through the gap (the check: 455 pixels beside 1282 under the car, for a car 20 m off). A depth image cannot give it up; for the player's car
//   (and TRAFFIC SHADOWS' cars) it is gone, for a far traffic car it is a few pixels wide.
inline constexpr float kUnderHeight = 0.9f;       // m: how far up the column the steps reach
inline constexpr float kUnderStepHigh = 1.0f;     // the heights of the two steps of a column, shares of kUnderHeight
inline constexpr float kUnderStepLow = 0.4f;
inline constexpr float kUnderJitterLow = 0.85f;   // the steps' heights are multiplied by 0.85 + 0.3 noise
inline constexpr float kUnderJitterSpan = 0.3f;
inline constexpr float kUnderReach = 6.0f;        // m: how far behind a vehicle's shell a step is still inside it
inline constexpr float kUnderEpsFloor = 0.05f;    // m: the shell must be nearer than the step by this ...
inline constexpr float kUnderEpsPixels = 3.0f;    // ... plus this many footprints (and is fully so at twice that)
inline constexpr float kUnderUpLow = 0.75f;       // the surface's normal against the world's up: none below this ...
inline constexpr float kUnderUpHigh = 0.9f;       // ... all from this
// The second column runs along the sun's ray: the sun's height (its sine) is kept at least this, so that a very low sun
// does not send the steps metres away (the steps' heights are then lower than the shares say).
inline constexpr float kUnderSunSinMin = 0.25f;
// A car the game now draws into the sun shadow map (TRAFFIC SHADOWS, traffic_shadow.h) has its shadow already: the
// term leaves it out. The hook's distance is to the car's origin (its middle) and the shell the step lands on is
// up to a car's half length nearer or farther, so the term fades in over twice this many metres around that distance.
inline constexpr float kUnderCastBand = 3.0f;
// The player's car is always in the game's sun shadow map, with or without TRAFFIC SHADOWS (traffic_shadow.h: it is the one
// vehicle the guest draws into the cascades), so the term is always left out for a car this near; the host passes
// UnderCastDistance(the traffic setting's distance). The camera is 4 to 9 m from the player's car in the chase view, the
// pause menu and the photo mode, and its shell is within 11 m of it; the fade is kUnderCastBand either side.
inline constexpr float kPlayerCarCastDistance = 12.0f;
inline float UnderCastDistance(float traffic_cast_distance) {
  return std::max(traffic_cast_distance, kPlayerCarCastDistance);
}

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
  uint32_t steps = kStepsUltra;
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
  bool vehicle_rule = true;    // a vehicle's own pixels march a short, thin ray (kVehicleReceiver*); false: the march as it was (the check's control)
  bool sun_column = true;      // the ground under a vehicle also needs the column along the sun's ray; false: the vertical column alone, as it was
  mutable int under_loads = 0;  // out: the depth reads of the under term, summed over the calls (for the check)
};

// One probe of the term: the weight of the point `p` (view space): 0 when it is behind the lens or off the picture or
// its pixel is not a vehicle's, else UnderHitWeight of how far behind that shell it is (times the cast distance's weight).
template <class DepthAt>
float UnderProbe(const Trace& t, DepthAt&& depth_at, Vec3 p) {
  const Camera& cam = t.camera;
  if (p.z <= kNearZ) {
    return 0.0f;
  }
  const float u = 0.5f + 0.5f * p.x / (p.z * cam.tan_x);
  const float v = 0.5f - 0.5f * p.y / (p.z * cam.tan_y);
  if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) {
    return 0.0f;
  }
  const int qx = std::min(int(u * float(cam.width)), cam.width - 1);
  const int qy = std::min(int(v * float(cam.height)), cam.height - 1);
  ++t.under_loads;
  const float td = depth_at(qx, qy);
  if (!DepthIsVehicle(td)) {
    return 0.0f;
  }
  float weight = UnderHitWeight(p.z - td, FootprintUnit(cam.tan_y, float(cam.height)) * p.z);
  if (t.cast_distance > 0.0f) {
    // The shell's distance from the camera along this pixel's ray (td is its depth, the ray's z).
    const float sx = p.x / p.z, sy = p.y / p.z;
    weight *= UnderCastWeight(td * std::sqrt(1.0f + sx * sx + sy * sy), t.cast_distance);
  }
  return weight;
}

// The ground-under-a-vehicle term of the pixel (x, y), 0 to 1 (the shader's Under): `normal` is the pixel's
// view-space normal (facing the camera), `pc` its position, `tc` its depth (with the vehicle bit).
// depth_at(x, y) is the level-0 depth with the vehicle bit. Two columns of two probes: straight up, and along the sun's ray
// (the same heights up it); the ground is under the car when both find it (the weaker one counts), and the second column
// is only read where the first found something (most of the picture's pixels read two pixels, the others four).
template <class DepthAt>
float UnderVehicle(const Trace& t, DepthAt&& depth_at, float tc, Vec3 pc, Vec3 normal) {
  if (!(t.under > 0.0f) || Dot(t.up, t.up) < 0.5f || DepthIsVehicle(tc)) {
    return 0.0f;
  }
  const float upness = UnderUpness(Dot(normal, t.up));
  if (!(upness > 0.0f)) {
    return 0.0f;
  }
  const float jitter = kUnderJitterLow + kUnderJitterSpan * t.noise;
  const float high = kUnderHeight * kUnderStepHigh * jitter, low = kUnderHeight * kUnderStepLow * jitter;
  const float vertical = std::max(UnderProbe(t, depth_at, pc + t.up * high), UnderProbe(t, depth_at, pc + t.up * low));
  float hit = vertical;
  if (t.sun_column && vertical > 0.0f) {
    // Up the sun's ray: a point that is `h` above the ground is h / (the sun's height) along it. A sun at or below the horizon
    // reaches no ground under a car (and the contact shadows are gated off then): none.
    const float sun_up = Dot(t.to_sun, t.up);
    if (!(sun_up > 0.0f)) {
      return 0.0f;
    }
    const float along = 1.0f / std::max(sun_up, kUnderSunSinMin);
    const float sun = std::max(UnderProbe(t, depth_at, pc + t.to_sun * (high * along)),
                               UnderProbe(t, depth_at, pc + t.to_sun * (low * along)));
    hit = std::min(vertical, sun);
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
  float thickness = std::max(t.thickness, 1e-3f);
  float length = t.length;
  uint32_t steps = t.steps;
  if (!t.classic && t.vehicle_rule && DepthIsVehicle(tc)) {
    // A car's own pixel (the wing's trunk lid, a door, a roof): a short, thin ray (kVehicleReceiver*).
    length = std::min(length, kVehicleReceiverLength);
    thickness = std::min(thickness, kVehicleReceiverThickness);
    steps = std::min(steps, kVehicleReceiverSteps);
  }
  // The ground under a vehicle starts the shadow (0 when the term is off or the pixel is not that).
  float shadow = t.classic ? 0.0f : UnderVehicle(t, depth_at, tc, pc, normal);
  // The steps: the setting's, or (the lever on) as many as the ray's length on the screen needs.
  if (!t.classic && t.adapt_pixels > 0.0f) {
    steps = AdaptiveSteps(RayPixelLength(cam, start, start + t.to_sun * length), steps, t.adapt_pixels,
                          t.adapt_min_steps);
  }
  t.steps_used = steps;
  for (uint32_t i = 0; i < steps; ++i) {
    if (shadow >= 0.99f) {
      break;
    }
    const float along = t.classic ? (float(i) + t.noise) / float(steps) : Along(i, t.noise, steps);
    const Vec3 p = start + t.to_sun * (along * length);
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
                     HitRamp(footprint, StepLength(i, t.noise, steps, length), t.to_sun.z));
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

// ---------------------------------------------------------------------------
// the skip under the game's own shadow (mcla_gpu_contact_shadows_fast, the third part of the cheaper path)
// ---------------------------------------------------------------------------
//
// WHAT IS SKIPPED. The apply pass darkens by 1 - weight * (1 - cs) with
//     weight = strength * sun_share * fade,   sun_share = Ls N.L v / (La + Ls N.L v),
//   v = saturate(gain * mask) the game's own sun shadow mask (the mask's stored value times the gain; 1 where the frame has
//   no mask), and, with the filter inside the apply (hlsl/ao_apply_ps.hlsl: the picture's size, the same place the march's
//   image is read), cs is the 3 x 3 filter over the march's image and the filter is left out wherever weight <=
//   kFoldedFilterSkip (cs stays 1: the factor is exactly 1). So where the game's mask is 0 the march's result is not used,
//   and the march (the only expensive part of the contact shadows) may return 1 at once.
//
// WHY IT IS EXACT (the check, SkipUnderTheGamesShadow, runs the twin of both passes and compares the factor bit for bit).
//   1. The factor of a pixel P is a function of the march's value at P and at its 8 neighbours Q, and only when
//      weight(P) > kFoldedFilterSkip: below it the apply never reads them (cs = 1, factor = 1 - weight * 0 = 1 exactly).
//   2. weight(P) is at most strength * Ls v / (La + Ls v), which grows with v (N.L <= 1, fade <= 1), and v is at most
//      saturate(gain * the largest texel of the 4 bilinear taps the apply samples at P's centre). SkipLimit turns the bound
//      weight <= kSkipWeight into the largest stored texel value `limit` for which it still holds; kSkipWeight is half of
//      kFoldedFilterSkip, so rounding (the apply's normal and the light are not exactly what the bound assumed) cannot
//      reach the apply's own threshold.
//   3. A pixel Q is skipped when every texel that any pixel of the 3 x 3 round Q (the pixels whose filter reads Q, and Q
//      itself) can sample is <= limit. Then each of those pixels has weight <= kSkipWeight, so none of them runs the filter
//      or reads Q: the skipped value (1) is read by nobody that could notice, and every pixel that does run the filter has
//      all of its neighbours marched, so its value is the march's own. The factor is the same as without the skip, to the bit.
//   Not skipping is always safe, so every doubt is resolved towards marching: the texel range is the bilinear footprint of
//   the 3 x 3 pixels' centres widened by kSkipTexelMargin of a texel (the sampler's fixed-point coordinates: 1/256), clamped
//   into the mask like the sampler's clamp-to-edge; a mask more than kSkipMaxScale times the picture's size is not tested
//   (SkipScaleOk); an ambient that is negative, a sun or gain that is not positive, NaN: no skip (SkipLimit is -1). The skip is
//   on only where the apply does the filter itself (the picture's size, not at half size, where the apply upsamples a
//   filtered image and reads further), only in a frame that has the mask (the shader reads the mask's slot), and not while a
//   debug picture is on (those run the filter on every pixel and show the visibility itself).
//   A test that skips FEWER pixels than the range says is exact too (marching a pixel is what the plain path does): that is
//   what lets the test below be cheap.
//
// WHAT IT COST, AND WHAT IT COSTS NOW. The first version measured in the game (PLAY4, 3840x2160, K-DAY 12 h, a view that is
// about half in the game's shade): the march 0.306 ms against 0.285 plain, slower in spite of skipping. The march is bound by
// its arithmetic, not by its depth reads (1594 DXIL instructions for a pixel that marches 16 steps, 161 for one that
// faces away from the sun, 31 for the sky; a 5080 issues about 28 tera of them a second, which is 0.28 ms for the 8.3 million
// pixels of 4K at the rate of a street, while its 24 depth reads a pixel are about a seventh of that), and the first version's
// test made the limit, the sizes' ratio and the range in 92 instructions for EVERY pixel of the picture that is not
// sky, then read the texels one by one: with the adaptive count's 38 the fast march had 130 instructions more than the
// plain one (8%) on a pixel in the sun, and a pixel in shade, which the plain march ends at 161 where it faces away from the
// sun, cost 356 there (25 texel reads at 4K, where the mask is the picture's size). With no pixel skipped at all that is +9% in the
// model, PLAY4's +7.4%: the measurement fits a skip that fired on little (the game's shade stored above 0?) as well as a bad test. Now:
//   (1) the host makes the limit, the scales and the reciprocals (push fields: no GetDimensions, no division in the shader);
//   (2) a BLOCK PASS (the same shader, flag kSideFlag; a small draw) makes, once a frame, an image of the mask's
//       size over kSkipBlock (4): a pixel is 1 where any of the 4 x 4 texels at it is above the limit, 0 where all are at
//       most the limit: the range of a pixel's footprint (at most 5 texels a side where the mask is the picture's size, 3 at a
//       third of that) touches 1 or 2 blocks a side, so;
//   (3) the march's test is ONE load of the block of the pixel's own centre (1 : the pixel is marched, 17 instructions: the
//       whole cost of a pixel in the sun) and, for a pixel in the shade, one gather of the 2 x 2 blocks that hold the range
//       (a shaded pixel ends at 106 instructions in all); the pixels within a few texels of the sun's edge are no longer skipped (the
//       check counts how many: SkipUnderTheGamesShadow prints the share of the shade that the exact range and this test skip).
//   The adaptive count is off above kAdaptMaxHeight lines, where its 38 instructions cost more than the steps it saves.
// The model is below (CostModelFast); the check recounts the shares it uses from the twin.
//
// Push fields (hlsl/ao_contact_ps.hlsl): flags bit 24 (kSkipFlag) on the march, bit 27 (kSideFlag) on the block pass; the
// march: slots2.w the block image's slot, sizes.xy the scale of the mask over the picture over kSkipBlock, extra.x and extra.z one
// over the block image's width and height; the block pass: slots2.w the mask's slot, shape.w the limit, extra.x and extra.z one over
// the mask's width and height.
inline constexpr float kSkipWeight = 0.5f * kFoldedFilterSkip;
inline constexpr float kSkipTexelMargin = 0.015625f;  // 1/64 of a mask texel
inline constexpr float kSkipMaxTexels = 8.0f;
inline constexpr float kSkipMaxScale = 2.4f;          // the mask's size over the picture's: up to here the range is at most 7 texels a side
inline constexpr float kSkipBlock = 4.0f;             // the block pass's block: texels a side
inline constexpr uint32_t kSkipFlag = 1u << 24;
inline constexpr uint32_t kStatFlag = 1u << 26;
inline constexpr uint32_t kSideFlag = 1u << 27;
// What a pixel writes in the statistics picture (flag bit 26; hlsl/ao_contact_ps.hlsl SkipStat): -1 skipped, 2 the sky or past the contact
// fade's end, v (0 to 1) a pixel that marches, 3 + v one that faces away from the sun (or has no sun), v the mask's stored value at its centre.
inline constexpr float kStatSkipped = -1.0f;
inline constexpr float kStatFar = 2.0f;
inline constexpr float kStatMarch = 0.0f;
inline constexpr float kStatBack = 3.0f;

// The largest stored mask value (0 to 1) at which the pixel's weight is surely <= kSkipWeight; a value above it forbids the
// skip. -1: never skip (the inputs are not the ones the bound is proven for).
inline float SkipLimit(float ambient, float sun, float strength, float gain) {
  if (!(ambient >= 0.0f) || !(sun > 0.0f) || !(gain > 0.0f) || !(strength > kSkipWeight)) {
    return -1.0f;
  }
  // strength * sun v / (ambient + sun v) <= kSkipWeight  <=>  sun v <= kSkipWeight * ambient / (strength - kSkipWeight)
  const float visible = (kSkipWeight * ambient) / ((strength - kSkipWeight) * sun);
  return visible >= 1.0f ? 3.0e38f : visible / gain;
}

// The mask is not larger than kSkipMaxScale times the picture (either way): the range of texels one pixel's footprint reaches is
// then 7 texels a side at most. Else the skip is not used.
inline bool SkipScaleOk(int mask_w, int mask_h, int image_w, int image_h) {
  return mask_w > 0 && mask_h > 0 && image_w > 0 && image_h > 0 && float(mask_w) / float(image_w) <= kSkipMaxScale &&
         float(mask_h) / float(image_h) <= kSkipMaxScale;
}

// Whether the host turns the skip on in a frame (ao.cpp RecordContact). `limit` is SkipLimit of the frame's numbers (-1: not
// provable), `scale_ok` SkipScaleOk of the mask's and the picture's sizes.
inline bool MaskSkipOn(bool fast, bool filter_in_apply, bool has_mask, bool debug_picture, float limit, bool scale_ok) {
  return fast && filter_in_apply && has_mask && !debug_picture && limit >= 0.0f && scale_ok;
}

// The mask texels (first and last, one axis) that the pixels `pixel - 1` to `pixel + 1` can sample: the bilinear taps of
// a sample at the pixel centre (p + 0.5) / image_size are the texels floor(t) and floor(t) + 1, t = (p + 0.5) mask / image - 0.5.
// The reference range: the skip tests at least these texels.
inline void SkipTexelRange(int pixel, int image_size, int mask_size, int* first, int* last) {
  const float scale = float(mask_size) / float(image_size);
  const float lo = (float(pixel) - 0.5f) * scale - 0.5f - kSkipTexelMargin;
  const float hi = (float(pixel) + 1.5f) * scale - 0.5f + kSkipTexelMargin;
  *first = std::clamp(int(std::floor(lo)), 0, mask_size - 1);
  *last = std::clamp(int(std::floor(hi)) + 1, 0, mask_size - 1);
}

// True when the march at (x, y) may return 1 without looking: every texel its 3 x 3 of pixels can sample is <= limit (the
// reference: exactly the range, texel by texel). mask_at(tx, ty) is the stored value, 0 to 1. `reach` is how many pixels round
// (x, y) are counted (1: the filter's 3 x 3; 0 is the pixel alone, which the check uses as the control that shows the dilation
// is needed).
template <class MaskAt>
bool MaskCannotLight(MaskAt&& mask_at, int mask_w, int mask_h, int image_w, int image_h, int x, int y, float limit, int reach = 1) {
  if (!(limit >= 0.0f)) {
    return false;
  }
  int x0, x1, y0, y1;
  if (reach == 1) {
    SkipTexelRange(x, image_w, mask_w, &x0, &x1);
    SkipTexelRange(y, image_h, mask_h, &y0, &y1);
  } else {
    // The pixel alone: the same range with the two pixels either side left out (the control).
    const float sx = float(mask_w) / float(image_w), sy = float(mask_h) / float(image_h);
    const float tx = (float(x) + 0.5f) * sx - 0.5f, ty = (float(y) + 0.5f) * sy - 0.5f;
    x0 = std::clamp(int(std::floor(tx)), 0, mask_w - 1);
    x1 = std::clamp(int(std::floor(tx)) + 1, 0, mask_w - 1);
    y0 = std::clamp(int(std::floor(ty)), 0, mask_h - 1);
    y1 = std::clamp(int(std::floor(ty)) + 1, 0, mask_h - 1);
  }
  if (float(x1 - x0) >= kSkipMaxTexels || float(y1 - y0) >= kSkipMaxTexels) {
    return false;
  }
  for (int ty = y0; ty <= y1; ++ty) {
    for (int tx = x0; tx <= x1; ++tx) {
      if (mask_at(tx, ty) > limit) {
        return false;
      }
    }
  }
  return true;
}

// ---- the block pass and the test, as the shader makes them (hlsl/ao_contact_ps.hlsl SkipBlocks, MaskCannotLight) ----

// The block image's size for a mask: one texel a block of kSkipBlock x kSkipBlock texels, rounded up.
inline int SkipBlockCount(int mask_size) { return (mask_size + int(kSkipBlock) - 1) / int(kSkipBlock); }

// The block image: 1 where any texel of the block (clamped into the mask: the sampler's clamp to edge repeats the border) is above
// the limit. mask_at(tx, ty) is the stored value.
struct SkipBlockImage {
  int w = 0, h = 0;
  std::vector<uint8_t> bit;  // 1 above the limit
  int At(int bx, int by) const { return bit[size_t(std::clamp(by, 0, h - 1)) * size_t(w) + size_t(std::clamp(bx, 0, w - 1))]; }
};
template <class MaskAt>
SkipBlockImage MakeSkipBlocks(MaskAt&& mask_at, int mask_w, int mask_h, float limit) {
  SkipBlockImage image;
  image.w = SkipBlockCount(mask_w);
  image.h = SkipBlockCount(mask_h);
  image.bit.assign(size_t(image.w) * size_t(image.h), 0);
  const int block = int(kSkipBlock);
  for (int by = 0; by < image.h; ++by) {
    for (int bx = 0; bx < image.w; ++bx) {
      int above = 0;
      for (int j = 0; j < block && !above; ++j) {
        for (int i = 0; i < block && !above; ++i) {
          above = mask_at(std::min(bx * block + i, mask_w - 1), std::min(by * block + j, mask_h - 1)) > limit ? 1 : 0;
        }
      }
      image.bit[size_t(by) * size_t(image.w) + size_t(bx)] = uint8_t(above);
    }
  }
  return image;
}

// What the test reads for one pixel (the check counts them): texture fetches, 1 for a pixel in the sun, 1 + the blocks for one that is not.
struct SkipTestCost {
  int fetches = 0;
  int gathers = 0;
};

// The shader's MaskCannotLight at (x, y): the block of the pixel's own centre first (a 1 ends the test: marched), then the 2 x 2
// groups of blocks from the first block of the range to the last (a group may hold one block more than the range: tested too).
// Float operations in the shader's order, so the check sees what the margin sees.
inline bool MaskCannotLightBlocks(const SkipBlockImage& blocks, int mask_w, int mask_h, int image_w, int image_h, int x, int y,
                                  SkipTestCost* cost = nullptr) {
  const float sx = float(mask_w) / float(image_w), sy = float(mask_h) / float(image_h);
  const float scale4x = sx * 0.25f, scale4y = sy * 0.25f;  // sizes.xy
  const float px = float(x) + 0.5f, py = float(y) + 0.5f;  // SV_Position
  if (cost) {
    ++cost->fetches;
  }
  if (blocks.At(int(px * scale4x), int(py * scale4y)) != 0) {
    return false;
  }
  const float lox = (px - 1.0f) * scale4x - (0.5f + kSkipTexelMargin) * 0.25f;
  const float loy = (py - 1.0f) * scale4y - (0.5f + kSkipTexelMargin) * 0.25f;
  const float firstx = std::floor(lox), firsty = std::floor(loy);
  const float endx = std::floor(lox + (2.0f * scale4x + (2.0f * kSkipTexelMargin + 1.0f) * 0.25f));
  const float endy = std::floor(loy + (2.0f * scale4y + (2.0f * kSkipTexelMargin + 1.0f) * 0.25f));
  for (float by = firsty; by <= endy; by += 2.0f) {
    for (float bx = firstx; bx <= endx; bx += 2.0f) {
      if (cost) {
        ++cost->fetches;
        ++cost->gathers;
      }
      for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
          if (blocks.At(int(bx) + i, int(by) + j) != 0) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

// What one invocation of the shader writes with the skip: the sky/distance return first, then the skip, then the march proper.
// can_skip(x, y) is the test over the frame's mask. steps_used is 0 for a skipped pixel, as for any return before the loop.
template <class DepthAt, class CanSkip>
float MarchWithSkip(const Trace& t, DepthAt&& depth_at, CanSkip&& can_skip, int x, int y) {
  const float tc = depth_at(x, y);
  if (tc >= t.sky * 0.5f || tc >= t.max_distance || can_skip(x, y)) {
    t.steps_used = 0;
    return 1.0f;
  }
  return March(t, depth_at, x, y);
}

}  // namespace mcla::gpu::contact_trace

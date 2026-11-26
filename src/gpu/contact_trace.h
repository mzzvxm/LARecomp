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
// game's sun shadow map holds are left out of the ground-under-a-vehicle term), kCarMapFlag, kCarMapViewFlag and kCarMapKeepMarchFlag (the
// push block's bits of the car's own shadow map); the rest is mirrored in the shader and run by the check.
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
// THE MARK. A vehicle pixel that marched its short ray (March's `marked`) stores its visibility v as v - kVehicleMarkOffset, a value in -2 to -1
// that no other pixel stores (they hold 0 to 1; the image is R16_FLOAT, so the mark costs half a unit of 2^-10 in the value). The filter reads
// it as the one fact it needs about a neighbour, "this value came from the noisy march" ("the vehicle march's stipple" below), and takes the
// value back out before it weighs or sums it, so the filter's output is a plain visibility. Only the march writes the mark and only the two
// filters (ao_common.hlsli AoBilateralEdge and AoBilateralEdgeFolded) read the image.
inline constexpr float kVehicleMarkOffset = 2.0f;
inline float MarkedVisibility(float v) { return v - kVehicleMarkOffset; }
inline bool IsMarked(float stored) { return stored < -0.5f; }
inline float UnmarkedVisibility(float stored) { return IsMarked(stored) ? stored + kVehicleMarkOffset : stored; }
// THE MARK AND THE RAY TRACED TERM (hlsl/ao_contact_ps.hlsl, car_rt_ao.h; the two lines of work met in play build 11). Where the device traces rays against the
// player's car (and the cars beside it) the march takes the darker of its shadow and the traced sun ray's (March's `traced_shadow`, 0 lit to 1 the car's own parts in
// the way), a term that is exact and crisp: no noise, no resolution. A vehicle pixel the ray holds in a shadow (traced_shadow at or above this) therefore stores its
// visibility plain, not marked, as a pixel the car's map covers does: the filter's visibility weight keeps the shadow's edge from the lit pixels beside it, and the
// ray's edge keeps the width it was traced with (VehicleStippleIsFiltered, the section on the ray traced term: marking those pixels too makes the edge about as wide
// again as the map's was in the control of section 3). The vehicle pixels the ray leaves lit still march and are marked, so a false hit of the march there is still
// averaged. Without the term (traced_shadow 0, every build without the ray tracing line) nothing changes.
inline constexpr float kRtUnmarkedShadow = 0.5f;
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
// AoBilateralEdgeFolded, which reads only the diagonal neighbours' depths itself. Both give the same value. `vis_at` gives the image's stored
// value, marked or not (THE MARK above); the result is a plain visibility. Between two marked pixels the visibility's weight is 1.
template <class DepthAt, class VisAt>
float EdgeFilter(const Camera& c, DepthAt&& depth_at, VisAt&& vis_at, int x, int y, float tc, Vec3 pc, Vec3 normal,
                 const float* cross = nullptr) {
  const float footprint = FootprintUnit(c.tan_y, float(c.height)) * tc;
  const float plane_sigma = kEdgePlaneFloor + kEdgePlanePixels * footprint;
  const float centre_stored = vis_at(x, y);
  const bool centre_marked = IsMarked(centre_stored);
  const float centre = UnmarkedVisibility(centre_stored);
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
      const float stored = vis_at(qx, qy);
      const bool marked = IsMarked(stored);
      const float value = UnmarkedVisibility(stored);
      const float w = w_depth * w_space * ((centre_marked && marked) ? 1.0f : EdgeRangeWeight(value - centre));
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
// the vehicle march's stipple
// ---------------------------------------------------------------------------
//
// Owner's pictures, 2026-11-19 (the run ports\run\wroot1-wing-stripes-2, tour_07 pause-map-off, and tour_12 pause-map-march, which keeps the march on the pixels the
// map covers): with CAR SHADOWS = OFF and CONTACT SHADOWS on, the R34's trunk lid under the wing shows a fine stipple, every other pixel dark over a band. The
// car's map is not at fault (with it the lid's pixels take no steps and the lid is flat: tour_05, tour_13): the vehicle rule above is.
// WHERE THE NOISE COMES FROM. A vehicle pixel's 12 steps start at (i + noise) / 12 of the 0.3 m ray, the noise being the pixel's interleaved gradient noise
// (ao_common.hlsli AoNoise; ao.cpp passes frame 0, so it is the same pattern every frame, unless a temporal upscaler runs). The plate that shades the lid is 4 cm
// thick and 18 to 30 cm above it; a step counts as hidden only where the depth image shows a surface about 1 to 12 cm in front of it along the view ray, and from a
// camera 10 to 25 degrees above the lid that is a band a few centimetres high under the plate, as wide as the steps are apart at the ray's end (2 t L / n = 4.5 cm). Whether a pixel's
// ray puts a step in the band depends on its noise: the pixel is hit or missed (0 or 1) where the mean over the noise is a fraction (0.1 to 0.9), and the pixel
// next to it has another noise. THERE IS NO HISTORY to average it, in the pause view or in play: the contact term keeps none of its own (the occlusion's history
// is not read for it); the noise turns every frame only while a temporal upscaler runs (ao.cpp RecordContact) and the upscaler averages it; without one
// (the player's choice, or a view it does not run in) the stipple is a fixed pattern of the picture, which is what the pause view shows. The 3 x 3 filter kept it:
// its visibility weight is 0.2 wide, so a hit (0) and the miss next to it (1) are not averaged (the filter keeps a shadow's edge, and cannot tell a stipple from one).
// WHAT WAS MEASURED, on the twin of this march (VehicleStippleIsFiltered: the wing rig, 3 cameras, the plate 28 and 18 cm above the lid, the sun at 55 and 70 degrees
// from 3 sides, 1080 lines, the 2 million pixels of the lid; "stipple energy" is the mean of |pixel - mean of its 4 neighbours|; the reference is the mean of 16
// noises, the picture a history of frames would show):
//   - the reference: 0.0027; the march filtered as it was: 0.0317 (12 times), 0.149 in the worst scenario;
//   (an earlier sweep of 96 scenarios of the same rig, the noise's energy 0.0187 and the reference's 0.0028:)
//   - a fixed start (no noise, 0.5): 0.0029 on the rig, but on the real R34's mesh (a car dump, local only, a pause-like camera, 64 scenarios) 0.013 against the
//     reference's 0.0046 and the noise's 0.046: the 12 steps sample a 4 cm band, and with no noise the pixels that miss it form bands and rings along the plate's
//     shadow instead of dots, and its darkness is short of the reference's (26% in the rig's worst case, plate 18 cm, sun 65 degrees; 1.6% over the R34's scenarios);
//   - more steps (16, the setting's: 0.037 on the R34) and an ordered 2 x 2 or 4 x 4 start (0.0190 and 0.0175 on the rig): no better than the noise;
//   - the filter without the visibility's weight, on every vehicle pixel: 0.0046 on the R34, the reference's own roughness, and the lowest distance from the
//     reference (0.009 against 0.037), at no cost in the march; but the car map's shadow has crisp edges that the same filter widens to 2.4 times their
//     width (the check's control: 34005 soft pixels against 14272), and softness of the contact shadows is a complaint the owner has made.
// THE CHANGE (THE MARK above): the march marks a vehicle pixel that took steps (the pixels the map covers took none, so the map's shadow is not marked) and the
// two filters weigh two marked pixels by position and depth alone. The noise stays (it is what makes the mean right), the 3 x 3 of marked pixels is the average of nine
// noises, and everything else is filtered exactly as before. Measured on the same rig (the check): the stipple energy 0.0033 (1.2 times the reference's, 0.10 of
// the old filter's, 0.010 against 0.149 in the worst scenario), the distance from the reference 0.0053 against 0.0225, the total darkness unchanged (0.0566
// against 0.0559, the reference's 0.0567); with the map in the frame nothing of the lid is marked but the grazing pixels (769 of 588,732, whose weight is 0), the
// filtered lid is the old one to the half's last unit and the map's edges keep their width; the road beside a car differs from the old filter by 0.00017 at most
// (the marked neighbours' decode); the mirror on a door keeps its darkness (14315 against 14314). The march's own values are the old ones to the bit.
// COST (DXIL 6.0 static instructions, dxc 1.9.2609): the march +4 (2004 to 2008: a vehicle test it had, a compare and a subtract); the apply's folded filter +51 on
// every variant (1044 to 1095 for the plain one: about 6 a tap and 3 for the pixel's own; they run for every pixel the contact term can darken, vehicle or not,
// as the filter did); the denoise pass's filter +9 (334 to 343, a loop). A vehicle pixel pays the same filter as any other and +4 in the march.

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

// ---------------------------------------------------------------------------
// the player's car's own sun shadow map (car_shadow_map.h; hlsl/ao_contact_ps.hlsl "THE CAR'S SHADOW MAP")
// ---------------------------------------------------------------------------
//
// WHAT IT IS FOR. A screen-space march cannot see a part's thickness, and the game's own sun shadow map holds the player's car at a forced low level
// of detail in cascades whose texels are 1 to 25 cm. car_shadow_map.h draws the car again, depth only, from the sun, at full detail, into a small
// square map fitted to the car: the exact shadow of the wing, the mirrors, the sills and the wheels on the car and on the road near it. The march
// reads it (CarMapShadow below, mirrored in the shader) and the pixel takes the DARKER of the two. The apply pass still puts contact shadows only on
// the sun's share where the game's own mask says lit (a vehicle's pixel takes the car sun image's visibility, "the game's sun shadow mask is not a car's" above), so a map
// shadow never darkens what the game's mask already shadows twice on the world.
//
// THE LOOKUP (car_shadow_map.h CarShadowMapView). The view position of the pixel goes through the three rows of the map's matrix: u, v (0 to 1, v
// down) and the depth (0 nearest the sun) as stored, `depth_m` metres over the whole range, `texel_m` the side of a texel in metres, `size` texels a
// side. Everything is done in metres along the sun's ray.
//   - Bias. The receiver's depth is not the stored depth: the map's texels are point samples of the car's surface at their centres, and the
//     receiver is at a point of its own in between. On a surface that is not square to the sun the stored depth changes by up to texel * tan(angle)
//     from one texel to the next (and a PCF tap that is r texels away by r times that), so a constant bias, or one that scales with the slope but
//     not with the tap's distance, either leaves acne on the lit side of every face or takes the contact out of the shadow (the check:
//     contact_trace_test.cpp CarMapNoAcne). The receiver's own PLANE is used instead: the pixel's normal gives, per texel of the map, how much the
//     surface's depth changes along u and along v (the plane's gradient), and every tap compares the stored depth with the receiver's depth AT THE
//     TAP's position on that plane. What remains is a constant (kCarMapBiasFloor, 1 cm: the producer's advice), a quarter of a texel and a few
//     footprints of the pixel (its position is that of the pixel's centre on the ray, while the depth image holds sample 0). The producer's own
//     pipeline also pushes every stored depth away from the sun by a slope scaled bias (kCarMapProducerSlope: twice the surface's depth change per
//     texel, a few millimetres on a flat face and centimetres on one nearly edge-on), which is what keeps a lit face from shadowing itself at the
//     steps of the receiver's extended plane; the check builds its maps with the same bias. The reader does not depend on it: on maps made without it
//     the plane reader still has no acne, where a nearest texel and constant 1 cm reader has 3141 acne pixels in 501329 lit ones (CarMapNoAcne).
//   - Normal offset. The sample point is also moved off the surface along its normal by half a texel times the sine of the angle to the sun, which
//     is the common cure for the contact at a concave crease (a post's foot on a lid) being cut off by the plane's own bias; at a crease the cost is a
//     lift of less than half a texel.
//   - Penumbra. The sun covers half a degree, so a shadow's edge is d * 0.0088 wide at a blocker distance d (the distance along the ray from the
//     blocker to the receiver). A blocker search (the centre and four taps on a ring that is the penumbra of the farthest blocker the map can hold,
//     kCarMapSearchMetres, wide) finds the mean blocker distance; the filter is then the centre and a ring of four bilinear (2 x 2 texel) taps at the
//     penumbra's half width, blocker distance times the sun's angular radius, at least kCarMapMinFilterTexels and at most kCarMapMaxFilterTexels. The
//     ring is turned per pixel by the march's noise (a quarter turn is the whole pattern); the apply's 3x3 filter averages it. No blocker in the
//     search: no shadow, 5 gathers. A wing 28 cm over the lid has a penumbra of 2.5 mm: the minimum (a texel and a half) is what sets its edge. The
//     roof's shadow on the road at a low sun, blocker distance several metres, has centimetres.
//   - The edge. The map's border fades the term out over kCarMapEdgeTexels texels (a shadow that crosses it is the game's from there on), and a
//     point outside has none.
//   - Authority. A pixel is "covered" when one of the four centre texels holds a part of the car that is at the pixel's own depth or in front of it
//     (not deeper than the bias): the map saw the surface at that place in the sun's view, so the map is the truth about the car's own parts
//     there. A pixel nearer to the sun than the car's part behind it (a traffic car between the sun and the player's car) is not. On a covered
//     pixel of a VEHICLE the short vehicle march is dropped (below).
//
// WHAT THE VEHICLE MARCH STILL ADDS, AND WHY IT GOES ON THE PLAYER'S CAR'S OWN PIXELS. The short vehicle march (kVehicleReceiver*) was made to be
// right for the small things on a car, and its only job on the car's own surface was the car's own parts: a mirror on a door, a wing on its posts.
// The map has those exactly, at a texel's resolution (4 to 8 mm for the producer's 8 m window at 2048 or 1024) where the march had a 6 cm slab guess
// (contact_trace.h "a car's own pixels": a thin plate seen from a grazing angle has no depth in a depth image) and only a 30 cm ray: on the wing rig
// at 17.7 degrees (CarMapFalseShadowOnTheWing) the march alone leaves 31205 of the 34106 pixels inside the car's own shadows light (92%), the map
// none, and the false shadow the march still makes in the light (beyond 3 cm of a shadow's edge) is 43 pixels in 610609 against none. What the march
// alone could add on those pixels is the contact of something that is not the car with the car within 30 cm (a wall, a pole, a kerb beside the
// door): the game's cascades hold the big ones and what is left of the march's is a band about a centimetre wide, which the occlusion's own darkening
// at a crease cannot be told from. So where the map covers a vehicle pixel the march takes no steps (12 steps fewer on 5 to 15% of the picture:
// cheaper) and the pixel's shadow is the map's. NOT SEEN IN A RUN: the first run compares the two with mcla_gpu_contact_shadows_car_map_march. The march stays everywhere the map is not the authority: every
// pixel that is not a vehicle's (the road beside the car, the walls: the contact the map's shadow is combined with by the darker of the two), a
// vehicle pixel outside the map or whose four texels hold nothing (a traffic car: the map holds the player's car alone), and every pixel in a
// frame without a map, to the bit. kCarMapKeepMarchFlag (mcla_gpu_contact_shadows_car_map_march) restores the march on the covered pixels as well,
// for the comparison.
//
// COST. A pixel that faces the sun reads the matrix (four loads of a 4 x 1 texture, one cache line); one the map's window holds then makes five
// gathers, nine where a blocker is near (5 to 9 gathers a pixel; 5.3 to 5.8 on average over the check's pictures, which include the road round
// the car); a pixel turned from the sun (facing under kSunFacingLow) reads nothing. A vehicle pixel that the map covers saves 12 steps of the march.
// NOT MEASURED on a GPU.
//
// Push fields (hlsl/ao_contact_ps.hlsl): slots.w the map's bindless slot (R32_FLOAT or R16_FLOAT: the depth, 1 where no part of the car is), slots2.x
// the matrix texture's slot (R32G32B32A32_FLOAT, 4 x 1: the three rows, then (texel_m, depth_m, size, 1)); flags bit 28 a map is bound, bit 29 the
// debug view (the map's term alone), bit 30 the vehicle march also on the covered pixels. Without bit 28 the pass is as it was.
inline constexpr uint32_t kCarMapFlag = 1u << 28;
inline constexpr uint32_t kCarMapViewFlag = 1u << 29;
inline constexpr uint32_t kCarMapKeepMarchFlag = 1u << 30;

inline constexpr float kCarMapSunRadius = 0.0044f;         // tan of the sun's angular radius (a quarter of a degree): the penumbra's half width per metre of blocker distance
inline constexpr float kCarMapMinFilterTexels = 0.75f;     // the filter's half width is at least this (a bilinear tap alone is soft by a texel)
inline constexpr float kCarMapMaxFilterTexels = 6.0f;
inline constexpr float kCarMapSearchMetres = 0.03f;        // the blocker search's radius in metres: the penumbra of a blocker about 7 m away
inline constexpr float kCarMapSearchMinTexels = 1.5f;
inline constexpr float kCarMapNormalOffsetTexels = 0.5f;   // along the normal, times the sine of the angle between the normal and the sun
inline constexpr float kCarMapBiasFloor = 0.01f;           // m: the constant bias (the producer's advice: 1 to 2 cm)
inline constexpr float kCarMapBiasTexels = 0.25f;
inline constexpr float kCarMapFootprintBias = 2.0f;        // footprints of the pixel
// The map's own depth is already pushed away from the sun by its pipeline's slope scaled bias (car_shadow_map_rules.h kSlopeScaledDepthBias, the
// same number): kCarMapProducerSlope times the larger of the surface's depth changes per texel along u and along v. A receiver on the car's own
// surface is therefore that much nearer the sun than the map says, which is what keeps it lit; the pixel is still "covered" by it (below).
inline constexpr float kCarMapProducerSlope = 2.0f;
inline constexpr float kCarMapMaxTan = 8.0f;               // the plane's gradient (texels of depth per texel) is held to this: a surface edge-on to the sun has none of the sun's light anyway
inline constexpr float kCarMapEdgeTexels = 4.0f;
inline constexpr float kCarMapEmpty = 0.9999f;             // a stored depth at or above this is "no part of the car here"

// THE FILTER FOLLOWS THE WORLD'S (mcla_gpu_shadow_filter; owner, 2026-11-19: the car's shadow and the world's must match). The sun's cascades are
// filtered by the setting (shadow_filter.h: game, soft, pcss) and the car's own map had a filter of its own, a ring of four taps turned by the pixel's
// noise at the real sun's penumbra, which is crisp (a roof a metre up has an edge 4 mm wide) beside a world shadow that is soft. The map's
// matrix texture carries the world's mode and sun size in its last component (kCarMapFilterCode: mode * 256 + the sun in sixteenths), and
//   game  the filter above, to the bit
//   soft  the soft filter's disk: kCarMapFilterTaps taps on a Vogel disk (golden angle, sqrt radius, the weight 1 - r^2 / 2: the same as
//         src/shaderpack/variants/shadow_filter.cpp) of kCarMapSoftRadiusMetres, every tap a bilinear gather (a true PCF tap, smooth over a texel)
//   pcss  the same disk at the penumbra's width: the mean blocker distance times the sun's tangent times the sun's size (4 times the real sun by default:
//         mcla_gpu_shadow_filter_sun), at least kCarMapMinFilterTexels and at most kCarMapFilterMaxRadii soft radii (the world's cap)
// both with a blocker search first (the centre and 8 taps over the disk's reach, which says "no shadow here" without the 16 taps), and the disk
// turned by the pixel's noise only while a temporal upscaler runs (the push's frame number is not 0); otherwise it keeps its angle, as the world's does
// (the noise is a grain that nothing averages: shadow_filter.h kModeNoise).
inline constexpr uint32_t kCarMapFilterGame = 0;
inline constexpr uint32_t kCarMapFilterSoft = 1;
inline constexpr uint32_t kCarMapFilterPcss = 2;
inline constexpr float kCarMapSoftRadiusMetres = 0.03f;    // the soft disk's radius: the nearest cascade's 4 texels at the high shadow detail, about 2.5 to 3.5 cm
inline constexpr float kCarMapSunTan = 0.0047123890f;      // tan of the sun's angular radius, 0.27 degrees (the world's: shadow_filter.cpp kShadowFilterSunTan)
inline constexpr float kCarMapFilterMaxRadii = 2.0f;
inline constexpr int kCarMapFilterTaps = 16;
inline constexpr int kCarMapSearchTaps = 8;
inline constexpr float kCarMapFixedAngle = 2.399963f;      // the disk's angle without noise
inline constexpr float kCarMapGoldenCos = -0.7373688780783197f;
inline constexpr float kCarMapGoldenSin = 0.6754902942615238f;
// The matrix texture's last component: an exact integer in a float.
constexpr float CarMapFilterCode(uint32_t mode, uint32_t sun_sixteenths) {
  return float(mode * 256u + (mode == kCarMapFilterPcss ? (sun_sixteenths > 255u ? 255u : sun_sixteenths) : 0u));
}
constexpr uint32_t CarMapFilterModeOf(float code) { return uint32_t(code + 0.5f) >> 8; }
constexpr float CarMapFilterSunOf(float code) { return float(uint32_t(code + 0.5f) & 255u) * (1.0f / 16.0f); }

// The map's blocker test at one texel: the receiver's depth at the texel (metres along the sun's ray, on the receiver's plane) is deeper than the
// stored depth by more than the bias, and the texel holds a part of the car.
inline float CarMapBiasMetres(float texel_m, float footprint) {
  return kCarMapBiasFloor + kCarMapBiasTexels * texel_m + kCarMapFootprintBias * footprint;
}
// The filter's half width in texels for a mean blocker distance in metres.
inline float CarMapFilterTexels(float blocker_m, float texel_m) {
  return std::clamp(blocker_m * kCarMapSunRadius / texel_m, kCarMapMinFilterTexels, kCarMapMaxFilterTexels);
}
inline float CarMapSearchTexels(float texel_m) {
  return std::clamp(kCarMapSearchMetres / texel_m, kCarMapSearchMinTexels, kCarMapMaxFilterTexels);
}

// The map as the twin sees it: the contract's view (car_shadow_map.h) and the depths themselves (the shader reads the texture).
struct CarMapView {
  float to_map[3][4] = {};
  float texel_m = 0.0f;
  float depth_m = 0.0f;
  uint32_t filter_mode = kCarMapFilterGame;  // kCarMapFilter*: the world's filter (the matrix texture's last component)
  float sun_times = 4.0f;                    // pcss: the sun's size in times the real sun's
  bool noise_turns = false;                  // the disk turns by the pixel's noise (a temporal upscaler runs)
  int size = 0;
  const float* texels = nullptr;  // size * size stored depths, rows downward (v down)
  float At(int tx, int ty) const {
    tx = std::clamp(tx, 0, size - 1);  // the sampler's clamp to edge
    ty = std::clamp(ty, 0, size - 1);
    return texels[size_t(ty) * size_t(size) + size_t(tx)];
  }
};
struct CarMapResult {
  float shadow = 0.0f;       // 0 lit to 1 in the car's shadow (before the facing weight)
  bool covered = false;      // the four texels at the pixel hold a part of the car
  int gathers = 0;           // texture gathers made (the cost)
  int loads = 0;             // loads of the matrix texture
};

// One gather of the map (the shader's GatherRed): the four texels whose centres surround the texel position (tx, ty) (centres at +0.5), in the
// gather's order: (i0, j1), (i1, j1), (i1, j0), (i0, j0).
struct CarMapQuad {
  float s[4];           // stored depths
  float over[4];        // the receiver's depth at the texel minus the stored one, metres
  float weight[4];      // the bilinear weights of a sample at (tx, ty)
};
inline CarMapQuad CarMapGather(const CarMapView& m, float tx, float ty, float cx, float cy, float receiver_m, float grad_u, float grad_v) {
  const float bx = std::floor(tx - 0.5f), by = std::floor(ty - 0.5f);
  const float fx = tx - 0.5f - bx, fy = ty - 0.5f - by;
  const int i0 = int(bx), j0 = int(by);
  const float ix[4] = {float(i0), float(i0 + 1), float(i0 + 1), float(i0)};
  const float jy[4] = {float(j0 + 1), float(j0 + 1), float(j0), float(j0)};
  const int ci[4] = {i0, i0 + 1, i0 + 1, i0};
  const int cj[4] = {j0 + 1, j0 + 1, j0, j0};
  CarMapQuad q;
  for (int k = 0; k < 4; ++k) {
    q.s[k] = m.At(ci[k], cj[k]);
    const float plane = grad_u * (ix[k] + 0.5f - cx) + grad_v * (jy[k] + 0.5f - cy);
    q.over[k] = receiver_m + plane - q.s[k] * m.depth_m;
  }
  q.weight[0] = (1.0f - fx) * fy;
  q.weight[1] = fx * fy;
  q.weight[2] = fx * (1.0f - fy);
  q.weight[3] = (1.0f - fx) * (1.0f - fy);
  return q;
}

// The car's shadow at the view position `pc` whose normal (facing the camera) is `normal`; `footprint` the world size of the pixel there,
// `noise` the pixel's 0 to 1 (the ring's turn). The angle to the sun is the map's own (its depth axis: the sun's direction). Twin of the
// shader's CarMapShadow.
inline CarMapResult CarMapShadow(const CarMapView& m, Vec3 pc, Vec3 normal, float footprint, float noise) {
  CarMapResult out;
  out.loads = 4;
  if (!(m.texel_m > 0.0f && m.depth_m > 0.0f && m.size >= 2) || m.texels == nullptr) {
    return out;
  }
  const float size = float(m.size);
  const Vec3 row_u{m.to_map[0][0], m.to_map[0][1], m.to_map[0][2]};
  const Vec3 row_v{m.to_map[1][0], m.to_map[1][1], m.to_map[1][2]};
  const Vec3 row_d{m.to_map[2][0], m.to_map[2][1], m.to_map[2][2]};
  // The map's axes in view space, unit: u and v across the map (the rows are 1 / side long), the depth away from the sun.
  const float side = size * m.texel_m;
  const Vec3 axis_u = row_u * side, axis_v = row_v * side, axis_d = row_d * m.depth_m;
  const float facing_map = std::max(-Dot(normal, axis_d), 1.0f / kCarMapMaxTan);
  const float sin_t = std::sqrt(Saturate(1.0f - facing_map * facing_map));
  const Vec3 p = pc + normal * (kCarMapNormalOffsetTexels * m.texel_m * sin_t);
  const float u = Dot(p, row_u) + m.to_map[0][3], v = Dot(p, row_v) + m.to_map[1][3];
  const float edge = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v)) * size;  // texels to the border
  if (edge <= 0.0f) {
    return out;
  }
  const float receiver_m = (Dot(p, row_d) + m.to_map[2][3]) * m.depth_m;
  // The plane's gradient: metres of depth per texel along u and along v, held to kCarMapMaxTan texels per texel.
  const float limit = kCarMapMaxTan * m.texel_m;
  const float grad_u = std::clamp(m.texel_m * Dot(normal, axis_u) / facing_map, -limit, limit);
  const float grad_v = std::clamp(m.texel_m * Dot(normal, axis_v) / facing_map, -limit, limit);
  const float bias = CarMapBiasMetres(m.texel_m, footprint);
  // How far the map's own surface at this slope is pushed deeper by the producer's bias: a pixel on that surface is nearer the sun than the map by about this.
  const float cover = bias + kCarMapProducerSlope * std::max(std::fabs(grad_u), std::fabs(grad_v));
  const float cx = u * size, cy = v * size;
  const float angle = noise * 1.57079632679f;
  const float ax = std::cos(angle), ay = std::sin(angle);  // the ring's axis; its quarter turns are the other three taps
  const float ox[4] = {ax, -ay, -ax, ay}, oy[4] = {ay, ax, -ay, -ax};
  const float r0 = CarMapSearchTexels(m.texel_m);
  // The blocker search: the centre and the ring at the search radius.
  float blocker_sum = 0.0f, blocker_count = 0.0f;
  float share = 0.0f;
  {
    const CarMapQuad q = CarMapGather(m, cx, cy, cx, cy, receiver_m, grad_u, grad_v);
    ++out.gathers;
    for (int k = 0; k < 4; ++k) {
      if (q.s[k] < kCarMapEmpty) {
        // covered: the map holds a part of the car at this texel that is not behind the receiver's surface (a pixel that is nearer to the sun
        // than the car's part, a traffic car in front of the player's car, is not the map's to judge)
        out.covered = out.covered || q.over[k] > -cover;
        if (q.over[k] > bias) {
          blocker_sum += q.over[k];
          blocker_count += 1.0f;
          share += q.weight[k];
        }
      }
    }
  }
  if (m.filter_mode != kCarMapFilterGame) {
    // The world's filter (kCarMapFilterSoft / kCarMapFilterPcss): the centre above, then the search over the disk's reach and the disk itself.
    const float soft = kCarMapSoftRadiusMetres / m.texel_m;  // texels
    const float reach = m.filter_mode == kCarMapFilterPcss ? kCarMapFilterMaxRadii * soft : soft;
    float dx = std::cos(m.noise_turns ? noise * 6.28318530718f : kCarMapFixedAngle);
    float dy = std::sin(m.noise_turns ? noise * 6.28318530718f : kCarMapFixedAngle);
    const auto turn = [&] {
      const float nx = dx * kCarMapGoldenCos - dy * kCarMapGoldenSin;
      const float ny = dx * kCarMapGoldenSin + dy * kCarMapGoldenCos;
      dx = nx;
      dy = ny;
    };
    for (int j = 0; j < kCarMapSearchTaps; ++j) {
      const float at = (float(j) + 0.5f) / float(kCarMapSearchTaps);
      const float r = std::sqrt(at) * reach;
      const CarMapQuad q = CarMapGather(m, cx + dx * r, cy + dy * r, cx, cy, receiver_m, grad_u, grad_v);
      ++out.gathers;
      for (int k = 0; k < 4; ++k) {
        if (q.s[k] < kCarMapEmpty && q.over[k] > bias) {
          blocker_sum += q.over[k];
          blocker_count += 1.0f;
        }
      }
      turn();
    }
    if (blocker_count < 0.5f) {
      return out;
    }
    float radius = soft;
    if (m.filter_mode == kCarMapFilterPcss) {
      radius = std::clamp(blocker_sum / blocker_count * kCarMapSunTan * m.sun_times / m.texel_m, kCarMapMinFilterTexels, reach);
    }
    float sum = 0.0f, weights = 0.0f;
    for (int i = 0; i < kCarMapFilterTaps; ++i) {
      const float at = (float(i) + 0.5f) / float(kCarMapFilterTaps);
      const float weight = 1.0f - 0.5f * at;
      const float r = std::sqrt(at) * radius;
      const CarMapQuad q = CarMapGather(m, cx + dx * r, cy + dy * r, cx, cy, receiver_m, grad_u, grad_v);
      ++out.gathers;
      float tap = 0.0f;
      for (int k = 0; k < 4; ++k) {
        if (q.s[k] < kCarMapEmpty && q.over[k] > bias) {
          tap += q.weight[k];
        }
      }
      sum += weight * tap;
      weights += weight;
      turn();
    }
    out.shadow = Saturate(sum / weights) * Saturate(edge / kCarMapEdgeTexels);
    return out;
  }
  for (int t = 0; t < 4; ++t) {
    const CarMapQuad q = CarMapGather(m, cx + r0 * ox[t], cy + r0 * oy[t], cx, cy, receiver_m, grad_u, grad_v);
    ++out.gathers;
    for (int k = 0; k < 4; ++k) {
      if (q.s[k] < kCarMapEmpty && q.over[k] > bias) {
        blocker_sum += q.over[k];
        blocker_count += 1.0f;
      }
    }
  }
  if (blocker_count < 0.5f) {
    return out;
  }
  // The filter: the centre's share again (above) and the ring at the penumbra's half width, turned an eighth.
  const float r1 = CarMapFilterTexels(blocker_sum / blocker_count, m.texel_m);
  const float c8 = 0.70710678118f;
  const float fx[4] = {(ax - ay) * c8, (-ax - ay) * c8, (-ax + ay) * c8, (ax + ay) * c8};
  const float fy[4] = {(ay + ax) * c8, (ax - ay) * c8, (-ay - ax) * c8, (-ax + ay) * c8};
  float total = share;
  for (int t = 0; t < 4; ++t) {
    const CarMapQuad q = CarMapGather(m, cx + r1 * fx[t], cy + r1 * fy[t], cx, cy, receiver_m, grad_u, grad_v);
    ++out.gathers;
    float tap = 0.0f;
    for (int k = 0; k < 4; ++k) {
      if (q.s[k] < kCarMapEmpty && q.over[k] > bias) {
        tap += q.weight[k];
      }
    }
    total += tap;
  }
  out.shadow = Saturate(total * 0.2f) * Saturate(edge / kCarMapEdgeTexels);
  return out;
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
  mutable bool marked = false;      // out: the last March was a vehicle pixel's short march (it stores MarkedVisibility of its result: THE MARK)
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
  float traced_shadow = 0.0f;           // the ray traced sun term at the pixel (car_rt_ao.h; THE MARK AND THE RAY TRACED TERM): 0 lit or no term, 1 the car's own parts in the sun's way
  const CarMapView* car_map = nullptr;  // the player's car's sun shadow map (CarMapShadow); null: no map, the march as it was
  bool car_map_keep_march = false;      // kCarMapKeepMarchFlag: the vehicle march also runs on the pixels the map covers
  bool car_map_only = false;            // kCarMapViewFlag: the debug view, March returns 1 minus the map's term alone
  mutable CarMapResult car_result;      // out: what the map gave for the last March (zero where it was not asked)
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
  t.marked = false;
  t.car_result = CarMapResult{};
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
  // The player's car's sun shadow map (CarMapShadow): the pixel's shadow from the car's own parts, exact. Where it covers a vehicle pixel the
  // short vehicle march has nothing left to add (the map holds the car's own parts; "what the vehicle march still adds" above): no steps.
  // A surface this nearly edge-on to the sun gets no contact shadow whatever made it (the weight below is 0): the map is not read there.
  t.car_result = CarMapResult{};
  if (!t.classic && t.car_map) {
    if (facing > kSunFacingLow) {
      t.car_result = CarMapShadow(*t.car_map, pc, normal, footprint_unit * tc, t.noise);
    }
    if (t.car_map_only) {
      t.steps_used = 0;
      return 1.0f - t.car_result.shadow * Smoothstep(kSunFacingLow, kSunFacingHigh, facing);
    }
    if (t.car_result.covered && DepthIsVehicle(tc) && !t.car_map_keep_march) {
      steps = 0;
    }
  }
  // The ground under a vehicle starts the shadow (0 when the term is off or the pixel is not that).
  float shadow = t.classic ? 0.0f : UnderVehicle(t, depth_at, tc, pc, normal);
  // The steps: the setting's, or (the lever on) as many as the ray's length on the screen needs.
  if (!t.classic && t.adapt_pixels > 0.0f) {
    steps = AdaptiveSteps(RayPixelLength(cam, start, start + t.to_sun * length), steps, t.adapt_pixels,
                          t.adapt_min_steps);
  }
  t.steps_used = steps;
  // A vehicle pixel that marches (the map did not take its steps) writes the marked value: the noise of its ray's start is in it.
  t.marked = !t.classic && t.vehicle_rule && DepthIsVehicle(tc) && steps > 0 && t.traced_shadow < kRtUnmarkedShadow;
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
  if (!t.classic && t.car_map) {
    shadow = std::max(shadow, t.car_result.shadow);  // the darker of the march and the map
  }
  if (!t.classic) {
    shadow = std::max(shadow, t.traced_shadow);  // ... and of the ray traced sun ray (0 without the term)
  }
  if (!t.classic) {
    shadow *= Smoothstep(kSunFacingLow, kSunFacingHigh, facing);
  }
  return 1.0f - shadow;
}

// What the march's pass stores for the pixel (x, y): March's visibility, marked when it was a vehicle pixel's short march (THE MARK).
template <class DepthAt>
float MarchStored(const Trace& t, DepthAt&& depth_at, int x, int y) {
  const float v = March(t, depth_at, x, y);
  return t.marked ? MarkedVisibility(v) : v;
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

// ---------------------------------------------------------------------------
// the game's sun shadow mask is not a car's (the comb under the R34's wing, owner, 2026-11-19)
// ---------------------------------------------------------------------------
//
// THE CASE. The pause menu's picture of the player's R34 at noon: "a comb of vertical dark lines on the trunk lid under the rear wing". The lid
// between the wing's posts is the wing's real shadow (the car's sun shadow map: the contact visibility, view 2, is 0 over all of it), a flat
// factor of 0.19, cut by columns 3 pixels wide about every 25 pixels where the factor is 1: bright lid in a black rectangle.
// MEASURED (ports\run\cwing1-wing-stripes-2, 2880 x 1620, lid rows 880 to 900): the columns of the bright lid are the columns where the game's
// sun shadow mask (contact view 3) is 0, run for run (x 2228..2231, 2252..2254, 2280..2282, 2306..2308, 2332..2334, 2405..2407, 2430..2432,
// 2454..2457: the same place and width in both), and the mask's columns are the iron fence BEHIND the car: the same bars, at the same x, run
// unbroken from the rows where the fence is seen through the rear window (850, 860) down over the lid (890). The car is not in the mask: the game
// collects it at phase 0x1000 and resolves it at 0x4000, "before vehicles and the world" (AO-DESIGN.md), so a car's pixel holds the mask of what
// is behind the car, and the car's own shadow on the road is in it as a black blob. The pitch is the fence's, 25.5 pixels (6.4 cm on the lid at
// about 2.5 mm a pixel), not the car's map's: its texel is 3.9 mm, 1.5 pixels, and a bias or acne error would run at that scale.
//
// THE APPLY. Its weight is strength * sun_share * fade, sun_share = Ls N.L v / (La + Ls N.L v), v the mask at the pixel: where the mask is 0
// the pixel is under the game's own shadow and the contact term leaves it alone (nothing is shadowed twice). On a car that is the fence's shadow.
// The map, the vehicle march and the vehicle bit are not at fault: with the mask lit everywhere the lid is one flat darkness (the check), the bit
// is set over the whole lid (view 8), the map was off live in the map-off picture (the log's "car shadow map off (set)") and that picture shows
// the same bright columns through the old march's stipple.
//
// THE GAME DOES NOT USE THE MASK ON A CAR EITHER. In the same pause picture, the game's own colours (contact shadows off) have a mean horizontal
// step of 1.27 at the mask's edges against 1.39 elsewhere on the pixels the depth pass takes for vehicles (no relation), and 17.29 against 1.58 on
// the world's pixels beside the car (the world's lit shaders multiply their sun by the mask). A car's shader takes the sun's visibility from its own
// shadow lookup (FastShadowCollector). So the sun's share of a car's colour that the apply works out from the mask is the share of the world
// behind the car. A car's pixel takes a visibility of 1 (the game's colour is not masked: the sun's share of it is Ls N.L / (La + Ls N.L)); what
// shadows the car's own parts is the map and the vehicle march. What the cascades do to a car (a building's shade on it) is in the car's colour
// and is not known here: a car in a building's shade gets the term of a lit one (CHECK: how that looks; the term is a small shadow of its own parts).
// (That last sentence is CarSunFromImage's below: a frame that has the game's car sun image uses it; this is the rule of a frame without.)
inline float CarSunVisibility(bool vehicle, float mask_visibility) { return vehicle ? 1.0f : mask_visibility; }

// ---------------------------------------------------------------------------
// a car's own sun visibility: the game's car sun image (CAR-SUNVIS, 2026-11-19)
// ---------------------------------------------------------------------------
//
// THE QUESTION. A car in a building's shade got the contact term of a lit car (the line above). The game's own colour knows better: every car shader
// takes the sun's visibility at the pixel from a lookup of its own, so the host can take it from the same place.
//
// WHAT THE CAR SHADERS READ (the game's translated HLSL, native\work\hlsl-head\cars: 33 pixel shaders fetch FastShadowCollectorSampler, which is slot 13,
// and 29 of them build the coordinate with `trunc(gViewInverse(3).xz)`, e.g. xCarPaintCustomizable__PS_CarPaint.hlsl:759-774, xCarPaint, xBumpSpec,
// xRimMain, xCarGlass, xChrome, xUnderbody, xLicensePlate, xGrill, xInteriorTrim, xWheelRubber ...). It is NOT the screen sized mask (that one is for the
// world: the lit road shaders read it at the screen position, and it is made before the cars are drawn) but a 256 m wide TOP-DOWN image of the ground's
// sun visibility around the camera, one texel a metre:
//     u = (world.x - trunc(camera.x)) / 256 + 0.5      v = (world.z - trunc(camera.z)) / 256 + 0.5      (c249.x = 0x3B800000 = 1/256, c253.x = 0.5;
//     the world's y is up, gViewInverse(3) the camera's position; trunc is toward 0, so the image's centre moves a whole metre at a time)
//     the car's sun term = saturate(s - 0.25) * 1.3333325                                                  (c250.z = 0xBE800000 = -0.25, c249.y = 0x3FAAAA8F)
// s the image's .x, bilinear (the guest's sampler: min and mag linear, GFX-UPGRADES-DESIGN.md 6B.2 item 3). The image by day is the 256x256
// k_8_8_8_8 product at guest page 0x0329E000, resolved once a frame before the main scene (census: 22 collector draws of PS 2cdea0e25045ad9e into a
// 256x256 target, then two draws and the resolve into 0329e: after the screen mask's resolve, before the first car is drawn); at night that page holds a
// 512x512 k_8 shadow, in rain the cars read an 8x8 stand-in at 0329d (a constant 0.5), and at 6 h, 18 h and in rain the cascades are not drawn: a frame
// without a 256x256 k_8_8_8_8 resolve into that page has no image and the visibility is 1, as before. The image has no height (G8A-CAR-SELFREFL.md 1.3:
// "world-space, no height: the car's own parts are not in it"), so a car's roof and its flank and its wheels read the same place of it, which is the
// ground under them: a car standing half in a building's shade is lit on the half that is over lit ground. That is the game's own colour; the apply
// now agrees with it. The cascades themselves (the 1280x1280 k_24_8 atlas at 06E65000 and the matrices c64-c79 of the collector) are reachable the same
// way (a resolve product by page, the constants of the collector's draws are frame constants), but no car shader reads them: the collector's car draws
// all come before the cars, so the cascades would only give the host a second guess at what this image already says, with a cascade's choice and a PCF
// of its own to copy.
//
// THE HOST gives the apply the image (its bindless slot) and the lookup: world x and z of a view-space point (x right, y up, z forward, the AO's)
// are  ax . p + ox  and  az . p + oz  with ax = (right.x, up.x, forward.x), az = (right.z, up.z, forward.z) (the rows of the camera's view inverse, the
// game's convention: forward is minus row 2) and (ox, oz) = the camera's position minus its truncation: the game's own world coordinate relative to
// trunc(camera). The pose is the frame's own when the main view's vertex constants are trusted for it (ao.cpp vs_pose_trusted_), else the last frame's.
// The 8 numbers go to the apply in a 4x1 RGBA32F image of their own (the push block has no room left: the apply reads every field) drawn with the
// vehicle table's pass: texel 0 (ax, ox), texel 1 (az, oz).
//
// CHECKS the first run settles (the scene car-shade.scene): (1) the stored value of a lit car's ground is 1 (visibility 1 on a lit car, a flat
// view 13 on the car), a shaded one 0.25 or below; (2) the image's .x is the red channel of the host's RGBA view (the mask's is); (3) the player's own
// cast shadow is not in the image under him (the sun's side of a lit car reads 1), else a lit car reads dark over its own footprint.
inline constexpr float kCarSunWindow = 256.0f;  // metres a side of the image
inline constexpr float kCarSunFloor = 0.25f;    // saturate(s - 0.25)
inline constexpr float kCarSunGain = 1.3333325f;  // c249.y = 0x3FAAAA8F: a hair under 4/3, so a lit car (stored 1) reads 0.9999994

// The sun's visibility a car shader makes of the image's stored value.
inline float CarSunFromImage(float stored) { return std::min(Saturate(stored - kCarSunFloor) * kCarSunGain, 1.0f); }

// The lookup's 8 numbers: world x and z (relative to the truncated camera) of the view-space point p are Dot(ax, p) + ox and Dot(az, p) + oz.
struct CarSunLookup {
  Vec3 ax;
  Vec3 az;
  float ox = 0.0f;
  float oz = 0.0f;
};

// From the camera's view inverse rows (right = row 0, up = row 1, forward = minus row 2, position = row 3), the game's world with y up.
inline CarSunLookup MakeCarSunLookup(const double right[3], const double up[3], const double forward[3], const double position[3]) {
  CarSunLookup l;
  l.ax = {float(right[0]), float(up[0]), float(forward[0])};
  l.az = {float(right[2]), float(up[2]), float(forward[2])};
  l.ox = float(position[0] - std::trunc(position[0]));
  l.oz = float(position[2] - std::trunc(position[2]));
  return l;
}

// The image's coordinates of the view-space point `pc` (the shader's CarSunVisibility).
inline void CarSunUv(const CarSunLookup& l, Vec3 pc, float* u, float* v) {
  *u = (Dot(l.ax, pc) + l.ox) * (1.0f / kCarSunWindow) + 0.5f;
  *v = (Dot(l.az, pc) + l.oz) * (1.0f / kCarSunWindow) + 0.5f;
}

// What the game computes for the same point: the world position `world` (x, y, z) and the camera's `position`.
inline void CarSunUvOfWorld(const double world[3], const double position[3], float* u, float* v) {
  *u = float((world[0] - std::trunc(position[0])) / double(kCarSunWindow) + 0.5);
  *v = float((world[2] - std::trunc(position[2])) / double(kCarSunWindow) + 0.5);
}

// The apply's visibility at a pixel (hlsl/ao_apply_ps.hlsl CarAwareSunVisibility): the world's pixel takes the mask's, a vehicle's pixel the car sun
// image's value at its place when the frame has one, else 1 (CarSunVisibility). `image_at(u, v)` is the bilinear .x of the image.
template <class ImageAt>
inline float CarAwareSunVisibility(bool vehicle, float mask_visibility, bool have_image, const CarSunLookup& l, Vec3 pc, ImageAt&& image_at) {
  if (!vehicle) {
    return mask_visibility;
  }
  if (!have_image) {
    return 1.0f;
  }
  float u = 0.0f, v = 0.0f;
  CarSunUv(l, pc, &u, &v);
  return CarSunFromImage(image_at(u, v));
}

// THE SKIP FOLLOWS. The skip returns 1 where the apply's weight is at most kSkipWeight, which is proven from the mask alone (above). A vehicle's
// pixel has a weight that is not (its visibility is 1) and the apply's filter reads the 8 neighbours' values, so a pixel is not skipped when it or
// any of its 8 neighbours is a vehicle's: every pixel that runs the filter then still has all of its neighbours marched, and the proof stands.
// `depth_at(x, y)` is the level-0 depth with its vehicle bit (the image the apply reads the bit from), the coordinates clamped like the shader's.
// The shader asks only for a pixel the mask test would skip (hlsl/ao_contact_ps.hlsl VehicleInReach).
template <class DepthAt>
bool VehicleInReach(DepthAt&& depth_at, int width, int height, int x, int y) {
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (DepthIsVehicle(depth_at(std::clamp(x + dx, 0, width - 1), std::clamp(y + dy, 0, height - 1)))) {
        return true;
      }
    }
  }
  return false;
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
// can_skip(x, y) is the test over the frame's mask; a pixel with a vehicle's pixel in its 3 x 3 is not skipped (VehicleInReach; the shader
// asks only for a pixel the mask test would skip). steps_used is 0 for a skipped pixel, as for any return before the loop.
template <class DepthAt, class CanSkip>
float MarchWithSkip(const Trace& t, DepthAt&& depth_at, CanSkip&& can_skip, int x, int y) {
  const float tc = depth_at(x, y);
  if (tc >= t.sky * 0.5f || tc >= t.max_distance ||
      (can_skip(x, y) && !VehicleInReach(depth_at, t.camera.width, t.camera.height, x, y))) {
    t.steps_used = 0;
    return 1.0f;
  }
  return March(t, depth_at, x, y);
}

// ---------------------------------------------------------------------------
// the contact factor per colour channel: a shadow takes the SUN's light away, in the sun's colour
// ---------------------------------------------------------------------------
// The scene colour is albedo * (ambient + sun * N.L * visibility) and a contact shadow (or the car's shadow map through the contact pass) takes the sun's
// share of it away. The first version took the share of the LUMAS (ambient 0.114, sun 0.903 at noon) and multiplied all three channels by it: the
// luma is right, the colour is not. The sunlit road is warm (a warm sun on a cool sky's ambient) and a grey multiplier keeps its hue: under the car at noon,
// where the car's map owns the shadow and the game's cascade no longer shades it, the shadow came out darker and WARMER than the game's own shadow beside it
// (measured on shdith1-shd-noon-x1, rear camera: the game's own shadow, the cascade holding the car, is (20.5, 24.8, 23.3) against a lit (105.5, 101.4, 79.6),
// the factor 0.19, 0.25, 0.29 for red, green, blue; the car map's, (21.6, 20.4, 14.8), the same 0.20 for all three). A shadow removes the sun's share of EACH
// channel: S_c * N.L * v / (A_c + S_c * N.L * v), and what stays is albedo * A_c, the sky's colour. The share of a grey light is the old number, bit for
// bit (the check below), so a night, a cloudy hour or a grey frame is as before.
//
// The apply pass takes the two lights' colours as chroma = channel / luma, red and blue of each (green follows from the luma), 7 bits each in 1/32 (0 to
// 3.97: a dusk sky is bluer than twice its luma): one dword in shape.w of the apply's push block (the apply took 0.5 * the fade's end there: the shader makes it itself now). The dword is read as a
// float by the GPU, so its top three bits are fixed (0, 0, 1: the exponent is never 0 or 255, never a denormal, an infinity or a NaN a copy might change).
inline constexpr float kChromaScale = 32.0f;
inline constexpr uint32_t kChromaMaxByte = 127;
inline constexpr uint32_t kChromaMarker = 0x20000000u;
inline constexpr uint32_t kNeutralChroma = kChromaMarker | 32u | (32u << 7) | (32u << 14) | (32u << 21);  // chroma 1 in red and blue, both lights: grey
inline constexpr float kLumaRed = 0.2126f;
inline constexpr float kLumaGreen = 0.7152f;
inline constexpr float kLumaBlue = 0.0722f;

struct Rgb {
  float r = 0.0f, g = 0.0f, b = 0.0f;
};

inline float LumaOf(const float c[3]) {
  const auto usable = [](float x) { return std::isfinite(x) && x > 0.0f ? x : 0.0f; };
  return kLumaRed * usable(c[0]) + kLumaGreen * usable(c[1]) + kLumaBlue * usable(c[2]);
}

// One chroma value: channel / luma in 1/32, clamped to 0..127; a light with no luma is grey (chroma 1).
inline uint32_t ChromaByte(float channel, float luma) {
  if (!(luma > 1e-6f) || !std::isfinite(channel)) {
    return uint32_t(kChromaScale);
  }
  return uint32_t(std::clamp(std::lround(std::max(channel, 0.0f) / luma * kChromaScale), 0L, long(kChromaMaxByte)));
}

// The code: bits 0-6 the ambient's red chroma, 7-13 its blue, 14-20 the sun's red, 21-27 its blue; bits 28-31 are the marker (kChromaMarker).
// An indoor authored key light is independent of outdoor rain and horizon.
constexpr float LightStrength(float setting, float elevation_gate, float wetness_gate, bool garage) {
  return garage ? setting : setting * elevation_gate * wetness_gate;
}

inline uint32_t PackChroma(const float ambient[3], const float sun[3]) {
  const float la = LumaOf(ambient), ls = LumaOf(sun);
  return kChromaMarker | ChromaByte(ambient[0], la) | (ChromaByte(ambient[2], la) << 7) | (ChromaByte(sun[0], ls) << 14) | (ChromaByte(sun[2], ls) << 21);
}

// A light's chroma (channel / luma for the three channels) from its red and blue bytes; the green follows from the luma weights.
inline Rgb ChromaRgb(uint32_t red_byte, uint32_t blue_byte) {
  const float r = float(red_byte) * (1.0f / kChromaScale);
  const float b = float(blue_byte) * (1.0f / kChromaScale);
  return {r, std::max((1.0f - kLumaRed * r - kLumaBlue * b) / kLumaGreen, 0.0f), b};
}

// The sun's share of the light in each channel: S_c * lit / (A_c + S_c * lit), lit = N.L times the sun's visibility. The lumas are the apply's
// light.w (ambient) and extra.x (sun); a pixel with no light at all has no share.
inline Rgb SunShareRgb(float ambient_luma, float sun_luma, uint32_t chroma_code, float lit) {
  const Rgb a = ChromaRgb(chroma_code & 0x7Fu, (chroma_code >> 7) & 0x7Fu);
  const Rgb s = ChromaRgb((chroma_code >> 14) & 0x7Fu, (chroma_code >> 21) & 0x7Fu);
  const auto share = [&](float ac, float sc) {
    const float amb = ambient_luma * ac, sunlit = sun_luma * sc * lit;
    return (amb + sunlit) > 1e-6f ? sunlit / (amb + sunlit) : 0.0f;
  };
  return {share(a.r, s.r), share(a.g, s.g), share(a.b, s.b)};
}

// The apply pass's contact factor: 1 - weight * share * (1 - cs), per channel; weight = the setting's strength times the distance fade, cs the
// contact visibility (1 open, 0 shadowed). With cs 0 and weight 1 what is left of the lit colour is albedo * ambient, in the ambient's colour.
inline Rgb ContactFactorRgb(const Rgb& share, float weight, float cs) {
  const float k = Saturate(weight) * (1.0f - Saturate(cs));
  return {1.0f - Saturate(k * share.r), 1.0f - Saturate(k * share.g), 1.0f - Saturate(k * share.b)};
}


}  // namespace mcla::gpu::contact_trace

#pragma once
// ===========================================================================
// MCLA GPU renderer -- RtDenoise: the shared denoiser of the ray traced effects that are noisy by design (CITY-RT package R6 of perf/CITY-RT-M2.md section 7 and 8; CITY-RT-M3.md G5 shares it)
// ===========================================================================
// The pure side: the numbers and functions of hlsl/rt_denoise_common.hlsli as C++ twins (rt_denoise_test.cpp compiles that .hlsli ITSELF as C++ through a shim of the HLSL types and compares every function
// with the twin below), and the words of the two consumers' push blocks. The host is rt_denoise.h / rt_denoise.cpp; the passes' bodies are hlsl/rt_denoise_engine.hlsli; the screen-space reflections' use
// of it (which frames, which iterations, the debug setting, the census) is ssr_denoise_rules.h.
//
// WHAT IT FILTERS. A signal of ONE noisy sample a pixel with a hit distance: the effect's radiance (the reflections' fallback ray, the probe field's per-pixel detail ray) at the signal's own size, the
// ray's length from the surface, and the guides the effect has (depth, normal, roughness). The design is the owner's order for the denoisers (11-19): DLSS Ray Reconstruction where it is on and
// supported (the effect then hands it the raw sample and none of this runs: ssr_denoise_rules.h Why::kRawFrame), ours everywhere else, NRD only as a measured A/B. Two stages, each a compute pass:
//
//   TEMPORAL  (hlsl RtDnTemporalMain) the last frame's result, moved by the reflection's VIRTUAL motion: the mirror image of the hit stands `hit` behind the receiver along the view ray (the reflected
//             path unfolded into a straight line), so that point is what the camera's motion moves, and the reflection of a wall on a wet road does not slide with the road. A rough lobe has no
//             mirror image to follow: the point goes from the virtual image to the surface itself between perceptual roughness 0.3 and 0.6 (VirtualShare). The history counts only where the surface was the same
//             one (SameSurface: the stored receiver depth against the expected, 8%), is held to the neighbourhood's VARIANCE BOX (mean +- 1.25 deviations inside the min / max widened by a tenth: the
//             reflection that changed does not leave a ghost; the clip keeps the colour, a clamp turns it) and is blended by its length: n / (n + 1), n at most 32 (the plain running mean while it
//             is young, an exponential one after). The new length goes in the result's alpha.
//   SPATIAL   (hlsl RtDnSpatialMain) a bilateral a-trous on the pixels the signal's SHARE names: five by five taps of a B3 spline (1 4 6 4 1) at a step of 1, 2 or 4 pixels (iterations 0, 1, 2), each tap
//             weighed by the depth's distance from the centre's tangent plane, the normal (cos^24), the roughness, the luminance against the neighbourhood's own variance and the hit distance in octaves
//             (two reflections whose hits are a factor apart are two objects). The kernel's RADIUS follows the reflection: the specular lobe's half width at the receiver in pixels (LobePixels: the lobe's
//             tangent times the hit, over the unfolded distance z + hit, over the pixel's size at z), shortened to 0.4 as the history grows to 12 frames, at most 12 pixels; a pixel takes the iteration of step s when its
//             radius is at least 0.75 s (so a near mirror is not touched, and a rough surface with a far hit takes all three). A pixel whose share is under 0.05 is copied.
//
// THE PROFILES. kSpecular (a reflection): the kernel follows the lobe, the hit distance is an edge, the weights are tight. kDiffuse (a diffuse term, the probe field's detail ray): a fixed radius of six
// pixels before the history shortens it, no hit distance, looser weights.
//
// The profile's numbers, the radius law, the weights and the temporal rules are tested twice: against definitions here and against the .hlsli that runs on the GPU.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::rt_denoise {

// ---------------------------------------------------------------------------
// the numbers (hlsl/rt_denoise_common.hlsli)
// ---------------------------------------------------------------------------

inline constexpr float kMaxRadius = 12.0f;
inline constexpr float kLobeScale = 1.0f;
inline constexpr float kDiffuseRadius = 6.0f;
inline constexpr float kMatureLength = 12.0f;
inline constexpr float kMatureScale = 0.4f;
inline constexpr float kStepReach = 0.75f;
inline constexpr float kMinShare = 0.05f;
inline constexpr float kClipGamma = 1.25f;
inline constexpr float kClipMargin = 0.1f;
inline constexpr float kMaxLength = 32.0f;
inline constexpr float kVirtualRough = 0.6f;
inline constexpr float kVirtualFull = 0.3f;
inline constexpr uint32_t kMaxIterations = 3;   // steps 1, 2 and 4

enum class Profile : uint32_t { kSpecular = 0, kDiffuse = 1 };

constexpr float DepthSigma(Profile p) { return p == Profile::kSpecular ? 0.04f : 0.06f; }
constexpr float NormalPower(Profile p) { return p == Profile::kSpecular ? 24.0f : 12.0f; }
constexpr float RoughSigma(Profile p) { return p == Profile::kSpecular ? 0.10f : 0.25f; }
constexpr float LumaSigma(Profile p) { return p == Profile::kSpecular ? 3.0f : 4.0f; }
constexpr float HitSigma(Profile p) { return p == Profile::kSpecular ? 1.0f : 0.0f; }

constexpr float Luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
constexpr float Saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
constexpr float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------------------
// the kernel's radius
// ---------------------------------------------------------------------------

constexpr float LengthScale(float history_length) { return Lerp(1.0f, kMatureScale, Saturate((history_length - 1.0f) / (kMatureLength - 1.0f))); }
inline float LobePixels(float cone_tan, float hit, float z, float pixel_angle) {
  if (!(hit > 0.0f) || !(z > 0.0f) || !(pixel_angle > 0.0f)) {
    return 0.0f;
  }
  return cone_tan * hit / ((z + hit) * pixel_angle);
}
inline float Radius(Profile profile, float cone_tan, float hit, float z, float pixel_angle, float history_length) {
  const float base = profile == Profile::kSpecular ? kLobeScale * LobePixels(cone_tan, hit, z, pixel_angle) : kDiffuseRadius;
  return std::min(base * LengthScale(history_length), kMaxRadius);
}
constexpr bool StepTaken(float radius, uint32_t step) { return radius >= kStepReach * float(step); }
constexpr float Mix(float share) { return share < kMinShare ? 0.0f : Saturate(share); }
// The iterations a radius takes (steps 1, 2, 4 in turn: the ones from the first whose step it reaches).
constexpr uint32_t IterationsTaken(float radius) {
  uint32_t n = 0;
  for (uint32_t step = 1; step <= 4; step <<= 1) {
    if (!StepTaken(radius, step)) break;
    ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// the edge-stopping weights
// ---------------------------------------------------------------------------

inline float DepthWeight(float plane_distance, float z, float sigma) {
  const float d = plane_distance / std::max(sigma * z, 1.0e-4f);
  return 1.0f / (1.0f + d * d);
}
inline float NormalWeight(float n_dot, float power) { return std::pow(Saturate(n_dot), power); }
inline float RoughWeight(float rough_centre, float rough_tap, float sigma) {
  const float d = (rough_centre - rough_tap) / std::max(sigma, 1.0e-4f);
  return 1.0f / (1.0f + d * d);
}
inline float LumaWeight(float luma_centre, float luma_tap, float variance, float sigma) {
  const float width = sigma * std::sqrt(std::max(variance, 0.0f)) + 1.0e-3f;
  return std::exp2(-1.442695f * std::fabs(luma_centre - luma_tap) / width);
}
inline float HitWeight(float hit_centre, float hit_tap, float sigma) {
  if (sigma <= 0.0f) {
    return 1.0f;
  }
  if (!(hit_centre > 0.0f) || !(hit_tap > 0.0f)) {
    return (hit_centre > 0.0f) == (hit_tap > 0.0f) ? 1.0f : 0.0f;
  }
  const float d = std::log2((hit_tap + 1.0f) / (hit_centre + 1.0f)) / sigma;
  return 1.0f / (1.0f + d * d);
}
constexpr float Kernel1(int d) {
  const int a = d < 0 ? -d : d;
  return a == 0 ? 0.375f : (a == 1 ? 0.25f : (a == 2 ? 0.0625f : 0.0f));
}
constexpr float Variance(float sum, float sum2, float count) {
  const float n = count > 1.0f ? count : 1.0f;
  const float mean = sum / n;
  const float v = sum2 / n - mean * mean;
  return v > 0.0f ? v : 0.0f;
}

// ---------------------------------------------------------------------------
// the temporal stage
// ---------------------------------------------------------------------------

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};
constexpr float VirtualShare(float rough, float hit) {
  if (!(hit > 0.0f)) {
    return 0.0f;
  }
  return 1.0f - Saturate((rough - kVirtualFull) / (kVirtualRough - kVirtualFull));
}
inline Vec3 ReprojectionPoint(Vec3 receiver, float hit, float rough) {
  const float d = std::sqrt(std::max(receiver.x * receiver.x + receiver.y * receiver.y + receiver.z * receiver.z, 1.0e-12f));
  const float t = VirtualShare(rough, hit);
  const float scale = (d + hit) / d;
  return {receiver.x + (receiver.x * scale - receiver.x) * t, receiver.y + (receiver.y * scale - receiver.y) * t, receiver.z + (receiver.z * scale - receiver.z) * t};
}
inline bool SameSurface(float stored_z, float expected_z) { return std::fabs(stored_z - expected_z) <= 0.08f * expected_z + 0.05f; }
constexpr float HistoryWeight(float history_length) {
  const float n = history_length < 0.0f ? 0.0f : (history_length > kMaxLength ? kMaxLength : history_length);
  return n / (n + 1.0f);
}
constexpr float NextLength(float history_length, bool history_used) {
  const float n = history_length < 0.0f ? 0.0f : (history_length > kMaxLength ? kMaxLength : history_length);
  return history_used ? (n + 1.0f < kMaxLength ? n + 1.0f : kMaxLength) : 1.0f;
}
constexpr float BoxLo(float mean, float sigma, float lo, float hi) {
  const float a = mean - kClipGamma * sigma;
  const float b = lo - kClipMargin * (hi - lo);
  return a > b ? a : b;
}
constexpr float BoxHi(float mean, float sigma, float lo, float hi) {
  const float a = mean + kClipGamma * sigma;
  const float b = hi + kClipMargin * (hi - lo);
  return a < b ? a : b;
}
inline Vec3 ClipToBox(Vec3 value, Vec3 box_lo, Vec3 box_hi) {
  const Vec3 centre{0.5f * (box_lo.x + box_hi.x), 0.5f * (box_lo.y + box_hi.y), 0.5f * (box_lo.z + box_hi.z)};
  const Vec3 half{std::max(0.5f * (box_hi.x - box_lo.x), 1.0e-3f), std::max(0.5f * (box_hi.y - box_lo.y), 1.0e-3f), std::max(0.5f * (box_hi.z - box_lo.z), 1.0e-3f)};
  const Vec3 v{value.x - centre.x, value.y - centre.y, value.z - centre.z};
  const float t = std::max(std::max(std::fabs(v.x) / half.x, std::fabs(v.y) / half.y), std::fabs(v.z) / half.z);
  if (!(t > 1.0f)) {
    return value;
  }
  return {centre.x + v.x * (1.0f / t), centre.y + v.y * (1.0f / t), centre.z + v.z * (1.0f / t)};
}

// ---------------------------------------------------------------------------
// the consumers' push blocks (32 words; the host class patches the words it owns through a Patch)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kPushWords = 32;
inline constexpr uint32_t kNoWord = 0xFFFFFFFFu;
// The words of a consumer's block the host class writes: the signal's slot (the low 16 bits of `src_word`: the iteration's input), the iteration (the low 4 bits of `iteration_word`), and for the
// temporal pass the history's slot (the low 16 bits of `history_word`) and the flag word and bit that say the frame has a history.
struct Patch {
  uint32_t src_word = kNoWord;
  uint32_t iteration_word = kNoWord;
  uint32_t history_word = kNoWord;
  uint32_t flags_word = kNoWord;
  uint32_t has_history_bit = 0;
};

// The PLAIN consumer (hlsl/rt_denoise_plain_consumer.hlsli RTDNP_W_*): the guides as plain images at the signal's size.
namespace plain {
inline constexpr uint32_t kSignal = 0;     // signal | depth << 16
inline constexpr uint32_t kGuides = 1;     // normal_rough | hit_share << 16
inline constexpr uint32_t kHistory = 2;    // history | history_depth << 16
inline constexpr uint32_t kSize = 3;       // width | height << 16
inline constexpr uint32_t kParams = 4;     // iteration (bits 0-3) | has_history << 8
inline constexpr uint32_t kTanX = 5;
inline constexpr uint32_t kTanY = 6;
inline constexpr uint32_t kPrevTanX = 7;
inline constexpr uint32_t kPrevTanY = 8;
inline constexpr uint32_t kMotion = 9;     // 12 floats
inline constexpr uint32_t kLength = 21;    // float: the history length the spatial pass assumes
inline constexpr uint32_t kHasHistoryBit = 8;
constexpr Patch PatchFor() { return Patch{kSignal, kParams, kHistory, kParams, kHasHistoryBit}; }
}  // namespace plain

// The compute layout every consumer shares (the same as the screen-space reflections' own compute layout, ssr_rt.cpp): the bindless texture set four times (sets 0 to 3), the samplers (set 4), this
// pass's output set (set 5: u0 the result image) and the push block at (b0, space 4).
inline constexpr uint32_t kTextureSets = 4;
inline constexpr uint32_t kSamplerSet = 4;
inline constexpr uint32_t kOutputSet = 5;
inline constexpr uint32_t kPushSpace = 4;
inline constexpr uint32_t kThreads = 8;     // numthreads(8, 8, 1)
constexpr uint32_t Groups(uint32_t extent) { return (extent + kThreads - 1) / kThreads; }

// The result images' format: RGBA16F (the radiance in rgb, the signal's alpha or the history's length in a).
inline constexpr uint32_t kBytesPerPixel = 8;
constexpr uint64_t ImageBytes(uint32_t width, uint32_t height) { return uint64_t(width) * height * kBytesPerPixel; }

}  // namespace mcla::gpu::rt_denoise

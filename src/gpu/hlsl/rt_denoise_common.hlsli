// MCLA GPU renderer -- RtDenoise (CITY-RT package R6 of perf/CITY-RT-M2.md; src/gpu/rt_denoise_rules.h is the C++ twin and the design, src/gpu/rt_denoise.h the host): the pure functions
// the passes share, for the ray traced effects that are noisy by design (the screen-space reflections' ray traced fallback now, the probe field's per-pixel detail pass, CITY-RT M3 G5, next).
//
// WHAT IT IS. A filter for a signal of ONE noisy sample a pixel with a hit distance: the radiance in rgb, how far the ray went in the hit distance. Two stages, each a compute pass over the signal's own
// pixels, each reading its guides (depth, normal, roughness, the hit distance) through a CONSUMER (the include that defines RtDnLoad and the pass's other hooks: hlsl/ssr_denoise_consumer.hlsli for
// the reflections' fallback, hlsl/rt_denoise_plain_consumer.hlsli for guides that are plain images):
//   TEMPORAL  reprojects the last frame's result with the reflection's VIRTUAL motion (the mirror image of the hit stands `hit` behind the surface along the view ray, so that is the point that is
//             moved by the camera's motion, not the surface: the reflection of a wall does not slide with the road), takes it only where the surface is the same one, holds it to the
//             neighbourhood's variance box (the variance clip: a reflection that changed does not leave a ghost), and blends it with a weight that grows with its history's length.
//   SPATIAL   a bilateral a-trous on the pixels the signal's share says are the noisy ones: five by five taps of a B3 spline at a step of 1, 2 or 4 pixels, edge-stopped by the depth's plane distance, the
//             normal, the roughness, the luminance (against the neighbourhood's own variance) and the hit distance, and with a RADIUS that follows the reflection: the width of the specular lobe
//             at the hit seen from the receiver (a rough surface and a far hit blur wide, a near mirror hardly), shrunk as the history grows. A pixel whose radius is under the step passes the
//             pass through, so the iterations (steps 1, 2, 4) each take the pixels that ask for them.
// The consumer decides everything that touches its own images and push block; nothing here reads a resource. Both languages read the same subset (src/gpu/rt_denoise_test.cpp compiles THIS FILE as C++
// through a shim of the HLSL types and compares every function with the twin): scalar arithmetic, no out parameters.

#ifndef MCLA_GPU_RT_DENOISE_COMMON_HLSLI
#define MCLA_GPU_RT_DENOISE_COMMON_HLSLI

// ---- the profiles ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#define RT_DN_PROFILE_SPECULAR 0u   // a reflection: the kernel follows the lobe at the hit, the hit distance is an edge
#define RT_DN_PROFILE_DIFFUSE 1u    // a diffuse term (the probe field's detail pass): a fixed radius, no hit distance
#define RT_DN_PROFILE_AMBIENT 2u    // ambient occlusion: a scalar at the receiving surface

static const float kRtDnMaxRadius = 12.0f;        // the widest kernel, in pixels of the signal (steps 1, 2 and 4 reach 2 + 4 + 8 = 14)
static const float kRtDnLobeScale = 1.0f;         // the specular kernel's radius over the lobe's half width at the receiver
static const float kRtDnDiffuseRadius = 6.0f;     // the diffuse profile's radius before the history shortens it
static const float kRtDnAmbientRadius = 5.0f;
static const float kRtDnMatureLength = 12.0f;     // a history this long shrinks the kernel to kRtDnMatureScale
static const float kRtDnMatureScale = 0.4f;
static const float kRtDnStepReach = 0.75f;        // a pixel takes the iteration of step s when its radius is at least this many s
static const float kRtDnMinShare = 0.05f;         // a pixel whose signal share is under this passes through every iteration
static const float kRtDnClipGamma = 1.25f;        // the history is held to the neighbourhood's mean +- this many standard deviations
static const float kRtDnClipMargin = 0.1f;        // ... and to its min / max widened by this share of their range
static const float kRtDnMaxLength = 32.0f;        // the history's length is capped here (the blend's floor is then 1 / 33)
static const float kRtDnVirtualRough = 0.6f;      // above this perceptual roughness the virtual image is blurred past use: the surface's own motion
static const float kRtDnVirtualFull = 0.3f;       // under this it is used whole; between the two the two motions are blended

// ---- the weights' numbers by profile ---------------------------------------------------------------------------------------------------------------------------------------------------------
#ifdef MCLA_RT_DENOISE_AO_CONSUMER_HLSLI
float RtDnDepthSigma(uint profile) { return profile == RT_DN_PROFILE_AMBIENT ? 0.03f : (profile == RT_DN_PROFILE_SPECULAR ? 0.04f : 0.06f); }
float RtDnNormalPower(uint profile) { return profile == RT_DN_PROFILE_AMBIENT ? 16.0f : (profile == RT_DN_PROFILE_SPECULAR ? 24.0f : 12.0f); }
#else
float RtDnDepthSigma(uint profile) { return profile == RT_DN_PROFILE_SPECULAR ? 0.04f : 0.06f; }   // the plane distance's width, over the receiver's depth
float RtDnNormalPower(uint profile) { return profile == RT_DN_PROFILE_SPECULAR ? 24.0f : 12.0f; }
#endif
float RtDnRoughSigma(uint profile) { return profile == RT_DN_PROFILE_SPECULAR ? 0.10f : 0.25f; }
float RtDnLumaSigma(uint profile) { return profile == RT_DN_PROFILE_SPECULAR ? 3.0f : 4.0f; }    // the luminance's width over the neighbourhood's own deviation
float RtDnHitSigma(uint profile) { return profile == RT_DN_PROFILE_SPECULAR ? 1.0f : 0.0f; }     // in octaves of the hit distance; 0 = the hit distance is not an edge

float RtDnLuma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// ---- numbers only ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A float that is a number (neither NaN nor infinite), by the bits: a compiler may fold x == x, and what min / max / lerp / clamp make of a NaN is the API's to say (D3D12 returns the other operand of a
// max, Vulkan's FMax leaves it open), so nothing in the passes leans on it. A NaN in the signal would be spread by the spatial filter (a weight times a NaN is a NaN) and kept by the history for good (NaN
// times a weight of 0 is NaN); infinity is no better (infinity times 0). A signal or a guide or a history that is not a number is dealt with where it is read (rt_denoise_engine.hlsli).
bool RtDnIsNumber(float v) { return (asuint(v) & 0x7F800000u) != 0x7F800000u; }
bool RtDnIsNumber3(float3 v) { return RtDnIsNumber(v.x) && RtDnIsNumber(v.y) && RtDnIsNumber(v.z); }
// A colour that is not a number is none: black.
float3 RtDnSane(float3 c) { return RtDnIsNumber3(c) ? c : float3(0.0f, 0.0f, 0.0f); }

// ---- the kernel's radius -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// How much a long history shrinks the kernel: 1 for a fresh pixel (length 1), kRtDnMatureScale from kRtDnMatureLength on.
float RtDnLengthScale(float history_length) {
  return lerp(1.0f, kRtDnMatureScale, saturate((history_length - 1.0f) / (kRtDnMatureLength - 1.0f)));
}
// The specular lobe's half width at the receiver, in pixels: a lobe of half angle `cone_tan` has the width cone_tan * hit at the hit, and the virtual image that width belongs to stands z + hit from
// the eye against the receiver's z, so the receiver's neighbours see it scaled by z / (z + hit); a pixel at depth z is z * pixel_angle across. Zero where there is no hit (nothing to follow).
float RtDnLobePixels(float cone_tan, float hit, float z, float pixel_angle) {
  if (!(hit > 0.0f) || !(z > 0.0f) || !(pixel_angle > 0.0f)) {
    return 0.0f;
  }
  return cone_tan * hit / ((z + hit) * pixel_angle);
}
// The kernel's radius in pixels of the signal: the profile's, shortened by the history, at most kRtDnMaxRadius.
float RtDnRadius(uint profile, float cone_tan, float hit, float z, float pixel_angle, float history_length) {
#ifdef MCLA_RT_DENOISE_AO_CONSUMER_HLSLI
  const float base = profile == RT_DN_PROFILE_SPECULAR ? kRtDnLobeScale * RtDnLobePixels(cone_tan, hit, z, pixel_angle)
                     : (profile == RT_DN_PROFILE_AMBIENT ? kRtDnAmbientRadius : kRtDnDiffuseRadius);
#else
  const float base = profile == RT_DN_PROFILE_SPECULAR ? kRtDnLobeScale * RtDnLobePixels(cone_tan, hit, z, pixel_angle) : kRtDnDiffuseRadius;
#endif
  return min(base * RtDnLengthScale(history_length), kRtDnMaxRadius);
}
// Whether a pixel of this radius takes the iteration of step `step` (1, 2, 4): its radius reaches 0.75 of the step. A radius under 0.75 takes none: that pixel is left as it is.
bool RtDnStepTaken(float radius, uint step) {
  return radius >= kRtDnStepReach * float(step);
}
#ifdef MCLA_RT_DENOISE_AO_CONSUMER_HLSLI
bool RtDnAmbientStep(float radius, uint step, float length, bool resolve, bool vehicle) {
  return (step == 1u || length < kRtDnMatureLength || (step == 2u && resolve && !vehicle)) && RtDnStepTaken(radius, step);
}
#endif
// How much of the filtered value a pixel takes: its signal share, 0 under kRtDnMinShare (a pixel the signal hardly made is not touched).
float RtDnMix(float share) {
  return share < kRtDnMinShare ? 0.0f : saturate(share);
}

// ---- the edge-stopping weights (each 0..1, 1 for the same surface and the same reflection) ---------------------------------------------------------------------------------------------
// The tap's distance from the centre's tangent plane (metres) against a width of `sigma` receiver depths.
float RtDnDepthWeight(float plane_distance, float z, float sigma) {
  const float d = plane_distance / max(sigma * z, 1.0e-4f);
  return 1.0f / (1.0f + d * d);
}
float RtDnNormalWeight(float n_dot, float power) {
  return pow(saturate(n_dot), power);
}
float RtDnRoughWeight(float rough_centre, float rough_tap, float sigma) {
  const float d = (rough_centre - rough_tap) / max(sigma, 1.0e-4f);
  return 1.0f / (1.0f + d * d);
}
// The luminance's difference against `sigma` standard deviations of the neighbourhood (a floor keeps a flat neighbourhood from stopping everything).
float RtDnLumaWeight(float luma_centre, float luma_tap, float variance, float sigma) {
  const float width = sigma * sqrt(max(variance, 0.0f)) + 1.0e-3f;
  return exp2(-1.442695f * abs(luma_centre - luma_tap) / width);
}
// Two reflections whose hits are far apart in distance are two objects: the ratio in octaves against `sigma` (0: not an edge). A distance of 0 (no hit) weighs against any other.
float RtDnHitWeight(float hit_centre, float hit_tap, float sigma) {
  if (sigma <= 0.0f) {
    return 1.0f;
  }
  if (!(hit_centre > 0.0f) || !(hit_tap > 0.0f)) {
    return (hit_centre > 0.0f) == (hit_tap > 0.0f) ? 1.0f : 0.0f;
  }
  const float d = log2((hit_tap + 1.0f) / (hit_centre + 1.0f)) / sigma;
  return 1.0f / (1.0f + d * d);
}
// The B3 spline's weight of the tap at offset `d` (-2 .. 2) on one axis: 1 4 6 4 1 over 16.
float RtDnKernel1(int d) {
  const int a = d < 0 ? -d : d;
  return a == 0 ? 0.375f : (a == 1 ? 0.25f : (a == 2 ? 0.0625f : 0.0f));
}
// The neighbourhood's luminance variance from its sum, sum of squares and count.
float RtDnVariance(float sum, float sum2, float count) {
  const float n = max(count, 1.0f);
  const float mean = sum / n;
  return max(sum2 / n - mean * mean, 0.0f);
}

// ---- the temporal stage --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The point the reflection was at in the last frame's view space, given where it is now: the receiver blended with its virtual image (the hit `hit` behind it along the view ray, as
// ssr_common.hlsli SsrVirtualPoint), by how mirror-like the surface is: whole under kRtDnVirtualFull of perceptual roughness, none over kRtDnVirtualRough. `t` in 0..1 is the virtual image's share.
float RtDnVirtualShare(float rough, float hit) {
  if (!(hit > 0.0f)) {
    return 0.0f;
  }
  return 1.0f - saturate((rough - kRtDnVirtualFull) / (kRtDnVirtualRough - kRtDnVirtualFull));
}
float3 RtDnReprojectionPoint(float3 receiver, float hit, float rough) {
  const float d = sqrt(max(dot(receiver, receiver), 1.0e-12f));
  const float t = RtDnVirtualShare(rough, hit);
  const float3 virtual_point = receiver * ((d + hit) / d);
  return receiver + (virtual_point - receiver) * t;
}
// Whether a history texel is still the same surface: its stored receiver depth against the one this frame's receiver has in the last frame's view space (the reflections' own rule, SsrSameSurface).
bool RtDnSameSurface(float stored_z, float expected_z) {
  return abs(stored_z - expected_z) <= 0.08f * expected_z + 0.05f;
}
// The weight the history keeps: n / (n + 1) of a history of n frames (the plain running mean), n at most kRtDnMaxLength; 0 for a pixel with none.
float RtDnHistoryWeight(float history_length) {
  const float n = clamp(history_length, 0.0f, kRtDnMaxLength);
  return n / (n + 1.0f);
}
// The length the result's history has: one more than the last (the length that was blended) up to the cap, 1 where there was none.
float RtDnNextLength(float history_length, bool history_used) {
  return history_used ? min(clamp(history_length, 0.0f, kRtDnMaxLength) + 1.0f, kRtDnMaxLength) : 1.0f;
}
// The variance clip's box, per channel: the neighbourhood's mean +- kRtDnClipGamma deviations, no wider than its min / max widened by kRtDnClipMargin of their range; and the history brought to
// it along the line to the box's centre (the clip keeps the colour, a clamp turns it). The same maths as ssr_common.hlsli SsrBoxLoChannel / SsrBoxHiChannel / SsrClipToBox.
float RtDnBoxLo(float mean, float sigma, float lo, float hi) {
  return max(mean - kRtDnClipGamma * sigma, lo - kRtDnClipMargin * (hi - lo));
}
float RtDnBoxHi(float mean, float sigma, float lo, float hi) {
  return min(mean + kRtDnClipGamma * sigma, hi + kRtDnClipMargin * (hi - lo));
}
float3 RtDnClipToBox(float3 value, float3 box_lo, float3 box_hi) {
  const float3 centre = float3(0.5f * (box_lo.x + box_hi.x), 0.5f * (box_lo.y + box_hi.y), 0.5f * (box_lo.z + box_hi.z));
  const float3 half_size = float3(max(0.5f * (box_hi.x - box_lo.x), 1.0e-3f), max(0.5f * (box_hi.y - box_lo.y), 1.0e-3f), max(0.5f * (box_hi.z - box_lo.z), 1.0e-3f));
  const float3 v = value - centre;
  const float t = max(max(abs(v.x) / half_size.x, abs(v.y) / half_size.y), abs(v.z) / half_size.z);
  return t > 1.0f ? centre + v * (1.0f / t) : value;
}


// ---- what a consumer hands the passes ----------------------------------------------------------------------------------------------------------------------------------------------------
// One pixel of the signal with its guides, as the consumer's RtDnLoad fills it. `ok` false: the pixel is not part of the signal (nothing was traced there): it is passed through and never a tap.
struct RtDnPixel {
  bool ok;
  float3 colour;   // the signal's radiance (rgb; may be negative: a road's term is a difference)
  float z;         // linear depth along the view axis, metres
  float3 pos;      // the view-space position
  float3 n;        // the view-space normal, facing the camera
  float rough;     // perceptual roughness 0..1 (the GGX alpha is its square)
  float hit;       // the ray's length from the surface, metres (0: none)
  float share;     // how much of the pixel's value the filter may replace, 0..1
  float length;    // the history's length in frames (1: fresh)
#ifdef MCLA_RT_DENOISE_AO_CONSUMER_HLSLI
  uint vehicle;    // 0 for world pixels, 1 + stencil ID for vehicles
#endif
};
// Whether a pixel is a usable part of the signal: `ok`, and its guides are numbers (a pixel whose guides are not is no signal: passed through, never a tap). The colour is looked at apart (RtDnSane, RtDnIsNumber3):
// a colour that is not a number is black at the centre and a tap that is left out.
bool RtDnGuidesOk(RtDnPixel p) {
  return p.ok && RtDnIsNumber(p.z) && RtDnIsNumber3(p.pos) && RtDnIsNumber3(p.n) && RtDnIsNumber(p.rough) && RtDnIsNumber(p.hit) && RtDnIsNumber(p.share) && RtDnIsNumber(p.length);
}
// A point of this frame's view space in the last frame's (the consumer's RtDnToPrevious): `ok` when it is in front of the camera and on the picture.
struct RtDnPrevious {
  bool ok;
  float u;
  float v;
  float z;
};

#endif  // MCLA_GPU_RT_DENOISE_COMMON_HLSLI

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city's ray traced sun shadows: the rules and the maths that need no renderer
// ===========================================================================
// city_rt_shadow.h records one compute pass (hlsl/city_rt_shadow_cs.hlsl) at the history trigger: for every pixel of the occlusion's linear depth image a few
// rays go from the pixel's surface toward the sun, spread over the sun's disc, against the frame's top level structure (the city's sectors and the cars,
// city_rt_scene.h), and the share that arrives is the pixel's sun visibility. The picture is the g channel of the image car_rt_ao writes, so the contact
// march (ao.cpp, hlsl/ao_contact_ps.hlsl THE RAY TRACED TERM) takes it as it takes the car's: the darker of its shadow and the traced one, on the sun's share of
// the light only.
//
// WHY THE LOOK IS RIGHT (the owner's brief: sharp near the contact, soft far from it, no cascade seams, no acne). The shadow of a caster at distance d from its
// receiver has a penumbra as wide as d times the sun's angular diameter. Rays spread over the real disc give exactly that, with no filter radius to tune: a pole's
// foot is sharp and its head's shadow is soft (contact hardening); and the shadow comes from the city's triangles, not from four cascades of a texture, so there are
// no seams and no cascade's bias. Acne (a surface shadowed by its own triangles) is handled by lifting the ray's origin off the surface (Lift) by what the depth
// image can be wrong by, and by the host keeping the shadow to where the structures are the hi detail the game drew (city_rt_rules.h SafeReceiverRange).
//
// This header holds the numbers and the arithmetic the shader shares: the sun, the disc samples, the lift, the alpha rule, the noise hash, the range of a ray.
// hlsl/city_rt_shadow_cs.hlsl has the same arithmetic in HLSL; city_rt_shadow_rules_test.cpp checks the shader's constants against this header and the maths
// against a brute force tracer (the penumbra of an edge against the exact disc integral, contact hardening, no acne on a plane at every sun height).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace mcla::gpu::city_rt_shadow {

// ---------------------------------------------------------------------------
// the sun
// ---------------------------------------------------------------------------
// The tangent of the sun's angular radius: 0.2666 degrees.
inline constexpr float kSunRadiusTan = 0.004653f;
// The disc is drawn this many times its real size (mcla_gpu_city_rt_sun): the real one gives a penumbra of 1 cm at 1 m, 9 cm at 10 m and 1.9 m at 200 m, which the
// few rays of a pixel and the filter after them turn into noise at the far end. The game's own PCSS draws the sun 4 times its size (shadow_filter.h kDefaultSunTuning).
inline constexpr float kDefaultSunScale = 3.0f;
inline constexpr float kMinSunScale = 0.25f;
inline constexpr float kMaxSunScale = 16.0f;
inline float ClampSunScale(double scale) { return float(std::clamp(scale, double(kMinSunScale), double(kMaxSunScale))); }
// A surface whose cosine to the sun is below this is left to the lit shader's own N.L (as the contact march's kSunFacingLow): the pass writes "open" there.
inline constexpr float kSunFacingLow = 0.03f;
// The sine of the lowest sun elevation the pass runs at (3 degrees: the contact shadows' own gate, where the sun's share of the light goes to nothing).
inline constexpr float kMinSunSin = 0.0523f;
// The tallest thing the city can cast a shadow from, metres: a ray that rises above it has left every caster.
inline constexpr float kMaxCasterHeight = 300.0f;

// How far a ray toward the sun can meet a caster: it climbs sin(elevation) a metre, the tallest caster is kMaxCasterHeight, and the far lod of the structures ends at
// `reach` (city_rt_rules.h Budget::far_radius). At 3 degrees that is 5.7 km, so the reach decides; at 30 degrees it is 600 m.
inline float RayLength(float sun_sin, float reach) { return std::min(reach, kMaxCasterHeight / std::max(sun_sin, kMinSunSin)); }

// The width of the penumbra (the distance between where the sun is a tenth hidden and nine tenths) of a caster `blocker_distance` metres along the ray from its
// receiver, for a sun `scale` times the real one, on a surface square to the ray. A straight edge sweeping across a disc hides 10 percent of its area at 0.155 of the
// way and 90 percent at 0.845 of the diameter: the width is 0.69 of the diameter (the circular segment's area, asin(y) + y sqrt(1 - y^2), at 10 and 90 percent).
inline constexpr float kPenumbraTenToNinety = 0.69f;
inline float PenumbraWidth(float blocker_distance, float scale) { return kPenumbraTenToNinety * 2.0f * blocker_distance * kSunRadiusTan * scale; }

// ---------------------------------------------------------------------------
// the ray's origin
// ---------------------------------------------------------------------------
// The ray starts on the surface the depth image shows, lifted along its normal: by a few millimetres, and by three pixel footprints (what the depth, which the
// normal is made from, can be wrong by across a pixel), at most kLiftCap (a ray lifted more would pass over a kerb). `footprint_unit` is 2 tan(half fov y) / the
// image's height: a pixel at distance t is t * footprint_unit metres across. (car_rt_ao_cs.hlsl's rule, with the cap.)
inline constexpr float kLiftFloor = 0.006f;
inline constexpr float kLiftPixels = 3.0f;
inline constexpr float kLiftCap = 0.4f;
inline float Lift(float t, float footprint_unit) { return std::min(kLiftFloor + kLiftPixels * footprint_unit * t, kLiftCap); }

// ---------------------------------------------------------------------------
// the disc samples
// ---------------------------------------------------------------------------
inline constexpr float kTwoPi = 6.283185307179586f;
inline constexpr float kGolden = 0.6180339887f;

inline float Frac(float x) { return x - std::floor(x); }

// The k-th of `rays` samples over the unit disc: the radius' square stratified (sample k takes the stratum k of `rays` of the disc's area, so the rays of one pixel
// cover the disc evenly), the angle turned by the golden angle a ray, and the whole pattern shifted by the pixel's two noise numbers in [0, 1) (so neighbours and
// frames take other places). Written to out[0], out[1] (x and y of the unit disc).
inline void DiscSample(uint32_t k, uint32_t rays, float noise1, float noise2, float out[2]) {
  const float u = (float(k) + std::min(noise1, 0.9999f)) / float(rays);
  const float r = std::sqrt(u);
  const float angle = kTwoPi * Frac(noise2 + kGolden * float(k));
  out[0] = r * std::cos(angle);
  out[1] = r * std::sin(angle);
}

// The rays a pixel casts: high 2, ultra 4. (High traces at half size above 1080 lines of render height with the occlusion's image, so 2 rays are 1 per output
// pixel of the picture; ultra 4.) mcla_gpu_city_rt_rays overrides (0 = this).
inline constexpr uint32_t kMaxRays = 8;
inline uint32_t RaysFor(bool ultra, int32_t forced) {
  if (forced > 0) return uint32_t(std::min<int32_t>(forced, int32_t(kMaxRays)));
  return ultra ? 4u : 2u;
}

// ---------------------------------------------------------------------------
// alpha tested triangles (the pack's second geometry of a sector)
// ---------------------------------------------------------------------------
// The pack keeps the triangles of cut-out shaders (fences, grates, foliage cards: 4.3% of the city's triangles) apart as a non opaque geometry. Their textures are
// not in the ray tracing structures, so a ray that meets one cannot read its cut-out. Three rules, mcla_gpu_city_rt_alpha:
//   exclude     never a blocker: leaves a fence's shadow out (the game's cascades have it)
//   opaque      always a blocker: a fence shadows as a wall
//   stochastic  a blocker with probability kAlphaCoverage, chosen by a hash of the pixel, the triangle and the frame: a fence's shadow is a half tone that the filter and
//               the upscaler average (the default: a true cut-out is somewhere between, and the cost is nothing)
enum class AlphaMode : uint32_t { kExclude = 0, kOpaque = 1, kStochastic = 2 };
inline constexpr float kAlphaCoverage = 0.5f;
inline AlphaMode ParseAlphaMode(std::string_view text) {
  if (text == "exclude") return AlphaMode::kExclude;
  if (text == "opaque") return AlphaMode::kOpaque;
  return AlphaMode::kStochastic;
}

// The noise: a PCG hash of the pixel, the triangle and the frame, as a number in [0, 1).
inline uint32_t Pcg(uint32_t v) {
  const uint32_t state = v * 747796405u + 2891336453u;
  const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
inline float Hash01(uint32_t x, uint32_t y, uint32_t primitive, uint32_t frame) {
  return float(Pcg(x ^ Pcg(y ^ Pcg(primitive ^ Pcg(frame)))) >> 8) * (1.0f / 16777216.0f);
}
inline bool AcceptAlpha(AlphaMode mode, float hash01) {
  switch (mode) {
    case AlphaMode::kExclude: return false;
    case AlphaMode::kOpaque: return true;
    default: return hash01 < kAlphaCoverage;
  }
}

// ---------------------------------------------------------------------------
// the push block's flag bits (hlsl/city_rt_shadow_cs.hlsl CityRtShadowPush.slots.w)
// ---------------------------------------------------------------------------
inline constexpr uint32_t kFlagRaysShift = 0;        // bits 0-3: the rays a pixel (1 to 8)
inline constexpr uint32_t kFlagAlphaShift = 4;       // bits 4-5: AlphaMode
inline constexpr uint32_t kFlagCarImage = 1u << 8;   // the car ray traced image's slot is in noise.w: the result is the darker of the two
inline constexpr uint32_t kFlagCheck = 1u << 30;     // the check: primary rays against the depth image, statistics into the counters (mcla_gpu_city_rt_check)
inline constexpr uint32_t kFlagView = 1u << 29;      // the traced value as the picture's g, b = the sector's colour code of the hit (debug)
inline constexpr uint32_t PackFlags(uint32_t rays, AlphaMode alpha, bool car_image) {
  return ((rays & 0xFu) << kFlagRaysShift) | (static_cast<uint32_t>(alpha) << kFlagAlphaShift) | (car_image ? kFlagCarImage : 0u);
}

// The check's statistics words (the counters buffer, uint32 each; the shader's atomics).
inline constexpr uint32_t kStatPixels = 0;     // pixels looked at (a surface of the city in the depth image, not a vehicle, not the sky)
inline constexpr uint32_t kStatHit = 1;        // the primary ray found a city triangle
inline constexpr uint32_t kStatWithin1 = 2;    // ... within 1 percent of the depth image's distance
inline constexpr uint32_t kStatWithin5 = 3;    // ... within 5 percent
inline constexpr uint32_t kStatNoHit = 4;      // no triangle on the ray
inline constexpr uint32_t kStatLod = 5;        // hits on a lod structure
inline constexpr uint32_t kStatHi = 6;         // ... on a hi detail one
inline constexpr uint32_t kStatHistogram = 8;  // 16 bins of the relative error of a hit (|t_hit - t| / t) in steps of 0.5 percent (the last bin: 7.5 percent and more)
inline constexpr uint32_t kStatWords = 24;
inline constexpr uint32_t kHistogramBins = 16;
inline constexpr float kHistogramStep = 0.005f;
inline constexpr float kCheckTMin = 0.01f;

}  // namespace mcla::gpu::city_rt_shadow

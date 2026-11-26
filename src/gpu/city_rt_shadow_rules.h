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

#include "traffic_shadow.h"

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

// Preserve a transition at the end of the resident range. When that range allows
// it, keep the traced visibility unfaded throughout the raster All caster ring.
inline constexpr float kCoverageFadeWidth = 20.0f;
inline float CoverageFadeStart(float range) {
  return std::max(0.5f * range, std::min(traffic_shadow::kAllRadius, range - kCoverageFadeWidth));
}

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

// Even if every remaining ray is blocked, this many open rays already
// leave the city visibility at least as high as the car carrier.
constexpr bool CarrierWins(uint32_t open, uint32_t rays, float car_visibility) {
  return float(open) / float(rays) >= car_visibility;
}

// ---------------------------------------------------------------------------
// alpha tested triangles (the pack's second geometry of a sector)
// ---------------------------------------------------------------------------
// The pack keeps the triangles of cut-out shaders (fences, grates, foliage cards: 4.3% of the city's triangles) apart as a non opaque geometry. A ray that meets one needs its cut-out.
// Four rules, mcla_gpu_city_rt_alpha:
//   exclude     never a blocker: leaves a fence's shadow out (the game's cascades have it)
//   opaque      always a blocker: a fence shadows as a wall
//   stochastic  a blocker with probability kAlphaCoverage, chosen by a hash of the pixel, the triangle and the frame: a fence's shadow is a half tone that the filter and
//               the upscaler average (what the structures did without hit data: a true cut-out is somewhere between, and the cost is nothing)
//   exact       (CITY-RT M1c, the default) the candidate's own alpha: its texture coordinates from the hit data's corner streams, its texture's alpha (hlsl/city_hit_alpha.hlsli,
//               city_hit_alpha_rules.h) against the reference (kDefaultAlphaReference, mcla_gpu_city_rt_alpha_ref). Where the alpha cannot be read (the hit data is not there, the
//               texture is not resident yet) the candidate follows the stochastic rule: AcceptCandidate. The shadow pass asks the scene service for the hit data and the cut-out
//               textures (rt_scene_rules.h Needs::textures) only in this mode.
enum class AlphaMode : uint32_t { kExclude = 0, kOpaque = 1, kStochastic = 2, kExact = 3 };
inline constexpr float kAlphaCoverage = 0.5f;
// The alpha at or above which a cut-out texel blocks a ray (the game's alpha test, RB_ALPHA_REF: the pack does not know the draw's own; 0.5 is where the cut-outs' edge is authored).
inline constexpr float kDefaultAlphaReference = 0.5f;
inline constexpr float kMinAlphaReference = 0.02f;
inline constexpr float kMaxAlphaReference = 0.98f;
inline float ClampAlphaReference(double reference) { return float(std::clamp(reference, double(kMinAlphaReference), double(kMaxAlphaReference))); }
inline AlphaMode ParseAlphaMode(std::string_view text) {
  if (text == "exclude") return AlphaMode::kExclude;
  if (text == "opaque") return AlphaMode::kOpaque;
  if (text == "exact") return AlphaMode::kExact;
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
    default: return hash01 < kAlphaCoverage;  // stochastic, and exact where its alpha could not be read (AcceptCandidate)
  }
}
// What the shader does with a cut-out candidate: in the exact mode, the alpha it read when it could read it (`known`), else the stochastic rule (today's behaviour); the other modes
// as they were. `exact_alpha` is the alpha of the candidate's texel, `reference` the push's.
inline bool AcceptCandidate(AlphaMode mode, bool known, float exact_alpha, float reference, float hash01) {
  if (mode == AlphaMode::kExact && known) return exact_alpha >= reference;
  return AcceptAlpha(mode, hash01);
}

// ---------------------------------------------------------------------------
// the push block's flag bits (hlsl/city_rt_shadow_cs.hlsl CityRtShadowPush.slots.w)
// ---------------------------------------------------------------------------
inline constexpr uint32_t kFlagRaysShift = 0;        // bits 0-3: the rays a pixel (1 to 8)
inline constexpr uint32_t kFlagAlphaShift = 4;       // bits 4-5: AlphaMode
inline constexpr uint32_t kFlagVehicleReceivers = 1u << 9;
inline constexpr uint32_t kFlagSelective = 1u << 11;
inline constexpr uint32_t kFlagBounds = 1u << 10;
inline constexpr uint32_t kFlagCarImage = 1u << 8;   // the car ray traced image's slot is in noise.w: the result is the darker of the two
inline constexpr uint32_t kFlagAlphaRefShift = 12;   // bits 12-19: the alpha reference of the exact mode, unorm8
inline constexpr uint32_t kFlagAlphaStats = 1u << 28;  // the exact alpha's statistics into the counters, and its picture as the g channel (mcla_gpu_city_rt_alpha_view)
inline constexpr uint32_t kFlagCheck = 1u << 30;     // the check: primary rays against the depth image, statistics into the counters (mcla_gpu_city_rt_check)
inline constexpr uint32_t kFlagView = 1u << 29;      // the traced value as the picture's g, b = the sector's colour code of the hit (debug)
// The reference as the push carries it (unorm8) and back.
inline uint32_t PackAlphaReference(float reference) { return uint32_t(std::lround(std::clamp(reference, 0.0f, 1.0f) * 255.0f)); }
inline float UnpackAlphaReference(uint32_t flags) { return float((flags >> kFlagAlphaRefShift) & 0xFFu) * (1.0f / 255.0f); }
inline uint32_t PackFlags(uint32_t rays, AlphaMode alpha, bool car_image, float alpha_reference = kDefaultAlphaReference, bool alpha_stats = false) {
  return ((rays & 0xFu) << kFlagRaysShift) | (static_cast<uint32_t>(alpha) << kFlagAlphaShift) | (car_image ? kFlagCarImage : 0u) | (PackAlphaReference(alpha_reference) << kFlagAlphaRefShift) |
         (alpha_stats ? kFlagAlphaStats : 0u);
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
// The exact alpha's counters (kFlagAlphaStats; the shader's atomics, one per cut-out candidate a ray met):
inline constexpr uint32_t kStatAlphaCandidates = 24;  // cut-out candidates met
inline constexpr uint32_t kStatAlphaExact = 25;       // ... whose alpha was read from the texture
inline constexpr uint32_t kStatAlphaBlocked = 26;     // ... of those, at or above the reference: the ray was blocked
inline constexpr uint32_t kStatAlphaPassed = 27;      // ... below it: the ray went through a hole
inline constexpr uint32_t kStatAlphaFallback = 28;    // candidates the stochastic rule decided (no hit data, no texture, the texture not resident)
inline constexpr uint32_t kStatAlphaLevels = 32;      // 8 bins: the level read (counted from the first resident one), the last bin 7 and more
inline constexpr uint32_t kAlphaLevelBins = 8;
inline constexpr uint32_t kStatWords = 40;
inline constexpr uint32_t kHistogramBins = 16;
inline constexpr float kHistogramStep = 0.005f;
inline constexpr float kCheckTMin = 0.01f;


// The exact alpha's picture (kFlagAlphaStats): the g channel says which way the pixel's rays went through the cut-outs, instead of the visibility.
inline constexpr uint32_t kPathCandidate = 1;  // some ray of the pixel met a cut-out candidate
inline constexpr uint32_t kPathFallback = 2;   // ... and some candidate was decided by the stochastic rule
inline constexpr uint32_t kPathExact = 4;      // ... and some candidate was decided by its alpha
inline constexpr float kViewNoCandidate = 1.0f;
inline constexpr float kViewExact = 0.7f;
inline constexpr float kViewFallback = 0.35f;
inline float AlphaViewValue(uint32_t path) {
  if ((path & kPathCandidate) == 0) return kViewNoCandidate;
  return (path & kPathFallback) != 0 ? kViewFallback : kViewExact;
}

struct AlphaSummary {
  uint64_t candidates = 0, exact = 0, blocked = 0, passed = 0, fallback = 0;
  uint64_t levels[kAlphaLevelBins] = {};
  double exact_share = 0.0;    // exact / candidates
  double blocked_share = 0.0;  // blocked / exact: the cut-outs' own coverage at the reference
  double mean_level = 0.0;
};
inline AlphaSummary DecodeAlpha(const uint32_t words[kStatWords]) {
  AlphaSummary a;
  a.candidates = words[kStatAlphaCandidates];
  a.exact = words[kStatAlphaExact];
  a.blocked = words[kStatAlphaBlocked];
  a.passed = words[kStatAlphaPassed];
  a.fallback = words[kStatAlphaFallback];
  uint64_t total = 0, weighted = 0;
  for (uint32_t b = 0; b < kAlphaLevelBins; ++b) {
    a.levels[b] = words[kStatAlphaLevels + b];
    total += a.levels[b];
    weighted += a.levels[b] * b;
  }
  a.exact_share = a.candidates ? double(a.exact) / double(a.candidates) : 0.0;
  a.blocked_share = a.exact ? double(a.blocked) / double(a.exact) : 0.0;
  a.mean_level = total ? double(weighted) / double(total) : 0.0;
  return a;
}

// What the check's counters say (the readback's words, the difference between two readbacks when the buffer was not cleared).
struct CheckSummary {
  uint64_t pixels = 0;
  uint64_t hit = 0, within1 = 0, within5 = 0, no_hit = 0, lod = 0, hi = 0;
  double coverage = 0.0;      // hit / pixels
  double within1_share = 0.0; // within 1 percent / hits
  double within5_share = 0.0;
  double p50 = 0.0;           // the relative error's median and 95th percentile, as the upper edge of the histogram's bin that holds them (the last bin: 1.0)
  double p95 = 0.0;
};

inline CheckSummary DecodeCheck(const uint32_t words[kStatWords]) {
  CheckSummary c;
  c.pixels = words[kStatPixels];
  // The shader counts kStatHit for every committed hit, within1 for those within 1 percent and within5 for those within 5 (the first are among the second).
  c.hit = words[kStatHit];
  c.within1 = words[kStatWithin1];
  c.within5 = words[kStatWithin5];
  c.no_hit = words[kStatNoHit];
  c.lod = words[kStatLod];
  c.hi = words[kStatHi];
  c.coverage = c.pixels ? double(c.hit) / double(c.pixels) : 0.0;
  c.within1_share = c.hit ? double(c.within1) / double(c.hit) : 0.0;
  c.within5_share = c.hit ? double(c.within5) / double(c.hit) : 0.0;
  uint64_t total = 0;
  for (uint32_t b = 0; b < kHistogramBins; ++b) total += words[kStatHistogram + b];
  if (total != 0) {
    uint64_t seen = 0;
    bool got50 = false;
    for (uint32_t b = 0; b < kHistogramBins; ++b) {
      seen += words[kStatHistogram + b];
      const double upper = b + 1 == kHistogramBins ? 1.0 : double(b + 1) * kHistogramStep;
      if (!got50 && seen * 2 >= total) {
        c.p50 = upper;
        got50 = true;
      }
      if (seen * 100 >= total * 95) {
        c.p95 = upper;
        break;
      }
    }
  }
  return c;
}

// The words of the interval between two readbacks (the counters are never cleared: every word only grows).
inline void CheckDelta(const uint32_t now[kStatWords], const uint32_t before[kStatWords], uint32_t out[kStatWords]) {
  for (uint32_t i = 0; i < kStatWords; ++i) out[i] = now[i] - before[i];
}

}  // namespace mcla::gpu::city_rt_shadow

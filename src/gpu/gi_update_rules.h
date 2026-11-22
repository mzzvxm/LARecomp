#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe update's rules: what needs no renderer (package G2 of perf/CITY-RT-M3.md, sections 3.1, 3.2, 3.3, 6, 7)
// ===========================================================================
// gi_frame.h (G0) FIXES what the field is, gi_field_rules.h (G1) what its owner decides. This file is what the UPDATE decides with those, as plain functions and small classes with no device, so that
// gi_update_rules_test.cpp checks them alone, and the C++ TWIN of hlsl/gi_update_common.hlsli (which that test compiles as C++ and compares with this file on random inputs):
//
//   THE RAY SET             a probe's rays are a spherical Fibonacci set (rays_per_probe of the budget: 64, lean 32) turned by a random rotation that is new at every update (a hash of the frame, the
//                           cascade and the probe), so that successive updates sample new directions: no banding (RayDirection).
//   THE HIT SHADING         hlsl/gi_update_trace_cs.hlsl calls the city hit library in GI mode (city_hit_shade.hlsli CityHitShadeHit, CITY_HIT_MODE_GI): the surface's average colour times the game's ambient,
//                           the sun by N.L with a shadow ray, one stochastic lamp of the nearest eight with a shadow ray (night), emissive glow, and the field itself, one frame old, times the bounce weight
//                           (the second bounce and the ones after it for free). Units: the paint's (params.units = 1), the field's (gi_frame.h UNITS).
//   THE BLEND               IntegrateTexel (gi_frame.h) over the rays into the probe's tile, then the blend with ADAPTIVE HYSTERESIS (Hysteresis): the stored value's weight is the running mean while a probe
//                           is young, at most gi::kHysteresis (0.97: about 33 updates); gi::kHysteresisJump (0.85) for a probe traced because its light changed (a sector built; an hour jump clears the probes instead: gi_field_rules.h CutKind) or whose mean
//                           luminance JUMPED (moved by more than kJumpRelative and by more than kJumpSigmas standard errors of the rays' mean: noise from a probe that sees a small bright thing with a few rays is not a change);
//                           none for a probe with no history (new, or back from being inside geometry).
//   THE CLASSIFICATION      a probe is INSIDE geometry (switched off: weight 0, no light) when more than kInsideBackShare of its rays hit back faces (the geometric normal against the ray) or its nearest hit is
//                           nearer than kInsideNearestSpacings of a spacing. It is traced again at one visit in eight (SkipInside) so that streaming geometry is noticed. CHECK C1: the pack's winding.
//   THE SCHEDULER           Scheduler: each frame, per cascade, the probes the field's dirty set names first (new ones the window's move exposed, ones a built sector or a cut made stale), nearest the camera
//                           first, then a round robin over the cascade's slots so that every probe is traced once in `interval_frames` (gi_frame.h Budget); the budget of a cascade is its ProbesPerFrame times
//                           the field's warm-up scale (4x for 12 frames after a cut) and, for the cascades after the finest, the governor's scale. A probe beyond M1's safe range is not traced (its weight
//                           is 0 and it stays dirty). The finest cascade is never scaled down: "never below the minimum that keeps the near cascade fresh".
//   THE GOVERNOR            Governor: an EWMA of the update's GPU time against the cell's cap scales the later cascades' budget between gi::kGovernorMin and gi::kGovernorMax.
//   THE BUDGETS             BudgetOf(fit): rays a probe, the chunk (the probes one pass of the two holds: the ray buffer is sized for the layout's rays a frame, G1's update_reserve; a larger plan, the
//                           warm-up's, runs in several chunks), the ray buffer's bytes, the cap of the governor, per (tier, level) cell through the layout (gi::LayoutFor).
//   THE CENSUS              the counters' words the two passes add to (Stats), their decode (DecodeStats) and the text of the four '[rt gi]' lines (perf/city-rt-m3/scenes/README.md).
//
// Everything here is plain data and pure functions (render thread only where a class has state). The header includes nothing of Plume. Numbers marked `G2a` are first guesses (values, changeable without
// a version); `COST PASS` as in gi_frame.h. gi_frame.h and gi_field_rules.h are not edited.
//
// CHECK (nothing here was run against the game):
//   G2a  kInsideBackShare / kInsideNearestSpacings / kJumpRelative / kJumpFloor and the retest rate are the design's and first guesses; the canyon, the underpass and the leak scene decide
//   G2b  the ray reach (RayReachM: the safe range, 64 m to 1,000 m): rays that meet nothing within it escape (the sky), which lifts the sky visibility and adds nothing to the irradiance
//   G2c  the radiance clamp (kMaxRadiance: 32 scene units) against the game's real sun and neon values
//   G2d  the half precision of the ray records: the shader rounds with f32tof16 (the GPU's rounding), the twin to nearest even; the records only carry radiance of at most 32
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gi_field_rules.h"
#include "gi_frame.h"

namespace mcla::gpu::gi_update {

inline constexpr uint32_t kFormatVersion = 1;  // GIU_FORMAT_VERSION in hlsl/gi_update_common.hlsli

// ---------------------------------------------------------------------------
// the numbers (the HLSL's #defines: gi_update_rules_test.cpp compares every one)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kTraceGroup = 64;       // GIU_TRACE_GROUP: the trace pass's threads in a group
inline constexpr uint32_t kGroupsX = 4096;        // GIU_GROUPS_X: both passes dispatch (kGroupsX, ceil(groups / kGroupsX), 1)
inline constexpr uint32_t kRayBytes = 16;         // GIU_RAY_BYTES: a ray record is one uint4
inline constexpr float kMaxRadiance = 32.0f;      // GIU_MAX_RADIANCE
inline constexpr float kTwoPi = 6.283185307179586f;
inline constexpr float kGolden = 0.6180339887498949f;
inline constexpr float kInsideBackShare = 0.30f;
inline constexpr float kInsideNearestSpacings = 0.1f;
inline constexpr uint32_t kInsideRetestMask = 7;  // GIU_INSIDE_RETEST_MASK: one visit in 8
inline constexpr float kJumpRelative = 0.25f;
inline constexpr float kJumpFloor = 0.05f;
inline constexpr float kJumpSigmas = 4.0f;
inline constexpr float kRayFixed = 4096.0f;
inline constexpr float kChangedStat = 0.02f;
inline constexpr uint32_t kConvergedUpdates = 32;
inline constexpr float kFixedScale = 1024.0f;
inline constexpr uint32_t kHistBins = 32;

inline constexpr uint32_t kRayHit = 1;            // GIU_RAY_*: the flags of a ray record
inline constexpr uint32_t kRayBack = 2;
inline constexpr uint32_t kRayLod = 4;
inline constexpr uint32_t kRaySkipped = 8;
inline constexpr uint32_t kProbeDirty = 1;        // GIU_PROBE_DIRTY: the probe word's flags
inline constexpr uint32_t kProbeFlagsShift = 2;
inline constexpr uint32_t kProbeIndexShift = 6;

// The rays' reach: how far a probe's ray goes before it counts as escaped (the sky): the safe range, so that what is beyond M1's structures is not looked for, between these.
inline constexpr float kRayReachMinM = 64.0f;
inline constexpr float kRayReachMaxM = 1000.0f;
inline float RayReachM(float safe_range_m) { return std::clamp(safe_range_m, kRayReachMinM, kRayReachMaxM); }

// The tangent of the half angle of one ray's cone: the rays of a probe spread 4 pi over their count, so a ray stands for a cone of solid angle 4 pi / n; the cut-outs' alpha is read at that width.
inline float RayConeTanHalf(uint32_t rays) { return rays > 0 ? 0.5f * std::sqrt(4.0f * 3.14159265358979f / float(rays)) : 0.0f; }

// The most chunks a frame holds (the warm-up's 4x budget).
inline constexpr uint32_t kMaxChunks = 4;

// How often the field's counters are read back and printed (frames), and the interval's length for the census.
inline constexpr uint64_t kReadbackEveryFrames = 120;

// ---------------------------------------------------------------------------
// the hash, the rotation and the rays (the C++ twin of hlsl/gi_update_common.hlsli: the same operations in the same order)
// ---------------------------------------------------------------------------

constexpr uint32_t Hash(uint32_t x) {
  x = x ^ (x >> 16u);
  x = x * 0x7feb352du;
  x = x ^ (x >> 15u);
  x = x * 0x846ca68bu;
  x = x ^ (x >> 16u);
  return x;
}
constexpr float Unit(uint32_t h) { return float(h >> 8u) * (1.0f / 16777216.0f); }
constexpr uint32_t Seed(uint32_t frame, uint32_t cascade, uint32_t index) { return Hash(index * 0x9E3779B1u + cascade * 0x85EBCA6Bu + Hash(frame + 0x27d4eb2fu)); }

inline gi::Vec3 Fibonacci(uint32_t i, uint32_t n) {
  const float z = 1.0f - (2.0f * float(i) + 1.0f) / float(n);
  const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
  const float x = float(i) * kGolden;
  const float phi = kTwoPi * (x - std::floor(x));
  return {r * std::cos(phi), z, r * std::sin(phi)};
}

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
inline Quat RotationQuat(uint32_t seed) {
  const float u1 = Unit(Hash(seed ^ 0xA511E9B3u));
  const float u2 = Unit(Hash(seed ^ 0x63D83595u));
  const float u3 = Unit(Hash(seed ^ 0xB5297A4Du));
  const float a = std::sqrt(1.0f - u1);
  const float b = std::sqrt(u1);
  const float t2 = kTwoPi * u2;
  const float t3 = kTwoPi * u3;
  return {a * std::sin(t2), a * std::cos(t2), b * std::sin(t3), b * std::cos(t3)};
}
inline gi::Vec3 Cross(gi::Vec3 a, gi::Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline gi::Vec3 Rotate(const Quat& q, gi::Vec3 v) {
  const gi::Vec3 qv = {q.x, q.y, q.z};
  const gi::Vec3 t = Cross(qv, v) * 2.0f;
  return v + t * q.w + Cross(qv, t);
}
inline gi::Vec3 RayDirection(uint32_t ray, uint32_t rays, const Quat& q) { return Rotate(q, Fibonacci(ray, rays)); }

// ---------------------------------------------------------------------------
// the probe word and the ray record
// ---------------------------------------------------------------------------

constexpr uint32_t ProbeWord(uint32_t cascade, uint32_t flags, uint32_t index) { return (cascade & 3u) | ((flags & 15u) << kProbeFlagsShift) | (index << kProbeIndexShift); }
constexpr uint32_t ProbeCascade(uint32_t word) { return word & 3u; }
constexpr uint32_t ProbeFlags(uint32_t word) { return (word >> kProbeFlagsShift) & 15u; }
constexpr uint32_t ProbeIndex(uint32_t word) { return word >> kProbeIndexShift; }

// A float as an IEEE half, to nearest even; and back. Overflow goes to infinity (the records never hold more than kMaxRadiance).
inline uint32_t FloatToHalf(float f) {
  const uint32_t b = std::bit_cast<uint32_t>(f);
  const uint32_t sign = (b >> 16) & 0x8000u;
  const int32_t exponent = int32_t((b >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = b & 0x7FFFFFu;
  if (((b >> 23) & 0xFFu) == 0xFFu) return sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u);  // infinity, NaN
  if (exponent >= 31) return sign | 0x7C00u;
  if (exponent <= 0) {
    if (exponent < -10) return sign;
    mantissa |= 0x800000u;
    const uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    const uint32_t rest = mantissa & ((1u << shift) - 1u);
    const uint32_t halfway = 1u << (shift - 1u);
    if (rest > halfway || (rest == halfway && (half & 1u))) ++half;
    return sign | half;
  }
  uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
  const uint32_t rest = mantissa & 0x1FFFu;
  if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;
  return sign | half;
}
inline float HalfToFloat(uint32_t v) {
  const int exponent = int((v >> 10) & 31u);
  const int mantissa = int(v & 1023u);
  const float sign = (v & 0x8000u) != 0 ? -1.0f : 1.0f;
  if (exponent == 0) return sign * std::ldexp(float(mantissa), -24);
  if (exponent == 31) return mantissa == 0 ? sign * INFINITY : NAN;
  return sign * std::ldexp(float(1024 + mantissa), exponent - 25);
}

struct Ray {
  gi::Vec3 radiance;
  float distance = 0.0f;
  uint32_t flags = 0;
  uint32_t extra = 0;
};
struct RayWords {
  uint32_t w[4] = {0, 0, 0, 0};
  bool operator==(const RayWords& o) const { return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2] && w[3] == o.w[3]; }
};
// (a NaN and a negative value are 0: the shader decides the NaN by the bits (GiuRadianceChannel), not by fmax, which Vulkan leaves open for a NaN)
inline float RadianceChannel(float v) { return v >= 0.0f ? std::min(v, kMaxRadiance) : 0.0f; }
inline RayWords PackRay(const Ray& r) {
  const uint32_t rr = FloatToHalf(RadianceChannel(r.radiance.x));
  const uint32_t gg = FloatToHalf(RadianceChannel(r.radiance.y));
  const uint32_t bb = FloatToHalf(RadianceChannel(r.radiance.z));
  RayWords out;
  out.w[0] = rr | (gg << 16u);
  out.w[1] = bb | ((r.flags & 0xFFFFu) << 16u);
  out.w[2] = std::bit_cast<uint32_t>(r.distance);
  out.w[3] = r.extra;
  return out;
}
inline Ray UnpackRay(const RayWords& w) {
  Ray r;
  r.radiance = {HalfToFloat(w.w[0] & 0xFFFFu), HalfToFloat(w.w[0] >> 16u), HalfToFloat(w.w[1] & 0xFFFFu)};
  r.flags = w.w[1] >> 16u;
  r.distance = std::bit_cast<float>(w.w[2]);
  r.extra = w.w[3];
  return r;
}

// The record of the trace's closest hit (hlsl gi_update_trace_cs.hlsl): a hit whose data decoded is a hit (the flags, its distance, its class in `extra`); a ray that hit nothing is a miss (no flag, the ray's
// `reach` as the distance, no light); and a hit with NO DATA (a geometry-only sector: its hit data blob is bad or missing, CityHitDecode says not valid) is a miss too, as the reflections and the pixel
// pass take it. It is not a black occluder: a black hit adds no light AND no sky to its direction (it reads as full shadow, where an unbuilt sector is open sky), and the blend would darken the probes
// beside every such sector (CITY-RT-REVIEW M5).
inline Ray HitRecord(bool geometry_hit, bool data_decoded, float t, float reach, gi::Vec3 radiance, bool back, bool lod, uint32_t cls) {
  Ray r;
  r.distance = reach;
  if (geometry_hit && data_decoded) {
    r.radiance = radiance;
    r.distance = t;
    r.flags = kRayHit | (back ? kRayBack : 0u) | (lod ? kRayLod : 0u);
    r.extra = cls;
  }
  return r;
}

// ---------------------------------------------------------------------------
// the classification and the blend
// ---------------------------------------------------------------------------

inline bool ProbeInside(uint32_t back_count, uint32_t ray_count, float nearest, float spacing) {
  return float(back_count) > kInsideBackShare * float(ray_count) || nearest < kInsideNearestSpacings * spacing;
}
constexpr uint32_t BackByte(uint32_t back_count, uint32_t ray_count) { return ray_count > 0 ? std::min((back_count * 255u + ray_count / 2u) / ray_count, 255u) : 0u; }
constexpr bool SkipInside(bool inside, uint32_t frame, uint32_t probe_number) { return inside && (Hash(frame * 0x9E3779B1u + probe_number) & kInsideRetestMask) != 0u; }

inline float Luma(gi::Vec3 c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; }
inline float RelativeChange(float old_mean, float new_mean) { return std::fabs(new_mean - old_mean) / std::max(std::max(old_mean, new_mean), kJumpFloor); }
inline float StandardError(float sum_luma, float sum_luma_sq, uint32_t count) {
  const float n = float(std::max(count, 1u));
  const float mean = sum_luma / n;
  const float variance = std::max(sum_luma_sq / n - mean * mean, 0.0f);
  return std::sqrt(variance / n);
}
inline bool MeanJumped(float old_mean, float new_mean, float standard_error) {
  return RelativeChange(old_mean, new_mean) > kJumpRelative && std::fabs(new_mean - old_mean) > kJumpSigmas * standard_error;
}
inline float Hysteresis(uint32_t updates, bool is_new, bool dirty, bool jumped, float base = gi::kHysteresis, float jump = gi::kHysteresisJump) {
  if (is_new) return 0.0f;
  float h = std::min(base, float(updates) / (float(updates) + 1.0f));
  if (dirty || jumped) h = std::min(h, jump);
  return h;
}
// A value that is not a number (NaN or infinite) must not stay in a probe for good (NaN * 0 is NaN): a sample that is not one leaves the history, a history that is not one is replaced by the sample, both
// not: 0 (GiuBlend1; for numbers old * h + new * (1 - h)).
inline float Blend(float old_value, float new_value, float h) {
  const bool old_ok = std::isfinite(old_value), new_ok = std::isfinite(new_value);
  if (!new_ok) return old_ok ? old_value : 0.0f;
  if (!old_ok) return new_value;
  return old_value * h + new_value * (1.0f - h);
}
inline gi::Vec3 Blend(gi::Vec3 old_value, gi::Vec3 new_value, float h) {
  return {Blend(old_value.x, new_value.x, h), Blend(old_value.y, new_value.y, h), Blend(old_value.z, new_value.z, h)};
}
// The census's histogram bin of a probe's mean luminance: half octaves from 2^-10 (bin 0) to 2^6 (bin 31).
inline uint32_t HistBin(float mean) {
  const float v = (std::log2(std::max(mean, 1.0e-6f)) + 10.0f) * 2.0f;
  return uint32_t(std::min(std::max(v, 0.0f), 31.0f));
}
// The upper edge of a bin (what a percentile that falls in the bin is reported as).
inline float HistBinUpper(uint32_t bin) { return std::exp2(float(bin + 1) * 0.5f - 10.0f); }

// What the blend does to the state word (the twin of the shader's last lines, see hlsl/gi_update_blend_cs.hlsl): a probe that is inside keeps its count and its new bit; one that is not has its count raised
// and the new bit cleared; the inside bit and the back face share are this update's; the frame's low 14 bits go in the reserved bits.
constexpr uint32_t StateAfterBlend(uint32_t state, bool inside, uint32_t back_byte, uint32_t frame) {
  const uint32_t kept = state & ~(gi_field::kStateInside | gi_field::kStateBackMask);
  uint32_t next = inside ? kept : gi_field::StateAfterUpdate(kept);
  next = next | (inside ? gi_field::kStateInside : 0u) | (back_byte << gi_field::kStateBackShift);
  return (next & 0x3FFFFu) | ((frame & 0x3FFFu) << 18u);
}

// ---------------------------------------------------------------------------
// the budgets of the cells
// ---------------------------------------------------------------------------

struct Budget {
  gi::Layout layout = gi::Layout::kNone;
  uint32_t cascades = 0;
  uint32_t rays_per_probe = 0;
  uint32_t probes_per_frame = 0;     // the nominal budget (gi_frame.h ProbesPerFrame summed), before the warm-up and the governor
  uint32_t rays_per_frame = 0;       // ... times the rays: the design's 12 K, 459 K and 983 K
  uint32_t chunk_probes = 0;         // the probes one chunk (one trace dispatch and one blend dispatch) holds: the ray buffer's capacity
  uint32_t max_probes = 0;           // the most a frame's plan holds (kMaxChunks chunks)
  uint64_t ray_buffer_bytes = 0;     // chunk_probes x rays_per_probe x kRayBytes: G1's update_reserve exactly
  uint64_t list_bytes = 0;           // the probe list's upload buffer: a uint a probe of max_probes
  float cap_ms = 0.0f;               // the governor's cap on the update's GPU time (the trace and the blend of a frame)
};

// The cap of the update's GPU time (COST PASS): the trace and blend share of the layout's estimated cost on a strong card (M3 section 6: trace 0.4 to 0.7, blend 0.1 to 0.2 of 0.9 to 1.5 at HIGH).
constexpr float CapMs(gi::Layout layout) {
  switch (layout) {
    case gi::Layout::kLean: return 0.45f;
    case gi::Layout::kStandard: return 0.9f;
    case gi::Layout::kExtended: return 1.8f;
    case gi::Layout::kNone: break;
  }
  return 0.0f;
}

// All the cascades of a budget trace the same number of rays (the trace pass packs 64 / rays probes in a group of 64 threads).
constexpr bool UniformRays(const gi::Budget& b) {
  for (uint32_t c = 1; c < b.cascade_count; ++c) {
    if (b.cascade[c].rays_per_probe != b.cascade[0].rays_per_probe) return false;
  }
  return true;
}

// The update's budget for the cascades a fit made (a smaller field than the cell asked for has fewer).
constexpr Budget BudgetOf(const gi::Budget& g) {
  Budget b;
  b.layout = g.layout;
  b.cascades = g.cascade_count;
  if (g.cascade_count == 0) return b;
  b.rays_per_probe = g.cascade[0].rays_per_probe;
  for (uint32_t c = 0; c < g.cascade_count; ++c) b.probes_per_frame += gi::ProbesPerFrame(g.cascade[c]);
  b.rays_per_frame = gi::BudgetRaysPerFrame(g);
  b.chunk_probes = b.rays_per_probe > 0 ? (b.rays_per_frame + b.rays_per_probe - 1) / b.rays_per_probe : 0;
  b.max_probes = b.chunk_probes * kMaxChunks;
  b.ray_buffer_bytes = uint64_t(b.chunk_probes) * b.rays_per_probe * kRayBytes;
  b.list_bytes = uint64_t(b.max_probes) * 4u;
  b.cap_ms = CapMs(g.layout);
  return b;
}

// ---------------------------------------------------------------------------
// the scheduler
// ---------------------------------------------------------------------------

struct PlanInputs {
  double camera[3] = {0, 0, 0};  // the play camera, world metres
  float safe_range_m = 0.0f;     // M1's SafeRange: a probe beyond it is not traced
  float governor_scale = 1.0f;   // the governor's, for the cascades after the finest
  float warmup_scale = 1.0f;     // gi_field_rules.h FieldState::BudgetScale
};

struct Plan {
  std::vector<uint32_t> words;                                // the probe words (ProbeWord), the finest cascade first, a cascade's dirty probes first
  std::array<uint32_t, gi::kMaxCascades> per_cascade = {};    // probes planned in each cascade
  std::array<uint32_t, gi::kMaxCascades> dirty_taken = {};    // ... of them from the field's dirty set (new probes)
  std::array<uint32_t, gi::kMaxCascades> stale_taken = {};    // ... and from the scheduler's stale set (a sector was built near them)
  uint32_t left_dirty_out_of_range = 0;                       // dirty probes seen beyond the safe range (they stay dirty)
  uint32_t Total() const { return uint32_t(words.size()); }
};

// The probes of a cascade a frame traces: the nominal ProbesPerFrame times the scale, at least 1, at most the cascade.
inline uint32_t CascadeShare(const gi::CascadeSpec& spec, float scale) {
  const float n = std::ceil(float(gi::ProbesPerFrame(spec)) * std::max(scale, 0.0f));
  return std::min<uint32_t>(std::max<uint32_t>(uint32_t(n), 1u), spec.dims.Probes());
}

class Scheduler {
 public:
  void Start(const gi::Budget& budget) {
    budget_ = budget;
    for (uint32_t c = 0; c < gi::kMaxCascades; ++c) {
      cursor_[c] = 0;
      stale_[c].Resize(c < budget.cascade_count ? budget.cascade[c].dims.Probes() : 0);
    }
    steps_ = 0;
  }
  uint32_t cursor(uint32_t c) const { return c < gi::kMaxCascades ? cursor_[c] : 0; }
  uint64_t steps() const { return steps_; }
  uint32_t StaleCount(uint32_t c) const { return c < gi::kMaxCascades ? stale_[c].Count() : 0; }

  // A built sector's box [lo, hi] (world metres): the probes of every cascade within `margin_m` of it (< 0: kMomentsMaxSpacings spacings of each cascade, as FieldState::MarkWorldBox) have data
  // that is STALE, not new: their rays can meet geometry that was not there. They are traced first, with the jump hysteresis, but they are NOT in the field's dirty sets: those drive the field's
  // fade (gi_field_rules.h FadeState: a finest cascade that is mostly dirty is not ready), and a sector that streams in must not fade the field out. Returns the probes newly marked.
  uint32_t MarkStaleBox(const gi_field::FieldState& state, const double lo[3], const double hi[3], double margin_m = -1.0) {
    uint32_t marked = 0;
    for (uint32_t c = 0; c < budget_.cascade_count; ++c) {
      const float s = budget_.cascade[c].spacing_m;
      const double m = margin_m >= 0.0 ? margin_m : double(gi::kMomentsMaxSpacings) * double(s);
      const gi::Box box = {{gi::CellOf(lo[0] - m, s), gi::CellOf(lo[1] - m, s), gi::CellOf(lo[2] - m, s)}, {gi::CellOf(hi[0] + m, s) + 1, gi::CellOf(hi[1] + m, s) + 1, gi::CellOf(hi[2] + m, s) + 1}};
      const gi::Dims& d = budget_.cascade[c].dims;
      const gi::Box win = state.Window(c);
      const gi::Box b = {{std::max(box.lo.x, win.lo.x), std::max(box.lo.y, win.lo.y), std::max(box.lo.z, win.lo.z)}, {std::min(box.hi.x, win.hi.x), std::min(box.hi.y, win.hi.y), std::min(box.hi.z, win.hi.z)}};
      if (b.Empty()) continue;
      for (int32_t z = b.lo.z; z < b.hi.z; ++z)
        for (int32_t y = b.lo.y; y < b.hi.y; ++y)
          for (int32_t x = b.lo.x; x < b.hi.x; ++x) marked += stale_[c].Mark(gi_field::IndexOfCoord({x, y, z}, d)) ? 1u : 0u;
    }
    return marked;
  }
  // Every probe of every cascade is stale (the builds were too many to follow one by one).
  void MarkAllStale() {
    for (uint32_t c = 0; c < budget_.cascade_count; ++c) stale_[c].MarkAll();
  }

  // One frame's plan. `state` is the field's (dirty sets, windows); the probes it names that are in range are taken from the dirty set (NoteTraced). `max_probes`: what the frame's chunks hold. The order
  // inside a cascade: the field's dirty probes (new), the stale ones, then the round robin; each of the first two nearest the camera first.
  Plan Make(gi_field::FieldState& state, const PlanInputs& in, uint32_t max_probes) {
    Plan plan;
    if (!state.running() || budget_.cascade_count == 0) return plan;
    uint32_t left = max_probes;
    for (uint32_t c = 0; c < budget_.cascade_count && left > 0; ++c) {
      const gi::CascadeSpec& spec = budget_.cascade[c];
      const float scale = in.warmup_scale * (c == 0 ? 1.0f : in.governor_scale);
      const uint32_t want = std::min(CascadeShare(spec, scale), left);
      const gi::Dims& d = spec.dims;
      const gi::Coord origin = state.origin(c);
      const float s = spec.spacing_m;
      const gi::Coord ref = {gi::CellOf(in.camera[0], s), gi::CellOf(in.camera[1], s), gi::CellOf(in.camera[2], s)};
      const gi::Vec3 cam = {float(in.camera[0]), float(in.camera[1]), float(in.camera[2])};
      uint32_t chosen = 0;
      std::vector<uint64_t> taken((size_t(d.Probes()) + 63) / 64, 0ull);
      const auto in_range = [&](uint32_t index) {
        const gi::Coord k = gi_field::CoordOfIndex(origin, d, index);
        const gi::Position p = gi::ProbeWorld(k, s);
        return gi_field::ProbeInRange({float(p.x), float(p.y), float(p.z)}, cam, in.safe_range_m);
      };
      const auto take = [&](uint32_t index, uint32_t flags) {
        taken[index >> 6] |= 1ull << (index & 63);
        plan.words.push_back(ProbeWord(c, flags, index));
        ++chosen;
      };
      // the field's dirty probes first: new ones, nearest the camera
      std::vector<uint32_t> found(want);
      const uint32_t n_dirty = state.CollectDirty(c, want, ref, found.data());
      for (uint32_t i = 0; i < n_dirty; ++i) {
        if (!in_range(found[i])) {
          ++plan.left_dirty_out_of_range;
          continue;
        }
        state.NoteTraced(c, found[i]);
        take(found[i], kProbeDirty);
        ++plan.dirty_taken[c];
      }
      // then the stale ones, nearest first
      if (chosen < want && stale_[c].Count() > 0) {
        std::vector<std::pair<uint32_t, uint32_t>> keys;  // (distance in cells, index)
        keys.reserve(stale_[c].Count());
        stale_[c].ForEach([&](uint32_t i) {
          const gi::Coord p = gi_field::CoordOfIndex(origin, d, i);
          keys.push_back({uint32_t(std::max({std::abs(p.x - ref.x), std::abs(p.y - ref.y), std::abs(p.z - ref.z)})), i});
        });
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) {
          if (chosen >= want) break;
          const uint32_t i = key.second;
          if (((taken[i >> 6] >> (i & 63)) & 1ull) != 0) {
            stale_[c].Clear(i);  // it was just taken as a dirty probe: nothing is stale about it any more
            continue;
          }
          if (!in_range(i)) continue;  // stays stale: its weight is 0 and the range may grow
          stale_[c].Clear(i);
          take(i, kProbeDirty);
          ++plan.stale_taken[c];
        }
      }
      // then the round robin over the cascade's slots, the rest of the share
      uint32_t examined = 0;
      while (chosen < want && examined < d.Probes()) {
        const uint32_t index = cursor_[c];
        cursor_[c] = cursor_[c] + 1 >= d.Probes() ? 0 : cursor_[c] + 1;
        ++examined;
        ++steps_;
        if (((taken[index >> 6] >> (index & 63)) & 1ull) != 0) continue;
        if (!in_range(index)) continue;
        take(index, 0);
      }
      plan.per_cascade[c] = chosen;
      left -= std::min<uint32_t>(left, chosen);
    }
    return plan;
  }

 private:
  gi::Budget budget_;
  std::array<uint32_t, gi::kMaxCascades> cursor_ = {};
  std::array<gi_field::DirtySet, gi::kMaxCascades> stale_ = {};
  uint64_t steps_ = 0;
};

// The chunks of a plan: [first, first + count) of its words, `chunk_probes` at most each.
struct Chunk {
  uint32_t first = 0;
  uint32_t count = 0;
};
inline std::vector<Chunk> ChunksOf(uint32_t total, uint32_t chunk_probes) {
  std::vector<Chunk> out;
  if (chunk_probes == 0) return out;
  for (uint32_t first = 0; first < total; first += chunk_probes) out.push_back({first, std::min(chunk_probes, total - first)});
  return out;
}
// A dispatch of `groups` thread groups as (x, y) with x at most kGroupsX: group g = y * kGroupsX + x.
struct Dispatch {
  uint32_t x = 0, y = 0;
};
constexpr Dispatch DispatchFor(uint32_t groups) { return {groups < kGroupsX ? groups : kGroupsX, groups == 0 ? 0u : (groups + kGroupsX - 1) / kGroupsX}; }
// The trace pass's groups for `probes` probes of `rays` rays each: 64 threads a group, 64 / rays probes in one.
constexpr uint32_t TraceGroups(uint32_t probes, uint32_t rays) {
  const uint32_t per_group = rays > 0 && rays <= kTraceGroup ? kTraceGroup / rays : 1u;
  return (probes + per_group - 1) / per_group;
}

// ---------------------------------------------------------------------------
// the governor
// ---------------------------------------------------------------------------

// The later cascades' budget scale from the update's measured GPU time against the cell's cap. An EWMA smooths the frames; over the cap the scale falls by 3% a sample down to gi::kGovernorMin, well
// under it (0.6 of the cap) it rises by 1% a sample up to gi::kGovernorMax, between them it holds. No sample (timestamps unavailable) holds the scale at 1.
class Governor {
 public:
  static constexpr float kAlpha = 0.1f;
  static constexpr float kOver = 1.0f;
  static constexpr float kUnder = 0.6f;
  static constexpr float kFall = 0.97f;
  static constexpr float kRise = 1.01f;
  void Reset() {
    scale_ = 1.0f;
    ewma_ms_ = 0.0f;
    samples_ = 0;
  }
  void AddSample(float ms, float cap_ms) {
    if (!(ms >= 0.0f) || !(cap_ms > 0.0f)) return;
    ewma_ms_ = samples_ == 0 ? ms : ewma_ms_ + kAlpha * (ms - ewma_ms_);
    ++samples_;
    const float ratio = ewma_ms_ / cap_ms;
    if (ratio > kOver) {
      scale_ = std::max(gi::kGovernorMin, scale_ * kFall);
    } else if (ratio < kUnder) {
      scale_ = std::min(gi::kGovernorMax, scale_ * kRise);
    }
  }
  float scale() const { return scale_; }
  float ewma_ms() const { return ewma_ms_; }
  uint64_t samples() const { return samples_; }

 private:
  float scale_ = 1.0f;
  float ewma_ms_ = 0.0f;
  uint64_t samples_ = 0;
};

// ---------------------------------------------------------------------------
// the census: the counters' words the two passes add to (never cleared: the host takes the difference between two readbacks)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kStatCascade0 = 0;   // 0..3: probes updated (traced and classified) in cascade c
inline constexpr uint32_t kStatRays = 4;       // rays traced
inline constexpr uint32_t kStatHit = 5;
inline constexpr uint32_t kStatEscaped = 6;
inline constexpr uint32_t kStatBack = 7;       // rays that hit a back face
inline constexpr uint32_t kStatLod = 8;        // rays that hit the lod's geometry
inline constexpr uint32_t kStatInside = 9;     // probes classified inside geometry at their update
inline constexpr uint32_t kStatChanged = 10;   // probes blended whose mean luminance moved by more than kChangedStat
inline constexpr uint32_t kStatRelit = 11;     // probes traced because they were dirty (not new)
inline constexpr uint32_t kStatConverged = 12; // probes blended with kConvergedUpdates updates or more
inline constexpr uint32_t kStatBlended = 13;   // probes blended (not inside)
inline constexpr uint32_t kStatLumaSum = 14;   // the sum of the blended probes' mean irradiance luminance x kFixedScale
inline constexpr uint32_t kStatSkySum = 15;    // ... of their mean sky visibility
inline constexpr uint32_t kStatSkipped = 16;   // inside probes the trace did not look at
inline constexpr uint32_t kStatHist0 = 18;     // 18..49: the histogram of the blended probes' mean luminance (HistBin)
inline constexpr uint32_t kStatWords = 50;
inline constexpr uint64_t kStatBytes = 256;    // the buffer (kStatWords x 4 = 200 bytes, rounded up)
static_assert(kStatHist0 + kHistBins == kStatWords && kStatWords * 4 <= kStatBytes, "the counters' words");

struct StatSummary {
  uint64_t rays = 0;
  double hit_share = 0, escaped_share = 0, back_share = 0, lod_share = 0;  // of the rays, 0..1
  uint64_t probes_blended = 0, probes_inside = 0, probes_skipped = 0, probes_relit = 0;
  double changed_share = 0, converged_share = 0;                            // of the blended probes
  double mean_irradiance = 0, p5 = 0, p95 = 0, mean_sky = 0;
};

// The difference of two readbacks (a counter wraps at 2^32: the unsigned difference is right across it).
inline void StatDelta(const uint32_t now[kStatWords], const uint32_t last[kStatWords], uint32_t out[kStatWords]) {
  for (uint32_t i = 0; i < kStatWords; ++i) out[i] = now[i] - last[i];
}

inline StatSummary DecodeStats(const uint32_t d[kStatWords]) {
  StatSummary s;
  s.rays = d[kStatRays];
  if (s.rays > 0) {
    s.hit_share = double(d[kStatHit]) / double(s.rays);
    s.escaped_share = double(d[kStatEscaped]) / double(s.rays);
    s.back_share = double(d[kStatBack]) / double(s.rays);
    s.lod_share = double(d[kStatLod]) / double(s.rays);
  }
  s.probes_blended = d[kStatBlended];
  s.probes_inside = d[kStatInside];
  s.probes_skipped = d[kStatSkipped];
  s.probes_relit = d[kStatRelit];
  if (s.probes_blended > 0) {
    s.changed_share = double(d[kStatChanged]) / double(s.probes_blended);
    s.converged_share = double(d[kStatConverged]) / double(s.probes_blended);
    s.mean_irradiance = double(d[kStatLumaSum]) / double(kFixedScale) / double(s.probes_blended);
    s.mean_sky = double(d[kStatSkySum]) / double(kFixedScale) / double(s.probes_blended);
    // the percentiles: the upper edge of the bin the cumulative count passes 5% and 95% in
    uint64_t total = 0;
    for (uint32_t b = 0; b < kHistBins; ++b) total += d[kStatHist0 + b];
    if (total > 0) {
      uint64_t cumulative = 0;
      bool have5 = false, have95 = false;
      for (uint32_t b = 0; b < kHistBins; ++b) {
        cumulative += d[kStatHist0 + b];
        if (!have5 && double(cumulative) >= 0.05 * double(total)) {
          s.p5 = HistBinUpper(b);
          have5 = true;
        }
        if (!have95 && double(cumulative) >= 0.95 * double(total)) {
          s.p95 = HistBinUpper(b);
          have95 = true;
        }
      }
    }
  }
  return s;
}

// What the interval's line needs besides the counters.
struct IntervalInfo {
  uint64_t frame = 0;
  uint32_t frames = 0;                                        // the interval's length
  std::array<uint64_t, gi::kMaxCascades> probes = {};         // planned in each cascade over the interval
  uint32_t cascades = 0;
  uint64_t cuts = 0;
  float budget_scale = 1.0f;                                  // the governor's scale now
  double finest_dirty_share = 0;                              // 0..1
};

// The four '[rt gi]' lines, the text after the prefix. The names and the order of the numbers are perf/city-rt-m3/scenes/README.md's (the report's regexes read them).
inline std::string FirstLine(uint64_t frame, gi::Layout layout, uint32_t cascades, uint32_t rays_per_probe, uint32_t instances, uint32_t hi, uint32_t lod, float bounce_weight, float hysteresis, float strength) {
  char text[512];
  std::snprintf(text, sizeof(text),
                "frame %llu: first update: layout %s (%u cascades), %u rays a probe, hit shading GI mode, %u instances in the top level (%u hi detail and %u lod sectors), bounce weight %.2f, hysteresis %.2f, strength %.2f",
                (unsigned long long)frame, gi::LayoutName(layout).data(), cascades, rays_per_probe, instances, hi, lod, double(bounce_weight), double(hysteresis), double(strength));
  return text;
}

inline std::string IntervalLine(const IntervalInfo& info, const StatSummary& s) {
  uint64_t probes = 0;
  std::string per;
  for (uint32_t c = 0; c < info.cascades && c < gi::kMaxCascades; ++c) {
    probes += info.probes[c];
    char one[64];
    std::snprintf(one, sizeof(one), "%s%llu in cascade %u", c == 0 ? "" : ", ", (unsigned long long)info.probes[c], c);
    per += one;
  }
  char text[1024];
  std::snprintf(text, sizeof(text),
                "frame %llu: %llu probes updated over the last %u frames (%s), %llu rays (%.1f%% hit, %.1f%% escaped, %.1f%% back faces), %llu probes inside geometry, %.1f%% changed by more than 2%% at the last blend "
                "(%llu relit), %.1f%% converged, irradiance mean %.3f (p5 %.3f, p95 %.3f), sky visibility mean %.2f, budget scale %.2f, %llu cuts, finest cascade %.1f%% dirty",
                (unsigned long long)info.frame, (unsigned long long)probes, info.frames, per.c_str(), (unsigned long long)s.rays, s.hit_share * 100.0, s.escaped_share * 100.0, s.back_share * 100.0,
                (unsigned long long)s.probes_inside, s.changed_share * 100.0, (unsigned long long)s.probes_relit, s.converged_share * 100.0, s.mean_irradiance, s.p5, s.p95, s.mean_sky, double(info.budget_scale),
                (unsigned long long)info.cuts, info.finest_dirty_share * 100.0);
  return text;
}

inline std::string CutLine(uint64_t frame, const std::string& why, uint64_t probes, float budget_scale, uint32_t warmup_frames) {
  char text[384];
  std::snprintf(text, sizeof(text), "cut at frame %llu: %s: %llu probes re-addressed, budget x%.1f for %u frames, fade from 0", (unsigned long long)frame, why.c_str(), (unsigned long long)probes, double(budget_scale), warmup_frames);
  return text;
}

inline std::string RateLine(uint64_t frame, double rays_per_frame, double trace_ms, double blend_ms) {
  const double mrays = trace_ms > 0.0 ? rays_per_frame / (trace_ms * 1.0e-3) / 1.0e6 : 0.0;
  char text[256];
  std::snprintf(text, sizeof(text), "frame %llu: rays %.1f M/s, trace %.3f ms, blend %.3f ms", (unsigned long long)frame, mrays, trace_ms, blend_ms);
  return text;
}

// Why a cut: the field's MoveReport says whether the camera jumped, and what the integration asked for says the rest.
inline std::string CutReason(bool relight, bool teleport_requested, double step_m) {
  char text[96];
  if (relight) return "hour jump";
  if (teleport_requested) return "warp";
  std::snprintf(text, sizeof(text), "camera step %.0f m", step_m);
  return text;
}

// ---------------------------------------------------------------------------
// the bindings (gi_update.cpp builds the layouts from these; gi_update_rules_test.cpp reads the two .hlsl files and checks their registers against them)
// ---------------------------------------------------------------------------
// THE PASS SET (one for each frame slot, shared by both passes): the registers are unique inside it (Vulkan numbers t and u registers together).
inline constexpr uint32_t kBindRows = 0;        // t0  ByteAddressBuffer: the frame's rows (gi_frame.h GpuGiFrame, 320 bytes: the update frame's)
inline constexpr uint32_t kBindProbes = 1;      // t1  ByteAddressBuffer: the probe list, a uint a probe
inline constexpr uint32_t kBindRays = 2;        // u2  RWByteAddressBuffer: the ray records
inline constexpr uint32_t kBindIrradiance = 3;  // u3  RWTexture2D<float4> RGBA16F (the blend)
inline constexpr uint32_t kBindMoments = 4;     // u4  RWTexture2D<float2> RG16F (the blend)
inline constexpr uint32_t kBindState = 5;       // u5  RWByteAddressBuffer: the state words
inline constexpr uint32_t kBindStats = 6;       // u6  RWByteAddressBuffer: the counters
inline constexpr uint32_t kSetViews = 7;        // the views the set has (D3D12 sizes a set by this count, not by the ranges)
// The trace layout: the bindless textures at space 0, the samplers at space 1, the pass set at space 2, the ray tracing set at space 3 (CAR_RT_SPACE), the push block at (b0, space 4). The blend layout:
// the pass set at space 0, the push block at (b0, space 1). The push block is one for both: 64 bytes.
inline constexpr uint32_t kTraceTextureSet = 0, kTraceSamplerSet = 1, kTracePassSet = 2, kTraceSceneSet = 3, kTracePushSpace = 4;
inline constexpr uint32_t kBlendPassSet = 0, kBlendPushSpace = 1;
inline constexpr uint32_t kPushBytes = 64;
inline constexpr uint32_t kBlendGroupSize = gi::kProbeTile;  // 10 x 10 threads: one probe's tile
inline constexpr uint32_t kUploadBytes = 512;               // the rows' upload buffer (320 bytes rounded up)

// ---------------------------------------------------------------------------
// the static checks (what the design promises, evaluated at compile time)
// ---------------------------------------------------------------------------

static_assert(UniformRays(gi::BudgetOf(gi::Layout::kLean)) && UniformRays(gi::BudgetOf(gi::Layout::kStandard)) && UniformRays(gi::BudgetOf(gi::Layout::kExtended)),
              "every cascade of a layout traces the same number of rays a probe (the trace pass packs 64 / rays probes in a group)");
static_assert(kTraceGroup % 32 == 0 && kTraceGroup % gi::BudgetOf(gi::Layout::kLean).cascade[0].rays_per_probe == 0 && kTraceGroup % gi::BudgetOf(gi::Layout::kStandard).cascade[0].rays_per_probe == 0 &&
                  kTraceGroup % gi::BudgetOf(gi::Layout::kExtended).cascade[0].rays_per_probe == 0,
              "a group of 64 threads holds a whole number of probes");
static_assert(BudgetOf(gi::BudgetOf(gi::Layout::kLean)).chunk_probes == 384 && BudgetOf(gi::BudgetOf(gi::Layout::kStandard)).chunk_probes == 7168 && BudgetOf(gi::BudgetOf(gi::Layout::kExtended)).chunk_probes == 15360,
              "a chunk is the layout's probes a frame: 384, 7,168 and 15,360");
static_assert(BudgetOf(gi::BudgetOf(gi::Layout::kStandard)).ray_buffer_bytes == gi_field::MemoryOf(gi::BudgetOf(gi::Layout::kStandard), false).update_reserve &&
                  BudgetOf(gi::BudgetOf(gi::Layout::kLean)).ray_buffer_bytes == gi_field::MemoryOf(gi::BudgetOf(gi::Layout::kLean), false).update_reserve &&
                  BudgetOf(gi::BudgetOf(gi::Layout::kExtended)).ray_buffer_bytes == gi_field::MemoryOf(gi::BudgetOf(gi::Layout::kExtended), false).update_reserve,
              "the ray buffer is exactly what the field's owner counted for the update (gi_field_rules.h kRayRecordBytes)");
static_assert(kRayBytes == gi_field::kRayRecordBytes, "a ray record is the owner's 16 bytes");
static_assert(kMaxChunks * 15360u * 4u < 0x4000000u, "the probe word's index has 26 bits");
static_assert(ProbeCascade(ProbeWord(3, 1, 16383)) == 3 && ProbeFlags(ProbeWord(3, 1, 16383)) == 1 && ProbeIndex(ProbeWord(3, 1, 16383)) == 16383, "the probe word");
static_assert(DispatchFor(0).y == 0 && DispatchFor(1).x == 1 && DispatchFor(1).y == 1 && DispatchFor(4096).x == 4096 && DispatchFor(4096).y == 1 && DispatchFor(4097).x == 4096 && DispatchFor(4097).y == 2,
              "the 2D dispatch");
static_assert(TraceGroups(7168, 64) == 7168 && TraceGroups(384, 32) == 192 && TraceGroups(385, 32) == 193, "the trace pass's groups");
static_assert(BackByte(0, 64) == 0 && BackByte(64, 64) == 255 && BackByte(20, 64) == 80, "the back face share's byte");
static_assert(gi_field::kStateBackShift == 10 && gi_field::kStateBackMask == (0xFFu << 10), "the state word's back face byte");

}  // namespace mcla::gpu::gi_update

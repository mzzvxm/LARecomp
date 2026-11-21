#pragma once
// ===========================================================================
// MCLA GPU renderer -- RT-HIT-SHADING, part two: how far a reflection reaches to another car, the sharp clear-coat ray on top of the glossy cone, and the level table
// ===========================================================================
// WHERE THIS STANDS. Part one (car_rt_material_rules.h, hlsl/car_rt_hit_shade.hlsli) shades a reflection ray's hit on another car from the car's own material: its paint, its
// textures, the sun, the environment map, the lamps at night. The owner's goal (2026-11-20) is to "clearly identify the car I am looking at through the reflection", the way Gran
// Turismo 7 does on the PS5 Pro. Four things were still between the pass and that, and this header holds their pure rules (the shaders keep their own copies of the numbers; the
// test checks that each file holds them):
//
//  1. THE REACH. A reflection ray was 4 m long (mcla_gpu_car_selfrefl_rt_length), right for the player's own parts (the wing on the lid, a mirror on the door) and far too short for a
//     car: the neighbour in the next lane is 3 m away, the car ahead 10 to 20. The ray now goes as far as the OTHER CARS reach (kHigh 18 m, kMid 12 m: Level::others_reach) when the
//     parts it hits include them (CastReach), and the weight of a hit falls with its own reach (HitFade: the player's parts over their 4 m, another car over its own, from
//     Level::others_fade_from of it). The cars the scene holds follow: ClassLimits' radius is the reach plus a car's half length (kCarHalfLength) so that a car whose far side is in
//     reach is in the scene (High 20 m, Mid 14 m: car_rt_instances_rules.h ClassLimits, which the test holds to RadiusFor).
//
//  2. THE CLEAR COAT. A car's paint is a glossy base under a clear coat: the base blurs what it reflects (the cone of rays, mcla_gpu_car_selfrefl_rt_gloss), the coat is a mirror. The
//     pass casts ONE more ray along the exact mirror direction (no cone, no filter: the texture is read at the pixel's footprint, the environment map at its sharpest level) and
//     adds it to the cone's result by a share that follows the paint's fresnel (CoatShare: kCoatNormal facing the camera up to kCoatGrazing at a grazing view, by (1 - N.V)^power with
//     the paint's own fresnel exponent). The sum is exact for the way the paint blends the image over its environment reflection (MixCoat: the two lobes' weights and premultiplied
//     colours add), so a texel is the glossy lobe by (1 - share) and the mirror lobe by share, each with its own coverage.
//     The denoise (hlsl/car_rt_refl_denoise_cs.hlsl) takes the cone's grain out; it must not blur the mirror: the trace says how much of a texel's weight is the coat's in the aux
//     image's w (EncodeAuxSharp: 1 for none down to 1 - kAuxSharpScale for all), and the denoise scales its strength by what is left (DenoiseScale).
//
//  3. THE SUN'S SHADOW (car_rt_material_rules.h SunCascades, CascadeLit, SunVisibility): the game's own cascades at the hit's world point, with the cars' ray.
//
//  4. THE LEVELS (Level, LevelFor): the High class gets everything; the Mid class (an RTX 2060 or RX 6600 class: three to four times the ray cost) a shorter reach, the outer pair of the
//     cone's rays alone and no clear-coat ray; the Low class has no ray tracing. mcla_gpu_car_rt_hit_reach and mcla_gpu_car_rt_clearcoat override the class (debug).
//
// COST (an estimate, not measured: PassTiming's "car rt refl" and "car rt refl opp", the run plan in the report): the pass's texels each cast a primary ray and 2 to 4 cone rays;
//  - the coat adds one ray to every paint texel that reaches the cone (about 215k at 4K with the half-size history), shortest of all rays (it hits a car or leaves the scene), and a
//    shaded hit where it hits another car: +0.05 to 0.12 ms on an RTX 5080 class card (a ray is about 0.1 to 0.2 microseconds there), twice that at 1080p's full-size image (the
//    image is 1080 rows either way); not on the Mid class;
//  - the longer reach makes a ray that finds nothing travel 18 m, not 4 m, in a scene of at most a few hundred instances: +0.02 to 0.04 ms; every hit on another car beyond 4 m is a
//    shaded hit (8 to 30 loads and the cascades' 9 loads): a few tens of thousands a frame, +0.01 to 0.03 ms;
//  - more cars in the scene (radius 14 -> 20 m on High): only where fewer than four stood within 14 m; a car costs one structure build when it comes (car_rt_memory_rules.h) and its
//    instances' share of the top level; +0.05 ms of the render thread's time on average in traffic.
//  In all, +0.15 to 0.3 ms on the High class at 4K (the brief's 0.1 to 0.3), less at 1080p's smaller pixel count of the player's car; the Mid class +0.05 ms for the reach alone.
//
// RUN PLAN (D3D12 only until Plume's ray query lands; the exe is built by the main session):
//  AFTER pictures: the SHOWCASE-RT chain on a new exe (its BEFORE pictures are perf\out\showcase-rt, the flat orange patch over the door in showcase-rt-day-b-zoom.png):
//    chain-showcase-rt.ps1 -Plan -Fresh -SrcRef feat/rt-hit-shading -ExeName rthit1 -TestRef test/rt-hit-shading-opt -TestWt C:\mcv\larecomp\h11-work\rt-hit-shading-opt
//    (then the same without -Plan, through chain-runq.ps1 -Chains 'chain-showcase-rt.ps1 -Fresh -SrcRef feat/rt-hit-shading -ExeName rthit1 -TestRef test/rt-hit-shading-opt -TestWt ...' -BatchName rthit1;
//     -Times day first). The pictures show the shaded flank, the sharp coat and the shade of the world on the neighbour; A/B by `set` lines in the tour or by the command line:
//    mcla_gpu_car_rt_clearcoat 0 (no coat), mcla_gpu_car_rt_hit_reach 4 (the old reach), mcla_gpu_car_rt_hit_cascade false, mcla_gpu_car_rt_hit_shading false (the old stand-in).
//  COST: exclusive runs (lockrun --lock --game, nothing else on the GPU, the display woken, Do Not Disturb), exe rthit1, mcla_gpu_pass_timing true, the same scene and tour at --window 1920x1080
//    and 3840x2160 DLAA, 600 frames each, four states: (1) all defaults, (2) mcla_gpu_car_rt_clearcoat 0, (3) mcla_gpu_car_rt_hit_reach 4 and mcla_gpu_car_rt_clearcoat 0, (4) also
//    mcla_gpu_car_rt_hit_shading false; the means of the labels "car rt refl", "car rt refl opp" and "car rt denoise" in the pass timing windows (renderer.cpp ReportFrameStats, one every 3600
//    presents): (1)-(2) is the clear coat, (2)-(3) the reach and the sun shadow, (3)-(4) the shading. Then --mcla_gpu_class=mid (the level table's Mid cell) for the Mid numbers.
//
// CHECK (nothing was run):
//   1. the cascade atlas read from the compute pass at the history trigger (its layout is what the volumetric light finds it in), the camera's world place and the axes (the shaded flank of a
//      car under a tree or a bridge should be shaded there as the road is; the log's "[car rt] materials" line counts the frames that carried the cascades);
//   2. the clear coat's share (kCoatNormal, kCoatGrazing): crops of the neighbour's door edge and lights with mcla_gpu_car_rt_clearcoat 0 and 1: crisper, no extra grain, no hard stair-steps
//      at the edge (the sharp ray is one ray a texel; the paint's 3x3 spline and the denoise's 0.51 floor smooth it);
//   3. the cost against the estimate above, and the memory of more cars in the scene (radius 20 m on High);
//   4. the mirror ray's self-hit rule on a curved panel (kSelfCos): a coat ray that skims the panel it left is a miss, so a lid or a bonnet must stay the environment's colour;
//   5. the push blocks (96 and 112 bytes) once Vulkan has ray queries (the limit is 128);
//   6. part one's lines (the units of the environment map, the paraboloid, the table's constants: car_rt_material_rules.h CHECK);
//   7. RT-HIT-SHADING-3 (the census, below): the first run's "[car rt refl] hits, frame N: ..." lines (the first at the 4th trigger, then every 300 triggers) say how many hits took the history, the
//      blend, the shading or the stand-in, by class and by the sun, and the exposure: "the model, with the gain then in force, is X stops from it": X goes to 0 and "the gain now" settles
//      (about +1 to +3 stops where the model is dark, near 0 where it fits; it must differ between the day and the night runs: the model's own light does not). Too few paint samples (the line
//      says so) means the camera saw too little of another car's paint for the exposure to move: the hits are then shaded as before, with the fill. The two switches: mcla_gpu_car_rt_hit_calibrate
//      false (no gain, the census still runs) and mcla_gpu_car_rt_hit_gain (the fixed gain). The stats frame's atomics are a frame in twenty: look for a 0.1 to 0.5 ms spike in "car rt refl" there;
//   8. the lamps' night factor is still read from the sun colour (kNightLow, kNightHigh), which was 0.97 at 14 h and 23 h alike: the census's "lamps glowing" counts the shaded hits that glow
//      with it over a half; a daylight run with a number there means the lamps glow in the sun (a follow-up: the factor from the world's hour, not from c39).
//
// This header includes nothing of the renderer; car_rt_hit_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "car_rt_material_rules.h"
#include "car_rt_refl_rules.h"
#include "gpu_class.h"

namespace mcla::gpu::car_rt_hit {

// ---------------------------------------------------------------------------
// the levels
// ---------------------------------------------------------------------------

// A car's half length, metres: a car whose centre is this much farther than the reach still has its near end in reach.
inline constexpr float kCarHalfLength = 2.0f;

struct Level {
  float others_reach = 0.0f;       // m: how far a reflection ray goes to find another car (0: no farther than the player's own parts' reach)
  float others_fade_from = 0.75f;  // the share of that reach up to which a hit on another car weighs 1
  float coat = 0.0f;               // the clear-coat ray's gain (0: no ray)
  uint32_t cone_rays = 4;          // 4: the whole cone, 2: the outer pair (hlsl: the inner two are never cast)
  bool cascade_shadow = true;      // the sun's visibility at a hit also reads the game's cascades
  bool sun_ray = true;             // ... and a ray towards the sun against the cars
  bool operator==(const Level&) const = default;
};

// The numbers by class (a cell of the table the owner asked for, 2026-11-20): High everything; Mid a shorter reach, the cone's outer pair and no coat; Low none (it has no ray tracing:
// the cell is only there for a debug override on a class that cannot ray trace).
inline Level LevelFor(gpu_class::GpuClass cls) {
  Level level;
  switch (cls) {
    case gpu_class::GpuClass::kHigh:
      level.others_reach = 18.0f;
      level.others_fade_from = 0.75f;
      level.coat = 1.0f;
      level.cone_rays = 4;
      break;
    case gpu_class::GpuClass::kMid:
      level.others_reach = 12.0f;
      level.others_fade_from = 0.7f;
      level.coat = 0.0f;
      level.cone_rays = 2;
      break;
    case gpu_class::GpuClass::kLow:
      break;
  }
  return level;
}

// The class's level with the two debug overrides: `reach_override` above 0 replaces the others' reach (clamped to the ray's longest), `coat_override` below 0 keeps the class's, 0 turns the
// coat off and above 0 turns it on at that gain (even on a class that has none: a way to look at it).
inline Level LevelWith(Level level, double reach_override, double coat_override) {
  if (reach_override > 0.0) {
    level.others_reach = std::clamp(float(reach_override), car_rt_refl::kMinRayLength, car_rt_refl::kMaxRayLength);
  }
  if (coat_override == 0.0) {
    level.coat = 0.0f;
  } else if (coat_override > 0.0) {
    level.coat = float(std::min(coat_override, 4.0));
  }
  return level;
}

// The distance a ray is cast: the player's own parts' reach, or the others' where the parts the ray may hit include another car (car_rt_refl_rules.h kPartOthers).
inline float CastReach(uint32_t parts, float self_reach, float others_reach) {
  return (parts & car_rt_refl::kPartOthers) != 0 ? std::max(self_reach, others_reach) : self_reach;
}

// The weight of a hit by its distance along the ray: 1 up to `fade_from` of the hit part's own reach, a smoothstep down to 0 at the reach (car_rt_refl_rules.h kFadeFrom for the
// player's parts, Level::others_fade_from for another car).
inline float HitFade(float distance, float reach, float fade_from) {
  if (!(reach > 0.0f)) {
    return 0.0f;
  }
  const float lo = fade_from * reach;
  const float t = std::clamp((distance - lo) / std::max(reach - lo, 1e-6f), 0.0f, 1.0f);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// How far from the player's vehicle another car may be to be in the scene for a reach: the reach and a car's half length.
inline double RadiusFor(float others_reach) { return double(others_reach) + double(kCarHalfLength); }

// ---------------------------------------------------------------------------
// the clear coat
// ---------------------------------------------------------------------------

inline constexpr float kCoatNormal = 0.45f;    // the coat's share of the paint's reflection where the paint faces the camera
inline constexpr float kCoatGrazing = 0.85f;   // ... at a grazing view
inline constexpr float kCoatMax = 0.9f;        // the most any gain gives it: the cone is always a little of the texel
inline constexpr float kCoatMinPower = 1.0f;   // the paint's fresnel exponent is read from its own constants (a record with none keeps the class's 4 to 5): never under this

// The share of a texel's reflection that is the clear coat's mirror: kCoatNormal to kCoatGrazing by the paint's own fresnel curve, (1 - N.V)^power, times the level's gain.
inline float CoatShare(float nv, float fresnel_power, float gain) {
  if (!(gain > 0.0f)) {
    return 0.0f;
  }
  const float curve = std::pow(std::clamp(1.0f - nv, 0.0f, 1.0f), std::max(fresnel_power, kCoatMinPower));
  return std::clamp(gain * (kCoatNormal + (kCoatGrazing - kCoatNormal) * curve), 0.0f, kCoatMax);
}

// One lobe's answer for a texel: its weight (0..1, the coverage by the rays that hit, each by its fade and visibility), its mean colour, and the part of the weight that is on
// parts the camera sees (for the code's sign: hit or hidden).
struct Lobe {
  float weight = 0.0f;
  float colour[3] = {};
  float visible_weight = 0.0f;
};

// The two lobes added: the glossy cone by (1 - share), the coat by share. Exact for the paint's blend (lerp(environment, colour, weight)): the weights and the weights' products with
// the colours (the premultiplied colours) are linear, and the reflection is environment + (1 - share) w_c (c_c - environment) + share w_s (c_s - environment). `sharp` is the part of the
// result's weight that is the coat's (the aux image's w says it to the denoise).
inline Lobe MixCoat(const Lobe& cone, const Lobe& coat, float share, float* sharp) {
  Lobe out;
  const float a = std::clamp(share, 0.0f, 1.0f);
  out.weight = (1.0f - a) * cone.weight + a * coat.weight;
  out.visible_weight = (1.0f - a) * cone.visible_weight + a * coat.visible_weight;
  if (out.weight > 0.0f) {
    for (int c = 0; c < 3; ++c) {
      out.colour[c] = ((1.0f - a) * cone.weight * cone.colour[c] + a * coat.weight * coat.colour[c]) / out.weight;
    }
  }
  if (sharp) {
    *sharp = out.weight > 0.0f ? std::clamp(a * coat.weight / out.weight, 0.0f, 1.0f) : 0.0f;
  }
  return out;
}

// The aux image's w: 1 where the texel has no coat (as before), down to 1 - kAuxSharpScale where all of its weight is the coat's (it stays over 0.5: "has data" for the denoise's test).
inline constexpr float kAuxSharpScale = 0.49f;
inline float EncodeAuxSharp(float sharp) { return 1.0f - kAuxSharpScale * std::clamp(sharp, 0.0f, 1.0f); }
inline float DecodeAuxSharp(float w) { return std::clamp((1.0f - w) / kAuxSharpScale, 0.0f, 1.0f); }
// What the denoise's strength is multiplied by: the cone's part of the texel is denoised, the mirror's is left alone.
inline float DenoiseScale(float w) { return 1.0f - DecodeAuxSharp(w); }

// ---------------------------------------------------------------------------
// the push block's packed word
// ---------------------------------------------------------------------------

// slots.w of the push: the cone pattern's phase (0..7) in the low byte, the cone's rays (1 to 4) from bit 8.
inline uint32_t PackPhaseRays(uint32_t phase, uint32_t rays) { return (phase & 7u) | (std::clamp<uint32_t>(rays, 1u, 4u) << 8); }
inline uint32_t PhaseOf(uint32_t word) { return word & 7u; }
inline uint32_t RaysOf(uint32_t word) {
  const uint32_t rays = (word >> 8) & 7u;
  return rays == 0 ? 4u : std::min(rays, 4u);  // a word with no rays in it is the whole cone (a host that does not say)
}

// ---------------------------------------------------------------------------
// RT-HIT-SHADING-3: the census of the hits and the exposure of the shaded colour
// ---------------------------------------------------------------------------
// WHAT THE FIRST AFTER PICTURES SHOWED (the rthit1 runs of 2026-11-21, ports\run\rthit1-rt-day and -night): the shading was in the exe and on (the log's reach line, "clear-coat ray: on", the
// materials census: every textured instance found in the cache, the cascades in every frame) and the Camaro's hidden flank WAS shaded (the window, the wheel arch and the body tell apart
// under a x6 exposure), but it read as a dull flat orange patch for three reasons the numbers below answer:
//   - the light it was shaded with does not follow the world: the cars' draws' c39 (the "sun") was (0.060 0.044 0.024) at 14 h and (0.060 0.048 0.020) at 23 h, the night factor 0.97 in both, and
//     the added reflection was 75% as bright at night (0.0077) as in the day (0.0103) while the picture was three times darker (the volumetric light scales the same colour by the sun's
//     height itself: volumetrics.cpp FillDefaults). The pass cannot know the exposure of the scene from the constants it reads;
//   - the ambient was c26's (0.000 0.004 0.004): no red at all, so a flank the sun does not reach was black or a teal tint under an orange paint;
//   - a body panel has no texture in this game (the paint is the colour, the gradient and the livery, car_rt_material_rules.h), so the structure of a flank is its light, its shadow and its
//     parts (windows, wheels, lamps), all of which were dark.
// THE FIX has two parts. (1) A sky fill: the ambient is at least kFillShare of the sun's colour (a shaded flank is a fifth as bright as a lit one, in the sun's own hue). (2) The EXPOSURE is
// measured against the game's own picture: wherever a ray hits a part of another car's PAINT that the history shows in full, the pass also shades it by the model and adds the log of the
// two colours' brightness to a counter (the sum of log2 of the history's, and of the model's); the host reads the counters back, turns the mean difference into a gain on the model
// (GainOf, smoothed, in the frame's rows) and the hits the history does not show are shaded with it. The same counters are the CENSUS of the pass: how many hits took the history,
// the blend, the shading or the stand-in, by class, in the sun or in a shadow, and the brightness of the shaded colour, so that a run's log says what the pass did.
namespace census {

// The counter words (the shader's own copy, hlsl/car_rt_refl_cs.hlsl kStat*): 32 words of the stats buffer (u2 of the output set), zeroed by the host before a stats frame.
inline constexpr uint32_t kWords = 32;
inline constexpr uint32_t kBytes = kWords * 4;
inline constexpr uint32_t kTexels = 0;       // texels with data: a primary hit on the player's car at the pixel's distance and a view that is not grazing
inline constexpr uint32_t kRays = 1;         // rays (the cone's and the coat's) that hit a part of the scene within their reach
inline constexpr uint32_t kOwn = 2;          // ... on the player's own parts
inline constexpr uint32_t kSeen = 3;         // ... on another car, the history shows the part in full: its colour
inline constexpr uint32_t kBlend = 4;        // ... on another car, the history shows part of it: shaded and blended
inline constexpr uint32_t kShaded = 5;       // ... on another car, the history shows none of it: shaded from its material
inline constexpr uint32_t kStandIn = 6;      // ... on another car, not shaded (no material, shading off, no frame light): the stand-in colour of the car
inline constexpr uint32_t kDropped = 7;      // ... on another car, hidden and no colour known: a miss
inline constexpr uint32_t kByClass = 8;      // 8 words: the shaded hits (blended and shaded) by class (none, paint, glass, tyre, chrome, light, interior, other)
inline constexpr uint32_t kLogLum = 16;      // the sum over the shaded hits of FixedLog2 of the shaded colour's luminance (in the history's units)
inline constexpr uint32_t kLit = 17;         // shaded hits the sun reaches (N.L > 0 and the visibility over a half)
inline constexpr uint32_t kShadow = 18;      // ... that face the sun and are in shadow (the cascades' or the cars')
inline constexpr uint32_t kAway = 19;        // ... that face away from the sun
inline constexpr uint32_t kGlow = 20;        // ... that glow (a lamp, a lit interior) with the night factor over a half
inline constexpr uint32_t kCalCount = 21;    // the calibration's samples: hits on another car's PAINT that the history shows in full (and both colours are over kCalFloor)
inline constexpr uint32_t kCalSeen = 22;     // the sum of FixedLog2 of the history's luminance there
inline constexpr uint32_t kCalModel = 23;    // the sum of FixedLog2 of the model's luminance for the same hits
inline constexpr uint32_t kAllCalCount = 24; // the same for a hit of any class (not used for the gain: the census says how the other classes' models do)
inline constexpr uint32_t kAllCalSeen = 25;
inline constexpr uint32_t kAllCalModel = 26;
inline constexpr uint32_t kCoat = 27;        // texels whose clear-coat ray hit another car
inline constexpr uint32_t kClassCount = 8;

// The push's slots.w bit that makes this frame a stats frame (the shader counts only then); bits 0..2 are the cone pattern's phase, 8..10 the cone's rays.
inline constexpr uint32_t kStatsFlag = 1u << 16;
// A stats frame every this many history triggers (the counters cost atomics on a few dozen addresses: a frame in twenty), the first at kStatsFirst.
inline constexpr uint64_t kStatsEvery = 20;
inline constexpr uint64_t kStatsFirst = 4;
// The line in the log: every this many stats frames.
inline constexpr uint64_t kLogEveryStats = 15;

// A luminance as a fixed-point log: (log2(max(l, kLogFloor)) + kLogBias) x kLogScale, clamped to 0 .. kLogMax (a sum over a million rays stays under 2^32; a step is a sixty-fourth of a stop).
inline constexpr float kLogFloor = 1e-4f;
inline constexpr float kLogBias = 16.0f;
inline constexpr float kLogScale = 64.0f;
inline constexpr uint32_t kLogMax = 2047;
inline uint32_t FixedLog2(float luminance) {
  const float v = (std::log2(std::max(luminance, kLogFloor)) + kLogBias) * kLogScale;
  return uint32_t(std::clamp(v, 0.0f, float(kLogMax)));
}
inline float FromFixedLog2(float mean_fixed) { return std::exp2(mean_fixed / kLogScale - kLogBias); }

// The calibration's samples count only where both colours are over this (in the history's units): a black part (glass, a tyre in shade) has no brightness to compare.
inline constexpr float kCalFloor = 0.02f;
inline constexpr uint32_t kCalMinSamples = 48;  // fewer samples in a window and the gain stays where it was
inline constexpr float kCalMinStops = -3.0f;    // the gain's range, in stops (a factor of 1/8 to 8)
inline constexpr float kCalMaxStops = 3.0f;
inline constexpr float kCalRate = 0.5f;         // the share of the way to the new measurement a window moves the gain (in stops)
inline constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};
inline float Luminance(const float rgb[3]) { return kLuma[0] * rgb[0] + kLuma[1] * rgb[1] + kLuma[2] * rgb[2]; }

// A window's counters (64 bits: several frames' sums).
struct Totals {
  uint64_t w[kWords] = {};
  uint64_t operator[](uint32_t i) const { return w[i]; }
};

// A frame's counters added into a window's.
inline void Add(Totals* sum, const uint32_t words[kWords]) {
  for (uint32_t i = 0; i < kWords; ++i) {
    sum->w[i] += words[i];
  }
}

// The mean of the log2 luminance difference (history over model) of a calibration's samples in stops, and whether there were enough of them to say.
inline bool CalibrationStops(uint64_t count, uint64_t seen_sum, uint64_t model_sum, float* stops) {
  if (count < kCalMinSamples) {
    return false;
  }
  const float diff = (float(double(seen_sum) - double(model_sum)) / float(count)) / kLogScale;
  *stops = std::clamp(diff, kCalMinStops, kCalMaxStops);
  return true;
}

// The new gain (in stops) from the old one and a window's measurement. The shader shades the comparison's hits WITH the gain in force, so the measurement is what is left to correct (the
// history's over the model's, in stops): the gain moves kCalRate of the way to cancel it, so it settles where the measurement is 0 and follows the hour as it changes.
inline float NextStops(float previous, float residual) { return std::clamp(previous + residual * kCalRate, kCalMinStops, kCalMaxStops); }
inline float GainOf(float stops) { return std::exp2(std::clamp(stops, kCalMinStops, kCalMaxStops)); }

// Is the trigger at `frame` a stats frame? (the first at kStatsFirst, then every kStatsEvery after the last)
inline bool StatsDue(uint64_t frame, uint64_t last_stats_frame, bool ever) { return ever ? frame >= last_stats_frame + kStatsEvery : frame >= kStatsFirst; }

// The word of the push for a stats frame: the cone pattern's phase and rays as PackPhaseRays has them, and the flag.
inline uint32_t PackWithStats(uint32_t phase_rays_word, bool stats) { return phase_rays_word | (stats ? kStatsFlag : 0u); }

// The census line's text for a window of stats frames.
inline unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }
inline std::string Text(const Totals& t, uint32_t frames, float gain_stops, bool calibrated) {
  static const char* const kClassName[kClassCount] = {"none", "paint", "glass", "tyre", "chrome", "light", "interior", "other"};
  const uint64_t other = t[kSeen] + t[kBlend] + t[kShaded] + t[kStandIn] + t[kDropped];
  const uint64_t shaded = t[kBlend] + t[kShaded];
  char head[640];
  std::snprintf(head, sizeof(head),
                "%llu stats frames: %llu texels with data, %llu rays hit a part (%llu on the player's own parts, %llu on other cars: %llu took the history's colour, %llu shaded and blended with it, %llu shaded, "
                "%llu the stand-in colour, %llu dropped); %llu texels' coat ray hit another car",
                ull(frames), ull(t[kTexels]), ull(t[kRays]), ull(t[kOwn]), ull(other), ull(t[kSeen]), ull(t[kBlend]), ull(t[kShaded]), ull(t[kStandIn]), ull(t[kDropped]), ull(t[kCoat]));
  std::string out = head;
  out += "; shaded by class:";
  for (uint32_t c = 0; c < kClassCount; ++c) {
    if (t[kByClass + c] != 0) {
      char part[64];
      std::snprintf(part, sizeof(part), " %s %llu", kClassName[c], ull(t[kByClass + c]));
      out += part;
    }
  }
  char tail[640];
  const float mean_lum = shaded != 0 ? FromFixedLog2(float(t[kLogLum]) / float(shaded)) : 0.0f;
  std::snprintf(tail, sizeof(tail),
                "; the sun: %llu lit, %llu in shadow, %llu facing away; %llu lamps glowing with the night factor over a half; the shaded colour's geometric mean luminance %.4f (the history's units)",
                ull(t[kLit]), ull(t[kShadow]), ull(t[kAway]), ull(t[kGlow]), mean_lum);
  out += tail;
  float stops = 0.0f;
  char cal[512];
  if (CalibrationStops(t[kCalCount], t[kCalSeen], t[kCalModel], &stops)) {
    std::snprintf(cal, sizeof(cal), "; exposure against the game's own picture: %llu paint hits the history shows (%llu of any class), the model, with the gain then in force, is %+.2f stops from it (history over model), the gain now %+.2f stops (x%.2f)%s",
                  ull(t[kCalCount]), ull(t[kAllCalCount]), stops, gain_stops, GainOf(gain_stops), calibrated ? "" : " (not applied: the calibration is off)");
  } else {
    std::snprintf(cal, sizeof(cal), "; exposure against the game's own picture: %llu paint hits the history shows (too few: the gain stays %+.2f stops, x%.2f)", ull(t[kCalCount]), gain_stops, GainOf(gain_stops));
  }
  out += cal;
  return out;
}

}  // namespace census

}  // namespace mcla::gpu::car_rt_hit

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
//   6. part one's lines (the units of the environment map, the paraboloid, the table's constants: car_rt_material_rules.h CHECK).
//
// This header includes nothing of the renderer; car_rt_hit_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

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

}  // namespace mcla::gpu::car_rt_hit

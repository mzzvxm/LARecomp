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
#include "rt_hit_calibration.h"
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
// another car (RT-HIT-SHADING-4: for the code's sign, which the paint reads as "add the clear coat's layer"; it said "on parts the camera sees" before).
struct Lobe {
  float weight = 0.0f;
  float colour[3] = {};
  float car_weight = 0.0f;
};

// The two lobes added: the glossy cone by (1 - share), the coat by share. Exact for the paint's blend (lerp(environment, colour, weight)): the weights and the weights' products with
// the colours (the premultiplied colours) are linear, and the reflection is environment + (1 - share) w_c (c_c - environment) + share w_s (c_s - environment). `sharp` is the part of the
// result's weight that is the coat's (the aux image's w says it to the denoise).
inline Lobe MixCoat(const Lobe& cone, const Lobe& coat, float share, float* sharp) {
  Lobe out;
  const float a = std::clamp(share, 0.0f, 1.0f);
  out.weight = (1.0f - a) * cone.weight + a * coat.weight;
  out.car_weight = (1.0f - a) * cone.car_weight + a * coat.car_weight;
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
// RT-HIT-SHADING-4: a reflected car that can be told apart (the mirror decides; the clear coat's layer)
// ---------------------------------------------------------------------------
// WHAT THE rthit2 PICTURES SHOWED (2026-11-21, perf\out\showcase-rt\showcase-rt-day-b-*, ports\run\rthit2-rt-day; the numbers are read off the 4K pictures in linear light, and from the code):
//   the census says the hits ARE shaded (1.06 M took the history's colour, 1.38 M were shaded, 0.06 M blended), yet the door showed "a dim, blurry orange tint". Three numbers say why.
//   1. THE WEIGHT IS NOT THE LIMIT. Fitting the ON picture against the OFF one over the door (luminance, cells of 150 px) gives a slope of 0.07 to 0.18 where the car is reflected: 82 to 93 % of
//      the game's environment reflection of the buildings is replaced (the pass's weight of 0.85 to 1; the paint's own fresnel then scales the car as it scales the environment).
//   2. THE AMPLITUDE. What replaces it goes through the paint's own reflectance for the environment, which at the door's grazing view (N.V 0.2 to 0.4) is 1 to 2 percent of what is reflected
//      (the buildings: 0.0024 of 0.1 to 0.3; the sky: 0.006 of 0.3 to 0.4). The neighbour's flank in the shade of the player's own car is a quarter of its sunlit fender's radiance (the lower flank
//      0.036 against the fender 0.19 in the same picture), so the car lands at L 0.0013 (8-bit codes 9, 3, 3) where the buildings it replaced were at 0.0049 (codes 8 to 25): over the pixels the
//      neighbour changes (the pass rule's pixels, below) the median luminance is 0.29 of the environment's it removed by day (the means 0.44) and 0.22 at night (0.18). A silhouette against the sky's reflection is a step of 0.0005 of
//      luminance: under two levels of an 8-bit picture. Nothing in the ray tracing is wrong; the paint's reflection of a car is as weak as its reflection of a building, and a shaded flank is darker.
//   3. THE BLUR. A texel's reflection is the glossy cone (a half angle of 2.5 degrees: 4 rays, mcla_gpu_car_selfrefl_rt_gloss) by (1 - share) and the clear coat's mirror ray by share, and the
//      share is 0.45 facing the camera to 0.85 at a grazing view by (1 - N.V)^power: 0.52 to 0.58 at the door. So 42 to 48 % of what the door shows of the car is the cone's, and on a flat panel
//      a cone of 2.5 degrees is a blur of about 2.5 x 57 to 101 px = 140 to 250 px of the 4K picture (57 to 101 px a degree at the cameras' field of view of 38): the wheel arch, the windows and
//      the rims of the neighbour are smaller than that, and its colour leaks over the door past its outline (the diff of the player's paint with the others on against off covers the whole door,
//      the car a part of it). The game's environment map, for comparison, is 512 x 256 over a hemisphere: 0.35 degrees a texel, about 20 px at 4K.
// THE FIX HAS TWO PARTS, both for a hit on ANOTHER CAR only (the player's own parts keep the soft cone and the paint's reflectance: that look stays):
//   (1) THE MIRROR DECIDES (MirrorShare, ConeHitCounts): where the clear coat's exact mirror ray hits another car, a car is what the door shows there, as sharp as a mirror: the mirror's share of
//       the texel is at least kOthersSharpShare (0.9: the base under a clear coat is dark, and a black paint has no glossy reflection of its own to speak of); where the mirror ray did not hit
//       another car, the cone's rays that did are dropped (no colour leaking past the outline: the 140 to 250 px haze). A level with no mirror ray (the Mid class) keeps the cone alone.
//   (2) THE CLEAR COAT'S LAYER (CoatLayerFresnel, CoatLayerAmount): the paint adds, on top of its own colour, the reflection of the car at the clear coat's own fresnel (Schlick, F0 0.04, power 5:
//       0.07 at N.V 0.5, 0.20 at 0.3, 0.36 at 0.2), by the share of the texel's weight that is on another car: lerp(paint, the car's colour, amount). The paint's own path stays (the environment
//       replaced as before), so the player's own reflections and a car out of reach are untouched. That is a clear coat as a dielectric layer over the paint, which the game's paint has no term for; it is
//       what makes the door read the neighbour at the strength a real black door shows at 12 to 25 degrees off its plane (about 10 times the paint's own). The picture carries it in the code's SIGN:
//       alpha -(1 + w) is "mostly on another car" (it was "mostly on parts the camera does not see", which the paint only drew in a debug view).
// THE PASS/RULE PAIR: car_rt_refl_cs.hlsl and car_rt_refl_opp.hlsli (MirrorShare, ConeHitCounts, the sign), car_rt_hit_shade.hlsli (their helpers); the paint: shaderpack/variants/car_selfrefl.cpp (version 8:
// CarReflCoat, g_CarReflC.w bit 1 = kCoatLayerFlag of car_selfrefl_rules.h, debug views 6 and 7). Switch: mcla_gpu_car_rt_coat_layer (debug).
// PASS RULE of the rerun (perf\scripts\showcase_rt_post.py, rows `bright`, `contrast`, `sharp`, verdict RT-HIT-SHADING-4; camera b, day and night, FAILS on any): over the pixels the neighbour changes (the player's paint with the
// others on against off, outside the control's noise) (1) the median linear luminance of the player picture is at least 1.0 times the off picture's by day, 0.7 by night (the lamps' glints are in the night's environment; rthit2: 0.29 day, 0.22 night; means 0.44, 0.18), (2) the
// coefficient of variation of the luminance over the interior of those pixels is at least 1.0 times the off picture's (the buildings': rthit2 0.68, 0.47), (3) in the picture of the debug view 6 (the layer's amount) the car's
// edge goes from a fifth to four fifths of its level over at most 30 px (the cone of rthit2 by the model of this header's test: 190 to 340 px).

// The mirror's least share of a texel whose mirror ray hit another car.
inline constexpr float kOthersSharpShare = 0.9f;
// The share of a texel's reflection that is the mirror's: the class's CoatShare, and at least kOthersSharpShare where the mirror ray hit another car.
inline float MirrorShare(float coat_share, bool mirror_on_car) {
  return mirror_on_car ? std::max(coat_share, kOthersSharpShare) : coat_share;
}
// Does a cone ray's hit count? A hit on another car counts only where the mirror ray hit another car too (`have_mirror`: the level casts one); everything else counts as before.
inline bool ConeHitCounts(bool cone_on_car, bool have_mirror, bool mirror_on_car) {
  return !cone_on_car || !have_mirror || mirror_on_car;
}

// The clear coat's layer: Schlick's fresnel of a dielectric with the index 1.5 (F0 0.04, power 5), at the paint's own N.V, and the most of the paint it may take over.
inline constexpr float kCoatLayerF0 = 0.04f;
inline constexpr float kCoatLayerPower = 5.0f;
inline constexpr float kCoatLayerMax = 0.6f;
inline float CoatLayerFresnel(float nv) {
  return std::min(kCoatLayerF0 + (1.0f - kCoatLayerF0) * std::pow(std::clamp(1.0f - nv, 0.0f, 1.0f), kCoatLayerPower), kCoatLayerMax);
}
// How much of the paint's colour the layer takes: the fresnel by the weight of the hit and the share of it that is on another car.
inline float CoatLayerAmount(float nv, float weight, float car_share) {
  return std::clamp(weight, 0.0f, 1.0f) * std::clamp(car_share, 0.0f, 1.0f) * CoatLayerFresnel(nv);
}
// The layer over the paint's colour (one channel): lerp(paint, car, amount).
inline float CoatLayerMix(float paint, float car_colour, float amount) { return paint + (car_colour - paint) * amount; }

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

// The fixed point log, the calibration's range and its arithmetic are shared with the city hit library (rt_hit_calibration.h: CITY-RT M2 H4); the numbers are the car's, unchanged.
// A luminance as a fixed-point log: (log2(max(l, kLogFloor)) + kLogBias) x kLogScale, clamped to 0 .. kLogMax (a sum over a million rays stays under 2^32; a step is a sixty-fourth of a stop).
inline constexpr float kLogFloor = rt_hit_cal::kLogFloor;
inline constexpr float kLogBias = rt_hit_cal::kLogBias;
inline constexpr float kLogScale = rt_hit_cal::kLogScale;
inline constexpr uint32_t kLogMax = rt_hit_cal::kLogMax;
inline uint32_t FixedLog2(float luminance) { return rt_hit_cal::FixedLog2(luminance); }
inline float FromFixedLog2(float mean_fixed) { return rt_hit_cal::FromFixedLog2(mean_fixed); }

// The calibration's samples count only where both colours are over this (in the history's units): a black part (glass, a tyre in shade) has no brightness to compare.
inline constexpr float kCalFloor = rt_hit_cal::kCarParams.floor;
inline constexpr uint32_t kCalMinSamples = rt_hit_cal::kCarParams.min_samples;  // fewer samples in a window and the gain stays where it was
inline constexpr float kCalMinStops = rt_hit_cal::kCarParams.min_stops;         // the gain's range, in stops (a factor of 1/8 to 8)
inline constexpr float kCalMaxStops = rt_hit_cal::kCarParams.max_stops;
inline constexpr float kCalRate = rt_hit_cal::kCarParams.rate;                  // the share of the way to the new measurement a window moves the gain (in stops)
inline constexpr const float(&kLuma)[3] = rt_hit_cal::kLuma;
inline float Luminance(const float rgb[3]) { return rt_hit_cal::Luminance(rgb); }

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
inline bool CalibrationStops(uint64_t count, uint64_t seen_sum, uint64_t model_sum, float* stops) { return rt_hit_cal::Stops(rt_hit_cal::kCarParams, count, seen_sum, model_sum, stops); }

// The new gain (in stops) from the old one and a window's measurement. The shader shades the comparison's hits WITH the gain in force, so the measurement is what is left to correct (the
// history's over the model's, in stops): the gain moves kCalRate of the way to cancel it, so it settles where the measurement is 0 and follows the hour as it changes.
inline float NextStops(float previous, float residual) { return rt_hit_cal::NextStops(rt_hit_cal::kCarParams, previous, residual); }
inline float GainOf(float stops) { return rt_hit_cal::GainOf(rt_hit_cal::kCarParams, stops); }

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

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the chrome of the OTHER cars under their own bodies, ray traced: the rules that need no renderer
// ===========================================================================
// WHAT WAS THERE. The chrome variant (shaderpack/variants/car_chrome.h, car_chrome_rules.h) darkens the sky's reflection and the sky's ambient colour of a
// chrome pixel where the car's own parts cover it (a tailpipe under a bumper), with two centimetre probes marched through the history image of the car
// (car_selfrefl.h: every vehicle's pixels of the frame before, distance in alpha) or, where the frame has the ray traced image (car_rt_refl.h), with the
// reflection probe's weight read from it. The ray traced image only knows the PLAYER'S car (its primary ray hits the player's body and nothing else), and
// its sky probe is always marched. The variant is chosen by the shader (pipelines.cpp WantedFeatures: xChrome::PS_Chrome, whoever draws it) and its constants by the
// draw's stencil ID, whose pixels are in the history: so another car's chrome has been taking the march all along. This is the ray traced version for it.
//
// WHAT THIS ADDS (where ray tracing runs: the RAY TRACED setting's devices, the car self-reflection on, mcla_gpu_car_rt_cars above 0, the other cars in the
// top level of car_rt_scene.h: the nearest 2 within 10 m on Mid, 4 within 14 m on High):
//  1. The instance of another car's draw of the chrome's shader carries a second mask bit, kMaskOtherChrome 0x20 (car_rt_instances_rules.h OtherMask), beside
//     kMaskOthers 0x10, so every ray that hits another car still hits it (the reflection's 0x17, the occlusion's 0x15, the check's 0xFF).
//  2. The reflection pass (hlsl/car_rt_refl_cs.hlsl) gives its primary ray the mask kPrimaryTraceParts (the player's body 0x01 and 0x20): a texel of the
//     history whose pixel is another car's chrome is found at its distance, with the exact point and the mesh's normal, as the player's paint is. Another
//     car's paint (0x10 only) and the player's glass, tyres and driver are not found: those texels have no data, as before, and the paint keeps its march.
//  3. For such a texel the pass traces the chrome's two probes, as real rays against the top level, with the mask kProbeParts (every part of another car):
//     the REFLECTION probe, one ray along the mirror direction, 0.25 m; the SKY probe, four rays (the centre, along normalize(n + up) with up the WORLD's up
//     in view space, and three on a cone of tan 0.6 around it), 0.20 m, the share of them that hit. The numbers are the march's (car_chrome_rules.h): the
//     same reaches, and a hit weighs 1 up to 0.3 of the reach and falls to 0 at 0.9 of it. A hit on the point's own triangle, or on its own instance on a
//     surface that faces the way the point's normal does, is the panel itself and a miss (the reflection pass's own rule, car_rt_refl_rules.h IsSelfHit).
//  4. The texel is written in the ray traced image as a code the player's path cannot mistake: alpha 1 + the reflection's weight (the code of a hit of
//     that weight, as ever) and the COLOUR's red = -(1 + the sky's weight), a value no colour has (colours are never negative). The aux image gets
//     (0, 1, 0, 0): "no aux", so the denoise copies the texel as it is and no player's texel takes it for its instance's (instance 255 does not exist).
//     The paint variant reads the same image: for a texel of this kind it takes the reflection's weight (small: a centimetre ray) and a colour of
//     max(negative, 0) = black, which is at most a faint darkening on the paint of another car next to its own chrome (the paint's read answers there only
//     where the chrome's texels are at least half of the footprint); CHECK in a picture.
//  5. The chrome variant (version 2) reads the image as before, in two classes: texels whose red is not negative are the player's (the arithmetic is
//     exactly the old one: ReadForChrome's `use` and `reflect` equal car_rt_refl::ReadForPaint's weight on every image without a chrome texel, the test
//     checks it), and texels whose red is under -0.5 are another car's chrome: where they are at least half of the footprint, the image answers BOTH probes
//     (reflect from the alpha, sky from the red) and the march is not made. Anywhere else (no top level, the car beyond the radius or the budget, the
//     chrome's mesh not built yet, a pixel the primary ray did not match) the march runs as it did. Debug view 4 shows these pixels cyan.
//
// WHERE IT DOES NOT RUN, the cheap fallback, decided: nothing new, the march of the variant through the history image as it is today, because (a) it already
// runs for another car's chrome (see above), so nothing is added to its cost on any class; (b) the other versions weighed are worse: a march through the DEPTH BUFFER
// is what the history image is (its alpha is the vehicles' view distance made from the depth plane, the only picture of the cars there is while they draw; the
// world's depth would add nothing under a bumper, where only the car's own parts cover), and a FIXED DARKENING under a car's body box would darken tailpipes in
// the open sun (the look must stay subtle there: an open tip reads open in the probes, a box has no idea). On the Low class nothing changes: it has no other
// cars in the scene (car_rt_instances_rules.h ClassLimits) and no ray traced pass; it keeps the march for every car, whatever it does today.
//
// COST. Texels of other cars' non-chrome parts cost what they did (the primary ray's mask skips their instances, as before); a chrome texel's primary ray now
// HITS (it missed) and it casts five short rays against a tree of at most a few hundred instances. A car has 4 to 8 chrome draws (tips, badges, trim), at 1080p and 10 m
// some 100 to 300 texels a car (history size), so 2 to 4 cars add 0.2 to 1.2 thousand texels and about 5 thousand rays of at most 25 cm to the pass: far
// under a hundredth of a millisecond on any card that can run it, Mid class included. The variant's read grows by one class in its 9 taps, only for chrome
// pixels with a ray traced frame. The structures are the same (the instance's mask is the only difference). CHECK: pass timing "car rt refl", with
// mcla_gpu_car_chrome_rt_others 0 and 1, against the cars on the grid.
//
// TEST PLAN (main session; the tours ports\scripts\tours\rt-grid.tour (point A: the side camera, shots side-*; point B: the chase camera, shots chase-*) and rt-roam.tour
// (live-* at the carfx spot, pause-* behind the pause menu: the pairs of one frame); D3D12 on the RTX 5080 (High), then Vulkan, then --mcla_gpu_class=mid (2 cars). The
// tours have no chrome set yet: each camera point and the roam's parts need these shots added (all after SETTLE_S, mcla_gpu_car_selfrefl auto, selfrefl_rt auto, rt_cars -1):
//   chrome-game    set mcla_gpu_car_chrome_occlusion 0                                    the game's chrome: the opponents' tips glow (the baseline, the complaint)
//   chrome-march   set mcla_gpu_car_chrome_occlusion 1 ; set mcla_gpu_car_chrome_rt_others 0   what another car's chrome gets today with ray tracing on (the march): the A
//   chrome-rt      set mcla_gpu_car_chrome_occlusion 1 ; set mcla_gpu_car_chrome_rt_others 1   the B (the default)
//   chrome-view4   ... ; set mcla_gpu_car_selfrefl_view 4                                  the state at the chrome's pixels: CYAN on the opponents' tips (the image answered both probes),
//                                                                                          blue on the player's, red where the point is not in the history (a wrong guess of the car's
//                                                                                          motion: the game's chrome there in all three), green / yellow the march
//   chrome-view1 / chrome-view2   ... ; set mcla_gpu_car_selfrefl_view 1 / 2               the reflection and the sky probe's weights alone: bright under the bumper, black in the open
// Each as a crop on the nearest opponent's tailpipes at 1080p and at 4K (the neighbour 3 to 4 m beside the player's flank at side-*, the car ahead at 7 to 12 m at chase-*, a
// passing car at 6 to 15 m at live-* in high sun). The log: "[car rt] ... (of the others' N are draws of the chrome's shader ...)" with N above 0 when an opponent is in the
// top level, "[car rt refl] the first ray traced pass ... the other cars' chrome: traced", pass timing "car rt refl" with rt_others 0 and 1.
// PASS: chrome-rt is darker than chrome-game under an opponent's bumper and is not worse than chrome-march (no frame to frame flicker: the probes are exact); chrome-rt equals
// chrome-game on a tip in the open sun (the probes find nothing within 25 cm: the two probes' weights are exactly 0, view 1 and 2 black there); chrome-view4 shows cyan on the
// tips that chrome-rt darkens; the opponent's paint beside its chrome is the same in chrome-march and chrome-rt (the new texels must not rim it); "car rt refl" is at most 0.02 ms
// more than with rt_others 0 (High) and 0.05 ms (Mid); the player's own tips are the same picture in all three; no validation error on Vulkan; Low (--mcla_gpu_class=low) has
// no pass and no change.
//
// The pure numbers and rules below are mirrored in hlsl/car_rt_refl_cs.hlsl and shaderpack/variants/car_chrome.cpp (which cannot include this header):
// change one, change the other; car_chrome_rt_rules_test.cpp checks that each file holds them.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "car_chrome_rules.h"
#include "car_rt_instances_rules.h"
#include "car_rt_refl_rules.h"
#include "car_rt_scene_rules.h"
#include "car_selfrefl_rules.h"

namespace mcla::gpu::car_chrome_rt {

namespace cc = car_chrome;
namespace cr = car_selfrefl;
namespace rt = car_rt_refl;
namespace rr = car_rt_rules;

// ---------------------------------------------------------------------------------------------------------------------
// The probes
// ---------------------------------------------------------------------------------------------------------------------
inline constexpr float kReflectReach = cc::kReflectLength;  // m: the mirror ray (the march's reflection probe: 0.25)
inline constexpr float kSkyReach = cc::kSkyLength;          // m: the sky rays (the march's sky probe: 0.20)
inline constexpr float kFadeFrom = cr::kFadeFrom;            // the share of the reach up to which a hit weighs 1 (the march's fade: 0.3)
inline constexpr float kFadeTo = cr::kFadeTo;                // ... and from which it weighs 0 (0.9)
inline constexpr int kSkyRays = 4;                           // the centre ray and three around it
inline constexpr float kSkySpread = 0.6f;                    // tan of the half angle of the cone the three are on (31 degrees)
inline constexpr float kSkyUpMin = 0.2f;                     // |n + up| under this: the centre ray takes up (a normal that points down; car_chrome_rules.h SkyDirection)
inline constexpr float kLiftBase = 0.002f;                   // m: the rays start this far off the surface along its normal ...
inline constexpr float kLiftPerMetre = 0.0005f;              // ... plus this a metre of the point's view distance (float noise of the structures)
inline constexpr float kMinNv = cc::kNormalMin;              // N.V under which the reflection ray is not cast (as the march's probe)
inline constexpr uint32_t kProbeParts = rr::kMaskOthers;     // the parts the probes hit: every part of another car (its chrome has this bit too)
static_assert((rr::kMaskOthers | rr::kMaskOtherChrome) != rr::kMaskOthers, "the chrome's instances have a bit of their own");

// A hit's weight by its distance along a probe of length `reach`: 1 up to kFadeFrom of it, a smoothstep down to 0 at kFadeTo of it.
inline float ProbeFade(float distance, float reach) {
  if (!(reach > 0.0f)) {
    return 0.0f;
  }
  return 1.0f - cr::SmoothStep(kFadeFrom * reach, kFadeTo * reach, distance);
}

inline float OriginLift(float view_distance) { return kLiftBase + kLiftPerMetre * view_distance; }

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 Normalized(const Vec3& a) {
  const float length = std::sqrt(Dot(a, a));
  return length > 0.0f ? a * (1.0f / length) : a;
}

// The sky direction of the chrome's probe: the surface's normal bent towards the world's up, normalize(n + up) (the march's, with the world's up given in
// whatever space the normal is in: view space for the pass); a normal that points straight down has no sky to look at and takes up.
inline Vec3 SkyCentre(const Vec3& normal, const Vec3& up) {
  const Vec3 sum = normal + up;
  const float length = std::sqrt(Dot(sum, sum));
  return length < kSkyUpMin ? up : sum * (1.0f / length);
}

// Ray `k` (0 .. kSkyRays - 1) of the sky probe: 0 the centre, 1 .. 3 on the cone of tan kSkySpread around it, 120 degrees apart, the cone's frame built
// from the world's up (or the x axis where the centre is within 25 degrees of it), the ray kept at least rt::kMinAbove above the surface's tangent plane
// (as a cone ray of the reflection is).
inline Vec3 SkyRay(const Vec3& normal, const Vec3& up, int k) {
  const Vec3 centre = SkyCentre(normal, up);
  Vec3 d = centre;
  if (k > 0) {
    const Vec3 helper = std::fabs(Dot(centre, up)) < 0.9f ? up : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 t = Normalized(Cross(helper, centre));
    const Vec3 b = Cross(centre, t);
    const float angle = 6.283185307179586f * float(k - 1) / 3.0f;
    d = Normalized(centre + (t * std::cos(angle) + b * std::sin(angle)) * kSkySpread);
  }
  const float above = Dot(d, normal);
  if (above < rt::kMinAbove) {
    d = Normalized(d + normal * (rt::kMinAbove - above));
  }
  return d;
}

// ---------------------------------------------------------------------------------------------------------------------
// The texel in the ray traced image
// ---------------------------------------------------------------------------------------------------------------------
// alpha = 1 + the reflection probe's weight (the code of a hit; 1 alone is a miss: car_rt_refl_rules.h EncodeAlpha), the colour's red = -(1 + the sky
// probe's weight); green and blue 0. Colours are never negative (the pass clamps them, the denoise averages them premultiplied by weights that are
// not negative), so a red under kChromeMarker says "another car's chrome" and nothing else does.
inline constexpr float kChromeMarker = -0.5f;
struct Texel {
  float red = 0.0f;
  float alpha = 0.0f;
};
inline Texel EncodeTexel(float reflect, float sky) {
  return {-(1.0f + std::clamp(sky, 0.0f, 1.0f)), 1.0f + std::clamp(reflect, 0.0f, 1.0f)};
}
struct Decoded {
  bool chrome = false;  // the texel is another car's chrome (data of this kind)
  float reflect = 0.0f;
  float sky = 0.0f;
};
inline Decoded DecodeTexel(float red, float alpha) {
  Decoded out;
  const float code = std::fabs(alpha);
  if (!(red < kChromeMarker) || !(code >= 0.5f)) {
    return out;
  }
  out.chrome = true;
  out.reflect = std::clamp(code - 1.0f, 0.0f, 1.0f);
  out.sky = std::clamp(-red - 1.0f, 0.0f, 1.0f);
  return out;
}
// The aux image's texel for it: x 0 (no hit distance: nothing to filter by), y the instance 255 / 255 (no instance of the player's car is that), z 0, w 0
// (no aux: the denoise copies the texel as it is). hlsl/car_rt_refl_denoise_cs.hlsl.
inline constexpr float kAuxInstanceCode = 1.0f;

// ---------------------------------------------------------------------------------------------------------------------
// What the chrome variant reads
// ---------------------------------------------------------------------------------------------------------------------
// The variant's read of the image at the shaded point's place, the 3x3 B-spline of car_rt_refl::ReadForPaint (the same footprint and the same weights),
// in two classes: the player's texels (a red that is not under kChromeMarker) as ever, and another car's chrome. The image answers with the first class where
// its texels are at least kUseShare of the footprint's matching texels, else with the second where its texels are, else it does not (the march).
struct ChromeTap {
  float history_distance = 0.0f;
  float alpha = 0.0f;
  float red = 0.0f;
};
struct ChromeRead {
  bool use = false;
  float reflect = 0.0f;
  bool sky_known = false;  // another car's chrome: the image has the sky probe's weight too
  float sky = 0.0f;
};
inline ChromeRead ReadForChrome(const float wx[3], const float wy[3], const ChromeTap tap[9], float depth, float tolerance) {
  float geometry = 0.0f;
  float known = 0.0f, weight = 0.0f;
  float known_other = 0.0f, weight_other = 0.0f, sky_other = 0.0f;
  for (int k = 0; k < 9; ++k) {
    const float wg = wx[k % 3] * wy[k / 3] * rt::MatchWeight(tap[k].history_distance, depth, tolerance);
    geometry += wg;
    const float code = std::fabs(tap[k].alpha);
    if (!(code >= 0.5f)) {
      continue;
    }
    if (tap[k].red < kChromeMarker) {
      known_other += wg;
      weight_other += wg * std::clamp(code - 1.0f, 0.0f, 1.0f);
      sky_other += wg * std::clamp(-tap[k].red - 1.0f, 0.0f, 1.0f);
    } else {
      known += wg;
      weight += wg * std::clamp(code - 1.0f, 0.0f, 1.0f);
    }
  }
  ChromeRead out;
  if (!(geometry > 1e-4f)) {
    return out;
  }
  if (known >= rt::kUseShare * geometry) {
    out.use = true;
    out.reflect = weight / known;
    return out;
  }
  if (known_other >= rt::kUseShare * geometry) {
    out.use = true;
    out.reflect = weight_other / known_other;
    out.sky_known = true;
    out.sky = sky_other / known_other;
  }
  return out;
}

}  // namespace mcla::gpu::car_chrome_rt

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the car's chrome parts under the car's own body: the rules that need no renderer
// ===========================================================================
// The numbers of the chrome variant (shaderpack/variants/car_chrome.h) as plain functions, so that car_chrome_rules_test.cpp can check
// what the picture depends on without a renderer or a compiler. The shader keeps its own copy of these numbers (it cannot include this
// header): change one, change the other (car_chrome_test.cpp checks that the shader's text holds each of them).
//
// WHAT THE VARIANT DOES (the story is in shaderpack/variants/car_chrome.h): xChrome::PS_Chrome, the shader of the tailpipes, the badges and
// the chrome trim of the player's car, makes its colour from three things and none of them knows the car's own body: out = AO * sun *
// mix(env, ambYPos, blend), where `env` is the dual paraboloid environment map at the reflected view vector, `ambYPos` the ambient colour
// of the sky above, `blend` how fast the normal changes from pixel to pixel (a thin tube seen from a distance is all ambient), `sun` the
// world's top-down shadow image (no car in it) and AO the vertex colour's alpha. A tailpipe sits in a cut-out of the bumper, 2 to 7 cm
// under and in front of the parts that cover it (measured on the car dump of the R34: the diffuser's lip is 1.7 to 3.7 cm above the tubes,
// the pipe's housing 3 to 7 cm ahead of their tops), and still reflects the sky and takes the sky's ambient. This variant reads the history image
// of the car's previous frame (car_selfrefl.h: the same image, the same matrix, the same constants as the car paint's variant) and finds
// the car's own parts with two short rays of CENTIMETRE scale, the paint's being decimetre scale:
//   - the reflection probe, along the reflected view vector (length 0.25 m, 12 points), darkens `env` where it hits;
//   - the sky probe, along the normal bent towards the sky (normalize(n + up), length 0.20 m, 8 points), darkens `ambYPos`.
// The ray, its window and its segments are the paint's (car_selfrefl_rules.h: WindowShape, SegmentOf, HitSum are used as they are), with
// numbers made for parts that are a few centimetres apart: the first point is 8 mm from the shaded point (the paint's is 40 mm), the
// window opens 14 mm behind a vehicle pixel at 6.7 m (the paint's: 37 mm) and closes at a thickness of 12 cm (the paint's: 30 cm).
// How sure that is: on the car dump of an R34 seen from 6.7 m (4K and 1080p, with 3 to 8 mm of noise in the history's depth and its
// half float storage) the probes found 73 to 94 percent of the hits a ray cast against the real triangles finds, and none that is not there
// (ports/scratch/carchrome, not committed: it needs the dump). car_chrome_rules_test.cpp checks the same on synthetic parts: a tube under
// a lip, a tube in the open, and a plane of every slope.
//
// THE RAY TRACED IMAGE (src/gpu/car_rt_refl.h): where the frame has it (constants g_CarReflC.w bit 0) the shader reads
// the reflection probe's weight from it, at the shaded point's place in the history (the same read as the paint's variant 6, weight only),
// and the march is not made: exact for every vehicle texel the host traced. The sky probe has no ray traced answer yet (the image holds
// the reflection's cone, not the hemisphere), so it is always marched; `kSkyProbe` is the hook where a ray traced ambient occlusion would
// replace it.
// ===========================================================================

#include <algorithm>
#include <cmath>

#include "car_selfrefl_rules.h"

namespace mcla::gpu::car_chrome {

namespace cr = mcla::gpu::car_selfrefl;

// The probes.
inline constexpr float kReflectLength = 0.25f;  // m: the reflection probe's reach
inline constexpr float kSkyLength = 0.20f;      // m: the sky probe's reach
inline constexpr int kReflectSteps = 12;        // points on the reflection probe
inline constexpr int kSkySteps = 8;             // points on the sky probe
inline constexpr float kStartMin = 0.008f;      // m: no point is nearer the shaded point than this (the paint's: 0.04)
inline constexpr float kDark = 0.90f;           // the most of `env` or of the ambient a hit takes away (the paint's: 0.85)
inline constexpr float kFull = 0.25f;           // the share of the points that, all inside a part, take the whole hit (the paint's default)
inline constexpr float kApproachSpan = 0.04f;   // m: the paint's 0.15, scaled to the shorter rays
inline constexpr float kNormalMin = 0.03f;      // N.V under which the reflection probe is not made (a grazing view, as the paint's)

// The distance of point `index` of `steps`: the paint's spacing (closer together near the shaded point, u^1.5), with this variant's start.
inline float RayDistance(int index, int steps, float length) {
  const float u = float(index + 1) / float(steps);
  return std::max(length * u * std::sqrt(u), kStartMin);
}

// How thick "just behind a vehicle pixel" is at a view distance: the parts to find are 1 to 10 cm apart, and the history's depth is noisy
// by a few millimetres at 7 m and more at distance (a texel is a pixel's footprint, 1.4 mm at 4K and 6.7 m).
inline float Thickness(float depth) { return 0.08f + 0.006f * depth; }

// How far off the stored distance of a slanted surface can be, as the paint's SurfaceSlack, with the cap of a centimetre scale variant.
inline float SurfaceSlack(float depth, float history_height, float focal, float nv) {
  const float texel = depth * 2.0f / (history_height * std::max(focal, 0.001f));
  const float slope = std::min(std::sqrt(std::max(1.0f - nv * nv, 0.0f)) / std::max(nv, 0.1f), 8.0f);
  return std::min(1.5f * texel * slope, 0.03f);
}

// The window of the probes (the paint's WindowShape, with the numbers of a smaller scale): opens `bias` behind a vehicle pixel (the
// history's noise and the slack of a slanted surface), rises over `ramp`, and falls between half the thickness and the thickness. The two
// ramps never overlap, so the window has the exact integral the segments use.
inline cr::WindowShape MakeWindowShape(float depth, float thickness, float slack) {
  const float ramp = std::min(0.006f + 0.0015f * depth, 0.25f * thickness);
  const float bias = std::min(0.004f + 0.0015f * depth + slack, 0.5f * thickness - ramp);
  return {bias, ramp, 0.5f * thickness, thickness};
}

// The self-test: the shaded point must be found in the history at its own view distance, else the whole effect fades (the paint's is
// 0.06 m + 2 percent of the distance, which at the chrome's scale would let the lip next to a tube pass for the tube).
inline float AgreementTolerance(float depth, float slack) { return 0.015f + 0.005f * depth + slack; }
inline float Agreement(float stored, float depth, float slack) {
  const float tolerance = AgreementTolerance(depth, slack);
  return 1.0f - cr::SmoothStep(0.5f * tolerance, tolerance, std::fabs(stored - depth));
}

// THE RIM OF A PART. The history holds ONE sample of each pixel (the guest's sample 0 of the stencil and the depth, car_history_ps.hlsl; at 2560 pixels
// across and more one texel stands for the lower right pixel of a 2x2 block), while the chrome's pixel shader runs at the pixel's centre. At the silhouette of
// a tip (its top and its left edge, where that sample lies outside the tip) the texel under a shaded pixel belongs to what is behind the tip, the bumper's cavity
// 3 to 7 cm further or no vehicle at all, so Agreement() above says 0, the whole effect is left to the game, and the pixel keeps the game's bright
// sky ambient: a thin bright line round the tip's top (the ray traced run rtint1, the debug view's red outline). The cure is to try the texels around the
// own one as well and take the best agreement: a texel one or two steps away that holds the shaded point's own surface is the same part, a centimetre away.
// A pixel that agrees fully takes no search; one whose neighbours do not agree either (a wrong guess of the car's motion, a disocclusion) stays at 0, as it was.
inline constexpr int kSilhouetteTaps = 12;
inline constexpr int kSilhouetteOffsets[kSilhouetteTaps][2] = {{-1, 0}, {0, -1}, {1, 0},  {0, 1},  {-1, -1}, {1, -1},
                                                               {-1, 1}, {1, 1},  {-2, 0}, {0, -2}, {2, 0},   {0, 2}};

// The self-test's agreement of one texel's stored distance (0 where it holds no vehicle).
inline float TexelAgreement(float stored, float depth, float slack) { return stored > 0.0f ? Agreement(stored, depth, slack) : 0.0f; }

// The shaded point's agreement given its own texel's (`own`) and the stored distances of the kSilhouetteTaps texels round it (0 where they hold no vehicle):
// never lower than `own`, and `own` itself where that is already 1.
inline float SilhouetteAgreement(float own, const float stored[kSilhouetteTaps], float depth, float slack) {
  float best = own;
  if (best >= 1.0f) return best;
  for (int k = 0; k < kSilhouetteTaps; ++k) best = std::max(best, TexelAgreement(stored[k], depth, slack));
  return best;
}

// The sky probe's direction: the normal bent towards the sky, normalize(n + up); a normal that points straight down (n + up near 0) has
// no sky to look at and takes up. `up` is the world's +y, as the paint's own ambient (ambYPos) is the ambient of +y.
struct Dir {
  float x = 0.0f, y = 1.0f, z = 0.0f;
};
inline Dir SkyDirection(float nx, float ny, float nz) {
  const float x = nx, y = ny + 1.0f, z = nz;
  const float length = std::sqrt(x * x + y * y + z * z);
  if (length < 0.2f) return {0.0f, 1.0f, 0.0f};
  return {x / length, y / length, z / length};
}

// What a hit weight does to the colour it is for (the reflection's `env`, or the ambient): scaled by the setting's strength, and
// the colour never goes under 1 - kDark of itself.
inline float Occlude(float colour, float weight, float strength) {
  return colour * (1.0f - kDark * std::clamp(weight * strength, 0.0f, 1.0f));
}

// The game's chrome with the two occluded terms: out = ao * sun * mix(env', amb', blend), the game's own arithmetic (PS_Chrome's last
// lines) with `env` and `amb` replaced. `sun` and `ao` are not touched.
inline float ChromeColour(float ao, float sun, float env, float amb, float blend, float reflect_hit, float sky_hit, float strength) {
  const float e = Occlude(env, reflect_hit, strength);
  const float a = Occlude(amb, sky_hit, strength);
  return ao * sun * (e + (a - e) * blend);
}

}  // namespace mcla::gpu::car_chrome

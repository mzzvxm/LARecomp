#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player car's ray traced self-reflection: the rules that need no renderer
// ===========================================================================
// The pass (hlsl/car_rt_refl_cs.hlsl, car_rt_refl.h) and the paint variant's read of its image (shaderpack/variants/car_selfrefl.cpp,
// versions 5 and 6) agree on the numbers below. This header holds them as plain functions with the same arithmetic, so that
// car_rt_refl_rules_test.cpp checks what the picture depends on: the image's codes, the places a pixel and a point of the view
// space have in it, the distance tolerances, the cone of rays, the self-hit rule, the fallback colour of a hit the camera cannot see, the sum of
// the rays and the edge-aware read the paint does, and the denoise between the two (the aux image's codes, the filter's taps and strength: the
// whole of hlsl/car_rt_refl_denoise_cs.hlsl). The shaders keep their own copies of these numbers (they cannot include this header; the two
// the trace and the denoise share are in hlsl/car_rt_refl_common.hlsli): change one, change the other.
//
// THE IMAGE (CarRtRefl's, the size of the car history image, R16G16B16A16_FLOAT, rgb in the history's units: the target's, the
// shaders' output times 2^exp_bias):
//   alpha 0                 no data: this pixel is not the player's car as the triangles see it (another vehicle, a part with no
//                           triangles, glass, a miss of the primary ray), or the pixel is no vehicle at all. The paint marches the
//                           previous frame's picture there as before (version 4).
//   alpha 1                 the player's car, traced, and none of the cone's rays hits a part of it: the environment stays.
//   alpha 1 + w, w in (0,1] the cone hits parts of the car the camera sees; w is the share of the cone (the rays' weights over their number),
//                           rgb the mean of the parts' colours in the history.
//   alpha -(1 + w)          the hits are mostly on parts the camera does not see (the back of a part, or one that another part hides), or
//                           off the picture; rgb is the stand-in colour (the fallback rule below).
// A value rounds to half precision (steps of 1/1024 up to 2), which the weight does not mind.
//
// THE CONE: the paint's own reflection of the world is soft (the environment map is 512x256 for a hemisphere and the paint adds its gloss),
// and the first version's one perfect mirror ray per texel gave the car's reflection of itself a razor edge, one texel wide (2 pixels at 4K,
// where the image is half the picture) and stepping at every texel: the stair-steps. Every texel now casts kConeRays rays spread over a cone of a
// few degrees (mcla_gpu_car_selfrefl_rt_gloss, 2.5 by default) around the mirror direction. The directions are two rings of equal area,
// opposite in pairs, so that they add up to the mirror's (ConeDisc), turned by an angle that differs from texel to texel (ConeRotation: an
// interleaved gradient noise) and, with a temporal upscaler running, from frame to frame (the upscaler averages the pattern; without one the
// pattern stands still on the screen, which the eye takes for the paint's grain). The texel's WEIGHT is the share of the rays that hit (each ray's
// own fade with distance and visibility counted), its COLOUR the weighted mean of the hits'. So a reflected part's edge is a ramp as wide as the cone is
// at the part's distance: sharp where the part is near, soft where it is far, as a glossy paint shows it, and no longer the size of a texel.
// The image stays at the history's size: the paint reads it with a 3x3 quadratic B-spline of texels (ReadForPaint), which has no corner at a
// texel's edge. After the first two rays (the outer pair) found nothing the inner two are not cast: the open sky over the roof and the bonnet is
// most of the car, half the rays saved there. A thin part that only the inner rays would catch is lost, and a soft edge loses at most
// half its weight at its very rim.
//
// THE FALLBACK RULE for a hit the history cannot show (the back or underside of a part, a part another part hides, a point off the picture):
// the colour of the hit part is not known. The part is certainly there (the triangles say so), so it hides the environment as it does in a real
// mirror, and the underside of a part is not lit by the sun, so it is dark: the reflection takes a colour in shade (kShade = 0.4, version 4's
// hidden-face rule) at NEARLY the visible hit's weight (kHiddenWeight = 0.85; the paint's own fresnel scales both). What is uncertain is the colour, and an estimate
// that is too dark shows, so the weight stays a little under 1. Version 1 took half the weight, and the hit of one texel switching between
// visible (the history's colour, the whole weight) and hidden (half) made dark patches in a band under the wing. The colour is NOT the shaded
// pixel's own times the shade (the pixel holds the sky's glints and the sun on the paint, so the colour jumped where a glint was): it is the mean of
// the same surface's texels around the pixel (SurfaceMean: a 3x3 of taps two texels apart, those within the distance tolerance, tent weighted),
// a smooth estimate of the panel's own colour. A hit that is only partly visible (the part's outline passes through the footprint of the point in
// the image) mixes the two by the visible share.
//
// A RAY UNDER ITS OWN SURFACE (the grazing self-hit): the mirror direction is made with the mesh's smooth normal, the triangle's own plane is
// another: on a large triangle the ray can run under the plane it left and hit its own panel a few centimetres on. The pass has no triangle
// positions to take the geometric normal from (the structures hold indices and normals only), so a hit counts as the panel hitting itself,
// and as nothing (the environment stays), when it is on the primary ray's instance and either on its own triangle or on a surface that faces
// the way the point's normal does (within 32 degrees: IsSelfHit). A real reflection of a part on another part (the wing on the lid, a mirror on
// the door) is on another instance or faces the surface it is seen from, never the same way.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "car_selfrefl_rules.h"

namespace mcla::gpu::car_rt_refl {

// ---------------------------------------------------------------------------------------------------------------------
// The setting
// ---------------------------------------------------------------------------------------------------------------------
// mcla_gpu_car_selfrefl_rt: auto (the default: on where the device can ray trace the car and the self-reflection is on) | off. A word
// that is no value counts as auto, as the other graphics settings' do.
enum class Setting : uint8_t { kAuto, kOff };
inline Setting SettingOf(std::string_view text) { return text == "off" ? Setting::kOff : Setting::kAuto; }
inline bool Wanted(Setting setting, bool device_supports, bool reflection_on) {
  return setting == Setting::kAuto && device_supports && reflection_on;
}

// ---------------------------------------------------------------------------------------------------------------------
// The parts a ray may hit (the instance masks of hlsl/car_rt_common.hlsli: CAR_RT_MASK_*)
// ---------------------------------------------------------------------------------------------------------------------
inline constexpr uint32_t kPartBody = 0x01;
inline constexpr uint32_t kPartGlass = 0x02;
inline constexpr uint32_t kPartTyres = 0x04;
inline constexpr uint32_t kPartDriver = 0x08;
// Another car (an opponent, traffic, a parked car: car_rt_instances_rules.h): the player's paint reflects a car driving alongside. Never in the primary
// ray's parts (the paint of the player's car is what the primary ray finds), only in the reflection ray's.
inline constexpr uint32_t kPartOthers = 0x10;
inline constexpr uint32_t kPartAll = 0x1F;
// The primary ray finds the car's pixel: only the opaque rigid parts, which are the paint's (and the rims', lights' and so on: the
// paint's own pixels are among them, and no pixel of glass, of the driver or of a tyre is read by the paint).
inline constexpr uint32_t kPrimaryParts = kPartBody;
// Another car's CHROME (car_rt_scene_rules.h kMaskOtherChrome: the instance of a draw of xChrome::PS_Chrome of another car has this bit and kPartOthers):
// the primary ray also finds it, with the window around the image's distance as for the player's body, because the chrome's occlusion is traced for its pixels
// (car_chrome_rt_rules.h). The mask is outside kPartAll: it is not a part a reflection ray can be asked for, and no other car's paint has it.
inline constexpr uint32_t kPartOtherChrome = 0x20;
inline constexpr uint32_t kPrimaryTraceParts = kPrimaryParts | kPartOtherChrome;
// The reflection ray: the body, the glass (a window reflects as dark as the history shows it, or by the fallback colour where the
// glass writes no depth), the tyres, and the other cars. Not the driver (skinned, seen through the glass only). The debug cvar
// mcla_gpu_car_selfrefl_rt_parts replaces it.
inline constexpr uint32_t kDefaultReflectionParts = kPartBody | kPartGlass | kPartTyres | kPartOthers;
static_assert((kPrimaryParts & kPartOthers) == 0, "the primary ray never finds another car");
static_assert((kPrimaryTraceParts & kPartOthers) == 0 && (kPartOtherChrome & kPartAll) == 0,
              "the primary ray never finds another car's paint either: only its chrome, by a bit of its own");

// ---------------------------------------------------------------------------------------------------------------------
// The image's codes
// ---------------------------------------------------------------------------------------------------------------------
enum class Kind : uint8_t { kNoData, kMiss, kHit, kHidden };

struct Texel {
  Kind kind = Kind::kNoData;
  float weight = 0.0f;  // 0..1
};

inline float EncodeAlpha(Kind kind, float weight) {
  const float w = std::clamp(weight, 0.0f, 1.0f);
  switch (kind) {
    case Kind::kNoData: return 0.0f;
    case Kind::kMiss: return 1.0f;
    case Kind::kHit: return 1.0f + w;
    case Kind::kHidden: return -(1.0f + w);
  }
  return 0.0f;
}

inline Texel DecodeAlpha(float alpha) {
  const float code = std::fabs(alpha);
  if (!(code >= 0.5f)) {
    return {Kind::kNoData, 0.0f};
  }
  const float w = std::clamp(code - 1.0f, 0.0f, 1.0f);
  if (w <= 0.0f) {
    return {Kind::kMiss, 0.0f};
  }
  return {alpha < 0.0f ? Kind::kHidden : Kind::kHit, w};
}

// ---------------------------------------------------------------------------------------------------------------------
// The numbers
// ---------------------------------------------------------------------------------------------------------------------
inline constexpr float kShade = car_selfrefl::kShade;              // 0.4: the surface's own colour in shade (version 4's hidden-face rule)
inline constexpr float kHiddenWeight = 0.85f;                      // the weight of a hit the history cannot show, against 1 for one it shows (version 1: 0.5)
inline constexpr float kMinNv = 0.03f;                             // as the variant: a ray that grazes the surface is not traced
inline constexpr float kDefaultRayLength = 4.0f;                   // m: the reflection ray's reach (version 4's march reached 2 m, faded from 0.6 m)
inline constexpr float kMinRayLength = 0.5f;
inline constexpr float kMaxRayLength = 20.0f;
inline constexpr float kFadeFrom = 0.6f;                           // the share of the ray's length from which a hit weighs less, to 0 at the length
inline constexpr float kVisibleFullShare = 0.5f;                   // the share of the 2x2 footprint that, matching, makes a hit fully visible
inline constexpr float kMinDepth = 0.05f;                          // m: a point nearer than this in view space is behind the camera
inline constexpr float kMaxSlack = 0.05f;                          // m: the most the slope of a surface adds to a distance tolerance
inline constexpr float kUseShare = 0.5f;                           // the paint's read: the share of the matching footprint that must have data
inline constexpr int kConeRays = 4;                                // rays a texel casts over the cone
inline constexpr int kFirstRays = 2;                               // rays cast before the others are given up on when none of them hit
inline constexpr float kConeDefaultDegrees = 2.5f;                 // the cone's half angle (mcla_gpu_car_selfrefl_rt_gloss), degrees
inline constexpr float kConeMaxDegrees = 12.0f;
inline constexpr float kMinAbove = 0.03f;                          // a cone ray is kept at least this far (as a cosine) above the surface's tangent plane
inline constexpr float kSelfCos = 0.85f;                           // a hit on the primary ray's instance facing within 32 degrees of the point's own normal is the panel itself
inline constexpr float kSurfaceSpacing = 2.0f;                     // texels between the taps of the surface's mean colour (SurfaceMean)

inline float ClampedLength(float length) { return std::clamp(length, kMinRayLength, kMaxRayLength); }
inline float ClampedGloss(float degrees) { return std::clamp(degrees, 0.0f, kConeMaxDegrees); }
// The tangent of the cone's half angle, which the pass spreads its rays by; 0 (a gloss of 0): a mirror, one ray.
inline float ConeTan(float degrees) { return std::tan(ClampedGloss(degrees) * 0.017453292519943295f); }

// Where along the reflection ray the origin is put off the surface, so that it does not hit its own triangle or its neighbours on a
// curved panel (metres, along the hit's normal).
inline float OriginOffset(float t) { return 0.004f + 0.002f * t; }

// The spacing of half precision floats at `t` (the image holds the view distance in a half float): 2^(floor(log2 t) - 10).
inline float HalfStep(float t) {
  return t > 6.2e-5f ? std::ldexp(1.0f, std::ilogb(t) - 10) : 6.0e-8f;
}

// The slope of a surface seen at N.V = nv, as the history's slack has it: how many metres of distance one metre across the picture
// takes (version 4's SurfaceSlack).
inline float SurfaceSlope(float nv) {
  return std::min(std::sqrt(std::max(1.0f - nv * nv, 0.0f)) / std::max(nv, 0.1f), 8.0f);
}

// One pixel's (or one image texel's) side in metres at view distance `t`, for a picture `edge` pixels high and a lens whose tan of
// half the vertical field of view is `tan_y`.
inline float PixelMeters(float t, float tan_y, float edge) { return t * 2.0f * tan_y / std::max(edge, 1.0f); }

// How far the distance of the same surface may differ between two sources of it (our triangles against the game's depth; a point's
// own distance against the image's at a texel up to a footprint away): a half float's step, the game's depth noise (a centimetre and
// half a percent: the car dump measured the rebuilt triangles within 0.33% at the 95th percentile of the depth), and the slope of the
// surface across `footprint` pixels, at most kMaxSlack.
inline float Tolerance(float t, float pixel_m, float footprint, float nv) {
  const float slack = std::min(footprint * pixel_m * SurfaceSlope(nv), kMaxSlack);
  return HalfStep(t) + 0.01f + 0.005f * t + slack;
}
// The width of the primary ray's window around the image's distance: the tolerance of the steepest slope (the match is judged again
// with the true slope once the hit is known).
inline float PrimaryWindow(float t, float pixel_m) { return Tolerance(t, pixel_m, 0.75f, 0.0f); }
inline float PrimaryTolerance(float t, float pixel_m, float nv) { return Tolerance(t, pixel_m, 0.75f, nv); }
// The tolerance of a point against a texel of the image around it (the 2x2 footprint: up to one texel away).
inline float VisibleTolerance(float t, float texel_m, float nv) { return Tolerance(t, texel_m, 1.0f, nv); }

// ---------------------------------------------------------------------------------------------------------------------
// Places in the image
// ---------------------------------------------------------------------------------------------------------------------
// Texel `i` (counting from 0) of an image whose divisor is `divisor` (car_selfrefl_rules.h HistoryDivisor) stands for the render pixel
// the history pass read: floor((i + 0.5) divisor) (the lower right of its block), clamped to the picture.
inline float PixelOfTexel(uint32_t texel, float divisor, float render_edge) {
  return std::min(std::floor((float(texel) + 0.5f) * divisor), render_edge - 1.0f);
}
// The render pixel's centre sits this far (in render pixels) from the start of the texel's block, so a point at render position X is
// at texel coordinate (X - TexelCentre) / divisor, texel i being centred at i.
inline float TexelCentre(float divisor) { return std::floor(divisor * 0.5f) + 0.5f; }

// The ray through the centre of render pixel (px, py), in view space (x right, y up, z forward: ao_common.hlsli AoViewPos): the
// direction has z = 1, so a distance along it is the view distance t the history holds.
struct Direction {
  float x = 0.0f, y = 0.0f, z = 1.0f;
};
inline Direction PixelDirection(float px, float py, float render_w, float render_h, float tan_x, float tan_y) {
  const float u = (px + 0.5f) / render_w;
  const float v = (py + 0.5f) / render_h;
  return {(u * 2.0f - 1.0f) * tan_x, (1.0f - v * 2.0f) * tan_y, 1.0f};
}

// A point of the view space as a place in the image: continuous texel coordinates, texel i centred at i. Outside the picture, or at
// or behind the camera: not inside.
struct ImagePoint {
  bool inside = false;
  float x = 0.0f;
  float y = 0.0f;
};
inline ImagePoint ViewToImage(const float q[3], float tan_x, float tan_y, float render_w, float render_h, float divisor) {
  ImagePoint out;
  if (!(q[2] > kMinDepth)) {
    return out;
  }
  const float nx = q[0] / (q[2] * tan_x);
  const float ny = q[1] / (q[2] * tan_y);
  if (!(std::fabs(nx) <= 1.0f && std::fabs(ny) <= 1.0f)) {
    return out;
  }
  const float centre = TexelCentre(divisor);
  out.inside = true;
  out.x = ((nx * 0.5f + 0.5f) * render_w - centre) / divisor;
  out.y = ((0.5f - ny * 0.5f) * render_h - centre) / divisor;
  return out;
}

// The four texels a bilinear read around texel coordinates (fx, fy) takes, with their weights (tap k: x offset k & 1, y offset k >> 1),
// clamped to the image.
struct Taps {
  int x[4] = {};
  int y[4] = {};
  float weight[4] = {};
};
inline Taps BilinearTaps(float fx, float fy, int width, int height) {
  Taps taps;
  const float bx = std::floor(fx);
  const float by = std::floor(fy);
  const float rx = fx - bx;
  const float ry = fy - by;
  for (int k = 0; k < 4; ++k) {
    const int ox = k & 1;
    const int oy = k >> 1;
    taps.x[k] = std::clamp(int(bx) + ox, 0, width - 1);
    taps.y[k] = std::clamp(int(by) + oy, 0, height - 1);
    taps.weight[k] = (ox ? rx : 1.0f - rx) * (oy ? ry : 1.0f - ry);
  }
  return taps;
}

// How much a history texel at view distance `texel_distance` is the surface a point at distance `t` is on: 0 where the texel holds no
// vehicle, 1 within half the tolerance, falling to 0 at the tolerance.
inline float MatchWeight(float texel_distance, float t, float tolerance) {
  if (!(texel_distance > 0.0f)) {
    return 0.0f;
  }
  return 1.0f - car_selfrefl::SmoothStep(0.5f * tolerance, tolerance, std::fabs(texel_distance - t));
}

// ---------------------------------------------------------------------------------------------------------------------
// The cone (the pass's rays of a texel)
// ---------------------------------------------------------------------------------------------------------------------
// Ray j (0 .. 3) of a texel is at the angle (turns + j / 4) of a turn around the mirror direction, at a radius of the cone's disc that
// alternates between two rings of equal area (0.5 and sqrt(3) / 2: a quarter of the disc's area each, and the mean of the squared radius is
// the disc's 1/2), the pairs j and j + 2 opposite each other: the four offsets add up to nothing, so the mean direction is the mirror's.
inline float ConeRadius(int ray) { return (ray & 1) == 0 ? 0.5f : 0.8660254037844386f; }
// The order the rays are cast in: the outer pair (1, 3, opposite each other) first, the inner pair (0, 2) after, if the first two found something.
inline int ConeOrder(int slot) {
  constexpr int kOrder[kConeRays] = {1, 3, 0, 2};
  return kOrder[slot & 3];
}
struct Disc {
  float x = 0.0f, y = 0.0f;
};
inline Disc ConeDisc(int ray, float turns) {
  const float angle = 6.283185307179586f * (turns + 0.25f * float(ray));
  return {ConeRadius(ray) * std::cos(angle), ConeRadius(ray) * std::sin(angle)};
}
// The angle the texel's pattern is turned by, in turns: an interleaved gradient noise of the texel's coordinates (Jimenez 2014), which has
// no structure the eye picks out, plus a golden step per phase (0 where no temporal upscaler runs: the pattern then stands still on the screen, which
// the eye takes for the paint's grain; 0 .. 7 by the frame where one does, which averages the pattern out).
inline float ConeRotation(uint32_t texel_x, uint32_t texel_y, uint32_t phase) {
  const float inner = 0.06711056f * float(texel_x) + 0.00583715f * float(texel_y);
  const float noise = 52.9829189f * (inner - std::floor(inner));
  const float sum = (noise - std::floor(noise)) + 0.6180339887f * float(phase);
  return sum - std::floor(sum);
}
// The direction of ray `ray` of the cone around the mirror direction `mirror` (a unit vector), for the point's normal `normal` (a unit vector)
// and the tangent of the cone's half angle: the mirror plus the disc's offset in the plane across it, made a unit vector, then lifted
// to at least kMinAbove above the point's tangent plane (a ray of the cone that dips under it is turned up: the mirror's own direction is
// above it by the N.V >= 0.03 the pass already requires).
inline void ConeDirection(const float mirror[3], const float normal[3], float tan_half, int ray, float turns, float out[3]) {
  const float up_y = std::fabs(mirror[1]) < 0.9f ? 1.0f : 0.0f;
  const float up[3] = {1.0f - up_y, up_y, 0.0f};
  float t[3] = {up[1] * mirror[2] - up[2] * mirror[1], up[2] * mirror[0] - up[0] * mirror[2], up[0] * mirror[1] - up[1] * mirror[0]};
  const float tl = 1.0f / std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
  t[0] *= tl;
  t[1] *= tl;
  t[2] *= tl;
  const float b[3] = {mirror[1] * t[2] - mirror[2] * t[1], mirror[2] * t[0] - mirror[0] * t[2], mirror[0] * t[1] - mirror[1] * t[0]};
  const Disc disc = ConeDisc(ray, turns);
  float d[3];
  for (int c = 0; c < 3; ++c) {
    d[c] = mirror[c] + (t[c] * disc.x + b[c] * disc.y) * tan_half;
  }
  float length = 1.0f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  for (int c = 0; c < 3; ++c) {
    d[c] *= length;
  }
  const float above = d[0] * normal[0] + d[1] * normal[1] + d[2] * normal[2];
  if (above < kMinAbove) {
    for (int c = 0; c < 3; ++c) {
      d[c] += (kMinAbove - above) * normal[c];
    }
    length = 1.0f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int c = 0; c < 3; ++c) {
      d[c] *= length;
    }
  }
  for (int c = 0; c < 3; ++c) {
    out[c] = d[c];
  }
}

// A hit of a reflection ray that is the surface the ray left (see the header's last paragraph): on the primary ray's instance and either on its
// own triangle or facing the way the point's normal does (`cos_between`: the dot of the hit's normal, turned to face the ray, and the point's).
inline bool IsSelfHit(bool same_instance, bool same_primitive, float cos_between) {
  return same_instance && (same_primitive || cos_between > kSelfCos);
}

// The mean colour of the pixel's own surface around it, for the fallback of a hidden hit: a 3x3 of history texels kSurfaceSpacing apart
// (tap k: x offset (k % 3) - 1, y offset (k / 3) - 1), those within `tolerance` of the pixel's distance `t`, tent weighted (1 at the middle,
// 1/2 at an edge, 1/4 at a corner). The middle tap is the pixel itself, so there is always a mean.
struct SurfaceTap {
  float distance = 0.0f;
  float rgb[3] = {};
};
inline void SurfaceMean(const SurfaceTap taps[9], float t, float tolerance, float out[3]) {
  float sum[3] = {};
  float total = 0.0f;
  for (int k = 0; k < 9; ++k) {
    const float tent = ((k % 3) == 1 ? 1.0f : 0.5f) * ((k / 3) == 1 ? 1.0f : 0.5f);
    const float w = tent * MatchWeight(taps[k].distance, t, tolerance);
    total += w;
    for (int c = 0; c < 3; ++c) {
      sum[c] += w * taps[k].rgb[c];
    }
  }
  for (int c = 0; c < 3; ++c) {
    out[c] = total > 1e-6f ? sum[c] / total : taps[4].rgb[c];
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Another car's colour (hlsl/car_rt_refl_cs.hlsl OtherCarColour)
// ---------------------------------------------------------------------------------------------------------------------
// A hit on another car's part the history does not show (the car's far side, a part another part hides) has no colour of its own: the surface's mean
// colour of the PLAYER's pixel (SurfaceMean), which a hit on the player's own car takes, would paint the other car in the player's paint. The history
// image holds every vehicle's pixels, though, so the car's colour is the one the camera sees of IT: the mean of the history's vehicle texels around the
// car's body centre, where they are at about the centre's distance (the car's roof, bonnet or the side the camera sees), made dark by the same shade
// (kShade) as any hidden hit. The centre is the mean of the origins of the car's draws, raised (car_rt_instances_rules.h OtherCar::Centre) and
// reaches the shader as the car's record in the instances' table; its radius (kCarRadius, 1.6 m) sets the sample: nine taps kOtherCarSpread radii
// apart in the picture, those within kOtherCarDepth radii of the centre's distance counting (tent weighted, as SurfaceMean's). A car whose
// taps hold too little (its centre is off the picture, or another car hides it) has no colour: kOtherCarMinShare of the weights; its hidden
// hits are then a miss, so the paint shows the environment there as it did before the other cars were in the scene. A centre outside the picture is
// moved in to kOtherCarEdge of it, so that a car at the edge is sampled where its visible part is.
// WHERE THE HISTORY SHOWS NOTHING OF A CAR THE GAME DRAWS (RT-MODEL-CARS-3): the game draws a car beside the player whenever the sphere round it reaches the view's
// frustum, so a neighbour 2 to 4 m from the player's door is drawn (its draws are in the scene, its +116 flag says visible) while the camera, standing in the gap beside the
// door and looking at it, sees none of that car: its centre is behind the camera. The history has nothing of it and, with no colour, its hidden hits were a miss: the door
// showed no neighbour (the first model-data check's flank pictures). Such a car takes its PAINT where its record has one (the model walk read it from the game's customisation
// record, car_model_rt_rules.h PaintForDrawn), exactly as a car placed from memory does; where the history does show it, the history's colour stays.
// WHY THIS AND NOT A PAINT CONSTANT: the pixel constants of the car paint shader (native/work/hlsl-head/cars/xCarPaintCustomizable__PS_CarPaint.hlsl:
// the lights DL0 to DL6, the ambient, the exposure) hold no base colour, it comes from textures; and the traffic's materials are other shaders with
// other constants. What the camera sees of a car is the one thing every car has, it needs nothing from the guest, and it costs nine loads for a hit
// that is not fully visible.
// CHECK: the choice is the first guess: the picture with a car alongside shows whether the colour is the car's (view 5 orange = a hidden hit; the
// reflection in the paint should be the car's own colour darkened) and whether the depth window takes the road or the player's own car.
inline constexpr float kOtherCarSpread = 0.5f;      // radii of the car between the taps
inline constexpr float kOtherCarDepth = 1.2f;       // radii: the depth window around the centre's distance (full weight within half of it)
inline constexpr float kOtherCarMaxStep = 24.0f;    // texels between the taps, at most (a car near the camera is wide in the picture)
inline constexpr float kOtherCarEdge = 0.97f;       // the centre's place in the picture is moved in to this share of the half width and height
inline constexpr float kOtherCarMinShare = 0.15f;   // the share of the nine taps' weights (out of 4) under which the car has no colour
// A car placed from memory (car_rt_memory_rules.h: drawn earlier, not seen by the camera now) has a NEGATIVE radius in its record: the history holds nothing of it,
// so its hidden hits take this grey (each channel; the shade of the pass makes it darker like any hidden part) and count as fully known. CHECK: the grey is a
// guess; the car's own colour is not known on this side (a remembered colour needs a persistent GPU buffer written by this pass while the car is in view).
inline constexpr float kRememberedCarColour = 0.45f;

// Texels between the taps: kOtherCarSpread radii at the centre's distance, in texels of `texel_m` metres, from one texel to kOtherCarMaxStep.
inline float OtherCarStep(float radius, float texel_m) {
  return texel_m > 0.0f ? std::clamp(kOtherCarSpread * radius / texel_m, 1.0f, kOtherCarMaxStep) : 1.0f;
}

struct OtherCarColour {
  float share = 0.0f;  // 0..1: the taps' weights over their sum when all match (4)
  float rgb[3] = {};   // their mean colour (0 where none matches)
  bool known() const { return share >= kOtherCarMinShare; }
};
// The colour of a car placed from memory (a record with a negative radius): known in full, a fixed grey.
inline OtherCarColour RememberedOtherCarColour(const float* paint = nullptr) {
  OtherCarColour out;
  out.share = 1.0f;
  for (int c = 0; c < 3; ++c) {
    out.rgb[c] = paint ? paint[c] : kRememberedCarColour;  // the car's paint where the record has one (CarPaintRow), else the grey
  }
  return out;
}
// `taps` as SurfaceMean's (tap k at the offset (k % 3 - 1, k / 3 - 1) times the step; the middle tap is the centre's own place), `centre_z` the
// centre's view distance and `radius` the car's (a car the camera does not draw has a negative one: RememberedOtherCarColour, the taps are not looked at;
// `paint`: its paint colour from the record's second row, null where the flag is clear). A car the camera draws takes the history's colour where the taps hold
// enough (known()), and its paint, in full, where they do not and it has one. `centre_z` at or under kOtherCarMinDepth (the centre is behind the camera) or a
// radius of 0 (no centre in the record) has no taps to look at: the paint or nothing.
inline constexpr float kOtherCarMinDepth = 0.05f;  // the shader's kMinDepth
inline OtherCarColour FindOtherCarColour(const SurfaceTap taps[9], float centre_z, float radius, const float* paint = nullptr) {
  if (radius < 0.0f) {
    return RememberedOtherCarColour(paint);
  }
  OtherCarColour out;
  float sum[3] = {};
  float total = 0.0f;
  const float tolerance = kOtherCarDepth * radius;
  for (int k = 0; k < 9 && radius > 0.0f && centre_z > kOtherCarMinDepth; ++k) {
    const float tent = ((k % 3) == 1 ? 1.0f : 0.5f) * ((k / 3) == 1 ? 1.0f : 0.5f);
    const float w = tent * MatchWeight(taps[k].distance, centre_z, tolerance);
    total += w;
    for (int c = 0; c < 3; ++c) {
      sum[c] += w * taps[k].rgb[c];
    }
  }
  out.share = total / 4.0f;
  if (total > 1e-6f) {
    for (int c = 0; c < 3; ++c) {
      out.rgb[c] = sum[c] / total;
    }
  }
  if (!out.known() && paint) {
    return RememberedOtherCarColour(paint);  // the history shows nothing of the car: its paint stands in
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// What a reflection ray's hit shows (the pass)
// ---------------------------------------------------------------------------------------------------------------------
struct VisibleColour {
  float coverage = 0.0f;  // 0..1: the share of the bilinear footprint that is the hit's surface
  float rgb[3] = {};      // the matching texels' mean colour (0 where none matches)
};
// `distance[k]` and `rgb[k]` are the history texels of the taps; `taps.weight` the bilinear weights.
inline VisibleColour FindVisible(const Taps& taps, const float distance[4], const float rgb[4][3], float t, float tolerance) {
  VisibleColour out;
  float sum[3] = {};
  for (int k = 0; k < 4; ++k) {
    const float w = taps.weight[k] * MatchWeight(distance[k], t, tolerance);
    out.coverage += w;
    for (int c = 0; c < 3; ++c) {
      sum[c] += w * rgb[k][c];
    }
  }
  if (out.coverage > 1e-6f) {
    for (int c = 0; c < 3; ++c) {
      out.rgb[c] = sum[c] / out.coverage;
    }
  }
  return out;
}

// One ray of the cone that hit something: its distance along the ray, and what FindVisible found at the hit point.
struct RayShare {
  float weight = 0.0f;   // fade with the distance x (kHiddenWeight .. 1 by the visible share); 0: nothing (past the reach)
  float visible = 0.0f;  // 0..1: how much of the hit the history shows
  float rgb[3] = {};     // the colour: the visible colour where the history shows it, the shade (kShade x the surface's mean colour) where not
};
// `ray_distance` along the ray of length `ray_length`, `visible` FindVisible's result at the hit, `surface` the mean colour of the
// pixel's own surface (SurfaceMean).
inline RayShare MakeShare(float ray_distance, float ray_length, const VisibleColour& visible, const float surface[3]) {
  RayShare out;
  const float fade = 1.0f - car_selfrefl::SmoothStep(kFadeFrom * ray_length, ray_length, ray_distance);
  out.visible = std::clamp(visible.coverage / kVisibleFullShare, 0.0f, 1.0f);
  out.weight = fade * (kHiddenWeight + (1.0f - kHiddenWeight) * out.visible);
  for (int c = 0; c < 3; ++c) {
    const float shade = surface[c] * kShade;
    out.rgb[c] = shade + (visible.rgb[c] - shade) * out.visible;
  }
  if (out.weight < 1.0f / 512.0f) {
    out = RayShare{};  // beyond the reach: nothing
  }
  return out;
}

struct Reflection {
  Kind kind = Kind::kMiss;
  float weight = 0.0f;
  float rgb[3] = {};
};
// The image's value of a texel: the rays' shares added up. `rays` is how many rays the texel counts (kConeRays; 1 for a gloss of 0): a ray that was not
// cast (the early out) or that found nothing counts as a miss. The weight is the sum of the shares over `rays`, the colour their
// weighted mean, the kind the majority of the weight's visibility.
inline Reflection CombineRays(const RayShare shares[], int count, float rays) {
  Reflection out;
  float weight = 0.0f;
  float visible = 0.0f;
  float colour[3] = {};
  for (int k = 0; k < count; ++k) {
    weight += shares[k].weight;
    visible += shares[k].weight * shares[k].visible;
    for (int c = 0; c < 3; ++c) {
      colour[c] += shares[k].weight * shares[k].rgb[c];
    }
  }
  out.weight = std::min(weight / std::max(rays, 1.0f), 1.0f);
  if (out.weight < 1.0f / 512.0f) {
    return Reflection{};
  }
  for (int c = 0; c < 3; ++c) {
    out.rgb[c] = colour[c] / weight;
  }
  out.kind = visible >= 0.5f * weight ? Kind::kHit : Kind::kHidden;
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// The denoise (hlsl/car_rt_refl_denoise_cs.hlsl): the cone's noise out of the image before the paint reads it
// ---------------------------------------------------------------------------------------------------------------------
// WHY. Every texel's four rays are turned by a different angle, so in the band where a reflected part's edge is (the cone's penumbra: a few
// to a dozen texels wide) the texel's weight is one of 0, 1/4 .. 1 chosen by the pattern's turn, not by the geometry: grain. The paint's 3x3
// B-spline averages about 2.8 texels' worth of it (the sum of the squared weights of the quadratic B-spline is 0.59 a row), and without a
// temporal upscaler nothing averages the pattern over frames. On a straight edge (the model of the unit test: the pass's own pattern and early
// out, the paint's own read) the standard deviation of what the paint reads along the edge is 0.020 of the weight for a cone 2 texels wide, 0.027
// for 4, 0.032 for 8 and 0.043 for 19; where the part and the environment differ much in brightness (a dark wing underside on a bright lid) that is the grain.
//
// WHAT. Up to two edge-aware a-trous passes over the image (the taps 1 then 2 texels apart, a 3x3 kernel of 1 2 1 / 2 4 2 / 1 2 1: together a
// 7 x 7 triangle of sigma 1.6 texels, about 30 texels' worth of samples), the first into a scratch image, the second back into the trace's. A
// tap counts only where
//   - it holds data (the paint's own rule: alpha 0, no data, is left out and stays no data; a miss, alpha 1, counts as weight 0),
//   - it is the same surface (the history's distance within the tolerance of the centre's, the slope's slack from the centre's N.V, the
//     distance between the taps counted as the footprint: MatchWeight with Tolerance) and the same instance (the same draw of the car:
//     the aux image carries both, and N.V, from the primary ray),
// and the colours are averaged premultiplied by their weights (a miss's black does not darken a hit; it is the paint's own sum).
// STRENGTH FOLLOWS THE CONE. The noise lives in the penumbra and the penumbra's width is the cone's at its hit: 2 distance tan(half angle),
// in texels of the surface (ConeFootprint). A part a few centimetres off the panel has a penumbra under a texel (a binary edge with no
// noise to remove, and one the filter must not blur); a part half a metre away has one of eight to ten. How much of a pass is applied is
// DenoiseStrength(the largest footprint among the taps that count, the pass): the first pass is 0 below kDenoiseFootprintFrom texels and
// full from kDenoiseFootprintTo, the second (the wider kernel: more noise out, more blur in) 0 below kDenoiseSecondFrom and full from
// kDenoiseSecondTo, smoothstep between; a texel whose strength is 0 is copied exactly as the trace made it. The aux image's hit distance
// (the mean over the rays that hit, 0 where none did) gives the footprint; the taps count too, so the texels just outside a penumbra (rays
// all missed, no distance of their own) take their share of the smoothing from their neighbours. A gloss of 0 (one ray, nothing to denoise) skips
// the passes. Measured on the same model (the unit test prints it): both passes take the grain to 0.009 (2 texels), 0.010 (4), 0.0085 (8) and
// 0.006 (19) of the weight, the edge's ramp (0.9 to 0.1) grows by at most a fifth (2% at 19 texels), and its 50% point stays within 0.05 texels. A real
// scene has more noise than this model (the four rays' colours and distances differ too: the same sums smooth them): CHECK against the pictures.
//
// NOT TEMPORAL. A temporal accumulation in the car's own space was weighed and left out: the reflection depends on the view, so even a
// perfectly reprojected texel (the car's motion and the camera's are known to the paint, car_selfrefl.cpp's matrix) holds a reflection of
// the last frame's direction, and the cone's whole penumbra moves by the angle the view turned (0.5 degrees a frame in an orbit: a fifth of a
// 2.5 degree cone) which an accumulation over a few frames smears into a trailing edge; its rejection rule (the history's depth and instance)
// cannot see that, and a cut, a pause view, a garage orbit and the vehicle's own motion having no valid matrix some frames would each need
// its own. With an upscaler the upscaler is the temporal filter (the pattern turns by the frame). A ghost is worse than grain; this does not
// ghost, and holds nothing from the frame before.
inline constexpr int kDenoisePasses = 2;               // the passes (a-trous steps 1, 2); mcla_gpu_car_selfrefl_rt_denoise 0 .. 2 (0: the raw image)
inline constexpr float kDenoiseFootprintFrom = 0.4f;   // texels: a cone this wide or less at its hit leaves the texel as it is
inline constexpr float kDenoiseFootprintTo = 2.5f;     // texels: from this width the first pass is applied in full
inline constexpr float kDenoiseSecondFrom = 4.0f;      // texels: the second pass (step 2) is left out under this width ...
inline constexpr float kDenoiseSecondTo = 10.0f;       // texels: ... and applied in full from this one

// The passes made for the setting `setting` (0 .. kDenoisePasses) and the cone's half angle's tangent: a mirror (one ray) has no noise.
inline int DenoisePassCount(int setting, float tan_half) {
  return tan_half > 0.0f ? std::clamp(setting, 0, kDenoisePasses) : 0;
}
// The a-trous step of pass `pass` (0, 1): 1 then 2 texels between the taps.
inline int DenoiseStep(int pass) { return 1 << std::clamp(pass, 0, 1); }

// The AUX image: RGBA8_UNORM, the trace's second output, one texel for each texel of the image (0 in every channel where the texel has no data):
//   x the hit distance, sqrt(mean distance of the cone's hits / the ray's reach) (0: none hit); a hit never rounds to 0
//   y the instance (the draw of the car the primary ray hit) / 255, at most 255
//   z N.V of the surface at the texel
//   w 1 where the texel has data
inline float Unorm8(float v) { return std::floor(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f) / 255.0f; }
inline float EncodeHitDistance(float distance, float reach) {
  if (!(distance > 0.0f)) {
    return 0.0f;
  }
  return std::max(Unorm8(std::sqrt(std::min(distance / std::max(reach, 1e-3f), 1.0f))), 1.0f / 255.0f);
}
inline float DecodeHitDistance(float code, float reach) { return code * code * reach; }
inline float EncodeInstance(uint32_t instance) { return float(std::min<uint32_t>(instance, 255u)) / 255.0f; }
inline int InstanceOfCode(float code) { return int(std::floor(code * 255.0f + 0.5f)); }

// The width of the cone at its hit, in texels of the surface at the texel's distance (`texel_m` metres): the penumbra of a part's edge.
inline float ConeFootprint(float hit_distance, float tan_half, float texel_m) {
  return texel_m > 0.0f ? 2.0f * hit_distance * tan_half / texel_m : 0.0f;
}
// How much of pass `pass` (0, 1) is applied at a texel whose cone is `footprint` texels wide at its hit: the first pass from kDenoiseFootprintFrom to
// kDenoiseFootprintTo, the second (the wider kernel: more noise out, more blur in) only from kDenoiseSecondFrom to kDenoiseSecondTo.
inline float DenoiseStrength(float footprint, int pass) {
  return pass <= 0 ? car_selfrefl::SmoothStep(kDenoiseFootprintFrom, kDenoiseFootprintTo, footprint)
                   : car_selfrefl::SmoothStep(kDenoiseSecondFrom, kDenoiseSecondTo, footprint);
}
// The 3x3 kernel (1 2 1 / 2 4 2 / 1 2 1) at the tap's offset (-1, 0, 1 in each axis).
inline float DenoiseKernel(int ox, int oy) { return (ox == 0 ? 2.0f : 1.0f) * (oy == 0 ? 2.0f : 1.0f); }

// One of the 3x3 taps of a texel, tap k at the offset (k % 3 - 1, k / 3 - 1) times the pass's step; tap 4 is the texel itself.
struct DenoiseTap {
  bool inside = false;            // the tap is in the image
  float alpha = 0.0f;             // the image's code there
  float rgb[3] = {};              // the image's colour
  float history_distance = 0.0f;  // the history's view distance there (0: no vehicle)
  float hit_code = 0.0f;          // the aux's x
  int instance = 0;               // the aux's y, as an instance number
};
struct DenoiseSetup {
  float tan_y = 0.0f;         // tan of half the vertical field of view of the history's lens
  float image_height = 1.0f;  // the image's height in texels
  float tan_half = 0.0f;      // the tangent of the cone's half angle
  float reach = 4.0f;         // the reflection ray's length (the aux's hit distance is a share of it)
  int pass = 0;               // 0: taps one texel apart, 1: two texels apart (DenoiseStep)
};
struct DenoiseOut {
  float rgb[3] = {};
  float alpha = 0.0f;  // the image's code
};
// A texel that has data (tap 4's alpha is no "no data"; the history says a distance and the aux its N.V and instance): the pass's result for it.
// The taps' weights are the kernel's times how well their history distance agrees with the texel's (the tolerance of their distance apart,
// the slope from `centre_nv`) and 0 for another instance, a tap with no data or outside the image. The sums are premultiplied by the taps' own
// weights (the paint's own sum); the result is the texel's own value mixed with them by DenoiseStrength.
inline DenoiseOut DenoiseTexel(const DenoiseTap taps[9], float centre_distance, float centre_nv, const DenoiseSetup& setup) {
  const DenoiseTap& own = taps[4];
  const float texel_m = PixelMeters(centre_distance, setup.tan_y, setup.image_height);
  float n = 0.0f;
  float weight = 0.0f;
  float hidden = 0.0f;
  float colour[3] = {};
  float footprint = 0.0f;
  for (int k = 0; k < 9; ++k) {
    const DenoiseTap& tap = taps[k];
    const int ox = k % 3 - 1;
    const int oy = k / 3 - 1;
    const Texel code = DecodeAlpha(tap.alpha);
    if (!tap.inside || code.kind == Kind::kNoData || tap.instance != own.instance) {
      continue;
    }
    const float apart = float(DenoiseStep(setup.pass)) * std::sqrt(float(ox * ox + oy * oy));
    const float k_weight = DenoiseKernel(ox, oy) *
                           MatchWeight(tap.history_distance, centre_distance, Tolerance(centre_distance, texel_m, apart, centre_nv));
    if (!(k_weight > 0.0f)) {
      continue;
    }
    n += k_weight;
    weight += k_weight * code.weight;
    hidden += code.kind == Kind::kHidden ? k_weight * code.weight : 0.0f;
    for (int c = 0; c < 3; ++c) {
      colour[c] += k_weight * code.weight * tap.rgb[c];
    }
    if (tap.hit_code > 0.0f) {
      footprint = std::max(footprint, ConeFootprint(DecodeHitDistance(tap.hit_code, setup.reach), setup.tan_half,
                                                    PixelMeters(tap.history_distance, setup.tan_y, setup.image_height)));
    }
  }
  const float strength = DenoiseStrength(footprint, setup.pass);
  DenoiseOut out;
  if (!(strength > 0.0f)) {
    // No cone wide enough around to have noise: the texel is exactly what the trace made it.
    out.alpha = own.alpha;
    for (int c = 0; c < 3; ++c) {
      out.rgb[c] = own.rgb[c];
    }
    return out;
  }
  const Texel mine = DecodeAlpha(own.alpha);
  const float own_hidden = own.alpha < 0.0f ? mine.weight : 0.0f;
  const float inverse = n > 0.0f ? 1.0f / n : 0.0f;
  const float mixed_weight = mine.weight + (weight * inverse - mine.weight) * strength;
  const float mixed_hidden = own_hidden + (hidden * inverse - own_hidden) * strength;
  if (mixed_weight < 1.0f / 512.0f) {
    out.alpha = EncodeAlpha(Kind::kMiss, 0.0f);
    return out;
  }
  for (int c = 0; c < 3; ++c) {
    const float own_colour = mine.weight * own.rgb[c];
    out.rgb[c] = (own_colour + (colour[c] * inverse - own_colour) * strength) / mixed_weight;
  }
  out.alpha = EncodeAlpha(mixed_hidden > 0.5f * mixed_weight ? Kind::kHidden : Kind::kHit, mixed_weight);
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// What the paint reads (the variant's CarReflRtLookup)
// ---------------------------------------------------------------------------------------------------------------------
// The quadratic B-spline's weights of the three texels (offsets -1, 0, +1 from the nearest) for a place `d` (-0.5 .. 0.5) from the nearest
// texel's centre: they add up to 1, are continuous, and their slope is too (a bilinear read's is not: its weights have a corner at every texel,
// which showed as the stair-steps of a half size image).
inline void BSplineWeights(float d, float out[3]) {
  out[0] = 0.5f * (0.5f - d) * (0.5f - d);
  out[1] = 0.75f - d * d;
  out[2] = 0.5f * (0.5f + d) * (0.5f + d);
}
// The nearest texel and the offset from its centre of a continuous texel coordinate (texel i centred at i).
inline int NearestTexel(float f, float* d) {
  const float n = std::floor(f + 0.5f);
  *d = f - n;
  return int(n);
}
// The nine texels of the image and of the history around the reprojected pixel (tap k: x offset (k % 3) - 1, y offset (k / 3) - 1 from the
// nearest): the history's distance and the image's alpha and colour.
struct PaintTap {
  float history_distance = 0.0f;
  float alpha = 0.0f;
  float rgb[3] = {};
};
struct PaintRead {
  bool use = false;       // the image answers: weight and colour are valid; false: the march
  Kind kind = Kind::kNoData;
  float weight = 0.0f;
  float rgb[3] = {};
};
// `wx` and `wy` are BSplineWeights of the place. A texel counts where it is the surface the paint's pixel is on (its history distance within
// `tolerance` of `depth`) and has data; the footprint's data must cover kUseShare of the texels that are that surface, else the march answers.
// The colour is the mean of the weighted hits (a miss next to a hit does not darken the hit's colour); the weight is the share of the footprint the
// hits cover.
inline PaintRead ReadForPaint(const float wx[3], const float wy[3], const PaintTap tap[9], float depth, float tolerance) {
  PaintRead out;
  float geometry = 0.0f;
  float known = 0.0f;
  float weight = 0.0f;
  float hidden = 0.0f;
  float colour[3] = {};
  for (int k = 0; k < 9; ++k) {
    const float agree = MatchWeight(tap[k].history_distance, depth, tolerance);
    const float wg = wx[k % 3] * wy[k / 3] * agree;
    geometry += wg;
    const Texel texel = DecodeAlpha(tap[k].alpha);
    if (texel.kind == Kind::kNoData) {
      continue;
    }
    known += wg;
    weight += wg * texel.weight;
    if (texel.kind == Kind::kHidden) {
      hidden += wg * texel.weight;
    }
    for (int c = 0; c < 3; ++c) {
      colour[c] += wg * texel.weight * tap[k].rgb[c];
    }
  }
  if (!(geometry > 1e-4f) || known < kUseShare * geometry) {
    return out;
  }
  out.use = true;
  out.weight = weight / known;
  if (weight > 1e-5f) {
    for (int c = 0; c < 3; ++c) {
      out.rgb[c] = colour[c] / weight;
    }
  }
  out.kind = out.weight <= 0.02f ? Kind::kMiss : (hidden > 0.5f * weight ? Kind::kHidden : Kind::kHit);
  return out;
}

}  // namespace mcla::gpu::car_rt_refl

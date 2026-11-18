#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player car's ray traced self-reflection: the rules that need no renderer
// ===========================================================================
// The pass (hlsl/car_rt_refl_cs.hlsl, car_rt_refl.h) and the paint variant's read of its image (shaderpack/variants/car_selfrefl.cpp,
// version 5) agree on the numbers below. This header holds them as plain functions with the same arithmetic, so that
// car_rt_refl_rules_test.cpp checks what the picture depends on: the image's codes, the places a pixel and a point of the view
// space have in it, the distance tolerances, the fallback colour of a hit the camera cannot see, and the edge-aware read the paint
// does. The shaders keep their own copies of these numbers (they cannot include this header): change one, change the other.
//
// THE IMAGE (CarRtRefl's, the size of the car history image, R16G16B16A16_FLOAT, rgb in the history's units: the target's, the
// shaders' output times 2^exp_bias):
//   alpha 0                 no data: this pixel is not the player's car as the triangles see it (another vehicle, a part with no
//                           triangles, glass, a miss of the primary ray), or the pixel is no vehicle at all. The paint marches the
//                           previous frame's picture there as before (version 4).
//   alpha 1                 the player's car, traced, and the reflection ray hits nothing of the car: the environment stays.
//   alpha 1 + w, w in (0,1] the reflection ray hits a part of the car the camera sees; rgb is that part's colour in the history.
//   alpha -(1 + w)          it hits a part the camera does not see (the back of a part, or one that another part hides), or one off
//                           the picture; rgb is the stand-in colour (the fallback rule below).
// A value rounds to half precision (steps of 1/1024 up to 2), which the weight does not mind.
//
// THE FALLBACK RULE for a hit the history cannot show: the colour of the hit part is not known, so the reflection takes the shaded
// pixel's own colour in shade (own x kShade = 0.4), as version 4 does for the hidden face of a part, and the weight is the visible
// hit's times kHiddenWeight (0.5, version 4's kBackFloor). Why: the part is certainly there (the triangles say so), so the
// environment's colour must not be shown whole; the colour it has is unknown, and the unlit back or inside of a part of the same
// paint is darker than the paint's lit side, which is what the 0.4 stood for in version 4; half the weight keeps a wrong guess from
// replacing the environment's reflection altogether. A hit that is only partly visible (the part's outline passes through the
// bilinear footprint of the point in the image) mixes the two by the visible share.
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
inline constexpr uint32_t kPartAll = 0x0F;
// The primary ray finds the car's pixel: only the opaque rigid parts, which are the paint's (and the rims', lights' and so on: the
// paint's own pixels are among them, and no pixel of glass, of the driver or of a tyre is read by the paint).
inline constexpr uint32_t kPrimaryParts = kPartBody;
// The reflection ray: the body, the glass (a window reflects as dark as the history shows it, or by the fallback colour where the
// glass writes no depth), the tyres. Not the driver (skinned, seen through the glass only). The debug cvar mcla_gpu_car_selfrefl_rt_parts
// replaces it.
inline constexpr uint32_t kDefaultReflectionParts = kPartBody | kPartGlass | kPartTyres;

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
inline constexpr float kShade = car_selfrefl::kShade;              // 0.4: the shaded pixel's own colour in shade
inline constexpr float kHiddenWeight = car_selfrefl::kBackFloor;   // 0.5: the weight of a hit the history cannot show
inline constexpr float kMinNv = 0.03f;                             // as the variant: a ray that grazes the surface is not traced
inline constexpr float kDefaultRayLength = 4.0f;                   // m: the reflection ray's reach (version 4's march reached 2 m, faded from 0.6 m)
inline constexpr float kMinRayLength = 0.5f;
inline constexpr float kMaxRayLength = 20.0f;
inline constexpr float kFadeFrom = 0.6f;                           // the share of the ray's length from which a hit weighs less, to 0 at the length
inline constexpr float kVisibleFullShare = 0.5f;                   // the share of the 2x2 footprint that, matching, makes a hit fully visible
inline constexpr float kMinDepth = 0.05f;                          // m: a point nearer than this in view space is behind the camera
inline constexpr float kMaxSlack = 0.05f;                          // m: the most the slope of a surface adds to a distance tolerance
inline constexpr float kUseShare = 0.5f;                           // the paint's read: the share of the matching footprint that must have data

inline float ClampedLength(float length) { return std::clamp(length, kMinRayLength, kMaxRayLength); }

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

// ---------------------------------------------------------------------------------------------------------------------
// What a reflection ray's hit shows (the pass)
// ---------------------------------------------------------------------------------------------------------------------
// How much a history texel at view distance `texel_distance` is the surface a point at distance `t` is on: 0 where the texel holds no
// vehicle, 1 within half the tolerance, falling to 0 at the tolerance.
inline float MatchWeight(float texel_distance, float t, float tolerance) {
  if (!(texel_distance > 0.0f)) {
    return 0.0f;
  }
  return 1.0f - car_selfrefl::SmoothStep(0.5f * tolerance, tolerance, std::fabs(texel_distance - t));
}

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

struct Reflection {
  Kind kind = Kind::kMiss;
  float weight = 0.0f;
  float rgb[3] = {};
};
// The image's value for a reflection ray that hit a part at `ray_distance` along its `ray_length`: `visible` is FindVisible's
// result at the hit point, `own` the shaded pixel's own colour in the history.
inline Reflection Combine(float ray_distance, float ray_length, const VisibleColour& visible, const float own[3]) {
  Reflection out;
  const float fade = 1.0f - car_selfrefl::SmoothStep(kFadeFrom * ray_length, ray_length, ray_distance);
  const float vis = std::clamp(visible.coverage / kVisibleFullShare, 0.0f, 1.0f);
  out.weight = fade * (kHiddenWeight + (1.0f - kHiddenWeight) * vis);
  for (int c = 0; c < 3; ++c) {
    const float shade = own[c] * kShade;
    out.rgb[c] = shade + (visible.rgb[c] - shade) * vis;
  }
  out.kind = vis >= 0.5f ? Kind::kHit : Kind::kHidden;
  if (out.weight < 1.0f / 512.0f) {
    out = Reflection{};  // beyond the reach: nothing
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// What the paint reads (the variant's CarReflRtLookup)
// ---------------------------------------------------------------------------------------------------------------------
// The four texels of the image and of the history around the reprojected pixel: the history's distance and the image's alpha and colour.
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
// A texel counts where it is the surface the paint's pixel is on (its history distance within `tolerance` of `depth`) and has data;
// the footprint's data must cover kUseShare of the texels that are that surface, else the march answers. The colour is the mean of the
// weighted hits (a miss next to a hit does not darken the hit's colour); the weight is the share of the footprint the hits cover.
inline PaintRead ReadForPaint(const Taps& taps, const PaintTap tap[4], float depth, float tolerance) {
  PaintRead out;
  float geometry = 0.0f;
  float known = 0.0f;
  float weight = 0.0f;
  float hidden = 0.0f;
  float colour[3] = {};
  for (int k = 0; k < 4; ++k) {
    const float agree = MatchWeight(tap[k].history_distance, depth, tolerance);
    const float wg = taps.weight[k] * agree;
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

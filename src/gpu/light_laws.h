#pragma once
// ===========================================================================
// MCLA GPU renderer -- the laws of a light: what each `law` of a Light means, in the game's units (perf/LIGHT-UNIFY.md 1.3 and 2.3, package U1)
// ===========================================================================
// Three attenuation laws for "a light" exist today (the survey's 1.3), and GB-H's validation ("a deferred lamp matches a grid lamp") needs the first two evaluated by ONE function
// on the CPU and in HLSL (hlsl/light_laws.hlsli is this file's twin, light_frame_test.cpp compiles it as C++ and compares it). They are written here once, as pure functions on
// plain numbers, and held to the numbers that exist by golden tests:
//
//   kHeadlight  the rig's term (headlight_rig_rules.h LampScale: kPeak 2.0 x 1/(1+(d/D)^2) x window^2 x cone x surface wrap x low beam cut x near fade; calibrated by eye, HEADLIGHT-FIX-2).
//   kGameLamp   the game's PSMultiLight term (volumetrics_lamps.h): colour x saturate(I/d^2) x cone x N.L, a plateau out to d = sqrt(I), reach (1 per cent) sqrt(100 I).
//               Here `color` already holds rgb x I x the prop pass's fade, so the term is  color x min(1/I, 1/d^2) x cone x N.L  (law_a = I).
//   kOmni       the same without the cone (police, neon, an omni lamp).
//   the volume's form of a lamp (hlsl/volumetrics_inject_cs.hlsl): colour x w(d)^2 / (d^2 + near^2), w = saturate(1 - (d/range)^4): VolumeUnits / VolumeNearSq / VolumeFalloff
//   are the units and the near radius the volume gets from a Light (light_convert.h ToVolLamp), so the volume's lamp is a PROJECTION of the light, not a second description.
//
// The cone of the two lamp laws is a smooth step in the cosine (the headlight's and the volume's: volumetrics_rules.h SpotFactor). // CHECK: the game's own ramp in
// sub_822FD2C8 may be linear in the cosine; the lamp A/B of GB-H1 (survey C8) says, and the cone is one line here and one in the HLSL twin.
// Beyond `range` every law is 0 (a hard cut: for a lamp it is where the game's term is 1 per cent, and the deferred pass scissors there).
//
// Surface: a world position and a unit normal facing the viewer. `visibility` is the shadow lookup's answer (1 without a shadow), the last factor of every law. Pure: no
// renderer, no guest, no SDK.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "light_frame.h"

namespace larecomp::gpu::light_laws {

inline constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// the game's lamp
// ---------------------------------------------------------------------------

// Where the game's term is 1 per cent: the reach the mod loader quotes (mod_lights.cpp "20 reaches sqrt(100 x 20) = 44.7 m"), kept between these (a lamp of intensity 1 would reach
// 10 m, one of 400 would reach 200).
inline constexpr float kReachShare = 100.0f;
inline constexpr float kMinReach = 8.0f;
inline constexpr float kMaxReach = 120.0f;

// The game's term where its lamps' light saturates: 1 inside the plateau (d^2 <= I), I / d^2 beyond (PSMultiLight's saturate(I / d^2)).
inline float GameLampTerm(float intensity, float dist) {
  if (!(intensity > 0.0f)) {
    return 0.0f;
  }
  const float d2 = dist * dist;
  return d2 <= intensity ? 1.0f : intensity / d2;
}
inline float GameReach(float intensity) { return intensity > 0.0f ? std::sqrt(kReachShare * intensity) : 0.0f; }
// The `range` of a lamp's Light.
inline float LampRange(float intensity) { return std::clamp(GameReach(intensity), kMinReach, kMaxReach); }
// How far a street lamp's SHADOW is made for, not its light (a 512 texel map cannot cover the volume's 45 to 100 m): where the lamp's term is an eighth, sqrt(8 I), kept between
// 14 and 30 m (lamp_shadow_rules.h ShadowReach on feat/lamp-shadows: 21 m for I 55, 28 m for 100). It is ShadowRef::reach of a lamp's Light.
inline float LampShadowReach(float intensity) {
  return intensity > 0.0f ? float(std::clamp(std::sqrt(8.0 * double(intensity)), 14.0, 30.0)) : 14.0f;
}

// The game's cone as the cosines of its two edges: full at `inner`, zero at `outer` (sub_822FD2C8; half angles of the angle and of the spread, degrees; a spread of 0 counts as 1, a
// negative one puts the soft edge inside the angle). A point light (type 0, or an angle of 360) has none: the caller gives it -1.
inline void ConeCosines(float cone_deg, float spread_deg, float* cos_outer, float* cos_inner) {
  const float half = 0.5f * std::clamp(cone_deg, 0.0f, 360.0f);
  const float s = 0.5f * (spread_deg == 0.0f ? 1.0f : spread_deg);
  float inner = half;
  float outer = half;
  if (s >= 0.0f) {
    outer = half + s;
  } else {
    inner = half + s;
  }
  inner = std::clamp(inner, 0.0f, 179.0f);
  outer = std::clamp(outer, inner + 0.1f, 179.9f);
  const float to_rad = kPi / 180.0f;
  *cos_inner = std::cos(inner * to_rad);
  *cos_outer = std::cos(outer * to_rad);
}

// ---------------------------------------------------------------------------
// the headlight's term (the rig's numbers, headlight_rig_rules.h)
// ---------------------------------------------------------------------------

inline constexpr float kHeadlightPeak = 2.0f;              // the term on the beam's axis, close to the lamp (before the fall-off)
inline constexpr float kHeadlightFalloffDistance = 26.0f;  // m: D, the distance at which the 1/(1 + (d/D)^2) fall-off is a half: Light::law_a of a headlight
inline constexpr float kSurfaceWrap = 0.45f;               // surface term = saturate(wrap + (1 - wrap) N.L)
inline constexpr float kCutLow = 0.10f;                    // low beam: the sine of the angle a point is above the lamp where the cut-off starts
inline constexpr float kCutHigh = 0.38f;                   // ... and where it is complete
inline constexpr float kNearStart = 0.8f;                  // m: nothing is lit nearer to the lamp than this
inline constexpr float kNearSpan = 1.5f;                   // m: and the light is complete this much farther

// ---------------------------------------------------------------------------
// small helpers (float, 3 components)
// ---------------------------------------------------------------------------

inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float Saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
// HLSL's smoothstep.
inline float SmoothStep(float a, float b, float x) {
  const float t = Saturate((x - a) / (b - a));
  return t * t * (3.0f - 2.0f * t);
}

// The surface a light is evaluated on.
struct LightSurface {
  float position[3] = {0.0f, 0.0f, 0.0f};
  float normal[3] = {0.0f, 1.0f, 0.0f};  // unit, facing the viewer
};

// ---------------------------------------------------------------------------
// the three laws
// ---------------------------------------------------------------------------

// kHeadlight: the scalar part of the term (without the colour). The order of the factors and every operation is headlight_rules::LampScale's, so the two agree bit for bit
// (D is the light's law_a, kHeadlightFalloffDistance when it says none).
inline float HeadlightScale(const Light& l, const LightSurface& s, float visibility = 1.0f) {
  const float to_light[3] = {l.pos[0] - s.position[0], l.pos[1] - s.position[1], l.pos[2] - s.position[2]};
  const float dist2 = Dot3(to_light, to_light);
  const float range2 = l.range * l.range;
  if (dist2 >= range2 || dist2 < 1.0e-6f) {
    return 0.0f;
  }
  const float inverse = 1.0f / std::sqrt(dist2);
  const float lv[3] = {to_light[0] * inverse, to_light[1] * inverse, to_light[2] * inverse};  // from the surface to the lamp
  const float cosine = -Dot3(lv, l.dir);                                                    // the angle from the beam's axis
  if (cosine <= l.cos_outer) {
    return 0.0f;
  }
  const float cone = SmoothStep(l.cos_outer, l.cos_inner, cosine);
  const float dist = dist2 * inverse;
  const float d_fall = l.law_a > 0.0f ? l.law_a : kHeadlightFalloffDistance;
  const float falloff = 1.0f / (1.0f + dist2 * (1.0f / (d_fall * d_fall)));
  float window = Saturate(1.0f - dist2 / range2);
  window *= window;
  const float surface_term = Saturate(kSurfaceWrap + (1.0f - kSurfaceWrap) * Dot3(s.normal, lv));
  const float cutoff = 1.0f - SmoothStep(kCutLow, kCutHigh, -lv[1]);
  const float near_fade = Saturate((dist - kNearStart) * (1.0f / kNearSpan));
  return kHeadlightPeak * cone * falloff * window * surface_term * cutoff * near_fade * visibility;
}

// kGameLamp and kOmni: the game's term. law_a is the plateau's radius squared (a lamp's intensity I); a point light (kOmni, or a cone of cos_outer <= -0.999) has no cone;
// the surface term is the game's N.L, saturated.  // CHECK: the game's N.L and cone ramp against a grid lamp (GB-H1's lamp A/B, survey C8).
inline float GameLampScale(const Light& l, const LightSurface& s, float visibility = 1.0f) {
  const float to_light[3] = {l.pos[0] - s.position[0], l.pos[1] - s.position[1], l.pos[2] - s.position[2]};
  const float dist2 = Dot3(to_light, to_light);
  const float range2 = l.range * l.range;
  if (!(l.law_a > 0.0f) || dist2 >= range2 || dist2 < 1.0e-6f) {
    return 0.0f;
  }
  const float inverse = 1.0f / std::sqrt(dist2);
  const float lv[3] = {to_light[0] * inverse, to_light[1] * inverse, to_light[2] * inverse};
  float cone = 1.0f;
  if (l.law == LightLaw::kGameLamp && l.cos_outer > -0.999f) {
    const float cosine = -Dot3(lv, l.dir);
    if (cosine <= l.cos_outer) {
      return 0.0f;
    }
    cone = SmoothStep(l.cos_outer, l.cos_inner, cosine);
  }
  const float distance_term = dist2 <= l.law_a ? 1.0f / l.law_a : 1.0f / dist2;
  const float n_dot_l = Saturate(Dot3(s.normal, lv));
  return distance_term * cone * n_dot_l * visibility;
}

// The scalar part of one light's term at a surface (colour excluded), by the light's law.
inline float LightScale(const Light& l, const LightSurface& s, float visibility = 1.0f) {
  return l.law == LightLaw::kHeadlight ? HeadlightScale(l, s, visibility) : GameLampScale(l, s, visibility);
}

// The light a surface gets: colour x LightScale.
inline void LightTerm(const Light& l, const LightSurface& s, float visibility, float out[3]) {
  const float scale = LightScale(l, s, visibility);
  for (int c = 0; c < 3; ++c) {
    out[c] = l.color[c] * scale;
  }
}

// ---------------------------------------------------------------------------
// the volume's form of a light
// ---------------------------------------------------------------------------

// The volume's lamp term is  colour x w(d)^2 / (d^2 + near^2)  (volumetrics_rules.h LampFalloff). A Light becomes a VolLamp by these units (light_convert.h ToVolLamp), so that the
// beam in the air and the pool on the road are the same light (volumetrics_lamps.h and volumetrics_headlights.h say the same for the two sources they have today).
inline constexpr float kVolumeNearMin = 0.8f;              // m: the volume's least near radius (volumetrics_rules.h kLampNear)
inline constexpr float kLampNearShare = 0.6180340f;        // the near radius squared as a share of a lamp's intensity: the golden ratio's conjugate (volumetrics_lamps.h)

// The multiplier from a Light's colour to the volume's colour: a headlight is colour x kPeak x D^2 (its term kPeak / (1 + (d/D)^2) is  kPeak D^2 / (d^2 + D^2)), a lamp's colour is
// already in the game's units.
inline float VolumeUnits(const Light& l) {
  if (l.law == LightLaw::kHeadlight) {
    const float d_fall = l.law_a > 0.0f ? l.law_a : kHeadlightFalloffDistance;
    return kHeadlightPeak * d_fall * d_fall;
  }
  return 1.0f;
}
// The near radius squared: a headlight's D^2, a lamp's share of its plateau (never under the volume's least).
inline float VolumeNearSq(const Light& l) {
  if (l.law == LightLaw::kHeadlight) {
    const float d_fall = l.law_a > 0.0f ? l.law_a : kHeadlightFalloffDistance;
    return d_fall * d_fall;
  }
  return std::max(kLampNearShare * std::max(l.law_a, 0.0f), kVolumeNearMin * kVolumeNearMin);
}
// The distance law of the volume's lamp: 1 / (d^2 + near^2), windowed to exactly 0 at the range by saturate(1 - (d / range)^4)^2.
inline float VolumeFalloff(float dist, float range, float near_sq) {
  if (!(dist < range)) {
    return 0.0f;
  }
  const float r = dist / range;
  const float w = std::clamp(1.0f - r * r * r * r, 0.0f, 1.0f);
  return w * w / (dist * dist + (near_sq > 0.0f ? near_sq : kVolumeNearMin * kVolumeNearMin));
}
// The volume's term of a light on its axis at `dist`, in the units of the colour's brightest channel (the same number as volumetrics_lamps.h VolLampTerm of its VolLamp).
inline float VolumeTerm(const Light& l, float dist) {
  const float peak = std::max({l.color[0], l.color[1], l.color[2]}) * l.vol_weight * VolumeUnits(l);
  return peak * VolumeFalloff(dist, l.range, VolumeNearSq(l));
}

// ---------------------------------------------------------------------------
// the shadow rule
// ---------------------------------------------------------------------------

// THE ONE COMBINE RULE (ShadowRef): `visibility[i]` is the lookup's answer for entry i of the ShadowFrame (1 lit .. 0 in shadow). The valid views of a light are averaged, a
// kStatic entry and the kLive entry after it are one view whose visibility is their PRODUCT, and the result is blended by the weight: lit = lerp(1, visibility, weight). A light
// with no atlas views (kNone, kRtMask: the mask's own channel is read by its consumer, a plain lerp) or none valid is lit.
inline float CombineShadow(const ShadowFrame& frame, const ShadowRef& ref, const float visibility[kMaxLightShadowViews]) {
  if (ref.kind != ShadowKind::kAtlas || ref.view_count == 0) {
    return 1.0f;
  }
  float sum = 0.0f;
  uint32_t n = 0;
  const uint32_t end = std::min<uint32_t>(uint32_t(ref.first_view) + ref.view_count, kMaxLightShadowViews);
  for (uint32_t e = ref.first_view; e < end; ++e) {
    const bool valid = frame.views[e].valid != 0;
    float v = visibility[e];
    if (frame.view_layer[e] == ShadowLayer::kStatic && e + 1 < end && frame.view_layer[e + 1] == ShadowLayer::kLive) {
      v *= visibility[e + 1];
      ++e;  // the live twin is part of this view (valid or not, it is not a view of its own)
    }
    if (valid) {
      sum += v;
      ++n;
    }
  }
  if (n == 0) {
    return 1.0f;
  }
  const float mean = sum / float(n);
  return 1.0f + (mean - 1.0f) * ref.weight;
}

}  // namespace larecomp::gpu::light_laws

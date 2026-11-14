#pragma once
// ===========================================================================
// MCLA GPU renderer -- the bounce light's cap, falloff and gain rules (pure maths)
// ===========================================================================
// What hlsl/ssgi_common.hlsli, ao_color_ps.hlsl, ao_main_ps.hlsl and ao_apply_ps.hlsl (AO_GI) compute, written
// once in plain C++ so that ssgi_test.cpp can check the rules and that the shaders' constants are these. This file
// includes nothing of Plume or ReXGlue.
//
// WHAT LIMITED THE BOUNCE LIGHT (owner, 2026-11-13: "nearly invisible, even strength 2 shows a minimal difference")
//   1. A per-pixel cap relative to the pixel's OWN colour (the bounce could add at most half of it). The bounce
//      matters most where the pixel is dark (a shaded wall beside a sunlit road, the ground under an overpass), and
//      there half of nothing is nothing: the cap made the strength setting stop working above about 0.5.
//   2. The gather looked no farther than the occlusion's own radius (1 to 3 m, the same samples give the
//      occlusion's visibility). A shaded street's light comes from the sunlit road and the walls across it, 4 to 10 m
//      away.
//   3. The strength was capped at 2 and the default was 0.75.
//
// THE NEW RULES
//   Incoming radiance. What a sample of the scene colour may carry into the bounce is capped by a SOFT clip of its
//   luma at `cap` times the frame's light (ambient + sun), never under kCapFloor (SoftClip: linear up to half the
//   cap, then bending to the cap, never over it): a lamp, neon or the sun-lit sky is a source of light, and it
//   lights its neighbours as brightly as a white surface in full sun would, not whiter. A diffuse surface is never
//   brighter than the frame's light (albedo is at most 1), so a cap of 1 or more keeps every real surface.
//   Source of light. A pixel whose colour is above kSourceMult times the frame's light (never under kCapFloor)
//   starts to count as a source, entirely so at kSourceSpan times that: it receives no bounce (it has no albedo).
//   Gain. The factor that lights a pixel is bounce = strength * fade * surface * B / max(T, luma(colour)) (T the light
//   the game's constants say fell on it). It is kept by SoftClip to `limit` times the pixel's own colour: a large
//   number (2 to 10), a guard against amplifying the colour's own noise in a nearly black pixel, not the look.
//   Ceiling. The bounce may fill a pixel up to what a white surface in full sun would have, `cap` times the frame's
//   light, and not past it: the light it adds is kept by SoftClip to the room left under that ceiling (a pixel at or
//   over it gets nothing). That is what makes the stronger settings safe: a shadow gets lighter, nothing gets
//   whiter than the sun-lit white of the frame.
//   Reach. A second ring of samples from the occlusion's radius out to `reach` metres (5 to 16), a step or two a
//   side of each slice, from the coarser levels of the colour and depth chains, with the horizon's falloff at the
//   reach (full to 38% of it, none at it). The ring only adds light (the occlusion's visibility stays what the
//   near samples found: it comes from the near mask alone) and only into sectors the near samples left open.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::ssgi_light {

// The same numbers as the shaders' (hlsl/ssgi_common.hlsli); ssgi_test.cpp compares them.
inline constexpr float kCapFloor = 0.5f;        // the least a cap is, in the scene colour's units (a night's lamp pool)
inline constexpr float kKnee = 0.5f;            // SoftClip is linear up to this share of its limit
inline constexpr float kSourceMult = 2.0f;      // a pixel starts to count as a source of light at this multiple of the frame's light
inline constexpr float kSourceSpan = 1.5f;      // ... and is entirely one at this multiple of that start
inline constexpr float kFalloffRatio = 0.615f;  // the horizon falloff range as a share of the radius (ao.cpp kFalloffRatio)

// The far ring (hlsl/ao_main_ps.hlsl, AO_GI).
inline constexpr float kFarMaxRadiusShare = 0.4f;  // the ring never reaches farther than this share of the image's height
inline constexpr float kFarMinGrowth = 1.25f;      // the ring exists where it reaches this much farther than the near samples ...
inline constexpr float kFarMinExtraPx = 2.0f;      // ... and at least this many pixels farther
inline constexpr uint32_t kFarStepsHigh = 2;       // ring steps a side of each slice at the bounce light's high
inline constexpr uint32_t kFarStepsLow = 1;        // ... at low
inline constexpr uint32_t kFarStepsMax = 7;        // what the push block's three bits hold

// How the apply pass receives its numbers in the push block's flags (ao_apply_ps.hlsl): the gain limit in bits
// 8-15 in units of kLimitUnit, the cap in bits 26-31 in units of kCapUnit.
inline constexpr float kLimitUnit = 0.04f;
inline constexpr uint32_t kLimitShift = 8;
inline constexpr uint32_t kLimitMask = 0xFFu << kLimitShift;
inline constexpr float kCapUnit = 0.25f;
inline constexpr uint32_t kCapShift = 26;
inline constexpr uint32_t kCapMask = 0x3Fu << kCapShift;
inline constexpr float kLimitMax = 255.0f * kLimitUnit;  // 10.2
inline constexpr float kCapMax = 63.0f * kCapUnit;       // 15.75

struct Rgb {
  float r = 0.0f, g = 0.0f, b = 0.0f;
};

inline float Luma(const Rgb& c) {
  return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}
inline float MaxChannel(const Rgb& c) {
  return std::max(std::max(c.r, c.g), c.b);
}
inline Rgb Scaled(const Rgb& c, float s) {
  return {c.r * s, c.g * s, c.b * s};
}
inline float Saturate(float x) {
  return std::min(std::max(x, 0.0f), 1.0f);
}

// x up to kKnee * limit unchanged, then bending to `limit` (1 - exp, so the slope is 1 at the knee and falls to 0):
// continuous, never over `limit`, never decreasing. A limit of 0 or less is 0.
inline float SoftClip(float x, float limit) {
  if (!(limit > 0.0f)) {
    return 0.0f;
  }
  const float knee = kKnee * limit;
  if (!(x > knee)) {
    return x;
  }
  const float span = limit - knee;
  return knee + span * (1.0f - std::exp(-(x - knee) / span));
}

// The cap of a sample's radiance (luma) for a cap multiple and the frame's ambient and sun lumas.
inline float RadianceCap(float cap_mult, float ambient, float sun) {
  return std::max(cap_mult * (ambient + sun), kCapFloor);
}

// What a sample of the scene colour carries into the bounce: its colour scaled so that its luma is SoftClip(luma,
// cap) (the hue stays). `colour` is finite and not negative.
inline Rgb CapRadiance(const Rgb& colour, float cap) {
  const float luma = Luma(colour);
  if (!(luma > 1e-6f)) {
    return colour;
  }
  return Scaled(colour, SoftClip(luma, cap) / luma);
}

// Where a pixel's colour starts to count as a source of light, in luma.
inline float SourceStart(float ambient, float sun) {
  return std::max(kSourceMult * (ambient + sun), kCapFloor);
}

// 0 a surface, 1 a source of light: a ramp from `start` to kSourceSpan times it.
inline float SourceShare(float luma, float start) {
  return Saturate((luma / std::max(start, 1e-3f) - 1.0f) / (kSourceSpan - 1.0f));
}

// The horizon falloff weight of a sample `dist` world units from the pixel for a ring of reach `reach` (ao_main_ps.hlsl
// falloff_mul and falloff_add): 1 up to (1 - kFalloffRatio) of the reach (38.5%), falling in a straight line to 0 at the
// reach.
inline float FarFalloff(float dist, float reach) {
  const float mul = -1.0f / (kFalloffRatio * std::max(reach, 1e-3f));
  const float add = 1.0f / kFalloffRatio;
  return Saturate(dist * mul + add);
}

// The ring's outer radius in pixels: the reach at this depth's scale, never over kFarMaxRadiusShare of the height.
inline float FarRadiusPx(float reach, float pixels_per_unit, float image_height) {
  return std::min(reach * pixels_per_unit, kFarMaxRadiusShare * image_height);
}

// Whether the ring is drawn for a pixel whose near samples end at `near_end_px` and whose ring would end at `far_px`.
inline bool FarRingExists(float near_end_px, float far_px, uint32_t steps) {
  return steps > 0 && far_px > near_end_px * kFarMinGrowth + kFarMinExtraPx;
}

// Where step `s` of `steps` is on its side of a slice, in pixels from the pixel: the share (s + noise) / steps,
// squared (most samples near the ring's inner edge, as the near samples are near the pixel), between the near
// samples' end and the ring's end.
inline float FarOffsetPx(float near_end_px, float far_px, uint32_t s, uint32_t steps, float noise) {
  float share = (float(s) + noise) / float(std::max(steps, 1u));
  share *= share;
  return near_end_px + (far_px - near_end_px) * share;
}

// The levels of the chains a ring sample of `length_px` is read from: 1 or 2 (never the full-size level: the ring
// is the far part of the picture), as the near samples' rule log2(length) - 3 says.
inline uint32_t FarLevel(float length_px) {
  const float level = std::min(std::max(std::log2(std::max(length_px, 1.0f)) - 3.0f, 1.0f), 2.0f);
  return uint32_t(level);
}

inline uint32_t FarSteps(bool high, int forced, float reach) {
  if (!(reach > 0.0f)) {
    return 0;
  }
  if (forced >= 0) {
    return std::min(uint32_t(forced), kFarStepsMax);
  }
  return high ? kFarStepsHigh : kFarStepsLow;
}

// The push block's codes of the limit and the cap, and back.
inline uint32_t LimitCode(float limit) {
  return uint32_t(std::lround(std::min(std::max(limit, 0.0f), kLimitMax) / kLimitUnit));
}
inline float LimitOfCode(uint32_t code) {
  return float(code & 0xFFu) * kLimitUnit;
}
inline uint32_t CapCode(float cap) {
  return uint32_t(std::lround(std::min(std::max(cap, 0.0f), kCapMax) / kCapUnit));
}
inline float CapOfCode(uint32_t code) {
  return float(code & 0x3Fu) * kCapUnit;
}

// The apply pass's numbers for one pixel (ao_apply_ps.hlsl, AO_GI).
struct ApplyIn {
  Rgb bounce_radiance;   // B: the mean radiance of what blocks the pixel's hemisphere (x of the image is the visibility, yzw this)
  Rgb colour;            // the pixel's own colour as the chain holds it (capped), the albedo estimate's numerator
  float source = 0.0f;   // the chain's alpha: 0 a surface, 1 a source of light
  float ambient = 0.0f;  // the frame's ambient luma (PS c26)
  float sun = 0.0f;      // the frame's full sun luma (PS c39)
  float sun_term = 0.0f; // sun * N.L * the game's shadow mask at the pixel: what T takes of the sun
  float ao_part = 1.0f;  // what the occlusion and the contact shadows already multiply the pixel by (their factor)
  float strength = 1.0f;
  float fade = 1.0f;     // the distance fade, 1 near
  float limit = 2.5f;    // the most the bounce may add, in multiples of the pixel's colour
  float cap = 2.0f;      // the cap multiple (the ceiling is cap times the frame's light)
};

// T, the light the constants say fell on the pixel.
inline float LightT(const ApplyIn& in) {
  return std::max(in.ambient + in.sun_term, 1e-3f);
}

// The bounce factor the pixel's colour is multiplied by, added to the occlusion's (and the contact shadows') factor.
inline Rgb BounceFactor(const ApplyIn& in) {
  const float luma_c = Luma(in.colour);
  const float surface = 1.0f - Saturate(in.source);
  const float scale = in.strength * in.fade * surface / std::max(LightT(in), luma_c);
  Rgb bounce = Scaled(in.bounce_radiance, scale);
  // The gain limit.
  const float peak = MaxChannel(bounce);
  if (peak > 0.0f) {
    bounce = Scaled(bounce, SoftClip(peak, in.limit) / peak);
  }
  // The ceiling: what is added may fill the pixel up to the cap's light and no more.
  const float ceiling = RadianceCap(in.cap, in.ambient, in.sun);
  const float room = std::max(ceiling - luma_c * in.ao_part, 0.0f);
  const Rgb added_rgb = {in.colour.r * bounce.r, in.colour.g * bounce.g, in.colour.b * bounce.b};
  const float added = Luma(added_rgb);
  if (added > 1e-6f) {
    bounce = Scaled(bounce, SoftClip(added, room) / added);
  }
  return bounce;
}

// The rule the bounce light had before: the factor kept under 0.5 of the pixel's own colour by a hard rescale (kept
// here for the test's comparison).
inline Rgb OldBounceFactor(const ApplyIn& in, float old_limit) {
  const float luma_c = Luma(in.colour);
  const float surface = 1.0f - Saturate(in.source);
  const float scale = in.strength * in.fade * surface / std::max(LightT(in), luma_c);
  Rgb bounce = Scaled(in.bounce_radiance, scale);
  const float peak = MaxChannel(bounce);
  if (peak > old_limit) {
    bounce = Scaled(bounce, old_limit / peak);
  }
  return bounce;
}

}  // namespace mcla::gpu::ssgi_light

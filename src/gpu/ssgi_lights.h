#pragma once
// ===========================================================================
// MCLA GPU renderer -- the screen-space bounce light carries the LIGHTS (GI-BOUNCE-SS, perf/GI-BOUNCE.md): the rules and the C++ twin of the shader's reprojection (pure maths)
// ===========================================================================
// WHAT THE OWNER ASKED (2026-11-22): neon, headlights, street lamps, tail lights, decorative lights, anything the game lights with, should BOUNCE (a wall next to a neon car picks up its colour; a garage's
// wall lit by the headlights lights the floor and the car's side). The bounce light's source of radiance is the colour chain (colour_pre: the main colour at the AO's trigger, BEFORE the deferred light pass
// of the same frame (kLocalLights: neon, headlights, street lamps), so it never saw them; the game's own forward-lit lights (tail light lenses, lit signs, the forward headlight) were in it already).
//
// THE DESIGN IN ONE PICTURE. The light pass of frame N-1 added light L(N-1) = (the colour after it) - (the colour before it) to the main colour. Frame N's colour chain takes, in each texel, the colour it has
// always taken (cleaned, capped, source-marked) PLUS what the lights of frame N-1 lit at the place the texel was, found by reprojecting the texel into frame N-1 (the AO's own camera motion into the last frame,
// the depth history, the rejection tests of ao_temporal_ps.hlsl), kept under the same cap. The two images that hold L(N-1) are two plain captures at the light pass's two sides (screen_shared.h
// CaptureLightsBefore / CaptureLightsAfter: the colour before and after it, averaged over the MSAA samples, at the traced size); the difference is taken where the chain reads them (hlsl/ao_color_ps.hlsl).
//
// WHY THE DIFFERENCE (and not the last frame's colour_post). colour_post is the colour after the occlusion's apply, the probe field's apply and the lights, with the bounce light's own result in it. Fed back
// as the radiance, it would (1) change every daylight picture (the apply multiplies the occlusion into it: a crease's radiance would be darkened twice, and the bounce would be in its own source: a feedback loop
// whose gain is the strength, up to 2 to 12), (2) double count: the bounce adds indirect light, the direct light of a neon stays the light pass's, and (3) not be capped as a source. The difference is
// exactly what the light pass added (zero where it added nothing: a day without lamps in view is the picture of today, to the bit), has no feedback (the bounce is in neither capture) and is capped like
// every radiance the bounce takes.
//
// THE LAG, and why it shows no trail. Frame N sees the light of frame N-1: one frame behind a moving light. The bounce's own accumulation (ao_temporal_ps.hlsl, up to 16 frames) is the same kind of lag, with a
// variance clip that follows a changed light in about ten frames, so the one frame added here is small against it. What the lag must not do is put the light where it no longer is on something that moved:
//   - a texel of a VEHICLE (the depth's vehicle mark) takes no light: the car's own neon and headlight lit its own body, which is not a radiance that bounces onto the road a frame behind it;
//   - a history texel that was a vehicle is rejected (what a car covered a frame ago, and the car left it: the road under it is lit by the neon of the car, which moved on);
//   - a texel that was another surface a frame ago (a disocclusion: the plane and depth tests below) takes nothing, and the confidence fades the light in over the same smoothstep as the AO's history.
// The first frame, a camera cut, another kind of view or size, and any frame whose last frame had no light pass: no history, the chain is today's (screen_shared.h LightHistory).
//
// ENERGY. The carried light never takes the texel's radiance over the cap the chain is already held to (ssgi_light.h RadianceCap: cap times the frame's ambient plus sun, never under 0.5): what is added is the
// part of the lights' luma that fits under the room the capped colour leaves, soft-clipped to it (SoftClip: linear to half the room, then bending to it), hue kept. Nothing above the cap, so the apply's ceiling and
// its gain limit see what they have always seen; a source of light (the texel's own source share) takes none of the lights' share (it is the lamp, not the wall).
//
// Written once here so that ssgi_lights_test.cpp can check the rules and that the shader's constants are these; hlsl/ao_color_ps.hlsl and hlsl/ssgi_common.hlsli repeat the numbers. Includes nothing of Plume.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ssgi_light.h"

namespace mcla::gpu::ssgi_lights {

using ssgi_light::Rgb;

// The colour chain's level 0 pass adds the lights' share when this bit of the flags (slots2.z) is set (hlsl/ao_color_ps.hlsl AoLights). Bits 0-7 are the debug view, 8-15 the slices and steps, 16 low, 17-19 the
// denoise radius and 21 the one-sample flag: the frame block FillPush makes sets nothing above 19, so the bit is the pass's alone.
inline constexpr uint32_t kFlag = 1u << 23;
// The normal image's slot rides in the high half of slots.y (the low bits are the colour target's host samples), as in the accumulation pass (ao_gbuffer.h kSlotShift).
inline constexpr uint32_t kNormalShift = 16;
inline constexpr uint32_t kHostSamplesMask = 0xFFu;

// The tests that find a texel's light in the last frame: the AO's own (hlsl/ao_temporal_ps.hlsl kPlaneShare, kPlaneFloor, kDepthShare, kConfidenceLow, kConfidenceHigh), the same numbers so that the lights follow
// the occlusion's history wherever the history holds.
inline constexpr float kPlaneShare = 0.012f;
inline constexpr float kPlaneFloor = 0.02f;
inline constexpr float kDepthShare = 0.08f;
inline constexpr float kDepthFloor = 0.05f;
inline constexpr float kConfidenceLow = 0.15f;
inline constexpr float kConfidenceHigh = 0.75f;
inline constexpr float kNearest = 0.05f;      // a reprojected point nearer than this (view space z) has no place in the history
inline constexpr float kSky = 60000.0f;       // hlsl/ao_common.hlsli kAoSky: what the depth chain holds for the sky and for what it could not place

// The gain of the lights' share (mcla_gpu_ssgi_lights_gain, debug only): 1 takes the lights' radiance as it was lit, 0 is the bounce light as it was.
inline constexpr float kGainDefault = 1.0f;
inline constexpr float kGainMin = 0.0f;
inline constexpr float kGainMax = 8.0f;
inline float GainOf(double setting) {
  if (!(setting == setting)) return kGainDefault;
  return float(std::min(std::max(setting, double(kGainMin)), double(kGainMax)));
}

// A colour channel a half float image keeps and a light is never negative: the difference of the two captures, where the lights added, is >= 0; rounding in a half float can make it a hair under 0 and a NaN is none.
inline float HeldDelta(float post, float pre) {
  const float d = post - pre;
  return d > 0.0f ? d : 0.0f;  // (also false for a NaN: 0)
}
inline Rgb DeltaOf(const Rgb& post, const Rgb& pre) {
  return {HeldDelta(post.r, pre.r), HeldDelta(post.g, pre.g), HeldDelta(post.b, pre.b)};
}

// ---------------------------------------------------------------------------
// the reprojection (hlsl/ao_color_ps.hlsl LightsDelta; the AO's AoToHistory / AoHistoryPos / the four taps of ao_temporal_ps.hlsl)
// ---------------------------------------------------------------------------

struct Lens {
  float tan_x = 1.0f;  // tan of the half field of view, horizontal and vertical
  float tan_y = 1.0f;
};
// This frame's view space (x right, y up, z forward) into the last frame's: the three rows of the 3x4 (the push block's lin, light and extra for this pass; the AO's lin, shape and light for its temporal pass).
struct Motion {
  float rows[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
};
struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 ViewPos(float centre_x, float centre_y, float width, float height, float t, const Lens& lens) {
  const float u = centre_x / width;
  const float v = centre_y / height;
  return {(u * 2.0f - 1.0f) * lens.tan_x * t, (1.0f - v * 2.0f) * lens.tan_y * t, t};
}
inline Vec3 ToHistory(const Motion& m, const Vec3& p) {
  return {m.rows[0][0] * p.x + m.rows[0][1] * p.y + m.rows[0][2] * p.z + m.rows[0][3], m.rows[1][0] * p.x + m.rows[1][1] * p.y + m.rows[1][2] * p.z + m.rows[1][3],
          m.rows[2][0] * p.x + m.rows[2][1] * p.y + m.rows[2][2] * p.z + m.rows[2][3]};
}
inline Vec3 DirToHistory(const Motion& m, const Vec3& n) {
  return {m.rows[0][0] * n.x + m.rows[0][1] * n.y + m.rows[0][2] * n.z, m.rows[1][0] * n.x + m.rows[1][1] * n.y + m.rows[1][2] * n.z,
          m.rows[2][0] * n.x + m.rows[2][1] * n.y + m.rows[2][2] * n.z};
}
inline float Dot(const Vec3& a, const Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float SmoothStep(float lo, float hi, float x) {
  const float t = std::min(std::max((x - lo) / (hi - lo), 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// What the history image holds at a texel: the linear depth the depth pass wrote then (its vehicle mark is the lowest bit of the float: the caller says it), and the two captures' colours.
struct HistoryTexel {
  float depth = kSky;
  bool vehicle = false;
  Rgb post;
  Rgb pre;
};

// What a texel of this frame is: its linear depth (kSky for the sky), whether the depth pass marked it a vehicle, its source share (the colour chain's alpha) and the normal (view space, unit).
struct Pixel {
  int x = 0, y = 0;
  float depth = kSky;
  bool vehicle = false;
  Vec3 normal{0.0f, 0.0f, -1.0f};
};

struct Found {
  Rgb delta;               // the last frame's light at the place the texel was (not yet capped), 0 where nothing is found
  float weight = 0.0f;     // the sum of the taps' weights (bilinear times agreement): 1 = a whole surface found
  float confidence = 0.0f; // smoothstep(kConfidenceLow, kConfidenceHigh, weight)
  bool skipped = false;    // the texel takes no light at all (the sky, a vehicle, no history point)
};

// The texel's light in the last frame: its view position moved into the last frame by the camera's motion, the four texels round the place it lands each by its bilinear weight times how well it agrees with
// this texel's surface (the plane test, the depth within 8 percent), a vehicle or a sky texel of the history counting for nothing. `fetch(x, y)` returns the history's texel (x, y in 0..w-1, 0..h-1).
template <class FetchFn>
Found HistoryLight(const Pixel& p, int width, int height, const Lens& now, const Lens& before, const Motion& motion, FetchFn fetch) {
  Found out;
  if (p.depth >= kSky * 0.5f || p.vehicle) {
    out.skipped = true;
    return out;
  }
  const float w = float(width), h = float(height);
  const Vec3 pc = ViewPos(float(p.x) + 0.5f, float(p.y) + 0.5f, w, h, p.depth, now);
  const Vec3 pe = ToHistory(motion, pc);
  const Vec3 ne = DirToHistory(motion, p.normal);
  if (!(pe.z > kNearest)) {
    out.skipped = true;
    return out;
  }
  const float u = 0.5f + 0.5f * pe.x / (pe.z * before.tan_x);
  const float v = 0.5f - 0.5f * pe.y / (pe.z * before.tan_y);
  const float hx = std::min(std::max(u * w - 0.5f, -2.0f), w + 2.0f);
  const float hy = std::min(std::max(v * h - 0.5f, -2.0f), h + 2.0f);
  const int x0 = int(std::floor(hx));
  const int y0 = int(std::floor(hy));
  const float fx = hx - float(x0);
  const float fy = hy - float(y0);
  const float sigma = kPlaneShare * pe.z + kPlaneFloor;
  Rgb sum;
  float weight = 0.0f;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const int qx = x0 + i, qy = y0 + j;
      if (qx < 0 || qy < 0 || qx >= width || qy >= height) continue;
      const HistoryTexel t = fetch(qx, qy);
      if (t.vehicle || t.depth >= kSky * 0.5f) continue;
      const Vec3 ph = ViewPos(float(qx) + 0.5f, float(qy) + 0.5f, w, h, t.depth, before);
      const Vec3 d{ph.x - pe.x, ph.y - pe.y, ph.z - pe.z};
      const float off_plane = Dot(ne, d) / sigma;
      float tap = std::exp(-0.5f * off_plane * off_plane);
      tap *= std::fabs(t.depth - pe.z) < kDepthShare * pe.z + kDepthFloor ? 1.0f : 0.0f;
      tap *= (i == 0 ? 1.0f - fx : fx) * (j == 0 ? 1.0f - fy : fy);
      const Rgb delta = DeltaOf(t.post, t.pre);
      sum.r += delta.r * tap;
      sum.g += delta.g * tap;
      sum.b += delta.b * tap;
      weight += tap;
    }
  }
  out.weight = weight;
  out.confidence = SmoothStep(kConfidenceLow, kConfidenceHigh, weight);
  const float inv = out.confidence / std::max(weight, 1e-4f);
  out.delta = {sum.r * inv, sum.g * inv, sum.b * inv};
  return out;
}

// ---------------------------------------------------------------------------
// the energy rule (hlsl/ao_color_ps.hlsl CarryLights)
// ---------------------------------------------------------------------------

// The chain's colour for a texel whose cleaned and capped colour is `pre` (its luma is under `cap`) and whose last frame's light is `delta`: the part of the lights' luma (times `gain`, times what is NOT a source of
// light in the texel) that fits under the cap, soft-clipped to the room the capped colour leaves, hue kept. A texel with no light, or a gain of 0, is `pre` to the bit.
inline Rgb CarryLights(const Rgb& pre, const Rgb& delta, float gain, float cap, float source_share) {
  const float keep = gain * (1.0f - ssgi_light::Saturate(source_share));
  const Rgb d = {delta.r * keep, delta.g * keep, delta.b * keep};
  const float luma_d = ssgi_light::Luma(d);
  if (!(luma_d > 1e-6f)) {
    return pre;
  }
  const float room = std::max(cap - ssgi_light::Luma(pre), 0.0f);
  const float scale = ssgi_light::SoftClip(luma_d, room) / luma_d;
  return {pre.r + d.r * scale, pre.g + d.g * scale, pre.b + d.b * scale};
}

// ---------------------------------------------------------------------------
// the history's validity (screen_shared.h LightHistory; the host side of the frame)
// ---------------------------------------------------------------------------

struct Key {
  bool valid = false;        // both captures were made in `frame`
  uint64_t frame = 0;        // the owner's frame count of the capture
  bool pause = false;        // the pause menu's, the garage's or the map's world
  uint32_t size[2] = {};     // the traced size both images are
};

// The history is usable in frame `frame` (of the view kind `pause`, traced size w x h) when the captures were made in the frame before it, of the same kind of view and at the same size, and the AO's own history
// holds this frame (its camera pair is known and no cut: `ao_history`), since the motion the reprojection uses is the AO's.
constexpr bool Usable(const Key& last, uint64_t frame, bool pause, uint32_t w, uint32_t h, bool ao_history) {
  return ao_history && last.valid && last.frame + 1 == frame && last.pause == pause && last.size[0] == w && last.size[1] == h && w != 0 && h != 0;
}

// The two captures are made in a frame that ran the bounce light (the colour chain this frame), after the light pass was drawn or not: with no light pass in the frame nothing was added and the images are not
// made valid (the next frame's chain is today's).
constexpr bool CaptureAfter(bool wanted, bool chain_made, bool before_done, bool light_pass_ran) {
  return wanted && chain_made && before_done && light_pass_ran;
}
constexpr bool CaptureBefore(bool wanted, bool chain_made, bool light_pass_wanted) {
  return wanted && chain_made && light_pass_wanted;
}

// The player-facing decision: the lights' share is part of the bounce light wherever the bounce light runs (the BOUNCE LIGHT row's levels HIGH and ULTRA, and the debug override), from the owner's passes
// (the extraction records the chain) unless the debug switch says the old chain.
constexpr bool Wanted(bool bounce_light_on, bool setting, bool extracting) {
  return bounce_light_on && setting && extracting;
}

}  // namespace mcla::gpu::ssgi_lights

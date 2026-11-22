#pragma once
// ===========================================================================
// MCLA GPU renderer -- the stochastic effects without a temporal upscaler: noise that changes every frame, resolved by a history of their own (pure maths, no GPU)
// ===========================================================================
// NOISE-RESOLVE (2026-11-22, on the NOISE-AUDIT, perf\NOISE-AUDIT.md CHECK 1). AGENTS.md AGENT-RULE-NOISE says every stochastic effect uses noise that changes EVERY frame and is resolved by a temporal
// or spatial filter whose kernel covers it. The first fixes kept six effects on ONE pattern while no temporal upscaler runs (`plan.upscaler ? frame : 0`, mcla_gpu_upscaler off: plain 2x MSAA, which
// is what most players get), because none of them had a history to average a pattern that changes: it would have boiled as grain at 60 Hz (the owner's "dithering artifacts on the world's shadows"
// of 2026-11-19 was exactly a per pixel noise nothing averaged). Now each of them has the resolve that lets it vary, and only while no temporal upscaler runs (with one the upscaler's own history
// resolves, as before):
//
//   effect                                   noise                                         resolve without an upscaler                                        where
//   contact shadows' march                   ray start (blue tile + R2 by the frame)        the occlusion's temporal pass over the filtered contact image      ao.cpp RecordContactHistory
//   car traced sun ray, city traced sun rays disc sample (blue tile + R2 by sun_frame)      the same history: the march takes the darker of its shadow and     ao.cpp (the rays go through the march)
//                                                                                          the ray's, the filter and the history follow
//   sun shafts                               ray start (blue tile + R2 by the frame)        a reprojected blend of the blurred image with the last frame's     shafts.cpp RecordShafts, shafts_temporal_ps.hlsl
//   car reflection cone                      cone turn (blue tile + golden step by phase)   the a-trous passes, then a blend with the last frame's result      car_rt_refl.cpp, car_rt_refl_denoise_cs.hlsl (pass 2)
//                                                                                          while the view of the car stands still
//   sun shadow filter's disk                 (not varied: see shadow_filter_resolve_test.cpp for the numbers; the disk keeps its fixed turn)
//
// mcla_gpu_noise_resolve_no_upscaler (debug, on) is the A/B: off, every effect is what the first fixes made it (a fixed blue pattern without an upscaler).
//
// This header is the place for the numbers and rules the hosts and the shaders share, with the twins that check them (noise_resolve_test.cpp, shafts_temporal_test.cpp, ...).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::noise_resolve {

// Whether an effect's frame term advances this frame: a temporal upscaler averages it, or (NOISE-RESOLVE) the effect's own history does. Else the first fixes' fixed pattern.
constexpr bool Varies(bool temporal_upscaler, bool own_history) { return temporal_upscaler || own_history; }

// The frame term the effect's noise takes: the scene's noise index (scene_noise.h NoiseIndex) masked to the effect's wrap, or 0 for the fixed pattern.
constexpr uint32_t FrameTerm(bool temporal_upscaler, bool own_history, uint64_t noise_index, uint32_t mask) {
  return Varies(temporal_upscaler, own_history) ? uint32_t(noise_index & mask) : 0u;
}

// ---------------------------------------------------------------------------
// the sun shafts' history (hlsl/shafts_temporal_ps.hlsl)
// ---------------------------------------------------------------------------
// The shafts' image is the sky's brightness blurred along the rays to the sun, a smooth low frequency picture (half the picture's size, 960 x 540 at most); the noise is the ray's start inside one step,
// so a texel's error is a fraction of one step's brightness change along its ray. It has no depth, but everything in it (the sky's edge: roofs, trees, walls against the sky) is far, so a camera that
// turns moves it with the sun's place in the picture: the history is read at the texel's place shifted by the sun's own move (a 2D shift, no matrices). A texel the shift takes off the image, or a
// frame whose sun moved more than kShaftsMaxShift of the image's height, has no history (a cut, a very fast turn, the first frame). The history is clamped to the range of the frame's 3 x 3
// neighbourhood plus a margin (the clamp of a temporal anti-aliasing filter), so a silhouette that moved by parallax leaves a ghost only inside the margin.
inline constexpr float kShaftsHistoryWeight = 0.8f;      // the share of the history in the blend: 1 - 1/5, a history of about 5 frames (the image is smooth: few frames resolve a start noise)
inline constexpr float kShaftsClampMargin = 0.02f;       // of the brightness (the image holds 0 to about 1)
inline constexpr float kShaftsMaxShift = 0.15f;          // of the image's height: the largest shift of the sun between two frames that keeps the history
// The history's weight for a frame that has `frames` frames of history (the plain mean for the first ones, then the exponential average at kShaftsHistoryWeight).
inline float ShaftsHistoryWeight(uint32_t frames) {
  return std::min(kShaftsHistoryWeight, 1.0f - 1.0f / float(std::max(frames + 1u, 1u)));
}
// Whether a move of the sun in the picture (uv units, x and y; the image's aspect `aspect` = width / height) keeps the history.
inline bool ShaftsKeepsHistory(float shift_u, float shift_v, float aspect) {
  const float du = shift_u * aspect;
  return std::sqrt(du * du + shift_v * shift_v) <= kShaftsMaxShift;
}

// ---------------------------------------------------------------------------
// the car reflection cone's history (hlsl/car_rt_refl_denoise_cs.hlsl pass 2)
// ---------------------------------------------------------------------------
// The cone's penumbra moves with the VIEW (car_rt_refl_rules.h "NOT TEMPORAL": 0.5 degrees a frame in an orbit is a fifth of a 2.5 degree cone), so a history that reprojects the texel still holds the
// last frame's direction and trails the edge. The blend is therefore gated by how still the view of the car stands: the history's weight is kConeHistoryWeight while the car's screen motion is under
// kConeStillTexels and falls to 0 at kConeMovingTexels (the motion is measured on the host from the camera's and the car's own steps), and a texel whose data kind, instance or distance differs from
// the history's takes the frame's own value. A car at rest (the pause menu, the garage orbit's pauses, a stop at a light), where a pattern that stands still is most visible, resolves fully; a car in
// motion keeps the per frame pattern and the a-trous passes alone (their residual is under 1% of the weight).
inline constexpr float kConeHistoryWeight = 0.75f;   // 1 - 1/4: a history of about 4 frames
inline constexpr float kConeStillTexels = 0.15f;     // the car's motion on the screen, in texels of the image, under which the history is whole
inline constexpr float kConeMovingTexels = 0.75f;    // ... and over which there is none
inline float ConeHistoryWeight(float motion_texels) {
  const float t = std::clamp((motion_texels - kConeStillTexels) / (kConeMovingTexels - kConeStillTexels), 0.0f, 1.0f);
  return kConeHistoryWeight * (1.0f - t * t * (3.0f - 2.0f * t));
}

}  // namespace mcla::gpu::noise_resolve

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
//   car reflection cone                      cone turn (blue tile + golden step by phase)   the a-trous passes, then a texel for texel blend with the last        car_rt_refl.cpp, car_rt_refl_denoise_cs.hlsl (pass 2)
//                                                                                          frame's result, clamped to the 3x3 neighbourhood
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
// The cone's turn changes every frame (the blue tile + the golden step by the frame's phase 0..7), the two a-trous passes take most of its grain out (the residual is under 1% of the weight) and the third
// pass averages the finished image with the last frame's, TEXEL FOR TEXEL (no reprojection). car_rt_refl_rules.h "NOT TEMPORAL" weighed a temporal accumulation and left it out because the cone's penumbra
// moves with the VIEW: a reprojected texel still holds the last frame's direction, the whole penumbra moves by the angle the view turned (0.5 degrees a frame in an orbit: a fifth of a 2.5 degree cone), and
// its rejection rule (the history's depth and instance) cannot see that. A rule that can is the clamp: the history's weight, hidden share and premultiplied colour are each clamped to the range of the finished
// image's 3 x 3 neighbourhood plus a margin, so wherever the penumbra's ramp moved the history is cut to what the neighbourhood holds NOW: the ghost is at most the ramp's change over the neighbourhood
// (about a texel of a ramp that is several texels wide, a fifth of the cone's width at its steepest), and where the ramp is a binary edge (a part within a few centimetres of the panel: a penumbra under a
// texel) the view's turn moves it by under a texel anyway. The same rule bounds a texel whose history is another surface (a car that moved on the screen). A texel with no data in either image, or whose
// weight is the clear coat's mirror, is the finished image's own. noise_resolve_test.cpp measures the grain's fall, the ghost under a moving penumbra and the cost.
inline constexpr float kConeHistoryWeight = 0.75f;   // 1 - 1/4: a history of about 4 frames
inline constexpr float kConeClampMargin = 0.05f;     // of the weight; of the premultiplied colour as a share of the neighbourhood's top (the shader's kTemporalMargin)
// The weight the history keeps when it already holds `frames` frames (the plain mean for the first ones, then the exponential average at kConeHistoryWeight).
inline float ConeHistoryWeight(uint32_t frames) { return std::min(kConeHistoryWeight, 1.0f - 1.0f / float(frames + 2u)); }

}  // namespace mcla::gpu::noise_resolve

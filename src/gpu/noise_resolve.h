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
inline constexpr float kShaftsHistoryWeight = 0.6f;      // the share of the history in the blend: a history of about 2.5 frames (the image is smooth and the noise a few thousandths: few frames resolve it, and a short one trails less)
inline constexpr float kShaftsClampMargin = 0.02f;       // of the brightness (the image holds 0 to about 1)
inline constexpr float kShaftsStaleFrom = 0.02f;         // a history that differs from the frame's value by this much (of the brightness) starts to count less (the noise is a few thousandths) ...
inline constexpr float kShaftsStaleTo = 0.05f;           // ... and by this much not at all (a silhouette that moved by parallax)
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
inline constexpr float kConeStaleFrom = 0.02f;       // a history that differs from the finished image by this much (of the weight, or of the colour's range) starts to count less (the noise is a few hundredths) ...
inline constexpr float kConeStaleTo = 0.05f;         // ... and by this much not at all (a ramp that moved)
// The share of the history's weight that is kept when it differs from the frame's own value by `stale`: 1 for a difference the noise makes, 0 for a reflection or a silhouette that moved (the shaders' smoothstep).
inline float StaleKeep(float stale, float from, float to) {
  const float t = std::clamp((stale - from) / (to - from), 0.0f, 1.0f);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}
// The weight the history keeps when it already holds `frames` frames (the plain mean for the first ones, then the exponential average at kConeHistoryWeight).
inline float ConeHistoryWeight(uint32_t frames) { return std::min(kConeHistoryWeight, 1.0f - 1.0f / float(frames + 2u)); }

// ---------------------------------------------------------------------------
// how long the histories' images live (NOISE-FOLLOW, perf/NOISE-REVIEW.md S2 / CHECK 2)
// ---------------------------------------------------------------------------
// The histories exist only on the frames where no temporal upscaler runs, but an upscaler session has plain frames too: the first frame after a switch between the play and the pause / map view, the frames
// before the upscaler's context is ready. Making and giving back the images on each of them (the contact image's two, the shafts' two, the cone's two: 97 MiB at 4K) is a texture creation hitch and a VRAM spike
// at every pause toggle. So an image whose history is not wanted ONLY because an upscaler runs stays for kHistoryKeepFrames frames of its effect's own passes in a row without one (the next plain frame finds it
// and starts from the frame as it is: the gap rule of each history says the content is stale), then goes back. A setting that turned the resolve off, a picture size that changed, and an effect that cannot run
// give the images back at once, as before. Each effect holds one HistoryLifetime and asks it once a frame, in the place that makes the images.
inline constexpr uint32_t kHistoryKeepFrames = 1200;   // 20 s at 60 fps, 5 s at 240 fps: the pause menu's toggles and the map are seconds apart, a long drive with an upscaler on is minutes
enum class HistoryImages : uint8_t {
  kMake,     // the resolve runs this frame: make the images (or keep them if they are the size the plan wants)
  kKeep,     // it does not, only because a temporal upscaler runs: leave the images as they are
  kRelease,  // it does not: give them back (nothing held makes this a no-op)
};
struct HistoryLifetime {
  uint32_t idle = 0;   // frames in a row the images were held and not wanted, only because an upscaler ran
  // `wanted`: the history's pass runs this frame. `upscaler_gap`: it does not, and the only reason is that a temporal upscaler runs (the setting is on, the effect can run). `held`: the images exist.
  // `size_ok`: the images are the size this frame's plan wants.
  HistoryImages Frame(bool wanted, bool upscaler_gap, bool held, bool size_ok) {
    if (wanted) {
      idle = 0;
      return HistoryImages::kMake;
    }
    if (!held || !upscaler_gap || !size_ok) {
      idle = 0;
      return HistoryImages::kRelease;
    }
    if (idle < kHistoryKeepFrames) {
      ++idle;
    }
    return idle >= kHistoryKeepFrames ? HistoryImages::kRelease : HistoryImages::kKeep;
  }
};

// ---------------------------------------------------------------------------
// what the histories hold (memory) at a picture size, for the logs and the owner's optimisation pass
// ---------------------------------------------------------------------------
// The contact shadows' history: two R16G16_FLOAT images (visibility, linear depth) at the contact image's size (the picture's, or half of it where the old rule says so), and, where the filter was inside the
// apply (mcla_gpu_contact_shadows_fast at the picture's size: Plan::ContactFilterInApply) and is a pass of its own now, its R16_FLOAT image (the filter's output; at the occlusion's own size it is
// the occlusion's scratch image, which exists anyway).
inline constexpr uint64_t ContactHistoryBytes(uint32_t width, uint32_t height) { return 2ull * width * height * 4ull; }
inline constexpr uint64_t ContactFilterImageBytes(uint32_t width, uint32_t height) { return 1ull * width * height * 2ull; }
// The shafts' history: two R16_FLOAT images at the blurred image's size (shafts_image.h: half the picture, at most 960 x 540).
inline constexpr uint64_t ShaftsHistoryBytes(uint32_t width, uint32_t height) { return 2ull * width * height * 2ull; }
// The cone's history: two R16G16B16A16_FLOAT images at the history image's size (car_selfrefl_rules.h HistoryEdge: the picture's, or half of it from 2560 across).
inline constexpr uint64_t ConeHistoryBytes(uint32_t width, uint32_t height) { return 2ull * width * height * 8ull; }

}  // namespace mcla::gpu::noise_resolve

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the bounce light: a steady picture at any size, and a history that is not thrown away (pure maths)
// ===========================================================================
// ao_stable.h has the occlusion's side (the blur that follows the picture's size, which the bounce light's blur and its
// wide blur use too). This file is what is the bounce light's alone (hlsl/ao_temporal_ps.hlsl and hlsl/ao_atrous_ps.hlsl
// with AO_GI, ao.cpp): the box its history is clipped to, and the number of a-trous passes. ssgi_stable_test.cpp checks
// them (and ao_stable_noise_test.cpp measures them on the C++ twin of the passes: ao_stable_twin.h); the shaders repeat
// the constants (marked "ssgi_stable.h": change them here first).
//
// WHAT WAS FOUND (the twin: a road, a wall lit by the sun and a car, a still camera, the look "default" and the bounce
// level DEFAULT, 12 and 16 frames of history; ao_stable_noise_test.cpp):
//   1. THE HISTORY WAS CLIPPED TO A BOX ONE AND A QUARTER DEVIATIONS WIDE, round the mean of this frame's 3x3
//      neighbourhood of the ALREADY BLURRED bounce. The blur makes the nine values alike (their deviation is about half
//      of one pixel's noise) and the mean of them moves with the frame's noise (by about the deviation of one frame), so
//      the box missed the true value in most frames and the clip pulled the history to the frame's value every time: the
//      accumulated bounce was as noisy as 2.5 to 3.5 times what the same frames average to without the clip (the
//      bounce's coefficient of variation 0.030 to 0.034, against 0.010), the accumulation hardly smoothed it. It is a
//      fine rule against a ghost (the light of a car that went by, gone in 1 to 2 frames) and a poor one against noise.
//      The box is now two deviations wide and never narrower than 15% of the history itself on each side: the noise
//      of no clip (0.011 to 0.013), and a light that changed in one frame (the wall going into shade) is followed to
//      within 10% of its new value in about 10 frames, half of the way in 0 to 1 (the first version: 1 to 2 frames
//      for 90%; no clip: 30 and 9).
//   2. THE A-TROUS FILTER HAD THE SAME TAPS AT EVERY SIZE: two passes, 5x5 then 9x9 pixels, 6 pixels of reach, 0.8% of a
//      720 line picture and 0.3% of a 2160 line one. The bounce is a smooth light: its noise is spread over pixels that
//      cannot be told apart by the edges the filter stops at. The passes now grow by one for each doubling of the
//      picture's height (3 from 1019 lines, 4 from 2037, 5 from 4073: 1080p and 1440p have 3, 2160p 4), the taps 1, 2, 4, 8, 16 pixels
//      apart; their reach in the pixels of a 720 line picture stays between 4.2 and 11 (6 at 720 lines, 2 at 2160 before).
//   With the occlusion's blur and noise lattice (ao_stable.h) the picture's flicker on a still camera at 3840x2160 is 0.0002
//      to 0.0014 of its brightness (0.0007 to 0.012 before: 0.17 of it on average over the four places of the test), the bounce's
//      own light 0.001 to 0.009 (0.024 to 0.071: 0.07), a frame that has no history 0.002 to 0.017 (0.007 to 0.065: 0.27). At 720
//      lines, where the blur and the passes are what they were, the box and the lattice make the picture 0.53 and the bounce 0.19
//      of it.
// Not changed on purpose: the colour chain (a deeper chain than 4x4 texels did not lower the twin's noise: the bounce's
// noise is the sampling of the directions, not the texture), the bounce's history length of 16.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ao_stable.h"

namespace mcla::gpu::ssgi_stable {

// ---------------------------------------------------------------------------
// the history's clip box (hlsl/ao_temporal_ps.hlsl, AO_GI)
// ---------------------------------------------------------------------------

// The box's half width is `width` deviations of the 3x3 neighbourhood (mcla_gpu_ssgi_test_history_clamp), never under
// kClipFloorShare of its mean, plus `history_share` of the history's own value.
inline constexpr float kClipWidth = 2.0f;              // the cvar's default (1.25 in the first version)
inline constexpr float kFirstClipWidth = 1.25f;
inline constexpr float kClipFloorShare = 0.05f;        // hlsl kBounceBoxFloor
inline constexpr float kClipHistoryShare = 0.15f;
inline constexpr float kFirstClipHistoryShare = 0.0f;  // the first version had none
// The share rides in the push block's flags, bits 26-29 in units of kClipHistoryUnit (the temporal pass reads no other bits there).
inline constexpr float kClipHistoryUnit = 0.05f;
inline constexpr uint32_t kClipHistoryShift = 26;
inline constexpr uint32_t kClipHistoryMask = 0xFu << kClipHistoryShift;

inline uint32_t ClipHistoryCode(float share) {
  return uint32_t(std::lround(std::clamp(share, 0.0f, 15.0f * kClipHistoryUnit) / kClipHistoryUnit));
}
inline float ClipHistoryOfCode(uint32_t flags) {
  return float((flags & kClipHistoryMask) >> kClipHistoryShift) * kClipHistoryUnit;
}

// The half width of the box round the neighbourhood's mean `mean` whose deviation is `deviation`, for a history value
// `history` (one channel).
inline float ClipHalfWidth(float mean, float deviation, float history, float width, float history_share) {
  return width * deviation + kClipFloorShare * mean + history_share * std::max(history, 0.0f);
}

// The history after the clip (one channel): kept where it is, brought to the box's edge where it is outside.
inline float ClipHistory(float history, float mean, float deviation, float width, float history_share) {
  const float half = ClipHalfWidth(mean, deviation, history, width, history_share);
  return std::clamp(history, std::max(mean - half, 0.0f), mean + half);
}

// ---------------------------------------------------------------------------
// the a-trous filter's passes (ao.cpp, hlsl/ao_atrous_ps.hlsl)
// ---------------------------------------------------------------------------

inline constexpr int kMaxAtrous = 5;  // taps 1, 2, 4, 8, 16 pixels apart

// The passes of the filter for a bounce image `ao_height` lines high, `base` being the setting's number at 720 lines
// (mcla_gpu_ssgi_test_atrous, 0 to 3; 0 is no filter at any size): one more for each doubling of the height.
inline int AtrousIterations(int base, uint32_t ao_height, bool trim_height_pass = false) {
  if (base <= 0) {
    return 0;
  }
  const double doublings = std::log2(std::max(double(ao_height), 1.0) / double(ao_stable::kReferenceHeight));
  const int passes = std::min(base + std::max(int(std::lround(doublings)), 0), kMaxAtrous);
  return trim_height_pass && passes > base ? passes - 1 : passes;
}

// How far `iterations` passes of the 5x5 kernel reach, in pixels: 2 * (1 + 2 + 4 + ...).
inline int AtrousReachPixels(int iterations) {
  int reach = 0;
  for (int i = 0; i < iterations; ++i) {
    reach += 2 << i;
  }
  return reach;
}

}  // namespace mcla::gpu::ssgi_stable

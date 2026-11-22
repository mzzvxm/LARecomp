#pragma once
// ===========================================================================
// MCLA GPU renderer -- ambient occlusion: a noise that stays as small at 4K as it is at 720p (pure maths, no GPU)
// ===========================================================================
// hlsl/ao_denoise_ps.hlsl and hlsl/ao_temporal_ps.hlsl blur and average the occlusion's sampling noise, ao.cpp
// (FillPush) hands them the blur's radius. The numbers and rules live HERE, with a program of their own that checks
// them (ao_stable_test.cpp: no renderer, no ReXGlue, no GPU); the shaders repeat the constants (marked "ao_stable.h"
// in their comments: change them here first) and the check compares them.
//
// WHAT THE OWNER SAW (2026-11-13, 4K, DLSS DLAA, render scale 3): "a bit of noise ... we want to make sure both the
// occlusion and the bounce light are stable". WHAT WAS FOUND, with the C++ twin of the main pass (ao_halo.h Visibility
// and ao_stable_twin.h; a still camera, the look "default", 3 slices and 3 steps, a road, a wall and a car, scale 3 =
// 3840x2160):
//   1. THE BLUR WAS 3x3 PIXELS AT EVERY SIZE. The main pass takes 18 samples a pixel whatever the picture's size, so a
//      pixel's noise is the same at 720p and at 4K (a standard deviation of 0.04 to 0.05 in the visibility after the blur,
//      5 to 8% of the picture's brightness, one frame of it), but a 3x3 blur at 4K reaches a third of what it does at 720p,
//      1/9 of the area, and averages 9 pixels of a picture that has 9 times the pixels. With the blur at 7x7 the same frame
//      has 0.010 to 0.014 (a quarter). Every frame that has no history to lean on shows that frame's noise: a car pixel
//      whose own motion is not known, a pixel that has just come into view, the first frames after a cut, a pixel on a
//      silhouette the jitter of an upscaler moves across. The blur now has the radius that keeps its reach in the picture's
//      own terms: 1 up to 1.5 times 720 lines, 2 up to 2.5 times, 3 above (7x7: 49 taps instead of 9; CHECK: the cost,
//      an estimate of 0.3 to 0.5 ms at 3840x2160 on an RTX 5080, is not measured). The 5x5 'wide' blur of the pixels that
//      have no history (the accumulation's own second blur) is one step wider than that (2 at 720p as before, 4 at 4K).
//   2. THE ACCUMULATION IS SOUND. A pixel finds itself in the last frame by the camera's motion (the jitter moves it by
//      under a pixel, which is under the plane test's width at any distance: checked by reading), the history is
//      clamped to this frame's neighbourhood and weighted by how well the surfaces agree. With the camera still the
//      12 frames of history leave a standard deviation of 0.0008 to 0.005 of the visibility after the 3x3 blur (up to 0.6% of
//      the brightness, a code and a half of the display) and 0.0003 to 0.0016 after the 7x7. The weight is the plain mean of
//      the frames so far and an exponential average (1/12) from the 12th on, and the history image's half floats (R16G16) lose
//      nothing that shows (the update of an average of 12 stalls under 0.003 of change: measured with the twin's own
//      rounding, 0.00086 against 0.00081 and 0.00519 against 0.00518).
//   3. THE NOISE'S SEQUENCE IN TIME DID NOT SUIT A HISTORY OF 12. Each pixel's noise is interleaved gradient noise in space
//      (a pattern the blur takes out well) plus a shift per frame that was the additive recurrence of the R2 sequence
//      (1/g and 1/g^2 of the plastic number: hlsl/ao_main_ps.hlsl). A one-dimensional recurrence with that step is even only at
//      3, 37, ... frames: 12 consecutive frames leave a gap of up to 0.23 of the slice rotation and 16 frames 0.21 (ao_stable_test
//      --print), so the 12 frames of the history never covered the directions evenly. The shift is now a lattice of 13 frames,
//      (m/13, 8m/13) for the frame's phase m (the Fibonacci lattice, the best two-dimensional lattice of 13 points): 13 evenly
//      spaced rotations and 13 evenly spaced step offsets, every one of 13 consecutive frames visiting each once (12 frames: no
//      gap over 2/13 = 0.15, 16 frames: 1/13). It goes through the rotations in order, so 4 or 8 frames cover an arc of them (the
//      widest gap 0.77 and 0.46, the recurrence's 0.27 and 0.25), and still wins at every length in the twin (the pixels
//      round it have other offsets of the gradient noise and fill the rotation in): over five places of the scene (3x3 blur, a
//      still camera, 3840x2160) the history of 12 leaves 0.44 to 0.83 of the recurrence's noise (0.6 on average), and the error of
//      the mean of the first 4, 8 and 12 frames after a cut is 0.5 to 1.0, 0.4 to 0.8 and 0.35 to 0.65 of its error
//      (measured with ao_stable_twin.h; ao_stable_noise_test.cpp checks the means). Tried and not better: lattices of 12, 16, 21, 24 and 32 frames, the other
//      generators of 13 ((3,4), (4,3), (2,3), (5,2), (8,3): 0.7 to 1.5 of the Fibonacci's accumulated noise, though they cover short
//      windows more evenly), white noise per frame (1.5 to 2.4 times the recurrence's), the golden ratio for the rotation (1.1 to
//      1.4 times), IGN moved by 5.588 pixels a frame (0.7 to 1.3 times). 13 shares no factor with the jitter's phase counts of the
//      upscalers (8 at 1x, 18 at 1.5x, 32 at 2x, 72 at 3x) except at 26 and 39 (a ratio of 1.8 or 2.2), and even then the noise and
//      the jitter do not move together (the shift is the occlusion's own, not the pixel's position).
//   4. THE PIXEL'S OWN NOISE WAS A LATTICE (2026-11-21, the owner's "thin diagonal lines" on the night road and the kerbs, static). Interleaved gradient noise is
//      a linear phase of the pixel's position: its spectrum is a handful of sharp frequencies, the strongest of them at a period of about 5 to 10 pixels along
//      the diagonal. A 3x3 blur takes a period of 2 or 3 pixels; this one it leaves. The shift of the lattice of 13 frames is the same for every pixel, so the
//      history of 12 frames averages every pixel to the right answer only to within the weights' unevenness (the newest frame weighs 2.6 times the oldest of
//      12), and that residue is a function of the pixel's noise: a diagonal lattice again. Measured on the C++ twin over a wall (ao_blue_noise_test.cpp):
//      the error after 4, 12 and 24 frames has 2 to 10% of its energy in ONE spatial frequency and 3 to 5 times the mean energy in its strongest direction.
//      The pixel's own part is now a BLUE noise tile (blue_noise.h: 32 x 32, void-and-cluster; mcla_gpu_ao_blue_noise): after the same blur it leaves 1/250 of
//      the gradient noise's low-frequency energy, and the error after the history has 0.2 to 0.3% of its energy in its strongest frequency and no direction
//      (1.2 to 1.5 times the mean), at the same rms (a little more: the gradient noise's 3x3 neighbourhoods are very even, which is what it was made for, but
//      only at those scales). The shift per frame is unchanged: any pixel's own sequence is still the 13 evenly spaced rotations, so 13 frames are exact for
//      either. The contact shadows' march takes the tile too: without a temporal upscaler its noise is a fixed pattern (frame 0) that only the 3x3 filter removes,
//      which is where a lattice shows most.
//   Not changed on purpose: the sample count (cost), the history length (a longer one lags a car's shadow; the numbers
//   above say the image is steady with 12), the order of the power and the blur (a blur before the power would change
//   the look).
// Left as it is and written down: the history weight counts frames of the WHOLE history, not of the pixel
// (ao_temporal_ps.hlsl): a pixel that had no history one frame ago takes the next frame's history at the full weight of
// 11/12 although it holds one frame. The wide blur is what keeps that one frame smooth; a per-pixel age needs a channel
// the history images do not have (a format change: +33 MB at 4K).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::ao_stable {

// ---------------------------------------------------------------------------
// the blur's radius
// ---------------------------------------------------------------------------

// The height of the picture the blur is tuned for: the game's own 720 lines. A picture of `k` times that many lines
// (the render scale, an upscaler's render size) has k times as many pixels along a screen distance, so the blur's
// radius in pixels grows with it.
inline constexpr float kReferenceHeight = 720.0f;
inline constexpr int kMinRadius = 1;
inline constexpr int kMaxRadius = 3;      // 7x7, 49 taps
inline constexpr int kMaxWideRadius = 4;  // 9x9, 81 taps, only for the pixels that have no history

// The radius of the blur of an occlusion image `ao_height` lines high: the ratio to 720 lines, rounded to a whole pixel,
// 1 to kMaxRadius. 540 (half of 1080p), 720: 1; 1080, 1440: 2; 2160 and over: 3. (The occlusion image is the picture's
// size, or half of it at the high quality; `ao_height` is the image's.)
inline int DenoiseRadius(uint32_t ao_height) {
  const double ratio = double(ao_height) / double(kReferenceHeight);
  return std::clamp(int(std::lround(ratio)), kMinRadius, kMaxRadius);
}

// The wide blur's radius (the pixels that have no history, ao_temporal_ps.hlsl): one more than the blur's, 2 at 720p
// (the first version's constant), at most kMaxWideRadius.
inline int WideRadius(int denoise_radius) {
  return std::min(std::max(denoise_radius, kMinRadius) + 1, kMaxWideRadius);
}

// The spatial sigma of the blur's gaussian weights, in pixels (hlsl/ao_common.hlsli AoBlurSigma): 0.9 for the 3x3
// and 1.4 for the 5x5 as before; 1.8 and 2.2 so that a 7x7 and a 9x9 are not cut off by their own edge.
inline float BlurSigma(int radius) {
  return radius >= 4 ? 2.2f : (radius == 3 ? 1.8f : (radius == 2 ? 1.4f : 0.9f));
}

// The blur's reach as a share of the picture's height, in the units of the 720 line picture's pixels: the radius at
// 720 lines is 1 pixel (3x3), and a picture of `ao_height` lines with radius r reaches r * 720 / ao_height of that.
// The rule keeps it between 0.5 and 1.4 pixels of the 720 line picture from 540 lines up (a test checks it).
inline double ReachIn720Pixels(uint32_t ao_height) {
  return double(DenoiseRadius(ao_height)) * double(kReferenceHeight) / double(std::max(ao_height, 1u));
}

// ---------------------------------------------------------------------------
// the noise (hlsl/ao_main_ps.hlsl, ao_common.hlsli AoNoiseShift): the constants the shader repeats
// ---------------------------------------------------------------------------

// The first version's shift per frame: the additive recurrence of the R2 sequence (Roberts), kept for comparing.
inline constexpr float kR2A = 0.75487766624669f;  // 1 / g, g the plastic number: the per-frame shift of the slice rotation
inline constexpr float kR2B = 0.56984029099806f;  // 1 / g^2: the per-frame shift of the step offsets
inline constexpr float kStepGolden = 0.6180339887f;
inline constexpr uint32_t kFrameWrap = 1023;      // the frame number wraps at 1024: small enough for frame * kR2A to keep its fraction in a float

// The shift now: a lattice of kLatticePeriod frames, (m / 13, (8 m mod 13) / 13) for the frame's phase m = frame mod 13: 13 distinct
// values on each axis, evenly spaced, and the pairs the Fibonacci lattice of 13 points (the best two-dimensional lattice of its size).
inline constexpr uint32_t kLatticePeriod = 13;
inline constexpr uint32_t kLatticeStepY = 8;
// How the main pass receives it: slots2.y of its push block is the phase in the low 12 bits with this flag set, or, without the flag,
// the first version's frame number (mcla_gpu_ao_stable false). (The other passes that read slots2.y as the frame number, the contact
// shadows', get the frame number as before.)
inline constexpr uint32_t kLatticeFlag = 1u << 30;
inline constexpr uint32_t kLatticePhaseMask = 0xFFFu;
// With this bit also set (slots2.y of the main pass, the contact march's too: its frame number is the low 10 bits) the pixel's own part of the noise is the
// blue noise tile's (blue_noise.h, hlsl/blue_noise.hlsli) in place of interleaved gradient noise: mcla_gpu_ao_blue_noise. The bit overlaps neither the phase
// (bits 0 to 11), the first version's frame number (bits 0 to 9) nor the lattice's flag.
inline constexpr uint32_t kBlueNoiseFlag = 1u << 29;

struct Shift {
  float x = 0.0f;  // added to the slice rotation's noise
  float y = 0.0f;  // added to the step offsets' noise
};

inline float Frac1(float x) { return x - std::floor(x); }

// What ao.cpp puts in the main pass's slots2.y for frame `frame`.
inline uint32_t NoiseCode(uint64_t frame, bool lattice, bool blue = false) {
  const uint32_t shift = lattice ? (kLatticeFlag | uint32_t(frame % kLatticePeriod)) : uint32_t(frame & kFrameWrap);
  return blue ? (shift | kBlueNoiseFlag) : shift;
}

// The shift the shader makes of the code (the shader adds it to the pixel's own noise and takes the fraction).
inline Shift DecodeShift(uint32_t code) {
  if ((code & kLatticeFlag) != 0) {
    const uint32_t m = code & kLatticePhaseMask;
    return {float(m) / float(kLatticePeriod), float((m * kLatticeStepY) % kLatticePeriod) / float(kLatticePeriod)};
  }
  const float frame = float(code & kFrameWrap);
  return {Frac1(frame * kR2A), Frac1(frame * kR2B)};
}

// The history weight a frame of age `age` (frames the history already has) gets: the plain mean of the frames so far,
// an exponential average from `cap` frames on (ao.cpp: 1 - 1 / min(age + 2, cap)).
inline float HistoryWeight(uint32_t age, uint32_t cap) {
  return 1.0f - 1.0f / float(std::max(std::min(age + 2u, cap), 1u));
}

}  // namespace mcla::gpu::ao_stable

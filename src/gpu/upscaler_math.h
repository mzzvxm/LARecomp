#pragma once
// ===========================================================================
// MCLA GPU renderer -- the temporal upscalers' pure arithmetic
// ===========================================================================
// What needs no GPU, no log and no state, in one header so that a program of its own can check it
// (upscaler_math_test.cpp, target larecomp_upscaler_math_test):
//
//   Halton       the radical inverse the jitter sequence is made of.
//   JitterPhases how many positions the jitter visits before it repeats, for an upscale ratio.
//   JitterOffset one position of that sequence, in render pixels, +y down.
//   SceneMipBias the sampler's extra level of detail bias of the scene's draws, in 1/32 levels.
//
// The vendors' recipes these follow:
//   FSR 3.1  ffxFsr3GetJitterPhaseCount: 8 x (display width / render width)^2, rounded down (the runtime
//            answers it for a context and the renderer takes its number: TemporalBackend::
//            JitterPhaseCount); ffxFsr3GetJitterOffset: Halton(index mod count + 1, 2) - 0.5 and
//            Halton(..., 3) - 0.5. Mip bias: log2(render / display) - 1.
//   DLSS     NVIDIA: at least 8 x ratio^2 phases, a Halton(2, 3) sequence; mip bias log2(render /
//            display) plus a small negative epsilon (the same -1 is used here for both).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::upscaler_math {

// Phases of the sequence at ratio 1: 8 x ratio^2 (F4-DESIGN.md 3), and the most the renderer keeps.
inline constexpr uint32_t kPhasesAtRatio1 = 8;
inline constexpr uint32_t kMaxPhases = 128;

// The extra mip bias FSR 3's guide asks for on top of log2(render / output): one more level.
inline constexpr double kRecommendedExtraBias = -1.0;
// The sampler holds the bias in 1/32 levels in an int8 (samplers.h extra_bias), and Vulkan's
// maxSamplerLodBias is at least 2 (device limits above that are the driver's): the total stays in
// +-3.9 levels.
inline constexpr double kMaxMipBias = 3.9;

// The radical inverse of `index` (from 1) in `base`: the Halton sequence, in [0, 1).
inline double Halton(uint32_t index, uint32_t base) {
  double f = 1.0;
  double r = 0.0;
  while (index > 0) {
    f /= double(base);
    r += f * double(index % base);
    index /= base;
  }
  return r;
}

// The sequence's length for an upscale `ratio` (output over render, per axis): `from_runtime` is what
// the upscaler's context says for its sizes (0: none), else 8 x ratio^2 rounded up (a ratio of exactly
// 2 gives 32 and no more); at least 1 and at most kMaxPhases.
inline uint32_t JitterPhases(double ratio, uint32_t from_runtime) {
  if (from_runtime != 0) {
    return std::min(from_runtime, kMaxPhases);
  }
  if (!(ratio > 0.0)) {
    return 1;  // no ratio (0, negative, NaN): no sequence to speak of
  }
  const double wanted = std::ceil(double(kPhasesAtRatio1) * ratio * ratio - 1e-6);
  return uint32_t(std::clamp(wanted, 1.0, double(kMaxPhases)));
}

// Phase `index` (any number: it is taken modulo `phases`) of the sequence: Halton(2, 3) from index 1,
// minus a half, so in (-0.5, 0.5) render pixels in x and y (+y down, the way the draws are moved).
inline void JitterOffset(uint32_t index, uint32_t phases, float out[2]) {
  const uint32_t phase = phases != 0 ? index % phases : 0;
  out[0] = float(Halton(phase + 1, 2) - 0.5);
  out[1] = float(Halton(phase + 1, 3) - 0.5);
}

// The scene's textures are sampled by a picture smaller than the one that is shown: the levels the
// output's size would pick are log2(render / output) lower, and `extra` more on top (the guide's -1,
// kRecommendedExtraBias). `render_scale` and `output_scale` are the sizes of the two pictures over the
// guest's (any one unit); the result is in 1/32 levels, as the sampler takes it, within +-kMaxMipBias.
// 0 when either scale is not a positive number.
inline int32_t SceneMipBias(double render_scale, double output_scale, double extra) {
  if (!(render_scale > 0.0) || !(output_scale > 0.0) || !std::isfinite(extra)) {
    return 0;
  }
  const double bias = std::log2(render_scale / output_scale) + extra;
  return int32_t(std::lround(std::clamp(bias, -kMaxMipBias, kMaxMipBias) * 32.0));
}

}  // namespace mcla::gpu::upscaler_math

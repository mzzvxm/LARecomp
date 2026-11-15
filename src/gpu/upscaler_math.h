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
//   JitterNdcOffset, ApplyZeroJitter, PostChainReadsOutput, the FSR* functions: the debug levers of the
//   static-scene flicker analysis (mcla_gpu_upscaler_zero_jitter, mcla_gpu_upscaler_post_reads_output,
//   mcla_gpu_fsr_velocity_factor / _shading_change_scale / _min_disocclusion_accumulation).
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

// What a draw's position is moved by for a frame's jitter (render pixels, +y down) on a viewport of
// `width` x `height` host pixels: NDC spans 2 of them and is +y up, so a shift down by jy pixels is
// -2 jy / height. The same offset moves every gated draw of the frame (temporal.cpp OnMainView).
inline void JitterNdcOffset(const float jitter[2], float width, float height, float out[2]) {
  out[0] = 2.0f * jitter[0] / width;
  out[1] = -(2.0f * jitter[1] / height);
}

// mcla_gpu_upscaler_zero_jitter (debug): the frame's jitter is zero. It is one value, read by the draws'
// projection offset (JitterNdcOffset), by the motion vectors (the jitter they take out) and by the
// upscaler (its jitterOffset), so zeroing it there leaves all three agreeing: no sub-pixel offset in the
// picture, none told to the upscaler.
inline void ApplyZeroJitter(bool zero, float jitter[2]) {
  if (zero) {
    jitter[0] = 0.0f;
    jitter[1] = 0.0f;
  }
}

// Whether the game's post chain before the anchor (the half-size downsample the bloom's bright pass, the
// exposure's log-luminance chain and the motion blur's half-size layer are made from) is given the
// previous frame's upscaled picture (temporal.cpp StandInForPostChain). At every ratio above 1 it is. At
// ratio 1 (the scene drawn at the output's size: Native AA, DLAA) it was not, on the argument that the
// jitter does not reach the bloom there; with mcla_gpu_upscaler_post_reads_output (`lever`) it is, the
// same as at the other ratios.
inline bool PostChainReadsOutput(uint32_t render_width, uint32_t render_height, uint32_t output_width,
                                 uint32_t output_height, bool lever) {
  const bool same_size = render_width == output_width && render_height == output_height;
  return !same_size || lever;
}

// ---- FSR's tuning constants (ffxConfigure with ffxConfigureDescUpscaleKeyValue) ------------------
// Three of FSR 3.1's constants can be overridden on a context (FidelityFX ffx_upscale.h,
// FfxApiConfigureUpscaleKey): the cvars mcla_gpu_fsr_velocity_factor, mcla_gpu_fsr_shading_change_scale
// and mcla_gpu_fsr_min_disocclusion_accumulation. A value below 0 is "not set": FSR's own default. The
// cvar is a double; the runtime reads a float through the description's `ptr`.
enum class FsrLever : uint32_t {
  kVelocityFactor = 0,
  kShadingChangeScale = 1,
  kMinDisocclusionAccumulation = 2,
};
inline constexpr uint32_t kFsrLeverCount = 3;

// The key the ffx API takes for a lever: FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR (0),
// ..._FSHADINGCHANGESCALE (2) and ..._FMINDISOCCLUSIONACCUMULATION (4). Numbers, so that this header
// needs no SDK; temporal_ffx.cpp static_asserts them against the SDK's own enum.
inline constexpr uint64_t FsrLeverKey(FsrLever lever) {
  switch (lever) {
    case FsrLever::kVelocityFactor:
      return 0;
    case FsrLever::kShadingChangeScale:
      return 2;
    case FsrLever::kMinDisocclusionAccumulation:
      return 4;
  }
  return ~uint64_t(0);
}

inline constexpr const char* FsrLeverCvar(FsrLever lever) {
  switch (lever) {
    case FsrLever::kVelocityFactor:
      return "mcla_gpu_fsr_velocity_factor";
    case FsrLever::kShadingChangeScale:
      return "mcla_gpu_fsr_shading_change_scale";
    case FsrLever::kMinDisocclusionAccumulation:
      return "mcla_gpu_fsr_min_disocclusion_accumulation";
  }
  return "";
}

// What a fresh FSR 3.1 context holds (its documented defaults: 1.0, 1.0 and -1/3).
inline constexpr float FsrLeverDefault(FsrLever lever) {
  return lever == FsrLever::kMinDisocclusionAccumulation ? -1.0f / 3.0f : 1.0f;
}

// Whether a cvar value asks for the lever: a number of 0 or more (-1, the default, and anything
// negative or not finite is "not set").
inline bool FsrLeverAsked(double cvar) {
  return std::isfinite(cvar) && cvar >= 0.0;
}

// The value passed to the runtime for a cvar value, in the range the runtime clamps it to anyway
// (velocity factor [0, 1], shading change scale [0, inf), minimum disocclusion accumulation [-1, 1]).
inline float FsrLeverValue(FsrLever lever, double cvar) {
  switch (lever) {
    case FsrLever::kVelocityFactor:
      return float(std::clamp(cvar, 0.0, 1.0));
    case FsrLever::kShadingChangeScale:
      return float(std::max(cvar, 0.0));
    case FsrLever::kMinDisocclusionAccumulation:
      return float(std::clamp(cvar, -1.0, 1.0));
  }
  return 0.0f;
}

// What one context holds of the levers: `asked[i]` is whether a value of ours was passed (false: the
// context has FSR's default), `value[i]` that value.
struct FsrLeverState {
  bool asked[kFsrLeverCount] = {};
  float value[kFsrLeverCount] = {};
};

// What to pass to a context now: bit i of `mask` is set when lever i must be configured, to `value[i]`
// (a value of ours, or FSR's default when the cvar went back to "not set" after one was passed).
struct FsrLeverPlan {
  uint32_t mask = 0;
  float value[kFsrLeverCount] = {};
};

// Compares the cvars with what the context holds, and updates `state` as if the plan were carried out
// (a runtime that refuses a key is not asked again every frame: the caller logs the refusal once). The
// first call on a context with no lever set sends nothing: its constants are FSR's defaults already.
inline FsrLeverPlan PlanFsrLevers(const double cvars[kFsrLeverCount], FsrLeverState* state) {
  FsrLeverPlan plan;
  for (uint32_t i = 0; i < kFsrLeverCount; ++i) {
    const FsrLever lever = FsrLever(i);
    if (FsrLeverAsked(cvars[i])) {
      const float value = FsrLeverValue(lever, cvars[i]);
      if (!state->asked[i] || state->value[i] != value) {
        plan.mask |= 1u << i;
        plan.value[i] = value;
        state->asked[i] = true;
        state->value[i] = value;
      }
    } else if (state->asked[i]) {
      plan.mask |= 1u << i;
      plan.value[i] = FsrLeverDefault(lever);
      state->asked[i] = false;
      state->value[i] = plan.value[i];
    }
  }
  return plan;
}

}  // namespace mcla::gpu::upscaler_math

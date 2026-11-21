#pragma once
// ===========================================================================
// MCLA GPU renderer -- the screen-space reflections' use of RtDenoise (CITY-RT package R6 of perf/CITY-RT-M2.md section 7): the host's pure rules
// ===========================================================================
// ssr.cpp asks here whether the denoiser runs in a frame and how many iterations it takes; ssr_denoise_rules / rt_denoise_test.cpp check it. The filter is rt_denoise.h (the shared host class, written once for
// this and the probe field's detail pass), its numbers and twins are rt_denoise_rules.h, its passes hlsl/rt_denoise_engine.hlsli over hlsl/ssr_denoise_consumer.hlsli's guides.
//
// WHERE IT SITS. The effect's frame (ssr.cpp OnTrigger) is: trace (with the confidence target) -> the ray traced fallback's compute passes -> the ACCUMULATION (the virtual image's reprojection, the
// variance clip, the vehicle rows: the temporal stage of this signal, which the effect already has) -> THE SPATIAL STAGE of RtDenoise (up to three iterations of the bilateral a-trous, between the
// accumulation and the resolve) -> the resolve. The accumulation's own image is the history of the next frame and is NOT filtered (a blur fed back would stack); the resolve reads the filtered image
// (the last iteration's). The filter touches the pixels the fallback made: a pixel's share is 1 - the picture's confidence (a pixel the picture's own ray spoke for is copied). The stage's temporal pass
// (RtDnTemporalMain) is not used here: the accumulation is it (and has the vehicles); the probe field's pass uses both.
//
// WHEN IT RUNS (Decide, the first reason that applies):
//   off          the debug setting mcla_gpu_rt_denoise is 0, or the effect is off (auto: its level gives the iterations: high 2, ultra 3; 1 to 3 forces that many)
//   raw frame    DLSS Ray Reconstruction takes the frame (ssr.h, the raw mode): the fallback's ONE ray a pixel goes to Ray Reconstruction as it is, with its hit distance, and this filter is skipped (filtering
//                twice is blur); the trace image is read raw by the resolve
//   no fallback  the frame did not follow the ray traced fallback (a card that does not trace, the fallback off, its pipelines still being made): nothing was filled, nothing to filter
//   multisampled the host's MSAA is above 1 (mcla_gpu_msaa_with_upscaler): the passes read the guides as plain textures; the frame keeps the plain accumulation
//   pipelines    the compute pipeline is still being made in the background (a frame never waits for a compile)
//   images       the filter's images could not be made
// With none of them in the way the stage records its iterations. With the effect off, the fallback off or the setting 0 nothing of it exists: no image, no pipeline, no pass, the resolve reads the
// accumulation as it always did (the identity).
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <string>

#include "rt_denoise_rules.h"
#include "ssr_rules.h"

namespace mcla::gpu::ssr_denoise {

namespace rd = rt_denoise;

// ---------------------------------------------------------------------------
// the push block (hlsl/ssr_denoise_consumer.hlsli: the SSR block's words this pass sets for its own)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kWordSrc = 2;         // SSR_W_SRC: the signal's slot | the trace image's slot << 16
inline constexpr uint32_t kWordConf = 6;        // SSR_W_COL01: the confidence image's slot (low 16 bits)
inline constexpr uint32_t kWordIteration = 7;   // SSR_W_COL23: the iteration (bits 0-3)
inline constexpr uint32_t kWordLength = 30;     // SSR_W_THICK: float, the history's length
constexpr rd::Patch PatchFor() { return rd::Patch{kWordSrc, kWordIteration, rd::kNoWord, rd::kNoWord, 0}; }

// The history length the accumulation stands for, in frames: its weight 0.9 is about ten, so a frame with a usable history is a mature signal (the kernel shrinks), a frame without is a fresh one.
inline constexpr float kLengthWithHistory = 12.0f;
inline constexpr float kLengthFresh = 1.0f;
constexpr float HistoryLength(bool history_used) { return history_used ? kLengthWithHistory : kLengthFresh; }

// ---------------------------------------------------------------------------
// the setting and the decision
// ---------------------------------------------------------------------------

// mcla_gpu_rt_denoise (debug-only, live): -1 auto (the effect's level gives the iterations), 0 off, 1 to 3 that many iterations (steps 1, 2, 4).
inline constexpr int kSettingAuto = -1;
inline constexpr int kSettingMax = 3;
constexpr uint32_t IterationsFor(ssr::Level level, int setting) {
  if (level == ssr::Level::kOff || setting == 0) {
    return 0;
  }
  if (setting >= 1) {
    return uint32_t(setting > kSettingMax ? kSettingMax : setting);
  }
  return level == ssr::Level::kUltra ? 3u : 2u;
}

enum class Why : uint8_t { kRuns, kOff, kRawFrame, kNoFallback, kMultisampled, kPipelines, kImages, kCount };
constexpr const char* WhyName(Why why) {
  switch (why) {
    case Why::kRuns: return "it runs";
    case Why::kOff: return "off (the setting, or the effect is off)";
    case Why::kRawFrame: return "Ray Reconstruction takes the frame: it denoises the raw ray";
    case Why::kNoFallback: return "the frame did not follow the ray traced fallback: nothing to filter";
    case Why::kMultisampled: return "the host's multisampling is above 1";
    case Why::kPipelines: return "the pipeline is still being made";
    case Why::kImages: return "the filter's images could not be made";
    case Why::kCount: break;
  }
  return "?";
}
struct Facts {
  ssr::Level level = ssr::Level::kOff;
  int setting = kSettingAuto;
  bool raw = false;               // the frame is raw (DLSS Ray Reconstruction takes it)
  bool fallback_followed = false; // the frame recorded the ray traced fallback
  bool multisampled = false;
  bool pipelines_ready = false;
  bool images_ok = false;
};
struct Plan {
  Why why = Why::kOff;
  uint32_t iterations = 0;   // the iterations to record: 0 unless why is kRuns
  constexpr bool runs() const { return why == Why::kRuns; }
};
constexpr Plan Decide(const Facts& f) {
  const uint32_t iterations = IterationsFor(f.level, f.setting);
  if (iterations == 0) return {Why::kOff, 0};
  if (f.raw) return {Why::kRawFrame, 0};
  if (!f.fallback_followed) return {Why::kNoFallback, 0};
  if (f.multisampled) return {Why::kMultisampled, 0};
  if (!f.pipelines_ready) return {Why::kPipelines, 0};
  if (!f.images_ok) return {Why::kImages, 0};
  return {Why::kRuns, iterations};
}

// ---------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------

struct Totals {
  uint64_t frames_run = 0;      // frames the stage recorded
  uint64_t iterations = 0;      // iterations recorded in them
  uint64_t frames_raw = 0;      // frames left to Ray Reconstruction (the stage skipped: it would be filtering twice)
  uint64_t frames_other = 0;    // frames it could not run for another reason (a pipeline, an image, multisampling)
  Why last_why = Why::kOff;
  uint32_t last_iterations = 0;
  bool wanted_once = false;     // the stage was wanted at least once (the line is empty when it never was)
};
inline std::string FormatTotals(const Totals& t) {
  if (!t.wanted_once) {
    return std::string();
  }
  char buf[320];
  std::snprintf(buf, sizeof(buf), "; ssr denoise: %llu frames x up to %u iterations (%llu iterations), %llu left to Ray Reconstruction, %llu could not run, now: %s", (unsigned long long)t.frames_run,
                t.last_iterations, (unsigned long long)t.iterations, (unsigned long long)t.frames_raw, (unsigned long long)t.frames_other, WhyName(t.last_why));
  return buf;
}

}  // namespace mcla::gpu::ssr_denoise

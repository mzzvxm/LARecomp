#pragma once
// ===========================================================================
// MCLA GPU renderer -- the masks' decisions and arithmetic that need no device (upscaler_masks.h)
// ===========================================================================
// What the second draws do with the main view's samples, and the arithmetic of hlsl/upscaler_masks_ps.hlsl
// as a function, so a standalone test (upscaler_masks_test.cpp) can run both.
//
// The samples. With an upscaler running the host's MSAA level is 1, and the masks have always been
// drawn at 1 sample. With mcla_gpu_msaa_with_upscaler on, the level of mcla_gpu_msaa stands, the game's
// depth has that many samples, and a second draw that tests it must draw into an image with the same
// count (a framebuffer's attachments have one count), so the coverage image has it too and the mask
// pass averages the samples. A view with samples while the lever is off is the few frames of a path or
// level change (msaa_level.h): left out, as before, and counted apart.
//
// Header only, no dependencies beyond the standard library.
// ===========================================================================

#include <algorithm>
#include <cstdint>

namespace mcla::gpu::upscaler_mask_plan {

// The largest sample count a host target takes (EdramTargets: 1, 2, 4 or 8).
constexpr uint32_t kMaxSamples = 8;

enum class Verdict : uint8_t {
  kDraw,           // the second draw is made, into a coverage image of `draw_samples` samples
  kMultisampled,   // the view has samples and the lever is off: left out (a level change's frames)
  kBadSamples,     // not a count a target has
};

// Whether a main view drawn with `draw_samples` samples gets its second draws. `kept`:
// mcla_gpu_msaa_with_upscaler.
constexpr Verdict ForView(uint32_t draw_samples, bool kept) {
  if (draw_samples == 0 || draw_samples > kMaxSamples || (draw_samples & (draw_samples - 1)) != 0) {
    return Verdict::kBadSamples;
  }
  if (draw_samples > 1 && !kept) {
    return Verdict::kMultisampled;
  }
  return Verdict::kDraw;
}

// Whether the game's depth `depth_samples` can be drawn against by a second draw into a coverage image
// with `coverage_samples` (a framebuffer takes one count for all its attachments).
constexpr bool FramebufferAgrees(uint32_t depth_samples, uint32_t coverage_samples) {
  return depth_samples != 0 && depth_samples == coverage_samples;
}

// ---- the mask pass (hlsl/upscaler_masks_ps.hlsl) ---------------------------------------------------

struct Layers {  // what a second draw left in one sample of the coverage image
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

inline float Saturate(float v) {
  return std::min(std::max(v, 0.0f), 1.0f);
}

// The mask of one sample: the larger of the alpha and the brightest colour channel, each times its gain.
inline float MaskOf(const Layers& l, float gain, float color_gain) {
  const float added = std::max(l.r, std::max(l.g, l.b));
  return Saturate(std::max(l.a * gain, added * color_gain));
}

// The mask pass over a coverage image of `n` samples (1: the single sample path, the same arithmetic):
// the mean of the samples' masks, which is the share of the pixel's samples the layer covers, what the
// game's own resolve gives the colour.
inline float MeanMask(const Layers* samples, uint32_t n, float gain, float color_gain) {
  n = std::max<uint32_t>(n, 1);
  float sum = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    sum += MaskOf(samples[i], gain, color_gain);
  }
  return sum / float(n);
}

// What the pass would give if it averaged the coverage first (not used: the gain saturates, so the
// mask of the mean is not the mean of the masks; kept for the test to show the difference).
inline float MaskOfMean(const Layers* samples, uint32_t n, float gain, float color_gain) {
  n = std::max<uint32_t>(n, 1);
  Layers mean;
  for (uint32_t i = 0; i < n; ++i) {
    mean.r += samples[i].r;
    mean.g += samples[i].g;
    mean.b += samples[i].b;
    mean.a += samples[i].a;
  }
  const float inv = 1.0f / float(n);
  mean.r *= inv;
  mean.g *= inv;
  mean.b *= inv;
  mean.a *= inv;
  return MaskOf(mean, gain, color_gain);
}

}  // namespace mcla::gpu::upscaler_mask_plan

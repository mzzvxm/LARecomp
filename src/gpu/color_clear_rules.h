#pragma once
// ===========================================================================
// MCLA GPU renderer -- what a colour clear stores in a render target of the host (the rules of EncodeColorClear, edram.h)
// ===========================================================================
// A guest clear writes a colour into a target as the target's own format holds it (IDB sub_824206A8): the colour times 2^exp_bias, and then the
// number the format can keep. The host's targets are wider than the console's, so the clear says the keeping itself:
//
//   k_2_10_10_10_FLOAT (7e3; 3, and its _AS_16_16_16_16 variant folded into it, 12)
//                     red, green and blue hold 0 to 31.875, never a NaN, never an infinity (Xenia's UnclampedFloat32To7e3: a NaN and a number
//                     below 0 are 0, a number above the range and an infinity are the largest). The host's float16 target would hold a NaN, a
//                     negative number or 65504, and what a later blend does to a NaN (every sum and product of it is one) the console never
//                     saw: its target held 0 there. Alpha is 2 bits on the console (0, 1/3, 2/3, 1); the host keeps it as it is, as the
//                     draws that blend with it expect (a NaN alpha is 0).
//   k_8_8_8_8_GAMMA   0 to 1 (kept linear on the host).
//   k_16_16, k_16_16_16_16 (SNORM on the host: +-1 is +-32)
//                     scaled by 1/32.
//   every other format
//                     the colour times 2^exp_bias, as it is.
//
// The twin of nothing in a shader: a header so that a test needs no renderer (color_clear_rules_test.cpp).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mcla::gpu::color_clear {

// The largest red, green or blue a 7e3 target holds, as stored (the colour times 2^exp_bias).
inline constexpr float kMax7e3 = 31.875f;

// Whether the format is the 7e3 colour target, folded or not (xenos ColorRenderTargetFormat 3 and 12).
inline constexpr bool Is7e3(uint32_t color_format) { return color_format == 3 || color_format == 12; }

// One component of the clear colour as the target stores it: `component` 0 to 2 are red, green and blue, 3 is alpha.
inline float EncodeComponent(uint32_t color_format, int32_t exp_bias, float value, uint32_t component) {
  float scale = std::ldexp(1.0f, exp_bias);
  if (color_format == 4 || color_format == 5) {
    scale *= 1.0f / 32.0f;  // k_16_16, k_16_16_16_16: SNORM +-1 on the host = +-32
  }
  float out = value * scale;
  if (color_format == 1) {
    out = std::clamp(out, 0.0f, 1.0f);  // k_8_8_8_8_GAMMA, kept linear
  } else if (Is7e3(color_format)) {
    if (!(out > 0.0f)) {
      return 0.0f;  // a NaN, a negative number and -0 (!(NaN > 0) is true)
    }
    if (component < 3) {
      out = std::min(out, kMax7e3);  // an infinity too
    } else if (std::isinf(out)) {
      out = std::numeric_limits<float>::max();  // alpha is not clamped to the range; keep it finite
    }
  }
  return out;
}

}  // namespace mcla::gpu::color_clear

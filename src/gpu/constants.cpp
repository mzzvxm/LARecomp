// MCLA GPU renderer -- the guest constants of a draw. See constants.h.
//
// The exponent bias handling follows Xenia (dxbc_translator_om.cpp: the
// colour output is multiplied by 2^bias after alpha to coverage;
// command_processor.cpp: fixed 16-bit targets lose 5 more on the host;
// Copyright Ben Vanik, the Xenia authors, BSD-3-Clause). The premult and
// alpha test rules are the legacy runtime's (d3d12/constant_upload.cpp,
// guest/render_state.cpp). The bank byte swap is reblue's
// (gpu/constant_buffers.cpp CopyByteSwap32Impl; Copyright Tom Clay,
// BSD-3-Clause), without its NaN flush: the bytes stay the guest's.

#include "constants.h"

#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#include <rex/hash.h>
#include <rex/logging.h>

namespace mcla::gpu {

namespace {

// Premult modes of shader_common.h (blendPremultRgb / blendPremultAlpha).
enum PremultMode : uint32_t {
  kPremultNone = 0,
  kPremultZero = 1,
  kPremultSrcColor = 2,
  kPremultOneMinusSrcColor = 3,
  kPremultSrcAlpha = 4,
  kPremultOneMinusSrcAlpha = 5,
  kPremultConstantColor = 6,
  kPremultOneMinusConstantColor = 7,
  kPremultConstantAlpha = 8,
  kPremultOneMinusConstantAlpha = 9,
};

// The Xenos applies the source factor before a MIN/MAX blend op; the host
// ignores both factors there, so the source factor is folded into the shader's
// output -- only when the destination factor is ONE (the destination term is
// not the shader's to scale) and the factor needs no destination value.
uint32_t PremultFor(xe::BlendOp op, xe::BlendFactor src, xe::BlendFactor dst) {
  if (op != xe::BlendOp::kMin && op != xe::BlendOp::kMax) {
    return kPremultNone;
  }
  if (dst != xe::BlendFactor::kOne) {
    return kPremultNone;
  }
  switch (src) {
    case xe::BlendFactor::kOne: return kPremultNone;
    case xe::BlendFactor::kZero: return kPremultZero;
    case xe::BlendFactor::kSrcColor: return kPremultSrcColor;
    case xe::BlendFactor::kOneMinusSrcColor: return kPremultOneMinusSrcColor;
    case xe::BlendFactor::kSrcAlpha: return kPremultSrcAlpha;
    case xe::BlendFactor::kOneMinusSrcAlpha: return kPremultOneMinusSrcAlpha;
    case xe::BlendFactor::kConstantColor: return kPremultConstantColor;
    case xe::BlendFactor::kOneMinusConstantColor: return kPremultOneMinusConstantColor;
    case xe::BlendFactor::kConstantAlpha: return kPremultConstantAlpha;
    case xe::BlendFactor::kOneMinusConstantAlpha: return kPremultOneMinusConstantAlpha;
    default: return kPremultNone;  // DST_*, SRC_ALPHA_SATURATE need the destination
  }
}

}  // namespace

const ConstantBanks::Banks& ConstantBanks::Apply(const DrawView& view, Check* check) {
  const DrawRecord& r = view.record();
  Banks& banks = devices_[r.device];
  for (uint32_t k = 0; k < r.vs_delta_count; ++k) {
    const uint32_t block = view.vs_block_indices()[k] % guest::kConstantBlocksPerBank;
    std::memcpy(banks.vs.data() + block * guest::kConstantBlockBytes,
                view.vs_blocks() + k * guest::kConstantBlockBytes, guest::kConstantBlockBytes);
  }
  if (r.vs_delta_count) {
    ++banks.vs_version;
  }
  for (uint32_t k = 0; k < r.ps_delta_count; ++k) {
    const uint32_t block = view.ps_block_indices()[k] % guest::kConstantBlocksPerBank;
    std::memcpy(banks.ps.data() + block * guest::kConstantBlockBytes,
                view.ps_blocks() + k * guest::kConstantBlockBytes, guest::kConstantBlockBytes);
  }
  if (r.ps_delta_count) {
    ++banks.ps_version;
  }
  if (check) {
    *check = Check{};
    if ((r.flags & kDrawBankCheck) != 0) {
      check->done = true;
      check->vs_matches = XXH3_64bits(banks.vs.data(), banks.vs.size()) == r.bank_check_vs;
      check->ps_matches = XXH3_64bits(banks.ps.data(), banks.ps.size()) == r.bank_check_ps;
    }
  }
  return banks;
}

namespace {

void SwapBankToHostScalar(const uint8_t* guest_bank, uint8_t* host_out) {
  for (uint32_t i = 0; i < guest::kConstantBankBytes; i += 4) {
    const uint32_t v = LoadBe32(guest_bank + i);
    std::memcpy(host_out + i, &v, 4);
  }
}

// Set by VerifySwapBankToHost when the vector path ever disagrees with the
// scalar one (logged): the scalar path is used from then on.
bool g_swap_scalar = false;

}  // namespace

void SwapBankToHost(const uint8_t* guest_bank, uint8_t* host_out) {
#if defined(__x86_64__) || defined(_M_X64)
  if (!g_swap_scalar) {
    // 16 bytes at a time into the upload ring (write-combined memory takes
    // whole 16-byte stores far better than 4-byte ones): the bytes of each
    // 16-bit half swapped, then the halves.
    for (uint32_t i = 0; i < guest::kConstantBankBytes; i += 16) {
      __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(guest_bank + i));
      v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
      v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
      v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(host_out + i), v);
    }
    return;
  }
#endif
  SwapBankToHostScalar(guest_bank, host_out);
}

bool VerifySwapBankToHost() {
  alignas(16) uint8_t guest[guest::kConstantBankBytes];
  alignas(16) uint8_t vector_out[guest::kConstantBankBytes];
  alignas(16) uint8_t scalar_out[guest::kConstantBankBytes];
  for (uint32_t i = 0; i < guest::kConstantBankBytes; ++i) {
    guest[i] = uint8_t(i * 151u + (i >> 7) * 13u + 7u);
  }
  SwapBankToHost(guest, vector_out);
  SwapBankToHostScalar(guest, scalar_out);
  if (std::memcmp(vector_out, scalar_out, sizeof(scalar_out)) == 0) {
    return true;
  }
  g_swap_scalar = true;
  REXLOG_ERROR("[gpu] constants: the vector bank swap differs from the scalar one; the scalar "
               "swap is used from now on (M6a.1)");
  return false;
}

AlphaTest ComputeAlphaTest(const XenosState& state) {
  AlphaTest test;
  if (!state.color_control.alpha_test_enable) {
    return test;
  }
  // The next float above `v`, so a strict ">" becomes the ">=" the shader runs.
  // One ULP above zero is a denormal, which the shader flushes to zero (and
  // then discards nothing): the smallest normal float is used instead -- the
  // foliage draws with (kGreater, 0.0) depend on it (legacy measurement).
  const auto next_above = [](float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    bits = v >= 0.0f ? bits + 1 : bits - 1;
    float out;
    std::memcpy(&out, &bits, 4);
    constexpr float kSmallestNormal = 1.17549435e-38f;
    if (out > 0.0f && out < kSmallestNormal) {
      out = kSmallestNormal;
    } else if (out < 0.0f && out > -kSmallestNormal) {
      out = -kSmallestNormal;
    }
    return out;
  };
  switch (state.color_control.alpha_func) {
    case xe::CompareFunction::kAlways:
      break;
    case xe::CompareFunction::kNever:
      test.discard = true;
      test.threshold = 3.402823466e+38f;  // nothing finite reaches it
      break;
    case xe::CompareFunction::kGreater:
      test.discard = true;
      test.threshold = next_above(state.alpha_ref);
      break;
    case xe::CompareFunction::kGreaterEqual:
      test.discard = true;
      test.threshold = state.alpha_ref;
      break;
    case xe::CompareFunction::kNotEqual:
      // A lower bound only with reference 0 -- the only form MCLA uses.
      if (state.alpha_ref == 0.0f) {
        test.discard = true;
        test.threshold = next_above(0.0f);
      } else {
        test.unsupported = true;
      }
      break;
    default:  // kLess, kEqual, kLessEqual
      test.unsupported = true;
      break;
  }
  return test;
}

uint32_t AlphaToMaskParameters(const XenosState& state, uint32_t host_samples) {
  if (!state.color_control.alpha_to_mask_enable) {
    return 0;
  }
  uint32_t samples_log2 = 0;
  if (host_samples >= 8) {
    samples_log2 = 3;  // contract v8
  } else if (host_samples >= 4) {
    samples_log2 = 2;
  } else if (host_samples >= 2) {
    samples_log2 = 1;
  }
  return (state.color_control.value >> 24) | 0x100u | (samples_log2 << 9);
}

uint32_t FillSharedConstants(const XenosState& state, const ViewportInfo& viewport,
                             uint32_t host_samples, SharedConstants& out) {
  uint32_t issues = 0;
  out.booleans = state.bool_words[0] & 0xFFFFu;  // VS bools 0-15; PS half 0
  out.swapped_texcoords = 0;                      // swapped per element on upload (24.8)
  out.half_pixel_offset[0] = out.half_pixel_offset[1] = 0.0f;
  out.alpha_threshold = ComputeAlphaTest(state).threshold;

  const ColorTargetState& rt0 = state.color[0];
  uint32_t mode_rgb =
      PremultFor(rt0.blend.color_comb_fcn, rt0.blend.color_srcblend, rt0.blend.color_destblend);
  uint32_t mode_a =
      PremultFor(rt0.blend.alpha_comb_fcn, rt0.blend.alpha_srcblend, rt0.blend.alpha_destblend);
  for (uint32_t i = 0; i < 4; ++i) {
    out.blend_premult_constant[i] = state.blend_constant[i];
  }
  // RT0's exponent bias: the hardware multiplies the colour output by 2^bias
  // after alpha to coverage; fixed 16-bit targets, stored as SNORM on the
  // host (-32..32 as -1..1), lose 5 more.
  int32_t bias = rt0.exp_bias;
  if (rt0.format == xe::ColorRenderTargetFormat::k_16_16 ||
      rt0.format == xe::ColorRenderTargetFormat::k_16_16_16_16) {
    bias -= 5;
  }
  if (rt0.bound && bias != 0) {
    if (mode_rgb != kPremultNone || mode_a != kPremultNone) {
      issues |= kIssueMinMaxWithExpBias;  // the bias wins
    }
    const uint32_t scale_bits = uint32_t(0x3F800000 + bias * (1 << 23));
    float scale;
    std::memcpy(&scale, &scale_bits, 4);
    mode_rgb = kPremultConstantColor;  // rgb * constant.rgb
    mode_a = kPremultConstantColor;    // a * constant.a
    for (float& c : out.blend_premult_constant) {
      c = scale;
    }
  }
  for (uint32_t i = 1; i < kColorTargetCount; ++i) {
    if (state.color[i].bound && state.color[i].exp_bias != 0) {
      issues |= kIssueExpBiasBeyondRt0;
    }
  }
  out.blend_premult_rgb = mode_rgb;
  out.blend_premult_a = mode_a;
  out.alpha_to_mask = AlphaToMaskParameters(state, host_samples);

  for (uint32_t i = 0; i < 3; ++i) {
    out.pos_scale[i] = viewport.ndc_scale[i];
    out.pos_offset[i] = viewport.ndc_offset[i];
  }
  out.pos_scale[3] = 1.0f;
  out.pos_offset[3] = 0.0f;
  return issues;
}

}  // namespace mcla::gpu

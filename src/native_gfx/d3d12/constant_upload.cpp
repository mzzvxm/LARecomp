#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — constant upload.
// See constant_upload.h.

#include "constant_upload.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>

#include "../guest/guest_constants.h"
#include "../guest/render_state.h"
#include "context.h"

REXCVAR_DECLARE(bool, mcla_native_gfx_color_exp_bias);
REXCVAR_DECLARE(bool, mcla_native_gfx_exp_bias_unit);

namespace mcla::native_gfx {
namespace {

// gInvColorExpBias always lands in constant register c27, in both banks: it is
// declared at packoffset(c27) by all 165 shaders that use it, and no shader
// declares anything else there, so touching it positionally is safe. Only .x is
// ever read (194 uses, all of them .x or a swizzle of it), so only .x is
// scaled.
constexpr size_t kInvColorExpBiasByteOffset = 27 * 16;  // c27.x

}  // namespace

void ApplyColorExpBias(void* bank, const uint8_t* base, uint32_t dev) {
  if (!bank || !base || !REXCVAR_GET(mcla_native_gfx_color_exp_bias)) {
    return;
  }
  static_assert(kInvColorExpBiasByteOffset + sizeof(float) <= kAluBankBytes,
                "c27 must fit inside a constant bank");

  const int32_t bias = ReadColorExpBias(base, dev);
  if (bias == 0) {
    return;  // nothing was divided out, so there is nothing to fold back
  }

  // Fold the scale the output merger would have applied straight into the
  // reciprocal the game uploaded, rather than assuming what that reciprocal is.
  // With the target's own bias the two cancel: a bias of 4 against an uploaded
  // 2^-4 leaves exactly 1.0, and a target the game gave no bias is untouched
  // above. Reading the register also keeps this correct when the bias changes
  // between targets within a frame, which a fixed 1.0 could not.
  float inv = 0.0f;
  std::memcpy(&inv, static_cast<const uint8_t*>(bank) + kInvColorExpBiasByteOffset, sizeof(inv));
  // O cancelamento pretendido leva a 1.0: o jogo sobe 2^-bias e o bias do alvo
  // desfaz. Medido na MCLA, isso SO fecha no alvo de cena (bias 4, upload 1/16
  // -> 1.0). No passe de reflexo da agua o alvo tem bias 2 e o jogo NAO reenvia
  // a constante, entao o produto da 0.25 -- um fator 4 de diferenca entre dois
  // passes do mesmo frame. Com o cvar, neutraliza para 1.0 sempre, que e o que
  // um alvo float host precisa (nada foi dividido, nada tem de ser desfeito).
  const float scaled = REXCVAR_GET(mcla_native_gfx_exp_bias_unit)
                           ? 1.0f
                           : inv * std::ldexp(1.0f, bias);
  if (!std::isfinite(scaled)) {
    return;
  }
  std::memcpy(static_cast<uint8_t*>(bank) + kInvColorExpBiasByteOffset, &scaled, sizeof(scaled));
}

// Xenos blend factor ids, as they appear in RB_BLENDCONTROL. Named here rather
// than shared with pipeline_cache.cpp's D3D12 tables because those map to host
// enums and this maps to the shader's own switch.
namespace xenos_factor {
constexpr uint32_t kZero = 0;
constexpr uint32_t kOne = 1;
constexpr uint32_t kSrcColor = 4;
constexpr uint32_t kOneMinusSrcColor = 5;
constexpr uint32_t kSrcAlpha = 6;
constexpr uint32_t kOneMinusSrcAlpha = 7;
constexpr uint32_t kConstantColor = 12;
constexpr uint32_t kOneMinusConstantColor = 13;
constexpr uint32_t kConstantAlpha = 14;
constexpr uint32_t kOneMinusConstantAlpha = 15;
}  // namespace xenos_factor

constexpr uint32_t kXenosBlendOpMin = 2;
constexpr uint32_t kXenosBlendOpMax = 3;

BlendPremultMode BlendPremultFor(uint32_t blend_op, uint32_t src_factor,
                                 uint32_t dest_factor) {
  if (blend_op != kXenosBlendOpMin && blend_op != kXenosBlendOpMax) {
    return BlendPremultMode::kNone;  // ADD/SUB apply the factors on their own
  }
  if (dest_factor != xenos_factor::kOne) {
    // The destination term is not the shader's to scale. Emulating this half
    // wrong would be worse than leaving it: bail instead.
    return BlendPremultMode::kNone;
  }
  switch (src_factor) {
    case xenos_factor::kOne:                  return BlendPremultMode::kNone;
    case xenos_factor::kZero:                 return BlendPremultMode::kZero;
    case xenos_factor::kSrcColor:             return BlendPremultMode::kSrcColor;
    case xenos_factor::kOneMinusSrcColor:     return BlendPremultMode::kOneMinusSrcColor;
    case xenos_factor::kSrcAlpha:             return BlendPremultMode::kSrcAlpha;
    case xenos_factor::kOneMinusSrcAlpha:     return BlendPremultMode::kOneMinusSrcAlpha;
    case xenos_factor::kConstantColor:        return BlendPremultMode::kConstantColor;
    case xenos_factor::kOneMinusConstantColor:return BlendPremultMode::kOneMinusConstantColor;
    case xenos_factor::kConstantAlpha:        return BlendPremultMode::kConstantAlpha;
    case xenos_factor::kOneMinusConstantAlpha:return BlendPremultMode::kOneMinusConstantAlpha;
    default:
      // DST_*, SRC_ALPHA_SATURATE: need the destination, which the shader has
      // no access to.
      return BlendPremultMode::kNone;
  }
}

bool UploadConstants(D3D12Context& context, const void* vs_bank, const void* ps_bank,
                     const SharedConstantValues& shared, ConstantBindings& out) {
  out = ConstantBindings{};
  if (!vs_bank) {
    return false;
  }

  // Constant buffer views require 256-byte alignment.
  constexpr uint64_t kCbvAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

  D3D12Context::UploadAlloc alloc;
  if (!context.AllocateUpload(kAluBankBytes, kCbvAlignment, alloc)) {
    return false;
  }
  std::memcpy(alloc.cpu, vs_bank, kAluBankBytes);
  out.vs = alloc.gpu;

  if (ps_bank) {
    if (!context.AllocateUpload(kAluBankBytes, kCbvAlignment, alloc)) {
      return false;
    }
    std::memcpy(alloc.cpu, ps_bank, kAluBankBytes);
    out.ps = alloc.gpu;
  }

  if (!context.AllocateUpload(kSharedConstantsBytes, kCbvAlignment, alloc)) {
    return false;
  }
  auto* bytes = static_cast<uint8_t*>(alloc.cpu);
  std::memset(bytes, 0, kSharedConstantsBytes);
  std::memcpy(bytes + kSharedBooleansByteOffset, &shared.booleans, 4);
  std::memcpy(bytes + kSharedSwappedTexcoordsByteOffset, &shared.swapped_texcoords, 4);
  std::memcpy(bytes + kSharedHalfPixelOffsetByteOffset, shared.half_pixel_offset, 8);
  std::memcpy(bytes + kSharedAlphaThresholdByteOffset, &shared.alpha_threshold, 4);
  std::memcpy(bytes + kSharedBlendPremultRgbByteOffset, &shared.blend_premult_rgb, 4);
  std::memcpy(bytes + kSharedBlendPremultAByteOffset, &shared.blend_premult_a, 4);
  std::memcpy(bytes + kSharedBlendPremultConstByteOffset, shared.blend_premult_constant, 16);
  out.shared = alloc.gpu;
  return true;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

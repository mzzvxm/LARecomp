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
  out.shared = alloc.gpu;
  return true;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

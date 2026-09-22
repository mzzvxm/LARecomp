#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — constant upload.
// See constant_upload.h.

#include "constant_upload.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

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

// The shaders the fold is NOT exact for, by runtime identity (FNV-1a over the
// vfetch-normalized ucode; checked against the pack: this key's two DXIL blobs
// are the ones RenderDoc shows bound on the neon draws).
constexpr uint64_t kPsCalcShadowsLight = 0xEC62E63D9A9C539Aull;
constexpr size_t kCalcShadowsLightColorByteOffset = 64 * 16;  // lightColor, c64
constexpr uint64_t kPsGenVelocityNoVehicleBlur = 0x120BB962920EB327ull;
constexpr uint64_t kVsVehBlurImposterMotionBlur = 0x0C93FA31BBB56F49ull;

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
  // The product is not always 1.0, and it is not supposed to be. The road
  // reflection pass sets bias 2 on its target (sub_823143C0, a literal 2) and
  // uploads no constant, so the 2^-4 the pass loop sent a moment earlier is
  // still there: 2^-4 * 2^2 = 0.25 on the hardware too. The fold reproduces
  // that. mcla_native_gfx_exp_bias_unit forces 1.0 instead, which is NOT what
  // the hardware does.
  const float scaled = REXCVAR_GET(mcla_native_gfx_exp_bias_unit)
                           ? 1.0f
                           : inv * std::ldexp(1.0f, bias);
  if (!std::isfinite(scaled)) {
    return;
  }
  std::memcpy(static_cast<uint8_t*>(bank) + kInvColorExpBiasByteOffset, &scaled, sizeof(scaled));
}

void CorrectColorExpBiasFold(uint64_t vs_id, uint64_t ps_id, void* ps_bank, const uint8_t* base,
                             uint32_t dev) {
  // Identity first: this runs on every draw and only three shaders care, so the
  // guest register read is left for them.
  if (ps_id != kPsCalcShadowsLight && ps_id != kPsGenVelocityNoVehicleBlur &&
      vs_id != kVsVehBlurImposterMotionBlur) {
    return;
  }
  if (!ps_bank || !base || !REXCVAR_GET(mcla_native_gfx_color_exp_bias)) {
    return;
  }
  const int32_t bias = ReadColorExpBias(base, dev);
  if (bias == 0) {
    return;  // the fold left the banks alone, so it cannot have been wrong
  }

  if (ps_id == kPsCalcShadowsLight) {
    // The output merger multiplies the whole export by 2^bias, the light term
    // included, and the fold only reaches the constant half. Scale the other
    // half the same way. Independent of the constant's value, so it is exact
    // on a pass whose constant is stale as well.
    const float scale = std::ldexp(1.0f, bias);
    float color[3];
    uint8_t* at = static_cast<uint8_t*>(ps_bank) + kCalcShadowsLightColorByteOffset;
    std::memcpy(color, at, sizeof(color));
    for (float& c : color) {
      c *= scale;
    }
    std::memcpy(at, color, sizeof(color));  // .w is never read by the shader
    return;
  }

  // The other two feed rcp(gInvColorExpBias) into further arithmetic, so the
  // exact treatment depends on how their output is blended -- and neither has
  // been seen on a biased target in any capture (GenVelocity* is not drawn in
  // gameplay at all). Say so once rather than guess a correction.
  static bool warned_velocity = false;
  static bool warned_imposter = false;
  bool& warned = ps_id == kPsGenVelocityNoVehicleBlur ? warned_velocity : warned_imposter;
  if (!warned) {
    warned = true;
    REXLOG_WARN(
        "[native_gfx] colour exp bias fold is inexact for vs={:016X} ps={:016X} on a target "
        "with bias {} and is left uncorrected -- capture this frame",
        vs_id, ps_id, bias);
  }
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

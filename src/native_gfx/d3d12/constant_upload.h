#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — constant upload
// ===========================================================================
// Turns the guest ALU banks into D3D12 root CBVs matching what the
// translated shaders declare:
//
//   b0 space4  VertexShaderConstants  <- guest VS bank (dev+1920), 4096 B
//   b1 space4  PixelShaderConstants   <- guest PS bank (dev+6016), 4096 B
//   b2 space4  SharedConstants        <- descriptor tables + scalars, 544 B
//
// The banks go through the context's per-frame upload ring, so a constant
// buffer lives exactly as long as the frame that uses it — which is what a
// per-draw snapshot needs, since consecutive draws routinely change them.
//
// First correct path: the FULL bank is uploaded per draw. The guest dirty
// masks are captured (they describe 4 registers per bit) and reported, but
// not yet used to narrow the copy: they are cleared by the guest's own draw
// path, so acting on them requires sampling before the original call and
// tracking residency across frames. Correctness first, granularity later.
// ===========================================================================

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

namespace mcla::native_gfx {

class D3D12Context;

// GPU addresses of the three constant buffers a draw binds.
struct ConstantBindings {
  D3D12_GPU_VIRTUAL_ADDRESS vs = 0;
  D3D12_GPU_VIRTUAL_ADDRESS ps = 0;
  D3D12_GPU_VIRTUAL_ADDRESS shared = 0;
  bool valid() const { return vs != 0 && shared != 0; }
};

// Root parameter indices, matching the register/space assignment above.
enum ConstantRootParameter : uint32_t {
  kRootParamVsConstants = 0,
  kRootParamPsConstants = 1,
  kRootParamSharedConstants = 2,
};

// Scalars the shared buffer carries (the descriptor-index tables at c0..c31
// are filled by the texture/sampler binding, which lands next).
struct SharedConstantValues {
  uint32_t booleans = 0;
  uint32_t swapped_texcoords = 0;
  float half_pixel_offset[2] = {0.0f, 0.0f};
  float alpha_threshold = 0.0f;
  // MIN/MAX blend factor premultiplication; 0 = leave the output alone, which
  // is what every draw that does not hit the case uses. See BlendPremultMode.
  uint32_t blend_premult_rgb = 0;
  uint32_t blend_premult_a = 0;
  float blend_premult_constant[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Which source blend factor the pixel shader has to fold into its own output,
// per equation. Must stay in step with blendPremultRgb/blendPremultAlpha in
// XenosRecomp's shader_common.h -- the shader switches on these numbers.
enum class BlendPremultMode : uint32_t {
  kNone = 0,  // factor is ONE, or the case does not apply
  kZero = 1,
  kSrcColor = 2,
  kOneMinusSrcColor = 3,
  kSrcAlpha = 4,
  kOneMinusSrcAlpha = 5,
  kConstantColor = 6,
  kOneMinusConstantColor = 7,
  kConstantAlpha = 8,
  kOneMinusConstantAlpha = 9,
};

// The mode for one equation, or kNone when this draw does not need it.
//
// Only MIN and MAX need it at all, and only with a destination factor of ONE:
// the destination term is not the shader's to scale, so any other destination
// factor is left unemulated rather than emulated wrong. Source factors that
// depend on the destination (DST_*, SRC_ALPHA_SATURATE) cannot be folded in
// either, and also return kNone.
BlendPremultMode BlendPremultFor(uint32_t blend_op, uint32_t src_factor,
                                 uint32_t dest_factor);

// Copies both banks (already host-order) and the shared values into the
// frame's upload ring and returns their GPU addresses. `vs_bank` and
// `ps_bank` are kAluBankBytes each; `ps_bank` may be null for depth-only
// draws, in which case b1 is left unbound.
bool UploadConstants(D3D12Context& context, const void* vs_bank, const void* ps_bank,
                     const SharedConstantValues& shared, ConstantBindings& out);

// Folds the bound render target's colour exponent bias back into the constant
// bank's gInvColorExpBias, in place. Call it on each bank right after reading it
// from the guest, before the bank is uploaded or mirrored, so every consumer
// sees the same bytes.
//
// Xenos biases a render target's colour exponent for precision in EDRAM's
// fixed-point formats: the output merger scales by 2^bias on write, the resolve
// undoes it, and the game uploads 2^-bias so its shaders can pre-divide. A D3D12
// float target does neither, so the shader's division survives with nothing to
// undo it -- measured as gInvColorExpBias.x = 0.0625 against a bias of 4,
// leaving the car body at six percent alpha.
//
// The bias comes from RB_COLOR_INFO in the device's own register shadow, so
// this needs no command processor, no PM4 parsing and no EDRAM emulation, and
// it stays right when the bias differs between targets in a frame. A target
// with no bias is left untouched. Gated by
// mcla_native_gfx_color_exp_bias.
void ApplyColorExpBias(void* bank, const uint8_t* base, uint32_t dev);

}  // namespace mcla::native_gfx

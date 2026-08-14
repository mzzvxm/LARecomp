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
};

// Copies both banks (already host-order) and the shared values into the
// frame's upload ring and returns their GPU addresses. `vs_bank` and
// `ps_bank` are kAluBankBytes each; `ps_bank` may be null for depth-only
// draws, in which case b1 is left unbound.
bool UploadConstants(D3D12Context& context, const void* vs_bank, const void* ps_bank,
                     const SharedConstantValues& shared, ConstantBindings& out);

}  // namespace mcla::native_gfx

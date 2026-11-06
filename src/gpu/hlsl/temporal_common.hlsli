// MCLA GPU renderer -- what the temporal system's passes share (temporal_mv.h).
//
// The stencil. The anchor draw's fetch 1 is the stencil view of the depth resolve: a depth resolve
// into k_8_8_8_8 writes the raw dword depth24 << 8 | stencil, and the host product of it
// (resolve_ps.hlsl kModeDepthRaw8888) is RGBA8 holding that dword's four bytes in memory order:
// R the stencil, G the low byte of the depth24, B its middle byte, A its high byte. The slot the
// anchor binds is not that product but a VIEW of it, with the fetch constant's swizzle over the
// format's (textures.cpp GuestToHostSwizzle), and this game's fetch swizzle for the view is
// ZYXW: the guest shader's .z is the product's R, and the guest's .x is the product's B, the
// middle byte of the depth. The first version of these passes read `.x` of the view and got that
// byte: a value that follows the depth in steps (stripes on every flat surface, black only where
// the depth is 0, the sky), not the vehicle ID. The stencil is where the swizzle puts the
// product's red, found on the host from the fetch constant (TemporalMotion::StencilChannel).
//
// Both backends are alike here: the product is a plain RGBA8 texture and the view's component
// mapping is applied by the sampler hardware (D3D12 shader component mapping, Vulkan swizzle).
// (The EDRAM's own stencil plane, whose first channel differs between D3D12's G and Vulkan's R,
// is read only by the resolves, host_pass_common.hlsli StencilOf; it never reaches these passes.)
//
// The passes get the view as one word, `stencil_ref`, made by TemporalMotion::StencilRef: the
// bindless slot in bits 0 to 27 (the heap has 32768) and, in bits 28 to 30, the component that
// holds the stencil byte, 4 when the swizzle has no red at all (then there is no ID: 0).

#ifndef MCLA_GPU_TEMPORAL_COMMON_HLSLI
#define MCLA_GPU_TEMPORAL_COMMON_HLSLI

#include "host_pass_common.hlsli"

// The stencil byte at `at` (0 to 255), 0 when the view has no stencil channel.
uint StencilByte(uint stencil_ref, int3 at) {
  const uint channel = (stencil_ref >> 28u) & 7u;
  if (channel > 3u) {
    return 0u;
  }
  const float4 texel = g_textures[stencil_ref & 0x0FFFFFFFu].Load(at);
  return uint(texel[channel] * 255.0f + 0.5f);
}

// The vehicle ID the game's draws write (ref = 4 ID + {0, 1}): stencil >> 2, and 0 for the
// world, which is also what 24 and above are (the UI's draws, not vehicles).
uint VehicleId(uint stencil_ref, int3 at) {
  const uint id = StencilByte(stencil_ref, at) >> 2u;
  return id >= 24u ? 0u : id;
}

#endif  // MCLA_GPU_TEMPORAL_COMMON_HLSLI

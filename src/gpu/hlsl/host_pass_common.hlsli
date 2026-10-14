// MCLA GPU renderer -- the full-screen host passes (resolve conversions,
// present), shared declarations.
//
// Binding model (host_passes.h), the same on both backends: the bindless
// texture set bound four times, so each texture type has a space of its own
// (the guest shaders' layout does the same for 2D/3D/cube, layout.h):
//   set 0  t0, space0  Texture2D<float4>[]     colour, depth plane, textures
//   set 1  t0, space1  Texture2DMS<float4>[]   multisampled colour or depth
//   set 2  t0, space2  Texture2D<uint4>[]      stencil plane
//   set 3  t0, space3  Texture2DMS<uint4>[]    multisampled stencil plane
//   push   b0, space4  32 bytes, the pass's own constants
// On D3D12 Plume turns the push range into root constants at (b0, space4); on
// Vulkan it is the one push-constant block, picked with __spirv__.

#ifndef MCLA_GPU_HOST_PASS_COMMON_HLSLI
#define MCLA_GPU_HOST_PASS_COMMON_HLSLI

Texture2D<float4> g_textures[] : register(t0, space0);
Texture2DMS<float4> g_textures_ms[] : register(t0, space1);
Texture2D<uint4> g_utextures[] : register(t0, space2);
Texture2DMS<uint4> g_utextures_ms[] : register(t0, space3);

// The stencil value of a stencil-plane view: D3D12's X32_TYPELESS_G8X24_UINT /
// X24_TYPELESS_G8_UINT put it in G, a Vulkan STENCIL-aspect view in R.
uint StencilOf(uint4 texel) {
#ifdef __spirv__
  return texel.r & 0xFFu;
#else
  return texel.g & 0xFFu;
#endif
}

#endif  // MCLA_GPU_HOST_PASS_COMMON_HLSLI

// MCLA GPU renderer -- overlay shaders, shared declarations.
//
// Binding model, the same on both
// backends:
//   set 0  t0, space0  bindless textures (descriptors.h)
//   set 1  s0, space1  bindless samplers
//   push   b0, space2  OverlayPush, one VERTEX|PIXEL range of 24 bytes
// On D3D12 Plume turns the push range into root constants at (b0, space2); on
// Vulkan it is the one push-constant block, picked here with __spirv__ (dxc
// defines it for -spirv).

#ifndef MCLA_GPU_OVERLAY_COMMON_HLSLI
#define MCLA_GPU_OVERLAY_COMMON_HLSLI

struct OverlayPush {
  // Coordinate space -> NDC: ndc = position * scale + translate.
  float2 scale;
  float2 translate;
  // Bindless texture slot, or kOverlayNoTexture for vertex color only.
  uint texture;
  uint sampler_index;
};

static const uint kOverlayNoTexture = 0xFFFFFFFFu;

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<OverlayPush> g_push;
#else
ConstantBuffer<OverlayPush> g_push : register(b0, space2);
#endif

struct OverlayVaryings {
  float4 position : SV_Position;
  [[vk::location(0)]] float2 texcoord : TEXCOORD0;
  [[vk::location(1)]] float4 color : COLOR0;
};

#endif  // MCLA_GPU_OVERLAY_COMMON_HLSLI

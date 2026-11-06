// MCLA GPU renderer -- the temporal system's copies of what the anchor reads (temporal_mv.h).
//
// The scene's depth, colour and the vehicle IDs live in resolve products the guest overwrites later in
// the frame and in the next one; the temporal system keeps its own images of them. Texel for texel
// from the bindless slot the anchor draw binds, into an image of the same size:
//   TEMPORAL_COPY 0  depth    the first channel (R32_FLOAT, 1 near / 0 far, as stored)
//   TEMPORAL_COPY 1  colour   all four channels (R16G16B16A16_FLOAT, linear, scene-referred)
//   TEMPORAL_COPY 2  IDs      stencil >> 2 as a byte (R8_UNORM); the debug view shows it. `source` is
//                             then a stencil reference (temporal_common.hlsli), not a plain slot.

#include "temporal_common.hlsli"

struct CopyPush {
  uint source;  // the bindless slot (a gamma bit 31 is dropped); for the IDs, the stencil reference
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CopyPush> g_push;
#else
ConstantBuffer<CopyPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const int3 at = int3(int2(position.xy), 0);
#if TEMPORAL_COPY == 2
  // The stencil byte itself, not the view's .x (temporal_common.hlsli says why that was wrong).
  return float4(float(StencilByte(g_push.source, at) >> 2u) / 255.0f, 0.0f, 0.0f, 0.0f);
#else
  const float4 texel = g_textures[g_push.source & 0x7FFFFFFFu].Load(at);
#if TEMPORAL_COPY == 0
  return float4(texel.x, 0.0f, 0.0f, 0.0f);
#else
  return texel;
#endif
#endif
}

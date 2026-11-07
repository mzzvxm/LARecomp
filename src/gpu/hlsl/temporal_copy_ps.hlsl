// MCLA GPU renderer -- the temporal system's copy of the depth the anchor reads (temporal_mv.h).
//
// The scene's depth lives in a resolve product the guest overwrites later in the frame and in the
// next one; the temporal system keeps its own image of it. Texel for texel from the bindless slot the
// anchor draw binds, into an image of the same size: the first channel (R32_FLOAT, 1 near / 0 far, as
// stored).

#include "host_pass_common.hlsli"

struct CopyPush {
  uint source;  // the bindless slot (a gamma bit 31 is dropped)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CopyPush> g_push;
#else
ConstantBuffer<CopyPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const int3 at = int3(int2(position.xy), 0);
  const float4 texel = g_textures[g_push.source & 0x7FFFFFFFu].Load(at);
  return float4(texel.x, 0.0f, 0.0f, 0.0f);
}

// MCLA GPU renderer -- the copy the scene's anti-aliasing reads (post.h).
//
// The composed scene, texel for texel from its EDRAM target into the pass's own
// image, with the perceptual luma in alpha for FXAA (Xenia's
// apply_gamma_table.xesli with XE_APPLY_GAMMA_FXAA_LUMA: dot(rgb, (0.299, 0.587,
// 0.114)); Copyright 2022 Ben Vanik, BSD-3-Clause). SMAA reads the colour and
// leaves the alpha alone. The target is an UNORM 8:8:8:8 colour target, a
// game-encoded image: nothing is linearised.

#include "host_pass_common.hlsli"

struct LumaPush {
  uint source;  // the EDRAM target's colour plane, a bindless 2D slot
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<LumaPush> g_push;
#else
ConstantBuffer<LumaPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  // The pass covers the image from the target's origin: the same pixel.
  const float3 rgb = g_textures[g_push.source].Load(int3(int2(position.xy), 0)).rgb;
  return float4(rgb, dot(rgb, float3(0.299f, 0.587f, 0.114f)));
}

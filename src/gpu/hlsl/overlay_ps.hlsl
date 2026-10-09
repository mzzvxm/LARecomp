// MCLA GPU renderer -- overlay pixel shader.
//
// Vertex color times the texture, sampled at mip 0 (overlay textures have no
// mips), or the vertex color alone for draws without a texture -- what the
// SDK's drawer gets from an SRV swizzled to 1111, without a filler texture.

#include "overlay_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
SamplerState g_samplers[] : register(s0, space1);

float4 main(OverlayVaryings input) : SV_Target {
  float4 color = input.color;
  if (g_push.texture != kOverlayNoTexture) {
    color *= g_textures[g_push.texture].SampleLevel(g_samplers[g_push.sampler_index],
                                                    input.texcoord, 0.0f);
  }
  return color;
}

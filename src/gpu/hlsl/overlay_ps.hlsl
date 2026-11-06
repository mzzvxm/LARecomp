// MCLA GPU renderer -- overlay pixel shader.
//
// Vertex color times the texture, sampled at mip 0 (overlay textures have no
// mips), or the vertex color alone for draws without a texture -- what the
// SDK's drawer gets from an SRV swizzled to 1111, without a filler texture.
//
// OVERLAY_HDR (overlay_hdr_ps): the back buffer is an HDR10 swap chain. The colour is still the
// SDR code values the SDK drew; its last step is hdr_output.hlsli's HdrFromSdr, which puts the UI
// at paper white. The blend then mixes HDR10 code values, as SDR mixes its encoded ones.

#include "overlay_common.hlsli"
#if OVERLAY_HDR
#include "hdr_output.hlsli"
#endif

Texture2D<float4> g_textures[] : register(t0, space0);
SamplerState g_samplers[] : register(s0, space1);

float4 main(OverlayVaryings input) : SV_Target {
  float4 color = input.color;
  if (g_push.texture != kOverlayNoTexture) {
    color *= g_textures[g_push.texture].SampleLevel(g_samplers[g_push.sampler_index],
                                                    input.texcoord, 0.0f);
  }
#if OVERLAY_HDR
  HdrLevels levels;
  levels.white = g_push.hdr_white;
  levels.peak = g_push.hdr_peak;
  levels.knee = g_push.hdr_knee;
  levels.flags = g_push.hdr_flags;
  color.rgb = HdrFromSdr(color.rgb, levels);
#endif
  return color;
}

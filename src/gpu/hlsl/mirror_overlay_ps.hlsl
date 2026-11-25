// MCLA GPU renderer -- the side mirrors' glass overlay, the pixel stage (mirror_overlay.h, perf/MIRROR-VIEW.md package MV4).
//
// For each pixel of the redrawn glass: the eye's ray to the point is reflected about the glass's normal and the shared rear view is read where the reflected ray goes (mirror_overlay.hlsli
// MirrorOverlayUv: the planar mirror lookup, exact for what is the assumed distance beyond the glass). The colour is the picture's, in the main colour's units (the view has the main colour's format
// and exponent bias: a straight copy, as the HUD stamp); the alpha is the edge fade, so the pipeline (blend SRC_ALPHA / INV_SRC_ALPHA on the colour channels, alpha untouched) leaves the game's own
// glass where the lookup runs out of picture.

#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"
#include "nan_guard.hlsli"
#include "mirror_overlay.hlsli"

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<MirrorOverlayPush> g_push;
#else
ConstantBuffer<MirrorOverlayPush> g_push : register(b0, space4);
#endif

struct PsInput {
  float4 position : SV_Position;
  float3 view_pos : TEXCOORD0;
};

float4 main(PsInput input) : SV_Target {
  const MirrorOverlayPush p = g_push;
  const float3 uv = MirrorOverlayUv(p, input.view_pos);
  float alpha = 0.0;
  if (uv.z > 0.0) {
    alpha = MirrorOverlayEdgeFade(uv.xy);
  }
  const float4 colour = g_textures[asuint(p.delta.w)].SampleLevel(g_samplers[kSamplerLinearClamp], uv.xy, 0.0);
  return float4(NgFinite3(colour.rgb), alpha);
}

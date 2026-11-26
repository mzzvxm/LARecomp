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
struct DrawAddress { uint64_t address; };
[[vk::push_constant]] ConstantBuffer<DrawAddress> g_address;
#define g_draw vk::RawBufferLoad<MirrorOverlayDraw>(g_address.address, 16)
#else
ConstantBuffer<MirrorOverlayDraw> g_draw : register(b0, space4);
#endif

struct PsInput {
  float4 position : SV_Position;
  float3 view_pos : TEXCOORD0;
  float2 mask_uv : TEXCOORD1;
};

float4 main(PsInput input) : SV_Target {
  const MirrorOverlayDraw draw = g_draw;
  const MirrorOverlayPush p = draw.mirror;
  const float3 uv = MirrorOverlayUv(p, input.view_pos);
  float alpha = 0.0;
  if (uv.z > 0.0) {
    alpha = MirrorOverlayEdgeFade(uv.xy);
  }
  const float4 colour = g_textures[asuint(p.delta.w)].SampleLevel(g_samplers[kSamplerLinearClamp], uv.xy, 0.0);
  float3 rgb = NgFinite3(colour.rgb);
  if (draw.mask_meta.z > 0.0) {
    const float coverage = g_textures[asuint(draw.mask_meta.x)].SampleLevel(g_samplers[kSamplerLinearClamp], input.mask_uv, 0.0).r;
    const float open = alpha * (1.0 - coverage);
    alpha = open + coverage;
    rgb = (rgb * open + draw.mask_meta.y.xxx * coverage) / max(alpha, 1e-6);
  }
  return float4(rgb, alpha);
}

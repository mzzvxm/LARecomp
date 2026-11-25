// MCLA GPU renderer -- the shared rear view's two passes (mirror_view.h, perf/MIRROR-VIEW.md).
//
// One source, two shaders (the CMake lines pick one with MIRROR_RESOLVE):
//   MIRROR_RESOLVE=1  the average of the host samples of the multisampled view, written whole into the single sample image the readers sample. Each sample is cleaned of NaN and infinity
//                     first (nan_guard.hlsli: one stray sample of the game's own mirror draws would spread through the average and then through the bloom of the post chain).
//   MIRROR_RESOLVE=0  the HUD stamp: the picture drawn into a rectangle of the main colour by a viewport and a scissor the size of that rectangle, a bilinear fetch of the part of the picture
//                     the crop names (the whole of it for the HUD mirror at the game's own field of view; the centred part when the view is wider than the stamp, package MV4).
// The picture is in the units of the main colour: the view has its format and its exponent bias, so the stamp is a straight copy (no conversion).

#include "host_pass_common.hlsli"
#include "nan_guard.hlsli"

#if MIRROR_RESOLVE
struct MirrorViewPush {
  // x the multisampled picture's bindless slot (read as a Texture2DMS: space 1), y its sample count
  uint4 slots;
};
#else
#include "host_pass_samplers.hlsli"
struct MirrorViewPush {
  // x the picture's bindless slot (one sample)
  uint4 slots;
  // xy the stamp's top left corner in the target's pixels, zw its size in pixels
  float4 rect;
  // u0 v0 u1 v1: the part of the picture the stamp shows
  float4 crop;
};
#endif

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<MirrorViewPush> g_push;
#else
ConstantBuffer<MirrorViewPush> g_push : register(b0, space4);
#endif

#if MIRROR_RESOLVE
float4 main(float4 position : SV_Position) : SV_Target {
  const int2 texel = int2(position.xy);
  const uint count = max(g_push.slots.y, 1u);
  float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
  [loop] for (uint s = 0u; s < count; ++s) {
    sum += NgFinite4(g_textures_ms[g_push.slots.x].Load(texel, s));
  }
  return sum / float(count);
}
#else
float4 main(float4 position : SV_Position) : SV_Target {
  const float2 local = (position.xy - g_push.rect.xy) / max(g_push.rect.zw, float2(1.0f, 1.0f));
  const float2 uv = lerp(g_push.crop.xy, g_push.crop.zw, saturate(local));
  return NgFinite4(g_textures[g_push.slots.x].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f));
}
#endif

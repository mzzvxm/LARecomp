// MCLA GPU renderer -- volumetric light, pass 3: the composite (volumetrics.h).
//
// Drawn into the main view's colour target at the game's end-of-scene copy (after the fog quad, before the copy, the bloom, the temporal
// upscaler and the tone map see the colour), a full-screen triangle on the host pass layout (host_passes.h), in a pipeline whose blend is
//   dst = src.rgb + dst.rgb * src.a        (ONE, SRC_ALPHA; the alpha is not written)
// on every sample of a multisampled target. The pixel is placed in the volume by its view depth (from the main depth plane, linearised with
// the lens as the occlusion does: t = (P32 - ndc P33) / (P22 - ndc P23), ndc = 1 - stored; the sky, stored 0, takes the far plane) and
// reads the accumulated volume at that depth: two bilinear fetches of the 2D atlas (the slices around it), (S.rgb, T).
// The output is (S, lerp(1, T, extinction_scale)): the added medium dims the picture only by the share the setting asks for (the game's own
// fog attenuates already).
//
// The depth is the guest's sample 0 of the (possibly multisampled) depth plane: APPLY_MS 1 reads a Texture2DMS in space 1.

#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"

#ifndef APPLY_MS
#define APPLY_MS 0
#endif

struct VolApplyPush {
  // x the main depth plane's bindless slot, y the accumulated volume's slot, z the grid packed as nx | ny << 8 | nz << 16 | tiles_x << 24,
  // w the depth's guest sample (the host sample that is the guest's sample 0)
  uint4 slots;
  // xy the picture's size in host pixels, z the extinction scale (0: purely additive), w unused
  float4 sizes;
  // x near, y far (metres), z 1 / ln(far / near), w unused
  float4 depth;
  // the projection's depth rows P22, P23, P32, P33 (ao.cpp: lin)
  float4 lin;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<VolApplyPush> g_push;
#else
ConstantBuffer<VolApplyPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
#if APPLY_MS
  const float stored = g_textures_ms[g_push.slots.x].Load(pixel, g_push.slots.w).r;
#else
  const float stored = g_textures[g_push.slots.x].Load(int3(pixel, 0)).r;
#endif
  const uint nx = g_push.slots.z & 0xFFu;
  const uint ny = (g_push.slots.z >> 8u) & 0xFFu;
  const uint nz = (g_push.slots.z >> 16u) & 0xFFu;
  const uint tiles_x = (g_push.slots.z >> 24u) & 0xFFu;
  const uint tiles_y = nz / tiles_x;
  // the view depth of the pixel; the sky (stored 0) and anything beyond the volume read its far end
  float t = g_push.depth.y;
  if (stored > 0.0) {
    const float ndc = 1.0 - saturate(stored);
    const float den = g_push.lin.x - ndc * g_push.lin.y;
    const float z = (g_push.lin.z - ndc * g_push.lin.w) / den;
    if (den != 0.0 && z > 0.0) {
      t = min(z, g_push.depth.y);
    }
  }
  const float s = float(nz) * log(max(t, g_push.depth.x) / g_push.depth.x) * g_push.depth.z - 0.5;
  const float s_floor = floor(s);
  const float blend = s - s_floor;
  const uint k0 = uint(clamp(s_floor, 0.0, float(nz) - 1.0));
  const uint k1 = min(uint(clamp(s_floor + 1.0, 0.0, float(nz) - 1.0)), nz - 1u);
  const float2 uv = position.xy / g_push.sizes.xy;
  const float2 tile_texel = clamp(uv * float2(nx, ny), 0.5, float2(nx, ny) - 0.5);
  const float2 atlas = float2(nx * tiles_x, ny * tiles_y);
  const float2 uv0 = (float2(k0 % tiles_x * nx, k0 / tiles_x * ny) + tile_texel) / atlas;
  const float2 uv1 = (float2(k1 % tiles_x * nx, k1 / tiles_x * ny) + tile_texel) / atlas;
  const Texture2D<float4> volume = g_textures[g_push.slots.y];
  const float4 a = volume.SampleLevel(g_samplers[kSamplerLinearClamp], uv0, 0);
  const float4 b = volume.SampleLevel(g_samplers[kSamplerLinearClamp], uv1, 0);
  const float4 v = lerp(a, b, saturate(blend));
  const float3 light = max(v.rgb, 0.0);
  const float transmittance = lerp(1.0, saturate(v.a), g_push.sizes.z);
  return float4(light, transmittance);
}

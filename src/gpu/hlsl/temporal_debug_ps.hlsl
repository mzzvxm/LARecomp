// MCLA GPU renderer -- the temporal system's debug views (temporal_mv.h, mcla_gpu_temporal_view).
//
// One of the images the system keeps, letterboxed in the present's bottom-right quarter (the
// whole picture with mcla_gpu_temporal_view_full; the host sets the viewport, `origin` and `scale`):
//   mode 0  motion vectors: mid grey is no motion; red and green move with the vector's x and y,
//           saturating at `range` render pixels. A still camera over still scenery is one flat grey.
//   mode 1  depth, as stored (1 near, 0 far) with a 4th-root curve so the far scene is visible:
//           near is bright. The raw host value, no assumption about its direction.
//   mode 2  stencil >> 2, the vehicle ID the game's draws write: black is 0 (the world), a colour
//           for each of 1 to 23, white for 24 and above (the UI's draws, not vehicles).

#include "host_pass_common.hlsli"

struct DebugPush {
  uint source;     // the image's bindless slot
  uint mode;
  float2 origin;   // back buffer pixel where the picture starts
  float2 scale;    // image pixels per back buffer pixel
  float range;     // mode 0: render pixels for full colour
  float reserved;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<DebugPush> g_push;
#else
ConstantBuffer<DebugPush> g_push : register(b0, space4);
#endif

float3 Hue(float h) {
  const float3 k = abs(frac(h + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f);
  return saturate(k - 1.0f);
}

float4 main(float4 position : SV_Position) : SV_Target {
  uint width;
  uint height;
  g_textures[g_push.source].GetDimensions(width, height);
  const int2 pixel = clamp(int2(floor((position.xy - g_push.origin) * g_push.scale)), int2(0, 0),
                           int2(width, height) - 1);
  const float4 texel = g_textures[g_push.source].Load(int3(pixel, 0));
  if (g_push.mode == 0u) {
    const float2 v = clamp(texel.xy / g_push.range, -1.0f, 1.0f);
    return float4(0.5f + 0.5f * v.x, 0.5f + 0.5f * v.y, 0.5f, 1.0f);
  }
  if (g_push.mode == 1u) {
    const float g = pow(saturate(texel.x), 0.25f);
    return float4(g, g, g, 1.0f);
  }
  const uint id = uint(texel.x * 255.0f + 0.5f);
  if (id == 0u) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }
  if (id >= 24u) {
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
  }
  return float4(Hue(frac(float(id) * 0.61803399f)), 1.0f);
}

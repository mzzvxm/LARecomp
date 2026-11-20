// MCLA GPU renderer -- the headlight shadow atlas's debug view (mcla_gpu_headlight_shadow_view 1, local_shadow.h).
//
// The atlas drawn into a rectangle of the scene colour (a viewport the size of the rectangle, the colour's rgb written, the alpha kept): grey by
// the distance from the lamps (near bright, far dark; the stored depth is the light's z/w, so it is turned back to metres with the view's near and
// far planes), a dim blue where nothing was drawn (the stored 1), the tiles framed in yellow. One pixel of the rectangle reads the atlas texel under
// it (Load: the rectangle is the atlas's size or smaller, so some texels are skipped on a small picture; this is a look, not a measurement).

#include "host_pass_common.hlsli"

struct LocalShadowViewPush {
  // x the atlas's bindless slot (a depth plane, read as one float), y its width and z its height in texels
  uint4 slots;
  // xy the rectangle's top left corner in pixels, zw its size in pixels
  float4 rect;
  // x the first view's near plane, y its far plane (metres from the eye), z a tile's edge in texels
  float4 depth;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<LocalShadowViewPush> g_push;
#else
ConstantBuffer<LocalShadowViewPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 local = position.xy - g_push.rect.xy;
  const float2 size = g_push.rect.zw;
  if (min(local.x, local.y) < 1.0f || local.x > size.x - 1.0f || local.y > size.y - 1.0f) {
    return float4(1.0f, 0.9f, 0.2f, 1.0f);
  }
  const int2 atlas = int2(g_push.slots.yz);
  const int2 texel = min(int2(local / size * float2(atlas)), atlas - 1);
  const int tile = max(int(g_push.depth.z), 1);
  if (min(texel.x % tile, texel.y % tile) < 1) {
    return float4(0.55f, 0.5f, 0.1f, 1.0f);
  }
  const float stored = g_textures[g_push.slots.x].Load(int3(texel, 0)).x;
  if (stored >= 1.0f) {
    return float4(0.02f, 0.03f, 0.10f, 1.0f);
  }
  const float n = g_push.depth.x;
  const float f = g_push.depth.y;
  // z/w = f/(f-n) (1 - n/d): d = n f / (f - z/w (f - n))
  const float metres = n * f / max(f - stored * (f - n), 1e-4f);
  const float t = saturate((metres - n) / max(f - n, 1e-3f));
  const float grey = lerp(1.0f, 0.15f, t);
  return float4(grey, grey, grey, 1.0f);
}

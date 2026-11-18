// MCLA GPU renderer -- the car shadow map's debug view (mcla_gpu_car_shadow_map_view 1, car_shadow_map.h).
//
// The map drawn into a square of the scene colour (a viewport the size of the square, the colour's rgb written, the alpha kept): grey
// by depth, near the sun bright, far dark, and a dim blue where nothing of the car is (the stored 1); a bright line frames the square.
// One pixel of the square reads the map texel under it (Load: the square is the map's size or a little smaller, so some texels
// are skipped on a small picture; this is a look, not a measurement).

#include "host_pass_common.hlsli"

struct CarShadowViewPush {
  // x the map's bindless slot (a depth plane, read as one float), y the map's edge in texels
  uint4 slots;
  // xy the square's top left corner in pixels, zw its edge in pixels
  float4 rect;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarShadowViewPush> g_push;
#else
ConstantBuffer<CarShadowViewPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 local = position.xy - g_push.rect.xy;
  const float edge = g_push.rect.z;
  if (min(local.x, local.y) < 1.0f || max(local.x, local.y) > edge - 1.0f) {
    return float4(1.0f, 0.9f, 0.2f, 1.0f);
  }
  const float map_edge = float(g_push.slots.y);
  const int2 texel = min(int2(local / edge * map_edge), int2(g_push.slots.y - 1u, g_push.slots.y - 1u));
  const float stored = g_textures[g_push.slots.x].Load(int3(texel, 0)).x;
  if (stored >= 1.0f) {
    return float4(0.02f, 0.03f, 0.10f, 1.0f);
  }
  const float grey = lerp(1.0f, 0.15f, saturate(stored));
  return float4(grey, grey, grey, 1.0f);
}

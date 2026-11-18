// MCLA GPU renderer -- the car ray tracing check's debug picture into the scene (car_rt_scene.h, mcla_gpu_car_rt_view).
//
// The check pass (car_rt_check_cs.hlsl) writes an RGBA8 picture the size of the ambient occlusion's linear depth image: the colour of a
// vehicle's pixel with alpha 0.85, all zero elsewhere. This pass puts it over the main view's colour target, scaled to the target's
// size, with the blend the pipeline has (source alpha over the destination, the rgb only). Host pass layout (host_pass_common.hlsli).

#include "host_pass_common.hlsli"

struct CarRtViewPush {
  // x the picture's bindless slot
  uint4 slots;
  // xy the picture's size, zw the target's size, in pixels
  float4 sizes;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarRtViewPush> g_push;
#else
ConstantBuffer<CarRtViewPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 uv = position.xy / g_push.sizes.zw;
  const int2 p = min(int2(uv * g_push.sizes.xy), int2(g_push.sizes.xy) - 1);
  return g_textures[g_push.slots.x].Load(int3(p, 0));
}

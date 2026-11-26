#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "reflhit.hlsli"

struct ReflHitNativePush {
  uint material;
  uint sample0;
  uint planar;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ReflHitNativePush> g_push;
#else
ConstantBuffer<ReflHitNativePush> g_push : register(b0, space4);
#endif

uint main(float4 position : SV_Position) : SV_Target {
  const int2 p = int2(position.xy);
#if GB_VIEW_MS
  const float2 rt3 = g_textures_ms[g_push.material].Load(p, g_push.sample0).xy;
#else
  const float2 rt3 = g_textures[g_push.material].Load(int3(p, 0)).xy;
#endif
  return ReflHitFallback(GbRt3Class(rt3), GbRt3Flags(rt3), g_push.planar != 0u);
}

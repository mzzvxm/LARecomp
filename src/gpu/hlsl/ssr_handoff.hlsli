#ifndef MCLA_GPU_SSR_HANDOFF_HLSLI
#define MCLA_GPU_SSR_HANDOFF_HLSLI

#include "env_fallback.hlsli"

// reflection_handoff_rules.h Frame. The RT pass already uses sets 5 and 6.
#if SSR_HANDOFF_RT
ByteAddressBuffer g_handoff : register(t0, space7);
#else
ByteAddressBuffer g_handoff : register(t0, space5);
#endif

bool SsrHandoffEnabled() { return g_handoff.Load(0u) != 0u; }

bool SsrRoadReceiver(uint cls) {
  return cls == uint(GB_CLASS_ROAD) || cls == uint(GB_CLASS_WET_ROAD);
}

float3 SsrViewToWorld(float3 v) {
  const float3 right = float3(SsrF(SSR_W_RIGHT), SsrF(SSR_W_RIGHT + 1u), SsrF(SSR_W_RIGHT + 2u));
  const float3 up = float3(SsrF(SSR_W_UP), SsrF(SSR_W_UP + 1u), SsrF(SSR_W_UP + 2u));
  return v.x * right + v.y * up + v.z * cross(up, right);
}

struct SsrFallback {
  float3 planar;
  float3 env;
  float3 stock;
  bool has_planar;
  bool has_env;
};

SsrFallback SsrSurfaceFallback(uint cls, int2 texel, float3 pc, float3 normal, float smoothness, bool handoff) {
  SsrFallback result = (SsrFallback)0;
  const bool road = SsrRoadReceiver(cls);
  const bool ground = road || cls == uint(GB_CLASS_SIDEWALK);
  const float share = SsrF(SSR_W_FALLBACK);
  const uint road_view = SsrW(SSR_W_ROAD_VIEW);
  const bool planar = share > 0.0f && SsrHasSlot(SsrRoadViewSlot(road_view));
  const float cap = SsrF(SSR_W_CAP);
  if ((road || (ground && handoff)) && planar) {
    const float2 uv = (float2(texel) + 0.5f) / float2(SsrRender());
    result.planar = clamp(NgFinite3(SsrRoadViewAt(road_view, uv)), 0.0f, cap) * share;
    result.has_planar = true;
    if (road) {
      result.stock = result.planar;
    }
  }
  if (handoff && !result.has_planar) {
    const uint env_word = g_handoff.Load(4u);
    result.has_env = EnvFallbackAvailable(env_word);
    if (result.has_env) {
      const float3 dir = reflect(normalize(pc), normal);
      const bool correct_ground = ground && g_handoff.Load(164u) != 0u;
      const float3 probe_to_receiver = correct_ground ? SsrViewToWorld(pc) + asfloat(g_handoff.Load3(152u)) : 0.0f;
      result.env = clamp(NgFinite3(EnvFallbackSample(env_word, probe_to_receiver, normalize(SsrViewToWorld(dir)), 1.0f - smoothness, correct_ground) *
                                    asfloat(g_handoff.Load(8u))), 0.0f, cap);
      // CHECK: compare glass energy against the guest view in both day and night.
      // Glass already shows the guest environment. Use the same probe as
      // the estimate of that contribution when replacing it with a hit.
      if (cls == uint(GB_CLASS_GLASS)) {
        result.stock = result.env;
      }
    }
  }
  return result;
}

#endif

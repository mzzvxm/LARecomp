#ifdef RT_DN_SSR
#include "rt_denoise_v2_ssr.hlsli"
#else
#ifdef RT_DN_AO
#include "rt_denoise_ao_consumer.hlsli"
#else
#include "rt_denoise_plain_consumer.hlsli"
#endif
#include "rt_denoise_v2_plain.hlsli"
#ifdef RT_DN_SHADOW
#include "rt_denoise_v2_shadow.hlsli"
#endif
#endif
#include "rt_denoise_v2_engine.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
#ifdef RT_DN_PASS_TEMPORAL
  Dn2Temporal(id);
#else
  Dn2Spatial(id);
#endif
}

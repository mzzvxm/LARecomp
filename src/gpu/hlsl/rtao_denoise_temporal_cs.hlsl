#define RT_DN_PASS_TEMPORAL 1
#include "rt_denoise_ao_consumer.hlsli"
#include "rtao_reproject.hlsli"
#include "rt_denoise_engine.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) { RtAoTemporalMain(id); }

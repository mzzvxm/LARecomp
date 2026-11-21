// MCLA GPU renderer -- RtDenoise (CITY-RT package R6): the temporal pass over plain guide images (hlsl/rt_denoise_plain_consumer.hlsli lists them and the push block): the last frame's result moved
// by the reflection's virtual motion, accepted where the surface was the same, held to the neighbourhood's variance box, blended by its length; the new length goes in the alpha.
#define RT_DN_PASS_TEMPORAL 1
#include "rt_denoise_plain_consumer.hlsli"
#include "rt_denoise_engine.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  RtDnTemporalMain(id);
}

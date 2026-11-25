// MCLA GPU renderer -- RtDenoise (CITY-RT package R6) for the screen-space reflections' ray traced fallback: one iteration of the bilateral a-trous over the accumulated reflection image
// (ssr.cpp records it up to three times, steps 1, 2 and 4, between the accumulation and the resolve, in a frame the fallback followed). The pass is hlsl/rt_denoise_engine.hlsli's spatial body over
// hlsl/ssr_denoise_consumer.hlsli's guides; a pixel whose share of the fallback is small, or whose kernel is narrower than the step, is copied.
#define RT_DN_PASS_SPATIAL 1
#include "ssr_denoise_consumer.hlsli"
#include "rt_denoise_engine.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  RtDnSpatialMain(id);
}

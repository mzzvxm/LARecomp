// MCLA GPU renderer -- RtDenoise (CITY-RT package R6): the spatial pass over plain guide images (hlsl/rt_denoise_plain_consumer.hlsli lists them and the push block). RT_DN_PROFILE is the compile's:
// the specular profile unless the build says otherwise (-D RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE for a diffuse term).
#define RT_DN_PASS_SPATIAL 1
#include "rt_denoise_plain_consumer.hlsli"
#include "rt_denoise_engine.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  RtDnSpatialMain(id);
}

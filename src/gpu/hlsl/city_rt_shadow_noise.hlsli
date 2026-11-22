// MCLA GPU renderer -- the ray traced city sun shadows' disc noise (city_rt_shadow_cs.hlsl; NOISE-AUDIT, perf\NOISE-AUDIT.md).
//
// Each pixel's sun rays are spread over the sun's disc by two numbers (the stratum's radius and the angle). They were interleaved gradient noise of the pixel plus the R2 shift of the frame, a lattice,
// and a fixed one while no temporal upscaler averages the frames (the host sends 0 then): the contact march's 3 x 3 filter was all that took the pattern out of the picture of the city's shadows.
//
// Now they are the blue noise tile's two channels at the pixel stepped along the R2 sequence by the frame (blue_noise_frame.hlsli; frame 0: a fixed blue pattern, the contact march's rule).
// `code` is the push's noise.x: the frame in the low 10 bits and the occlusion's blue noise flag in bit 29 (ao_stable.h kBlueNoiseFlag, mcla_gpu_ao_blue_noise); without the flag the first version's.
//
// Plain enough to compile as C++ (city_rt_shadow_noise_test.cpp does).

#ifndef MCLA_GPU_CITY_RT_SHADOW_NOISE_HLSLI
#define MCLA_GPU_CITY_RT_SHADOW_NOISE_HLSLI

#include "blue_noise_frame.hlsli"

static const uint kCityNoiseBlueFlag = 1u << 29;   // ao_stable.h kBlueNoiseFlag

// The first version's per-pixel noise (ao_common.hlsli AoNoise).
float CityRtIgn(float2 p) {
  return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float2 CityRtShadowNoise(int px, int py, uint code) {
  const uint shift = code & 1023u;
  if ((code & kCityNoiseBlueFlag) != 0u) {
    return BlueNoiseFrame2(px, py, shift);
  }
  const float2 p = float2(float(px), float(py));
  const float f = float(shift);
  return float2(frac(CityRtIgn(p) + f * 0.75487766624669f), frac(CityRtIgn(float2(p.y + 31.0f, p.x + 11.0f)) + f * 0.56984029099806f));
}

#endif  // MCLA_GPU_CITY_RT_SHADOW_NOISE_HLSLI

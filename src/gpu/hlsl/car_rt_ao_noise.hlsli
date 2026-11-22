// MCLA GPU renderer -- the ray traced car occlusion's and sun ray's noise (car_rt_ao_cs.hlsl; NOISE-AUDIT, perf\NOISE-AUDIT.md).
//
// The pixel's part of both is the blue noise tile (blue_noise.hlsli) in place of interleaved gradient noise, which is a lattice: the traced occlusion goes through the same blur and history as the
// screen-space occlusion (ao_main_ps.hlsl takes min() of the two), so its error was the same diagonal lattice, and the sun ray's spread, which a still view without a temporal upscaler keeps at one
// pattern, was a fixed lattice that only the contact march's 3 x 3 filter took out. The flag is the occlusion's (bit 29 of the noise code, ao_stable.h kBlueNoiseFlag, mcla_gpu_ao_blue_noise);
// without it the first version's numbers, for the A/B.
//
//   occlusion   the tile's two channels at the pixel plus the occlusion's frame shift (the 13 frame lattice or the R2 recurrence of the code: ao_common.hlsli AoNoiseShift), modulo 1
//   sun ray     the tile's two channels at the pixel moved by (17, 5) (not the occlusion's numbers) stepped by the R2 sequence of `sun_frame` (0 where no temporal upscaler averages the frames:
//               a fixed blue pattern, the contact march's rule)
//
// Plain enough to compile as C++ (car_rt_ao_noise_test.cpp does).

#ifndef MCLA_GPU_CAR_RT_AO_NOISE_HLSLI
#define MCLA_GPU_CAR_RT_AO_NOISE_HLSLI

#include "blue_noise_frame.hlsli"

// The first version's per-pixel noise (ao_common.hlsli AoNoise).
float CarRtIgn(float2 p) {
  return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// The occlusion rays' two numbers of the pixel: `blue` is the code's bit 29, `shift` the frame's shift (the shader's NoiseShift).
float2 CarRtOcclusionNoise(int px, int py, bool blue, float2 shift) {
  const float2 p = float2(float(px), float(py));
  const float2 spatial = blue ? BlueNoiseTile2At(px, py) : float2(CarRtIgn(p), CarRtIgn(float2(p.y + 31.0f, p.x + 11.0f)));
  return frac(spatial + shift);
}

// The sun ray's two numbers (the disc's radius and angle): `sun_frame` the frame (0: a fixed pattern).
float2 CarRtSunNoise(int px, int py, bool blue, uint sun_frame) {
  if (blue) {
    return BlueNoiseFrame2(px + 17, py + 5, sun_frame);
  }
  const float f = float(sun_frame & 1023u);
  const float2 p = float2(float(px), float(py));
  return float2(frac(CarRtIgn(float2(p.x + 17.0f, p.y + 5.0f)) + f * 0.75487766624669f), frac(CarRtIgn(float2(p.y + 3.0f, p.x + 29.0f)) + f * 0.56984029099806f));
}

#endif  // MCLA_GPU_CAR_RT_AO_NOISE_HLSLI

// MCLA GPU renderer -- the noise of a city hit's sun ray and lamp choice in the car reflections (city_refl.hlsli CityReflHit; NOISE-AUDIT, perf\NOISE-AUDIT.md).
//
// A city hit seen in a car's chrome is shaded with one sun ray over the sun's disc and one lamp picked by weight; the two numbers of the disc and the lamp's number come from the texel being traced, the
// frame and the hit's place in the texel's cone. The first version took interleaved gradient noise of the texel (a lattice) plus the R2 shift of the frame plus a SALT hashed from the ray's direction (to
// make the cone's rays differ: a white number, which made the sum white at best and left the quantised steps of the hash as a pattern: dot(direction, ...) * 43758 loses its fraction in a float), and
// the frame's number went into the shift as a float of the whole count (coarser every minute of play).
//
// Now the numbers are the blue noise tile's two channels at the texel stepped along the R2 sequence by the sample number frame * 5 + the hit's number in the texel's cone (blue_noise_frame.hlsli: the
// product wraps in 32 bit integers, exact for any frame count; 5 steps of either R2 fraction are not near a whole turn): a texel's numbers over the frames and over the cone's rays are one low
// discrepancy sequence, the picture's pattern is the tile moved round the circle and nothing slides. Flag: the rows' flags bit 5 (city_refl_rules.h kFlagBlueNoise, mcla_gpu_noise_blue);
// without it the first version's numbers.
//
// Plain enough to compile as C++ (city_refl_noise_test.cpp does).

#ifndef MCLA_GPU_CITY_REFL_NOISE_HLSLI
#define MCLA_GPU_CITY_REFL_NOISE_HLSLI

#include "blue_noise_frame.hlsli"

static const uint kCityReflRaysPerFrame = 5u;   // the cone's four rays and the clear coat's: the hits of one texel number from 0 to 4

// The first version's per-texel noise (ao_common.hlsli AoNoise).
float CityReflIgn(float2 p) {
  return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// The hit's first two numbers: `texel` the history image's texel being traced, `serial` the frame's count (the whole number), `hit` the hit's number in the texel (0 to 4), `direction` the ray's direction
// (the first version's salt).
float2 CityReflNoise(int tx, int ty, uint serial, uint hit, float3 direction, bool blue) {
  if (blue) {
    return BlueNoiseFrame2(tx, ty, serial * kCityReflRaysPerFrame + hit);
  }
  const float salt = frac(dot(direction, float3(12.9898f, 78.233f, 37.719f)) * 43758.5453f);
  const float2 p = float2(float(tx), float(ty));
  return frac(float2(CityReflIgn(p), CityReflIgn(float2(p.y + 31.0f, p.x + 11.0f))) + float(serial) * float2(0.75487766624669f, 0.56984029099806f) + float2(salt, salt));
}

#endif  // MCLA_GPU_CITY_REFL_NOISE_HLSLI

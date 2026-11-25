// MCLA GPU renderer -- the sun shafts' ray-start noise (shafts_blur_ps.hlsl; NOISE-AUDIT, perf\NOISE-AUDIT.md).
//
// The start of every ray is shifted by a fraction of one step so that the steps do not show as bands. The first version's shift was interleaved gradient noise of the texel's position, a lattice:
// on the half size image it shows as diagonal lines (a period of about ten texels, twenty pixels of the picture) in the streaks wherever the sky's brightness changes along the ray, and no frame
// shift moves it unless a temporal upscaler runs (the host sends 0 otherwise: a fixed pattern that nothing averages but the bilinear read of the add pass).
//
// Now the shift is the blue noise tile at the texel (blue_noise.hlsli: no lattice, no low spatial frequencies) stepped along the R2 sequence by the frame while an upscaler averages the frames
// (`shift` = the frame's number 0 to 1023, 0: the fixed blue pattern), the same step for every texel. The flag is the push's slots.z bit 0 (mcla_gpu_noise_blue); without it the first version's noise.
//
// Plain enough to compile as C++ (shafts_noise_test.cpp does).

#ifndef MCLA_GPU_SHAFTS_NOISE_HLSLI
#define MCLA_GPU_SHAFTS_NOISE_HLSLI

#include "blue_noise_frame.hlsli"

// The first version's noise of a texel position (pixel centre, in the image's texels).
float ShaftsIgNoise(float2 p) {
  return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// The fraction of a step the ray of the texel (px, py), position `position` (the centre: x + 0.5, y + 0.5), starts at. `shift` is the frame's number (0 to 1023) or 0.
float ShaftsStartNoise(int px, int py, float2 position, bool blue, uint shift) {
  if (blue) {
    return BlueNoiseFrame(px, py, 0u, shift, kBlueNoiseR2StepA);
  }
  return frac(ShaftsIgNoise(position) + float(shift) * 0.6180339887f);
}

#endif  // MCLA_GPU_SHAFTS_NOISE_HLSLI

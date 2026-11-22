// MCLA GPU renderer -- the fog volume's sample jitter (volumetrics_inject_cs.hlsl; NOISE-AUDIT, perf\NOISE-AUDIT.md).
//
// A voxel's sample is moved inside the voxel by the frame's offsets (the host's Halton terms in base 2, 3 and 5, the same for every voxel: 16 frames cover the voxel evenly) plus an offset of the
// voxel's own. The first version's own offset was interleaved gradient noise of the voxel's cell, MOVED by 5.588 cells along the diagonal every frame: a lattice that slid across the volume, so
// what the history left of it (a function of the lattice's phase, advanced every frame) was a diagonal pattern that crawled over the sun shadow's edges and the beams' (the same fault the
// reflections' noise had, ssr_common.hlsli).
//
// Now the voxel's own offset is the blue noise tile at the voxel (x and y from the tile's two channels, z from the tile moved by (7, 3)), with NO frame term of its own: the frame's change is the
// Halton offset, so a voxel's samples over 16 frames are the Halton points shifted by a constant, evenly spread, and nothing slides. The flag is the inject's slots.w bit 2
// (volumetrics_stable.h kInjectFlagBlueNoise, mcla_gpu_noise_blue); without it the first version's noise.
//
// Plain enough to compile as C++ (volumetrics_noise_test.cpp does).

#ifndef MCLA_GPU_VOLUMETRICS_NOISE_HLSLI
#define MCLA_GPU_VOLUMETRICS_NOISE_HLSLI

#include "blue_noise.hlsli"

// The first version's per-voxel noise (volumetrics_rules.h InterleavedGradient): gradient noise of the cell, shifted along the diagonal by the frame (0 to 63).
float VolInterleavedGradient(float2 p, float frame) {
  const float shift = 5.588238f * fmod(frame, 64.0f);
  const float v = 0.06711056f * (p.x + shift) + 0.00583715f * (p.y + shift);
  return frac(52.9829189f * frac(v));
}

// The voxel's own offsets in 0..1 (before the frame's `jitter_xyz` is added modulo 1 and 0.5 taken out): x, y, z of the voxel (cell x, y and slice z), `frame` the frame's number.
float3 VolVoxelNoise(int x, int y, int z, float frame, bool blue) {
  if (blue) {
    const int px = x + 37 * z, py = y + 17 * z;
    const float2 t = BlueNoiseTile2At(px, py);
    return float3(t.x, t.y, BlueNoiseTileAt(px + 7, py + 3, 0u));
  }
  const float n0 = VolInterleavedGradient(float2(float(x) + 37.0f * float(z), float(y) + 17.0f * float(z)), frame);
  return float3(n0, frac(n0 * 7.31f), frac(n0 * 3.77f));
}

#endif  // MCLA_GPU_VOLUMETRICS_NOISE_HLSLI

// MCLA GPU renderer -- the blue noise tile stepped along the R2 sequence: the one per-frame noise the stochastic passes share (NOISE-AUDIT, perf\NOISE-AUDIT.md).
//
// THE RULE (AGENTS.md AGENT-RULE-NOISE): a stochastic effect's noise changes EVERY frame and a filter whose kernel covers it resolves it; it is never a fixed per-pixel lattice or a pattern that
// slides with the camera. Interleaved gradient noise (frac(52.98 * frac(0.0671 x + 0.0058 y)), a linear phase of the pixel's position) is a lattice, whatever is added to it: a few sharp spatial
// frequencies that a blur of a few pixels and an average over frames leave as thin diagonal lines, and moved by a number of pixels a frame it slides across the picture.
//
// THE PIXEL'S PART is the tile (blue_noise.hlsli: 32 x 32, no lattice and no low spatial frequencies; channel 1 is the same tile read 13 pixels right and 8 down), THE FRAME'S PART a step along the R2
// sequence (Roberts 2018: 1/g and 1/g^2 of the plastic number, as 32 bit fractions so that the product wraps exactly and a long session keeps its precision), the same for every pixel:
//     value = frac(tile + sample * step)        sample = the frame (or frame * rays + ray)
// A pixel's own numbers over the frames are a low discrepancy sequence in the square, a frame's picture of the noise is the tile moved round the circle (a blue noise for anything periodic in the
// value: an angle, a position inside a step), and the pattern stays where it is: no pixel's step differs from another's, so nothing slides. The value stays uniform on [0, 1) whatever the sample.
//
// Plain enough to compile as C++ (noise_frame_test.cpp does, through hlsl_shim.h); needs blue_noise.hlsli first (or this file includes it).

#ifndef MCLA_GPU_BLUE_NOISE_FRAME_HLSLI
#define MCLA_GPU_BLUE_NOISE_FRAME_HLSLI

#include "blue_noise.hlsli"

static const uint kBlueNoiseR2StepA = 3242174889u;   // 0.7548776662466927 * 2^32 (1 / g)
static const uint kBlueNoiseR2StepB = 2447445413u;   // 0.5698402909980532 * 2^32 (1 / g^2)

// One channel of the pixel's noise at a sample: the tile's value plus the sample's step, modulo 1. `channel` 0 or 1 (BlueNoiseTileAt), `step` kBlueNoiseR2StepA or B.
float BlueNoiseFrame(int px, int py, uint channel, uint sample_index, uint step) {
  return frac(BlueNoiseTileAt(px, py, channel) + float(sample_index * step) * (1.0f / 4294967296.0f));
}
// Both channels of the pixel at a sample (the two numbers a lobe or a disc takes), channel 0 stepped by A and channel 1 by B.
float2 BlueNoiseFrame2(int px, int py, uint sample_index) {
  return float2(BlueNoiseFrame(px, py, 0u, sample_index, kBlueNoiseR2StepA), BlueNoiseFrame(px, py, 1u, sample_index, kBlueNoiseR2StepB));
}

#endif  // MCLA_GPU_BLUE_NOISE_FRAME_HLSLI

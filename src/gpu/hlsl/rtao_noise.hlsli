#ifndef MCLA_GPU_RTAO_NOISE_HLSLI
#define MCLA_GPU_RTAO_NOISE_HLSLI

#include "blue_noise_frame.hlsli"

// The sample number of one of the eight lanes (0 spare, 1-2 the first rays' pairs, 3 the alpha hash, 4-5 the debug rays' pairs, 7 the cabin probe):
// the frame is the LOW 20 bits and the lane the bits above, so every lane steps ONE R2 step a frame (the whole 16-frame set of a pixel is a low
// discrepancy sequence: about 0.2 against the guard's 0.35) and the lanes sit at fixed, well apart offsets (lane x 2^20 x step: 0.60 / 0.85,
// 0.21 / 0.71, 0.81 / 0.56, 0.42 / 0.41 ...). The contract's first draft put the lane in the LOW bits (frame x 8 + lane): that is a stride of eight R2 steps,
// 6.039 turns = 0.039 a frame, and 16 frames covered a tenth of the circle (discrepancy 0.41, C2 of the noise guard failed in all three modes).
uint RtAoNoiseSample(uint frame_term, uint lane) {
  return (frame_term & 0xFFFFFu) | (lane << 20u);
}

uint RtAoSubpixel(int px, int py, uint frame_term) {
  // RTAO.md 3.1: one R2 step per frame (the lanes of RtAoNoiseSample step the same way). A stride of eight steps advances by only 0.039 modulo 1
  // and leaves corners unsampled in a 16-frame window.
  return uint(4.0f * BlueNoiseFrame(px + 31, py + 7, 0u, frame_term & 0xFFFFFu, kBlueNoiseR2StepA));
}

float2 RtAoRayNoise(int px, int py, uint frame_term, uint ray) {
  // Extra debug rays use the reserved lanes rather than sharing the alpha lane.
  return BlueNoiseFrame2(px, py, RtAoNoiseSample(frame_term, ray < 2u ? ray + 1u : ray + 2u));
}

#endif

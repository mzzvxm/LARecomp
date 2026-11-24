#ifndef MCLA_GPU_RTAO_NOISE_HLSLI
#define MCLA_GPU_RTAO_NOISE_HLSLI

#include "blue_noise_frame.hlsli"

uint RtAoNoiseSample(uint frame_term, uint lane) {
  return (frame_term & 0xFFFFFu) * 8u + lane;
}

uint RtAoSubpixel(int px, int py, uint frame_term) {
  // RTAO.md 3.1: one step per frame. Eight steps would advance by only
  // 0.039 modulo 1 and leave some corners unsampled in a 16-frame window.
  return uint(4.0f * BlueNoiseFrame(px + 31, py + 7, 0u, frame_term & 0xFFFFFu, kBlueNoiseR2StepA));
}

float2 RtAoRayNoise(int px, int py, uint frame_term, uint ray) {
  // Extra debug rays use the reserved lanes rather than sharing the alpha lane.
  return BlueNoiseFrame2(px, py, RtAoNoiseSample(frame_term, ray < 2u ? ray + 1u : ray + 2u));
}

#endif

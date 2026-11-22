// MCLA GPU renderer -- RtDenoise's 8 bit results (RT-LIGHTS-D1 part 2, C15; src/gpu/rt_lights_rules.h "the memory" has the numbers): what the temporal pass does differently when its images are RGBA8_UNORM
// (RT_DN_OUT_UNORM8: hlsl/rt_denoise_engine.hlsli includes this file under that macro). Scalar maths only: src/gpu/rt_lights_test.cpp compiles THIS FILE as C++ through hlsl_cpp_shim.h and compares it with
// the twins in rt_lights_rules.h (Round8, Store8) and with brute force.
//
// A result in 8 bits holds a signal in [0, 1] (a visibility) in the rgb and the history's LENGTH in the alpha as n / 32 (the engine's cap, kRtDnMaxLength).
//
// THE STALL AND THE BIAS. The temporal pass's exponential average moves a pixel 1 / (n + 1) of the way to the current sample each frame (n up to 32). Rounded to the nearest level of 1 / 255 the move is lost when
// it is under half a level, which is whenever the distance is under about 6 %: a lit pixel (the sample is 1) whose history says 0.94 stays at 0.94 for ever (the measured worst case is 4.3 %, and 59 % of the
// runs of the test end more than 2 % off), and a binary visibility of probability 0.05 averages to 0.072. Adding a level toward the sample where the average stalls cures the first and makes the second worse
// (0.012: the average then decays faster than it can rise). So the stored value is ROUNDED AT RANDOM: floor(v x 255 + u) / 255 with u a number in [0, 1) that changes every frame and every pixel (the
// consumer's RtDnDither3: the shared per-frame blue noise, AGENT-RULE-NOISE), which makes the stored value's expectation the average itself: no stall, no bias, and a noise of one level (0.4 %) that the average
// resolves with the rest. The values the pass does not blend (a pixel that starts again: the sample itself) are stored by the hardware's own rounding.

#ifndef MCLA_GPU_RT_DENOISE_UNORM8_HLSLI
#define MCLA_GPU_RT_DENOISE_UNORM8_HLSLI

static const float kRtDn8Levels = 255.0f;
static const float kRtDn8LengthScale = 32.0f;   // rt_lights_rules.h kLengthScale = kRtDnMaxLength

// A value as the 8 bit image stores it by the hardware's rounding (clamped, rounded to the nearest level).
float RtDnRound8(float v) {
  return floor(saturate(v) * kRtDn8Levels + 0.5f) / kRtDn8Levels;
}
// A value rounded at random: `u` in [0, 1) is the number the pixel has this frame; the expectation over u is the value itself (rt_lights_rules.h Store8 is the twin).
float RtDnStore8(float v, float u) {
  return min(floor(saturate(v) * kRtDn8Levels + u), kRtDn8Levels) / kRtDn8Levels;   // (255 + u rounds to 256 in a float for u within a few millionths of 1: the image would clamp it, the twin does too)
}
// The history's length as the alpha stores it, and back (the alpha of a bilinear read is a blend of lengths: not rounded to a whole one).
float RtDnEncodeLength(float n) {
  return saturate(n / kRtDn8LengthScale);
}
float RtDnDecodeLength(float a) {
  return saturate(a) * kRtDn8LengthScale;
}

#endif  // MCLA_GPU_RT_DENOISE_UNORM8_HLSLI

// MCLA GPU renderer -- the dither of the last pass that writes the swap chain (present.h).
//
// The back buffer holds 8 bits per channel (B8G8R8A8_UNORM) or, for HDR10, 10 (R10G10B10A2_UNORM),
// and the hardware rounds the shader's float to the nearest code. A slow gradient (the sky at
// dusk) then breaks into bands one code wide wherever the picture has more precision than the
// target: the display's 10-bit gamma table, a bilinear or FSR1 upscale, the float scene of HDR
// stage 3. Adding a threshold in -0.5 .. +0.5 of a code before that rounding makes the share of
// pixels that land on the next code equal to the fraction the value had, so the mean of the
// dithered picture is the undithered value exactly and a band edge dissolves into a stipple finer
// than the eye resolves. A value that is already a whole code stays where it is (the threshold
// never reaches a half), so flat areas and black get no noise at all.
//
// The threshold is interleaved gradient noise (J. Jimenez, "Next Generation Post Processing in
// Call of Duty: Advanced Warfare", SIGGRAPH 2014 course notes; a published formula): a pattern of
// the pixel's position alone whose error is pushed to high spatial frequencies, like blue noise.
// It is the same on every frame (no flicker, and a frame generator that interpolates two frames
// meets the same pattern in both), and it is counted from the picture's own corner, so the present
// pass and FSR1's RCAS give a pixel of the picture the same value wherever the letterbox puts it.
//
// Written for HLSL 2018 too (FSR1's RCAS builds with it).

#ifndef MCLA_GPU_OUTPUT_DITHER_HLSLI
#define MCLA_GPU_OUTPUT_DITHER_HLSLI

// `code`: the colour in the target's encoding (after the gamma table, or the PQ encode), the value
// the hardware is about to round. `pixel`: the pixel's position from the picture's corner (any
// fraction is dropped). `lsb`: one code of the target in that encoding (1/255, HDR10 1/1023); 0
// adds nothing (the passes that write an intermediate, a test's offscreen target, the HDR test
// pattern).
float3 OutputDither(float3 code, float2 pixel, float lsb) {
  if (lsb <= 0.0f) {
    return code;
  }
  const float gradient = dot(floor(pixel), float2(0.06711056f, 0.00583715f));
  const float threshold = (frac(52.9829189f * frac(gradient)) - 0.5f) * lsb;
  return code + float3(threshold, threshold, threshold);
}

#endif  // MCLA_GPU_OUTPUT_DITHER_HLSLI

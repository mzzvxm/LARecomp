// MCLA GPU renderer -- sun shafts (shafts.h), shared declarations.
//
// The passes (shafts_sky_ps, shafts_blur_ps, shafts_add_ps) are full-screen triangles on the host
// pass layout of host_passes.h (the bindless texture sets at spaces 0-3, the sampler set at space 4,
// 64 bytes of push constants), one ShaftsPush for all of them; each reads the fields it needs.
// Pixels are in the pass's own target pixels, y down; the guest's depth is 1 near and 0 far. The
// two images the passes make are single-channel (R16_FLOAT): a brightness, to which the sun's
// colour is given by the last pass. They are half the picture's size, or capped at a 960x540 box
// (shafts_image.h): the blur and the add pass work in UV, so they do not depend on it.

#ifndef MCLA_GPU_SHAFTS_COMMON_HLSLI
#define MCLA_GPU_SHAFTS_COMMON_HLSLI

#include "host_pass_common.hlsli"

struct ShaftsPush {
  // sky:  x the main depth plane's bindless slot, y the main colour plane's, z the host samples
  //       of both (1: plain textures, more: Texture2DMS)
  // blur: y the sky image's slot, w the number of steps
  // add:  y the image it reads (the sky image or the blurred one for a debug view, else the
  //       blurred one), w the view (0: add the light, 1 and 2: the sky or the blurred image as a
  //       grey, 3: the light alone)
  uint4 slots;
  // xy: this pass's target size; zw: the picture's size in host pixels (sky: the planes read)
  float4 sizes;
  // sky:  x the stored depth at or under which a sample is sky, y the largest brightness kept
  // blur: xy the sun's position in the image (0 to 1, y down), z the density (how much of the way
  //       to the sun a ray covers), w the decay of one step
  float4 sun;
  // sky:  xy the footprint of one texel of the sky image in the picture's pixels (2, 2 for an image half the
  //       picture's size; render / image when the cap made it smaller), w the threshold, the brightness
  //       below which the sky does not shine
  // blur: w the frame's noise shift (0 to 1023)
  // add:  xyz the light's colour times its strength (the sun's colour, the intensity and the fades)
  float4 tint;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ShaftsPush> g_push;
#else
ConstantBuffer<ShaftsPush> g_push : register(b0, space4);
#endif

#endif  // MCLA_GPU_SHAFTS_COMMON_HLSLI

// MCLA GPU renderer -- the shared screen products' Hi-Z (screen_shared.h): the pure functions of its two operations. screen_rules.h is their C++ twin and
// screen_shared_test.cpp compiles this file as C++ and compares them.
//
// The Hi-Z is a MIN chain of the linear depth: level 0 is the depth chain's level 0 with its marks cleared, every level above it the nearest of the 2x2 texels below (a mean over
// a depth edge would hide the surface a ray hits, which is why the AO's coarser levels, weighted means, cannot serve).

#ifndef MCLA_GPU_SCREEN_HIZ_HLSLI
#define MCLA_GPU_SCREEN_HIZ_HLSLI

// The low bits of a level 0 linear depth that are marks and not depth: the vehicle mark (bit 0, ao_common.hlsli AoIsVehicle) and the half size source (bits 1 and 2,
// ao_gbuffer_normal.hlsli AoGbSetSource). screen_frame.h kDepthMarkMask.
static const uint kScreenDepthMarkMask = 7u;

// Level 0 of the chain: the depth with the marks off (at most seven units in the last place lower).
float ScreenHizStrip(float level0_depth) {
  return asfloat(asuint(level0_depth) & ~kScreenDepthMarkMask);
}

// A level above 0: the nearest of the four texels of the level below.
float ScreenHizMin4(float4 d) {
  return min(min(d.x, d.y), min(d.z, d.w));
}

#endif  // MCLA_GPU_SCREEN_HIZ_HLSLI

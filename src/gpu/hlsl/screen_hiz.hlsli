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

// The FUSED passes (SS-3, screen_shared.h): the depth chain's passes also write the Hi-Z's first levels, so that the Hi-Z's own passes for them are not recorded.
//   level 0   ao_depth_ps.hlsl compiled with AO_HIZ0 writes ScreenHizStrip(its own texel) into a second target: what the Hi-Z's level 0 pass made of the depth chain's level 0.
//   level 1   ao_mip_ps.hlsl compiled with AO_HIZ writes the nearest of the four level 0 texels it reads for the depth chain's level 1, each with its marks off: ScreenHizMin4 of what the Hi-Z's
//             level 0 holds at those four texels (the strip is applied to each texel before the min, not to the min, so that this is the old pass's statement and not an argument about order).
//   level 2   the same pass reads the Hi-Z's level 1 for it: those texels have their marks off already, which the strip leaves as it is.
float ScreenHizMin4Stripped(float4 d) {
  return ScreenHizMin4(float4(ScreenHizStrip(d.x), ScreenHizStrip(d.y), ScreenHizStrip(d.z), ScreenHizStrip(d.w)));
}

#endif  // MCLA_GPU_SCREEN_HIZ_HLSLI

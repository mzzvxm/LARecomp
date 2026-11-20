// MCLA GPU renderer -- the occlusion's use of the G-buffer's normal plane (ao.h, GB-E): the rules every pass shares.
//
// The G-buffer (gbuffer_types.h) keeps the WORLD normal of the surfaces whose shader has the G-buffer variant, octahedral, in RT1 (hlsl/gbuffer_common.hlsli), and 0 =
// "no data" in every pixel that no variant wrote. The occlusion works in VIEW space (hlsl/ao_common.hlsli: x right, y up, z forward, in the game's world units) and used to
// take the normal from the depth's neighbours in each of its passes. Now one pass (ao_normal_ps) makes a normal image once a frame, the passes load from it, and this file is
// where the choice is made PER PIXEL: a pixel whose RT1 texel holds a normal that makes sense takes it (turned into view space with the camera's rotation), every other pixel
// takes the depth's, exactly as before.
//
// This file is the C++ twin's shared source (src/gpu/ao_gbuffer.h; ao_gbuffer_test.cpp compiles THIS TEXT as C++ through a shim of the HLSL vector types and compares each
// function with the twin and with the old way of the depth). So it keeps to the subset both languages read: scalar and vector arithmetic with .x .y .z .w, dot, cross,
// normalize, saturate, sqrt, asuint, asfloat, no swizzles of more than one component, no `out` parameters. It needs hlsl/gbuffer_common.hlsli first (GbRt1Valid, GbRt1Decode).

#ifndef MCLA_AO_GBUFFER_NORMAL_HLSLI
#define MCLA_AO_GBUFFER_NORMAL_HLSLI

// A G-buffer normal that faces away from the camera by more than this (the cosine of the angle between the normal and the direction to the camera) is not taken: a smooth
// interpolated normal can face a little away at a silhouette, but a surface seen from behind is not a surface the camera can see: what the texel holds is then stale (a plane
// the backend left under a later draw that has no variant) or wrong, and the pixel takes the depth's normal. -0.25 is 104 degrees.
static const float kAoGbRejectFacing = -0.25f;
// A normal that faces away by less than the rejection is kept as the nearest grazing one: the cosine it is given. The passes treat a normal as facing the camera (the horizon search
// takes the cosine of its angle to the view vector as a non negative number).
static const float kAoGbMinFacing = 0.05f;

// A world direction in view space, from the camera's rotation: the rows are the camera's right, up and forward in world space.
float3 AoGbToView(float3 world, float3 row_x, float3 row_y, float3 row_z) {
  return float3(dot(world, row_x), dot(world, row_y), dot(world, row_z));
}

// The view-space normal a pixel takes from the G-buffer: xyz the unit normal facing the camera, w 1; w 0 (and xyz 0) where the pixel takes the depth's: the texel holds no data, or
// holds a normal that faces away (kAoGbRejectFacing). `rt1` is what a load of RT1 returns, `pc` the pixel's view position (the camera is at the origin).
float4 AoGbViewNormal(float2 rt1, float3 pc, float3 row_x, float3 row_y, float3 row_z) {
  if (!GbRt1Valid(rt1)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  float3 n = AoGbToView(GbRt1Decode(rt1), row_x, row_y, row_z);
  const float3 to_camera = pc * (-1.0f / sqrt(dot(pc, pc)));
  const float facing = dot(n, to_camera);
  if (!(facing >= kAoGbRejectFacing)) {  // (a NaN is rejected too)
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  if (facing < kAoGbMinFacing) {
    n = n + to_camera * (kAoGbMinFacing - facing);
    n = n * (1.0f / sqrt(dot(n, n)));
  }
  return float4(n.x, n.y, n.z, 1.0f);
}

// THE DEPTH'S LOW BITS. The depth pass (ao_depth_ps) keeps the vehicle mark in the lowest bit of each level 0 depth (ao_common.hlsli AoIsVehicle). At half size (AoLow) a pixel
// stands for a 2x2 block of the depth plane and takes the depth of ONE of its four samples (the one nearest the block's mean) or the mean of them; with the G-buffer's
// normals on, the two bits above the mark say which of the four it was (0 top left, 1 top right, 2 bottom left, 3 bottom right), so that the normal pass reads the texel of
// RT1 that belongs to the surface the depth is of, not the block's first: across a depth edge the two are different surfaces. One unit in the last place of a 32 bit float is a
// relative 1.2e-7: three bits are 7 units at the most, far below anything that reads the depth.
uint AoGbSourceOf(float depth) {
  return (asuint(depth) >> 1u) & 3u;
}
float AoGbSetSource(float depth, uint index) {
  return asfloat((asuint(depth) & ~6u) | ((index & 3u) << 1u));
}

// THE FACING OF A SAMPLE'S SURFACE, for the bounce light's bitmask (ao_main_ps.hlsl AoGiSample), from the surface's own normal in place of a second depth. The bitmask pass asks whether
// a sample's surface faces the pixel: the normal of the surface's profile in the slice plane (`axis` is the plane's unit normal; the profile is the surface cut by it), turned to
// face the camera at the sample (`sp`, its view position), against the direction from the sample back to the pixel (`dir` is the unit direction from the pixel to the sample): 0 for a
// surface that faces away, 1 from a quarter of the way to facing straight at it. The depth's own way (the position one texel further along the slice, then a cross product with
// the axis) gives the same normal on a plane; it reads a second depth, and at a depth edge the second texel lands on another surface. Where the slice plane is parallel to the
// surface (the profile is a point) there is no profile and the surface is taken to face the pixel, as it is where the second depth is missing.
float AoGbFacingFromNormal(float3 normal, float3 axis, float3 sp, float3 dir) {
  float3 in_plane = normal - axis * dot(normal, axis);
  const float length_squared = dot(in_plane, in_plane);
  if (length_squared < 1.0e-6f) {
    return 1.0f;
  }
  in_plane = in_plane * (1.0f / sqrt(length_squared));
  if (dot(in_plane, sp) > 0.0f) {
    in_plane = in_plane * -1.0f;
  }
  return saturate(4.0f * dot(in_plane, dir * -1.0f));
}

#endif  // MCLA_AO_GBUFFER_NORMAL_HLSLI

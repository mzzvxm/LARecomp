// MCLA GPU renderer -- ambient occlusion, pass 3: the denoise (ao.h).
//
// A bilateral blur of the visibility image, R16_FLOAT, 3x3 (radius 1) or 5x5 (radius 2). A
// neighbour counts by its distance in pixels and by how far it is from the surface plane the
// centre pixel is on (its position against the centre's tangent plane, from the depth-derived
// normal), so a blur does not run across a depth edge or round a corner, and a surface seen at a
// grazing angle, whose neighbours differ a lot in depth but lie in one plane, is blurred like any
// other. The noise of the main pass is a per-pixel pattern that changes every frame: this pass takes
// most of it out of any one frame, and the accumulation over frames (ao_temporal_ps) the rest. It
// runs once while there is a history to accumulate into, twice (the second reading the first's
// output) when there is none, and never not at all.

#include "ao_common.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.zw;
  const int2 isize = int2(size);
  const uint visibility_slot = g_push.slots.z;
  const uint depth_slot = g_push.slots.w;
  const float tc = AoLoadLinear(depth_slot, pixel);
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  const float3 normal = AoNormalFromImage(depth_slot, pixel, isize, tc, pc);
  const float result =
      AoBilateral(visibility_slot, depth_slot, pixel, isize, tc, pc, normal, AoDenoiseRadius());
  return float4(result, 0.0f, 0.0f, 0.0f);
}

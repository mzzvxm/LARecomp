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
  const float centre_value = g_textures[visibility_slot].Load(int3(pixel, 0)).x;
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  const float3 normal = AoNormalFromImage(depth_slot, pixel, isize, tc, pc);
  const int radius = AoDenoiseRadius();
  // How far from the plane a neighbour may be and still be this surface: a share of the distance.
  const float plane_sigma = 0.012f * tc + 0.02f;
  const float spatial_sigma = radius >= 2 ? 1.4f : 0.9f;

  float sum = 0.0f;
  float weights = 0.0f;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int2 q = clamp(pixel + int2(dx, dy), int2(0, 0), isize - 1);
      const float tq = AoLoadLinear(depth_slot, q);
      const float3 pq = AoViewPos(float2(q) + 0.5f, size, tq);
      const float off_plane = dot(normal, pq - pc) / plane_sigma;
      const float w_depth = exp(-0.5f * off_plane * off_plane);
      const float w_space = exp(-0.5f * float(dx * dx + dy * dy) / (spatial_sigma * spatial_sigma));
      const float w = w_depth * w_space;
      sum += g_textures[visibility_slot].Load(int3(q, 0)).x * w;
      weights += w;
    }
  }
  // The centre is always in the sum with weight 1: no division by zero, and a pixel with no
  // neighbour on its plane keeps its own value.
  return float4(weights > 0.0f ? sum / weights : centre_value, 0.0f, 0.0f, 0.0f);
}

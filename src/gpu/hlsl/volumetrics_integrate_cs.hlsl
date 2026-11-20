// MCLA GPU renderer -- volumetric light, pass 2: the integrate (volumetrics.h).
//
// One thread per column (x, y) of the grid, a loop over the slices from the camera outward: the energy conserving march
//
//   S += T (Sv - Sv exp(-sigma dz)) / sigma,   T *= exp(-sigma dz)
//
// over the inject's (Sv = in-scattered radiance per metre, sigma = extinction), written as (S.rgb, T) into the accumulated volume at each
// slice's CENTRE depth (the composite reads it trilinearly: a texel stands for the depth of its slice's centre, so the march is split in two halves
// per slice and the value is written between them).
//
// Shader Model 6.0, 8 x 8 x 1 threads over the columns.

#include "volumetrics_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint4 grid = VolU4(kO_grid);
  if (id.x >= grid.x || id.y >= grid.y) {
    return;
  }
  const float4 depth = VolF4(kO_depth);  // near, far, ln(far / near)
  const Texture2D<float4> scatter = g_textures[g_push.slots.y];
  float3 s = 0.0;
  float t = 1.0;
  float d0 = depth.x;
  [loop] for (uint k = 0u; k < grid.z; ++k) {
    const float d_centre = depth.x * exp(depth.z * (float(k) + 0.5) / float(grid.z));
    const float d1 = depth.x * exp(depth.z * float(k + 1u) / float(grid.z));
    const uint2 texel = AtlasTexel(id.xy, k, grid);
    const float4 sv = scatter.Load(int3(texel, 0));
    const float sigma = max(sv.w, 1e-6);
    // the first half of the slice, up to its centre
    float tz = exp(-sigma * (d_centre - d0));
    s += t * (sv.rgb - sv.rgb * tz) / sigma;
    t *= tz;
    g_out[texel] = float4(min(s, 60000.0), t);
    // the second half
    tz = exp(-sigma * (d1 - d_centre));
    s += t * (sv.rgb - sv.rgb * tz) / sigma;
    t *= tz;
    d0 = d1;
  }
}

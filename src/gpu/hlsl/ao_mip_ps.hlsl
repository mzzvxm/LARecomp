// MCLA GPU renderer -- ambient occlusion, pass 1b: a coarser level of the linear depth chain
// (ao.h). Run twice, from level 0 to 1 and from 1 to 2, R16_FLOAT.
//
// The main pass takes its far samples from these levels (the farther from the pixel, the coarser,
// which keeps the texture reads of a large radius in the cache). Each texel is a weighted mean of
// the 2x2 texels below it; the weights favour the farthest of the four, as XeGTAO's filter does: a
// level that leaned to the nearest depth would grow every occluder's silhouette into halos.

#include "ao_common.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const int2 last = int2(g_push.sizes.zw) - 1;
  const uint slot = g_push.slots.z;
  float4 d;
  d.x = AoLoadLinear(slot, min(pixel * 2 + int2(0, 0), last));
  d.y = AoLoadLinear(slot, min(pixel * 2 + int2(1, 0), last));
  d.z = AoLoadLinear(slot, min(pixel * 2 + int2(0, 1), last));
  d.w = AoLoadLinear(slot, min(pixel * 2 + int2(1, 1), last));
  const float far_d = max(max(d.x, d.y), max(d.z, d.w));
  // Samples nearer than the farthest by more than this are another surface.
  const float range = max(0.75f * g_push.lens.z * g_push.lens.w, 0.01f);
  const float4 w = saturate(1.0f - (far_d - d) / range);
  const float sum = dot(w, d);
  const float weights = w.x + w.y + w.z + w.w;
  return float4(min(sum / max(weights, 1e-6f), kAoSky), 0.0f, 0.0f, 0.0f);
}

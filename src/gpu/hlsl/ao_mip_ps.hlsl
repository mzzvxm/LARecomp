// MCLA GPU renderer -- ambient occlusion, pass 1b: a coarser level of the linear depth chain
// (ao.h). Run twice, from level 0 to 1 and from 1 to 2, R16_FLOAT.
//
// The main pass takes its far samples from these levels (the farther from the pixel, the coarser,
// which keeps the texture reads of a large radius in the cache). Each texel is a weighted mean of
// the 2x2 texels below it; the weights favour the farthest of the four, as XeGTAO's filter does: a
// level that leaned to the nearest depth would grow every occluder's silhouette into halos (ao_mip.hlsli).
//
// AO_HIZ (SS-3, screen_shared.h): the same pass also writes the Hi-Z's level of the same size into a second target (SV_Target1, R32_FLOAT): the nearest of the four texels of the Hi-Z's level below, with
// the depth's marks off (screen_hiz.hlsli ScreenHizMin4Stripped). The four texels of level 1 are the four this pass reads (level 0 of the depth chain, marks on: the strip is the Hi-Z's own level 0
// pass); those of level 2 are the Hi-Z's level 1 (push slots.w; the depth chain's level 1 is a mean and cannot stand for them). slots.w equal to slots.z says "the same image": the four loads
// are shared. The coarser depth level is exactly what it was: the same function on the same four texels.

#include "ao_common.hlsli"
#include "ao_mip.hlsli"
#if AO_HIZ
#include "screen_hiz.hlsli"
#endif

#if AO_HIZ
struct MipHizOut {
  float4 mean : SV_Target0;
  float4 hiz : SV_Target1;
};
MipHizOut main(float4 position : SV_Position) {
#else
float4 main(float4 position : SV_Position) : SV_Target {
#endif
  const int2 pixel = int2(position.xy);
  const int2 last = int2(g_push.sizes.zw) - 1;
  const uint slot = g_push.slots.z;
  float4 d;
  d.x = AoLoadLinear(slot, min(pixel * 2 + int2(0, 0), last));
  d.y = AoLoadLinear(slot, min(pixel * 2 + int2(1, 0), last));
  d.z = AoLoadLinear(slot, min(pixel * 2 + int2(0, 1), last));
  d.w = AoLoadLinear(slot, min(pixel * 2 + int2(1, 1), last));
  // Samples nearer than the farthest by more than this are another surface.
  const float range = max(0.75f * g_push.lens.z * g_push.lens.w, 0.01f);
  const float mean = AoMipMean4(d, range);
#if AO_HIZ
  float4 h = d;
  if (g_push.slots.w != slot) {
    h.x = AoLoadLinear(g_push.slots.w, min(pixel * 2 + int2(0, 0), last));
    h.y = AoLoadLinear(g_push.slots.w, min(pixel * 2 + int2(1, 0), last));
    h.z = AoLoadLinear(g_push.slots.w, min(pixel * 2 + int2(0, 1), last));
    h.w = AoLoadLinear(g_push.slots.w, min(pixel * 2 + int2(1, 1), last));
  }
  MipHizOut o;
  o.mean = float4(mean, 0.0f, 0.0f, 0.0f);
  o.hiz = float4(ScreenHizMin4Stripped(h), 0.0f, 0.0f, 0.0f);
  return o;
#else
  return float4(mean, 0.0f, 0.0f, 0.0f);
#endif
}

// MCLA GPU renderer -- ambient occlusion, pass 1b's arithmetic (ao_mip_ps.hlsl): the weighted mean of four texels of the linear depth chain's level below.
//
// A header of its own since SS-3 so that the fused pass (ao_mip_ps.hlsl compiled with AO_HIZ: the coarser depth level and the Hi-Z's level beside it, one draw) and the old one call one function,
// and so that src/gpu/screen_hiz_fuse_test.cpp compiles it as C++ and holds the fused pass to the old one. The includer defines kAoSky (ao_common.hlsli).
//
// The weights favour the farthest of the four, as XeGTAO's filter does: a level that leaned to the nearest depth would grow every occluder's silhouette into halos. `range`: samples nearer than the
// farthest by more than this are another surface.

#ifndef MCLA_GPU_AO_MIP_HLSLI
#define MCLA_GPU_AO_MIP_HLSLI

float AoMipMean4(float4 d, float range) {
  const float far_d = max(max(d.x, d.y), max(d.z, d.w));
  const float4 w = saturate(1.0f - (far_d - d) / range);
  const float sum = dot(w, d);
  const float weights = w.x + w.y + w.z + w.w;
  return min(sum / max(weights, 1e-6f), kAoSky);
}

#endif  // MCLA_GPU_AO_MIP_HLSLI

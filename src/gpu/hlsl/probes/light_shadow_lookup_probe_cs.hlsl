// TEST ONLY, not built by CMake: a compute shader that includes hlsl/light_shadow_lookup.hlsli the way a deferred light pass (GB-H) would, to prove the include compiles with dxc (D3D12) and
// dxc -spirv (Vulkan, then spirv-val), with both modes, a light's ShadowRef and the five seams defined by the includer (LIGHT-UNIFY U5). Run by hand (dxc and dxv: C:\mcv\larecomp\downloads\dxc-v1.9.2609\x\bin\x64; spirv-val: the Vulkan SDK's Bin):
//   dxc -T cs_6_0 -E main -I src/gpu/hlsl src/gpu/hlsl/probes/light_shadow_lookup_probe_cs.hlsl -Fo probe.dxil && dxv probe.dxil
//   dxc -spirv -fspv-target-env=vulkan1.1 -T cs_6_0 -E main -I src/gpu/hlsl src/gpu/hlsl/probes/light_shadow_lookup_probe_cs.hlsl -Fo probe.spv && spirv-val --target-env vulkan1.1 probe.spv
#include "../light_shadow_lookup.hlsli"

Texture2D<float4> g_textures[8] : register(t0, space1);  // the bindless depth atlas (a depth plane read as one float)
StructuredBuffer<float4> g_views : register(t0, space2); // GpuShadowBuffer's rows: 6 float4 a view (LocalShadowView), the header in the cbuffer below
cbuffer ProbeConstants : register(b0) {
  float4 g_point;   // world point
  uint4 g_light;    // x shadow kind, y first view, z view count, w atlas slot
  float4 g_weight;  // x the light's shadow weight
  uint4 g_layers;   // x GpuShadowHeader::layers
};
RWStructuredBuffer<float> g_out : register(u0);

float LslTexel(uint slot, int ix, int iy) {
  uint w, h;
  g_textures[slot].GetDimensions(w, h);
  return g_textures[slot].Load(int3(clamp(ix, 0, int(w) - 1), clamp(iy, 0, int(h) - 1), 0)).x;
}
uint LslAtlasWidth(uint slot) {
  uint w, h;
  g_textures[slot].GetDimensions(w, h);
  return w;
}
uint LslAtlasHeight(uint slot) {
  uint w, h;
  g_textures[slot].GetDimensions(w, h);
  return h;
}
LslView LslLoadView(uint entry) {
  uint b = entry * 6u;
  LslView v;
  v.m0 = g_views[b];
  v.m1 = g_views[b + 1u];
  v.m2 = g_views[b + 2u];
  v.m3 = g_views[b + 3u];
  v.rect = g_views[b + 4u];
  float4 info = g_views[b + 5u];
  v.bias = info.x;
  v.texel = info.y;
  v.light_index = asuint(info.z);
  v.valid = asuint(info.w);
  return v;
}
uint LslLayerOf(uint entry) {
  return LslLayerBits(g_layers.x, entry);
}

[numthreads(1, 1, 1)]
void main() {
  g_out[0] = LslLightLit(g_light.x, g_light.y, g_light.z, g_weight.x, g_point.x, g_point.y, g_point.z, g_light.w, LSL_MODE_SURFACE);
  g_out[1] = LslLightLit(g_light.x, g_light.y, g_light.z, g_weight.x, g_point.x, g_point.y, g_point.z, g_light.w, LSL_MODE_AIR);
}

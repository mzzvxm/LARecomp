// The guest's display gamma ramp, applied to the finished frame.
//
// The console's display controller runs every presented pixel through a ramp
// the title builds itself: it asks the kernel for the display gamma type
// (VdGetCurrentDisplayGamma -> 2, BT.709, in this SDK) and fills a 1024-entry
// 10-bit table at guest 0x828CDA48 (sub_82426B68). The emulated path applies
// it in the command processor's present as the DC_LUT; the no-CP native path
// never saw those register writes, so it presented the ramp-less image and
// every dark tone came out lifted.
//
// Measured against the emulated DC_LUT: guest[257]=194 vs DC_LUT[64]=193,
// guest[512]=461 vs 462, guest[770]=741 vs 741. Same curve.
Texture2D<float4> g_src : register(t0);
Buffer<uint> g_ramp : register(t1);
RWTexture2D<float4> g_dst : register(u0);

cbuffer Params : register(b0) {
  uint2 g_dims;
  uint g_enabled;
  uint g_pad;
};

float Apply(float c) {
  // The table is indexed by the 10-bit quantisation of the channel, which is
  // what the display controller feeds it, and returns 10 bits.
  uint index = (uint)clamp(c * 1023.0 + 0.5, 0.0, 1023.0);
  return float(g_ramp.Load(index)) * (1.0 / 1023.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_dims.x || id.y >= g_dims.y) return;
  float4 c = g_src.Load(int3(id.xy, 0));
  if (g_enabled != 0) {
    c.rgb = float3(Apply(c.r), Apply(c.g), Apply(c.b));
  }
  g_dst[id.xy] = c;
}

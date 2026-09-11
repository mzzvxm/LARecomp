// Resolves a multisampled depth surface into a single-sampled R32_FLOAT copy.
//
// D3D12's own ResolveSubresourceRegion refuses this: the guest depth surface is
// a two-plane R32G8X24_TYPELESS, and every fully typed format that could name
// its depth plane was rejected -- three variants (R32_FLOAT scratch, typeless
// scratch, R32_TYPELESS scratch) each made command list Close fail, taking every
// later draw with it. Doing the resolve in a compute pass needs no format
// negotiation at all.
//
// MAX, not MIN or average: the main scene is reverse-Z, so the largest sample is
// the one nearest the camera, which is the sample a single-sampled pass would
// have kept. Averaging depth is meaningless.
Texture2DMS<float> g_src : register(t0);
RWTexture2D<float> g_dst : register(u0);

cbuffer Params : register(b0) {
  uint2 g_dims;
  uint g_samples;
  uint g_pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_dims.x || id.y >= g_dims.y) {
    return;
  }
  int2 p = int2(id.xy);
  float d = g_src.Load(p, 0);
  for (uint s = 1; s < g_samples; ++s) {
    d = max(d, g_src.Load(p, s));
  }
  g_dst[p] = d;
}

// Resolves the STENCIL plane of a multisampled depth surface into a linear
// buffer, one byte per pixel, rows g_pitch bytes apart -- the placed footprint
// a CopyTextureRegion then lands in the resolved copy's stencil plane.
//
// Why it exists: with mcla_native_gfx_msaa on, depth resolves go through the
// compute depth resolve, whose scratch is a single-plane R32_FLOAT. The stencil
// plane was never carried over, the resolved copy's stencil stayed at its
// zero-initialised contents, and xrage_postfx__PSStreakMotionBlur read vehicle
// index 0 for every pixel -- the player's car was reprojected as static world
// and blurred with it, the exact failure the depth+stencil pack was written to
// fix, back only when MSAA is on. D3D12 cannot copy or resolve a multisampled
// stencil plane, and writing stencil from a shader needs SV_StencilRef, which
// the target GPU lacks.
//
// Sample 0, not a blend: the stencil holds vehicle ids, and an average of two
// ids is a third, unrelated vehicle. Every sample a triangle covers gets the
// same REPLACE value, so sample 0 is the id of whatever covers that sample.
//
// dxc -T cs_6_0 -E main stencil_msaa_resolve_cs.hlsl -Fo stencil_msaa_resolve.dxil
Texture2DMS<uint2> g_src : register(t0);  // stencil-plane view, X32_TYPELESS_G8X24_UINT
RWByteAddressBuffer g_dst : register(u0);

cbuffer Params : register(b0) {
  uint2 g_dims;
  uint g_pitch;  // bytes per row, a multiple of 256 (and so of 4)
  uint g_pad;
};

// Each thread writes one dword: four horizontally adjacent pixels.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint x0 = id.x * 4u;
  const uint y = id.y;
  if (x0 >= g_dims.x || y >= g_dims.y) {
    return;
  }
  uint packed = 0u;
  [unroll] for (uint k = 0u; k < 4u; ++k) {
    const uint x = x0 + k;
    if (x < g_dims.x) {
      packed |= (g_src.Load(int2(x, y), 0).g & 0xFFu) << (8u * k);
    }
  }
  g_dst.Store(y * g_pitch + x0, packed);
}

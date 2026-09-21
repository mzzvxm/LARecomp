// Packs a two-plane depth-stencil surface into the single sampleable textures a
// Xenos fetch behaves like. Two outputs, because MCLA reads the same resolved
// depth through two DIFFERENT guest formats.
//
// u0, R32G32_FLOAT -- for a k_24_8 / k_24_8_FLOAT fetch.
//
//   Depth in R and stencil/256 in G, because one D3D12 view cannot expose both
//   planes; the fetch's component mapping puts G where .z is. MCLA's motion
//   blur reads only .x of this one (DepthMapSampler).
//
// u1, R8G8B8A8_UNORM -- for a k_8_8_8_8 fetch over the SAME depth resolve.
//
//   MCLA resolves the depth buffer a SECOND time, to its own address, and reads
//   that copy as an ordinary 8888 colour texture to get at the stencil byte:
//   the scene pass writes a vehicle id there with REPLACE, and
//   xrage_postfx__PSStreakMotionBlur's StencilSampler samples it and decodes
//   trunc(v * 256) to index Mc4MotionBlurVehicleMtxs, so the vehicle's own
//   motion is cancelled instead of being reprojected as static world.
//
//   Measured in blur.rdc: that fetch is addr 0x06ACD000, 1280x720, format 6
//   (k_8_8_8_8), swizzle 0x60A, endian 2 -- and its producing resolve is
//   `RESOLVE_SRCIDX idx=4`, i.e. source = depth. Served the raw two-plane
//   surface instead, the shader's .z read zero, every pixel took vehicle
//   index 0, and the player's car was reprojected with the static-world
//   matrix: |velocity| 123 px at the car against 2.5 px with the right index,
//   which saturates the pass's output alpha to 0.997 and makes the tonemap's
//   lerp take the car wholly from the 640x360 blurred buffer.
//
//   Component order reproduces what the hardware fetch yields, so no host-side
//   swizzle is needed on top: the guest dword is D24 in the high 24 bits and
//   S8 in the low 8, and the fetch constant asks for endian 2 (8-in-32), which
//   reverses the dword's bytes. After that reversal the components are
//   (S, D[7:0], D[15:8], D[23:16]).
//
// dxc -T cs_6_0 -E main depth_stencil_pack_cs.hlsl -Fo depth_stencil_pack.dxil
Texture2D<float> g_depth : register(t0);    // plane 0, R32_FLOAT_X8X24 / R24_UNORM_X8
Texture2D<uint2> g_stencil : register(t1);  // plane 1, X32_TYPELESS_G8X24_UINT / X24_TYPELESS_G8_UINT
RWTexture2D<float2> g_dst : register(u0);   // R32G32_FLOAT
RWTexture2D<float4> g_dst8888 : register(u1);  // R8G8B8A8_UNORM

cbuffer Params : register(b0) {
  uint2 g_dims;
  uint2 g_pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_dims.x || id.y >= g_dims.y) {
    return;
  }
  int3 p = int3(int2(id.xy), 0);
  float depth = g_depth.Load(p);
  // Stencil is the SECOND component of a stencil-plane view, in both the 24_8
  // and the 32_8X24 shapes.
  uint stencil = g_stencil.Load(p).g & 0xFFu;

  // Scaled by 1/256, not 1/255: the shader multiplies by 256 and truncates, so
  // the byte has to come back exactly.
  g_dst[p.xy] = float2(depth, stencil * (1.0 / 256.0));

  // The pool's depth targets are D32S8, so the depth arrives as a float with no
  // 24-bit integer form left; quantise it back to the 24 bits the guest surface
  // holds. Only the stencil byte is exact by construction, which is what the
  // motion blur reads -- the depth bytes are here so a shader that unpacks this
  // alias for depth gets something sane rather than nothing.
  uint d24 = (uint)(saturate(depth) * 16777215.0 + 0.5);
  g_dst8888[p.xy] = float4(float(stencil), float(d24 & 0xFFu), float((d24 >> 8) & 0xFFu),
                           float((d24 >> 16) & 0xFFu)) * (1.0 / 255.0);
}

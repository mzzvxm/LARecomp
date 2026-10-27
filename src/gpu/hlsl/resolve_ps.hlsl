// MCLA GPU renderer -- a resolve's copy into its product (resolve.h).
//
// Draws over the product level's rectangle; each pixel reads the EDRAM target
// at itself plus `offset` and writes what the console's resolve writes to
// memory, as the texture fetch of the destination then reads it
//: the colour of resolve_common.hlsli
// packed with the D3D11.3 UNORM rule, depth from its raw dword. The resolve
// memory gets the same values as guest bytes (resolve_memory_cs.hlsl).
//
// Variants (compile-time): RESOLVE_DEPTH (a depth/stencil source) and
// RESOLVE_MSAA (a multisampled source).

#include "host_pass_common.hlsli"
#include "xenos_formats.hlsli"

struct ResolvePush {
  uint source;   // bindless slot: the colour or depth plane view
  uint stencil;  // bindless slot: the stencil plane view (depth sources)
  int offset_x;  // source pixel = target pixel + offset
  int offset_y;
  uint samples;  // bits 0-3 count, bits 8 + 3i the host index of sample i
  float scale;   // 2^exp_bias (x32 for the 16-bit EDRAM formats) / count
  uint mode;
  uint fill;     // host pixels of a scaled source's half-pixel gap (0: none)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ResolvePush> g_push;
#else
ConstantBuffer<ResolvePush> g_push : register(b0, space4);
#endif

#include "resolve_common.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  // The first `fill` columns and rows of a scaled source no full-screen draw
  // covers (the half pixel at a scale) are read from the first
  // one covered, as Xenia's resolve does. 0 leaves every pixel as it is (the
  // source pixel is never negative).
  const int2 source = max(int2(position.xy) + int2(g_push.offset_x, g_push.offset_y),
                          int2(int(g_push.fill), int(g_push.fill)));
  const uint mode = g_push.mode & 0xFFu;
#if RESOLVE_DEPTH
  uint stencil;
  const uint d24 = ResolveDepth24(source, stencil);
  const uint dword = (d24 << 8u) | stencil;
  if (mode == kModeDepthFloat) {
    const float value = (g_push.mode & kFlagDestFloat24) != 0u ? XeFloat20e4To32(d24)
                                                                 : XeUNorm24To32(d24);
    return float4(value, 0.0f, 0.0f, 0.0f);
  }
  if (mode == kModeDepthRaw8888) {
    // The dword in memory read back as k_8_8_8_8 with the same endian: R is
    // the stencil, then the depth from its low byte up.
    return float4(float(dword & 0xFFu), float((dword >> 8u) & 0xFFu),
                  float((dword >> 16u) & 0xFFu), float(dword >> 24u)) *
           (1.0f / 255.0f);
  }
  // kModeDepthFloatBits: the dword as a k_32_FLOAT texel.
  return float4(asfloat(dword), 0.0f, 0.0f, 0.0f);
#else
  const float4 c = ResolveColor(source);
  switch (mode) {
    case kModeUnorm8888:
      return XeQuantizeUNorm(c, float4(255.0f, 255.0f, 255.0f, 255.0f));
    case kModeUnorm1555:
      return XeQuantizeUNorm(c, float4(31.0f, 31.0f, 31.0f, 1.0f));
    case kModeUnorm8:
      // Red, or blue with the swap (Xenia: the 8 bpp resolve takes one channel).
      return float4(XeQuantizeUNorm(c.rrrr, float4(255.0f, 255.0f, 255.0f, 255.0f)).r, 0.0f,
                    0.0f, 0.0f);
    case kModeRed32:
      return float4(c.r, 0.0f, 0.0f, 0.0f);
    case kModeFloat32:
      // The R32G32_FLOAT product keeps X and Y exactly.
      return float4(c.r, c.g, 0.0f, 0.0f);
    default:  // kModeFloat: the float16 target rounds to nearest even, as pack_half does.
      return c;
  }
#endif
}

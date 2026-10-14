// MCLA GPU renderer -- a resolve's copy into its product (resolve.h).
//
// Draws over the product level's rectangle; each pixel reads the EDRAM target
// at itself plus `offset` and writes what the console's resolve writes to
// memory, as the texture fetch of the destination then reads it
// (Xenia's resolve.xesli and
// resolve_full_*.xesli: sum of the selected samples, times the exponent bias
// halved per averaging step, red/blue swapped after the bias, packed with the
// D3D11.3 UNORM rule; depth copied raw as depth24 << 8 | stencil).
//
// Variants (compile-time): RESOLVE_DEPTH (a depth/stencil source) and
// RESOLVE_MSAA (a multisampled source).

#include "host_pass_common.hlsli"
#include "xenos_formats.hlsli"

#ifndef RESOLVE_DEPTH
#define RESOLVE_DEPTH 0
#endif
#ifndef RESOLVE_MSAA
#define RESOLVE_MSAA 0
#endif

// Conversions (resolve.cpp ResolveMode), bits 0-7 of `mode`.
#define kModeUnorm8888 0u
#define kModeUnorm1555 1u
#define kModeUnorm8 2u
#define kModeFloat 3u
#define kModeRed32 4u
#define kModeDepthFloat 5u
#define kModeDepthRaw8888 6u
#define kModeDepthFloatBits 7u
// Flags.
#define kFlagSwapRedBlue (1u << 8)
#define kFlagGammaSource (1u << 9)
#define kFlagSourceFloat24 (1u << 10)
#define kFlagDestFloat24 (1u << 11)

struct ResolvePush {
  uint source;   // bindless slot: the colour or depth plane view
  uint stencil;  // bindless slot: the stencil plane view (depth sources)
  int offset_x;  // source pixel = target pixel + offset
  int offset_y;
  uint samples;  // bits 0-2 count, bits 8 + 2i the host index of sample i
  float scale;   // 2^exp_bias (x32 for the 16-bit EDRAM formats) / count
  uint mode;
  uint reserved;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ResolvePush> g_push;
#else
ConstantBuffer<ResolvePush> g_push : register(b0, space4);
#endif

uint HostSample(uint i) {
  return (g_push.samples >> (8u + 2u * i)) & 3u;
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 source = int2(position.xy) + int2(g_push.offset_x, g_push.offset_y);
  const uint mode = g_push.mode & 0xFFu;
#if RESOLVE_DEPTH
  // Depth is never averaged (SanitizeCopySampleSelect): one sample.
#if RESOLVE_MSAA
  const float depth = g_textures_ms[g_push.source].Load(source, HostSample(0u)).r;
  const uint stencil = StencilOf(g_utextures_ms[g_push.stencil].Load(source, HostSample(0u)));
#else
  const float depth = g_textures[g_push.source].Load(int3(source, 0)).r;
  const uint stencil = StencilOf(g_utextures[g_push.stencil].Load(int3(source, 0)));
#endif
  // The EDRAM's 24 bits: 20e4 (truncated, Xenia's default) for kD24FS8, the
  // UNORM rounded to nearest even for kD24S8.
  const uint d24 = (g_push.mode & kFlagSourceFloat24) != 0u
                       ? XeFloat32To20e4(asuint(depth), false)
                       : uint(round(saturate(depth) * 16777215.0f));
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
  const uint count = g_push.samples & 7u;
  float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
  [loop] for (uint i = 0u; i < count; ++i) {
#if RESOLVE_MSAA
    float4 c = g_textures_ms[g_push.source].Load(source, HostSample(i));
#else
    float4 c = g_textures[g_push.source].Load(int3(source, 0));
#endif
    if ((g_push.mode & kFlagGammaSource) != 0u) {
      // The host keeps _GAMMA targets linear; the EDRAM holds the PWL value.
      c.rgb = XeLinearToPWLGamma3(c.rgb);
    }
    sum += c;
  }
  float4 c = sum * g_push.scale;
  if ((g_push.mode & kFlagSwapRedBlue) != 0u) {
    c = c.bgra;
  }
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
    default:  // kModeFloat: the float16 target rounds to nearest even, as pack_half does.
      return c;
  }
#endif
}

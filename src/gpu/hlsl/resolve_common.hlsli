// MCLA GPU renderer -- what a resolve writes, shared by its two outputs: the
// product (resolve_ps.hlsl, a host texture the destination's fetch reads) and
// the resolve memory (resolve_memory_cs.hlsl, the guest bytes the console's
// resolve leaves in memory).
//
// From Xenia (resolve.xesli and resolve_full_*.xesli, Copyright Ben Vanik and
// the Xenia authors, BSD-3-Clause): the sum of the selected samples, times the
// exponent bias halved per averaging step, red/blue swapped after the bias;
// depth copied raw as depth24 << 8 | stencil.
//
// The including shader includes host_pass_common.hlsli and
// xenos_formats.hlsli, sets RESOLVE_DEPTH (a depth/stencil source) and
// RESOLVE_MSAA (a multisampled source), and declares `g_push` -- its push
// constants -- with at least these fields, which the functions below read
// where the original single shader read them (so resolve_ps.hlsl compiles to
// the same bytes as before the split):
//   uint source;   bindless slot: the colour or depth plane view
//   uint stencil;  bindless slot: the stencil plane view (depth sources)
//   uint samples;  bits 0-2 count, bits 8 + 2i the host index of sample i
//   float scale;   2^exp_bias (x32 for the 16-bit EDRAM formats) / count
//   uint mode;     the conversion (bits 0-7) and the flags below

#ifndef MCLA_GPU_RESOLVE_COMMON_HLSLI
#define MCLA_GPU_RESOLVE_COMMON_HLSLI

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

uint HostSample(uint i) {
  return (g_push.samples >> (8u + 2u * i)) & 3u;
}

#if RESOLVE_DEPTH
// The EDRAM's depth at `source` as its 24 bits, and its stencil: the raw
// dword a depth resolve writes is depth24 << 8 | stencil. Depth is never
// averaged (SanitizeCopySampleSelect): one sample. The 24 bits are 20e4
// (truncated, Xenia's default) for kD24FS8, the UNORM rounded to nearest even
// for kD24S8.
uint ResolveDepth24(int2 source, out uint stencil) {
#if RESOLVE_MSAA
  const float depth = g_textures_ms[g_push.source].Load(source, HostSample(0u)).r;
  stencil = StencilOf(g_utextures_ms[g_push.stencil].Load(source, HostSample(0u)));
#else
  const float depth = g_textures[g_push.source].Load(int3(source, 0)).r;
  stencil = StencilOf(g_utextures[g_push.stencil].Load(int3(source, 0)));
#endif
  return (g_push.mode & kFlagSourceFloat24) != 0u ? XeFloat32To20e4(asuint(depth), false)
                                                  : uint(round(saturate(depth) * 16777215.0f));
}
#else
// The colour the resolve writes at `source`, before the destination format
// quantizes it: the selected samples summed (in the EDRAM's PWL for a _GAMMA
// target, which the host keeps linear), scaled, red and blue swapped when asked.
float4 ResolveColor(int2 source) {
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
  return c;
}
#endif

#endif  // MCLA_GPU_RESOLVE_COMMON_HLSLI

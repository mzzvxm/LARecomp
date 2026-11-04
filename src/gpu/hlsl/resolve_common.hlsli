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
//   uint samples;  bits 0-3 count, bits 8 + 3i the host index of sample i
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
#define kModeFloat32 8u  // k_32_32_FLOAT: X and Y as 32-bit floats
// Flags.
#define kFlagSwapRedBlue (1u << 8)
#define kFlagGammaSource (1u << 9)
#define kFlagSourceFloat24 (1u << 10)
#define kFlagDestFloat24 (1u << 11)
#define kFlagSourceNanToZero (1u << 12)

uint HostSample(uint i) {
  return (g_push.samples >> (8u + 3u * i)) & 7u;
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
// Selected sample `i` at `source`, in the EDRAM's PWL for a _GAMMA target
// (the host keeps those linear).
float4 ResolveSample(int2 source, uint i) {
#if RESOLVE_MSAA
  float4 c = g_textures_ms[g_push.source].Load(source, HostSample(i));
#else
  float4 c = g_textures[g_push.source].Load(int3(source, 0));
#endif
  if ((g_push.mode & kFlagSourceNanToZero) != 0u) {
    // Per sample, before the samples are summed: one NaN sample would otherwise make the whole
    // pixel NaN and cost an edge its antialiasing. Tested on the bits so no optimizer assuming
    // finite maths can fold it away.
    const uint4 bits = asuint(c);
    const bool4 nan = (bits & 0x7FFFFFFFu) > 0x7F800000u;
    c = float4(nan.x ? 0.0f : c.x, nan.y ? 0.0f : c.y, nan.z ? 0.0f : c.z, nan.w ? 0.0f : c.w);
  }
  if ((g_push.mode & kFlagGammaSource) != 0u) {
    c.rgb = XeLinearToPWLGamma3(c.rgb);
  }
  return c;
}

// The colour the resolve writes at `source`, before the destination format
// quantizes it: the selected samples summed, scaled, red and blue swapped when
// asked. The usual counts (1, 2, 4; the same for every pixel of a resolve) load
// all their samples before the first add, so the loads overlap instead of each
// waiting for the one before, as in a rolled loop: on a small GPU the resolves
// are latency-bound. The adds keep the loop's order from 0, so the sum is the
// same bits as the loop's.
float4 ResolveColor(int2 source) {
  const uint count = g_push.samples & 15u;
  float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
  [branch] if (count == 4u) {
    const float4 c0 = ResolveSample(source, 0u);
    const float4 c1 = ResolveSample(source, 1u);
    const float4 c2 = ResolveSample(source, 2u);
    const float4 c3 = ResolveSample(source, 3u);
    sum += c0;
    sum += c1;
    sum += c2;
    sum += c3;
  } else if (count == 2u) {
    const float4 c0 = ResolveSample(source, 0u);
    const float4 c1 = ResolveSample(source, 1u);
    sum += c0;
    sum += c1;
  } else if (count == 1u) {
    sum += ResolveSample(source, 0u);
  } else {
    [loop] for (uint i = 0u; i < count; ++i) {
      sum += ResolveSample(source, i);
    }
  }
  float4 c = sum * g_push.scale;
  if ((g_push.mode & kFlagSwapRedBlue) != 0u) {
    c = c.bgra;
  }
  return c;
}
#endif

#endif  // MCLA_GPU_RESOLVE_COMMON_HLSLI

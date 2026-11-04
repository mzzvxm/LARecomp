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
//
// RESOLVE_FUSED (a depth source at a whole resolution scale s, resolve_memory.h
// FusedWrite): the pass also writes the resolve memory's dwords of the same
// resolve, the bytes resolve_memory_cs.hlsl would write from the EDRAM planes.
// The encode reads, for the guest texel (tx, ty), the EDRAM sample
// floor((t + 1/2) s) in each axis (t = the texel + the resolve's offset, in
// guest texels), that is t s + s/2. This pass copies the sample at
// (product pixel + offset) into the product pixel, with the offset a whole
// number of guest texels, -dx s: so the invocation at the product pixel
// (tx s + s/2, ty s + s/2) of the texel's block loaded exactly the sample the
// encode reads. That invocation, and only that one, writes the dword it has just
// built, to the address the encode
// would use (resolve_memory.hlsli, shared). s = 1: every pixel.
//
// RESOLVE_FUSED=2 is the same at a fractional scale (n/20 host pixels per guest texel, n not a
// multiple of 20; resolve.cpp DepthSampleWritten checks that the pass wrote every pixel the
// encode would read). There the encode reads, for the guest texel g, the EDRAM sample
// max(floor((2 u + 1) n / 40), fill), u = g + the resolve's offset in guest texels, and the pass
// copied the sample max(pixel + offset, fill) into each product pixel. So the invocation that
// loaded a texel's sample is the one that holds its depth and stencil, wherever in the texel's
// block it sits. It stores for every texel of the rectangle whose sample it loaded: none, one,
// or two per axis (at a scale below 1, or just above it where `fill` shares a sample).
// Samples below `fill` are nobody's; `fill` itself is the sample of every texel whose own is at
// or below it.

#include "host_pass_common.hlsli"
#include "xenos_formats.hlsli"

#ifndef RESOLVE_FUSED
#define RESOLVE_FUSED 0
#endif

struct ResolvePush {
  uint source;   // bindless slot: the colour or depth plane view
  uint stencil;  // bindless slot: the stencil plane view (depth sources)
  int offset_x;  // source pixel = target pixel + offset
  int offset_y;
  uint samples;  // bits 0-3 count, bits 8 + 3i the host index of sample i
  float scale;   // 2^exp_bias (x32 for the 16-bit EDRAM formats) / count
  uint mode;
  uint fill;     // host pixels of a scaled source's half-pixel gap (0: none)
#if RESOLVE_FUSED
  // The resolve memory's write (resolve_memory_cs.hlsl EncodePush, same meaning).
  uint address;  // the destination level's storage origin (physical)
  uint pitch;    // tiled: row pitch in blocks; linear: in bytes
  uint layout;   // bits 0-1 bpb_log2, bit 2 tiled, bits 3-4 endian
  uint tail;     // the level's place in a packed tail: x bits 0-15, y bits 16-31
  uint rect0;    // the rectangle in level texels: x0 bits 0-15, y0 bits 16-31
  uint rect1;    //                                 x1, y1 (exclusive)
  uint whole;    // host pixels per guest texel (the whole resolution scale s)
#if RESOLVE_FUSED == 2
  int guest_x;   // the resolve's offset in guest texels: source texel = texel + offset
  int guest_y;
  uint scale_n;  // the resolution scale in twentieths (Scale::n)
#endif
#endif
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ResolvePush> g_push;
#elif RESOLVE_FUSED
ConstantBuffer<ResolvePush> g_push : register(b0, space5);
#else
ConstantBuffer<ResolvePush> g_push : register(b0, space4);
#endif

#include "resolve_common.hlsli"

#if RESOLVE_FUSED
#if !RESOLVE_DEPTH
#error RESOLVE_FUSED is for a depth source
#endif
RWByteAddressBuffer g_memory : register(u0, space4);
ByteAddressBuffer g_memory_table : register(t1, space4);

#define RESOLVE_MEMORY_WRITE
#include "resolve_memory.hlsli"
#endif

#if RESOLVE_FUSED == 2
// The guest texels of one axis, the range [x, y] (empty when x > y), whose encode sample is
// `raw` (the pixel plus the pass's offset): those with u = g + offset and
// max(floor((2 u + 1) n / 40), fill) = max(raw, fill), inside the rectangle [rect0, rect1).
int2 OwnedTexels(int raw, int fill, int n, int offset, int rect0, int rect1) {
  if (raw < fill) {
    return int2(1, 0);
  }
  // The smallest u with (2 u + 1) n >= 40 raw, and the largest with (2 u + 1) n <= 40 raw + 39.
  // The sample `fill` is also that of every texel below it, so it starts at the first texel.
  const int first = raw == fill ? 0 : (40 * raw + n - 1) / (2 * n);
  const int last_numerator = 40 * raw + 39 - n;
  const int last = last_numerator < 0 ? -1 : last_numerator / (2 * n);
  return int2(max(first - offset, rect0), min(last - offset, rect1 - 1));
}
#endif

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
#if RESOLVE_FUSED == 2
  {
    const int2 raw = int2(position.xy) + int2(g_push.offset_x, g_push.offset_y);
    const int fill = int(g_push.fill);
    const int n = int(g_push.scale_n);
    const int2 r0 = int2(int(g_push.rect0 & 0xFFFFu), int(g_push.rect0 >> 16u));
    const int2 r1 = int2(int(g_push.rect1 & 0xFFFFu), int(g_push.rect1 >> 16u));
    const int2 xs = OwnedTexels(raw.x, fill, n, g_push.guest_x, r0.x, r1.x);
    const int2 ys = OwnedTexels(raw.y, fill, n, g_push.guest_y, r0.y, r1.y);
    if (xs.x <= xs.y && ys.x <= ys.y) {
      const GuestLevel l =
          MakeGuestLevel(g_push.address, g_push.pitch, g_push.layout, g_push.tail);
      [loop] for (int y = ys.x; y <= ys.y; ++y) {
        [loop] for (int x = xs.x; x <= xs.y; ++x) {
          StoreDword(GuestBlockAddress(l, int2(x, y)), dword, l.endian);
        }
      }
    }
  }
#elif RESOLVE_FUSED
  {
    const uint s = g_push.whole;
    const uint2 pixel = uint2(position.xy);
    uint2 texel = pixel;
    bool centre = true;
    [branch] if (s != 1u) {
      texel = pixel / s;
      centre = all((pixel - texel * s) == uint2(s >> 1u, s >> 1u));
    }
    const int2 r0 = int2(int(g_push.rect0 & 0xFFFFu), int(g_push.rect0 >> 16u));
    const int2 r1 = int2(int(g_push.rect1 & 0xFFFFu), int(g_push.rect1 >> 16u));
    if (centre && all(int2(texel) >= r0) && all(int2(texel) < r1)) {
      const GuestLevel l =
          MakeGuestLevel(g_push.address, g_push.pitch, g_push.layout, g_push.tail);
      StoreDword(GuestBlockAddress(l, int2(texel)), dword, l.endian);
    }
  }
#endif
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

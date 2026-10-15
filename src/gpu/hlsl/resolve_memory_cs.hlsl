// MCLA GPU renderer -- a resolve's bytes in the resolve memory.
//
// The console's resolve writes guest memory; this writes the same bytes into
// the resolve memory, right after the conversion pass made the product
// (resolve_ps.hlsl). One thread per guest dword of the destination rectangle:
// in the Xenos layouts 4 blocks of 8 bpp, 2 of 16 bpp or 1 of 32 bpp share a
// dword, and a 64 bpp block takes two (the low bits of a tiled offset come
// from x & 7, texture_util::GetTiledOffset2D). The bits are the inverse of the
// texture load (texture_decode.cpp): X in the low bits, the destination's
// endian swap last. Blocks of a dword outside the rectangle keep the memory's
// bytes (the CPU uploaded that page first when it was not valid).
//
// Colour comes from the product the conversion pass just wrote, which holds
// exactly what the destination's fetch reads (the float16 rounding included).
// Depth comes from the EDRAM target, as resolve_ps.hlsl reads it: the raw
// depth24 << 8 | stencil the console writes, which an R32F product cannot
// keep.
//
// Variants (compile-time): RESOLVE_DEPTH (an EDRAM depth/stencil source),
// RESOLVE_MSAA (that source multisampled).

#include "host_pass_common.hlsli"
#include "xenos_formats.hlsli"

struct EncodePush {
  uint source;    // depth: the EDRAM depth plane's slot; colour: the product's (every level, RGBA)
  uint stencil;   // depth: the EDRAM stencil plane's slot
  int offset_x;   // depth: EDRAM pixel = destination texel + offset
  int offset_y;
  uint samples;   // depth: the resolve's sample select, as resolve_ps.hlsl takes it
  float scale;    // unused here (resolve_common.hlsli's colour path)
  uint mode;      // the resolve's conversion (bits 0-7) and flags
  uint level;     // colour: the product level read
  uint address;   // the destination level's storage origin (physical)
  uint pitch;     // tiled: row pitch in blocks; linear: in bytes
  uint layout;    // bits 0-1 bpb_log2, bit 2 tiled, bits 3-4 endian
  uint tail;      // the level's place in a packed tail: x bits 0-15, y bits 16-31
  uint rect0;     // the rectangle in level texels: x0 bits 0-15, y0 bits 16-31
  uint rect1;     //                                 x1, y1 (exclusive)
  int group_x0;   // level x of the first dword group of the dispatch (storage-aligned)
  uint reserved;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<EncodePush> g_push;
#else
ConstantBuffer<EncodePush> g_push : register(b0, space5);
#endif

#include "resolve_common.hlsli"

RWByteAddressBuffer g_memory : register(u0, space4);
ByteAddressBuffer g_memory_table : register(t1, space4);

#include "resolve_memory.hlsli"

void StoreDword(uint address, uint value, uint endian) {
  const uint offset = ResolveMemoryOffset(address);
  if (offset != kResolveMemoryNoChunk) {
    g_memory.Store(offset, XeEndianSwap32(value, endian));
  }
}

#if !RESOLVE_DEPTH
uint4 QuantizeUNorm(float4 value, float4 max_value) {
  return uint4(floor(saturate(value) * max_value + 0.5f));
}
#endif

// The guest bits of one destination texel, before the endian swap: 1, 2 or 4
// bytes in the low bits of .x, or 8 bytes in .xy.
uint2 TexelBits(int2 texel) {
#if RESOLVE_DEPTH
  // Every depth destination (k_24_8, k_24_8_FLOAT, k_8_8_8_8, k_32_FLOAT) gets
  // the raw dword.
  uint stencil;
  const uint d24 = ResolveDepth24(texel + int2(g_push.offset_x, g_push.offset_y), stencil);
  return uint2((d24 << 8u) | stencil, 0u);
#else
  const float4 v = g_textures[g_push.source].Load(int3(texel, int(g_push.level)));
  switch (g_push.mode & 0xFFu) {
    case kModeUnorm8888: {
      const uint4 q = QuantizeUNorm(v, float4(255.0f, 255.0f, 255.0f, 255.0f));
      return uint2(q.r | (q.g << 8u) | (q.b << 16u) | (q.a << 24u), 0u);
    }
    case kModeUnorm1555: {
      // The product's B5G5R5A1 view reads X in .r (format.h: the load swaps
      // the 5-bit X and Z fields into the host layout).
      const uint4 q = QuantizeUNorm(v, float4(31.0f, 31.0f, 31.0f, 1.0f));
      return uint2(q.r | (q.g << 5u) | (q.b << 10u) | (q.a << 15u), 0u);
    }
    case kModeUnorm8:
      return uint2(QuantizeUNorm(v.rrrr, float4(255.0f, 255.0f, 255.0f, 255.0f)).r, 0u);
    case kModeRed32:
      return uint2(asuint(v.r), 0u);
    default: {  // kModeFloat: the product's float16 values, X and Y, Z and W.
      const uint4 h = f32tof16(v);
      return uint2(h.r | (h.g << 16u), h.b | (h.a << 16u));
    }
  }
#endif
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const GuestLevel l = MakeGuestLevel(g_push.address, g_push.pitch, g_push.layout, g_push.tail);
  const int2 r0 = int2(int(g_push.rect0 & 0xFFFFu), int(g_push.rect0 >> 16u));
  const int2 r1 = int2(int(g_push.rect1 & 0xFFFFu), int(g_push.rect1 >> 16u));
  const uint group = l.bpb_log2 >= 2u ? 1u : (4u >> l.bpb_log2);
  const int x = g_push.group_x0 + int(id.x * group);
  const int y = r0.y + int(id.y);
  if (x >= r1.x || y >= r1.y) {
    return;
  }
  const uint address = GuestBlockAddress(l, int2(x, y));
  if (l.bpb_log2 == 3u) {
    const uint2 bits = TexelBits(int2(x, y));
    StoreDword(address, bits.x, l.endian);
    StoreDword(address + 4u, bits.y, l.endian);
    return;
  }
  if (l.bpb_log2 == 2u) {
    StoreDword(address, TexelBits(int2(x, y)).x, l.endian);
    return;
  }
  // 8 or 16 bpp: the dword's blocks, in address order, merged into what the
  // memory holds where they fall outside the rectangle.
  const uint dword_address = address & ~3u;
  const uint block_bits = 8u << l.bpb_log2;
  const uint block_mask = (1u << block_bits) - 1u;
  uint value = XeEndianSwap32(ResolveMemoryLoad(dword_address), l.endian);
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    if (i >= group) {
      break;
    }
    const int bx = x + int(i);
    if (bx < r0.x || bx >= r1.x) {
      continue;
    }
    const uint shift = (address - dword_address + (i << l.bpb_log2)) * 8u;
    value = (value & ~(block_mask << shift)) | ((TexelBits(int2(bx, y)).x & block_mask) << shift);
  }
  StoreDword(dword_address, value, l.endian);
}

// MCLA GPU renderer -- a texture read from the resolve memory.
//
// A fetch that reads memory a resolve wrote with another shape (another
// format, size, tiling or base) gets what the console's memory holds there:
// one level of its texture, drawn over the level, each pixel loading its block
// from the resolve memory in the fetch's own layout and decoding it as the CPU
// load does (texture_decode.cpp: the endian swap, then the format.h load), so
// the host target stores exactly what a texture decoded from guest memory
// would hold.

#include "xenos_formats.hlsli"

struct DecodePush {
  uint address;  // the level's storage origin (physical)
  uint pitch;    // tiled: row pitch in blocks; linear: in bytes
  uint layout;   // bits 0-1 bpb_log2, bit 2 tiled, bits 3-4 endian, bits 8-11 the kind below
  uint tail;     // the level's place in a packed tail: x bits 0-15, y bits 16-31
  uint reserved[4];
};

// Host formats and loads (format.h TextureHost).
#define kDecodeR8 0u              // k_8: R8_UNORM
#define kDecodeB5G5R5A1 1u        // k_1_5_5_5: X and Z fields swapped into B5G5R5A1
#define kDecodeR8G8B8A8 2u        // k_8_8_8_8
#define kDecodeR16G16B16A16F 3u   // 64 bpp float16 (EXPAND, FLOAT)
#define kDecodeR32F 4u            // k_32_FLOAT
#define kDecodeDepthUnorm24 5u    // k_24_8: UNorm24To32(v >> 8)
#define kDecodeDepthFloat20e4 6u  // k_24_8_FLOAT: Float20e4To32(v >> 8)
#define kDecodeR32G32F 7u         // k_32_32_FLOAT: X and Y in two dwords

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<DecodePush> g_push;
#else
ConstantBuffer<DecodePush> g_push : register(b0, space1);
#endif

ByteAddressBuffer g_memory : register(t0, space0);
ByteAddressBuffer g_memory_table : register(t1, space0);

#include "resolve_memory.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const GuestLevel l =
      MakeGuestLevel(g_push.address, g_push.pitch, g_push.layout & 0x1Fu, g_push.tail);
  const uint kind = (g_push.layout >> 8u) & 0xFu;
  const uint address = GuestBlockAddress(l, int2(position.xy));
  // The dword holding the block, with the fetch's endian swap undone (a block
  // smaller than the swap unit is cut out of its swapped unit, as the GPU
  // reads memory in aligned units).
  const uint dword = XeEndianSwap32(ResolveMemoryLoad(address), l.endian);
  const uint shift = (address & 3u) * 8u;
  switch (kind) {
    case kDecodeR8:
      return float4(float((dword >> shift) & 0xFFu) * (1.0f / 255.0f), 0.0f, 0.0f, 0.0f);
    case kDecodeB5G5R5A1: {
      const uint v = (dword >> shift) & 0xFFFFu;
      return float4(float(v & 31u) * (1.0f / 31.0f), float((v >> 5u) & 31u) * (1.0f / 31.0f),
                    float((v >> 10u) & 31u) * (1.0f / 31.0f), float(v >> 15u));
    }
    case kDecodeR8G8B8A8:
      return float4(float(dword & 0xFFu), float((dword >> 8u) & 0xFFu),
                    float((dword >> 16u) & 0xFFu), float(dword >> 24u)) *
             (1.0f / 255.0f);
    case kDecodeR16G16B16A16F: {
      const uint high = XeEndianSwap32(ResolveMemoryLoad(address + 4u), l.endian);
      return float4(f16tof32(dword & 0xFFFFu), f16tof32(dword >> 16u), f16tof32(high & 0xFFFFu),
                    f16tof32(high >> 16u));
    }
    case kDecodeR32F:
      return float4(asfloat(dword), 0.0f, 0.0f, 0.0f);
    case kDecodeR32G32F: {
      const uint high = XeEndianSwap32(ResolveMemoryLoad(address + 4u), l.endian);
      return float4(asfloat(dword), asfloat(high), 0.0f, 0.0f);
    }
    case kDecodeDepthUnorm24:
      return float4(XeUNorm24To32(dword >> 8u), 0.0f, 0.0f, 0.0f);
    default:  // kDecodeDepthFloat20e4
      return float4(XeFloat20e4To32(dword >> 8u), 0.0f, 0.0f, 0.0f);
  }
}

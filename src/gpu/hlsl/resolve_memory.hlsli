// MCLA GPU renderer -- guest memory as the resolve memory keeps it
//: guest physical
// memory in 64 KiB chunks, each placed in a pool buffer by a table of 8192
// entries (the chunk's place in the pool, or kResolveMemoryNoChunk).
//
// Addressing from Xenia (BSD-3-Clause, Copyright Ben Vanik and the Xenia
// authors): the 2D tiled offset is the ReXGlue SDK's
// texture_util::GetTiledOffset2D (Xenia's XeTextureTiledOffset2D), the endian
// swap Xenia's XeEndianSwap32.
//
// The including shader declares the pool, g_memory, as a ByteAddressBuffer or
// RWByteAddressBuffer, and the table, g_memory_table, as a ByteAddressBuffer.

#ifndef MCLA_GPU_RESOLVE_MEMORY_HLSLI
#define MCLA_GPU_RESOLVE_MEMORY_HLSLI

#define kResolveMemoryNoChunk 0xFFFFFFFFu

// xenos::Endian: kNone 0, k8in16 1, k8in32 2, k16in32 3. Its own inverse.
uint XeEndianSwap32(uint value, uint endian) {
  if (endian == 1u || endian == 2u) {
    value = ((value & 0x00FF00FFu) << 8u) | ((value & 0xFF00FF00u) >> 8u);
  }
  if (endian == 2u || endian == 3u) {
    value = (value << 16u) | (value >> 16u);
  }
  return value;
}

// The byte offset of block (x, y) of a tiled 2D surface with rows of `pitch`
// blocks (texture_util::GetTiledOffset2D).
int XeTiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  const int macro_tile = ((x >> 5) + (y >> 5) * int(pitch >> 5)) << (bpb_log2 + 7u);
  const int micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  const int offset = macro_tile + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// One level of a guest texture in memory (textures.h TextureLevelStorage).
struct GuestLevel {
  uint address;   // physical address of the level's storage origin
  uint pitch;     // tiled: row pitch in blocks; linear: row pitch in bytes
  uint bpb_log2;  // bytes per block: 1, 2, 4 or 8
  bool tiled;
  uint endian;
  int2 tail;      // the level's place inside a packed mip tail, in blocks
};

// `layout`: bits 0-1 bpb_log2, bit 2 tiled, bits 3-4 endian; `tail`: x in bits
// 0-15, y in bits 16-31.
GuestLevel MakeGuestLevel(uint address, uint pitch, uint layout, uint tail) {
  GuestLevel l;
  l.address = address;
  l.pitch = pitch;
  l.bpb_log2 = layout & 3u;
  l.tiled = (layout & 4u) != 0u;
  l.endian = (layout >> 3u) & 3u;
  l.tail = int2(int(tail & 0xFFFFu), int(tail >> 16u));
  return l;
}

// The guest physical address of block `block` of the level.
uint GuestBlockAddress(GuestLevel l, int2 block) {
  const int2 b = block + l.tail;
  const int offset = l.tiled ? XeTiledOffset2D(b.x, b.y, l.pitch, l.bpb_log2)
                             : b.y * int(l.pitch) + (b.x << l.bpb_log2);
  return l.address + uint(offset);
}

// The pool offset of guest physical byte `address`, or kResolveMemoryNoChunk
// when its chunk has no place (the CPU places every chunk a pass touches
// before recording it).
uint ResolveMemoryOffset(uint address) {
  const uint slot = g_memory_table.Load((address >> 16u) << 2u);
  return slot == kResolveMemoryNoChunk ? kResolveMemoryNoChunk
                                       : ((slot << 16u) | (address & 0xFFFFu));
}

uint ResolveMemoryLoad(uint address) {
  const uint offset = ResolveMemoryOffset(address & ~3u);
  return offset == kResolveMemoryNoChunk ? 0u : g_memory.Load(offset);
}

#endif  // MCLA_GPU_RESOLVE_MEMORY_HLSLI

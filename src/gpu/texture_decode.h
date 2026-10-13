#pragma once
// ===========================================================================
// MCLA GPU renderer -- guest texture levels decoded on the CPU
// ===========================================================================
// One stored level of a guest
// texture -- or one level inside a packed mip tail -- read from guest memory
// and written as host rows: blocks untiled with the SDK's
// texture_util::GetTiledOffset2D (the function Xenia's texture load shaders
// address with, texture_address.xesli), the fetch constant's endian swap
// undone per block, then the format's load (format.h TextureLoad):
// k_1_5_5_5 to B5G5R5A1, depth to one float. Block-compressed levels of a
// texture whose base is not 4x4-aligned are decompressed to RGBA8 with Xenia's
// DXT arithmetic (pixel_formats.xesli: XeDXTColorEndpointsToBGR8In10, the
// weights and the integer divisions by 3, 5 and 7; Copyright Ben Vanik and the
// Xenia authors, BSD-3-Clause), so a decompressed texel equals what the
// emulated path loads.
//
// Where a level lives is the caller's: the SDK's GetGuestTextureLayout gives
// its storage origin, row pitch (tile-aligned) and extent, GetPackedMipOffset
// its block offset inside a packed tail. Nothing here reads beyond `readable`
// bytes from the origin: a block that would is zero-filled and reported, so a
// bad fetch constant cannot fault the process.
//
// Threading: pure functions.
// ===========================================================================

#include <cstdint>

#include "format.h"

namespace mcla::gpu::texture_decode {

// One stored level (or packed tail) of a guest texture in host memory.
struct Source {
  const uint8_t* data = nullptr;  // the storage origin
  uint64_t readable = 0;          // bytes that may be read from `data`
  uint32_t row_pitch_bytes = 0;   // the layout's row pitch, tile-aligned for tiled storage
  uint32_t bytes_per_block = 0;   // guest block size: 1, 2, 4, 8 or 16
  uint32_t offset_x = 0;          // blocks: the level's place inside a packed tail
  uint32_t offset_y = 0;
  bool tiled = false;
  uint32_t endian = 0;            // xenos::Endian of the fetch constant
};

// Byte offset from the storage origin of the block (x, y) of the level
// (offsets not yet applied).
uint32_t BlockOffset(const Source& source, uint32_t x, uint32_t y);

// One past the highest byte any block [0, width) x [0, height) of the level
// touches. Exact for tiled storage, by visiting every block: no shortcut over
// the swizzle is exact (the legacy runtime measured 1728 wrong of 28875
// extents for "the last macro-tile row", memory project_native_gfx_tiled_reach).
uint64_t Reach(const Source& source, uint32_t width_blocks, uint32_t height_blocks);

struct Request {
  Source source;
  format::TextureLoad load = format::TextureLoad::kCopy;
  // kNone: one host block per guest block, a row per block row. Otherwise
  // RGBA8 texels, a row per texel row.
  format::TextureDecompress decompress = format::TextureDecompress::kNone;
  uint32_t width_blocks = 0;   // of this level
  uint32_t height_blocks = 0;
  uint32_t width = 0;          // texels, used when decompressing
  uint32_t height = 0;
};

// Bytes of one host row of the request's output: width_blocks * block (the
// depth loads keep 4 bytes), or width * 4 when decompressing.
uint32_t HostRowBytes(const Request& request);
// Rows of the output: height_blocks, or height when decompressing.
uint32_t HostRows(const Request& request);

// Writes the level into `out`, rows `out_pitch` bytes apart. False when a
// block lay beyond `readable` (it was written as zeros).
bool Decode(const Request& request, uint8_t* out, uint32_t out_pitch);

}  // namespace mcla::gpu::texture_decode

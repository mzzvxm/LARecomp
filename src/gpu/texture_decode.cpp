// MCLA GPU renderer -- guest texture levels decoded on the CPU. See
// texture_decode.h.
//
// The DXT decompression is Xenia's arithmetic (src/xenia/gpu/shaders/
// pixel_formats.xesli: XeDXTColorEndpointsToBGR8In10, XeDXTHighColorWeights,
// XeDXTOpaqueRowToRGB8, XeDXT1TransRowToRGBA8, XeDXT58StepRowToA8,
// XeDXT56StepRowToA8, the 4-bit DXT3 alpha replication), written per texel.
// Copyright 2022 Ben Vanik and the Xenia authors, BSD-3-Clause.

#include "texture_decode.h"

#include <algorithm>
#include <cstring>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace mcla::gpu::texture_decode {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

namespace {

uint32_t Log2(uint32_t bytes_per_block) {
  uint32_t log2 = 0;
  while ((1u << (log2 + 1)) <= bytes_per_block) {
    ++log2;
  }
  return log2;
}

uint32_t SwapUnit(uint32_t endian) {
  switch (xe::Endian(endian)) {
    case xe::Endian::k8in16: return 2;
    case xe::Endian::k8in32:
    case xe::Endian::k16in32: return 4;
    default: return 1;
  }
}

// Undoes the fetch's endian swap over `length` bytes (a multiple of the unit).
void Swap(uint8_t* data, uint32_t length, uint32_t endian) {
  switch (xe::Endian(endian)) {
    case xe::Endian::k8in16:
      for (uint32_t i = 0; i + 1 < length; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xe::Endian::k8in32:
      for (uint32_t i = 0; i + 3 < length; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xe::Endian::k16in32:
      for (uint32_t i = 0; i + 3 < length; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

uint16_t LoadLe16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t LoadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Reads the block at `offset` into `out` (bytes_per_block bytes) with the
// endian swap undone. A block smaller than the swap unit is cut out of its
// swapped unit, as the GPU sees memory in aligned units. False (zeros) when
// any byte needed lies beyond `readable`.
bool ReadBlock(const Source& s, uint32_t offset, uint8_t* out) {
  const uint32_t bpb = s.bytes_per_block;
  const uint32_t unit = SwapUnit(s.endian);
  if (bpb >= unit) {
    if (uint64_t(offset) + bpb > s.readable) {
      std::memset(out, 0, bpb);
      return false;
    }
    std::memcpy(out, s.data + offset, bpb);
    Swap(out, bpb, s.endian);
    return true;
  }
  const uint32_t aligned = offset & ~(unit - 1);
  if (uint64_t(aligned) + unit > s.readable) {
    std::memset(out, 0, bpb);
    return false;
  }
  uint8_t word[4];
  std::memcpy(word, s.data + aligned, unit);
  Swap(word, unit, s.endian);
  std::memcpy(out, word + (offset - aligned), bpb);
  return true;
}

// --- DXT (Xenia's arithmetic, see the top of the file) ----------------------

// One endpoint of a 5:6:5 color as 8-bit components with bit replication.
struct Rgb8 {
  uint32_t r, g, b;
};
Rgb8 Endpoint(uint16_t c) {
  const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
  return Rgb8{(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)};
}

// 16 RGBA8 texels (row-major, R in the low byte) of a DXT color block (8
// bytes, PC layout after the endian swap). `punchthrough`: the DXT1 rule that
// color0 <= color1 selects three colors and transparent black; DXT3/DXT5 color
// blocks are always four-color.
void DecodeColorBlock(const uint8_t* block, bool punchthrough, uint32_t texels[16]) {
  const uint16_t c0 = LoadLe16(block);
  const uint16_t c1 = LoadLe16(block + 2);
  const uint32_t codes = LoadLe32(block + 4);
  const Rgb8 e0 = Endpoint(c0), e1 = Endpoint(c1);
  const bool three_color = punchthrough && c0 <= c1;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t code = (codes >> (2 * i)) & 3;
    uint32_t r, g, b, a = 0xFF;
    if (!three_color) {
      // Weights of (color0, color1): 00 = 3:0, 01 = 0:3, 10 = 2:1, 11 = 1:2,
      // integer division by 3 (XeDXTOpaqueRowToRGB8).
      static constexpr uint32_t kW0[4] = {3, 0, 2, 1};
      const uint32_t w0 = kW0[code], w1 = 3 - w0;
      r = (w0 * e0.r + w1 * e1.r) / 3;
      g = (w0 * e0.g + w1 * e1.g) / 3;
      b = (w0 * e0.b + w1 * e1.b) / 3;
    } else {
      // 00 = color0, 01 = color1, 10 = (color0 + color1) >> 1, 11 = transparent
      // black (XeDXT1TransRowToRGBA8).
      switch (code) {
        case 0: r = e0.r; g = e0.g; b = e0.b; break;
        case 1: r = e1.r; g = e1.g; b = e1.b; break;
        case 2:
          r = (e0.r + e1.r) >> 1;
          g = (e0.g + e1.g) >> 1;
          b = (e0.b + e1.b) >> 1;
          break;
        default: r = g = b = 0; a = 0; break;
      }
    }
    texels[i] = r | g << 8 | b << 16 | a << 24;
  }
}

// DXT3 explicit alpha: 4 bits per texel, replicated to 8 (a4 * 17).
void ApplyExplicitAlpha(const uint8_t* block, uint32_t texels[16]) {
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t a4 = (block[i >> 1] >> ((i & 1) * 4)) & 0xF;
    texels[i] = (texels[i] & 0x00FFFFFFu) | ((a4 * 17u) << 24);
  }
}

// DXT5 interpolated alpha: alpha0 > alpha1 interpolates 8 steps by integer
// division by 7; otherwise 6 steps by 5, with 110 = 0 and 111 = 255
// (XeDXT58StepRowToA8, XeDXT56StepRowToA8).
void ApplyInterpolatedAlpha(const uint8_t* block, uint32_t texels[16]) {
  const uint32_t a0 = block[0], a1 = block[1];
  uint64_t codes = 0;
  for (uint32_t i = 0; i < 6; ++i) {
    codes |= uint64_t(block[2 + i]) << (8 * i);
  }
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t code = uint32_t(codes >> (3 * i)) & 7;
    uint32_t a;
    if (code == 0) {
      a = a0;
    } else if (code == 1) {
      a = a1;
    } else if (a0 > a1) {
      const uint32_t w1 = code - 1, w0 = 7 - w1;
      a = (w0 * a0 + w1 * a1) / 7;
    } else if (code < 6) {
      const uint32_t w1 = code - 1, w0 = 5 - w1;
      a = (w0 * a0 + w1 * a1) / 5;
    } else {
      a = code == 6 ? 0 : 255;
    }
    texels[i] = (texels[i] & 0x00FFFFFFu) | (a << 24);
  }
}

}  // namespace

uint32_t BlockOffset(const Source& s, uint32_t x, uint32_t y) {
  if (s.tiled) {
    const uint32_t pitch_blocks = s.row_pitch_bytes / s.bytes_per_block;
    return uint32_t(tu::GetTiledOffset2D(int32_t(x), int32_t(y), pitch_blocks,
                                         Log2(s.bytes_per_block)));
  }
  return y * s.row_pitch_bytes + x * s.bytes_per_block;
}

uint64_t Reach(const Source& s, uint32_t width_blocks, uint32_t height_blocks) {
  if (width_blocks == 0 || height_blocks == 0 || s.bytes_per_block == 0) {
    return 0;
  }
  const uint32_t unit = std::max(SwapUnit(s.endian), s.bytes_per_block);
  uint64_t reach = 0;
  if (!s.tiled) {
    const uint32_t last = BlockOffset(s, s.offset_x + width_blocks - 1, s.offset_y + height_blocks - 1);
    return ((uint64_t(last) & ~uint64_t(unit - 1)) + unit);
  }
  for (uint32_t y = 0; y < height_blocks; ++y) {
    for (uint32_t x = 0; x < width_blocks; ++x) {
      const uint64_t end =
          (uint64_t(BlockOffset(s, s.offset_x + x, s.offset_y + y)) & ~uint64_t(unit - 1)) + unit;
      reach = std::max(reach, end);
    }
  }
  return reach;
}

uint32_t HostRowBytes(const Request& r) {
  if (r.decompress != format::TextureDecompress::kNone) {
    return r.width * 4;
  }
  return r.width_blocks * r.source.bytes_per_block;
}

uint32_t HostRows(const Request& r) {
  return r.decompress != format::TextureDecompress::kNone ? r.height : r.height_blocks;
}

bool Decode(const Request& r, uint8_t* out, uint32_t out_pitch) {
  const Source& s = r.source;
  const uint32_t bpb = s.bytes_per_block;
  bool complete = true;
  uint8_t block[16];
  if (r.decompress == format::TextureDecompress::kNone) {
    for (uint32_t y = 0; y < r.height_blocks; ++y) {
      uint8_t* row = out + uint64_t(y) * out_pitch;
      for (uint32_t x = 0; x < r.width_blocks; ++x) {
        const uint32_t offset = BlockOffset(s, s.offset_x + x, s.offset_y + y);
        complete &= ReadBlock(s, offset, block);
        uint8_t* dst = row + x * bpb;
        switch (r.load) {
          case format::TextureLoad::kCopy:
            std::memcpy(dst, block, bpb);
            break;
          case format::TextureLoad::kR5G5B5A1ToB5G5R5A1: {
            const uint16_t v = LoadLe16(block);
            const uint16_t swapped =
                uint16_t((v & 0x83E0u) | ((v & 0x1Fu) << 10) | ((v >> 10) & 0x1Fu));
            std::memcpy(dst, &swapped, 2);
            break;
          }
          case format::TextureLoad::kDepthUnorm24: {
            const float f = xe::UNorm24To32(LoadLe32(block) >> 8);
            std::memcpy(dst, &f, 4);
            break;
          }
          case format::TextureLoad::kDepthFloat20e4: {
            const float f = xe::Float20e4To32(LoadLe32(block) >> 8);
            std::memcpy(dst, &f, 4);
            break;
          }
        }
      }
    }
    return complete;
  }
  // Decompression: 4x4 texels per block, cut at the level's edge.
  uint32_t texels[16];
  for (uint32_t by = 0; by < r.height_blocks; ++by) {
    for (uint32_t bx = 0; bx < r.width_blocks; ++bx) {
      const uint32_t offset = BlockOffset(s, s.offset_x + bx, s.offset_y + by);
      complete &= ReadBlock(s, offset, block);
      switch (r.decompress) {
        case format::TextureDecompress::kDXT1:
          DecodeColorBlock(block, /*punchthrough=*/true, texels);
          break;
        case format::TextureDecompress::kDXT2_3:
          DecodeColorBlock(block + 8, false, texels);
          ApplyExplicitAlpha(block, texels);
          break;
        case format::TextureDecompress::kDXT4_5:
          DecodeColorBlock(block + 8, false, texels);
          ApplyInterpolatedAlpha(block, texels);
          break;
        case format::TextureDecompress::kNone:
          break;
      }
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t y = by * 4 + ty;
        if (y >= r.height) {
          break;
        }
        uint8_t* row = out + uint64_t(y) * out_pitch;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t x = bx * 4 + tx;
          if (x >= r.width) {
            break;
          }
          std::memcpy(row + x * 4, &texels[ty * 4 + tx], 4);
        }
      }
    }
  }
  return complete;
}

}  // namespace mcla::gpu::texture_decode

#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — texture fetch decoding.
// See texture_format.h for the telemetry that bounds the supported set.

#include "texture_format.h"

#include <cstring>

#include <dxgiformat.h>

namespace mcla::native_gfx {

TextureFetch DecodeTextureFetch(const uint32_t d[6]) {
  TextureFetch t;
  t.type_valid = (d[0] & 0x3u) == 2u;
  t.pitch = ((d[0] >> 22) & 0x1FFu) << 5;
  t.tiled = ((d[0] >> 31) & 0x1u) != 0;

  t.format = d[1] & 0x3Fu;
  t.endianness = (d[1] >> 6) & 0x3u;
  t.base_address = ((d[1] >> 12) & 0xFFFFFu) << 12;

  // Size is stored with 1 subtracted from each component; MCLA is 100% 2D.
  t.width = (d[2] & 0x1FFFu) + 1;
  t.height = ((d[2] >> 13) & 0x1FFFu) + 1;

  t.swizzle = (d[3] >> 1) & 0xFFFu;

  t.dimension = (d[5] >> 9) & 0x3u;
  t.mip_address = ((d[5] >> 12) & 0xFFFFFu) << 12;
  return t;
}

uint32_t TextureFormatToDxgi(uint32_t xenos_format) {
  switch (GuestTextureFormat(xenos_format)) {
    case GuestTextureFormat::k_8:
      return DXGI_FORMAT_R8_UNORM;
    case GuestTextureFormat::k_1_5_5_5:
      return DXGI_FORMAT_B5G5R5A1_UNORM;
    case GuestTextureFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GuestTextureFormat::k_DXT1:
      return DXGI_FORMAT_BC1_UNORM;
    case GuestTextureFormat::k_DXT2_3:
      return DXGI_FORMAT_BC2_UNORM;
    case GuestTextureFormat::k_DXT4_5:
      return DXGI_FORMAT_BC3_UNORM;
    case GuestTextureFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case GuestTextureFormat::k_16_16_16_16_EXPAND:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case GuestTextureFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    // Depth formats: sampled as a single float channel. Reached only through
    // the render-target path (IsRenderTargetSourcedFormat).
    case GuestTextureFormat::k_24_8:
    case GuestTextureFormat::k_24_8_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

FormatInfo GetFormatInfo(uint32_t xenos_format) {
  FormatInfo fi;
  switch (GuestTextureFormat(xenos_format)) {
    case GuestTextureFormat::k_8:
      fi.bytes_per_block = 1;
      break;
    case GuestTextureFormat::k_1_5_5_5:
      fi.bytes_per_block = 2;
      break;
    case GuestTextureFormat::k_8_8_8_8:
    case GuestTextureFormat::k_24_8:
    case GuestTextureFormat::k_24_8_FLOAT:
    case GuestTextureFormat::k_32_FLOAT:
      fi.bytes_per_block = 4;
      break;
    case GuestTextureFormat::k_16_16_16_16_FLOAT:
    case GuestTextureFormat::k_16_16_16_16_EXPAND:
      fi.bytes_per_block = 8;
      break;
    case GuestTextureFormat::k_DXT1:
      fi.block_width = fi.block_height = 4;
      fi.bytes_per_block = 8;
      fi.is_compressed = true;
      break;
    case GuestTextureFormat::k_DXT2_3:
    case GuestTextureFormat::k_DXT4_5:
      fi.block_width = fi.block_height = 4;
      fi.bytes_per_block = 16;
      fi.is_compressed = true;
      break;
    default:
      fi.bytes_per_block = 0;  // unsupported — caller must fail loudly
      break;
  }
  return fi;
}

void SwapTextureData(uint32_t endianness, uint8_t* data, uint64_t size_bytes) {
  if (!data) {
    return;
  }
  switch (TextureEndian(endianness)) {
    case TextureEndian::k8in16: {
      const uint64_t whole = size_bytes & ~1ull;
      for (uint64_t i = 0; i < whole; i += 2) {
        const uint8_t t = data[i];
        data[i] = data[i + 1];
        data[i + 1] = t;
      }
      break;
    }
    case TextureEndian::k8in32: {
      const uint64_t whole = size_bytes & ~3ull;
      for (uint64_t i = 0; i < whole; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(data + i, &v, 4);
      }
      break;
    }
    case TextureEndian::k16in32: {
      // Exchange the two halves of each dword without swapping their bytes.
      const uint64_t whole = size_bytes & ~3ull;
      for (uint64_t i = 0; i < whole; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(data + i, &v, 4);
      }
      break;
    }
    case TextureEndian::kNone:
    default:
      break;
  }
}

bool IsRenderTargetSourcedFormat(uint32_t xenos_format) {
  switch (GuestTextureFormat(xenos_format)) {
    case GuestTextureFormat::k_24_8:
    case GuestTextureFormat::k_24_8_FLOAT:
    case GuestTextureFormat::k_32_FLOAT:
      return true;
    default:
      return false;
  }
}

namespace {

// Xenos 2D address swizzle. Ported from the SDK's texture conversion path
// (src/graphics/pipeline/texture/conversion.cpp), which is the reference
// implementation for this layout.
inline uint32_t Log2Bpp(uint32_t bytes_per_block) {
  return (bytes_per_block / 4) + ((bytes_per_block / 2) >> (bytes_per_block / 4));
}

inline uint32_t TiledOffset2DRow(uint32_t y, uint32_t width, uint32_t log2_bpp) {
  const uint32_t macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  const uint32_t micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xFu) + ((y & 8) << (3 + log2_bpp)) +
         ((y & 1) << 4);
}

inline uint32_t TiledOffset2DColumn(uint32_t x, uint32_t y, uint32_t log2_bpp,
                                    uint32_t base_offset) {
  const uint32_t macro = (x / 32) << (log2_bpp + 7);
  const uint32_t micro = (x & 7) << log2_bpp;
  const uint32_t offset = base_offset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xFu));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0u) << 2) + (offset & 0x3Fu) +
         ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

}  // namespace

uint64_t TiledSurfaceSizeBytes(uint32_t width_blocks, uint32_t height_blocks,
                               uint32_t bytes_per_block) {
  // Naive padded_w * padded_h * bpb is NOT an upper bound on the addresses
  // the swizzle produces. The formula's `(y & 16) << 7` term assumes each
  // 16-row band spans 2048 bytes, i.e. a row pitch of at least 128 bytes;
  // when the real pitch is smaller (narrow surfaces, 1- and 2-byte formats)
  // the addresses spread past the naive size. Measured: a 32x32 k_8 surface
  // reaches byte 2560 while the naive size is 1024.
  //
  // Bound verified by brute force over 9035 (width, height, bpp)
  // combinations covering every format and resolution the game binds: zero
  // under-estimates, 1.13x median over-estimate. It is a read clamp, not an
  // allocation size, so erring high is free.
  const uint64_t pitch = AlignToTile(width_blocks) * uint64_t(bytes_per_block);
  const uint64_t effective_pitch = pitch < 128 ? 128 : pitch;
  const uint32_t rows = AlignToTile(height_blocks);
  return effective_pitch * (rows < 32 ? 32 : rows) + 2048;
}

uint32_t TiledOffset2D(uint32_t x_block, uint32_t y_block, uint32_t width_blocks,
                       uint32_t bytes_per_block) {
  const uint32_t log2_bpp = Log2Bpp(bytes_per_block);
  const uint32_t pitch = AlignToTile(width_blocks);
  const uint32_t row = TiledOffset2DRow(y_block, pitch, log2_bpp);
  const uint32_t off = TiledOffset2DColumn(x_block, y_block, log2_bpp, row) >> log2_bpp;
  return off * bytes_per_block;
}

bool UntileSurface2D(uint8_t* dst, uint32_t dst_pitch_bytes, const uint8_t* src,
                     uint64_t src_size_bytes, uint32_t width_blocks, uint32_t height_blocks,
                     uint32_t bytes_per_block) {
  if (!dst || !src || bytes_per_block == 0) {
    return false;
  }
  const uint32_t log2_bpp = Log2Bpp(bytes_per_block);
  // Macro tiles are 32x32 blocks; the swizzle needs the PADDED pitch, and
  // the surface itself is padded in both axes.
  const uint32_t pitch_blocks = AlignToTile(width_blocks);
  bool complete = true;
  for (uint32_t y = 0; y < height_blocks; ++y) {
    const uint32_t row = TiledOffset2DRow(y, pitch_blocks, log2_bpp);
    uint8_t* out = dst + size_t(y) * dst_pitch_bytes;
    for (uint32_t x = 0; x < width_blocks; ++x) {
      const uint32_t in = TiledOffset2DColumn(x, y, log2_bpp, row) >> log2_bpp;
      const uint64_t byte_off = uint64_t(in) * bytes_per_block;
      if (byte_off + bytes_per_block > src_size_bytes) {
        complete = false;  // malformed fetch constant — never read past the end
        out += bytes_per_block;
        continue;
      }
      const uint8_t* p = src + byte_off;
      for (uint32_t b = 0; b < bytes_per_block; ++b) {
        out[b] = p[b];
      }
      out += bytes_per_block;
    }
  }
  return complete;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

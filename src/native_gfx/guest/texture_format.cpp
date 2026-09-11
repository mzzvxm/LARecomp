#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — texture fetch decoding.
// See texture_format.h for the telemetry that bounds the supported set.
//
// The Xenos data-format layer (tiled address swizzle, block geometry, endian
// swap) comes from the SDK: those translation units are compiled into rexcore,
// which lives inside rexruntime, so they link without the rexgpu-xenos plugin
// and without any part of the command processor.

#include <cstdio>
#include "texture_format.h"

#include <cstring>

#include <dxgiformat.h>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>

namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

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

namespace {

// Is this one of the formats MCLA has actually been observed to bind? The gate
// stays hand-written even though the block geometry below now comes from the
// SDK: rg::FormatInfo::Get answers for every Xenos format there is, so without
// it an unsupported format would quietly decode as garbage instead of failing.
bool IsSupportedFormat(uint32_t xenos_format) {
  switch (GuestTextureFormat(xenos_format)) {
    case GuestTextureFormat::k_8:
    case GuestTextureFormat::k_1_5_5_5:
    case GuestTextureFormat::k_8_8_8_8:
    case GuestTextureFormat::k_DXT1:
    case GuestTextureFormat::k_DXT2_3:
    case GuestTextureFormat::k_DXT4_5:
    case GuestTextureFormat::k_24_8:
    case GuestTextureFormat::k_24_8_FLOAT:
    case GuestTextureFormat::k_16_16_16_16_EXPAND:
    case GuestTextureFormat::k_32_FLOAT:
    case GuestTextureFormat::k_16_16_16_16_FLOAT:
      return true;
    default:
      return false;
  }
}

// log2 of the block size, which is what the SDK's tiled address functions
// take. Every supported format has a power-of-two block size.
inline uint32_t BppLog2(uint32_t bytes_per_block) {
  return rex::log2_floor(bytes_per_block);
}

}  // namespace

FormatInfo GetFormatInfo(uint32_t xenos_format) {
  FormatInfo fi;
  if (!IsSupportedFormat(xenos_format)) {
    fi.bytes_per_block = 0;  // unsupported — caller must fail loudly
    return fi;
  }
  // rg::FormatInfo (pipeline/texture/info.h) carries the whole Xenos format
  // table: block geometry and bits per pixel for every format.
  const rg::FormatInfo* info = rg::FormatInfo::Get(xenos_format);
  if (!info) {
    fi.bytes_per_block = 0;
    return fi;
  }
  fi.block_width = info->block_width;
  fi.block_height = info->block_height;
  fi.bytes_per_block = info->bytes_per_block();
  fi.is_compressed = info->type == rg::FormatType::kCompressed;
  return fi;
}

void SwapTextureData(uint32_t endianness, uint8_t* data, uint64_t size_bytes) {
  if (!data || size_bytes == 0) {
    return;
  }
  // CopySwapBlock is safe in place: every backend swaps element by element at
  // the same index, and the SIMD paths load and store the same lane.
  //
  // k16in32 is the exception and is deliberately NOT routed there.
  // CopySwapBlock passes the byte length straight through to
  // copy_and_swap_16_in_32_unaligned, which consumes 4 bytes per count
  // (core/memory.cpp), so it would run four times past the end of the buffer;
  // the k8in16 and k8in32 cases divide the length correctly. MCLA has never
  // been observed to bind a k16in32 texture (see the telemetry in
  // texture_format.h), so this is a guard, not a hot path.
  switch (TextureEndian(endianness)) {
    case TextureEndian::k8in16:
      tc::CopySwapBlock(xe::Endian::k8in16, data, data, size_t(size_bytes));
      break;
    case TextureEndian::k8in32:
      tc::CopySwapBlock(xe::Endian::k8in32, data, data, size_t(size_bytes));
      break;
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
  //
  // Deliberately NOT tu::GetTextureTotalSize. That returns the size the guest
  // allocated for the texture, which is a different quantity from how far the
  // address swizzle can reach, and substituting it here would under-clamp the
  // very reads this exists to bound.
  const uint64_t pitch = AlignToTile(width_blocks) * uint64_t(bytes_per_block);
  const uint64_t effective_pitch = pitch < 128 ? 128 : pitch;
  const uint32_t rows = AlignToTile(height_blocks);
  return effective_pitch * (rows < 32 ? 32 : rows) + 2048;
}

uint32_t TiledOffset2D(uint32_t x_block, uint32_t y_block, uint32_t width_blocks,
                       uint32_t bytes_per_block) {
  // tu::GetTiledOffset2D aligns the pitch to a 32-block tile internally and
  // returns a byte offset. Verified equivalent to the open-coded swizzle this
  // used to carry, over 339840 (bpb, width, x, y) combinations spanning every
  // format and surface width MCLA binds.
  return uint32_t(tu::GetTiledOffset2D(int32_t(x_block), int32_t(y_block), width_blocks,
                                       BppLog2(bytes_per_block)));
}

bool UntileSurface2D(uint8_t* dst, uint32_t dst_pitch_bytes, const uint8_t* src,
                     uint64_t src_size_bytes, uint32_t width_blocks, uint32_t height_blocks,
                     uint32_t bytes_per_block) {
  if (!dst || !src || bytes_per_block == 0) {
    return false;
  }
  // Macro tiles are 32x32 blocks, so the swizzle works on a padded pitch and
  // the surface is padded in both axes.
  //
  // TiledSurfaceSizeBytes is an upper bound on every address the swizzle can
  // produce for this geometry. When the caller hands over at least that much
  // readable source, no offset can land outside it, so the whole per-block
  // bound check is provably dead and tc::Untile runs unguarded. That is the
  // path every call from the texture cache takes: it passes exactly this
  // value, having already proven the range readable.
  //
  // tc::Untile also hoists the row term out of the inner loop and only walks
  // the column per block. Recomputing the full address per block instead costs
  // an align and a multiply on every one of them, which on a 1024x1024 DXT5
  // is 65536 times over.
  if (src_size_bytes >= TiledSurfaceSizeBytes(width_blocks, height_blocks, bytes_per_block) &&
      dst_pitch_bytes % bytes_per_block == 0) {
    // Untile reads nothing from the format infos but the block size, so a
    // synthetic 1x1-block descriptor carrying the right stride gives the exact
    // same addresses without plumbing the Xenos format down here. Untile's
    // pitches are in blocks, and it does not align the input pitch itself.
    rg::FormatInfo block_desc{};
    block_desc.block_width = 1;
    block_desc.block_height = 1;
    block_desc.bits_per_pixel = bytes_per_block * 8;

    tc::UntileInfo untile{};
    untile.offset_x = 0;
    untile.offset_y = 0;
    untile.width = width_blocks;
    untile.height = height_blocks;
    untile.input_pitch = AlignToTile(width_blocks);
    untile.output_pitch = dst_pitch_bytes / bytes_per_block;
    untile.input_format_info = &block_desc;
    untile.output_format_info = &block_desc;
    untile.copy_callback = [](void* out, const void* in, size_t length) {
      std::memcpy(out, in, length);
    };
    tc::Untile(dst, src, &untile);
    return true;
  }

  // Short source: a malformed fetch constant, or a caller that knows less than
  // the geometry implies. Gather block by block and skip anything that would
  // read past the end rather than faulting the process.
  const uint32_t bpb_log2 = BppLog2(bytes_per_block);
  bool complete = true;
  for (uint32_t y = 0; y < height_blocks; ++y) {
    uint8_t* out = dst + size_t(y) * dst_pitch_bytes;
    for (uint32_t x = 0; x < width_blocks; ++x) {
      const int32_t off = tu::GetTiledOffset2D(int32_t(x), int32_t(y), width_blocks, bpb_log2);
      if (off < 0 || uint64_t(uint32_t(off)) + bytes_per_block > src_size_bytes) {
        complete = false;  // never read past the end
        out += bytes_per_block;
        continue;
      }
      std::memcpy(out, src + uint32_t(off), bytes_per_block);
      out += bytes_per_block;
    }
  }
  return complete;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

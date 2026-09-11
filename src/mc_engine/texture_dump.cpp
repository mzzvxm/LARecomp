// Guest texture dumping, ported from the ReXGlue SDK's
// src/graphics/pipeline/texture/replacement.cpp so larecomp builds against the
// stock SDK, which has no rex/graphics/pipeline/texture/replacement.h.
//
// Only the dump half is here. Replacement injection stays in the SDK's
// TextureCache; nothing on the game side drives it. The file naming
// (dump/<hash16>_<w>x<h>_<fmt>.dds) and the write-once behaviour are kept
// byte-for-byte because the vinyl extractor keys off them.

#include "mc_engine/texture_dump.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>

#include "mc_engine/logging.h"

REXCVAR_DEFINE_STRING(larecomp_texture_folder, "textures", "Graphics",
                      "Folder for dumped and replacement textures; only consulted when "
                      "the SDK does not register texture_folder itself");

namespace mcla {

using rex::graphics::FormatInfo;
using rex::graphics::FormatType;
namespace texture_conversion = rex::graphics::texture_conversion;

// Resolve the dump folder the same way whichever SDK is underneath: the fork
// registers texture_folder, the stock one does not, so fall back to our own.
std::filesystem::path TextureDumpDir() {
  std::string folder = rex::cvar::GetFlagByName("texture_folder");
  if (folder.empty()) {
    folder = rex::cvar::GetFlagByName("larecomp_texture_folder");
  }
  if (folder.empty()) {
    folder = "textures";
  }
  return std::filesystem::path(folder) / "dump";
}

namespace {

// ---------------------------------------------------------------------------
// DDS constants and structures
// ---------------------------------------------------------------------------

static constexpr uint32_t kDdsMagic = 0x20534444u;  // "DDS "
static constexpr uint32_t kDdsdCaps = 0x00000001u;
static constexpr uint32_t kDdsdHeight = 0x00000002u;
static constexpr uint32_t kDdsdWidth = 0x00000004u;
static constexpr uint32_t kDdsdPitch = 0x00000008u;
static constexpr uint32_t kDdsdLinearSize = 0x00080000u;
static constexpr uint32_t kDdsdPixelFormat = 0x00001000u;
static constexpr uint32_t kDdsPfRgb = 0x00000040u;
static constexpr uint32_t kDdsPfAlphaPixels = 0x00000001u;
static constexpr uint32_t kDdsPfFourCC = 0x00000004u;
static constexpr uint32_t kDdsCapsTexture = 0x00001000u;

static constexpr uint32_t kFourCC_DXT1 = 0x31545844u;  // "DXT1"
static constexpr uint32_t kFourCC_DXT3 = 0x33545844u;  // "DXT3"
static constexpr uint32_t kFourCC_DXT5 = 0x35545844u;  // "DXT5"
static constexpr uint32_t kFourCC_DX10 = 0x30315844u;  // "DX10"
static constexpr uint32_t kFourCC_ATI1 = 0x31495441u;  // "ATI1" (BC4 / DXN red)
static constexpr uint32_t kFourCC_ATI2 = 0x32495441u;  // "ATI2" (BC5 / DXN rg)

static constexpr uint32_t kDxgiFormatR8G8B8A8UNorm = 28;
static constexpr uint32_t kDxgiFormatR8G8B8A8UNormSRGB = 29;
static constexpr uint32_t kDxgiFormatBC1UNorm = 71;
static constexpr uint32_t kDxgiFormatBC1UNormSRGB = 72;
static constexpr uint32_t kDxgiFormatBC2UNorm = 74;
static constexpr uint32_t kDxgiFormatBC2UNormSRGB = 75;
static constexpr uint32_t kDxgiFormatBC3UNorm = 77;
static constexpr uint32_t kDxgiFormatBC3UNormSRGB = 78;
static constexpr uint32_t kDxgiFormatB8G8R8A8UNorm = 87;
static constexpr uint32_t kDxgiFormatB8G8R8X8UNorm = 88;
static constexpr uint32_t kDxgiFormatB8G8R8A8UNormSRGB = 91;
static constexpr uint32_t kDxgiFormatB8G8R8X8UNormSRGB = 93;

#pragma pack(push, 1)
struct DdsPixelFormat {
  uint32_t size = 32;
  uint32_t flags = 0;
  uint32_t four_cc = 0;
  uint32_t rgb_bit_count = 0;
  uint32_t r_bit_mask = 0;
  uint32_t g_bit_mask = 0;
  uint32_t b_bit_mask = 0;
  uint32_t a_bit_mask = 0;
};
struct DdsHeader {
  uint32_t magic = kDdsMagic;
  uint32_t size = 124;
  uint32_t flags = 0;
  uint32_t height = 0;
  uint32_t width = 0;
  uint32_t pitch_or_linear = 0;
  uint32_t depth = 0;
  uint32_t mip_map_count = 1;
  uint32_t reserved1[11] = {};
  DdsPixelFormat ddspf;
  uint32_t caps = kDdsCapsTexture;
  uint32_t caps2 = 0;
  uint32_t caps3 = 0;
  uint32_t caps4 = 0;
  uint32_t reserved2 = 0;
};
struct DdsHeaderDX10 {
  uint32_t dxgi_format = 0;
  uint32_t resource_dimension = 0;
  uint32_t misc_flag = 0;
  uint32_t array_size = 0;
  uint32_t misc_flags2 = 0;
};
#pragma pack(pop)
static_assert(sizeof(DdsHeader) == 128);
static_assert(sizeof(DdsHeaderDX10) == 20);

// ---------------------------------------------------------------------------
// Internal helpers: tiled address decode
// ---------------------------------------------------------------------------
// Untile a 2-D block-based texture into a linear output buffer.
//   src              : tiled guest bytes
//   dst              : output buffer (row-major, no padding)
//   width_blocks     : visible width in blocks
//   height_blocks    : visible height in blocks
//   pitch_blocks     : row pitch in blocks (aligned to 32 for tiled)
//   fi               : format geometry (block size drives the swizzle)
//
// This used to open-code the Xenos address swizzle, copied out of the SDK's
// conversion.cpp. It is the SDK's own function now: texture_conversion is
// compiled into rexcore, hence into rexruntime, so it links with no plugin and
// no command processor. Untile puts no bound on the offsets it computes, which
// is fine here and only here — the dump path is fed a resource whose extent
// came from the caller, not from a guest-controlled fetch constant.
static void UntileBlocks(const uint8_t* src, uint8_t* dst, uint32_t width_blocks,
                         uint32_t height_blocks, uint32_t pitch_blocks, const FormatInfo* fi) {
  texture_conversion::UntileInfo info{};
  info.offset_x = 0;
  info.offset_y = 0;
  info.width = width_blocks;
  info.height = height_blocks;
  info.input_pitch = pitch_blocks;   // in blocks
  info.output_pitch = width_blocks;  // in blocks, no padding
  info.input_format_info = fi;
  info.output_format_info = fi;
  // The endian swap runs as one pass over the staging buffer afterwards, so
  // the per-block copy here is a plain move.
  info.copy_callback = [](void* out, const void* in, size_t length) {
    std::memcpy(out, in, length);
  };
  texture_conversion::Untile(dst, src, &info);
}

// ---------------------------------------------------------------------------
// RGBA8 expansion helpers
// ---------------------------------------------------------------------------
// Each returns an RGBA8 pixel from a pointer into the (already endian-swapped)
// source data.

static uint32_t Expand5To8(uint32_t v) {
  return (v << 3) | (v >> 2);
}
static uint32_t Expand6To8(uint32_t v) {
  return (v << 2) | (v >> 4);
}

// Convert one texel from the given format (already endian-corrected) to RGBA8.
// Returns false for formats that need the BC path (compressed blocks).
static bool TexelToRGBA8(const uint8_t* src, xenos::TextureFormat fmt, uint8_t out[4]) {
  using F = xenos::TextureFormat;
  switch (fmt) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_GAMMA_EDRAM:
      out[0] = src[0];
      out[1] = src[1];
      out[2] = src[2];
      out[3] = src[3];
      return true;
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      out[0] = out[1] = out[2] = src[0];
      out[3] = 255;
      return true;
    case F::k_8_8:
      out[0] = src[0];
      out[1] = src[1];
      out[2] = 0;
      out[3] = 255;
      return true;
    case F::k_5_6_5: {
      uint16_t v;
      std::memcpy(&v, src, 2);
      out[0] = static_cast<uint8_t>(Expand5To8((v >> 11) & 0x1F));
      out[1] = static_cast<uint8_t>(Expand6To8((v >> 5) & 0x3F));
      out[2] = static_cast<uint8_t>(Expand5To8(v & 0x1F));
      out[3] = 255;
      return true;
    }
    case F::k_1_5_5_5: {
      uint16_t v;
      std::memcpy(&v, src, 2);
      out[0] = static_cast<uint8_t>(Expand5To8((v >> 10) & 0x1F));
      out[1] = static_cast<uint8_t>(Expand5To8((v >> 5) & 0x1F));
      out[2] = static_cast<uint8_t>(Expand5To8(v & 0x1F));
      out[3] = static_cast<uint8_t>(((v >> 15) & 1) ? 255 : 0);
      return true;
    }
    case F::k_4_4_4_4: {
      uint16_t v;
      std::memcpy(&v, src, 2);
      out[0] = static_cast<uint8_t>(((v >> 12) & 0xF) * 17);
      out[1] = static_cast<uint8_t>(((v >> 8) & 0xF) * 17);
      out[2] = static_cast<uint8_t>(((v >> 4) & 0xF) * 17);
      out[3] = static_cast<uint8_t>((v & 0xF) * 17);
      return true;
    }
    case F::k_2_10_10_10: {
      uint32_t v;
      std::memcpy(&v, src, 4);
      out[0] = static_cast<uint8_t>((v >> 22) & 0xFF);
      out[1] = static_cast<uint8_t>((v >> 12) & 0xFF);
      out[2] = static_cast<uint8_t>((v >> 2) & 0xFF);
      out[3] = static_cast<uint8_t>(((v & 3) * 85));
      return true;
    }
    default:
      // Unsupported / compressed — caller should use BC path or skip
      out[0] = out[1] = out[2] = out[3] = 0;
      return false;
  }
}

// ---------------------------------------------------------------------------
// DDS file writers
// ---------------------------------------------------------------------------

static bool WriteDDS_RGBA8(const std::filesystem::path& path, uint32_t width,
                             uint32_t height, const uint8_t* rgba8_rows,
                             uint32_t row_pitch_bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open())
    return false;

  DdsHeader hdr;
  hdr.flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat | kDdsdPitch;
  hdr.height = height;
  hdr.width = width;
  hdr.pitch_or_linear = width * 4;
  hdr.ddspf.flags = kDdsPfRgb | kDdsPfAlphaPixels;
  hdr.ddspf.rgb_bit_count = 32;
  hdr.ddspf.r_bit_mask = 0x000000FFu;
  hdr.ddspf.g_bit_mask = 0x0000FF00u;
  hdr.ddspf.b_bit_mask = 0x00FF0000u;
  hdr.ddspf.a_bit_mask = 0xFF000000u;

  f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

  const uint32_t row_bytes = width * 4;
  for (uint32_t y = 0; y < height; ++y) {
    f.write(reinterpret_cast<const char*>(rgba8_rows + y * row_pitch_bytes), row_bytes);
  }
  return f.good();
}

static bool WriteDDS_BC(const std::filesystem::path& path, uint32_t width,
                                     uint32_t height, const uint8_t* bc_blocks,
                                     uint32_t bytes_per_block, uint32_t fourcc) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open())
    return false;

  const uint32_t w_blocks = (width + 3) / 4;
  const uint32_t h_blocks = (height + 3) / 4;
  const uint32_t linear_size = w_blocks * h_blocks * bytes_per_block;

  DdsHeader hdr;
  hdr.flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat | kDdsdLinearSize;
  hdr.height = height;
  hdr.width = width;
  hdr.pitch_or_linear = linear_size;
  hdr.ddspf.flags = kDdsPfFourCC;
  hdr.ddspf.four_cc = fourcc;

  f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  f.write(reinterpret_cast<const char*>(bc_blocks), linear_size);
  return f.good();
}

}  // namespace

// expanded to RGBA8. Everything else — depth (k_24_8*), and HDR/float render-
// target formats (k_*_FLOAT) — are transient GPU buffers, not source art; the
// old code wrote their raw bytes as "RGBA8" and produced garbage images. Skip
// them so the dump folder only contains textures worth replacing.
static bool IsDumpableFormat(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  const FormatInfo* fi = FormatInfo::Get(format);
  if (!fi)
    return false;
  if (fi->type == FormatType::kCompressed)
    return true;
  switch (format) {
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
    case F::k_8_8:
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_GAMMA_EDRAM:
    case F::k_5_6_5:
    case F::k_1_5_5_5:
    case F::k_4_4_4_4:
    case F::k_2_10_10_10:
      return true;
    default:
      return false;
  }
}

void DumpGuestTexture(const std::filesystem::path& dump_dir, uint64_t content_hash,
                      uint32_t width, uint32_t height, uint32_t pitch_blocks, bool tiled,
                      xenos::TextureFormat format, xenos::Endian endianness,
                      const uint8_t* guest_bytes, uint32_t guest_size) {
  using F = xenos::TextureFormat;

  const FormatInfo* fi = FormatInfo::Get(format);
  if (!fi)
    return;

  // Skip depth/float/other GPU-internal formats — not authorable source art.
  if (!IsDumpableFormat(format))
    return;

  // Build filename: <hash16>_<w>x<h>_<format_name>.dds
  char name[128];
  std::snprintf(name, sizeof(name), "%016llx_%ux%u_%s.dds",
                static_cast<unsigned long long>(content_hash), width, height, fi->name);
  const auto dest = dump_dir / name;

  // Only write once per unique texture to avoid hammering the disk.
  if (std::filesystem::exists(dest))
    return;

  const uint32_t bpb = fi->bytes_per_block();
  const uint32_t w_blocks = (width + fi->block_width - 1) / fi->block_width;
  const uint32_t h_blocks = (height + fi->block_height - 1) / fi->block_height;
  // pitch_blocks is in units of 32 texels, convert to block units
  const uint32_t pitch_b32 = pitch_blocks * 32;  // pitch in texels
  const uint32_t pitch_blk = (pitch_b32 + fi->block_width - 1) / fi->block_width;

  // Step 1 — allocate a linear staging buffer and untile (or copy linear)
  const uint32_t linear_bytes = w_blocks * h_blocks * bpb;
  std::vector<uint8_t> linear(linear_bytes);

  if (tiled) {
    UntileBlocks(guest_bytes, linear.data(), w_blocks, h_blocks, pitch_blk, fi);
  } else {
    // Linear: rows are already in order but may have pitch padding — copy
    // only the visible region.
    const uint32_t src_row_bytes = pitch_blk * bpb;
    const uint32_t dst_row_bytes = w_blocks * bpb;
    for (uint32_t y = 0; y < h_blocks; ++y) {
      const uint32_t src_off = y * src_row_bytes;
      if (src_off + dst_row_bytes > guest_size)
        break;
      std::memcpy(linear.data() + y * dst_row_bytes, guest_bytes + src_off, dst_row_bytes);
    }
  }

  // Step 2 — endian-swap the staging buffer in-place using CopySwapBlock
  if (endianness != xenos::Endian::kNone) {
    texture_conversion::CopySwapBlock(endianness, linear.data(), linear.data(), linear_bytes);
  }

  // Step 3 — write to DDS
  if (fi->type == FormatType::kCompressed) {
    // Determine DDS FOURCC
    uint32_t fourcc = 0;
    switch (format) {
      case F::k_DXT1:
      case F::k_DXT1_AS_16_16_16_16:
        fourcc = kFourCC_DXT1;
        break;
      case F::k_DXT2_3:
      case F::k_DXT2_3_AS_16_16_16_16:
        fourcc = kFourCC_DXT3;
        break;
      case F::k_DXT4_5:
      case F::k_DXT4_5_AS_16_16_16_16:
        fourcc = kFourCC_DXT5;
        break;
      case F::k_DXN:
        fourcc = kFourCC_ATI2;
        break;
      case F::k_DXT5A:
        fourcc = kFourCC_ATI1;
        break;
      // DXT3A variants: treat as DXT3 (same block layout)
      case F::k_DXT3A:
      case F::k_DXT3A_AS_1_1_1_1:
        fourcc = kFourCC_DXT3;
        break;
      default:
        fourcc = kFourCC_DXT5;
        break;
    }

    if (!WriteDDS_BC(dest, width, height, linear.data(), bpb, fourcc)) {
      MC_WARN("texture dump: failed to write BC dump {}", dest.string());
    } else {
      MC_DEBUG("texture dump: dumped BC  {}", dest.filename().string());
    }
  } else {
    // Uncompressed — expand each texel to RGBA8
    const uint32_t out_row_bytes = w_blocks * 4;  // w_blocks == width for uncompressed
    std::vector<uint8_t> rgba(static_cast<size_t>(w_blocks) * h_blocks * 4);

    bool ok = true;
    for (uint32_t y = 0; y < h_blocks && ok; ++y) {
      for (uint32_t x = 0; x < w_blocks; ++x) {
        const uint8_t* src = linear.data() + (y * w_blocks + x) * bpb;
        uint8_t* dst = rgba.data() + y * out_row_bytes + x * 4;
        if (!TexelToRGBA8(src, format, dst)) {
          // Unsupported format — write raw bytes zero-padded to RGBA8 as
          // a best-effort so at least something useful shows up.
          dst[0] = bpb > 0 ? src[0] : 0;
          dst[1] = bpb > 1 ? src[1] : 0;
          dst[2] = bpb > 2 ? src[2] : 0;
          dst[3] = bpb > 3 ? src[3] : 255;
        }
      }
    }

    if (!WriteDDS_RGBA8(dest, width, height, rgba.data(), out_row_bytes)) {
      MC_WARN("texture dump: failed to write RGBA8 dump {}", dest.string());
    } else {
      MC_DEBUG("texture dump: dumped RGBA8 {}", dest.filename().string());
    }
  }
}

}  // namespace mcla

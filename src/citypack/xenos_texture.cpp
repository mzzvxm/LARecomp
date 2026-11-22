#include "citypack/xenos_texture.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace larecomp::citypack::xenos {

namespace {

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint32_t Log2Floor(uint32_t v) { return v == 0 ? 0 : 31u - uint32_t(std::countl_zero(v)); }
uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32u - uint32_t(std::countl_zero(v - 1)); }
uint32_t NextPow2(uint32_t v) { return v <= 1 ? 1u : (1u << Log2Ceil(v)); }

}  // namespace

FormatInfo GetFormatInfo(uint32_t format) {
  switch (format) {
    case kFormat8888: return {1, 1, 4, "8_8_8_8"};
    case kFormatDxt1: return {4, 4, 8, "DXT1"};
    case kFormatDxt23: return {4, 4, 16, "DXT2_3"};
    case kFormatDxt45: return {4, 4, 16, "DXT4_5"};
    default: return {};
  }
}

bool DescribeTexture(const uint32_t d[6], TextureDesc* out, std::string* why) {
  const auto fail = [&](const char* text) {
    if (why) *why = text;
    return false;
  };
  TextureDesc t;
  std::memcpy(t.dwords, d, sizeof(t.dwords));
  t.type = d[0] & 3u;
  if (t.type != 2) return fail("not a texture fetch constant");
  t.tiled = (d[0] >> 31) != 0;
  t.pitch_div32 = (d[0] >> 22) & 0x1FFu;
  t.gamma = ((d[0] >> 2) & 3u) == kSignGamma && ((d[0] >> 4) & 3u) == kSignGamma && ((d[0] >> 6) & 3u) == kSignGamma;
  t.format = d[1] & 63u;
  t.endian = (d[1] >> 6) & 3u;
  t.stacked = ((d[1] >> 10) & 1u) != 0;
  t.dimension = (d[5] >> 9) & 3u;
  t.packed_mips = ((d[5] >> 11) & 1u) != 0;
  t.swizzle = (d[3] >> 1) & 0xFFFu;
  if (t.dimension != 1) return fail("not a 2D texture (1D, 3D or cube)");
  if (t.stacked) return fail("a stacked texture");
  t.width = (d[2] & 0x1FFFu) + 1;
  t.height = ((d[2] >> 13) & 0x1FFFu) + 1;

  // GetSubresourcesFromFetchConstant: the mip range and the two addresses are cleaned up from what is really there.
  uint32_t base = d[1] & 0xFFFFF000u;
  uint32_t mip = d[5] & 0xFFFFF000u;
  const uint32_t size_mip_max_level = Log2Floor(std::max(t.width, t.height));
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;
  if (mip != 0) {
    mip_min_level = std::min((d[4] >> 2) & 15u, size_mip_max_level);
    mip_max_level = std::max(std::min((d[4] >> 6) & 15u, size_mip_max_level), mip_min_level);
  }
  if (mip_max_level != 0) {
    if (base == 0) mip_min_level = std::max(mip_min_level, 1u);
    if (mip_min_level != 0) base = 0;
  } else {
    mip = 0;
  }
  t.base_address = base;
  t.mip_address = mip;
  t.mip_min_level = mip_min_level;
  t.mip_max_level = mip_max_level;
  *out = t;
  return true;
}

// ---------------------------------------------------------------------------
// tiling
// ---------------------------------------------------------------------------

int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = Align(pitch, kTileBlocks);
  // Top bits of the coordinates.
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  // Lower bits of the coordinates (a 6 bit value).
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  // Mix micro and macro, add the few remaining x and y bits.
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  // Mix the bits again.
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

uint32_t GetTiledAddressUpperBound2D(uint32_t right, uint32_t bottom, uint32_t pitch, uint32_t bytes_per_block_log2) {
  if (right == 0 || bottom == 0) return 0;
  // The origin of the 32 x 32 tile that holds the last block.
  uint32_t upper = uint32_t(GetTiledOffset2D(int32_t((right - 1) & ~(kTileBlocks - 1)), int32_t((bottom - 1) & ~(kTileBlocks - 1)), pitch, bytes_per_block_log2));
  switch (bytes_per_block_log2) {
    case 0: upper += 0xA00; break;  // independent addressing within 128 x 128 portions, the extent 0xA00 from the tile's origin
    case 1: upper += 0xC00; break;  // within 64 x 64 portions, 0xC00
    default: upper += 0x400u << bytes_per_block_log2; break;
  }
  return upper;
}

uint32_t GetPackedLevel(uint32_t width, uint32_t height) {
  const uint32_t log2_size = Log2Ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

bool GetPackedMipOffset(uint32_t width, uint32_t height, uint32_t block_width, uint32_t block_height, uint32_t mip, uint32_t* x_blocks, uint32_t* y_blocks) {
  // Tile size is 32 x 32, and once a level's shorter side is 16 texels or less it is packed into one tile with the others (Xenia: "the math here is insane": graph paper,
  // dds dumps and executable reverse engineering). Logical sizes: the input is the whole texture's, not the level's.
  const uint32_t log2_width = Log2Ceil(width);
  const uint32_t log2_height = Log2Ceil(height);
  const uint32_t log2_size = std::min(log2_width, log2_height);
  uint32_t x = 0;
  uint32_t y = 0;
  if (log2_size > 4 + mip) {
    *x_blocks = 0;
    *y_blocks = 0;
    return false;
  }
  const uint32_t packed_mip_base = log2_size > 4 ? log2_size - 4 : 0;
  const uint32_t packed_mip = mip - packed_mip_base;
  if (packed_mip < 3) {
    if (log2_width > log2_height) {
      x = 0;  // wider than tall: laid out vertically
      y = 16u >> packed_mip;
    } else {
      x = 16u >> packed_mip;  // taller than wide (or square): laid out horizontally
      y = 0;
    }
  } else {
    uint32_t offset;
    if (log2_width > log2_height) {
      offset = (1u << (log2_width - packed_mip_base)) >> (packed_mip - 2);
      x = offset;
      y = 0;
    } else {
      x = 0;
      offset = (1u << (log2_height - packed_mip_base)) >> (packed_mip - 2);
      y = offset;
    }
  }
  *x_blocks = x / block_width;
  *y_blocks = y / block_height;
  return true;
}

// ---------------------------------------------------------------------------
// the layout
// ---------------------------------------------------------------------------

GuestLayout GetGuestLayout(const TextureDesc& desc) {
  GuestLayout layout;
  const FormatInfo info = GetFormatInfo(desc.format);
  if (!info.supported() || desc.width == 0 || desc.height == 0) return layout;
  const uint32_t bytes_per_block = info.block_bytes;
  const uint32_t bytes_per_block_log2 = Log2Floor(bytes_per_block);

  layout.max_level = std::min(desc.mip_max_level, Log2Floor(std::max(desc.width, desc.height)));
  layout.packed_level = desc.packed_mips ? GetPackedLevel(desc.width, desc.height) : 0xFFFFFFFFu;
  const uint32_t max_stored_level = std::min(layout.max_level, layout.packed_level);
  if (max_stored_level >= kMaxLevels) return layout;

  const bool has_base = desc.has_base();
  uint32_t loop_level_last;
  if (layout.packed_level == 0) {
    // The tail is level 0: loop 0 is the base's tail, loop 1 the other levels' tail (both are "level 0", stored apart).
    loop_level_last = layout.max_level != 0 ? 1u : 0u;
  } else {
    loop_level_last = max_stored_level;
  }
  uint32_t mip_offset_bytes = 0;
  for (uint32_t loop_level = has_base ? 0u : 1u; loop_level <= loop_level_last; ++loop_level) {
    const bool is_base = loop_level == 0;
    const uint32_t level = layout.packed_level == 0 ? 0u : loop_level;
    LevelLayout& out = is_base ? layout.base : layout.mips[level];

    // Strides: the base takes its row pitch from the fetch constant, the other levels from a texture whose base size is a power of two, every dimension aligned to 32 blocks.
    uint32_t row_pitch_texels;
    uint32_t slice_texel_rows;
    if (is_base) {
      row_pitch_texels = desc.pitch_div32 << 5;
      slice_texel_rows = desc.height;
    } else {
      row_pitch_texels = std::max(NextPow2(desc.width) >> level, 1u);
      slice_texel_rows = std::max(NextPow2(desc.height) >> level, 1u);
    }
    const uint32_t pitch_blocks = Align(Align(row_pitch_texels, info.block_width) / info.block_width, kTileBlocks);
    out.row_pitch_blocks = pitch_blocks;
    out.row_pitch_bytes = pitch_blocks * bytes_per_block;
    if (!desc.tiled && !is_base) out.row_pitch_bytes = Align(out.row_pitch_bytes, kLinearRowAlignment);
    out.y_stride_block_rows = Align(Align(slice_texel_rows, info.block_height) / info.block_height, kTileBlocks);
    out.slice_stride_bytes = Align(out.row_pitch_bytes * out.y_stride_block_rows, kSubresourceAlignment);

    if (level == layout.packed_level) {
      // The part of the tail the needed levels use.
      out.x_extent_blocks = 0;
      out.y_extent_blocks = 0;
      const uint32_t last_sublevel = is_base ? 0u : layout.max_level;
      for (uint32_t sub = layout.packed_level; sub <= last_sublevel; ++sub) {
        uint32_t px = 0;
        uint32_t py = 0;
        GetPackedMipOffset(desc.width, desc.height, info.block_width, info.block_height, sub, &px, &py);
        out.x_extent_blocks = std::max(out.x_extent_blocks, px + Align(std::max(desc.width >> sub, 1u), info.block_width) / info.block_width);
        out.y_extent_blocks = std::max(out.y_extent_blocks, py + Align(std::max(desc.height >> sub, 1u), info.block_height) / info.block_height);
      }
    } else {
      out.x_extent_blocks = Align(std::max(desc.width >> level, 1u), info.block_width) / info.block_width;
      out.y_extent_blocks = Align(std::max(desc.height >> level, 1u), info.block_height) / info.block_height;
    }
    if (desc.tiled) {
      out.data_extent_bytes = GetTiledAddressUpperBound2D(out.x_extent_blocks, out.y_extent_blocks, pitch_blocks, bytes_per_block_log2);
    } else {
      out.data_extent_bytes = out.row_pitch_bytes * (out.y_extent_blocks - 1) + bytes_per_block * out.x_extent_blocks;
    }
    if (!is_base) {
      layout.mip_offsets_bytes[level] = mip_offset_bytes;
      layout.mips_total_extent_bytes = std::max(layout.mips_total_extent_bytes, mip_offset_bytes + out.data_extent_bytes);
      mip_offset_bytes += out.slice_stride_bytes;
    }
  }
  layout.ok = true;
  return layout;
}

LevelPlan PlanLevel(const TextureDesc& desc, const GuestLayout& layout, uint32_t level) {
  LevelPlan plan;
  const FormatInfo info = GetFormatInfo(desc.format);
  if (!layout.ok || !info.supported() || level > layout.max_level) return plan;
  plan.level = level;
  plan.width = std::max(desc.width >> level, 1u);
  plan.height = std::max(desc.height >> level, 1u);
  plan.width_blocks = Align(plan.width, info.block_width) / info.block_width;
  plan.height_blocks = Align(plan.height, info.block_height) / info.block_height;

  const LevelLayout* where = nullptr;
  const bool in_tail = desc.packed_mips && layout.packed_level != 0xFFFFFFFFu && level >= layout.packed_level;
  if (in_tail) {
    if (!GetPackedMipOffset(desc.width, desc.height, info.block_width, info.block_height, level, &plan.tail_x, &plan.tail_y)) return plan;
    if (layout.packed_level == 0) {
      plan.in_base_region = level == 0;
      where = level == 0 ? &layout.base : &layout.mips[0];
      plan.offset_bytes = 0;
    } else {
      plan.in_base_region = false;
      where = &layout.mips[layout.packed_level];
      plan.offset_bytes = layout.mip_offsets_bytes[layout.packed_level];
    }
  } else if (level == 0) {
    plan.in_base_region = true;
    where = &layout.base;
    plan.offset_bytes = 0;
  } else {
    plan.in_base_region = false;
    where = &layout.mips[level];
    plan.offset_bytes = layout.mip_offsets_bytes[level];
  }
  plan.region_address = plan.in_base_region ? desc.base_address : desc.mip_address;
  if (plan.region_address == 0) return plan;
  plan.pitch_blocks = where->row_pitch_blocks;
  plan.row_pitch_bytes = where->row_pitch_bytes;
  plan.reach_bytes = uint64_t(plan.offset_bytes) + where->data_extent_bytes;
  plan.ok = true;
  return plan;
}

// ---------------------------------------------------------------------------
// decoding
// ---------------------------------------------------------------------------

void SwapEndian(uint32_t endian, uint8_t* data, size_t size) {
  switch (endian) {
    case kEndian8in16:
      for (size_t i = 0; i + 1 < size; i += 2) std::swap(data[i], data[i + 1]);
      break;
    case kEndian8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case kEndian16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default: break;
  }
}

bool DecodeLevel(const TextureDesc& desc, const LevelPlan& plan, const uint8_t* region, size_t region_size, std::vector<uint8_t>* out) {
  const FormatInfo info = GetFormatInfo(desc.format);
  out->assign(size_t(plan.width_blocks) * plan.height_blocks * info.block_bytes, 0);
  if (!plan.ok || !info.supported()) return false;
  const uint32_t bytes_per_block = info.block_bytes;
  const uint32_t log2 = Log2Floor(bytes_per_block);
  bool whole = true;
  for (uint32_t by = 0; by < plan.height_blocks; ++by) {
    uint8_t* row = out->data() + size_t(by) * plan.width_blocks * bytes_per_block;
    for (uint32_t bx = 0; bx < plan.width_blocks; ++bx) {
      uint64_t at;
      if (desc.tiled) {
        at = uint64_t(plan.offset_bytes) + uint32_t(GetTiledOffset2D(int32_t(bx + plan.tail_x), int32_t(by + plan.tail_y), plan.pitch_blocks, log2));
      } else {
        at = uint64_t(plan.offset_bytes) + uint64_t(by + plan.tail_y) * plan.row_pitch_bytes + uint64_t(bx + plan.tail_x) * bytes_per_block;
      }
      uint8_t* dst = row + size_t(bx) * bytes_per_block;
      if (at + bytes_per_block > region_size) {
        whole = false;
        continue;
      }
      std::memcpy(dst, region + at, bytes_per_block);
      SwapEndian(desc.endian, dst, bytes_per_block);
    }
  }
  return whole;
}

}  // namespace larecomp::citypack::xenos

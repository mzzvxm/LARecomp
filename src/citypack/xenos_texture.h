#pragma once
// ===========================================================================
// LARecomp city pack -- Xbox 360 (Xenos) texture storage: the fetch constant, the layout, the untiling
// ===========================================================================
// What the city's textures are stored as, read from the player's own files (CITY-RT M2 package H2) and checked on all of them: every texture of the city is a 2D texture, tiled,
// 8in16 swapped, DXT1 (18) or DXT4/5 (20); its fetch constant is the six dwords the game's grcTexture keeps at D3DBaseTexture + 0x1C (the resource holds them as the console does, the
// addresses being pointers into the resource itself: 0x5xxxxxxx virtual, 0x6xxxxxxx physical, here `base_address` and `mip_address` with the low 12 bits cleared).
//
// This file is the layout maths of Xenia's texture code (src/xenia/gpu/texture_util.cc and its LAR-RXSDK copy src/graphics/pipeline/texture/util.cpp: GetSubresourcesFromFetchConstant,
// GetPackedMipLevel, GetPackedMipOffset, GetGuestTextureLayout, GetTiledOffset2D, GetTiledAddressUpperBound2D), written for the 2D case alone and with no dependency on the SDK, so
// that the builder, the tool and the tests link it by itself. Copyright 2022 Ben Vanik and the Xenia authors, BSD-3-Clause (the SDK's copy is "Adapted for ReXGlue" by Tom Clay,
// 2026); the tiled address function comes from the UModel project (the SDK's own comment: gildor2/UModel, UnTexture.cpp). It is cross checked in the unit test against the
// renderer's other copy of the tiling (mc_engine/modloader/texture.cpp, a port of XGAddress2DTiled that the mod loader writes textures with) and, on the real files, by the
// agreement of every mip level's mean colour with the base level's.
//
// A texture's storage: the BASE level at `base_address`, the other levels at `mip_address`, each level on its own tile aligned image (the width and height of the base rounded up to
// a power of two and shifted by the level, at least one 32 x 32 tile of blocks), the levels one after the other in 4096 byte steps. When `packed_mips` is set the levels whose shorter
// side is 16 texels or less share ONE tile (GetPackedMipOffset says where each sits): they are the "packed tail", level GetPackedLevel() and above.
//
// DecodeLevel hands out the level as linear blocks of the format (a block row after a block row, no padding), byte swapped to the host's order, which is what BC1/BC2/BC3 and RGBA8
// textures are uploaded as.
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace larecomp::citypack::xenos {

// ---------------------------------------------------------------------------
// formats and endianness (the fetch constant's numbers)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kFormat8888 = 6;     // k_8_8_8_8: four bytes a texel (the fetch swizzle names the channels)
inline constexpr uint32_t kFormatDxt1 = 18;    // k_DXT1 = BC1, 8 bytes a 4 x 4 block
inline constexpr uint32_t kFormatDxt23 = 19;   // k_DXT2_3 = BC2, 16
inline constexpr uint32_t kFormatDxt45 = 20;   // k_DXT4_5 = BC3, 16

inline constexpr uint32_t kEndianNone = 0;
inline constexpr uint32_t kEndian8in16 = 1;
inline constexpr uint32_t kEndian8in32 = 2;
inline constexpr uint32_t kEndian16in32 = 3;

inline constexpr uint32_t kTileBlocks = 32;                 // a tile is 32 x 32 blocks
inline constexpr uint32_t kSubresourceAlignment = 4096;     // levels start on 4 KiB
inline constexpr uint32_t kLinearRowAlignment = 256;
inline constexpr uint32_t kMaxLevels = 14;                  // 8192 pixels is the console's limit: 14 levels

struct FormatInfo {
  uint32_t block_width = 0;   // 0: not a format this reader decodes
  uint32_t block_height = 0;
  uint32_t block_bytes = 0;
  const char* name = "";
  bool supported() const { return block_bytes != 0; }
};
FormatInfo GetFormatInfo(uint32_t format);

// The fetch constant's texture "sign" field: gamma (3) on x, y and z asks for the game's piecewise linear gamma.
inline constexpr uint32_t kSignGamma = 3;

// ---------------------------------------------------------------------------
// the fetch constant
// ---------------------------------------------------------------------------

struct TextureDesc {
  uint32_t dwords[6] = {};
  uint32_t type = 0;           // dword 0 bits 0..1 (2 = a texture)
  bool tiled = false;
  bool gamma = false;          // sign x, y and z are all "gamma"
  uint32_t pitch_div32 = 0;    // dword 0 bits 22..30: the base row pitch in texels / 32
  uint32_t format = 0;
  uint32_t endian = 0;
  uint32_t dimension = 0;      // dword 5 bits 9..10: 1 = 2D (or stacked), 0 1D, 2 3D, 3 cube
  bool stacked = false;
  bool packed_mips = false;
  uint32_t swizzle = 0;        // dword 3 bits 1..12: four 3 bit selectors, 0..3 = r, g, b, a, 4 = 0, 5 = 1
  uint32_t width = 0;          // texels
  uint32_t height = 0;
  uint32_t base_address = 0;   // the resource pointer of the base level (low 12 bits clear); 0: no base level stored
  uint32_t mip_address = 0;    // ... of the other levels; 0: none
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;  // after the clean up of GetSubresourcesFromFetchConstant: 0 when only the base is there
  uint32_t level_count() const { return mip_max_level + 1; }
  bool has_base() const { return base_address != 0 && mip_min_level == 0; }
};
inline constexpr uint32_t kIdentitySwizzle = 0x688;  // r g b a = 0 1 2 3: bits 0..2 = 0, 3..5 = 1, 6..8 = 2, 9..11 = 3

// Reads the six dwords (as the console has them, already host integers) the way GetSubresourcesFromFetchConstant does. False (with `why`) for what this reader does not handle:
// not a texture fetch constant, a 1D, 3D, cube or stacked texture, an empty size.
bool DescribeTexture(const uint32_t dwords[6], TextureDesc* out, std::string* why);

// ---------------------------------------------------------------------------
// tiling
// ---------------------------------------------------------------------------

// The byte offset of the block (x, y) of a tiled 2D image whose row is `pitch_blocks` blocks (rounded up to a tile) wide, blocks of 1 << bytes_per_block_log2 bytes.
int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t pitch_blocks, uint32_t bytes_per_block_log2);
// One past the highest byte a tiled image of `right` x `bottom` blocks reaches.
uint32_t GetTiledAddressUpperBound2D(uint32_t right, uint32_t bottom, uint32_t pitch_blocks, uint32_t bytes_per_block_log2);

// The level the packed tail starts at (0 for a texture whose shorter side is 16 texels or less, else log2(shorter side) - 4).
uint32_t GetPackedLevel(uint32_t width, uint32_t height);
// Where the packed level `mip` sits inside the tail, in blocks; false (and zeros) when it is stored on its own.
bool GetPackedMipOffset(uint32_t width, uint32_t height, uint32_t block_width, uint32_t block_height, uint32_t mip, uint32_t* x_blocks, uint32_t* y_blocks);

// ---------------------------------------------------------------------------
// the layout of a whole texture (GetGuestTextureLayout, 2D)
// ---------------------------------------------------------------------------

struct LevelLayout {
  uint32_t row_pitch_bytes = 0;
  uint32_t row_pitch_blocks = 0;        // tile aligned
  uint32_t y_stride_block_rows = 0;
  uint32_t slice_stride_bytes = 0;      // the distance to the next level in the mip region (4 KiB aligned)
  uint32_t x_extent_blocks = 0;
  uint32_t y_extent_blocks = 0;
  uint32_t data_extent_bytes = 0;       // what the level really reaches
};

struct GuestLayout {
  bool ok = false;
  uint32_t max_level = 0;
  uint32_t packed_level = 0xFFFFFFFFu;  // UINT32_MAX: no tail
  LevelLayout base;
  std::array<LevelLayout, kMaxLevels> mips;
  std::array<uint32_t, kMaxLevels> mip_offsets_bytes{};  // from mip_address
  uint32_t mips_total_extent_bytes = 0;
};
GuestLayout GetGuestLayout(const TextureDesc& desc);

// Where the blocks of one level are.
struct LevelPlan {
  bool ok = false;
  uint32_t level = 0;
  uint32_t width = 0;                  // texels
  uint32_t height = 0;
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
  bool in_base_region = false;         // else the mip region
  uint32_t region_address = 0;         // the resource pointer the region starts at
  uint32_t offset_bytes = 0;           // of the level's image inside the region
  uint32_t pitch_blocks = 0;           // the image's row, tile aligned
  uint32_t row_pitch_bytes = 0;        // linear images
  uint32_t tail_x = 0;                 // blocks: where the level sits in the packed tail
  uint32_t tail_y = 0;
  uint64_t reach_bytes = 0;            // one past the last byte of the region the level touches, from the region's start
};
LevelPlan PlanLevel(const TextureDesc& desc, const GuestLayout& layout, uint32_t level);

// Reads `plan`'s blocks from `region` (the bytes at plan.region_address, `region_size` of them) into `out` as linear blocks (width_blocks x height_blocks x block bytes), undoing the
// fetch constant's endian swap on every block. False when a block lay beyond `region_size` (it is written as zeros): a damaged or cut texture.
bool DecodeLevel(const TextureDesc& desc, const LevelPlan& plan, const uint8_t* region, size_t region_size, std::vector<uint8_t>* out);

// The byte swap of the fetch constant over `size` bytes (a multiple of the unit).
void SwapEndian(uint32_t endian, uint8_t* data, size_t size);

}  // namespace larecomp::citypack::xenos

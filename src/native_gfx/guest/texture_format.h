#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — texture fetch decoding
// ===========================================================================
// Decodes xe_gpu_texture_fetch_t (rex/graphics/xenos.h) into what the native
// texture cache needs, restricted to what MCLA actually binds.
//
// Telemetry over a real session (11.7k unique fetches, 4.5M draws):
//   dimension : 100% 2D, zero stacked / 3D / cube
//   tiled     : 66% of draws
//   formats (by draw share):
//     8_8_8_8 34% | DXT1 25% | DXT4_5 14% | 16_16_16_16_FLOAT 6.7%
//     24_8_FLOAT 6.4% | 8 5.3% | 16_16_16_16_EXPAND 4.3% | 32_FLOAT 3.1%
//     1_5_5_5 <1% | 24_8 <1%
//
// Anything outside that set is reported, never guessed: an unsupported
// format must fail loudly rather than render garbage.
//
// NOTE on 24_8 / 24_8_FLOAT / 32_FLOAT at screen resolutions: those fetches
// are render targets being read back as textures (shadow maps, post). They
// must be served by the render-target system, not decoded from guest memory
// — see RenderTargetLookup in d3d12/texture_cache.h.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

// Xenos TextureFormat values MCLA uses (xenos::TextureFormat).
enum class GuestTextureFormat : uint32_t {
  k_8 = 2,
  k_1_5_5_5 = 3,
  k_8_8_8_8 = 6,
  k_DXT1 = 18,
  k_DXT2_3 = 19,  // BC2 (DXT2/3, explicit 4-bit alpha) — rare, missed by first telemetry
  k_DXT4_5 = 20,
  k_24_8 = 22,
  k_24_8_FLOAT = 23,
  k_16_16_16_16_EXPAND = 29,
  k_32_FLOAT = 36,
  k_16_16_16_16_FLOAT = 32,
};

struct TextureFetch {
  uint32_t base_address = 0;  // byte address (base_address field << 12)
  uint32_t mip_address = 0;
  uint32_t width = 0;   // in texels
  uint32_t height = 0;
  uint32_t pitch = 0;   // in texels (pitch field << 5); 0 for tiled
  uint32_t format = 0;  // raw xenos::TextureFormat
  uint32_t endianness = 0;
  uint32_t swizzle = 0;
  uint32_t dimension = 0;  // 1 == 2D
  // Mip chain description out of the fetch constant. `mip_address` is where
  // level 1 and smaller live; `packed_mips` says the tail levels share one
  // block. See xenos.h xe_gpu_texture_fetch_t: mip_min_level +2 / mip_max_level
  // +6 in dword 4, packed_mips +11 in dword 5.
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;
  bool packed_mips = false;
  bool tiled = false;
  bool type_valid = false;  // fetch constant type bits == 2
  // The fetch constant's four 2-bit sign fields (dword 0, bits 2..9) say how
  // the hardware interprets each component on read; value 3 is GAMMA, i.e. an
  // sRGB->linear conversion in the texture unit. MCLA flags its albedo with
  // 3,3,3,0 -- gamma on colour, linear on alpha. The conversion is NOT done
  // with a DXGI _SRGB format: the Xenos curve is piecewise linear and differs
  // from sRGB by up to 69% in the deep darks, so the shader applies the real
  // curve instead. Measured over 64 distinct textures in one frame:
  // 55 carry 3,3,3,0, six are 0,0,0,0 (render targets and data maps) and three
  // are 1,1,1,1 (signed normal maps).
  bool gamma = false;
  bool operator==(const TextureFetch& o) const {
    return base_address == o.base_address && width == o.width && height == o.height &&
           format == o.format && tiled == o.tiled && pitch == o.pitch &&
           endianness == o.endianness && swizzle == o.swizzle && gamma == o.gamma;
  }
};

// Decodes a 6-dword texture fetch constant group (host-order dwords).
TextureFetch DecodeTextureFetch(const uint32_t d[6]);

// Native format mapping. Returns DXGI_FORMAT_UNKNOWN (0) for anything MCLA
// has not been observed to use — callers must treat that as an error.
uint32_t TextureFormatToDxgi(uint32_t xenos_format);

// Bytes per block and block dimensions for the supported formats.
// Uncompressed formats report a 1x1 block.
struct FormatInfo {
  uint32_t block_width = 1;
  uint32_t block_height = 1;
  uint32_t bytes_per_block = 0;  // 0 == unsupported
  bool is_compressed = false;
};
FormatInfo GetFormatInfo(uint32_t xenos_format);

// True when the format is only ever produced by the render-target/resolve
// path (depth and single-channel float targets read back as textures).
bool IsRenderTargetSourcedFormat(uint32_t xenos_format);

// --- endianness ------------------------------------------------------------
// Guest texture data is big-endian and the Xenos applies the swap during the
// fetch, so it has to be undone before D3D12 reads it. The width comes from
// the fetch constant's `endianness` field, NOT from the format.
//
// Observed over a full session: DXT1 (884) and DXT4_5 (357) are k8in16,
// k_8_8_8_8 (31) is k8in32, k_8 is kNone.
//
// k8in16 is right for DXT even though a block contains a 32-bit index word:
// the Xbox 360 stores the block as a run of big-endian 16-bit units, so
//   [c0_hi c0_lo c1_hi c1_lo row1 row0 row3 row2]
// becomes
//   [c0_lo c0_hi c1_lo c1_hi row0 row1 row2 row3]
// which is exactly the PC layout — the index field is four per-row BYTES, and
// the 16-bit swap is what puts those rows back in order.
//
// Swapping after untiling is equivalent to swapping before: untiling moves
// whole blocks, so every block boundary stays aligned to the swap width.
enum class TextureEndian : uint32_t {
  kNone = 0,
  k8in16 = 1,
  k8in32 = 2,
  k16in32 = 3,
};
void SwapTextureData(uint32_t endianness, uint8_t* data, uint64_t size_bytes);

// --- tiling ----------------------------------------------------------------
// Xenos 2D textures are stored in a swizzled ("tiled") layout built from
// 32x32-block macro tiles. A tiled surface is therefore PADDED to a multiple
// of 32 blocks in both axes, and occupies more memory than width*height:
// reading only width*height bytes walks off the end (and the macro-tile
// stride must use the padded pitch, or every address past the first tile row
// is wrong).

// Padded extent of a tiled surface, in blocks.
inline constexpr uint32_t kTileBlockAlignment = 32;
inline uint32_t AlignToTile(uint32_t blocks) {
  return (blocks + kTileBlockAlignment - 1) & ~(kTileBlockAlignment - 1);
}

// Bytes a tiled surface occupies in guest memory, including tile padding.
uint64_t TiledSurfaceSizeBytes(uint32_t width_blocks, uint32_t height_blocks,
                               uint32_t bytes_per_block);

// Converts a tiled surface to linear. `src_size_bytes` is the number of
// bytes readable at `src` and MUST be at least TiledSurfaceSizeBytes(...);
// blocks whose swizzled address would fall outside it are skipped rather
// than read, so a bad fetch constant cannot fault the process.
// Returns false if the source was too small (partial output).
bool UntileSurface2D(uint8_t* dst, uint32_t dst_pitch_bytes, const uint8_t* src,
                     uint64_t src_size_bytes, uint32_t width_blocks, uint32_t height_blocks,
                     uint32_t bytes_per_block);

// Byte offset of a tiled block within a 2D surface (Xenos address swizzle).
// `width_blocks` is the unpadded width; the tile-aligned pitch is applied
// internally.
uint32_t TiledOffset2D(uint32_t x_block, uint32_t y_block, uint32_t width_blocks,
                       uint32_t bytes_per_block);

}  // namespace mcla::native_gfx

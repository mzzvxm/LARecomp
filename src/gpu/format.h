#pragma once
// ===========================================================================
// MCLA GPU renderer -- guest formats to Plume formats
// ===========================================================================
// Exhaustive over what MCLA uses, each row with where it comes from. Three
// outcomes, kept apart on purpose:
//
//   kOk              mapped
//   kMissingInPlume  a guest format whose host equivalent Plume does not
//                    expose (R10G10B10A2_UNORM for the k_2_10_10_10 targets,
//                    which MCLA was never seen to use); `format` is
//                    UNKNOWN. Added to the Plume fork if one ever appears.
//   kUnknown         a value MCLA was never seen to use: reported, not guessed.
//
// Sources: vertex declaration types -- dword_827D42B0 (8 non-zero entries,
// read in the IDB) with the widths measured by the legacy DECLGAP probe;
// render targets -- the legacy table (guest/render_state.cpp), whose DXGI
// choices map one to one, except that k_8_8_8_8_GAMMA targets are stored
// linear in float16; textures -- Xenia's host format tables in
// the SDK (D3D12TextureCache::host_formats_, VulkanTextureCache::
// kBestHostFormats; Copyright Ben Vanik, BSD-3-Clause), for the formats MCLA
// uses: k_1_5_5_5 is B5G5R5A1 with red and blue swapped at load,
// k_16_16_16_16_EXPAND is float16, depth formats are one float channel.
//
// Threading: pure functions.
// ===========================================================================

#include <cstdint>

#include <plume_render_interface_types.h>

namespace mcla::gpu::format {

enum class Status : uint8_t { kOk, kMissingInPlume, kUnknown };

struct Mapping {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  Status status = Status::kUnknown;
  const char* guest_name = "unknown";
};

const char* StatusName(Status status);
const char* PlumeFormatName(plume::RenderFormat format);

// Packed Xenon D3DDECLTYPE as stored in a D3DVERTEXELEMENT9.
Mapping VertexFormat(uint32_t packed_decltype);
// Packed decltypes the input assembler hands over whole, for the shader to
// unpack (k_2_10_10_10 as one R32_UINT): with one of these on NORMAL/TANGENT/
// BINORMAL the VS needs spec constant R11G11B10_NORMAL.
bool IsPackedNormalDeclType(uint32_t packed_decltype);

// xenos::ColorRenderTargetFormat (RB_COLOR_INFO bits 16..19).
Mapping ColorTargetFormat(uint32_t color_format);
// xenos::DepthRenderTargetFormat (RB_DEPTH_INFO bit 16): kD24S8 = 0, kD24FS8 = 1.
Mapping DepthTargetFormat(uint32_t depth_format);
// How a guest block becomes host data when a texture is decoded from memory
// (texture_decode.h). Every kind starts with the fetch constant's endian swap.
enum class TextureLoad : uint8_t {
  kCopy,                // the block as is
  kR5G5B5A1ToB5G5R5A1,  // 16 bpp: the 5-bit X (bits 0-4) and Z (10-14) fields swapped
  kDepthUnorm24,        // 32 bpp: xenos::UNorm24To32(v >> 8) as float
  kDepthFloat20e4,      // 32 bpp: xenos::Float20e4To32(v >> 8)
};

// Block-compressed formats whose base is not a multiple of the 4x4 block are
// decompressed to RGBA8 on the CPU (Xenia's IsDecompressionNeeded: Direct3D
// creates BC textures with 4-aligned dimensions only, and a padded texture
// would shift every normalized coordinate).
enum class TextureDecompress : uint8_t { kNone, kDXT1, kDXT2_3, kDXT4_5 };

// Xenos swizzles (XE_GPU_MAKE_TEXTURE_SWIZZLE: 3 bits per component, X first)
// of the host format's components: what each guest component
// reads in the host texture before the fetch constant's own swizzle.
inline constexpr uint16_t kHostSwizzleRGBA = 0 | 1 << 3 | 2 << 6 | 3 << 9;  // 0x688
inline constexpr uint16_t kHostSwizzleRRRR = 0;

struct TextureHostFormat {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  Status status = Status::kUnknown;
  const char* guest_name = "unknown";
  uint16_t host_swizzle = kHostSwizzleRGBA;
  TextureLoad load = TextureLoad::kCopy;
  TextureDecompress decompress = TextureDecompress::kNone;
};

// xenos::TextureFormat (fetch constant dword 1 bits 0..5): the host texture
// format, swizzle and load. status kOk only for the formats MCLA uses.
TextureHostFormat TextureHost(uint32_t texture_format);
// The host format alone (the validator's view of the same table).
Mapping TextureFormat(uint32_t texture_format);

// D3DDECLUSAGE -> the semantic name XenosRecomp emits, or null.
const char* UsageSemantic(uint32_t usage);

}  // namespace mcla::gpu::format

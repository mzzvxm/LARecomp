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
// render targets and textures -- the legacy tables (guest/render_state.cpp,
// guest/texture_format.cpp), whose DXGI choices map one to one, except where
// M4 decided otherwise: k_8_8_8_8_GAMMA targets are stored
// linear in float16, k_1_5_5_5 textures are expanded to 8888, and
// k_16_16_16_16_EXPAND is float16.
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
// xenos::TextureFormat (fetch constant dword 1 bits 0..5).
Mapping TextureFormat(uint32_t texture_format);

// D3DDECLUSAGE -> the semantic name XenosRecomp emits, or null.
const char* UsageSemantic(uint32_t usage);

}  // namespace mcla::gpu::format

#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — sampler state from texture fetch constants
// ===========================================================================
// A Xenos texture fetch constant carries both the resource description and
// the sampler state; there is no separate sampler object. The sampler fields
// live in dwords 3 and 4 of xe_gpu_texture_fetch_t (rex/graphics/xenos.h):
//
//   dword_0: clamp_x +10, clamp_y +13, clamp_z +16   (3 bits each)
//   dword_3: mag_filter +19, min_filter +21, mip_filter +23,
//            aniso_filter +25 (3 bits)
//   dword_4: mip_min_level +2, mip_max_level +6, lod_bias +12 (10 bits,
//            5 fractional)
//   dword_5: border_color +0
//
// No comparison sampler exists on this path: the Xenos does depth compares
// through a separate fetch mode, and MCLA was not observed using one. Nothing
// is implemented for features the game does not use.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

// Decoded sampler description. Kept as the raw Xenos values plus the derived
// D3D12 pieces so the key stays exact.
struct SamplerDescription {
  uint32_t clamp_x = 0;
  uint32_t clamp_y = 0;
  uint32_t clamp_z = 0;
  uint32_t mag_filter = 0;  // xenos::TextureFilter: 0 point, 1 linear, 3 keep
  uint32_t min_filter = 0;
  uint32_t mip_filter = 0;
  uint32_t aniso_filter = 0;
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;
  int32_t lod_bias_raw = 0;  // 5 fractional bits
  uint32_t border_color = 0;

  bool operator==(const SamplerDescription& o) const {
    return clamp_x == o.clamp_x && clamp_y == o.clamp_y && clamp_z == o.clamp_z &&
           mag_filter == o.mag_filter && min_filter == o.min_filter &&
           mip_filter == o.mip_filter && aniso_filter == o.aniso_filter &&
           mip_min_level == o.mip_min_level && mip_max_level == o.mip_max_level &&
           lod_bias_raw == o.lod_bias_raw && border_color == o.border_color;
  }
};

// Decodes the sampler half of a 6-dword texture fetch constant group.
SamplerDescription DecodeSampler(const uint32_t d[6]);

// Fills a D3D12_SAMPLER_DESC-shaped output. Declared with raw types so this
// header stays free of d3d12.h; `out` must point at a D3D12_SAMPLER_DESC.
void BuildD3D12SamplerDesc(const SamplerDescription& s, void* out);

}  // namespace mcla::native_gfx

#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — sampler state decoding.
// See sampler_state.h. Field positions from rex/graphics/xenos.h
// (xe_gpu_texture_fetch_t).

#include "sampler_state.h"

#include <d3d12.h>

namespace mcla::native_gfx {

namespace {

// xenos::ClampMode -> D3D12 address mode.
D3D12_TEXTURE_ADDRESS_MODE AddressMode(uint32_t clamp) {
  switch (clamp) {
    case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;            // kRepeat
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;          // kMirroredRepeat
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;           // kClampToEdge
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;     // kMirrorClampToEdge
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;          // kClampToHalfway
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;     // kMirrorClampToHalfway
    case 6: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;          // kClampToBorder
    default: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;    // kMirrorClampToBorder
  }
}

// xenos::TextureFilter: 0 point, 1 linear, 2 "bicubic", 3 "use fetch
// constant" (only meaningful on the instruction, never in the constant).
bool FilterIsLinear(uint32_t f) { return f == 1 || f == 2; }

}  // namespace

SamplerDescription DecodeSampler(const uint32_t d[6]) {
  SamplerDescription s;
  s.clamp_x = (d[0] >> 10) & 0x7u;
  s.clamp_y = (d[0] >> 13) & 0x7u;
  s.clamp_z = (d[0] >> 16) & 0x7u;

  s.mag_filter = (d[3] >> 19) & 0x3u;
  s.min_filter = (d[3] >> 21) & 0x3u;
  s.mip_filter = (d[3] >> 23) & 0x3u;
  s.aniso_filter = (d[3] >> 25) & 0x7u;

  s.mip_min_level = (d[4] >> 2) & 0xFu;
  s.mip_max_level = (d[4] >> 6) & 0xFu;
  // lod_bias is a signed 10-bit field with 5 fractional bits.
  const uint32_t raw = (d[4] >> 12) & 0x3FFu;
  s.lod_bias_raw = raw & 0x200u ? int32_t(raw) - 1024 : int32_t(raw);

  s.border_color = d[5] & 0x3u;
  return s;
}

void BuildD3D12SamplerDesc(const SamplerDescription& s, void* out) {
  auto& d = *static_cast<D3D12_SAMPLER_DESC*>(out);
  d = {};

  // xenos::AnisoFilter: 0 disabled, then 1..5 = max 1x/2x/4x/8x/16x, and
  // 7 = "use the fetch constant". So the ratio is 1 << (value - 1), NOT
  // 1 << value: the shift used to be off by one, which turned kMax_16_1 into
  // MaxAnisotropy 32. D3D12 accepts only [0, 16] and answers an out-of-range
  // sampler by REMOVING THE DEVICE (CreateSampler2 error 742, then
  // DXGI_ERROR_INVALID_CALL) — no page fault and no DRED breadcrumb, because
  // nothing ever reached the GPU.
  const bool aniso = s.aniso_filter != 0;
  if (aniso) {
    d.Filter = D3D12_FILTER_ANISOTROPIC;
    const uint32_t ratio = s.aniso_filter <= 5u ? (1u << (s.aniso_filter - 1u))
                                                : 16u;  // 6/7 are not real ratios
    d.MaxAnisotropy = ratio > 16u ? 16u : ratio;
  } else {
    const bool min_lin = FilterIsLinear(s.min_filter);
    const bool mag_lin = FilterIsLinear(s.mag_filter);
    const bool mip_lin = FilterIsLinear(s.mip_filter);
    d.Filter = D3D12_FILTER(((min_lin ? 1 : 0) << 4) | ((mag_lin ? 1 : 0) << 2) |
                            ((mip_lin ? 1 : 0) << 0));
    d.MaxAnisotropy = 1;
  }

  d.AddressU = AddressMode(s.clamp_x);
  d.AddressV = AddressMode(s.clamp_y);
  d.AddressW = AddressMode(s.clamp_z);
  d.MipLODBias = float(s.lod_bias_raw) / 32.0f;  // 5 fractional bits
  d.MinLOD = float(s.mip_min_level);
  d.MaxLOD = s.mip_max_level ? float(s.mip_max_level) : D3D12_FLOAT32_MAX;
  d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

  // xenos::BorderColor: 0 = transparent black, 1 = opaque black,
  // 2 = opaque white. Anything else is not used by the game.
  switch (s.border_color) {
    case 1:
      d.BorderColor[3] = 1.0f;
      break;
    case 2:
      d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
      break;
    default:
      break;  // transparent black
  }
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

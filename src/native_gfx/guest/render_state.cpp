#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest render state.
// See render_state.h for how each shadow address was derived.

#include "render_state.h"

#include <cstring>

#include <dxgiformat.h>

#include <rex/system/xmemory.h>

namespace mcla::native_gfx {

namespace {

inline uint32_t R32(const uint8_t* base, uint32_t ea) {
  if (ea < 0x1000u) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

}  // namespace

GuestRenderState ReadRenderState(const uint8_t* base, uint32_t dev) {
  GuestRenderState s;
  if (!base || dev < 0x1000u) {
    return s;
  }
  s.surface_info = R32(base, dev + kDevRegSurfaceInfo);
  s.color_info = R32(base, dev + kDevRegColorInfo);
  s.depth_info = R32(base, dev + kDevRegDepthInfo);
  s.color_mask = R32(base, dev + kDevRegColorMask);
  s.depth_control = R32(base, dev + kDevRegDepthControl);
  s.blend_control0 = R32(base, dev + kDevRegBlendControl0);
  s.color_control = R32(base, dev + kDevRegColorControl);
  s.mode_control = R32(base, dev + kDevRegModeControl);
  s.pa_su_sc_mode_cntl = R32(base, dev + kDevRegPaSuScModeCntl);

  s.msaa_samples = (s.surface_info >> 16) & 0x3u;
  s.color_format = (s.color_info >> 16) & 0xFu;
  s.depth_format = (s.depth_info >> 16) & 0x1u;

  s.stencil_enable = (s.depth_control & 0x1u) != 0;
  s.backface_enable = (s.depth_control & 0x80u) != 0;
  s.stencil_func = (s.depth_control >> 8) & 0x7u;
  s.stencil_fail = (s.depth_control >> 11) & 0x7u;
  s.stencil_zpass = (s.depth_control >> 14) & 0x7u;
  s.stencil_zfail = (s.depth_control >> 17) & 0x7u;
  s.stencil_func_bf = (s.depth_control >> 20) & 0x7u;
  s.stencil_fail_bf = (s.depth_control >> 23) & 0x7u;
  s.stencil_zpass_bf = (s.depth_control >> 26) & 0x7u;
  s.stencil_zfail_bf = (s.depth_control >> 29) & 0x7u;
  const uint32_t refmask = R32(base, dev + kDevRegStencilRefMask);
  s.stencil_ref = refmask & 0xFFu;
  s.stencil_read_mask = (refmask >> 8) & 0xFFu;
  s.stencil_write_mask = (refmask >> 16) & 0xFFu;
  s.depth_enable = (s.depth_control & 0x2u) != 0;
  s.depth_write = (s.depth_control & 0x4u) != 0;
  s.depth_func = (s.depth_control >> 4) & 0x7u;

  s.alpha_func = s.color_control & 0x7u;
  s.alpha_test_enable = (s.color_control & 0x8u) != 0;

  s.cull_front = (s.pa_su_sc_mode_cntl & 0x1u) != 0;
  s.cull_back = (s.pa_su_sc_mode_cntl & 0x2u) != 0;
  s.front_face_is_cw = (s.pa_su_sc_mode_cntl & 0x4u) != 0;
  s.poly_offset_front_enable = (s.pa_su_sc_mode_cntl & (1u << 11)) != 0;
  s.poly_offset_back_enable = (s.pa_su_sc_mode_cntl & (1u << 12)) != 0;

  s.edram_mode = s.mode_control & 0x7u;

  const auto read_float = [&](uint32_t offset) {
    const uint32_t bits = R32(base, dev + offset);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  };

  {
    // Two signed 15-bit fields per register: x at bits 0..14, y at 16..30.
    const auto sext15 = [](uint32_t v) {
      const int32_t x = int32_t(v & 0x7FFFu);
      return (x & 0x4000) ? (x - 0x8000) : x;
    };
    const uint32_t tl = R32(base, dev + kDevRegScreenScissorTl);
    const uint32_t br = R32(base, dev + kDevRegScreenScissorBr);
    s.scissor_left = sext15(tl);
    s.scissor_top = sext15(tl >> 16);
    s.scissor_right = sext15(br);
    s.scissor_bottom = sext15(br >> 16);
    s.scissor_valid = s.scissor_right > s.scissor_left && s.scissor_bottom > s.scissor_top;
  }

  s.alpha_ref = read_float(kDevRegAlphaRef);
  if (s.poly_offset_front_enable || s.poly_offset_back_enable) {
    s.poly_offset_front_scale = read_float(kDevRegPolyOffsetFrontScale);
    s.poly_offset_front_offset = read_float(kDevRegPolyOffsetFrontOffset);
    s.poly_offset_back_scale = read_float(kDevRegPolyOffsetBackScale);
    s.poly_offset_back_offset = read_float(kDevRegPolyOffsetBackOffset);
  }
  // PA_CL_VTE_CNTL bits 0..5 enable each scale/offset independently; a
  // disabled field is not applied at all, which is 1.0 for a scale and 0.0
  // for an offset.
  s.blend_constant[0] = read_float(kDevRegBlendRed);
  s.blend_constant[1] = read_float(kDevRegBlendGreen);
  s.blend_constant[2] = read_float(kDevRegBlendBlue);
  s.blend_constant[3] = read_float(kDevRegBlendAlpha);

  s.vte_cntl = R32(base, dev + kDevRegVteCntl);
  s.vport_x_scale = (s.vte_cntl & 0x01u) ? read_float(kDevRegVportXScale) : 1.0f;
  s.vport_x_offset = (s.vte_cntl & 0x02u) ? read_float(kDevRegVportXOffset) : 0.0f;
  s.vport_y_scale = (s.vte_cntl & 0x04u) ? read_float(kDevRegVportYScale) : 1.0f;
  s.vport_y_offset = (s.vte_cntl & 0x08u) ? read_float(kDevRegVportYOffset) : 0.0f;
  s.vport_z_scale = (s.vte_cntl & 0x10u) ? read_float(kDevRegVportZScale) : 1.0f;
  s.vport_z_offset = (s.vte_cntl & 0x20u) ? read_float(kDevRegVportZOffset) : 0.0f;
  return s;
}

HostViewport ComputeHostViewport(const GuestRenderState& s) {
  HostViewport v;
  v.width = 2.0f * s.vport_x_scale;
  v.top_left_x = s.vport_x_offset - s.vport_x_scale;
  v.height = -2.0f * s.vport_y_scale;
  v.top_left_y = s.vport_y_offset + s.vport_y_scale;
  v.min_depth = s.vport_z_offset;
  v.max_depth = s.vport_z_offset + s.vport_z_scale;
  if (v.height < 0.0f) {
    // D3D12 has no negative-height viewport, so the flip cannot be expressed
    // here; report it so the caller can compensate on the winding instead.
    v.y_flipped = true;
    v.height = -v.height;
    v.top_left_y = s.vport_y_offset - s.vport_y_scale;
  }
  if (v.width < 0.0f) {
    v.width = -v.width;
    v.top_left_x = s.vport_x_offset + s.vport_x_scale;
  }
  return v;
}

float AlphaTestThreshold(const GuestRenderState& s) {
  if (!s.alpha_test_enable) {
    return 0.0f;  // alpha is never negative, so nothing is discarded
  }
  // The next representable float above `v`, so a strict ">" becomes the ">="
  // the shader implements.
  const auto next_above = [](float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    if (v >= 0.0f) {
      ++bits;
    } else {
      --bits;
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
  };
  switch (s.alpha_func) {
    case 0:  // kNever: discard everything.
      return 3.402823466e+38f;
    case 4:  // kGreater: keep alpha > ref.
      return next_above(s.alpha_ref);
    case 5:  // kNotEqual: only expressible as a lower bound when ref is 0,
             // which is the only form MCLA uses (RB_COLORCONTROL 0x...0D on
             // the composite pass). Any other reference degrades to keeping
             // everything rather than cutting a band out of the middle.
      return s.alpha_ref == 0.0f ? next_above(0.0f) : 0.0f;
    case 6:  // kGreaterEqual: exactly what the shader compiles.
      return s.alpha_ref;
    case 1:  // kLess
    case 2:  // kEqual
    case 3:  // kLessEqual
    case 7:  // kAlways
    default:
      return 0.0f;
  }
}

uint32_t ColorRenderTargetFormatToDxgi(uint32_t color_format) {
  switch (color_format) {
    case 0:   // k_8_8_8_8
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 1:   // k_8_8_8_8_GAMMA
      return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3:   // k_2_10_10_10_FLOAT
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
      // No DXGI equivalent of the 7e3 float format; the wider float target
      // is the lossless substitute Xenia also uses.
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4:   // k_16_16
      return DXGI_FORMAT_R16G16_SNORM;
    case 5:   // k_16_16_16_16
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 6:   // k_16_16_FLOAT
      return DXGI_FORMAT_R16G16_FLOAT;
    case 7:   // k_16_16_16_16_FLOAT
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 14:  // k_32_FLOAT
      return DXGI_FORMAT_R32_FLOAT;
    case 15:  // k_32_32_FLOAT
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

uint32_t DepthRenderTargetFormatToDxgi(uint32_t depth_format) {
  // kD24S8 = 0, kD24FS8 = 1. D24FS8 has no direct equivalent; D32_FLOAT_S8
  // preserves the float depth range without losing stencil.
  return depth_format == 0 ? DXGI_FORMAT_D24_UNORM_S8_UINT
                           : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

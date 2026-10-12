// MCLA GPU renderer -- host viewport, NDC transform and scissor. See viewport.h.
//
// Ported from Xenia's draw_util::GetHostViewportInfo and GetScissor
// (src/graphics/util/draw.cpp in the ReXGlue SDK; Copyright Ben Vanik, the
// Xenia authors, BSD-3-Clause). The comments there explain every step; the
// ones here say what was left out and why.

#include "viewport.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mcla::gpu {

namespace {

float Clamp(float value, float low, float high) {
  // NaN goes to `low`, as rex::clamp_float does.
  return value > low ? (value < high ? value : high) : low;
}

float Saturate(float value) { return Clamp(value, 0.0f, 1.0f); }

}  // namespace

ViewportInfo ComputeViewport(const XenosState& state, bool pixel_shader_writes_depth) {
  ViewportInfo out;

  const float scale_xy[2] = {state.vte.vport_x_scale_ena ? state.viewport[0] : 1.0f,
                             state.vte.vport_y_scale_ena ? state.viewport[2] : 1.0f};
  const float scale_z = state.vte.vport_z_scale_ena ? state.viewport[4] : 1.0f;
  const float offset_base_xy[2] = {state.vte.vport_x_offset_ena ? state.viewport[1] : 0.0f,
                                   state.vte.vport_y_offset_ena ? state.viewport[3] : 0.0f};
  const float offset_z = state.vte.vport_z_offset_ena ? state.viewport[5] : 0.0f;

  // The window offset (vtx_window_offset_enable) is left out: it is in no
  // device shadow -- D3D writes PA_SC_WINDOW_OFFSET straight into the command
  // buffer -- and is 0 outside tiled rendering, while inside it is the per-tile
  // offset that drawing the whole surface at once does without (24.10, 24.12).
  float offset_add_xy[2] = {0.0f, 0.0f};
  if (state.vtx.pix_center == xe::PixelCenter::kD3DZero) {
    offset_add_xy[0] += 0.5f;
    offset_add_xy[1] += 0.5f;
  }

  float z_min;
  float z_max;
  uint32_t xy_offset[2];
  uint32_t xy_extent[2];

  if (state.clip.clip_disable) {
    for (uint32_t i = 0; i < 2; ++i) {
      xy_offset[i] = 0;
      xy_extent[i] = kMaxViewportExtent;
      const float extent = float(kMaxViewportExtent);
      const float pixels_to_ndc = 2.0f / extent;
      out.ndc_scale[i] = scale_xy[i] * pixels_to_ndc;
      out.ndc_offset[i] = (offset_base_xy[i] - extent * 0.5f + offset_add_xy[i]) * pixels_to_ndc;
    }
    z_min = 0.0f;
    z_max = 1.0f;
    out.ndc_scale[2] = scale_z;
    out.ndc_offset[2] = offset_z;
  } else {
    for (uint32_t i = 0; i < 2; ++i) {
      const float offset_axis = offset_base_xy[i] + offset_add_xy[i];
      const float scale_axis = scale_xy[i];
      const float scale_axis_abs = std::abs(scale_xy[i]);
      const float axis_max = float(kMaxViewportExtent);
      const uint32_t axis_0 = uint32_t(Clamp(offset_axis - scale_axis_abs, 0.0f, axis_max));
      const uint32_t axis_1 = uint32_t(Clamp(offset_axis + scale_axis_abs, 0.0f, axis_max));
      const uint32_t axis_extent = axis_1 - axis_0;
      xy_offset[i] = axis_0;
      xy_extent[i] = axis_extent;
      if (axis_extent) {
        const float extent_rounded = float(axis_extent);
        out.ndc_scale[i] = scale_axis * 2.0f / extent_rounded;
        out.ndc_offset[i] =
            (offset_axis - (float(axis_0) + extent_rounded * 0.5f)) * 2.0f / extent_rounded;
      } else {
        out.ndc_scale[i] = 1.0f;
        out.ndc_offset[i] = 0.0f;
      }
    }
    float host_clip_offset_z;
    float host_clip_scale_z;
    if (state.clip.dx_clip_space_def) {
      host_clip_offset_z = offset_z;
      host_clip_scale_z = scale_z;
      out.ndc_scale[2] = 1.0f;
      out.ndc_offset[2] = 0.0f;
    } else {
      host_clip_offset_z = offset_z - scale_z;
      host_clip_scale_z = scale_z * 2.0f;
      out.ndc_scale[2] = 0.5f;
      out.ndc_offset[2] = 0.5f;
    }
    if (pixel_shader_writes_depth) {
      z_min = 0.0f;
      z_max = 1.0f;
    } else {
      z_min = Saturate(host_clip_offset_z);
      z_max = Saturate(host_clip_offset_z + host_clip_scale_z);
      // Direct3D 12 does not allow a reverse depth range: swap it and mirror Z
      // in the shader instead. MCLA's main scene has a reverse-Z viewport (24.3).
      if (z_min > z_max) {
        std::swap(z_min, z_max);
        out.ndc_scale[2] = -out.ndc_scale[2];
        out.ndc_offset[2] = 1.0f - out.ndc_offset[2];
      }
    }
  }

  // Xenia's arithmetic produces NDC whose +Y points down the render target;
  // Direct3D's NDC +Y points up, so Y is negated -- Xenia's D3D12 backend asks
  // for the same (origin_bottom_left = true, command_processor.cpp). The
  // SPIR-V vertex shaders are compiled with -fvk-invert-y (the pack builder),
  // so they also take a Direct3D-oriented position: the same negation serves
  // Vulkan.
  out.ndc_scale[1] = -out.ndc_scale[1];
  out.ndc_offset[1] = -out.ndc_offset[1];

  out.viewport = plume::RenderViewport(float(xy_offset[0]), float(xy_offset[1]),
                                       float(xy_extent[0]), float(xy_extent[1]), z_min, z_max);
  return out;
}

plume::RenderRect ComputeScissor(const XenosState& state, uint32_t target_height) {
  xreg::PA_SC_WINDOW_SCISSOR_TL tl;
  tl.value = state.scissor_tl;
  xreg::PA_SC_WINDOW_SCISSOR_BR br;
  br.value = state.scissor_br;
  int32_t tl_x = int32_t(tl.tl_x);
  int32_t tl_y = int32_t(tl.tl_y);
  int32_t br_x = int32_t(br.br_x);
  int32_t br_y = int32_t(br.br_y);
  const int32_t pitch = int32_t(state.surface_pitch);
  tl_x = std::min(tl_x, pitch);
  br_x = std::min(br_x, pitch);
  tl_y = std::min(tl_y, int32_t(target_height));
  br_y = std::min(br_y, int32_t(target_height));
  tl_x = std::max(tl_x, 0);
  tl_y = std::max(tl_y, 0);
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  return plume::RenderRect(tl_x, tl_y, br_x, br_y);
}

}  // namespace mcla::gpu

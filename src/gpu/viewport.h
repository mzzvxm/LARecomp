#pragma once
// ===========================================================================
// MCLA GPU renderer -- host viewport, NDC transform and scissor of a draw
// ===========================================================================
// The arithmetic is Xenia's
// draw_util::GetHostViewportInfo (src/graphics/util/draw.cpp in the SDK,
// BSD-3-Clause), ported to the cases this renderer has: resolution scale 1,
// Direct3D NDC orientation on both backends -- Xenia's origin_bottom_left
// case, Y negated (the SPIR-V vertex shaders flip Y themselves,
// -fvk-invert-y) --, no reverse depth range (Direct3D 12 does not allow one,
// so a guest viewport with near > far is swapped and Z mirrored in the
// shader), no float24 conversion of Z (D24FS8 is stored as D32 float, 24.12).
//
// The host viewport is integer; everything fractional -- the guest viewport's
// own offset, the half-pixel offset of the D3D pixel centre, the remapping
// from the snapped rectangle back to the guest one, the Z remap -- goes to the
// vertex shader epilogue through g_PosScale/g_PosOffset (shader contract v4,
// 24.5): oPos.xyz = oPos.xyz * scale + offset * oPos.w.
//
// With clipping disabled (screen-space draws, the VTE-disabled rects of 24.3)
// the host viewport spans kMaxExtent in both axes, a power of two, and the
// whole pixel-to-NDC conversion is in the shader.
//
// Threading: pure functions.
// ===========================================================================

#include <cstdint>

#include <plume_render_interface_types.h>

#include "draw_state.h"

namespace mcla::gpu {

struct ViewportInfo {
  plume::RenderViewport viewport;
  float ndc_scale[3] = {1.0f, 1.0f, 1.0f};
  float ndc_offset[3] = {0.0f, 0.0f, 0.0f};
};

// Largest host viewport (and render target) extent, xenos::kTexture2DCubeMaxWidthHeight.
inline constexpr uint32_t kMaxViewportExtent = 8192;

// `pixel_shader_writes_depth`: the depth range is then the full 0..1, the
// guest having no viewport Z clamp. `resolution_scale`: host pixels per guest
// pixel of the target -- the viewport is found in guest pixels
// and only its integer offset and extent are scaled, as Xenia's
// GetHostViewportInfo does; NDC stays in guest units.
ViewportInfo ComputeViewport(const XenosState& state, bool pixel_shader_writes_depth,
                             uint32_t resolution_scale);

// D3D's effective window scissor (draw_state.h), clamped to the surface pitch
// and to [0, height), as Xenia's draw_util::GetScissor does for the pitch.
// `target_height` in guest pixels; the rect returned is in host pixels
// (`resolution_scale` per guest pixel).
plume::RenderRect ComputeScissor(const XenosState& state, uint32_t target_height,
                                 uint32_t resolution_scale);

}  // namespace mcla::gpu

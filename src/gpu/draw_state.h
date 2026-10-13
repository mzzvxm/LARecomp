#pragma once
// ===========================================================================
// MCLA GPU renderer -- the Xenos state of one draw, decoded
// ===========================================================================
// Every register the replay reads for a draw is decoded here once, from the
// captured window R (records.h, guest_device.h), with the field layouts of the
// SDK's rex/graphics/registers.h (Xenia's), into what the pipelines, the
// viewport, the EDRAM targets and the constants need.
//
// A colour target counts as bound when D3D has a surface set for it and
// RB_COLOR_MASK writes at least one of its channels (the rule the frame census
// used, 24.3); the depth target when a depth surface is set and depth or
// stencil is enabled. The pixel shader's own outputs narrow the colour targets
// further at pipeline time (pipelines.h), the shader not being known here.
//
// (XenosState, not DrawState: winuser.h, which the SDK headers pull in,
// defines DrawState as a macro.)
//
// Threading: pure functions.
// ===========================================================================

#include <cstdint>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include "draw_decode.h"

namespace mcla::gpu {

namespace xe = rex::graphics::xenos;
namespace xreg = rex::graphics::reg;

inline constexpr uint32_t kColorTargetCount = 4;

struct ColorTargetState {
  bool bound = false;
  uint32_t base_tiles = 0;  // EDRAM tile index (color_base with bit 11)
  xe::ColorRenderTargetFormat format = xe::ColorRenderTargetFormat::k_8_8_8_8;
  int32_t exp_bias = 0;     // RB_COLOR_INFO color_exp_bias, a signed power of two
  uint32_t write_mask = 0;  // RGBA bits from RB_COLOR_MASK
  xreg::RB_BLENDCONTROL blend{};
};

struct XenosState {
  uint32_t surface_pitch = 0;  // pixels
  xe::MsaaSamples msaa = xe::MsaaSamples::k1X;
  ColorTargetState color[kColorTargetCount];
  bool depth_bound = false;
  uint32_t depth_base_tiles = 0;
  xe::DepthRenderTargetFormat depth_format = xe::DepthRenderTargetFormat::kD24S8;

  xreg::RB_DEPTHCONTROL depth_control{};
  xreg::RB_STENCILREFMASK stencil{};
  xreg::RB_STENCILREFMASK stencil_bf{};
  xreg::RB_COLORCONTROL color_control{};
  xreg::RB_MODECONTROL mode{};
  xreg::PA_SU_SC_MODE_CNTL sc_mode{};
  xreg::PA_CL_CLIP_CNTL clip{};
  xreg::PA_CL_VTE_CNTL vte{};
  xreg::PA_SU_VTX_CNTL vtx{};
  uint32_t aa_config = 0;  // PA_SC_AA_CONFIG
  uint32_t aa_mask = 0;    // PA_SC_AA_MASK
  // VGT_MULTI_PRIM_IB_RESET_INDX: the strip reset index while
  // PA_SU_SC_MODE_CNTL.multi_prim_ib_ena is on.
  uint32_t reset_index = 0;

  // PA_CL_VPORT_XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET as stored
  // (whether each applies is PA_CL_VTE_CNTL's).
  float viewport[6] = {};
  // PA_SU_POLY_OFFSET_FRONT_SCALE, FRONT_OFFSET, BACK_SCALE, BACK_OFFSET.
  float poly_offset[4] = {};
  float alpha_ref = 0.0f;
  float blend_constant[4] = {};

  // D3D's effective window scissor -- viewport intersected with the scissor
  // rectangle when scissor is on -- that SetScissorRect (sub_82415C80) keeps
  // at dev+10436/10440: x | y << 16, 15 bits each.
  uint32_t scissor_tl = 0;
  uint32_t scissor_br = 0;

  // SHADER_CONSTANT_BOOL_000_031..224_255: bool i is bit (i & 31) of word i/32.
  uint32_t bool_words[8] = {};
};

XenosState DecodeXenosState(const DrawView& view);

// The sample count a guest MSAA mode stands for.
inline uint32_t MsaaSampleCount(xe::MsaaSamples msaa) {
  switch (msaa) {
    case xe::MsaaSamples::k2X: return 2;
    case xe::MsaaSamples::k4X: return 4;
    default: return 1;
  }
}

}  // namespace mcla::gpu

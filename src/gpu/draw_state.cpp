// MCLA GPU renderer -- the Xenos state of one draw, decoded. See draw_state.h.

#include "draw_state.h"

namespace mcla::gpu {

namespace {

namespace xg = rex::graphics;

// The colour target registers, by target (RB_COLOR_INFO, RB_COLOR1_INFO..).
constexpr uint32_t kColorInfoRegisters[kColorTargetCount] = {
    xg::XE_GPU_REG_RB_COLOR_INFO, xg::XE_GPU_REG_RB_COLOR1_INFO, xg::XE_GPU_REG_RB_COLOR2_INFO,
    xg::XE_GPU_REG_RB_COLOR3_INFO};
constexpr uint32_t kBlendControlRegisters[kColorTargetCount] = {
    xg::XE_GPU_REG_RB_BLENDCONTROL0, xg::XE_GPU_REG_RB_BLENDCONTROL1,
    xg::XE_GPU_REG_RB_BLENDCONTROL2, xg::XE_GPU_REG_RB_BLENDCONTROL3};

// D3D's effective window scissor (guest_device.h, block 0x2000 dwords 17/18).
constexpr uint32_t kDevEffectiveScissorTl = 10436;
constexpr uint32_t kDevEffectiveScissorBr = 10440;

template <typename T>
T Decode(const RegisterWindow& regs, uint32_t reg) {
  T value;
  value.value = regs.Register(reg);
  return value;
}

}  // namespace

XenosState DecodeXenosState(const DrawView& view) {
  const DrawRecord& r = view.record();
  const RegisterWindow regs = view.registers();
  XenosState s;

  const auto surface_info = Decode<xreg::RB_SURFACE_INFO>(regs, xg::XE_GPU_REG_RB_SURFACE_INFO);
  s.surface_pitch = surface_info.surface_pitch;
  s.msaa = surface_info.msaa_samples;

  const uint32_t color_mask = regs.Register(xg::XE_GPU_REG_RB_COLOR_MASK);
  for (uint32_t i = 0; i < kColorTargetCount; ++i) {
    ColorTargetState& t = s.color[i];
    const auto info = Decode<xreg::RB_COLOR_INFO>(regs, kColorInfoRegisters[i]);
    t.base_tiles = info.color_base | (info.color_base_bit_11 << 11);
    t.format = info.color_format;
    t.exp_bias = info.color_exp_bias;
    t.write_mask = (color_mask >> (4 * i)) & 0xFu;
    t.blend = Decode<xreg::RB_BLENDCONTROL>(regs, kBlendControlRegisters[i]);
    t.bound = r.surfaces[i] != 0 && t.write_mask != 0;
  }

  s.depth_control = Decode<xreg::RB_DEPTHCONTROL>(regs, xg::XE_GPU_REG_RB_DEPTHCONTROL);
  const auto depth_info = Decode<xreg::RB_DEPTH_INFO>(regs, xg::XE_GPU_REG_RB_DEPTH_INFO);
  s.depth_base_tiles = depth_info.depth_base | (depth_info.depth_base_bit_11 << 11);
  s.depth_format = depth_info.depth_format;
  s.depth_bound = r.surfaces[4] != 0 &&
                  (s.depth_control.z_enable || s.depth_control.z_write_enable ||
                   s.depth_control.stencil_enable);

  s.stencil = Decode<xreg::RB_STENCILREFMASK>(regs, xg::XE_GPU_REG_RB_STENCILREFMASK);
  s.stencil_bf = Decode<xreg::RB_STENCILREFMASK>(regs, xg::XE_GPU_REG_RB_STENCILREFMASK_BF);
  s.color_control = Decode<xreg::RB_COLORCONTROL>(regs, xg::XE_GPU_REG_RB_COLORCONTROL);
  s.mode = Decode<xreg::RB_MODECONTROL>(regs, xg::XE_GPU_REG_RB_MODECONTROL);
  s.sc_mode = Decode<xreg::PA_SU_SC_MODE_CNTL>(regs, xg::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
  s.clip = Decode<xreg::PA_CL_CLIP_CNTL>(regs, xg::XE_GPU_REG_PA_CL_CLIP_CNTL);
  s.vte = Decode<xreg::PA_CL_VTE_CNTL>(regs, xg::XE_GPU_REG_PA_CL_VTE_CNTL);
  s.vtx = Decode<xreg::PA_SU_VTX_CNTL>(regs, xg::XE_GPU_REG_PA_SU_VTX_CNTL);
  s.aa_config = regs.Register(xg::XE_GPU_REG_PA_SC_AA_CONFIG);
  s.aa_mask = regs.Register(xg::XE_GPU_REG_PA_SC_AA_MASK);

  s.viewport[0] = regs.RegisterFloat(xg::XE_GPU_REG_PA_CL_VPORT_XSCALE);
  s.viewport[1] = regs.RegisterFloat(xg::XE_GPU_REG_PA_CL_VPORT_XOFFSET);
  s.viewport[2] = regs.RegisterFloat(xg::XE_GPU_REG_PA_CL_VPORT_YSCALE);
  s.viewport[3] = regs.RegisterFloat(xg::XE_GPU_REG_PA_CL_VPORT_YOFFSET);
  s.viewport[4] = regs.RegisterFloat(xg::XE_GPU_REG_PA_CL_VPORT_ZSCALE);
  s.viewport[5] = regs.RegisterFloat(xg::XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
  s.poly_offset[0] = regs.RegisterFloat(xg::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
  s.poly_offset[1] = regs.RegisterFloat(xg::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
  s.poly_offset[2] = regs.RegisterFloat(xg::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
  s.poly_offset[3] = regs.RegisterFloat(xg::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
  s.alpha_ref = regs.RegisterFloat(xg::XE_GPU_REG_RB_ALPHA_REF);
  s.blend_constant[0] = regs.RegisterFloat(xg::XE_GPU_REG_RB_BLEND_RED);
  s.blend_constant[1] = regs.RegisterFloat(xg::XE_GPU_REG_RB_BLEND_GREEN);
  s.blend_constant[2] = regs.RegisterFloat(xg::XE_GPU_REG_RB_BLEND_BLUE);
  s.blend_constant[3] = regs.RegisterFloat(xg::XE_GPU_REG_RB_BLEND_ALPHA);

  s.scissor_tl = regs.DeviceDword(kDevEffectiveScissorTl);
  s.scissor_br = regs.DeviceDword(kDevEffectiveScissorBr);

  for (uint32_t i = 0; i < 8; ++i) {
    s.bool_words[i] = regs.Register(xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i);
  }
  return s;
}

}  // namespace mcla::gpu

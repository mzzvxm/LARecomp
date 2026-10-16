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

// A register read at an offset fixed at compile time: the
// decode used RegisterWindow::Register, which the compiler left out of line
// at most calls -- each one searched the nine register blocks again. Every
// register the decode reads lies in window R, checked here.
template <uint32_t kReg>
uint32_t Reg(const DrawRecord& r) {
  constexpr uint32_t offset = guest::RegisterDeviceOffset(kReg);
  static_assert(offset >= guest::kRegisterWindowBegin &&
                    offset + 4 <= guest::kRegisterWindowBegin + guest::kRegisterWindowBytes,
                "a register outside the draw's window R");
  return LoadBe32(r.registers + (offset - guest::kRegisterWindowBegin));
}

template <uint32_t kReg>
float RegFloat(const DrawRecord& r) {
  const uint32_t bits = Reg<kReg>(r);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

template <typename T, uint32_t kReg>
T Decode(const DrawRecord& r) {
  T value;
  value.value = Reg<kReg>(r);
  return value;
}

// A device dword of window R by offset, fixed at compile time.
template <uint32_t kOffset>
uint32_t DeviceDword(const DrawRecord& r) {
  static_assert(kOffset >= guest::kRegisterWindowBegin &&
                    kOffset + 4 <= guest::kRegisterWindowBegin + guest::kRegisterWindowBytes,
                "a device dword outside the draw's window R");
  return LoadBe32(r.registers + (kOffset - guest::kRegisterWindowBegin));
}

template <uint32_t kTarget>
void DecodeColorTarget(const DrawRecord& r, uint32_t color_mask, ColorTargetState& t) {
  const auto info = Decode<xreg::RB_COLOR_INFO, kColorInfoRegisters[kTarget]>(r);
  t.base_tiles = info.color_base | (info.color_base_bit_11 << 11);
  t.format = info.color_format;
  t.exp_bias = info.color_exp_bias;
  t.write_mask = (color_mask >> (4 * kTarget)) & 0xFu;
  t.blend = Decode<xreg::RB_BLENDCONTROL, kBlendControlRegisters[kTarget]>(r);
  t.bound = r.surfaces[kTarget] != 0 && t.write_mask != 0;
}

}  // namespace

XenosState DecodeXenosState(const DrawView& view) {
  const DrawRecord& r = view.record();
  XenosState s;

  const auto surface_info = Decode<xreg::RB_SURFACE_INFO, xg::XE_GPU_REG_RB_SURFACE_INFO>(r);
  s.surface_pitch = surface_info.surface_pitch;
  s.msaa = surface_info.msaa_samples;

  const uint32_t color_mask = Reg<xg::XE_GPU_REG_RB_COLOR_MASK>(r);
  static_assert(kColorTargetCount == 4);
  DecodeColorTarget<0>(r, color_mask, s.color[0]);
  DecodeColorTarget<1>(r, color_mask, s.color[1]);
  DecodeColorTarget<2>(r, color_mask, s.color[2]);
  DecodeColorTarget<3>(r, color_mask, s.color[3]);

  s.depth_control = Decode<xreg::RB_DEPTHCONTROL, xg::XE_GPU_REG_RB_DEPTHCONTROL>(r);
  const auto depth_info = Decode<xreg::RB_DEPTH_INFO, xg::XE_GPU_REG_RB_DEPTH_INFO>(r);
  s.depth_base_tiles = depth_info.depth_base | (depth_info.depth_base_bit_11 << 11);
  s.depth_format = depth_info.depth_format;
  s.depth_bound = r.surfaces[4] != 0 &&
                  (s.depth_control.z_enable || s.depth_control.z_write_enable ||
                   s.depth_control.stencil_enable);

  s.stencil = Decode<xreg::RB_STENCILREFMASK, xg::XE_GPU_REG_RB_STENCILREFMASK>(r);
  s.stencil_bf = Decode<xreg::RB_STENCILREFMASK, xg::XE_GPU_REG_RB_STENCILREFMASK_BF>(r);
  s.color_control = Decode<xreg::RB_COLORCONTROL, xg::XE_GPU_REG_RB_COLORCONTROL>(r);
  s.mode = Decode<xreg::RB_MODECONTROL, xg::XE_GPU_REG_RB_MODECONTROL>(r);
  s.sc_mode = Decode<xreg::PA_SU_SC_MODE_CNTL, xg::XE_GPU_REG_PA_SU_SC_MODE_CNTL>(r);
  s.clip = Decode<xreg::PA_CL_CLIP_CNTL, xg::XE_GPU_REG_PA_CL_CLIP_CNTL>(r);
  s.vte = Decode<xreg::PA_CL_VTE_CNTL, xg::XE_GPU_REG_PA_CL_VTE_CNTL>(r);
  s.vtx = Decode<xreg::PA_SU_VTX_CNTL, xg::XE_GPU_REG_PA_SU_VTX_CNTL>(r);
  s.aa_config = Reg<xg::XE_GPU_REG_PA_SC_AA_CONFIG>(r);
  s.aa_mask = Reg<xg::XE_GPU_REG_PA_SC_AA_MASK>(r);
  s.reset_index = Reg<xg::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX>(r);

  s.viewport[0] = RegFloat<xg::XE_GPU_REG_PA_CL_VPORT_XSCALE>(r);
  s.viewport[1] = RegFloat<xg::XE_GPU_REG_PA_CL_VPORT_XOFFSET>(r);
  s.viewport[2] = RegFloat<xg::XE_GPU_REG_PA_CL_VPORT_YSCALE>(r);
  s.viewport[3] = RegFloat<xg::XE_GPU_REG_PA_CL_VPORT_YOFFSET>(r);
  s.viewport[4] = RegFloat<xg::XE_GPU_REG_PA_CL_VPORT_ZSCALE>(r);
  s.viewport[5] = RegFloat<xg::XE_GPU_REG_PA_CL_VPORT_ZOFFSET>(r);
  s.poly_offset[0] = RegFloat<xg::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE>(r);
  s.poly_offset[1] = RegFloat<xg::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET>(r);
  s.poly_offset[2] = RegFloat<xg::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE>(r);
  s.poly_offset[3] = RegFloat<xg::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET>(r);
  s.alpha_ref = RegFloat<xg::XE_GPU_REG_RB_ALPHA_REF>(r);
  s.blend_constant[0] = RegFloat<xg::XE_GPU_REG_RB_BLEND_RED>(r);
  s.blend_constant[1] = RegFloat<xg::XE_GPU_REG_RB_BLEND_GREEN>(r);
  s.blend_constant[2] = RegFloat<xg::XE_GPU_REG_RB_BLEND_BLUE>(r);
  s.blend_constant[3] = RegFloat<xg::XE_GPU_REG_RB_BLEND_ALPHA>(r);

  s.scissor_tl = DeviceDword<kDevEffectiveScissorTl>(r);
  s.scissor_br = DeviceDword<kDevEffectiveScissorBr>(r);

  static_assert(sizeof(XenosState::bool_words) == 8 * sizeof(uint32_t));
  s.bool_words[0] = Reg<xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 0>(r);
  s.bool_words[1] = Reg<xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 1>(r);
  s.bool_words[2] = Reg<xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 2>(r);
  s.bool_words[3] = Reg<xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 3>(r);
  s.bool_words[4] = Reg<xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 4>(r);
  s.bool_words[5] = Reg<xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 5>(r);
  s.bool_words[6] = Reg<xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 6>(r);
  s.bool_words[7] = Reg<xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 7>(r);
  return s;
}

}  // namespace mcla::gpu

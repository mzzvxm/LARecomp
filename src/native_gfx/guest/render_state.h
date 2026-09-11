#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — guest render state
// ===========================================================================
// The D3DDevice keeps a shadow of the Xenos register file and flushes dirty
// blocks into the command buffer at draw time. The draw builder
// (sub_8241D620) shows which shadow block maps to which PM4 register base:
//
//   sub_82423548(dev, ..., 0x2000 region, dev + 10368)  RB_SURFACE_INFO...
//   sub_82423548(dev, ..., 0x2100,        dev + 10444)  RB_COLOR_MASK...
//   sub_82423548(dev, ..., 0x2180,        dev + 10528)
//   sub_82423548(dev, ..., 0x2200,        dev + 10548)  RB_DEPTHCONTROL...
//   sub_82423548(dev, ..., 0x2300,        dev + 10680)
//
// A register's shadow address is `block_base + (reg - pm4_base) * 4`.
// Two independent cross-checks in sub_82424670 confirm the mapping:
//   * dev+10580 is read with `& 7` and written 4 or 5 -> RB_MODECONTROL
//     (0x2208 = 10548 + 8*4), whose edram_mode field is exactly 3 bits;
//   * dev+10372 is assigned from a render target object's field -> that is
//     RB_COLOR_INFO (0x2001 = 10368 + 4), and it is later masked with
//     0xFFF0FFFF, i.e. the color_format field at bits 16..19.
//
// Register bit layouts come from rex/graphics/registers.h.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

// Shadow addresses, relative to the D3DDevice.
inline constexpr uint32_t kDevRegSurfaceInfo = 10368;   // RB_SURFACE_INFO   0x2000
inline constexpr uint32_t kDevRegColorInfo = 10372;     // RB_COLOR_INFO     0x2001
inline constexpr uint32_t kDevRegDepthInfo = 10376;     // RB_DEPTH_INFO     0x2002
// RB_COLOR1_INFO (0x2003) shares the 0x2000 shadow block with RB_COLOR_INFO, so
// it is two dwords past it. MCLA does use a second render target: the impostor
// bake (xPropFoliage__PSGenerateImposterNight) writes oC0 -- the tree's colour
// tile -- and oC1 -- the same tree's packed normal, n*0.5+0.5 -- and comes in
// with RB_COLOR_MASK = 0x000000FF, both targets fully write-enabled.
//
// Binding only target 0 loses oC1 outright, and the guest's resolve of target 1
// then hands target 0's pixels to the normal atlas's address. Measured on cam
// 44: all 59 of the 128x128 tiles in one frame reduce to 29 distinct images,
// each present twice, and the impostor shader's NormalSampler and DiffuseSampler
// sample byte-identical textures. Its unpack -- normalize(sample.zyx * 2 - 1) --
// then produces one constant direction for the whole canopy, which is the flat,
// uniformly lit foliage.
inline constexpr uint32_t kDevRegColorInfo1 = 10380;    // RB_COLOR1_INFO    0x2003
inline constexpr uint32_t kDevRegColorInfo2 = 10384;    // RB_COLOR2_INFO    0x2004
inline constexpr uint32_t kDevRegColorInfo3 = 10388;    // RB_COLOR3_INFO    0x2005

// RB_COLOR_INFO bits 20..25 are the render target's colour exponent bias, and
// the field is SIGNED, so a negative bias must sign-extend rather than read as
// a large positive. The bias exists because EDRAM's colour formats are fixed
// point: the output merger scales a shader's result by 2^bias on write and the
// resolve undoes it, which buys precision without a wider format. The game
// therefore uploads 2^-bias to its shaders (gInvColorExpBias) so they can
// pre-divide. Nothing in a D3D12 float render target does either half, so the
// runtime has to fold the scale back in -- see ApplyColorExpBias.
inline constexpr int32_t ColorExpBiasFromColorInfo(uint32_t color_info) {
  const uint32_t field = (color_info >> 20) & 0x3Fu;
  return int32_t(field << 26) >> 26;
}

// The bound target's bias on its own, for callers that need it without reading
// the whole render state. Kept here so there is one implementation of the guest
// read rather than a second copy beside the constant upload.
int32_t ReadColorExpBias(const uint8_t* base, uint32_t dev);
// PA_SC_SCREEN_SCISSOR_TL / _BR (0x200E / 0x200F) live in the SAME shadow block
// as RB_SURFACE_INFO, which spans 0x2000..0x2012 at dev+10368, so unlike
// PA_SC_WINDOW_SCISSOR (0x2081, in a block the draw builder never flushes) they
// are readable. Each holds two signed 15-bit fields: x at bits 0..14, y at bits
// 16..30.
//
// This is what actually bounds the drawn area when the guest uses a guard-band
// viewport: the Xenos allows a viewport far larger than the surface (scale 8192,
// i.e. 16384 wide) and relies on the scissor to clip. Deriving a target size
// from such a viewport produces nonsense.
inline constexpr uint32_t kDevRegScreenScissorTl = 10424;  // 0x200E
inline constexpr uint32_t kDevRegScreenScissorBr = 10428;  // 0x200F
inline constexpr uint32_t kDevRegColorMask = 10460;     // RB_COLOR_MASK     0x2104
inline constexpr uint32_t kDevRegDepthControl = 10548;  // RB_DEPTHCONTROL   0x2200
inline constexpr uint32_t kDevRegBlendControl0 = 10552; // RB_BLENDCONTROL0  0x2201
inline constexpr uint32_t kDevRegBlendControl1 = 10584; // RB_BLENDCONTROL1  0x2209
inline constexpr uint32_t kDevRegColorControl = 10556;  // RB_COLORCONTROL   0x2202
inline constexpr uint32_t kDevRegModeControl = 10580;   // RB_MODECONTROL    0x2208
inline constexpr uint32_t kDevRegPaSuScModeCntl = 10568;  // PA_SU_SC_MODE_CNTL 0x2205
// Offsets inside the 0x2100 block, whose shadow base is dev+10444:
// address = 10444 + (reg - 0x2100) * 4. RB_COLOR_MASK (0x2104) landing on
// 10460 is the check that pins the base.
// RB_BLEND_{RED,GREEN,BLUE,ALPHA} 0x2105..0x2108. Feeds OMSetBlendFactor,
// which is command list state, not part of the PSO: without it every
// CONSTANT_COLOR / CONSTANT_ALPHA blend factor silently reads (0,0,0,0).
inline constexpr uint32_t kDevRegBlendRed = 10464;
inline constexpr uint32_t kDevRegBlendGreen = 10468;
inline constexpr uint32_t kDevRegBlendBlue = 10472;
inline constexpr uint32_t kDevRegBlendAlpha = 10476;
inline constexpr uint32_t kDevRegStencilRefMask = 10496;    // RB_STENCILREFMASK  0x210D
inline constexpr uint32_t kDevRegStencilRefMaskBf = 10492;  // RB_STENCILREFMASK_BF 0x210C
// RB_ALPHA_REF (0x210E, float) is the reference value of the alpha test. The
// translated pixel shaders take it through SharedConstants.g_AlphaThreshold
// and compile the comparison as `discard(alpha < threshold)`, so the register
// has to be read: a hardcoded threshold cuts every UI element whose alpha
// falls on the wrong side of the guess.
inline constexpr uint32_t kDevRegAlphaRef = 10500;  // RB_ALPHA_REF 0x210E
// Polygon offset. Base 0x2300 shadows at dev+10680 and the block runs up to
// the vertex-declaration pointer at dev+11820 (0x2300..0x241B), so
// PA_SU_POLY_OFFSET_FRONT_SCALE (0x2380) lands at 10680 + 0x80*4.
inline constexpr uint32_t kDevRegPolyOffsetFrontScale = 11192;   // 0x2380
inline constexpr uint32_t kDevRegPolyOffsetFrontOffset = 11196;  // 0x2381
inline constexpr uint32_t kDevRegPolyOffsetBackScale = 11200;    // 0x2382
inline constexpr uint32_t kDevRegPolyOffsetBackOffset = 11204;   // 0x2383
// Viewport transform. The Xenos applies screen = ndc * scale + offset per
// axis; the translated shaders do NOT fold it in (unlike the SDK's own
// translator, which carries it in ndc_scale/ndc_offset system constants), so
// the host viewport has to reproduce it exactly.
inline constexpr uint32_t kDevRegVportXScale = 10504;   // PA_CL_VPORT_XSCALE  0x210F
inline constexpr uint32_t kDevRegVportXOffset = 10508;  // PA_CL_VPORT_XOFFSET 0x2110
inline constexpr uint32_t kDevRegVportYScale = 10512;   // PA_CL_VPORT_YSCALE  0x2111
inline constexpr uint32_t kDevRegVportYOffset = 10516;  // PA_CL_VPORT_YOFFSET 0x2112
inline constexpr uint32_t kDevRegVportZScale = 10520;   // PA_CL_VPORT_ZSCALE  0x2113
inline constexpr uint32_t kDevRegVportZOffset = 10524;  // PA_CL_VPORT_ZOFFSET 0x2114
inline constexpr uint32_t kDevRegVteCntl = 10572;       // PA_CL_VTE_CNTL      0x2206

// Everything that can change a D3D12 pipeline state, read straight out of
// the shadow. Kept as raw register values plus the decoded fields the PSO
// actually needs, so the key stays exact and debuggable.
struct GuestRenderState {
  uint32_t surface_info = 0;
  uint32_t color_info = 0;
  uint32_t depth_info = 0;
  uint32_t color_mask = 0;
  uint32_t depth_control = 0;
  uint32_t blend_control0 = 0;
  uint32_t color_control = 0;
  uint32_t color1_info = 0;
  // RB_BLENDCONTROL1 is 0x2209, in the 0x2200 block whose shadow base is
  // dev+10548: 10548 + (0x2209 - 0x2200) * 4.
  uint32_t blend_control1 = 0;
  // RB_COLORCONTROL bit 4. Xenos resolves alpha into the coverage mask, which is
  // how the game gets feathered foliage edges instead of a hard cutout. It only
  // means anything on a multisampled target (see mcla_native_gfx_msaa).
  bool alpha_to_mask_enable = false;
  uint32_t mode_control = 0;
  uint32_t pa_su_sc_mode_cntl = 0;

  // --- decoded
  uint32_t msaa_samples = 0;          // RB_SURFACE_INFO +16, 0 = 1x
  uint32_t color_format = 0;          // xenos::ColorRenderTargetFormat
  uint32_t color1_format = 0;         // RB_COLOR1_INFO +16, only when mrt is set
  // RB_COLOR_MASK carries one write-enable nibble per target; anything above
  // 0xF means the guest asked for a second colour target on this draw.
  bool mrt = false;
  int32_t color_exp_bias = 0;         // RB_COLOR_INFO bits 20..25, signed
  uint32_t depth_format = 0;          // xenos::DepthRenderTargetFormat
  bool depth_enable = false;
  bool depth_write = false;
  uint32_t depth_func = 0;            // xenos::CompareFunction
  bool stencil_enable = false;
  // Stencil operations. These MUST be filled when stencil_enable is set:
  // D3D12 validates FrontFace/BackFace and its enums start at 1, so a
  // zero-initialised D3D12_DEPTH_STENCILOP_DESC is rejected outright
  // (observed: CreateGraphicsPipelineState returning E_INVALIDARG with no
  // message until the stencil ops were filled in).
  uint32_t stencil_func = 0;      // RB_DEPTHCONTROL +8
  uint32_t stencil_fail = 0;      // +11
  uint32_t stencil_zpass = 0;     // +14
  uint32_t stencil_zfail = 0;     // +17
  bool backface_enable = false;   // +7
  uint32_t stencil_func_bf = 0;   // +20
  uint32_t stencil_fail_bf = 0;   // +23
  uint32_t stencil_zpass_bf = 0;  // +26
  uint32_t stencil_zfail_bf = 0;  // +29
  uint32_t stencil_ref = 0;       // RB_STENCILREFMASK +0
  uint32_t stencil_read_mask = 0xFF;   // +8
  uint32_t stencil_write_mask = 0xFF;  // +16
  bool alpha_test_enable = false;
  uint32_t alpha_func = 0;
  float alpha_ref = 0.0f;             // RB_ALPHA_REF
  bool cull_front = false;
  bool cull_back = false;
  bool front_face_is_cw = false;      // PA_SU_SC_MODE_CNTL.face
  // PA_SU_SC_MODE_CNTL +11/+12: polygon offset, applied to the D3D12
  // rasterizer's depth bias. Left at zero the decal/road-marking geometry
  // z-fights with the surface it is meant to sit on.
  bool poly_offset_front_enable = false;
  bool poly_offset_back_enable = false;
  float poly_offset_front_scale = 0.0f;
  float poly_offset_front_offset = 0.0f;
  float poly_offset_back_scale = 0.0f;
  float poly_offset_back_offset = 0.0f;
  uint32_t edram_mode = 0;            // RB_MODECONTROL +0, 3 bits

  float blend_constant[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // RGBA, for OMSetBlendFactor

  // Viewport, already resolved through PA_CL_VTE_CNTL's per-field enables
  // (a disabled scale reads as 1.0, a disabled offset as 0.0).
  // Screen scissor, already decoded. `scissor_valid` is false when the register
  // pair is degenerate (empty or inverted), which is how a never-written shadow
  // reads.
  bool scissor_valid = false;
  int32_t scissor_left = 0, scissor_top = 0, scissor_right = 0, scissor_bottom = 0;

  uint32_t vte_cntl = 0;
  float vport_x_scale = 1.0f, vport_x_offset = 0.0f;
  float vport_y_scale = 1.0f, vport_y_offset = 0.0f;
  float vport_z_scale = 1.0f, vport_z_offset = 0.0f;
};

// Guest viewport -> D3D12_VIEWPORT. The Xenos computes
//   screen = ndc * scale + offset
// while D3D12 computes
//   screen_x = ndc_x * (Width/2)  + (TopLeftX + Width/2)
//   screen_y = ndc_y * (-Height/2) + (TopLeftY + Height/2)
// so Width = 2*x_scale and Height = -2*y_scale. A standard D3D9 viewport has
// a NEGATIVE y_scale (that is where the NDC-y-up to screen-y-down flip lives),
// which yields a positive height here. `y_flipped` reports the opposite case,
// which D3D12 cannot express as a viewport (unlike Vulkan it rejects a
// negative height) and which reverses the screen-space winding.
struct HostViewport {
  float top_left_x = 0.0f, top_left_y = 0.0f;
  float width = 0.0f, height = 0.0f;
  float min_depth = 0.0f, max_depth = 1.0f;
  bool y_flipped = false;
};
HostViewport ComputeHostViewport(const GuestRenderState& s);

GuestRenderState ReadRenderState(const uint8_t* base, uint32_t dev);

// The value SharedConstants.g_AlphaThreshold has to carry so the generated
// pixel shader's `discard(alpha - threshold < 0)` -- i.e. keep alpha >= t --
// reproduces the guest's (alpha_func, alpha_ref) pair as closely as one
// threshold can. kGreaterEqual is exact, kGreater and kNotEqual (against a
// zero reference, which is what MCLA's composite pass uses) are exact up to
// one ULP, and the comparisons that cannot be expressed as a lower bound
// degrade to "keep everything" rather than to a guess that erases geometry.
float AlphaTestThreshold(const GuestRenderState& s);

// Sample count for the PSO: MsaaSamples is log2 of the count.
inline uint32_t SampleCountFromMsaa(uint32_t msaa_samples) { return 1u << msaa_samples; }

// Native formats for the Xenos EDRAM formats MCLA uses. Return
// DXGI_FORMAT_UNKNOWN (0) for anything unexpected so it fails loudly.
uint32_t ColorRenderTargetFormatToDxgi(uint32_t color_format);
uint32_t DepthRenderTargetFormatToDxgi(uint32_t depth_format);

}  // namespace mcla::native_gfx

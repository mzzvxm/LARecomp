#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest D3DDevice, as the capture reads it
// ===========================================================================
// Every offset the capture (guest_capture.cpp) and the decoders (draw_decode.cpp)
// use, with where it was read in the IDB of default.xex. The full research is
// facts here come from the
// disassembly where the decompiler failed ("local variable allocation has
// failed" in every draw builder).
//
// The device keeps a shadow of the Xenos register file that the draw builders
// flush into the command buffer. sub_82423548(dev, mask, base_register,
// shadow): for every run of set bits from the most significant end, bit i is
// register base_register + i AND dword i of the shadow. So the blocks are
// PACKED one after another in the device, not laid out by register number --
// kRegisterBlocks is the map, and RegisterDeviceOffset the only way to find a
// register. (The legacy runtime assumed register-number layout and reads
// PA_SU_POLY_OFFSET from dev+11192; the 0x2380 block is at dev+10832.)
//
// Threading: constants only.
// ===========================================================================

#include <cstdint>

#include "native_gfx/guest/guest_constants.h"
#include "native_gfx/guest/guest_resources.h"
#include "native_gfx/guest/render_state.h"
#include "native_gfx/vertex_declaration.h"

namespace mcla::gpu::guest {

// --- dirty header (sub_8241D620 disassembly: ld r4,0(r31) .. ld r27,0x20(r31))
// Every draw builder flushes the parts these qwords name and writes them back
// as zero, so they must be read BEFORE the original call.
inline constexpr uint32_t kDevVsConstantDirty = 0;   // 1 bit = 64 B of the VS bank, MSB first
inline constexpr uint32_t kDevPsConstantDirty = 8;   // same for the PS bank
inline constexpr uint32_t kDevStateDirty = 16;       // shaders, surfaces, blocks 0x2000/2100/2180/2200
inline constexpr uint32_t kDevFetchDirty = 24;       // 32 fetch groups, block 0x2280
inline constexpr uint32_t kDevMiscDirty = 32;        // blocks 0x2300/2380, clip planes, bools
inline constexpr uint32_t kDevSurfaceDirtyMask = 40; // ANDed with +16 to flush the surfaces

// --- fetch constants: 32 groups of 24 bytes (one texture fetch, or three
// vertex fetch pairs), flushed by sub_82423788.
inline constexpr uint32_t kDevFetchShadow = 1152;
inline constexpr uint32_t kFetchGroupBytes = 24;
inline constexpr uint32_t kFetchGroupCount = 32;
inline constexpr uint32_t kFetchShadowBytes = kFetchGroupBytes * kFetchGroupCount;  // 768

// --- ALU constant banks (sub_824238E0): 256 float4 each, flushed 64 B per
// dirty bit, bit 63 = registers 0..3.
inline constexpr uint32_t kDevVsConstantBank = 1920;
inline constexpr uint32_t kDevPsConstantBank = 6016;
inline constexpr uint32_t kConstantBankBytes = 4096;
inline constexpr uint32_t kConstantBlockBytes = 64;
inline constexpr uint32_t kConstantBlocksPerBank = 64;

// --- register shadow blocks, in device order.
struct RegisterBlock {
  uint32_t first_register;
  uint32_t count;          // registers present in the shadow
  uint32_t device_offset;  // where register `first_register` lives
};
inline constexpr RegisterBlock kRegisterBlocks[] = {
    // SHADER_CONSTANT_BOOL_000_031.. (8 dwords = 256 bools); bit 56 of +32.
    {0x4900, 8, 10112},
    // PA_CL_UCP_0_X .. PA_CL_UCP_5_W: six user clip planes, written in groups
    // of four by sub_82423608 from bits 49..54 of +32.
    {0x2388, 24, 10272},
    // RB_SURFACE_INFO.. The draw flush sends 16 of them (bits 42..57 of +16);
    // dwords 17/18 (dev+10436/10440) hold D3D's window scissor, which the clear
    // and the resolve read and write straight into the command buffer.
    {0x2000, 19, 10368},
    {0x2100, 21, 10444},  // bits 21..41 of +16 (RB_COLOR_MASK, blend, PA_CL_VPORT_*)
    {0x2180, 5, 10528},   // bits 12..16 of +16 (SQ_PROGRAM_CNTL..)
    {0x2200, 12, 10548},  // bits 0..11 of +16 (RB_DEPTHCONTROL, RB_MODECONTROL..)
    {0x2280, 21, 10596},  // bits 34..54 of +24
    {0x2300, 38, 10680},  // bits 0..37 of +32: PA_SC_LINE_CNTL .. RB_SAMPLE_COUNT_ADDR
    {0x2380, 8, 10832},   // bits 38..45 of +32: PA_SU_POLY_OFFSET_*, PA_CL_POINT_*
};

// Device offset of a register's shadow dword, or 0 when the device keeps none.
constexpr uint32_t RegisterDeviceOffset(uint32_t reg) {
  for (const RegisterBlock& b : kRegisterBlocks) {
    if (reg >= b.first_register && reg < b.first_register + b.count) {
      return b.device_offset + 4 * (reg - b.first_register);
    }
  }
  return 0;
}

// --- window R: every register the draw builders send, plus the 128 bytes
// between the bools and the clip planes (dev+10144..10272; by position the 32
// SHADER_CONSTANT_LOOP registers, which no draw flush reads -- not verified).
inline constexpr uint32_t kRegisterWindowBegin = 10112;
inline constexpr uint32_t kRegisterWindowEnd = 10864;
inline constexpr uint32_t kRegisterWindowBytes = kRegisterWindowEnd - kRegisterWindowBegin;  // 752

// --- window P: the device state a clear, resolve or tiling call reads before
// it runs: registers, D3D's flags (+10940..10943), the declaration, scissor
// enable, per-sampler bytes, viz query, surfaces, strides, textures, viewport,
// scissor, shaders, the surface cache and the whole tiling block up to the
// clear values BeginTiling stores (+13200..13223).
inline constexpr uint32_t kEventWindowBegin = 10112;
inline constexpr uint32_t kEventWindowEnd = 13224;
inline constexpr uint32_t kEventWindowBytes = kEventWindowEnd - kEventWindowBegin;  // 3112

// --- device fields
inline constexpr uint32_t kDevFlags = 10940;              // 4 D3D flag bytes: +0 bit0 predicated tiling,
                                                          // 0x10/0x20 tiling, 0x40 Z pass
inline constexpr uint32_t kDevVertexDeclaration = 11820;  // sub_82424670
inline constexpr uint32_t kDevScissorEnable = 11856;      // sub_82418E48
inline constexpr uint32_t kDevVizQueryTable = 12122;      // byte per index (draw packet dword 0)
inline constexpr uint32_t kDevVizQueryIndex = 12187;      // byte; non-zero = query on
inline constexpr uint32_t kDevIndexBuffer = 12436;        // sub_8241D620
inline constexpr uint32_t kDevColorSurfaces = 12440;      // 4 pointers
inline constexpr uint32_t kDevDepthSurface = 12456;
inline constexpr uint32_t kSurfaceCount = 5;              // colour 0..3, depth
inline constexpr uint32_t kDevStreamStrides = 12528;      // byte per stream, in dwords
inline constexpr uint32_t kStreamCount = 16;
inline constexpr uint32_t kDevSamplerTextures = 12544;    // SetTexture sub_82410F88
inline constexpr uint32_t kSamplerCount = 26;             // ends where the viewport starts
inline constexpr uint32_t kDevViewport = 12648;           // floats X, Y, W, H (sub_82418E48)
inline constexpr uint32_t kDevScissorRect = 12676;        // x0, y0, x1, y1
inline constexpr uint32_t kDevPixelShader = 12692;
inline constexpr uint32_t kDevVertexShader = 12696;
inline constexpr uint32_t kDevSurfaceCache = 12728;       // last flushed surfaces, 5 pointers
inline constexpr uint32_t kDevTileCount = 12748;          // BeginTiling sub_8241BE78
inline constexpr uint32_t kDevTileRects = 12752;          // up to 15 x 16 B
inline constexpr uint32_t kMaxTiles = 15;
inline constexpr uint32_t kDevTilingExtent = 13180;       // max x1, max y1 over the tiles

// --- guest objects
inline constexpr uint32_t kSurfaceHeaderBytes = 48;   // D3DSurface (memory: project_mcla_edram_surface_model)
inline constexpr uint32_t kTextureHeaderBytes = 52;   // D3DBaseTexture, fetch constant at +28..+51
inline constexpr uint32_t kTextureFetchOffset = 28;
inline constexpr uint32_t kIndexBufferBytes = 28;     // +0 flags (bit 31 = 32-bit, 30..29 endian), +24 address
inline constexpr uint32_t kIndexBufferAddressOffset = 24;
inline constexpr uint32_t kDeclElementBytes = 12;     // D3DVERTEXELEMENT9, big-endian
inline constexpr uint32_t kMaxDeclElements = 64;      // the legacy bound; MCLA's largest is far below

// Shader objects (sub_82424670).
//   VS: variant = (no PS bound && u32(vs+872) & 0x20) ? 1 : 0
//       off = u32(vs + 896 + 8*variant); address = u32(vs+off+872) + u32(vs+32);
//       size = u32(vs+off+876)
//   PS: sub = ps + u32(ps+64); address = u32(sub+40) + u32(ps+24); size = u32(sub+44)
inline constexpr uint32_t kVsVariantFlags = 872;
inline constexpr uint32_t kVsVariantFlagHasDepthOnly = 0x20;
inline constexpr uint32_t kVsVariantTable = 896;
inline constexpr uint32_t kVsVariantStride = 8;
inline constexpr uint32_t kVsUcodeAddress = 872;  // relative to vs + off
inline constexpr uint32_t kVsUcodeSize = 876;
inline constexpr uint32_t kVsUcodeBase = 32;
inline constexpr uint32_t kPsSubObject = 64;
inline constexpr uint32_t kPsUcodeAddress = 40;  // relative to the sub-object
inline constexpr uint32_t kPsUcodeSize = 44;
inline constexpr uint32_t kPsUcodeBase = 24;

// D3DDevice_Resolve takes ClearStencil and pParameters on the caller's stack:
// GPR argument slot k sits at sp + 0x14 + 8k (both EndTiling call sites:
// stw r24, 0x5C(r1) / stw r23, 0x64(r1)).
inline constexpr uint32_t kResolveStencilStackOffset = 0x5C;
inline constexpr uint32_t kResolveParamsStackOffset = 0x64;

// The legacy constants that name the same fields agree (one source of truth).
static_assert(kDevVertexShader == mcla::native_gfx::kDevVertexShaderOffset);
static_assert(kDevPixelShader == mcla::native_gfx::kDevPixelShaderOffset);
static_assert(kDevSamplerTextures == mcla::native_gfx::kDevSamplerTextureOffset);
static_assert(kDevFetchShadow == mcla::native_gfx::kDevFetchShadowOffset);
static_assert(kFetchGroupCount == mcla::native_gfx::kFetchShadowGroups);
static_assert(kDevVsConstantBank == mcla::native_gfx::kDevVsConstantBankOffset);
static_assert(kDevPsConstantBank == mcla::native_gfx::kDevPsConstantBankOffset);
static_assert(kDevVsConstantDirty == mcla::native_gfx::kDevVsConstantDirtyOffset);
static_assert(kDevPsConstantDirty == mcla::native_gfx::kDevPsConstantDirtyOffset);
static_assert(kConstantBankBytes == mcla::native_gfx::kAluBankBytes);
static_assert(kDevVertexDeclaration == mcla::native_gfx::kDevVertexDeclarationOffset);
static_assert(kDevStreamStrides == mcla::native_gfx::kDevStreamStrideTableOffset);
static_assert(kDevColorSurfaces == mcla::native_gfx::kDevColorSurface0);
static_assert(kDevDepthSurface == mcla::native_gfx::kDevDepthSurface);
// Registers the legacy reads inside the flushed part of a block, where its
// register-number arithmetic and the packed map coincide.
static_assert(RegisterDeviceOffset(0x2000) == mcla::native_gfx::kDevRegSurfaceInfo);
static_assert(RegisterDeviceOffset(0x2001) == mcla::native_gfx::kDevRegColorInfo);
static_assert(RegisterDeviceOffset(0x2002) == mcla::native_gfx::kDevRegDepthInfo);
static_assert(RegisterDeviceOffset(0x2104) == mcla::native_gfx::kDevRegColorMask);
static_assert(RegisterDeviceOffset(0x2200) == mcla::native_gfx::kDevRegDepthControl);
static_assert(RegisterDeviceOffset(0x2202) == mcla::native_gfx::kDevRegColorControl);
static_assert(RegisterDeviceOffset(0x2208) == mcla::native_gfx::kDevRegModeControl);
static_assert(RegisterDeviceOffset(0x2302) == mcla::native_gfx::kDevRegPaSuVtxCntl);
static_assert(RegisterDeviceOffset(0x2380) == 10832, "PA_SU_POLY_OFFSET_FRONT_SCALE (packed map)");
static_assert(kRegisterWindowBegin == kRegisterBlocks[0].device_offset);
static_assert(kRegisterWindowEnd == 10832 + 4 * 8, "window R ends with the 0x2380 block");
static_assert(kDevSamplerTextures + 4 * kSamplerCount == kDevViewport);

// Registers decoded from window R (xenos register numbers, rex/graphics/register_table.inc).
namespace reg {
inline constexpr uint32_t kRbSurfaceInfo = 0x2000;
inline constexpr uint32_t kRbColorInfo = 0x2001;
inline constexpr uint32_t kRbDepthInfo = 0x2002;
inline constexpr uint32_t kRbColor1Info = 0x2003;  // 2 and 3 follow
inline constexpr uint32_t kRbColorMask = 0x2104;
inline constexpr uint32_t kRbDepthControl = 0x2200;
inline constexpr uint32_t kRbColorControl = 0x2202;
inline constexpr uint32_t kPaSuScModeCntl = 0x2205;
inline constexpr uint32_t kRbModeControl = 0x2208;
inline constexpr uint32_t kPaSuPolyOffsetFrontScale = 0x2380;
inline constexpr uint32_t kRbSampleCountCtl = 0x2324;
}  // namespace reg

}  // namespace mcla::gpu::guest

#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the shader contract, as hard errors
// ===========================================================================
// A port, check for check, of what native/tools/build_shader_pack_v8.py (mzzvxm)
// verifies on the HLSL and on the compiled blobs. The renderer (src/gpu) reads
// the blobs with this contract in mind, and a translator or dxc bump that breaks
// it must fail the build loudly instead of producing a pack that renders
// wrong. Every function appends one line per problem to `problems`, starting
// with the shader's name, and returns true when it found none.
//
// What is checked, and where it comes from in the Python:
//   HLSL text   collect loop of main(): the v4..v7 header marks, the 8-sample
//               branch (v8), no host-pixel position prologue, precise position
//               and depth, the alpha rounding, declared outputs vs the
//               container, the conditional SV_Coverage block, oC0 => spec 0x2.
//   control flow  check_tree(): no pc machine (the report's control_flow column;
//               "dxil=ok spirv=ok" is the compile itself succeeding).
//   blobs       the contract pass: SV_Coverage in the DXIL and BuiltIn
//               SampleMask in the SPIR-V exactly for pixel shader variant 1 of
//               shaders that write oC0; no SPIR-V specialization constant.
//   position    SPIR-V: every BuiltIn Position variable is Invariant and the
//               module has NoContraction; DXIL: no `fast` fp op reaches
//               SV_Position (from the disassembly).
//
// The HDR variant pack (build_hdr_variants.py) has its own, smaller checks;
// they belong to its step, not here.
//
// Pure functions on bytes and text; the one that disassembles takes the
// worker. No logging.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dxc.h"

namespace larecomp::shaderpack {

using Problems = std::vector<std::string>;

// ---------------------------------------------------------------------------
// HLSL text (build_shader_pack_v8.py main(): the per-shader text checks)
// ---------------------------------------------------------------------------

struct HlslCheckInput {
  std::string_view name;  // "group__entry": starts every message
  std::string_view hlsl;  // exactly the text that gets compiled
  ShaderStage stage = ShaderStage::kPixel;
  uint32_t spec_mask = 0;   // the container's spec constants mask (the variant 1 value)
  uint32_t ps_outputs = 0;  // PixelShader::outputs: pack flags >> 8 & 0x1F (colour 0..3 = bits 0..3, depth = bit 4); pixel shaders only
};

// *rounds_alpha (optional) is set when this shader's source carries the alpha
// to coverage function with its rounding line (the Python counts those).
bool CheckHlslText(const HlslCheckInput& input, Problems* problems, bool* rounds_alpha = nullptr);

// Mirrors LAR-XenosRecomp's ControlFlowForm (the library API maps its enum onto this).
enum class ControlFlowForm : uint8_t { kFlat, kStructured, kPcMachine };

// The pc machine crashes vkd3d-proton's translator; no shader of the contract uses it.
bool CheckControlFlow(std::string_view name, ControlFlowForm form, Problems* problems);

// ---------------------------------------------------------------------------
// Blobs
// ---------------------------------------------------------------------------

struct BlobContractInput {
  std::string_view name;
  ShaderStage stage = ShaderStage::kPixel;
  int variant = 0;          // 0: XENOS_SPEC_CONSTANTS=0, 1: the whole mask
  bool writes_oc0 = false;  // pixel shader whose container lists colour 0 (ps_outputs bit 0)
  // The blobs of this variant. An empty span (a single-format build) skips that format.
  std::span<const uint8_t> dxil;
  std::span<const uint8_t> spirv;
};
bool CheckBlobContract(const BlobContractInput& input, Problems* problems);

struct SpirvTraits {
  bool writes_sample_mask = false;   // OpDecorate .. BuiltIn SampleMask
  bool has_spec_constants = false;   // OpDecorate .. SpecId, or an OpSpecConstant* op
};
// False with *error set on a malformed module ("malformed instruction at word N").
bool ReadSpirvTraits(std::span<const uint8_t> module, SpirvTraits* traits, std::string* error);

struct SpirvPositionTraits {
  bool position_invariant = false;  // BuiltIn Position variable(s) present and all decorated Invariant
  uint32_t no_contraction = 0;      // number of NoContraction decorations
};
bool ReadSpirvPositionTraits(std::span<const uint8_t> module, SpirvPositionTraits* traits,
                             std::string* error);

// Vertex shader blobs: Invariant position and at least one NoContraction.
bool CheckSpirvPosition(std::string_view name, int variant, std::span<const uint8_t> module,
                        Problems* problems);

struct DxilPositionStats {
  uint32_t fp_ops = 0;    // fadd/fsub/fmul/fdiv/frem/fneg that feed SV_Position
  uint32_t fast_ops = 0;  // how many of them carry `fast`
};
// The scan of `dxc -dumpbin`'s text: roots are the storeOutput.f32(i32 5, i32 0,
// i32 0, i8 c, float %x operands (SV_Position), walked through the %n = ...
// definitions.
DxilPositionStats DxilPositionFastOps(std::string_view disassembly);

// Fails when any such op is `fast`, or none is found at all.
bool CheckDxilPositionText(std::string_view name, int variant, std::string_view disassembly,
                           Problems* problems, DxilPositionStats* stats = nullptr);
// The same on a DXIL container, disassembled through the worker.
bool CheckDxilPosition(DxcWorker& worker, std::string_view name, int variant,
                       std::span<const uint8_t> dxil, Problems* problems,
                       DxilPositionStats* stats = nullptr);

}  // namespace larecomp::shaderpack

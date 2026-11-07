#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the container to HLSL step
// ===========================================================================
// The translator is LAR-XenosRecomp (the library target XenosRecompLib, interface
// XenosRecomp/xenos_recomp.h), which turns one Xbox 360 shader container into the HLSL the
// renderer's contract is written against. The text starts with the translator's shader_common.h
// (embedded in the library) and is exactly what the generator's `--hlsl` mode writes to a file: the
// pack's blobs are compiled from it as it is.
//
// Stateless: any number of workers can translate at once (RecompileContainer keeps no state).
// ===========================================================================

#include <cstdint>
#include <span>
#include <string>

#include "shaderpack/contract_checks.h"
#include "shaderpack/dxc.h"

namespace larecomp::shaderpack {

struct TranslatedShader {
  std::string hlsl;
  uint32_t spec_mask = 0;  // the shader's specialization constants (the variant 1 value; 0: one variant)
  ShaderStage stage = ShaderStage::kPixel;
  ControlFlowForm control_flow = ControlFlowForm::kFlat;
};

// False with *error set when the container is not one the translator can use.
bool TranslateContainer(std::span<const uint8_t> container, TranslatedShader* out, std::string* error);

// The translator's revision (a git hash, or "unknown") and the XXH3-64 of its embedded
// shader_common.h: the stamp records both, so a bump of either rebuilds the packs.
std::string GeneratorRevision();
uint64_t ShaderCommonHash();

}  // namespace larecomp::shaderpack

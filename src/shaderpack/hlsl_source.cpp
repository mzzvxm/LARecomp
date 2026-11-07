// LARecomp shader pack builder -- the container to HLSL step (see hlsl_source.h).

#include "shaderpack/hlsl_source.h"

#include <utility>

#include "XenosRecomp/xenos_recomp.h"
#include "shaderpack/hash.h"

namespace larecomp::shaderpack {

// The HLSL is the translator's output with its embedded shader_common.h: the bytes of that file in
// the LAR-XenosRecomp checkout, which marks it eol=crlf so every checkout embeds the same text (the
// stamp's shader_common_xxh3 records it). LARecomp links the SDK's fmt (12.1.0) and the translator's
// own submodule has 11.0.2: the packs come out byte for byte the same with either.
bool TranslateContainer(std::span<const uint8_t> container, TranslatedShader* out, std::string* error) {
  *out = TranslatedShader{};
  xenos_recomp::RecompiledShader result;
  if (!xenos_recomp::RecompileContainer(container.data(), container.size(),
                                        xenos_recomp::ShaderCommonText(), result)) {
    *error = "the translator rejected the container (malformed, or it refers to something the "
             "container does not declare)";
    return false;
  }
  out->hlsl = std::move(result.hlsl);
  out->spec_mask = result.specConstantsMask;
  out->stage = result.isPixelShader ? ShaderStage::kPixel : ShaderStage::kVertex;
  // xenos_recomp.h's enum is a global one; contract_checks.h mirrors it.
  switch (result.controlFlowForm) {
    case ::ControlFlowForm::Flat:
      out->control_flow = ControlFlowForm::kFlat;
      break;
    case ::ControlFlowForm::Structured:
      out->control_flow = ControlFlowForm::kStructured;
      break;
    case ::ControlFlowForm::PcMachine:
      out->control_flow = ControlFlowForm::kPcMachine;
      break;
  }
  return true;
}

std::string GeneratorRevision() {
  return xenos_recomp::Revision();
}

uint64_t ShaderCommonHash() {
  return Xxh3(xenos_recomp::ShaderCommonText());
}

}  // namespace larecomp::shaderpack

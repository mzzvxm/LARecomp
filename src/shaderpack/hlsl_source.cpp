// LARecomp shader pack builder -- the container to HLSL step (see hlsl_source.h).

#include "shaderpack/hlsl_source.h"

#include <utility>

#include "XenosRecomp/xenos_recomp.h"
#include "shaderpack/hash.h"

namespace larecomp::shaderpack {

// CHECK: the HLSL is the translator's output with its embedded shader_common.h, which is the bytes of
// that file in the LAR-XenosRecomp checkout (feat/library-api marks it eol=crlf so a Windows checkout
// and the devs' tree agree). A checkout with LF endings embeds different bytes: the HLSL differs
// from the Python chain's tree, and with it every blob (the stamp's shader_common_xxh3 tells them apart).
// CHECK: LARecomp links its own fmt (the SDK's 12.1.0), the translator was validated against its
// submodule's 11.0.2; the HLSL has to come out byte for byte the same (compare_packs.py shows it).
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

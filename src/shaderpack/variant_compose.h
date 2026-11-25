#pragma once
// ===========================================================================
// LARecomp shader pack builder -- composing the stages of a shader (the chain's driver)
// ===========================================================================
// ComposeStages applies the transforms of a set of features to one shader's translated text, in chain order (ascending feature
// index, variant_chain.h), each on the text the one before left, and hands each the place of its constants block in THIS
// permutation (StageContext). A stage alone is called with its own bit and its block at 608, which is what version 2 of the
// pack did, so the text is byte for byte the same. Pure: no compiler, no I/O (the builder, variant_pack.cpp, compiles the
// result; the tests call it on fixtures and on the retail text).
// ===========================================================================

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// The stage of `feature` among `defs`, or null.
inline const VariantDef* FindStage(std::span<const VariantDef> defs, uint32_t feature) {
  for (const VariantDef& def : defs) {
    if (def.feature == feature) return &def;
  }
  return nullptr;
}

// The text of `shader` after the stages of `features` (a set of the features of `defs`, composable in `table`'s rule).
// `ok`: every stage applied. `skip`: a stage said the shader is not one its edits are for (the composition does not exist for it),
// with its reason. Else `error` names the stage that failed and the stages that were applied before it.
inline VariantTransformResult ComposeStages(std::span<const VariantDef> defs, uint32_t features, const VariantShader& shader,
                                            std::string_view hlsl, std::span<const chain::FeatureInfo> table = chain::kFeatures) {
  VariantTransformResult out;
  if (features == 0) {
    out.error = "no stage to apply";
    return out;
  }
  uint32_t known = 0;
  for (const chain::FeatureInfo& row : table) known |= chain::FeatureBit(row.feature);
  if ((features & ~known) != 0) {
    out.error = std::format("{}: the features {:#x} are not in the chain's table", shader.name, features & ~known);
    return out;
  }
  std::string text(hlsl);
  std::string applied;
  for (const chain::FeatureInfo& row : table) {
    if ((features & chain::FeatureBit(row.feature)) == 0) continue;
    const VariantDef* def = FindStage(defs, row.feature);
    if (!def || !def->transform) {
      out.error = std::format("{}: no stage for the feature {} ({})", shader.name, row.feature, row.name);
      return out;
    }
    const VariantTransformResult step = def->transform(shader, text, ContextIn(row.feature, features, table));
    if (!step.ok) {
      out.skip = step.skip;
      out.error = applied.empty() ? step.error : std::format("{} (after {})", step.error, applied);
      return out;
    }
    text = step.hlsl;
    applied += (applied.empty() ? "" : ", ") + std::string(row.name);
  }
  out.ok = true;
  out.hlsl = std::move(text);
  return out;
}

}  // namespace larecomp::shaderpack

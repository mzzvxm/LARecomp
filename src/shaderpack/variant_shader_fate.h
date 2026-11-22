#pragma once
// ===========================================================================
// LARecomp shader pack builder -- what becomes of one shader when a stage's edits are applied to its text
// ===========================================================================
// A stage (a VariantDef) is applied to every shader it names. For each one its transform answers one of three things, and the builder (variant_pack.cpp BuildVariant) acts on the answer for THAT
// shader alone:
//
//   built     the edits fit: the shader gets the stage's variant.
//   left out  the transform looked and the shader is not one the edits are for (`skip`: a shadow collector shader with no lookup, a lit shader with no per pixel light). Counted by reason in the summary line.
//   failed    the edits do not fit this text (`!ok`, not `skip`: an anchor not found, an input the text does not declare, a recipe the game's text is not the one it was written for). Since GB-H2-FOLLOW
//             (the review's S5) this costs that SHADER its variant and nothing else: it keeps the game's own shader for the stage, the rest of the stage's shaders still get theirs, and the builder says
//             it in one log line per failed shader and in the stamp. Before, one such shader failed the whole stage ("the shader variant gbuffer was not built", all 142 shaders), so on a game build
//             other than the retail one (a title update, another translator) one text lost the planes of every shader. The reason this is acceptable is the same as for a left out shader: the draw of
//             a shader with no variant keeps working with the game's own text; what the stage must never do is write a QUIET wrong value, and a failed shader writes nothing.
//
// A stage with no shader that could be built (every one left out or failed) is still a failure of the stage, with the first reason: that is a stage written for another game, not a shader that
// does not fit. A compile that fails, and a cancel, are not decided here: they stop the stage as before.
//
// Pure: no compiler, no I/O (variant_chain_test and gbuffer_night_test call it).
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

enum class ShaderFate : uint8_t { kBuilt, kLeftOut, kFailed };

inline ShaderFate FateOf(const VariantTransformResult& result) {
  return result.ok ? ShaderFate::kBuilt : (result.skip ? ShaderFate::kLeftOut : ShaderFate::kFailed);
}

// The one log line of a shader whose edits did not fit (the builder logs it as a warning, once per failed shader).
inline std::string FailedShaderLine(std::string_view stage, std::string_view shader, std::string_view key_hex, std::string_view why) {
  return std::format("shader variants: the {} variant was not built for {} [{}]: {}; that shader keeps the game's own shader for it and the others still get the variant", stage, shader, key_hex, why);
}

// What one stage did with its shaders, as the builder goes along.
struct ShaderFateTally {
  size_t built = 0;
  std::map<std::string, size_t> left_out;  // reason -> how many shaders were left out for it
  std::vector<std::string> failed;         // "<shader> [<key>]: <why>", one per failed shader, in the order met

  // Records one shader's answer and says which of the three it was. The caller logs a failed shader with FailedShaderLine.
  ShaderFate Note(std::string_view shader, std::string_view key_hex, const VariantTransformResult& result) {
    const ShaderFate fate = FateOf(result);
    switch (fate) {
      case ShaderFate::kBuilt: ++built; break;
      case ShaderFate::kLeftOut: ++left_out[result.error]; break;
      case ShaderFate::kFailed: failed.push_back(std::format("{} [{}]: {}", shader, key_hex, result.error)); break;
    }
    return fate;
  }
  bool any_failed() const { return !failed.empty(); }
  // For the summary line: "; 3 left out: why; ..." and "; 1 failed: a [key]: why" (empty when there is nothing to say).
  std::string NoteText() const {
    std::string text;
    for (const auto& [why, count] : left_out) text += std::format("; {} left out: {}", count, why);
    if (!failed.empty()) {
      text += std::format("; {} failed (no variant for them)", failed.size());
      for (const std::string& line : failed) text += "; " + line;
    }
    return text;
  }
  // The stamp's informational line (variants_<stage>_failed_shaders): the shaders that have no variant because their text did not fit.
  std::string StampText() const {
    std::string text;
    for (const std::string& line : failed) text += (text.empty() ? "" : " | ") + line;
    for (char& c : text) {
      if (c == '\n' || c == '\r') c = ' ';  // the stamp is one line a key
    }
    return text;
  }
};

}  // namespace larecomp::shaderpack

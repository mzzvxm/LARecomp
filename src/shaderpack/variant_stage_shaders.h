#pragma once
// ===========================================================================
// LARecomp shader pack builder -- one stage applied to each of its shaders (the loop of variant_pack.cpp BuildVariant)
// ===========================================================================
// The loop that turns a stage's shaders into pack entries lives here, with the three things it needs from outside given as hooks, so a test can run it with a fake translator and a fake
// compiler (variants/gbuffer_night_test.cpp): the real hooks are TranslateContainer, CompileChecked (dxc, variant_pack.cpp) and the log.
//
// What becomes of each shader is its own (variant_shader_fate.h):
//   - the stage's edits do not fit its text        -> no variant for it (a warning), the others go on                  (GB-H2-FOLLOW, the review's S5)
//   - its edited text does not compile (or breaks the blob contract the runtime relies on)
//                                                 -> no variant for it, the same warning with dxc's first error line;   (GB-H2-DXC)
//                                                    the others go on. A shader with an alpha test twin has both or neither: a twin that does not compile
//                                                    takes the variant 0 entry of the same shader with it, so the pack never has half a shader.
//   - the compile could not be tried or must not go on (a cancel; no usable compiler: dxil.dll or dxcompiler missing, an unsigned DXIL, a blob that is not DXIL / SPIR-V)
//                                                 -> the stage stops, as before: that is the build, not one shader. The step still succeeds without this stage (variant_pack.cpp).
// A stage with no shader made at all (every one left out or failed) is still a failure of the stage, with the first reason (the caller).
//
// A partial set is a SUCCESS with warnings, never a failure: the step returns `built`, the builder reports no error to the boot popup (the popup shows a phase and the shader being done;
// each failed shader still takes its progress step), the warnings go to the log and to logs/shader_build.txt, and the stamp names the shaders (variants_<stage>_failed_shaders). The renderer
// draws the game's own shader for a shader with no variant.
//
// Pure: no compiler, no I/O (the hooks do that).
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shaderpack/builder.h"
#include "shaderpack/hash.h"
#include "shaderpack/hlsl_source.h"
#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_pack.h"
#include "shaderpack/variant_pack_format.h"
#include "shaderpack/variant_shader_fate.h"

namespace larecomp::shaderpack {

// A shader of a stage and where its container is.
struct Target {
  VariantShader shader;
  const ShaderSource* source = nullptr;
};

// What one stage made for one shader on its own: the composition pass builds from these.
struct SoloShader {
  uint32_t feature = 0;
  uint64_t key = 0;
  bool vertex = false;
  uint32_t spec_mask = 0;  // the shader's spec mask
  VariantShader shader;
  const ShaderSource* source = nullptr;
};

struct StageBlobs {
  std::vector<uint8_t> dxil;
  std::vector<uint8_t> spirv;
};

// How one compile of a text ended.
//   kOk      the blobs are in place (the formats asked, checked)
//   kFailed  THIS text does not compile, or its blobs break the contract the runtime relies on: the shader loses its variant
//   kStop    not this shader's doing: a cancel, or a compiler that cannot be used at all (not found, not able to sign, producing something that is not a DXIL / SPIR-V blob)
enum class CompileVerdict : uint8_t { kOk, kFailed, kStop };

struct StageHooks {
  // The translator's text of a shader's container (TranslateContainer).
  std::function<bool(const ShaderSource& source, TranslatedShader* out, std::string* error)> translate;
  // One compile of `hlsl` with the spec value (0, or the shader's whole mask) for every format asked, and the checks of the blobs (CompileChecked). `label` decorates the messages.
  std::function<CompileVerdict(const std::string& hlsl, uint32_t spec, const std::string& label, StageBlobs* blobs, std::string* error)> compile;
  // One line for the log, a warning.
  std::function<void(const std::string& line)> warn;
};

// The first line of a compiler's message that says what is wrong: the first one with "error:" in it (dxc: "<file>:<line>:<col>: error: ..."), else the first line that is not empty (the header
// line "dxc failed for <shader> (...):" when dxc gave no diagnostics). At most `max_chars` of it (an overlong line is cut and ends with "...").
inline std::string FirstErrorLine(std::string_view text, size_t max_chars = 240) {
  std::string_view first;
  std::string_view chosen;
  while (!text.empty()) {
    const size_t eol = text.find('\n');
    std::string_view line = text.substr(0, eol);
    text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty()) continue;
    if (first.empty()) first = line;
    if (line.find("error:") != std::string_view::npos) {
      chosen = line;
      break;
    }
  }
  if (chosen.empty()) chosen = first;
  std::string out(chosen);
  if (out.size() > max_chars) {
    out.resize(max_chars > 3 ? max_chars - 3 : max_chars);
    out += "...";
  }
  return out;
}

// The compiles of one stage, once per text: the same edited text gives the same blobs (the shadow collector's 147 shaders are four texts). A text that failed is remembered too, so the shaders
// that share it fail at once instead of asking the compiler again. Spec variant 0 and the spec variant 1 twin are kept apart (the twin's key adds the shader's spec mask).
class StageCompiles {
 public:
  explicit StageCompiles(const StageHooks& hooks) : hooks_(hooks) {}

  // *blobs points into the cache (valid until this object goes). On kFailed / kStop *error says why.
  CompileVerdict Get(const std::string& hlsl, uint32_t spec, const std::string& label, const StageBlobs** blobs, std::string* error) {
    const bool twin = spec != 0;
    const std::string key = twin ? hlsl + std::format("\n//spec {}", spec) : hlsl;
    auto& done = twin ? twin_ : zero_;
    if (const auto found = done.find(key); found != done.end()) {
      *blobs = &found->second;
      return CompileVerdict::kOk;
    }
    auto& bad = twin ? twin_failed_ : zero_failed_;
    if (const auto found = bad.find(key); found != bad.end()) {
      *error = found->second;
      return CompileVerdict::kFailed;
    }
    StageBlobs made;
    const CompileVerdict verdict = hooks_.compile(hlsl, spec, label, &made, error);
    if (verdict == CompileVerdict::kOk) {
      *blobs = &done.emplace(key, std::move(made)).first->second;
    } else if (verdict == CompileVerdict::kFailed) {
      bad.emplace(key, *error);
    }
    return verdict;
  }
  size_t compiles() const { return zero_.size(); }
  size_t twin_compiles() const { return twin_.size(); }

 private:
  const StageHooks& hooks_;
  std::unordered_map<std::string, StageBlobs> zero_;   // by the variant's HLSL
  std::unordered_map<std::string, StageBlobs> twin_;   // by the HLSL and the spec mask
  std::unordered_map<std::string, std::string> zero_failed_;
  std::unordered_map<std::string, std::string> twin_failed_;
};

// What BuildStageShaders made.
struct StageShaders {
  std::vector<VariantPackNewEntry> made;  // entries of the shaders that were made (variant 0 and, with a twin, variant 1), in target order
  std::vector<SoloShader> solo;           // one per shader made, for the composition pass
  ShaderFateTally fates;                  // built, left out (by reason), failed (no variant for them: the edits did not fit, or the text did not compile)
  size_t compiles = 0;                    // distinct texts compiled for spec variant 0
  size_t twin_compiles = 0;               // distinct texts compiled for the spec variant 1 twin
};

// The stage `def` applied to its `targets` (in order): translate, edit, compile, entries. `row` is the stage's row of the chain's table, `vs_keys` the vertex shaders its pixel shaders are drawn
// with. False with *error for what stops the stage (a container that does not translate, a shader of the wrong stage, a cancel, a compile the hooks call kStop); a shader that loses only its own
// variant is in out->fates and does not. Every target takes one progress step, made or not.
inline bool BuildStageShaders(const VariantDef& def, const chain::FeatureInfo& row, const std::array<uint64_t, kVariantVertexShaders>& vs_keys, const std::vector<Target>& targets, ProgressSink& sink,
                              const StageHooks& hooks, StageShaders* out, std::string* error) {
  const ShaderStage stage = def.vertex ? ShaderStage::kVertex : ShaderStage::kPixel;
  const char* const stage_text = def.vertex ? "vertex" : "pixel";
  const StageContext context = SoloContext(def.feature);
  StageCompiles compiles(hooks);
  StageShaders& made = *out;
  for (const Target& target : targets) {
    const ShaderSource* source = target.source;
    const std::string key_hex = Hex64(source->key);
    TranslatedShader translated;
    if (!hooks.translate(*source, &translated, error)) {
      *error = source->name + ": " + *error;
      return false;
    }
    if (translated.stage != stage) {
      *error = std::format("{} is not a {} shader", source->name, stage_text);
      return false;
    }
    const VariantTransformResult variant = def.transform(target.shader, translated.hlsl, context);
    const ShaderFate fate = made.fates.Note(source->name, key_hex, variant);
    if (fate == ShaderFate::kLeftOut) {
      sink.Step(source->name);
      continue;
    }
    if (fate == ShaderFate::kFailed) {
      // The edits do not fit THIS text: the shader keeps the game's own shader for this stage and the others still get theirs (variant_shader_fate.h; GB-H2-FOLLOW, the review's S5).
      hooks.warn(FailedShaderLine(def.name, source->name, key_hex, variant.error));
      sink.Step(source->name);
      continue;
    }
    if (sink.Cancelled()) {
      *error = "The shader build was cancelled.";
      return false;
    }

    // The blobs: spec variant 0 (no alpha test, the formats of the main pack, the same dxc arguments as every other shader of the pack, dxc.h) and, for a stage with an alpha test twin
    // (the row's spec_twin) and a shader with a spec mask, the same text compiled with the shader's own mask, for the draws that have the alpha test or alpha to coverage on (a pixel shader), or
    // the packed normal (a vertex shader). A shader with no spec mask has no spec variant 1 in the main pack either, so it has no twin. Both are compiled before anything of the shader is kept:
    // a shader whose text does not compile in either has no variant at all (GB-H2-DXC), never half of one.
    const bool want_twin = row.spec_twin && translated.spec_mask != 0;
    const StageBlobs* zero = nullptr;
    const StageBlobs* twin = nullptr;
    std::string compile_error;
    CompileVerdict verdict = compiles.Get(variant.hlsl, 0, std::format("{} [{}] ({} variant)", source->name, key_hex, def.name), &zero, &compile_error);
    if (verdict == CompileVerdict::kOk && want_twin) {
      verdict = compiles.Get(variant.hlsl, translated.spec_mask, std::format("{} [{}] ({} variant, spec variant 1)", source->name, key_hex, def.name), &twin, &compile_error);
    }
    if (verdict == CompileVerdict::kStop) {
      *error = compile_error;
      return false;
    }
    if (verdict == CompileVerdict::kFailed) {
      // The edited text of THIS shader does not compile: it keeps the game's own shader, one warning with dxc's first error line, and the stage goes on (GB-H2-DXC).
      const std::string why = "its edited text did not compile: " + FirstErrorLine(compile_error);
      made.fates.CompileFailed(source->name, key_hex, why);
      hooks.warn(FailedShaderLine(def.name, source->name, key_hex, why));
      sink.Step(source->name);
      continue;
    }

    VariantPackNewEntry entry;
    entry.variant = chain::Word(chain::FeatureBit(def.feature), 0);
    entry.flags = def.vertex ? kVariantEntryVertex : 0;
    entry.key = source->key;
    entry.vs_keys = vs_keys;
    entry.vs_features = chain::VertexNeedsOf(chain::FeatureBit(def.feature));
    entry.dxil = zero->dxil;
    entry.spirv = zero->spirv;
    made.made.push_back(std::move(entry));
    if (want_twin) {
      VariantPackNewEntry at;
      at.variant = chain::Word(chain::FeatureBit(def.feature), 1);
      at.flags = def.vertex ? kVariantEntryVertex : 0;
      at.key = source->key;
      at.vs_keys = vs_keys;
      at.vs_features = chain::VertexNeedsOf(chain::FeatureBit(def.feature));
      at.dxil = twin->dxil;
      at.spirv = twin->spirv;
      made.made.push_back(std::move(at));
    }
    made.fates.Built();
    made.solo.push_back({def.feature, source->key, def.vertex, translated.spec_mask, target.shader, source});
    sink.Step(source->name);
  }
  made.compiles = compiles.compiles();
  made.twin_compiles = compiles.twin_compiles();
  return true;
}

}  // namespace larecomp::shaderpack

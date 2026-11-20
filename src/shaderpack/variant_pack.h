#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the pixel shader variant pack
// ===========================================================================
// The renderer draws some of the game's pixel shaders with another version of their text: the HDR
// scene's tone map (luminance curve and encode continued above white, into a float image), the bloom's
// Gaussians (taps that follow the render scale), and whatever the next graphics packages add. A
// variant is not written by hand: it is the HLSL the translator makes of the game's shader with a few
// anchored text edits, so its sampling, constants, grade and encode are the game's own.
//
// One pack, mcla_shaders_variants.v3.pack (MSHV v3, pack_format.h), holds every variant: one entry per
// (variant word, shader identity), with the DXIL and the SPIR-V blob. A variant is a STAGE of the transform chain
// (variant_chain.h): a VariantDef. A shader can have the edits of several stages at once (a composed shader: the stages
// applied one after the other in chain order, each on the text the one before left), and a vertex shader can have stages too.
// The pack has an entry for each stage alone and for each composition the chain's table allows (variant_chain.h
// Permutations), so the renderer picks per draw the most of what the settings want that the pack has. A stage is a VariantDef:
//
//   feature        pack_format.h kFeature*: the stage's bit in the variant word (PipelineKey::ps_variant), its row in
//                  variant_chain.h kFeatures: its constants block, who it composes with, its alpha test twin, its vertex needs
//   name, version  the stamp lines variants_<name>_version; bump the version by hand when the edits or
//                  the checks of that variant change, and only the variant pack is rebuilt
//   input_xxh3     a fingerprint of anything else the text depends on (the HDR curve), 0 for none
//   pixel_shaders  the shaders to make the variant of, by identity on the retail game and by name
//   pixel_name_suffixes
//                  more pixel shaders, by name only: every pixel shader of the game whose name ends with
//                  one of these (the shadow collector's PS_ShadowBlend* are 159 of them); empty for none
//   vertex         true for a VERTEX stage (the VS-variant stage: a vertex shader's text with an extra output, for the
//                  pixel stages that need it: variant_chain.h needs_vs): pixel_shaders and pixel_name_suffixes then
//                  name vertex shaders, and the transform gets a vertex shader's text
//   needs_rpf      a shader of the variant (pixel or vertex) lives in xarchive_cache.rpf and not in the executable:
//                  a variant-only rebuild then reads the RPF's containers too (VariantStep::NeedsRpfShaders)
//   vertex_shaders the vertex shaders the pixel shaders are drawn with, when the variant reads their
//                  outputs (the HDR tone map: the runtime requires one of them); empty for none
//   transform      the edits: translated HLSL in (the text of the stage before it in a composed shader), the variant's
//                  HLSL out, or why not; or `skip`: this shader is not one the edits are for (a shadow collector shader
//                  with no lookup), leave it out without an error. It gets the StageContext: where its constants block
//                  is in this permutation (608 when alone), so it declares it with DeclareConstants
//                  (variants/stage_constants.h) instead of a literal offset
//
// Each variant is built on its own: one whose edit does not match (a game that is not the retail one)
// or does not compile is left out, with a warning, and the others are still in the pack; the renderer
// draws the game's own shader for what is left out. A variant that was left out is in the stamp like a built
// one (its version line) with a variants_<name>_left_out line that says why, so that the next start is not
// stale and does not translate the shaders again; it is tried again when its version changes or the game
// files do. When none could be built the pack has no entries. The step fails (no pack, no stamp lines) only
// for a problem of the build itself (no compiler worker, a file that cannot be written, a cancel). A variant
// is compiled for spec variant 0 (no alpha test) with the pack's usual
// dxc arguments; a draw that needs the alpha test keeps the main pack's shader, unless the variant has an alpha test twin (its row's spec_twin: the UI sharpening). Shaders of one variant whose
// edited text comes out the same share one compile (the shadow collector's 147 shaders are four texts), and
// their blobs one copy in the file.
//
// The variants of this build: variants/hdr_scene.h (when the HDR curve is embedded, CMakeLists.txt),
// variants/bloom_gauss.h, variants/env_map.h, variants/shadow_filter.h, variants/car_selfrefl.h, variants/car_chrome.h, variants/luminance_scale.h and variants/ui_sharp.h. A package that adds one writes
// variants/<name>.{h,cpp} with a function that returns its VariantDef, adds its feature index to pack_format.h and its row to
// variant_chain.h's table (with the partners it composes with, in both rows), and adds it to BuiltInVariants() in
// variant_pack.cpp.
//
// A variant whose shaders live in the RPF and not in the executable (the environment map's 64x32 reduction, the
// shadow filter's collector lookups) sets VariantDef::needs_rpf: when the main pack stays and only the variant
// pack is stale, the builder then reads the RPF too (builder.cpp `read_rpf`, VariantStep::NeedsRpfShaders).
//
// The step is the builder's (builder.h VariantStep): it runs after the main pack, polls the progress
// sink for cancel, and writes the file under a temporary name and moves it into place.
//
// Composition: after every stage was built on its own, the step looks, per shader, at which stages made an entry for it and
// builds the compositions the table allows (a text edited by stage A, then B on A's output ...), each compiled and checked like
// any variant. A composition whose anchors no longer match after an earlier stage's edits is left out with a warning (the
// stages stay in the pack alone), never an error of the stages themselves.
// ===========================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/builder.h"
#include "shaderpack/variant_chain.h"

namespace larecomp::shaderpack {

// Hand bumped: the framework, the pack layout or the checks of this step (every variant is rebuilt). The
// edits of one variant are its VariantDef::version.
inline constexpr uint32_t kVariantStepVersion = 2;  // 2: the chain (feature words, compositions, vertex stages, 88 byte entries)

// A shader a variant needs, by its identity on the retail game (FNV-1a 64 of the vertex-fetch
// normalised microcode, the runtime's own) and by its name (the group and entry point of the .updb path
// inside the container, as the translator names its HLSL files): the name finds it when the game is
// not the retail one (a warning), and has to be unique then.
struct VariantShader {
  std::string_view name;
  uint64_t key = 0;
};

struct VariantTransformResult {
  bool ok = false;
  // !ok and the shader is not one this variant's edits are for (the transform looked at its text and
  // there is nothing to change: no error, the shader is left out of the variant). `error` says why in a
  // few words, for the summary line; shaders that are left out for the same reason are counted together.
  bool skip = false;
  std::string hlsl;   // the variant's HLSL
  std::string error;  // plain English when !ok: names the anchor and how often it was found
};

// What a stage knows about the permutation it is applied in (the chain, variant_chain.h): the features of the text, and where the
// block of its constants goes in it. A stage that is alone has features == its own bit and its block at 608.
struct StageContext {
  uint32_t feature = 0;       // this stage's feature index
  uint32_t features = 0;      // the permutation's feature set: this stage's bit and the others applied to the same text
  uint32_t constants_offset = chain::kSharedConstantsBytes;  // its block, from the start of the shared constants
  uint32_t constants_bytes = 0;                              // its row's constants_bytes
};

// The context of a stage alone, from the chain's table (or `table` for a test's own).
inline StageContext SoloContext(uint32_t feature, std::span<const chain::FeatureInfo> table = chain::kFeatures) {
  StageContext ctx;
  ctx.feature = feature;
  ctx.features = chain::FeatureBit(feature);
  if (const chain::FeatureInfo* row = chain::Find(feature, table)) ctx.constants_bytes = row->constants_bytes;
  return ctx;
}

// The context of a stage in the permutation `features`.
inline StageContext ContextIn(uint32_t feature, uint32_t features, std::span<const chain::FeatureInfo> table = chain::kFeatures) {
  StageContext ctx = SoloContext(feature, table);
  ctx.features = features;
  if (const uint32_t at = chain::OffsetOf(features, feature, table)) ctx.constants_offset = at;
  return ctx;
}

struct VariantDef {
  uint32_t feature = chain::kNoFeature;  // pack_format.h kFeature*: the stage's bit of the variant word (and its table row)
  std::string name;  // lower case words and underscores: "hdr_scene", "bloom_gauss"; the table row's name
  uint32_t version = 0;
  uint64_t input_xxh3 = 0;
  bool needs_rpf = false;
  bool vertex = false;  // a vertex shader stage: the lists below name vertex shaders (see the top of this file)
  std::vector<VariantShader> pixel_shaders;
  std::vector<std::string_view> pixel_name_suffixes;  // every pixel shader named ...<suffix>, found by name alone
  std::vector<VariantShader> vertex_shaders;  // at most kVariantVertexShaders: the vertex shaders a pixel stage is drawn with when it reads their outputs
  // The shader (one of pixel_shaders: its name tells two of them apart) and its translated HLSL in, the
  // variant's HLSL out. Called once per shader and permutation, on the thread that builds the step.
  std::function<VariantTransformResult(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx)> transform;
};

// The variants this build knows: the bloom's, the environment map's, the shadow filter's, the car self-reflection's and the car chrome's always, the HDR
// scene's when the curve is embedded. For each one that is left out (`left_out`, when given) a line says which and why, for a log.
std::vector<VariantDef> BuiltInVariants(std::vector<std::string>* left_out = nullptr);

// The step for a list of variants (tests, tools), or for this build's (BuiltInVariants).
std::unique_ptr<VariantStep> CreateVariantStep(std::vector<VariantDef> variants);
std::unique_ptr<VariantStep> CreateBuiltInVariantStep(std::vector<std::string>* left_out = nullptr);

}  // namespace larecomp::shaderpack

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
// One pack, mcla_shaders_variants.v2.pack (MSHV v2, pack_format.h), holds every variant: one entry per
// (variant id, pixel shader identity), with the DXIL and the SPIR-V blob. A variant is a VariantDef:
//
//   id             pack_format.h kVariant*: the entry's variant id, which is PipelineKey::ps_variant
//   name, version  the stamp lines variants_<name>_version; bump the version by hand when the edits or
//                  the checks of that variant change, and only the variant pack is rebuilt
//   input_xxh3     a fingerprint of anything else the text depends on (the HDR curve), 0 for none
//   pixel_shaders  the shaders to make the variant of, by identity on the retail game and by name
//   pixel_name_suffixes
//                  more pixel shaders, by name only: every pixel shader of the game whose name ends with
//                  one of these (the shadow collector's PS_ShadowBlend* are 159 of them); empty for none
//   needs_rpf      a shader of the variant (pixel or vertex) lives in xarchive_cache.rpf and not in the executable:
//                  a variant-only rebuild then reads the RPF's containers too (VariantStep::NeedsRpfShaders)
//   vertex_shaders the vertex shaders the pixel shaders are drawn with, when the variant reads their
//                  outputs (the HDR tone map: the runtime requires one of them); empty for none
//   transform      the edits: translated HLSL in, the variant's HLSL out, or why not; or `skip`: this
//                  shader is not one the edits are for (a shadow collector shader with no lookup),
//                  leave it out without an error
//
// Each variant is built on its own: one whose edit does not match (a game that is not the retail one)
// or does not compile is left out, with a warning, and the others are still in the pack; the renderer
// draws the game's own shader for what is left out. The step fails (no pack, no stamp lines) only when
// nothing could be built. A variant is compiled for spec variant 0 (no alpha test) with the pack's usual
// dxc arguments; a draw that needs the alpha test keeps the main pack's shader. Shaders of one variant whose
// edited text comes out the same share one compile (the shadow collector's 147 shaders are four texts), and
// their blobs one copy in the file.
//
// The variants of this build: variants/hdr_scene.h (when the HDR curve is embedded, CMakeLists.txt),
// variants/bloom_gauss.h and variants/shadow_filter.h. A package that adds one writes variants/<name>.{h,cpp}
// with a function that returns its VariantDef and adds it to BuiltInVariants() in variant_pack.cpp.
//
// A variant whose shaders live in the RPF and not in the executable (the shadow filter) sets
// VariantDef::needs_rpf: when the main pack stays and only the variant pack is stale, the builder then reads
// the RPF too (builder.cpp `read_rpf`, VariantStep::NeedsRpfShaders).
//
// The step is the builder's (builder.h VariantStep): it runs after the main pack, polls the progress
// sink for cancel, and writes the file under a temporary name and moves it into place.
// ===========================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/builder.h"

namespace larecomp::shaderpack {

// Hand bumped: the framework, the pack layout or the checks of this step (every variant is rebuilt). The
// edits of one variant are its VariantDef::version.
inline constexpr uint32_t kVariantStepVersion = 1;

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

struct VariantDef {
  uint32_t id = 0;
  std::string name;  // lower case words and underscores: "hdr_scene", "bloom_gauss"
  uint32_t version = 0;
  uint64_t input_xxh3 = 0;
  bool needs_rpf = false;
  std::vector<VariantShader> pixel_shaders;
  std::vector<std::string_view> pixel_name_suffixes;  // every pixel shader named ...<suffix>, found by name alone
  std::vector<VariantShader> vertex_shaders;  // at most kVariantVertexShaders
  // The shader (one of pixel_shaders: its name tells two of them apart) and its translated HLSL in, the
  // variant's HLSL out. Called once per shader, on the thread that builds the step.
  std::function<VariantTransformResult(const VariantShader& shader, std::string_view hlsl)> transform;
};

// The variants this build knows: the bloom's and the shadow filter's always, the HDR scene's when the curve
// is embedded. For each one that is left out (`left_out`, when given) a line says which and why, for a log.
std::vector<VariantDef> BuiltInVariants(std::vector<std::string>* left_out = nullptr);

// The step for a list of variants (tests, tools), or for this build's (BuiltInVariants).
std::unique_ptr<VariantStep> CreateVariantStep(std::vector<VariantDef> variants);
std::unique_ptr<VariantStep> CreateBuiltInVariantStep(std::vector<std::string>* left_out = nullptr);

}  // namespace larecomp::shaderpack

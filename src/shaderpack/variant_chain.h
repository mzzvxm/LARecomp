#pragma once
// ===========================================================================
// LARecomp shader variants -- the transform chain: features, variant words, composed shaders, constants layout
// ===========================================================================
// Version 2 of the variant pack gave a shader ONE variant (ids 2..15, "a shader has one of them at most"). The graphics work
// that follows needs several edits of the same shader at once: the lit PSMultiLight shaders get the headlights' spot terms, the
// G-buffer's outputs and later the lamps' shadows; the car paint gets its self-reflection and the G-buffer's outputs; the UI
// shaders have an alpha test twin. This header is the one source of truth for how that composes, shared by the pack builder
// (src/shaderpack: variant_pack.cpp, variant_chain.cpp) and the renderer (src/gpu: shaders.cpp, pipelines.cpp, scene.cpp). It is
// pure: no I/O, no compiler, no renderer.
//
// FEATURES. One bit of the variant word per change (pack_format.h kFeature*), at most kMaxFeatures = 31. A feature is one
// STAGE of the chain: a text transform of the translated HLSL (a pixel shader's, or a vertex shader's: Stage). Each stage has
// a row in kFeatures below that says what the renderer and the builder both need to know:
//   constants_bytes  the block of constants it reads after the 608 byte SharedConstants (0: none), a multiple of 16
//   composes_with    the features it may share a shader with (a mask; symmetric). A pair that is not listed is NEVER put in
//                    one text: the builder builds each of them alone, and the renderer draws one of them per draw
//   spec_twin        the stage has the spec variant 1 twin: the same text compiled with the shader's own spec mask (a pixel
//                    shader's ALPHA_TEST: the alpha test and alpha to coverage, with SV_Coverage; a vertex shader's
//                    R11G11B10_NORMAL). A composed shader has a twin when all its stages do
//   needs_vs         a pixel stage that reads an interpolant only a vertex stage exports: the VERTEX features the draw's
//                    vertex shader must have (the vertex stage's own bits), else the renderer takes a shader without it
//
// THE WORD. PipelineKey::ps_variant / vs_variant and the pack's `variant` are one u32: bit 0 the main pack's spec variant, bits
// 1..31 the feature set (Word, FeaturesOf, SpecOf). 0 is the game's own shader, 1 its alpha test one, 2 the first packed one.
//
// THE ORDER. The stages of one text are applied in ASCENDING FEATURE INDEX, each on the text the earlier ones left: a stage's
// anchors must therefore be disjoint from the lines an earlier stage rewrites (an anchor that does not match leaves the composed
// shader out, with a warning, and the renderer takes a subset of its features: Pick below). A new feature gets the next free
// index, so a new stage is applied after the existing ones; the table's order is the chain's, fixed and documented here, and the
// constants' layout follows it.
//
// THE CONSTANTS. A stage's block is placed after the shared constants and after the blocks of the active stages before it in
// the chain (LayoutConstants): dense, 16 byte aligned, never overlapping. A stage that is alone is at byte 608, as in version 2
// of the pack, so its text is byte for byte what it was; the builder gives each stage its offset (StageContext) and the
// renderer asks the same function where to write the block. A stage says how its constants are declared with DeclareConstants
// (variants/stage_constants.h), which names its members by offset, so the text is right at any place. Extra textures and
// samplers are bindless: a stage's texture or sampler is a descriptor index (a uint in its block) into the heaps the shader
// already declares, so a block has all it needs.
//
// THE BOUND. At most kMaxStagesPerShader stages are composed in one text, and a shader has at most kMaxWordsPerShader words
// (its permutations, twins included): Permutations() lists the ones to build, smallest first (every stage alone, then the
// pairs ...), and says how many it dropped. The renderer's choice per draw (Pick) takes the best of what the pack has, and a
// pipeline is made when a draw first asks for it, as for every other key.
// ===========================================================================

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "shaderpack/pack_format.h"

namespace larecomp::shaderpack::chain {

inline constexpr uint32_t kSharedConstantsBytes = 608;  // src/gpu/constants.h SharedConstants
inline constexpr uint32_t kMaxStagesPerShader = 4;      // stages composed in one text
inline constexpr uint32_t kMaxWordsPerShader = 16;      // variant words (permutations and their twins) one shader has in the pack and in the renderer's table
inline constexpr uint32_t kConstantsAlignment = 16;     // the blocks' size and place
inline constexpr uint32_t kNoFeature = 0xFFFFFFFFu;     // VariantDef::feature before a stage sets it

// ---------------------------------------------------------------------------
// The word
// ---------------------------------------------------------------------------

constexpr uint32_t FeatureBit(uint32_t feature) { return 1u << feature; }
constexpr uint32_t Word(uint32_t features, uint32_t spec = 0) { return (features << 1) | (spec & 1u); }
constexpr uint32_t FeaturesOf(uint32_t word) { return word >> 1; }
constexpr uint32_t SpecOf(uint32_t word) { return word & 1u; }
constexpr bool IsPacked(uint32_t word) { return word >= kFirstPackedVariant; }
constexpr bool HasFeature(uint32_t word, uint32_t feature) { return (FeaturesOf(word) & FeatureBit(feature)) != 0; }
// The word of one feature alone, spec variant 0.
constexpr uint32_t SoloWord(uint32_t feature) { return Word(FeatureBit(feature)); }

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

enum class Stage : uint8_t { kPixel, kVertex };

struct FeatureInfo {
  uint32_t feature = 0;  // pack_format.h kFeature*; the rows are in increasing order of it, which is the chain's order
  std::string_view name;  // the variant's name (VariantDef::name, the stamp's variants_<name>_...)
  Stage stage = Stage::kPixel;
  uint32_t constants_bytes = 0;  // after the shared constants; a multiple of kConstantsAlignment
  uint32_t composes_with = 0;    // feature mask, symmetric, never the feature itself
  bool spec_twin = false;
  uint32_t needs_vs = 0;         // a pixel stage: the vertex features the draw's vertex shader must have
};

// The existing variants of version 2, ported: none composes with another (each edits shaders no other one does:
// shaderpack/variants/variant_overlap_test.cpp checks it on the retail text), so every one is built alone, as before.
// A feature that is added here and composes lists the partners in BOTH rows.
inline constexpr FeatureInfo kFeatures[] = {
    {kFeatureHdrScene, "hdr_scene", Stage::kPixel, 16, 0, false, 0},
    {kFeatureBloomGauss, "bloom_gauss", Stage::kPixel, 0, 0, false, 0},
    {kFeatureEnvMap, "env_map", Stage::kPixel, 0, 0, false, 0},
    {kFeatureShadowFilter, "shadow_filter", Stage::kPixel, 16, 0, false, 0},
    {kFeatureGlowSoft, "glow_soft", Stage::kPixel, 0, 0, false, 0},
    {kFeatureCarSelfRefl, "car_selfrefl", Stage::kPixel, 112, 0, false, 0},
    {kFeatureLuminanceScale, "luminance_scale", Stage::kPixel, 0, 0, false, 0},
    {kFeatureCarChrome, "car_chrome", Stage::kPixel, 112, 0, false, 0},
    {kFeatureUiSharp, "ui_sharp", Stage::kPixel, 16, 0, true, 0},
    // The headlights and the G-buffer's normal plane compose: both edit the city's PSMultiLight shaders (xCityDefault's night shader among them), so the builder makes the
    // pair in one text for those (the chain's order: the headlight's edits first, the G-buffer's on the text they leave: shaderpack/variants/gbuffer.h anchors on lines the
    // headlight does not rewrite). Their variants are in feat/headlights (variants/headlight.cpp) and feat/gbuffer-spike (variants/gbuffer.cpp): a row whose variant is not in the
    // tree is harmless (nothing builds it, nothing picks it). 672 bytes: src/gpu/headlight_rig_rules.h HeadlightConstants; 48 bytes: src/gpu/gbuffer_types.h GBufferConstants.
    {kFeatureHeadlight, "headlight", Stage::kPixel, 672, FeatureBit(kFeatureGBuffer), true, 0},
    {kFeatureGBuffer, "gbuffer", Stage::kPixel, 48, FeatureBit(kFeatureHeadlight), true, 0},
    // kFeatureLampShadow (11) is reserved: its row comes with its variant, and names the headlight and the G-buffer in composes_with (in all three rows).
    // The G-buffer's vertex stage (variants/gbuffer_day.h): a vertex shader that also writes the vertex normal to TEXCOORD15. It composes with nothing, has no block of constants and the
    // vertex shader's spec variant 1 twin. No pixel stage lists it in needs_vs: the G-buffer's pixel words read the export where it is and fall back to a derivative normal where it is
    // not, so they are valid with any vertex shader and the renderer pairs the vertex word by itself (Scene::GBufferKey).
    {kFeatureGBufferVs, "gbuffer_vs", Stage::kVertex, 0, 0, true, 0},
};

constexpr size_t kFeatureCount = sizeof(kFeatures) / sizeof(kFeatures[0]);

// The row of a feature, null when the table has none.
constexpr const FeatureInfo* Find(uint32_t feature, std::span<const FeatureInfo> table = kFeatures) {
  for (const FeatureInfo& row : table) {
    if (row.feature == feature) return &row;
  }
  return nullptr;
}

// The table is well formed: rows in increasing index order within kMaxFeatures, names set and different, blocks aligned, the
// partners symmetric, none with itself, a pixel stage's vertex needs name vertex stages. False says why in *why.
constexpr bool TableIsValid(std::span<const FeatureInfo> table, const char** why = nullptr) {
  const auto fail = [&](const char* text) {
    if (why) *why = text;
    return false;
  };
  uint32_t seen = 0;
  for (size_t i = 0; i < table.size(); ++i) {
    const FeatureInfo& row = table[i];
    if (row.feature >= kMaxFeatures) return fail("a feature index is not below kMaxFeatures");
    if (i > 0 && row.feature <= table[i - 1].feature) return fail("the rows are not in increasing index order");
    if ((seen & FeatureBit(row.feature)) != 0) return fail("an index is used twice");
    seen |= FeatureBit(row.feature);
    if (row.name.empty()) return fail("a row has no name");
    for (size_t j = 0; j < i; ++j) {
      if (table[j].name == row.name) return fail("two rows have the same name");
    }
    if (row.constants_bytes % kConstantsAlignment != 0) return fail("a constants block is not a multiple of 16 bytes");
    // The draw has one shared constants block for both its shaders and it is laid out for the pixel shader's permutation, so a vertex
    // stage's own block would move the pixel stages' (a later layout version can give vertex blocks a region of their own).
    if (row.stage == Stage::kVertex && row.constants_bytes != 0) return fail("a vertex stage has no block of its own: the draw's shared block is laid out for the pixel stages");
    if ((row.composes_with & FeatureBit(row.feature)) != 0) return fail("a feature composes with itself");
  }
  for (const FeatureInfo& row : table) {
    for (uint32_t other = 0; other < kMaxFeatures; ++other) {
      if ((row.composes_with & FeatureBit(other)) == 0) continue;
      const FeatureInfo* partner = Find(other, table);
      if (!partner) return fail("a feature composes with one the table does not have");
      if ((partner->composes_with & FeatureBit(row.feature)) == 0) return fail("a composition is not listed in both rows");
      if (partner->stage != row.stage) return fail("a pixel stage and a vertex stage cannot be one text");
    }
    for (uint32_t other = 0; other < kMaxFeatures; ++other) {
      if ((row.needs_vs & FeatureBit(other)) == 0) continue;
      const FeatureInfo* vertex = Find(other, table);
      if (!vertex || vertex->stage != Stage::kVertex) return fail("needs_vs names a feature that is not a vertex stage");
      if (row.stage != Stage::kPixel) return fail("only a pixel stage has needs_vs");
    }
  }
  return true;
}
static_assert(TableIsValid(kFeatures), "variant_chain.h kFeatures is not well formed");

// The features of `set` that `table` knows are all of one stage kind.
constexpr uint32_t FeaturesOfStage(uint32_t set, Stage stage, std::span<const FeatureInfo> table = kFeatures) {
  uint32_t out = 0;
  for (const FeatureInfo& row : table) {
    if ((set & FeatureBit(row.feature)) != 0 && row.stage == stage) out |= FeatureBit(row.feature);
  }
  return out;
}

// Whether every pair of `set` is listed as composable (one feature is always fine).
constexpr bool CanCompose(uint32_t set, std::span<const FeatureInfo> table = kFeatures) {
  for (const FeatureInfo& row : table) {
    if ((set & FeatureBit(row.feature)) == 0) continue;
    if (((set & ~FeatureBit(row.feature)) & ~row.composes_with) != 0) return false;
  }
  return true;
}

// Whether the composed shader of `set` has the spec variant 1 twin: every stage of it has one.
constexpr bool HasTwin(uint32_t set, std::span<const FeatureInfo> table = kFeatures) {
  for (const FeatureInfo& row : table) {
    if ((set & FeatureBit(row.feature)) != 0 && !row.spec_twin) return false;
  }
  return set != 0;
}

// The vertex features a pixel shader of `set` needs its draw's vertex shader to have.
constexpr uint32_t VertexNeedsOf(uint32_t set, std::span<const FeatureInfo> table = kFeatures) {
  uint32_t out = 0;
  for (const FeatureInfo& row : table) {
    if ((set & FeatureBit(row.feature)) != 0) out |= row.needs_vs;
  }
  return out;
}

// ---------------------------------------------------------------------------
// The constants' layout
// ---------------------------------------------------------------------------

struct ConstantsWindow {
  uint32_t feature = 0;
  uint32_t offset = 0;  // from the start of the shared constants (so at least kSharedConstantsBytes)
  uint32_t bytes = 0;
};

struct ConstantsLayout {
  std::array<ConstantsWindow, kMaxFeatures> windows{};
  uint32_t count = 0;        // the windows used: the active features that have a block, in chain order
  uint32_t total_bytes = kSharedConstantsBytes;  // what a draw of this permutation uploads: the shared constants and the blocks
};

// Where the blocks of the features in `features` go: after the shared constants, in chain order (ascending index), each right
// after the one before. A feature alone is at kSharedConstantsBytes.
constexpr ConstantsLayout LayoutConstants(uint32_t features, std::span<const FeatureInfo> table = kFeatures) {
  ConstantsLayout layout;
  uint32_t at = kSharedConstantsBytes;
  for (const FeatureInfo& row : table) {
    if ((features & FeatureBit(row.feature)) == 0 || row.constants_bytes == 0) continue;
    layout.windows[layout.count++] = {row.feature, at, row.constants_bytes};
    at += row.constants_bytes;
  }
  layout.total_bytes = at;
  return layout;
}

// The offset of one feature's block in the permutation `features` (0 when the feature has none or is not in it).
constexpr uint32_t OffsetOf(uint32_t features, uint32_t feature, std::span<const FeatureInfo> table = kFeatures) {
  const ConstantsLayout layout = LayoutConstants(features, table);
  for (uint32_t i = 0; i < layout.count; ++i) {
    if (layout.windows[i].feature == feature) return layout.windows[i].offset;
  }
  return 0;
}

// The most bytes a draw of any permutation can upload: every block of the table (a bound for a stack buffer).
constexpr uint32_t MaxConstantsBytes(std::span<const FeatureInfo> table = kFeatures) {
  uint32_t at = kSharedConstantsBytes;
  for (const FeatureInfo& row : table) at += row.constants_bytes;
  return at;
}

// ---------------------------------------------------------------------------
// Which permutations to build
// ---------------------------------------------------------------------------

struct PermutationList {
  std::vector<uint32_t> sets;  // feature sets, smallest first (every one alone, then pairs ...), each in increasing numeric order
  uint32_t dropped = 0;        // compositions left out because of the bounds
};

// The feature sets to build for one shader that the stages in `applicable` all edit: every stage alone, then every composition
// of them that the table allows, up to kMaxStagesPerShader stages in one text, while the shader's words (a set and its twin,
// when the shader has a spec variant 1 and every stage of the set has one) stay within kMaxWordsPerShader. Alone-sets always
// come first, so a bound never costs a stage its own shader.
inline PermutationList Permutations(uint32_t applicable, bool shader_has_spec, std::span<const FeatureInfo> table = kFeatures) {
  PermutationList out;
  uint32_t words = 0;
  const auto words_of = [&](uint32_t set) { return 1u + (shader_has_spec && HasTwin(set, table) ? 1u : 0u); };
  for (uint32_t size = 1; size <= kMaxStagesPerShader; ++size) {
    // Every subset of `applicable` with `size` features, in increasing numeric order (a walk over the submasks).
    std::vector<uint32_t> level;
    for (uint32_t sub = applicable; sub != 0; sub = (sub - 1) & applicable) {
      if (uint32_t(std::popcount(sub)) == size && CanCompose(sub, table)) level.push_back(sub);
    }
    for (size_t i = level.size(); i-- > 0;) {  // the walk goes from large to small: list them in increasing order
      if (words + words_of(level[i]) > kMaxWordsPerShader) {
        ++out.dropped;
        continue;
      }
      words += words_of(level[i]);
      out.sets.push_back(level[i]);
    }
  }
  // Subsets above kMaxStagesPerShader are not built either: count the ones the table would have allowed.
  for (uint32_t sub = applicable; sub != 0; sub = (sub - 1) & applicable) {
    if (uint32_t(std::popcount(sub)) > kMaxStagesPerShader && CanCompose(sub, table)) ++out.dropped;
  }
  return out;
}

// ---------------------------------------------------------------------------
// The renderer's choice
// ---------------------------------------------------------------------------

// The best word of a shader's `words` for a draw that wants the features in `wanted` and has spec variant `spec`: the one with
// the most features, all of them wanted (the settings decide what is wanted; a composed shader is taken only when every one of
// its stages is), the lowest word among equals; `accept` may turn a word down (its vertex shader needs are not met). Returns
// Word(0, spec), the game's own shader, when none fits.
template <class Accept>
constexpr uint32_t Pick(std::span<const uint32_t> words, uint32_t wanted, uint32_t spec, Accept&& accept) {
  uint32_t best = Word(0, spec);
  uint32_t best_count = 0;
  for (const uint32_t word : words) {
    const uint32_t features = FeaturesOf(word);
    if (SpecOf(word) != (spec & 1u) || features == 0 || (features & ~wanted) != 0) continue;
    const uint32_t count = uint32_t(std::popcount(features));
    if (count < best_count || (count == best_count && word >= best)) continue;
    if (!accept(word)) continue;
    best = word;
    best_count = count;
  }
  return best;
}

constexpr uint32_t Pick(std::span<const uint32_t> words, uint32_t wanted, uint32_t spec) {
  return Pick(words, wanted, spec, [](uint32_t) { return true; });
}

}  // namespace larecomp::shaderpack::chain

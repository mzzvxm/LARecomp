// Checks of the transform chain (variant_chain.h, variant_compose.h, variants/stage_constants.h, variant_pack_format.h): the variant word, the
// feature table, the constants' layout of every permutation (never overlapping, a stage alone at byte 608), which permutations are built and how
// many, the renderer's choice, composing synthetic stages on a fixture (the order, the declarations, a stage that does not match), a vertex stage,
// and the variant pack's file round trip with the refusal of a file of another version. A program of its own: it needs no renderer, no ReXGlue, no
// compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_variant_chain_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_variant_chain_test

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variant_pack.h"
#include "shaderpack/variant_pack_format.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"

namespace sp = larecomp::shaderpack;
namespace chain = larecomp::shaderpack::chain;
using namespace sp::textedit;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

size_t Count(std::string_view text, std::string_view needle) { return CountOccurrences(text, needle); }

// ---- a table of this test's own: the built-in rows, then stages the test makes ---------------------------------------------------------------

std::vector<chain::FeatureInfo> TestTable(std::initializer_list<chain::FeatureInfo> more) {
  std::vector<chain::FeatureInfo> rows(std::begin(chain::kFeatures), std::end(chain::kFeatures));
  for (const chain::FeatureInfo& row : more) rows.push_back(row);
  return rows;
}

// ---- the word ------------------------------------------------------------------------------------------------------------------------------

void TestWord() {
  Check(chain::Word(0, 0) == 0 && chain::Word(0, 1) == 1, "words 0 and 1 are the main pack's spec variants");
  Check(chain::Word(chain::FeatureBit(0)) == 2 && chain::SoloWord(sp::kFeatureHdrScene) == 2, "the HDR tone map's word is still 2");
  Check(chain::FeaturesOf(chain::Word(0x5, 1)) == 0x5 && chain::SpecOf(chain::Word(0x5, 1)) == 1 && chain::SpecOf(chain::Word(0x5, 0)) == 0, "the word round trips");
  Check(chain::Word(chain::FeatureBit(30), 1) == 0x80000001u && chain::FeaturesOf(0xFFFFFFFFu) == 0x7FFFFFFFu, "feature 30 is the word's top bit, and 31 features fit");
  Check(chain::IsPacked(2) && !chain::IsPacked(1) && !chain::IsPacked(0), "words from 2 are the variant pack's");
  Check(chain::HasFeature(chain::Word(chain::FeatureBit(3) | chain::FeatureBit(9), 1), 9) && !chain::HasFeature(chain::Word(chain::FeatureBit(3)), 9),
        "HasFeature");
  // The old ids 2..11 of version 2 are the new solo words' features plus 2 (a pipeline cache of an old build never matches a word of a shader that lacks it,
  // except the HDR tone map's id 2, which is the same word).
  Check(chain::SoloWord(sp::kFeatureBloomGauss) == 4 && chain::SoloWord(sp::kFeatureUiSharp) == 0x200, "the old ids moved to feature words");
  Check(sp::kMaxFeatures == 31 && sp::kMaxFeatures > 16, "more than the 16 ids of version 2");
}

// ---- the table ------------------------------------------------------------------------------------------------------------------------------

void TestTable_() {
  const char* why = nullptr;
  Check(chain::TableIsValid(chain::kFeatures, &why), std::string("the built-in table is valid ") + (why ? why : ""));
  Check(chain::kFeatureCount == 11, "eleven rows: nine built in this tree, the headlights' and the G-buffer's (their variants are in two other branches)");
  Check(sp::kFeatureHeadlight == 9 && sp::kFeatureGBuffer == 10 && sp::kFeatureLampShadow == 11 && !chain::Find(sp::kFeatureLampShadow), "the bits 9, 10 and 11 are the headlight's, the G-buffer's and the lamps' shadows' (reserved, no row yet)");
  for (const chain::FeatureInfo& row : chain::kFeatures) {
    Check(chain::Find(row.feature) == &row, "Find returns the row of " + std::string(row.name));
  }
  // Every built-in variant's table row (name, stage) is the variant's: the builder refuses one that says otherwise.
  const struct {
    uint32_t feature;
    uint32_t bytes;
    bool twin;
  } expect[] = {{sp::kFeatureHdrScene, 16, false},  {sp::kFeatureBloomGauss, 0, false}, {sp::kFeatureEnvMap, 0, false},  {sp::kFeatureShadowFilter, 16, false},
                {sp::kFeatureGlowSoft, 0, false},   {sp::kFeatureCarSelfRefl, 112, false}, {sp::kFeatureLuminanceScale, 0, false},
                {sp::kFeatureCarChrome, 112, false}, {sp::kFeatureUiSharp, 16, true},   {sp::kFeatureHeadlight, 672, true}, {sp::kFeatureGBuffer, 48, true}};
  for (const auto& e : expect) {
    const chain::FeatureInfo* row = chain::Find(e.feature);
    Check(row && row->constants_bytes == e.bytes && row->spec_twin == e.twin && row->stage == chain::Stage::kPixel, "row of feature " + std::to_string(e.feature));
  }
  // The headlights' and the G-buffer's rows are the only pair that composes (both edit PSMultiLight shaders); no stage of the table needs a vertex stage yet.
  for (const chain::FeatureInfo& row : chain::kFeatures) {
    const uint32_t partner = row.feature == sp::kFeatureHeadlight ? chain::FeatureBit(sp::kFeatureGBuffer) : row.feature == sp::kFeatureGBuffer ? chain::FeatureBit(sp::kFeatureHeadlight) : 0u;
    Check(row.composes_with == partner && row.needs_vs == 0, std::string(row.name) + (partner ? " composes with its partner only" : " composes with nothing yet"));
  }
  Check(chain::CanCompose(chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer)) &&
            !chain::CanCompose(chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureShadowFilter)) &&
            chain::HasTwin(chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer)),
        "the headlight and the G-buffer compose (and have the twin together), the headlight and the shadow filter do not");

  // A table that is not well formed says why.
  const auto bad = [](std::vector<chain::FeatureInfo> rows, std::string_view expected, const std::string& what) {
    const char* reason = nullptr;
    const bool ok = chain::TableIsValid(rows, &reason);
    Check(!ok && reason && std::string_view(reason).find(expected) != std::string_view::npos, "a bad table is refused: " + what);
  };
  bad({{0, "a", chain::Stage::kPixel, 0, 0, false, 0}, {0, "b", chain::Stage::kPixel, 0, 0, false, 0}}, "increasing", "an index twice");
  bad({{3, "a", chain::Stage::kPixel, 0, 0, false, 0}, {2, "b", chain::Stage::kPixel, 0, 0, false, 0}}, "increasing", "rows out of order");
  bad({{31, "a", chain::Stage::kPixel, 0, 0, false, 0}}, "kMaxFeatures", "index 31");
  bad({{0, "", chain::Stage::kPixel, 0, 0, false, 0}}, "no name", "no name");
  bad({{0, "a", chain::Stage::kPixel, 0, 0, false, 0}, {1, "a", chain::Stage::kPixel, 0, 0, false, 0}}, "same name", "the same name twice");
  bad({{0, "a", chain::Stage::kPixel, 8, 0, false, 0}}, "multiple of 16", "a block of 8 bytes");
  bad({{0, "a", chain::Stage::kPixel, 0, 1, false, 0}}, "itself", "composes with itself");
  bad({{0, "a", chain::Stage::kPixel, 0, 2, false, 0}, {1, "b", chain::Stage::kPixel, 0, 0, false, 0}}, "both rows", "a one sided composition");
  bad({{0, "a", chain::Stage::kPixel, 0, 4, false, 0}}, "does not have", "a partner that is not in the table");
  bad({{0, "a", chain::Stage::kPixel, 0, 2, false, 0}, {1, "b", chain::Stage::kVertex, 0, 1, false, 0}}, "pixel stage and a vertex stage", "a pixel and a vertex stage in one text");
  bad({{0, "a", chain::Stage::kPixel, 0, 0, false, 1}}, "vertex stage", "needs_vs naming a pixel stage");
  bad({{0, "v", chain::Stage::kVertex, 16, 0, false, 0}}, "vertex stage has no block", "a vertex stage with a constants block");
}

// ---- the constants' layout ---------------------------------------------------------------------------------------------------------------

bool Disjoint(const chain::ConstantsLayout& layout) {
  uint32_t end = chain::kSharedConstantsBytes;
  for (uint32_t i = 0; i < layout.count; ++i) {
    const chain::ConstantsWindow& w = layout.windows[i];
    if (w.offset < end || w.offset % 16 != 0 || w.bytes % 16 != 0 || w.bytes == 0) return false;
    end = w.offset + w.bytes;
  }
  return layout.total_bytes == end;
}

void TestLayout() {
  // Every feature alone is at 608: the text of a stage alone is version 2's.
  for (const chain::FeatureInfo& row : chain::kFeatures) {
    const chain::ConstantsLayout layout = chain::LayoutConstants(chain::FeatureBit(row.feature));
    Check(row.constants_bytes == 0 ? layout.count == 0 && layout.total_bytes == 608
                                   : layout.count == 1 && layout.windows[0].offset == 608 && layout.total_bytes == 608 + row.constants_bytes,
          std::string(row.name) + " alone is at byte 608");
    Check(chain::OffsetOf(chain::FeatureBit(row.feature), row.feature) == (row.constants_bytes ? 608u : 0u), std::string(row.name) + ": OffsetOf alone");
  }
  // Every subset of the built-in features: dense, in order, 16 byte aligned, never overlapping, total as the sum.
  bool all_disjoint = true;
  bool all_dense = true;
  const uint32_t all = (1u << chain::kFeatureCount) - 1;
  for (uint32_t set = 0; set <= all; ++set) {
    const chain::ConstantsLayout layout = chain::LayoutConstants(set);
    all_disjoint &= Disjoint(layout);
    uint32_t sum = 608;
    for (const chain::FeatureInfo& row : chain::kFeatures) {
      if ((set & chain::FeatureBit(row.feature)) != 0) sum += row.constants_bytes;
    }
    all_dense &= layout.total_bytes == sum && layout.total_bytes <= chain::MaxConstantsBytes();
  }
  Check(all_disjoint, "no two blocks overlap in any of the 2048 subsets of the table's features");
  Check(all_dense, "the layouts are dense: shared constants plus the blocks of the active features, never more than MaxConstantsBytes");
  Check(chain::MaxConstantsBytes() == 608 + 16 + 16 + 112 + 112 + 16 + 672 + 48, "the most a draw uploads is every block of the table");

  // A table of 31 features with blocks of different sizes, random subsets (a fixed generator).
  std::vector<chain::FeatureInfo> wide;
  for (uint32_t i = 0; i < sp::kMaxFeatures; ++i) {
    static const std::string kNames[] = {"f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",  "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
                                         "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30"};
    wide.push_back({i, kNames[i], chain::Stage::kPixel, 16 * ((i % 7) + (i % 3 == 0 ? 0 : 1)), 0, i % 2 == 0, 0});
  }
  Check(chain::TableIsValid(wide), "a table of 31 features is valid");
  uint64_t state = 0x9E3779B97F4A7C15ull;
  bool wide_ok = true;
  for (int i = 0; i < 4000; ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const uint32_t set = uint32_t(state >> 33) & 0x7FFFFFFFu;
    const chain::ConstantsLayout layout = chain::LayoutConstants(set, wide);
    wide_ok &= Disjoint(layout);
    for (uint32_t k = 0; k < layout.count; ++k) wide_ok &= chain::OffsetOf(set, layout.windows[k].feature, wide) == layout.windows[k].offset;
  }
  Check(wide_ok, "4000 random subsets of 31 features never overlap, and OffsetOf says where each block is");
  // Composition shifts the later block: two 16 byte stages and a 112 byte one.
  const std::vector<chain::FeatureInfo> three = {{1, "a", chain::Stage::kPixel, 16, 0, false, 0}, {4, "b", chain::Stage::kPixel, 112, 0, false, 0},
                                                 {9, "c", chain::Stage::kPixel, 32, 0, false, 0}};
  Check(chain::OffsetOf(0x10 | 0x200, 4, three) == 608 && chain::OffsetOf(0x10 | 0x200, 9, three) == 608 + 112, "c comes after b");
  Check(chain::OffsetOf(0x2 | 0x10 | 0x200, 4, three) == 608 + 16 && chain::OffsetOf(0x2 | 0x10 | 0x200, 9, three) == 608 + 16 + 112, "and after a when a is active too");
  Check(chain::OffsetOf(0x2, 4, three) == 0, "a feature that is not in the permutation has no offset");
}

// ---- which permutations are built ------------------------------------------------------------------------------------------------------------

void TestPermutations() {
  const auto rows = [](uint32_t count, bool twins, bool compose_all) {
    std::vector<chain::FeatureInfo> t;
    const uint32_t all = (1u << count) - 1;
    for (uint32_t i = 0; i < count; ++i) t.push_back({i, std::string_view(i == 0 ? "p0" : i == 1 ? "p1" : i == 2 ? "p2" : i == 3 ? "p3" : i == 4 ? "p4" : "p5"),
                                                      chain::Stage::kPixel, 16, compose_all ? (all & ~(1u << i)) : 0, twins, 0});
    return t;
  };
  {
    const std::vector<chain::FeatureInfo> t = rows(3, true, true);
    const chain::PermutationList plan = chain::Permutations(0x7, true, t);
    Check(plan.sets == std::vector<uint32_t>({1, 2, 4, 3, 5, 6, 7}) && plan.dropped == 0, "three stages that compose: alone first, then the pairs, then all three (14 words with twins)");
    const chain::PermutationList no_spec = chain::Permutations(0x7, false, t);
    Check(no_spec.sets == plan.sets && no_spec.dropped == 0, "the same sets for a shader with no spec variant");
  }
  {
    const std::vector<chain::FeatureInfo> t = rows(3, true, false);
    const chain::PermutationList plan = chain::Permutations(0x7, true, t);
    Check(plan.sets == std::vector<uint32_t>({1, 2, 4}) && plan.dropped == 0, "stages that do not compose are built alone only");
  }
  {
    // Four stages that all compose with twins: 15 sets and 30 words; the bound is 16 words, singles (8) and then pairs in order.
    const std::vector<chain::FeatureInfo> t = rows(4, true, true);
    const chain::PermutationList plan = chain::Permutations(0xF, true, t);
    uint32_t words = 0;
    for (const uint32_t set : plan.sets) words += chain::HasTwin(set, t) ? 2 : 1;
    Check(words <= chain::kMaxWordsPerShader && words == 16, "four composable stages with twins stay within 16 words (" + std::to_string(words) + ")");
    Check(plan.sets.size() >= 4 && std::vector<uint32_t>(plan.sets.begin(), plan.sets.begin() + 4) == std::vector<uint32_t>({1, 2, 4, 8}),
          "every stage is alone in the list whatever the bound");
    Check(plan.dropped == 15 - plan.sets.size(), "the sets over the bound are counted as dropped");
    const chain::PermutationList no_twin = chain::Permutations(0xF, false, t);
    Check(no_twin.sets.size() == 15 && no_twin.dropped == 0, "without a spec variant the 15 sets fit (15 words)");
  }
  {
    // More than kMaxStagesPerShader stages in one text: the sets of five or six are not built, and said so.
    const std::vector<chain::FeatureInfo> t = rows(6, false, true);
    const chain::PermutationList plan = chain::Permutations(0x3F, false, t);
    size_t biggest = 0;
    for (const uint32_t set : plan.sets) biggest = std::max<size_t>(biggest, size_t(std::popcount(set)));
    Check(biggest <= chain::kMaxStagesPerShader, "no set has more than kMaxStagesPerShader stages");
    Check(plan.sets.size() <= chain::kMaxWordsPerShader && plan.dropped > 0, "and the bound on words drops the rest, counted");
  }
  {
    const std::vector<chain::FeatureInfo> t = rows(3, true, true);
    Check(chain::Permutations(0, true, t).sets.empty(), "nothing applicable, nothing to build");
    Check(chain::Permutations(0x2, true, t).sets == std::vector<uint32_t>({2}), "one stage: itself");
    // Mixed twins: a set has a twin only when every stage has one.
    std::vector<chain::FeatureInfo> mixed = t;
    mixed[1].spec_twin = false;
    Check(chain::HasTwin(0x5, mixed) && !chain::HasTwin(0x3, mixed) && !chain::HasTwin(0x7, mixed) && !chain::HasTwin(0, mixed), "HasTwin: all stages or none");
  }
  // The renderer's table holds the same bound.
  Check(chain::kMaxWordsPerShader == 16 && chain::kMaxStagesPerShader == 4, "the bounds are 4 stages in a text and 16 words a shader");
}

// ---- the renderer's choice -----------------------------------------------------------------------------------------------------------------

void TestPick() {
  const uint32_t a = chain::FeatureBit(1);
  const uint32_t b = chain::FeatureBit(4);
  const uint32_t c = chain::FeatureBit(9);
  const std::vector<uint32_t> words = {chain::Word(a), chain::Word(b), chain::Word(a | b), chain::Word(a, 1), chain::Word(a | b, 1), chain::Word(c)};
  Check(chain::Pick(words, a | b | c, 0) == chain::Word(a | b), "the most features of what is wanted that the pack has");
  Check(chain::Pick(words, a, 0) == chain::Word(a), "only a: a alone");
  Check(chain::Pick(words, b, 0) == chain::Word(b), "only b: b alone");
  Check(chain::Pick(words, c, 0) == chain::Word(c), "only c: c alone");
  Check(chain::Pick(words, 0, 0) == 0, "nothing wanted: the game's own shader (word 0)");
  Check(chain::Pick(words, chain::FeatureBit(20), 0) == 0, "a feature the shader has no variant of: the game's own");
  Check(chain::Pick(words, a | b, 1) == chain::Word(a | b, 1), "an alpha test draw takes the twin of the composition");
  Check(chain::Pick(words, b, 1) == 1, "an alpha test draw whose wanted feature has no twin keeps the main pack's spec variant 1");
  Check(chain::Pick(words, a | c, 0) == chain::Word(a), "a composition is taken only when every stage of it is wanted: a and c are not composed, the lower word wins");
  // A tie: two solos wanted, no composition: the lowest word (the lowest feature index: the old ChoosePackedVariant order).
  const std::vector<uint32_t> two = {chain::Word(b), chain::Word(a)};
  Check(chain::Pick(two, a | b, 0) == chain::Word(a), "a tie takes the lowest feature");
  // accept: the vertex needs are not met for the composition: fall back to the next best.
  const uint32_t refused = chain::Word(a | b);
  Check(chain::Pick(words, a | b, 0, [&](uint32_t w) { return w != refused; }) == chain::Word(a), "a word that is turned down is skipped for the next best");
  Check(chain::Pick(words, a | b, 0, [](uint32_t) { return false; }) == 0, "all turned down: the game's own shader");
  Check(chain::Pick(std::span<const uint32_t>(), a, 0) == 0 && chain::Pick(std::span<const uint32_t>(), a, 1) == 1, "no variants: word 0 or 1 as the draw's spec");
  // The vertex needs of a feature set come from the table.
  const std::vector<chain::FeatureInfo> t = {{0, "v", chain::Stage::kVertex, 0, 0, true, 0}, {1, "p", chain::Stage::kPixel, 16, 0, true, 1}};
  Check(chain::TableIsValid(t) && chain::VertexNeedsOf(0x2, t) == 1 && chain::VertexNeedsOf(0x1, t) == 0, "a pixel stage's vertex needs");
  Check(chain::FeaturesOfStage(0x3, chain::Stage::kVertex, t) == 1 && chain::FeaturesOfStage(0x3, chain::Stage::kPixel, t) == 2, "features of a stage kind");
}

// ---- composing stages ----------------------------------------------------------------------------------------------------------------------

// The parts of a translated pixel shader the stages anchor on (as variant_overlap_test.cpp's fixture).
std::string Fixture() {
  return "#define g_PosOffset                vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 576, 0x10)\n"
         "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n"
         "#else\n"
         "#define DEFINE_SHARED_CONSTANTS() \\\n"
         "    float4 g_PosOffset : packoffset(c36); \\\n"
         "    float2 g_InvResolutionScale : packoffset(c37.x);\n"
         "\n"
         "float4 gInvColorExpBias;\n"
         "#ifndef __spirv__\n"
         "[shader(\"pixel\")]\n"
         "#endif\n"
         "void main(\n"
         "\tin float4 iPos : SV_Position,\n"
         "\tout float4 oC0 : SV_Target0)\n"
         "{\n"
         "\tfloat4 r0 = 0.0;\n"
         "\tCubeMapData cubeMapData = (CubeMapData)0;\n"
         "\toC0.xyz = r3.www * -r0.xzy + r0.xzy;\n"
         "}\n";
}

// A stage of this test's own: a helper before main, a local after the cube map's line, and `blocks` float4 members declared at the offset the chain
// gives it. It edits only the anchors every stage may share (main's head, the cube map's line, the constants' declarations).
sp::VariantDef Probe(uint32_t feature, std::string name, uint32_t blocks, std::string_view marker) {
  sp::VariantDef def;
  def.feature = feature;
  def.name = name;
  def.version = 1;
  def.pixel_shaders = {{"probe__PS", 1}};
  def.transform = [name, blocks, marker](const sp::VariantShader& shader, std::string_view hlsl, const sp::StageContext& ctx) {
    sp::VariantTransformResult r;
    std::string text(hlsl);
    std::string error;
    std::vector<sp::ConstantMember> members;
    static std::deque<std::string> names;  // kept alive (and in place) for the string_views
    for (uint32_t i = 0; i < blocks; ++i) {
      names.push_back("g_" + name + std::to_string(i));
      members.push_back({"float4", names.back()});
    }
    if (blocks != 0 && !sp::DeclareConstants(text, ctx, name + " test stage", members, &error)) {
      r.error = error;
      return r;
    }
    static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0), Lit("#endif"), Ws(0), Lit("void main(")};
    if (!ReplaceOnce(text, kMain, 0, 6, "main", [&](std::string_view group) { return "// helper of " + name + "\n" + std::string(group); }, &error)) {
      r.error = error;
      return r;
    }
    static constexpr Piece kCube[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
    if (!ReplaceOnce(text, kCube, 0, 0, "the cube map's line", [&](std::string_view group) { return std::string(group) + "\n\t// " + std::string(marker); }, &error)) {
      r.error = error;
      return r;
    }
    (void)shader;
    r.ok = true;
    r.hlsl = std::move(text);
    return r;
  };
  return def;
}

void TestCompose() {
  // Two stages of one text: a (feature 20, one block) and b (feature 21, two blocks), composing; and c (feature 22) that does not compose with them.
  const std::vector<chain::FeatureInfo> t = TestTable({{20, "ta", chain::Stage::kPixel, 16, chain::FeatureBit(21), true, 0},
                                                       {21, "tb", chain::Stage::kPixel, 32, chain::FeatureBit(20), true, 0},
                                                       {22, "tc", chain::Stage::kPixel, 0, 0, false, 0}});
  Check(chain::TableIsValid(t), "the test's table is valid");
  const std::vector<sp::VariantDef> defs = {Probe(20, "ta", 1, "A's local"), Probe(21, "tb", 2, "B's local"), Probe(22, "tc", 0, "C's local")};
  const sp::VariantShader shader{"probe__PS", 1};
  const std::string fixture = Fixture();

  const auto solo_a = sp::ComposeStages(defs, chain::FeatureBit(20), shader, fixture, t);
  const auto solo_b = sp::ComposeStages(defs, chain::FeatureBit(21), shader, fixture, t);
  Check(solo_a.ok && solo_b.ok, "each stage alone applies");
  Check(Count(solo_a.hlsl, "g_PushConstants.SharedConstants + 608, 0x10)") == 1 && Count(solo_a.hlsl, "g_ta0 : packoffset(c38);") == 1, "a alone: its block is at 608 (c38)");
  Check(Count(solo_b.hlsl, "SharedConstants + 608, 0x10)") == 1 && Count(solo_b.hlsl, "SharedConstants + 624, 0x10)") == 1 && Count(solo_b.hlsl, "g_tb1 : packoffset(c39);") == 1,
        "b alone: its two blocks are at 608 and 624");

  const auto both = sp::ComposeStages(defs, chain::FeatureBit(20) | chain::FeatureBit(21), shader, fixture, t);
  Check(both.ok, "a and b compose: " + both.error);
  Check(Count(both.hlsl, "#define g_ta0 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)") == 1 &&
            Count(both.hlsl, "#define g_tb0 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 624, 0x10)") == 1 &&
            Count(both.hlsl, "#define g_tb1 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 640, 0x10)") == 1,
        "SPIR-V: a's block at 608, b's after it at 624 and 640 (chain order, no overlap)");
  Check(Count(both.hlsl, "float4 g_ta0 : packoffset(c38);") == 1 && Count(both.hlsl, "float4 g_tb0 : packoffset(c39);") == 1 && Count(both.hlsl, "float4 g_tb1 : packoffset(c40);") == 1,
        "DXIL: the same registers (c38, c39, c40)");
  Check(Count(both.hlsl, "float2 g_InvResolutionScale : packoffset(c37.x);") == 1 && Count(both.hlsl, "#define g_InvResolutionScale ") == 1, "the translator's own declaration is there once");
  // The macro stays one: every declaration but the last continues the line.
  {
    const size_t at = both.hlsl.find("#define DEFINE_SHARED_CONSTANTS()");
    const size_t end = both.hlsl.find("\n\n", at);
    std::string_view macro = std::string_view(both.hlsl).substr(at, end - at);
    size_t lines = 0, continued = 0;
    for (size_t pos = 0; pos < macro.size();) {
      size_t nl = macro.find('\n', pos);
      if (nl == std::string_view::npos) nl = macro.size();
      ++lines;
      if (nl > pos && macro[nl - 1] == '\\') ++continued;
      pos = nl + 1;
    }
    Check(lines >= 5 && continued == lines - 1, "the macro's lines all continue but the last (" + std::to_string(continued) + " of " + std::to_string(lines) + ")");
  }
  Check(Count(both.hlsl, "// A's local") == 1 && Count(both.hlsl, "// B's local") == 1 && Count(both.hlsl, "// helper of ta") == 1 && Count(both.hlsl, "// helper of tb") == 1,
        "both stages' edits are in the text once");
  // The order is the chain's, not the order the features are named in: the same text from {b, a} (they are a set).
  const auto again = sp::ComposeStages(defs, chain::FeatureBit(21) | chain::FeatureBit(20), shader, fixture, t);
  Check(again.ok && again.hlsl == both.hlsl, "a set composes to one text, in ascending feature order");
  // A stage after another sees the text the first left: b's helper was put before main, so a's comes before b's.
  Check(both.hlsl.find("// helper of tb") < both.hlsl.find("// helper of ta") || both.hlsl.find("// helper of ta") < both.hlsl.find("// helper of tb"), "both helpers are before main");

  // A stage whose anchor is not there is the composition's failure, named, with the stages before it.
  const std::string no_cube = [&] {
    std::string text = fixture;
    text.replace(text.find("CubeMapData cubeMapData = (CubeMapData)0;"), 41, "");
    return text;
  }();
  const auto broken = sp::ComposeStages(defs, chain::FeatureBit(20) | chain::FeatureBit(21), shader, no_cube, t);
  Check(!broken.ok && !broken.skip && broken.error.find("cube map") != std::string::npos, "an anchor that is gone fails the composition: " + broken.error);
  // A stage that skips makes the whole composition a skip.
  std::vector<sp::VariantDef> with_skip = defs;
  with_skip[1].transform = [](const sp::VariantShader&, std::string_view, const sp::StageContext&) {
    sp::VariantTransformResult r;
    r.skip = true;
    r.error = "it reads nothing b is for";
    return r;
  };
  const auto skipped = sp::ComposeStages(with_skip, chain::FeatureBit(20) | chain::FeatureBit(21), shader, fixture, t);
  Check(!skipped.ok && skipped.skip && skipped.error.find("(after ta)") != std::string::npos, "a stage that skips skips the composition, saying what came before: " + skipped.error);
  // A feature with no stage, no features.
  Check(!sp::ComposeStages(defs, chain::FeatureBit(25), shader, fixture, t).ok && !sp::ComposeStages(defs, 0, shader, fixture, t).ok, "a feature with no stage and an empty set are refused");

  // A block whose members do not add up to the table's size is refused (the table and the stage disagree).
  std::vector<chain::FeatureInfo> wrong = t;
  wrong[wrong.size() - 3].constants_bytes = 32;  // ta says 32, its stage declares 16
  wrong[wrong.size() - 3].composes_with = chain::FeatureBit(21);
  const auto mismatch = sp::ComposeStages(defs, chain::FeatureBit(20), shader, fixture, wrong);
  Check(!mismatch.ok && mismatch.error.find("table says 32") != std::string::npos, "members that do not add up to the table's bytes are refused: " + mismatch.error);

  // Declaring twice (a name already there) is refused: the declaration check finds two.
  {
    std::string text = fixture;
    std::string error;
    const sp::ConstantMember members[] = {{"float4", "g_same"}};
    const sp::StageContext ctx = {20, chain::FeatureBit(20), 608, 16};
    Check(sp::DeclareConstants(text, ctx, "x", members, &error), "a declaration is added");
    Check(!sp::DeclareConstants(text, ctx, "x", members, &error) && error.find("2 times") != std::string::npos, "the same name twice is refused: " + error);
  }
}

void TestDeclareText() {
  // The text of a stage alone is version 2's, byte for byte: the one declaration of the shadow filter and the three-line DXIL of the paint.
  std::string text = Fixture();
  std::string error;
  const sp::ConstantMember one[] = {{"float4", "g_ShadowFilter"}};
  Check(sp::DeclareConstants(text, sp::SoloContext(sp::kFeatureShadowFilter), "Shadow filter variant (src/gpu/shadow_filter.h ShadowFilterConstants): 16 bytes after the 608.", one, &error), error);
  const std::string spirv_want =
      "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n"
      "// Shadow filter variant (src/gpu/shadow_filter.h ShadowFilterConstants): 16 bytes after the 608.\n"
      "#define g_ShadowFilter vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)\n";
  Check(text.find(spirv_want) != std::string::npos, "SPIR-V: version 2's lines, byte for byte");
  Check(text.find("float2 g_InvResolutionScale : packoffset(c37.x); \\\n    float4 g_ShadowFilter : packoffset(c38);\n") != std::string::npos, "DXIL: version 2's line, byte for byte");

  // An array member (the headlights' shape): the SPIR-V macro takes the index, the DXIL declares the array.
  std::string array_text = Fixture();
  const sp::ConstantMember arrays[] = {{"float4", "g_HlHeader"}, {"float4", "g_HlLights", 12, "g_HlLight(INDEX)"}, {"uint4", "g_HlAtlas"}};
  const sp::StageContext ctx = {9, chain::FeatureBit(9), 608, 16 + 16 * 12 + 16};
  Check(sp::DeclareConstants(array_text, ctx, "arrays", arrays, &error), error);
  Check(array_text.find("#define g_HlHeader vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)\n"
                        "#define g_HlLight(INDEX) vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 624 + (INDEX) * 16, 0x10)\n"
                        "#define g_HlAtlas vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 816, 0x10)") != std::string::npos,
        "SPIR-V: an array is a macro with an argument, the next member follows it");
  Check(array_text.find("float4 g_HlHeader : packoffset(c38); \\\n    float4 g_HlLights[12] : packoffset(c39); \\\n    uint4 g_HlAtlas : packoffset(c51);") != std::string::npos,
        "DXIL: the array is declared with its count, the next register follows it");
  // A block that is not at a 16 byte step from 608 is refused.
  std::string bad_text = Fixture();
  const sp::StageContext misplaced = {9, chain::FeatureBit(9), 612, 16};
  Check(!sp::DeclareConstants(bad_text, misplaced, "x", one, &error) && error.find("612") != std::string::npos, "an offset that is not a 16 byte step is refused: " + error);
  const sp::StageContext early = {9, chain::FeatureBit(9), 592, 16};
  Check(!sp::DeclareConstants(bad_text, early, "x", one, &error), "an offset inside the shared constants is refused");
  // A text without the translator's declarations is refused with the anchor named.
  std::string headless = "void main() {}\n";
  Check(!sp::DeclareConstants(headless, sp::SoloContext(sp::kFeatureShadowFilter), "x", one, &error) && error.find("g_InvResolutionScale") != std::string::npos,
        "a text without the declarations names the anchor: " + error);
}

// ---- a vertex stage --------------------------------------------------------------------------------------------------------------------------

std::string VertexFixture() {
  return "void main(\n"
         "\t[[vk::location(0)]] in float4 iPosition0 : POSITION0,\n"
         "\t[[vk::ext_decorate(18)]] out precise float4 oPos : SV_Position,\n"
         "\tout float4 oTexCoord0 : TEXCOORD0,\n"
         "\tout float4 oTexCoord15 : TEXCOORD15)\n"
         "{\n"
         "\toTexCoord0 = 0.0;\n"
         "\toTexCoord15 = 0.0;\n"
         "\tfloat4 r1 = iPosition0;\n"
         "\toTexCoord0.xyz = r1.xyz;\n"
         "\toPos = r1;\n"
         "}\n";
}

void TestVertexStage() {
  // A pixel stage that needs a vertex stage's interpolant (the G-buffer's normal): the vertex stage writes the slot the vertex shader exports as
  // zeros (every vertex shader of the game declares all sixteen), the pixel stage reads it.
  const std::vector<chain::FeatureInfo> t = TestTable({{20, "vs_normal", chain::Stage::kVertex, 0, 0, true, 0},
                                                       {21, "ps_normal", chain::Stage::kPixel, 16, 0, true, chain::FeatureBit(20)}});
  Check(chain::TableIsValid(t), "a vertex stage and a pixel stage that needs it make a valid table");
  sp::VariantDef vertex;
  vertex.feature = 20;
  vertex.name = "vs_normal";
  vertex.vertex = true;
  vertex.pixel_shaders = {{"probe__VS", 2}};
  vertex.transform = [](const sp::VariantShader& shader, std::string_view hlsl, const sp::StageContext& ctx) {
    sp::VariantTransformResult r;
    std::string text(hlsl);
    // The slot must be exported as the zero initialisation and written nowhere else.
    if (CountOccurrences(text, "oTexCoord15 = 0.0;") != 1 || CountOccurrences(text, "oTexCoord15") != 2) {
      r.error = std::string(shader.name) + ": TEXCOORD15 is not an unused interpolant";
      return r;
    }
    if (ctx.constants_bytes != 0) {
      r.error = "a vertex stage has no block";
      return r;
    }
    const size_t at = text.find("\toPos = r1;");
    if (at == std::string::npos) {
      r.error = std::string(shader.name) + ": no position write";
      return r;
    }
    text.insert(at, "\toTexCoord15 = float4(normalize(r1.xyz), 1.0);\n");
    r.ok = true;
    r.hlsl = std::move(text);
    return r;
  };
  const std::vector<sp::VariantDef> defs = {vertex};
  const auto made = sp::ComposeStages(defs, chain::FeatureBit(20), {"probe__VS", 2}, VertexFixture(), t);
  Check(made.ok && Count(made.hlsl, "oTexCoord15 = float4(normalize(r1.xyz), 1.0);") == 1 && Count(made.hlsl, "oTexCoord15 = 0.0;") == 1, "the vertex stage writes the free interpolant: " + made.error);
  std::string used = VertexFixture();
  used.insert(used.find("\toPos = r1;"), "\toTexCoord15.x = 1.0;\n");
  Check(!sp::ComposeStages(defs, chain::FeatureBit(20), {"probe__VS", 2}, used, t).ok, "a vertex shader that uses the slot is refused");
  // The pixel stage's word records the vertex needs; the vertex word has its own spec bit (R11G11B10_NORMAL).
  const uint32_t ps_set = chain::FeatureBit(21);
  Check(chain::VertexNeedsOf(ps_set, t) == chain::FeatureBit(20), "the pixel stage needs the vertex stage's feature");
  Check(chain::Word(chain::VertexNeedsOf(ps_set, t), 1) == chain::Word(chain::FeatureBit(20), 1), "the draw's vertex word is the needs with the vertex shader's own spec bit");
}

// ---- the variant pack file -----------------------------------------------------------------------------------------------------------------

uint64_t Fnv(const uint8_t* data, size_t size) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 1099511628211ull;
  }
  return h;
}

std::vector<uint8_t> Bytes(const char* text) { return std::vector<uint8_t>(text, text + std::char_traits<char>::length(text)); }

void TestPackFile() {
  std::vector<sp::VariantPackNewEntry> entries;
  const auto add = [&](uint64_t key, uint32_t word, bool vertex, const char* dxil, const char* spirv, uint32_t vs_features = 0) {
    sp::VariantPackNewEntry e;
    e.key = key;
    e.variant = word;
    e.flags = vertex ? sp::kVariantEntryVertex : 0;
    e.vs_features = vs_features;
    e.dxil = Bytes(dxil);
    e.spirv = Bytes(spirv);
    entries.push_back(std::move(e));
  };
  // Pixel entries by (word, key); a composed word with its twin; a vertex entry after them. Entries 1 and 2 share their blobs.
  add(0x10, chain::SoloWord(sp::kFeatureShadowFilter), false, "DXBC-one", "spirv-one");
  add(0x11, chain::SoloWord(sp::kFeatureShadowFilter), false, "DXBC-one", "spirv-one");
  add(0x20, chain::SoloWord(sp::kFeatureUiSharp), false, "DXBC-two", "spirv-two");
  add(0x20, chain::Word(chain::FeatureBit(sp::kFeatureUiSharp), 1), false, "DXBC-twin", "spirv-twin");
  add(0x30, chain::SoloWord(sp::kFeatureHdrScene), false, "DXBC-hdr", "spirv-hdr");
  std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    return sp::variant_pack_detail::SortKey(a.vertex(), a.variant, a.key) < sp::variant_pack_detail::SortKey(b.vertex(), b.variant, b.key);
  });
  entries[0].vs_keys[0] = 0xAAAA;
  std::vector<uint8_t> bytes;
  std::string error;
  Check(sp::LayOutVariantPack(entries, sp::kDxil | sp::kSpirv, sp::kPackVersion, &Fnv, &bytes, &error), "the pack is laid out: " + error);
  Check(bytes.size() >= sp::kVariantPackHeaderBytes + entries.size() * sp::kVariantPackEntryBytes, "header and entries, then blobs");
  Check(std::string(bytes.begin(), bytes.begin() + 4) == "MSHV" && bytes[4] == 3, "magic MSHV and version 3");
  // Shared blobs are one copy: the file is smaller than the sum of the blobs.
  size_t blob_sum = 0;
  for (const auto& e : entries) blob_sum += e.dxil.size() + e.spirv.size();
  Check(bytes.size() - (sp::kVariantPackHeaderBytes + entries.size() * sp::kVariantPackEntryBytes) < blob_sum, "blobs that are byte for byte the same are written once");

  sp::ParsedVariantPack parsed;
  sp::VariantPackExpect expect;
  expect.formats_needed = sp::kSpirv;
  Check(sp::ParseVariantPack(bytes, expect, &parsed, &error), "the pack reads back: " + error);
  Check(parsed.entries.size() == entries.size() && parsed.main_pack_version == sp::kPackVersion && parsed.formats == (sp::kDxil | sp::kSpirv), "entry count, main pack version, formats");
  bool same = parsed.entries.size() == entries.size();
  for (size_t i = 0; same && i < entries.size(); ++i) {
    const sp::VariantPackEntry& got = parsed.entries[i];
    same &= got.key == entries[i].key && got.variant == entries[i].variant && got.flags == entries[i].flags && got.vs_keys == entries[i].vs_keys &&
            got.vs_features == entries[i].vs_features;
    same &= got.spirv.size == entries[i].spirv.size() && got.spirv.fnv == Fnv(entries[i].spirv.data(), entries[i].spirv.size());
    same &= std::equal(entries[i].spirv.begin(), entries[i].spirv.end(), bytes.begin() + got.spirv.offset);
    same &= std::equal(entries[i].dxil.begin(), entries[i].dxil.end(), bytes.begin() + got.dxil.offset);
  }
  Check(same, "every field and blob of every entry round trips");

  // Refusals: a file of another version is refused whole, with a sentence that says so.
  const auto refused = [&](std::vector<uint8_t> file, std::string_view expected, const std::string& what, sp::VariantPackExpect ex = {}) {
    sp::ParsedVariantPack out;
    std::string why;
    const bool ok = sp::ParseVariantPack(file, ex, &out, &why);
    Check(!ok && why.find(expected) != std::string::npos && out.entries.empty(), "refused: " + what + " (" + why + ")");
  };
  {
    std::vector<uint8_t> v2 = bytes;  // a version 2 header on 80 byte entries
    v2[4] = 2;
    refused(v2, "another version of the variant pack", "version 2");
    std::vector<uint8_t> v4 = bytes;
    v4[4] = 4;
    refused(v4, "another version of the variant pack", "version 4 (a newer build's)");
    std::vector<uint8_t> contract = bytes;
    contract[16] = 1;
    refused(contract, "constants contract 1", "contract 1");
    std::vector<uint8_t> other_main = bytes;
    other_main[20] = 7;
    refused(other_main, "built for shader pack 7", "another main pack");
    std::vector<uint8_t> magic = bytes;
    magic[0] = 'X';
    refused(magic, "not a shader variant pack", "bad magic");
    refused(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 20), "not a shader variant pack", "a cut header");
    refused(std::vector<uint8_t>(bytes.begin(), bytes.begin() + sp::kVariantPackHeaderBytes + 100), "past the end", "a cut entry table");
    std::vector<uint8_t> cut = bytes;
    cut.resize(cut.size() - 4);
    refused(cut, "past the end", "a blob past the end");
    sp::VariantPackExpect need_dxil;
    need_dxil.formats_needed = sp::kDxil;
    std::vector<uint8_t> only_spirv = bytes;
    only_spirv[12] = sp::kSpirv;
    refused(only_spirv, "format this backend reads", "no DXIL in the file", need_dxil);
    std::vector<uint8_t> word_one = bytes;  // the first entry's word -> 1 (the main pack's)
    const size_t first = sp::kVariantPackHeaderBytes;
    word_one[first + 40] = 1;
    word_one[first + 41] = word_one[first + 42] = word_one[first + 43] = 0;
    refused(word_one, "the main pack's", "a word that is the main pack's");
    std::vector<uint8_t> unknown = bytes;  // feature 25 is not in the table
    unknown[first + 40] = 0;
    unknown[first + 41] = 0;
    unknown[first + 42] = 0;
    unknown[first + 43] = 0x02;  // bit 25 of the word is feature 24
    refused(unknown, "does not know", "an unknown feature");
    std::vector<uint8_t> flags = bytes;
    flags[first + 44] = 0x04;
    refused(flags, "flags this build does not know", "unknown flags");
    std::vector<uint8_t> needs = bytes;
    needs[first + 48] = 1;
    refused(needs, "vertex features", "vertex needs the features do not ask for");
    std::vector<uint8_t> order = bytes;  // swap the keys of entries 1 and 2 (0x10, 0x11 of one word)
    std::swap_ranges(order.begin() + first + sp::kVariantPackEntryBytes, order.begin() + first + sp::kVariantPackEntryBytes + 8,
                     order.begin() + first + 2 * sp::kVariantPackEntryBytes);
    refused(order, "out of order", "entries out of order");
  }
  // A vertex entry: its flag, no pixel fields.
  {
    std::vector<sp::VariantPackNewEntry> with_vs = entries;
    sp::VariantPackNewEntry v;
    v.key = 0x99;
    v.variant = chain::Word(chain::FeatureBit(sp::kFeatureBloomGauss), 1);  // (a pixel feature on a vertex entry: the reader refuses it)
    v.flags = sp::kVariantEntryVertex;
    v.dxil = Bytes("DXBC-v");
    v.spirv = Bytes("spirv-v");
    with_vs.push_back(v);
    std::vector<uint8_t> file;
    Check(sp::LayOutVariantPack(with_vs, sp::kDxil | sp::kSpirv, sp::kPackVersion, &Fnv, &file, &error), "a pack with a vertex entry is laid out");
    refused(file, "vertex shader with a pixel feature", "a vertex entry with a pixel feature");
  }
  {
    // The writer refuses entries out of order.
    std::vector<sp::VariantPackNewEntry> wrong = entries;
    std::swap(wrong[0], wrong[2]);
    std::vector<uint8_t> file;
    Check(!sp::LayOutVariantPack(wrong, sp::kDxil, sp::kPackVersion, &Fnv, &file, &error), "the writer refuses entries out of order");
  }
  // Too many variants of one shader.
  {
    std::vector<sp::VariantPackNewEntry> many;
    // 19 words of one shader: the soloes of nine features, their twins and a pair, in word order.
    std::set<uint32_t> words;
    for (uint32_t f = 0; f < 9; ++f) {
      words.insert(chain::Word(chain::FeatureBit(f), 0));
      words.insert(chain::Word(chain::FeatureBit(f), 1));
    }
    words.insert(chain::Word(chain::FeatureBit(0) | chain::FeatureBit(1), 0));
    for (const uint32_t w : words) {
      sp::VariantPackNewEntry e;
      e.key = 0x77;
      e.variant = w;
      e.dxil = Bytes("D");
      e.spirv = Bytes("S");
      many.push_back(e);
    }
    std::vector<uint8_t> file;
    Check(sp::LayOutVariantPack(many, sp::kDxil | sp::kSpirv, sp::kPackVersion, &Fnv, &file, &error), "19 words of one shader are laid out (the writer does not judge)");
    refused(file, "more than 16 variants", "a shader with more than 16 variants");
  }
}

}  // namespace

int main() {
  TestWord();
  TestTable_();
  TestLayout();
  TestPermutations();
  TestPick();
  TestCompose();
  TestDeclareText();
  TestVertexStage();
  TestPackFile();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

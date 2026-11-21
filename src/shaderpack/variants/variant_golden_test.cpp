// The chain against the retail text. Two checks that need the translated HLSL of the game (the builder's own output: `<group>__<entry>.hlsl` files, for
// example shaderpack/baseline/hlsl-rpf and hlsl-xex of the development PC; no game file is in this repository):
//
//  1. BYTE IDENTITY. Every built-in variant, run alone through the chain (ComposeStages with its own feature: version 2 of the variant pack's text), must
//     give for every shader it is for exactly the text the variant gave before the chain existed. The reference is a "golden" file made by the pre-chain
//     code (origin/main, whose VariantDef::transform takes (shader, hlsl)), by a small dumper that is NOT in this tree (it only builds against
//     that API; it lives in golden_old.cpp in the folder variant-compose-golden next to the worktree): one line per
//     (variant, shader file) with the status, the size and the FNV-1a 64 of the text:
//       <variant> TAB <file stem> TAB <ok|skip|error> TAB <size> TAB <fnv1a64, 16 hex digits> TAB <the first 100 characters of the error>
//     and, before them, header lines the dumper writes: `# reference <what the golden was made from, e.g. origin/main a2f91fdd>` (printed) and one
//     `# version <variant> <n>` per variant (VariantDef::version of the pre-chain code): the text AND the version must be the main's, so a variant
//     whose text was carried over but whose version stamp was not (or the other way round) fails (the pack's stamp lines are what the player's pack
//     is checked against). A golden without header lines (the older ones) is checked on the text only.
//     Nothing of the text is in the file, only its hash, and the file is not committed. `--golden FILE` checks against it; `--write FILE` writes this
//     tree's own lines in the same format (to make a new reference after a variant's text was changed on purpose). WHENEVER MAIN CHANGES A VARIANT'S
//     TEXT (fix/shadow-dither's shadow_filter version 3, fix/rt-chrome-edge's and the RT line's car_chrome versions 2 and 3): make the golden again from
//     origin/main as it is now (perf\scripts\variant-golden.ps1 does the export, the dumper's build and this test in one go) and run this test on the
//     branch that carries the chain: 0 differ is the pass. The mechanical port of a changed stage: the transform takes the StageContext, DeclareConstants
//     replaces the two declarations and the kChecks lines about them (variants/stage_constants.h).
//  2. COMPOSITION ON RETAIL TEXT. Each built-in stage composed with a test stage of this file's own (32 bytes of constants, a helper before main, a local after
//     the cube map's line), with the real stage first and with it second: the composition applies, both blocks are declared once at the offsets the chain gives,
//     and with `--dxc DIR` (the folder of dxc.exe and dxil.dll) the composed text compiles to DXIL and SPIR-V with the pack's arguments. A vertex stage is
//     composed with a real vertex shader the same way.
//     2b. THE REAL PAIR. A tree that has both the headlight's and the G-buffer's variants (feat/headlights and feat/gbuffer-spike together) also composes the two on the
//     retail shaders they share (xCityDefault's night shader) and compiles that: the headlight's block at 608, the G-buffer's at 1680, both helpers once, and DXIL and SPIR-V
//     with the shader's spec constants 0 and 2 (the alpha test twin).
//
//     2c. THE G-BUFFER'S VERTEX STAGE (feature 12, shaderpack/variants/gbuffer_day.h). It composes with nothing: each of its three real vertex shaders (the road, the tunnel road, the foliage
//     props) is made alone, has the export once, and compiles to DXIL and SPIR-V at the vertex shader's spec variants 0 and 1.
//  The G-buffer's own families: the variant has grown past the spike's two shaders (the night family: gbuffer_night.h, the day families: gbuffer_day.h), so its lines are not part of the
//  identity of a tree that adds those stages to a main that has the spike: `--expect-changes gbuffer` counts them against the golden (INFO lines: same / differ / new / gone) and never fails on
//  them; the stages' own tests (gbuffer_night_test, gbuffer_day_test) hold their texts to the game's plus the insertions, and every other variant stays byte for byte the golden's.
//
//   cmake --build <build dir> --target larecomp_variant_golden_test     (src/shaderpack/CMakeLists.txt)
//   larecomp_variant_golden_test --hlsl <folder>... --hlsli <hdr_tonemap_shoulder.hlsli> [--gbuffer-hlsli <gbuffer_common.hlsli>] [--golden FILE | --write FILE] [--expect-changes VARIANT]... [--dxc DIR] [--work DIR]
// Exit 0 when every check holds.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variant_pack.h"
#include "shaderpack/variants/bloom_gauss.h"
#include "shaderpack/variants/car_chrome.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/env_map.h"
#include "shaderpack/variants/glow_soft.h"
#include "shaderpack/variants/hdr_scene.h"
#include "shaderpack/variants/luminance_scale.h"
#include "shaderpack/variants/shadow_filter.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"
#include "shaderpack/variants/ui_sharp.h"
// The variants of the branches that are not in every tree (feat/headlights, feat/gbuffer-spike): the test takes the ones this tree has, so that the same file serves each branch and
// their union. The G-buffer's helper (src/gpu/hlsl/gbuffer_common.hlsli) is a file the variant pastes: `--gbuffer-hlsli FILE`.
#if __has_include("shaderpack/variants/headlight.h")
#include "shaderpack/variants/headlight.h"
#define GOLDEN_HAS_HEADLIGHT 1
#endif
#if __has_include("shaderpack/variants/gbuffer.h")
#include "shaderpack/variants/gbuffer.h"
#define GOLDEN_HAS_GBUFFER 1
#endif
// The G-buffer's day stage (package GB-D): its vertex stage (feature 12) is checked on the three real vertex shaders it is for (section 2c).
#if __has_include("shaderpack/variants/gbuffer_day.h")
#include "shaderpack/variants/gbuffer_day.h"
#define GOLDEN_HAS_GBUFFER_DAY 1
#endif

// The one function of the pack builder's hash library this test needs (hdr_scene.cpp fingerprints its curve with it; the text it makes does not depend on it).
namespace larecomp::shaderpack {
uint64_t Xxh3(const void*, size_t) { return 0; }
}  // namespace larecomp::shaderpack

namespace sp = larecomp::shaderpack;
namespace chain = larecomp::shaderpack::chain;
namespace fs = std::filesystem;
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

uint64_t Fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string Slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Hex16(uint64_t v) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(v));
  return buffer;
}

struct Shader {
  std::string stem;
  fs::path path;
};

struct Line {
  std::string variant, stem, status, size, fnv, error;
};

std::string StatusOf(const sp::VariantTransformResult& r) { return r.ok ? "ok" : r.skip ? "skip" : "error"; }

std::string ErrorOf(const sp::VariantTransformResult& r) {
  std::string err = r.error.substr(0, 100);
  for (char& c : err) {
    if (c == '\t' || c == '\n') c = ' ';
  }
  return err;
}

// One (variant, shader) of the golden's scope: the variants' own target lists, by name.
std::vector<const Shader*> TargetsOf(const sp::VariantDef& def, const std::vector<Shader>& files, std::vector<sp::VariantShader>* shaders) {
  std::vector<const Shader*> out;
  for (const Shader& file : files) {
    const sp::VariantShader* named = nullptr;
    for (const auto& p : def.pixel_shaders) {
      if (p.name == file.stem) named = &p;
    }
    bool by_suffix = false;
    for (const auto& s : def.pixel_name_suffixes) {
      if (std::string_view(file.stem).ends_with(s)) by_suffix = true;
    }
    if (!named && !by_suffix) continue;
    out.push_back(&file);
    shaders->push_back(named ? *named : sp::VariantShader{file.stem, 0});
  }
  return out;
}

// ---- the compile of a composed text with dxc.exe ------------------------------------------------------------------------------------------------

struct Dxc {
  fs::path exe;
  fs::path work;
  bool enabled() const { return !exe.empty(); }
  // 0 on success. The arguments are the pack's (src/shaderpack/dxc.cpp BuildArguments).
  int Compile(const std::string& text, const std::string& tag, bool spirv, bool vertex, uint32_t spec) const {
    const fs::path source = work / (tag + ".hlsl");
    {
      std::ofstream out(source, std::ios::binary);
      out << text;
    }
    const fs::path product = work / (tag + (spirv ? ".spv" : ".dxil"));
    const fs::path log = work / (tag + (spirv ? ".spv.log" : ".dxil.log"));
    std::string command = "\"\"" + exe.string() + "\" -T " + (vertex ? "vs_6_0" : "ps_6_0") + " -E main ";
    if (spirv) command += std::string("-HV 2021 -all-resources-bound -spirv -fvk-use-dx-layout ") + (vertex ? "-fvk-invert-y " : "") + "-Qstrip_debug ";
    command += "-DXENOS_SPEC_CONSTANTS=" + std::to_string(spec) + " -Fo \"" + product.string() + "\" \"" + source.string() + "\" > \"" + log.string() + "\" 2>&1\"";
    return std::system(command.c_str());
  }
};

// ---- the test stage ------------------------------------------------------------------------------------------------------------------------------

// A stage of this test: 32 bytes of constants (two float4), a helper before main, a local after the cube map's line. The words in the text carry `tag`.
sp::VariantDef TestStage(uint32_t feature, std::string name, std::string tag) {
  sp::VariantDef def;
  def.feature = feature;
  def.name = name;
  def.version = 1;
  def.pixel_shaders = {{"test__PS", 1}};
  def.transform = [name, tag](const sp::VariantShader& shader, std::string_view hlsl, const sp::StageContext& ctx) {
    sp::VariantTransformResult r;
    std::string text(hlsl);
    std::string error;
    static const std::string kNames[2] = {"g_TestProbe0", "g_TestProbe1"};
    const std::string a = kNames[0] + tag;
    const std::string b = kNames[1] + tag;
    const sp::ConstantMember members[] = {{"float4", a}, {"uint4", b}};
    if (!sp::DeclareConstants(text, ctx, "test stage " + name, members, &error)) {
      r.error = std::string(shader.name) + ": " + error;
      return r;
    }
    static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0), Lit("#endif"), Ws(0), Lit("void main(")};
    if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                     [&](std::string_view group) { return "float TestHelper" + tag + "() { return " + a + ".x + float(" + b + ".y); }\n" + std::string(group); },
                     &error)) {
      r.error = std::string(shader.name) + ": " + error;
      return r;
    }
    static constexpr Piece kCube[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
    if (!ReplaceOnce(text, kCube, 0, 0, "the start of main's registers",
                     [&](std::string_view group) { return std::string(group) + "\n\tfloat testProbe" + tag + " = TestHelper" + tag + "();"; }, &error)) {
      r.error = std::string(shader.name) + ": " + error;
      return r;
    }
    r.ok = true;
    r.hlsl = std::move(text);
    return r;
  };
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<fs::path> roots;
  fs::path golden, write, hlsli, gbuffer_hlsli, dxc_dir, work;
  std::set<std::string> expect_changes;  // --expect-changes NAME: the variant's texts are not part of the identity (its stage was edited on purpose); the golden's lines are only counted
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    if (a == "--hlsl" && more) roots.push_back(argv[++i]);
    else if (a == "--golden" && more) golden = argv[++i];
    else if (a == "--write" && more) write = argv[++i];
    else if (a == "--hlsli" && more) hlsli = argv[++i];
    else if (a == "--gbuffer-hlsli" && more) gbuffer_hlsli = argv[++i];
    else if (a == "--dxc" && more) dxc_dir = argv[++i];
    else if (a == "--work" && more) work = argv[++i];
    else if (a == "--expect-changes" && more) expect_changes.insert(argv[++i]);
    else {
      std::printf("usage: larecomp_variant_golden_test --hlsl DIR... --hlsli FILE [--gbuffer-hlsli FILE] [--golden FILE | --write FILE] [--expect-changes VARIANT]... [--dxc DIR] [--work DIR]\n");
      return 2;
    }
  }
  if (roots.empty() || hlsli.empty()) {
    std::printf("usage: larecomp_variant_golden_test --hlsl DIR... --hlsli FILE [--gbuffer-hlsli FILE] [--golden FILE | --write FILE] [--expect-changes VARIANT]... [--dxc DIR] [--work DIR]\n");
    return 2;
  }
  std::vector<Shader> files;
  for (const fs::path& root : roots) {
    for (const auto& e : fs::recursive_directory_iterator(root)) {
      if (e.is_regular_file() && e.path().extension() == ".hlsl") files.push_back({e.path().stem().string(), e.path()});
    }
  }
  std::sort(files.begin(), files.end(), [](const Shader& a, const Shader& b) { return a.stem < b.stem; });

  std::vector<sp::VariantDef> defs;
  defs.push_back(sp::MakeHdrSceneVariantFromText(Slurp(hlsli)));
  defs.push_back(sp::MakeBloomGaussVariant());
  defs.push_back(sp::MakeEnvMapVariant());
  defs.push_back(sp::MakeShadowFilterVariant());
  defs.push_back(sp::MakeGlowSoftVariant());
  defs.push_back(sp::MakeCarSelfReflVariant());
  defs.push_back(sp::MakeCarChromeVariant());
  defs.push_back(sp::MakeLuminanceScaleVariant());
  defs.push_back(sp::MakeUiSharpVariant());
#ifdef GOLDEN_HAS_HEADLIGHT
  defs.push_back(sp::MakeHeadlightVariant());
#endif
#ifdef GOLDEN_HAS_GBUFFER
  if (!gbuffer_hlsli.empty()) defs.push_back(sp::MakeGBufferVariantFromText(Slurp(gbuffer_hlsli)));
  else std::printf("no --gbuffer-hlsli: the G-buffer variant is not checked\n");
#endif

  // The table says what the variants say.
  for (const sp::VariantDef& def : defs) {
    const chain::FeatureInfo* row = chain::Find(def.feature);
    Check(row && row->name == def.name && row->stage == chain::Stage::kPixel, "the table has the row of " + def.name);
  }

  // ---- 1. the text of every variant alone ---------------------------------------------------------------------------------------------------------
  std::vector<Line> mine;
  std::map<std::string, size_t> per_variant;
  for (const sp::VariantDef& def : defs) {
    std::vector<sp::VariantShader> shaders;
    const std::vector<const Shader*> targets = TargetsOf(def, files, &shaders);
    for (size_t i = 0; i < targets.size(); ++i) {
      const std::string text = Slurp(targets[i]->path);
      const sp::VariantTransformResult r = sp::ComposeStages(defs, chain::FeatureBit(def.feature), shaders[i], text);
      mine.push_back({def.name, targets[i]->stem, StatusOf(r), std::to_string(r.hlsl.size()), Hex16(Fnv(r.hlsl)), ErrorOf(r)});
      ++per_variant[def.name];
    }
  }
  std::printf("%zu (variant, shader) texts made alone\n", mine.size());
  if (!write.empty()) {
    std::ofstream out(write, std::ios::binary);
    for (const Line& l : mine) out << l.variant << '\t' << l.stem << '\t' << l.status << '\t' << l.size << '\t' << l.fnv << '\t' << l.error << '\n';
    std::printf("wrote %s\n", write.string().c_str());
  }
  if (!golden.empty()) {
    std::map<std::pair<std::string, std::string>, Line> want;
    std::map<std::string, unsigned> golden_versions;
    {
      std::ifstream in(golden);
      std::string row;
      while (std::getline(in, row)) {
        if (!row.empty() && row.back() == '\r') row.pop_back();
        if (row.empty()) continue;
        if (row[0] == '#') {  // the dumper's header: what the golden was made from, and the pre-chain version of each variant
          if (row.rfind("# reference ", 0) == 0) {
            std::printf("golden made from: %s\n", row.c_str() + 12);
          } else if (row.rfind("# version ", 0) == 0) {
            char name[64] = {};
            unsigned version = 0;
            if (std::sscanf(row.c_str() + 10, "%63s %u", name, &version) == 2) golden_versions[name] = version;
          }
          continue;
        }
        Line l;
        size_t from = 0;
        std::string* fields[] = {&l.variant, &l.stem, &l.status, &l.size, &l.fnv, &l.error};
        for (int f = 0; f < 6; ++f) {
          const size_t tab = row.find('\t', from);
          *fields[f] = row.substr(from, tab == std::string::npos ? std::string::npos : tab - from);
          from = tab == std::string::npos ? row.size() : tab + 1;
        }
        want[{l.variant, l.stem}] = l;
      }
    }
    Check(!want.empty(), "the golden file has lines");
    // The versions of the pre-chain code: the stamp lines of the pack (variants_<name>_version) must read what they read before.
    for (const sp::VariantDef& def : defs) {
      const auto v = golden_versions.find(def.name);
      if (golden_versions.empty()) break;
      if (expect_changes.count(def.name)) continue;  // edited on purpose: its version moves with its text
      Check(v != golden_versions.end(), "the golden file has the version of " + def.name);
      if (v != golden_versions.end()) Check(v->second == def.version, def.name + " is version " + std::to_string(def.version) + ", the golden's is " + std::to_string(v->second));
    }
    size_t same = 0, different = 0, missing = 0;
    std::map<std::string, std::array<size_t, 4>> expected;  // per variant of --expect-changes: same, differ, new, gone (counted, never failed)
    std::set<std::pair<std::string, std::string>> seen;
    for (const Line& l : mine) {
      seen.insert({l.variant, l.stem});
      const auto it = want.find({l.variant, l.stem});
      if (expect_changes.count(l.variant)) {
        std::array<size_t, 4>& count = expected[l.variant];
        if (it == want.end()) {
          ++count[2];
        } else if (it->second.status == l.status && it->second.size == l.size && it->second.fnv == l.fnv && it->second.error == l.error) {
          ++count[0];
        } else {
          ++count[1];
          std::printf("INFO: %s %s differs from the golden (a stage edited on purpose: before %s %s, now %s %s)\n", l.variant.c_str(), l.stem.c_str(), it->second.size.c_str(),
                      it->second.fnv.c_str(), l.size.c_str(), l.fnv.c_str());
        }
        continue;
      }
      if (it == want.end()) {
        ++missing;
        std::printf("FAILED: %s %s is not in the golden file\n", l.variant.c_str(), l.stem.c_str());
        continue;
      }
      const Line& g = it->second;
      if (g.status == l.status && g.size == l.size && g.fnv == l.fnv && g.error == l.error) {
        ++same;
      } else {
        ++different;
        if (different <= 10) {
          std::printf("FAILED: %s %s: before %s %s %s, now %s %s %s\n", l.variant.c_str(), l.stem.c_str(), g.status.c_str(), g.size.c_str(), g.fnv.c_str(),
                      l.status.c_str(), l.size.c_str(), l.fnv.c_str());
        }
      }
    }
    size_t gone = 0;
    for (const auto& [key, g] : want) {
      if (!seen.count(key)) {
        if (expect_changes.count(key.first)) ++expected[key.first][3];
        else ++gone;
      }
    }
    for (const auto& [name, count] : expected) {
      std::printf("INFO: %s (--expect-changes): %zu texts as the golden's, %zu differ, %zu new, %zu gone; not part of the identity (the stage's own tests hold its texts to the game's plus the insertions)\n",
                  name.c_str(), count[0], count[1], count[2], count[3]);
    }
    g_checks += 3;
    g_failed += (different != 0) + (missing != 0) + (gone != 0);
    std::printf("byte identity against %s: %zu of %zu texts are the same, %zu differ, %zu are new, %zu are gone\n", golden.string().c_str(), same, want.size(),
                different, missing, gone);
    for (const auto& [name, n] : per_variant) std::printf("  %-16s %zu shaders\n", name.c_str(), n);
  }

  // ---- 2. composition on retail text --------------------------------------------------------------------------------------------------------------
  Dxc dxc;
  if (!dxc_dir.empty()) {
    dxc.exe = dxc_dir / "dxc.exe";
    dxc.work = work.empty() ? fs::temp_directory_path() / "variant_golden_test" : work;
    fs::create_directories(dxc.work);
  }
  struct Case {
    std::string variant;
    std::string stem;
  };
  // One or two shaders of each stage that has them on this PC (the first that the stage makes a text of).
  std::vector<Case> cases;
  {
    std::map<std::string, int> taken;
    for (const Line& l : mine) {
      if (l.status != "ok") continue;
      const int limit = l.variant == "shadow_filter" ? 3 : l.variant == "ui_sharp" ? 2 : 1;
      if (taken[l.variant] >= limit) continue;
      // The shadow collectors have four texts: take different sizes.
      if (l.variant == "shadow_filter") {
        bool new_size = true;
        for (const Case& c : cases) {
          for (const Line& m : mine) {
            if (m.variant == c.variant && m.stem == c.stem && m.size == l.size) new_size = false;
          }
        }
        if (!new_size) continue;
      }
      ++taken[l.variant];
      cases.push_back({l.variant, l.stem});
    }
  }
  int composed_texts = 0, compiled_texts = 0;
  for (const Case& c : cases) {
    const sp::VariantDef* real = nullptr;
    for (const sp::VariantDef& def : defs) {
      if (def.name == c.variant) real = &def;
    }
    const Shader* shader = nullptr;
    for (const Shader& f : files) {
      if (f.stem == c.stem) shader = &f;
    }
    if (!real || !shader) continue;
    const std::string text = Slurp(shader->path);
    const chain::FeatureInfo* real_row = chain::Find(real->feature);
    // "after": the real stage is first (its index is below the test stage's 20); "before": the real stage is renumbered to 25, after the test stage.
    for (const bool real_first : {true, false}) {
      const uint32_t real_feature = real_first ? real->feature : 25;
      sp::VariantDef moved = *real;
      moved.feature = real_feature;
      const std::string tag = "A";
      std::vector<chain::FeatureInfo> table;
      for (const chain::FeatureInfo& row : chain::kFeatures) {
        if (row.feature == real->feature && !real_first) continue;  // moved to 25
        chain::FeatureInfo copy = row;
        copy.composes_with = 0;
        table.push_back(copy);
      }
      chain::FeatureInfo probe = {20, "test_stage", chain::Stage::kPixel, 32, chain::FeatureBit(real_feature), real_row->spec_twin, 0};
      chain::FeatureInfo real_copy = *real_row;
      real_copy.feature = real_feature;
      real_copy.composes_with = chain::FeatureBit(20);
      if (real_first) {
        for (chain::FeatureInfo& row : table) {
          if (row.feature == real_feature) row.composes_with = chain::FeatureBit(20);
        }
        table.push_back(probe);
      } else {
        table.push_back(probe);
        table.push_back(real_copy);
      }
      std::sort(table.begin(), table.end(), [](const chain::FeatureInfo& x, const chain::FeatureInfo& y) { return x.feature < y.feature; });
      const char* why = nullptr;
      Check(chain::TableIsValid(table, &why), std::string("the test table is valid: ") + (why ? why : ""));
      const std::vector<sp::VariantDef> pair = {moved, TestStage(20, "test_stage", tag)};
      const uint32_t set = chain::FeatureBit(20) | chain::FeatureBit(real_feature);
      const std::string label = c.variant + " + the test stage on " + c.stem + (real_first ? " (real stage first)" : " (real stage second)");
      const sp::VariantShader named{c.stem, 0};
      const sp::VariantTransformResult composed = sp::ComposeStages(pair, set, named, text, table);
      Check(composed.ok, label + ": composes (" + composed.error + ")");
      if (!composed.ok) continue;
      ++composed_texts;
      const uint32_t real_at = chain::OffsetOf(set, real_feature, table);
      const uint32_t test_at = chain::OffsetOf(set, 20, table);
      if (real_row->constants_bytes != 0) {
        Check(real_at == (real_first ? 608u : 608u + 32u) && test_at == (real_first ? 608u + real_row->constants_bytes : 608u), label + ": the blocks follow each other");
      }
      Check(CountOccurrences(composed.hlsl, "#define g_TestProbe0A vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + " + std::to_string(test_at) + ", 0x10)") == 1 &&
                CountOccurrences(composed.hlsl, "float4 g_TestProbe0A : packoffset(c" + std::to_string(test_at / 16) + ");") == 1 &&
                CountOccurrences(composed.hlsl, "uint4 g_TestProbe1A : packoffset(c" + std::to_string(test_at / 16 + 1) + ");") == 1,
            label + ": the test stage's block is at " + std::to_string(test_at));
      if (real_row->constants_bytes != 0) {
        Check(CountOccurrences(composed.hlsl, "SharedConstants + " + std::to_string(real_at) + ", 0x10)") == 1 &&
                  CountOccurrences(composed.hlsl, "packoffset(c" + std::to_string(real_at / 16) + ")") == 1,
              label + ": the real stage's block is at " + std::to_string(real_at) + " and nothing else is there");
      }
      // The composed text is not the real stage's alone, and the real stage's own helper is in it.
      const sp::VariantTransformResult alone = sp::ComposeStages(pair, chain::FeatureBit(real_feature), named, text, table);
      Check(alone.ok && composed.hlsl.size() > alone.hlsl.size(), label + ": the composition has more than the stage alone");
      if (dxc.enabled()) {
        const std::string tagged = "c" + std::to_string(composed_texts);
        const bool twin = real_row->spec_twin;
        int bad = 0;
        for (const bool spirv : {false, true}) {
          const int rc = dxc.Compile(composed.hlsl, tagged + (spirv ? "_spv" : "_dxil"), spirv, false, 0);
          if (rc != 0) {
            ++bad;
            std::printf("FAILED: %s: dxc %s exit %d (%s)\n", label.c_str(), spirv ? "SPIR-V" : "DXIL", rc, (dxc.work / (tagged + (spirv ? "_spv" : "_dxil") + (spirv ? ".spv.log" : ".dxil.log"))).string().c_str());
          }
        }
        (void)twin;
        Check(bad == 0, label + ": the composed text compiles to DXIL and SPIR-V");
        compiled_texts += bad == 0 ? 1 : 0;
      }
    }
  }
  std::printf("%d texts composed on retail shaders%s\n", composed_texts, dxc.enabled() ? (", " + std::to_string(compiled_texts) + " compiled with dxc (DXIL and SPIR-V)").c_str() : " (no --dxc: not compiled)");

  // ---- 2b. the real pair: the headlight and the G-buffer on the shaders both edit (xCityDefault's night shader) ---------------------------------------
  // The chain applies the headlight's edits (feature 9) first and the G-buffer's (feature 10) on the text they leave: the headlight's block is at 608 and the G-buffer's follows it
  // at 1280, each member once in each backend, both helpers before main, the term in the ambient sum and the second output; the text compiles to DXIL and SPIR-V, with and without
  // the alpha test's spec constants (the twin: both stages have it). Only when the tree has both variants (each branch alone has one).
  {
    const sp::VariantDef* headlight = nullptr;
    const sp::VariantDef* gbuffer = nullptr;
    for (const sp::VariantDef& def : defs) {
      if (def.feature == sp::kFeatureHeadlight) headlight = &def;
      if (def.feature == sp::kFeatureGBuffer) gbuffer = &def;
    }
    if (headlight && gbuffer) {
      const uint32_t both = chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer);
      Check(chain::CanCompose(both) && chain::HasTwin(both), "the table composes the headlight and the G-buffer, with the twin");
      Check(chain::OffsetOf(both, sp::kFeatureHeadlight) == 608 && chain::OffsetOf(both, sp::kFeatureGBuffer) == 608 + 1072, "the headlight's block is at 608, the G-buffer's at 1680");
      std::vector<sp::VariantShader> hl_shaders;
      const std::vector<const Shader*> hl_targets = TargetsOf(*headlight, files, &hl_shaders);
      int pairs = 0, pair_compiles = 0;
      for (const Shader* file : hl_targets) {
        bool gb_names = false;
        for (const auto& p : gbuffer->pixel_shaders) gb_names = gb_names || p.name == file->stem;
        for (const auto& suffix : gbuffer->pixel_name_suffixes) gb_names = gb_names || std::string_view(file->stem).ends_with(suffix);
        if (!gb_names) continue;
        const std::string text = Slurp(file->path);
        const sp::VariantShader named{file->stem, 0};
        const sp::VariantTransformResult composed = sp::ComposeStages(defs, both, named, text);
        const sp::VariantTransformResult hl_alone = sp::ComposeStages(defs, chain::FeatureBit(sp::kFeatureHeadlight), named, text);
        const sp::VariantTransformResult gb_alone = sp::ComposeStages(defs, chain::FeatureBit(sp::kFeatureGBuffer), named, text);
        const std::string label = "headlight + G-buffer on " + file->stem;
        // A stage that leaves the shader out (the headlight: no light grid; the G-buffer's night stage: no normal) has no pair with the other.
        if (hl_alone.skip || gb_alone.skip) continue;
        Check(composed.ok && hl_alone.ok && gb_alone.ok, label + ": composes (" + composed.error + hl_alone.error + gb_alone.error + ")");
        if (!composed.ok || !hl_alone.ok || !gb_alone.ok) continue;
        ++pairs;
        const std::string& t = composed.hlsl;
        Check(CountOccurrences(t, "#define g_HlHeader vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)") == 1 &&
                  CountOccurrences(t, "float4 g_HlHeader : packoffset(c38);") == 1 &&
                  CountOccurrences(t, "#define g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 1680, 0x10)") == 1 &&
                  CountOccurrences(t, "uint4 g_GBufferMode : packoffset(c105);") == 1 && CountOccurrences(t, "float4 g_GBufferCam : packoffset(c107);") == 1,
              label + ": the blocks are at 608 (c38) and 1680 (c105), once in each backend");
        Check(CountOccurrences(t, "float4 HeadlightLight(") == 1 && CountOccurrences(t, "float2 GbRt1Encode(float3 n)") == 1 &&
                  t.find("float4 HeadlightLight(") < t.rfind("void main(") && t.find("float2 GbRt1Encode(float3 n)") < t.rfind("void main("),
              label + ": both helpers are before main (the last `void main(`: the G-buffer helper's header comment names it too), once");
        Check(CountOccurrences(t, "hlLight.") == CountOccurrences(hl_alone.hlsl, "hlLight.") && CountOccurrences(t, " = HeadlightLight(") == 1 &&
                  CountOccurrences(t, "oC1 : SV_Target1") == 1 && CountOccurrences(t, "oC1 = GbRt1Encode(") == 1,
              label + ": the headlight's term and the G-buffer's output are in once");
        Check(t.size() > hl_alone.hlsl.size() && t.size() > gb_alone.hlsl.size(), label + ": more than either alone");
        if (dxc.enabled()) {
          int bad = 0;
          for (const bool spirv : {false, true}) {
            for (const uint32_t spec : {0u, 2u}) {  // the shader's own spec mask 2 is the alpha test twin's
              const std::string tag = "pair" + std::to_string(pairs) + (spirv ? "_spv" : "_dxil") + std::to_string(spec);
              if (dxc.Compile(t, tag, spirv, false, spec) != 0) {
                ++bad;
                std::printf("FAILED: %s: dxc %s spec %u (%s)\n", label.c_str(), spirv ? "SPIR-V" : "DXIL", spec, (dxc.work / (tag + (spirv ? ".spv.log" : ".dxil.log"))).string().c_str());
              }
            }
          }
          Check(bad == 0, label + ": the composed text compiles to DXIL and SPIR-V (spec constants 0 and 2)");
          pair_compiles += bad == 0 ? 1 : 0;
        }
      }
      Check(pairs > 0, "the headlight and the G-buffer share at least one retail shader (" + std::to_string(pairs) + ")");
      std::printf("%d composed headlight + G-buffer texts%s\n", pairs, dxc.enabled() ? (", " + std::to_string(pair_compiles) + " compiled with dxc (DXIL and SPIR-V, both spec constants)").c_str() : " (no --dxc: not compiled)");
    }
  }

#ifdef GOLDEN_HAS_GBUFFER_DAY
  // ---- 2c. the G-buffer's vertex stage (feature 12) on the real vertex shaders it is for ----------------------------------------------------------------
  // The three vertex shaders of the day families that fetch a normal and export none (the road, the tunnel road, the foliage props): composed alone through the chain, the export is in once, the
  // text is the game's plus it, and it compiles to DXIL and SPIR-V at the vertex shader's spec variants 0 and 1 (the packed normal twin: the table's row has it). The pixel words of the day
  // families read what this stage exports where it is there and take a derivative normal where it is not (gbuffer_day.h), so no pixel word needs it: the renderer pairs it per draw.
  {
    const std::vector<sp::VariantDef> vs_defs = {sp::MakeGBufferVsVariant()};
    const chain::FeatureInfo* vs_row = chain::Find(sp::kFeatureGBufferVs);
    Check(vs_row && vs_row->name == vs_defs[0].name && vs_row->stage == chain::Stage::kVertex && vs_defs[0].vertex && vs_row->spec_twin && vs_row->constants_bytes == 0,
          "the table has the row of gbuffer_vs: a vertex stage with the twin and no block of constants");
    int made = 0, compiled = 0;
    for (const sp::GBufferVsShader& vs_shader : sp::kGBufferVsShaders) {
      const Shader* file = nullptr;
      for (const Shader& f : files) {
        if (f.stem == vs_shader.name) file = &f;
      }
      if (!file) continue;
      const std::string text = Slurp(file->path);
      const sp::VariantTransformResult r = sp::ComposeStages(vs_defs, chain::FeatureBit(sp::kFeatureGBufferVs), {std::string(vs_shader.name), 0}, text);
      Check(r.ok && CountOccurrences(r.hlsl, "oTexCoord15 = float4(") == 1, std::string("the vertex stage exports the normal in ") + std::string(vs_shader.name) + ": " + r.error);
      if (!r.ok) continue;
      ++made;
      if (dxc.enabled()) {
        int bad = 0;
        for (const bool spirv : {false, true}) {
          for (const uint32_t spec : {0u, 1u}) {
            const std::string tag = "gbvs" + std::to_string(made) + (spirv ? "_spv" : "_dxil") + std::to_string(spec);
            if (dxc.Compile(r.hlsl, tag, spirv, true, spec) != 0) {
              ++bad;
              std::printf("FAILED: the G-buffer's vertex stage on %s: dxc %s spec %u\n", std::string(vs_shader.name).c_str(), spirv ? "SPIR-V" : "DXIL", spec);
            }
          }
        }
        Check(bad == 0, "the G-buffer's vertex stage text of " + std::string(vs_shader.name) + " compiles (DXIL and SPIR-V, spec variants 0 and 1)");
        compiled += bad == 0 ? 1 : 0;
      }
    }
    Check(made == int(sp::kGBufferVsShaderCount), "the three real vertex shaders of the G-buffer's vertex stage are in the HLSL folders (" + std::to_string(made) + " of " + std::to_string(sp::kGBufferVsShaderCount) + ")");
    std::printf("%d G-buffer vertex stage texts made on real vertex shaders, %d compiled with dxc\n", made, compiled);
  }
#endif

  // ---- 3. a vertex stage on a real vertex shader --------------------------------------------------------------------------------------------------
  {
    const std::vector<chain::FeatureInfo> table = {{20, "vs_probe", chain::Stage::kVertex, 0, 0, true, 0}};
    sp::VariantDef vertex;
    vertex.feature = 20;
    vertex.name = "vs_probe";
    vertex.vertex = true;
    vertex.pixel_shaders = {{"city__VS", 1}};
    vertex.transform = [](const sp::VariantShader& shader, std::string_view hlsl, const sp::StageContext&) {
      sp::VariantTransformResult r;
      std::string text(hlsl);
      if (CountOccurrences(text, "\toTexCoord15 = 0.0;") != 1 || CountOccurrences(text, "oTexCoord15") != 2) {
        r.skip = true;
        r.error = "TEXCOORD15 is not an unused interpolant";
        return r;
      }
      const size_t at = text.find("\toTexCoord15 = 0.0;");
      text.replace(at, std::string_view("\toTexCoord15 = 0.0;").size(), "\toTexCoord15 = float4(0.25, 0.5, 0.75, 1.0);");
      (void)shader;
      r.ok = true;
      r.hlsl = std::move(text);
      return r;
    };
    const std::vector<sp::VariantDef> vdefs = {vertex};
    int made = 0, skipped = 0, compiled = 0;
    for (const Shader& f : files) {
      if (f.stem.find("__VS") == std::string::npos || made >= 6) continue;
      const std::string text = Slurp(f.path);
      if (text.find("[[vk::location(0)]] in float4 iPosition0") == std::string::npos) continue;
      const sp::VariantTransformResult r = sp::ComposeStages(vdefs, chain::FeatureBit(20), {f.stem, 0}, text, table);
      if (r.skip) {
        ++skipped;
        continue;
      }
      Check(r.ok && CountOccurrences(r.hlsl, "oTexCoord15 = float4(0.25, 0.5, 0.75, 1.0);") == 1, "the vertex stage edits " + f.stem + ": " + r.error);
      if (!r.ok) continue;
      ++made;
      if (dxc.enabled()) {
        int bad = 0;
        for (const bool spirv : {false, true}) {
          for (const uint32_t spec : {0u, 1u}) {
            const std::string tag = "vs" + std::to_string(made) + (spirv ? "_spv" : "_dxil") + std::to_string(spec);
            if (dxc.Compile(r.hlsl, tag, spirv, true, spec) != 0) {
              ++bad;
              std::printf("FAILED: vertex stage on %s: dxc %s spec %u\n", f.stem.c_str(), spirv ? "SPIR-V" : "DXIL", spec);
            }
          }
        }
        Check(bad == 0, "the vertex stage's text of " + f.stem + " compiles (DXIL and SPIR-V, spec variants 0 and 1)");
        compiled += bad == 0 ? 1 : 0;
      }
    }
    Check(made > 0, "at least one real vertex shader takes the vertex stage (" + std::to_string(made) + " made, " + std::to_string(skipped) + " skipped)");
    std::printf("%d real vertex shaders edited by a vertex stage, %d compiled with dxc\n", made, compiled);
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

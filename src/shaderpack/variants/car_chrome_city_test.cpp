// Check of the chrome's city stage (car_chrome_city.h, CITY-RT M2 R3a). A program of its own: it needs no renderer, no ReXGlue, no compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_chrome_city_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_car_chrome_city_test [<folder of translated HLSL> [<folder for the result>]]
//
// Without arguments it runs on a small text made here (the lines the chrome's anchors look at). With a folder of translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files, for
// example shaderpack/baseline/hlsl-rpf of the development PC) it also composes the chrome's stage and this one on xChrome__PS_Chrome.hlsl found in it and, with a second folder, writes the
// text there for a compile with dxc (-T ps_6_0 -E main -HV 2021 -DXENOS_SPEC_CONSTANTS=0 -all-resources-bound -spirv -fvk-use-dx-layout, and without -spirv for DXIL; the game's text is
// never committed, only read).
//
// THE POINT OF THE STAGE IS THAT IT TOUCHES NOTHING OF THE CHROME'S OWN WORDS: the checks below hold the chrome's text (variant version 3) apart from the composition's, byte for byte, and the
// composition's text to the chrome's plus two insertions (the helper, before main, and one call replaced).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variant_overlap.h"
#include "shaderpack/variants/car_chrome.h"
#include "shaderpack/variants/car_chrome_city.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/text_edit.h"

namespace sp = larecomp::shaderpack;
namespace chain = larecomp::shaderpack::chain;

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

size_t Count(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// The same fixture as car_chrome_test.cpp: the parts of the translated chrome the chrome's transform looks at.
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
         "\tr2.x = tfetch2D(FastShadowCollectorSampler_Texture2DDescriptorIndex, FastShadowCollectorSampler_SamplerDescriptorIndex, r3.yx, float2(0, 0)).x;\n"
         "\tr1.xyz = tfetch2DGrad(dpFrontSampler_Texture2DDescriptorIndex, dpFrontSampler_SamplerDescriptorIndex, r1.zw, float2(0, 0), tfetchGradientH.xy, tfetchGradientV.xy).xyz;\n"
         "\tr2.yz = saturate(r0.xz + c255.ww);\n"
         "\tr0.xyz = -r1.xyz + ambYPos.xyz;\n"
         "\tr1.w = saturate(r2.y + r2.z);\n"
         "\tr0.xyz = r1.www * r0.xyz + r1.xyz;\n"
         "\toC0.xyz = r0.www * r0.xyz;\n"
         "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
         "\t}\n"
         "}\n";
}

constexpr uint32_t kChromeBit = 1u << sp::kFeatureCarChrome;
constexpr uint32_t kCityBit = 1u << sp::kFeatureCarChromeCity;

std::vector<sp::VariantDef> Defs() { return {sp::MakeCarChromeVariant(), sp::MakeCarChromeCityVariant()}; }

// The start of the helper this stage inserts (its first line).
constexpr std::string_view kHelperStart = "// LARecomp variant car_chrome_city (";

// The composition's text with the stage's two insertions taken out: the helper (from its first line to the line before main's `#ifndef __spirv__`) and the call put back.
std::string WithoutTheStage(std::string text) {
  const size_t start = text.find(kHelperStart);
  const size_t main_group = text.find("#ifndef __spirv__\n[shader(\"pixel\")]");
  if (start != std::string::npos && main_group != std::string::npos && start < main_group) {
    text.erase(start, main_group - start);
  }
  const std::string call = "r1.xyz = CarChromeCityEnv(r1.xyz, carChrome);";
  const size_t at = text.find(call);
  if (at != std::string::npos) {
    text.replace(at, call.size(), "r1.xyz = CarChromeEnv(r1.xyz, carChrome);");
  }
  return text;
}

void RunOnText(const std::string& game, const std::string& label, bool expect_numbers_too) {
  const std::vector<sp::VariantDef> defs = Defs();
  // The chrome's own word: the chrome's stage alone, and the chain's composition of it alone, are the same text (nothing of this stage in it).
  const sp::VariantTransformResult chrome_alone = sp::TransformCarChromeHlsl(sp::kChrome, game);
  Check(chrome_alone.ok, label + ": the chrome's own text transforms: " + chrome_alone.error);
  if (!chrome_alone.ok) return;
  const sp::VariantTransformResult chain_chrome = sp::ComposeStages(defs, kChromeBit, sp::kChrome, game);
  Check(chain_chrome.ok && chain_chrome.hlsl == chrome_alone.hlsl, label + ": the chrome's word in the chain is the chrome stage's own text, byte for byte");
  Check(!Contains(chrome_alone.hlsl, "CarChromeCity") && !Contains(chrome_alone.hlsl, "car_chrome_city"), label + ": the chrome's own text knows nothing of this stage");
  Check(sp::kCarChromeVariantVersion == 3, label + ": the chrome's own version is still 3 (its stamp, its golden hashes)");

  // The stage alone: nothing to rewrite, the game's text (the pack's entry the compositions are built from).
  const sp::VariantTransformResult alone = sp::ComposeStages(defs, kCityBit, sp::kChrome, game);
  Check(alone.ok && alone.hlsl == sp::textedit::NormalizeNewlines(game), label + ": the stage alone leaves the game's text as it is");

  // With the chrome's stage: the chrome's text with two insertions.
  const sp::VariantTransformResult both = sp::ComposeStages(defs, kChromeBit | kCityBit, sp::kChrome, game);
  Check(both.ok, label + ": the chrome and the city stage compose: " + both.error);
  if (!both.ok) return;
  const std::string& text = both.hlsl;
  Check(WithoutTheStage(text) == chrome_alone.hlsl, label + ": taken out the helper and the call, the composition is the chrome's own text byte for byte (the stage only adds)");
  Check(Count(text, "r1.xyz = CarChromeCityEnv(r1.xyz, carChrome);") == 1 && Count(text, "r1.xyz = CarChromeEnv(r1.xyz, carChrome);") == 0, label + ": the call to the chrome's occlusion is the city's now, once");
  Check(Count(text, "float3 CarChromeCityEnv(float3 env, CarChrState s)") == 1 && Count(text, "CarChrCityRead CarChrCityLookup(") == 1, label + ": the helper is in the text once");
  Check(text.find("float3 CarChromeEnv(") < text.find("float3 CarChromeCityEnv(") && text.find("struct CarChrState") < text.find("struct CarChrCityRead") &&
            text.find("float3 CarChromeCityEnv(") < text.find("void main("),
        label + ": the helper follows the chrome's own (it uses its state and its occlusion) and stands before main");
  Check(text.find("CarChromeCityEnv(r1.xyz, carChrome)") > text.find("carChrome = CarChrSetup(iTexCoord0"), label + ": the call is after the chrome's setup");
  Check(Count(text, "g_CarReflM0 : packoffset(") == 1 && Count(text, "g_CarReflC : packoffset(") == 1, label + ": the chrome's block is declared once and the stage declares none of its own");
  Check(Contains(text, "r0.xyz = -r1.xyz + CarChromeAmbient(ambYPos.xyz, carChrome, saturate(r2.y + r2.z));") && Contains(text, "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"),
        label + ": the chrome's ambient line and the game's alpha test line are untouched");
  if (!expect_numbers_too) return;
  // The numbers of car_chrome_rt_rules.h (ReadForChromeCity, ChromeCityEnv: its test holds the same strings).
  Check(Contains(text, "static const float kCarChrCityMaxColour = 64.0;") && Contains(text, "static const float kCarChrCityModeMin = 1.5;"), label + ": the cap and the mode of the replacement are the rules'");
  Check(Contains(text, "if (code >= 0.5 && !(t.r < -0.5))") && Contains(text, "other += (t.a < 0.0) ? wg * wt : 0.0;") && Contains(text, "if (geometry > 1e-4 && known >= 0.5 * geometry)") &&
            Contains(text, "r.other = (weight > 1e-5) ? saturate(other / weight) : 0.0;") && Contains(text, "r.colour = (weight > 1e-5) ? colour / weight : float3(0.0, 0.0, 0.0);"),
        label + ": the read is the chrome's own (the player's texels, half of the footprint) with the colour and the other-car share kept");
  Check(Contains(text, "if (s.state < 4.5 || s.state > 5.5 || g_CarReflA.x < kCarChrCityModeMin)") && Count(text, "return CarChromeEnv(env, s);") == 2,
        label + ": only the image's answer for the player's own texel at the reflection level is rewritten; everything else keeps the chrome's line");
  Check(Contains(text, "2.0 * (0.004 + 0.0015 * s.ray.depth + s.ray.slack)"), label + ": the read's tolerance is the chrome's own (car_chrome.cpp CarChrSetup)");
  Check(Contains(text, "const float whole = c.weight * s.agreement;") && Contains(text, "const float city = whole * c.other;") && Contains(text, "const float own = whole - city;") &&
            Contains(text, "const float3 base = env * (1.0 - kCarChrDark * saturate(own * strength));") &&
            Contains(text, "const float3 seen = min(max(c.colour, 0.0) * g_CarReflB.z, kCarChrCityMaxColour);") && Contains(text, "return lerp(base, seen, saturate(city * strength));"),
        label + ": the split: the own parts occlude as before, the city and the cars beside replace by the colour and the gain");
  Check(Count(text, "[unroll]") == Count(chrome_alone.hlsl, "[unroll]") + 1 && Count(text, "history.Load(") == Count(chrome_alone.hlsl, "history.Load(") + 1 && Count(text, "[loop]") == 1,
        label + ": one more unrolled read of nine texels, no new loop");
  Check(Count(text, "history.Sample") == 0 && Count(text, "Gather") == 0, label + ": the history is still read as points only");
}

void RunNegatives() {
  const std::vector<sp::VariantDef> defs = Defs();
  const std::string game = Fixture();
  const sp::VariantTransformResult chrome = sp::TransformCarChromeHlsl(sp::kChrome, game);
  Check(chrome.ok, "the chrome's text for the negative checks");
  if (!chrome.ok) return;
  // The chrome's stage claimed in the context but its line missing from the text: refused, with the line named.
  const sp::VariantTransformResult missing = sp::TransformCarChromeCityHlsl(sp::kChrome, game, sp::ContextIn(sp::kFeatureCarChromeCity, kChromeBit | kCityBit));
  Check(!missing.ok && !missing.skip && Contains(missing.error, "line for `env`"), "a text without the chrome stage's line is refused and the line named: " + missing.error);
  // The line doubled: refused.
  std::string doubled = chrome.hlsl;
  const std::string line = "\tr1.xyz = CarChromeEnv(r1.xyz, carChrome);\n";
  const size_t at = doubled.find(line);
  Check(at != std::string::npos, "the chrome's line is in its text");
  if (at != std::string::npos) {
    doubled.insert(at, line);
    const sp::VariantTransformResult twice = sp::TransformCarChromeCityHlsl(sp::kChrome, doubled, sp::ContextIn(sp::kFeatureCarChromeCity, kChromeBit | kCityBit));
    Check(!twice.ok && Contains(twice.error, "line for `env`"), "a doubled line is refused: " + twice.error);
  }
  // No main: refused.
  std::string no_main = chrome.hlsl;
  no_main.replace(no_main.find("void main("), 10, "void other(");
  const sp::VariantTransformResult lost = sp::TransformCarChromeCityHlsl(sp::kChrome, no_main, sp::ContextIn(sp::kFeatureCarChromeCity, kChromeBit | kCityBit));
  Check(!lost.ok && Contains(lost.error, "main"), "a text without main is refused: " + lost.error);
  // Another shader: refused (the stage is for the chrome only).
  const sp::VariantTransformResult paint = sp::TransformCarChromeCityHlsl(sp::kCarPaint, game);
  Check(!paint.ok, "the paint's name is refused");

  // The definition.
  const sp::VariantDef def = sp::MakeCarChromeCityVariant();
  Check(def.feature == sp::kFeatureCarChromeCity && def.feature == 13 && def.feature < sp::kMaxFeatures && def.feature != sp::kFeatureCarChrome, "the feature is 13, inside the word and not the chrome's");
  const chain::FeatureInfo* row = chain::Find(def.feature);
  Check(row && row->name == def.name && row->constants_bytes == 0 && !row->spec_twin && row->stage == chain::Stage::kPixel, "its chain row: no block of constants, no twin, a pixel stage");
  Check(def.name == "car_chrome_city" && def.needs_rpf && def.pixel_shaders.size() == 1 && def.pixel_shaders[0].key == sp::kChrome.key && def.pixel_shaders[0].name == sp::kChrome.name,
        "the definition: one shader, the chrome's identity");
  Check(def.name != sp::MakeCarChromeVariant().name, "its name is not the chrome's (the stamp lines are by name)");
  Check(def.version == sp::kCarChromeCityVariantVersion && sp::kCarChromeCityVariantVersion == 1, "the variant's version is 1");
  // Two stages of one shader that the table composes are not an overlap the builder warns about as a mistake: the table lists the pair (the step builds the composition).
  Check(chain::CanCompose(kChromeBit | kCityBit), "the table composes the chrome's stage and this one");
  const std::vector<sp::VariantTarget> entries = {{"car_chrome", sp::kChrome.key}, {"car_chrome_city", sp::kChrome.key}};
  Check(sp::FindVariantOverlaps(entries).size() == 1, "the two are in one shader (the overlap report names it; the composition is what the table allows)");
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

void RunOnTree(const std::filesystem::path& folder, const std::filesystem::path& results) {
  std::filesystem::path found;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec)) {
    if (entry.is_regular_file() && entry.path().filename() == "xChrome__PS_Chrome.hlsl") {
      found = entry.path();
      break;
    }
  }
  if (found.empty()) {
    std::printf("no xChrome__PS_Chrome.hlsl under %s\n", folder.string().c_str());
    ++g_failed;
    return;
  }
  std::string text;
  Check(ReadFile(found, &text), "read " + found.string());
  RunOnText(text, "retail", true);
  if (!results.empty()) {
    const std::vector<sp::VariantDef> defs = Defs();
    const sp::VariantTransformResult both = sp::ComposeStages(defs, kChromeBit | kCityBit, sp::kChrome, text);
    if (both.ok) {
      std::filesystem::create_directories(results, ec);
      const std::filesystem::path out_path = results / "xChrome__PS_Chrome.car_chrome_city.hlsl";
      std::ofstream out(out_path, std::ios::binary);
      out << both.hlsl;
      std::printf("wrote %s (%zu bytes, the game's %zu)\n", out_path.string().c_str(), both.hlsl.size(), text.size());
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  RunOnText(Fixture(), "fixture", true);
  RunNegatives();
  if (argc > 1) {
    RunOnTree(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

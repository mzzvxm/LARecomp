// Check of the G-buffer variant's text transform (gbuffer.h). A program of its own: it needs no renderer, no ReXGlue, no compiler and no game files, and exits 0
// when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_variant_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_gbuffer_variant_test [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the results>]]]
//
// Without arguments it runs on small texts made here (the shape of the translator's output the edits look at) and a one line stand-in for the helper. With the
// helper file as the first argument the real one is pasted. With a folder of translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files, for example
// shaderpack/baseline/hlsl-rpf/city of the development PC) it also runs on xCityDefault__PSCityDefault and xCityDefault__PSMultiLight, the retail text. A third
// argument is a folder that receives the edited texts, for a compile with dxc (-T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout, and without -spirv for DXIL;
// with -DXENOS_SPEC_CONSTANTS=0 and =2 for the alpha test twin).

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variant_overlap.h"
#include "shaderpack/variants/bloom_gauss.h"
#include "shaderpack/variants/car_chrome.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/env_map.h"
#include "shaderpack/variants/gbuffer.h"
#include "shaderpack/variants/glow_soft.h"
#include "shaderpack/variants/luminance_scale.h"
#include "shaderpack/variants/shadow_filter.h"
#include "shaderpack/variants/ui_sharp.h"

namespace sp = larecomp::shaderpack;

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
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Takes the variant's four insertions out again (the second output, the three constants rows, the helper, the write of the sidecar) and gives back what is
// left: it must be the game's text, byte for byte, with the line ends of the transform (LF). Whatever the game computes its colour with is then untouched by
// the variant, so a colour that differs with the G-buffer on is not the shader text's doing (the spike's pictures of 2026-11-19 differed in the sky, which is
// on the game's own clock: perf\out\GBUFFER-SPIKE.txt, S1).
std::string WithoutVariant(std::string text, std::string helper) {
  std::string missing;
  const auto cut = [&](const char* name, std::string_view piece) {
    const size_t at = text.find(piece);
    if (at == std::string::npos || text.find(piece, at + piece.size()) != std::string::npos) {
      missing += std::string(" ") + name;
      return;
    }
    text.erase(at, piece.size());
  };
  while (!helper.empty() && (helper.back() == '\n' || helper.back() == '\r' || helper.back() == ' ' || helper.back() == '\t')) helper.pop_back();
  std::string no_cr;
  for (const char c : helper) {
    if (c != '\r') no_cr += c;
  }
  cut("the second output", "\n\t, out float2 oC1 : SV_Target1");
  // the SPIR-V rows: a comment line and three #defines after the game's g_InvResolutionScale define
  constexpr std::string_view kCamRow = "#define g_GBufferCam vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 640, 0x10)";
  const size_t begin = text.find("\n// G-buffer variant (src/gpu/gbuffer_types.h GBufferConstants)");
  const size_t cam = text.find(kCamRow);
  if (begin == std::string::npos || cam == std::string::npos || cam < begin) {
    missing += " the SPIR-V rows";
  } else {
    text.erase(begin, cam + kCamRow.size() - begin);
  }
  cut("the DXIL rows", " \\\n    uint4 g_GBufferMode : packoffset(c38); \\\n    float4 g_GBufferRay : packoffset(c39); \\\n    float4 g_GBufferCam : packoffset(c40);");
  cut("the helper", no_cr + "\n\n");
  cut("the sidecar write", "\n\toC1 = float2(0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n\t\toC1 = GbRt1Encode(iTexCoord0.xyz);\n\t}");
  return missing.empty() ? text : "<missing or twice there:" + missing + ">";
}

std::string LfOnly(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c != '\r') out += c;
  }
  return out;
}

// "" when the two texts are the same, else where they first differ (a few bytes of each) for the message of a failed check.
std::string FirstDifference(const std::string& got, const std::string& want) {
  if (got == want) return std::string();
  size_t at = 0;
  while (at < got.size() && at < want.size() && got[at] == want[at]) ++at;
  const auto around = [&](const std::string& text) { return text.substr(at > 20 ? at - 20 : 0, 60); };
  return " (first difference at byte " + std::to_string(at) + ": got '" + around(got) + "', want '" + around(want) + "'; sizes " + std::to_string(got.size()) + " and " +
         std::to_string(want.size()) + ")";
}

// The shape of the translator's pixel shader the edits look at (a day shader: the real text has 700 lines).
constexpr const char* kFixture =
    "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n"
    "#define DEFINE_SHARED_CONSTANTS() \\\n"
    "    float4 g_PosOffset : packoffset(c36); \\\n"
    "    float2 g_InvResolutionScale : packoffset(c37.x);\n"
    "float4 applyBlendPremult(float4 color) { return color; }\n"
    "#ifndef __spirv__\n"
    "[shader(\"pixel\")]\n"
    "#endif\n"
    "void main(\n"
    "\tin float4 iPos : SV_Position,\n"
    "\tin centroid float4 iTexCoord0 : TEXCOORD0,\n"
    "\tin centroid float4 iTexCoord1 : TEXCOORD1,\n"
    ",\n"
    "\tout float4 oC0 : SV_Target0\n"
    "#if XENOS_ALPHA_COVERAGE\n"
    "\t, out uint oCoverage : SV_Coverage\n"
    "#endif\n"
    ")\n"
    "{\n"
    "\tfloat4 r0 = iTexCoord0;\n"
    "\toC0.xyz = r0.xyz;\n"
    "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
    "\t}\t\toC0 = applyBlendPremult(oC0);\n"
    "\treturn;\n"
    "}\n";

constexpr const char* kHelper =
    "float2 GbOctEncode(float3 n) { return n.xy; }\n"
    "float2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\n"
    "#define GB_MODE_WRITE_NORMAL 1\n";

void CheckFixture() {
  const sp::VariantShader day{sp::kGBufferDayShader, 0};
  const sp::VariantShader night{sp::kGBufferNightShader, 0};
  for (const sp::VariantShader& shader : {day, night}) {
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl(shader, kFixture, kHelper);
    Check(r.ok && !r.skip && r.error.empty(), std::string(shader.name) + ": the fixture transforms: " + r.error);
    if (!r.ok) continue;
    Check(Count(r.hlsl, "out float4 oC0 : SV_Target0\n\t, out float2 oC1 : SV_Target1") == 1, "the second output follows the colour output");
    Check(Count(r.hlsl, "oC0 = applyBlendPremult(oC0);\n\toC1 = float2(0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n\t\toC1 = GbRt1Encode(iTexCoord0.xyz);\n\t}") == 1,
          "the write follows the game's last colour statement");
    Check(Count(r.hlsl, "#define g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 608, 0x10)") == 1 &&
              Count(r.hlsl, "#define g_GBufferRay vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 624, 0x10)") == 1 &&
              Count(r.hlsl, "#define g_GBufferCam vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 640, 0x10)") == 1,
          "the SPIR-V block's three rows are at 608, 624 and 640");
    Check(Count(r.hlsl, "float2 g_InvResolutionScale : packoffset(c37.x); \\\n    uint4 g_GBufferMode : packoffset(c38); \\\n    float4 g_GBufferRay : packoffset(c39); \\\n    float4 g_GBufferCam : packoffset(c40);") == 1,
          "the DXIL block's three rows are c38, c39 and c40, the macro's last line has no backslash");
    // the helper is before main, after the declarations
    const size_t helper_at = r.hlsl.find("float2 GbOctEncode(");
    const size_t main_at = r.hlsl.find("void main(");
    const size_t decl_at = r.hlsl.find("#define g_GBufferMode");
    Check(helper_at != std::string::npos && main_at != std::string::npos && decl_at < helper_at && helper_at < main_at, "the declarations, the helper, then main");
    const std::string fixture_diff = FirstDifference(WithoutVariant(r.hlsl, kHelper), LfOnly(kFixture));
    Check(fixture_diff.empty(), std::string(shader.name) + ": with the variant's four insertions taken out the fixture is the game's text again, byte for byte" + fixture_diff);
    // a line end convention does not matter
    std::string crlf;
    for (const char c : std::string(kFixture)) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    const sp::VariantTransformResult again = sp::TransformGBufferHlsl(shader, crlf, std::string(kHelper) + "\r\n");
    Check(again.ok && again.hlsl == r.hlsl, "a text with CRLF line ends gives the same variant");
  }
  // not one of the shaders: left out, no error
  {
    const sp::VariantShader other{"xCityDefault__PSMultiLightInterior", 0};
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl(other, kFixture, kHelper);
    Check(!r.ok && r.skip, "another shader of the group is left out");
  }
  // the anchors: each must be found once
  {
    std::string text = kFixture;
    text.replace(text.find("\tin centroid float4 iTexCoord0 : TEXCOORD0,\n"), std::string("\tin centroid float4 iTexCoord0 : TEXCOORD0,\n").size(), "");
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferDayShader, 0}, text, kHelper);
    Check(!r.ok && !r.skip && r.error.find("iTexCoord0") != std::string::npos, "no TEXCOORD0 interpolant: an error that names it");
  }
  {
    std::string text = kFixture;
    text += "void second(out float4 oC0 : SV_Target0) {}\n";
    // the output anchor is `\tout float4 oC0 : SV_Target0` with the tab: the second has none, so this is still one; make a real second one
    text += "\tout float4 oC0 : SV_Target0\n";
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferDayShader, 0}, text, kHelper);
    Check(!r.ok && !r.skip && r.error.find("oC0 : SV_Target0") != std::string::npos, "two colour outputs: an error that names the anchor");
  }
  {
    std::string text = kFixture;
    text.replace(text.find("oC0 = applyBlendPremult(oC0);"), std::string("oC0 = applyBlendPremult(oC0);").size(), "oC0 = oC0;");
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferNightShader, 0}, text, kHelper);
    Check(!r.ok && !r.skip && r.error.find("colour write") != std::string::npos, "no premultiplied write: an error that names it");
  }
  {
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferDayShader, 0}, kFixture, "");
    Check(!r.ok && !r.skip, "an empty helper is an error");
  }
}

void CheckVariantDef() {
  const sp::VariantDef def = sp::MakeGBufferVariantFromText(kHelper);
  Check(def.feature == sp::kFeatureGBuffer && def.feature == 10, "its feature is the table's bit 10");
  const sp::chain::FeatureInfo* row = sp::chain::Find(def.feature);
  Check(row && row->name == def.name && row->spec_twin && row->constants_bytes == 48 && row->composes_with == sp::chain::FeatureBit(sp::kFeatureHeadlight),
        "the table's row: 48 bytes, the alpha test twin, composes with the headlight's stage");
  Check(def.name == "gbuffer" && def.version == sp::kGBufferVariantVersion && def.needs_rpf, "the name, the version, and it reads the RPF");
  Check(def.pixel_shaders.empty() && def.pixel_name_suffixes.size() == 2 && def.pixel_name_suffixes[0] == sp::kGBufferDayShader &&
            def.pixel_name_suffixes[1] == sp::kGBufferNightShader,
        "the two shaders are found by name");
  Check(def.input_xxh3 != 0, "the helper is in the fingerprint");
  const sp::VariantDef other = sp::MakeGBufferVariantFromText(std::string(kHelper) + "// changed\n");
  Check(other.input_xxh3 != def.input_xxh3, "a changed helper changes the fingerprint");
  const sp::VariantDef crlf = sp::MakeGBufferVariantFromText("float2 GbOctEncode(float3 n) { return n.xy; }\r\nfloat2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\r\n#define GB_MODE_WRITE_NORMAL 1\r\n");
  Check(crlf.input_xxh3 == def.input_xxh3, "the fingerprint does not depend on the line ends");
  // The two shaders are in no other variant of this branch: not named by one (bloom, environment map, glow, car paint, chrome, luminance, UI sharpening) and not
  // under the suffixes of the two that find theirs by name (the shadow collectors, the UI). The headlight's variant (feat/headlights) finds every ...__PSMultiLight by
  // suffix and does edit xCityDefault::PSMultiLight: the chain composes the two (variant_chain.h: the table lists each in the other's row; the golden test composes the
  // real pair when a tree has both).
  {
    const sp::VariantShader others[] = {sp::kBloomGaussX, sp::kBloomGaussY, sp::kChrome, sp::kCarPaint, sp::kEnvMipPass, sp::kEnvReduce, sp::kGlowSoftTrail,
                                        sp::kGlowSoftStar, sp::kLuminanceSample, sp::kUiSharpTextured};
    bool named = false;
    for (const sp::VariantShader& other : others) named |= other.name == sp::kGBufferDayShader || other.name == sp::kGBufferNightShader;
    for (const sp::VariantShader& other : sp::kUiSharpRetail) named |= other.name == sp::kGBufferDayShader || other.name == sp::kGBufferNightShader;
    Check(!named, "no other variant of this branch names xCityDefault::PSCityDefault or ::PSMultiLight");
    bool suffixed = false;
    for (const std::string_view mine : {sp::kGBufferDayShader, sp::kGBufferNightShader}) {
      for (const std::string_view suffix : {sp::kShadowBlendSuffix, sp::kShadowBlendAlphaSuffix}) suffixed |= mine.ends_with(suffix);
      for (const std::string_view suffix : sp::kUiSharpSuffixes) suffixed |= mine.ends_with(suffix);
    }
    Check(!suffixed, "no suffix of the shadow filter's or the UI sharpening's variant finds xCityDefault::PSCityDefault or ::PSMultiLight");
  }
  const sp::VariantShader day{sp::kGBufferDayShader, 1}, night{sp::kGBufferNightShader, 2};
  Check(sp::FindVariantOverlaps(std::vector<sp::VariantTarget>{{"gbuffer", day.key}, {"gbuffer", night.key}, {"shadow_filter", 3}}).empty(), "no overlap with the shadow filter's shaders");
  // the transform through the definition works like the function
  const sp::VariantTransformResult r = def.transform(day, kFixture, sp::SoloContext(sp::kFeatureGBuffer));
  Check(r.ok, "the definition's transform runs");
  // In a composition the block is where the chain puts it: after the headlight's 1072 bytes it is at byte 1680 (c105 to c107), in both backends, and nothing is at 608.
  const sp::StageContext after = sp::ContextIn(sp::kFeatureGBuffer, sp::chain::FeatureBit(sp::kFeatureHeadlight) | sp::chain::FeatureBit(sp::kFeatureGBuffer));
  const sp::VariantTransformResult moved = def.transform(day, kFixture, after);
  Check(after.constants_offset == 1680 && moved.ok, "composed after the headlight's block the offset is 1680");
  Check(Count(moved.hlsl, "#define g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 1680, 0x10)") == 1 &&
            Count(moved.hlsl, "#define g_GBufferRay vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1696, 0x10)") == 1 &&
            Count(moved.hlsl, "#define g_GBufferCam vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1712, 0x10)") == 1 &&
            Count(moved.hlsl, "uint4 g_GBufferMode : packoffset(c105);") == 1 && Count(moved.hlsl, "float4 g_GBufferRay : packoffset(c106);") == 1 &&
            Count(moved.hlsl, "float4 g_GBufferCam : packoffset(c107);") == 1 && Count(moved.hlsl, "SharedConstants + 608") == 0,
        "composed: the three rows are at 1680, 1696 and 1712 (c105 to c107), once in each backend");
}

void CheckRetail(const std::filesystem::path& folder, const std::string& helper, const std::filesystem::path& out) {
  for (const std::string_view name : {sp::kGBufferDayShader, sp::kGBufferNightShader}) {
    const std::filesystem::path file = folder / (std::string(name) + ".hlsl");
    if (!std::filesystem::exists(file)) {
      Check(false, "the retail text is there: " + file.string());
      continue;
    }
    const std::string hlsl = ReadFile(file);
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({name, 0}, hlsl, helper);
    Check(r.ok, std::string(name) + ": the retail text transforms: " + r.error);
    if (!r.ok) continue;
    std::printf("%s: %zu -> %zu bytes\n", std::string(name).c_str(), hlsl.size(), r.hlsl.size());
    // the facts the edits stand on, in the retail text
    Check(Count(hlsl, "float4 r0 = iTexCoord0;") == 1, std::string(name) + ": r0 is loaded from TEXCOORD0");
    Check(Count(hlsl, "float4 r1 = iTexCoord1;") == 1, std::string(name) + ": r1 is loaded from TEXCOORD1 (the position)");
    Check(Count(hlsl, "if (oCoverage == 0u) discard;") == 1, std::string(name) + ": the alpha test discards before the colour write");
    Check(Count(r.hlsl, "oC1 : SV_Target1") == 1 && Count(r.hlsl, "GbRt1Encode(iTexCoord0.xyz)") == 1, std::string(name) + ": one second output, one write");
    // the colour path: nothing the game computes oC0 with is touched (the four insertions are all the variant does)
    const std::string retail_diff = FirstDifference(WithoutVariant(r.hlsl, helper), LfOnly(hlsl));
    Check(retail_diff.empty(), std::string(name) + ": with the variant's four insertions taken out the retail text is the game's again, byte for byte" + retail_diff);
    if (!out.empty()) {
      std::filesystem::create_directories(out);
      std::ofstream(out / (std::string(name) + ".gbuffer.hlsl"), std::ios::binary) << r.hlsl;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckFixture();
  CheckVariantDef();
  std::string helper;
  if (argc > 1) {
    helper = ReadFile(argv[1]);
    Check(!helper.empty(), std::string("the helper file is readable: ") + argv[1]);
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferDayShader, 0}, kFixture, helper);
    Check(r.ok, "the real helper pastes into the fixture: " + r.error);
    Check(Count(r.hlsl, "float2 GbRt1Encode(float3 n)") == 1 && Count(r.hlsl, "float3 GbOctDecode(float2 f)") == 1, "the real helper is in it once");
  }
  if (argc > 2) {
    CheckRetail(argv[2], helper.empty() ? std::string(kHelper) : helper, argc > 3 ? std::filesystem::path(argv[3]) : std::filesystem::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

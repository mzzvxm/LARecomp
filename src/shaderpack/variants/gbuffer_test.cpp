// Check of the G-buffer variant's text transform (gbuffer.h). A program of its own: it needs no renderer, no ReXGlue, no compiler and no game files, and exits 0
// when every check holds. Every shader of the variant goes to the night stage (gbuffer_night.h, xCityDefault::PSMultiLight among them: checked here byte for byte against the game's text, the
// stage's insertions taken out again) or the day stage (gbuffer_day.h, which has xCityDefault::PSCityDefault): this test checks the dispatch and that the definition lists them all, the stages' own
// tests (gbuffer_night_test.cpp, gbuffer_day_test.cpp) check their texts. It also holds the anchor of RT2's albedo (gbuffer_albedo.h, package GB-W1), which both stages share.
//
//   cmake --build <build dir> --target larecomp_gbuffer_variant_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_gbuffer_variant_test [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the results>]]]
//
// Without arguments it runs on small texts made here (the shape of the translator's output the edits look at) and a one line stand-in for the helper. With the
// helper file as the first argument the real one is pasted. With a folder of translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files, for example
// shaderpack/baseline/hlsl-rpf/city of the development PC) it also runs on xCityDefault__PSMultiLight (the spike's text) and xCityDefault__PSCityDefault (the day stage's), the retail text. A third
// argument is a folder that receives the edited texts, for a compile with dxc (-T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout, and without -spirv for DXIL;
// with -DXENOS_SPEC_CONSTANTS=0 and =2 for the alpha test twin).

#include <algorithm>
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
#include "shaderpack/variants/gbuffer_albedo.h"
#include "shaderpack/variants/gbuffer_cars.h"
#include "shaderpack/variants/gbuffer_day.h"
#include "shaderpack/variants/gbuffer_night.h"
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

// Takes the night stage's insertions out again (the second and third outputs, the three constants rows, the helper, the captures, the writes of the sidecar) and gives back what is left: it
// must be the game's text, byte for byte, with the line ends of the transform (LF). Whatever the game computes its colour with is then untouched by the variant, so a colour that differs
// with the G-buffer on is not the shader text's doing (the spike's pictures of 2026-11-19 differed in the sky, which is on the game's own clock: perf\out\GBUFFER-SPIKE.txt, S1).
// `night` is the stage's analysis of the game's text: what it reads the normal from and where the albedo is.
std::string WithoutVariant(std::string text, std::string helper, const sp::NightAnalysis& night) {
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
  cut("the second and third outputs", "\n\t, out float2 oC1 : SV_Target1\n\t, out float4 oC2 : SV_Target2");
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
  if (night.capture_at != std::string::npos) cut("the normal's capture", night.capture);
  if (night.albedo.found && night.albedo.captured) cut("the albedo's capture", night.albedo.capture);
  cut("the sidecar write", "\n\toC1 = float2(0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n\t\toC1 = GbRt1Encode(" + night.expression + ");\n\t}" +
                               sp::GBufferAlbedoWriteText(night.albedo));
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
    "#ifdef __spirv__\n"
    "\tin bool iFace : SV_IsFrontFace\n"
    "#else\n"
    "\tin uint iFace : SV_IsFrontFace\n"
    "#endif\n"
    ",\n"
    "\tout float4 oC0 : SV_Target0\n"
    "#if XENOS_ALPHA_COVERAGE\n"
    "\t, out uint oCoverage : SV_Coverage\n"
    "#endif\n"
    ")\n"
    "{\n"
    "\tfloat4 r0 = iTexCoord0;\n"
    "\tfloat4 r5 = 0.0;\n"
    "\tfloat4 r6 = 0.0;\n"
    "\tr6.xyzw = tfetch2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xwyz;\n"
    "\tr5.w = saturate(dot(r0.zxy, gMultiLightDynamicLightData(10).zxy));\n"
    "\toC0.xyz = r6.xzw * r5.www;\n"
    "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
    "\t}\t\toC0 = applyBlendPremult(oC0);\n"
    "\treturn;\n"
    "}\n";

constexpr const char* kHelper =
    "float2 GbOctEncode(float3 n) { return n.xy; }\n"
    "float2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\n"
    "float3 GbAlbedoEncode(float3 albedo) { return albedo; }\n"
    "float GbMaterialLit(uint word) { return 1.0f; }\n"
    "#define GB_MODE_WRITE_NORMAL 1\n"
    "#define GB_MODE_WRITE_ALBEDO 2\n";

void CheckFixture() {
  const sp::VariantShader day{sp::kGBufferDayShader, 0};
  const sp::VariantShader night{sp::kGBufferNightShader, 0};
  // xCityDefault::PSCityDefault is the day stage's (gbuffer_day.h): the same four insertions, the write with the back face flip. The dispatch gives the day stage's own text.
  {
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl(day, kFixture, kHelper);
    Check(r.ok && !r.skip && r.error.empty(), std::string(day.name) + ": the fixture transforms: " + r.error);
    const sp::VariantTransformResult direct = sp::TransformGBufferDayHlsl(day, kFixture, kHelper);
    Check(direct.ok && direct.hlsl == r.hlsl, std::string(day.name) + ": the text is the day stage's (TransformGBufferDayHlsl), not the spike's");
    Check(Count(r.hlsl, "out float4 oC0 : SV_Target0\n\t, out float2 oC1 : SV_Target1") == 1 && Count(r.hlsl, "float3 gbN = iTexCoord0.xyz;") == 1 &&
              Count(r.hlsl, "oC0 = applyBlendPremult(oC0);\n\toC1 = float2(0.0f, 0.0f);") == 1,
          std::string(day.name) + ": its second output and its write (the interpolant TEXCOORD0, the table's row) follow the game's last colour statement");
  }
  for (const sp::VariantShader& shader : {night}) {
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl(shader, kFixture, kHelper);
    Check(r.ok && !r.skip && r.error.empty(), std::string(shader.name) + ": the fixture transforms: " + r.error);
    if (!r.ok) continue;
    Check(Count(r.hlsl, "out float4 oC0 : SV_Target0\n\t, out float2 oC1 : SV_Target1\n\t, out float4 oC2 : SV_Target2") == 1, "the second and third outputs follow the colour output");
    Check(Count(r.hlsl, "oC0 = applyBlendPremult(oC0);\n\toC1 = float2(0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n\t\toC1 = GbRt1Encode(iTexCoord0.xyz);\n\t}"
                        "\n\toC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_ALBEDO)) != 0u) {\n\t\toC2 = float4(GbAlbedoEncode(r6.xzw), GbMaterialLit(g_GBufferMode.y));\n\t}\n\treturn;") == 1,
          "the writes follow the game's last colour statement: the normal, then the albedo (the fixture's diffuse fetch is at r6.xyzw = tex.xwyz: its lanes r6.xzw, nothing writes them later) and the draw's lit weight");
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
    const std::string fixture_diff = FirstDifference(WithoutVariant(r.hlsl, kHelper, sp::AnalyseGBufferNight(kFixture)), LfOnly(kFixture));
    Check(fixture_diff.empty(), std::string(shader.name) + ": with the stage's insertions taken out the fixture is the game's text again, byte for byte" + fixture_diff);
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
    const sp::VariantShader other{"xCityDefault__PSMultiLightDebug", 0};  // the game's debug shader: not in the night family (gbuffer_night.h; Interior is)
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl(other, kFixture, kHelper);
    Check(!r.ok && r.skip, "another shader of the group is left out");
  }
  // the anchors: each must be found once
  {
    std::string text = kFixture;
    text.replace(text.find("\tin centroid float4 iTexCoord0 : TEXCOORD0,\n"), std::string("\tin centroid float4 iTexCoord0 : TEXCOORD0,\n").size(), "");
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferNightShader, 0}, text, kHelper);
    Check(!r.ok && !r.skip && r.error.find("iTexCoord0") != std::string::npos, "no TEXCOORD0 interpolant: an error that names it");
  }
  {
    std::string text = kFixture;
    text += "void second(out float4 oC0 : SV_Target0) {}\n";
    // the output anchor is `\tout float4 oC0 : SV_Target0` with the tab: the second has none, so this is still one; make a real second one
    text += "\tout float4 oC0 : SV_Target0\n";
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferNightShader, 0}, text, kHelper);
    Check(!r.ok && !r.skip && r.error.find("oC0 : SV_Target0") != std::string::npos, "two colour outputs: an error that names the anchor");
  }
  {
    std::string text = kFixture;
    text.replace(text.find("oC0 = applyBlendPremult(oC0);"), std::string("oC0 = applyBlendPremult(oC0);").size(), "oC0 = oC0;");
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferNightShader, 0}, text, kHelper);
    Check(!r.ok && !r.skip && r.error.find("colour write") != std::string::npos, "no premultiplied write: an error that names it");
  }
  {
    const sp::VariantTransformResult r = sp::TransformGBufferHlsl({sp::kGBufferNightShader, 0}, kFixture, "");
    Check(!r.ok && !r.skip, "an empty helper is an error");
    const sp::VariantTransformResult day_empty = sp::TransformGBufferHlsl({sp::kGBufferDayShader, 0}, kFixture, "");
    Check(!day_empty.ok && !day_empty.skip, "an empty helper is an error for the day stage's shaders too");
  }
}

void CheckVariantDef() {
  const sp::VariantDef def = sp::MakeGBufferVariantFromText(kHelper);
  Check(def.feature == sp::kFeatureGBuffer && def.feature == 10, "its feature is the table's bit 10");
  const sp::chain::FeatureInfo* row = sp::chain::Find(def.feature);
  Check(row && row->name == def.name && row->spec_twin && row->constants_bytes == 48 &&
            row->composes_with == (sp::chain::FeatureBit(sp::kFeatureHeadlight) | sp::chain::FeatureBit(sp::kFeatureCarSelfRefl) | sp::chain::FeatureBit(sp::kFeatureCarChrome)),
        "the table's row: 48 bytes, the alpha test twin, composes with the headlight's stage and the two car variants");
  Check(def.name == "gbuffer" && def.version == sp::kGBufferVariantVersion && def.needs_rpf, "the name, the version, and it reads the RPF");
  Check(def.pixel_shaders.empty() && def.pixel_name_suffixes.size() == 2 + std::size(sp::kGBufferNightSuffixes) + (sp::kGBufferDayShaderCount - 1) + sp::kGBufferCarShaderCount &&
            def.pixel_name_suffixes[0] == sp::kGBufferDayShader && def.pixel_name_suffixes[1] == sp::kGBufferNightShader && def.pixel_name_suffixes[2] == sp::kGBufferNightSuffixes[0],
        "the two shaders, the night family (gbuffer_night.h), the day table's other shaders (gbuffer_day.h) and the car, traffic and character table (gbuffer_cars.h) are found by name");
  {
    bool all_there = true;
    for (const sp::GBufferDayShader& row : sp::kGBufferDayShaders) {
      all_there &= std::find(def.pixel_name_suffixes.begin(), def.pixel_name_suffixes.end(), row.name) != def.pixel_name_suffixes.end();
    }
    Check(all_there, "every shader of the day table is one the definition finds, once");
    bool cars_there = true;
    for (const std::string_view name : sp::GBufferCarShaderNames()) cars_there &= std::find(def.pixel_name_suffixes.begin(), def.pixel_name_suffixes.end(), name) != def.pixel_name_suffixes.end();
    Check(cars_there, "every shader of the car table is one the definition finds, once");
    std::vector<std::string_view> sorted = def.pixel_name_suffixes;
    std::sort(sorted.begin(), sorted.end());
    Check(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "no name is in the definition twice");
  }
  // The version is the pixel stage's stamp line: 6 = the traffic and the pedestrians (a pack stamped with 1 to 5 is built again); the day stage's own number is 2, the car stage's 3.
  Check(sp::kGBufferVariantVersion == 6, "the variant's version is 6: the night family (2), the day families (3), RT2, the albedo and the lit weight (4), the car family (5), the traffic and the pedestrians (6)");
  Check(sp::kGBufferDayVersion == 2 && sp::kGBufferVsVersion == 1 && sp::kGBufferCarsVersion == 3, "the day stage's own version is 2 (RT2), the day vertex stage's is untouched, the car stage's is 3");
  Check(def.input_xxh3 != 0, "the helper is in the fingerprint");
  const sp::VariantDef other = sp::MakeGBufferVariantFromText(std::string(kHelper) + "// changed\n");
  Check(other.input_xxh3 != def.input_xxh3, "a changed helper changes the fingerprint");
  std::string helper_crlf;
  for (const char c : std::string(kHelper)) {
    if (c == '\n') helper_crlf += '\r';
    helper_crlf += c;
  }
  const sp::VariantDef crlf = sp::MakeGBufferVariantFromText(helper_crlf);
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
  // a car shader goes to the car stage (gbuffer_cars.h): a character's shader (normal from TEXCOORD0, the diffuse texel read at the end of main) writes RT1 and RT2 (the stage's planes: no RT3 yet)
  {
    const sp::VariantShader car{"xCharacter__PS", 4};
    const sp::VariantTransformResult rc = def.transform(car, kFixture, sp::SoloContext(sp::kFeatureGBuffer));
    Check(rc.ok && Count(rc.hlsl, "oC1 = GbRt1Encode(gbN);") == 1 && Count(rc.hlsl, "float3 gbN = iTexCoord0.xyz;") == 1 && Count(rc.hlsl, "oC1 : SV_Target1") == 1 && Count(rc.hlsl, "oC2 : SV_Target2") == 1 &&
              Count(rc.hlsl, "oC2 = float4(GbAlbedoEncode(r6.xzw), GbMaterialLit(gbMat));") == 1 && Count(rc.hlsl, "oC3 : SV_Target3") == (sp::kGBufferCarsPlanes >= 3 ? 1u : 0u),
          "a car shader goes to the car stage through the definition: " + rc.error);
    const sp::VariantTransformResult via_function = sp::TransformGBufferHlsl(car, kFixture, kHelper);
    Check(via_function.ok && via_function.hlsl == rc.hlsl, "TransformGBufferHlsl and the definition's transform send a car shader to the same stage");
    // none of the three tables' names is in another
    bool clash = false;
    for (const std::string_view name : sp::GBufferCarShaderNames()) clash |= sp::IsGBufferDayShader(name) || sp::IsGBufferNightShader(name);
    Check(!clash, "no car shader is a day or a night shader: one stage per shader");
  }
  // In a composition the block is where the chain puts it: after the headlight's 672 bytes it is at byte 1280 (c80 to c82), in both backends, and nothing is at 608.
  const sp::StageContext after = sp::ContextIn(sp::kFeatureGBuffer, sp::chain::FeatureBit(sp::kFeatureHeadlight) | sp::chain::FeatureBit(sp::kFeatureGBuffer));
  const sp::VariantTransformResult moved = def.transform(day, kFixture, after);
  Check(after.constants_offset == 1280 && moved.ok, "composed after the headlight's block the offset is 1280");
  Check(Count(moved.hlsl, "#define g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 1280, 0x10)") == 1 &&
            Count(moved.hlsl, "#define g_GBufferRay vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1296, 0x10)") == 1 &&
            Count(moved.hlsl, "#define g_GBufferCam vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1312, 0x10)") == 1 &&
            Count(moved.hlsl, "uint4 g_GBufferMode : packoffset(c80);") == 1 && Count(moved.hlsl, "float4 g_GBufferRay : packoffset(c81);") == 1 &&
            Count(moved.hlsl, "float4 g_GBufferCam : packoffset(c82);") == 1 && Count(moved.hlsl, "SharedConstants + 608") == 0,
        "composed: the three rows are at 1280, 1296 and 1312 (c80 to c82), once in each backend");
}

// The anchor of RT2's albedo (gbuffer_albedo.h) on small texts: which fetch, which lanes, when it is captured and when it is none.
std::string FetchLine(const std::string& dest, const std::string& offset, const std::string& swizzle, const char* indent = "\t", const char* sampler = "DiffuseSampler") {
  return std::string(indent) + dest + " = tfetch2D(" + sampler + "_Texture2DDescriptorIndex, " + sampler + "_SamplerDescriptorIndex, r2.xy, float2(" + offset + "))." + swizzle + ";\n";
}
std::string MainOf(const std::string& body) {
  return "float4 other() { return 0; }\nvoid main(\n\tin float4 iPos : SV_Position\n)\n{\n\tfloat4 r6 = 0.0;\n" + body + "\treturn;\n}\n";
}

void CheckAlbedoAnchors() {
  const std::string kCentre = "0, 0";
  // the lanes: the destination's lanes that hold the texel's x, y, z
  struct Case {
    const char* dest;
    const char* swizzle;
    const char* want;  // empty: none
  };
  const Case cases[] = {
      {"r6.xyzw", "xwyz", "r6.xzw"},  // the city's: r6.x = tex.x, r6.y = tex.w, r6.z = tex.y, r6.w = tex.z
      {"r6.xyzw", "xyzw", "r6.xyz"},
      {"r6.xyzw", "wxyz", "r6.yzw"},
      {"r1.yzw", "xyz", "r1.yzw"},     // a three lane fetch
      {"r5.xyw", "xyz", "r5.xyw"},
      {"r5.xyz", "zxy", "r5.yzx"},     // dest.x = tex.z, dest.y = tex.x, dest.z = tex.y: x in lane y, y in lane z, z in lane x
      {"r0.xyw", "xzy", "r0.xwy"},
      {"r6", "xyzw", "r6.xyz"},        // a bare register: all four lanes
      {"r5.xy", "xy", ""},             // two lanes: an opacity mask, not a colour
      {"r5.xyw", "xyw", ""},           // no z
      {"r5.xyz", "xxz", ""},           // no y
      {"r11.x", "w", ""},              // one channel
  };
  for (const Case& c : cases) {
    const sp::GBufferAlbedo a = sp::AnalyseGBufferAlbedo(MainOf(FetchLine(c.dest, kCentre, c.swizzle)));
    const std::string want = c.want;
    Check(want.empty() ? (!a.found && a.why_not == sp::kGBufferAlbedoSkipNoFetch) : (a.found && a.lanes == want && a.expression == want && !a.captured && a.capture_at == std::string::npos),
          std::string("the fetch `") + c.dest + " = tex." + c.swizzle + "` gives " + (want.empty() ? std::string("no albedo") : want) + " (got '" + a.lanes + "', " + a.why_not + ")");
  }
  // not the texel's centre, not the diffuse sampler, not a plain statement
  Check(!sp::AnalyseGBufferAlbedo(MainOf(FetchLine("r6.xyz", "-0.5, 0", "xyz"))).found, "a fetch at half a texel's offset is a height sample, not the albedo");
  Check(!sp::AnalyseGBufferAlbedo(MainOf(FetchLine("r6.xyz", kCentre, "xyz", "\t", "LitUpSampler"))).found, "another sampler's fetch is not the albedo");
  Check(!sp::AnalyseGBufferAlbedo("float4 x;\n").found, "no main: no albedo");
  Check(!sp::AnalyseGBufferAlbedo(MainOf("\t[branch] if (p0) { r6.xyz = tfetch2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r2.xy, float2(0, 0)).xyz; }\n")).found,
        "a fetch on a line with other statements is not a plain statement of main");
  // the garage floor's and the decals' shape: four half texel fetches of one channel each, then the colour at the centre
  {
    const std::string body = FetchLine("r11.x", "-0.5, 0", "w") + FetchLine("r11.y", "0, -0.5", "w") + FetchLine("r11.z", "0.5, 0", "w") + FetchLine("r11.w", "0, 0.5", "w") + FetchLine("r5.xyw", kCentre, "xyz");
    const sp::GBufferAlbedo a = sp::AnalyseGBufferAlbedo(MainOf(body));
    Check(a.found && a.lanes == "r5.xyw" && a.line == 9, "the garage floor's five fetches: the centre colour one is the albedo (line " + std::to_string(a.line) + ")");
  }
  // the first qualifying fetch is the one
  {
    const sp::GBufferAlbedo a = sp::AnalyseGBufferAlbedo(MainOf(FetchLine("r6.xyz", kCentre, "xyz") + FetchLine("r7.xyz", kCentre, "xyz")));
    Check(a.found && a.lanes == "r6.xyz" && a.line == 5, "two centre fetches: the first");
  }
  // inside a branch: no value on the other path
  {
    const sp::GBufferAlbedo a = sp::AnalyseGBufferAlbedo(MainOf("\tif (p0)\n\t{\n" + FetchLine("r1.xyz", kCentre, "xyz", "\t\t") + "\t}\n"));
    Check(!a.found && a.why_not == sp::kGBufferAlbedoSkipNested, "a fetch inside a branch (the store interiors): no albedo, for that reason");
    const sp::GBufferAlbedo b = sp::AnalyseGBufferAlbedo(MainOf("\tif (p0)\n\t{\n" + FetchLine("r1.xyz", kCentre, "xyz", "\t\t") + "\t}\n" + FetchLine("r6.xyz", kCentre, "xyz")));
    Check(b.found && b.lanes == "r6.xyz", "...unless a later fetch at the top level is there");
  }
  // captured or read at the end of main: a later statement that writes one of the three lanes (any depth), not one that reads them, writes another lane or another register
  struct Later {
    const char* statement;
    bool captured;
    const char* why;
  };
  const Later laters[] = {
      {"\tr3.xyz = r6.xzw * r1.www;\n", false, "a read"},
      {"\tr6.y = r1.x;\n", false, "the alpha lane (y of r6.xyzw = tex.xwyz) is not part of the albedo"},
      {"\tr6.x = r1.x;\n", true, "one lane of the albedo"},
      {"\tr6.xyzw = r1.xyzw;\n", true, "the whole swizzle"},
      {"\tr6 = r1;\n", true, "the bare register"},
      {"\tr6.xzw *= r1.www;\n", true, "a compound assignment"},
      {"\tr6.w += r1.x;\n", true, "a compound assignment on one lane"},
      {"\tif (p0)\n\t{\n\t\tr6.z = r1.x;\n\t}\n", true, "a write inside a branch"},
      {"\tr6.x == r1.x;\n", false, "a comparison is not a write"},
      {"\tr16.x = r1.x;\n", false, "another register that has r6 in its name"},
      {"\tr60.x = r1.x;\n", false, "r60 is not r6"},
      {"\tr7.xyz = r6.xzw;\n", false, "r6 is only the source"},
      {"\tps = r6.x * r6.z;\n\tr3.w = ps;\n", false, "reads"},
  };
  for (const Later& l : laters) {
    const std::string body = FetchLine("r6.xyzw", kCentre, "xwyz") + l.statement;
    const sp::GBufferAlbedo a = sp::AnalyseGBufferAlbedo(MainOf(body));
    Check(a.found && a.captured == l.captured && (a.captured ? a.expression == "gbAlbedo" && a.capture == "\tfloat3 gbAlbedo = r6.xzw;\n" : a.expression == "r6.xzw"),
          std::string("after the fetch: ") + l.why + (l.captured ? " -> captured" : " -> read at the end"));
    if (a.found && a.captured) {
      // the capture goes in the line right after the fetch
      const std::string text = sp::textedit::NormalizeNewlines(MainOf(body));
      Check(a.capture_at == text.find(l.statement), std::string("...the capture is at the start of the line after the fetch: ") + l.why);
    }
  }
  // CRLF is the same
  {
    std::string crlf;
    for (const char c : MainOf(FetchLine("r6.xyzw", kCentre, "xwyz") + "\tr6.x = r1.x;\n")) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    const sp::GBufferAlbedo a = sp::AnalyseGBufferAlbedo(crlf);
    const sp::GBufferAlbedo b = sp::AnalyseGBufferAlbedo(MainOf(FetchLine("r6.xyzw", kCentre, "xwyz") + "\tr6.x = r1.x;\n"));
    Check(a.found && b.found && a.capture_at == b.capture_at && a.capture == b.capture && a.expression == b.expression, "CRLF line ends give the same albedo");
  }
  // the text of the write
  {
    sp::GBufferAlbedo none;
    Check(sp::GBufferAlbedoWriteText(none) == "\n\toC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);", "no albedo: the no data code only");
    const sp::GBufferAlbedo a = sp::AnalyseGBufferAlbedo(MainOf(FetchLine("r6.xyzw", kCentre, "xwyz")));
    Check(sp::GBufferAlbedoWriteText(a) ==
              "\n\toC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_ALBEDO)) != 0u) {\n\t\toC2 = float4(GbAlbedoEncode(r6.xzw), GbMaterialLit(g_GBufferMode.y));\n\t}",
          "the write: the albedo and the draw's lit weight under the mode bit, the no data code else");
  }
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
    if (name == sp::kGBufferDayShader) {
      // the day stage's text (its own test holds it to the game's plus its insertions): here the dispatch, and the text for the compile + instruction identity scripts of the spike
      const sp::VariantTransformResult direct = sp::TransformGBufferDayHlsl({name, 0}, hlsl, helper);
      Check(direct.ok && direct.hlsl == r.hlsl, std::string(name) + ": the retail text goes to the day stage");
      Check(Count(r.hlsl, "oC1 : SV_Target1") == 1 && Count(r.hlsl, "oC1 = GbRt1Encode(gbN);") == 1 && Count(r.hlsl, "oC2 : SV_Target2") == 1, std::string(name) + ": one second output, one write, one third output");
      if (!out.empty()) {
        std::filesystem::create_directories(out);
        std::ofstream(out / (std::string(name) + ".gbuffer.hlsl"), std::ios::binary) << r.hlsl;
      }
      continue;
    }
    // the facts the edits stand on, in the retail text
    Check(Count(hlsl, "float4 r0 = iTexCoord0;") == 1, std::string(name) + ": r0 is loaded from TEXCOORD0");
    Check(Count(hlsl, "float4 r1 = iTexCoord1;") == 1, std::string(name) + ": r1 is loaded from TEXCOORD1 (the position)");
    Check(Count(hlsl, "if (oCoverage == 0u) discard;") == 1, std::string(name) + ": the alpha test discards before the colour write");
    Check(Count(r.hlsl, "oC1 : SV_Target1") == 1 && Count(r.hlsl, "GbRt1Encode(iTexCoord0.xyz)") == 1 && Count(r.hlsl, "oC2 : SV_Target2") == 1, std::string(name) + ": one second output, one write, one third output");
    // RT2: the spike's own shader has its diffuse fetch at r6.xyzw = tex.xwyz (lanes r6.xzw)
    const sp::GBufferAlbedo albedo = sp::AnalyseGBufferAlbedo(hlsl);
    Check(albedo.found && albedo.lanes == "r6.xzw" && Count(r.hlsl, "GbAlbedoEncode(" + albedo.expression + ")") == 1, std::string(name) + ": the albedo is the diffuse fetch's lanes (" + albedo.detail + ")");
    // the colour path: nothing the game computes oC0 with is touched (the stage's insertions are all the variant does)
    const std::string retail_diff = FirstDifference(WithoutVariant(r.hlsl, helper, sp::AnalyseGBufferNight(hlsl)), LfOnly(hlsl));
    Check(retail_diff.empty(), std::string(name) + ": with the stage's insertions taken out the retail text is the game's again, byte for byte" + retail_diff);
    if (!out.empty()) {
      std::filesystem::create_directories(out);
      std::ofstream(out / (std::string(name) + ".gbuffer.hlsl"), std::ios::binary) << r.hlsl;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckFixture();
  CheckAlbedoAnchors();
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

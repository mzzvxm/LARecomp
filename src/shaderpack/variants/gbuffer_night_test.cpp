// Check of the G-buffer variant's night stage (gbuffer_night.h). A program of its own: it needs no renderer, no ReXGlue, no compiler and no game files, and exits 0 when
// every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_night_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_gbuffer_night_test [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the transformed texts>]]]
//
// Without arguments it runs on small texts made here: the shapes of the translator's output the three anchors look at, one by one. With the helper file as the first argument
// the real one is pasted. With a folder of translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files, searched in the folder and under it, for example
// shaderpack/baseline/hlsl-rpf of the development PC) it also runs the CENSUS on the retail text: for every shader of the night family (every ...__PSMultiLight, ...Env and
// ...Interior) it prints "EDITED" with where the normal comes from, or "LEFT OUT" with why, and checks, for each shader it edits:
//   - IDENTITY: the transformed text is the game's text line by line with lines added (the DXIL block's last line gets a ` \` to continue), and no added line assigns one of the game's registers or
//     names oC0: the colour is the game's. Alone, and composed with the headlight's stage (the chain's order: the headlight first), where the text left after the headlight is what is compared
//   - the normal's source agrees where more than one anchor is there (the sun term's interpolant is the lamp sum's, the normalisation's: the shader is analysed again with its sun term hidden)
//   - the composed text has both blocks (608 and 1280) once and the same normal
//   - RT2 (GB-W1, gbuffer_albedo.h): every edited shader declares the third output once; the albedo write is there exactly where the shader has a DiffuseSampler texel at its centre (read at the end
//     of main, or captured right after the fetch where a later statement writes the register), the "no data" write everywhere
//   - xCityDefault::PSMultiLight is the stage's like every other shader of the family (gbuffer.cpp's dispatch; the spike's own edits of it are gone)
// and pins the census' numbers. A third argument is a folder that receives the edited texts (`<name>.gb.hlsl`, and `<name>.hlgb.hlsl` composed with the headlight) for the compile with dxc
// (perf/scripts/gbuffer_night_compile.py: DXIL and SPIR-V, the spec constants 0 and 2).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variants/gbuffer.h"
#include "shaderpack/variants/gbuffer_night.h"
#include "shaderpack/variants/headlight.h"
#include "shaderpack/variants/shadow_filter.h"
#include "shaderpack/variants/ui_sharp.h"

namespace sp = larecomp::shaderpack;
namespace chain = larecomp::shaderpack::chain;
namespace fs = std::filesystem;

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

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string LfOnly(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c != '\r') out += c;
  }
  return out;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t at = 0;
  while (at <= text.size()) {
    const size_t end = text.find('\n', at);
    lines.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return lines;
}

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

// "" when `after` is `before` with lines added: every line of the game's text is in it, in order, as it was (the macro's last line may gain a ` \`), and no line that was added assigns one
// of the game's registers (r0..r31, ps, p0, a0, aL) or mentions the colour output oC0. Else what is wrong.
std::string AdditiveError(const std::string& before_text, const std::string& after_text) {
  const std::vector<std::string> before = SplitLines(LfOnly(before_text));
  const std::vector<std::string> after = SplitLines(after_text);
  std::vector<std::string> added;
  size_t j = 0;
  for (size_t i = 0; i < before.size(); ++i) {
    while (j < after.size() && after[j] != before[i] && after[j] != before[i] + " \\") added.push_back(after[j++]);
    if (j == after.size()) return "line " + std::to_string(i + 1) + " of the game's text is missing or changed: '" + before[i] + "'";
    ++j;
  }
  while (j < after.size()) added.push_back(after[j++]);
  for (const std::string& line : added) {
    if (Contains(line, "oC0")) return "an added line names oC0: '" + line + "'";
    // an assignment to a register of the game's: `r12.xyz = ...`, `ps = ...`, `r3 += ...` at the start of the statement
    size_t at = 0;
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    size_t end = at;
    while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_')) ++end;
    const std::string word = line.substr(at, end - at);
    const bool is_register = (word.size() >= 2 && word[0] == 'r' && std::all_of(word.begin() + 1, word.end(), [](char c) { return c >= '0' && c <= '9'; })) ||
                             word == "ps" || word == "p0" || word == "a0" || word == "aL";
    if (is_register) {
      size_t k = end;
      if (k < line.size() && line[k] == '.') {
        ++k;
        while (k < line.size() && std::string_view("xyzw").find(line[k]) != std::string_view::npos) ++k;
      }
      while (k < line.size() && line[k] == ' ') ++k;
      const bool plain = k < line.size() && line[k] == '=' && !(k + 1 < line.size() && line[k + 1] == '=');
      const bool compound = k + 1 < line.size() && std::string_view("+-*/").find(line[k]) != std::string_view::npos && line[k + 1] == '=';
      if (plain || compound) return "an added line assigns the game's register " + word + ": '" + line + "'";
    }
  }
  return std::string();
}

// ---- the fixtures: the shape of the translator's pixel shader the edits look at ------------------------------------------------------------------------------------------------

std::string Shader(const std::string& body) {
  return "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n"
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
         "\tin centroid float4 iTexCoord2 : TEXCOORD2,\n"
         ",\n"
         "\tout float4 oC0 : SV_Target0\n"
         "#if XENOS_ALPHA_COVERAGE\n"
         "\t, out uint oCoverage : SV_Coverage\n"
         "#endif\n"
         ")\n"
         "{\n"
         "\tfloat4 r0 = iTexCoord0;\n"
         "\tfloat4 r1 = iTexCoord1;\n"
         "\tfloat4 r2 = iTexCoord2;\n"
         "\tfloat4 r3 = 0.0;\n"
         "\tfloat4 r5 = 0.0;\n"
         "\tfloat4 r7 = 0.0;\n"
         "\tfloat4 r14 = 0.0;\n"
         "\tfloat4 r19 = 0.0;\n"
         "\tfloat ps = 0.0;\n"
         "\tfloat4 psSrc = 0.0;\n"
         "\tCubeMapData cubeMapData = (CubeMapData)0;\n"
         "\n" +
         body +
         "\toC0.xyz = r3.xyz;\n"
         "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
         "\t}\t\toC0 = applyBlendPremult(oC0);\n"
         "\treturn;\n"
         "}\n";
}

constexpr const char* kHelper =
    "float2 GbOctEncode(float3 n) { return n.xy; }\n"
    "float2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\n"
    "float3 GbAlbedoEncode(float3 albedo) { return albedo; }\n"
    "float GbMaterialLit(uint word) { return 1.0f; }\n"
    "#define GB_MODE_WRITE_NORMAL 1\n"
    "#define GB_MODE_WRITE_ALBEDO 2\n";

constexpr std::string_view kSunDot = "\tr5.w = saturate(dot(r0.zxy, gMultiLightDynamicLightData(10).zxy));\n";

sp::VariantShader Night(std::string_view name = "xTest__PSMultiLight") { return {name, 0}; }

void CheckAnchors() {
  // 1. the sun term, the interpolant untouched (and its w lane written before: the xyz lanes are what counts)
  {
    const std::string text = Shader("\tr0.w = r1.x;\n" + std::string(kSunDot));
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(text);
    Check(a.found && a.how == sp::NightNormalHow::kSunTerm && a.interpolant && a.expression == "iTexCoord0.xyz" && a.capture_at == std::string::npos,
          "the sun term on an interpolant: iTexCoord0.xyz, no capture (" + a.detail + ")");
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(Night(), text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok && Count(r.hlsl, "oC1 = GbRt1Encode(iTexCoord0.xyz);") == 1 && Count(r.hlsl, "gbNormal") == 0, "...and its text: the write reads the interpolant, " + r.error);
    Check(r.ok && AdditiveError(text, r.hlsl).empty(), "...is the game's text with lines added: " + AdditiveError(text, r.hlsl));
  }
  // the sun term on the interpolant of another index, and its lanes in another order
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr5.w = saturate(dot(r1.yxz, gMultiLightDynamicLightData(10).zxy));\n"));
    // sun z <- r1.y, sun x <- r1.x, sun y <- r1.z: the normal is (r1.x, r1.z, r1.y)
    Check(a.found && a.interpolant && a.expression == "iTexCoord1.xzy", "the lanes are paired, not copied: " + a.expression);
  }
  // 2. the normal is rewritten before the sun term: captured before the statement
  {
    const std::string text = Shader("\tr0.xyz = r0.xyz * r1.www;\n" + std::string(kSunDot));
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(text);
    Check(a.found && !a.interpolant && a.expression == "gbNormal" && a.capture == "\tfloat3 gbNormal = r0.xyz;\n" && a.capture_at == text.find(kSunDot),
          "a rewritten normal is captured before the sun term (" + a.detail + ")");
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(Night(), text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok && Count(r.hlsl, "\tfloat3 gbNormal = r0.xyz;\n\tr5.w = saturate(dot(") == 1 && Count(r.hlsl, "oC1 = GbRt1Encode(gbNormal);") == 1, "...the capture is before the term, the write reads it: " + r.error);
    Check(r.ok && AdditiveError(text, r.hlsl).empty(), "...and only lines were added: " + AdditiveError(text, r.hlsl));
  }
  // a computed register with lanes in the sun's order
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr7.xyw = r1.xyz;\n\tr5.w = saturate(dot(r7.wxy, gMultiLightDynamicLightData(10).zxy));\n"));
    Check(a.found && a.capture == "\tfloat3 gbNormal = r7.xyw;\n", "sun z <- r7.w: the capture is r7.xyw (" + a.capture + ")");
  }
  // the normal is the sun's operand, whichever side it is on
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr5.w = saturate(dot(gMultiLightDynamicLightData(10).zxy, r0.zxy));\n"));
    Check(a.found && a.interpolant && a.expression == "iTexCoord0.xyz", "the sun may be the first operand");
  }
  // 3. the sun's direction scaled first (the road, the windows: lanes in the sun's z y x order, or x y z in the interiors)
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr14.xyz = r5.www * gMultiLightDynamicLightData(10).zyx;\n\tr0.w = saturate(dot(r14.xzy, r0.zxy));\n"));
    Check(a.found && a.interpolant && a.expression == "iTexCoord0.xyz", "the scaled sun, z y x: the normal is r0's own order (" + a.detail + ")");
    const sp::NightAnalysis b = sp::AnalyseGBufferNight(Shader("\tr14.xyz = r5.www * gMultiLightDynamicLightData(10).xyz;\n\tr0.w = saturate(dot(r14.zxy, r1.zxy));\n"));
    Check(b.found && b.interpolant && b.expression == "iTexCoord1.xyz", "the scaled sun, x y z, on interpolant 1 (" + b.detail + ")");
    // the scaled sun's lanes are not the normal's: x y w
    const sp::NightAnalysis c = sp::AnalyseGBufferNight(Shader("\tr14.xzw = r5.zzz * gMultiLightDynamicLightData(10).zyx;\n\tr0.w = saturate(dot(r14.xzw, r2.zxy));\n"));
    // r14.x = sun z, r14.z = sun y, r14.w = sun x; dot(r14.xzw, r2.zxy): z<-r2.z, y<-r2.x, x<-r2.y: (r2.y, r2.x, r2.z)
    Check(c.found && c.interpolant && c.expression == "iTexCoord2.yxz", "lanes x z w: " + c.expression);
  }
  // the scaled sun through scalars
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tps = gMultiLightDynamicLightData(10).z * r5.w;\n\tr19.x = ps;\n\tps = gMultiLightDynamicLightData(10).y * r5.w;\n\tr19.y = ps;\n"
                                                                   "\tps = gMultiLightDynamicLightData(10).x * r5.w;\n\tr19.z = ps;\n\tr2.w = saturate(dot(r19.xzy, r2.zxy));\n"));
    Check(a.found && a.interpolant && a.expression == "iTexCoord2.xyz", "the sun's components through scalars: " + a.detail);
  }
  // the sun dotted with itself is not the normal; a lane the shader overwrote since is not the sun's any more
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr14.xyz = r5.www * gMultiLightDynamicLightData(10).xyz;\n\tr0.w = dot(r14.zxy, r14.zxy);\n"));
    Check(!a.found, "the sun dotted with itself: no normal");
    const sp::NightAnalysis b = sp::AnalyseGBufferNight(Shader("\tr14.xyz = r5.www * gMultiLightDynamicLightData(10).xyz;\n\tr14.y = r1.x;\n\tr0.w = dot(r14.zxy, r0.zxy);\n"));
    Check(!b.found, "a lane overwritten between: it is not the sun's");
  }
  // the tangent space term
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr5.w = saturate(dot(r14.xy, gMultiLightDynamicLightData(10).xy) + gMultiLightDynamicLightData(10).z);\n"));
    Check(!a.found && a.why_not == sp::kGBufferNightSkipTangent, "the (x, y, 1) term is left out for its own reason");
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(Night(), Shader("\tr5.w = saturate(dot(r14.xy, gMultiLightDynamicLightData(10).xy) + gMultiLightDynamicLightData(10).z);\n"), kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(!r.ok && r.skip && r.error == sp::kGBufferNightSkipTangent, "...a skip, with the reason in it");
  }
  // a sun term inside a branch: fine on an untouched interpolant (read at the end of main), left out when the normal would need a capture (a declaration before the statement)
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tif (p0)\n\t{\n\t\tr5.w = saturate(dot(r0.zxy, gMultiLightDynamicLightData(10).zxy));\n\t}\n"));
    Check(a.found && a.interpolant && a.expression == "iTexCoord0.xyz", "a sun term in a branch on an untouched interpolant is fine");
    const sp::NightAnalysis b = sp::AnalyseGBufferNight(Shader("\tr0.xyz = r0.xyz * r1.www;\n\tif (p0)\n\t{\n\t\tr5.w = saturate(dot(r0.zxy, gMultiLightDynamicLightData(10).zxy));\n\t}\n"));
    Check(!b.found && b.why_not == sp::kGBufferNightSkipNested, "a sun term in a branch on a computed normal is left out");
  }
  // 4. no sun term: the lamp sum multiplies an interpolant lane by lane
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr5.xyzw = r7.xwzy * r1.zzzz;\n\tr5.xyzw = r14.xzyw * r1.yyyy + r5.xzwy;\n\tr5.xyzw = r19.xywz * r1.xxxx + r5.xwzy;\n\tr14.x = r1.x * 2.0 + r0.x;\n"));
    Check(a.found && a.how == sp::NightNormalHow::kLampTerm && a.interpolant && a.expression == "iTexCoord1.xyz", "the lamp sum: r1 (" + a.detail + ")");
    // a lane written first is no interpolant's any more
    const sp::NightAnalysis b = sp::AnalyseGBufferNight(Shader("\tr1.y = r0.x;\n\tr5.xyzw = r7.xwzy * r1.zzzz;\n\tr5.xyzw = r14.xzyw * r1.yyyy + r5.xzwy;\n\tr5.xyzw = r19.xywz * r1.xxxx + r5.xwzy;\n"));
    Check(!b.found, "a lane written before the products: not the interpolant");
    // two interpolants with the shape
    const sp::NightAnalysis c = sp::AnalyseGBufferNight(Shader("\tr5.xyzw = r7.xwzy * r1.zzzz + r7.xyzw * r2.xxxx;\n\tr5.xyzw = r14.xzyw * r1.yyyy + r14.xzyw * r2.yyyy;\n\tr5.xyzw = r19.xywz * r1.xxxx + r19.xywz * r2.zzzz;\n\tr5.w = r5.x * r1.z + r5.y * r2.x + r5.z * r2.y;\n"));
    Check(!c.found && c.why_not == sp::kGBufferNightSkipAmbiguous, "two interpolants with the lamps' shape: left out as ambiguous");
  }
  // 5. the normalisation
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr1.w = dot(r1.zxy, r1.zxy);\n\tps = rsqrt(r1.w);\n"));
    Check(a.found && a.how == sp::NightNormalHow::kNormalisation && a.interpolant && a.expression == "iTexCoord1.xyz", "dot(n, n) of an untouched interpolant: its xyz (" + a.detail + ")");
  }
  // 6. nothing: lamps by distance only (it reads the grid), or no light at all
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader("\tr5.xy = r0.xz * LightSortScaleBias.xy + LightSortScaleBias.zw;\n"));
    Check(!a.found && a.why_not == sp::kGBufferNightSkipLampsOnly, "a shader that reads the lamp grid and has no normal");
    const sp::NightAnalysis b = sp::AnalyseGBufferNight(Shader("\tr3.xyz = r0.xyz * r1.www;\n"));
    Check(!b.found && b.why_not == sp::kGBufferNightSkipUnlit, "a shader with no light at all");
  }
  // the diffuse fetch: RT2's albedo (gbuffer_albedo.h; its own anchors are checked in gbuffer_test.cpp)
  {
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(Shader(std::string(kSunDot) + "\tr6.xyzw = tfetch2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r2.xy, float2(0, 0)).xwyz;\n"));
    Check(a.found && a.albedo.found && a.albedo.lanes == "r6.xzw" && a.albedo.expression == "r6.xzw" && !a.albedo.captured, "the diffuse fetch is found for RT2: '" + a.albedo.detail + "'");
    const sp::NightAnalysis none = sp::AnalyseGBufferNight(Shader(std::string(kSunDot)));
    Check(none.found && !none.albedo.found && none.albedo.why_not == sp::kGBufferAlbedoSkipNoFetch, "a shader with a normal and no diffuse fetch has no albedo, for that reason");
  }
}

void CheckTransform() {
  const sp::VariantShader shader = Night("xTest__PSMultiLightInterior");
  const std::string text = Shader("\tr0.xyz = r0.xyz * r1.www;\n" + std::string(kSunDot));
  const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
  Check(r.ok && !r.skip && r.error.empty(), "the fixture transforms: " + r.error);
  if (!r.ok) return;
  Check(Count(r.hlsl, "out float4 oC0 : SV_Target0\n\t, out float2 oC1 : SV_Target1\n\t, out float4 oC2 : SV_Target2") == 1, "the second and third outputs follow the colour output");
  Check(Count(r.hlsl, "oC0 = applyBlendPremult(oC0);\n\toC1 = float2(0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n\t\toC1 = GbRt1Encode(gbNormal);\n\t}"
                      "\n\toC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n\treturn;") == 1,
        "the writes follow the game's last colour statement (this fixture has no diffuse fetch: RT2 gets the no data code only)");
  Check(Count(r.hlsl, "#define g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 608, 0x10)") == 1 &&
            Count(r.hlsl, "#define g_GBufferCam vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 640, 0x10)") == 1 &&
            Count(r.hlsl, "float2 g_InvResolutionScale : packoffset(c37.x); \\\n    uint4 g_GBufferMode : packoffset(c38); \\\n    float4 g_GBufferRay : packoffset(c39); \\\n    float4 g_GBufferCam : packoffset(c40);") == 1,
        "the block is at 608 (c38 to c40) in both backends");
  const size_t helper_at = r.hlsl.find("float2 GbOctEncode(");
  Check(helper_at != std::string::npos && helper_at < r.hlsl.rfind("void main(") && r.hlsl.find("#define g_GBufferMode") < helper_at, "the declarations, the helper, then main");
  Check(r.hlsl.find("\tfloat3 gbNormal = r0.xyz;\n") > r.hlsl.find("void main(") && r.hlsl.find("\tfloat3 gbNormal = r0.xyz;\n") < r.hlsl.find(kSunDot), "the capture is in main, before the term");
  Check(AdditiveError(text, r.hlsl).empty(), "only lines were added: " + AdditiveError(text, r.hlsl));
  // the block moves when the stage is composed after the headlight's 672 bytes
  const sp::StageContext after = sp::ContextIn(sp::kFeatureGBuffer, chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer));
  const sp::VariantTransformResult moved = sp::TransformGBufferNightHlsl(shader, text, kHelper, after);
  Check(moved.ok && Count(moved.hlsl, "SharedConstants + 1280, 0x10)") == 1 && Count(moved.hlsl, "uint4 g_GBufferMode : packoffset(c80);") == 1 && Count(moved.hlsl, "SharedConstants + 608") == 0,
        "composed after the headlight the block is at 1280 (c80)");
  // a text with CRLF line ends is the same
  std::string crlf;
  for (const char c : text) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  const sp::VariantTransformResult again = sp::TransformGBufferNightHlsl(shader, crlf, std::string(kHelper) + "\r\n", sp::SoloContext(sp::kFeatureGBuffer));
  Check(again.ok && again.hlsl == r.hlsl, "CRLF line ends give the same variant");
  // not in the family: left out, no error
  {
    const sp::VariantShader other{"xTest__PSMultiLightDebug", 0};
    const sp::VariantTransformResult skipped = sp::TransformGBufferNightHlsl(other, text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(!skipped.ok && skipped.skip, "a debug shader is not in the family");
  }
  // an anchor that is not there: an error that names it (the translator's text is not the one this was written for)
  {
    std::string broken = text;
    broken.replace(broken.find("oC0 = applyBlendPremult(oC0);"), std::string("oC0 = applyBlendPremult(oC0);").size(), "oC0 = oC0;");
    const sp::VariantTransformResult e = sp::TransformGBufferNightHlsl(shader, broken, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(!e.ok && !e.skip && e.error.find("colour write") != std::string::npos, "no premultiplied write: an error that names it");
    std::string no_tc = Shader("\tr5.w = saturate(dot(r1.zxy, gMultiLightDynamicLightData(10).zxy));\n");
    no_tc.replace(no_tc.find("\tin centroid float4 iTexCoord1 : TEXCOORD1,\n"), std::string("\tin centroid float4 iTexCoord1 : TEXCOORD1,\n").size(), "");
    const sp::VariantTransformResult e2 = sp::TransformGBufferNightHlsl(shader, no_tc, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(!e2.ok && !e2.skip && e2.error.find("iTexCoord1") != std::string::npos, "the interpolant the normal is read from is not declared: an error that names it");
    const sp::VariantTransformResult e3 = sp::TransformGBufferNightHlsl(shader, text, "", sp::SoloContext(sp::kFeatureGBuffer));
    Check(!e3.ok && !e3.skip, "an empty helper is an error");
  }
  // no normal in the shader: a skip with the reason
  {
    const sp::VariantTransformResult s = sp::TransformGBufferNightHlsl(shader, Shader("\tr3.xyz = r0.xyz * r1.www;\n"), kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(!s.ok && s.skip && s.error == sp::kGBufferNightSkipUnlit, "no normal: a skip with the reason");
  }
}

// RT2 in the night stage's text (GB-W1): the albedo is read at the end of main, or captured right after the fetch; both captures (the normal's and the albedo's) at once; none where the
// shader has no texel. Only lines are added in every case. (The anchor's own rules are in gbuffer_test.cpp.)
void CheckAlbedoTransform() {
  const sp::VariantShader shader = Night("xTest__PSMultiLight");
  const std::string fetch = "\tr6.xyzw = tfetch2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r2.xy, float2(0, 0)).xwyz;\n";
  const std::string kWriteTail = "\toC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_ALBEDO)) != 0u) {\n\t\toC2 = float4(GbAlbedoEncode(";
  // 1. the register is untouched after the fetch: read at the end of main, no capture
  {
    const std::string text = Shader(fetch + std::string(kSunDot) + "\tr3.xyz = r6.xzw * r1.www;\n");
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok, "RT2, the register untouched: transforms: " + r.error);
    if (r.ok) {
      Check(Count(r.hlsl, kWriteTail + "r6.xzw), GbMaterialLit(g_GBufferMode.y));\n\t}") == 1 && Count(r.hlsl, "gbAlbedo") == 0, "...the write reads the fetch's own lanes at the end of main (r6.xzw), no capture");
      Check(Count(r.hlsl, "oC2 : SV_Target2") == 1 && Count(r.hlsl, "oC1 = GbRt1Encode(iTexCoord0.xyz);") == 1, "...one third output, the normal still read from the interpolant");
      Check(AdditiveError(text, r.hlsl).empty(), "...and only lines were added: " + AdditiveError(text, r.hlsl));
    }
  }
  // 2. a later statement writes the register: captured right after the fetch, before whatever follows it
  {
    const std::string text = Shader(fetch + std::string(kSunDot) + "\tr6.xyz = r6.xzw * r1.www;\n");
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok, "RT2, the register written later: transforms: " + r.error);
    if (r.ok) {
      Check(Count(r.hlsl, fetch + "\tfloat3 gbAlbedo = r6.xzw;\n" + std::string(kSunDot)) == 1, "...captured on the line right after the fetch");
      Check(Count(r.hlsl, kWriteTail + "gbAlbedo), GbMaterialLit(g_GBufferMode.y));\n\t}") == 1 && Count(r.hlsl, "float3 gbAlbedo = ") == 1, "...and the write reads the capture, once");
      Check(AdditiveError(text, r.hlsl).empty(), "...and only lines were added: " + AdditiveError(text, r.hlsl));
    }
  }
  // 3. both values captured (the normal is rewritten before the sun term, the diffuse register after the fetch): each in its place, in either order of the two lines
  for (const bool fetch_first : {true, false}) {
    const std::string text = Shader(std::string(fetch_first ? fetch : "") + "\tr0.xyz = r0.xyz * r1.www;\n" + std::string(fetch_first ? "" : fetch) + std::string(kSunDot) + "\tr6.x = r6.x * r1.w;\n");
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    const std::string label = std::string("both captured, the fetch ") + (fetch_first ? "first" : "after the normal's rewrite");
    Check(r.ok, label + ": transforms: " + r.error);
    if (r.ok) {
      Check(Count(r.hlsl, "float3 gbNormal = r0.xyz;\n\tr5.w = saturate(dot(") == 1 && Count(r.hlsl, "float3 gbAlbedo = r6.xzw;\n") == 1, label + ": both captures are there once");
      Check(Count(r.hlsl, fetch + "\tfloat3 gbAlbedo = r6.xzw;\n") == 1, label + ": the albedo's is right after the fetch");
      Check(Count(r.hlsl, "oC1 = GbRt1Encode(gbNormal);") == 1 && Count(r.hlsl, "GbAlbedoEncode(gbAlbedo)") == 1, label + ": the writes read them");
      Check(AdditiveError(text, r.hlsl).empty(), label + ": only lines were added: " + AdditiveError(text, r.hlsl));
    }
  }
  // 4. the fetch directly before the sun term's line, which is also a capture point (the same offset): both lines, each once, the colour statements untouched
  {
    const std::string text = Shader("\tr0.xyz = r0.xyz * r1.www;\n" + fetch + std::string(kSunDot) + "\tr6.xyz = r6.xzw * r1.www;\n");
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok && Count(r.hlsl, "float3 gbNormal = r0.xyz;\n") == 1 && Count(r.hlsl, "float3 gbAlbedo = r6.xzw;\n") == 1 && AdditiveError(text, r.hlsl).empty(),
          "the albedo's capture and the normal's at one line: both are in once, only lines were added " + r.error + AdditiveError(text, r.hlsl));
    if (r.ok) {
      const size_t a = r.hlsl.find("float3 gbAlbedo = r6.xzw;"), n = r.hlsl.find("float3 gbNormal = r0.xyz;"), t = r.hlsl.find(kSunDot);
      Check(a != std::string::npos && n != std::string::npos && a < t && n < t, "...and both are before the sun term's statement");
    }
  }
  // 5. no texel: RT2 gets the no data code, nothing more
  {
    const std::string text = Shader("\tr0.xyz = r0.xyz * r1.www;\n" + std::string(kSunDot));
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok && Count(r.hlsl, "oC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);") == 1 && Count(r.hlsl, "GB_MODE_WRITE_ALBEDO") == 1 /* the stand-in helper's define */ && Count(r.hlsl, "gbAlbedo") == 0,
          "a shader with no DiffuseSampler texel: the no data write only, no albedo code " + r.error);
  }
  // 6. a helper without the albedo functions is an error that names them (the real helper has them: the real-helper run checks it)
  {
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, Shader(std::string(kSunDot)), "float2 GbOctEncode(float3 n) { return n.xy; }\nfloat2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\n",
                                                                       sp::SoloContext(sp::kFeatureGBuffer));
    Check(!r.ok && !r.skip && r.error.find("GbAlbedoEncode") != std::string::npos, "a helper without GbAlbedoEncode: an error that names it (" + r.error.substr(0, 100) + ")");
  }
  // 7. composed after the headlight's block: the albedo's write is the same text, the block is at 1280
  {
    const std::string text = Shader(fetch + std::string(kSunDot) + "\tr3.xyz = r6.xzw * r1.www;\n");
    const sp::StageContext after = sp::ContextIn(sp::kFeatureGBuffer, chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer));
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, text, kHelper, after);
    Check(r.ok && Count(r.hlsl, "oC2 = float4(GbAlbedoEncode(r6.xzw), GbMaterialLit(g_GBufferMode.y));") == 1 && Count(r.hlsl, "SharedConstants + 1280, 0x10)") == 1, "composed: the albedo's write and the block at 1280 " + r.error);
  }
}

// The identity check itself must see what it is for: a changed line of the game's, an added assignment to one of its registers, an added mention of the colour.
void CheckIdentityCheck() {
  const std::string text = Shader("\tr0.xyz = r0.xyz * r1.www;\n" + std::string(kSunDot));
  const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(Night(), text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
  if (!r.ok) {
    Check(false, "the identity check's fixture transforms: " + r.error);
    return;
  }
  Check(AdditiveError(text, r.hlsl).empty(), "the identity check accepts the transform's text");
  const auto edited = [&](std::string_view from, std::string_view to) {
    std::string copy = r.hlsl;
    const size_t at = copy.find(from);
    if (at != std::string::npos) copy.replace(at, from.size(), to);
    return copy;
  };
  Check(!AdditiveError(text, edited("\toC0.xyz = r3.xyz;", "\toC0.xyz = r3.xyz * 0.5;")).empty(), "the identity check sees a changed line of the game's");
  Check(!AdditiveError(text, edited("\toC1 = float2(0.0f, 0.0f);", "\tr3.x = 0.0;\n\toC1 = float2(0.0f, 0.0f);")).empty(), "the identity check sees an added assignment to a register of the game's");
  Check(!AdditiveError(text, edited("\toC1 = float2(0.0f, 0.0f);", "\tr3.xy += 1.0;\n\toC1 = float2(0.0f, 0.0f);")).empty(), "...and a compound one");
  Check(!AdditiveError(text, edited("\toC1 = float2(0.0f, 0.0f);", "\tfloat4 mine = oC0;\n\toC1 = float2(0.0f, 0.0f);")).empty(), "the identity check sees an added mention of the colour output");
  Check(AdditiveError(text, edited("\toC1 = float2(0.0f, 0.0f);", "\tfloat4 mine = r3;\n\toC1 = float2(0.0f, 0.0f);")).empty(), "a read of the game's register is fine");
  std::string removed = r.hlsl;
  removed.erase(removed.find("\treturn;\n"), 9);
  Check(!AdditiveError(text, removed).empty(), "the identity check sees a line of the game's that is gone");
}

void CheckVariantDef() {
  const sp::VariantDef def = sp::MakeGBufferVariantFromText(kHelper);
  bool all = true;
  for (const std::string_view suffix : sp::kGBufferNightSuffixes) all &= std::find(def.pixel_name_suffixes.begin(), def.pixel_name_suffixes.end(), suffix) != def.pixel_name_suffixes.end();
  Check(all, "the variant finds the night family by its three suffixes");
  Check(sp::IsGBufferNightShader("xCityRoad__PSMultiLight") && sp::IsGBufferNightShader("xCityRoad__PSMultiLightEnv") && sp::IsGBufferNightShader("xCityRoad__PSMultiLightInterior") &&
            !sp::IsGBufferNightShader("xCityRoad__PSMultiLightDebug") && !sp::IsGBufferNightShader("xPropFoliageImpostor__PSMultiLightNFLDebug") && !sp::IsGBufferNightShader("xCityRoad__PSCityRoad"),
        "the family's names: the debug shaders are not in it");
  // the family is in no other variant: not under the suffixes of the shadow filter's or the UI sharpening's variant
  bool suffixed = false;
  for (const std::string_view mine : sp::kGBufferNightSuffixes) {
    const std::string name = "xTest" + std::string(mine);
    for (const std::string_view suffix : {sp::kShadowBlendSuffix, sp::kShadowBlendAlphaSuffix}) suffixed |= name.ends_with(suffix);
    for (const std::string_view suffix : sp::kUiSharpSuffixes) suffixed |= name.ends_with(suffix);
  }
  Check(!suffixed, "no suffix of the shadow filter's or the UI sharpening's variant finds the family");
  // the registration reaches the night stage through the variant's own transform
  const sp::VariantTransformResult via = def.transform({"xTest__PSMultiLight", 0}, Shader(std::string(kSunDot)), sp::SoloContext(sp::kFeatureGBuffer));
  Check(via.ok && Count(via.hlsl, "oC1 = GbRt1Encode(iTexCoord0.xyz);") == 1, "the variant's transform sends a family shader to the night stage: " + via.error);
  const sp::VariantTransformResult spike = def.transform({sp::kGBufferNightShader, 0}, Shader(std::string(kSunDot)), sp::SoloContext(sp::kFeatureGBuffer));
  Check(spike.ok && via.ok && spike.hlsl == via.hlsl, "xCityDefault::PSMultiLight takes the night stage's text like every other shader of the family (the dispatch)");
}

// ---- the census on the retail text ------------------------------------------------------------------------------------------------------------------------------------------

struct Shader_ {
  std::string stem;
  fs::path path;
};

const char* HowText(sp::NightNormalHow how) {
  switch (how) {
    case sp::NightNormalHow::kSunTerm: return "sun term";
    case sp::NightNormalHow::kLampTerm: return "lamp sum";
    case sp::NightNormalHow::kNormalisation: return "normalisation";
    default: return "-";
  }
}

// What the shader's text says when its sun term is hidden (the constant read as another slot): the other anchors on their own.
std::string WithoutSun(std::string text) {
  const std::string from = "gMultiLightDynamicLightData(10)";
  const std::string to = "gMultiLightDynamicLightData(9)";
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
  return text;
}

void CheckRetail(const fs::path& folder, const std::string& helper, const fs::path& out) {
  std::vector<Shader_> files;
  for (const auto& e : fs::recursive_directory_iterator(folder)) {
    if (e.is_regular_file() && e.path().extension() == ".hlsl") files.push_back({e.path().stem().string(), e.path()});
  }
  std::sort(files.begin(), files.end(), [](const Shader_& a, const Shader_& b) { return a.stem < b.stem; });
  std::vector<const Shader_*> family;
  size_t debug = 0;
  for (const Shader_& file : files) {
    if (sp::IsGBufferNightShader(file.stem)) family.push_back(&file);
    else if (file.stem.find("__PSMultiLight") != std::string::npos) ++debug;
  }
  if (family.empty()) {
    Check(false, "the retail folder has no night family shader: " + folder.string());
    return;
  }
  // The real pair's stages, as the pack builder makes them.
  const std::vector<sp::VariantDef> defs = {sp::MakeHeadlightVariant(), sp::MakeGBufferVariantFromText(helper)};
  const uint32_t both = chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer);
  Check(chain::CanCompose(both) && chain::HasTwin(both) && chain::OffsetOf(both, sp::kFeatureGBuffer) == 1280, "the table composes the headlight and the G-buffer, the block at 1280");

  std::printf("\n=== CENSUS of the night family on %s ===\n", folder.string().c_str());
  std::map<std::string, std::pair<int, int>> by_kind;  // kind -> (edited, left out)
  std::map<std::string, int> by_how, by_reason;
  int edited = 0, left_out = 0, interpolants = 0, captured = 0, composed = 0, with_diffuse = 0, albedo_captured = 0, cross_checked = 0, identical = 0;
  std::map<std::string, int> no_albedo_why;
  std::vector<std::string> no_albedo_names;
  std::map<std::string, std::string> albedo_lanes;
  std::vector<std::string> left_names;
  for (const Shader_* file : family) {
    const std::string text = ReadFile(file->path);
    const std::string kind = file->stem.ends_with("Interior") ? "PSMultiLightInterior" : file->stem.ends_with("Env") ? "PSMultiLightEnv" : "PSMultiLight";
    const sp::VariantShader shader{file->stem, 0};
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(text);
    const sp::VariantTransformResult alone = sp::ComposeStages(defs, chain::FeatureBit(sp::kFeatureGBuffer), shader, text);
    if (!a.found) {
      ++left_out;
      ++by_kind[kind].second;
      ++by_reason[a.why_not];
      left_names.push_back(file->stem);
      std::printf("LEFT OUT  %-46s %s -- %s\n", file->stem.c_str(), a.why_not.c_str(), a.detail.c_str());
      Check(!alone.ok && alone.skip && alone.error == a.why_not, file->stem + ": left out as a skip with the analysis' reason: " + alone.error);
      continue;
    }
    ++edited;
    ++by_kind[kind].first;
    ++by_how[HowText(a.how)];
    (a.interpolant ? interpolants : captured)++;
    if (a.albedo.found) {
      ++with_diffuse;
      albedo_captured += a.albedo.captured;
      albedo_lanes[file->stem] = a.albedo.lanes;
    } else {
      ++no_albedo_why[a.albedo.why_not];
      no_albedo_names.push_back(file->stem);
    }
    std::printf("EDITED    %-46s %-14s %-24s %s%s\n", file->stem.c_str(), HowText(a.how), a.expression.c_str(), a.interpolant ? "" : ("capture: " + a.capture.substr(a.capture.find('f'), a.capture.size() - a.capture.find('f') - 1)).c_str(),
                a.albedo.found ? ("   [albedo " + a.albedo.detail + "]").c_str() : ("   [no albedo: " + a.albedo.why_not + "]").c_str());
    Check(alone.ok, file->stem + ": transforms: " + alone.error);
    if (!alone.ok) continue;
    const std::string identity = AdditiveError(text, alone.hlsl);
    Check(identity.empty(), file->stem + ": the colour is the game's (the text is the game's with lines added): " + identity);
    identical += identity.empty();
    Check(Count(alone.hlsl, "oC1 : SV_Target1") == 1 && Count(alone.hlsl, "oC1 = GbRt1Encode(" + a.expression + ");") == 1 && Count(alone.hlsl, "float3 gbNormal = ") == (a.interpolant ? 0u : 1u),
          file->stem + ": one output, one write, " + (a.interpolant ? "no capture" : "one capture"));
    {
      const std::string albedo_call = "oC2 = float4(GbAlbedoEncode(" + a.albedo.expression + "), GbMaterialLit(g_GBufferMode.y));";
      Check(Count(alone.hlsl, "oC2 : SV_Target2") == 1 && Count(alone.hlsl, "oC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);") == 1 && Count(alone.hlsl, albedo_call) == (a.albedo.found ? 1u : 0u) &&
                Count(alone.hlsl, "float3 gbAlbedo = ") == (a.albedo.found && a.albedo.captured ? 1u : 0u),
            file->stem + ": RT2: one output, the no data write, " + (a.albedo.found ? (a.albedo.captured ? "the albedo from one capture" : "the albedo from the fetch's lanes") : "no albedo"));
    }
    if (!out.empty()) {
      fs::create_directories(out);
      std::ofstream(out / (file->stem + ".gb.hlsl"), std::ios::binary) << alone.hlsl;
    }
    // the same normal when only the other anchors are there (sun term hidden): the interpolant of the sun term must be the lamp sum's and the normalisation's
    if (a.how == sp::NightNormalHow::kSunTerm && a.interpolant) {
      const sp::NightAnalysis other = sp::AnalyseGBufferNight(WithoutSun(text));
      if (other.found) {
        ++cross_checked;
        Check(other.interpolant && other.expression == a.expression, file->stem + ": the lamp sum / the normalisation agree with the sun term: " + other.expression + " against " + a.expression);
      }
    }
    // composed with the headlight's stage
    const sp::VariantTransformResult hl_alone = sp::ComposeStages(defs, chain::FeatureBit(sp::kFeatureHeadlight), shader, text);
    if (hl_alone.ok) {
      const sp::VariantTransformResult pair = sp::ComposeStages(defs, both, shader, text);
      Check(pair.ok, file->stem + ": composes with the headlight: " + pair.error);
      if (pair.ok) {
        ++composed;
        const std::string pair_identity = AdditiveError(hl_alone.hlsl, pair.hlsl);
        Check(pair_identity.empty(), file->stem + ": composed, the headlight's text is the game's with lines added: " + pair_identity);
        const sp::NightAnalysis on_hl = sp::AnalyseGBufferNight(hl_alone.hlsl);
        Check(on_hl.found && on_hl.expression == a.expression && on_hl.interpolant == a.interpolant && on_hl.how == a.how, file->stem + ": the headlight's edits leave the normal where it was");
        Check(Count(pair.hlsl, "float4 g_HlHeader : packoffset(c38);") == 1 && Count(pair.hlsl, "uint4 g_GBufferMode : packoffset(c80);") == 1 && Count(pair.hlsl, "SharedConstants + 1280, 0x10)") == 1 &&
                  Count(pair.hlsl, "hlLight.") == Count(hl_alone.hlsl, "hlLight.") && Count(pair.hlsl, "oC1 = GbRt1Encode(" + a.expression + ");") == 1,
              file->stem + ": composed: the blocks are at 608 and 1280, the headlight's term and the G-buffer's write are in");
        // RT2 composed: the analysis runs on the text the headlight left, so the same anchor must be found there (the headlight does not touch the fetch or write its register)
        const sp::GBufferAlbedo on_hl_albedo = sp::AnalyseGBufferAlbedo(hl_alone.hlsl);
        Check(on_hl_albedo.found == a.albedo.found && on_hl_albedo.lanes == a.albedo.lanes && on_hl_albedo.captured == a.albedo.captured,
              file->stem + ": composed: the headlight's edits leave the albedo's anchor where it was");
        Check(Count(pair.hlsl, "oC2 : SV_Target2") == 1 && Count(pair.hlsl, "GbAlbedoEncode(" + a.albedo.expression + ")") == (a.albedo.found ? 1u : 0u),
              file->stem + ": composed: RT2's output and write are in");
        if (!out.empty()) std::ofstream(out / (file->stem + ".hlgb.hlsl"), std::ios::binary) << pair.hlsl;
      }
    }
    // the spike's own edits and the night stage make the very same text for xCityDefault::PSMultiLight
    if (file->stem == sp::kGBufferNightShader) {
      const sp::VariantTransformResult spike = sp::TransformGBufferHlsl(shader, text, helper, sp::SoloContext(sp::kFeatureGBuffer));
      const sp::VariantTransformResult night = sp::TransformGBufferNightHlsl(shader, text, helper, sp::SoloContext(sp::kFeatureGBuffer));
      Check(spike.ok && night.ok && spike.hlsl == night.hlsl, "xCityDefault::PSMultiLight: the spike's edits and the night stage make the same text");
    }
  }
  std::printf("\nCENSUS: %zu shaders in the night family (%zu debug shaders of the game are not in it): %d EDITED, %d LEFT OUT\n", family.size(), debug, edited, left_out);
  for (const auto& [kind, counts] : by_kind) std::printf("  %-22s edited %3d, left out %3d\n", kind.c_str(), counts.first, counts.second);
  for (const auto& [how, n] : by_how) std::printf("  normal found by the %-14s %3d\n", how.c_str(), n);
  std::printf("  read from the interpolants (no capture) %d, captured %d\n", interpolants, captured);
  std::printf("  RT2: %d of the %d edited shaders have a DiffuseSampler texel for the albedo (%d read at the end of main, %d captured after the fetch); %d have none\n", with_diffuse, edited, with_diffuse - albedo_captured,
              albedo_captured, edited - with_diffuse);
  for (const auto& [why, n] : no_albedo_why) std::printf("  no albedo: %3d  %s\n", n, why.c_str());
  for (const std::string& name : no_albedo_names) std::printf("      no albedo: %s\n", name.c_str());
  for (const auto& [why, n] : by_reason) std::printf("  left out: %3d  %s\n", n, why.c_str());
  std::printf("  %d texts are the game's with lines added; %d composed with the headlight; %d normals cross-checked with the other anchors\n", identical, composed, cross_checked);
  Check(identical == edited, "every edited shader's colour is the game's");
  // The census pinned: the retail game's numbers (shaderpack/baseline/hlsl-rpf of 2026-11-20). A game or translator that is not the retail one says so here.
  if (family.size() == 115) {
    Check(edited == 100 && left_out == 15, "the retail census: 100 edited, 15 left out (got " + std::to_string(edited) + " and " + std::to_string(left_out) + ")");
    const std::set<std::string> want_left = {
        "xCityGrimeMetal__PSMultiLight", "xCityGrimeMetal__PSMultiLightInterior", "xCityRoadStud__PSMultiLight", "xCityRoadStud__PSMultiLightInterior",
        "xCityStoreInterior__PSMultiLight", "xCityStoreInterior__PSMultiLightInterior", "xCityTintBumpDecal__PSMultiLight", "xCityTintBumpDecal__PSMultiLightInterior",
        "xCityTunnelTile__PSMultiLight", "xCityTunnelTile__PSMultiLightInterior", "xPropFoliage__PSMultiLight", "xPropFoliageTrunk__PSMultiLight",
        "xPropFoliageTrunk__PSMultiLightInterior", "xglass_break__PSMultiLight", "xmc4_rmptfx_model__PSMultiLight"};
    Check(std::set<std::string>(left_names.begin(), left_names.end()) == want_left, "the shaders left out are the ones the census names");
    // RT2 (GB-W1): 71 of the 100 edited shaders have a DiffuseSampler texel at its centre that yields x, y and z at the top level of main (51 of them capture it, the shader writes the register again);
    // the 29 that have none are the ones with no diffuse (the traffic car body, the glass, the neon and lamp lens tints), another diffuse (the road and the tunnel road fetch it as two lanes: a
    // luminance and something, the grass and the terrain splat three layers A, B, C by a channel map), or a tint of their own (the grime and the bump spec tints) -- GB-W1 follow ups
    Check(with_diffuse == 71 && albedo_captured == 51, "the retail RT2 census: 71 shaders have the albedo (51 captured) (got " + std::to_string(with_diffuse) + " and " + std::to_string(albedo_captured) + ")");
    const std::set<std::string> want_no_albedo = {
        "xAmbientVehicle__PSMultiLight",
        "xCityGrass__PSMultiLight",
        "xCityGrass__PSMultiLightInterior",
        "xCityGrimeMetalTint__PSMultiLight",
        "xCityGrimeMetalTint__PSMultiLightInterior",
        "xCityGrimeTintDecal__PSMultiLight",
        "xCityGrimeTintDecal__PSMultiLightInterior",
        "xCityGrimeTint__PSMultiLight",
        "xCityGrimeTint__PSMultiLightInterior",
        "xCityLampGlassTint__PSMultiLight",
        "xCityLampGlassTint__PSMultiLightInterior",
        "xCityNeonTint__PSMultiLight",
        "xCityNeonTint__PSMultiLightInterior",
        "xCityRoad__PSMultiLight",
        "xCityRoad__PSMultiLightEnv",
        "xCityRoad__PSMultiLightInterior",
        "xCityTerrain__PSMultiLight",
        "xCityTerrain__PSMultiLightInterior",
        "xCityTunnelRoad__PSMultiLight",
        "xCityTunnelRoad__PSMultiLightEnv",
        "xCityTunnelRoad__PSMultiLightInterior",
        "xPropBumpSpecTint__PSMultiLight",
        "xPropBumpSpecTint__PSMultiLightInterior",
        "xPropGlass__PSMultiLight",
        "xPropGlass__PSMultiLightInterior",
        "xPropLampGlassTint__PSMultiLight",
        "xPropLampGlassTint__PSMultiLightInterior",
        "xPropNeonTint__PSMultiLight",
        "xPropNeonTint__PSMultiLightInterior"};
    Check(std::set<std::string>(no_albedo_names.begin(), no_albedo_names.end()) == want_no_albedo, "the shaders with no albedo are the ones the census names");
    const std::map<std::string, std::string> want_lanes = {
        {"xCityDefault__PSMultiLight", "r6.xzw"},
        {"xCityDefault__PSMultiLightInterior", "r7.xzw"},
        {"xCityBumpSpec__PSMultiLight", "r7.yzw"},
        {"xCityBumpSpec__PSMultiLightEnv", "r2.xyz"},
        {"xCityNormalMap__PSMultiLight", "r9.xyz"},
        {"xCityNormalMap__PSMultiLightInterior", "r15.xzy"},
        {"xPropDefault__PSMultiLight", "r6.yzw"},
        {"xCityLOD__PSMultiLight", "r4.yzw"},
        {"xCityGrime__PSMultiLight", "r7.xyw"},
        {"xCityGrime__PSMultiLightEnv", "r2.xyz"},
        {"xPropMetal__PSMultiLight", "r7.yzw"}};
    for (const auto& [name, lanes] : want_lanes) {
      Check(albedo_lanes.contains(name) && albedo_lanes[name] == lanes, name + ": the albedo is the fetch's lanes " + lanes + " (got " + (albedo_lanes.contains(name) ? albedo_lanes[name] : std::string("none")) + ")");
    }
    Check(by_how["sun term"] == 89 && by_how["lamp sum"] == 8 && by_how["normalisation"] == 3, "the anchors: 89 sun terms, 8 lamp sums, 3 normalisations (got " + std::to_string(by_how["sun term"]) + ", " +
                                                                                                       std::to_string(by_how["lamp sum"]) + ", " + std::to_string(by_how["normalisation"]) + ")");
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckAnchors();
  CheckTransform();
  CheckAlbedoTransform();
  CheckIdentityCheck();
  CheckVariantDef();
  std::string helper;
  if (argc > 1) {
    helper = ReadFile(argv[1]);
    Check(!helper.empty(), std::string("the helper file is readable: ") + argv[1]);
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(Night(), Shader(std::string(kSunDot)), helper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok, "the real helper pastes into the fixture: " + r.error);
    Check(Count(r.hlsl, "float2 GbRt1Encode(float3 n)") == 1 && Count(r.hlsl, "float3 GbOctDecode(float2 f)") == 1, "the real helper is in it once");
  }
  if (argc > 2) {
    CheckRetail(argv[2], helper.empty() ? std::string(kHelper) : helper, argc > 3 ? fs::path(argv[3]) : fs::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

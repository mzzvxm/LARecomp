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
//   - the composed text has both blocks (608 and 1680) once and the same normal
//   - RT2 (GB-W1, gbuffer_albedo.h): every edited shader declares the third output once; the albedo write is there exactly where the shader has a DiffuseSampler texel at its centre (read at the end
//     of main, or captured right after the fetch where a later statement writes the register), the "no data" write everywhere
//   - THE ALBEDO RECIPES (gbuffer_night_albedo.h, GB-H2-COVER): the 11 shaders with no DiffuseSampler texel that the forward headlight stage lights have their anchor found once and write RT2; and
//     THE COVER RULE: every shader the headlight variant edits has the night stage's normal AND an albedo, except the eight named in kGBufferNightHeadlightExempt (the seven emissive ones, tints, plain neon, store sign and lamp glass, which the light
//     pass never lights, and the tangent space decal): a shader that drops out of the G-buffer while the forward stage still lights it fails here, so the deferred headlights cannot lose a surface unseen
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

#include "gpu/gbuffer_material.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variant_shader_fate.h"
#include "shaderpack/variants/gbuffer.h"
#include "shaderpack/variants/gbuffer_night.h"
#include "shaderpack/variants/gbuffer_night_albedo.h"
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
         "\tin centroid float4 iColor1 : COLOR1,\n"
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
    "float2 GbMaterialRt3(uint word) { return float2(0.0f, 0.0f); }\n"
    "#define GB_MODE_WRITE_NORMAL 1\n"
    "#define GB_MODE_WRITE_ALBEDO 2\n"
    "#define GB_MODE_WRITE_MATERIAL 4\n";

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
  Check(Count(r.hlsl, "out float4 oC0 : SV_Target0\n\t, out float2 oC1 : SV_Target1\n\t, out float4 oC2 : SV_Target2\n\t, out float2 oC3 : SV_Target3") == 1,
        "the second, third and fourth outputs follow the colour output");
  Check(Count(r.hlsl, "oC0 = applyBlendPremult(oC0);\n\toC1 = float2(0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n\t\toC1 = GbRt1Encode(gbNormal);\n\t}"
                      "\n\toC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);"
                      "\n\toC3 = float2(0.0f, 0.0f);\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_MATERIAL)) != 0u) {\n\t\toC3 = GbMaterialRt3(g_GBufferMode.y);\n\t}\n\treturn;") == 1,
        "the writes follow the game's last colour statement (this fixture has no diffuse fetch: RT2 gets the no data code only; RT3 gets the material word's bytes all the same)");
  Check(Count(r.hlsl, "#define g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 608, 0x10)") == 1 &&
            Count(r.hlsl, "#define g_GBufferCam vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 640, 0x10)") == 1 &&
            Count(r.hlsl, "float2 g_InvResolutionScale : packoffset(c37.x); \\\n    uint4 g_GBufferMode : packoffset(c38); \\\n    float4 g_GBufferRay : packoffset(c39); \\\n    float4 g_GBufferCam : packoffset(c40);") == 1,
        "the block is at 608 (c38 to c40) in both backends");
  const size_t helper_at = r.hlsl.find("float2 GbOctEncode(");
  Check(helper_at != std::string::npos && helper_at < r.hlsl.rfind("void main(") && r.hlsl.find("#define g_GBufferMode") < helper_at, "the declarations, the helper, then main");
  Check(r.hlsl.find("\tfloat3 gbNormal = r0.xyz;\n") > r.hlsl.find("void main(") && r.hlsl.find("\tfloat3 gbNormal = r0.xyz;\n") < r.hlsl.find(kSunDot), "the capture is in main, before the term");
  Check(AdditiveError(text, r.hlsl).empty(), "only lines were added: " + AdditiveError(text, r.hlsl));
  // the block moves when the stage is composed after the headlight's 1072 bytes
  const sp::StageContext after = sp::ContextIn(sp::kFeatureGBuffer, chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer));
  const sp::VariantTransformResult moved = sp::TransformGBufferNightHlsl(shader, text, kHelper, after);
  Check(moved.ok && Count(moved.hlsl, "SharedConstants + 1680, 0x10)") == 1 && Count(moved.hlsl, "uint4 g_GBufferMode : packoffset(c105);") == 1 && Count(moved.hlsl, "SharedConstants + 608") == 0,
        "composed after the headlight the block is at 1680 (c105)");
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
    Check(r.ok && Count(r.hlsl, "oC3 = GbMaterialRt3(g_GBufferMode.y);") == 1 && Count(r.hlsl, "oC3 = float2(0.0f, 0.0f);") == 1,
          "... and RT3 is written all the same: the class and the smoothness are the draw's, not the texel's");
  }
  // 6. a helper without the albedo functions is an error that names them (the real helper has them: the real-helper run checks it)
  {
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, Shader(std::string(kSunDot)), "float2 GbOctEncode(float3 n) { return n.xy; }\nfloat2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\n",
                                                                       sp::SoloContext(sp::kFeatureGBuffer));
    Check(!r.ok && !r.skip && r.error.find("GbAlbedoEncode") != std::string::npos, "a helper without GbAlbedoEncode: an error that names it (" + r.error.substr(0, 100) + ")");
  }
  // 7. composed after the headlight's block: the albedo's write is the same text, the block is at 1680
  {
    const std::string text = Shader(fetch + std::string(kSunDot) + "\tr3.xyz = r6.xzw * r1.www;\n");
    const sp::StageContext after = sp::ContextIn(sp::kFeatureGBuffer, chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer));
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, text, kHelper, after);
    Check(r.ok && Count(r.hlsl, "oC2 = float4(GbAlbedoEncode(r6.xzw), GbMaterialLit(g_GBufferMode.y));") == 1 && Count(r.hlsl, "SharedConstants + 1680, 0x10)") == 1, "composed: the albedo's write and the block at 1680 " + r.error);
  }
}

// The identity check itself must see what it is for: a changed line of the game's, an added assignment to one of its registers, an added mention of the colour.
// ---- the albedo recipes (gbuffer_night_albedo.h) -----------------------------------------------------------------------------------------------------------------------------

std::string CrlfOf(const std::string& text) {
  std::string out;
  for (const char ch : text) {
    if (ch == '\n') out += '\r';
    out += ch;
  }
  return out;
}

void CheckAlbedoRecipes() {
  // the table: sorted, unique, all in the night family, the rows' own shape
  {
    Check(sp::kGBufferNightAlbedoRowCount == 11, "the table has 11 recipes (the road, the tunnel road, the terrain x2, the grass x2, the tinted grime x3, the tinted prop, the traffic body)");
    bool sorted = true, shape = true, family = true;
    for (size_t i = 0; i < sp::kGBufferNightAlbedoRowCount; ++i) {
      const sp::NightAlbedoRow& row = sp::kGBufferNightAlbedoRows[i];
      if (i > 0 && !(sp::kGBufferNightAlbedoRows[i - 1].name < row.name)) sorted = false;
      family = family && sp::IsGBufferNightShader(row.name);
      const std::string expression(row.expression);
      bool has_register = false;  // `r12.xyz`: a register of the game's at a word boundary
      for (size_t at = expression.find('r'); at != std::string::npos; at = expression.find('r', at + 1)) {
        const bool word_start = at == 0 || !(std::isalnum(static_cast<unsigned char>(expression[at - 1])) || expression[at - 1] == '_');
        if (word_start && at + 1 < expression.size() && std::isdigit(static_cast<unsigned char>(expression[at + 1]))) has_register = true;
      }
      if (row.site == sp::NightAlbedoSite::kGlobal) {
        shape = shape && row.anchor.empty() && !expression.empty() && !has_register;  // constants only: nothing a later statement can overwrite
      } else {
        shape = shape && !row.anchor.empty() && row.anchor.ends_with(";") && !expression.empty() && has_register && row.anchor.find('\n') == std::string_view::npos;
      }
      shape = shape && !row.source.empty();
    }
    Check(sorted, "the recipes are sorted by name and the names are unique");
    Check(family, "every recipe is a shader of the night family");
    Check(shape, "every recipe: a statement and registers, or constants alone, and its words");
    bool exempt_ok = true;
    for (size_t i = 0; i < std::size(sp::kGBufferNightHeadlightExempt); ++i) {
      const sp::NightHeadlightExempt& row = sp::kGBufferNightHeadlightExempt[i];
      if (i > 0 && !(sp::kGBufferNightHeadlightExempt[i - 1].name < row.name)) exempt_ok = false;
      exempt_ok = exempt_ok && sp::IsGBufferNightShader(row.name) && sp::FindGBufferNightAlbedoRow(row.name) == nullptr && !row.reason.empty();
    }
    Check(exempt_ok && std::size(sp::kGBufferNightHeadlightExempt) == 8, "the eight shaders the light pass does not light are sorted, in the family and have no recipe (the two lists do not overlap)");
    Check(sp::FindGBufferNightAlbedoRow("xCityRoad__PSMultiLight") != nullptr && sp::FindGBufferNightAlbedoRow("xCityDefault__PSMultiLight") == nullptr && sp::FindGBufferNightAlbedoRow("") == nullptr,
          "a recipe is found by name; a shader with a diffuse texel has none");
  }
  // the baked occlusion rows (RT2.a)
  {
    bool sorted = true, family = true, power = true, not_emissive = true;
    for (size_t i = 0; i < sp::kGBufferNightOcclusionRowCount; ++i) {
      const sp::NightOcclusionRow& row = sp::kGBufferNightOcclusionRows[i];
      if (i > 0 && !(sp::kGBufferNightOcclusionRows[i - 1].name < row.name)) sorted = false;
      family = family && sp::IsGBufferNightShader(row.name);
      power = power && (row.power == 1 || row.power == 2);
      not_emissive = not_emissive && !(sp::FindGBufferNightHeadlightExempt(row.name) != nullptr) && mcla::gpu::gbuffer::MaterialOfShaderName(row.name).lit > 0.0f;
    }
    Check(sp::kGBufferNightOcclusionRowCount == 48, "the table has 48 occlusion rows (44 by iColor1.w, 4 windows by its square)");
    Check(sorted && family && power, "the occlusion rows are sorted by name, in the night family, power 1 or 2");
    Check(not_emissive, "no emissive or exempt shader carries an occlusion (the pass never lights them)");
    Check(sp::NightOcclusionExpression({"x", 1}) == "iColor1.w" && sp::NightOcclusionExpression({"x", 2}) == "iColor1.w * iColor1.w", "the expressions: the vertex alpha, its square");
    Check(sp::FindGBufferNightOcclusionRow("xCityDefault__PSMultiLight") != nullptr && sp::FindGBufferNightOcclusionRow("xCityLOD__PSMultiLight") == nullptr &&
              sp::FindGBufferNightOcclusionRow("xCityWindowLower__PSMultiLight") != nullptr && sp::FindGBufferNightOcclusionRow("xCityWindowLower__PSMultiLight")->power == 2,
          "the walls have a row, the LOD does not, the windows have the square");
    sp::GBufferAlbedo albedo;
    albedo.found = true;
    albedo.expression = "r6.xzw";
    Check(sp::GBufferAlbedoWriteText(albedo).find("GbMaterialLit(g_GBufferMode.y));") != std::string::npos && sp::GBufferAlbedoWriteText(albedo).find("saturate") == std::string::npos,
          "the albedo write with no occlusion is the old text, byte for byte");
    Check(sp::GBufferAlbedoWriteText(albedo, "iColor1.w").find("oC2 = float4(GbAlbedoEncode(r6.xzw), GbMaterialLit(g_GBufferMode.y) * saturate(iColor1.w));") != std::string::npos,
          "the albedo write with an occlusion multiplies the lit weight by its saturate");
    // the transform: a window (the square), a shader with a row and no iColor1 declaration (the variant is not built), and a shader with no row (the old text)
    const std::string fetch = "\tr6.xyzw = tfetch2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r2.xy, float2(0, 0)).xwyz;\n";
    const std::string window_text = Shader(std::string(kSunDot) + fetch);
    const sp::VariantTransformResult w = sp::TransformGBufferNightHlsl({"xCityWindowLower__PSMultiLight", 0}, window_text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(w.ok && Count(w.hlsl, "oC2 = float4(GbAlbedoEncode(r6.xzw), GbMaterialLit(g_GBufferMode.y) * saturate(iColor1.w * iColor1.w));") == 1 && AdditiveError(window_text, w.hlsl).empty(),
          "a window's lit weight carries the square of the vertex alpha: " + w.error);
    std::string no_decl = window_text;
    const size_t decl = no_decl.find("\tin centroid float4 iColor1 : COLOR1,\n");
    if (decl != std::string::npos) no_decl.erase(decl, std::string("\tin centroid float4 iColor1 : COLOR1,\n").size());
    const sp::VariantTransformResult nd = sp::TransformGBufferNightHlsl({"xCityWindowLower__PSMultiLight", 0}, no_decl, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(decl != std::string::npos && !nd.ok && !nd.skip && nd.error.find("iColor1") != std::string::npos, "a row whose input the text does not declare makes the transform fail: " + nd.error);
    const sp::VariantTransformResult none = sp::TransformGBufferNightHlsl({"xCityLOD__PSMultiLight", 0}, window_text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(none.ok && Count(none.hlsl, "oC2 = float4(GbAlbedoEncode(r6.xzw), GbMaterialLit(g_GBufferMode.y));") == 1 && Count(none.hlsl, "saturate(iColor1") == 0, "a shader with no row keeps the draw's lit weight alone");
  }
  // the machinery on a made-up row: before, after, constants, and the ways an anchor can fail
  {
    const std::string body = "\tr12.xyz = r1.xyz;\n\tr7.z = r1.x + r2.y;\n\tr5.w = r7.z * r12.x;\n";
    const std::string text = Shader(body);
    const std::string lf = LfOnly(text);
    const sp::NightAlbedoRow before{"xTest__PSMultiLight", sp::NightAlbedoSite::kBefore, "r7.z = r1.x + r2.y;", "r12.xyz", "a test"};
    const sp::GBufferAlbedo a = sp::AnalyseGBufferNightAlbedoRow(text, before);
    Check(a.found && a.captured && a.expression == "gbAlbedo" && a.lanes == "r12.xyz" && a.capture == "\tfloat3 gbAlbedo = r12.xyz;\n" && lf.substr(a.capture_at, 8) == "\tr7.z = ",
          "a recipe before its anchor: the capture goes in front of the statement: '" + a.detail + "'");
    const sp::NightAlbedoRow after{"xTest__PSMultiLight", sp::NightAlbedoSite::kAfter, "r7.z = r1.x + r2.y;", "r12.xyz", "a test"};
    const sp::GBufferAlbedo b = sp::AnalyseGBufferNightAlbedoRow(text, after);
    Check(b.found && b.captured && lf.substr(b.capture_at, 7) == "\tr5.w =" && b.capture_at > a.capture_at, "a recipe after its anchor: the capture goes on the next line");
    const sp::NightAlbedoRow global{"xTest__PSMultiLight", sp::NightAlbedoSite::kGlobal, "", "DiffuseColor.xyz", "a test"};
    const sp::GBufferAlbedo c = sp::AnalyseGBufferNightAlbedoRow(text, global);
    Check(c.found && !c.captured && c.expression == "DiffuseColor.xyz" && c.capture.empty() && c.capture_at == std::string::npos, "a recipe of constants: no capture, the expression is written as it is");
    const sp::NightAlbedoRow missing{"xTest__PSMultiLight", sp::NightAlbedoSite::kBefore, "r7.z = r1.x + r2.z;", "r12.xyz", "a test"};
    const sp::GBufferAlbedo d = sp::AnalyseGBufferNightAlbedoRow(text, missing);
    Check(!d.found && d.why_not.find("found 0 times") != std::string::npos, "an anchor that is not there: not found, for that reason (" + d.why_not + ")");
    const sp::GBufferAlbedo e = sp::AnalyseGBufferNightAlbedoRow(Shader(body + body), before);
    Check(!e.found && e.why_not.find("found 2 times") != std::string::npos, "an anchor found twice: not found, for that reason (" + e.why_not + ")");
    const sp::GBufferAlbedo f = sp::AnalyseGBufferNightAlbedoRow(Shader("\tif (p0)\n\t{\n\t\tr7.z = r1.x + r2.y;\n\t}\n"), before);
    Check(!f.found && f.why_not.find("top level") != std::string::npos, "an anchor inside a branch is not the top level's (" + f.why_not + ")");
    Check(!sp::AnalyseGBufferNightAlbedoRow("float4 x;\n", before).found && !sp::AnalyseGBufferNightAlbedoRow("float4 x;\n", global).found, "no main: no recipe");
    const sp::GBufferAlbedo h = sp::AnalyseGBufferNightAlbedoRow(CrlfOf(text), before);
    Check(h.found && h.capture_at == a.capture_at && h.capture == a.capture, "CRLF line ends give the same recipe");
  }
  // the transform with a real recipe's name: the terrain's (before the statement), the traffic body's (constants); and an anchor that is not there
  {
    const sp::NightAlbedoRow* terrain = sp::FindGBufferNightAlbedoRow("xCityTerrain__PSMultiLight");
    const std::string terrain_text = Shader(std::string(kSunDot) + "\tr12.xyz = r1.xyz;\n\t" + std::string(terrain->anchor) + "\n\tr5.w = r7.z * r12.x;\n");
    const sp::VariantShader shader{"xCityTerrain__PSMultiLight", 0};
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, terrain_text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok && Count(r.hlsl, "\tfloat3 gbAlbedo = r12.xyz;\n\t" + std::string(terrain->anchor)) == 1 && Count(r.hlsl, "oC2 = float4(GbAlbedoEncode(gbAlbedo), GbMaterialLit(g_GBufferMode.y) * saturate(iColor1.w));") == 1 &&
              AdditiveError(terrain_text, r.hlsl).empty(),
          "a shader with a recipe writes its albedo from the capture before the anchor, and only adds lines: " + r.error);
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(terrain_text, "xCityTerrain__PSMultiLight");
    Check(a.found && a.recipe && a.albedo.found && a.albedo.captured, "the analysis with the shader's name finds the recipe");
    const sp::NightAnalysis b = sp::AnalyseGBufferNight(terrain_text);
    Check(b.found && !b.recipe && !b.albedo.found, "the analysis without a name looks for the diffuse texel only");
    // the HLSL is not the one the recipe was written for: the variant is not built, never a quiet "no data"
    const std::string other = Shader(std::string(kSunDot) + "\tr12.xyz = r1.xyz;\n");
    const sp::VariantTransformResult bad = sp::TransformGBufferNightHlsl(shader, other, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(!bad.ok && !bad.skip && bad.error.find("recipe") != std::string::npos, "an anchor that is not found makes the transform fail (not skip): " + bad.error);
    // the same text under a name with no recipe is the old behaviour: a normal and the "no data" albedo
    const sp::VariantShader plain{"xCityNothing__PSMultiLight", 0};
    const sp::VariantTransformResult old = sp::TransformGBufferNightHlsl(plain, other, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(old.ok && Count(old.hlsl, "float3 gbAlbedo") == 0 && Count(old.hlsl, "oC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);") == 1, "a shader with no recipe and no texel still writes the no data code");
    // constants: no capture
    const sp::VariantShader vehicle{"xAmbientVehicle__PSMultiLight", 0};
    const std::string vehicle_text = Shader(std::string(kSunDot) + "\tr5.xyz = r1.xyz;\n");
    const sp::VariantTransformResult v = sp::TransformGBufferNightHlsl(vehicle, vehicle_text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(v.ok && Count(v.hlsl, "float3 gbAlbedo") == 0 && Count(v.hlsl, "oC2 = float4(GbAlbedoEncode(float3(1.0f, 1.0f, 1.0f)), GbMaterialLit(g_GBufferMode.y));") == 1,
          "a recipe of constants writes the expression itself: " + v.error);
  }
  // GB-H2-REVIEW: the road's planar reflection mix (kGBufferNightMixRows): the lit weight keeps `1 - w` of itself where the game's colour is lerped to the reflection by w
  {
    bool sorted = true, family = true, with_occlusion = true, with_albedo = true, not_exempt = true;
    for (size_t i = 0; i < sp::kGBufferNightMixRowCount; ++i) {
      const sp::NightMixRow& row = sp::kGBufferNightMixRows[i];
      if (i > 0 && !(sp::kGBufferNightMixRows[i - 1].name < row.name)) sorted = false;
      family = family && sp::IsGBufferNightShader(row.name);
      with_occlusion = with_occlusion && sp::FindGBufferNightOcclusionRow(row.name) != nullptr;
      with_albedo = with_albedo && sp::FindGBufferNightAlbedoRow(row.name) != nullptr;  // the mix is a factor of the lit weight, which only a shader with an albedo has
      not_exempt = not_exempt && sp::FindGBufferNightHeadlightExempt(row.name) == nullptr;
    }
    Check(sp::kGBufferNightMixRowCount == 2 && sorted && family, "the mix table: the road and the tunnel road, sorted, in the night family");
    Check(with_occlusion && with_albedo && not_exempt, "a mix row is a lit road shader with an occlusion row and an albedo recipe");
    Check(sp::FindGBufferNightMixRow("xCityRoad__PSMultiLight") != nullptr && sp::FindGBufferNightMixRow("xCityRiverRoad__PSMultiLight") == nullptr,
          "the road has a row; the river road, which adds its reflection, has none");
    // the text: the road's recipe anchor and the mix anchor, then a statement that reads the weight
    const sp::NightMixRow* mix = sp::FindGBufferNightMixRow("xCityRoad__PSMultiLight");
    const sp::NightAlbedoRow* recipe = sp::FindGBufferNightAlbedoRow("xCityRoad__PSMultiLight");
    const std::string road_text =
        Shader(std::string(kSunDot) + "\tr7.xyz = r1.xyz;\n\t" + std::string(recipe->anchor) + "\n\tr3.w = r7.x;\n\t" + std::string(mix->anchor) + "\n\tr4.w = r5.w * r7.x;\n");
    const sp::VariantShader shader{"xCityRoad__PSMultiLight", 0};
    const sp::VariantTransformResult r = sp::TransformGBufferNightHlsl(shader, road_text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(r.ok && Count(r.hlsl, "\t" + std::string(mix->anchor) + "\n\tfloat gbMix = r5.w;\n\tr4.w = r5.w * r7.x;\n") == 1 &&
              Count(r.hlsl, "oC2 = float4(GbAlbedoEncode(gbAlbedo), GbMaterialLit(g_GBufferMode.y) * saturate(iColor1.w) * saturate(1.0f - gbMix));") == 1 && Count(r.hlsl, "float gbMix = ") == 1 &&
              Count(r.hlsl, "float3 gbAlbedo = ") == 1 && AdditiveError(road_text, r.hlsl).empty(),
          "the road's lit weight keeps 1 - w: the weight is captured right after its statement, and only lines are added: " + r.error);
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(road_text, "xCityRoad__PSMultiLight");
    const sp::NightAnalysis b = sp::AnalyseGBufferNight(road_text);
    Check(a.found && a.mix_row && a.mix.found && a.mix.capture == "\tfloat gbMix = r5.w;\n" && b.found && !b.mix_row && !b.mix.found, "the analysis finds the row by the shader's name only");
    // the HLSL is not the one the row was written for: the variant is not built, never a lit weight that keeps what the game does not
    const std::string no_mix = Shader(std::string(kSunDot) + "\tr7.xyz = r1.xyz;\n\t" + std::string(recipe->anchor) + "\n\tr3.w = r7.x;\n");
    const sp::VariantTransformResult bad = sp::TransformGBufferNightHlsl(shader, no_mix, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(!bad.ok && !bad.skip && bad.error.find("reflection mix") != std::string::npos, "a mix anchor that is not found makes the transform fail (not skip): " + bad.error);
    const sp::VariantTransformResult twice = sp::TransformGBufferNightHlsl(shader, road_text + "\t" + std::string(mix->anchor) + "\n", kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(!twice.ok && !twice.skip, "a mix anchor found twice makes it fail too");
    // a shader with no mix row keeps the lit weight it had
    const sp::VariantTransformResult plain = sp::TransformGBufferNightHlsl({"xCityTunnelRoad__PSMultiLightEnv", 0}, road_text, kHelper, sp::SoloContext(sp::kFeatureGBuffer));
    Check(plain.ok && Count(plain.hlsl, "gbMix") == 0, "a shader with no mix row has no gbMix: " + plain.error);
    // the albedo's capture (after its anchor) and the weight's (after its own) are both made, in the order of the text
    const size_t at_albedo = r.hlsl.find("float3 gbAlbedo = ");
    const size_t at_mix = r.hlsl.find("float gbMix = ");
    Check(at_albedo != std::string::npos && at_mix != std::string::npos && at_albedo < at_mix, "the captures are in the order of the statements they follow");
    // the write text itself
    sp::GBufferAlbedo albedo;
    albedo.found = true;
    albedo.expression = "gbAlbedo";
    Check(sp::GBufferAlbedoWriteText(albedo, "iColor1.w", "gbMix").find("GbMaterialLit(g_GBufferMode.y) * saturate(iColor1.w) * saturate(1.0f - gbMix));") != std::string::npos &&
              sp::GBufferAlbedoWriteText(albedo, {}, "gbMix").find("GbMaterialLit(g_GBufferMode.y) * saturate(1.0f - gbMix));") != std::string::npos &&
              sp::GBufferAlbedoWriteText(albedo, "iColor1.w") == sp::GBufferAlbedoWriteText(albedo, "iColor1.w", {}),
          "the lit weight's factors: the occlusion, then the mix; no mix is the text without it");
  }
}

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

// ---- one shader whose text does not fit costs that shader its variant and nothing else (GB-H2-FOLLOW, the review's S5; variant_shader_fate.h) -------------------------------------------------

void CheckPerShaderFailure() {
  const sp::VariantDef def = sp::MakeGBufferVariantFromText(kHelper);
  const sp::NightAlbedoRow* terrain = sp::FindGBufferNightAlbedoRow("xCityTerrain__PSMultiLight");
  const sp::NightAlbedoRow* grass = sp::FindGBufferNightAlbedoRow("xCityGrass__PSMultiLight");
  Check(terrain != nullptr && grass != nullptr, "the two recipes the case uses are in the table");
  if (terrain == nullptr || grass == nullptr) return;
  // five shaders of the family, as the builder meets them in key order: one that fits (a recipe, its anchor in the text), one whose recipe anchor is NOT in this game's text (the edits do not fit: a failure),
  // one with no recipe that fits, one the stage leaves out (no per pixel light: a skip), and one more that fits after the failed one
  struct Case {
    std::string name;
    uint64_t key;
    std::string text;
    sp::ShaderFate want;
  };
  const std::string fits = Shader(std::string(kSunDot) + "\tr12.xyz = r1.xyz;\n\t" + std::string(terrain->anchor) + "\n\tr5.w = r7.z * r12.x;\n");
  const std::string other = Shader(std::string(kSunDot) + "\tr12.xyz = r1.xyz;\n");
  const std::vector<Case> cases = {
      {"xCityTerrain__PSMultiLight", 0x1001, fits, sp::ShaderFate::kBuilt},
      {"xCityGrass__PSMultiLight", 0x1002, other, sp::ShaderFate::kFailed},
      {"xCityNothing__PSMultiLight", 0x1003, other, sp::ShaderFate::kBuilt},
      {"xCityUnlit__PSMultiLight", 0x1004, Shader("\tr3.xyz = r0.xyz * r1.www;\n"), sp::ShaderFate::kLeftOut},
      {"xCityNothingElse__PSMultiLight", 0x1005, other, sp::ShaderFate::kBuilt},
  };
  sp::ShaderFateTally tally;
  std::vector<std::string> built;
  std::vector<std::string> log_lines;
  for (const Case& c : cases) {
    const sp::VariantTransformResult r = def.transform({c.name, c.key}, c.text, sp::SoloContext(sp::kFeatureGBuffer));
    const sp::ShaderFate fate = tally.Note(c.name, "key" + std::to_string(c.key), r);
    Check(fate == c.want, c.name + ": the shader's fate is " + std::to_string(int(c.want)) + " (got " + std::to_string(int(fate)) + "; " + r.error + ")");
    if (fate == sp::ShaderFate::kBuilt) built.push_back(c.name);
    if (fate == sp::ShaderFate::kFailed) log_lines.push_back(sp::FailedShaderLine(def.name, c.name, "key" + std::to_string(c.key), r.error));
  }
  // the failed shader does not stop the others: the three that fit are built, the one left out is counted by its reason, the failed one is named
  Check(built.size() == 3 && built[0] == "xCityTerrain__PSMultiLight" && built[2] == "xCityNothingElse__PSMultiLight" && tally.built == 3, "the shaders after a failed one are still built");
  Check(tally.failed.size() == 1 && tally.failed[0].find("xCityGrass__PSMultiLight [key4098]: ") == 0 && tally.failed[0].find("recipe") != std::string::npos, "one shader failed, named with its key and the reason: " + (tally.failed.empty() ? std::string() : tally.failed[0]));
  Check(tally.left_out.size() == 1 && tally.left_out.begin()->first == sp::kGBufferNightSkipUnlit && tally.left_out.begin()->second == 1, "the shader the stage leaves out is counted by its reason, apart from the failure");
  Check(log_lines.size() == 1 && log_lines[0].find("the gbuffer variant was not built for xCityGrass__PSMultiLight [key4098]") != std::string::npos && log_lines[0].find("keeps the game's own shader") != std::string::npos,
        "one log line per failed shader, which says what happens to it: " + (log_lines.empty() ? std::string() : log_lines[0]));
  const std::string note = tally.NoteText();
  Check(note.find("1 left out: ") != std::string::npos && note.find("1 failed (no variant for them); xCityGrass__PSMultiLight [key4098]: ") != std::string::npos, "the summary line names both: " + note);
  Check(tally.any_failed() && tally.StampText().find("xCityGrass__PSMultiLight [key4098]") == 0 && tally.StampText().find('\n') == std::string::npos, "the stamp line is one line and names the failed shader");
  // two failures: one line each, in the order met, and the stamp joins them
  sp::ShaderFateTally two;
  two.Note("a", "k1", def.transform({"xCityGrass__PSMultiLight", 1}, other, sp::SoloContext(sp::kFeatureGBuffer)));
  two.Note("b", "k2", def.transform({"xCityGrass__PSMultiLight", 2}, other, sp::SoloContext(sp::kFeatureGBuffer)));
  Check(two.failed.size() == 2 && two.failed[0].find("a [k1]") == 0 && two.failed[1].find("b [k2]") == 0 && two.StampText().find(" | b [k2]") != std::string::npos, "two failed shaders: two entries, in order, joined in the stamp");
  // a stage with nothing but failures is not a tally of built shaders: the builder fails the stage (it checks made.empty()) and quotes the first failure
  Check(two.built == 0 && two.any_failed(), "a stage whose shaders all failed has built none (the builder then fails the stage, with the first reason)");
  // the fate is the transform's own flags: ok, skip, neither
  sp::VariantTransformResult ok;
  ok.ok = true;
  sp::VariantTransformResult skip;
  skip.skip = true;
  sp::VariantTransformResult bad;
  Check(sp::FateOf(ok) == sp::ShaderFate::kBuilt && sp::FateOf(skip) == sp::ShaderFate::kLeftOut && sp::FateOf(bad) == sp::ShaderFate::kFailed, "the fate follows the transform's ok / skip flags");
  // the builder uses it for every shader of a stage (variant_pack.cpp): not an `*error = ...; return false;` of the whole stage
  const std::string builder = ReadFile(std::string(__FILE__).substr(0, std::string(__FILE__).rfind("variants")) + "variant_pack.cpp");
  Check(!builder.empty() && builder.find("const ShaderFate fate = fates.Note(source->name, Hex64(source->key), variant);") != std::string::npos &&
            builder.find("if (fate == ShaderFate::kFailed) {") != std::string::npos && builder.find("*error = source->name + \": \" + variant.error;") == std::string::npos,
        "the builder asks the tally for each shader's fate, and a failed shader no longer fails the stage");
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
  Check(chain::CanCompose(both) && chain::HasTwin(both) && chain::OffsetOf(both, sp::kFeatureGBuffer) == 1680, "the table composes the headlight and the G-buffer, the block at 1680");

  std::printf("\n=== CENSUS of the night family on %s ===\n", folder.string().c_str());
  std::map<std::string, std::pair<int, int>> by_kind;  // kind -> (edited, left out)
  std::map<std::string, int> by_how, by_reason;
  int edited = 0, left_out = 0, interpolants = 0, captured = 0, composed = 0, with_diffuse = 0, albedo_captured = 0, with_recipe = 0, recipe_captured = 0, with_occlusion = 0, cross_checked = 0, identical = 0;
  std::map<std::string, int> no_albedo_why;
  std::vector<std::string> no_albedo_names;
  std::map<std::string, std::string> albedo_lanes;
  std::vector<std::string> left_names;
  std::map<int, int> by_class;  // RT3's class (GB-F1's table) of the edited shaders: the census of what the material plane says
  int unclassed = 0;
  for (const Shader_* file : family) {
    const std::string text = ReadFile(file->path);
    const std::string kind = file->stem.ends_with("Interior") ? "PSMultiLightInterior" : file->stem.ends_with("Env") ? "PSMultiLightEnv" : "PSMultiLight";
    const sp::VariantShader shader{file->stem, 0};
    const sp::NightAnalysis a = sp::AnalyseGBufferNight(text, file->stem);
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
    {
      // RT3 (GB-W2) writes the draw's word, whose class comes from the table of GB-F1 by the shader's name: a shader of the stage that the table calls "none" (or does not know) would write class 0,
      // which a consumer reads as "nothing written here" over a pixel that has a normal. Every shader of the stage has a class.
      const mcla::gpu::gbuffer::ShaderMaterial material = mcla::gpu::gbuffer::MaterialOfShaderName(file->stem);
      ++by_class[int(material.cls)];
      unclassed += !(material.known && material.cls != mcla::gpu::gbuffer::SurfaceClass::kNone);
      Check(material.known && material.cls != mcla::gpu::gbuffer::SurfaceClass::kNone, file->stem + ": the material table gives it a class (so RT3 never says 'no data' where the stage wrote the normal)");
    }
    ++by_kind[kind].first;
    ++by_how[HowText(a.how)];
    (a.interpolant ? interpolants : captured)++;
    with_occlusion += !a.occlusion.empty() && a.albedo.found;
    if (a.albedo.found && a.recipe) {
      ++with_recipe;  // GB-H2-COVER: the shader has no texel of the shape; its recipe gives the colour
      recipe_captured += a.albedo.captured;
    } else if (a.albedo.found) {
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
      const std::string albedo_call = "oC2 = float4(GbAlbedoEncode(" + a.albedo.expression + "), GbMaterialLit(g_GBufferMode.y)" + (a.occlusion.empty() ? std::string() : " * saturate(" + a.occlusion + ")") +
                                      (a.mix_row && a.albedo.found ? std::string(" * saturate(1.0f - gbMix)") : std::string()) + ");";
      Check(Count(alone.hlsl, "oC2 : SV_Target2") == 1 && Count(alone.hlsl, "oC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);") == 1 && Count(alone.hlsl, albedo_call) == (a.albedo.found ? 1u : 0u) &&
                Count(alone.hlsl, "float3 gbAlbedo = ") == (a.albedo.found && a.albedo.captured ? 1u : 0u),
            file->stem + ": RT2: one output, the no data write, " + (a.albedo.found ? (a.albedo.captured ? "the albedo from one capture" : "the albedo from the fetch's lanes") : "no albedo"));
      // RT3 in every shader of the stage, with an albedo or without, and after RT2's write
      Check(Count(alone.hlsl, "oC3 : SV_Target3") == 1 && Count(alone.hlsl, "oC3 = float2(0.0f, 0.0f);") == 1 && Count(alone.hlsl, "oC3 = GbMaterialRt3(g_GBufferMode.y);") == 1 &&
                alone.hlsl.find("oC3 = GbMaterialRt3(") > alone.hlsl.find("oC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);"),
            file->stem + ": RT3: one output, the no data write and the word's bytes, after RT2's");
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
        const sp::NightAnalysis on_hl = sp::AnalyseGBufferNight(hl_alone.hlsl, file->stem);
        Check(on_hl.found && on_hl.expression == a.expression && on_hl.interpolant == a.interpolant && on_hl.how == a.how, file->stem + ": the headlight's edits leave the normal where it was");
        Check(Count(pair.hlsl, "float4 g_HlHeader : packoffset(c38);") == 1 && Count(pair.hlsl, "uint4 g_GBufferMode : packoffset(c105);") == 1 && Count(pair.hlsl, "SharedConstants + 1680, 0x10)") == 1 &&
                  Count(pair.hlsl, "hlLight.") == Count(hl_alone.hlsl, "hlLight.") && Count(pair.hlsl, "oC1 = GbRt1Encode(" + a.expression + ");") == 1,
              file->stem + ": composed: the blocks are at 608 and 1680, the headlight's term and the G-buffer's write are in");
        // RT2 composed: the analysis runs on the text the headlight left, so the same anchor must be found there (the headlight does not touch the fetch or write its register)
        const sp::GBufferAlbedo on_hl_albedo = on_hl.albedo;  // the texel's anchor or the recipe's statement, on the text the headlight left
        Check(on_hl_albedo.found == a.albedo.found && on_hl_albedo.lanes == a.albedo.lanes && on_hl_albedo.captured == a.albedo.captured,
              file->stem + ": composed: the headlight's edits leave the albedo's anchor where it was");
        Check(Count(pair.hlsl, "oC2 : SV_Target2") == 1 && Count(pair.hlsl, "GbAlbedoEncode(" + a.albedo.expression + ")") == (a.albedo.found ? 1u : 0u),
              file->stem + ": composed: RT2's output and write are in");
        Check(Count(pair.hlsl, "oC3 : SV_Target3") == 1 && Count(pair.hlsl, "oC3 = GbMaterialRt3(g_GBufferMode.y);") == 1, file->stem + ": composed: RT3's output and write are in");
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
  std::printf("  RT2: %d of the %d edited shaders have a DiffuseSampler texel for the albedo (%d read at the end of main, %d captured after the fetch); %d have a recipe (%d captured, %d constants); %d have none\n",
              with_diffuse, edited, with_diffuse - albedo_captured, albedo_captured, with_recipe, recipe_captured, with_recipe - recipe_captured, edited - with_diffuse - with_recipe);
  std::printf("  RT2.a: %d of the edited shaders multiply the lit weight by the baked occlusion the game multiplies its lights by (kGBufferNightOcclusionRows: %zu rows)\n", with_occlusion, sp::kGBufferNightOcclusionRowCount);
  for (const auto& [why, n] : no_albedo_why) std::printf("  no albedo: %3d  %s\n", n, why.c_str());
  for (const std::string& name : no_albedo_names) std::printf("      no albedo: %s\n", name.c_str());
  for (const auto& [why, n] : by_reason) std::printf("  left out: %3d  %s\n", n, why.c_str());
  std::printf("  %d texts are the game's with lines added; %d composed with the headlight; %d normals cross-checked with the other anchors\n", identical, composed, cross_checked);
  {
    std::printf("  RT3 class of the edited shaders (the table of GB-F1 by name; %d with no class):", unclassed);
    for (const auto& [cls, n] : by_class) std::printf(" %d:%d", cls, n);
    std::printf("\n");
  }
  // THE COVER RULE (GB-H2-COVER). Every shader the forward headlight variant edits is a writer of the G-buffer -- the night stage's normal AND an albedo -- except the shaders of
  // kGBufferNightHeadlightExempt, which are held to their reasons. The deferred light pass lights only what the planes hold (RT2 = 0 is skipped), so a shader that drops out while the forward stage
  // still lights it leaves the headlights of the pass darker than the forward ones there (the road, the terrain, the grass, the tints and the traffic body did, until the recipes): it fails here.
  {
    int headlight_edited = 0, covered_texel = 0, covered_recipe = 0, exempt_seen = 0;
    std::vector<std::string> uncovered;
    std::vector<std::string> dark;  // covered by an albedo, but the class's lit weight is 0: the light pass skips the pixel (RT2.a = 0), so the pass lights none of it
    std::set<std::string> exempt_edited;
    for (const Shader_* file : family) {
      const std::string text = ReadFile(file->path);
      const sp::VariantShader shader{file->stem, 0};
      const sp::VariantTransformResult hl = sp::ComposeStages(defs, chain::FeatureBit(sp::kFeatureHeadlight), shader, text);
      if (!hl.ok) continue;  // the forward stage does not edit it: nothing is lost where the pass has no light to match
      ++headlight_edited;
      const sp::NightAnalysis a = sp::AnalyseGBufferNight(text, file->stem);
      if (const sp::NightHeadlightExempt* exempt = sp::FindGBufferNightHeadlightExempt(file->stem)) {
        ++exempt_seen;
        exempt_edited.insert(file->stem);
        std::printf("EXEMPT    %-46s %s\n", file->stem.c_str(), std::string(exempt->reason).c_str());
        if (exempt->reason == sp::kExemptEmissive) {
          // lit weight 0 by the class: the light pass adds nothing to an emissive surface whatever its albedo is
          const mcla::gpu::gbuffer::ShaderMaterial material = mcla::gpu::gbuffer::MaterialOfShaderName(file->stem);
          Check(material.known && material.cls == mcla::gpu::gbuffer::SurfaceClass::kEmissive && material.lit == 0.0f, file->stem + ": the emissive exemption holds: the class table says emissive, lit weight 0");
        } else {
          Check(!a.found && a.why_not == sp::kGBufferNightSkipTangent, file->stem + ": the tangent space exemption holds: the night stage leaves it out for that reason (" + a.why_not + ")");
        }
      } else if (a.found && a.albedo.found) {
        (a.recipe ? covered_recipe : covered_texel)++;
        // GB-H2-REVIEW: an albedo is not enough: the lit weight of the shader's class is RT2.a, and the pass skips a pixel whose weight is 0 (every emissive class)
        const mcla::gpu::gbuffer::ShaderMaterial material = mcla::gpu::gbuffer::MaterialOfShaderName(file->stem);
        if (!(material.lit > 0.0f)) dark.push_back(file->stem + " (class " + std::to_string(int(material.cls)) + ")");
      } else {
        uncovered.push_back(file->stem + (a.found ? " (no albedo: " + a.albedo.why_not + ")" : " (no normal: " + a.why_not + ")"));
      }
    }
    std::printf("\nHEADLIGHT COVER: the forward headlight stage edits %d shaders: %d have the G-buffer's normal and a diffuse texel, %d a recipe, %d are exempt, %zu are not covered\n", headlight_edited, covered_texel,
                covered_recipe, exempt_seen, uncovered.size());
    for (const std::string& name : uncovered) std::printf("      NOT COVERED: %s\n", name.c_str());
    for (const std::string& name : dark) std::printf("      LIT WEIGHT 0: %s\n", name.c_str());
    Check(dark.empty(), "every shader the forward headlight stage edits that is not exempt has a lit weight above 0 (an emissive class is skipped by the light pass, whatever its albedo): " + std::to_string(dark.size()) + " have none");
    Check(uncovered.empty(), "every shader the forward headlight stage edits writes the G-buffer's normal and albedo (or is exempt for its reason): " + std::to_string(uncovered.size()) + " are not");
    bool all_seen = true;
    for (const sp::NightHeadlightExempt& row : sp::kGBufferNightHeadlightExempt) all_seen = all_seen && exempt_edited.contains(std::string(row.name));
    Check(all_seen, "every exempt shader is one the forward headlight stage edits (a stale exemption is a hole nobody looks at)");
    if (family.size() == 115) {
      Check(headlight_edited == 58 && covered_texel == 39 && covered_recipe == 11 && exempt_seen == 8,
            "the retail cover: 58 edited by the headlight = 39 with a texel + 11 recipes + 8 exempt (got " + std::to_string(headlight_edited) + " = " + std::to_string(covered_texel) + " + " +
                std::to_string(covered_recipe) + " + " + std::to_string(exempt_seen) + ")");
    }
  }
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
    Check(with_diffuse == 71 && albedo_captured == 51, "the retail RT2 census: 71 shaders have the albedo from a texel (51 captured) (got " + std::to_string(with_diffuse) + " and " + std::to_string(albedo_captured) + ")");
    // GB-H2-COVER: the occlusion rows: every row is a shader of the retail family, edited, with an albedo (a row on a shader that writes no albedo would be dead), and the text has its write once
    {
      Check(with_occlusion == int(sp::kGBufferNightOcclusionRowCount), "the retail census: all " + std::to_string(sp::kGBufferNightOcclusionRowCount) + " occlusion rows are edited shaders with an albedo (got " + std::to_string(with_occlusion) + ")");
      for (const sp::NightOcclusionRow& row : sp::kGBufferNightOcclusionRows) {
        const Shader_* hit = nullptr;
        for (const Shader_* file : family) {
          if (file->stem == row.name) hit = file;
        }
        Check(hit != nullptr, std::string(row.name) + ": the occlusion row's shader is in the retail game");
        if (hit == nullptr) continue;
        const std::string text = ReadFile(hit->path);
        Check(Count(text, std::string(sp::kNightOcclusionDeclaration)) == 1, std::string(row.name) + ": the shader declares iColor1 once");
      }
    }
    // GB-H2-COVER: the recipes (gbuffer_night_albedo.h) are exactly the shaders of the table, each found with its anchor; the ones with a statement capture, the ones of constants do not (counted from the table itself)
    {
      int want_captured = 0;
      for (const sp::NightAlbedoRow& row : sp::kGBufferNightAlbedoRows) want_captured += row.site != sp::NightAlbedoSite::kGlobal;
      Check(with_recipe == int(sp::kGBufferNightAlbedoRowCount) && recipe_captured == want_captured,
            "the retail RT2 census: the 11 recipes all fit the text (got " + std::to_string(with_recipe) + ", " + std::to_string(recipe_captured) + " captured)");
      for (const sp::NightAlbedoRow& row : sp::kGBufferNightAlbedoRows) {
        const Shader_* hit = nullptr;
        for (const Shader_* file : family) {
          if (file->stem == row.name) hit = file;
        }
        Check(hit != nullptr, std::string(row.name) + ": the recipe's shader is in the retail game");
        if (hit == nullptr) continue;
        const std::string text = ReadFile(hit->path);
        const sp::NightAnalysis a = sp::AnalyseGBufferNight(text, row.name);
        Check(a.found && a.recipe && a.albedo.found && a.albedo.lanes == row.expression, std::string(row.name) + ": the recipe fits and gives its expression (" + a.albedo.why_not + ")");
        Check(!sp::AnalyseGBufferAlbedo(text).found, std::string(row.name) + ": the shader has no DiffuseSampler texel of the shape: the recipe shadows nothing");
        if (row.site != sp::NightAlbedoSite::kGlobal) {
          // the capture lands in main at the top level, and the statement it is read from is not written between a `before` capture and the anchor (the anchor line itself is the next statement)
          const std::string lf = LfOnly(text);
          Check(a.albedo.capture_at != std::string::npos && a.albedo.capture_at > lf.rfind("\nvoid main(") && Count(lf, std::string("\t") + std::string(row.anchor) + "\n") == 1,
                std::string(row.name) + ": the anchor is one line at the top level of main");
        }
      }
    }
    // GB-H2-REVIEW: the mix rows (gbuffer_night_albedo.h kGBufferNightMixRows) fit the retail text: the anchor once at the top level of main, and no later statement writes the weight's register
    {
      int fitted = 0;
      for (const sp::NightMixRow& row : sp::kGBufferNightMixRows) {
        const Shader_* hit = nullptr;
        for (const Shader_* file : family) {
          if (file->stem == row.name) hit = file;
        }
        Check(hit != nullptr, std::string(row.name) + ": the mix row's shader is in the retail game");
        if (hit == nullptr) continue;
        const std::string text = ReadFile(hit->path);
        const sp::NightAnalysis a = sp::AnalyseGBufferNight(text, row.name);
        Check(a.found && a.mix_row && a.mix.found && a.albedo.found, std::string(row.name) + ": the mix row fits (" + a.mix.why_not + ")");
        fitted += a.mix.found ? 1 : 0;
        const std::string lf = LfOnly(text);
        const size_t main_at = lf.rfind("\nvoid main(");
        const std::string anchor_line = "\t" + std::string(row.anchor) + "\n";
        const size_t anchor_at = lf.find(anchor_line, main_at);
        Check(main_at != std::string::npos && anchor_at != std::string::npos && Count(lf.substr(main_at), anchor_line) == 1, std::string(row.name) + ": the mix anchor is once in main");
        if (anchor_at == std::string::npos) continue;
        // the register the weight is in (`r5`): a later `r5 = ...` or `r5.<lanes with the weight's> = ...` would change it before the end of main. The capture is right after the anchor, so this
        // is only the proof that the lit weight reads the very value the game's lerp reads.
        const std::string reg(row.weight.substr(0, row.weight.find('.')));
        const char lane = row.weight.back();
        int later_writes = 0;
        size_t at = anchor_at + anchor_line.size();
        while (at < lf.size()) {
          size_t end = lf.find('\n', at);
          if (end == std::string::npos) end = lf.size();
          const std::string_view line = std::string_view(lf).substr(at, end - at);
          const size_t eq = line.find(" = ");
          const size_t first = line.find_first_not_of("\t ");
          if (eq != std::string_view::npos && first != std::string_view::npos && first < eq) {
            const std::string_view dest = line.substr(first, eq - first);
            if (dest == reg || (dest.starts_with(reg + ".") && dest.find(lane) != std::string_view::npos)) ++later_writes;
          }
          at = end + 1;
        }
        Check(later_writes == 0, std::string(row.name) + ": nothing writes " + std::string(row.weight) + " after the anchor (" + std::to_string(later_writes) + " writes)");
      }
      Check(fitted == int(sp::kGBufferNightMixRowCount), "the retail census: both mix rows fit the text");
    }
    const std::set<std::string> want_no_albedo = {
        "xCityGrimeMetalTint__PSMultiLightInterior",
        "xCityGrimeTintDecal__PSMultiLightInterior",
        "xCityGrimeTint__PSMultiLightInterior",
        "xCityLampGlassTint__PSMultiLight",
        "xCityLampGlassTint__PSMultiLightInterior",
        "xCityNeonTint__PSMultiLight",
        "xCityNeonTint__PSMultiLightInterior",
        "xCityRoad__PSMultiLightEnv",
        "xCityRoad__PSMultiLightInterior",
        "xCityTunnelRoad__PSMultiLightEnv",
        "xCityTunnelRoad__PSMultiLightInterior",
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
  CheckAlbedoRecipes();
  CheckIdentityCheck();
  CheckVariantDef();
  CheckPerShaderFailure();
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

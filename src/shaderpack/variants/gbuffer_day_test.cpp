// Check of the G-buffer's day families (gbuffer_day.h): the pixel stage's normal sources and the vertex stage. A program of its own: it needs no renderer, no ReXGlue, no compiler and
// no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_day_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_gbuffer_day_test [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the results>]]]
//
// Without arguments it runs on small texts made here (the shape of the translator's output the edits look at) and a stand-in for the helper. With the helper file as the first argument
// the real one is pasted. With a folder of translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files, for example shaderpack/baseline/hlsl-rpf/city of the development PC)
// it runs on every shader of the table and on the three vertex shaders: the retail text must transform, and with the variant's insertions taken out must be the game's text again, byte for
// byte (the colour path is untouched), and every day pixel shader of that folder must be in the table or in the left-out list. A third argument is a folder that receives the edited
// texts (`<name>.gbday.hlsl`, `<name>.gbvs.hlsl`), for a compile with dxc (-T ps_6_0 / vs_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout, and without -spirv for DXIL; with
// -DXENOS_SPEC_CONSTANTS=0 and =2 for the pixel shaders' alpha test twin, =0 and =1 for the vertex shaders' packed normal one).

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variants/bloom_gauss.h"
#include "shaderpack/variants/car_chrome.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/env_map.h"
#include "shaderpack/variants/gbuffer_day.h"
#include "shaderpack/variants/glow_soft.h"
#include "shaderpack/variants/headlight.h"
#include "shaderpack/variants/luminance_scale.h"
#include "shaderpack/variants/shadow_filter.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/ui_sharp.h"

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
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

std::string ReadFile(const std::filesystem::path& path) {
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

// "" when the two texts are the same, else where they first differ for the message of a failed check.
std::string FirstDifference(const std::string& got, const std::string& want) {
  if (got == want) return std::string();
  size_t at = 0;
  while (at < got.size() && at < want.size() && got[at] == want[at]) ++at;
  const auto around = [&](const std::string& text) { return text.substr(at > 20 ? at - 20 : 0, 60); };
  return " (first difference at byte " + std::to_string(at) + ": got '" + around(got) + "', want '" + around(want) + "'; sizes " + std::to_string(got.size()) + " and " +
         std::to_string(want.size()) + ")";
}

// Takes the pixel stage's insertions out again (the second output, the three constants rows, the helper, the derivative block, the write of the sidecar) and gives back what is left: it must be
// the game's text, byte for byte, with the line ends of the transform (LF). The colour is then computed by the game's own statements alone.
std::string WithoutDayStage(std::string text, std::string helper, const sp::GBufferDayShader& row) {
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
  const std::string no_cr = LfOnly(helper);
  cut("the second output", "\n\t, out float2 oC1 : SV_Target1");
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
  if (row.source == sp::GBufferDaySource::kVertexOrDerivative || row.source == sp::GBufferDaySource::kDerivative) cut("the derivative block", sp::GBufferDayDerivativeText());
  cut("the sidecar write", sp::GBufferDayWriteText(row));
  return missing.empty() ? text : "<missing or twice there:" + missing + ">";
}

std::string WithoutVsStage(std::string text, const sp::GBufferVsShader& row) {
  const std::string piece = sp::GBufferVsExportText(row.kind);
  const size_t at = text.find(piece);
  if (at == std::string::npos || text.find(piece, at + piece.size()) != std::string::npos) return "<the export is missing or twice there>";
  text.erase(at, piece.size());
  return text;
}

// ---- the fixtures: the shape of the translator's pixel and vertex shaders the edits look at ----------------------------------------------------------------

constexpr const char* kPixelFixture =
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
    "\tin centroid float4 iTexCoord2 : TEXCOORD2,\n"
    "\tin centroid float4 iTexCoord15 : TEXCOORD15,\n"
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
    "\tCubeMapData cubeMapData = (CubeMapData)0;\n"
    "\toC0.xyz = r0.xyz;\n"
    "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
    "\t}\t\toC0 = applyBlendPremult(oC0);\n"
    "\treturn;\n"
    "}\n";

constexpr const char* kHelper =
    "float2 GbOctEncode(float3 n) { return n.xy; }\n"
    "float2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\n"
    "float3 GbWorldPoint(float4 ray, float4 q, float px, float py, float w) { return ray.xyz; }\n"
    "float3 GbFaceNormal(float3 dpdx, float3 dpdy, float3 to_camera) { return dpdx; }\n"
    "#define GB_MODE_WRITE_NORMAL 1\n"
    "#define GB_MODE_FLIP_BACKFACE 8\n";

constexpr const char* kVertexFixture =
    "#ifndef __spirv__\n"
    "[shader(\"vertex\")]\n"
    "#endif\n"
    "void main(\n"
    "\t[[vk::location(0)]] in float4 iPosition0 : POSITION0,\n"
    "\t[[vk::location(1)]] in uint4 iNormal0 : NORMAL0,\n"
    "\t[[vk::ext_decorate(18)]] out precise float4 oPos : SV_Position,\n"
    "\tout float4 oTexCoord0 : TEXCOORD0,\n"
    "\tout float4 oTexCoord15 : TEXCOORD15)\n"
    "{\n"
    "\toTexCoord0 = 0.0;\n"
    "\toTexCoord15 = 0.0;\n"
    "\tfloat4 r4 = 0.0;\n"
    "\tr4.xyz = tfetchR11G11B10(iNormal0).xyz;\n"
    "\toTexCoord0.xyz = max(r4.xyz, r4.xyz);\n"
    "\tr4.xyz = r4.xyz * gWorld(0).xyz;\n"
    "\toPos.xyz = oPos.xyz * g_PosScale.xyz + g_PosOffset.xyz * oPos.w;\n"
    "\treturn;\n"
    "}\n";

constexpr const char* kFixtureNames[] = {"xCityDefault__PSCityDefault",        "xCityGrime__PSCityGrime",           "xCitySidewalk__PSCitySidewalk", "xCityRoad__PSCityRoad",
                                         "xPropTrafficSignal__PSPropTrafficSignal", "xCityBillboard__PSCityBillboard"};

void CheckPixelFixture() {
  for (const char* name : kFixtureNames) {
    const sp::GBufferDayShader* row = sp::FindGBufferDayShader(name);
    Check(row != nullptr, std::string(name) + " is in the table");
    if (!row) continue;
    const sp::VariantShader shader{name, 0};
    const sp::VariantTransformResult r = sp::TransformGBufferDayHlsl(shader, kPixelFixture, kHelper);
    Check(r.ok && !r.skip && r.error.empty(), std::string(name) + ": the fixture transforms: " + r.error);
    if (!r.ok) continue;
    Check(Count(r.hlsl, "out float4 oC0 : SV_Target0\n\t, out float2 oC1 : SV_Target1") == 1, std::string(name) + ": the second output follows the colour output");
    Check(Count(r.hlsl, "#define g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 608, 0x10)") == 1 &&
              Count(r.hlsl, "float4 g_GBufferCam : packoffset(c40);") == 1,
          std::string(name) + ": the 48 byte block is at 608 (c38 to c40)");
    const size_t helper_at = r.hlsl.find("float2 GbOctEncode(");
    const size_t main_at = r.hlsl.find("void main(");
    Check(helper_at != std::string::npos && main_at != std::string::npos && helper_at < main_at, std::string(name) + ": the helper is before main");
    const size_t write_at = r.hlsl.find("oC0 = applyBlendPremult(oC0);\n\toC1 = float2(0.0f, 0.0f);");
    Check(write_at != std::string::npos, std::string(name) + ": the write follows the game's last colour statement");
    const bool derivative = row->source == sp::GBufferDaySource::kVertexOrDerivative || row->source == sp::GBufferDaySource::kDerivative;
    const size_t deriv_at = r.hlsl.find("float3 gbP = GbWorldPoint(");
    const size_t alpha_at = r.hlsl.find("[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)");
    const size_t first_game_statement = r.hlsl.find("\toC0.xyz = r0.xyz;");
    Check(derivative ? deriv_at != std::string::npos && deriv_at < first_game_statement && first_game_statement < alpha_at && Count(r.hlsl, "float3 gbDx = ddx(gbP);") == 1 &&
                           Count(r.hlsl, "float3 gbDy = ddy(gbP);") == 1
                     : deriv_at == std::string::npos,
          std::string(name) + ": the derivatives are at the top of main, before any statement of the game's and its alpha test, and only for the derivative sources");
    switch (row->source) {
      case sp::GBufferDaySource::kInterpolant: {
        const std::string expect = "float3 gbN = iTexCoord" + std::to_string(row->interpolant) + ".xyz;";
        Check(Count(r.hlsl, expect) == 1 && Count(r.hlsl, "oC1 = GbRt1Encode(gbN);") == 1 && Count(r.hlsl, "GbFaceNormal(gbDx") == 0, std::string(name) + ": the normal is " + expect);
        break;
      }
      case sp::GBufferDaySource::kVertexOrDerivative:
        Check(Count(r.hlsl, "float3 gbN = iTexCoord15.xyz;") == 1 && Count(r.hlsl, "gbN = GbFaceNormal(gbDx, gbDy, -gbP);") == 1 && Count(r.hlsl, "oC1 = GbRt1Encode(gbN);") == 1,
              std::string(name) + ": TEXCOORD15, else the derivative normal");
        break;
      case sp::GBufferDaySource::kDerivative:
        Check(Count(r.hlsl, "oC1 = GbRt1Encode(GbFaceNormal(gbDx, gbDy, -gbP));") == 1 && Count(r.hlsl, "iTexCoord") == Count(kPixelFixture, "iTexCoord"),
              std::string(name) + ": the derivative normal alone");
        break;
      case sp::GBufferDaySource::kMasked:
        Check(Count(r.hlsl, "GbRt1Encode(") == 1 /* the helper's own definition */ && Count(r.hlsl, "oC1 = ") == 1, std::string(name) + ": nothing but the no-data code is written");
        break;
    }
    const std::string diff = FirstDifference(WithoutDayStage(r.hlsl, kHelper, *row), LfOnly(kPixelFixture));
    Check(diff.empty(), std::string(name) + ": with the stage's insertions taken out the fixture is the game's text again, byte for byte" + diff);
    std::string crlf;
    for (const char c : std::string(kPixelFixture)) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    const sp::VariantTransformResult again = sp::TransformGBufferDayHlsl(shader, crlf, std::string(kHelper) + "\r\n");
    Check(again.ok && again.hlsl == r.hlsl, std::string(name) + ": a text with CRLF line ends gives the same variant");
    // In a composition the block moves where the chain puts it (after the headlight's 1072 bytes: 1680).
    const sp::StageContext after = sp::ContextIn(sp::kFeatureGBuffer, chain::FeatureBit(sp::kFeatureHeadlight) | chain::FeatureBit(sp::kFeatureGBuffer));
    const sp::VariantTransformResult moved = sp::TransformGBufferDayHlsl(shader, kPixelFixture, kHelper, after);
    Check(moved.ok && Count(moved.hlsl, "uint4 g_GBufferMode : packoffset(c105);") == 1 && Count(moved.hlsl, "SharedConstants + 608") == 0, std::string(name) + ": composed, the block is at 1680");
  }
  // Not one of the shaders: left out, no error.
  {
    const sp::VariantTransformResult r = sp::TransformGBufferDayHlsl({"xCityDefault__PSMultiLight", 0}, kPixelFixture, kHelper);
    Check(!r.ok && r.skip, "the night shader of the group is not the day stage's");
    const sp::VariantTransformResult env = sp::TransformGBufferDayHlsl({"xCityRoad__PSCityRoadEnv", 0}, kPixelFixture, kHelper);
    Check(!env.ok && env.skip, "an Env shader is left out");
  }
  // The anchors: each must be found once.
  const auto broken = [&](const std::string& name, const std::string& from, const std::string& to, const std::string& message_has, const std::string& what) {
    std::string text = kPixelFixture;
    const size_t at = text.find(from);
    if (at == std::string::npos) {
      Check(false, "the fixture has " + from);
      return;
    }
    text.replace(at, from.size(), to);
    const sp::VariantTransformResult r = sp::TransformGBufferDayHlsl({name, 0}, text, kHelper);
    Check(!r.ok && !r.skip && r.error.find(message_has) != std::string::npos, what + (r.error.empty() ? "" : " (" + r.error.substr(0, 90) + ")"));
  };
  broken("xCityDefault__PSCityDefault", "\tin centroid float4 iTexCoord0 : TEXCOORD0,\n", "", "iTexCoord0", "no TEXCOORD0 interpolant: an error that names it");
  broken("xCityRoad__PSCityRoad", "\tin centroid float4 iTexCoord15 : TEXCOORD15,\n", "", "iTexCoord15", "no TEXCOORD15 for a vertex-or-derivative shader: an error that names it");
  broken("xCityDefault__PSCityDefault", "\tin uint iFace : SV_IsFrontFace\n", "\tin uint iFace2 : SV_IsFrontFace\n", "iFace", "no face flag in DXIL: an error that names it");
  broken("xPropTrafficSignal__PSPropTrafficSignal", "\tin float4 iPos : SV_Position,\n", "", "iPos", "a derivative shader without iPos: an error that names it");
  broken("xPropTrafficSignal__PSPropTrafficSignal", "\tCubeMapData cubeMapData = (CubeMapData)0;\n", "", "CubeMapData", "a derivative shader without the last declaration of main: an error");
  broken("xCityDefault__PSCityDefault", "oC0 = applyBlendPremult(oC0);", "oC0 = oC0;", "colour write", "no premultiplied write: an error that names it");
  broken("xCityDefault__PSCityDefault", "\tout float4 oC0 : SV_Target0\n", "\tout float4 oC0 : SV_Target0\n\tout float4 oC0 : SV_Target0\n", "oC0 : SV_Target0", "two colour outputs: an error that names the anchor");
  {
    const sp::VariantTransformResult r = sp::TransformGBufferDayHlsl({"xCityDefault__PSCityDefault", 0}, kPixelFixture, "");
    Check(!r.ok && !r.skip, "an empty helper is an error");
  }
}

void CheckVertexFixture() {
  for (const sp::GBufferVsShader& row : sp::kGBufferVsShaders) {
    const sp::VariantShader shader{row.name, 0};
    const sp::VariantTransformResult r = sp::TransformGBufferVsHlsl(shader, kVertexFixture);
    Check(r.ok && !r.skip && r.error.empty(), std::string(row.name) + ": the vertex fixture transforms: " + r.error);
    if (!r.ok) continue;
    Check(Count(r.hlsl, "oTexCoord15 = float4(") == 1 && Count(r.hlsl, "oTexCoord15 = 0.0;") == 1, std::string(row.name) + ": one export, after the game's zero");
    const size_t last_at = r.hlsl.find("oPos.xyz = oPos.xyz * g_PosScale.xyz");
    const size_t export_at = r.hlsl.find("oTexCoord15 = float4(");
    const size_t return_at = r.hlsl.find("\treturn;");
    Check(last_at < export_at && export_at < return_at, std::string(row.name) + ": the export is after the game's last statement and before the return");
    Check(row.kind == sp::GBufferVsKind::kProp ? Count(r.hlsl, "gbF.x * gWorld(0).xyz + gbF.y * gWorld(1).xyz + gbF.z * gWorld(2).xyz") == 1 : Count(r.hlsl, "tfetchR11G11B10(iNormal0).xyz, 0.0f)") == 1,
          std::string(row.name) + ": the normal is the game's fetch" + (row.kind == sp::GBufferVsKind::kProp ? ", rotated by gWorld" : ""));
    const std::string diff = FirstDifference(WithoutVsStage(r.hlsl, row), LfOnly(kVertexFixture));
    Check(diff.empty(), std::string(row.name) + ": with the export taken out the fixture is the game's text again, byte for byte" + diff);
  }
  const auto broken = [&](const std::string& name, const std::string& from, const std::string& to, const std::string& message_has, const std::string& what) {
    std::string text = kVertexFixture;
    const size_t at = text.find(from);
    if (at == std::string::npos) {
      Check(false, "the vertex fixture has " + from);
      return;
    }
    text.replace(at, from.size(), to);
    const sp::VariantTransformResult r = sp::TransformGBufferVsHlsl({name, 0}, text);
    Check(!r.ok && !r.skip && r.error.find(message_has) != std::string::npos, what + (r.error.empty() ? "" : " (" + r.error.substr(0, 90) + ")"));
  };
  broken("xCityRoad__VSCityRoad", "\tr4.xyz = tfetchR11G11B10(iNormal0).xyz;\n", "", "fetch a normal", "no normal fetch: an error that names it");
  broken("xCityRoad__VSCityRoad", "\toTexCoord15 = 0.0;\n", "\toTexCoord15 = 0.0;\n\toTexCoord15.x = 1.0;\n", "TEXCOORD15", "TEXCOORD15 written by the game: an error that names it");
  broken("xPropFoliage__VSPropFoliage", "\tr4.xyz = r4.xyz * gWorld(0).xyz;\n", "", "gWorld", "a prop vertex shader without gWorld: an error that names it");
  broken("xCityRoad__VSCityRoad", "\toPos.xyz = oPos.xyz * g_PosScale.xyz + g_PosOffset.xyz * oPos.w;\n", "", "position", "no position scale and offset: an error that names it");
  {
    const sp::VariantTransformResult r = sp::TransformGBufferVsHlsl({"xCityRoad__VSCityRoadNight", 0}, kVertexFixture);
    Check(!r.ok && r.skip, "another vertex shader of the group is left out");
    sp::StageContext with_block = sp::SoloContext(sp::kFeatureGBufferVs);
    with_block.constants_bytes = 16;
    const sp::VariantTransformResult blocked = sp::TransformGBufferVsHlsl({"xCityRoad__VSCityRoad", 0}, kVertexFixture, with_block);
    Check(!blocked.ok && !blocked.skip, "a constants block for the vertex stage is refused");
  }
}

void CheckTable() {
  const auto shaders = std::span<const sp::GBufferDayShader>(sp::kGBufferDayShaders);
  Check(sp::kGBufferDayShaderCount == 42 && sp::kGBufferDayLeftOutCount == 27 && sp::kGBufferDayShaderCount + sp::kGBufferDayLeftOutCount == 69,
        "42 shaders are edited and 27 are left out: the 69 day shaders of the survey");
  std::set<std::string_view> names;
  for (const sp::GBufferDayShader& row : shaders) names.insert(row.name);
  for (const sp::GBufferDayLeftOut& row : sp::kGBufferDayLeftOut) {
    Check(names.insert(row.name).second, std::string(row.name) + ": in the table or the left-out list, not both, once");
    Check(!row.reason.empty(), std::string(row.name) + ": has a reason");
  }
  Check(names.size() == 69, "69 different names");
  size_t by_source[4] = {};
  for (const sp::GBufferDayShader& row : shaders) {
    ++by_source[size_t(row.source)];
    Check(row.name.find("__PS") != std::string_view::npos || row.name == "xTestFoliage__PS", std::string(row.name) + ": a pixel shader's name");
    Check(!row.name.ends_with("Env"), std::string(row.name) + ": no Env shader is edited");
    Check(row.source == sp::GBufferDaySource::kInterpolant ? row.interpolant <= 2 : row.interpolant == 0, std::string(row.name) + ": the interpolant is 0..2 for an interpolant source only");
    Check(!row.family.empty(), std::string(row.name) + ": has a family");
  }
  Check(by_source[size_t(sp::GBufferDaySource::kInterpolant)] == 35 && by_source[size_t(sp::GBufferDaySource::kVertexOrDerivative)] == 3 &&
            by_source[size_t(sp::GBufferDaySource::kDerivative)] == 2 && by_source[size_t(sp::GBufferDaySource::kMasked)] == 2,
        "35 interpolant, 3 vertex-or-derivative, 2 derivative, 2 masked");
  // every vertex-or-derivative pixel shader has its vertex shader in the vertex stage (the road, the tunnel road, the foliage props), and the interpolant it reads is 15
  Check(sp::kGBufferVsInterpolant == 15 && sp::kGBufferVsShaderCount == 3, "the vertex stage has three shaders and writes TEXCOORD15");
  Check(sp::FindGBufferDayShader("xCityDefault__PSMultiLight") == nullptr && sp::FindGBufferDayShader("xCityRoad__PSCityRoadEnv") == nullptr && sp::IsGBufferDayShader("xCityRoad__PSCityRoad"),
        "FindGBufferDayShader knows the table and nothing else");
  Check(sp::GBufferDayShaderNames().size() == sp::kGBufferDayShaderCount, "the names are the table's");
  Check(sp::FindGBufferVsShader("xPropFoliage__VSPropFoliage") != nullptr && sp::FindGBufferVsShader(sp::kGBufferVsLeftOutSkinned) == nullptr, "the skinned foliage vertex shader is not in the vertex stage");
}

void CheckDefinitions() {
  const sp::VariantDef day = sp::MakeGBufferDayStageFromText(kHelper);
  Check(day.feature == sp::kFeatureGBuffer && day.name == "gbuffer" && !day.vertex && day.needs_rpf && day.pixel_name_suffixes.size() == 42 && day.input_xxh3 != 0,
        "the day stage is the G-buffer's feature 10, 42 shaders by name, a pixel stage, with the helper in its fingerprint");
  const chain::FeatureInfo* row = chain::Find(sp::kFeatureGBuffer);
  Check(row && row->name == day.name && row->spec_twin && row->constants_bytes == 48, "the G-buffer's row: 48 bytes, the alpha test twin");
  // the chain's driver gives the same text as the function
  const sp::VariantShader shader{"xCityDefault__PSCityDefault", 0};
  const sp::VariantDef defs[] = {day};
  const sp::VariantTransformResult composed = sp::ComposeStages(defs, chain::FeatureBit(sp::kFeatureGBuffer), shader, kPixelFixture);
  const sp::VariantTransformResult direct = sp::TransformGBufferDayHlsl(shader, kPixelFixture, kHelper);
  Check(composed.ok && direct.ok && composed.hlsl == direct.hlsl, "ComposeStages with the day stage alone is the function's text");

  const sp::VariantDef vs = sp::MakeGBufferVsVariant();
  const chain::FeatureInfo* vrow = chain::Find(sp::kFeatureGBufferVs);
  Check(sp::kFeatureGBufferVs == 12 && vrow && vrow->name == vs.name && vrow->stage == chain::Stage::kVertex && vrow->constants_bytes == 0 && vrow->composes_with == 0 && vrow->needs_vs == 0 && vrow->spec_twin,
        "the vertex stage's row: feature 12, a vertex stage, no block, composes with nothing, the packed normal twin");
  Check(vs.feature == sp::kFeatureGBufferVs && vs.vertex && vs.needs_rpf && vs.pixel_name_suffixes.size() == 3 && vs.pixel_shaders.empty() && vs.vertex_shaders.empty() &&
            vs.version == sp::kGBufferVsVersion && static_cast<bool>(vs.transform),
        "the vertex stage's definition: three vertex shaders by name, a vertex stage, no vertex shaders named for a pixel stage");
  Check(!chain::CanCompose(chain::FeatureBit(sp::kFeatureGBuffer) | chain::FeatureBit(sp::kFeatureGBufferVs)), "a pixel stage and the vertex stage never share a text");
  Check(chain::LayoutConstants(chain::FeatureBit(sp::kFeatureGBufferVs)).total_bytes == 608, "the vertex stage uploads nothing");
  const sp::VariantTransformResult vr = vs.transform({"xCityRoad__VSCityRoad", 0}, kVertexFixture, sp::SoloContext(sp::kFeatureGBufferVs));
  Check(vr.ok, "the vertex definition's transform runs");
}

// ---- composed with another stage -----------------------------------------------------------------------------------------------------------------------

// A stage of this test's own, the shape of what the headlight and the lamps' shadows do to a lit shader: 32 bytes of constants, a helper before main, a local after the cube map's line
// (the same line the derivative block follows). The day stage composes with it in either order, as the golden test (variant_golden_test.cpp) proves for the built-in stages.
sp::VariantDef MakeProbeStage() {
  sp::VariantDef def;
  def.feature = 20;
  def.name = "test_stage";
  def.version = 1;
  def.transform = [](const sp::VariantShader& shader, std::string_view hlsl, const sp::StageContext& ctx) {
    using namespace sp::textedit;
    sp::VariantTransformResult r;
    std::string text = NormalizeNewlines(hlsl);
    std::string error;
    static constexpr sp::ConstantMember kMembers[] = {{"float4", "g_GbTestProbe0"}, {"uint4", "g_GbTestProbe1"}};
    static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0), Lit("#endif"), Ws(0), Lit("void main(")};
    static constexpr Piece kCube[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
    if (!sp::DeclareConstants(text, ctx, "test stage", kMembers, &error) ||
        !ReplaceOnce(text, kMain, 0, 6, "main", [](std::string_view group) { return "float GbTestHelper(float x) { return x * 2.0f; }\n\n" + std::string(group); }, &error) ||
        !ReplaceOnce(text, kCube, 0, 0, "cube map", [](std::string_view group) { return std::string(group) + "\n\tfloat gbTestLocal = GbTestHelper(float(g_GbTestProbe1.x) + g_GbTestProbe0.x);"; }, &error)) {
      r.error = std::string(shader.name) + ": " + error;
      return r;
    }
    r.ok = true;
    r.hlsl = std::move(text);
    return r;
  };
  return def;
}

void CheckComposed(const std::filesystem::path& folder, const std::string& helper, const std::filesystem::path& out) {
  // one shader of each kind
  const char* const names[] = {"xCityDefault__PSCityDefault", "xCitySidewalk__PSCitySidewalk", "xPropDefault__PSPropDefault", "xCityRoad__PSCityRoad", "xCityStoreInterior__PSCityStoreInterior",
                               "xCityBillboard__PSCityBillboard"};
  for (const char* name : names) {
    const std::filesystem::path file = folder / (std::string(name) + ".hlsl");
    if (!std::filesystem::exists(file)) {
      Check(false, std::string("the retail text is there: ") + file.string());
      continue;
    }
    const std::string text = ReadFile(file);
    const sp::GBufferDayShader* row = sp::FindGBufferDayShader(name);
    for (const bool real_first : {true, false}) {
      const uint32_t real_feature = real_first ? sp::kFeatureGBuffer : 25;
      sp::VariantDef moved = sp::MakeGBufferDayStageFromText(helper);
      moved.feature = real_feature;
      chain::FeatureInfo real_row = *chain::Find(sp::kFeatureGBuffer);
      real_row.feature = real_feature;
      real_row.composes_with = chain::FeatureBit(20);
      const chain::FeatureInfo probe = {20, "test_stage", chain::Stage::kPixel, 32, chain::FeatureBit(real_feature), true, 0};
      std::vector<chain::FeatureInfo> table = {real_row, probe};
      std::sort(table.begin(), table.end(), [](const chain::FeatureInfo& x, const chain::FeatureInfo& y) { return x.feature < y.feature; });
      const char* why = nullptr;
      Check(chain::TableIsValid(table, &why), std::string("the test table is valid: ") + (why ? why : ""));
      const std::vector<sp::VariantDef> defs = {moved, MakeProbeStage()};
      const uint32_t set = chain::FeatureBit(20) | chain::FeatureBit(real_feature);
      const std::string label = std::string(name) + " + the test stage (" + (real_first ? "day stage first" : "day stage second") + ")";
      const sp::VariantTransformResult composed = sp::ComposeStages(defs, set, {name, 0}, text, table);
      Check(composed.ok, label + ": composes (" + composed.error + ")");
      if (!composed.ok) continue;
      const uint32_t real_at = chain::OffsetOf(set, real_feature, table);
      const uint32_t test_at = chain::OffsetOf(set, 20, table);
      Check(real_at == (real_first ? 608u : 640u) && test_at == (real_first ? 656u : 608u), label + ": the blocks follow each other at the offsets the chain gives");
      Check(Count(composed.hlsl, "g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + " + std::to_string(real_at) + ", 0x10)") == 1 &&
                Count(composed.hlsl, "g_GBufferMode : packoffset(c" + std::to_string(real_at / 16) + ")") == 1 &&
                Count(composed.hlsl, "g_GbTestProbe0 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + " + std::to_string(test_at) + ", 0x10)") == 1,
            label + ": both blocks are declared once, at their places");
      Check(Count(composed.hlsl, "float2 GbRt1Encode(float3 n)") == 1 && Count(composed.hlsl, "float GbTestHelper(float x)") == 1 && Count(composed.hlsl, sp::GBufferDayWriteText(*row)) == 1 &&
                Count(composed.hlsl, "float gbTestLocal = ") == 1,
            label + ": both stages' text is in it once");
      if (!out.empty()) {
        std::filesystem::create_directories(out / "composed");
        std::ofstream(out / "composed" / (std::string(name) + (real_first ? ".first" : ".second") + ".gbday.hlsl"), std::ios::binary) << composed.hlsl;
      }
    }
  }
}

// No shader of the day stage or of the vertex stage is also a shader of another variant of this tree (a shader has one stage per feature and only the pairs the table composes share a
// text: the day shaders are the headlight's neighbours, not its targets, because it edits the PSMultiLight shaders).
void CheckNoOverlap() {
  std::vector<std::string_view> mine;
  for (const sp::GBufferDayShader& row : sp::kGBufferDayShaders) mine.push_back(row.name);
  for (const sp::GBufferVsShader& row : sp::kGBufferVsShaders) mine.push_back(row.name);
  for (const sp::GBufferDayLeftOut& row : sp::kGBufferDayLeftOut) mine.push_back(row.name);
  std::vector<std::string_view> named = {sp::kBloomGaussX.name, sp::kBloomGaussY.name, sp::kChrome.name, sp::kCarPaint.name, sp::kEnvMipPass.name, sp::kEnvReduce.name,
                                         sp::kGlowSoftTrail.name, sp::kGlowSoftStar.name, sp::kLuminanceSample.name, sp::kUiSharpTextured.name};
  for (const sp::VariantShader& other : sp::kUiSharpRetail) named.push_back(other.name);
  std::vector<std::string_view> suffixes = {sp::kShadowBlendSuffix, sp::kShadowBlendAlphaSuffix};
  for (const std::string_view suffix : sp::kUiSharpSuffixes) suffixes.push_back(suffix);
  for (const std::string_view suffix : sp::kHeadlightSuffixes) suffixes.push_back(suffix);
  bool overlaps = false;
  for (const std::string_view name : mine) {
    for (const std::string_view other : named) overlaps |= name == other;
    for (const std::string_view suffix : suffixes) overlaps |= name.ends_with(suffix);
  }
  Check(!overlaps, "no shader of the day stage, its left-out list or the vertex stage is named by another variant of this tree, or found by the suffixes of the shadow filter, the UI sharpening or the headlight");
}

// ---- the retail text ------------------------------------------------------------------------------------------------------------------------------------

// The day pixel shaders of a folder of translated HLSL, by the survey's rule: the `PS...` entry points of the city's groups that are not the shadow, depth, AO, debug or night ones.
bool IsDayEntry(std::string_view stem) {
  const size_t at = stem.find("__");
  if (at == std::string_view::npos) return false;
  const std::string_view entry = stem.substr(at + 2);
  if (!entry.starts_with("PS")) return false;
  for (const std::string_view skip : {"Shadow", "zPre", "AmbientOcclusion", "Debug", "MultiLight", "Complexity", "MipMaps", "WorldY", "Copy", "Generate", "Blur"}) {
    if (entry.find(skip) != std::string_view::npos) return false;
  }
  return entry.starts_with("PSCity") || entry.starts_with("PSProp") || stem == "xPhysics__PSPhysics" || stem == "xTestFoliage__PS" || stem.starts_with("xCityLampGlassTint__PS");
}

void CheckRetail(const std::filesystem::path& folder, const std::string& helper, const std::filesystem::path& out) {
  size_t edited = 0;
  size_t reads_interpolant = 0;
  for (const sp::GBufferDayShader& row : sp::kGBufferDayShaders) {
    const std::filesystem::path file = folder / (std::string(row.name) + ".hlsl");
    if (!std::filesystem::exists(file)) {
      Check(false, "the retail text is there: " + file.string());
      continue;
    }
    const std::string hlsl = ReadFile(file);
    const sp::VariantTransformResult r = sp::TransformGBufferDayHlsl({row.name, 0}, hlsl, helper);
    Check(r.ok, std::string(row.name) + ": the retail text transforms: " + r.error);
    if (!r.ok) continue;
    ++edited;
    const std::string diff = FirstDifference(WithoutDayStage(r.hlsl, helper, row), LfOnly(hlsl));
    Check(diff.empty(), std::string(row.name) + ": with the stage's insertions taken out the retail text is the game's again, byte for byte" + diff);
    // the facts the sources stand on, in the retail text
    Check(Count(hlsl.substr(hlsl.rfind("void main(")), "iTexCoord15") == 1, std::string(row.name) + ": TEXCOORD15 is declared and nothing reads it (the translator declares all sixteen)");
    if (row.source == sp::GBufferDaySource::kInterpolant) {
      const std::string preamble = "= iTexCoord" + std::to_string(row.interpolant) + ";";
      if (hlsl.find(preamble, hlsl.rfind("void main(")) != std::string::npos) ++reads_interpolant;
    }
    if (!out.empty()) {
      std::filesystem::create_directories(out);
      std::ofstream(out / (std::string(row.name) + ".gbday.hlsl"), std::ios::binary) << r.hlsl;
    }
  }
  std::printf("%zu day pixel shaders transformed; %zu of the interpolant ones read the interpolant themselves too\n", edited, reads_interpolant);
  Check(reads_interpolant == 35, "every interpolant shader loads its interpolant into a register itself (the vertex shader's slot is one the pixel shader expects)");

  // The left-out shaders are real files (the list is not stale), and nothing the survey's rule calls a day shader is missing from both lists.
  for (const sp::GBufferDayLeftOut& row : sp::kGBufferDayLeftOut) {
    Check(std::filesystem::exists(folder / (std::string(row.name) + ".hlsl")), std::string(row.name) + ": the left-out shader exists");
  }
  size_t day_found = 0;
  for (const auto& entry : std::filesystem::directory_iterator(folder)) {
    if (entry.path().extension() != ".hlsl") continue;
    const std::string stem = entry.path().stem().string();
    if (!IsDayEntry(stem)) continue;
    ++day_found;
    const bool listed = sp::IsGBufferDayShader(stem) || std::any_of(std::begin(sp::kGBufferDayLeftOut), std::end(sp::kGBufferDayLeftOut), [&](const sp::GBufferDayLeftOut& l) { return l.name == stem; });
    Check(listed, stem + ": a day pixel shader of the folder is in the table or the left-out list");
  }
  std::printf("%zu day pixel shaders in the folder by the survey's rule\n", day_found);
  Check(day_found == 69, "the folder has the survey's 69 day pixel shaders");

  // The vertex stage on the three vertex shaders.
  for (const sp::GBufferVsShader& row : sp::kGBufferVsShaders) {
    const std::filesystem::path file = folder / (std::string(row.name) + ".hlsl");
    if (!std::filesystem::exists(file)) {
      Check(false, "the retail vertex shader is there: " + file.string());
      continue;
    }
    const std::string hlsl = ReadFile(file);
    const sp::VariantTransformResult r = sp::TransformGBufferVsHlsl({row.name, 0}, hlsl);
    Check(r.ok, std::string(row.name) + ": the retail vertex shader transforms: " + r.error);
    if (!r.ok) continue;
    const std::string diff = FirstDifference(WithoutVsStage(r.hlsl, row), LfOnly(hlsl));
    Check(diff.empty(), std::string(row.name) + ": with the export taken out the retail text is the game's again, byte for byte" + diff);
    std::printf("%s: %zu -> %zu bytes\n", std::string(row.name).c_str(), hlsl.size(), r.hlsl.size());
    if (!out.empty()) {
      std::filesystem::create_directories(out);
      std::ofstream(out / (std::string(row.name) + ".gbvs.hlsl"), std::ios::binary) << r.hlsl;
    }
  }
  // The skinned foliage vertex shader and the night/other vertex shaders of the same groups stay as they are (skip), and no vertex shader of the three families writes TEXCOORD15 but with zero.
  for (const std::string_view name : {sp::kGBufferVsLeftOutSkinned, std::string_view("xCityRoad__VSCityRoadNight")}) {
    const sp::VariantTransformResult r = sp::TransformGBufferVsHlsl({name, 0}, "");
    Check(!r.ok && r.skip, std::string(name) + ": left out of the vertex stage");
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckTable();
  CheckPixelFixture();
  CheckVertexFixture();
  CheckDefinitions();
  CheckNoOverlap();
  std::string helper;
  if (argc > 1) {
    helper = ReadFile(argv[1]);
    Check(!helper.empty(), std::string("the helper file is readable: ") + argv[1]);
    const sp::VariantTransformResult r = sp::TransformGBufferDayHlsl({"xPropTrafficSignal__PSPropTrafficSignal", 0}, kPixelFixture, helper);
    Check(r.ok, "the real helper pastes into the fixture: " + r.error);
    Check(Count(r.hlsl, "float2 GbRt1Encode(float3 n)") == 1 && Count(r.hlsl, "float3 GbWorldPoint(float4 ray") == 1 && Count(r.hlsl, "float3 GbFaceNormal(float3 dpdx") == 1,
          "the real helper is in it once, with the derivative normal's functions");
  }
  if (argc > 2) {
    const std::string used_helper = helper.empty() ? std::string(kHelper) : helper;
    const std::filesystem::path out = argc > 3 ? std::filesystem::path(argv[3]) : std::filesystem::path();
    CheckRetail(argv[2], used_helper, out);
    CheckComposed(argv[2], used_helper, out);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

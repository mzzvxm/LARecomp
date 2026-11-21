// Check of the G-buffer's car, traffic and character families (gbuffer_cars.h, package GB-G1). A program of its own: it needs no renderer, no ReXGlue, no compiler and no game files, and exits 0
// when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_cars_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_gbuffer_cars_test [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the transformed texts>]]]
//
// Without arguments it runs on small texts made here (the shape of the translator's paint, plastic and tyre shaders the edits look at) and a stand-in for the helper. With the helper file as the
// first argument the real one is pasted. With a folder of translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files; the folder shaderpack/baseline/hlsl-rpf of the development PC
// has them in its `cars`, `characters`, `city`, `effects` and `ui` subfolders, and the test finds a shader in the folder itself or in any of those) it runs on every row of the table: the retail
// text must transform at 1, 2 and 3 planes, and with the insertions taken out must be the game's text again, byte for byte (the colour path is untouched); the albedo of every row that has one is
// found; the names of the table are the full names of the shaders (no suffix matches another); no identifier of the helper is the name of a constant of any shader; and the census prints. A third
// argument is a folder that receives the edited texts (`<name>.gbcars.hlsl` at the stage's planes, `<name>.gbcars3.hlsl` at three, and the composed ones in `composed`), for a compile with dxc
// (-T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout, and without -spirv for DXIL; with -DXENOS_SPEC_CONSTANTS=0 and =2 for the alpha test twin) and spirv-val.

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
#include "gpu/gbuffer_rules.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variants/car_chrome.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/gbuffer_cars.h"
#include "shaderpack/variants/gbuffer_day.h"
#include "shaderpack/variants/stage_constants.h"

namespace sp = larecomp::shaderpack;
namespace chain = larecomp::shaderpack::chain;
namespace gb = mcla::gpu::gbuffer;

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

bool EndsWith(std::string_view text, std::string_view suffix) { return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix; }

// "" when the two texts are the same, else where they first differ for the message of a failed check.
std::string FirstDifference(const std::string& got, const std::string& want) {
  if (got == want) return std::string();
  size_t at = 0;
  while (at < got.size() && at < want.size() && got[at] == want[at]) ++at;
  const auto around = [&](const std::string& text) { return text.substr(at > 20 ? at - 20 : 0, 60); };
  return " (first difference at byte " + std::to_string(at) + ": got '" + around(got) + "', want '" + around(want) + "'; sizes " + std::to_string(got.size()) + " and " +
         std::to_string(want.size()) + ")";
}

// Takes the car stage's insertions out again (the outputs, the constants rows, the helper, the derivative block, the capture, the write) and gives back what is left: it must be the game's text,
// byte for byte, with the line ends of the transform (LF). The colour is then computed by the game's own statements alone.
std::string WithoutCarStage(std::string text, std::string helper, const sp::GBufferCarShader& row, const sp::GBufferCarEdits& edits, uint32_t planes) {
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
  cut("the outputs", sp::GBufferCarsOutputText(planes));
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
  if (row.normal == sp::GBufferCarNormal::kDerivative) cut("the derivative block", sp::GBufferDayDerivativeText());
  if (planes >= 2 && edits.found && edits.insert_at != std::string::npos) cut("the capture", edits.insert_text);
  cut("the sidecar write", sp::GBufferCarsWriteText(row, planes >= 2 ? edits : sp::GBufferCarEdits(), planes));
  return missing.empty() ? text : "<missing or twice there:" + missing + ">";
}

// ---- the fixtures: the shape of the translator's shaders the edits look at --------------------------------------------------------------------------------------

// The declarations every translated text has (the constants the edits add to, the derivative block's anchors, the interpolants, the front face flag), the constants of the game's own that a row
// names (SPIR-V `#define`, DXIL member: `constants`), and the statements of main (`body`).
std::string Fixture(const std::string& constants, const std::string& body) {
  return std::string(
             "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n"
             "#define DEFINE_SHARED_CONSTANTS() \\\n"
             "    float4 g_PosOffset : packoffset(c36); \\\n"
             "    float2 g_InvResolutionScale : packoffset(c37.x);\n"
             "float4 applyBlendPremult(float4 color) { return color; }\n") +
         constants +
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
         "\tfloat4 r2 = iTexCoord2;\n"
         "\tfloat4 r3 = iTexCoord1;\n"
         "\tfloat4 r7 = iTexCoord1;\n"
         "\tCubeMapData cubeMapData = (CubeMapData)0;\n" +
         body +
         "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
         "\t}\t\toC0 = applyBlendPremult(oC0);\n"
         "\treturn;\n"
         "}\n";
}

constexpr const char* kPaintBody =
    "\tr7.xyz = r7.xyz * r0.yyy + r3.yzx;\n"
    "\toC0.xyz = r7.xyz;\n";

// A bump spec plastic: a diffuse fetch whose register is written again later (so it is captured), baseColor and smoothness.
constexpr const char* kPlasticConstants =
    "#define baseColor vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 1600, 0x10)\n"
    "#define smoothness vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 1664, 0x10)\n"
    "cbuffer PixelShaderConstants : register(b1, space4)\n"
    "{\n"
    "\tfloat4 baseColor : packoffset(c100);\n"
    "\tfloat4 smoothness : packoffset(c104);\n"
    "};\n";
constexpr const char* kPlasticBody =
    "\tr2.xyzw = tfetch2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).yzxw;\n"
    "\tr4.xyz = baseColor.xyz * r2.zxy;\n"
    "\tr2.xyz = r4.xyz * smoothness.xxx;\n"
    "\toC0.xyz = r2.xyz;\n";

// A tyre: the gradient fetch, its register is not written again (read at the end of main).
constexpr const char* kTyreBody =
    "\tr10.xyzw = tfetch2DGrad(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r1.xy, float2(0, 0), tfetchGradientH.xy, tfetchGradientV.xy).xyzw;\n"
    "\toC0.xyz = r10.xyz;\n";

// The rage shaders: the sampler has another name.
constexpr const char* kRageBody =
    "\tr4.xyzw = tfetch2D(TextureSampler_Texture2DDescriptorIndex, TextureSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).wxyz;\n"
    "\toC0.xyz = r4.yzw;\n";

constexpr const char* kHelper =
    "#define GB_CLASS_PAINT 8\n"
    "#define GB_CLASS_WORLD 1\n"
    "#define GB_FLAG_VEHICLE 4\n"
    "#define GB_MODE_WRITE_NORMAL 1\n"
    "#define GB_MODE_WRITE_ALBEDO 2\n"
    "#define GB_MODE_WRITE_MATERIAL 4\n"
    "#define GB_MODE_FLIP_BACKFACE 8\n"
    "float2 GbOctEncode(float3 n) { return n.xy; }\n"
    "float2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\n"
    "float3 GbAlbedoEncode(float3 a) { return a; }\n"
    "uint GbMaterialWord(float s, uint c, uint f, float l) { return c; }\n"
    "float2 GbMaterialRt3(uint w) { return float2(w, w); }\n"
    "float GbMaterialLit(uint w) { return 1.0f; }\n"
    "float3 GbWorldPoint(float4 ray, float4 q, float px, float py, float w) { return ray.xyz; }\n"
    "float3 GbFaceNormal(float3 dpdx, float3 dpdy, float3 to_camera) { return dpdx; }\n";

constexpr std::string_view kPaintName = "xCarPaintCustomizable__PS_CarPaint";

void CheckFixture() {
  const sp::GBufferCarShader* row = sp::FindGBufferCarShader(kPaintName);
  Check(row != nullptr, "the player's paint is in the table");
  if (!row) return;
  Check(row->normal == sp::GBufferCarNormal::kInterpolant && row->slot == 2 && row->albedo == sp::GBufferCarAlbedo::kPaint && row->material == sp::GBufferCarMaterial::kHost,
        "the paint row: interpolant 2, the paint register, the host's word");
  const std::string paint = Fixture("", kPaintBody);
  for (const uint32_t planes : {1u, 2u, 3u}) {
    const std::string label = "the paint fixture at " + std::to_string(planes) + " plane(s)";
    const sp::VariantShader shader{kPaintName, 0};
    const sp::VariantTransformResult r = sp::TransformGBufferCarsHlsl(shader, paint, kHelper, sp::SoloContext(sp::kFeatureGBuffer), planes);
    Check(r.ok && !r.skip && r.error.empty(), label + " transforms: " + r.error);
    if (!r.ok) continue;
    Check(Count(r.hlsl, "oC1 : SV_Target1") == 1 && Count(r.hlsl, "oC2 : SV_Target2") == (planes >= 2 ? 1u : 0u) && Count(r.hlsl, "oC3 : SV_Target3") == (planes >= 3 ? 1u : 0u),
          label + ": as many outputs as planes");
    Check(Count(r.hlsl, "float3 gbN = iTexCoord2.xyz;") == 1 && Count(r.hlsl, "oC1 = GbRt1Encode(gbN);") == 1, label + ": the normal is the interpolant's");
    Check(Count(r.hlsl, "if ((g_GBufferMode.x & uint(GB_MODE_FLIP_BACKFACE)) != 0u && !bool(iFace))") == 1, label + ": a back face of a two sided draw is turned to the viewer");
    Check(Count(r.hlsl, "float3 gbAlbedo = float3(r7.z, r7.x, r7.y);") == (planes >= 2 ? 1u : 0u), label + ": the paint's capture is there with the albedo plane only");
    Check(Count(r.hlsl, "oC2 = float4(GbAlbedoEncode(gbAlbedo), GbMaterialLit(gbMat));") == (planes >= 2 ? 1u : 0u), label + ": the albedo write");
    Check(Count(r.hlsl, "uint gbMat = g_GBufferMode.y;") == (planes >= 2 ? 1u : 0u), label + ": the draw's word is the material");
    Check(Count(r.hlsl, "oC3 = GbMaterialRt3(gbMat);") == (planes >= 3 ? 1u : 0u), label + ": the material write");
    Check(Count(r.hlsl, "GbMaterialWord(") == Count(kHelper, "GbMaterialWord("), label + ": no class is made in the shader (no call of the helper's word): the host's word is used");
    // the capture is after the game's statement and before the write
    const size_t anchor = r.hlsl.find(std::string(row->anchor));
    const size_t capture = r.hlsl.find("float3 gbAlbedo = ");
    const size_t write = r.hlsl.find("oC0 = applyBlendPremult(oC0);");
    Check(planes < 2 || (anchor != std::string::npos && capture > anchor && write > capture), label + ": the capture follows the game's statement and the write follows it");
    // the colour path: the insertions out leave the original
    const sp::GBufferCarEdits edits = planes >= 2 ? sp::PlanGBufferCarEdits(*row, LfOnly(paint)) : sp::GBufferCarEdits();
    const std::string back = WithoutCarStage(r.hlsl, kHelper, *row, edits, planes);
    Check(back == LfOnly(paint), label + ": the game's text is untouched" + FirstDifference(back, LfOnly(paint)));
  }
  // a shader that is not in the table is skipped; a text that is not the translator's one keeps the normal and loses the albedo, or fails with a name
  const sp::VariantShader other{"xCityDefault__PSCityDefault", 0};
  const sp::VariantTransformResult skipped = sp::TransformGBufferCarsHlsl(other, paint, kHelper);
  Check(!skipped.ok && skipped.skip, "a city shader is skipped");
  std::string broken = paint;
  broken.replace(broken.find("r7.xyz = r7.xyz * r0.yyy + r3.yzx;"), 34, "r7.xyz = r7.xyz;");
  const sp::VariantTransformResult no_anchor = sp::TransformGBufferCarsHlsl({kPaintName, 0}, broken, kHelper, sp::SoloContext(sp::kFeatureGBuffer), 2);
  Check(no_anchor.ok && Count(no_anchor.hlsl, "oC1 = GbRt1Encode(gbN);") == 1 && Count(no_anchor.hlsl, "oC2 = float4(GbAlbedoEncode(") == 0 && Count(no_anchor.hlsl, "oC2 : SV_Target2") == 1,
        "without the paint's statement the shader still writes its normal, and RT2 is 'no data'");
  const sp::GBufferCarEdits lost = sp::PlanGBufferCarEdits(*row, broken);
  Check(!lost.found && lost.why_not.find("paint's statement") != std::string::npos, "the lost anchor is named: " + lost.why_not);
  std::string no_decl = paint;
  no_decl.replace(no_decl.find("in centroid float4 iTexCoord2 : TEXCOORD2"), 41, "in centroid float4 iTexCoordX : TEXCOORDX");
  const sp::VariantTransformResult no_slot = sp::TransformGBufferCarsHlsl({kPaintName, 0}, no_decl, kHelper);
  Check(!no_slot.ok && !no_slot.skip && no_slot.error.find("iTexCoord2") != std::string::npos, "without the interpolant's declaration it fails and names it");
  std::string no_face = paint;
  no_face.replace(no_face.find("in uint iFace : SV_IsFrontFace"), 31, "in uint iFaceX : SV_IsFrontFace");
  const sp::VariantTransformResult no_face_r = sp::TransformGBufferCarsHlsl({kPaintName, 0}, no_face, kHelper);
  Check(!no_face_r.ok && !no_face_r.skip && no_face_r.error.find("iFace") != std::string::npos, "without the front face flag it fails and names it");
  const sp::VariantTransformResult bad_planes = sp::TransformGBufferCarsHlsl({kPaintName, 0}, paint, kHelper, sp::SoloContext(sp::kFeatureGBuffer), 4);
  Check(!bad_planes.ok && !bad_planes.skip, "four planes do not exist");

  // a derivative row: the same text with the derivative block and no interpolant
  const sp::GBufferCarShader* deriv = sp::FindGBufferCarShader("xblackmatte__PS_BlackMatte");
  Check(deriv && deriv->normal == sp::GBufferCarNormal::kDerivative, "the black matte is a derivative row");
  if (deriv) {
    const sp::VariantTransformResult r = sp::TransformGBufferCarsHlsl({deriv->name, 0}, paint, kHelper);
    Check(r.ok && Count(r.hlsl, "float3 gbP = GbWorldPoint(") == 1 && Count(r.hlsl, "GbFaceNormal(gbDx, gbDy, -gbP)") == 1 && Count(r.hlsl, "gbN") == 0,
          "a derivative row transforms with the derivative block and no interpolant");
    const std::string back = WithoutCarStage(r.hlsl, kHelper, *deriv, sp::PlanGBufferCarEdits(*deriv, LfOnly(paint)), sp::kGBufferCarsPlanes);
    Check(back == LfOnly(paint), "a derivative row leaves the game's text untouched" + FirstDifference(back, LfOnly(paint)));
  }

  // the plastic: the diffuse fetch is overwritten later (captured right after it), times baseColor, and the shader's own smoothness replaces the word's byte
  const sp::GBufferCarShader* plastic = sp::FindGBufferCarShader("xBumpSpec__PS_BumpSpec_true_true");
  Check(plastic && plastic->albedo == sp::GBufferCarAlbedo::kDiffuse && plastic->tint == "baseColor.xyz" && plastic->material == sp::GBufferCarMaterial::kBumpSpec &&
            plastic->sampler == "DiffuseSampler" && !plastic->gradient,
        "the bump spec plastic: the diffuse texel times baseColor, its own smoothness");
  if (plastic) {
    const std::string text = Fixture(kPlasticConstants, kPlasticBody);
    const sp::GBufferCarEdits edits = sp::PlanGBufferCarEdits(*plastic, text);
    Check(edits.found && edits.smoothness && edits.expression == "gbAlbedo * baseColor.xyz" && edits.insert_text == "\tfloat3 gbAlbedo = r2.zxy;\n" && edits.insert_at != std::string::npos,
          "the plastic's texel is captured after its fetch (r2 is written again) and multiplied by baseColor: " + edits.expression + " / " + edits.why_not);
    const sp::VariantTransformResult r = sp::TransformGBufferCarsHlsl({plastic->name, 0}, text, kHelper, sp::SoloContext(sp::kFeatureGBuffer), 3);
    Check(r.ok, "the plastic transforms: " + r.error);
    if (r.ok) {
      Check(Count(r.hlsl, "gbMat = (gbMat & 0xFFFFFF00u) | uint(saturate(smoothness.x) * 255.0f + 0.5f);") == 1, "the plastic's smoothness replaces the word's byte");
      Check(Count(r.hlsl, "oC2 = float4(GbAlbedoEncode(gbAlbedo * baseColor.xyz), GbMaterialLit(gbMat));") == 1, "the plastic's albedo is written");
      const size_t fetch = r.hlsl.find("r2.xyzw = tfetch2D(");
      const size_t capture = r.hlsl.find("float3 gbAlbedo = r2.zxy;");
      const size_t next = r.hlsl.find("r4.xyz = baseColor.xyz * r2.zxy;");
      Check(fetch != std::string::npos && capture > fetch && next > capture, "the capture is between the fetch and the next statement of the game's");
      const std::string back = WithoutCarStage(r.hlsl, kHelper, *plastic, edits, 3);
      Check(back == LfOnly(text), "the plastic's game text is untouched" + FirstDifference(back, LfOnly(text)));
    }
    // a text that does not declare the constant has no albedo and no smoothness of its own (the HLSL would not compile with them) and keeps its normal
    const sp::GBufferCarEdits none = sp::PlanGBufferCarEdits(*plastic, Fixture("", kPlasticBody));
    Check(!none.found && !none.smoothness && none.why_not.find("baseColor") != std::string::npos, "a text without the colour constant has no albedo and no own smoothness: " + none.why_not);
  }
  // the tyre: the gradient fetch (not captured: its register stays); the rage shaders: another sampler and the swizzle w x y z
  const sp::GBufferCarShader* tyre = sp::FindGBufferCarShader("xAmbientTire__PSAmbientTire");
  Check(tyre && tyre->gradient && tyre->albedo == sp::GBufferCarAlbedo::kDiffuse && tyre->tint.empty(), "the traffic tyre takes the gradient fetch of its texel");
  if (tyre) {
    const sp::GBufferCarEdits edits = sp::PlanGBufferCarEdits(*tyre, Fixture("", kTyreBody));
    Check(edits.found && edits.expression == "r10.xyz" && edits.insert_at == std::string::npos, "the tyre's gradient fetch is the albedo, read at the end of main: " + edits.why_not + edits.expression);
    // the same text with the gradient fetch is no albedo for a shader whose row says the plain fetch
    const sp::GBufferCarShader* plain = sp::FindGBufferCarShader("xUnderbody__PS");
    Check(plain && !plain->gradient && !sp::PlanGBufferCarEdits(*plain, Fixture("", kTyreBody)).found, "a plain fetch row does not take a gradient fetch");
  }
  const sp::GBufferCarShader* rage = sp::FindGBufferCarShader("xrage_emissive__PS_Textured");
  Check(rage && rage->sampler == "TextureSampler", "the rage shaders' texel is the TextureSampler's");
  if (rage) {
    const sp::GBufferCarEdits edits = sp::PlanGBufferCarEdits(*rage, Fixture("", kRageBody));
    Check(edits.found && edits.expression == "r4.yzw", "the rage shader's texel (lanes y z w of a .wxyz fetch): " + edits.why_not + edits.expression);
    const sp::GBufferCarShader* diffuse = sp::FindGBufferCarShader("xUnderbody__PS");
    Check(diffuse && !sp::PlanGBufferCarEdits(*diffuse, Fixture("", kRageBody)).found, "a DiffuseSampler row does not take the TextureSampler's fetch");
  }
  // the constants alone (the carbon fibre)
  const sp::GBufferCarShader* carbon = sp::FindGBufferCarShader("xCarbonFiber__PS_CarbonFiber");
  Check(carbon && carbon->albedo == sp::GBufferCarAlbedo::kConstant && carbon->tint == "BaseColorC.xyz * TintColorC.xyz", "the carbon fibre is the product of its two colour constants");
  if (carbon) {
    const std::string constants =
        "#define BaseColorC vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 1040, 0x10)\n"
        "#define TintColorC vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 1024, 0x10)\n"
        "cbuffer PixelShaderConstants : register(b1, space4)\n{\n\tfloat4 BaseColorC : packoffset(c65);\n\tfloat4 TintColorC : packoffset(c64);\n};\n";
    const sp::GBufferCarEdits edits = sp::PlanGBufferCarEdits(*carbon, Fixture(constants, "\toC0.xyz = BaseColorC.xyz;\n"));
    Check(edits.found && edits.expression == "BaseColorC.xyz * TintColorC.xyz" && edits.insert_at == std::string::npos, "the carbon fibre's albedo is the constants' product: " + edits.why_not);
  }
}

// ---- the table -------------------------------------------------------------------------------------------------------------------------------------------

// Whether a piece of inserted text assigns a register of the game's (`r12.xyz = ...`, `ps = ...`, `p0 = ...`, `a0 = ...`): the stage only adds names that start with gb, or the outputs.
bool AssignsGameRegister(std::string_view text) {
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    const bool start = i == 0 || !(std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_');
    if (!start) continue;
    size_t j = i;
    if (text[j] == 'r' && std::isdigit(static_cast<unsigned char>(text[j + 1]))) {
      ++j;
      while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
    } else if (text.substr(j, 2) == "ps" || text.substr(j, 2) == "p0" || text.substr(j, 2) == "a0" || text.substr(j, 2) == "aL") {
      j += 2;
    } else {
      continue;
    }
    if (j < text.size() && text[j] == '.') {
      ++j;
      while (j < text.size() && std::isalpha(static_cast<unsigned char>(text[j]))) ++j;
    }
    while (j < text.size() && text[j] == ' ') ++j;
    if (j + 1 < text.size() && text[j] == '=' && text[j + 1] != '=') return true;
  }
  return false;
}

void CheckTable() {
  std::set<std::string_view> names;
  size_t interpolants = 0, derivatives = 0, tinted = 0, own_smoothness = 0;
  std::map<std::string_view, size_t> by_family;
  std::map<std::string_view, size_t> by_plan;
  std::map<uint32_t, size_t> by_slot;
  for (size_t i = 0; i < sp::kGBufferCarShaderCount; ++i) {
    const sp::GBufferCarShader& row = sp::kGBufferCarShaders[i];
    const std::string who(row.name);
    Check(names.insert(row.name).second, who + " is in the table once");
    Check(row.name.find("__") != std::string_view::npos, who + " is named group__entry");
    Check(row.slot < 16, who + ": the slot is an interpolant");
    Check(row.normal == sp::GBufferCarNormal::kInterpolant || row.slot == 0, who + ": a derivative row has no slot");
    Check(!EndsWith(row.name, "__PSMultiLight") && !EndsWith(row.name, "__PSMultiLightEnv") && !EndsWith(row.name, "__PSMultiLightInterior"), who + " is not a night shader (GB-C's stage owns those)");
    Check(!sp::IsGBufferDayShader(row.name), who + " is not a day shader (GB-D's table)");
    // the albedo fields fit the source
    switch (row.albedo) {
      case sp::GBufferCarAlbedo::kNone:
        Check(row.sampler.empty() && !row.gradient && row.tint.empty() && row.anchor.empty() && row.expression.empty(), who + ": no albedo, no fields");
        break;
      case sp::GBufferCarAlbedo::kDiffuse:
        Check((row.sampler == "DiffuseSampler" || row.sampler == "TextureSampler") && row.anchor.empty() && row.expression.empty(), who + ": the diffuse texel names its sampler");
        tinted += row.tint.empty() ? 0 : 1;
        break;
      case sp::GBufferCarAlbedo::kPaint:
        Check(!row.anchor.empty() && !row.expression.empty() && row.sampler.empty() && row.tint.empty() && !row.gradient, who + ": the paint names its statement and its register");
        Check(row.anchor.find('\n') == std::string_view::npos && row.anchor.back() == ';', who + ": the paint's statement is one statement");
        break;
      case sp::GBufferCarAlbedo::kConstant:
        Check(!row.tint.empty() && row.sampler.empty() && row.anchor.empty() && !row.gradient, who + ": the constant colour names its constants");
        break;
    }
    Check(row.gradient ? row.albedo == sp::GBufferCarAlbedo::kDiffuse : true, who + ": only a diffuse texel is fetched with the gradients");
    Check(row.tint.find("baseColor") == std::string_view::npos || row.albedo == sp::GBufferCarAlbedo::kDiffuse, who + ": baseColor multiplies a texel");
    if (row.material == sp::GBufferCarMaterial::kBumpSpec) {
      ++own_smoothness;
      Check(row.albedo == sp::GBufferCarAlbedo::kDiffuse && row.tint.find("baseColor.xyz") != std::string_view::npos, who + ": the xBumpSpec family's smoothness goes with its baseColor");
    }
    Check(!AssignsGameRegister(sp::GBufferCarsWriteText(row, sp::GBufferCarEdits(), 3)) && !AssignsGameRegister(sp::GBufferCarsOutputText(3)), who + ": no inserted statement assigns a register of the game's");
    (row.normal == sp::GBufferCarNormal::kInterpolant ? interpolants : derivatives)++;
    if (row.normal == sp::GBufferCarNormal::kInterpolant) ++by_slot[row.slot];
    ++by_family[row.family];
    ++by_plan[sp::GBufferCarAlbedoSource(row)];
  }
  for (size_t i = 0; i < sp::kGBufferCarLeftOutCount; ++i) {
    const sp::GBufferCarLeftOut& row = sp::kGBufferCarLeftOut[i];
    Check(!row.reason.empty(), std::string(row.name) + " is left out with a reason");
    Check(!names.contains(row.name), std::string(row.name) + " is not in both lists");
  }
  Check(interpolants == 65 && derivatives == 8 && sp::kGBufferCarShaderCount == 73, "the table has 65 exported normals and 8 derivative rows (73)");
  Check(sp::kGBufferCarLeftOutCount == 19, "19 shaders are left out");
  Check(sp::kGBufferCarShaderCount + sp::kGBufferCarLeftOutCount == 92, "92 lit pixel shaders in all: 64 of the cars, 8 of the traffic, 20 of the characters");
  Check(by_family["player/racer"] == 48 && by_family["traffic"] == 6 && by_family["ped/character"] == 19, "census by family: 48 cars, 6 traffic, 19 characters edited");
  Check(by_plan[sp::kPlanDiffuse] + by_plan[sp::kPlanDiffuseTint] == 51 && by_plan[sp::kPlanDiffuseTint] == 22 && by_plan[sp::kPlanPaint] == 4 && by_plan[sp::kPlanConstant] == 2 && by_plan[sp::kPlanNone] == 16,
        "census by albedo: 51 diffuse texels (22 times colour constants), 4 paint registers, 2 colour constants, 16 none");
  Check(tinted == 22 && own_smoothness == 11, "22 tinted texels and 11 shaders with a smoothness of their own");
  std::printf("census: %zu edited (%zu interpolant, %zu derivative), %zu left out; by slot:", sp::kGBufferCarShaderCount, interpolants, derivatives, sp::kGBufferCarLeftOutCount);
  for (const auto& [slot, n] : by_slot) std::printf(" T%u=%zu", slot, n);
  std::printf("; by albedo source:");
  for (const auto& [plan, n] : by_plan) std::printf(" '%s'=%zu", std::string(plan).c_str(), n);
  std::printf("; own smoothness %zu\n", own_smoothness);
  // the planes the stage declares are the core's; GB-D's derivative block is shared
  Check(sp::kGBufferCarsPlanes == gb::PlaneCount(gb::kStagePlanes) && sp::kGBufferCarsPlanes >= 1 && sp::kGBufferCarsPlanes <= 3, "the stage declares as many planes as the core carries");
  Check(!sp::GBufferDayDerivativeText().empty(), "GB-D's derivative block is there to share");
  // the pack's names: a shader name of the stage is one the VariantDef finds
  const sp::VariantDef def = sp::MakeGBufferCarsStageFromText(kHelper);
  Check(def.feature == sp::kFeatureGBuffer && def.pixel_name_suffixes.size() == sp::kGBufferCarShaderCount && def.needs_rpf, "the stage's definition lists the table's names, in the RPF");
}

// ---- the host's word: the classes the shaders write (GB-F1) ------------------------------------------------------------------------------------------------------

struct ClassPin {
  std::string_view group;
  gb::SurfaceClass cls;
  bool vehicle;
};

// What the F1 class table (gpu/gbuffer_material.h) says of each group of the table, pinned: the stage writes the host's word, so this is the class, the flags and the lit weight each car shader
// has in RT3 and RT2.a. A change of F1's table moves a car's class here and the test says which.
constexpr ClassPin kPins[] = {
    // paint: the clear coat (the game's own environment reflections stay; no SSR flag), the traffic bodies, the carbon fibre under clear coat
    {"xCarPaintCustomizable", gb::SurfaceClass::kPaint, true},
    {"xCarPaint", gb::SurfaceClass::kPaint, true},
    {"xCarPaintNormalMapped", gb::SurfaceClass::kPaint, true},
    {"xCarbonFiber", gb::SurfaceClass::kPaint, true},
    {"xAmbientCarPaint", gb::SurfaceClass::kPaint, true},
    {"xAmbientBumpSpec", gb::SurfaceClass::kPaint, true},
    {"xAmbientVehicle", gb::SurfaceClass::kPaint, true},
    // chrome, rims, badges
    {"xBumpedAlphaChrome", gb::SurfaceClass::kChrome, true},
    {"xChrome", gb::SurfaceClass::kChrome, true},
    {"xRimBadge", gb::SurfaceClass::kChrome, true},
    {"xRimMain", gb::SurfaceClass::kChrome, true},
    {"xRimNormalMapped", gb::SurfaceClass::kChrome, true},
    // metal
    {"xAnisotropicBilletMetal", gb::SurfaceClass::kMetal, true},
    {"xBrushedMetal", gb::SurfaceClass::kMetal, true},
    {"xBumpedBrushedMetal", gb::SurfaceClass::kMetal, true},
    {"xbrakecaliper", gb::SurfaceClass::kMetal, true},
    {"xWheelBrakeRotor", gb::SurfaceClass::kMetal, true},
    // glass drawn opaque (the blended glass is left out)
    {"xBlackOpaqueGlass", gb::SurfaceClass::kGlass, true},
    {"xbumpedglass", gb::SurfaceClass::kGlass, true},
    // lights: emissive, lit weight 0
    {"xCarLight", gb::SurfaceClass::kEmissive, true},
    {"xAmbientCarLight", gb::SurfaceClass::kEmissive, true},
    {"xrage_emissive", gb::SurfaceClass::kEmissive, true},
    // the cockpit's emissive displays: emissive, lit 0. CHECK (cosmetic): the class table does not flag them as a vehicle (their groups are listed with the city's emissive ones)
    {"xInteriorTexturedEmissive", gb::SurfaceClass::kEmissive, false},
    {"xInteriorTexturedEmissiveAlpha", gb::SurfaceClass::kEmissive, false},
    // tyres and rubber, interior trim, plates, plastics: the world's class flagged as a vehicle
    {"xAmbientTire", gb::SurfaceClass::kWorld, true},
    {"xWheelRubber", gb::SurfaceClass::kWorld, true},
    {"xInteriorTrim", gb::SurfaceClass::kWorld, true},
    {"xInteriorTrimAlpha", gb::SurfaceClass::kWorld, true},
    {"xLicensePlate", gb::SurfaceClass::kWorld, true},
    {"xTexturedPlastic", gb::SurfaceClass::kWorld, true},
    {"xBumpSpec", gb::SurfaceClass::kWorld, true},
    {"xBumpSpecAlpha", gb::SurfaceClass::kWorld, true},
    {"xBumpSpecSkinned", gb::SurfaceClass::kWorld, true},
    {"xGrill", gb::SurfaceClass::kWorld, true},
    {"xGrillOpaque", gb::SurfaceClass::kWorld, true},
    {"xUnderbody", gb::SurfaceClass::kWorld, true},
    {"xEngineBay", gb::SurfaceClass::kWorld, true},
    {"xBumperSubframe", gb::SurfaceClass::kWorld, true},
    {"xblackmatte", gb::SurfaceClass::kWorld, true},
    {"xAmbientBlackMatte", gb::SurfaceClass::kWorld, true},
    {"xrage_bump_spec", gb::SurfaceClass::kWorld, true},
    {"xrage_bump_spec_alpha", gb::SurfaceClass::kWorld, true},
    {"xrage_bump_spec_reflect", gb::SurfaceClass::kWorld, true},
    {"xrage_diffuse", gb::SurfaceClass::kWorld, true},
    {"xrage_spec_reflect", gb::SurfaceClass::kWorld, true},
    // the driver (skin, flagged as part of the car), pedestrians and characters
    {"xCharacterSkin", gb::SurfaceClass::kSkin, true},
    {"xPed", gb::SurfaceClass::kSkin, false},
    {"xCharacter", gb::SurfaceClass::kSkin, false},
    {"xCharacter_alpha", gb::SurfaceClass::kSkin, false},
    {"xCharacter_alpha_doublesided", gb::SurfaceClass::kSkin, false},
    {"xCharacter_alpha_normalmap", gb::SurfaceClass::kSkin, false},
    {"xCharacter_alpha_normalmap_doublesided", gb::SurfaceClass::kSkin, false},
    {"xCharacter_normalmap", gb::SurfaceClass::kSkin, false},
    {"xCharacter_skin", gb::SurfaceClass::kSkin, false},
    {"xCharacter_skin_arm", gb::SurfaceClass::kSkin, false},
    {"xCharacter_skin_arm_normalmap", gb::SurfaceClass::kSkin, false},
    {"xCharacter_skin_blendshape", gb::SurfaceClass::kSkin, false},
    {"xCharacter_skin_blendshape_normalmap", gb::SurfaceClass::kSkin, false},
    {"xCharacter_skin_normalmap", gb::SurfaceClass::kSkin, false},
    {"xCharacter_teeth", gb::SurfaceClass::kSkin, false},
    {"xCharacter_teeth_normalmap", gb::SurfaceClass::kSkin, false},
    {"xCharacter_plastic", gb::SurfaceClass::kWorld, false},
    {"xCharacter_plastic_normalmap", gb::SurfaceClass::kWorld, false},
    {"xCharacter_visor", gb::SurfaceClass::kGlass, false},
    {"xCharacter_visor_normalmap", gb::SurfaceClass::kGlass, false},
};

void CheckHostWord() {
  std::set<std::string_view> pinned;
  for (const ClassPin& pin : kPins) Check(pinned.insert(pin.group).second, std::string(pin.group) + " is pinned once");
  std::set<std::string_view> groups_used;
  std::map<int, size_t> by_class;
  for (size_t i = 0; i < sp::kGBufferCarShaderCount; ++i) {
    const sp::GBufferCarShader& row = sp::kGBufferCarShaders[i];
    const std::string who(row.name);
    const gb::ShaderMaterial m = gb::MaterialOfShaderName(row.name);
    Check(m.known && m.cls != gb::SurfaceClass::kNone, who + ": the class table knows its group and writes a class (the host's word has one)");
    const gb::ShaderName split = gb::SplitShaderName(row.name);
    groups_used.insert(split.group);
    const ClassPin* pin = nullptr;
    for (const ClassPin& candidate : kPins) {
      if (candidate.group == split.group) pin = &candidate;
    }
    Check(pin != nullptr, who + ": its group has a pinned class");
    if (pin) {
      Check(m.cls == pin->cls, who + ": the class is the pinned one (the host's word)");
      Check(((m.flags & gb::kMaterialFlagVehicle) != 0) == pin->vehicle, who + ": the vehicle flag is the pinned one");
    }
    Check((m.lit == 0.0f) == (m.cls == gb::SurfaceClass::kEmissive), who + ": the lit weight is 0 for the emissive class only (lamp lenses and displays: the deferred lights add nothing)");
    Check((m.flags & gb::kMaterialFlagSsr) == 0, who + ": a car's pixel is not SSR eligible (the game's own environment reflections stay)");
    ++by_class[int(m.cls)];
    // the word the host sends and what the shader does to it: class, flags and lit survive the smoothness replacement
    const uint32_t word = gb::MaterialWord(m.smoothness, m.cls, m.flags, m.lit);
    const float own = 0.37f;
    const uint32_t replaced = (word & 0xFFFFFF00u) | uint32_t(std::min(std::max(own, 0.0f), 1.0f) * 255.0f + 0.5f);
    Check((replaced >> 8) == (word >> 8) && (replaced & 255u) == 94u, who + ": replacing the smoothness byte keeps the class, the flags and the lit weight");
  }
  for (const ClassPin& pin : kPins) Check(groups_used.contains(pin.group), std::string(pin.group) + " is a group of the table (a pin for nothing)");
  std::printf("host word classes of the 73 rows:");
  for (const auto& [cls, n] : by_class) std::printf(" %d=%zu", cls, n);
  std::printf("\n");
}

// ---- the retail text -------------------------------------------------------------------------------------------------------------------------------------

const char* const kSubfolders[] = {"", "cars", "characters", "city", "effects", "ui"};

// The translated file of a shader: in the folder itself or in one of the subfolders of shaderpack/baseline/hlsl-rpf. Empty when it is in none.
std::filesystem::path Locate(const std::filesystem::path& folder, std::string_view name) {
  for (const char* sub : kSubfolders) {
    const std::filesystem::path file = folder / sub / (std::string(name) + ".hlsl");
    if (std::filesystem::exists(file)) return file;
  }
  return {};
}

// Every shader name (file stem) of the folder and its subfolders.
std::set<std::string> AllStems(const std::filesystem::path& folder) {
  std::set<std::string> stems;
  for (const char* sub : kSubfolders) {
    const std::filesystem::path dir = folder / sub;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.is_regular_file() && entry.path().extension() == ".hlsl") stems.insert(entry.path().stem().string());
    }
  }
  return stems;
}

// The identifiers of the helper (words of letters, digits and underscores), comments left out: a name in a comment expands to nothing.
std::set<std::string> Identifiers(std::string_view text) {
  std::string code;
  for (size_t i = 0; i < text.size();) {
    if (text.substr(i, 2) == "//") {
      while (i < text.size() && text[i] != '\n') ++i;
    } else {
      code += text[i++];
    }
  }
  std::set<std::string> words;
  size_t i = 0;
  while (i < code.size()) {
    if (std::isalpha(static_cast<unsigned char>(code[i])) || code[i] == '_') {
      size_t j = i;
      while (j < code.size() && (std::isalnum(static_cast<unsigned char>(code[j])) || code[j] == '_')) ++j;
      words.insert(code.substr(i, j - i));
      i = j;
    } else {
      ++i;
    }
  }
  return words;
}

// The names the game's shader gives its constants for SPIR-V (`#define smoothness vk::RawBufferLoad<float4>(...)`): a helper parameter of the same name would be macro expanded.
std::set<std::string> GameConstantNames(std::string_view text) {
  std::set<std::string> names;
  constexpr std::string_view kDefine = "\n#define ";
  for (size_t at = text.find(kDefine); at != std::string_view::npos; at = text.find(kDefine, at + 1)) {
    const size_t begin = at + kDefine.size();
    size_t end = begin;
    while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
    if (text.substr(end, 22) == " vk::RawBufferLoad<flo" || text.substr(end, 22) == " vk::RawBufferLoad<uin") names.insert(std::string(text.substr(begin, end - begin)));
  }
  return names;
}

void CheckRetail(const std::filesystem::path& folder, const std::string& helper, const std::filesystem::path& out) {
  size_t seen = 0, edited = 0, captured = 0, found_albedo = 0;
  std::map<std::string, size_t> failures;
  std::map<std::string, std::set<std::string>> collisions;  // helper identifier -> the shaders whose constants have that name
  std::map<std::string, size_t> details;                    // the census of the albedo's reading
  const std::set<std::string> helper_names = Identifiers(helper);
  const std::set<std::string> stems = AllStems(folder);
  for (size_t i = 0; i < sp::kGBufferCarShaderCount; ++i) {
    const sp::GBufferCarShader& row = sp::kGBufferCarShaders[i];
    const std::filesystem::path file = Locate(folder, row.name);
    if (file.empty()) continue;
    ++seen;
    const std::string who(row.name);
    const std::string text = ReadFile(file);
    // the names are full names: no other shader of the game ends with this one (the pack finds its shaders by suffix)
    size_t ends = 0;
    for (const std::string& stem : stems) ends += EndsWith(stem, row.name) ? 1 : 0;
    Check(ends == 1, who + ": the only shader whose name ends with it (the pack matches by suffix): " + std::to_string(ends));
    // what the stage reads from the text
    const sp::GBufferCarEdits edits = sp::PlanGBufferCarEdits(row, text);
    Check(edits.found == (row.albedo != sp::GBufferCarAlbedo::kNone), who + ": the albedo of its row is found in the retail text: " + edits.why_not);
    Check(edits.smoothness == (row.material == sp::GBufferCarMaterial::kBumpSpec), who + ": the shader declares the smoothness its row says it has");
    if (edits.found) {
      ++found_albedo;
      captured += edits.insert_at != std::string::npos ? 1 : 0;
      ++details[std::string(sp::GBufferCarAlbedoSource(row)) + (edits.insert_at != std::string::npos ? ", captured" : ", read at the end of main")];
    }
    for (const uint32_t planes : {1u, 2u, 3u}) {
      const sp::VariantTransformResult r = sp::TransformGBufferCarsHlsl({row.name, 0}, text, helper, sp::SoloContext(sp::kFeatureGBuffer), planes);
      Check(r.ok, who + " at " + std::to_string(planes) + " plane(s): transforms on the retail text (" + r.error + ")");
      if (!r.ok) {
        ++failures[who];
        continue;
      }
      const sp::GBufferCarEdits used = planes >= 2 ? sp::PlanGBufferCarEdits(row, LfOnly(text)) : sp::GBufferCarEdits();
      const std::string back = WithoutCarStage(r.hlsl, helper, row, used, planes);
      Check(back == LfOnly(text), who + " at " + std::to_string(planes) + " plane(s): the game's text is untouched" + FirstDifference(back, LfOnly(text)));
      Check(Count(r.hlsl, "oC0 = applyBlendPremult(oC0);") == 1, who + ": the game's colour statement is there once");
      if (planes == sp::kGBufferCarsPlanes) ++edited;
      if (!out.empty() && (planes == sp::kGBufferCarsPlanes || planes == 3)) {
        std::filesystem::create_directories(out);
        std::ofstream(out / (who + (planes == 3 ? ".gbcars3.hlsl" : ".gbcars.hlsl")), std::ios::binary) << r.hlsl;
      }
    }
    for (const std::string& constant : GameConstantNames(text)) {
      if (helper_names.contains(constant)) collisions[constant].insert(who);
    }
  }
  for (size_t i = 0; i < sp::kGBufferCarLeftOutCount; ++i) {
    // every left-out shader is a retail shader of the folder (a name typed wrong would hide a shader)
    Check(!Locate(folder, sp::kGBufferCarLeftOut[i].name).empty(), std::string(sp::kGBufferCarLeftOut[i].name) + " (left out) is a file of the retail folder");
  }
  std::printf("retail folder %s: %zu table rows found, %zu transformed at the stage's planes, %zu with a failure; albedo found in %zu rows (%zu captured copies):", folder.string().c_str(), seen, edited,
              failures.size(), found_albedo, captured);
  for (const auto& [what, n] : details) std::printf(" [%s: %zu]", what.c_str(), n);
  std::printf("\n");
  // a parameter of the helper that is the name of a constant of a game shader breaks that shader's SPIR-V compile (`#define smoothness vk::RawBufferLoad<float4>(...)` expands the parameter): the helper's
  // GbMaterialWord parameter was `smoothness` in the 13 shaders of the xBumpSpec family; it is `gbSmoothness` now and this must stay empty
  for (const auto& [name, shaders] : collisions) {
    Check(false, "the helper's identifier `" + name + "` is also the name of a game constant in " + std::to_string(shaders.size()) + " of the edited shaders (e.g. " + *shaders.begin() + ")");
  }
  Check(collisions.empty(), "no identifier of the helper is the name of a constant of a car shader");
  if (seen == sp::kGBufferCarShaderCount) {
    // the whole table was in the folder: the helper's names against every shader of the game that the other stages edit too (the SPIR-V compile of any of them would break the same way)
    size_t clashes = 0;
    for (const std::string& stem : stems) {
      if (stem.find("__PS") == std::string::npos) continue;
      const std::filesystem::path file = Locate(folder, stem);
      if (file.empty()) continue;
      for (const std::string& constant : GameConstantNames(ReadFile(file))) clashes += helper_names.contains(constant) ? 1 : 0;
    }
    Check(clashes == 0, "no identifier of the helper is the name of a constant of any pixel shader of the game: " + std::to_string(clashes));
  }
}

// ---- the composition with the car variants that edit the same shaders (the player's paint, the chrome) ---------------------------------------------------------------

void CheckComposed(const std::filesystem::path& folder, const std::string& helper, const std::filesystem::path& out) {
  // the chain's own table (variant_chain.h kFeatures): the G-buffer composes with the two car variants, not with anything else of the car's
  const char* why = nullptr;
  Check(chain::TableIsValid(chain::kFeatures, &why), std::string("the chain's table is valid: ") + (why ? why : ""));
  const uint32_t self = chain::FeatureBit(sp::kFeatureCarSelfRefl), chrome = chain::FeatureBit(sp::kFeatureCarChrome), gbuf = chain::FeatureBit(sp::kFeatureGBuffer);
  Check(chain::CanCompose(self | gbuf) && chain::CanCompose(chrome | gbuf) && !chain::CanCompose(self | chrome) && !chain::CanCompose(self | chrome | gbuf), "the G-buffer composes with each car variant, the two car variants not with each other");
  Check(chain::OffsetOf(self | gbuf, sp::kFeatureGBuffer) == chain::kSharedConstantsBytes + 112u && chain::OffsetOf(chrome | gbuf, sp::kFeatureGBuffer) == chain::kSharedConstantsBytes + 112u &&
            chain::OffsetOf(gbuf, sp::kFeatureGBuffer) == chain::kSharedConstantsBytes,
        "the G-buffer's block follows the car variant's 112 bytes (byte 720), or is at 608 alone");
  // the words of the two shaders: each stage alone and the pair; the pair has no alpha test twin (the car variants have none)
  for (const uint32_t other : {self, chrome}) {
    const chain::PermutationList list = chain::Permutations(other | gbuf, true);
    Check(list.sets.size() == 3 && list.dropped == 0, "the paint and the chrome have three permutations: each stage alone and the pair");
    Check(!chain::HasTwin(other | gbuf) && chain::HasTwin(gbuf), "the pair has no alpha test twin, the G-buffer's stage alone has");
  }
  struct Pair {
    std::string_view name;
    uint32_t other;
    sp::VariantDef def;
  };
  const std::vector<Pair> pairs = {{"xCarPaintCustomizable__PS_CarPaint", sp::kFeatureCarSelfRefl, sp::MakeCarSelfReflVariant()}, {"xChrome__PS_Chrome", sp::kFeatureCarChrome, sp::MakeCarChromeVariant()}};
  for (const Pair& pair : pairs) {
    const std::filesystem::path file = Locate(folder, pair.name);
    if (file.empty()) continue;
    const std::string text = ReadFile(file);
    const std::vector<sp::VariantDef> defs = {pair.def, sp::MakeGBufferCarsStageFromText(helper)};
    const uint32_t set = chain::FeatureBit(pair.other) | gbuf;
    const std::string label = std::string(pair.name) + " + the G-buffer (the car variant first)";
    const sp::VariantShader shader{pair.name, pair.name == "xChrome__PS_Chrome" ? sp::kChrome.key : sp::kCarPaint.key};
    const sp::VariantTransformResult composed = sp::ComposeStages(defs, set, shader, text, chain::kFeatures);
    Check(composed.ok, label + ": composes (" + composed.error + ")");
    if (!composed.ok) continue;
    Check(Count(composed.hlsl, "float2 GbRt1Encode(float3 n)") == 1 && Count(composed.hlsl, "oC1 : SV_Target1") == 1 && Count(composed.hlsl, "oC0 = applyBlendPremult(oC0);") == 1,
          label + ": the G-buffer's text is in it once and the game's colour statement too");
    Check(Count(composed.hlsl, "#define g_GBufferMode vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 720, 0x10)") == 1 && Count(composed.hlsl, "uint4 g_GBufferMode : packoffset(c45);") == 1,
          label + ": the G-buffer's constants are declared once at byte 720 (c45), after the car variant's 112 bytes, in both backends");
    const uint32_t at = chain::OffsetOf(set, sp::kFeatureGBuffer);
    Check(at == chain::kSharedConstantsBytes + 112u, label + ": the G-buffer's block follows the car variant's 112 bytes");
    // the stages alone are the same text as the solo transforms' (the car variant's edits only add to it)
    const sp::VariantTransformResult solo = sp::TransformGBufferCarsHlsl(shader, text, helper);
    Check(solo.ok && !composed.hlsl.empty() && composed.hlsl.size() > solo.hlsl.size(), label + ": the composed text is the solo text plus the car variant's edits");
    if (!out.empty()) {
      std::filesystem::create_directories(out / "composed");
      std::ofstream(out / "composed" / (std::string(pair.name) + ".gbcars.hlsl"), std::ios::binary) << composed.hlsl;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string helper = kHelper;
  bool real_helper = false;
  if (argc > 1) {
    const std::string text = ReadFile(argv[1]);
    if (text.empty()) {
      std::printf("FAILED: the helper file %s is empty or missing\n", argv[1]);
      return 1;
    }
    helper = text;
    real_helper = true;
  }
  CheckFixture();
  CheckTable();
  CheckHostWord();
  if (real_helper) {
    for (const char* needle : {"#define GB_CLASS_PAINT 8", "#define GB_CLASS_CHROME 9", "#define GB_CLASS_METAL 10", "#define GB_CLASS_GLASS 11", "#define GB_CLASS_SKIN 12", "#define GB_CLASS_EMISSIVE 13",
                               "#define GB_CLASS_WORLD 1", "#define GB_FLAG_VEHICLE 4", "#define GB_MODE_WRITE_MATERIAL 4", "#define GB_MODE_FLIP_BACKFACE 8", "uint GbMaterialWord(float gbSmoothness,",
                               "float2 GbMaterialRt3(uint word)", "float GbMaterialLit(uint word)", "float3 GbAlbedoEncode(float3 albedo)"}) {
      Check(Count(helper, needle) == 1, std::string("the real helper has ") + needle);
    }
    Check(Count(helper, "float smoothness") == 0, "the helper has no parameter named smoothness (a game constant's name)");
  }
  if (argc > 2) {
    const std::filesystem::path out = argc > 3 ? argv[3] : std::filesystem::path();
    CheckRetail(argv[2], helper, out);
    CheckComposed(argv[2], helper, out);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

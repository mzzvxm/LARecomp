// Check of the G-buffer's car, traffic and character families (gbuffer_cars.h, the prototype of the GB-G0 spike). A program of its own: it needs no renderer, no ReXGlue, no compiler and
// no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_cars_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_gbuffer_cars_test [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the transformed texts>]]]
//
// Without arguments it runs on a small text made here (the shape of the translator's paint shader the edits look at) and a stand-in for the helper. With the helper file as the first
// argument the real one is pasted. With a folder of translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files in one folder, for example the `cars` and `characters`
// folders of shaderpack/baseline/hlsl-rpf of the development PC copied or linked together, or pass the folder `cars` and run again with `characters`) it runs on every row of the table
// whose file is in that folder: the retail text must transform at 1, 2 and 3 planes, and with the insertions taken out must be the game's text again, byte for byte (the colour path is
// untouched), and the census numbers print. A third argument is a folder that receives the edited texts (`<name>.gbcars.hlsl`), for a compile with dxc (-T ps_6_0 -E main -HV 2021 -spirv
// -fvk-use-dx-layout, and without -spirv for DXIL; with -DXENOS_SPEC_CONSTANTS=0 and =2 for the alpha test twin).

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
#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_compose.h"
#include "shaderpack/variants/car_chrome.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/gbuffer_cars.h"
#include "shaderpack/variants/gbuffer_day.h"
#include "shaderpack/variants/stage_constants.h"

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

// Takes the car stage's insertions out again (the outputs, the constants rows, the helper, the derivative block, the paint's capture, the write) and gives back what is left: it must be
// the game's text, byte for byte, with the line ends of the transform (LF). The colour is then computed by the game's own statements alone.
std::string WithoutCarStage(std::string text, std::string helper, const sp::GBufferCarShader& row, uint32_t planes) {
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
  if (planes >= 2 && row.albedo == sp::GBufferCarAlbedo::kPaintVinyl) cut("the paint's capture", sp::kPaintCapture);
  cut("the sidecar write", sp::GBufferCarsWriteText(row, planes));
  return missing.empty() ? text : "<missing or twice there:" + missing + ">";
}

// ---- the fixtures: the shape of the translator's paint shader the edits look at --------------------------------------------------------------------------------

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
    "\tfloat4 r7 = iTexCoord1;\n"
    "\tCubeMapData cubeMapData = (CubeMapData)0;\n"
    "\tr7.xyz = r7.xyz * r0.yyy + r3.yzx;\n"
    "\toC0.xyz = r7.xyz;\n"
    "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
    "\t}\t\toC0 = applyBlendPremult(oC0);\n"
    "\treturn;\n"
    "}\n";

constexpr const char* kHelper =
    "#define GB_CLASS_PAINT 8\n"
    "#define GB_CLASS_WORLD 1\n"
    "#define GB_FLAG_VEHICLE 4\n"
    "#define GB_MODE_WRITE_NORMAL 1\n"
    "#define GB_MODE_WRITE_ALBEDO 2\n"
    "#define GB_MODE_WRITE_MATERIAL 4\n"
    "float2 GbOctEncode(float3 n) { return n.xy; }\n"
    "float2 GbRt1Encode(float3 n) { return GbOctEncode(n); }\n"
    "float3 GbAlbedoEncode(float3 a) { return a; }\n"
    "uint GbMaterialWord(float s, uint c, uint f, float l) { return c; }\n"
    "float2 GbMaterialRt3(uint w) { return float2(w, w); }\n"
    "float GbMaterialLit(uint w) { return 1.0f; }\n"
    "float3 GbWorldPoint(float4 ray, float4 q, float px, float py, float w) { return ray.xyz; }\n"
    "float3 GbFaceNormal(float3 dpdx, float3 dpdy, float3 to_camera) { return dpdx; }\n";

// The paint row of the fixture: the table's own row of the player's paint.
constexpr std::string_view kPaintName = "xCarPaintCustomizable__PS_CarPaint";

void CheckFixture() {
  const sp::GBufferCarShader* row = sp::FindGBufferCarShader(kPaintName);
  Check(row != nullptr, "the paint is in the table");
  if (!row) return;
  Check(row->normal == sp::GBufferCarNormal::kInterpolant && row->slot == 2 && row->albedo == sp::GBufferCarAlbedo::kPaintVinyl && row->class_name == "GB_CLASS_PAINT" && row->vehicle,
        "the paint row: interpolant 2, the vinyl albedo, class paint, vehicle");
  for (const uint32_t planes : {1u, 2u, 3u}) {
    const std::string label = "the fixture at " + std::to_string(planes) + " plane(s)";
    const sp::VariantShader shader{kPaintName, 0};
    const sp::VariantTransformResult r = sp::TransformGBufferCarsHlsl(shader, kPixelFixture, kHelper, sp::SoloContext(sp::kFeatureGBuffer), planes);
    Check(r.ok && !r.skip && r.error.empty(), label + " transforms: " + r.error);
    if (!r.ok) continue;
    Check(Count(r.hlsl, "oC1 : SV_Target1") == 1 && Count(r.hlsl, "oC2 : SV_Target2") == (planes >= 2 ? 1u : 0u) && Count(r.hlsl, "oC3 : SV_Target3") == (planes >= 3 ? 1u : 0u),
          label + ": as many outputs as planes");
    Check(Count(r.hlsl, "oC1 = GbRt1Encode(iTexCoord2.xyz);") == 1, label + ": the normal is the interpolant's");
    Check(Count(r.hlsl, "float3 gbAlbedo = float3(r7.z, r7.x, r7.y);") == (planes >= 2 ? 1u : 0u), label + ": the paint's capture is there with the albedo plane only");
    Check(Count(r.hlsl, "oC2 = float4(GbAlbedoEncode(gbAlbedo), GbMaterialLit(gbMat));") == (planes >= 2 ? 1u : 0u), label + ": the albedo write");
    Check(Count(r.hlsl, "oC3 = GbMaterialRt3(gbMat);") == (planes >= 3 ? 1u : 0u), label + ": the material write");
    Check(Count(r.hlsl, "GbMaterialWord(0.85f, uint(GB_CLASS_PAINT), uint(GB_FLAG_VEHICLE), 1.0f)") == (planes >= 2 ? 1u : 0u), label + ": the paint's own material word is the fallback");
    // the capture is after the game's statement and before the write
    const size_t anchor = r.hlsl.find(std::string(sp::kPaintAnchor));
    const size_t capture = r.hlsl.find("float3 gbAlbedo = ");
    const size_t write = r.hlsl.find("oC0 = applyBlendPremult(oC0);");
    Check(planes < 2 || (anchor != std::string::npos && capture > anchor && write > capture), label + ": the capture follows the game's statement and the write follows it");
    // the colour path: the insertions out leave the original
    const std::string back = WithoutCarStage(r.hlsl, kHelper, *row, planes);
    Check(back == LfOnly(kPixelFixture), label + ": the game's text is untouched" + FirstDifference(back, LfOnly(kPixelFixture)));
  }
  // a shader that is not in the table is skipped, a text that is not the translator's fails with a name
  const sp::VariantShader other{"xCityDefault__PSCityDefault", 0};
  const sp::VariantTransformResult skipped = sp::TransformGBufferCarsHlsl(other, kPixelFixture, kHelper);
  Check(!skipped.ok && skipped.skip, "a city shader is skipped");
  std::string broken = kPixelFixture;
  broken.replace(broken.find("r7.xyz = r7.xyz * r0.yyy + r3.yzx;"), 34, "r7.xyz = r7.xyz;");
  const sp::VariantTransformResult no_anchor = sp::TransformGBufferCarsHlsl({kPaintName, 0}, broken, kHelper);
  Check(!no_anchor.ok && !no_anchor.skip && no_anchor.error.find("paint's anchor") != std::string::npos, "without the paint's statement it fails and names the anchor");
  std::string no_decl = kPixelFixture;
  no_decl.replace(no_decl.find("in centroid float4 iTexCoord2 : TEXCOORD2"), 41, "in centroid float4 iTexCoordX : TEXCOORDX");
  const sp::VariantTransformResult no_slot = sp::TransformGBufferCarsHlsl({kPaintName, 0}, no_decl, kHelper);
  Check(!no_slot.ok && !no_slot.skip && no_slot.error.find("iTexCoord2") != std::string::npos, "without the interpolant's declaration it fails and names it");
  const sp::VariantTransformResult bad_planes = sp::TransformGBufferCarsHlsl({kPaintName, 0}, kPixelFixture, kHelper, sp::SoloContext(sp::kFeatureGBuffer), 4);
  Check(!bad_planes.ok && !bad_planes.skip, "four planes do not exist");

  // a derivative row: the same text with the derivative block and no interpolant
  const sp::GBufferCarShader* deriv = sp::FindGBufferCarShader("xblackmatte__PS_BlackMatte");
  Check(deriv && deriv->normal == sp::GBufferCarNormal::kDerivative, "the black matte is a derivative row");
  if (deriv) {
    const sp::VariantTransformResult r = sp::TransformGBufferCarsHlsl({deriv->name, 0}, kPixelFixture, kHelper);
    Check(r.ok && Count(r.hlsl, "float3 gbP = GbWorldPoint(") == 1 && Count(r.hlsl, "GbFaceNormal(gbDx, gbDy, -gbP)") == 1, "a derivative row transforms with the derivative block");
    const std::string back = WithoutCarStage(r.hlsl, kHelper, *deriv, sp::kGBufferCarsDefaultPlanes);
    Check(back == LfOnly(kPixelFixture), "a derivative row leaves the game's text untouched" + FirstDifference(back, LfOnly(kPixelFixture)));
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
  size_t interpolants = 0, derivatives = 0, paint_albedo = 0;
  std::map<std::string_view, size_t> by_family;
  std::map<std::string_view, size_t> by_plan;
  std::map<uint32_t, size_t> by_slot;
  for (size_t i = 0; i < sp::kGBufferCarShaderCount; ++i) {
    const sp::GBufferCarShader& row = sp::kGBufferCarShaders[i];
    Check(names.insert(row.name).second, std::string(row.name) + " is in the table once");
    Check(row.name.find("__") != std::string_view::npos, std::string(row.name) + " is named group__entry");
    Check(row.slot < 16, std::string(row.name) + ": the slot is an interpolant");
    Check(row.class_name.starts_with("GB_CLASS_"), std::string(row.name) + ": the class is a helper name");
    Check(!EndsWith(row.name, "__PSMultiLight") && !EndsWith(row.name, "__PSMultiLightEnv") && !EndsWith(row.name, "__PSMultiLightInterior"),
          std::string(row.name) + " is not a night shader (GB-C's stage owns those)");
    Check(!sp::IsGBufferDayShader(row.name), std::string(row.name) + " is not a day shader (GB-D's table)");
    Check(!AssignsGameRegister(sp::GBufferCarsWriteText(row, 3)) && !AssignsGameRegister(sp::GBufferCarsOutputText(3)) && !AssignsGameRegister(sp::kPaintCapture),
          std::string(row.name) + ": no inserted statement assigns a register of the game's");
    Check(sp::kPlanDiffuse == row.plan || sp::kPlanDiffuseBase == row.plan || sp::kPlanPaint == row.plan || sp::kPlanConstant == row.plan || sp::kPlanNone == row.plan,
          std::string(row.name) + ": the plan is one of the five");
    (row.normal == sp::GBufferCarNormal::kInterpolant ? interpolants : derivatives)++;
    if (row.normal == sp::GBufferCarNormal::kInterpolant) ++by_slot[row.slot];
    paint_albedo += row.albedo == sp::GBufferCarAlbedo::kPaintVinyl ? 1 : 0;
    ++by_family[row.family];
    ++by_plan[row.plan];
  }
  for (size_t i = 0; i < sp::kGBufferCarLeftOutCount; ++i) {
    const sp::GBufferCarLeftOut& row = sp::kGBufferCarLeftOut[i];
    Check(!row.reason.empty(), std::string(row.name) + " is left out with a reason");
    Check(!names.contains(row.name), std::string(row.name) + " is not in both lists");
  }
  Check(interpolants == 65 && derivatives == 8 && sp::kGBufferCarShaderCount == 73, "the table has 65 exported normals and 8 derivative rows (73)");
  Check(sp::kGBufferCarLeftOutCount == 19, "19 shaders are left out");
  Check(sp::kGBufferCarShaderCount + sp::kGBufferCarLeftOutCount == 92, "92 lit pixel shaders in all: 64 of the cars, 8 of the traffic, 20 of the characters");
  Check(paint_albedo == 1, "the prototype edits the albedo of one shader, the player's paint");
  Check(by_family["player/racer"] == 48 && by_family["traffic"] == 6 && by_family["ped/character"] == 19, "census by family: 48 cars, 6 traffic, 19 characters edited");
  std::printf("census: %zu edited (%zu interpolant, %zu derivative), %zu left out; by slot:", sp::kGBufferCarShaderCount, interpolants, derivatives, sp::kGBufferCarLeftOutCount);
  for (const auto& [slot, n] : by_slot) std::printf(" T%u=%zu", slot, n);
  std::printf("; by albedo plan:");
  for (const auto& [plan, n] : by_plan) std::printf(" '%s'=%zu", std::string(plan).c_str(), n);
  std::printf("\n");
  // the planes the stage declares: the day stage's name for the derivative block is the one the car stage shares
  Check(!sp::GBufferDayDerivativeText().empty(), "GB-D's derivative block is there to share");
  // the pack's names: a shader name of the stage is one the VariantDef finds
  const sp::VariantDef def = sp::MakeGBufferCarsStageFromText(kHelper);
  Check(def.feature == sp::kFeatureGBuffer && def.pixel_name_suffixes.size() == sp::kGBufferCarShaderCount && def.needs_rpf, "the stage's definition lists the table's names, in the RPF");
}

// ---- the retail text -------------------------------------------------------------------------------------------------------------------------------------

// The translated file of a shader: in the folder itself, or in its `cars` or `characters` subfolder (shaderpack/baseline/hlsl-rpf has both). Empty when it is in none.
std::filesystem::path Locate(const std::filesystem::path& folder, std::string_view name) {
  for (const char* sub : {"", "cars", "characters"}) {
    const std::filesystem::path file = folder / sub / (std::string(name) + ".hlsl");
    if (std::filesystem::exists(file)) return file;
  }
  return {};
}

// The identifiers of the helper (words of letters, digits and underscores).
std::set<std::string> Identifiers(std::string_view text) {
  std::set<std::string> words;
  size_t i = 0;
  while (i < text.size()) {
    if (std::isalpha(static_cast<unsigned char>(text[i])) || text[i] == '_') {
      size_t j = i;
      while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) ++j;
      words.insert(std::string(text.substr(i, j - i)));
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
  size_t seen = 0, edited = 0;
  std::map<std::string, size_t> failures;
  std::map<std::string, std::set<std::string>> collisions;  // helper identifier -> the shaders whose constants have that name
  const std::set<std::string> helper_names = Identifiers(helper);
  for (size_t i = 0; i < sp::kGBufferCarShaderCount; ++i) {
    const sp::GBufferCarShader& row = sp::kGBufferCarShaders[i];
    const std::filesystem::path file = Locate(folder, row.name);
    if (file.empty()) continue;
    ++seen;
    const std::string text = ReadFile(file);
    for (const uint32_t planes : {1u, 2u, 3u}) {
      const sp::VariantTransformResult r = sp::TransformGBufferCarsHlsl({row.name, 0}, text, helper, sp::SoloContext(sp::kFeatureGBuffer), planes);
      Check(r.ok, std::string(row.name) + " at " + std::to_string(planes) + " plane(s): transforms on the retail text (" + r.error + ")");
      if (!r.ok) {
        ++failures[std::string(row.name)];
        continue;
      }
      const std::string back = WithoutCarStage(r.hlsl, helper, row, planes);
      Check(back == LfOnly(text), std::string(row.name) + " at " + std::to_string(planes) + " plane(s): the game's text is untouched" + FirstDifference(back, LfOnly(text)));
      if (planes == sp::kGBufferCarsDefaultPlanes) {
        ++edited;
        for (const std::string& constant : GameConstantNames(text)) {
          if (helper_names.contains(constant)) collisions[constant].insert(std::string(row.name));
        }
        // the edited text never assigns a register of the game's: every added line names gb, oC1..3 or the mode block
        Check(Count(r.hlsl, "oC0 = applyBlendPremult(oC0);") == 1, std::string(row.name) + ": the game's colour statement is there once");
        if (!out.empty()) {
          std::filesystem::create_directories(out);
          std::ofstream(out / (std::string(row.name) + ".gbcars.hlsl"), std::ios::binary) << r.hlsl;
        }
      }
    }
  }
  for (size_t i = 0; i < sp::kGBufferCarLeftOutCount; ++i) {
    // every left-out shader is a retail shader of the folder (a name typed wrong would hide a shader): found when the folder is the translator's
    Check(!Locate(folder, sp::kGBufferCarLeftOut[i].name).empty(), std::string(sp::kGBufferCarLeftOut[i].name) + " (left out) is a file of the retail folder");
  }
  std::printf("retail folder %s: %zu table rows found, %zu transformed at the full plane count, %zu with a failure\n", folder.string().c_str(), seen, edited, failures.size());
  // KNOWN DEFECT of the frozen helper, reported and not failed (the spike does not edit the interface): a parameter of the helper that is the name of a constant of a game shader breaks that
  // shader's SPIR-V compile (`#define smoothness vk::RawBufferLoad<float4>(...)` expands the parameter). GbMaterialWord's `smoothness` is one in the 13 shaders of the xBumpSpec family.
  for (const auto& [name, shaders] : collisions) {
    std::printf("CHECK (needs: the helper's interface): the helper's identifier `%s` is also the name of a game constant in %zu of the edited shaders (e.g. %s): rename it in gbuffer_common.hlsli\n",
                name.c_str(), shaders.size(), shaders.begin()->c_str());
  }
}

// ---- the composition with the car variants that edit the same shaders (the player's paint, the chrome) ---------------------------------------------------------------

void CheckComposed(const std::filesystem::path& folder, const std::string& helper, const std::filesystem::path& out) {
  // a local table: the car's two variants and the G-buffer's, as variant_chain.h's kFeatures would list them once GB-G1 adds the pairs (the real table lists the G-buffer with the headlight only)
  chain::FeatureInfo selfrefl = *chain::Find(sp::kFeatureCarSelfRefl);
  chain::FeatureInfo carchrome = *chain::Find(sp::kFeatureCarChrome);
  chain::FeatureInfo gbuffer = *chain::Find(sp::kFeatureGBuffer);
  selfrefl.composes_with = chain::FeatureBit(sp::kFeatureGBuffer);
  carchrome.composes_with = chain::FeatureBit(sp::kFeatureGBuffer);
  gbuffer.composes_with = chain::FeatureBit(sp::kFeatureCarSelfRefl) | chain::FeatureBit(sp::kFeatureCarChrome);
  const std::vector<chain::FeatureInfo> table = {selfrefl, carchrome, gbuffer};
  const char* why = nullptr;
  Check(chain::TableIsValid(table, &why), std::string("the local table is valid: ") + (why ? why : ""));
  struct Pair {
    std::string_view name;
    uint32_t other;
    sp::VariantDef def;
  };
  const std::vector<Pair> pairs = {{"xCarPaintCustomizable__PS_CarPaint", sp::kFeatureCarSelfRefl, sp::MakeCarSelfReflVariant()},
                                   {"xChrome__PS_Chrome", sp::kFeatureCarChrome, sp::MakeCarChromeVariant()}};
  for (const Pair& pair : pairs) {
    const std::filesystem::path file = Locate(folder, pair.name);
    if (file.empty()) continue;
    const std::string text = ReadFile(file);
    const std::vector<sp::VariantDef> defs = {pair.def, sp::MakeGBufferCarsStageFromText(helper)};
    const uint32_t set = chain::FeatureBit(pair.other) | chain::FeatureBit(sp::kFeatureGBuffer);
    const std::string label = std::string(pair.name) + " + the G-buffer (the car variant first)";
    const sp::VariantShader shader{pair.name, pair.name == "xChrome__PS_Chrome" ? sp::kChrome.key : sp::kCarPaint.key};
    const sp::VariantTransformResult composed = sp::ComposeStages(defs, set, shader, text, table);
    Check(composed.ok, label + ": composes (" + composed.error + ")");
    if (!composed.ok) continue;
    Check(Count(composed.hlsl, "float2 GbRt1Encode(float3 n)") == 1 && Count(composed.hlsl, "oC1 : SV_Target1") == 1 && Count(composed.hlsl, "oC0 = applyBlendPremult(oC0);") == 1,
          label + ": the G-buffer's text is in it once and the game's colour statement too");
    const uint32_t at = chain::OffsetOf(set, sp::kFeatureGBuffer, table);
    Check(at == chain::kSharedConstantsBytes + 112u, label + ": the G-buffer's block follows the car variant's 112 bytes");
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
  if (real_helper) {
    for (const char* needle : {"#define GB_CLASS_PAINT 8", "#define GB_CLASS_CHROME 9", "#define GB_CLASS_METAL 10", "#define GB_CLASS_GLASS 11", "#define GB_CLASS_SKIN 12", "#define GB_CLASS_EMISSIVE 13",
                               "#define GB_CLASS_WORLD 1", "#define GB_FLAG_VEHICLE 4", "uint GbMaterialWord(", "float2 GbMaterialRt3(uint word)", "float GbMaterialLit(uint word)",
                               "float3 GbAlbedoEncode(float3 albedo)"}) {
      Check(Count(helper, needle) == 1, std::string("the real helper has ") + needle);
    }
    for (size_t i = 0; i < sp::kGBufferCarShaderCount; ++i) {
      Check(Count(helper, "#define " + std::string(sp::kGBufferCarShaders[i].class_name) + " ") == 1, std::string(sp::kGBufferCarShaders[i].name) + ": its class is defined in the helper");
    }
  }
  if (argc > 2) {
    const std::filesystem::path out = argc > 3 ? argv[3] : std::filesystem::path();
    CheckRetail(argv[2], helper, out);
    CheckComposed(argv[2], helper, out);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// Check of the car self-reflection variant's text transform (car_selfrefl.h). A program of its own: it needs no
// renderer, no ReXGlue, no compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_selfrefl_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_car_selfrefl_test [<folder of translated HLSL> [<folder for the result>]]
//
// Without arguments it runs on a small text made here (the lines the anchors look at and some that must stay as they are).
// With a folder of translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files, for example
// shaderpack/baseline/hlsl-rpf/cars of the development PC) it also runs on xCarPaintCustomizable__PS_CarPaint.hlsl found
// in it and, with a second folder, writes the edited text there for a compile with dxc (-T ps_6_0 -E main -HV 2021
// -DXENOS_SPEC_CONSTANTS=0 -all-resources-bound -spirv -fvk-use-dx-layout, and without -spirv for DXIL).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

#include "shaderpack/variants/car_selfrefl.h"

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
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// The parts of the translated paint the transform looks at, in the shape the translator writes them (the declarations
// of both backends, the macro that ends without a backslash, main's start and the three lines), and some other lines.
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
         "\tr0.y = tfetch2D(FastShadowCollectorSampler_Texture2DDescriptorIndex, FastShadowCollectorSampler_SamplerDescriptorIndex, r1.zx, float2(0, 0)).x;\n"
         "\tr10.xyz = tfetch2DGrad(dpFrontSampler_Texture2DDescriptorIndex, dpFrontSampler_SamplerDescriptorIndex, r1.yw, float2(0, 0), tfetchGradientH.xy, tfetchGradientV.xy).xyz;\n"
         "\tr9.xyzw = tfetch2D(VinylSampler_Texture2DDescriptorIndex, VinylSampler_SamplerDescriptorIndex, r5.xy, float2(0, 0)).xyzw;\n"
         "\tr13.xyz = tfetch2D(dpSpecFrontSampler_Texture2DDescriptorIndex, dpSpecFrontSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyz;\n"
         "\toC0.xyz = r3.www * -r0.xzy + r0.xzy;\n"
         "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
         "\t}\n"
         "}\n";
}

void RunOnFixture() {
  const sp::VariantTransformResult result = sp::TransformCarSelfReflHlsl(sp::kCarPaint, Fixture());
  Check(result.ok, "the fixture transforms: " + result.error);
  if (!result.ok) return;
  const std::string& text = result.hlsl;
  Check(Count(text, "CarReflTrace(iTexCoord1.xyz, iTexCoord2.xyz, iTexCoord6.xyz)") == 1,
        "one trace call, with no pixel position (the points of the ray have no per-pixel noise)");
  Check(Count(text, "CarReflApply(r10.xyz, carRefl, 1.0)") == 1, "the sharp read is applied once at full weight");
  Check(Count(text, "CarReflApply(r13.xyz, carRefl, 0.5)") == 1, "the blurred read is applied once at half weight");
  Check(Count(text, "CarReflDebug(carRefl, iTexCoord4.w)") == 1, "one debug view");
  Check(Contains(text, "#define g_CarReflM0 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)"),
        "the SPIR-V declaration of the first matrix row");
  Check(Contains(text, "#define g_CarReflC vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 704, 0x10)"),
        "the SPIR-V declaration of the slots");
  Check(Contains(text, "float2 g_InvResolutionScale : packoffset(c37.x); \\\n    float4 g_CarReflM0 : packoffset(c38);"),
        "the DXIL declarations continue the macro");
  Check(Contains(text, "uint4 g_CarReflC : packoffset(c44);\n"), "the macro ends after the last declaration, with no backslash");
  Check(Count(text, "struct CarReflResult") == 1, "the helper's type once");
  Check(text.find("struct CarReflResult") < text.find("void main("), "the helper is before main");
  Check(text.find("CarReflResult carRefl = (CarReflResult)0;") > text.find("CubeMapData cubeMapData"),
        "the local result follows the cube map data");
  // The ray of version 3 (src/gpu/car_selfrefl_rules.h has the same arithmetic, which car_selfrefl_rules_test.cpp checks): the
  // numbers in the helper are the ones of that header.
  Check(sp::kCarSelfReflVariantVersion == 3, "the variant's version is 3 (the stamp rebuilds the pack's paint)");
  Check(Count(text, "CarReflNoise") == 0 && Count(text, "g_CarReflB.w") == 0,
        "no per-pixel noise: the trace does not read the noise frame");
  Check(Count(text, "[unroll]") == 0 && Count(text, "[loop]") == 1, "one loop and no bisection after it: the heaviest point is the hit");
  Check(Count(text, "CarReflDistance(i, steps, rayLength)") == 1, "the points come from the one distance function, the same for every pixel");
  Check(Count(text, "float CarReflWindow(") == 1 && Count(text, "float CarReflApproach(") == 1 && Count(text, "float CarReflThin(") == 1 &&
            Count(text, "float CarReflSlack(") == 1 && Count(text, "float3 CarReflSeen(") == 1,
        "the helpers of the weights are there once each");
  Check(Contains(text, "static const float kCarReflStartMin = 0.04;") && Contains(text, "static const float kCarReflDark = 0.85;") &&
            Contains(text, "static const float kCarReflBackFloor = 0.5;") && Contains(text, "static const float kCarReflThinFloor = 0.6;") &&
            Contains(text, "static const float kCarReflShade = 0.4;"),
        "the numbers are those of car_selfrefl_rules.h (start 0.04 m, dark 0.85, hidden face 0.5, thin part 0.6, shade 0.4)");
  Check(Contains(text, "max(rayLength * u * sqrt(u), kCarReflStartMin)"), "the points' distance grows as u^1.5");
  Check(Contains(text, "lerp(kCarReflBackFloor, 1.0, face)") && Contains(text, "CarReflThin(sum)") &&
            Contains(text, "lerp(hit.own * kCarReflShade, hit.colour, hit.face)"),
        "a hidden-face or thin hit takes less weight, and mode 2 shows the paint's own colour in shade for the hidden face");
  Check(Contains(text, "env * (1.0 - kCarReflDark * weight)") && Contains(text, "return lerp(env, CarReflSeen(hit), weight);"),
        "the two modes: the occlusion scaled by the weight, the reflection of what the ray sees");
  Check(Count(text, "history.Load(") == 1 && Count(text, "history.Sample") == 0 && Count(text, "Gather") == 0,
        "the history is only read as points");
  // What must stay: the other fetches and the alpha test line.
  Check(Contains(text, "r0.y = tfetch2D(FastShadowCollectorSampler_"), "the shadow read is untouched");
  Check(Contains(text, "r9.xyzw = tfetch2D(VinylSampler_"), "the vinyl read is untouched");
  Check(Contains(text, "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"), "the alpha test line is untouched");
}

void RunNegatives() {
  // A text with an anchor missing or doubled is refused, with the anchor named.
  std::string text = Fixture();
  const std::string needle = "oC0.xyz = r3.www * -r0.xzy + r0.xzy;";
  std::string missing = text;
  missing.replace(missing.find(needle), needle.size(), "oC0.xyz = r0.xzy;");
  sp::VariantTransformResult result = sp::TransformCarSelfReflHlsl(sp::kCarPaint, missing);
  Check(!result.ok && Contains(result.error, "last write"), "a missing final write is refused and named: " + result.error);

  std::string doubled = text;
  const size_t at = doubled.find("\tr13.xyz = tfetch2D(dpSpecFront");
  doubled.insert(at, doubled.substr(at, doubled.find('\n', at) - at + 1));
  result = sp::TransformCarSelfReflHlsl(sp::kCarPaint, doubled);
  Check(!result.ok && Contains(result.error, "blurred environment read"), "a doubled blurred read is refused: " + result.error);

  std::string noMain = text;
  noMain.replace(noMain.find("void main("), 10, "void other(");
  result = sp::TransformCarSelfReflHlsl(sp::kCarPaint, noMain);
  Check(!result.ok && Contains(result.error, "main"), "a text without main is refused: " + result.error);

  const sp::VariantShader other = {"xCarPaint__PS_CarPaint", 0x1ull};
  result = sp::TransformCarSelfReflHlsl(other, Fixture());
  Check(!result.ok, "another shader is refused");

  // The definition of the variant.
  const sp::VariantDef def = sp::MakeCarSelfReflVariant();
  Check(def.id == sp::kVariantCarSelfRefl && def.id == 7 && def.id < sp::kVariantSlots, "the variant id is 7, inside the slots");
  Check(def.name == "car_selfrefl" && def.needs_rpf && def.pixel_shaders.size() == 1, "the definition");
  Check(def.pixel_shaders[0].key == 0x439B193A3F675533ull, "the identity of the paint");
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
    if (entry.is_regular_file() && entry.path().filename() == "xCarPaintCustomizable__PS_CarPaint.hlsl") {
      found = entry.path();
      break;
    }
  }
  if (found.empty()) {
    std::printf("no xCarPaintCustomizable__PS_CarPaint.hlsl under %s\n", folder.string().c_str());
    ++g_failed;
    return;
  }
  std::string text;
  Check(ReadFile(found, &text), "read " + found.string());
  const sp::VariantTransformResult result = sp::TransformCarSelfReflHlsl(sp::kCarPaint, text);
  Check(result.ok, "the retail paint transforms: " + result.error);
  if (!result.ok) return;
  Check(result.hlsl.size() > text.size(), "the edited text is longer than the game's");
  if (!results.empty()) {
    std::filesystem::create_directories(results, ec);
    std::ofstream out(results / "xCarPaintCustomizable__PS_CarPaint.car_selfrefl.hlsl", std::ios::binary);
    out << result.hlsl;
    std::printf("wrote %s (%zu bytes, the game's %zu)\n",
                (results / "xCarPaintCustomizable__PS_CarPaint.car_selfrefl.hlsl").string().c_str(), result.hlsl.size(),
                text.size());
  }
}

}  // namespace

int main(int argc, char** argv) {
  RunOnFixture();
  RunNegatives();
  if (argc > 1) {
    RunOnTree(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

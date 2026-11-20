// Check of the car chrome variant's text transform (car_chrome.h). A program of its own: it needs no renderer, no ReXGlue, no compiler
// and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_chrome_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_car_chrome_test [<folder of translated HLSL> [<folder for the result>]]
//
// Without arguments it runs on a small text made here (the lines the anchors look at and some that must stay as they are). With a folder of
// translated HLSL (the builder's own output: `<group>__<entry>.hlsl` files, for example the cars folder of the development PC) it also runs
// on xChrome__PS_Chrome.hlsl found in it and, with a second folder, writes the edited text there for a compile with dxc (-T ps_6_0 -E main
// -HV 2021 -DXENOS_SPEC_CONSTANTS=0 -all-resources-bound -spirv -fvk-use-dx-layout, and without -spirv for DXIL; the game's text is never
// committed, only read).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variant_overlap.h"
#include "shaderpack/variants/car_chrome.h"
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

// The parts of the translated chrome the transform looks at, in the shape the translator writes them (the declarations of both backends,
// the macro that ends without a backslash, main's start and the three lines), and some other lines.
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

void RunOnFixture() {
  const sp::VariantTransformResult result = sp::TransformCarChromeHlsl(sp::kChrome, Fixture());
  Check(result.ok, "the fixture transforms: " + result.error);
  if (!result.ok) return;
  const std::string& text = result.hlsl;
  Check(Count(text, "carChrome = CarChrSetup(iTexCoord0.xyz, iTexCoord1.xyz, gViewInverse(3).xyz - iTexCoord0.xyz);") == 1,
        "one setup, from the world position, the world normal and the vector to the camera the shader itself uses");
  Check(text.find("tfetch2DGrad(dpFrontSampler") < text.find("CarChrSetup(iTexCoord0") &&
            text.find("CarChrSetup(iTexCoord0") < text.find("CarChromeEnv(r1.xyz, carChrome)"),
        "the setup and the occlusion of `env` follow the environment read");
  Check(Contains(text, "r0.xyz = -r1.xyz + CarChromeAmbient(ambYPos.xyz, carChrome, saturate(r2.y + r2.z));") &&
            !Contains(text, "-r1.xyz + ambYPos.xyz"),
        "the ambient line takes the sky probe and the game's own blend (r2.y and r2.z are set on the line before it)");
  Check(Count(text, "CarChromeDebug(carChrome, iColor0.w)") == 1, "one debug view, with the vertex colour's alpha");
  Check(text.find("oC0.xyz = r0.www * r0.xyz;") < text.find("CarChromeDebug(carChrome"), "the debug view follows the last write of the colour");
  Check(Contains(text, "r0.xyz = r1.www * r0.xyz + r1.xyz;"), "the game's mix of env and ambient is untouched: its blend, AO and sun stay the game's");
  Check(Contains(text, "#define g_CarReflM0 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)"),
        "the SPIR-V declaration of the first matrix row (the paint's constants)");
  Check(Contains(text, "#define g_CarReflC vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 704, 0x10)"),
        "the SPIR-V declaration of the slots");
  Check(Contains(text, "float2 g_InvResolutionScale : packoffset(c37.x); \\\n    float4 g_CarReflM0 : packoffset(c38);"),
        "the DXIL declarations continue the macro");
  Check(Contains(text, "uint4 g_CarReflC : packoffset(c44);\n"), "the macro ends after the last declaration, with no backslash");
  Check(Count(text, "struct CarChrState") == 1 && text.find("struct CarChrState") < text.find("void main("), "the state type once, before main");
  Check(text.find("CarChrState carChrome = (CarChrState)0;") > text.find("CubeMapData cubeMapData"), "the local state follows the cube map data");
  // The numbers of car_chrome_rules.h (checked by src/gpu/car_chrome_rules_test.cpp): the shader's copy holds each.
  Check(Contains(text, "static const float kCarChrReflectLength = 0.25;") && Contains(text, "static const float kCarChrSkyLength = 0.2;") &&
            Contains(text, "static const int kCarChrReflectSteps = 12;") && Contains(text, "static const int kCarChrSkySteps = 8;") &&
            Contains(text, "static const float kCarChrStartMin = 0.008;") && Contains(text, "static const float kCarChrDark = 0.9;") &&
            Contains(text, "static const float kCarChrFull = 0.25;") && Contains(text, "static const float kCarChrBackFloor = 0.5;") &&
            Contains(text, "static const float kCarChrFadeFrom = 0.3;") && Contains(text, "static const float kCarChrFadeTo = 0.9;") &&
            Contains(text, "static const float kCarChrApproachSpan = 0.04;") && Contains(text, "static const float kCarChrNormalMin = 0.03;") &&
            Contains(text, "static const int kCarChrOutlineSteps = 3;"),
        "the numbers are those of car_chrome_rules.h (reflection 0.25 m x 12, sky 0.2 m x 8, start 8 mm, dark 0.9, full 0.25, hidden face 0.5, fade 0.3 to "
        "0.9, approach span 4 cm, N.V 0.03, 3 halvings)");
  Check(Contains(text, "max(rayLength * u * sqrt(u), kCarChrStartMin)"), "the points' distance grows as u^1.5");
  Check(Contains(text, "return 0.08 + 0.006 * depth;") && Contains(text, "min(0.006 + 0.0015 * depth, 0.25 * thickness)") &&
            Contains(text, "min(0.004 + 0.0015 * depth + slack, 0.5 * thickness - ramp)") && Contains(text, "return min(1.5 * texel * slope, 0.03);"),
        "the thickness, the window and the slack are the rules' (12 cm at 6.7 m, a bias of 14 mm, a slack capped at 3 cm)");
  Check(Contains(text, "const float tolerance = 0.015 + 0.005 * start.w + s.ray.slack;") &&
            Contains(text, "return (stored > 0.0) ? 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(stored - depth)) : 0.0;") &&
            Contains(text, "CarChrTexelAgreement(own.a, start.w, tolerance)"),
        "the self-test's tolerance is the rules' (a few centimetres, not the paint's 6 cm + 2 percent)");
  // Version 3: the rim of a tip. The twelve offsets are car_chrome_rules.h's kSilhouetteOffsets, in its order; only a pixel that does not agree fully reads them.
  Check(Contains(text, "static const int kCarChrSilhouetteTaps = 12;") &&
            Contains(text, "static const int2 kCarChrSilhouetteOffsets[12] = {int2(-1, 0), int2(0, -1), int2(1, 0), int2(0, 1), int2(-1, -1), int2(1, -1),\n"
                            "                                                  int2(-1, 1), int2(1, 1), int2(-2, 0), int2(0, -2), int2(2, 0), int2(0, 2)};"),
        "version 3: the self-test's twelve neighbour offsets are the rules' (the eight round the texel, then two steps along each axis)");
  Check(Contains(text, "float CarChrSilhouetteAgreement(") && Contains(text, "if (own < 1.0)") && Contains(text, "best = max(best, CarChrTexelAgreement(stored, depth, tolerance));") &&
            Contains(text, "s.agreement = CarChrSilhouetteAgreement(history, s.ray.size, s.ray.startUv, start.w, tolerance,") &&
            Contains(text, "if (s.agreement <= 0.0)\n    {\n        return s;"),
        "version 3: the setup takes the best agreement of the own texel and its neighbours, searched only where the own texel is not fully in, and returns where it is 0");
  Check(!Contains(text, "abs(own.a - start.w)") && !Contains(text, "!(own.a > 0.0)"), "version 3: the old test of the own texel alone is gone");
  Check(Contains(text, "sky = (skyLength < 0.2) ? float3(0.0, 1.0, 0.0) : sky / skyLength;"), "the sky probe's direction is the rules' (n + up, or up)");
  Check(Contains(text, "env * (1.0 - kCarChrDark * saturate(s.reflectHit * g_CarReflA.w))") &&
            Contains(text, "ambient * (1.0 - kCarChrDark * saturate(s.skyHit * g_CarReflA.w))"),
        "the two terms are darkened by the weight, the strength and the dark floor, nothing is replaced");
  Check(Contains(text, "if ((slots.w & 1u) != 0u)") && Contains(text, "CarChrRtRead(history, s.ray.size, s.ray.startUv, start.w,") &&
            Contains(text, "s.reflectHit = rt.y * s.agreement;"),
        "the ray traced image's hook: read where the frame has one, with the march left out");
  // Version 2: another car's chrome. Two kinds of texel, told apart by the sign of the colour's red; the player's arithmetic is the older one's.
  Check(Contains(text, "float4 CarChrRtRead(") && Contains(text, "const float4 t = image.Load(int3(texel, 0));") && Contains(text, "if (t.r < -0.5)") &&
            Contains(text, "skyOther += wg * saturate(-t.r - 1.0);") && Contains(text, "knownOther += wg;") && Contains(text, "weightOther += wg * saturate(code - 1.0);"),
        "version 2: the image's texels are of two kinds, told apart by a red under -0.5 (the sky weight is -(1 + w) there)");
  Check(Contains(text, "known += wg;\n                weight += wg * saturate(code - 1.0);") &&
            Contains(text, "if (geometry > 1e-4 && known >= 0.5 * geometry)\n    {\n        return float4(1.0, weight / known, 0.0, 0.0);"),
        "the player's texels are read as before: the same sums, the same half of the footprint, the same weight (the answer is first)");
  Check(Contains(text, "if (geometry > 1e-4 && knownOther >= 0.5 * geometry)") &&
            Contains(text, "return float4(1.0, weightOther / knownOther, 1.0, skyOther / knownOther);"),
        "another car's chrome answers where its texels are at least half of the footprint, with both weights");
  Check(Contains(text, "if (rt.z > 0.5)") && Contains(text, "s.skyHit = rt.w * s.agreement;") && Contains(text, "s.skyKnown = 1.0;") && Contains(text, "s.state = 6.0;") &&
            Contains(text, "s.reflectHit = (s.nv >= kCarChrNormalMin) ? rt.y * s.agreement : 0.0;"),
        "another car's chrome: both probes' weights from the image (the reflection's not at a grazing view), the state 6, no march");
  Check(Contains(text, "if (s.skyKnown > 0.5)") && Count(text, "CarChrProbe(history, s,") == 2,
        "the sky probe is not marched where the image gave it, and the march is still there for the rest");
  Check(Contains(text, "if (s.state > 5.5)") && Contains(text, "float3(0.0, 0.9, 0.9)"), "the debug view 4 shows another car's chrome cyan");
  Check(Count(text, "[loop]") == 1 && Count(text, "[unroll]") == 3,
        "one loop over the points, one unrolled search of three halvings, one unrolled B-spline read of nine texels and one unrolled read of the rim's twelve");
  Check(Count(text, "history.Load(") == 3 && Count(text, "history.Sample") == 0 && Count(text, "Gather") == 0, "the history is only read as points");
  Check(Count(text, "CarChrLoad(history, ray.size, uv)") == 1 && Count(text, "CarChrLoad(history, s.ray.size, s.ray.startUv)") == 1,
        "one function reads the history for a point of a probe (the loop's and the halvings'), one read for the shaded point");
  Check(Count(text, "CarChrProbe(history, s,") == 2, "two probes: the reflection's and the sky's");
  // What must stay: the other fetches and the alpha test line.
  Check(Contains(text, "r2.x = tfetch2D(FastShadowCollectorSampler_"), "the shadow read is untouched");
  Check(Contains(text, "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"), "the alpha test line is untouched");
}

void RunNegatives() {
  // A text with an anchor missing or doubled is refused, with the anchor named.
  const std::string text = Fixture();
  const std::string needle = "oC0.xyz = r0.www * r0.xyz;";
  std::string missing = text;
  missing.replace(missing.find(needle), needle.size(), "oC0.xyz = r0.xyz;");
  sp::VariantTransformResult result = sp::TransformCarChromeHlsl(sp::kChrome, missing);
  Check(!result.ok && Contains(result.error, "last write"), "a missing final write is refused and named: " + result.error);

  std::string ambient = text;
  ambient.replace(ambient.find("-r1.xyz + ambYPos.xyz"), 21, "-r1.xyz + gLightAmbient.xyz");
  result = sp::TransformCarChromeHlsl(sp::kChrome, ambient);
  Check(!result.ok && Contains(result.error, "ambient line"), "a missing ambient line is refused and named: " + result.error);

  std::string doubled = text;
  const size_t at = doubled.find("\tr1.xyz = tfetch2DGrad(dpFront");
  doubled.insert(at, doubled.substr(at, doubled.find('\n', at) - at + 1));
  result = sp::TransformCarChromeHlsl(sp::kChrome, doubled);
  Check(!result.ok && Contains(result.error, "environment read"), "a doubled environment read is refused: " + result.error);

  std::string noMain = text;
  noMain.replace(noMain.find("void main("), 10, "void other(");
  result = sp::TransformCarChromeHlsl(sp::kChrome, noMain);
  Check(!result.ok && Contains(result.error, "main"), "a text without main is refused: " + result.error);

  // The paint is not the chrome: each variant refuses the other's shader, and the paint's text has no chrome anchor.
  result = sp::TransformCarChromeHlsl(sp::kCarPaint, text);
  Check(!result.ok, "the paint's name is refused by the chrome variant");
  result = sp::TransformCarSelfReflHlsl(sp::kChrome, text);
  Check(!result.ok, "the chrome's name is refused by the paint variant");
  result = sp::TransformCarSelfReflHlsl(sp::kCarPaint, text);
  Check(!result.ok, "the chrome's text has none of the paint's anchors (a shader is in one variant only)");

  // The definition of the variant.
  const sp::VariantDef def = sp::MakeCarChromeVariant();
  Check(def.id == sp::kVariantCarChrome && def.id == 9 && def.id < sp::kVariantSlots && def.id != sp::kVariantCarSelfRefl,
        "the variant id is 9, inside the slots and not the paint's");
  Check(def.name == "car_chrome" && def.needs_rpf && def.pixel_shaders.size() == 1 && def.pixel_shaders[0].key == 0x2D8CA0430FF88ED7ull,
        "the definition: one shader, the identity of xChrome::PS_Chrome");
  Check(def.name != sp::MakeCarSelfReflVariant().name, "the chrome's name is not the paint's (the stamp lines are by name)");
  Check(sp::kCarChromeVariantVersion == 3, "the variant's version is 3 (the stamp builds the pack's chrome)");
  const std::vector<sp::VariantTarget> entries = {{"car_selfrefl", sp::kCarPaint.key}, {"car_chrome", sp::kChrome.key}};
  Check(sp::FindVariantOverlaps(entries).empty(), "the paint and the chrome are in one variant each");
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
  const sp::VariantTransformResult result = sp::TransformCarChromeHlsl(sp::kChrome, text);
  Check(result.ok, "the retail chrome transforms: " + result.error);
  if (!result.ok) return;
  Check(result.hlsl.size() > text.size(), "the edited text is longer than the game's");
  // The paint's variant must leave the retail chrome alone (a shader in one variant only).
  const sp::VariantTransformResult paint = sp::TransformCarSelfReflHlsl(sp::kCarPaint, text);
  Check(!paint.ok, "the paint's anchors are not in the retail chrome text");
  if (!results.empty()) {
    std::filesystem::create_directories(results, ec);
    const std::filesystem::path out_path = results / "xChrome__PS_Chrome.car_chrome.hlsl";
    std::ofstream out(out_path, std::ios::binary);
    out << result.hlsl;
    std::printf("wrote %s (%zu bytes, the game's %zu)\n", out_path.string().c_str(), result.hlsl.size(), text.size());
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

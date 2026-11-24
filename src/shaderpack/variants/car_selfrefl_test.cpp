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
  // The ray of version 4 (src/gpu/car_selfrefl_rules.h has the same arithmetic, which car_selfrefl_rules_test.cpp checks): the
  // numbers in the helper are the ones of that header. Version 5 adds the host's ray traced image in front of the march, version 6 reads it with a 3x3 B-spline.
  Check(sp::kCarSelfReflVariantVersion == 9, "the variant's version is 9 (the stamp rebuilds the pack's paint)");
  Check(Count(text, "CarReflNoise") == 0 && Count(text, "g_CarReflB.w") == 0 && Count(text, "more.w") == 2,
        "no per-pixel noise: the trace reads no noise frame, the constant's w is the share of the points that take the whole hit");
  Check(Count(text, "[unroll]") == 2 && Count(text, "[loop]") == 1,
        "one loop over the points, one unrolled search of three halvings in a segment with an outline in it, and the ray traced image's nine taps");
  Check(Count(text, "CarReflDistance(i, steps, ray.reach)") == 1, "the points come from the one distance function, the same for every pixel");
  Check(Count(text, "float CarReflWindow(") == 1 && Count(text, "float CarReflWindowIntegral(") == 1 && Count(text, "float CarReflApproach(") == 1 &&
            Count(text, "CarReflSegment CarReflSpan(") == 1 && Count(text, "void CarReflSegmentOf(") == 1 && Count(text, "bool CarReflHasOutline(") == 1 &&
            Count(text, "float CarReflSlack(") == 1 && Count(text, "float3 CarReflSeen(") == 1 && Count(text, "bool CarReflSample(") == 1 &&
            Count(text, "float CarReflThin(") == 0,
        "the helpers of the segments are there once each, and the thin-part floor of version 3 is gone");
  Check(Contains(text, "static const float kCarReflStartMin = 0.04;") && Contains(text, "static const float kCarReflDark = 0.85;") &&
            Contains(text, "static const float kCarReflBackFloor = 0.5;") && Contains(text, "static const float kCarReflShade = 0.4;") &&
            Contains(text, "static const float kCarReflFadeFrom = 0.3;") && Contains(text, "static const float kCarReflFadeTo = 0.9;") &&
            Contains(text, "static const float kCarReflApproachSpan = 0.15;") && Contains(text, "static const int kCarReflOutlineSteps = 3;"),
        "the numbers are those of car_selfrefl_rules.h (start 0.04 m, dark 0.85, hidden face 0.5, shade 0.4, fade 0.3 to 0.9, approach span 0.15 m, "
        "3 halvings)");
  Check(Contains(text, "max(rayLength * u * sqrt(u), kCarReflStartMin)"), "the points' distance grows as u^1.5");
  Check(Contains(text, "min(0.02 + 0.004 * depth, 0.25 * thickness)") && Contains(text, "min(0.01 + 0.004 * depth + slack, 0.5 * thickness - ramp)") &&
            Contains(text, "smoothstep(shape.x, shape.x + shape.y, diff) - smoothstep(shape.z, thickness, diff)") &&
            Contains(text, "width * (u * u * u - 0.5 * u * u * u * u) + max(x - edge1, 0.0)"),
        "the window and its integral are the ones of the rules (a rise, a fall, an exact integral)");
  Check(Contains(text, "sum.covers * kCarReflBackFloor + sum.seen * (1.0 - kCarReflBackFloor)") &&
            Contains(text, "lerp(hit.own * kCarReflShade, hit.colour, hit.face)") && Contains(text, "result.hit = saturate(occupied / full) * agreement;"),
        "a hidden-face hit takes less weight, a hit is the sum of the covers over the points that take the whole of it, and mode 2 shows "
        "the paint's own colour in shade for the hidden face");
  Check(Contains(text, "env * (1.0 - kCarReflDark * weight)") && Contains(text, "return lerp(env, CarReflSeen(hit), weight);"),
        "the two modes: the occlusion scaled by the weight, the reflection of what the ray sees");
  Check(Count(text, "history.Load(") == 2 && Count(text, "history.Sample") == 0 && Count(text, "Gather") == 0,
        "the history is only read as points: the march's one function, and the ray traced image's lookup (the same texels, not filtered)");
  // Version 5: the ray traced image (src/gpu/car_rt_refl.h, car_rt_refl_rules.h ReadForPaint: the same arithmetic, checked by
  // car_rt_refl_rules_test.cpp).
  Check(Count(text, "CarReflRtRead CarReflRtLookup(") == 1 && Count(text, "= CarReflRtLookup(history, ray.size, startUv, start.w,") == 1 &&
            Count(text, "image.Load(") == 1 && Count(text, "image.Sample") == 0,
        "one lookup of the ray traced image, at the shaded point's place in the history, read as texels");
  Check(Contains(text, "for (int k = 0; k < 9; ++k)") && Contains(text, "const float wb = wx[k % 3] * wy[k / 3];") &&
            Contains(text, "const float3 wx = float3(0.5 * (0.5 - d.x) * (0.5 - d.x), 0.75 - d.x * d.x, 0.5 * (0.5 + d.x) * (0.5 + d.x));") &&
            Contains(text, "const float2 nearest = floor(f + 0.5);") && !Contains(text, "const int2 offset = int2(k & 1, k >> 1);"),
        "version 6: the image is read with the quadratic B-spline of the 3x3 texels around the nearest one (car_rt_refl_rules.h BSplineWeights), "
        "not the 2x2 bilinear taps");
  Check(Contains(text, "if ((slots.w & 1u) != 0u)") && Contains(text, "g_Texture2DDescriptorHeap[TFETCH_INDEX(slot)]") &&
            Contains(text, "slots.z);") && Contains(text, "result.hit = rt.weight * agreement;"),
        "the image is read only where the constants' flag says the frame has one (the slot in c.z, the flag in c.w), with the self-test's "
        "agreement; without the flag version 4's march is the whole answer");
  Check(Contains(text, "known >= 0.5 * geometry") && Contains(text, "const float code = abs(t.a);") && Contains(text, "(t.a < 0.0) ? wg * wt : 0.0") &&
            Contains(text, "1.0 - smoothstep(0.5 * tolerance, tolerance, abs(h.a - depth))"),
        "the lookup's rules are the ones of car_rt_refl_rules.h: the sign is a hit on another car (version 8), the texels of another surface are left out, half "
        "the matching texels must have data");
  // Version 8 (RT-HIT-SHADING-4): the clear coat's layer (car_rt_hit_rules.h CoatLayerFresnel, CoatLayerAmount, CoatLayerMix, the same arithmetic) and the debug views 6 and 7.
  Check(Contains(text, "static const float kCarReflCoatF0 = 0.04;") && Contains(text, "static const float kCarReflCoatPower = 5.0;") && Contains(text, "static const float kCarReflCoatMax = 0.6;") &&
            Contains(text, "const float fresnel = min(kCarReflCoatF0 + (1.0 - kCarReflCoatF0) * pow(saturate(1.0 - hit.nv), kCarReflCoatPower), kCarReflCoatMax);") &&
            Contains(text, "return saturate(hit.coat) * fresnel;") && Contains(text, "return lerp(paint, hit.colour, CarReflCoatAmount(hit));"),
        "version 8: the layer is lerp(paint, the car's colour, fresnel x the share of the hit on another car), Schlick with F0 0.04, power 5, at most 0.6");
  Check(Contains(text, "if ((g_CarReflC.w & 2u) == 0u || g_CarReflA.x < 1.5 || hit.coat <= 0.0)") && Contains(text, "oC0.xyz = CarReflCoat(oC0.xyz, carRefl);") &&
            text.find("oC0.xyz = CarReflCoat(oC0.xyz, carRefl);") < text.find("CarReflDebug(carRefl, iTexCoord4.w)"),
        "version 8: the layer is switched by the constants' flag bit 1, only in mode 2 and only where there is a hit on another car; it follows the paint's last write and comes before the debug view");
  Check(Contains(text, "result.coat = result.hit * rt.car;") && Contains(text, "result.nv = nv;") && Contains(text, "r.car = (weight > 1e-5) ? saturate(car / weight) : 0.0;"),
        "version 8: the layer's share is the hit's weight times the share of it with the sign (the lookup's car), the fresnel's N.V is the paint's own");
  Check(Contains(text, "if (view == 6u)") && Contains(text, "if (view == 7u)") && Contains(text, "return hit.colour * CarReflCoatAmount(hit);"),
        "version 8: the debug views 6 (the layer's amount) and 7 (what it adds)");
  Check(Contains(text, "static const float kCarReflAmountViewGain = 64.0;") && Contains(text, "return float3(amount, amount, amount) * (unit * kCarReflAmountViewGain);"),
        "RT-HIT-SHADING-6: the debug view 6 writes the amount 64 times the target's unit (rthit4's pictures showed it at the unit as codes 2 to 4: no ramp could be read off them)");
  Check(Contains(text, "if (rt.use > 0.5)") && text.find("if (rt.use > 0.5)") < text.find("CarReflSum sum = (CarReflSum)0;"),
        "an answer from the image returns before the march's loop");
  Check(Contains(text, "if (view == 5u)") && Contains(text, "float rt;"), "the debug view 5 and the result's field for it");
  Check(Count(text, "CarReflLoad(history, ray.size, uv)") == 1 && Count(text, "CarReflLoad(history, ray.size, startUv)") == 1,
        "one function reads the history for a point of the ray (the loop's and the halvings'), one read for the shaded point");
  // Version 9: no double precision in the DXIL. A ternary of bare literals is a double in HLSL and dxc kept `select double` for the lookup's state (the nested one):
  // the paint's pipelines of this variant failed on a GPU without doubles (the Intel iGPUs on D3D12: "Shader uses double precision float ops"; 27 of the
  // starter list's pipelines on the laptop's UHD). The builder refuses a variant blob with the flag (contract_checks.h CheckDxilNoDoubles, dxil_features_test.cpp).
  Check(Contains(text, "? 2.0f : ((car > 0.5 * weight) ? 4.0f : 3.0f)") && Contains(text, "result.state = (result.hit > 0.02) ? 4.0f : 3.0f;") &&
            Contains(text, "p.on = (texel.a > 0.0) ? 1.0f : 0.0f;"),
        "version 9: the ternaries of bare literals (the lookup's state, the trace's state, the point's on) have f suffixes: float, never double");
  Check(Count(text, "? 4.0 : 3.0") == 0 && Count(text, "? 2.0 :") == 0 && Count(text, "? 1.0 : 0.0") == 0,
        "version 9: no ternary of two bare literals is left in the paint's helper");
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
  Check(def.feature == sp::kFeatureCarSelfRefl && def.feature == 5 && def.feature < sp::kMaxFeatures, "the feature is 5 (the old id 7), inside the word");
  Check(sp::chain::Find(def.feature) && sp::chain::Find(def.feature)->constants_bytes == 112 && sp::chain::Find(def.feature)->name == def.name,
        "its chain row says 112 bytes of constants");
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

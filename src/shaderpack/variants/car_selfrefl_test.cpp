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
  Check(Count(text, "SpecularOcclusion(iTexCoord6.w, iTexCoord4.w, kSpecularOcclusionPaintRoughness)") == 1 &&
            Contains(text, "float carEnvVisibility = 1.0f;") &&
            Contains(text, "if ((g_CarReflC.w & kSpecularOcclusionPaintFlag) != 0u)"),
        "paint visibility is opt-out and uses baked AO and N.V even without car history");
  Check(Contains(text, "r10.xyz *= carEnvVisibility;\n\tr10.xyz = CarReflApply") &&
            Contains(text, "r13.xyz *= carEnvVisibility;\n\tr13.xyz = CarReflApply"),
        "only environment texels are attenuated before either can be replaced by a hit");
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
  Check(sp::kCarSelfReflVariantVersion == 12, "the stamp rebuilds the variant for paint specular occlusion");
  // Version 10 (RT-PARKED-PAINT): a draw with the flag bit 2 reads the history's distance as abs(a) in the self-test and in the lookup of the ray traced image, and nothing else changes
  // (src/gpu/car_selfrefl_rules.h PaintHistoryDistance is the same arithmetic).
  Check(Contains(text, "const float ownDistance = ((slots.w & 4u) != 0u) ? abs(own.a) : own.a;") && Contains(text, "if (!(ownDistance > 0.0) || agreement <= 0.0)") &&
            Contains(text, "const float agreement = 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(ownDistance - start.w));"),
        "version 10: the self-test takes the history's distance as abs(a) for a draw with the parked flag");
  Check(Contains(text, "float tolerance, uint slot, uint flags)") && Contains(text, "const float texelDistance = ((flags & 4u) != 0u) ? abs(h.a) : h.a;") &&
            Contains(text, "const float agree = (texelDistance > 0.0) ? 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(texelDistance - depth)) : 0.0;") &&
            Contains(text, "2.0 * (0.01 + 0.004 * start.w + ray.slack), slots.z, slots.w);"),
        "version 10: the lookup of the ray traced image takes the texels' distance as abs(a) for a draw with the parked flag");
  Check(!Contains(text, "own.a > 0.0)") && Count(text, "texel.a > 0.0") == 1 && Count(text, "h.a > 0.0") == 0,
        "version 10: no other read of the history's alpha changed (the march's point reads and the outline tests still ask `a > 0`: a mark is no vehicle there)");
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
            Contains(text, "slots.z, slots.w);") && Contains(text, "result.hit = rt.weight * agreement;"),
        "the image is read only where the constants' flag says the frame has one (the slot in c.z, the flag in c.w), with the self-test's "
        "agreement; without the flag version 4's march is the whole answer");
  Check(Contains(text, "known >= 0.5 * geometry") && Contains(text, "const float code = abs(t.a);") && Contains(text, "(t.a < 0.0) ? wg * wt : 0.0") &&
            Contains(text, "1.0 - smoothstep(0.5 * tolerance, tolerance, abs(texelDistance - depth))"),
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

// The parts of the translated TRAFFIC paint (xAmbientCarPaint__PSAmbientCarPaint) the transform looks at, in the shape the translator writes them: both backends' declarations of the
// shader's own pixel constants (DL0, DL4, the view inverse), the shared constants' declarations, main's start, the two environment reads (plain tfetch2D, at r0.xy), and the colour's last write.
std::string TrafficFixture() {
  return "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n"
         "#define DL0 vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 1600, 0x10)\n"
         "#define DL4 vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 1616, 0x10)\n"
         "#define gViewInverse(INDEX) select((INDEX) < 212, vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + (12 + min(INDEX, 211)) * 16, 0x10), 0.0)\n"
         "#else\n"
         "cbuffer PixelShaderConstants : register(b1, space4)\n"
         "{\n"
         "\tfloat4 DL0 : packoffset(c100);\n"
         "\tfloat4 DL4 : packoffset(c101);\n"
         "\tfloat4 gViewInverse[4] : packoffset(c12);\n"
         "};\n"
         "#define DEFINE_SHARED_CONSTANTS() \\\n"
         "    float4 g_PosOffset : packoffset(c36); \\\n"
         "    float2 g_InvResolutionScale : packoffset(c37.x);\n"
         "\n"
         "#ifndef __spirv__\n"
         "[shader(\"pixel\")]\n"
         "#endif\n"
         "void main(\n"
         "\tin float4 iPos : SV_Position,\n"
         "\tout float4 oC0 : SV_Target0)\n"
         "{\n"
         "\tfloat4 r0 = 0.0;\n"
         "\tCubeMapData cubeMapData = (CubeMapData)0;\n"
         "\tr0.z = tfetch2D(FastShadowCollectorSampler_Texture2DDescriptorIndex, FastShadowCollectorSampler_SamplerDescriptorIndex, r0.wx, float2(0, 0)).x;\n"
         "\tr4.xyz = tfetch2D(dpFrontSampler_Texture2DDescriptorIndex, dpFrontSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyz;\n"
         "\tr0.xyz = tfetch2D(dpSpecFrontSampler_Texture2DDescriptorIndex, dpSpecFrontSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyz;\n"
         "\tr3.xy = tfetch2D(ScrapeSampler_Texture2DDescriptorIndex, ScrapeSampler_SamplerDescriptorIndex, r3.xy, float2(0, 0)).yw;\n"
         "\toC0.xyzw = max(r1.xyzw, r1.xyzw);\n"
         "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\t{\t\tclip(oC0.w - g_AlphaThreshold);\n"
         "\t}\n"
         "}\n";
}

void RunOnTrafficFixture() {
  const sp::VariantTransformResult result = sp::TransformCarSelfReflTrafficHlsl(sp::kTrafficPaint, TrafficFixture());
  Check(result.ok, "the traffic fixture transforms: " + result.error);
  if (!result.ok) return;
  const std::string& text = result.hlsl;
  Check(Count(text, "carRefl = CarReflTrace(iTexCoord0.xyz, iTexCoord1.xyz, iTexCoord4.xyz);") == 1 && Count(text, "= CarReflTrace(") == 1,
        "one trace call, with the shader's own interpolants: the world position, the normal and the view vector");
  Check(Count(text, "r4.xyz = CarReflApply(r4.xyz, carRefl, 1.0);") == 1, "the sharp read (r4) is applied once at full weight");
  Check(Count(text, "r0.xyz = CarReflApply(r0.xyz, carRefl, 0.5);") == 1, "the blurred read (r0) is applied once at half weight");
  Check(Count(text, "CarReflDebug(carRefl, 1.0)") == 1 && Count(text, "oC0.xyz = CarReflDebug(") == 1, "one debug view, with no baked AO (1)");
  Check(text.find("r4.xyz = tfetch2D(dpFrontSampler") < text.find("carRefl = CarReflTrace(") && text.find("carRefl = CarReflTrace(") < text.find("r4.xyz = CarReflApply(") &&
            text.find("r4.xyz = CarReflApply(") < text.find("r0.xyz = tfetch2D(dpSpecFrontSampler") && text.find("r0.xyz = tfetch2D(dpSpecFrontSampler") < text.find("r0.xyz = CarReflApply(") &&
            text.find("r0.xyz = CarReflApply(") < text.find("oC0.xyzw = max(r1.xyzw, r1.xyzw);") && text.find("oC0.xyzw = max(r1.xyzw, r1.xyzw);") < text.find("CarReflDebug(carRefl, 1.0)"),
        "the order: the sharp read, the trace and its apply, the blurred read and its apply, the colour's write, the debug view");
  Check(text.find("CarReflDebug(carRefl, 1.0)") < text.find("SPEC_CONSTANT_ALPHA_TEST)\t{"), "the debug view comes before the alpha test line");
  // the exponent bias, declared once in each backend after the shader's own last constant
  Check(Contains(text, "#define DL4 vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 1616, 0x10)\n#define gInvColorExpBias vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 432, 0x10)\n"),
        "the SPIR-V declaration of the exponent bias follows DL4's (register c27: byte 432)");
  Check(Contains(text, "\tfloat4 DL4 : packoffset(c101);\n\tfloat4 gInvColorExpBias : packoffset(c27);\n"), "the DXIL declaration of the exponent bias follows DL4's");
  Check(Count(text, "gInvColorExpBias") == 3, "the exponent bias is named three times: the two declarations and the debug view");
  // the constants and the helper, as the paint's
  Check(Contains(text, "#define g_CarReflM0 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)") &&
            Contains(text, "#define g_CarReflC vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 704, 0x10)") &&
            Contains(text, "float2 g_InvResolutionScale : packoffset(c37.x); \\\n    float4 g_CarReflM0 : packoffset(c38);") && Contains(text, "uint4 g_CarReflC : packoffset(c44);\n"),
        "the constants are the paint's 112 bytes at 608, in both backends");
  Check(Count(text, "struct CarReflResult") == 1 && text.find("struct CarReflResult") < text.find("void main(") &&
            text.find("CarReflResult carRefl = (CarReflResult)0;") > text.find("CubeMapData cubeMapData"),
        "the helper once, before main; the local result after the cube map data");
  // at an other position in a composed text the constants move (the chain's layout): the declarations follow the context's offset
  sp::StageContext moved = sp::SoloContext(sp::kFeatureCarSelfRefl);
  moved.constants_offset = 608 + 48;
  const sp::VariantTransformResult shifted = sp::TransformCarSelfReflTrafficHlsl(sp::kTrafficPaint, TrafficFixture(), moved);
  Check(shifted.ok && Contains(shifted.hlsl, "g_CarReflM0 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 656, 0x10)"), "composed after another stage, the block is declared where the chain puts it");
  // the helper is the paint's, with the two returns of the host's-image-alone rule and nothing else
  const sp::VariantTransformResult paint = sp::TransformCarSelfReflHlsl(sp::kCarPaint, Fixture());
  const size_t paint_from = paint.hlsl.find("// LARecomp variant car_selfrefl"), paint_to = paint.hlsl.find("[shader(\"pixel\")]");
  const size_t traffic_from = text.find("// LARecomp variant car_selfrefl"), traffic_to = text.find("[shader(\"pixel\")]");
  std::string traffic_helper = traffic_from == std::string::npos ? std::string() : text.substr(traffic_from, traffic_to - traffic_from);
  const std::string no_image =
      "    // The traffic paint (RT-TRAFFIC-REFL, version 9): the host's ray traced image is its only source, a frame without one is the game's paint.\n"
      "    if ((g_CarReflC.w & 1u) == 0u)\n"
      "    {\n"
      "        return result;\n"
      "    }\n";
  const std::string no_march =
      "    // The traffic paint: no march where the image has no data here (the game's paint stays).\n"
      "    return result;\n"
      "\n";
  Check(Count(traffic_helper, no_image) == 1 && Count(traffic_helper, no_march) == 1, "the traffic helper has the early return for a frame with no image and the return before the march, once each");
  Check(traffic_helper.find(no_image) < traffic_helper.find("const float4 more = g_CarReflB;") && traffic_helper.find(no_march) < traffic_helper.find("// 2. The march: ") &&
            traffic_helper.find(no_march) > traffic_helper.find("if (rt.use > 0.5)"),
        "the early return comes before the settings' second read, the march's return after the image's answer and before the march");
  for (const std::string& block : {no_image, no_march}) {
    const size_t at = traffic_helper.find(block);
    if (at != std::string::npos) traffic_helper.erase(at, block.size());
  }
  Check(paint.ok && paint_from != std::string::npos && traffic_from != std::string::npos && paint.hlsl.substr(paint_from, paint_to - paint_from) == traffic_helper,
        "with those two blocks taken out the helper is the player's paint's, byte for byte");
  Check(Count(paint.hlsl, "The traffic paint") == 0, "the player's paint's text has neither of the two returns");
  // what must stay
  Check(Contains(text, "r0.z = tfetch2D(FastShadowCollectorSampler_") && Contains(text, "r3.xy = tfetch2D(ScrapeSampler_") && Contains(text, "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"),
        "the shadow collector's read, the scrape's and the alpha test line are untouched");
}

void RunTrafficNegatives() {
  const std::string text = TrafficFixture();
  auto without = [&](const std::string& needle, const std::string& with) {
    std::string t = text;
    t.replace(t.find(needle), needle.size(), with);
    return t;
  };
  sp::VariantTransformResult result = sp::TransformCarSelfReflTrafficHlsl(sp::kTrafficPaint, without("oC0.xyzw = max(r1.xyzw, r1.xyzw);", "oC0.xyzw = r1.xyzw;"));
  Check(!result.ok && Contains(result.error, "last write"), "a missing final write is refused and named: " + result.error);
  result = sp::TransformCarSelfReflTrafficHlsl(sp::kTrafficPaint, without("\tr4.xyz = tfetch2D(dpFrontSampler", "\tr5.xyz = tfetch2D(dpFrontSampler"));
  Check(!result.ok && Contains(result.error, "sharp environment read"), "a missing sharp read is refused: " + result.error);
  std::string doubled = text;
  const size_t at = doubled.find("\tr0.xyz = tfetch2D(dpSpecFront");
  doubled.insert(at, doubled.substr(at, doubled.find('\n', at) - at + 1));
  result = sp::TransformCarSelfReflTrafficHlsl(sp::kTrafficPaint, doubled);
  Check(!result.ok && Contains(result.error, "blurred environment read"), "a doubled blurred read is refused: " + result.error);
  result = sp::TransformCarSelfReflTrafficHlsl(sp::kTrafficPaint, without("\tfloat4 DL4 : packoffset(c101);\n", "\tfloat4 DL5 : packoffset(c102);\n"));
  Check(!result.ok && Contains(result.error, "DL4"), "a shader whose last pixel constant is not where the edit looks is refused, DL4 named: " + result.error);
  result = sp::TransformCarSelfReflTrafficHlsl(sp::kTrafficPaint, without("void main(", "void other("));
  Check(!result.ok && Contains(result.error, "main"), "a text without main is refused: " + result.error);
  result = sp::TransformCarSelfReflTrafficHlsl(sp::kCarPaint, text);
  Check(!result.ok, "the player's paint is not the traffic paint: refused");
  result = sp::TransformCarSelfReflHlsl(sp::kTrafficPaint, text);
  Check(!result.ok, "... and the traffic paint is not the player's paint's edit");
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
  Check(def.name == "car_selfrefl" && def.needs_rpf && def.pixel_shaders.size() == 2, "the definition: two pixel shaders");
  Check(def.pixel_shaders[0].key == 0x439B193A3F675533ull, "the identity of the paint");
  Check(def.pixel_shaders[1].key == 0x9A6BE351ED46662Dull && def.pixel_shaders[1].name == "xAmbientCarPaint__PSAmbientCarPaint", "the identity of the traffic paint (the runtime's own, perf/out/night-probe-ids.txt)");
  {
    // the definition's transform picks the text edit by the shader's identity
    const sp::VariantTransformResult by_def_paint = def.transform(sp::kCarPaint, Fixture(), sp::SoloContext(sp::kFeatureCarSelfRefl));
    const sp::VariantTransformResult by_def_traffic = def.transform(sp::kTrafficPaint, TrafficFixture(), sp::SoloContext(sp::kFeatureCarSelfRefl));
    Check(by_def_paint.ok && Contains(by_def_paint.hlsl, "CarReflTrace(iTexCoord1.xyz, iTexCoord2.xyz, iTexCoord6.xyz)"), "the definition's transform edits the paint as the paint's own function does");
    Check(by_def_traffic.ok && Contains(by_def_traffic.hlsl, "CarReflTrace(iTexCoord0.xyz, iTexCoord1.xyz, iTexCoord4.xyz)"), "... and the traffic paint as the traffic function does");
    const sp::VariantTransformResult crossed = def.transform(sp::kTrafficPaint, Fixture(), sp::SoloContext(sp::kFeatureCarSelfRefl));
    Check(!crossed.ok, "the traffic paint's edit refuses the player's paint's text (its anchors are not there)");
  }
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
  std::filesystem::path traffic_found;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec)) {
    if (entry.is_regular_file() && entry.path().filename() == "xAmbientCarPaint__PSAmbientCarPaint.hlsl") {
      traffic_found = entry.path();
      break;
    }
  }
  if (traffic_found.empty()) {
    std::printf("no xAmbientCarPaint__PSAmbientCarPaint.hlsl under %s\n", folder.string().c_str());
    ++g_failed;
  } else {
    std::string traffic_text;
    Check(ReadFile(traffic_found, &traffic_text), "read " + traffic_found.string());
    const sp::VariantTransformResult traffic = sp::TransformCarSelfReflTrafficHlsl(sp::kTrafficPaint, traffic_text);
    Check(traffic.ok, "the retail traffic paint transforms: " + traffic.error);
    if (traffic.ok) {
      Check(traffic.hlsl.size() > traffic_text.size(), "the edited traffic text is longer than the game's");
      Check(Count(traffic.hlsl, "CarReflTrace(iTexCoord0.xyz, iTexCoord1.xyz, iTexCoord4.xyz)") == 1 && Count(traffic.hlsl, "gInvColorExpBias") == 3, "the retail text takes the edit once");
      if (!results.empty()) {
        std::filesystem::create_directories(results, ec);
        std::ofstream out(results / "xAmbientCarPaint__PSAmbientCarPaint.car_selfrefl.hlsl", std::ios::binary);
        out << traffic.hlsl;
        std::printf("wrote %s (%zu bytes, the game's %zu)\n", (results / "xAmbientCarPaint__PSAmbientCarPaint.car_selfrefl.hlsl").string().c_str(), traffic.hlsl.size(), traffic_text.size());
      }
    }
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
  RunOnTrafficFixture();
  RunNegatives();
  RunTrafficNegatives();
  if (argc > 1) {
    RunOnTree(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

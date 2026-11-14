// Check of the rule that a pixel shader is in one shader variant only (variant_overlap.h) and of the one real case the
// car self-reflection feature depends on: the player's car paint is edited by the car self-reflection variant and left alone by
// the shadow filter's (which edits the sun shadow collectors' atlas lookup, a lookup the paint does not have). A program of its
// own: it needs no renderer, no ReXGlue, no compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_variant_overlap_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_variant_overlap_test [<folder of translated HLSL>]
//
// Without an argument it runs on the small paint-like text made here. With a folder of translated HLSL (the builder's own output:
// `<group>__<entry>.hlsl` files, for example shaderpack/baseline/hlsl-rpf/cars of the development PC) it also runs on
// xCarPaintCustomizable__PS_CarPaint.hlsl found in it, the retail text.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variant_overlap.h"
#include "shaderpack/variants/car_selfrefl.h"
#include "shaderpack/variants/shadow_filter.h"

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

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
  }
}

// ---- the rule -----------------------------------------------------------------------------------------------

void TestRule() {
  Check(sp::FindVariantOverlaps({}).empty(), "no entries: no overlap");

  // The built-in variants' shape: every shader in one variant only.
  const std::vector<sp::VariantTarget> disjoint = {
      {"bloom_gauss", 0xBF2ABD24C5329903ull}, {"bloom_gauss", 0x80ED62377E87264Aull},
      {"env_map", 0x4AA33D052D8AAD13ull},     {"shadow_filter", 0x0372DB00C39B196Full},
      {"shadow_filter", 0x04F338EFCD63C4E7ull}, {"car_selfrefl", 0x439B193A3F675533ull},
  };
  Check(sp::FindVariantOverlaps(disjoint).empty(), "every shader in one variant: no overlap");

  // A shader two variants have an entry for.
  std::vector<sp::VariantTarget> shared = disjoint;
  shared.push_back({"shadow_filter", 0x439B193A3F675533ull});  // the paint, as if the shadow filter edited it too
  const std::vector<sp::VariantOverlap> found = sp::FindVariantOverlaps(shared);
  Check(found.size() == 1, "one shader in two variants: one overlap");
  if (found.size() == 1) {
    Check(found[0].ps_key == 0x439B193A3F675533ull, "the overlap names the paint");
    Check(found[0].variants.size() == 2 && found[0].variants[0] == "car_selfrefl" && found[0].variants[1] == "shadow_filter",
          "the overlap names both variants, in the order they were made");
    CheckEq(sp::DescribeVariantOverlap(found[0]), "439B193A3F675533 in car_selfrefl and shadow_filter", "the overlap's words");
  }

  // The same variant twice is not an overlap; three variants are listed with commas; the order is by identity.
  const std::vector<sp::VariantTarget> odd = {
      {"a", 9}, {"a", 9}, {"b", 7}, {"c", 7}, {"a", 7}, {"d", 3},
  };
  const std::vector<sp::VariantOverlap> odds = sp::FindVariantOverlaps(odd);
  Check(odds.size() == 1 && odds[0].ps_key == 7 && odds[0].variants.size() == 3, "the same variant twice is not an overlap; three variants are one");
  if (odds.size() == 1) CheckEq(sp::DescribeVariantOverlap(odds[0]), "0000000000000007 in b, c and a", "three variants' words");
  const std::vector<sp::VariantTarget> two = {{"x", 5}, {"y", 5}, {"x", 2}, {"y", 2}};
  const std::vector<sp::VariantOverlap> twos = sp::FindVariantOverlaps(two);
  Check(twos.size() == 2 && twos[0].ps_key == 2 && twos[1].ps_key == 5, "overlaps come in increasing identity order");
}

// ---- the paint -----------------------------------------------------------------------------------------------

// The parts of the translated paint the transforms look at (as car_selfrefl_test.cpp): the paint reads the world's shadow
// image (FastShadowCollectorSampler) and its environment maps, and no cascade atlas (DepthTextureSampler0).
std::string PaintFixture() {
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

void TestPaint(const std::string& text, const std::string& what) {
  const sp::VariantTransformResult car = sp::TransformCarSelfReflHlsl(sp::kCarPaint, text);
  Check(car.ok && !car.skip, what + ": the car self-reflection variant edits the paint (" + car.error + ")");
  const sp::VariantTransformResult filter = sp::TransformShadowFilterHlsl(sp::kCarPaint, text);
  Check(!filter.ok && filter.skip, what + ": the shadow filter variant leaves the paint out (" + filter.error + ")");
  CheckEq(filter.error, std::string(sp::kShadowSkipNoLookup), what + ": because the paint reads no shadow atlas");
  // The shadow filter finds its shaders by name: the paint's name is not one of the collectors'.
  const std::string_view name = sp::kCarPaint.name;
  Check(!name.ends_with(sp::kShadowBlendSuffix) && !name.ends_with(sp::kShadowBlendAlphaSuffix),
        what + ": the paint's name is not a shadow collector's (PS_ShadowBlend, PS_ShadowBlendAlpha)");
  // So the step's own check has nothing to report for the two.
  const std::vector<sp::VariantTarget> entries = {{"car_selfrefl", sp::kCarPaint.key}};
  Check(sp::FindVariantOverlaps(entries).empty(), what + ": the paint is in one variant");
}

std::string Slurp(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
  TestRule();
  TestPaint(PaintFixture(), "fixture");
  if (argc > 1) {
    const std::filesystem::path file = std::filesystem::path(argv[1]) / "xCarPaintCustomizable__PS_CarPaint.hlsl";
    const std::string text = Slurp(file);
    if (text.empty()) {
      std::printf("FAILED: %s is not there\n", file.string().c_str());
      ++g_failed;
    } else {
      TestPaint(text, "retail");
    }
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

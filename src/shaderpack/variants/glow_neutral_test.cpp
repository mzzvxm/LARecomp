// Standalone unit test. Optional arguments: <translated HLSL> <output HLSL>.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "shaderpack/variants/glow_neutral.h"
#include "shaderpack/variants/text_edit.h"

namespace sp = larecomp::shaderpack;

namespace {

int checks = 0;
int failed = 0;

void Check(bool passed, std::string_view message) {
  ++checks;
  if (!passed) {
    ++failed;
    std::printf("FAIL: %.*s\n", int(message.size()), message.data());
  }
}

constexpr std::string_view kFetch =
    "tfetch2D(LightGlowTexSampler_Texture2DDescriptorIndex, LightGlowTexSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyz";

std::string Fixture() {
  return std::string("#define TFETCH_GAMMA_BIT 0x80000000u\n"
                     "float4 applyTfetchGamma(float4 value, uint index);\n"
                     "#ifndef __spirv__\n[shader(\"pixel\")]\n#endif\nvoid main(\n"
                     "    out float4 color : SV_Target0) {\n"
                     "    float4 r0 = 0;\n    float3 sample = ") + std::string(kFetch) +
         ";\n    color = float4(sample * float3(8, 4, 2), 0.75);\n}\n";
}

void CheckTransform() {
  const std::string original = Fixture();
  const auto corrected = sp::TransformGlowNeutralHlsl(sp::kGlowNeutralSeed, original);
  Check(corrected.ok, corrected.error);
  const auto& hlsl = corrected.hlsl;
  Check(sp::textedit::CountOccurrences(hlsl, "GlowNeutralSample(") == 2, "one definition and one call");
  const size_t helper = hlsl.find("float3 GlowNeutralSample(");
  const size_t main = hlsl.find("void main(");
  Check(main != std::string::npos && helper < main, "helper precedes the entry point");
  Check(hlsl.contains("float3 sample = GlowNeutralSample(LightGlowTexSampler_Texture2DDescriptorIndex, LightGlowTexSampler_SamplerDescriptorIndex, r0.xy);") &&
            !hlsl.contains(kFetch), "the original glow lookup is replaced");
  const size_t fetch = hlsl.find("tfetch2D(textureIndex & ~TFETCH_GAMMA_BIT, samplerIndex, uv, float2(0, 0))");
  const size_t chroma = hlsl.find("max(abs(texel.r - texel.g), abs(texel.b - texel.g))");
  const size_t mix = hlsl.find("texel.rgb = lerp(texel.ggg, texel.rgb, keep)");
  const size_t gamma = hlsl.find("return applyTfetchGamma(texel, textureIndex).rgb");
  Check(fetch < chroma && chroma < mix && mix < gamma && gamma < main,
        "sample once with the original sampler, correct both channels, then apply the original gamma flag");
  Check(hlsl.contains("saturate((chroma - 9.0 / 255.0) * (255.0 / 9.0))"), "shader thresholds span 9 to 18 levels");
  Check(hlsl.contains("color = float4(sample * float3(8, 4, 2), 0.75);"), "the rest of the entry point is preserved");

  std::string crlf;
  for (char ch : original) {
    if (ch == '\n') crlf += '\r';
    crlf += ch;
  }
  const auto crlf_result = sp::TransformGlowNeutralHlsl(sp::kGlowNeutralSeed, crlf);
  Check(crlf_result.ok && crlf_result.hlsl == hlsl, "CRLF and LF produce identical output");
  Check(!sp::TransformGlowNeutralHlsl(sp::kGlowNeutralSeed, hlsl).ok, "a second transform is refused");
  Check(!sp::TransformGlowNeutralHlsl(sp::kGlowNeutralSeed, original + std::string(kFetch)).ok, "multiple glow lookups are refused");
  Check(!sp::TransformGlowNeutralHlsl(sp::kGlowNeutralSeed, std::string(kFetch)).ok, "the gamma helpers and entry point are required");
  std::string wrong_fetch = original;
  wrong_fetch.replace(wrong_fetch.find("r0.xy"), 5, "r0.zw");
  Check(!sp::TransformGlowNeutralHlsl(sp::kGlowNeutralSeed, wrong_fetch).ok, "a changed lookup is refused");
  Check(!sp::TransformGlowNeutralHlsl({"different_shader", 0}, original).ok, "an unrelated shader is refused");

  const auto def = sp::MakeGlowNeutralVariant();
  const auto* row = sp::chain::Find(def.feature);
  Check(def.feature == sp::kFeatureGlowNeutral && def.pixel_shaders.size() == 1 &&
            def.pixel_shaders.front().key == sp::kGlowNeutralSeed.key && !def.needs_rpf,
        "the variant targets the executable's glow shader");
  Check(row && row->spec_twin && row->constants_bytes == 0 && row->composes_with == 0,
        "the variant has an alpha-test twin and uses the existing bindings");
}

// Independent model in 8-bit levels. Endpoints use the bit expansion and
// the integer palette interpolation of the software DXT1 reference path.
using Rgb = std::array<double, 3>;

Rgb Correct(const Rgb& sample) {
  const auto [r, g, b] = sample;
  const double delta = std::max(std::abs(r - g), std::abs(b - g));
  const double keep = std::clamp((delta - sp::kGlowNeutralFullLevels) /
                                     (sp::kGlowNeutralNoneLevels - sp::kGlowNeutralFullLevels),
                                 0.0, 1.0);
  return {std::lerp(g, r, keep), g, std::lerp(g, b, keep)};
}

Rgb GreyEndpoint(int level) {
  const int rb = (level * 31 + 127) / 255;
  const int g = (level * 63 + 127) / 255;
  const double rb8 = (rb << 3) | (rb >> 2);
  return {rb8, double((g << 2) | (g >> 4)), rb8};
}

void CheckBc1() {
  int pink = 0;
  int green = 0;
  int remaining = 0;
  int samples = 0;
  const auto measure = [&](const Rgb& c) {
    ++samples;
    pink += c[0] > c[1];
    green += c[0] < c[1];
    const Rgb n = Correct(c);
    remaining += n != Rgb{c[1], c[1], c[1]};
  };
  for (int lo = 0; lo < 256; ++lo) {
    const Rgb a = GreyEndpoint(lo);
    measure(a);
    for (int hi = lo + 1; hi < 256; ++hi) {
      const Rgb b = GreyEndpoint(hi);
      for (double w : {1.0 / 3, 0.5, 2.0 / 3}) {
        measure({std::floor(std::lerp(a[0], b[0], w)), std::floor(std::lerp(a[1], b[1], w)), std::floor(std::lerp(a[2], b[2], w))});
      }
      measure({std::lerp(a[0], b[0], 0.25), std::lerp(a[1], b[1], 0.25), std::lerp(a[2], b[2], 0.25)});
    }
  }
  std::printf("BC1 gray endpoints, palette and filtered samples: %d, %d pink, %d green, %d with chroma remaining\n",
              samples, pink, green, remaining);
  Check(pink > 0 && green > 0, "565 quantization produces both signs of chroma before any gain");
  Check(remaining == 0, "all modeled gray samples are neutralized at their green intensity");
  Check(Correct({89, 80, 80}) == Rgb{80, 80, 80}, "correction is full at nine levels");
  Check(Correct({80, 80, 62}) == Rgb{80, 80, 62}, "correction ends at eighteen levels");
  Check(Correct({93.5, 80, 66.5}) == Rgb{86.75, 80, 73.25}, "transition retains half the chroma at its midpoint");
  Check(Correct({40, 12, 0}) == Rgb{40, 12, 0}, "strongly colored samples retain their original channels");
}

void WriteTranslated(const char* input, const char* output) {
  std::ifstream in(input, std::ios::binary);
  Check(in.is_open(), "translated HLSL opens");
  if (!in) return;
  const std::string source{std::istreambuf_iterator<char>(in), {}};
  const auto result = sp::TransformGlowNeutralHlsl(sp::kGlowNeutralSeed, source);
  Check(result.ok, result.error);
  if (!result.ok) return;
  std::ofstream out(output, std::ios::binary);
  out << result.hlsl;
  out.close();
  Check(bool(out), "transformed HLSL is written");
}

}  // namespace

int main(int argc, char** argv) {
  CheckTransform();
  CheckBc1();
  if (argc == 3) {
    WriteTranslated(argv[1], argv[2]);
  } else {
    Check(argc == 1, "provide zero arguments or both translated HLSL paths");
  }
  std::printf("%d checks, %d failed\n", checks, failed);
  return failed != 0;
}

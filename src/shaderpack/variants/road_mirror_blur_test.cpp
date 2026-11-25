#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "shaderpack/variant_compose.h"
#include "shaderpack/variants/gbuffer.h"
#include "shaderpack/variants/headlight.h"
#include "shaderpack/variants/road_mirror_blur.h"
#include "shaderpack/variants/text_edit.h"

namespace sp = larecomp::shaderpack;
namespace chain = sp::chain;
namespace fs = std::filesystem;

namespace {

float saturate(float value) {
  return std::clamp(value, 0.0f, 1.0f);
}

float smoothstep(float lo, float hi, float value) {
  const float t = saturate((value - lo) / (hi - lo));
  return t * t * (3.0f - 2.0f * t);
}

using std::lerp;
#include "shaderpack/variants/road_mirror_blur.hlsli"

int checks = 0;
int failed = 0;
void Check(bool good, const std::string& description) {
  ++checks;
  if (!good) {
    ++failed;
    std::printf("FAIL %s\n", description.c_str());
  }
}

bool Near(float a, float b) {
  return std::abs(a - b) < 1e-6f;
}

void TestFilter() {
  const auto at = [](float metres, float wetness) { return RoadMirrorParams(1.0f / metres, wetness); };
  for (const float depth : {1.0f, 18.0f, 48.0f, 100.0f}) {
    const auto dry = at(depth, 0.095f);
    Check(dry.radius == 0.0f && dry.keep == 1.0f, "dry-road bypass");
  }
  const auto invalid = RoadMirrorParams(0.0f, 1.17f);
  Check(invalid.radius == 0.0f && invalid.keep == 1.0f, "invalid clip W bypass");
  const auto near = at(4.0f, 1.17f);
  const auto middle = at(18.0f, 1.17f);
  const auto far = at(32.0f, 1.17f);
  Check(Near(near.radius * 256.0f, 0.65f) && near.keep == 1.0f, "near wet road keeps its reflection strength");
  Check(Near(middle.radius * 256.0f, 1.575f) && middle.keep < 1.0f, "the wet reflection is wider and starts to fade");
  Check(Near(far.radius * 256.0f, 2.5f) && far.keep < middle.keep, "far blur reaches its cap as the reflection fades");
  Check(at(10.0f, 1.17f).keep == 1.0f && Near(at(29.0f, 1.17f).keep, 0.5f) && at(48.0f, 1.17f).keep == 0.0f,
        "smooth fade endpoints and midpoint");
  Check(Near(at(29.0f, 0.49f).keep, 0.75f) && Near(at(4.0f, 0.49f).radius * 256.0f, 0.407875f),
        "partial wetness ramps the effect and broadens the footprint with roughness");
  Check(at(4.0f, 0.8f).radius > near.radius && at(4.0f, 0.18f).radius == 0.0f,
        "fully active damp asphalt is rougher than the most saturated road");
  for (int depth = 2; depth <= 56; ++depth) {
    const auto prev = at(float(depth - 1), 1.17f);
    const auto next = at(float(depth), 1.17f);
    Check(next.radius >= prev.radius && next.keep <= prev.keep && next.keep >= 0.0f, "distance response is bounded and monotonic");
  }
  Check(RoadMirrorTapWeight(0) == 0.5f &&
        RoadMirrorTapWeight(-1) == 0.25f && RoadMirrorTapWeight(1) == 0.25f,
        "separable filter preserves constant colour and has no directional bias");
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

void TestEdits(const sp::VariantDef& road) {
  using sp::textedit::CountOccurrences;
  constexpr std::string_view preamble =
      "#ifndef __spirv__\n[shader(\"pixel\")]\n#endif\nvoid main(in float4 iPos : SV_Position) {\n"
      "CubeMapData cubeMapData = (CubeMapData)0;\n";
  constexpr std::string_view fetch =
      "tfetch2D(ReflectionSampler_Texture2DDescriptorIndex, ReflectionSampler_SamplerDescriptorIndex, r2.xy, float2(0, 0))";
  constexpr std::string_view mix = "r5.w = r5.z * gReflectionParams.y;";
  const std::string fixture = std::string(preamble) + std::string(mix) + "\nfloat gbMix = r5.w;\nreflect = " + std::string(fetch) + ";\n}";
  const sp::VariantShader night{"xCityRoad__PSMultiLight", 0};
  const auto transform = [&](std::string_view text) {
    return road.transform(night, text, sp::SoloContext(road.feature));
  };
  const auto edit = transform(fixture);
  Check(edit.ok && !edit.skip && CountOccurrences(edit.hlsl, "RoadMirrorSample(") == 2 && !edit.hlsl.contains(fetch),
        "one filtered lookup and no original planar lookup");
  Check(edit.hlsl.contains("RoadMirrorParams(iPos.w, gReflectionParams.x)") &&
        edit.hlsl.contains("r5.w *= roadMirrorFilter.keep;\nfloat gbMix = r5.w;"),
        "the distance and wetness inputs feed the filter and the lit-weight capture sees the faded mix");
  Check(edit.hlsl.contains("float2 tap = clamp(uv + float2(x, y) * radius, edge, 1.0f - edge);") &&
        edit.hlsl.contains("tfetch2DLod(image, samplerIndex, tap, float2(0, 0), 0.0f)") &&
        edit.hlsl.contains("RoadMirrorTapWeight(x) * RoadMirrorTapWeight(y)"),
        "the separable filter reads mip zero and keeps taps inside the resolve");
  Check(!transform(edit.hlsl).ok, "applying the variant twice fails");
  for (const std::string_view anchor : {preamble, mix, fetch}) {
    const size_t pos = fixture.find(anchor);
    std::string missing = fixture;
    missing.erase(pos, anchor.size());
    Check(!transform(missing).ok, "a missing anchor is refused");
    Check(!transform(fixture + std::string(anchor)).ok, "a duplicate anchor is refused");
  }
  const auto glass = road.transform({"xCityWindowLower__PSMultiLight", 0}, fixture, sp::SoloContext(road.feature));
  Check(!glass.ok && road.pixel_shaders.size() == 4 && road.needs_rpf && road.input_xxh3 != 0, "only four RPF road shaders are targeted");
}

void TestRetail(const sp::VariantDef& road, const fs::path& city, const fs::path& helper, const fs::path& out) {
  const auto gb = Read(helper);
  Check(!gb.empty(), "read the G-buffer helper");
  if (gb.empty()) return;
  fs::create_directories(out);
  const sp::VariantDef stages[]{road, sp::MakeGBufferVariantFromText(gb), sp::MakeHeadlightVariant()};
  const uint32_t blur = chain::FeatureBit(sp::kFeatureRoadMirrorBlur);
  const uint32_t gbuffer = chain::FeatureBit(sp::kFeatureGBuffer);
  const uint32_t headlight = chain::FeatureBit(sp::kFeatureHeadlight);
  for (const sp::VariantShader& shader : road.pixel_shaders) {
    const std::string name(shader.name);
    const auto hlsl = Read(city / (name + ".hlsl"));
    Check(!hlsl.empty(), name + " retail HLSL");
    if (hlsl.empty()) continue;
    const bool night = shader.name.ends_with("__PSMultiLight");
    const uint32_t available = night ? blur | gbuffer | headlight : blur | gbuffer;
    const auto plan = chain::Permutations(available, true);
    Check(plan.dropped == 0 && plan.sets.size() == (night ? 7u : 3u), name + " fits all compositions in the variant word budget");
    std::vector<uint32_t> words;
    for (const uint32_t features : plan.sets) {
      const uint32_t word = chain::Word(features);
      words.push_back(word);
      if ((features & blur) == 0) continue;
      const auto edit = sp::ComposeStages(stages, features, shader, hlsl);
      Check(edit.ok, name + " word " + std::to_string(word) + " " + edit.error);
      if (!edit.ok) continue;
      if (night && (features & gbuffer) != 0) {
        Check(edit.hlsl.contains("r5.w *= roadMirrorFilter.keep;\n\tfloat gbMix = r5.w;"), name + " fades before capturing gbMix");
      }
      std::ofstream output(out / (name + "." + std::to_string(word) + ".hlsl"), std::ios::binary);
      output << edit.hlsl;
      Check(bool(output), name + " wrote transformed text for DXC");
    }
    Check(chain::Pick(words, available, 0) == chain::Word(available) &&
              chain::Pick(words, available & ~blur, 0) == chain::Word(available & ~blur),
          name + " picks the existing composition when the toggle is off");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const auto road = sp::MakeRoadMirrorBlurVariant();
  TestFilter();
  TestEdits(road);
  if (argc == 4) {
    TestRetail(road, argv[1], argv[2], argv[3]);
  } else if (argc != 1) {
    std::fprintf(stderr, "usage: road_mirror_blur_test [city-hlsl gbuffer-helper scratch-output]\n");
    return 2;
  }
  std::printf("%d checks, %d failed\n", checks, failed);
  return failed == 0 ? 0 : 1;
}

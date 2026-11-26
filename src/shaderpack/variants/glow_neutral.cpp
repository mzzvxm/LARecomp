// LARecomp shader pack builder -- variant "glow_neutral": the road reflection's light glow without the colour noise of its texture
// (see glow_neutral.h).

#include "shaderpack/variants/glow_neutral.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// Correct the filtered BC1 sample in UNORM space, where the 565 error bounds apply.
// Leave the translator's Xenos PWL gamma conversion after the correction.
constexpr std::string_view kHelper = R"HLSL(float3 GlowNeutralSample(uint textureIndex, uint samplerIndex, float2 uv)
{{
    float4 texel = tfetch2D(textureIndex & ~TFETCH_GAMMA_BIT, samplerIndex, uv, float2(0, 0));
    float chroma = max(abs(texel.r - texel.g), abs(texel.b - texel.g));
    float keep = saturate((chroma - {0}.0 / 255.0) * (255.0 / {1}.0));
    texel.rgb = lerp(texel.ggg, texel.rgb, keep);
    return applyTfetchGamma(texel, textureIndex).rgb;
}}
)HLSL";

// The glow texture's fetch, whole: the edit wraps it in the helper's call.
constexpr std::string_view kFetch =
    "tfetch2D(LightGlowTexSampler_Texture2DDescriptorIndex, LightGlowTexSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyz";
constexpr std::string_view kCorrectedFetch =
    "GlowNeutralSample(LightGlowTexSampler_Texture2DDescriptorIndex, LightGlowTexSampler_SamplerDescriptorIndex, r0.xy)";

}  // namespace

VariantTransformResult TransformGlowNeutralHlsl(const VariantShader& shader, std::string_view hlsl) {
  VariantTransformResult result;
  if (shader.name != kGlowNeutralSeed.name) {
    result.error = std::format("{} is not the road reflection's light glow shader", shader.name);
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  std::string error;
  const auto fail = [&](std::string why) {
    result.ok = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(why);
    return result;
  };

  const size_t fetches = CountOccurrences(text, "tfetch2D(LightGlowTexSampler_");
  if (fetches != 1 || text.contains("GlowNeutralSample")) {
    return fail(std::format("expected one unmodified LightGlowTexSampler fetch ({} fetches)", fetches));
  }
  if (!text.contains("#define TFETCH_GAMMA_BIT ") || !text.contains("float4 applyTfetchGamma(")) {
    return fail("missing the translator's texture gamma helpers");
  }
  if (!ReplaceAllExactly(text, kFetch, kCorrectedFetch, 1, "the glow texture's fetch", &error)) {
    return fail(std::move(error));
  }

  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) {
                     const int ramp = kGlowNeutralNoneLevels - kGlowNeutralFullLevels;
                     return std::format(kHelper, kGlowNeutralFullLevels, ramp) + "\n" + std::string(group);
                   }, &error)) {
    return fail(std::move(error));
  }

  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeGlowNeutralVariant() {
  VariantDef def;
  def.feature = kFeatureGlowNeutral;
  def.name = "glow_neutral";
  def.version = kGlowNeutralVariantVersion;
  def.pixel_shaders = {kGlowNeutralSeed};
  def.needs_rpf = false;  // xrage_postfx is in the executable (the translator's xex containers), not in the RPF
  def.transform = [](const VariantShader& shader, std::string_view hlsl, const StageContext&) {
    return TransformGlowNeutralHlsl(shader, hlsl);
  };
  return def;
}

}  // namespace larecomp::shaderpack

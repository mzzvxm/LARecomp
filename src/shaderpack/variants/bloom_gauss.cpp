// LARecomp shader pack builder -- variant "bloom_gauss": the bloom's Gaussians at a render scale (see
// bloom_gauss.h).

#include "shaderpack/variants/bloom_gauss.h"

#include <charconv>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// What every one of the nine taps of a Gaussian is made of; the call is `tfetch2D(` + this + `(a, b)).xyzw;`.
constexpr std::string_view kTapArguments =
    "RenderMapBilinearSampler_Texture2DDescriptorIndex, RenderMapBilinearSampler_SamplerDescriptorIndex, "
    "r0.xy, float2(";
constexpr size_t kTapsPerShader = 9;

// The helper the nine calls go to, before main. The sub-taps are explicit level 0 fetches: the image
// has one level, and a gradient operation (tfetch2D) has no place in a loop. The loop's bound comes from
// the shared constants: the same for every pixel.
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant bloom_gauss (src/shaderpack/variants/bloom_gauss.cpp): one tap of the bloom's Gaussian at the
// render scale. The game's offsets are whole texels of the console's 320x180 image; the host makes that image
// S times larger (g_InvResolutionScale is 1 / S), so the same offsets would reach 1 / S as far on the screen.
// A tap is the area average over the S texels it stood for: its offset times S, as ceil(S) bilinear taps
// S / ceil(S) texels apart (the finer image has detail one tap would skip: a dotted comb round a small light),
// each with 1 / ceil(S) of the weight. At S = 1 it is the game's own call, unchanged. At S = 3 (4K) the three
// sub-taps are exactly one texel apart, so together they weigh four texels (1 - f, 1, 1, f) / 3 (f the position's
// fraction along the axis) and two bilinear fetches give that same sum: one over the first two texels with the
// weight (2 - f) / 3 and one over the last two with (1 + f) / 3 (at f = 0 a fetch between two texels and a fetch
// on the third). Not for a texture the game reads with its gamma bit: the curve is not linear.
float4 GaussTap(float2 axis, uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    float scale = 1.0 / dot(axis, g_InvResolutionScale);
    if (scale > 0.99 && scale < 1.01)
    {
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset);
    }
    int count = clamp(int(ceil(scale - 0.01)), 1, 8);
    if (count == 1)
    {
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset * scale);
    }
    if (count == 3 && abs(scale - 3.0) < 0.001 && (resourceDescriptorIndex & TFETCH_GAMMA_BIT) == 0)
    {
        float phase = dot(axis, getWeights2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset * scale));
        float4 first = tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset * scale + axis * (1.0 / (2.0 - phase) - phase - 1.0), 0.0);
        float4 second = tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset * scale + axis * (phase / (1.0 + phase) - phase + 1.0), 0.0);
        return first * ((2.0 - phase) * (1.0 / 3.0)) + second * ((1.0 + phase) * (1.0 / 3.0));
    }
    float spacing = scale / float(count);
    float4 sum = 0.0;
    [loop]
    for (int j = 0; j < count; ++j)
    {
        float along = (float(j) - 0.5 * float(count - 1)) * spacing;
        sum += tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset * scale + axis * along, 0.0);
    }
    return sum * (1.0 / float(count));
}
)HLSL";

bool ParseInt(std::string_view text, int* value) {
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
  if (text.empty()) return false;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), *value);
  return ec == std::errc() && end == text.data() + text.size();
}

// Every tap's offset (a, b) runs along the axis: the other component is 0. A pair the other way round
// means this is not the shader the axis was chosen for (the identity and the name disagree).
bool TapsRunAlong(std::string_view text, std::string_view needle, bool x_axis, std::string_view name,
                  std::string* error) {
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    const size_t begin = at + needle.size();
    const size_t close = text.find(')', begin);
    const size_t comma = text.find(',', begin);
    int a = 0;
    int b = 0;
    if (close == std::string_view::npos || comma == std::string_view::npos || comma > close ||
        !ParseInt(text.substr(begin, comma - begin), &a) ||
        !ParseInt(text.substr(comma + 1, close - comma - 1), &b)) {
      *error = std::format("{}: a tap's offset is not two whole numbers", name);
      return false;
    }
    if ((x_axis ? b : a) != 0) {
      *error = std::format("{}: a tap's offset ({}, {}) does not run along the {} axis", name, a, b,
                           x_axis ? "horizontal" : "vertical");
      return false;
    }
  }
  return true;
}

}  // namespace

VariantTransformResult TransformBloomGaussHlsl(const VariantShader& shader, std::string_view hlsl) {
  VariantTransformResult result;
  const bool x_axis = shader.name == kBloomGaussX.name;
  if (!x_axis && shader.name != kBloomGaussY.name) {
    result.error = std::format("{} is not one of the bloom's Gaussians", shader.name);
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  const std::string needle = "tfetch2D(" + std::string(kTapArguments);
  std::string error;

  // 1. The nine taps: each along its axis, and each now a call of the helper with the axis given.
  if (!TapsRunAlong(text, needle, x_axis, shader.name, &error)) {
    result.error = std::move(error);
    return result;
  }
  const std::string replacement =
      std::string("GaussTap(") + (x_axis ? "float2(1.0, 0.0)" : "float2(0.0, 1.0)") + ", " + std::string(kTapArguments);
  if (!ReplaceAllExactly(text, needle, replacement, kTapsPerShader, "the nine tfetch2D taps of the Gaussian",
                         &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // 2. The helper, before main (the same anchor as the HDR variant's).
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); },
                   &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // What the text must hold now: the helper (its definition) and the nine calls, and no tap of the game's left.
  const size_t helpers = CountOccurrences(text, "GaussTap(");
  const size_t old_taps = CountOccurrences(text, needle);
  if (helpers != kTapsPerShader + 1 || old_taps != 0) {
    result.error = std::format("{}: the transformed HLSL has {} GaussTap( and {} of the game's taps (need {} and 0)",
                               shader.name, helpers, old_taps, kTapsPerShader + 1);
    return result;
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeBloomGaussVariant() {
  VariantDef def;
  def.id = kVariantBloomGauss;
  def.name = "bloom_gauss";
  def.version = kBloomGaussVariantVersion;
  def.pixel_shaders = {kBloomGaussX, kBloomGaussY};
  def.transform = [](const VariantShader& shader, std::string_view hlsl) {
    return TransformBloomGaussHlsl(shader, hlsl);
  };
  return def;
}

}  // namespace larecomp::shaderpack

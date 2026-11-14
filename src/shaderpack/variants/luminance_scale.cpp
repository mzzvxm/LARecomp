// LARecomp shader pack builder -- variant "luminance_scale": the auto-exposure's sample at a render scale (see
// luminance_scale.h).

#include "shaderpack/variants/luminance_scale.h"

#include <array>
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

// What each of the nine taps is made of; the call is `tfetch2D(` + this + `(a, b)).xyz;`.
constexpr std::string_view kTapArguments =
    "ToneMapSampler_Texture2DDescriptorIndex, ToneMapSampler_SamplerDescriptorIndex, r0.xy, float2(";
constexpr size_t kTaps = 9;

// The helper the nine calls go to, before main. The scale is the texture's size over the guest's 320x180 (P6's own
// target is the guest's size, so g_InvResolutionScale says 1). Both axes at 1 (within 1%, or below): the game's
// own call. Otherwise the tap is the average of the S x S texels it stood for, as ceil(S) x ceil(S) point sub-taps
// of level 0 (the image has one level, and a gradient operation has no place in a loop) spaced S / ceil(S)
// texels around the game's offset times S.
constexpr std::string_view kHelper =
    R"HLSL(// LARecomp variant luminance_scale (src/shaderpack/variants/luminance_scale.cpp): one tap of the auto-exposure's sample
// as the average of the screen area it stands for. The game's offsets are whole texels of the console's 320x180 image;
// the host makes that image S times larger (its size over 320x180: the draw's own target is the guest's size, so
// the draw's scale constant says 1), so the same nine taps would cover 1 / S^2 of the area they cover at 1x and a
// small bright light would enter and leave the sample. Here a tap is the S x S texels it stood for, centred at its offset
// times S, read as ceil(S) x ceil(S) point sub-taps S / ceil(S) texels apart and averaged; the game's code goes on with
// the colour as before (luminance, log, mean of nine). At scale 1 (within 1%, or below) it is the game's own call.
float4 LuminanceTap(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    float2 size = float2(getTexture2DDimensions(g_Texture2DDescriptorHeap[TFETCH_INDEX(resourceDescriptorIndex)]));
    float2 scale = size * float2(1.0 / 320.0, 1.0 / 180.0);
    if (scale.x < 1.01 && scale.y < 1.01)
    {
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset);
    }
    scale = float2(scale.x > 1.01 ? scale.x : 1.0, scale.y > 1.01 ? scale.y : 1.0);
    int2 count = clamp(int2(ceil(scale - 0.01)), int2(1, 1), int2(4, 4));
    float2 spacing = scale / float2(count);
    float2 centre = offset * scale;
    float4 sum = 0.0;
    [loop]
    for (int j = 0; j < count.y; ++j)
    {
        [loop]
        for (int i = 0; i < count.x; ++i)
        {
            float2 sub = (float2(float(i), float(j)) - 0.5 * float2(count - int2(1, 1))) * spacing;
            sum += tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, centre + sub, 0.0);
        }
    }
    return sum * (1.0 / float(count.x * count.y));
}
)HLSL";

bool ParseInt(std::string_view text, int* value) {
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
  if (text.empty()) return false;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), *value);
  return ec == std::errc() && end == text.data() + text.size();
}

// The nine taps' offsets must be the 3 x 3 window, each pair once, as whole numbers: anything else means this is
// not the shader the edit was written for.
bool TapsAreTheWindow(std::string_view text, std::string_view needle, std::string_view name, std::string* error) {
  std::array<int, kTaps> seen{};
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
    if (a < -1 || a > 1 || b < -1 || b > 1) {
      *error = std::format("{}: a tap's offset ({}, {}) is outside the 3 x 3 window", name, a, b);
      return false;
    }
    ++seen[size_t((a + 1) * 3 + (b + 1))];
  }
  for (const int count : seen) {
    if (count != 1) {
      *error = std::format("{}: the nine offsets are not the 3 x 3 window, each once", name);
      return false;
    }
  }
  return true;
}

}  // namespace

VariantTransformResult TransformLuminanceScaleHlsl(const VariantShader& shader, std::string_view hlsl) {
  VariantTransformResult result;
  if (shader.name != kLuminanceSample.name) {
    result.error = std::format("{} is not the auto-exposure's sample pass", shader.name);
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  const std::string needle = "tfetch2D(" + std::string(kTapArguments);
  std::string error;

  // 1. The nine taps: the 3 x 3 window, and each now a call of the helper.
  if (!TapsAreTheWindow(text, needle, shader.name, &error)) {
    result.error = std::move(error);
    return result;
  }
  const std::string replacement = "LuminanceTap(" + std::string(kTapArguments);
  if (!ReplaceAllExactly(text, needle, replacement, kTaps, "the nine tfetch2D taps of the sample", &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // 2. The helper, before main (the same anchor as the bloom's and the HDR variant's).
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); },
                   &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // What the text must hold now: the helper's definition and the nine calls, and no tap of the game's left.
  const size_t helpers = CountOccurrences(text, "LuminanceTap(");
  const size_t old_taps = CountOccurrences(text, needle);
  if (helpers != kTaps + 1 || old_taps != 0) {
    result.error = std::format("{}: the transformed HLSL has {} LuminanceTap( and {} of the game's taps (need {} and 0)",
                               shader.name, helpers, old_taps, kTaps + 1);
    return result;
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeLuminanceScaleVariant() {
  VariantDef def;
  def.id = kVariantLuminanceScale;
  def.name = "luminance_scale";
  def.version = kLuminanceScaleVariantVersion;
  def.pixel_shaders = {kLuminanceSample};
  def.transform = [](const VariantShader& shader, std::string_view hlsl) {
    return TransformLuminanceScaleHlsl(shader, hlsl);
  };
  return def;
}

}  // namespace larecomp::shaderpack

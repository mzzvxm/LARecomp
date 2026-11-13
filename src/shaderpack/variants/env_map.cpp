// LARecomp shader pack builder -- variant "env_map": the car reflections' environment map at its own scale (see
// env_map.h).

#include "shaderpack/variants/env_map.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// ---- xrage_fastmipmap::PSFastMipMap ---------------------------------------------------------------------

// The two lines that put the four taps at the corners of the pixel, +-0.5 TexelSize (c255.y is 0.5). The
// second one swaps the axes (.yx): its pair of components is (y, x), so the scale goes in before the swap.
constexpr std::string_view kMipLowerLeft = "r1.xw = -TexelSize.xy * c255.yy + r0.xy;";
constexpr std::string_view kMipLowerLeftScaled = "r1.xw = -(TexelSize.xy * g_InvResolutionScale) * c255.yy + r0.xy;";
constexpr std::string_view kMipUpperRight = "r1.yz = TexelSize.yx * c255.yy + r0.yx;";
constexpr std::string_view kMipUpperRightScaled = "r1.yz = (TexelSize.xy * g_InvResolutionScale).yx * c255.yy + r0.yx;";
// The four taps, all with no immediate offset: if the translator wrote another shape this is not the
// shader the edit was written for.
constexpr std::string_view kMipTap =
    "tfetch2D(BaseSampler_Texture2DDescriptorIndex, BaseSampler_SamplerDescriptorIndex, r1.";
constexpr std::string_view kMipTapNoOffset = ", float2(0, 0)).xyzw;";
constexpr size_t kMipTaps = 4;

VariantTransformResult TransformMipPass(const VariantShader& shader, std::string_view hlsl) {
  VariantTransformResult result;
  std::string text = NormalizeNewlines(hlsl);
  std::string error;

  if (CountOccurrences(text, kMipTap) != kMipTaps || CountOccurrences(text, kMipTapNoOffset) < kMipTaps) {
    result.error = std::format(
        "{}: expected {} taps of BaseSampler with no immediate offset; the HLSL is not the one this transform was "
        "written for",
        shader.name, kMipTaps);
    return result;
  }
  if (!ReplaceAllExactly(text, kMipLowerLeft, kMipLowerLeftScaled, 1, "the lower left corner of the four taps",
                         &error) ||
      !ReplaceAllExactly(text, kMipUpperRight, kMipUpperRightScaled, 1, "the upper right corner of the four taps",
                         &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  // What the text must hold now: the two scaled corners, and no corner of the game's left.
  if (CountOccurrences(text, "(TexelSize.xy * g_InvResolutionScale)") != 2 ||
      CountOccurrences(text, kMipLowerLeft) != 0 || CountOccurrences(text, kMipUpperRight) != 0) {
    result.error = std::format("{}: the transformed HLSL does not have the two scaled corners", shader.name);
    return result;
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

// ---- xSpecMapGen::PS_Blit -----------------------------------------------------------------------------

// The one tap of the reduction: the map's base level at the pixel's UV.
constexpr std::string_view kBlitTap =
    "tfetch2D(CurrentFaceSampler_Texture2DDescriptorIndex, CurrentFaceSampler_SamplerDescriptorIndex, r0.xy, "
    "float2(0, 0))";
constexpr std::string_view kBlitTapReplacement =
    "EnvReduceTap(CurrentFaceSampler_Texture2DDescriptorIndex, CurrentFaceSampler_SamplerDescriptorIndex, r0.xy)";

// The helper, before main. The scale is the width of the texture read over the game's 512 (the copy's
// own target is the game's size, so g_InvResolutionScale does not say it). n x n bilinear taps of level 0,
// spaced d host texels so that their footprint has the variance of the game's one tap, a tent one game
// texel (S host texels) wide: S^2 / 6 = d^2 (n^2 - 1) / 12 + 1 / 6 (the tent of a bilinear tap, one host
// texel wide, is the 1 / 6). At S = 1 (n = 1, d = 0) it is the game's tap at the same UV.
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant env_map (src/shaderpack/variants/env_map.cpp): the specular copy's one tap of the environment map as an
// area average at the map's scale. The game reads 2 x 2 texels of an 8 x 8 block of the 512x256 map with one bilinear
// tap; the map is S times larger (its width over 512), so the same tap would read 1 / S^2 of the block's area and the copy
// would sparkle. ceil(S)^2 bilinear taps of level 0, spaced d texels so that the footprint has the variance of the game's
// tap (a tent one game texel wide): d = sqrt(2 (S^2 - 1) / (n^2 - 1)), which is 1.41 texels at every whole scale. At S = 1
// it is the game's tap.
float4 EnvReduceTap(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord)
{
    float scale = float(getTexture2DDimensions(g_Texture2DDescriptorHeap[TFETCH_INDEX(resourceDescriptorIndex)]).x) * (1.0 / 512.0);
    int count = clamp(int(ceil(scale - 0.01)), 1, 4);
    float spacing = sqrt(2.0 * max(scale * scale - 1.0, 0.0) / float(max(count * count - 1, 1)));
    float4 sum = 0.0;
    [loop]
    for (int j = 0; j < count; ++j)
    {
        [loop]
        for (int i = 0; i < count; ++i)
        {
            float2 offset = (float2(float(i), float(j)) - 0.5 * float(count - 1)) * spacing;
            sum += tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, 0.0);
        }
    }
    return sum * (1.0 / float(count * count));
}
)HLSL";

VariantTransformResult TransformReduce(const VariantShader& shader, std::string_view hlsl) {
  VariantTransformResult result;
  std::string text = NormalizeNewlines(hlsl);
  std::string error;

  // 1. The one tap.
  if (!ReplaceAllExactly(text, kBlitTap, kBlitTapReplacement, 1, "the one tfetch2D tap of the reduction", &error)) {
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

  // What the text must hold now: the helper's definition and its one call, and the game's tap nowhere.
  if (CountOccurrences(text, "EnvReduceTap(") != 2 || CountOccurrences(text, kBlitTap) != 0) {
    result.error = std::format("{}: the transformed HLSL does not have one EnvReduceTap( call and its definition",
                               shader.name);
    return result;
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

}  // namespace

VariantTransformResult TransformEnvMapHlsl(const VariantShader& shader, std::string_view hlsl) {
  if (shader.name == kEnvMipPass.name) return TransformMipPass(shader, hlsl);
  if (shader.name == kEnvReduce.name) return TransformReduce(shader, hlsl);
  VariantTransformResult result;
  result.error = std::format("{} is not one of the environment map's shaders", shader.name);
  return result;
}

VariantDef MakeEnvMapVariant() {
  VariantDef def;
  def.id = kVariantEnvMap;
  def.name = "env_map";
  def.version = kEnvMapVariantVersion;
  def.needs_rpf = true;  // xSpecMapGen::PS_Blit lives in the RPF (cars); the mip pass is in the executable
  def.pixel_shaders = {kEnvMipPass, kEnvReduce};
  def.transform = [](const VariantShader& shader, std::string_view hlsl) {
    return TransformEnvMapHlsl(shader, hlsl);
  };
  return def;
}

}  // namespace larecomp::shaderpack

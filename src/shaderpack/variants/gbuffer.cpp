// LARecomp shader pack builder -- variant "gbuffer": a lit pixel shader that also writes the G-buffer's normal (see gbuffer.h).

#include "shaderpack/variants/gbuffer.h"

#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/hash.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"

#ifdef LARECOMP_GBUFFER_HLSLI_EMBEDDED
namespace {
// src/gpu/hlsl/gbuffer_common.hlsli, turned into bytes by CMake (src/shaderpack/CMakeLists.txt), with a closing 0 that is not part of the file.
const unsigned char kEmbeddedGBufferHelper[] = {
#include "gbuffer_hlsli_bytes.inc"
};
}  // namespace
#endif

namespace larecomp::shaderpack {

using namespace textedit;

VariantTransformResult TransformGBufferHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx) {
  VariantTransformResult result;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.skip = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(message);
    return result;
  };
  if (shader.name != kGBufferDayShader && shader.name != kGBufferNightShader) {
    result.skip = true;
    result.error = "not a shader the G-buffer variant is for";
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  std::string helper = NormalizeNewlines(hlsli_text);
  while (!helper.empty() && IsSpace(helper.back())) helper.pop_back();
  if (helper.empty()) return fail("the helper text (gbuffer_common.hlsli) is empty");
  std::string error;

  // The normal is the interpolant TEXCOORD0's xyz in both shaders (gbuffer.h): the declaration must be there, once, as the translator makes it.
  if (CountOccurrences(text, "in centroid float4 iTexCoord0 : TEXCOORD0") != 1) {
    return fail("the pixel shader does not declare `in centroid float4 iTexCoord0 : TEXCOORD0` once: not the translator's HLSL this was written for");
  }

  // 1. The second output, after the game's colour output (before the SV_Coverage of the alpha test spec variant).
  static constexpr Piece kOutput[] = {Lit("\tout float4 oC0 : SV_Target0")};
  if (!ReplaceOnce(text, kOutput, 0, 0, "oC0 : SV_Target0 output",
                   [](std::string_view group) { return std::string(group) + "\n\t, out float2 oC1 : SV_Target1"; }, &error)) {
    return fail(std::move(error));
  }

  // 2. The block of constants (48 bytes: gbuffer_types.h GBufferConstants), declared at the place the chain gives it: byte 608 alone (c38 to c40), after the blocks of the
  // stages before it in a composition (the headlight's 672 bytes come first: variant_chain.h, so 1280 there).
  static constexpr ConstantMember kMembers[] = {{"uint4", "g_GBufferMode"}, {"float4", "g_GBufferRay"}, {"float4", "g_GBufferCam"}};
  if (!DeclareConstants(text, ctx, "G-buffer variant (src/gpu/gbuffer_types.h GBufferConstants): 48 bytes after the 608.", kMembers, &error)) {
    return fail(std::move(error));
  }

  // 3. The helper, before main.
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0), Lit("#endif"), Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [&](std::string_view group) { return helper + "\n\n" + std::string(group); }, &error)) {
    return fail(std::move(error));
  }

  // 4. The write, after the game's last statement of the colour (so after the alpha test's discard).
  static constexpr Piece kWrite[] = {Lit("oC0 = applyBlendPremult(oC0);")};
  if (!ReplaceOnce(text, kWrite, 0, 0, "the game's colour write",
                   [](std::string_view group) {
                     return std::string(group) +
                            "\n\toC1 = float2(0.0f, 0.0f);\n"
                            "\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n"
                            "\t\toC1 = GbRt1Encode(iTexCoord0.xyz);\n"
                            "\t}";
                   },
                   &error)) {
    return fail(std::move(error));
  }

  // What the text must hold now: one second output, one call, the helper once, one declaration of the block in each backend.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"oC1 : SV_Target1", 1},
      {"oC1 = GbRt1Encode(iTexCoord0.xyz);", 1},
      {"float2 GbRt1Encode(float3 n)", 1},
      {"float2 GbOctEncode(float3 n)", 1},
      {"oC0 = applyBlendPremult(oC0);", 1},
      {"\x01", 0},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      return fail(std::format("the transformed HLSL has '{}' {} times (need {})", check.needle, count, check.want));
    }
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

namespace {

// The embedded helper, or empty text when this build has none.
std::string EmbeddedHelper() {
#ifdef LARECOMP_GBUFFER_HLSLI_EMBEDDED
  return std::string(reinterpret_cast<const char*>(kEmbeddedGBufferHelper), sizeof(kEmbeddedGBufferHelper) - 1);
#else
  return std::string();
#endif
}

}  // namespace

VariantDef MakeGBufferVariantFromText(std::string_view hlsli_text) {
  VariantDef def;
  def.feature = kFeatureGBuffer;
  def.name = "gbuffer";
  def.version = kGBufferVariantVersion;
  def.needs_rpf = true;  // the xCityDefault group lives in the RPF
  // The helper is part of the variant's text: the fingerprint is of the file as the transform reads it (LF line ends), as the HDR curve's is.
  def.input_xxh3 = Xxh3(NormalizeNewlines(hlsli_text));
  def.pixel_name_suffixes = {kGBufferDayShader, kGBufferNightShader};
  def.transform = [helper = std::string(hlsli_text)](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    return TransformGBufferHlsl(shader, hlsl, helper, ctx);
  };
  return def;
}

bool MakeGBufferVariant(VariantDef* out, std::string* why) {
  const std::string helper = EmbeddedHelper();
  if (helper.empty()) {
#ifdef LARECOMP_GBUFFER_HLSLI_EMBEDDED
    *why = "the embedded gbuffer_common.hlsli is empty";
#else
    *why = "this build has no gbuffer_common.hlsli embedded (src/gpu/hlsl/gbuffer_common.hlsli was not in the source tree when CMake configured)";
#endif
    return false;
  }
  *out = MakeGBufferVariantFromText(helper);
  return true;
}

}  // namespace larecomp::shaderpack

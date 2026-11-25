// LARecomp shader pack builder -- variant "gbuffer": a lit pixel shader that also writes the G-buffer's normal and albedo (see gbuffer.h).

#include "shaderpack/variants/gbuffer.h"

#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/hash.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variants/gbuffer_cars.h"
#include "shaderpack/variants/gbuffer_day.h"
#include "shaderpack/variants/gbuffer_night.h"
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
  // The day families (the PSCity / PSProp shaders: shaderpack/variants/gbuffer_day.h, package GB-D) have their own transform, xCityDefault::PSCityDefault among them (the table row says where
  // each shader's normal is); the night family (PSMultiLight, ...Env, ...Interior: shaderpack/variants/gbuffer_night.h, package GB-C) has its own, xCityDefault::PSMultiLight among them (the
  // spike's own edits of it, which this function made before, are the night stage's text now: the stage finds the same normal, and adds RT2's albedo and lit weight, gbuffer_albedo.h).
  if (IsGBufferDayShader(shader.name)) return TransformGBufferDayHlsl(shader, hlsl, hlsli_text, ctx);
  if (IsGBufferNightShader(shader.name)) return TransformGBufferNightHlsl(shader, hlsl, hlsli_text, ctx);
  // The cars, the traffic and the characters (shaderpack/variants/gbuffer_cars.h, package GB-G1): the game's own exported world normal, the paint's register, the draw's word.
  if (IsGBufferCarShader(shader.name)) return TransformGBufferCarsHlsl(shader, hlsl, hlsli_text, ctx);
  VariantTransformResult result;
  result.skip = true;
  result.error = "not a shader the G-buffer variant is for";
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
  // The helper is part of the variant's text: the fingerprint is of the file as the transform reads it (LF line ends), as the HDR curve's is. So are the night stage's tables (the albedo recipes,
  // the baked occlusion and the reflection mix: gbuffer_night_albedo.h), which are text the transform writes into the shaders: a pack made from other rows is stale (GB-H2-REVIEW).
  def.input_xxh3 = Xxh3(NormalizeNewlines(hlsli_text) + "\n" + NightTablesFingerprintText());
  def.pixel_name_suffixes = {kGBufferDayShader, kGBufferNightShader};
  for (const std::string_view suffix : kGBufferNightSuffixes) def.pixel_name_suffixes.push_back(suffix);  // GB-C: the night family
  for (const std::string_view name : GBufferDayShaderNames()) {                                             // GB-D: the day families (kGBufferDayShader is their first row)
    if (name != kGBufferDayShader) def.pixel_name_suffixes.push_back(name);
  }
  for (const std::string_view name : GBufferCarShaderNames()) def.pixel_name_suffixes.push_back(name);  // GB-G1: the cars, the traffic and the characters (none of the names above)
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

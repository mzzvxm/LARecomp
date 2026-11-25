#include "shaderpack/variants/road_mirror_blur.h"

#include <algorithm>
#include <iterator>

#include "shaderpack/hash.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {
namespace {

struct RoadShader {
  VariantShader shader;
  std::string_view uv;
  std::string_view mix;
  std::string_view faded_mix;
};

constexpr RoadShader kRoadShaders[] = {
    {{"xCityRoad__PSCityRoad", 0xBD23D361E8AB343Aull}, "r4.xz",
     "r1.xyz = r4.yyy * r4.xzw + r1.xyz;", "r1.xyz = (r4.yyy * roadMirrorFilter.keep) * r4.xzw + r1.xyz;"},
    {{"xCityTunnelRoad__PSCityTunnelRoad", 0x8EC549775CE702DFull}, "r1.xw",
     "oC0.xyz = r2.www * r0.xyz + r4.xyz;", "oC0.xyz = (r2.www * roadMirrorFilter.keep) * r0.xyz + r4.xyz;"},
    // Keep the mix assignment before the G-buffer's gbMix capture. Deferred lights then
    // get the same lit-colour weight that the forward colour kept.
    {{"xCityRoad__PSMultiLight", 0xA59C1BB99CEA3F7Bull}, "r2.xy",
     "r5.w = r5.z * gReflectionParams.y;", "r5.w = r5.z * gReflectionParams.y;\n\tr5.w *= roadMirrorFilter.keep;"},
    {{"xCityTunnelRoad__PSMultiLight", 0x922697AD3493F601ull}, "r2.xy",
     "r5.w = r5.z * gReflectionParams.y;", "r5.w = r5.z * gReflectionParams.y;\n\tr5.w *= roadMirrorFilter.keep;"},
};

VariantTransformResult Transform(const VariantShader& shader, std::string_view hlsl, std::string_view helper) {
  using namespace textedit;
  const auto row = std::find_if(std::begin(kRoadShaders), std::end(kRoadShaders),
                                [&](const RoadShader& road) { return road.shader.name == shader.name; });
  VariantTransformResult result;
  if (row == std::end(kRoadShaders)) {
    result.error = std::string(shader.name) + ": no road reflection recipe";
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  constexpr std::string_view fetch_prefix = "tfetch2D(ReflectionSampler_";
  if (CountOccurrences(text, fetch_prefix) != 1 ||
      CountOccurrences(text, "in float4 iPos : SV_Position") != 1 ||
      CountOccurrences(text, "roadMirrorFilter") != 0) {
    result.error = std::string(shader.name) + ": expected one planar fetch, one SV_Position and unedited road code";
    return result;
  }

  constexpr std::string_view samplers = "ReflectionSampler_Texture2DDescriptorIndex, ReflectionSampler_SamplerDescriptorIndex, ";
  const std::string fetch = "tfetch2D(" + std::string(samplers) + std::string(row->uv) + ", float2(0, 0))";
  const std::string filtered = "RoadMirrorSample(" + std::string(samplers) + std::string(row->uv) + ", roadMirrorFilter)";
  constexpr std::string_view locals = "CubeMapData cubeMapData = (CubeMapData)0;";
  const std::string params = std::string(locals) +
                             "\n\tRoadMirrorFilter roadMirrorFilter = RoadMirrorParams(iPos.w, gReflectionParams.x);";
  static constexpr Piece main[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"),
                                    Ws(0), Lit("#endif"), Ws(0), Lit("void main(")};
  if (!ReplaceAllExactly(text, fetch, filtered, 1, "planar reflection lookup", &result.error) ||
      !ReplaceAllExactly(text, row->mix, row->faded_mix, 1, "planar reflection blend", &result.error) ||
      !ReplaceAllExactly(text, locals, params, 1, "road shader locals", &result.error) ||
      !ReplaceOnce(text, main, 0, 6, "pixel shader main",
                   [&](std::string_view anchor) { return std::string(helper) + "\n" + std::string(anchor); }, &result.error)) {
    result.error = std::string(shader.name) + ": " + result.error;
    return result;
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

}  // namespace

VariantDef MakeRoadMirrorBlurVariant() {
  static constexpr char helper[] = {
#include "road_mirror_hlsli_bytes.inc"
  };
  VariantDef def;
  def.feature = kFeatureRoadMirrorBlur;
  def.name = "road_mirror_blur";
  def.version = kRoadMirrorBlurVariantVersion;
  def.needs_rpf = true;
  std::string text = textedit::NormalizeNewlines(std::string_view(helper, sizeof(helper) - 1));
  def.input_xxh3 = Xxh3(text);
  for (const RoadShader& road : kRoadShaders) {
    def.pixel_shaders.push_back(road.shader);
  }
  def.transform = [text = std::move(text)](const VariantShader& shader, std::string_view hlsl, const StageContext&) {
    return Transform(shader, hlsl, text);
  };
  return def;
}

}  // namespace larecomp::shaderpack

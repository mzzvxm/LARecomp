// LARecomp shader pack builder -- the G-buffer's day world families: the pixel stage's normal sources and the vertex stage (see gbuffer_day.h).

#include "shaderpack/variants/gbuffer_day.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/hash.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

const GBufferDayShader* FindGBufferDayShader(std::string_view name) {
  for (const GBufferDayShader& row : kGBufferDayShaders) {
    if (row.name == name) return &row;
  }
  return nullptr;
}

std::vector<std::string_view> GBufferDayShaderNames() {
  std::vector<std::string_view> names;
  names.reserve(kGBufferDayShaderCount);
  for (const GBufferDayShader& row : kGBufferDayShaders) names.push_back(row.name);
  return names;
}

namespace {

// Whether this kind computes the derivative normal (and so the fragment's world position and its two derivatives, at the top of main).
bool UsesDerivative(GBufferDaySource source) { return source == GBufferDaySource::kVertexOrDerivative || source == GBufferDaySource::kDerivative; }

// The interpolant the pixel shader reads the normal from, when it reads one.
uint32_t InterpolantOf(const GBufferDayShader& shader) {
  return shader.source == GBufferDaySource::kInterpolant ? shader.interpolant : kGBufferVsInterpolant;
}

}  // namespace

std::string GBufferDayDerivativeText() {
  // `iPos` is the pixel shader's SV_Position (x, y in pixels, w = 1 / the view-space depth): GbWorldPoint of the frozen helper turns it into the point relative to the camera, in world axes.
  return "\n\tfloat3 gbP = GbWorldPoint(g_GBufferRay, g_GBufferCam, iPos.x, iPos.y, iPos.w);\n"
         "\tfloat3 gbDx = ddx(gbP);\n"
         "\tfloat3 gbDy = ddy(gbP);";
}

std::string GBufferDayWriteText(const GBufferDayShader& shader) {
  // The plane is "no data" (0) unless the draw writes the normal (kModeWriteNormal): a draw of the variant always overwrites what the sidecar held at its pixels.
  std::string text = "\n\toC1 = float2(0.0f, 0.0f);";
  switch (shader.source) {
    case GBufferDaySource::kMasked:
      // Nothing more: the card covers whatever was behind it, and the consumers fall back to the depth there.
      break;
    case GBufferDaySource::kInterpolant:
      text += std::format(
          "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {{\n"
          "\t\tfloat3 gbN = iTexCoord{0}.xyz;\n"
          "\t\tif ((g_GBufferMode.x & uint(GB_MODE_FLIP_BACKFACE)) != 0u && !bool(iFace)) {{\n"
          "\t\t\tgbN = -gbN;\n"
          "\t\t}}\n"
          "\t\toC1 = GbRt1Encode(gbN);\n"
          "\t}}",
          InterpolantOf(shader));
      break;
    case GBufferDaySource::kVertexOrDerivative:
      text += std::format(
          "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {{\n"
          "\t\tfloat3 gbN = iTexCoord{0}.xyz;\n"
          "\t\tif (dot(gbN, gbN) > 1.0e-8f) {{\n"
          "\t\t\tif ((g_GBufferMode.x & uint(GB_MODE_FLIP_BACKFACE)) != 0u && !bool(iFace)) {{\n"
          "\t\t\t\tgbN = -gbN;\n"
          "\t\t\t}}\n"
          "\t\t}} else {{\n"
          "\t\t\tgbN = GbFaceNormal(gbDx, gbDy, -gbP);\n"
          "\t\t}}\n"
          "\t\toC1 = GbRt1Encode(gbN);\n"
          "\t}}",
          InterpolantOf(shader));
      break;
    case GBufferDaySource::kDerivative:
      text +=
          "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n"
          "\t\toC1 = GbRt1Encode(GbFaceNormal(gbDx, gbDy, -gbP));\n"
          "\t}";
      break;
  }
  return text;
}

VariantTransformResult TransformGBufferDayHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx) {
  VariantTransformResult result;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.skip = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(message);
    return result;
  };
  const GBufferDayShader* row = FindGBufferDayShader(shader.name);
  if (!row) {
    result.skip = true;
    result.error = "not a shader the G-buffer's day stage is for";
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  std::string helper = NormalizeNewlines(hlsli_text);
  while (!helper.empty() && IsSpace(helper.back())) helper.pop_back();
  if (helper.empty()) return fail("the helper text (gbuffer_common.hlsli) is empty");
  std::string error;

  // What the edits stand on: the interpolant the normal is read from is declared once (the translator declares all sixteen, in every pixel shader), the screen position and the face
  // flag are there, and (derivative shaders) the line that ends the translator's declarations at the top of main, once (the derivatives go right after it).
  if (row->source == GBufferDaySource::kInterpolant || row->source == GBufferDaySource::kVertexOrDerivative) {
    const std::string declaration = std::format("in centroid float4 iTexCoord{0} : TEXCOORD{0}", InterpolantOf(*row));
    if (CountOccurrences(text, declaration) != 1) {
      return fail(std::format("the pixel shader does not declare `{}` once: not the translator's HLSL this was written for", declaration));
    }
  }
  if (row->source != GBufferDaySource::kMasked) {
    if (CountOccurrences(text, "in bool iFace : SV_IsFrontFace") != 1 || CountOccurrences(text, "in uint iFace : SV_IsFrontFace") != 1) {
      return fail("the pixel shader does not declare the front face flag `iFace : SV_IsFrontFace` once in each backend");
    }
  }
  if (UsesDerivative(row->source)) {
    if (CountOccurrences(text, "in float4 iPos : SV_Position") != 1) return fail("the pixel shader does not declare `in float4 iPos : SV_Position` once");
    if (CountOccurrences(text, "CubeMapData cubeMapData = (CubeMapData)0;") != 1) {
      return fail("the pixel shader does not have the last declaration of main `CubeMapData cubeMapData = (CubeMapData)0;` once");
    }
  }

  // 1. The second output, after the game's colour output (before the SV_Coverage of the alpha test spec variant).
  static constexpr Piece kOutput[] = {Lit("\tout float4 oC0 : SV_Target0")};
  if (!ReplaceOnce(text, kOutput, 0, 0, "oC0 : SV_Target0 output", [](std::string_view group) { return std::string(group) + "\n\t, out float2 oC1 : SV_Target1"; }, &error)) {
    return fail(std::move(error));
  }

  // 2. The block of constants (48 bytes: gbuffer_types.h GBufferConstants), at the place the chain gives it.
  static constexpr ConstantMember kMembers[] = {{"uint4", "g_GBufferMode"}, {"float4", "g_GBufferRay"}, {"float4", "g_GBufferCam"}};
  if (!DeclareConstants(text, ctx, "G-buffer variant (src/gpu/gbuffer_types.h GBufferConstants): 48 bytes after the 608.", kMembers, &error)) {
    return fail(std::move(error));
  }

  // 3. The helper, before main.
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0), Lit("#endif"), Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main", [&](std::string_view group) { return helper + "\n\n" + std::string(group); }, &error)) {
    return fail(std::move(error));
  }

  // 4. The fragment's world position and its derivatives, at the top of main: before any statement of the game's, so before its alpha test (a pixel that discards ends its invocation on
  // Vulkan, and the derivatives of its quad's other pixels would be undefined after it), and ahead of every instruction the game's code has, so the compiler keeps those in their order.
  if (UsesDerivative(row->source)) {
    static constexpr Piece kLastDeclaration[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
    if (!ReplaceOnce(text, kLastDeclaration, 0, 0, "the last declaration of main", [](std::string_view group) { return std::string(group) + GBufferDayDerivativeText(); }, &error)) {
      return fail(std::move(error));
    }
  }

  // 5. The write, after the game's last statement of the colour (so after the alpha test's discard).
  static constexpr Piece kWrite[] = {Lit("oC0 = applyBlendPremult(oC0);")};
  if (!ReplaceOnce(text, kWrite, 0, 0, "the game's colour write", [&](std::string_view group) { return std::string(group) + GBufferDayWriteText(*row); }, &error)) {
    return fail(std::move(error));
  }

  // What the text must hold now: one second output, one write, the helper once, the derivative block once or not at all, the game's colour statement once.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"oC1 : SV_Target1", 1},
      {"oC1 = float2(0.0f, 0.0f);", 1},
      {"float2 GbRt1Encode(float3 n)", 1},
      {"float3 gbP = GbWorldPoint(", UsesDerivative(row->source) ? 1u : 0u},
      {"oC1 = GbRt1Encode(", row->source == GBufferDaySource::kMasked ? 0u : 1u},
      {"oC0 = applyBlendPremult(oC0);", 1},
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

VariantDef MakeGBufferDayStageFromText(std::string_view hlsli_text) {
  VariantDef def;
  def.feature = kFeatureGBuffer;
  def.name = "gbuffer";
  def.version = kGBufferDayVersion;
  def.needs_rpf = true;
  def.input_xxh3 = Xxh3(NormalizeNewlines(hlsli_text));
  def.pixel_name_suffixes = GBufferDayShaderNames();
  def.transform = [helper = std::string(hlsli_text)](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    return TransformGBufferDayHlsl(shader, hlsl, helper, ctx);
  };
  return def;
}

// ---- the vertex stage ----------------------------------------------------------------------------------------------------------------------------

const GBufferVsShader* FindGBufferVsShader(std::string_view name) {
  for (const GBufferVsShader& row : kGBufferVsShaders) {
    if (row.name == name) return &row;
  }
  return nullptr;
}

std::string GBufferVsExportText(GBufferVsKind kind) {
  if (kind == GBufferVsKind::kCity) {
    return "\n\t// G-buffer variant (gbuffer_day.cpp): the vertex's normal, in world space, for the pixel shader's sidecar write (TEXCOORD15: no pixel shader of the game reads it)\n"
           "\toTexCoord15 = float4(tfetchR11G11B10(iNormal0).xyz, 0.0f);";
  }
  return "\n\t// G-buffer variant (gbuffer_day.cpp): the vertex's normal rotated to world space like the game's own, for the pixel shader's sidecar write (TEXCOORD15: no pixel shader of the game reads it)\n"
         "\t{\n"
         "\t\tfloat3 gbF = tfetchR11G11B10(iNormal0).xyz;\n"
         "\t\toTexCoord15 = float4(gbF.x * gWorld(0).xyz + gbF.y * gWorld(1).xyz + gbF.z * gWorld(2).xyz, 0.0f);\n"
         "\t}";
}

VariantTransformResult TransformGBufferVsHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
  VariantTransformResult result;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.skip = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(message);
    return result;
  };
  const GBufferVsShader* row = FindGBufferVsShader(shader.name);
  if (!row) {
    result.skip = true;
    result.error = "not a vertex shader the G-buffer's vertex stage is for";
    return result;
  }
  if (ctx.constants_bytes != 0) return fail("the vertex stage has no constants block of its own");
  std::string text = NormalizeNewlines(hlsl);

  // What the export stands on: the vertex format has a normal the game fetches itself, TEXCOORD15 is an output of the vertex shader that nothing writes but its zero at the top, and (props) the
  // game's own gWorld is declared.
  if (CountOccurrences(text, "in uint4 iNormal0 : NORMAL0") != 1 || CountOccurrences(text, "tfetchR11G11B10(iNormal0)") < 1) {
    return fail("the vertex shader does not fetch a normal `tfetchR11G11B10(iNormal0)` from `in uint4 iNormal0 : NORMAL0`: not the translator's HLSL this was written for");
  }
  if (CountOccurrences(text, "out float4 oTexCoord15 : TEXCOORD15") != 1 || CountOccurrences(text, "oTexCoord15 = 0.0;") != 1 || CountOccurrences(text, "oTexCoord15") != 2) {
    return fail("TEXCOORD15 is not an output that the vertex shader only sets to zero: it is not free for the normal");
  }
  if (row->kind == GBufferVsKind::kProp && CountOccurrences(text, "gWorld(0)") < 1) {
    return fail("the vertex shader does not use gWorld(0): the world matrix is not declared in it");
  }

  // The export, after the game's last statement (the position's scale and offset), before `return;`.
  // (with the tab of a statement of main: the translator's own comment about this line, in the shared header, has the same words)
  static constexpr Piece kLast[] = {Lit("\toPos.xyz = oPos.xyz * g_PosScale.xyz + g_PosOffset.xyz * oPos.w;")};
  std::string error;
  if (!ReplaceOnce(text, kLast, 0, 0, "the position's scale and offset", [&](std::string_view group) { return std::string(group) + GBufferVsExportText(row->kind); }, &error)) {
    return fail(std::move(error));
  }
  if (CountOccurrences(text, "oTexCoord15 = float4(") != 1) return fail("the export is not there once");
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeGBufferVsVariant() {
  VariantDef def;
  def.feature = kFeatureGBufferVs;
  def.name = "gbuffer_vs";
  def.version = kGBufferVsVersion;
  def.needs_rpf = true;  // the city's and the props' shaders live in the RPF
  def.vertex = true;
  for (const GBufferVsShader& row : kGBufferVsShaders) def.pixel_name_suffixes.push_back(row.name);
  def.transform = [](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) { return TransformGBufferVsHlsl(shader, hlsl, ctx); };
  return def;
}

}  // namespace larecomp::shaderpack

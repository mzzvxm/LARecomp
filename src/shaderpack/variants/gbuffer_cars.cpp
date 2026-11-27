// LARecomp shader pack builder -- the G-buffer's car, traffic and character families: the table, the transform (see gbuffer_cars.h). Package GB-G1.

#include "shaderpack/variants/gbuffer_cars.h"

#include <cctype>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/hash.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variants/gbuffer_day.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

// The census of perf/GB-G0.md, one row per lit pixel shader of the cars, the traffic and the characters of the retail translation (shaderpack/baseline/hlsl-rpf/cars and characters):
// name, where the normal comes from and its slot, the albedo source (with the sampler, the gradient fetch, the colour constants; the paint's statement and register), the material, the family.
// Generated from the numeric runs of the vertex shaders (h11-work/gb-g0-analysis/scripts/vsnorm_cars.py), the pixel shaders' declarations (census_ps.py) and, for the albedo, the Jacobians of the
// colour constants (h11-work/gb-g1-notes/jac_batch.py: oC0.rgb is linear in each, per channel) and the paint registers (paint_final.py). The class of a shader is not here: it is the host's word.
const GBufferCarShader kGBufferCarShaders[] = {
    {"xCarPaintCustomizable__PS_CarPaint", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kPaint, "", false, "", "r7.xyz = r7.xyz * r0.yyy + r3.yzx;", "float3(r7.z, r7.x, r7.y)", GBufferCarMaterial::kHost, "player/racer"},
    {"xCarPaintCustomizable__PS_HoodCarbonFiber", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kConstant, "", false, "BaseColorC.xyz", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xTexturedPlastic__PS_BumpSpec", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xBumpedAlphaChrome__PS_BumpedChrome", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xAmbientCarLight__PSAmbientCarLight", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "traffic"},
    {"xBumpSpecAlpha__PS_BumpSpec_true_true", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xBumpSpecAlpha__PS_BumpSpec_false_true", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xAnisotropicBilletMetal__PS_AnisoMetal", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xAmbientBumpSpec__PSAmbientBumpSpec", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "traffic"},
    {"xUnderbody__PS", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xrage_bump_spec_reflect__PS_Textured", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "TextureSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xAmbientVehicle__PS_AmbientVehicle", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "traffic"},
    {"xGrillOpaque__PS_BumpSpec_SpecMap", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xGrillOpaque__PS_BumpSpec_true_true", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xGrillOpaque__PS_BumpSpec_false_true", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xrage_bump_spec__PS_Textured", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "TextureSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xWheelBrakeRotor__PS_BrakeRotor", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", true, "MetalColor.xyz", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xBlackOpaqueGlass__PS_GlassFront", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xBumpSpec__PS_BumpSpec_true_true", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xChrome__PS_Chrome", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xbrakecaliper__PS_BrakeCaliper", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz * brakeCaliperTintColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xRimBadge__PS_Badge", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xrage_diffuse__PS_Textured", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "TextureSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xLicensePlate__PS_Plate", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xrage_bump_spec_alpha__PS_Textured", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "TextureSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xRimMain__PS_Multi", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xbumpedglass__PS_Light", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xCharacterSkin__PS_BumpSpec", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xCarLight__PS_Light", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xrage_spec_reflect__PS_Textured", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "TextureSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xCarPaint__PS_CarPaint", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kPaint, "", false, "", "r3.xyz = r9.xyz * r0.zzz + r3.zyx;", "r3.xyz", GBufferCarMaterial::kHost, "player/racer"},
    {"xCarPaint__PS_CarPaintLowLod", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kPaint, "", false, "", "r3.xyz = r3.xyz * r2.xxx + r2.wzy;", "r3.xyz", GBufferCarMaterial::kHost, "player/racer"},
    {"xEngineBay__PS", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xBrushedMetal__PS_BrakeRotor", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xrage_emissive__PS_Textured", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "TextureSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xBumperSubframe__PS", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xBumpedBrushedMetal__PS_BumpedBrushedmetal", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xAmbientCarPaint__PSAmbientCarPaint", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kVertex, "", false, "", "", "iTexCoord15", GBufferCarMaterial::kHost, "traffic"},
    {"xInteriorTrimAlpha__PS_BumpSpecAlpha", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xCarPaintNormalMapped__PS_CarPaintNormalMapped", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kPaint, "", false, "", "r4.xyz = r4.xyz * r0.yyy + r5.xzy;", "float3(r4.x, r4.z, r4.y)", GBufferCarMaterial::kHost, "player/racer"},
    {"xInteriorTrim__PS_BumpSpec", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xAmbientTire__PSAmbientTire", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", true, "", "", "", GBufferCarMaterial::kHost, "traffic"},
    {"xGrill__PS_BumpSpec_SpecMap", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xGrill__PS_BumpSpec_true_true", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xGrill__PS_BumpSpec_false_true", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kBumpSpec, "player/racer"},
    {"xBumpSpecSkinned__PS_BumpSpec", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xRimNormalMapped__PS_Multi", GBufferCarNormal::kInterpolant, 2, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xCharacter_skin_blendshape_normalmap__PS", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_alpha_normalmap_doublesided__PSAlphaDoubleSided", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_visor__PS", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "tintColor.xyz", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_visor_normalmap__PS", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "tintColor.xyz", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_alpha_normalmap__PSAlpha", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_skin_normalmap__PS", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_skin_blendshape__PS", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_teeth__PS", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "tintColor.xyz", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_skin_arm_normalmap__PS_HighQualitySkin", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_alpha__PSAlpha", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_teeth_normalmap__PS", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "tintColor.xyz", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_skin_arm__PS_HighQualitySkin", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter__PS", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_plastic_normalmap__PS", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "tintColor.xyz", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_alpha_doublesided__PSAlphaDoubleSided", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_normalmap__PS", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_skin__PS", GBufferCarNormal::kInterpolant, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xCharacter_plastic__PS", GBufferCarNormal::kInterpolant, 1, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "tintColor.xyz", "", "", GBufferCarMaterial::kHost, "ped/character"},
    {"xWheelRubber__PS_ParallaxRubber2_false", GBufferCarNormal::kDerivative, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", true, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xWheelRubber__PS_ParallaxRubber2_true", GBufferCarNormal::kDerivative, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xblackmatte__PS_BlackMatte", GBufferCarNormal::kDerivative, 0, GBufferCarAlbedo::kNone, "", false, "", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xCarbonFiber__PS_CarbonFiber", GBufferCarNormal::kDerivative, 0, GBufferCarAlbedo::kConstant, "", false, "BaseColorC.xyz * TintColorC.xyz", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xAmbientBlackMatte__PSAmbientBlackMatte", GBufferCarNormal::kDerivative, 0, GBufferCarAlbedo::kBlack, "", false, "", "", "", GBufferCarMaterial::kHost, "traffic"},
    {"xInteriorTexturedEmissiveAlpha__PS_EmissiveAlpha", GBufferCarNormal::kDerivative, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xInteriorTexturedEmissive__PS_Emissive", GBufferCarNormal::kDerivative, 0, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "baseColor.xyz", "", "", GBufferCarMaterial::kHost, "player/racer"},
    {"xPed__PS", GBufferCarNormal::kVertexOrDerivative, 15, GBufferCarAlbedo::kDiffuse, "DiffuseSampler", false, "", "", "", GBufferCarMaterial::kHost, "ped/character"},
};
const size_t kGBufferCarShaderCount = sizeof(kGBufferCarShaders) / sizeof(kGBufferCarShaders[0]);

// ---- the shaders that are left out, with the reason ---------------------------------------------------------------------------------------------------------------------
const GBufferCarLeftOut kGBufferCarLeftOut[] = {
    {"xCarPaintCustomizable__PS_CarPaintEnv", "draws the environment map views, never the main view: no sidecar there"},
    {"xrage_bump_spec_reflect__PS_TexturedUnlit", "unlit variant of the rage shaders (no lighting: class unlit)"},
    {"xAmbientVehicle__PSMultiLight", "the night stage owns it (GB-C: every __PSMultiLight pixel shader, which finds its own normal)"},
    {"xPowerupIceCube__PS_Shield", "power-up effect (blended)"},
    {"xPowerupIceCube__PS_IceBlock", "power-up effect (blended)"},
    {"xPowerupIceCube__PS_Stealth", "power-up effect (blended)"},
    {"xrage_bump_spec__PS_TexturedUnlit", "unlit variant of the rage shaders (no lighting: class unlit)"},
    {"xrage_diffuse__PS_TexturedUnlit", "unlit variant of the rage shaders (no lighting: class unlit)"},
    {"xrage_bump_spec_alpha__PS_TexturedUnlit", "unlit variant of the rage shaders (no lighting: class unlit)"},
    {"xrage_spec_reflect__PS_TexturedUnlit", "unlit variant of the rage shaders (no lighting: class unlit)"},
    {"xCarPaint__PS_NormalMap", "normal-map bake pass of the paint (not the main view)"},
    {"xrage_emissive__PS_TexturedUnlit", "unlit variant of the rage shaders (no lighting: class unlit)"},
    {"xCarGlass__PS_GlassFront", "blended glass"},
    {"xCarGlass__PS_InteriorGlass", "blended glass"},
    {"xCarGlass__PS_InteriorGrime", "blended glass"},
    {"xAmbientCarGlass__PSAmbientGlassFront", "blended glass"},
    {"xPowerupStealthSkinned__PS_Stealth", "power-up effect"},
    {"xTestRimBlur__PS_fixupAlpha", "test shader (alpha fix-up)"},
    {"xPed__PSMultiLight", "the night stage owns it (GB-C: every __PSMultiLight pixel shader, which finds its own normal)"},
};
const size_t kGBufferCarLeftOutCount = sizeof(kGBufferCarLeftOut) / sizeof(kGBufferCarLeftOut[0]);

const GBufferCarShader* FindGBufferCarShader(std::string_view name) {
  for (size_t i = 0; i < kGBufferCarShaderCount; ++i) {
    if (kGBufferCarShaders[i].name == name) return &kGBufferCarShaders[i];
  }
  return nullptr;
}

std::vector<std::string_view> GBufferCarShaderNames() {
  std::vector<std::string_view> names;
  names.reserve(kGBufferCarShaderCount);
  for (size_t i = 0; i < kGBufferCarShaderCount; ++i) names.push_back(kGBufferCarShaders[i].name);
  return names;
}

std::string_view GBufferCarAlbedoSource(const GBufferCarShader& row) {
  switch (row.albedo) {
    case GBufferCarAlbedo::kNone: return kPlanNone;
    case GBufferCarAlbedo::kDiffuse: return row.tint.empty() ? kPlanDiffuse : kPlanDiffuseTint;
    case GBufferCarAlbedo::kPaint: return kPlanPaint;
    case GBufferCarAlbedo::kConstant: return kPlanConstant;
    case GBufferCarAlbedo::kVertex: return kPlanVertex;
    case GBufferCarAlbedo::kBlack: return kPlanBlack;
  }
  return kPlanNone;
}

namespace {

// The kinds that compute the face's normal from the derivatives of the fragment's world point: the one that has no other normal, and the one that has the vertex stage's where it is paired.
bool UsesDerivative(const GBufferCarShader& row) { return row.normal != GBufferCarNormal::kInterpolant; }
// The kinds that read an interpolant for the normal (slot): the game's own export, or the vertex stage's.
bool ReadsSlot(const GBufferCarShader& row) { return row.normal != GBufferCarNormal::kDerivative; }

// Whether the text declares a constant of the game's by this name: the SPIR-V text has a `#define name vk::RawBufferLoad<...>`, the DXIL one a member `float4 name : packoffset(cN);` of the pixel
// shader's constants; the stage's expressions use the name as it is in both.
bool DeclaresConstant(std::string_view text, std::string_view name) {
  const std::string define = std::format("\n#define {} vk::RawBufferLoad<float4>(", name);
  const std::string member = std::format("float4 {} : packoffset(c", name);
  return CountOccurrences(text, define) == 1 && CountOccurrences(text, member) == 1;
}

// The names of the constants of a product `baseColor.xyz * brakeCaliperTintColor.xyz`.
std::vector<std::string> ConstantsOf(std::string_view product) {
  std::vector<std::string> names;
  size_t at = 0;
  while (at < product.size()) {
    const size_t end = product.find('.', at);
    if (end == std::string_view::npos) break;
    size_t begin = end;
    while (begin > at && (std::isalnum(static_cast<unsigned char>(product[begin - 1])) || product[begin - 1] == '_')) --begin;
    if (begin < end) names.emplace_back(product.substr(begin, end - begin));
    at = end + 1;
  }
  return names;
}

}  // namespace

GBufferCarEdits PlanGBufferCarEdits(const GBufferCarShader& row, std::string_view hlsl) {
  GBufferCarEdits out;
  const std::string text = NormalizeNewlines(hlsl);
  // the material: the shader's own smoothness constant, where the row says it has one and the text declares it
  if (row.material == GBufferCarMaterial::kBumpSpec) out.smoothness = DeclaresConstant(text, "smoothness");
  out.why_not = "no albedo source in the row";
  if (row.albedo == GBufferCarAlbedo::kNone) return out;
  // the colour constants the row multiplies in must be declared
  for (const std::string& constant : ConstantsOf(row.tint)) {
    if (!DeclaresConstant(text, constant)) {
      out.why_not = std::format("the shader does not declare the colour constant `{}` once", constant);
      return out;
    }
  }
  switch (row.albedo) {
    case GBufferCarAlbedo::kNone: break;
    case GBufferCarAlbedo::kDiffuse: {
      GBufferAlbedoSource source;
      source.sampler = row.sampler;
      source.gradient = row.gradient;
      const GBufferAlbedo albedo = AnalyseGBufferAlbedo(text, source);
      if (!albedo.found) {
        out.why_not = albedo.why_not;
        return out;
      }
      out.found = true;
      out.expression = albedo.expression;
      if (!row.tint.empty()) out.expression += " * " + std::string(row.tint);
      if (albedo.captured) {
        out.insert_at = albedo.capture_at;
        out.insert_text = albedo.capture;
      }
      out.detail = albedo.detail + (row.tint.empty() ? std::string() : " x " + std::string(row.tint));
      break;
    }
    case GBufferCarAlbedo::kPaint: {
      // the game's statement at the top level of main, once: the capture is the line after it
      const std::string line = "\n\t" + std::string(row.anchor) + "\n";
      if (CountOccurrences(text, line) != 1) {
        out.why_not = std::format("the paint's statement `{}` is not at the top level of main once: not the translator's HLSL this was written for", row.anchor);
        return out;
      }
      out.found = true;
      out.insert_at = text.find(line) + line.size();
      out.insert_text = std::format("\tfloat3 {} = {};\n", kGBufferAlbedoCapture, row.expression);
      out.expression = std::string(kGBufferAlbedoCapture);
      out.detail = std::format("after `{}`: {}", row.anchor, row.expression);
      break;
    }
    case GBufferCarAlbedo::kConstant:
      out.found = true;
      out.expression = std::string(row.tint);
      out.detail = std::string(row.tint);
      break;
    case GBufferCarAlbedo::kVertex: {
      // the interpolant the vertex stage writes the colour in (xyz) and the proof that it did (w): declared once; the albedo is written where w says the colour is there
      constexpr std::string_view kPrefix = "iTexCoord";
      if (row.expression.size() <= kPrefix.size() || row.expression.substr(0, kPrefix.size()) != kPrefix) {
        out.why_not = "the row names no interpolant `iTexCoord<k>` for the vertex stage's colour";
        return out;
      }
      const std::string declaration = std::format("in centroid float4 {} : TEXCOORD{}", row.expression, row.expression.substr(kPrefix.size()));
      if (CountOccurrences(text, declaration) != 1) {
        out.why_not = std::format("the pixel shader does not declare `{}` once: not the translator's HLSL this was written for", declaration);
        return out;
      }
      out.found = true;
      out.expression = std::string(row.expression) + ".xyz";
      out.condition = std::string(row.expression) + ".w > 0.5f";
      out.detail = std::format("{}: xyz the colour, w > 0.5 where the vertex stage wrote it", row.expression);
      break;
    }
    case GBufferCarAlbedo::kBlack:
      out.found = true;
      out.expression = "float3(0.0f, 0.0f, 0.0f)";
      out.detail = "the shader writes oC0.rgb = 0";
      break;
  }
  out.why_not.clear();
  return out;
}

std::string GBufferCarsOutputText(uint32_t planes) {
  std::string text = "\n\t, out float2 oC1 : SV_Target1";
  if (planes >= 2) text += std::string(kGBufferAlbedoOutput);
  if (planes >= 3) text += "\n\t, out float4 oC3 : SV_Target3";
  return text;
}

std::string GBufferCarsWriteText(const GBufferCarShader& row, const GBufferCarEdits& edits, uint32_t planes) {
  // Every plane is "no data" (0) unless the draw writes it (the mode bits): a draw of the variant always overwrites what the sidecar held at its pixels.
  std::string text = "\n\toC1 = float2(0.0f, 0.0f);";
  if (planes >= 2) text += std::string(kGBufferAlbedoNoData);
  if (planes >= 3) text += "\n\toC3 = float4(0.0f, 0.0f, 0.0f, 0.0f);";
  // The draw's material word (GB-F1, the host's per draw word: row 0 of the block, `y`); the xBumpSpec family's own smoothness replaces its smoothness byte.
  const bool albedo = planes >= 2 && edits.found;
  if (albedo || planes >= 3) {
    text += "\n\tuint gbMat = g_GBufferMode.y;";
    if (edits.smoothness) text += "\n\tgbMat = (gbMat & 0xFFFFFF00u) | uint(saturate(smoothness.x) * 255.0f + 0.5f);";
  }
  if (row.normal == GBufferCarNormal::kVertexOrDerivative) {
    // the vertex stage's normal where it wrote one (the game's own vertex shader leaves TEXCOORD15 at zero), else the face's: the day families' rule (gbuffer_day.cpp GBufferDayWriteText)
    text += std::format(
        "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {{\n"
        "\t\tfloat3 gbN = iTexCoord{}.xyz;\n"
        "\t\tif (dot(gbN, gbN) > 1.0e-8f) {{\n"
        "\t\t\tif ((g_GBufferMode.x & uint(GB_MODE_FLIP_BACKFACE)) != 0u && !bool(iFace)) {{\n"
        "\t\t\t\tgbN = -gbN;\n"
        "\t\t\t}}\n"
        "\t\t}} else {{\n"
        "\t\t\tgbN = GbFaceNormal(gbDx, gbDy, -gbP);\n"
        "\t\t}}\n"
        "\t\toC1 = GbRt1Encode(gbN);\n"
        "\t}}",
        row.slot);
  } else if (UsesDerivative(row)) {
    text +=
        "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {\n"
        "\t\toC1 = GbRt1Encode(GbFaceNormal(gbDx, gbDy, -gbP));\n"
        "\t}";
  } else {
    text += std::format(
        "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_NORMAL)) != 0u) {{\n"
        "\t\tfloat3 gbN = iTexCoord{}.xyz;\n"
        "\t\tif ((g_GBufferMode.x & uint(GB_MODE_FLIP_BACKFACE)) != 0u && !bool(iFace)) {{\n"
        "\t\t\tgbN = -gbN;\n"
        "\t\t}}\n"
        "\t\toC1 = GbRt1Encode(gbN);\n"
        "\t}}",
        row.slot);
  }
  if (albedo) {
    text += std::format(
        "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_ALBEDO)) != 0u{}) {{\n"
        "\t\toC2 = float4(GbAlbedoEncode({}), GbMaterialLit(gbMat));\n"
        "\t}}",
        edits.condition.empty() ? std::string() : " && " + edits.condition, edits.expression);
  }
  if (planes >= 3) {
    text +=
        "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_MATERIAL)) != 0u) {\n"
        "\t\toC3 = GbMaterialRt3(gbMat);\n"
        "\t}";
  }
  return text;
}

VariantTransformResult TransformGBufferCarsHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx, uint32_t planes) {
  VariantTransformResult result;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.skip = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(message);
    return result;
  };
  const GBufferCarShader* row = FindGBufferCarShader(shader.name);
  if (!row) {
    result.skip = true;
    result.error = "not a shader the G-buffer's car stage is for";
    return result;
  }
  if (planes < 1 || planes > 3) return fail("the stage declares 1 to 3 planes");
  std::string text = NormalizeNewlines(hlsl);
  std::string helper = NormalizeNewlines(hlsli_text);
  while (!helper.empty() && IsSpace(helper.back())) helper.pop_back();
  if (helper.empty()) return fail("the helper text (gbuffer_common.hlsli) is empty");
  std::string error;

  // What the edits stand on: the interpolant the normal is read from is declared once (the translator declares all sixteen) and so is the face flag in each backend; the screen position and
  // the last declaration of main (where the derivatives go) for the shaders with no exported normal.
  if (ReadsSlot(*row)) {
    const std::string declaration = std::format("in centroid float4 iTexCoord{0} : TEXCOORD{0}", row->slot);
    if (CountOccurrences(text, declaration) != 1) {
      return fail(std::format("the pixel shader does not declare `{}` once: not the translator's HLSL this was written for", declaration));
    }
    if (CountOccurrences(text, "in bool iFace : SV_IsFrontFace") != 1 || CountOccurrences(text, "in uint iFace : SV_IsFrontFace") != 1) {
      return fail("the pixel shader does not declare the front face flag `iFace : SV_IsFrontFace` once in each backend");
    }
  }
  if (UsesDerivative(*row)) {
    if (CountOccurrences(text, "in float4 iPos : SV_Position") != 1) return fail("the pixel shader does not declare `in float4 iPos : SV_Position` once");
    if (CountOccurrences(text, "CubeMapData cubeMapData = (CubeMapData)0;") != 1) {
      return fail("the pixel shader does not have the last declaration of main `CubeMapData cubeMapData = (CubeMapData)0;` once");
    }
  }

  // 0. The albedo and the material's own constant (offsets of the text as it is now: first). A shader whose text is not the one the albedo was written for keeps its normal and gets "no data" in
  // RT2 (the unit test fails on the retail text, so the table and the game cannot drift apart unseen).
  GBufferCarEdits edits = PlanGBufferCarEdits(*row, text);
  if (planes < 2) edits = GBufferCarEdits();
  if (edits.found && edits.insert_at != std::string::npos) text.insert(edits.insert_at, edits.insert_text);

  // 1. The outputs after the game's colour output (before the SV_Coverage of the alpha test spec variant).
  static constexpr Piece kOutput[] = {Lit("\tout float4 oC0 : SV_Target0")};
  if (!ReplaceOnce(text, kOutput, 0, 0, "oC0 : SV_Target0 output", [&](std::string_view group) { return std::string(group) + GBufferCarsOutputText(planes); }, &error)) {
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

  // 4. The derivative block (the fragment's world point and its two derivatives) at the top of main, before the game's alpha test: GB-D's text.
  if (UsesDerivative(*row)) {
    static constexpr Piece kLastDeclaration[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
    if (!ReplaceOnce(text, kLastDeclaration, 0, 0, "the last declaration of main", [](std::string_view group) { return std::string(group) + GBufferDayDerivativeText(); }, &error)) {
      return fail(std::move(error));
    }
  }

  // 5. The write, after the game's last statement of the colour (so after the alpha test's discard).
  static constexpr Piece kWrite[] = {Lit("oC0 = applyBlendPremult(oC0);")};
  if (!ReplaceOnce(text, kWrite, 0, 0, "the game's colour write", [&](std::string_view group) { return std::string(group) + GBufferCarsWriteText(*row, edits, planes); }, &error)) {
    return fail(std::move(error));
  }

  const std::string albedo_call = "oC2 = float4(GbAlbedoEncode(" + edits.expression + "), GbMaterialLit(gbMat));";
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"oC1 : SV_Target1", 1},
      {"oC2 : SV_Target2", planes >= 2 ? 1u : 0u},
      {"oC3 : SV_Target3", planes >= 3 ? 1u : 0u},
      {"oC1 = GbRt1Encode(", 1},
      {albedo_call, planes >= 2 && edits.found ? 1u : 0u},
      {"float3 gbAlbedo = ", edits.found && edits.insert_at != std::string::npos ? 1u : 0u},
      {"oC3 = GbMaterialRt3(gbMat);", planes >= 3 ? 1u : 0u},
      {"float2 GbRt1Encode(float3 n)", 1},
      {"float3 gbP = GbWorldPoint(", UsesDerivative(*row) ? 1u : 0u},
      {"oC0 = applyBlendPremult(oC0);", 1},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) return fail(std::format("the transformed HLSL has '{}' {} times (need {})", check.needle, count, check.want));
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

// ---- the car stage's rows of the G-buffer's vertex stage (GB-G2) -------------------------------------------------------------------------------------------------------------

// Both statements were found by running the retail vertex shaders on made-up vertices (h11-work/gb-g2-notes/scripts: ped_vs_check.py of GB-G0 for the pedestrian, traffic_paint_vs*.py for the paint).
const GBufferCarVsShader kGBufferCarVsShaders[] = {
    // r0.xyz after this statement is the unit world normal the game lights the pedestrian with (the bone blend, then gWorld, normalised): cos 1.0 with N.C.W in the run; r0.x is written again on the next line
    {"xPed__VSPedDay", "xPed__PS", "r0.xyz = r5.xyz * r0.xxx;", "", "r0.xyz", false, "the pedestrian's world normal (the bone blended, gWorld rotated, normalised vector the game lights with)"},
    // r1.xwy after the second `r1.xyw = r2.xzy + -r1.xwy;` that follows the (once only) `r1.xyw = saturate(r3.xzy * r1.xwy);` is the unlit paint: screen(screen(c.x * siColor0, c.y * siColor1), c.z * siColor2)
    // (c = iColor0), error 0 over 10 random vertices and vertex alphas; TEXCOORD3 is that blended again with the vertex lighting (r1 is written again at the end), so the pixel shader has no unlit colour
    {"xAmbientCarPaint__VSAmbientCarPaint", "xAmbientCarPaint__PSAmbientCarPaint", "r1.xyw = saturate(r3.xzy * r1.xwy);", "r1.xyw = r2.xzy + -r1.xwy;", "r1.xwy", true,
     "the traffic paint's colour before the vertex lighting (the per car palette mixed by the vertex colour's masks)"},
};
const size_t kGBufferCarVsShaderCount = sizeof(kGBufferCarVsShaders) / sizeof(kGBufferCarVsShaders[0]);

const GBufferCarVsShader* FindGBufferCarVsShader(std::string_view name) {
  for (size_t i = 0; i < kGBufferCarVsShaderCount; ++i) {
    if (kGBufferCarVsShaders[i].name == name) return &kGBufferCarVsShaders[i];
  }
  return nullptr;
}

std::string GBufferCarVsExportText(const GBufferCarVsShader& row) {
  return std::format(
      "\t// G-buffer variant (gbuffer_cars.cpp): {} for the sidecar write of the pixel shader (TEXCOORD15: no pixel shader of the game reads it)\n"
      "\toTexCoord15 = float4({}, {});\n",
      row.what, row.value, row.flag ? "1.0f" : "0.0f");
}

VariantTransformResult TransformGBufferCarsVsHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
  VariantTransformResult result;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.skip = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(message);
    return result;
  };
  const GBufferCarVsShader* row = FindGBufferCarVsShader(shader.name);
  if (!row) {
    result.skip = true;
    result.error = "not a vertex shader the G-buffer's car rows are for";
    return result;
  }
  if (ctx.constants_bytes != 0) return fail("the vertex stage has no constants block of its own");
  std::string text = NormalizeNewlines(hlsl);

  // What the export stands on: TEXCOORD15 is an output of the vertex shader that nothing writes but its zero at the top (the pixel shader's other readers: none)...
  if (CountOccurrences(text, "out float4 oTexCoord15 : TEXCOORD15") != 1 || CountOccurrences(text, "oTexCoord15 = 0.0;") != 1 || CountOccurrences(text, "oTexCoord15") != 2) {
    return fail("TEXCOORD15 is not an output that the vertex shader only sets to zero: it is not free for the export");
  }
  // ...and the statement the export follows is at the top level of main (one tab), once; the second statement (when the row has one) is the next one of its text after it.
  const std::string after = "\n\t" + std::string(row->after) + "\n";
  if (CountOccurrences(text, after) != 1) {
    return fail(std::format("the statement `{}` is not at the top level of main once: not the translator's HLSL this was written for", row->after));
  }
  size_t at = text.find(after) + after.size();
  if (!row->then.empty()) {
    const std::string then = "\n\t" + std::string(row->then) + "\n";
    const size_t next = text.find(then, at - 1);
    if (next == std::string::npos) return fail(std::format("the statement `{}` does not follow `{}` at the top level of main: not the translator's HLSL this was written for", row->then, row->after));
    at = next + then.size();
  }
  text.insert(at, GBufferCarVsExportText(*row));
  if (CountOccurrences(text, "oTexCoord15 = float4(") != 1) return fail("the export is not there once");
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeGBufferVsVariantAll() {
  VariantDef def = MakeGBufferVsVariant();
  for (size_t i = 0; i < kGBufferCarVsShaderCount; ++i) def.pixel_name_suffixes.push_back(kGBufferCarVsShaders[i].name);
  def.version = kGBufferVsVersion + kGBufferCarsVsVersion;
  def.transform = [day = std::move(def.transform)](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    if (IsGBufferCarVsShader(shader.name)) return TransformGBufferCarsVsHlsl(shader, hlsl, ctx);
    return day(shader, hlsl, ctx);
  };
  return def;
}

VariantDef MakeGBufferCarsStageFromText(std::string_view hlsli_text, uint32_t planes) {
  VariantDef def;
  def.feature = kFeatureGBuffer;
  def.name = "gbuffer";
  def.version = kGBufferCarsVersion;
  def.needs_rpf = true;
  def.input_xxh3 = Xxh3(NormalizeNewlines(hlsli_text));
  def.pixel_name_suffixes = GBufferCarShaderNames();
  def.transform = [helper = std::string(hlsli_text), planes](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    return TransformGBufferCarsHlsl(shader, hlsl, helper, ctx, planes);
  };
  return def;
}

}  // namespace larecomp::shaderpack

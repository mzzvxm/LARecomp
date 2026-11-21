#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the G-buffer's DAY world families (package GB-D of perf/GBUFFER-SURVEY.md 7.2)
// ===========================================================================
// The G-buffer stage (feature 10, shaderpack/variants/gbuffer.h, the frozen interface src/gpu/gbuffer_types.h and hlsl/gbuffer_common.hlsli) for the city's and the props' DAY
// pixel shaders: the 69 `PSCity*` / `PSProp*` shaders of the translator's output (the ones the night's PSMultiLight and PSMultiLightInterior replace after dark). Each is edited the
// way the spike edited xCityDefault::PSCityDefault (a second output SV_Target1 = the sidecar's RT1, the 48 byte block, the helper, a write after the game's last colour statement:
// the colour path is untouched), with the NORMAL taken from the place the shader's family has it:
//
//   kInterpolant         iTexCoord<k>.xyz is the vertex normal. 35 shaders: the 28 city ones (the city's meshes are in world space: the vertex shader exports the fetched normal as it
//                        is, `oTexCoord<k>.xyz = max(r.xyz, r.xyz)`) and 7 props (the vertex shader rotates it with gWorld and normalises it). k is per shader (0, 1 or 2). It was
//                        found by RUNNING each paired vertex shader of the retail text on a made-up vertex (a small interpreter of the translated HLSL, h11-work\gb-day-analysis\scripts:
//                        numeval.py, pairs.py, crosscheck.py) and taking the output that equals the world normal (the fetched normal, or its gWorld rotation), and by the pixel shader
//                        loading that interpolant itself. The pairing is by name (`PS<X>` with the group's `VS<X>`, the city default's with `VSCityDefaultDay`): a vertex shader of the day
//                        pass that is not in these pairs and shares a pixel shader would have to export the normal at the same k. CHECK: a coverage view of a day frame (a pixel with a
//                        normal where a shader of this table draws) confirms the pairing in play; the skinned prop vertex shaders (a bone palette the interpreter does not run) were
//                        compared by their set of written interpolants only, which is the non-skinned one's in every case. Back faces (kModeFlipBackface) are turned to face the viewer.
//   kVertexOrDerivative  the shader's vertex shader fetches a normal and exports none (the road, the tunnel road, the foliage props: 3 shaders). The pixel shader reads TEXCOORD15, which
//                        the vertex stage below writes, and takes the derivative normal where it finds zero (the game's own vertex shader: TEXCOORD15 is 0.0 there). Valid with any
//                        vertex shader, so the renderer needs no pairing to be correct, only to be smooth.
//   kDerivative          no vertex normal exists (the traffic signals' and the store interiors' quads have no normal in their vertex format): the normal is the face's, from the screen
//                        derivatives of the fragment's world position (GbWorldPoint, GbFaceNormal of the frozen helper: exact on a flat face, faceted on a round one).
//   kMasked              the shader draws cards with no normal (tree impostors, billboards): the plane gets the "no data" code (0), so a consumer falls back to the depth there and the
//                        sidecar never keeps the wall that was behind the card. (kMaskedDrawsAttach is false today, so a masked draw binds the sidecar like any draw of the variant.)
//
// RT2, THE ALBEDO AND THE LIT WEIGHT (package GB-W1, gbuffer_albedo.h): every shader of the table also declares `out float4 oC2 : SV_Target2` (gbuffer_rules.h kStagePlanes is RT1 and RT2) and writes it
// after the normal: `oC2 = float4(GbAlbedoEncode(<the shader's DiffuseSampler texel>), GbMaterialLit(<the draw's material word>))` under the mode bit GB_MODE_WRITE_ALBEDO, else the "no data" code (all
// zero). The texel is found by AnalyseGBufferAlbedo on the retail text (the fetch at the texel's centre that yields x, y and z, at the top level of main) and read at the end of main, or captured right
// after the fetch when the shader writes its register again. The shaders with no such fetch (the lamp lens and the traffic signal quads, ...) and the cards (kMasked) write the "no data" code. The edits
// only add lines, so the colour path is still the game's.
//
// The derivative shaders compute the fragment's world position and its two derivatives at the TOP of main, right after the translator's declarations (`CubeMapData cubeMapData = ...;`),
// so before the game's alpha test: a pixel that discards ends its invocation on Vulkan (OpKill), and the derivatives of its quad's other pixels would be undefined after it. (At the top they
// also leave the game's own instructions in the order the compiler gave them: the compiled-code identity check keeps its in-order match.) They depend on the host filling
// GBufferConstants::view_ray and camera_to_world; zeros there make the normal NaN, which GbRt1Encode writes as "no data". CHECK (needs a run): SV_Position.w = 1 / the view-space depth
// on both APIs, and the polarity of SV_IsFrontFace (DXIL declares it `uint`, SPIR-V `bool`; the flip tests `bool(iFace)`).
//
// WHAT IS LEFT OUT (kGBufferDayLeftOut, 27 shaders; the reasons are in the table): the nine `...Env` shaders (they draw the environment map's views, never the main view's colour),
// water (4: blended, and no vertex normal), windows, glass and decals (13: blended draws are masked by the host, Why::kMaskedBlended, so a variant would never draw), and a test shader.
// A blended family that turns out to draw opaque in some pass is one row moved from that list to the table. CHECK: perf/GBUFFER-SURVEY.md 1.2 is the evidence for "blended"; a day
// frame's family picture (the class view) shows whether any of them draws opaque.
//
// THE VERTEX STAGE (feature 12, kFeatureGBufferVs): the three vertex shaders of the kVertexOrDerivative pixel shaders also write the normal to TEXCOORD15 (`tfetchR11G11B10(iNormal0)`
// once more at the end, the props' rotated by gWorld like the game's own). It is a vertex-stage variant of the chain (variant_chain.h): its own pack entries (keys of vertex shaders) and
// the spec variant 1 twin (the packed normal). The skinned foliage vertex shader is not edited (its pixel shader takes the derivative normal with it). The renderer picks the vertex word per
// draw: Scene::GBufferKey, see INTEGRATION.
//
// WIRING (done in feat/gb-phase1, GB-INTEGRATE-1):
//   1. gbuffer.cpp's TransformGBufferHlsl sends every IsGBufferDayShader name to TransformGBufferDayHlsl (xCityDefault::PSCityDefault included: the table has it with k = 0, the spike's colour
//      path plus the back face flip), and its VariantDef lists GBufferDayShaderNames() with its other names: one stage, one VariantDef. kGBufferVariantVersion is 4 (2 = the night family, 3 =
//      the day families, 4 = RT2, the albedo and the lit weight): the pack's variants_gbuffer_version stamp line. kGBufferDayVersion below is the day stage's own number (the stage VariantDef of the tests); a change to the day
//      table or edits bumps both.
//   2. BuiltInVariants() (variant_pack.cpp) adds MakeGBufferVsVariant() (stamp lines variants_gbuffer_vs_*).
//   3. Scene::GBufferKey pairs the vertex word per draw, after the pixel word was chosen (the vertex word is the renderer's to pair: no pixel word needs it, see above): when the draw's pixel
//      word has the G-buffer's stage and its vertex shader has the word of feature kFeatureGBufferVs at the vertex shader's own spec variant, the key's vs_variant is that word.
//   4. Scene::FillVariantBlock fills GBufferConstants::view_ray and camera_to_world from Scene::FillGBufferCamera (MakeCameraRows). The camera is the previous frame's, which the G-buffer
//      reads itself at every anchor of a frame it tracks (GBuffer::OnAnchor, gbuffer_rules.h CameraKeeper), so it exists with or without a temporal upscaler (the first phase-1 run had
//      it only with one: the derivative shaders - store interiors, the traffic signals, the skinned foliage's fallback - wrote "no data" without). A frame that has none (the first,
//      one after a cutscene's or the pause world's) writes "no data" on those shaders (zeros make the normal NaN, which GbRt1Encode writes as 0). CHECK: that the anchor's c12-c14 rows
//      are the view inverse's right, up and backward as temporal::ExtractCamera takes them (the unit test cannot: the interval line "views and camera" counts the frames that had it).
//      The mode bit kModeDerivativeNormal is not read by this stage (each shader's source is fixed by its name).
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variant_pack.h"
#include "shaderpack/variants/gbuffer_albedo.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the table or the checks of the day pixel stage / of the vertex stage (a change to the first also bumps gbuffer.h's kGBufferVariantVersion, the second is its own stamp line).
// 2: RT2 (the albedo and the lit weight: the third output, gbuffer_albedo.h); every pack stamped with 1 is built again.
inline constexpr uint32_t kGBufferDayVersion = 2;
inline constexpr uint32_t kGBufferVsVersion = 1;

// The interpolant the vertex stage writes and the kVertexOrDerivative pixel shaders read: no pixel shader of the game reads TEXCOORD15 and no vertex shader of the families writes it.
inline constexpr uint32_t kGBufferVsInterpolant = 15;

enum class GBufferDaySource : uint8_t {
  kInterpolant,
  kVertexOrDerivative,
  kDerivative,
  kMasked,
};

struct GBufferDayShader {
  std::string_view name;      // the group and the entry point, as the translator names its HLSL files
  GBufferDaySource source;
  uint32_t interpolant;       // kInterpolant: k of iTexCoord<k>; else 0
  std::string_view family;    // for the coverage list
};

struct GBufferDayLeftOut {
  std::string_view name;
  std::string_view reason;
};

// ---- the table: 42 shaders edited -------------------------------------------------------------------------------------------------------------
inline constexpr GBufferDayShader kGBufferDayShaders[] = {
    // the city's lit shaders: the vertex normal is TEXCOORD0, 1 or 2
    {"xCityDefault__PSCityDefault", GBufferDaySource::kInterpolant, 0, "city default"},
    {"xCityPowerlines__PSCityDefault", GBufferDaySource::kInterpolant, 0, "city default"},
    {"xCityDefaultGrime__PSCityDefaultGrime", GBufferDaySource::kInterpolant, 0, "city grime"},
    {"xCityDefaultLOD__PSCityDefaultLOD", GBufferDaySource::kInterpolant, 0, "city LOD"},
    {"xCityBumpSpec__PSCityBumpSpec", GBufferDaySource::kInterpolant, 0, "city bump spec"},
    {"xCityGrime__PSCityGrime", GBufferDaySource::kInterpolant, 1, "city grime"},
    {"xCityGrimeMetal__PSCityGrimeMetal", GBufferDaySource::kInterpolant, 1, "city grime"},
    {"xCityGrimeMetalTint__PSCityGrimeMetalTint", GBufferDaySource::kInterpolant, 1, "city grime"},
    {"xCityGrimeTint__PSCityGrimeTint", GBufferDaySource::kInterpolant, 0, "city grime"},
    {"xCityGroundGrime__PSCityGroundGrime", GBufferDaySource::kInterpolant, 0, "city grime"},
    {"xCityGroundLOD__PSCityGroundLOD", GBufferDaySource::kInterpolant, 0, "city LOD"},
    {"xCityLOD__PSCityLOD", GBufferDaySource::kInterpolant, 0, "city LOD"},
    {"xCityNormalMap__PSCityNormalMap", GBufferDaySource::kInterpolant, 1, "city normal map"},
    {"xCityTerrain__PSCityTerrain", GBufferDaySource::kInterpolant, 1, "road, sidewalk, terrain"},
    {"xCitySidewalk__PSCitySidewalk", GBufferDaySource::kInterpolant, 2, "road, sidewalk, terrain"},
    {"xCityRiverRoad__PSCityRiverRoad", GBufferDaySource::kInterpolant, 0, "road, sidewalk, terrain"},
    {"xCityTunnelTile__PSCityTunnelTile", GBufferDaySource::kInterpolant, 1, "road, sidewalk, terrain"},
    {"xCityRoadStud__PSCityRoadStud", GBufferDaySource::kInterpolant, 0, "road, sidewalk, terrain"},
    {"xCityNeon__PSCityNeon", GBufferDaySource::kInterpolant, 0, "neon, signs"},
    {"xCityNeonTint__PSCityNeonTint", GBufferDaySource::kInterpolant, 0, "neon, signs"},
    {"xCityStoreSign__PSCityStoreSign", GBufferDaySource::kInterpolant, 0, "neon, signs"},
    {"xCityGrass__PSCityGrass", GBufferDaySource::kInterpolant, 1, "foliage"},
    {"xCityFoliage__PSCityFoliage", GBufferDaySource::kInterpolant, 1, "foliage"},
    {"xCityLampGlassTint__PSCityLampGlassTint", GBufferDaySource::kInterpolant, 0, "lamps"},
    {"xCityLampGlassTint__PSDiffuseTexture", GBufferDaySource::kInterpolant, 0, "lamps"},
    {"xCityLampGlassTint__PSMetal", GBufferDaySource::kInterpolant, 0, "lamps"},
    {"xCityLampGlassTint__PSBumpSpec", GBufferDaySource::kInterpolant, 0, "lamps"},
    {"xPhysics__PSPhysics", GBufferDaySource::kInterpolant, 1, "physics props"},
    // the props: the vertex shader rotates the normal to world space and normalises it
    {"xPropDefault__PSPropDefault", GBufferDaySource::kInterpolant, 1, "props"},
    {"xPropBumpSpec__PSPropBumpSpec", GBufferDaySource::kInterpolant, 0, "props"},
    {"xPropBumpSpecTint__PSPropBumpSpecTint", GBufferDaySource::kInterpolant, 0, "props"},
    {"xPropMetal__PSPropMetal", GBufferDaySource::kInterpolant, 1, "props"},
    {"xPropNeonTint__PSPropNeonTint", GBufferDaySource::kInterpolant, 0, "props"},
    {"xPropFoliageTrunk__PSPropFoliageTrunk", GBufferDaySource::kInterpolant, 1, "foliage"},
    {"xPropLampGlassTint__PSPropLampGlassTint", GBufferDaySource::kInterpolant, 0, "lamps"},
    // the vertex shader fetches a normal and exports none: the vertex stage's TEXCOORD15, else the derivative normal
    {"xCityRoad__PSCityRoad", GBufferDaySource::kVertexOrDerivative, 0, "road, sidewalk, terrain"},
    {"xCityTunnelRoad__PSCityTunnelRoad", GBufferDaySource::kVertexOrDerivative, 0, "road, sidewalk, terrain"},
    {"xPropFoliage__PSPropFoliage", GBufferDaySource::kVertexOrDerivative, 0, "foliage"},
    // no vertex normal exists
    {"xCityStoreInterior__PSCityStoreInterior", GBufferDaySource::kDerivative, 0, "store interiors"},
    {"xPropTrafficSignal__PSPropTrafficSignal", GBufferDaySource::kDerivative, 0, "neon, signs"},
    // cards: no normal
    {"xCityBillboard__PSCityBillboard", GBufferDaySource::kMasked, 0, "impostors, billboards"},
    {"xPropFoliageImpostor__PSPropFoliage", GBufferDaySource::kMasked, 0, "impostors, billboards"},
};
inline constexpr size_t kGBufferDayShaderCount = sizeof(kGBufferDayShaders) / sizeof(kGBufferDayShaders[0]);

// ---- the day shaders that are left out, with the reason ---------------------------------------------------------------------------------------
inline constexpr std::string_view kReasonEnv = "draws the environment map's views, never the main view's colour: the sidecar is not attached there";
inline constexpr std::string_view kReasonWater = "water: blended, no vertex normal; the reflections' phase gives it its own treatment";
inline constexpr std::string_view kReasonWindow = "glass window: a blended draw, never attached (Why::kMaskedBlended)";
inline constexpr std::string_view kReasonDecal = "decal layer: a blended draw, never attached (Why::kMaskedBlended)";
inline constexpr std::string_view kReasonGlass = "glass: a blended draw, never attached (Why::kMaskedBlended)";
inline constexpr std::string_view kReasonTest = "a test shader of the game's tools, not in a retail frame";
inline constexpr GBufferDayLeftOut kGBufferDayLeftOut[] = {
    {"xCityGrime__PSCityGrimeEnv", kReasonEnv},
    {"xCityTunnelRoad__PSCityTunnelRoadEnv", kReasonEnv},
    {"xCityRiverRoad__PSCityRiverRoadEnv", kReasonEnv},
    {"xCityGroundLOD__PSCityGroundLODEnv", kReasonEnv},
    {"xCityRoad__PSCityRoadEnv", kReasonEnv},
    {"xCityWindowLOD__PSCityWindowLODEnv", kReasonEnv},
    {"xCityLOD__PSCityLODEnv", kReasonEnv},
    {"xCityGrimeTint__PSCityGrimeTintEnv", kReasonEnv},
    {"xCityBumpSpec__PSCityBumpSpecEnv", kReasonEnv},
    {"xCityOceanWater__PSCityOceanWater", kReasonWater},
    {"xCityOceanWaterLOD__PSCityOceanWater", kReasonWater},
    {"xCityOceanShore__PSCityOceanShore", kReasonWater},
    {"xCityPondWater__PSCityPondWater", kReasonWater},
    {"xCityWindowDirt__PSCityWindowDirt", kReasonWindow},
    {"xCityWindowLower__PSCityWindowLower", kReasonWindow},
    {"xCityWindowUpper__PSCityWindowUpper", kReasonWindow},
    {"xCityWindowLOD__PSCityWindowLOD", kReasonWindow},
    {"xCityLampGlassTint__PSLowerWindow", kReasonWindow},
    {"xCityDecal__PSCityDecal", kReasonDecal},
    {"xCityGrimeDecal__PSCityGrimeDecal", kReasonDecal},
    {"xCityGrimeTintDecal__PSCityGrimeTintDecal", kReasonDecal},
    {"xCityGroundDecalGrime__PSCityGroundDecalGrime", kReasonDecal},
    {"xCityGroundGrimeDecal__PSCityGroundGrimeDecal", kReasonDecal},
    {"xCityTintBumpDecal__PSCityTintBumpDecal", kReasonDecal},
    {"xCityTintBumpDecal2__PSCityTintBumpDecal2", kReasonDecal},
    {"xPropGlass__PSPropGlass", kReasonGlass},
    {"xTestFoliage__PS", kReasonTest},
};
inline constexpr size_t kGBufferDayLeftOutCount = sizeof(kGBufferDayLeftOut) / sizeof(kGBufferDayLeftOut[0]);

// The row of a day shader, null for any other name.
const GBufferDayShader* FindGBufferDayShader(std::string_view name);
inline bool IsGBufferDayShader(std::string_view name) { return FindGBufferDayShader(name) != nullptr; }
// The names of kGBufferDayShaders, for the VariantDef that finds its shaders by name.
std::vector<std::string_view> GBufferDayShaderNames();

// The text transform of the day pixel stage (pure; the integrator's gbuffer.cpp calls it for IsGBufferDayShader names). `hlsli_text` is gbuffer_common.hlsli. `skip` for any other shader,
// an error naming the anchor when a shader's text is not the translator's one the edits were written for.
VariantTransformResult TransformGBufferDayHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx = SoloContext(kFeatureGBuffer));

// The inserted statements (public for the tests, which take them out again to check the colour path is the game's text byte for byte).
std::string GBufferDayDerivativeText();                         // after the declarations at the top of main, for the shaders that take a derivative normal
std::string GBufferDayWriteText(const GBufferDayShader& shader);  // after the game's last colour statement

// A stage for the pixel family, as a VariantDef of the G-buffer's feature (tests; the pack builder gets these names through gbuffer.cpp's one definition).
VariantDef MakeGBufferDayStageFromText(std::string_view hlsli_text);

// ---- the vertex stage ----------------------------------------------------------------------------------------------------------------------------

enum class GBufferVsKind : uint8_t {
  kCity,  // the city's vertex shaders have no gWorld: the fetched normal is a world normal
  kProp,  // the props' are in object space: n.x * gWorld(0).xyz + n.y * gWorld(1).xyz + n.z * gWorld(2).xyz, the game's own rotation
};
struct GBufferVsShader {
  std::string_view name;
  GBufferVsKind kind;
};
inline constexpr GBufferVsShader kGBufferVsShaders[] = {
    {"xCityRoad__VSCityRoad", GBufferVsKind::kCity},
    {"xCityTunnelRoad__VSCityTunnelRoad", GBufferVsKind::kCity},
    {"xPropFoliage__VSPropFoliage", GBufferVsKind::kProp},
};
inline constexpr size_t kGBufferVsShaderCount = sizeof(kGBufferVsShaders) / sizeof(kGBufferVsShaders[0]);
// The skinned vertex shaders of the props (their bone palettes would have to be applied to the normal) are not edited: the pixel shader takes the derivative normal with them.
inline constexpr std::string_view kGBufferVsLeftOutSkinned = "xPropFoliage__VSPropFoliageSkinned";

const GBufferVsShader* FindGBufferVsShader(std::string_view name);
std::string GBufferVsExportText(GBufferVsKind kind);  // after the game's last statement but the return (public for the tests)

VariantTransformResult TransformGBufferVsHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx = SoloContext(kFeatureGBufferVs));

// The vertex stage's definition: feature 12, the three vertex shaders found by name, needs the RPF (the city's shaders live there).
VariantDef MakeGBufferVsVariant();

}  // namespace larecomp::shaderpack

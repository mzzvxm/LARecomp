#pragma once
// ===========================================================================
// MCLA GPU renderer -- the G-buffer's material channel, part 1 (package GB-F1 of perf/LIGHT-UNIFY.md 11.2; perf/GBUFFER-SURVEY.md 2.4 and 5)
// ===========================================================================
// What RT3 (smoothness, class, flags) and RT2.a (the lit weight) hold for the pixels of a DRAW, before any per-pixel refinement: the draw's material word
// (gbuffer_types.h GBufferConstants::material, the layout in gbuffer_types.h; the encodings and the gloss formula in gbuffer_rules.h part 3). The shaders (GB-W2) put the
// word's bytes into the planes and multiply the smoothness by their own spec map where they have one; the per-texture table (GB-F2) will refine the word per draw.
//
// Three tables, all keyed by the game's own names so that nothing here is game content beyond what the shader sources of this repo already name (variants/gbuffer.h):
//
//   1. THE CLASS TABLE (kGroupRows): the shader group's name (the translator's `group__entry` file naming: xCityRoad, xCarPaint, xPed ...; every one of the 192 groups of
//      the retail game has a row, none is left to a default) -> the surface class, the flags (SSR eligible, vehicle). Rules on the entry's name come on top (IsAuxiliaryEntry:
//      the shadow, depth-prepass and occlusion passes are class 0; the interior light model's entry makes a plain world surface an interior one). Defaults per class
//      (DefaultSmoothness, DefaultLit) give the rest of the word: the lit weight is 0 for the emissive classes and 1 elsewhere.
//   2. THE GLOSS TABLE (kGlossRows): the 85 shaders that read the game's `SpecularExponent` as a constant, by name -> which constant bank holds it (the vertex shader's: the
//      day city shaders are lit per vertex, so the exponent is in their VERTEX shader's bank; the pixel shader's: the night shaders and the props) and the register. The
//      draw's own exponent (a number in the bank the draw is made with, c73 of VSCityDefaultDay, c101 of PSMultiLight ...) becomes the smoothness (gbuffer_rules.h part 3).
//      Registers verified against the translator's HLSL by gbuffer_material_test.cpp (given the folder of translated shaders).
//   3. THE TABLE FILE (mcla_gbuffer_materials.v1.bin): what the shader pack build writes beside the variant pack, because the runtime knows shaders by IDENTITY (a hash of the
//      microcode) and the names are only known while the pack is built from the player's own game files: one 16 byte record per shader (class, flags, default smoothness,
//      lit weight, gloss bank and register) sorted by identity. The class and gloss tables above are compiled into the BUILDER; the renderer reads the file
//      (MaterialTable). A game build that is not the retail one (names unknown) gets few or no records, and its draws the default word: world class, smoothness 0, lit.
//
// Where the word is made: Scene::FillVariantBlock's G-buffer case, per draw, from the draw's pixel and vertex shader identities and its constant banks (MaterialTable::WordFor).
//
// CHECK (nothing here was run against the game): the numbers of DefaultSmoothness, the SSR flags and the class of each group are a first reading of the group names and of the
// survey's families, not a measurement; GB-F2's census (per texture: name, hash, family, exponent, intensity) and the pictures decide. The vertex shaders' exponent is the
// Blinn-Phong power itself (VSCityDefaultDay: exp2(SpecularExponent.x * log2(N.H))); the pixel shaders' is read as half the power (kGlossPowerScale).
//
// Pure: no Plume, no renderer, no I/O (MaterialTable::LoadFile and EnsureLoaded are in gbuffer_material.cpp), so the shader pack builder (src/shaderpack) includes this header too
// and the tests build alone. Render thread for MaterialTable; the rest is pure.
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gbuffer_rules.h"

namespace mcla::gpu::gbuffer {

// Hand bumped when a table, a default or a rule below changes what a shader's record says (the builder's stamp line variants_gbuffer_material_version rebuilds the file; the
// reader refuses a file with another number).
inline constexpr uint32_t kMaterialRulesVersion = 1;

// ---------------------------------------------------------------------------
// The defaults of a class
// ---------------------------------------------------------------------------

// The smoothness of a pixel of a class when the draw has no exponent of its own (0 rough .. 1 mirror, the perceptual scale of gbuffer_rules.h part 3). First values (CHECK).
constexpr float DefaultSmoothness(SurfaceClass cls) {
  switch (cls) {
    case SurfaceClass::kNone: return 0.0f;
    case SurfaceClass::kWorld: return 0.15f;      // plaster, brick, painted metal sheets: mostly matte
    case SurfaceClass::kRoad: return 0.30f;       // dry asphalt: a faint sheen, the puddles are the wet road's
    case SurfaceClass::kWetRoad: return 0.90f;
    case SurfaceClass::kSidewalk: return 0.20f;
    case SurfaceClass::kTerrain: return 0.05f;
    case SurfaceClass::kLod: return 0.0f;
    case SurfaceClass::kFoliage: return 0.10f;
    case SurfaceClass::kPaint: return 0.85f;      // clear coat
    case SurfaceClass::kChrome: return 0.95f;
    case SurfaceClass::kMetal: return 0.70f;
    case SurfaceClass::kGlass: return 0.95f;
    case SurfaceClass::kSkin: return 0.20f;
    case SurfaceClass::kEmissive: return 0.0f;
    case SurfaceClass::kInterior: return 0.20f;
    case SurfaceClass::kWater: return 0.95f;
    case SurfaceClass::kCount: break;
  }
  return 0.0f;
}
// The lit weight (RT2.a): 0 for what lights itself (the deferred lights add nothing there), 1 elsewhere.
constexpr float DefaultLit(SurfaceClass cls) { return cls == SurfaceClass::kEmissive ? 0.0f : 1.0f; }

// ---------------------------------------------------------------------------
// 1. The class table: by the shader's group name
// ---------------------------------------------------------------------------

struct GroupRow {
  std::string_view group;
  SurfaceClass cls = SurfaceClass::kNone;
  uint32_t flags = 0;  // kMaterialFlagSsr | kMaterialFlagVehicle (kMaterialFlagAlphaCovered is per draw: its shader is the alpha test twin)
};

// Every shader group of the retail game (the pack's categories city, cars, characters, effects, ui and the executable's own), one row each, in the order of the families.
// "Class 0" rows are the groups the G-buffer never writes (the sky, the shadow and depth passes, particles, light volumes, the interface, the post effects, the generators): they
// are in the table so that "every group has a row" can be tested, and a shader of one that somehow carries the sidecar is written as "no data".
inline constexpr GroupRow kGroupRows[] = {
    // the city: walls, props and their decal layers
    {"xCityBillboard", SurfaceClass::kWorld, 0},
    {"xCityBumpSpec", SurfaceClass::kWorld, 0},
    {"xCityDecal", SurfaceClass::kWorld, 0},
    {"xCityDefault", SurfaceClass::kWorld, 0},
    {"xCityDefaultGrime", SurfaceClass::kWorld, 0},
    {"xCityGrime", SurfaceClass::kWorld, 0},
    {"xCityGrimeDecal", SurfaceClass::kWorld, 0},
    {"xCityGrimeTint", SurfaceClass::kWorld, 0},
    {"xCityGrimeTintDecal", SurfaceClass::kWorld, 0},
    {"xCityNormalMap", SurfaceClass::kWorld, 0},
    {"xCityPowerlines", SurfaceClass::kWorld, 0},
    {"xCityTintBumpDecal", SurfaceClass::kWorld, 0},
    {"xCityTintBumpDecal2", SurfaceClass::kWorld, 0},
    {"xPropBumpSpec", SurfaceClass::kWorld, 0},
    {"xPropBumpSpecTint", SurfaceClass::kWorld, 0},
    {"xPropDefault", SurfaceClass::kWorld, 0},
    // tiles (a wet floor shows reflections)
    {"xCityTunnelTile", SurfaceClass::kWorld, kMaterialFlagSsr},
    // far geometry: a coarse normal
    {"xCityDefaultLOD", SurfaceClass::kLod, 0},
    {"xCityGroundLOD", SurfaceClass::kLod, 0},
    {"xCityLOD", SurfaceClass::kLod, 0},
    {"xCityWindowLOD", SurfaceClass::kLod, 0},
    // asphalt: the puddle mask and the planar reflection are the game's; SSR eligible (wetness gates it)
    {"xCityGarageFloor", SurfaceClass::kRoad, kMaterialFlagSsr},
    {"xCityRiverRoad", SurfaceClass::kRoad, kMaterialFlagSsr},
    {"xCityRoad", SurfaceClass::kRoad, kMaterialFlagSsr},
    {"xCityRoadStud", SurfaceClass::kRoad, kMaterialFlagSsr},
    {"xCityTunnelRoad", SurfaceClass::kRoad, kMaterialFlagSsr},
    // sidewalks
    {"xCitySidewalk", SurfaceClass::kSidewalk, kMaterialFlagSsr},
    // ground
    {"xCityGroundDecalGrime", SurfaceClass::kTerrain, 0},
    {"xCityGroundGrime", SurfaceClass::kTerrain, 0},
    {"xCityGroundGrimeDecal", SurfaceClass::kTerrain, 0},
    {"xCityTerrain", SurfaceClass::kTerrain, 0},
    // leaves, grass, trunks, tree cards
    {"xCityFoliage", SurfaceClass::kFoliage, 0},
    {"xCityGrass", SurfaceClass::kFoliage, 0},
    {"xPropFoliage", SurfaceClass::kFoliage, 0},
    {"xPropFoliageImpostor", SurfaceClass::kFoliage, 0},
    {"xPropFoliageTrunk", SurfaceClass::kFoliage, 0},
    {"xTestFoliage", SurfaceClass::kFoliage, 0},
    // metal props and metal grime layers
    {"xCityGrimeMetal", SurfaceClass::kMetal, kMaterialFlagSsr},
    {"xCityGrimeMetalTint", SurfaceClass::kMetal, kMaterialFlagSsr},
    {"xPropMetal", SurfaceClass::kMetal, kMaterialFlagSsr},
    // windows and glass props
    {"xCityWindowDirt", SurfaceClass::kGlass, kMaterialFlagSsr},
    {"xCityWindowLower", SurfaceClass::kGlass, kMaterialFlagSsr},
    {"xCityWindowUpper", SurfaceClass::kGlass, kMaterialFlagSsr},
    {"xPropBrokenGlass", SurfaceClass::kGlass, kMaterialFlagSsr},
    {"xPropGlass", SurfaceClass::kGlass, kMaterialFlagSsr},
    {"xglass_break", SurfaceClass::kGlass, kMaterialFlagSsr},
    // emissive: neon, signs, lamp glass, signals (the lit weight is 0)
    {"xCityLampGlassTint", SurfaceClass::kEmissive, 0},
    {"xCityNeon", SurfaceClass::kEmissive, 0},
    {"xCityNeonTint", SurfaceClass::kEmissive, 0},
    {"xCityStoreSign", SurfaceClass::kEmissive, 0},
    {"xInteriorTexturedEmissive", SurfaceClass::kEmissive, 0},
    {"xInteriorTexturedEmissiveAlpha", SurfaceClass::kEmissive, 0},
    {"xPropLampGlassTint", SurfaceClass::kEmissive, 0},
    {"xPropNeonTint", SurfaceClass::kEmissive, 0},
    {"xPropTrafficSignal", SurfaceClass::kEmissive, 0},
    // store interiors
    {"xCityStoreInterior", SurfaceClass::kInterior, 0},
    // water
    {"xCityOceanShore", SurfaceClass::kWater, kMaterialFlagSsr},
    {"xCityOceanWater", SurfaceClass::kWater, kMaterialFlagSsr},
    {"xCityOceanWaterLOD", SurfaceClass::kWater, kMaterialFlagSsr},
    {"xCityPondWater", SurfaceClass::kWater, kMaterialFlagSsr},
    // car paint, the traffic cars' bodies (the game's own environment reflections stay: no SSR)
    {"xAmbientCarPaint", SurfaceClass::kPaint, kMaterialFlagVehicle},
    {"xAmbientBumpSpec", SurfaceClass::kPaint, kMaterialFlagVehicle},
    {"xAmbientVehicle", SurfaceClass::kPaint, kMaterialFlagVehicle},
    {"xCarPaint", SurfaceClass::kPaint, kMaterialFlagVehicle},
    {"xCarPaintCustomizable", SurfaceClass::kPaint, kMaterialFlagVehicle},
    {"xCarPaintNormalMapped", SurfaceClass::kPaint, kMaterialFlagVehicle},
    {"xCarbonFiber", SurfaceClass::kPaint, kMaterialFlagVehicle},
    // chrome, rims and bright trim (F0 = albedo; the game's environment reflections stay: no SSR)
    {"xBumpedAlphaChrome", SurfaceClass::kChrome, kMaterialFlagVehicle},
    {"xChrome", SurfaceClass::kChrome, kMaterialFlagVehicle},
    {"xRimBadge", SurfaceClass::kChrome, kMaterialFlagVehicle},
    {"xRimMain", SurfaceClass::kChrome, kMaterialFlagVehicle},
    {"xRimNormalMapped", SurfaceClass::kChrome, kMaterialFlagVehicle},
    // brushed and billet metal, rotors, calipers
    {"xAnisotropicBilletMetal", SurfaceClass::kMetal, kMaterialFlagVehicle},
    {"xBrushedMetal", SurfaceClass::kMetal, kMaterialFlagVehicle},
    {"xBumpedBrushedMetal", SurfaceClass::kMetal, kMaterialFlagVehicle},
    {"xbrakecaliper", SurfaceClass::kMetal, kMaterialFlagVehicle},
    {"xWheelBrakeRotor", SurfaceClass::kMetal, kMaterialFlagVehicle},
    // car glass and the dark glass drawn opaque
    {"xAmbientCarGlass", SurfaceClass::kGlass, kMaterialFlagVehicle},
    {"xBlackOpaqueGlass", SurfaceClass::kGlass, kMaterialFlagVehicle},
    {"xCarGlass", SurfaceClass::kGlass, kMaterialFlagVehicle},
    {"xbumpedglass", SurfaceClass::kGlass, kMaterialFlagVehicle},
    // car lights: the lens is emissive
    {"xAmbientCarLight", SurfaceClass::kEmissive, kMaterialFlagVehicle},
    {"xCarLight", SurfaceClass::kEmissive, kMaterialFlagVehicle},
    {"xrage_emissive", SurfaceClass::kEmissive, kMaterialFlagVehicle},
    // car body parts, trim, rubber, plates (no better class: the world's, flagged as a vehicle)
    {"xAmbientBlackMatte", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xAmbientTire", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xBumpSpec", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xBumpSpecAlpha", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xBumpSpecSkinned", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xBumperSubframe", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xEngineBay", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xGrill", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xGrillOpaque", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xInteriorTrim", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xInteriorTrimAlpha", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xLicensePlate", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xTexturedPlastic", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xUnderbody", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xWheelRubber", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xblackmatte", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xrage_bump_spec", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xrage_bump_spec_alpha", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xrage_bump_spec_reflect", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xrage_diffuse", SurfaceClass::kWorld, kMaterialFlagVehicle},
    {"xrage_spec_reflect", SurfaceClass::kWorld, kMaterialFlagVehicle},
    // pedestrians and characters
    {"xCharacter", SurfaceClass::kSkin, 0},
    {"xCharacter_alpha", SurfaceClass::kSkin, 0},
    {"xCharacter_alpha_doublesided", SurfaceClass::kSkin, 0},
    {"xCharacter_alpha_normalmap", SurfaceClass::kSkin, 0},
    {"xCharacter_alpha_normalmap_doublesided", SurfaceClass::kSkin, 0},
    {"xCharacter_normalmap", SurfaceClass::kSkin, 0},
    {"xCharacter_skin", SurfaceClass::kSkin, 0},
    {"xCharacter_skin_arm", SurfaceClass::kSkin, 0},
    {"xCharacter_skin_arm_normalmap", SurfaceClass::kSkin, 0},
    {"xCharacter_skin_blendshape", SurfaceClass::kSkin, 0},
    {"xCharacter_skin_blendshape_normalmap", SurfaceClass::kSkin, 0},
    {"xCharacter_skin_normalmap", SurfaceClass::kSkin, 0},
    {"xCharacter_teeth", SurfaceClass::kSkin, 0},
    {"xCharacter_teeth_normalmap", SurfaceClass::kSkin, 0},
    {"xPed", SurfaceClass::kSkin, 0},
    // the driver (a character shader of the car set)
    {"xCharacterSkin", SurfaceClass::kSkin, kMaterialFlagVehicle},
    // characters' plastic
    {"xCharacter_plastic", SurfaceClass::kWorld, 0},
    {"xCharacter_plastic_normalmap", SurfaceClass::kWorld, 0},
    // visors
    {"xCharacter_visor", SurfaceClass::kGlass, 0},
    {"xCharacter_visor_normalmap", SurfaceClass::kGlass, 0},
    // sky and the ground dome: no data
    {"xGroundHat", SurfaceClass::kNone, 0},
    {"xSkyhat", SurfaceClass::kNone, 0},
    {"xrage_atmoscatt_clouds", SurfaceClass::kNone, 0},
    // the shadow, depth and occlusion passes
    {"xAmbientOcclusionGenerator", SurfaceClass::kNone, 0},
    {"xAmbientOcclusionShadows", SurfaceClass::kNone, 0},
    {"xShadowImposterCollector", SurfaceClass::kNone, 0},
    {"xShadows", SurfaceClass::kNone, 0},
    {"xrage_blendshadows", SurfaceClass::kNone, 0},
    {"xrage_shadowcollector", SurfaceClass::kNone, 0},
    {"xrage_shadowdepth", SurfaceClass::kNone, 0},
    {"xVehBlurImposter", SurfaceClass::kNone, 0},
    // particles, rain, light volumes, fog, glows, beams, trails (blended, never written)
    {"xCheckpointProj", SurfaceClass::kNone, 0},
    {"xPhysics", SurfaceClass::kNone, 0},
    {"xPowerupIceCube", SurfaceClass::kNone, 0},
    {"xPowerupShootEffect", SurfaceClass::kNone, 0},
    {"xPowerupStealthSkinned", SurfaceClass::kNone, 0},
    {"xPowerupTargetEffect", SurfaceClass::kNone, 0},
    {"xRoarShockwave", SurfaceClass::kNone, 0},
    {"xSSTTrail", SurfaceClass::kNone, 0},
    {"xSkidMark", SurfaceClass::kNone, 0},
    {"xSoftPtx", SurfaceClass::kNone, 0},
    {"xlightning", SurfaceClass::kNone, 0},
    {"xmc4_rmptfx_column", SurfaceClass::kNone, 0},
    {"xmc4_rmptfx_default", SurfaceClass::kNone, 0},
    {"xmc4_rmptfx_flat", SurfaceClass::kNone, 0},
    {"xmc4_rmptfx_gta_litsprite", SurfaceClass::kNone, 0},
    {"xmc4_rmptfx_litsprite", SurfaceClass::kNone, 0},
    {"xmc4_rmptfx_mainlist", SurfaceClass::kNone, 0},
    {"xmc4_rmptfx_model", SurfaceClass::kNone, 0},
    {"xmc4_rmptfx_projected", SurfaceClass::kNone, 0},
    {"xpowerup", SurfaceClass::kNone, 0},
    {"xrage_spotlightfog", SurfaceClass::kNone, 0},
    {"xrain_system", SurfaceClass::kNone, 0},
    {"xrmptfx_collision", SurfaceClass::kNone, 0},
    {"xrmptfx_default", SurfaceClass::kNone, 0},
    {"xrmptfx_litsprite", SurfaceClass::kNone, 0},
    {"xstar_glow", SurfaceClass::kNone, 0},
    {"xseam", SurfaceClass::kNone, 0},
    // the user interface, post effects and generators (other phases of the frame)
    {"xAlphaModulate", SurfaceClass::kNone, 0},
    {"xArrow", SurfaceClass::kNone, 0},
    {"xArrowBlit", SurfaceClass::kNone, 0},
    {"xFontDraw", SurfaceClass::kNone, 0},
    {"xGloss", SurfaceClass::kNone, 0},
    {"xGlossRuntimeAlpha", SurfaceClass::kNone, 0},
    {"xLitRuntimeAlpha", SurfaceClass::kNone, 0},
    {"xLitRuntimeColor", SurfaceClass::kNone, 0},
    {"xMetal", SurfaceClass::kNone, 0},
    {"xMetalSkinned", SurfaceClass::kNone, 0},
    {"xRadialGradient", SurfaceClass::kNone, 0},
    {"xRearView", SurfaceClass::kNone, 0},
    {"xRippleCustomDepth", SurfaceClass::kNone, 0},
    {"xRippleTarget", SurfaceClass::kNone, 0},
    {"xScreenTint", SurfaceClass::kNone, 0},
    {"xShaderBlits", SurfaceClass::kNone, 0},
    {"xSpecMapGen", SurfaceClass::kNone, 0},
    {"xTestRimBlur", SurfaceClass::kNone, 0},
    {"xUnlitCustomDepth", SurfaceClass::kNone, 0},
    {"xUnlitRuntimeAlpha", SurfaceClass::kNone, 0},
    {"xUnlitRuntimeColor", SurfaceClass::kNone, 0},
    {"xVertDamageBlit", SurfaceClass::kNone, 0},
    {"xVertDamageLayoutGenerator", SurfaceClass::kNone, 0},
    {"xVinylGenerator", SurfaceClass::kNone, 0},
    {"xrage_bink", SurfaceClass::kNone, 0},
    {"xrage_fastmipmap", SurfaceClass::kNone, 0},
    {"xrage_im", SurfaceClass::kNone, 0},
    {"xrage_perlinnoise", SurfaceClass::kNone, 0},
    {"xrage_postfx", SurfaceClass::kNone, 0},
};
inline constexpr size_t kGroupRowCount = sizeof(kGroupRows) / sizeof(kGroupRows[0]);

// The row of a group (a linear search: the builder calls it once per shader of the game, the renderer never).
inline const GroupRow* FindGroupRow(std::string_view group) {
  for (const GroupRow& row : kGroupRows) {
    if (row.group == group) return &row;
  }
  return nullptr;
}

// "group__entry" as the translator names a shader (the first "__": the groups have single underscores only, xCharacter_skin_arm__PS_HighQualitySkin); a name without one is
// all group ("unnamed_<identity>", a container that carried no .updb path).
struct ShaderName {
  std::string_view group;
  std::string_view entry;
};
constexpr ShaderName SplitShaderName(std::string_view name) {
  const size_t at = name.find("__");
  if (at == std::string_view::npos) return {name, {}};
  return {name.substr(0, at), name.substr(at + 2)};
}

// The entries of a group that are not the group's lit material: the shadow collector and caster passes (PS_ShadowBlend, PS_ShadowDepthAlpha, the night prop shadow phase
// PS_ShadowNight ...), the depth pre-pass, the occlusion passes and the AO generator, the car layout pass, the impostor generation, and the debug shaders. Class 0.
inline constexpr std::string_view kAuxiliaryEntryPrefixes[] = {
    "PS_Shadow", "PS_zPrePass", "PS_zprepass", "PSAmbientOcclusion", "PS_AO", "PS_drawAo", "PS_Layout", "PS_Blur", "PS_Copy", "PS_CalcShadows", "PS_ImposterBlend",
    "PSGenerateImposter",
};
constexpr bool IsAuxiliaryEntry(std::string_view entry) {
  for (const std::string_view prefix : kAuxiliaryEntryPrefixes) {
    if (entry.starts_with(prefix)) return true;
  }
  return entry.contains("Debug");
}
// The interior light model's entry of every city material: a plain world surface drawn with it is an interior one (the other classes keep theirs: a store's glass is glass).
inline constexpr std::string_view kInteriorEntry = "PSMultiLightInterior";

// What the tables say of a shader by its name.
struct ShaderMaterial {
  bool known = false;  // the group has a row (false: another game build, a mod, an "unnamed" container)
  SurfaceClass cls = SurfaceClass::kNone;
  uint32_t flags = 0;
  float smoothness = 0.0f;  // the class default
  float lit = 1.0f;
};
constexpr ShaderMaterial MaterialOfShaderName(std::string_view name) {
  ShaderMaterial m;
  const ShaderName split = SplitShaderName(name);
  const GroupRow* row = nullptr;
  for (const GroupRow& candidate : kGroupRows) {
    if (candidate.group == split.group) {
      row = &candidate;
      break;
    }
  }
  if (row == nullptr) return m;
  m.known = true;
  m.cls = row->cls;
  m.flags = row->flags;
  if (IsAuxiliaryEntry(split.entry)) {
    m.cls = SurfaceClass::kNone;
    m.flags = 0;
  } else if (split.entry == kInteriorEntry && m.cls == SurfaceClass::kWorld) {
    m.cls = SurfaceClass::kInterior;
  }
  m.smoothness = DefaultSmoothness(m.cls);
  m.lit = DefaultLit(m.cls);
  return m;
}

// ---------------------------------------------------------------------------
// 2. The gloss table: where the game keeps a shader's SpecularExponent
// ---------------------------------------------------------------------------

// Which constant bank of the draw: the one the vertex shader reads (the day city shaders are lit per vertex) or the pixel shader's.
enum class GlossBank : uint8_t { kNone = 0, kVertex = 1, kPixel = 2 };

struct GlossRow {
  std::string_view name;  // "group__entry"
  GlossBank bank = GlossBank::kNone;
  uint16_t reg = 0;  // the float constant register: `float4 SpecularExponent : packoffset(c<reg>)`, the exponent is its x
};

// Sorted by name (byte order). Generated from the translator's HLSL of the retail game (shaderpack\baseline: the shaders whose text declares SpecularExponent; every use is
// `.x`); gbuffer_material_test.cpp checks each row against the folder of translated shaders and that no shader that declares it is missing. The tire shader's
// SpecularExponentLow / High pair (xAmbientTire::PSAmbientTire) is not here: their class default is used.
inline constexpr GlossRow kGlossRows[] = {
    {"xCityBumpSpec__PSCityBumpSpec", GlossBank::kPixel, 101},
    {"xCityBumpSpec__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityBumpSpec__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityDecal__PSCityDecal", GlossBank::kPixel, 101},
    {"xCityDecal__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityDecal__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityDefaultGrime__VSCityDefaultGrime", GlossBank::kVertex, 73},
    {"xCityDefaultGrime__VSCityDefaultGrimeEnv", GlossBank::kVertex, 73},
    {"xCityDefaultLOD__VSCityDefaultLOD", GlossBank::kVertex, 73},
    {"xCityDefaultLOD__VSCityDefaultLODEnv", GlossBank::kVertex, 73},
    {"xCityDefaultLOD__VSCityDefaultLODReflect", GlossBank::kVertex, 73},
    {"xCityDefault__VSCityDefaultDay", GlossBank::kVertex, 73},
    {"xCityDefault__VSCityDefaultEnv", GlossBank::kVertex, 73},
    {"xCityGarageFloor__PSMultiLight", GlossBank::kPixel, 102},
    {"xCityGrimeDecal__PSCityGrimeDecal", GlossBank::kPixel, 101},
    {"xCityGrimeDecal__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityGrimeDecal__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityGrimeMetalTint__PSCityGrimeMetalTint", GlossBank::kPixel, 101},
    {"xCityGrimeMetal__PSCityGrimeMetal", GlossBank::kPixel, 100},
    {"xCityGrimeMetal__PSMultiLight", GlossBank::kPixel, 100},
    {"xCityGrimeMetal__PSMultiLightInterior", GlossBank::kPixel, 100},
    {"xCityGrimeTintDecal__PSCityGrimeTintDecal", GlossBank::kPixel, 102},
    {"xCityGrimeTintDecal__PSMultiLight", GlossBank::kPixel, 102},
    {"xCityGrimeTintDecal__PSMultiLightInterior", GlossBank::kPixel, 102},
    {"xCityGrimeTint__PSCityGrimeTint", GlossBank::kPixel, 102},
    {"xCityGrimeTint__PSMultiLight", GlossBank::kPixel, 102},
    {"xCityGrimeTint__PSMultiLightInterior", GlossBank::kPixel, 102},
    {"xCityGrime__PSCityGrime", GlossBank::kPixel, 101},
    {"xCityGrime__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityGrime__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityGroundDecalGrime__PSCityGroundDecalGrime", GlossBank::kPixel, 101},
    {"xCityGroundDecalGrime__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityGroundDecalGrime__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityGroundGrimeDecal__PSCityGroundGrimeDecal", GlossBank::kPixel, 101},
    {"xCityGroundGrimeDecal__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityGroundGrimeDecal__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityGroundGrime__PSCityGroundGrime", GlossBank::kPixel, 101},
    {"xCityGroundGrime__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityGroundGrime__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityNeon__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityNeon__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityNeon__VSCityNeon", GlossBank::kVertex, 75},
    {"xCityNeon__VSCityNeonEnv", GlossBank::kVertex, 75},
    {"xCityPondWater__PSCityPondWater", GlossBank::kPixel, 100},
    {"xCityPowerlines__VSCityDefaultDay", GlossBank::kVertex, 73},
    {"xCityRiverRoad__PSCityRiverRoad", GlossBank::kPixel, 101},
    {"xCityRiverRoad__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityRiverRoad__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityRoad__VSCityRoad", GlossBank::kVertex, 73},
    {"xCitySidewalk__PSCitySidewalk", GlossBank::kPixel, 101},
    {"xCitySidewalk__PSMultiLight", GlossBank::kPixel, 101},
    {"xCitySidewalk__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityStoreSign__PSCityStoreSign", GlossBank::kPixel, 101},
    {"xCityStoreSign__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityStoreSign__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xCityTintBumpDecal2__PSCityTintBumpDecal2", GlossBank::kPixel, 103},
    {"xCityTintBumpDecal2__PSMultiLight", GlossBank::kPixel, 103},
    {"xCityTintBumpDecal2__PSMultiLightInterior", GlossBank::kPixel, 103},
    {"xCityTintBumpDecal__PSCityTintBumpDecal", GlossBank::kPixel, 103},
    {"xCityTintBumpDecal__PSMultiLight", GlossBank::kPixel, 103},
    {"xCityTintBumpDecal__PSMultiLightInterior", GlossBank::kPixel, 103},
    {"xCityTunnelRoad__VSCityTunnelRoad", GlossBank::kVertex, 73},
    {"xCityTunnelTile__PSCityTunnelTile", GlossBank::kPixel, 100},
    {"xCityTunnelTile__PSMultiLight", GlossBank::kPixel, 100},
    {"xCityTunnelTile__PSMultiLightInterior", GlossBank::kPixel, 100},
    {"xCityWindowDirt__PSCityWindowDirt", GlossBank::kPixel, 100},
    {"xCityWindowDirt__PSMultiLight", GlossBank::kPixel, 100},
    {"xCityWindowDirt__PSMultiLightInterior", GlossBank::kPixel, 100},
    {"xCityWindowLower__PSCityWindowLower", GlossBank::kPixel, 100},
    {"xCityWindowLower__PSMultiLight", GlossBank::kPixel, 100},
    {"xCityWindowLower__PSMultiLightInterior", GlossBank::kPixel, 100},
    {"xCityWindowUpper__PSCityWindowUpper", GlossBank::kPixel, 101},
    {"xCityWindowUpper__PSMultiLight", GlossBank::kPixel, 101},
    {"xCityWindowUpper__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xPropBumpSpecTint__PSMultiLight", GlossBank::kPixel, 102},
    {"xPropBumpSpecTint__PSMultiLightInterior", GlossBank::kPixel, 102},
    {"xPropBumpSpecTint__PSPropBumpSpecTint", GlossBank::kPixel, 102},
    {"xPropBumpSpec__PSMultiLight", GlossBank::kPixel, 101},
    {"xPropBumpSpec__PSMultiLightInterior", GlossBank::kPixel, 101},
    {"xPropBumpSpec__PSPropBumpSpec", GlossBank::kPixel, 101},
    {"xPropDefault__PSPropDefault", GlossBank::kPixel, 100},
    {"xPropFoliageTrunk__PSGenerateImposter", GlossBank::kPixel, 101},
    {"xPropFoliageTrunk__PSPropFoliageTrunk", GlossBank::kPixel, 101},
    {"xPropMetal__PSPropMetal", GlossBank::kPixel, 100},
    {"xglass_break__PSCityWindowLower", GlossBank::kPixel, 113},
};
inline constexpr size_t kGlossRowCount = sizeof(kGlossRows) / sizeof(kGlossRows[0]);

inline const GlossRow* FindGlossRow(std::string_view name) {
  const GlossRow* end = kGlossRows + kGlossRowCount;
  const GlossRow* at = std::lower_bound(kGlossRows, end, name, [](const GlossRow& row, std::string_view key) { return row.name < key; });
  return at != end && at->name == name ? at : nullptr;
}

// The power the exponent stands for (gbuffer_rules.h part 3): the vertex shaders' is the Blinn-Phong power itself, the pixel shaders' is half of it (they double it into
// Schlick's approximation of the power). CHECK: read from the translated code of PSMultiLight (`ps = SpecularExponent.x + SpecularExponent.x;` then x / (n (1 - x) + x)), the
// census will tell whether the two stages' numbers for one material agree after the scale.
constexpr float GlossPowerScale(GlossBank bank) { return bank == GlossBank::kPixel ? 2.0f : 1.0f; }
// The smoothness of an exponent read from a constant bank; `fallback` when the bank holds no sane number (a NaN, an infinity, a negative: the game's own shaders would not
// light with it either).
inline float GlossSmoothness(GlossBank bank, float exponent, float fallback) {
  if (!std::isfinite(exponent) || exponent < 0.0f) return fallback;
  return SmoothnessOfPower(exponent * GlossPowerScale(bank));
}

// ---------------------------------------------------------------------------
// 3. The table file
// ---------------------------------------------------------------------------

inline constexpr char kMaterialTableMagic[4] = {'M', 'S', 'G', 'M'};
inline constexpr uint32_t kMaterialTableVersion = 1;  // the file's layout
inline constexpr const char* kMaterialTableFileName = "mcla_gbuffer_materials.v1.bin";
inline constexpr uint32_t kMaterialTableHeaderBytes = 32;
inline constexpr uint32_t kMaterialTableEntryBytes = 16;
inline constexpr uint32_t kMaterialBankRegisters = 256;  // a constant bank is 256 registers (guest::kConstantBankBytes / 16)

//   header (32 bytes): magic "MSGM" | version u32 | entry count u32 | rules version u32 (kMaterialRulesVersion) | 16 reserved (0)
//   entries (16 bytes, sorted by key, strictly increasing, little endian):
//     key u64 | stage u8 ('p' or 'v') | class u8 | flags u8 (kMaterialFlag*, 3 bits) | smoothness u8 (the class default, unorm8) | lit u8 (unorm8) |
//     gloss bank u8 (GlossBank) | gloss register u16 (0 with no bank)
//   A pixel shader's record is its class and defaults and, when it reads SpecularExponent, the bank and register; a vertex shader's has a bank and register only (class 0).
struct MaterialRecord {
  uint64_t key = 0;  // the shader's identity (FNV-1a 64 of the vertex-fetch normalised microcode: ShaderStore::Entry::key)
  bool pixel = true;
  SurfaceClass cls = SurfaceClass::kNone;
  uint8_t flags = 0;
  uint8_t smoothness = 0;
  uint8_t lit = 255;
  GlossBank gloss_bank = GlossBank::kNone;
  uint16_t gloss_reg = 0;
};

// A shader of the game as the builder sees it.
struct NamedShader {
  uint64_t key = 0;
  bool pixel = true;
  std::string_view name;
};

constexpr uint8_t ToByte(float unit) { return uint8_t(unit <= 0.0f ? 0 : unit >= 1.0f ? 255 : int(unit * 255.0f + 0.5f)); }

// The records of the game's shaders, sorted by identity: every pixel shader of a known group (class 0 ones too: a shader the table knows is told from one it does not), and the
// vertex shaders that have a gloss row. A shader whose gloss row names the other stage's bank is not given the gloss. The first of two shaders with one identity wins.
inline std::vector<MaterialRecord> BuildMaterialRecords(std::span<const NamedShader> shaders) {
  std::vector<MaterialRecord> records;
  records.reserve(shaders.size());
  for (const NamedShader& shader : shaders) {
    const GlossRow* gloss = FindGlossRow(shader.name);
    if (gloss != nullptr && gloss->bank != (shader.pixel ? GlossBank::kPixel : GlossBank::kVertex)) gloss = nullptr;
    MaterialRecord record;
    record.key = shader.key;
    record.pixel = shader.pixel;
    if (shader.pixel) {
      const ShaderMaterial m = MaterialOfShaderName(shader.name);
      if (!m.known) continue;
      record.cls = m.cls;
      record.flags = uint8_t(m.flags & 7u);
      record.smoothness = ToByte(m.smoothness);
      record.lit = ToByte(m.lit);
    } else if (gloss == nullptr) {
      continue;
    }
    if (gloss != nullptr) {
      record.gloss_bank = gloss->bank;
      record.gloss_reg = gloss->reg;
    }
    records.push_back(record);
  }
  std::stable_sort(records.begin(), records.end(), [](const MaterialRecord& a, const MaterialRecord& b) { return a.key < b.key; });
  records.erase(std::unique(records.begin(), records.end(), [](const MaterialRecord& a, const MaterialRecord& b) { return a.key == b.key; }), records.end());
  return records;
}

namespace material_detail {
inline void Put16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
inline void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(v >> shift));
}
inline void Put64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(uint8_t(v >> shift));
}
inline uint32_t Get16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
inline uint32_t Get32(const uint8_t* p) { return Get16(p) | (Get16(p + 2) << 16); }
inline uint64_t Get64(const uint8_t* p) { return uint64_t(Get32(p)) | (uint64_t(Get32(p + 4)) << 32); }
}  // namespace material_detail

// The file's bytes for records sorted by strictly increasing key. False with *error when they are not, or a field is out of range (a bug of the caller's: the parser refuses
// the same).
inline bool LayOutMaterialTable(std::span<const MaterialRecord> records, std::vector<uint8_t>* out, std::string* error) {
  using namespace material_detail;
  for (size_t i = 0; i < records.size(); ++i) {
    const MaterialRecord& r = records[i];
    if (i > 0 && !(records[i - 1].key < r.key)) {
      *error = "the material records are not in increasing order of identity";
      return false;
    }
    if (uint32_t(r.cls) > kClassMask || r.flags > 7 || uint32_t(r.gloss_bank) > uint32_t(GlossBank::kPixel) || r.gloss_reg >= kMaterialBankRegisters ||
        (r.gloss_bank == GlossBank::kNone && r.gloss_reg != 0)) {
      *error = "a material record has a field out of range";
      return false;
    }
  }
  out->clear();
  out->reserve(kMaterialTableHeaderBytes + records.size() * kMaterialTableEntryBytes);
  out->insert(out->end(), kMaterialTableMagic, kMaterialTableMagic + 4);
  Put32(*out, kMaterialTableVersion);
  Put32(*out, uint32_t(records.size()));
  Put32(*out, kMaterialRulesVersion);
  for (int i = 0; i < 4; ++i) Put32(*out, 0);
  for (const MaterialRecord& r : records) {
    Put64(*out, r.key);
    out->push_back(r.pixel ? 'p' : 'v');
    out->push_back(uint8_t(r.cls));
    out->push_back(r.flags);
    out->push_back(r.smoothness);
    out->push_back(r.lit);
    out->push_back(uint8_t(r.gloss_bank));
    Put16(*out, r.gloss_reg);
  }
  return true;
}

// The records of a file. False with *why (a sentence) for a file that is not this version's, truncated, unsorted or out of range: nothing of it is then used.
inline bool ParseMaterialTable(std::span<const uint8_t> bytes, std::vector<MaterialRecord>* out, std::string* why) {
  using namespace material_detail;
  out->clear();
  if (bytes.size() < kMaterialTableHeaderBytes || std::memcmp(bytes.data(), kMaterialTableMagic, 4) != 0) {
    *why = "it is not a G-buffer material table";
    return false;
  }
  const uint32_t version = Get32(bytes.data() + 4);
  const uint32_t count = Get32(bytes.data() + 8);
  const uint32_t rules = Get32(bytes.data() + 12);
  if (version != kMaterialTableVersion) {
    *why = "built by another version of the table's layout (" + std::to_string(version) + ", this build reads " + std::to_string(kMaterialTableVersion) + ")";
    return false;
  }
  if (rules != kMaterialRulesVersion) {
    *why = "built from other class tables (rules " + std::to_string(rules) + ", this build has " + std::to_string(kMaterialRulesVersion) + "); the shader pack build writes the current one";
    return false;
  }
  if (bytes.size() != kMaterialTableHeaderBytes + uint64_t(count) * kMaterialTableEntryBytes) {
    *why = "its size does not fit its entry count";
    return false;
  }
  out->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* p = bytes.data() + kMaterialTableHeaderBytes + size_t(i) * kMaterialTableEntryBytes;
    MaterialRecord r;
    r.key = Get64(p);
    if (p[8] != 'p' && p[8] != 'v') {
      *why = "an entry has a stage that is neither pixel nor vertex";
      out->clear();
      return false;
    }
    r.pixel = p[8] == 'p';
    r.cls = SurfaceClass(p[9]);
    r.flags = p[10];
    r.smoothness = p[11];
    r.lit = p[12];
    r.gloss_bank = GlossBank(p[13]);
    r.gloss_reg = uint16_t(Get16(p + 14));
    if (uint32_t(r.cls) > kClassMask || r.flags > 7 || p[13] > uint8_t(GlossBank::kPixel) || r.gloss_reg >= kMaterialBankRegisters ||
        (r.gloss_bank == GlossBank::kNone && r.gloss_reg != 0) || (i > 0 && !(out->back().key < r.key))) {
      *why = "an entry is out of range or out of order";
      out->clear();
      return false;
    }
    out->push_back(r);
  }
  return true;
}

// ---------------------------------------------------------------------------
// The renderer's side: the loaded table and the draw's word
// ---------------------------------------------------------------------------

// Component x of register `reg` of a constant bank of raw big-endian guest words (what Scene's banks hold). NaN when the bank is too short for the register.
inline float BankX(std::span<const uint8_t> bank, uint32_t reg) {
  const size_t at = size_t(reg) * 16;
  if (at + 4 > bank.size()) return std::numeric_limits<float>::quiet_NaN();
  const uint32_t bits = (uint32_t(bank[at]) << 24) | (uint32_t(bank[at + 1]) << 16) | (uint32_t(bank[at + 2]) << 8) | uint32_t(bank[at + 3]);
  return std::bit_cast<float>(bits);
}

class MaterialTable {
 public:
  // What the draws found (the render thread's counters; the Scene or a test may read them).
  struct Stats {
    uint64_t words = 0;           // draws given a word
    uint64_t unknown = 0;         // ... whose pixel shader the table does not have: the default word
    uint64_t gloss_pixel = 0;     // ... smoothness from the pixel shader's exponent
    uint64_t gloss_vertex = 0;    // ... smoothness from the vertex shader's exponent
    uint64_t class_default = 0;   // ... smoothness from the class
  };

  // Takes the records of a file's bytes (ParseMaterialTable). False with *why; the table is then empty.
  bool Load(std::span<const uint8_t> bytes, std::string* why) {
    records_.clear();
    return ParseMaterialTable(bytes, &records_, why);
  }
  // Reads the file, once per process, from the pack folder, and says what it found in the log (gbuffer_material.cpp). Does nothing while `folder` is empty (the shader store
  // has no pack yet). The Scene calls it before the first word.
  void EnsureLoaded(const std::filesystem::path& folder);
  bool LoadFile(const std::filesystem::path& file, std::string* why);  // gbuffer_material.cpp

  size_t size() const { return records_.size(); }
  const Stats& stats() const { return stats_; }

  // The record of a shader, null for a shader the file does not have.
  const MaterialRecord* Find(uint64_t key) const {
    if (key == 0) return nullptr;
    const auto at = std::lower_bound(records_.begin(), records_.end(), key, [](const MaterialRecord& r, uint64_t k) { return r.key < k; });
    return at != records_.end() && at->key == key ? &*at : nullptr;
  }

  // The material word of a draw made with pixel shader `ps_key` and vertex shader `vs_key` (0: none), whose constant banks (raw big-endian guest registers) are `ps_bank` and
  // `vs_bank`; `alpha_test`: the draw's pixel shader is the alpha test twin (the pack's spec variant 1), which sets kMaterialFlagAlphaCovered. The class, flags and lit weight
  // are the pixel shader's; the smoothness is the exponent of the pixel shader's own bank when it reads one, else of the vertex shader's, else the class default. A pixel
  // shader the table does not have gets DefaultMaterialWord().
  uint32_t WordFor(uint64_t ps_key, uint64_t vs_key, std::span<const uint8_t> ps_bank, std::span<const uint8_t> vs_bank, bool alpha_test) {
    ++stats_.words;
    const uint32_t alpha_flag = alpha_test ? kMaterialFlagAlphaCovered : 0u;
    const MaterialRecord* ps = Find(ps_key);
    if (ps == nullptr || !ps->pixel) {
      ++stats_.unknown;
      return MaterialWordWithFlags(DefaultMaterialWord(), alpha_flag);
    }
    float smoothness = float(ps->smoothness) / 255.0f;
    bool from_gloss = false;
    if (ps->gloss_bank == GlossBank::kPixel) {
      const float exponent = BankX(ps_bank, ps->gloss_reg);
      if (std::isfinite(exponent) && exponent >= 0.0f) {
        smoothness = GlossSmoothness(GlossBank::kPixel, exponent, smoothness);
        from_gloss = true;
        ++stats_.gloss_pixel;
      }
    } else if (const MaterialRecord* vs = Find(vs_key); vs != nullptr && !vs->pixel && vs->gloss_bank == GlossBank::kVertex) {
      const float exponent = BankX(vs_bank, vs->gloss_reg);
      if (std::isfinite(exponent) && exponent >= 0.0f) {
        smoothness = GlossSmoothness(GlossBank::kVertex, exponent, smoothness);
        from_gloss = true;
        ++stats_.gloss_vertex;
      }
    }
    if (!from_gloss) ++stats_.class_default;
    return MaterialWord(smoothness, uint32_t(ps->cls), uint32_t(ps->flags) | alpha_flag, float(ps->lit) / 255.0f);
  }

 private:
  std::vector<MaterialRecord> records_;
  Stats stats_;
  bool tried_ = false;
};

}  // namespace mcla::gpu::gbuffer

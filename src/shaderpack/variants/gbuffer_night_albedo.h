#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the night stage's ALBEDO RECIPES: the lit shaders whose surface colour is not a DiffuseSampler texel (package GB-H2-COVER)
// ===========================================================================
// gbuffer_albedo.h finds the albedo as the shader's own `DiffuseSampler` fetch at the texel's centre. Sixteen of the 58 shaders the forward headlight stage edits have none: the road and the
// tunnel road (a grey texel, a decal and a grime layer), the terrain and the grass (three diffuse layers weighted by a channel map), the tinted grime, grime decal and grime metal (a tint
// constant lerped to the grime texel), the tinted prop (a constant), the traffic car's body (no surface colour at all), and the neon and lamp glass tints. A pixel of such a shader got the "no
// data" code in RT2, so the deferred light pass (which skips RT2 = 0) could not light it: the headlights of the light pass lit walls and cars but not the road, the terrain, the grass, the
// tints or the traffic's body (GB-H2-ANALYSE of 2026-11-22: 89 to 97 per cent of the `forward only` lit pixels are the road). This file is the table that gives them an albedo.
//
// A RECIPE says, for one shader by name, where in the translated text the surface colour is and what it is (the retail text, found by hand and CHECKED NUMERICALLY, below):
//   kBefore / kAfter  the statement `anchor` (one line at the top level of main, found exactly once) is the place: the stage inserts `float3 gbAlbedo = <expression>;` right before / after it
//                     and writes `gbAlbedo` into RT2, as the diffuse fetch's capture does (a value the game's registers will overwrite later, so it is read where it is complete)
//   kGlobal           the colour is made of the shader's constants alone (`DiffuseColor.xyz`, a white): no register, no capture, the expression is written at the end of main as it is
// The expression is the NATURAL rgb of the surface (the translator permutes lanes: the tinted grime keeps its colour in `r2.zyx`), before the lights, the vertex occlusion (iColor1.w), the
// height fade and the shadow collector: the same thing the diffuse fetch gives the other shaders, which is what the game multiplies the sum of its lights with. A recipe whose anchor is not
// found once (a game or translator that is not the retail one) makes the variant NOT BUILD for that shader (the transform fails, like every anchor of the stages: the renderer keeps the game's
// shader), it never writes a wrong albedo quietly.
//
// THE CHECK. h11-work/gb-h2-notes/albedo_check.py runs each shader of the retail translation in the numeric interpreter of the GB-G0 analysis on 12 random inputs and constant sets (grime and
// decal branches both ways), reads the expression at its anchor and compares it with the game's own colour: oC0 = (sum of the lights + ambient) * albedo * F + specular, so d oC0.k / d ambient.k
// = albedo.k * F with F a scalar, and the cross terms vanish. All 11 rows below give the same F in the three channels and no cross term (cover/table_rows.py writes the rows file from this
// table, so what is checked is what the stage uses). The interior shaders (the interior light model picks the ambient with a flag) are checked by the vertex colour that joins their light sum
// instead (`in:iColor1`). The numeric interpreter is not the GPU: CHECK the first picture (the road, the terrain and the grass in the headlight beam against the forward stage's).
//
// WHAT STAYS WITHOUT ONE (kGBufferNightHeadlightExempt): the lit weight of an EMISSIVE class is 0 (gbuffer_material.h), the light pass never lights an emissive surface (RT2.a = 0), so an albedo
// there would change nothing: the neon and the lamp glass tints (5 shaders) keep the "no data" code, and a headlight no longer lights a neon tube or a lamp lens, which the forward stage did by
// accident of adding the sum of the lights to their colour. The plain neon and the store sign (xCityNeon, xCityStoreSign: GB-H2-REVIEW) do have a diffuse texel, but their class is emissive too, so
// their weight is 0 and the pass skips them just the same: they are on the list with the tints (8 shaders), not among the covered ones, and carry no occlusion row. The tangent space bump decal
// (xCityTintBumpDecal) has no world normal in its text, so it has no stage at all (kGBufferNightSkipTangent).
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variants/gbuffer_albedo.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

// Hand bumped with any change of the tables below, as the record of what changed (also bump gbuffer.h's kGBufferVariantVersion); the tables themselves are in the variant's fingerprint
// (NightTablesFingerprintText), so a pack made from other rows is stale even when nobody bumped it.
inline constexpr uint32_t kGBufferNightAlbedoVersion = 3;  // 1: the albedo recipes; 2: the baked occlusion rows (RT2.a); 3: the road's planar reflection mix (RT2.a)

enum class NightAlbedoSite : uint8_t {
  kBefore,  // the capture goes right before the anchor statement
  kAfter,   // the capture goes right after the anchor statement
  kGlobal,  // no capture: the expression reads the shader's constants only
};

struct NightAlbedoRow {
  std::string_view name;        // the group and the entry point, as the translator names its HLSL files
  NightAlbedoSite site;
  std::string_view anchor;      // kBefore, kAfter: one line of main at the top level, as the translator writes it, without its indentation; "" for kGlobal
  std::string_view expression;  // the natural rgb, as HLSL
  std::string_view source;      // what the surface colour is (the census' words)
};

// Sorted by name. 11 shaders: 9 have a statement to read the colour at (the road and the tunnel road, the terrain and the grass with their interior twins, the three tinted grimes), 2 are
// constants (the traffic body's white, the tinted prop's DiffuseColor).
inline constexpr NightAlbedoRow kGBufferNightAlbedoRows[] = {
    {"xAmbientVehicle__PSMultiLight", NightAlbedoSite::kGlobal, "", "float3(1.0f, 1.0f, 1.0f)",
     "the traffic body's night shader outputs the sum of the lights alone (oC0 = lamps + ambient, no surface colour, no texture): its surface is white"},
    {"xCityGrass__PSMultiLight", NightAlbedoSite::kBefore, "r4.z = -r1.w + gMultiLightDynamicLightData(5).x;", "r13.xyz",
     "the three diffuse layers weighted by the channel map (r13 = sum of weight * DiffuseSampler A, B, C), complete before the lamp terms"},
    {"xCityGrass__PSMultiLightInterior", NightAlbedoSite::kBefore, "r1.w = dot(r1.zxy, r1.zxy);", "r13.xyz",
     "the three diffuse layers weighted by the channel map (r13), complete before the normal's length"},
    {"xCityGrimeMetalTint__PSMultiLight", NightAlbedoSite::kAfter, "r5.xyz = r8.yyy * r5.xzy + DiffuseColor.zxy;", "float3(r5.y, r5.z, r5.x)",
     "the grime texel (times the vertex colour) lerped to the tint constant DiffuseColor by its alpha; the lanes are permuted (y, z, x)"},
    {"xCityGrimeTintDecal__PSMultiLight", NightAlbedoSite::kBefore, "r14.xyz = r6.xwy * r7.www;", "r6.xyw",
     "the decal texel lerped to DiffuseColor, then the grime texel (times the vertex colour) lerped over it: the colour (r6.x, r6.y, r6.w) before the vertex occlusion multiplies it"},
    {"xCityGrimeTint__PSMultiLight", NightAlbedoSite::kAfter, "r2.xyz = r15.www * r3.xyz + DiffuseColor.zyx;", "r2.zyx",
     "the grime texel (times the vertex colour) lerped to the tint constant DiffuseColor by its alpha; the lanes are reversed (z, y, x)"},
    {"xCityRoad__PSMultiLight", NightAlbedoSite::kAfter, "r5.y = saturate(r15.w * gReflectionParams.x + r0.w);", "r7.xyz + (r7.xyz * r7.xyz * r7.xyz - r7.xyz) * r5.y",
     "the grey diffuse texel lerped to the decal texel, then the grime layer (r7, final here), darkened by the puddle share r5.y the way the game does (c + (c^3 - c) * puddle)"},
    {"xCityTerrain__PSMultiLight", NightAlbedoSite::kBefore, "r7.z = -r4.x + gMultiLightDynamicLightData(5).x;", "r12.xyz",
     "the three diffuse layers weighted by the channel map (r12), complete before the lamp terms"},
    {"xCityTerrain__PSMultiLightInterior", NightAlbedoSite::kBefore,
     "r0.w = getCompTexLod2D(ChannelMapSampler_Texture2DDescriptorIndex, ChannelMapSampler_SamplerDescriptorIndex, r5.zw).x;", "r9.xyz",
     "the three diffuse layers weighted by the channel map (r9), complete before the map's level of detail"},
    {"xCityTunnelRoad__PSMultiLight", NightAlbedoSite::kAfter, "r5.x = saturate(r16.w * gReflectionParams.x + r5.x);", "r7.xyz + (r7.xyz * r7.xyz * r7.xyz - r7.xyz) * r5.x",
     "as the road: the grey texel, the decal and the grime layer (r7), darkened by the puddle share r5.x"},
    {"xPropBumpSpecTint__PSMultiLight", NightAlbedoSite::kGlobal, "", "DiffuseColor.xyz",
     "the prop has no diffuse texel: its colour is the tint constant DiffuseColor (times the vertex occlusion)"},
};
inline constexpr size_t kGBufferNightAlbedoRowCount = sizeof(kGBufferNightAlbedoRows) / sizeof(kGBufferNightAlbedoRows[0]);

inline const NightAlbedoRow* FindGBufferNightAlbedoRow(std::string_view name) {
  for (const NightAlbedoRow& row : kGBufferNightAlbedoRows) {
    if (row.name == name) return &row;
  }
  return nullptr;
}

// The shaders the forward headlight stage edits that the light pass does NOT light, and why (the test holds every other one to a normal and an albedo: nothing can drop out unseen).
struct NightHeadlightExempt {
  std::string_view name;
  std::string_view reason;
};
inline constexpr std::string_view kExemptEmissive = "emissive class (lit weight 0: the light pass never lights it); the forward stage lit it by adding the lights' sum to its colour";
inline constexpr std::string_view kExemptTangent = "tangent space bump decal: no world normal in its text (kGBufferNightSkipTangent), a blended draw keeps the planes of the surface under it";
inline constexpr NightHeadlightExempt kGBufferNightHeadlightExempt[] = {
    {"xCityLampGlassTint__PSMultiLight", kExemptEmissive},
    {"xCityLampGlassTint__PSMultiLightInterior", kExemptEmissive},
    {"xCityNeonTint__PSMultiLight", kExemptEmissive},
    {"xCityNeon__PSMultiLight", kExemptEmissive},
    {"xCityStoreSign__PSMultiLight", kExemptEmissive},
    {"xCityTintBumpDecal__PSMultiLight", kExemptTangent},
    {"xPropLampGlassTint__PSMultiLight", kExemptEmissive},
    {"xPropNeonTint__PSMultiLight", kExemptEmissive},
};
inline const NightHeadlightExempt* FindGBufferNightHeadlightExempt(std::string_view name) {
  for (const NightHeadlightExempt& row : kGBufferNightHeadlightExempt) {
    if (row.name == name) return &row;
  }
  return nullptr;
}

// ---- the baked occlusion (RT2.a) -------------------------------------------------------------------------------------------------------------------------------------------
// The game's lit shaders multiply the SUM of their lights (the grid's four lamps, the dynamic slots, the ambient, and the headlights the forward stage adds) by the baked occlusion the city's meshes
// carry in the alpha of the vertex colour COLOR1 (`iColor1.w`; the windows by its square). The deferred light pass has no plane for it, so its light was brighter than the same light through the
// forward stage by 1 / occlusion: GB-H2's A/B measured pass over forward at 2.1 to 2.2 on the walls, larger where the picture is darker (the occlusion is smallest there). RT2.a, the lit weight,
// is the draw's weight "how much the deferred lights add", so it carries the shader's occlusion too: lit weight x saturate(occlusion), and the pass multiplies it in as it does the weight
// (hlsl/local_light.hlsli LpPixelLight: `lit`). Measured on the retail text, h11-work/gb-h2-notes/cover/fwd_factors.py (the numeric interpreter: d [the ambient's term] / d [the input scaled by 1.25]
// is 1.25 for iColor1.w in the 48 shaders below with power 1 and 1.5625 in the 4 windows with power 2, and 1 for every other input of the vertex stage's colours that is not the weight of a grime or
// decal layer). The shaders that have no row do not have the factor (the LOD and foliage materials, the props' metal and default, the ocean, the emissive tints, the traffic body) or were not clean
// in the measurement (the interior twins of the grime and decal families, whose ambient is picked with a flag), or write no albedo anyway (the interior twins of the road, the tunnel road,
// the grime metal and the bump spec tints): they keep the draw's weight alone, exactly as before.
struct NightOcclusionRow {
  std::string_view name;
  uint8_t power;  // 1: iColor1.w, 2: iColor1.w * iColor1.w
};
// Sorted by name.
inline constexpr NightOcclusionRow kGBufferNightOcclusionRows[] = {
    {"xCityBillboard__PSMultiLight", 1},
    {"xCityBillboard__PSMultiLightInterior", 1},
    {"xCityBumpSpec__PSMultiLight", 1},
    {"xCityDefaultGrime__PSMultiLight", 1},
    {"xCityDefaultGrime__PSMultiLightInterior", 1},
    {"xCityDefaultLOD__PSMultiLight", 1},
    {"xCityDefaultLOD__PSMultiLightInterior", 1},
    {"xCityDefault__PSMultiLight", 1},
    {"xCityDefault__PSMultiLightInterior", 1},
    {"xCityGarageFloor__PSMultiLight", 1},
    {"xCityGrass__PSMultiLight", 1},
    {"xCityGrass__PSMultiLightInterior", 1},
    {"xCityGrimeDecal__PSMultiLight", 1},
    {"xCityGrimeMetalTint__PSMultiLight", 1},
    {"xCityGrimeTintDecal__PSMultiLight", 1},
    {"xCityGrimeTint__PSMultiLight", 1},
    {"xCityGrime__PSMultiLight", 1},
    {"xCityGroundDecalGrime__PSMultiLight", 1},
    {"xCityGroundGrimeDecal__PSMultiLight", 1},
    {"xCityGroundGrime__PSMultiLight", 1},
    {"xCityNormalMap__PSMultiLight", 1},
    {"xCityNormalMap__PSMultiLightInterior", 1},
    {"xCityPondWater__PSMultiLight", 1},
    {"xCityPondWater__PSMultiLightInterior", 1},
    {"xCityPowerlines__PSMultiLight", 1},
    {"xCityRiverRoad__PSMultiLight", 1},
    {"xCityRiverRoad__PSMultiLightEnv", 1},
    {"xCityRiverRoad__PSMultiLightInterior", 1},
    {"xCityRoad__PSMultiLight", 1},
    {"xCitySidewalk__PSMultiLight", 1},
    {"xCitySidewalk__PSMultiLightInterior", 1},
    {"xCityTerrain__PSMultiLight", 1},
    {"xCityTerrain__PSMultiLightInterior", 1},
    {"xCityTintBumpDecal2__PSMultiLight", 1},
    {"xCityTintBumpDecal2__PSMultiLightInterior", 1},
    {"xCityTunnelRoad__PSMultiLight", 1},
    {"xCityWindowDirt__PSMultiLight", 2},
    {"xCityWindowDirt__PSMultiLightInterior", 2},
    {"xCityWindowLOD__PSMultiLight", 1},
    {"xCityWindowLOD__PSMultiLightEnv", 1},
    {"xCityWindowLOD__PSMultiLightInterior", 1},
    {"xCityWindowLower__PSMultiLight", 2},
    {"xCityWindowLower__PSMultiLightInterior", 2},
    {"xCityWindowUpper__PSMultiLight", 1},
    {"xCityWindowUpper__PSMultiLightInterior", 1},
    {"xPropBumpSpecTint__PSMultiLight", 1},
    {"xPropBumpSpec__PSMultiLight", 1},
    {"xPropBumpSpec__PSMultiLightInterior", 1},
};
inline constexpr size_t kGBufferNightOcclusionRowCount = sizeof(kGBufferNightOcclusionRows) / sizeof(kGBufferNightOcclusionRows[0]);

inline const NightOcclusionRow* FindGBufferNightOcclusionRow(std::string_view name) {
  for (const NightOcclusionRow& row : kGBufferNightOcclusionRows) {
    if (row.name == name) return &row;
  }
  return nullptr;
}
// The HLSL of a row: the interpolant the translator declares as `in centroid float4 iColor1 : COLOR1` (an input: no later statement of the shader can have changed it).
inline std::string NightOcclusionExpression(const NightOcclusionRow& row) { return row.power == 2 ? "iColor1.w * iColor1.w" : "iColor1.w"; }
inline constexpr std::string_view kNightOcclusionDeclaration = "in centroid float4 iColor1 : COLOR1";

// ---- the planar reflection's mix (RT2.a) -----------------------------------------------------------------------------------------------------------------------------------
// The road and the tunnel road do not output their lit colour as it is: `oC0 = lerp(lit, ReflectionSampler texel, w)` with `w = (puddle mask ...) * gReflectionParams.y` (`r5.w = r5.z *
// gReflectionParams.y;`, a register nothing writes again), so every light of the sum, the forward headlight included, keeps only `1 - w` of itself: 0.095 of the reflection on a dry road, and up
// to the whole of it on a puddle in rain (VS c43.y 0.095 dry, 1.19 in rain: ssr_rules.h). The deferred pass has no plane for it, so it was brighter than the same light through the forward stage
// by 1 / (1 - w) on the road in the wet (GB-H2-REVIEW: the numeric interpreter's jacobian of the ambient term, with the reflection strength 0 and 0.5, is 1 - r5.w exactly on 5 seeds, both shaders: perf/scripts/gb_h2_review_probes/wet2.py).
// The stage captures the weight right after its statement and the lit weight (RT2.a) takes `saturate(1 - w)` like the occlusion. The other shaders that read a reflection add it
// (`oC0 = lit + reflection * k`: the sidewalk, the river road, the garage floor, the windows, the pond and the glass) and leave the lit colour as it is, so they have no row.
struct NightMixRow {
  std::string_view name;
  std::string_view anchor;  // one line at the top level of main, found once: the weight's statement
  std::string_view weight;  // the register that holds it after the anchor
};
// Sorted by name.
inline constexpr NightMixRow kGBufferNightMixRows[] = {
    {"xCityRoad__PSMultiLight", "r5.w = r5.z * gReflectionParams.y;", "r5.w"},
    {"xCityTunnelRoad__PSMultiLight", "r5.w = r5.z * gReflectionParams.y;", "r5.w"},
};
inline constexpr size_t kGBufferNightMixRowCount = sizeof(kGBufferNightMixRows) / sizeof(kGBufferNightMixRows[0]);

inline const NightMixRow* FindGBufferNightMixRow(std::string_view name) {
  for (const NightMixRow& row : kGBufferNightMixRows) {
    if (row.name == name) return &row;
  }
  return nullptr;
}
inline constexpr std::string_view kGBufferMixCapture = "gbMix";

struct NightMixCapture {
  bool found = false;
  std::string why_not;
  size_t capture_at = std::string::npos;  // the offset (text with LF line ends) of the line the capture goes before: the line after the anchor
  std::string capture;                    // `\tfloat gbMix = r5.w;\n`
};

// The capture of a mix row for the text of its shader (pure; public for the stage and the tests): the anchor must be one line at the top level of main and occur exactly once.
inline NightMixCapture AnalyseGBufferNightMixRow(std::string_view hlsl, const NightMixRow& row) {
  using namespace albedo_detail;
  NightMixCapture out;
  const std::string text = textedit::NormalizeNewlines(hlsl);
  const size_t main_at = text.rfind("\nvoid main(");
  if (main_at == std::string::npos) {
    out.why_not = "no `void main(`";
    return out;
  }
  const std::vector<Line> lines = LinesFrom(text, main_at + 1);
  size_t count = 0;
  size_t at = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].code != row.anchor) continue;
    ++count;
    at = i;
  }
  if (count != 1 || lines[at].indent != 1 || at + 1 >= lines.size()) {
    out.why_not = "the reflection mix's anchor `" + std::string(row.anchor) + "` is " + (count == 1 ? std::string("not at the top level of main") : "found " + std::to_string(count) + " times in main (need once)");
    return out;
  }
  out.found = true;
  out.capture_at = lines[at + 1].begin;
  out.capture = "\tfloat " + std::string(kGBufferMixCapture) + " = " + std::string(row.weight) + ";\n";
  return out;
}

// THE TABLES' TEXT, for the variant's fingerprint (gbuffer.cpp: MakeGBufferVariantFromText adds it to the helper's): the pack's stamp keeps the fingerprint (variant_pack.cpp `variants_gbuffer_input`),
// so an edit of a row, of a number or of an expression above makes the pack stale by itself, whether or not anyone remembers to bump the versions (kGBufferNightAlbedoVersion and gbuffer.h's
// kGBufferVariantVersion stay the record of what changed; this was the only thing that said a table had changed, by hand: GB-H2-REVIEW).
inline std::string NightTablesFingerprintText() {
  std::string out = "night albedo tables, version " + std::to_string(kGBufferNightAlbedoVersion) + "\n";
  for (const NightAlbedoRow& row : kGBufferNightAlbedoRows) {
    out += "albedo|" + std::string(row.name) + "|" + std::to_string(int(row.site)) + "|" + std::string(row.anchor) + "|" + std::string(row.expression) + "\n";
  }
  for (const NightOcclusionRow& row : kGBufferNightOcclusionRows) {
    out += "occlusion|" + std::string(row.name) + "|" + NightOcclusionExpression(row) + "\n";
  }
  for (const NightMixRow& row : kGBufferNightMixRows) {
    out += "mix|" + std::string(row.name) + "|" + std::string(row.anchor) + "|" + std::string(row.weight) + "\n";
  }
  out += "declaration|" + std::string(kNightOcclusionDeclaration) + "\n";
  return out;
}

// A recipe's albedo for the text of its shader (pure; public for the stage, the tests and the census). `hlsl` may have any line ends; offsets are into the text with LF line ends. For a
// kBefore / kAfter row the anchor must be one line at the top level of main (one tab of indentation) and occur exactly once in main; the result then holds the capture. For a kGlobal row the
// text only needs a main. When it does not fit, `found` is false and `why_not` says how many times the anchor was found.
inline GBufferAlbedo AnalyseGBufferNightAlbedoRow(std::string_view hlsl, const NightAlbedoRow& row) {
  using namespace albedo_detail;
  GBufferAlbedo out;
  const std::string text = textedit::NormalizeNewlines(hlsl);
  const size_t main_at = text.rfind("\nvoid main(");
  if (main_at == std::string::npos) {
    out.why_not = "no `void main(`";
    return out;
  }
  if (row.site == NightAlbedoSite::kGlobal) {
    out.found = true;
    out.expression = std::string(row.expression);
    out.lanes = out.expression;
    out.captured = false;
    out.detail = "recipe (constants): " + std::string(row.source);
    return out;
  }
  const std::vector<Line> lines = LinesFrom(text, main_at + 1);
  size_t count = 0;
  size_t at = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].code != row.anchor) continue;
    ++count;
    at = i;
  }
  if (count != 1 || lines[at].indent != 1) {
    out.why_not = "the recipe's anchor `" + std::string(row.anchor) + "` is " + (count == 1 ? std::string("not at the top level of main") : "found " + std::to_string(count) + " times in main (need once)");
    return out;
  }
  const size_t next = at + 1 < lines.size() ? lines[at + 1].begin : text.size();
  out.found = true;
  out.line = int(at);
  out.expression = std::string(kGBufferAlbedoCapture);
  out.lanes = std::string(row.expression);
  out.captured = true;
  out.capture_at = row.site == NightAlbedoSite::kBefore ? lines[at].begin : next;
  out.capture = "\tfloat3 " + std::string(kGBufferAlbedoCapture) + " = " + std::string(row.expression) + ";\n";
  out.detail = std::string("recipe, captured ") + (row.site == NightAlbedoSite::kBefore ? "before" : "after") + " line " + std::to_string(at) + ": " + std::string(row.source);
  return out;
}

}  // namespace larecomp::shaderpack

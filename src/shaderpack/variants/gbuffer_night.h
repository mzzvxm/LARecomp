#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the G-buffer stage of the NIGHT world: the PSMultiLight, PSMultiLightEnv and PSMultiLightInterior pixel shaders
// ===========================================================================
// Package GB-C of perf/GBUFFER-SURVEY.md 7.2. By night every material of the city draws its PSMultiLight shader (per pixel lamps, the sun's term, the ambient) and
// the shader has the world normal in a register, so it can write it into the sidecar's RT1 (src/gpu/gbuffer_types.h) with no change to its colour. The family
// is the pixel shaders whose names end in `__PSMultiLight` (58), `__PSMultiLightEnv` (8) and `__PSMultiLightInterior` (49): 115 of the 120 shaders the headlight
// variant edits (the other five are the game's debug shaders, `...Debug` and `...NFLDebug`: never drawn in play, not in this family).
//
// THE STAGE. It is the G-buffer variant's (feature kFeatureGBuffer, shaderpack/variants/gbuffer.h: one stage, one table row, one VariantDef). gbuffer.cpp sends the
// family's shaders here (everything but xCityDefault::PSMultiLight, which the spike's own edits keep: the text this file makes for that shader is byte for byte the
// spike's, gbuffer_night_test.cpp checks it). The edits are the spike's four insertions (the second output, the 48 byte block of constants, the helper, the write of
// the plane after `oC0 = applyBlendPremult(oC0);`), and, when the normal is not an interpolant the shader leaves alone, one more: a capture of it (below). All of them
// only add: nothing the shader computes its colour with is changed, so the colour is the game's (the test checks that the transformed text, minus the added lines, is the
// original line by line, and that no added line assigns a register of the game's). The stage composes with the headlight's (chain order: the headlight first; its edits
// are the ambient sum, a local after the cube map's line and its helper: none of the anchors here).
//
// THE NORMAL: found by what the shader lights with. Three anchors, in this order; the first that is found is used (AnalyseGBufferNight):
//   1. THE SUN TERM. The shader lights with the sun as N.L: either `saturate(dot(rN.zxy, gMultiLightDynamicLightData(10).zxy))` (the sun direction is the game's PS
//      constant c94, `gMultiLightDynamicLightData(10)`; 69 shaders), or with the direction first scaled by a scalar (`r14.xyz = r5.www * ...(10).zyx;` then
//      `dot(r14.xzy, r0.zxy)`, the road, the sidewalk, the windows, the neon; 20 shaders: the dot's operand that is a register of the sun's lanes is followed through
//      the copies). A dot pairs lanes: the sun's x lane meets one lane of the other operand, and so on, so the normal is the other operand's lanes in the sun's order.
//      That is the vector the game lights the pixel with, in the sun's space, which is the world space (the lamp positions and the pixel's position are in it too).
//      It is what the game made of the vertex's normal at that point: the interpolant itself, or a normalised or perturbed (normal mapped, water) copy.
//   2. THE LAMP TERM. A shader without a sun term (the Env shaders, the foliage, the pedestrians) lights with the three dynamic lamps by a sum of products
//      `lampDelta * rN.x + lampDelta * rN.y + lampDelta * rN.z`: the interpolant whose three lanes are each multiplied (broadcast or alone) before anything writes them.
//   3. THE NORMALISATION. The interpolant whose three lanes are dotted with themselves (`dot(r1.zxy, r1.zxy)`, the length the shader divides by): the traffic car's body.
// The three agree where more than one is there (the census test cross-checks them on the retail shaders).
//
// WHERE IT IS READ. When every lane of the normal is a lane of an interpolant (`float4 rN = iTexCoordK;`) that nothing wrote before the sun term, the write at the end
// of main reads the interpolants (`iTexCoord0.xyz`, as the spike's: no extra live register). Otherwise the stage inserts one line before the statement of the term,
// `float3 gbNormal = r10.xyz;`: the value the game's term is about to use (a register of the game's can be overwritten by the end of main), and the write encodes that.
//
// WHAT IS LEFT OUT (the transform says `skip`, with the reason; the shader draws as the game's, and the consumers fall back to depth normals where RT1 is 0):
//   - a shader whose light has no normal: lamps by attenuation only (the road studs, the grime metal, the tunnel tile) or no per pixel light (the foliage, the store
//     interiors, the constant coloured glass and effects)
//   - the tangent space term of xCityTintBumpDecal (`dot(r14.xy, ...(10).xy) + ...(10).z`: the normal is (x, y, 1) in the decal's own frame, not the world's)
//   - a sun term that is inside a branch and whose normal needs a capture (none on the retail text: the powerlines' term is in a branch, on an untouched interpolant, which is read at the end of main)
// The census on the retail text (gbuffer_night_test.cpp, with the shaders' folder as an argument) prints every shader, edited or left out, and why.
//
// RT2 (package GB-W1, shaderpack/variants/gbuffer_albedo.h: the albedo and the lit weight): the stage declares a third output, `out float4 oC2 : SV_Target2` (gbuffer_rules.h kStagePlanes
// is RT1, RT2 and RT3), and writes after the normal `oC2 = float4(GbAlbedoEncode(<the shader's DiffuseSampler texel>), GbMaterialLit(<the draw's material word>))`. The texel is found by its own
// anchor (AnalyseGBufferAlbedo, the first fetch of the texture at the texel's centre that yields x, y and z) and read at the end of main, or captured right after the fetch when the shader
// writes its register again (`float3 gbAlbedo = r6.xzw;`), the same rule as the normal's. A shader with no such fetch writes the "no data" code (all zero) to RT2, EXCEPT the 11 shaders that have a
// RECIPE (gbuffer_night_albedo.h, GB-H2-COVER: the road, the terrain, the grass, the tints and the traffic body, which the forward headlight stage lights): their surface colour is a register after
// a statement the table names, or a constant, and is captured / written the same way. The edits only add lines.
// RT3 (package GB-W2, shaderpack/variants/gbuffer_material_plane.h: the smoothness, the class and the flags): a fourth output, `out float2 oC3 : SV_Target3`, and after the albedo
// `oC3 = GbMaterialRt3(<the draw's material word>)` under GB_MODE_WRITE_MATERIAL (else the "no data" code). The word needs nothing from the shader's text, so every shader of the stage
// writes it, the ones with no albedo too. The write block comes from one function (NightWriteBlock in gbuffer_night.cpp).
// ===========================================================================

#include <cstddef>
#include <string>
#include <string_view>

#include "shaderpack/variant_pack.h"
#include "shaderpack/variants/gbuffer_albedo.h"
#include "shaderpack/variants/gbuffer_night_albedo.h"
#include "shaderpack/variants/gbuffer_material_plane.h"

namespace larecomp::shaderpack {

// The family, by name (the group and the entry point, as the translator names its HLSL files).
inline constexpr std::string_view kGBufferNightSuffixes[] = {"__PSMultiLight", "__PSMultiLightEnv", "__PSMultiLightInterior"};
bool IsGBufferNightShader(std::string_view name);

// The reasons a shader is left out (also what the tests look for): the variant step counts the shaders left out for the same reason together.
inline constexpr std::string_view kGBufferNightSkipLampsOnly = "its light has no normal (the lamps by distance only)";
inline constexpr std::string_view kGBufferNightSkipUnlit = "it has no per pixel light";
inline constexpr std::string_view kGBufferNightSkipTangent = "its sun term uses a tangent space normal (z is 1)";
inline constexpr std::string_view kGBufferNightSkipNested = "its sun term is inside a branch";
inline constexpr std::string_view kGBufferNightSkipAmbiguous = "more than one interpolant could be its normal";

enum class NightNormalHow : unsigned char {
  kNone = 0,
  kSunTerm = 1,        // the operand of the sun's dot, paired with the sun's lanes
  kLampTerm = 2,       // the interpolant the lamp sum multiplies lane by lane
  kNormalisation = 3,  // the interpolant that is dotted with itself
};

// What the analysis finds in the text of one shader.
struct NightAnalysis {
  bool found = false;
  std::string why_not;           // when !found: one of the kGBufferNightSkip* reasons
  NightNormalHow how = NightNormalHow::kNone;
  bool interpolant = false;      // every lane is a lane of an interpolant nothing wrote before the term: read at the end of main, no capture
  std::string expression;        // what GbRt1Encode takes: `iTexCoord0.xyz`, or `gbNormal` when it is captured
  size_t capture_at = std::string::npos;  // the offset (in the text with LF line ends) of the line the capture goes before; npos: none
  std::string capture;           // the line to insert there, indentation and line end included
  int line = -1;                 // the term's line, counted from `void main(` (0): for the census
  std::string detail;            // the census' words: where the normal comes from
  GBufferAlbedo albedo;          // RT2: the shader's diffuse texel (gbuffer_albedo.h), or its recipe's colour (gbuffer_night_albedo.h); !albedo.found when it has none (RT2 then gets the "no data" code)
  bool recipe = false;           // the shader has a recipe in kGBufferNightAlbedoRows (the albedo is the recipe's; when its anchor is not found the transform fails instead of writing "no data")
};

// The text's normal (pure; public for tests and the census). `hlsl` may have any line ends; offsets are into the text with LF line ends. `name` (the group and the entry point) says which
// albedo recipe applies, if the shader has one (kGBufferNightAlbedoRows: GB-H2-COVER); without it only the diffuse texel is looked for.
NightAnalysis AnalyseGBufferNight(std::string_view hlsl, std::string_view name = {});

// The stage's text transform for one shader of the family (pure). `hlsli_text` is gbuffer_common.hlsli. Skips a shader that is not in the family or has no normal.
VariantTransformResult TransformGBufferNightHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx);

}  // namespace larecomp::shaderpack

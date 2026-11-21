#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the G-buffer's CAR, TRAFFIC and CHARACTER families (package GB-G1 of perf/LIGHT-UNIFY.md 11.2; the decision: perf/GB-G0.md)
// ===========================================================================
// One more family of the G-buffer variant (feature kFeatureGBuffer, shaderpack/variants/gbuffer.h: one stage, one VariantDef, one pack stamp line): gbuffer.cpp's TransformGBufferHlsl sends
// every IsGBufferCarShader name here, as it sends the night's to gbuffer_night.cpp and the day's to gbuffer_day.cpp, and its VariantDef lists GBufferCarShaderNames().
//
// WHERE EACH PLANE OF A CAR PIXEL COMES FROM (the evidence is in perf/GB-G0.md; every number there is from the retail translation in shaderpack/baseline/hlsl-rpf):
//   NORMAL (RT1). The game's own vertex shader of a car, a traffic car and a character already exports the vertex's WORLD normal, unit length, as one of the interpolants (TEXCOORD1, 2 or 0
//   by family: the car matrix and gWorld have rotated it) and the pixel shader declares it: the same `kInterpolant` source GB-D's day stage uses for the city. Of the 92 lit pixel shaders 65
//   take it from there (42 cars, 5 traffic, 18 characters), 8 have no exported normal (6 cars, 1 traffic, 1 pedestrian: the face's normal from the derivatives of the fragment's world point,
//   GbFaceNormal, GB-D's text) and 19 are left out with a reason (kGBufferCarLeftOut). A draw that is two sided (the host sets GB_MODE_FLIP_BACKFACE) turns the normal of a back face to
//   the viewer, as the day stage does. So the survey's vertex shader variant (oTexCoord15) is NOT needed for the cars, and the CarFrame normal pass is dominated (no hooks in car_frame).
//   ALBEDO (RT2.rgb), by shader (GBufferCarAlbedo): the diffuse texel (51: GB-W1's fetch anchor, gbuffer_albedo.h, with the sampler and the gradient fetch the car shaders also use) times the
//   colour constants the game multiplies into it (`baseColor`, `tintColor`, `MetalColor` ...: 22 rows; each is a per channel multiplier of the game's colour, checked on the retail text
//   with a numeric run); the PAINT (4 shaders: the register the game keeps the clear coat's colour in, after its vinyl layer; the anchors are statements of the retail text found once and
//   checked numerically, h11-work/gb-g1-notes/paint_final.py: the register equals lerp(lerp(DL3, DL2, f), vertex gradient colour, DL2.w) mixed with the vinyl texel); a colour constant
//   alone (2: the carbon fibre); and none (16: chrome, lamp lenses, glass, black, metal with no diffuse: RT2 is "no data" there, the consumer takes the class).
//   LIT WEIGHT (RT2.a) and MATERIAL (RT3): THE DRAW'S WORD (g_GBufferMode.y, GB-F1: gpu/gbuffer_material.h: the class table by the shader's group name, the flags, the gloss), as the night and
//   day stages take it: the shader writes what the host says, no table is kept here a second time (the unit test compares the word the host will send with the classes this stage was
//   designed for: paint, chrome, glass, light, tyre, interior). One refinement lives in the shader: the 11 shaders of the xBumpSpec family have a constant of their own named
//   `smoothness` (PS c104 or c105: the game's own material parameter), which replaces the word's smoothness byte (kBumpSpec). `metallic` has no plane. RT3 is written when the core carries
//   it (kGBufferCarsPlanes below: gbuffer_rules.h kStagePlanes; today RT1 and RT2).
//
// WHAT THE STAGE EDITS (each anchor must be found the number of times said or the variant is not built: the HLSL is not the one it was written for):
//   1. the outputs: `, out float2 oC1 : SV_Target1` (+ `oC2 : SV_Target2` float4 and `oC3 : SV_Target3` float2 as the planes say), after `out float4 oC0 : SV_Target0`
//   2. the 48 byte block of GBufferConstants (DeclareConstants), the helper (gbuffer_common.hlsli) before main
//   3. a captured copy where a value must outlive its register: the diffuse texel (GB-W1's capture) or the paint (one line after the game's vinyl statement: `float3 gbAlbedo = r3.xyz;`)
//   4. for the shaders with no exported normal: the fragment's world point and its derivatives at the top of main (GB-D's text)
//   5. the write after the game's `oC0 = applyBlendPremult(oC0);` (so after the alpha test's discard): the normal, the albedo and the lit weight, the material
// The colour path is untouched: the test takes the insertions out again and compares with the retail text byte for byte, at 1, 2 and 3 planes.
//
// THE CHAIN. The stage composes with the car variants that edit the same shaders (variant_chain.h kFeatures): car_selfrefl (the player's paint, xCarPaintCustomizable::PS_CarPaint) and
// car_chrome (xChrome::PS_Chrome): the car variant's edits first (ascending feature index), then these insertions on the text it leaves; its 112 byte block comes first, the G-buffer's
// 48 bytes after it (byte 720). The two are the only car shaders another stage edits (the night's xAmbientVehicle::PSMultiLight and xPed::PSMultiLight are the night stage's).
//
// CHECK (nothing here has run): that the outputs bind on every back end (4 colour targets + depth, MSAA, SV_Coverage: gbuffer_rules.h kStagePlanes); the normal at four headings of the car
// (C1: that `N.C` is the world normal on the player's car); `smoothness` as a perceptual smoothness (C5); the tyres' derivative normal (C11); the cost (C8).
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/gbuffer_rules.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variant_pack.h"
#include "shaderpack/variants/gbuffer_albedo.h"

namespace larecomp::shaderpack {

// Hand bumped: the table, the edits or the checks of the car stage (a change also bumps gbuffer.h's kGBufferVariantVersion, the pixel stage's stamp line).
// 1 was the GB-G0 spike's prototype, which never was in a pack. 2: the stage in the pack: the albedo of 51 + 4 + 2 shaders, the back face flip, the draw's word, the xBumpSpec smoothness.
inline constexpr uint32_t kGBufferCarsVersion = 2;

// How many planes the stage declares (1: the normal, 2: + the albedo and the lit weight, 3: + the material): the core's, so that the car stage declares exactly what every pipeline
// that carries the sidecar has (gbuffer_rules.h: a draw that carries it carries ALL the stage's planes). The night and the day stages declare RT1 and RT2 in their own text; the package
// that raises kStagePlanes (GB-W2) raises them in the same commit, and this stage follows by itself.
inline constexpr uint32_t kGBufferCarsPlanes = mcla::gpu::gbuffer::PlaneCount(mcla::gpu::gbuffer::kStagePlanes);

enum class GBufferCarNormal : uint8_t {
  kInterpolant,  // iTexCoord<slot>.xyz is the world normal, unit length: the game's vertex shader exports it (found by running the paired vertex shaders: vsnorm_cars.py)
  kDerivative,   // no vertex shader of the family exports a normal: the face's, from the derivatives of the fragment's world point (GbFaceNormal)
};

enum class GBufferCarAlbedo : uint8_t {
  kNone,      // "no data": no diffuse fetch and no colour constant of the shader's own (chrome, lenses, glass, black, metal)
  kDiffuse,   // the shader's diffuse texel (gbuffer_albedo.h: `sampler`, `gradient`) times the colour constants of `tint`
  kPaint,     // the paint register after the vinyl layer: the statement `anchor`, the value `expression`
  kConstant,  // the colour constants of `tint` alone (the carbon fibre)
};

enum class GBufferCarMaterial : uint8_t {
  kHost,      // the draw's word as it is
  kBumpSpec,  // the word with its smoothness byte replaced by the shader's own constant `smoothness.x` (the xBumpSpec family)
};

struct GBufferCarShader {
  std::string_view name;  // the group and the entry point, as the translator names its HLSL files
  GBufferCarNormal normal;
  uint32_t slot;  // kInterpolant: k of iTexCoord<k>; else 0
  GBufferCarAlbedo albedo;
  std::string_view sampler;     // kDiffuse: the texel's sampler: "DiffuseSampler", or "TextureSampler" (the rage shaders)
  bool gradient;                // kDiffuse: the texel is fetched with the gradients (the tyre, the brake rotor, the parallax rubber)
  std::string_view tint;        // kDiffuse, kConstant: what the texel is multiplied by, as HLSL: `baseColor.xyz`, `baseColor.xyz * brakeCaliperTintColor.xyz`; "" for none
  std::string_view anchor;      // kPaint: the statement after which the paint register holds the colour (at the top level of main, found once)
  std::string_view expression;  // kPaint: the colour in that register, natural rgb: `float3(r7.z, r7.x, r7.y)`, `r3.xyz`
  GBufferCarMaterial material;
  std::string_view family;  // "player/racer", "traffic", "ped/character"
};

struct GBufferCarLeftOut {
  std::string_view name;
  std::string_view reason;
};

// ---- the table: 73 shaders edited (65 with an exported normal, 8 with the derivative), the census of perf/GB-G0.md -------------------------------------------------------------
extern const GBufferCarShader kGBufferCarShaders[];
extern const size_t kGBufferCarShaderCount;
// ---- the shaders that are left out, with the reason --------------------------------------------------------------------------------------------------------------------
extern const GBufferCarLeftOut kGBufferCarLeftOut[];
extern const size_t kGBufferCarLeftOutCount;

// The census' words for the albedo source of a row.
inline constexpr std::string_view kPlanDiffuse = "diffuse fetch";
inline constexpr std::string_view kPlanDiffuseTint = "diffuse x colour constants";
inline constexpr std::string_view kPlanPaint = "paint register";
inline constexpr std::string_view kPlanConstant = "colour constants";
inline constexpr std::string_view kPlanNone = "none";
std::string_view GBufferCarAlbedoSource(const GBufferCarShader& row);

const GBufferCarShader* FindGBufferCarShader(std::string_view name);
inline bool IsGBufferCarShader(std::string_view name) { return FindGBufferCarShader(name) != nullptr; }
std::vector<std::string_view> GBufferCarShaderNames();

// What the stage reads in one shader's text beyond the normal (pure; public for the transform, the tests and the census): the albedo, and whether the material word takes the shader's own
// `smoothness`. `found` false says why in `why_not` (the plane is then "no data": a text that is not the one the row was written for loses the albedo, not the normal); else `expression` is what
// GbAlbedoEncode takes (a register read at the end of main, a captured copy, or a constant), and when a copy is made `insert_text` goes at `insert_at` (an offset into the text with LF line ends).
struct GBufferCarEdits {
  bool found = false;
  std::string why_not;
  std::string expression;
  size_t insert_at = std::string::npos;
  std::string insert_text;
  bool smoothness = false;  // GBufferCarMaterial::kBumpSpec and the shader declares `smoothness`
  std::string detail;       // the census' words
};
GBufferCarEdits PlanGBufferCarEdits(const GBufferCarShader& row, std::string_view hlsl);

// The text transform (pure). `planes` as kGBufferCarsPlanes. `skip` for any other shader, an error naming the anchor when the text is not the translator's one the edits were written for.
VariantTransformResult TransformGBufferCarsHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx = SoloContext(kFeatureGBuffer),
                                                uint32_t planes = kGBufferCarsPlanes);

// The inserted statements (public for the tests, which take them out again to check the colour path is the game's text byte for byte).
std::string GBufferCarsOutputText(uint32_t planes);                                                  // after `out float4 oC0 : SV_Target0`
std::string GBufferCarsWriteText(const GBufferCarShader& row, const GBufferCarEdits& edits, uint32_t planes);  // after the game's last colour statement

// A stage for the family, as a VariantDef of the G-buffer's feature (tests; gbuffer.cpp puts the names into its one definition).
VariantDef MakeGBufferCarsStageFromText(std::string_view hlsli_text, uint32_t planes = kGBufferCarsPlanes);

}  // namespace larecomp::shaderpack

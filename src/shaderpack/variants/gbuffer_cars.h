#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the G-buffer's CAR, TRAFFIC and CHARACTER families (GB-G0, the decision spike of perf/LIGHT-UNIFY.md 11.2)
// ===========================================================================
// THIS FILE IS A SPIKE'S PROTOTYPE (branch spike/gb-g0). It is not wired into gbuffer.cpp, the pack or the renderer: GB-G1 takes it over (variants/gbuffer_cars.*) and wires it the
// way GB-D's day stage is wired (gbuffer.cpp's TransformGBufferHlsl sends IsGBufferCarShader names here, the VariantDef lists CarShaderNames()), after GB-W1's albedo and GB-F1's
// material word have landed. What it settles is WHERE each plane of a car pixel comes from, by running the retail text, so that G1 and G2 do not have to find out again.
//
// THE DECISION (the evidence: perf/GB-G0.md; every number there is from the retail translation in shaderpack/baseline/hlsl-rpf and the numeric runs of the scripts in
// h11-work/gb-g0-analysis/scripts):
//   NORMAL. The game's own vertex shader of a car, a traffic car and a character already exports the vertex's WORLD normal, unit length, as one of the interpolants (TEXCOORD1, 2
//   or 0 by family) and the pixel shader declares it: the same `kInterpolant` source GB-D's day stage uses for the city. Of the 116 lit vertex shaders of the cars and the characters
//   105 export it (the car matrix and gWorld have rotated it: the survey's count of 6 of 178 looked for the raw vertex normal only), and of the 92 lit pixel shaders 65 take it from
//   there (42 cars, 5 traffic, 18 characters). 8 have no exported normal (6 cars, 1 traffic, 1 pedestrian: the derivative normal as in GB-D; the pedestrian's day vertex shader can
//   export one through GB-D's vertex stage: one anchor, checked numerically), 19 are left out with a reason (blended glass, the power-up effects, the unlit variants, the environment
//   map's views, the normal map bake, and the two night shaders GB-C's stage already edits). So the survey's VS variant (oTexCoord15) is NOT needed for the cars, and the CarFrame
//   normal pass is a second draw of the player's car alone: dominated.
//   ALBEDO. Three sources, by shader: the diffuse fetch (30 of the car shaders, 20 of 20 characters: GB-W1's anchor), a constant the shader has (`baseColor`, `BaseColorC`,
//   `MetalColor`, `tintColor`: the game's own material constant), and the PAINT: the colour is a function of the draw's constants DL2, DL3, DL1.w and the vertex's gradient colour,
//   and the shader has a register that holds it, found by the anchor below and checked numerically (paint_check*.py: the formula of the shader equals the register to 1e-16).
//   MATERIAL. The class is a table by shader group (the rows below carry it for the prototype: GB-F1's class table owns the numbers); the smoothness of a car is a constant of the
//   class here (the paint reads its reflection from two environment maps, there is no specular exponent in it: DL0.w is the power of the environment map's contrast curve).
//
// WHAT THIS PROTOTYPE EDITS (the paint family of the player's car, `xCarPaintCustomizable::PS_CarPaint`, 14 draws a frame; every other row only writes its normal and its material):
//   1. the outputs: `, out float2 oC1 : SV_Target1` (+ `oC2 : SV_Target2` float4 and `oC3 : SV_Target3` float2 when `planes` says so), after `out float4 oC0 : SV_Target0`
//   2. the 48 byte block of GBufferConstants (DeclareConstants), the helper (gbuffer_common.hlsli) before main
//   3. for the paint: one capture after the game's statement `r7.xyz = r7.xyz * r0.yyy + r3.yzx;`: `float3 gbAlbedo = float3(r7.z, r7.x, r7.y);` (the paint with its vinyl layer, rgb)
//   4. for the shaders with no exported normal: the fragment's world point and its derivatives at the top of main (GB-D's text)
//   5. the write after the game's `oC0 = applyBlendPremult(oC0);`: the normal from the interpolant, the albedo (`GbAlbedoEncode`) and the lit weight, the material (RT3)
// The colour path is untouched: the test takes the insertions out again and compares with the retail text byte for byte.
//
// CHECK (nothing here has run): the DXIL and SPIR-V compiles of the edited texts (dxc, the pack's arguments); that the three outputs bind on every back end (the core's kStagePlanes
// must be 3 for the stage to declare them: GB-W1 changes it); the plane order against GB-W1's writers; the smoothness numbers against GB-F1's class table.
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the table, the edits or the checks of the car stage.
inline constexpr uint32_t kGBufferCarsVersion = 1;

enum class GBufferCarNormal : uint8_t {
  kInterpolant,  // iTexCoord<slot>.xyz is the world normal, unit length: the game's vertex shader exports it (found by running the paired vertex shaders: vsnorm_cars.py)
  kDerivative,   // no vertex shader of the family exports a normal: the face's, from the derivatives of the fragment's world point (GbFaceNormal)
};

enum class GBufferCarAlbedo : uint8_t {
  kNone,        // the plane is "no data" for this shader (the prototype; the census says which source G1 uses: GBufferCarShader::plan)
  kPaintVinyl,  // the paint register after the vinyl layer: `float3(r7.z, r7.x, r7.y)` of xCarPaintCustomizable::PS_CarPaint
};

struct GBufferCarShader {
  std::string_view name;       // the group and the entry point, as the translator names its HLSL files
  GBufferCarNormal normal;
  uint32_t slot;               // kInterpolant: k of iTexCoord<k>; else 0
  GBufferCarAlbedo albedo;
  std::string_view class_name; // GB_CLASS_* of the helper
  bool vehicle;                // GB_FLAG_VEHICLE
  std::string_view family;     // "player/racer", "traffic", "ped/character"
  std::string_view plan;       // where GB-G1 takes the albedo from: "diffuse fetch", "diffuse x baseColor", "paint constants", "constant colour", "none"
};

struct GBufferCarLeftOut {
  std::string_view name;
  std::string_view reason;
};

// The numbers of the plan of a shader (GBufferCarShader::plan), for the census.
inline constexpr std::string_view kPlanDiffuse = "diffuse fetch";
inline constexpr std::string_view kPlanDiffuseBase = "diffuse x baseColor";
inline constexpr std::string_view kPlanPaint = "paint constants";
inline constexpr std::string_view kPlanConstant = "constant colour";
inline constexpr std::string_view kPlanNone = "none";

// ---- the table: 73 shaders edited (65 with an exported normal, 8 with the derivative), the census of perf/GB-G0.md -------------------------------------------------------------
extern const GBufferCarShader kGBufferCarShaders[];
extern const size_t kGBufferCarShaderCount;
// ---- the shaders that are left out, with the reason --------------------------------------------------------------------------------------------------------------------
extern const GBufferCarLeftOut kGBufferCarLeftOut[];
extern const size_t kGBufferCarLeftOutCount;

// The anchor of the paint's albedo and the statement that captures it (the only albedo edit of the prototype).
inline constexpr std::string_view kPaintAnchor = "r7.xyz = r7.xyz * r0.yyy + r3.yzx;";
inline constexpr std::string_view kPaintCapture = "\n\tfloat3 gbAlbedo = float3(r7.z, r7.x, r7.y);";

// The smoothness of a class, a placeholder until GB-F1's class table owns it (0..1).
float GBufferCarClassSmoothness(std::string_view class_name);

const GBufferCarShader* FindGBufferCarShader(std::string_view name);
inline bool IsGBufferCarShader(std::string_view name) { return FindGBufferCarShader(name) != nullptr; }
std::vector<std::string_view> GBufferCarShaderNames();

// How many planes the stage declares (1: the normal, 2: + the albedo, 3: + the material): the core's gbuffer::kStagePlanes, which GB-W1 raises.
inline constexpr uint32_t kGBufferCarsDefaultPlanes = 3;

// The text transform (pure). `planes` as above. `skip` for any other shader, an error naming the anchor when the text is not the translator's one the edits were written for.
VariantTransformResult TransformGBufferCarsHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx = SoloContext(kFeatureGBuffer),
                                                uint32_t planes = kGBufferCarsDefaultPlanes);

// The inserted statements (public for the tests, which take them out again to check the colour path is the game's text byte for byte).
std::string GBufferCarsOutputText(uint32_t planes);                                // after `out float4 oC0 : SV_Target0`
std::string GBufferCarsWriteText(const GBufferCarShader& shader, uint32_t planes);  // after the game's last colour statement

// A stage for the family, as a VariantDef of the G-buffer's feature (tests; G1 puts the names into gbuffer.cpp's one definition).
VariantDef MakeGBufferCarsStageFromText(std::string_view hlsli_text, uint32_t planes = kGBufferCarsDefaultPlanes);

}  // namespace larecomp::shaderpack

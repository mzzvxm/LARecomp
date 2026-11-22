#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "gbuffer": a lit pixel shader that also writes the G-buffer's normal, albedo and material
// ===========================================================================
// perf/GBUFFER-SURVEY.md (the hybrid path): the game's forward lighting stays, and the lit pixel shaders are drawn as a variant that, besides the
// game's colour, writes the G-buffer's planes into the sidecar render targets the main view's pipelines carry (src/gpu/gbuffer_types.h: RT1 the
// octahedral world normal, RT2 the albedo and the lit weight, RT3 the smoothness, the class and the flags). The family started as xCityDefault alone (phase 0, the spike); the night's shaders (gbuffer_night.h) and the
// day's (gbuffer_day.h) are all of it now, each with a transform of its own that this file's TransformGBufferHlsl sends them to. The planes are RT1 (the normal), RT2 (the albedo
// and the lit weight: gbuffer_albedo.h, package GB-W1) and RT3 (the smoothness, the class and the flags of the draw's material word: gbuffer_material_plane.h, package GB-W2); the stage declares
// `out float2 oC1 : SV_Target1`, `out float4 oC2 : SV_Target2` and `out float2 oC3 : SV_Target3` (src/gpu/gbuffer_rules.h kStagePlanes).
//
// The shaders (found by name, in the executable's and the RPF's containers: needs_rpf):
//   NIGHT: every `__PSMultiLight`, `...Env` and `...Interior` pixel shader (the city's materials, the props, the traffic cars' body, the pedestrians), xCityDefault::PSMultiLight
//          among them (the spike's shader: `r0` is the normal the lamps are lit with, `r5.w = saturate(dot(r0.zxy, gMultiLightDynamicLightData(10).zxy))`, the sun's term): shaderpack/
//          variants/gbuffer_night.cpp (package GB-C), which finds each one's normal itself
//   CARS:  the 73 car, traffic and character shaders (the player's and the racers' paint, plastics, chrome, rims, tyres; the traffic cars' bodies; the drivers, the pedestrians and the characters) that
//          are not night shaders: shaderpack/variants/gbuffer_cars.cpp (package GB-G1), which takes the normal the game's own vertex shader exports, the albedo from the diffuse texel / the paint's
//          register / a colour constant, and the material from the draw's word; two of its vertex shaders (the day pedestrian's, the traffic paint's) are also edited by the G-buffer's vertex
//          stage, which writes what no interpolant has (GB-G2: the car stage's rows of feature kFeatureGBufferVs, MakeGBufferVsVariantAll in gbuffer_cars.h)
//   DAY:   the 69 `PSCity*` / `PSProp*` shaders (42 edited), xCityDefault::PSCityDefault among them (its vertex shader, VSCityDefaultDay, exports the vertex's normal as oTexCoord0.xyz and the
//          position as oTexCoord1.xyz, both in WORLD space): shaderpack/variants/gbuffer_day.cpp (package GB-D); the day table also says which of them are the day stage's vertex shaders'
//          (feature kFeatureGBufferVs, its own pack entries: gbuffer_day.h) to pair per draw (Scene::GBufferKey)
// The other pixel shaders of the group (the shadow, depth and AO passes) are not in the variant.
//
// What every transform edits (each anchor must be found the number of times said or the variant is not built: the HLSL is not the one it was written for):
//   1. the outputs: `, out float2 oC1 : SV_Target1`, `, out float4 oC2 : SV_Target2` and `, out float2 oC3 : SV_Target3` after `out float4 oC0 : SV_Target0` (RT1 is R16G16_UNORM, RT2 R8G8B8A8_UNORM,
//      RT3 R8G8_UNORM; a pixel shader with more outputs next to SV_Coverage is the shape of the game's own alpha tested shaders)
//   2. the constants: the 48 bytes of gbuffer_types.h GBufferConstants after the 608 of SharedConstants, declared as g_GBufferMode (uint4), g_GBufferRay and
//      g_GBufferCam (float4) with DeclareConstants (variants/stage_constants.h) at the offset the chain gives the block: byte 608 (c38 of the block) alone, for SPIR-V
//      (RawBufferLoad) and DXIL (packoffset), and byte 1680 after the headlight's 1072 bytes when the two are composed
//   3. the helper: src/gpu/hlsl/gbuffer_common.hlsli, pasted before `void main(` (the pack builder embeds the file: CMake, src/shaderpack/CMakeLists.txt)
//   4. the writes, after the game's `oC0 = applyBlendPremult(oC0);` (so after the alpha test's discard):
//        oC1 = float2(0, 0);
//        if ((g_GBufferMode.x & GB_MODE_WRITE_NORMAL) != 0) oC1 = GbRt1Encode(<the shader's normal>);
//        oC2 = float4(0, 0, 0, 0);
//        if ((g_GBufferMode.x & GB_MODE_WRITE_ALBEDO) != 0) oC2 = float4(GbAlbedoEncode(<the shader's diffuse texel>), GbMaterialLit(g_GBufferMode.y));
//        oC3 = float2(0, 0);
//        if ((g_GBufferMode.x & GB_MODE_WRITE_MATERIAL) != 0) oC3 = GbMaterialRt3(g_GBufferMode.y);
//      the normal as the game's lighting has it (the encode divides by the L1 norm, so it need not be normalised); the albedo line only where the shader has a DiffuseSampler texel
//      (gbuffer_albedo.h), the lit weight from the draw's material word; the material line in every shader (gbuffer_material_plane.h: the word's smoothness, class and flags bytes)
//   5. where a value must outlive the register the game keeps it in, one line right after the statement that makes it captures it (`float3 gbNormal = r10.xyz;`,
//      `float3 gbAlbedo = r6.xzw;`)
//
// The mode bits are the host's per-draw switches (Scene::FillVariantBlock sets them for a draw that writes the plane); with one clear the draw writes the "no data" code (0) into
// that plane.
//
// The alpha test twin (the table's spec_twin: the word's bit 0): the same text compiled with the shader's own spec mask, for the draws with the
// alpha test or alpha to coverage on (fences, railings of the family). A discarded pixel writes nothing to either target.
//
// Runtime: src/gpu/scene.cpp chooses it per draw (Scene::GBufferKey), not src/gpu/pipelines.cpp WantedFeatures: it needs to know that the draw is the main view's and
// that the sidecar is attached, and it widens the pipeline's colour targets by the sidecar's. The word it takes is the pack's best one for the draw's features plus this
// one (ShaderStore::ChooseVariant): the composed word on a shader that also has the headlight's stage, else the G-buffer's alone.
//
// THE CHAIN (shaderpack/variant_chain.h): feature kFeatureGBuffer (10), 48 bytes, the alpha test twin; it composes with the headlight's (feature 9: shaderpack/variants/
// headlight.cpp) on the PSMultiLight shaders both edit. The stages go in ascending feature index: the headlight's edits first, then these insertions on the text they leave. This
// stage's anchors are `out float4 oC0 : SV_Target0`, the translator's g_InvResolutionScale declarations (DeclareConstants), `void main(` and `oC0 = applyBlendPremult(oC0);`, the
// shader's normal term and its DiffuseSampler fetch; the headlight rewrites none of them (it adds a local after the cube map's line, a term to the ambient sum and its own helper and
// declarations), so the pair composes in the order the table gives.
// ===========================================================================

#include <string>
#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper's use or the checks of this variant (its stamp line variants_gbuffer_version). The file src/gpu/hlsl/gbuffer_common.hlsli is
// part of the variant's text and is in the stamp through the fingerprint (variants_gbuffer_input).
// 2: the night family (PSMultiLight, ...Env, ...Interior: gbuffer_night.h) is in the variant; every pack that had version 1 is built again.
// 3: the day families (gbuffer_day.h: 42 PSCity / PSProp shaders, their normals from the interpolant, the vertex stage's export or the derivatives); xCityDefault::PSCityDefault is the
//    day table's now (the back face flip is in its text). The day stage's own kGBufferDayVersion and the vertex stage's kGBufferVsVersion (its own pack entry: gbuffer_vs) are bumped by hand
//    for the edits of their own; a change to the day table or the day edits also bumps THIS number, which is the pixel stage's stamp line (variants_gbuffer_version).
// 4: RT2, the albedo and the lit weight (gbuffer_albedo.h, GB-W1): every shader of the stage declares a third output SV_Target2 (src/gpu/gbuffer_rules.h kStagePlanes is RT1 and RT2 in the same
//    commit) and writes it; xCityDefault::PSMultiLight takes the night stage's text (the spike's own edits are gone). kGBufferDayVersion is 2.
// 5: the car, traffic and character families (gbuffer_cars.h, GB-G1: 73 shaders; the albedo from the diffuse texel, the paint's register or a colour constant; the draw's word). The xBumpSpec shaders'
//    own `smoothness` constant. gbuffer_albedo.h's analysis takes a sampler and the gradient fetch (the night and the day stages' own results are unchanged). The helper's GbMaterialWord parameter is
//    gbSmoothness (the file is in the stamp through the fingerprint). Built on feat/gb-g1 alone: its packs have no RT3.
// 6: RT3, the smoothness, the class and the flags (gbuffer_material_plane.h, GB-W2): every shader of the stage declares a fourth output SV_Target3 and writes it (gbuffer_rules.h kStagePlanes is
//    RT1, RT2 and RT3 in the same commit); the helper's parameters are renamed (`smoothness` is a macro in the cars' shaders); kGBufferDayVersion is 3. Built on feat/gb-w2 alone: its packs have no cars.
// 7: both together (the merge of GB-G1 and GB-W2 in feat/gb-phase2): the car stage takes its plane count from kStagePlanes, so it writes oC3 too.
// 8: the traffic and the pedestrians on top (GB-G2, gbuffer_cars.h: the traffic paint's albedo from the vertex stage, the traffic black matte's black albedo, the day pedestrian's normal from the vertex stage;
//    kGBufferCarsVersion 3). GB-G2 alone stamped this 6 (on GB-G1's 5): the merge takes the next free number. A pack of 5, 6 or 7 is stale.
// 9: the night stage's albedo recipes (gbuffer_night_albedo.h, GB-H2-COVER, kGBufferNightAlbedoVersion 1): the road, the tunnel road, the terrain, the grass, the tinted grime, grime decal and grime
//    metal, the tinted prop and the traffic body write an albedo to RT2 where they wrote "no data" (the deferred headlights light them); no other shader's text changes. A pack of 8 is stale.
inline constexpr uint32_t kGBufferVariantVersion = 9;

// The pixel shaders, by name (the group and the entry point, as the translator names its HLSL files). Found by name alone: the identities are whatever
// the game's containers hold.
inline constexpr std::string_view kGBufferDayShader = "xCityDefault__PSCityDefault";
inline constexpr std::string_view kGBufferNightShader = "xCityDefault__PSMultiLight";

// The variant, with the helper text given (tests, a tool) or the build's embedded one (false with *why when this build has none).
VariantDef MakeGBufferVariantFromText(std::string_view hlsli_text);
bool MakeGBufferVariant(VariantDef* out, std::string* why);

// The text transform alone (pure; public for tests). `shader` is only used in messages; `hlsli_text` is gbuffer_common.hlsli.
VariantTransformResult TransformGBufferHlsl(const VariantShader& shader, std::string_view hlsl, std::string_view hlsli_text, const StageContext& ctx = SoloContext(kFeatureGBuffer));

}  // namespace larecomp::shaderpack

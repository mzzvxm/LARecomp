#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "gbuffer": a lit pixel shader that also writes the G-buffer's normal
// ===========================================================================
// perf/GBUFFER-SURVEY.md (the hybrid path): the game's forward lighting stays, and the lit pixel shaders are drawn as a variant that, besides the
// game's colour, writes the G-buffer's planes into the sidecar render targets the main view's pipelines carry (src/gpu/gbuffer_types.h: RT1 the
// octahedral world normal, RT2 and RT3 later). THIS IS THE SPIKE (phase 0): the family is xCityDefault alone, the plane is RT1 alone.
//
// The shaders (found by name, in the executable's and the RPF's containers: needs_rpf):
//   xCityDefault::PSCityDefault    the day shader of the family's material: its vertex shader (VSCityDefaultDay) exports the vertex's normal as
//                                  oTexCoord0.xyz and the position as oTexCoord1.xyz, both in WORLD space (the city's meshes are in world space and the
//                                  vertex shader has no gWorld multiply), and the pixel shader reads them into r0 and r1 and overwrites r0 at once
//   xCityDefault::PSMultiLight     the night shader of the same material: the same interpolants, r0 is the normal the lamps are lit with
//                                  (`r5.w = saturate(dot(r0.zxy, gMultiLightDynamicLightData(10).zxy))`, the sun's term)
// The other pixel shaders of the group (PSMultiLightInterior, the shadow, depth and AO passes) are not in the variant.
//
// What the transform edits (each anchor must be found the number of times said or the variant is not built: the HLSL is not the one it was written for):
//   1. the output: `, out float2 oC1 : SV_Target1` after `out float4 oC0 : SV_Target0` (RT1 is R16G16_UNORM; a pixel shader with a second output next to
//      SV_Coverage is the shape of the game's own alpha tested shaders)
//   2. the constants: the 48 bytes of gbuffer_types.h GBufferConstants after the 608 of SharedConstants, declared as g_GBufferMode (uint4), g_GBufferRay and
//      g_GBufferCam (float4) with DeclareConstants (variants/stage_constants.h) at the offset the chain gives the block: byte 608 (c38 of the block) alone, for SPIR-V
//      (RawBufferLoad) and DXIL (packoffset), and byte 1280 after the headlight's 672 bytes when the two are composed
//   3. the helper: src/gpu/hlsl/gbuffer_common.hlsli, pasted before `void main(` (the pack builder embeds the file: CMake, src/shaderpack/CMakeLists.txt)
//   4. the write, after the game's `oC0 = applyBlendPremult(oC0);` (so after the alpha test's discard):
//        oC1 = float2(0, 0);
//        if ((g_GBufferMode.x & GB_MODE_WRITE_NORMAL) != 0) oC1 = GbRt1Encode(iTexCoord0.xyz);
//      the interpolated normal as the game's lighting has it: the encode divides by the L1 norm, so it need not be normalised
//
// The mode bit is the host's per-draw switch (Scene::FillVariantBlock sets it for a draw that writes the plane); with it clear the draw writes the "no
// data" code (0) into RT1.
//
// The alpha test twin (the table's spec_twin: the word's bit 0): the same text compiled with the shader's own spec mask, for the draws with the
// alpha test or alpha to coverage on (fences, railings of the family). A discarded pixel writes nothing to either target.
//
// Runtime: src/gpu/scene.cpp chooses it per draw (Scene::GBufferKey), not src/gpu/pipelines.cpp WantedFeatures: it needs to know that the draw is the main view's and
// that the sidecar is attached, and it widens the pipeline's colour targets by the sidecar's. The word it takes is the pack's best one for the draw's features plus this
// one (ShaderStore::ChooseVariant): the composed word on a shader that also has the headlight's stage, else the G-buffer's alone.
//
// THE CHAIN (shaderpack/variant_chain.h): feature kFeatureGBuffer (10), 48 bytes, the alpha test twin; it composes with the headlight's (feature 9: shaderpack/variants/
// headlight.cpp) on xCityDefault::PSMultiLight, the one shader both edit. The stages go in ascending feature index: the headlight's edits first, then these four insertions
// on the text they leave. This stage's anchors are `out float4 oC0 : SV_Target0`, the translator's g_InvResolutionScale declarations (DeclareConstants), `void main(`
// and `oC0 = applyBlendPremult(oC0);`; the headlight rewrites none of them (it adds a local after the cube map's line, a term to the ambient sum and its own helper and
// declarations), so the pair composes in the order the table gives.
// ===========================================================================

#include <string>
#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper's use or the checks of this variant (its stamp line variants_gbuffer_version). The file src/gpu/hlsl/gbuffer_common.hlsli is
// part of the variant's text and is in the stamp through the fingerprint (variants_gbuffer_input).
inline constexpr uint32_t kGBufferVariantVersion = 1;

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

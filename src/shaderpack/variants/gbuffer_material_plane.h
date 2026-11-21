#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the G-buffer stage's RT3 writer: the smoothness, the class and the flags (package GB-W2 of perf/LIGHT-UNIFY.md 11.2)
// ===========================================================================
// src/gpu/gbuffer_types.h: RT3 is R8G8_UNORM, r = the smoothness (the perceptual one: the GGX alpha is (1 - s)^2, gbuffer_rules.h part 3), g = the class (5 bits) with the three flags
// (SSR eligible, alpha covered, vehicle) in the byte's top bits. Both stages of the G-buffer variant (the night's gbuffer_night.cpp and the day's gbuffer_day.cpp; the car stage of
// GB-G1 builds its own word first, see below) put them there with the code of this file, so the two cannot drift apart. Header only (the day test builds without gbuffer.cpp).
//
// WHERE THE NUMBERS COME FROM. Nothing is computed in the shader: RT3 is the draw's material word (GBufferConstants::material, which the host makes per draw and uploads as
// g_GBufferMode.y: src/gpu/scene.cpp FillVariantBlock) cut into its two bytes by `GbMaterialRt3` of the frozen helper (hlsl/gbuffer_common.hlsli):
//     the class table of GB-F1 (gbuffer_material.h: the shader group's class and flags, the smoothness of the class, the gloss the draw's own `SpecularExponent` gives),
//     then the per-texture row of GB-F2 (gbuffer_materials.h: the texture the draw takes its colour from says it is glass, chrome, wet asphalt, or how rough it is),
//     then the alpha test twin's flag (the pixel shader the draw takes is the spec 1 / 2 variant).
// The word's other fields are not in RT3: the lit weight (bits 16 to 23) goes to RT2.a (gbuffer_albedo.h), and the metalness byte (bits 24 to 31: the per-texture rows' `metal=`) has no plane
// (the interface is frozen: kMetal and kChrome mean F0 = albedo, which is what a consumer reads), so the shader does not write it.
//
// THE PIXEL'S OWN PART is not here yet: a shader that has a specular map could multiply the smoothness by its texel (the survey's "smoothness times a specular map"); the plane is the draw's
// value for now, the same for every pixel of the draw. CHECK: whether the reflections' pictures want the map (a brushed floor, a chrome strip on a wall) once they run.
//
// WHAT IS WRITTEN. After the game's colour statement (so after the alpha test's discard) every shader of the stage writes, in this order,
//     oC3 = float2(0.0f, 0.0f);                                                      the "no data" code (class 0): a draw whose mode bit is clear, the day stage's cards
//     if ((g_GBufferMode.x & uint(GB_MODE_WRITE_MATERIAL)) != 0u) {
//       oC3 = GbMaterialRt3(g_GBufferMode.y);                                        the word's bytes
//     }
// Unlike RT2's albedo this needs no anchor in the shader's text, so EVERY shader of the stage writes it, the ones with no diffuse texel (roads, terrain, the traffic cars' body ...) too:
// a pixel of the stage has a class and a smoothness whether or not it has an albedo. The cards of the day stage (kMasked: tree impostors, billboards) write "no data" in every plane.
// The mode bit is the host's per-draw switch (gbuffer_rules.h ModeBits: the plane is written when the frame's plan has it, `mcla_gpu_gbuffer=full` or a consumer's Want of RT3).
//
// All of it only ADDS lines: the output and the write. The colour is the game's (the stages' tests check that the text, minus the added lines, is the game's, line by line).
// ===========================================================================

#include <string>
#include <string_view>

namespace larecomp::shaderpack {

// The fourth colour output, after the third (the albedo): SV_Target3 is RT3 (gbuffer_rules.h kStagePlanes: the stage declares as many outputs as it has planes).
inline constexpr std::string_view kGBufferMaterialOutput = "\n\t, out float2 oC3 : SV_Target3";
// What every shader of the stage writes to RT3 whatever else: the "no data" code. A draw that does not write the material plane (the mode bit clear) and the day stage's cards end here.
inline constexpr std::string_view kGBufferMaterialNoData = "\n\toC3 = float2(0.0f, 0.0f);";
// The write the mode bit guards (the one line the tests look for).
inline constexpr std::string_view kGBufferMaterialWriteLine = "oC3 = GbMaterialRt3(g_GBufferMode.y);";

// What the stage writes to RT3 after the game's colour statement: the "no data" code, and, unless `card` (a shader with no surface of its own), the draw's material word under the
// host's mode bit.
inline std::string GBufferMaterialWriteText(bool card = false) {
  std::string text(kGBufferMaterialNoData);
  if (!card) {
    text += "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_MATERIAL)) != 0u) {\n\t\t";
    text += kGBufferMaterialWriteLine;
    text += "\n\t}";
  }
  return text;
}

}  // namespace larecomp::shaderpack

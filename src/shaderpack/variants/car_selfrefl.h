#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "car_selfrefl": the player's car paint reflects its own car (G8b, a test prototype)
// ===========================================================================
// The car's paint (xCarPaintCustomizable::PS_CarPaint, identity 439B193A3F675533 on the retail game, in the RPF: the
// 14 draws a frame of the player's body) lights the body and reflects the world with two environment reads: the sharp one
// (`dpFrontSampler`, the 4-level 512x256 dual paraboloid map at the reflection vector, `r10.xyz`) and the pre-blurred one
// (`dpSpecFrontSampler`, the 64x32 copy, `r13.xyz`). The only occlusion on that reflection is the baked per-vertex AO and a
// world-space sun visibility, so the wing does not show in the trunk lid and the mirrors do not show in the doors
// (feature-parity/G8A-CAR-SELFREFL.md). The car is drawn before the world, so the paint cannot see the current frame's
// depth; this variant reads what the host kept of the PREVIOUS frame (src/gpu/car_selfrefl.h): one image with the frame's
// scene colour in rgb and, in alpha, the view distance of the pixels that belong to a vehicle (0 elsewhere).
//
// What the edit does, in one helper block before main and three anchored lines:
//   1. after each of the two environment reads the colour goes through CarReflApply: a short ray is marched from the
//      shaded point along the reflected view vector through that image (the world position, the normal and the view
//      vector are the pixel shader's own interpolants, iTexCoord1, iTexCoord2 and iTexCoord6; the matrix that takes
//      this frame's world position into the history's clip space has the vehicle's own motion in it); where it passes
//      just behind a vehicle pixel the reflection is darkened (mode 1, the specular occlusion) or replaced by that pixel's
//      colour (mode 2, the self-reflection), by a weight made of: how well the shaded point finds itself in the history
//      (a disocclusion or a wrong motion guess fades the whole effect), the distance along the ray and the screen edge.
//      The sharp read takes the weight, the blurred one half. Everything else of the paint (its fresnel, its gloss, its
//      metal tint, the baked AO) weighs the changed colour as it weighed the game's, which is how the hit is "blended into the
//      reflection by the paint's own fresnel".
//   2. a debug view replaces the paint's colour at the end (the constants' view number).
// The constants (112 bytes at byte 608 of the draw's SharedConstants block, src/gpu/constants.h CarSelfReflConstants):
//   float4 g_CarReflM0..M3   world to the history frame's clip space (rows of a row-vector matrix: clip = p.x M0 + p.y M1 + p.z M2 + M3)
//   float4 g_CarReflA        x mode (0: the game's paint, 1: occlusion, 2: occlusion and self-reflection), y ray length (m),
//                            z thickness (m), w strength
//   float4 g_CarReflB        x steps, y debug view, z colour gain (the history's units to the paint's), w noise frame
//   uint4  g_CarReflC        x the history image's bindless slot, y a linear clamp sampler's slot (not read since version 2: the
//                            image is read as points)
// With the mode 0 the helper returns the game's colour untouched, so a draw that has no history yet (the first frames, a
// pause view, a camera cut) is the game's own paint.
//
// Edits (anchors that must each be found the number of times said, or the variant is not built):
//   `r10.xyz = tfetch2DGrad(dpFrontSampler_...).xyz;`  (once)     the sharp read, followed by the trace and the apply
//   `r13.xyz = tfetch2D(dpSpecFrontSampler_...).xyz;`  (once)     the blurred read, followed by the apply at half weight
//   `oC0.xyz = r3.www * -r0.xzy + r0.xzy;`             (once)     the last write of the colour, followed by the debug view
//   `CubeMapData cubeMapData = (CubeMapData)0;` (once)  the local result;  the two declarations of g_InvResolutionScale
//   (SPIR-V and DXIL, once each) get the constants after them;  `void main(` (once) gets the helper before it
//
// CHECK: both edited texts (the translator's HLSL of the baseline tree with these edits) compile with dxc to DXIL
// (ps_6_0) and SPIR-V with the pack's arguments (the development notes of this package say with which dxc). CHECK in the pack
// step: the stamp says variants_car_selfrefl_shaders=439b193a3f675533. CHECK in a run: the paint draws have no alpha test or
// alpha to coverage (the census of G8A: variant 0, a2m 0 for the paint groups), else they keep the game's shader.
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line variants_car_selfrefl_version).
inline constexpr uint32_t kCarSelfReflVariantVersion = 2;  // 2: the hit's colour is a point read of an image whose rgb is 0 outside the vehicles

// The identity on the retail game (the runtime's own: FNV-1a 64 of the microcode) and the name the translator gives it.
inline constexpr VariantShader kCarPaint = {"xCarPaintCustomizable__PS_CarPaint", 0x439B193A3F675533ull};

VariantDef MakeCarSelfReflVariant();

// The text transform alone (pure; public for tests).
VariantTransformResult TransformCarSelfReflHlsl(const VariantShader& shader, std::string_view hlsl);

}  // namespace larecomp::shaderpack

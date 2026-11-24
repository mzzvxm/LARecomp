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
//      just behind a vehicle pixel the reflection is darkened (mode 1, the specular occlusion) or replaced by what the ray
//      sees there (mode 2, the self-reflection), by a weight made of: how well the shaded point finds itself in the history
//      (a disocclusion or a wrong motion guess fades the whole effect), the distance along the ray, the screen edge, and
//      how sure the hit is (see below). The sharp read takes the weight, the blurred one half. Everything else of the paint
//      (its fresnel, its gloss, its metal tint, the baked AO) weighs the changed colour as it weighed the game's, which is
//      how the hit is "blended into the reflection by the paint's own fresnel".
//   2. a debug view replaces the paint's colour at the end (the constants' view number).
//
// The ray (version 4, after pictures at 4K of version 3 showed a patch under the door mirror of hard-edged bands that follow the
// points, and a stray blob on the door; version 3 had removed the dither of version 2):
//   - Its points are the same for every pixel and every frame, closer together near the shaded point (distance = length u^1.5,
//     u = (point + 1) / steps). Version 2 moved the points of each pixel by an interleaved noise, so a part narrower than the
//     steps (a mirror's arm, a wing's blade, a door seam) was caught by one pixel and missed by the next: a dotted patch.
//   - The ray is a chain of SEGMENTS, point to point (the first from the shaded point, which is on the vehicle and in no window). What
//     a segment takes is the share of its length inside the window of "just behind a vehicle pixel" (a rise from the bias, which holds
//     the game's depth noise and a slack for a slanted surface, to 1, and a fall between half the thickness and the thickness): the
//     mean of the window along the line in the signed depth difference from one point to the next, which is exact (the window is the
//     difference of two smoothsteps, and its integral is a polynomial). So a shell between two points counts for the part of the
//     segment it takes, not for whether a point fell in it; version 3's heaviest point and its widened thickness are gone.
//   - A segment whose ends disagree about the surface in a way a line does not say (one end in a window and the other on no vehicle,
//     or on a layer more than half the thickness away: the outline of a part, a mirror in front of a door) is halved three times to
//     find where (three more reads of the history, only for such segments: 12.8 reads a ray on average, 18 at most, over the unit test's
//     rows, which all cross a part) and
//     takes the share of its length on the window's side of the outline. A part's outline that moves across a segment moves the
//     weight by the share it moved: a ramp as wide as the distance between two points, where version 3's weight stepped by 0.4 to
//     0.7 at the outline of every point's copy of the part (the bands), and a part narrower than the segment takes the share it is wide.
//   - The ray's hit is the sum of the segments' covers, each times a fade with its distance along the ray (1 up to 0.3 of the ray's
//     length, 0 from 0.9: a lone catch far along the ray, the stray blob, takes nothing), the screen edge's fade and a hidden-face
//     factor. The whole hit takes `full` (0.25, the constants' b.w, the cvar mcla_gpu_car_selfrefl_full) of the ray's points, 3 of 12:
//     a part the ray goes through takes the whole hit, a part one point catches a third of it, and a point that enters or leaves a part
//     changes the weight by a third of the hit at most. The hit's colour, face and distance are the covers' weighted means: no
//     "heaviest point" to switch between. The hidden face: a ray that is coming back out towards the surface from behind (the
//     previous point behind the vehicle pixel, this one nearer by a tenth of the thickness per 0.15 m of ray: CarReflApproach)
//     reaches the hidden side of the part, which the image does not hold (a wing's underside, a mirror's door-facing back): its
//     weight is halved, and mode 2 shows the paint's own colour of the shaded point in shade (0.4 of it) instead of the colour of
//     the outer shell. A ray that goes in at the front and deeper, or that comes into a part's outline from the side, is the visible
//     face, at full weight. The darkest a hit makes the reflection is 85%.
//   - The history's depth is read as points, so the self-test and the bias carry a slack for a slanted surface (the distance
//     changes across a texel by tan of the angle to the view): at a grazing angle and at low resolutions they stay quiet.
// The ray traced image (version 5, src/gpu/car_rt_refl.h): on a device that can trace the player's car's triangles the host makes, at the same
//   trigger as the history and of the same size, an image that holds for every vehicle texel of the history the car's reflection of ITSELF,
//   exact (a part the camera does not see, a thin part): rgb the colour the reflection ray's hit shows (the history's colour where the camera
//   sees that part, the texel's own colour in shade at half the weight where it does not: the hidden-face rule of this variant, kept), alpha
//   the code (0 no data, 1 the car and no hit, 1 + w a hit of weight w, -(1 + w) a hit on a part the camera does not see). When the frame has
//   such an image (g_CarReflC.w bit 0) the trace reads it at the shaded point's place in the history (the one the self-test above finds the
//   point at: its 2x2 texels, those that are the same surface and have data), and its answer is the hit: its weight times the self-test's
//   agreement, its colour, no march. Where the image has no data there (alpha 0: not the player's car by the triangles, another vehicle, the
//   frame's first) the march above runs. Without the bit (the setting off, a device that cannot, no structures this frame) the output is
//   version 4's, bit for bit. The debug view 5 shows the image's state at the pixel.
// Version 6 (the image's weight is the share of a CONE of rays that hit, a glossy reflection, and its texels are no longer a hit or a miss): the
//   paint reads the image with a quadratic B-spline of the 3x3 texels around the shaded point's place in the history, in place of the 2x2
//   bilinear read (src/gpu/car_rt_refl_rules.h BSplineWeights, ReadForPaint: the same arithmetic). The bilinear read's weights have a corner at
//   every texel, which at 4K with the half size image showed as two pixel stair-steps at the edge of a reflected part; the B-spline's have
//   none. Nothing else of the paint's text changes: the agreement test with the history's distance, the half-of-the-matching-texels
//   rule and the codes of the image are version 5's.
// The constants (112 bytes at byte 608 of the draw's SharedConstants block, src/gpu/constants.h CarSelfReflConstants):
//   float4 g_CarReflM0..M3   world to the history frame's clip space (rows of a row-vector matrix: clip = p.x M0 + p.y M1 + p.z M2 + M3)
//   float4 g_CarReflA        x mode (0: the game's paint, 1: occlusion, 2: occlusion and self-reflection), y ray length (m),
//                            z thickness (m), w strength
//   float4 g_CarReflB        x steps, y debug view, z colour gain (the history's units to the paint's), w the share of the points that,
//                            all inside a part, take the whole hit (0.25; it was the noise frame before version 4)
//   uint4  g_CarReflC        x the history image's bindless slot, y a linear clamp sampler's slot (not read since version 2: the
//                            image is read as points), z the ray traced image's bindless slot, w flags: bit 0 the frame has that image
//                            (version 5; the constants' words zw were reserved before)
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
inline constexpr uint32_t kCarSelfReflVariantVersion = 9;  // 2: the hit's colour is a point read of an image whose rgb is 0 outside the vehicles; 3: no per-pixel noise, weights that ramp, hidden-face and thin-part floors; 4: the ray is a chain of segments with a continuous cover, the outline of a part found by halving; 5: the paint reads the host's ray traced image of the car's reflection of itself in place of the march where it has data (constants g_CarReflC.zw), debug view 5; 6: the ray traced image is read with a 3x3 quadratic B-spline of texels (the cone's weights, no stair-steps); 7: the image's sign is "mostly on another car" and the paint adds the clear coat's layer for it (constants g_CarReflC.w bit 1), debug views 6 and 7; 9: no double precision in the DXIL (the ternaries of bare literals take an f suffix): a GPU without it (the Intel iGPUs on D3D12) could not create the paint's pipelines of this variant

// The identity on the retail game (the runtime's own: FNV-1a 64 of the microcode) and the name the translator gives it.
inline constexpr VariantShader kCarPaint = {"xCarPaintCustomizable__PS_CarPaint", 0x439B193A3F675533ull};

VariantDef MakeCarSelfReflVariant();

// The text transform alone (pure; public for tests).
VariantTransformResult TransformCarSelfReflHlsl(const VariantShader& shader, std::string_view hlsl,
                                                const StageContext& ctx = SoloContext(kFeatureCarSelfRefl));

}  // namespace larecomp::shaderpack

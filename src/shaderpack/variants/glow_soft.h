#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "glow_soft": the glow sprites' visibility as a share, not a compare
// ===========================================================================
// The halo of a street lamp, its star and the other glow sprites of the night are drawn by the game's glow
// renderer (AddGlow -> flush -> the xstar_glow shaders): about 64 draws a frame with
// xstar_glow__PSTrailSeedRT (the round glows, and the trail) and one with xstar_glow__PSSeedRT1 (the star).
// Each decides in its pixel shader, with ONE hard depth compare at the glow's projected centre against the
// mid-scene depth copy (the shader's DepthMapSampler), whether the sprite draws at all: the scene at that
// one texel nearer than the glow, and the whole sprite is left out (it writes zeros). A glow whose centre
// sits on an edge, a palm trunk or a pole, pops fully on and off as the wind or the camera moves it by a
// fraction of a pixel (feature-parity/LAMP3X.md: the K-NIGHT street lamp beside a palm, dim in 2 of 5 runs
// at 3x and in two dusk pictures at 1x).
//
// The variant replaces the one tap by a share. GlowVisibleShare() takes 24 depth taps on a disc of radius
// 2.5 pixels of the game's 1280x720 screen round the projected centre (a Vogel spiral: a golden angle
// turn per tap, the radius the square root of the index, so the taps cover the disc evenly), makes the
// game's own compare at each, and the share of the taps that pass is the sprite's visibility:
//
//     vis = smoothstep(0, 1, saturate(2 * share of the taps that pass))
//
// so a sprite is shown in full while half of its disc or more is in front of the scene and fades to nothing
// below that, along a smoothstep (it is flat where a lamp beside an edge mostly sits, and fades in the
// middle): it is a halo, not a point, and the part of it round the edge of a trunk still lights the air.
// A glow wholly in front of the scene (all taps pass) is the game's picture, a glow wholly behind it (none)
// is left out as the game does; only the edge is different: the fade takes about two pixels of the glow's
// travel (from half of the disc hidden to all of it), in place of a step.
//
// No snap to the guest pixel (LAMP3X.md 7a proposed one for scale parity). The disc is sized in guest pixels,
// so at a render scale of S it covers the same part of the picture and holds S times as many depth texels; a
// tap is a point fetch of the finer copy, and the share is an area estimate that is the same on average at every
// scale. Moving each tap to the centre of its guest pixel would give back the console's 720p quantisation: the
// share would only change when an edge crossed a guest pixel centre, then by a whole column of taps (about a
// fifth of the disc), where the finer copy lets it follow a sway of 0.3 px in steps of single taps. It would
// buy exact parity with 1x, which nothing measured asks for: LAMP3X.md finds no scale effect (2 of 5 dim runs at
// 3x against 0 of 6 at 1x and 1.5x, p = 0.18) and the single tap, the only thing that depended on the scale
// of the copy, is gone. At 1x the two are the same, a point fetch takes the texel the position is in.
//
// Cost: the shader takes the 8 outer taps first (the last 8 of the spiral, round the rim in all directions) and
// stops when all 8 agree, which is every glow that is not near an edge (the rim all clear, or all hidden); the
// 16 inner taps only for the sprites that are.
// All pixels of a sprite share the same taps (the centre is the sprite's, from its vertices), so the branch
// is coherent. The depth copy's single level is fetched with an explicit level (SampleLevel), as the
// bloom variant does, so no gradient operation sits in a branch.
//
// The two shaders keep their own control flow. The tap line is replaced by the call of the helper and by
// a "depth" that makes the game's compare come out as the share's answer (-1e20 when any tap passes: the
// compare passes; 1e20 when none does: it fails), so the game's `if (p0)` paths run unchanged for every
// sprite with a share above 0, and only the colour that is written, the one the glow is added to the picture
// with, is multiplied by the share (the blend of both is ONE, ONE, ADD on the colour target: the pipeline
// cache of the retail game shows it, so the share is the sprite's brightness). The alpha written keeps the
// game's own value. No extra constants: the 1280x720 is the game's (render_scale.h kAssumedScreenWidth)
// and the render scale does not enter. The edits (anchors that must match, or the variant is not built):
//
//   PSTrailSeedRT: the tap (r3.x = tfetch2D(DepthMapSampler...), r3.yx), the compare against 1 - TexCoord0.z
//                  and its `TexCoord0.z > 0` guard (the test is off for a glow with z <= 0: then share 1), the
//                  colour write oC0.xyz = saturate(r2.wwww * r2.xyzz + c254.yyyy) times the share
//   PSSeedRT1:     the tap (r0.w = tfetch2D(DepthMapSampler...), r1.xy), the compare of c255.x - TexCoord0.z with the depth
//                  (strict: the star draws when the limit is greater than the depth), the colour write
//                  oC0.xyz = saturate(r1.zyx * r2.xyz) times the share
//   both:          the helper, before `void main(`
//
// THE SCREEN'S EDGE (version 2, HL-EDGE-FADE: the owner saw a traffic car's headlights go weak as the car neared the edge of the frame). The depth copy's
// sampler clamps, so a tap outside the screen reads the edge pixel, which has nothing to do with a lamp that is off the screen: the lamp was tested
// against whatever stands at the edge in its row. A tap outside the screen now counts the less the farther outside it is: its answer goes to "visible"
// over kGlowSoftEdgeFade (16) guest pixels, so a glow whose centre is 16 + 2.5 pixels or more beyond the edge is shown in full, and one that crosses the edge
// changes its visibility smoothly (the edge pixel's compare, then more and more of "visible"). Taps on the screen are as before.
//
// Runtime: src/gpu/pipelines.cpp selects it per draw (the feature kFeatureGlowSoft) while the setting
// mcla_gpu_glow_visibility is `soft`; `game` selects nothing, so the picture is the game's own, bit for bit.
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line
// variants_glow_soft_version). 2: a tap outside the screen is blended to "visible" (kGlowSoftEdgeFade).
inline constexpr uint32_t kGlowSoftVariantVersion = 2;

// The glow shaders' identities on the retail game (the runtime's own: FNV-1a 64 of the microcode; the
// names are the translator's HLSL files, group__entry).
inline constexpr VariantShader kGlowSoftTrail = {"xstar_glow__PSTrailSeedRT", 0xF6A55A0E0E26E0BBull};
inline constexpr VariantShader kGlowSoftStar = {"xstar_glow__PSSeedRT1", 0x92CAA8C0D85D8EC2ull};

// The numbers the helper is made of (the unit test reads them back from the transformed text).
inline constexpr int kGlowSoftTaps = 24;           // taps of the disc
inline constexpr int kGlowSoftFirstTaps = 8;       // taps of the first stage: the outer 8 of the spiral (16..23)
inline constexpr double kGlowSoftRadius = 2.5;     // the disc's radius, in pixels of the game's 1280x720 screen
inline constexpr double kGlowSoftFullShare = 0.5;  // the share of the taps that shows the sprite in full
inline constexpr double kGlowSoftEdgeFade = 16.0;  // guest pixels: a tap this far outside the screen (or more) counts as visible, nearer in the blend of the clamped edge
                                                   // pixel's compare (the depth copy's sampler clamps) and "visible"

VariantDef MakeGlowSoftVariant();

// The text transform alone (pure; public for tests). `shader` is kGlowSoftTrail or kGlowSoftStar (by name).
VariantTransformResult TransformGlowSoftHlsl(const VariantShader& shader, std::string_view hlsl);

}  // namespace larecomp::shaderpack

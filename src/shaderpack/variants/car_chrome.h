#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "car_chrome": the car's chrome under the car's own body (the tailpipes)
// ===========================================================================
// The owner's words: "the tailpipes are unrealistically bright and shiny, as if they were lit from above even though the car body is
// supposed to be blocking the light".
//
// WHICH SHADER. The four exhaust tips of the player's R34 (two dual tips, 324 triangles each) are drawn by xChrome::PS_Chrome, identity
// 2D8CA0430FF88ED7 on the retail game (the car dump of frame 14104, cardump2-run: the tip groups are `1_e90f9598de6c8e52_2d8ca0430ff88ed7`,
// 7 draws a frame of the player's car: the two tips, a badge or trim at the middle of the boot (209 vertices), a thin plate (32) and
// three small trims; feature-parity/G8A-CAR-SELFREFL.md 1.1 names the identity). The same shader draws the chrome of every car.
//
// WHY IT IS BRIGHT (xChrome__PS_Chrome.hlsl, 745 lines of the translator's text, the whole colour is the last ten lines):
//     sun  = 0.125 + 1.185 * saturate(shadowImage(world.xz) - 0.25)             FastShadowCollector: the 256 m TOP-DOWN image of the world's
//                                                                               sun shadows, by world x and z, no height: no car in it
//     env  = dpFrontSampler(paraboloid(v - 2 (v.n) n))                          the dual paraboloid environment map, a sharp read, whose
//                                                                               gradients come from how fast the normal changes
//     blend = saturate(|dn/dx| - 0.15) + saturate(|dn/dy| - 0.15)               a thin tube far away: every pixel's normal is far from its neighbour's
//     out  = vertexColour.a * sun * mix(env, ambYPos, saturate(blend))          ambYPos = the ambient colour of the sky above (PS c57)
// Nothing in it knows the car's own body: the reflection looks up the sky wherever the normal points, ambYPos is the sky's ambient at every
// pixel the blend reaches (at 1080p nearly all of a tip), the sun image has no car in it, and the vertex alpha (the baked AO) is what the
// artists painted, which for these parts is 1 (CHECK: not read from the game's data). And the geometry says the tips are the opposite of open:
// in the dump the diffuser's lip is 1.7 to 3.7 cm above the tubes, the exhaust's brushed metal housing 3 to 7 cm in front of their tops,
// and a hemisphere of 48 random rays from a tip's top finds the sky for 0 to 10 percent of them (the rim that faces the back, 50 to 90).
// So the light "from above" is the sky ambient and the sky's reflection, both unoccluded.
//
// THE FIX. The history image of the car (src/gpu/car_selfrefl.h: the picture's vehicle pixels of the previous frame, rgb the scene colour,
// alpha the view distance) is the paint variant's input and is here too, with the same matrix and constants (112 bytes at byte 608 of the
// shared block, src/gpu/constants.h CarSelfReflConstants): the car paint's variant (car_selfrefl.h, variant id 7) is the model and its
// helper (version 4's segments, the window, the outline found by halving) is copied here with the numbers of parts that are centimetres
// apart (src/gpu/car_chrome_rules.h): two short rays from the shaded point, each marched through the image,
//   - the REFLECTION probe, along the reflected view vector (0.25 m, 12 points): its weight darkens `env`;
//   - the SKY probe, along the normal bent towards the sky (0.20 m, 8 points): its weight darkens `ambYPos`,
// by up to 90 percent, scaled by mcla_gpu_car_selfrefl_strength; the chrome's sun and AO are left to the game. The reflection is
// darkened and not replaced by what the ray sees (the paint's mode 2): the history's colour of a part centimetres away is the previous
// frame's chrome, a loop with a gain of 1, and a darkening cannot run away; the setting's two levels (occlusion, reflection) are the
// same here. The probes run on the chrome's pixels only (a tip is a few hundred pixels at 4K; a pair of rims a few percent).
// Why not the paint's own ray: its window opens 3.7 cm plus the slack behind a vehicle pixel at 6.7 m and its first point is 4 cm out,
// which is past the lip (and its agreement test lets the lip next to a tube pass for the tube). Why not
// the car's sun shadow map (car_shadow_map.h): this shader has no sun highlight, only the sun factor above, and the map of frame N is
// made at the history trigger AFTER the car's draws, so the chrome of frame N would need the map of frame N-1 bound in a draw of the
// scene (its layout, its matrix from the previous camera): a host change in files that are being rewritten, with a texture state that
// only a run can prove. It is the next step for the sun factor (it would replace `sun` by the car's own shadow: see the report of the
// package). Why not a baked vertex AO: the vertex buffers are the game's.
//
// THE RAY TRACED IMAGE (src/gpu/car_rt_refl.h): where the frame has one (g_CarReflC.w bit 0, the slot in g_CarReflC.z),
// the reflection probe's weight is read from it at the shaded point's place in the history (the paint's version 6 read, 3x3 B-spline,
// weight only; the image holds every vehicle texel) and the march is not made: the exact answer for the reflection. The image has nothing
// of the hemisphere, so the sky probe is always marched; the helper's CarChrSkyProbe is where a ray traced ambient occlusion replaces it.
// Without the bit this reads exactly as written above. Until that branch is merged the bit is never set.
//
// ANOTHER CAR'S CHROME (version 2; src/gpu/car_chrome_rt_rules.h has the whole story): the variant is the shader's, so the chrome of every car goes through
// it, and for another car it has marched the history image all along (the history holds every vehicle's pixels, the constants are the draw's stencil ID's).
// Where the host traced the frame's image AND the car is in its scene (the nearest few, within a radius), the image holds, at the texels of that car's chrome,
// a texel of a second kind: alpha 1 + the reflection probe's weight (as the first kind) and the colour's red -(1 + the sky probe's weight), a value no colour
// has. The read tells the kinds apart by that sign (the first kind's arithmetic is the older one's, unchanged), and where the second kind covers the footprint
// it answers BOTH probes and nothing is marched: the sky probe's answer is the only place the image has one. The debug view 4 shows it cyan.
//
// THE RIM OF A TIP (version 3). The history holds one sample of each pixel (the guest's sample 0; one texel per 2x2 pixels from 2560 across), the shader runs at
// the pixel's centre: at the top and the left edge of a tip the texel under a pixel is what is behind it (the bumper's cavity, 3 to 7 cm further, or no vehicle)
// and the self-test of the setup failed on that 1 pixel rim, leaving the game's unoccluded chrome there: a thin bright line round the tip (the debug view 4 shows
// that rim red; ray traced run rtint1, chase shots). The self-test now takes the best agreement of the own texel and the 12 round it (one or two steps away;
// car_chrome_rules.h SilhouetteAgreement), read only for a pixel whose own texel does not agree fully.
//
// What the edit does, in one helper block before main and four anchored lines:
//   `r1.xyz = tfetch2DGrad(dpFrontSampler_...).xyz;`  (once)     the environment read, followed by the setup (the probes' state and the
//                                                               reflection probe) and `env` through CarChromeEnv
//   `r0.xyz = -r1.xyz + ambYPos.xyz;`                  (once)     the ambient, through CarChromeAmbient (the sky probe, run where the blend
//                                                               is above 0.02)
//   `oC0.xyz = r0.www * r0.xyz;`                       (once)     the last write of the colour, followed by the debug view
//   `CubeMapData cubeMapData = (CubeMapData)0;` (once)  the local state;  the two declarations of g_InvResolutionScale (SPIR-V and DXIL,
//                                                      once each) get the constants after them;  `void main(` (once) gets the helper before it
// The pixel shader's own inputs are the paint's: iTexCoord0.xyz the world position, iTexCoord1.xyz the world normal (xChrome__VS_Chrome.hlsl:
// oTexCoord0.xyz = gWorld * the bone's position, oTexCoord1.xyz = the car's normal matrices applied to the vertex normal), and the camera's
// position is gViewInverse(3).xyz, which the shader itself subtracts from iTexCoord0 for the view vector. The constants are the paint's
// block read as g_CarReflM0..M3 (the matrix to the history frame's clip space, the vehicle's motion in it), g_CarReflA.x (0 the game's
// chrome: the host sends 0 where there is no history), g_CarReflA.w (strength), g_CarReflB.y (the debug view), g_CarReflC.x (history slot),
// .z and .w (the ray traced image, above). Debug views (mcla_gpu_car_selfrefl_view: they replace the colour of the chrome AND of the paint):
// 1 the reflection probe's weight, 2 the sky probe's, 3 the baked vertex AO, 4 the state (red: the point is not in the history, green: no
// hit, yellow: a hit, blue: the ray traced image answered).
//
// The setting and the switch: the variant is drawn wherever the paint's is (CarSelfRefl::Selected(): mcla_gpu_car_selfrefl, auto = on on
// Mid and High) unless the debug-only mcla_gpu_car_chrome_occlusion (default on) is off, for an A/B; pipelines.cpp WantedFeatures.
// Feature 7 (pack_format.h kFeatureCarChrome; id 9 in version 2 of the pack): its own feature so the switch can tell the chrome from the paint,
// the host reads the same constants block for both (scene.cpp UploadVariantConstants).
//
// CHECK: both edited texts compile with dxc (ps_6_0 DXIL and SPIR-V; version 3's compiled clean, dxc 1.9 and spirv-val of the Vulkan SDK 1.4.363); in a run: that the chrome's draws are not alpha tested or alpha to
// mask (else they keep the game's shader: Pipelines key.ps_variant 1), that the pixel shader's TEXCOORD0 and TEXCOORD1 are the world
// position and normal (the debug view 4 is all red if not), that the tips' vertex alpha is 1 (view 3), that the history's texel at a tip
// agrees with the shaded point (view 4 not red on the tubes: a tip is 6 to 25 pixels across), the 2x MSAA edge of a tube (the history holds
// sample 0), and how much of the rim a viewer sees stays bright (it faces the open back: that is the reflection of the street).
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line variants_car_chrome_version).
inline constexpr uint32_t kCarChromeVariantVersion = 3;  // 1: reflection and sky probes of centimetre scale, the ray traced image's hook;
                                                          // 2: another car's chrome from the ray traced image (both probes: the sky's weight in the red);
                                                          // 3: the self-test tries the texels round the shaded pixel's own (the rim of a tip)

// The identity on the retail game (the runtime's own: FNV-1a 64 of the microcode) and the name the translator gives it.
inline constexpr VariantShader kChrome = {"xChrome__PS_Chrome", 0x2D8CA0430FF88ED7ull};

VariantDef MakeCarChromeVariant();

// The text transform alone (pure; public for tests).
VariantTransformResult TransformCarChromeHlsl(const VariantShader& shader, std::string_view hlsl,
                                              const StageContext& ctx = SoloContext(kFeatureCarChrome));

}  // namespace larecomp::shaderpack

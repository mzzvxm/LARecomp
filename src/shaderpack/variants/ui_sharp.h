#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "ui_sharp": the UI's textured pixel shaders, sharper where the console's bitmaps are magnified
// ===========================================================================
// The HUD and the menus are Flash movies the game draws with RAGE's immediate mode: inline vertices through the xrage_im shaders, a bitmap
// (a button, an icon, a panel, a glyph) in one 2D fetch, the result times the vertex colour (src/gpu/ui_sharp.h, perf/UI-SHARP.md). The 3D
// pieces of the UI (the in-world icons, the arrow, the Sidekick phone, the pause card) are drawn with the "ui" category's shaders (xUnlit*,
// xGloss*: a diffuse bitmap, sometimes with the scene's depth beside it).
// The bitmaps are made for 1280x720. At a render scale of 1.5 (1080p) or 3 (4K) the game's bilinear fetch spreads each texel's edge over
// 1.5 or 3 pixels. This variant is the game's own translated text with every
//
//     tfetch2D(<bitmap>, ...)    (the call of the 2D fetch with an implicit level; the definition in shader_common.h is left alone)
//
// whose texture is the shader's colour bitmap (a sampler named TextureSampler, DiffuseSampler or TransparentSrcMapSampler: not the depth, the
// environment map, the bump, specular or gloss maps) replaced by a call of a helper, tfetch2DUiSharp(<slot>u, ...), with the same arguments and the
// texture's fetch slot in front. The helper first makes the game's own fetch, then asks the draw (g_UiSharp.w: a bit for every fetch slot whose texture
// the Scene found is a linearly filtered colour bitmap) whether this slot is for it, works out how many pixels a texel covers (the screen-space
// derivatives of the texel coordinate; coarse, so the whole quad agrees) and below kMinMagnification (the constant's z, 1.25) returns the
// game's own result: so a draw that does not magnify, the pause view's 1:1 copy of the scene, and a render scale of 1 are the game's, to the last bit
// of the fetch. Above it:
//
//   colour  five bilinear taps make a Catmull-Rom reconstruction (the interpolating bicubic: it keeps the edges the B-spline of
//           tfetch2DBicubic would blur), clamped to 0..1 and mixed with the game's fetch by the constant's x (1 = all Catmull-Rom).
//   alpha   GatherAlpha reads the four texels around the sample. Where their alpha differs by more than about 0.3 (smoothstep 0.30..0.55:
//           an edge of a glyph or an icon, not a fade or a glow) the alpha is stretched to the local range [lo, hi], pushed away from its
//           middle by a gain of max(1, magnification x the constant's y), and put back into that range: a ramp that was a texel wide and
//           is magnified m times is about a pixel wide again, and lo and hi, so a thin stroke's peak, are kept.
//
// Two ids (VariantDef::alpha_test_id): kVariantUiSharp (10) is the text compiled for spec variant 0 (no alpha test) and kVariantUiSharpAlphaTest (11)
// the same text compiled with the shader's own spec mask (the alpha test and alpha to coverage, SV_Coverage), for the UI draws that have one on:
// the round 1 run never took the variant for any draw, and a UI draw with the alpha test on (key spec variant 1) was the first suspect.
//
// The edits (anchors that must match, or the variant is not built for the shader):
//   1. the SPIR-V declaration of g_InvResolutionScale gets `g_UiSharp` (float4 at byte 608: x colour mix, y edge gain, z minimum
//      magnification, w the bitmap slots) after it; 2. so does its DXIL twin (packoffset(c38)); the same two edits as the HDR scene's variant
//      makes for g_HdrScene;
//   3. each call of tfetch2D( on a colour bitmap (the definition not counted: exactly one `float4 tfetch2D(uint resourceDescriptorIndex`) ->
//      tfetch2DUiSharp(<slot>u, ; the slot is read from the shader's own `#define <sampler>_Texture2DDescriptorIndex ... SharedConstants + 4 x slot`;
//   4. the helper, before `void main(`.
// A shader with no such call is skipped (no error): the draws of it have nothing to sharpen.
//
// WHICH SHADERS (round 2, from the census of perf/out/UISHARP.txt, every pixel shader the UI phase drew; the runtime identities were mapped to the
// game's names with the pack builder's own key, FNV-1a 64 of the normalised microcode):
//   5CA2EECD341441FF xrage_im__PS_Textured                          the Flash player's bitmaps and text (188 K reads in 12 windows); identity verified earlier
//   805FD1A239E5E5D2 xUnlitRuntimeAlpha__PS_TexturedAlternateDepth   the in-world icons (gas stations...: 128x128 DXT5 with mips) and plain quads (1.06 M reads),
//                                                                     with the scene's depth (point filtered, 1280x720) as a second fetch that is not touched
//   9EDF073903432E07 xUnlitCustomDepth__PS_TexturedCustomDepth       the waypoint arrow (icon_arrow) and a 256x256 8888 picture, with depth
//   4ACC11B04045702F xUnlitRuntimeAlpha__PS_Textured                 one bitmap
//   7B4C406C7606CB64 xUnlitRuntimeColor__PS_Textured                 one bitmap
//   7730BAD21E9EF4D5 xGloss__PS_TexturedGloss                        3D UI meshes (the pause card): the diffuse is sharpened, the environment map
//                                                                     (k_16_16_16_16_EXPAND) is not
//   C4EDA9C4D07C9F58 xGlossRuntimeAlpha__PS_TexturedGloss            the Sidekick phone (sidekick_tex, sidekick_glossmap, the contact photo and the
//                                                                     T-Mobile logo): the diffuse is sharpened, bump, specular, gloss and the environment are not
// plus xrage_im's PS, PS_Blit and PS_BlitTransparent (the census never saw them; they are the same family and cost nothing when not drawn). They
// are taken by NAME (a name the game does not have is not an error); the retail identities are in kUiSharpRetail below, for the tests and the logs.
//
// CHECK: the census (perf/out/UISHARP.txt) saw only these seven in the tour's screens (free roam, the phone, the pause menu, SETTINGS, the map). The garage,
// the race HUD, the photo mode and the shops may draw others of the ui category (xLit*, xMetal*, xRadialGradient, xArrow, xRearView, xAlphaModulate...): the
// census of those screens names them and they go into kUiSharpSuffixes, with a new kUiSharpVariantVersion.
//
// Runtime: src/gpu/scene.cpp (UiSharpDraw) chooses it per draw (ps_variant kVariantUiSharp = 10, or 11 where the key was spec variant 1) for draws in the
// UI phase while mcla_ui_sharp is above 0; nothing else does.
//
// In the pack step's log: "ui_sharp (N shaders ...)" in the variant pack summary line, and in its stamp variants_ui_sharp_version=2.
// ===========================================================================

#include <cstdint>
#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line variants_ui_sharp_version). 2: the six "ui" category shaders of the
// census, the per-slot helper and the alpha test twin (id 11).
inline constexpr uint32_t kUiSharpVariantVersion = 2;

// Verified on the retail game (the runtime's identity, FNV-1a 64 of the microcode): the pause menu's panel, the Flash player's bitmaps and text.
inline constexpr VariantShader kUiSharpTextured = {"xrage_im__PS_Textured", 0x5CA2EECD341441FFull};
// By name only: every pixel shader of the game whose name ends with one of these (each is a whole name, so each matches the one shader).
inline constexpr std::string_view kUiSharpSuffixes[] = {
    "xrage_im__PS",
    "xrage_im__PS_Blit",
    "xrage_im__PS_BlitTransparent",
    "xUnlitRuntimeAlpha__PS_TexturedAlternateDepth",
    "xUnlitRuntimeAlpha__PS_Textured",
    "xUnlitCustomDepth__PS_TexturedCustomDepth",
    "xUnlitRuntimeColor__PS_Textured",
    "xGloss__PS_TexturedGloss",
    "xGlossRuntimeAlpha__PS_TexturedGloss",
};
// The retail game's runtime identities of the six "ui" category shaders, with their names (for the tests and the logs; the build finds them by name).
inline constexpr VariantShader kUiSharpRetail[] = {
    {"xUnlitRuntimeAlpha__PS_TexturedAlternateDepth", 0x805FD1A239E5E5D2ull},
    {"xUnlitRuntimeAlpha__PS_Textured", 0x4ACC11B04045702Full},
    {"xUnlitCustomDepth__PS_TexturedCustomDepth", 0x9EDF073903432E07ull},
    {"xUnlitRuntimeColor__PS_Textured", 0x7B4C406C7606CB64ull},
    {"xGloss__PS_TexturedGloss", 0x7730BAD21E9EF4D5ull},
    {"xGlossRuntimeAlpha__PS_TexturedGloss", 0xC4EDA9C4D07C9F58ull},
};
// The samplers whose fetch is a colour bitmap the variant sharpens: `<name>_Texture2DDescriptorIndex` is the first argument of its call. The
// others a shader of the family has (DepthSampler, dpFrontSampler: the environment map, BumpSampler, SpecSampler, GlossSampler) stay the game's.
inline constexpr std::string_view kUiSharpBitmapSamplers[] = {"TextureSampler", "DiffuseSampler", "TransparentSrcMapSampler"};

VariantDef MakeUiSharpVariant();

// The text transform alone (pure; public for tests). `shader` only names the shader in the messages.
VariantTransformResult TransformUiSharpHlsl(const VariantShader& shader, std::string_view hlsl);

}  // namespace larecomp::shaderpack

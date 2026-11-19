#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "ui_sharp": the UI's textured pixel shaders, sharper where the console's bitmaps are magnified
// ===========================================================================
// The HUD and the menus are Flash movies the game draws with RAGE's immediate mode: inline vertices through the xrage_im shaders, a bitmap
// (a button, an icon, a panel, a glyph) in one 2D fetch, the result times the vertex colour (src/gpu/ui_sharp.h, perf/UI-SHARP.md).
// The bitmaps are made for 1280x720. At a render scale of 1.5 (1080p) or 3 (4K) the game's bilinear fetch spreads each texel's edge over
// 1.5 or 3 pixels. This variant is the game's own translated text with every
//
//     tfetch2D(...)        (the call of the 2D fetch with an implicit level; the definition in shader_common.h is left alone)
//
// replaced by a call of a helper, tfetch2DUiSharp, with the same arguments, that works out how many pixels a texel covers (the screen-space
// derivatives of the texel coordinate; coarse, so the whole quad agrees) and below kMinMagnification (the constant's z, 1.25) returns the
// game's own tfetch2D result: so a draw that does not magnify, the pause view's 1:1 copy of the scene, and a render scale of 1 are the
// game's, to the last bit of the fetch. Above it:
//
//   colour  five bilinear taps make a Catmull-Rom reconstruction (the interpolating bicubic: it keeps the edges the B-spline of
//           tfetch2DBicubic would blur), clamped to 0..1 and mixed with the game's fetch by the constant's x (1 = all Catmull-Rom).
//   alpha   GatherAlpha reads the four texels around the sample. Where their alpha differs by more than about 0.3 (smoothstep 0.30..0.55:
//           an edge of a glyph or an icon, not a fade or a glow) the alpha is stretched to the local range [lo, hi], pushed away from its
//           middle by a gain of max(1, magnification x the constant's y), and put back into that range: a ramp that was a texel wide and
//           is magnified m times is about a pixel wide again, and lo and hi, so a thin stroke's peak, are kept.
//
// The edits (anchors that must match, or the variant is not built for the shader):
//   1. the SPIR-V declaration of g_InvResolutionScale gets `g_UiSharp` (float4 at byte 608: x colour mix, y edge gain, z minimum
//      magnification) after it; 2. so does its DXIL twin (packoffset(c38)); the same two edits as the HDR scene's variant makes for g_HdrScene;
//   3. each call of tfetch2D( (the definition not counted: exactly one `float4 tfetch2D(uint resourceDescriptorIndex`) -> tfetch2DUiSharp(;
//   4. the helper, before `void main(`.
// A shader with no call of tfetch2D( is skipped (no error): the draws of it have nothing to sharpen.
//
// WHICH SHADERS. xrage_im's PS_Textured is verified: 5CA2EECD341441FF is the pixel shader of the pause menu's panel and of the 1:1 scene
// copy its UI target starts with (perf/out/ASPECT.txt, G8A.txt log it by name). PS, PS_Blit and PS_BlitTransparent are the same family (one
// fetch times the vertex colour or not) and are taken by name; whether the Flash player's glyphs and shapes use them or other shaders is
// what mcla_ui_sharp_census prints on the first run (the PS identity of every texture read after the composite marker): the names of the
// shaders it finds go into kUiSharpSuffixes, by name, which is robust to a different game version.
//
// Runtime: src/gpu/scene.cpp (UiSharpDraw) chooses it per draw (ps_variant kVariantUiSharp = 10) for draws in the UI phase while
// mcla_ui_sharp is above 0; nothing else does.
//
// In the pack step's log: "ui_sharp (N shaders)" in the variant pack's summary line, and in its stamp variants_ui_sharp_version=1.
// ===========================================================================

#include <string_view>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the helper or the checks of this variant (its stamp line variants_ui_sharp_version).
inline constexpr uint32_t kUiSharpVariantVersion = 1;

// Verified on the retail game (the runtime's identity, FNV-1a 64 of the microcode): the pause menu's panel and picture copy.
inline constexpr VariantShader kUiSharpTextured = {"xrage_im__PS_Textured", 0x5CA2EECD341441FFull};
// By name only (their identities are not known yet): every pixel shader of the game whose name ends with one of these.
inline constexpr std::string_view kUiSharpSuffixes[] = {"xrage_im__PS", "xrage_im__PS_Blit", "xrage_im__PS_BlitTransparent"};

VariantDef MakeUiSharpVariant();

// The text transform alone (pure; public for tests). `shader` only names the shader in the messages.
VariantTransformResult TransformUiSharpHlsl(const VariantShader& shader, std::string_view hlsl);

}  // namespace larecomp::shaderpack

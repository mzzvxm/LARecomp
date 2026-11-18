// MCLA GPU renderer -- the sun's shadow cascades as a side view class: its identity, its setting and its
// samples. See side_views.h.
//
// What the game draws (GFX-UPGRADES-DESIGN.md 3.1, 6A.2 item 2, 6B.2 item 1; census of 2026-11-13): while the
// sun casts shadows (8 to 17 hours in clear weather) it draws the four cascades, about 1,300 draws a frame
// (89% depth only), each into
//   depth kD24S8 at EDRAM tile 0, pitch 640, 4 samples, no colour target, declared 640x640,
// and resolves them with ONE sample each (sample bits 1, host sample 0 of 4) into the quadrants of a
// 1280x1280 k_24_8 texture, the atlas (base 06E65000; the rect (0,0)-(640,640) to the points (0,0), (0,640),
// (640,0), (640,640): cascades 1 to 4). The shadow collector (phase 0x1000: the PS_ShadowBlend and
// PS_ShadowBlendAlpha of every material) looks the atlas up with UV from the light matrices c64-c79 and
// writes a screen sized mask; the lit shaders read only that mask.
//
// The class: the one depth identity above, at a scale of its own (the setting: 2x = four cascades of
// 1280x1280 in an atlas of 2560x2560, 26 MB; 3x = 1920x1920 and 3840x3840, 59 MB; R32F) with ONE host sample
// whenever the scale is above the guest's: the resolve reads one sample, so three quarters of the game's
// 4x multisampled raster and bandwidth are spent for nothing, and at 2x one sample of a texel a quarter of
// the size is as dense as the guest's four (the draw count does not change with the size: the cascade pass
// costs about what the game's does). At 1x it keeps the guest's 4 samples: `game` is the game's own
// picture, shader for shader and sample for sample.
//
// Everything else is GF1's (edram.cpp, resolve.cpp, textures.cpp): the viewport, scissor, clears and the
// four resolves' rectangles follow the target's scale (Scale::Edge), the resolve's destination offsets
// (multiples of 32) times the scale, the product is as big as its scaled source and is read at its own
// scale (Product::own_scale: no reduced copy, whatever the reader's scale).
//
// WHAT READS THE ATLAS, AND WHETHER IT SCALES (every translated shader of the pack that uses the light
// matrices or the atlas's sampler, 155 in all; no other shader reads it):
//  1. PS_ShadowBlend / PS_ShadowBlendAlpha, 147 of the 152 that read it (the shadow_filter variant, G1b):
//     four taps at (0.75, 1, 0.5, 0.25) x count / 1280 UV, every immediate texel offset float2(0, 0). UV is
//     normalised, so the game's own filter reads the finer atlas as it is: the same width in the world
//     (count x 1/1280 of the cascade), a smoother edge, nothing to change. The variant takes its centre and
//     radius from the game's own tap coordinates and multiplies the radius by the constant that
//     Scene::UploadShadowFilterConstants sets from this class's scale (shadow_filter.h AtlasRadiusScale,
//     1 / scale): the soft filter keeps its width in ATLAS TEXELS, so its edge is 1/2 (2x) and 1/3 (3x) as
//     wide in the world. Its Gather taps read the host texture's own size (getTexture2DDimensions). With
//     pcss (G1c) the blocker search covers twice that radius and the kernel's floor (0.75 atlas texels) and cap
//     (twice the radius) follow the same constant, so the dense sampling and the sharpest edge stay in texels;
//     between them the kernel is in WORLD units (the sun's disc at the blockers' distance, through the light
//     matrix of the cascade the game picked: a0), so a finer atlas makes the contact sharper and leaves a tall
//     caster's penumbra where it is.
//  2. The same shaders of the car paint, the skinned metal and the breaking glass (5 of the 152, left out
//     of the variant): four taps at immediate texel offsets of (+-0.5, +-0.5) and the weights of a bilinear
//     2x2 block, frac(uv * size + offset - 0.5). The translator's tfetch2D divides an immediate offset by the
//     HOST texture's size (GetDimensions) and getWeights2D multiplies the UV by it, so the block is the host's
//     texels at any size, the same rule as the bloom's immediate offsets (bloom_gauss.h) but here it is the
//     right reading: the offsets mean half a texel. The depth falloff after it (exp2 of the squared
//     difference times gBounds, with gDepthBias, PS c80 and c81) is in depth units. So these edges are also
//     1/2 and 1/3 as wide at high and ultra, and nothing in them was written for 1280.
//  3. PS_ShadowFastBlend of the LOD city (xCityLOD, xCityGroundLOD): two taps, immediate offsets 0, the
//     second tap displaced by a literal in world units (c255) before the light matrix: scale free, the game's
//     width on a finer map; not in the variant.
//  4. PS_ImposterBlend of the tree imposters' collector (xShadowImposterCollector): the four rotated taps of
//     item 1, immediate offsets 0; not in the variant (its name is not PS_ShadowBlend): scale free, the
//     game's width on a finer map. CHECK: with the soft filter the far trees and the LOD sectors keep the
//     game's width and the rest of the picture is narrower.
//  No texel size constant exists for the atlas: c62 fbSize is the screen's; c64-c87 (light matrices, planes,
//  boxes) map the world to UV of the whole atlas (a cascade is a quadrant of 0.5) and are scale free; the
//  1/1280 is a shader literal (item 1). The cascade pass's bias is its own: PA_SU_POLY_OFFSET (slope scaled,
//  so it follows the texel) plus a constant in depth units (and pipelines.cpp's shift where kD24S8 is kept
//  as D32F): CHECK for acne at ultra in the pictures.
//
// CHECK (nothing here was built or run; D3D12 and Vulkan, debug layer / validation on, K-DAY and K-H17 at
// game / high / ultra with the soft filter; the chain is perf\scripts\chain-g1a.ps1):
//  1. The log at the first cascade draw has "[gpu] edram: depth kD24S8@0 pitch 640 4x made at 2x (the sun
//     shadows side view, 1 host sample; declared 640x640 ...)" (3x at ultra) and no "was made at ... and is
//     now bound through a surface declared ..." line (Matches asks for 640x640 whenever the size is known).
//  2. The atlas product 06E65000 is 2560x2560 (3840x3840) and read by the collector draws "at their own
//     scale" (the texture cache summary), never "through a reduced copy"; the memory line grows by about
//     25 MiB (56 MiB) for the atlas and as much for the maps being drawn.
//  3. The census build's cascade viewport is host 1280x1280 (1920x1920), 1 sample, with the draws a frame
//     as before.
//  4. K-DAY and K-H17 with `game`: pixel-identical to main (the class changes nothing at 1x); with `high` and
//     `ultra` and the soft filter the car shadow's edge is about 1/2 and 1/3 as wide (gfx_measure.py edge),
//     lit surfaces differ from `game` by under 3 levels away from edges (no acne), a lamp post's shadow
//     touches its base (no peter-panning).
//  5. The seams: the quadrants meet at the host column and row 1280 (1920). The four resolves each fill the
//     half pixel gap at their SOURCE's origin (resolve.cpp push.fill), which is the same for all four: look
//     for a line on a shadow where the camera crosses from one cascade to the next.
//  6. A live change game -> ultra -> game: no validation message, the "side view ... is now made at" lines, a
//     few frames without cascade shadows at most while the pipelines of the cascade draws at 1 sample are
//     built the first time (new pipeline keys, as the road reflection's: pending draws in the statistics),
//     and nothing in the picture after the swap but the edge.
//  7. The pause menu and the GPS map draw the same identity (6B.2 item 6): the scale applies there too.
//  8. The foliage cascades (PS 24df5d90eb724450 and four more: 205 of 1,310 draws) test alpha to mask: at 1
//     host sample the pass takes the guest's dither offsets at one sample (constants.cpp
//     AlphaToMaskParameters) where the game's sample 0 of 4 took the 4x pattern, so a tree's shadow can have
//     a little different density: compare a palm tree's shadow at game and at high.

#include <cstdint>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/xenos/format.h>

#include "edram.h"
#include "live_text.h"
#include "side_views.h"

namespace mcla::gpu {

namespace {

namespace xe = rex::graphics::xenos;

constexpr uint16_t kPitch = 640;
constexpr uint8_t kSamples = 4;
constexpr uint16_t kDepthBase = 0;
constexpr uint32_t kDeclared = 640;  // the surface's declared width and height

bool Matches(const EdramKey& key, uint32_t size_bits) {
  // The depth identity of the four cascades and nothing else: the main view's depth and the other side
  // views' have other pitches or formats, and no colour target takes part.
  if (!key.depth || key.pitch != kPitch || key.samples != kSamples || key.reserved != 0 ||
      key.base_tiles != kDepthBase ||
      key.format != uint8_t(xe::DepthRenderTargetFormat::kD24S8)) {
    return false;
  }
  // The declared size, where it is known: any other size at this key is not this view.
  return size_bits == 0 || side_views::DeclaredSize(size_bits, kDeclared, kDeclared);
}

std::string Setting() {
  static LiveText text("mcla_gpu_shadow_resolution");  // the render thread does not read the cvar's string (live_text.h)
  return text.Get();
}

uint32_t HostSamples(Scale scale, uint32_t guest_samples) {
  // At the guest's size (or below it) the game's own 4; above it one, the only sample the resolve
  // reads (see the top).
  return scale.unit() || scale < Scale{} ? guest_samples : 1u;
}

}  // namespace

const SideViewRule kShadowRule = {
    .view = SideView::kShadow,
    .name = "sun shadows",
    .matches = Matches,
    .setting = Setting,
    // `auto` is game on Low, high on Mid and High (gpu_class.h, GFX-UPGRADES-DESIGN.md 9).
    .effect = gpu_class::Effect::kShadowResolution,
    .host_samples = HostSamples,
};

}  // namespace mcla::gpu

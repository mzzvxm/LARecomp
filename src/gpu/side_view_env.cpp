// MCLA GPU renderer -- the environment map as a side view class: its identities, its setting and its samples.
// See side_views.h.
//
// What the game draws (GFX-UPGRADES-DESIGN.md 1.1, 4.2 2a, 6A.2 item 6 and 6B.2 item 2, the census of
// 2026-11-13): the car paint, the windows and the bonnet reflect a 512x256 dual paraboloid view of the world
// that the game draws every frame (about 240 draws, in two halves) into
//   colour k_2_10_10_10_FLOAT at EDRAM tile 416, pitch 520, 4 samples, and
//   depth kD24FS8 at tile 0, pitch 520, 4 samples,
// both bound through a surface declared 512x256, and resolves into a 512x256 k_16_16_16_16_EXPAND texture
// (base 032E2000; the mip levels at 033E2000). The levels 1 to 3 are made by three more draws into THE SAME
// target and the same identities (VS 54039B323F03FC58, PS 4AA33D052D8AAD13, the game's PSFastMipMap: an
// average of 4 taps), each resolved into the next level (rects 256x128, 128x64 and 64x32): so a class chosen
// by the EDRAM key already holds them and the product's four levels are one scale; no second identity. A second
// resolve of the whole 512x256 rect into level 0 follows the first (the left half's draws, then the right
// half's). About 100 car shader draws a frame read the 4-level map in slot 0, trilinear.
//
// The 64x32 specular copy that the cars read as well (product 032DE000, made by three draws of PS_Blit and
// the two blurs on a separate 64x32 4-sample target, colour k_2_10_10_10_FLOAT at tile 0 or 8, pitch 80) is
// not in the class: its target stays the game's size, and what it reads, the environment map's base level,
// is read at the map's own scale like any product of a side view. Its first draw takes ONE bilinear tap of
// the map per pixel (an 8:1 reduction), which at 2x and 3x would read 1.6% and 0.7% of the block it stands for
// instead of 6%, and the level pass's taps are a pixel shader constant in the game's texel size, so two
// variants of the game's own shaders (shaderpack/variants/env_map.h, the variant pack, kEnvVariant) put those
// taps back: the level pass on the 2x2 texels of a pixel of the next level, the reduction as an area average
// of the same footprint. pipelines.cpp (ChoosePackedVariant) draws them while this setting is above `game`.
//
// The class: both identities, at a scale of its own (the setting: 2x = 1,024x512, 3x = 1,536x768) with as
// many host samples as keep the density of the game's four per guest pixel (HostSamples: one from 2x on, the
// resolve averages a colour source's samples). At 1x it keeps the guest's four and the game's shaders: `game`
// is the game's own picture.
//
// CHECK (nothing here was built or run; D3D12 and Vulkan, debug layer / validation on, K-DAY with the
// camera on the car and K-DRIVE):
//  1. The log at the first env draw has the two "[gpu] edram: ... made at 2x (the car reflections side
//     view, 1 host sample; declared 512x256 ...)" lines, for colour k_2_10_10_10_FLOAT@416 pitch 520 2x and
//     depth kD24FS8@0 pitch 520 2x, and no "was made at ... and is now bound through a surface declared ..."
//     line for either (a second declared size at these keys would show there and keep the view at the guest's
//     size: Matches asks for 512x256 whenever the size is known).
//  2. The census build's env view: host viewport 8192x8192 (unclipped, the scissor clips it: viewport.cpp
//     divides the cap by the scale, so 8190 at 3x), scissor (0,0)-(1024,512) at 2x; the product 032E2000 is
//     1,024x512 with 4 levels (512x256, 256x128, 128x64) and read by the car shaders "at its own scale"
//     (the texture cache summary), never "through a reduced copy"; no "null texture is bound" line for it
//     (a product with more than one level had no texture at all at another scale than its reader's before
//     GF1: the black reflection).
//  3. The three level draws and the reduction use the env_map variants (the pack stamp's
//     variants_env_map_shaders has 4aa33d052d8aad13 and f92316724cfa62ed); the levels of the 2x map look like
//     the game's (not twice as blurry) and the 3x map's levels do not shimmer; the specular copy does not
//     sparkle while the sun or a lamp crosses the map (K-DRIVE).
//  4. K-DAY with `game` against main: pixel-identical (the class and the variants change nothing at 1x);
//     with `high` and `ultra` a building edge in the car's rear-window reflection is about 1/2 and 1/3 as
//     wide (gfx_measure.py edge).
//  5. A live change game -> ultra -> game: no validation message, "the car reflections side view ... is now
//     made at" lines, a few frames of a reflection from the game's map at most while the pipelines of the
//     1 sample draws (new keys, as the road reflection's) and of the variants are built the first time, the
//     memory line grows by about 4 MiB (high) and 16 MiB (ultra).
//  6. The pause menu's and the GPS map's world views draw the same identities (6B.2 item 6): the same scale
//     applies, and the pause menu's car view (`tour_01_pause` of the race tour) shows the car with its
//     reflection.

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>

#include "edram.h"
#include "side_views.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_env_resolution, "auto", "MCLA/GPU",
    "How sharp the reflections in the paint, windows and bonnet of the cars are: the game draws the "
    "world around the car as a 512x256 picture for them to reflect, which is why the reflections look "
    "soft up close. game (the game's own 512x256), high (1024x512), ultra (1536x768), or auto (game on "
    "an integrated graphics chip or a card with less than about 6 GB of memory, high on any other card). "
    "Costs a little graphics card time and video memory (about 4 MiB for high, 16 MiB for ultra). Live.")
    .allowed({"auto", "game", "high", "ultra"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

namespace xe = rex::graphics::xenos;

constexpr uint16_t kPitch = 520;
constexpr uint8_t kSamples = 4;
constexpr uint16_t kColorBase = 416;
constexpr uint16_t kDepthBase = 0;
constexpr uint32_t kDeclaredWidth = 512;  // the surface's declared size
constexpr uint32_t kDeclaredHeight = 256;

bool Matches(const EdramKey& key, uint32_t size_bits) {
  if (key.pitch != kPitch || key.samples != kSamples || key.reserved != 0) {
    return false;
  }
  // The declared size, where it is known: any other size at these keys is not this view.
  if (size_bits != 0 && !side_views::DeclaredSize(size_bits, kDeclaredWidth, kDeclaredHeight)) {
    return false;
  }
  if (key.depth) {
    return key.base_tiles == kDepthBase &&
           key.format == uint8_t(xe::DepthRenderTargetFormat::kD24FS8);
  }
  return key.base_tiles == kColorBase &&
         key.format == uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
}

std::string Setting() {
  return REXCVAR_GET(mcla_gpu_env_resolution);
}

uint32_t HostSamples(Scale scale, uint32_t guest_samples) {
  // At the guest's size (or below it) the game's own samples. Above it as many as keep the density of the
  // game's samples per guest pixel, ceil(guest / s^2), up to a count the host has: at 2x and 3x one (a
  // pixel there is a quarter or a ninth of the guest's, so one sample is as dense as the guest's four at
  // 2x, denser at 3x), at 1.5x two.
  if (scale.unit() || scale < Scale{}) {
    return guest_samples;
  }
  const uint64_t n = scale.n;
  const uint64_t d = Scale::kDenominator;
  const uint64_t wanted = (uint64_t(guest_samples) * d * d + n * n - 1) / (n * n);
  return wanted <= 1 ? 1u : wanted <= 2 ? 2u : guest_samples;
}

}  // namespace

const SideViewRule kEnvRule = {
    .view = SideView::kEnv,
    .name = "car reflections",
    .matches = Matches,
    .setting = Setting,
    // `auto` is game on Low and Mid, high on High (gpu_class.h, GFX-UPGRADES-DESIGN.md 9).
    .effect = gpu_class::Effect::kEnvResolution,
    .host_samples = HostSamples,
};

}  // namespace mcla::gpu

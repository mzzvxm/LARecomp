// MCLA GPU renderer -- the environment map as a side view class: its identities, its setting and its samples.
// See side_views.h.
//
// What the game draws (GFX-UPGRADES-DESIGN.md 1.1, 4.2 2a, 6A.2 item 6 and 6B.2 item 2, the census of
// 2026-11-13): the car paint, the windows and the bonnet reflect a 512x256 dual paraboloid view of the world
// that the game normally draws every frame (about 240 draws, in two halves) into
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
// of the same footprint. pipelines.cpp (WantedFeatures) draws them when the 2x mode is on.
//
// Both identities use one sample at 2x (Car Reflections On) or the guest's
// four samples at 1x (Off). The hold cadence is independent of the scale.
//
// The depth is not the class's alone: the night shadow's pass (dusk and night, colour k_8_8_8_8 at tile 832, pitch
// 520, 4 samples, declared 512x512, resolved into a 512x512 k_8 texture) draws with the very same depth identity.
// Matches asks for 512x256 whenever the size is known, so that pass's use is a FOREIGN USE (side_views.h, ONE
// IDENTITY, TWO USES): EdramTargets::DepthLayer gives it layer 2, a host depth of its own at the guest's size, and
// its colour and depth are one scale (before, the depth was 2x and the colour 1x: every draw of the pass was left
// out, "targets of different resolution scales"). Whatever Matches accepts here must stay what DepthLayer asks.
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
//  4. With hold disabled, Off uses the stock picture and On halves the width of a building
//     edge in the rear-window reflection (gfx_measure.py edge).
//  5. A live change Off -> On -> Off: no validation message, "the car reflections side view ... is now
//     made at" lines, a few frames of a reflection from the game's map at most while the pipelines of the
//     1 sample draws (new keys, as the road reflection's) and of the variants are built the first time, the
//     memory line grows by about 4 MiB at 2x.
//  6. The pause menu's and the GPS map's world views draw the same identities (6B.2 item 6): the same scale
//     applies, and the pause menu's car view (`tour_01_pause` of the race tour) shows the car with its
//     reflection.

#include <algorithm>
#include <cstdint>
#include <string>

#include "settings/immortal.h"
#include "settings/larecomp_settings.h"
#include <rex/xenos/format.h>

#include "edram.h"
#include "env_fallback_rules.h"
#include "live_text.h"
#include "resolve.h"
#include "side_views.h"

namespace mcla::gpu {

namespace {

namespace xe = rex::xenos;

constexpr uint16_t kPitch = 520;
constexpr uint8_t kSamples = 4;
constexpr uint16_t kColorBase = 416;
constexpr uint16_t kDepthBase = 0;
constexpr uint32_t kDeclaredWidth = 512;  // the surface's declared size
constexpr uint32_t kDeclaredHeight = 256;
constexpr uint32_t kMapBase = 0x032E2000;
constexpr uint32_t kMapMips = 0x033E2000;

class EnvFallbackSettings final : public rex::SettingOwner {
 public:
  EnvFallbackSettings() : SettingOwner("larecomp") { larecomp::settings::RegisterExtraOwner(*this); }

  REX_SETTING(bool, mcla_gpu_env_fallback, true,
              "Use the environment map where screen reflections have no usable result. Turn off for comparison. Live.",
              rex::kDebugOnly);
};

EnvFallbackSettings& g_env_fallback_settings = larecomp::Leak<EnvFallbackSettings>();

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
  static LiveText text("mcla_gpu_env_resolution");  // the render thread does not read the cvar's string (live_text.h)
  return text.Get();
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
    .host_samples = HostSamples,
};

bool env_fallback::Enabled() { return g_env_fallback_settings.mcla_gpu_env_fallback.Get(); }

uint32_t env_fallback::MapWord(ResolveCache& resolves, const EdramTargets& edram, Device& device,
                              BindlessHeap& heap, uint64_t written) {
  if (!written) return 0;
  // Recheck after the marker in case the guest since aliased or rewrote the
  // texture. A read can update Product::last_frame without changing its mips.
  // CHECK: verify this binding and probe alignment on fresh and held captures,
  // during cuts, live scale changes and menu transitions, on both APIs.
  resolves.Sync();
  const ResolveCache::Product* found = resolves.FindOverlap(kMapBase, 1);
  if (!found || !found->active || !found->texture || !found->own_scale() || found->depth_source ||
      found->layout != plume::RenderTextureLayout::SHADER_READ ||
      found->format != plume::RenderFormat::R16G16B16A16_FLOAT || found->scale != edram.side_scales()[SideView::kEnv] ||
      found->key.base_page != (kMapBase >> 12) ||
      found->key.mip_page != (kMapMips >> 12) || found->key.width != kDeclaredWidth ||
      found->key.height != kDeclaredHeight || found->key.levels() != kLevels ||
      found->coverage.size() != kLevels ||
      !std::all_of(found->coverage.begin(), found->coverage.end(),
                   [written](const ResolveCache::Product::Coverage& level) {
                     return LevelMatches(written, level.last_write_frame,
                                         level.cells_x && level.cells_y && level.complete());
                   })) {
    return 0;
  }
  ResolveCache::Product* product = resolves.Find(found->key, found->key.base_page);
  if (!product || !product->texture) {
    return 0;
  }
  const uint32_t slot = product->views.Slot(device, heap, *product->texture, product->format,
                                            kLevels, format::kHostSwizzleRGBA, "env fallback map");
  return PackMapWord(slot, product->scale.Edge(kDeclaredHeight));
}

}  // namespace mcla::gpu

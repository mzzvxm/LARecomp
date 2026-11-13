// MCLA GPU renderer -- the road reflection side view: its identities, its setting and its samples.
// See side_views.h.
//
// What the game draws (GFX-UPGRADES-DESIGN.md 5.1 and 6A.2 item 4, census of 2026-11-13): the
// reflection view draws the LOD city again with the MAIN camera and the geometry mirrored about a
// plane by the vertex shader (gReflectionParams.z), 53 to 277 draws a frame, every frame, into
//   colour k_2_10_10_10_FLOAT (7e3) at EDRAM tile 128, pitch 320, 2 samples, and
//   depth kD24FS8 at tile 0, pitch 320, 2 samples,
// both bound through a surface declared 256x256; it is resolved into a 256x256 k_16_16_16_16_EXPAND
// texture of one level. Every reflective city material reads that one texture with its screen UV (plus
// a ripple): wet roads, windows and water, so one image serves all of them. It stretches 256x256 over
// the whole screen, which is why it is soft.
//
// The class: both identities, at a scale of its own (the setting: 2x = 512x512, 3x = 768x768) with ONE
// host sample whenever the scale is above the guest's (the resolve averages the samples of a colour
// source; at 2x or more a pixel is a quarter of the guest's, one sample there is denser than the guest's
// two at 1x). At 1x it keeps the guest's 2: `game` is the game's own picture.
//
// CHECK (nothing here was built or run; D3D12 and Vulkan, debug layer / validation on, K-WET and K-RAIN):
//  1. The log at the first reflection draw has the two "[gpu] edram: ... made at 2x (the road reflection
//     side view, 1 host sample; declared 256x256 ...)" lines, for colour k_2_10_10_10_FLOAT@128 pitch 320
//     2x and depth kD24FS8@0 pitch 320 2x, and no "was made at ... and is now bound through a surface
//     declared ..." line for either (a second declared size at these keys would show there and keep the
//     view at the guest's size: Matches asks for 256x256 whenever the size is known).
//  2. The census build's reflection view: host viewport 512x512 (768x768 at ultra), 1 sample; the draws
//     a frame as before (61 parked by day); the product 05AC3000 is 512x512 (768x768) and read directly
//     by the road, window and water shaders: the texture cache summary counts those reads "at their
//     own scale" and mcla_gpu_census_scale says "read by a draw at 1x directly" for it, never "through
//     a reduced copy".
//  3. K-WET and K-RAIN with `game` against main: pixel-identical (the class changes nothing at 1x); with
//     `high` and `ultra` the window-light streaks of the wet road are about 1/2 and 1/3 as wide.
//  4. A live change game -> ultra -> game: no validation message, "the road reflection side view ... is
//     now made at" lines, a few frames of an empty reflection at most while the 1 sample pipelines are
//     built the first time (below), the memory line grows by about 4 MiB (high) and 13 MiB (ultra).
//  5. The pipelines of the reflection's draws at 1 host sample are new keys: a first run, or a pipeline
//     cache and starter list recorded before this change, builds them on demand ("pending" draws in the
//     pipeline statistics). Pipelines::StartSampleCounts adds counts for the MSAA level only; a count
//     for the side views would have to name the reflection's records, not every multisampled one.
//  6. The pause menu's and the GPS map's world views draw the same identities: the same scale applies
//     (the reflection is read by their roads too).

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>

#include "edram.h"
#include "side_views.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_reflection_resolution, "auto", "MCLA/GPU",
    "How sharp the road reflection is: the wet roads, the shop and building windows and the water "
    "show a mirror image of the city that the game draws at 256x256 and stretches over the whole "
    "screen, which is why it looks soft. game (the game's own 256x256), high (512x512), ultra "
    "(768x768), or auto (game on an integrated graphics chip, high on any other card). Costs a "
    "little graphics card time and video memory (about 4 MiB for high, 13 MiB for ultra). Live.")
    .allowed({"auto", "game", "high", "ultra"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_reflection_scale, 0.0, "MCLA/GPU",
    "Diagnostic: the road reflection's size as a multiple of the game's 256x256 (any step of 0.05), "
    "instead of what mcla_gpu_reflection_resolution says. 0: the setting's. Live.")
    .debug_only()
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

namespace xe = rex::graphics::xenos;

constexpr uint16_t kPitch = 320;
constexpr uint8_t kSamples = 2;
constexpr uint16_t kColorBase = 128;
constexpr uint16_t kDepthBase = 0;
constexpr uint32_t kDeclared = 256;  // the surface's declared width and height

bool Matches(const EdramKey& key, uint32_t size_bits) {
  if (key.pitch != kPitch || key.samples != kSamples || key.reserved != 0) {
    return false;
  }
  // The declared size, where it is known: any other size at these keys is not this view.
  if (size_bits != 0 && !side_views::DeclaredSize(size_bits, kDeclared, kDeclared)) {
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
  return REXCVAR_GET(mcla_gpu_reflection_resolution);
}

// A step of the grid of twentieths, from 0.05 up (a test: 1.5x is 30).
Scale TestScale() {
  const double value = REXCVAR_GET(mcla_gpu_reflection_scale);
  if (!(value >= 0.05)) {
    return Scale::None();
  }
  return Scale{uint32_t(value * double(Scale::kDenominator) + 0.5)};
}

uint32_t HostSamples(Scale scale, uint32_t guest_samples) {
  // At the guest's size the game's own samples; above it one (see the top).
  return scale.unit() || scale < Scale{} ? guest_samples : 1u;
}

}  // namespace

const SideViewRule kReflectionRule = {
    .view = SideView::kReflection,
    .name = "road reflection",
    .matches = Matches,
    .setting = Setting,
    // Low (integrated) game, Mid (under 6 GiB) high, High high: GFX-UPGRADES-DESIGN.md 9.
    .autos = {SideLevel::kGame, SideLevel::kHigh, SideLevel::kHigh},
    .test_scale = TestScale,
    .host_samples = HostSamples,
};

}  // namespace mcla::gpu

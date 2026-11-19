// MCLA GPU renderer -- the HUD's small render-to-texture views (the minimap, and the two views beside it) as a side view class that follows the
// render scale: its identities and its setting. See side_views.h.
//
// What the game draws (VERIFIED in the 3x logs, ports\run\p10-4k-hdr "edram: ... made at 1x (the guest's size; declared ...)" and perf\out\GFXCENSUS2.txt;
// INFERENCE for what each one is): every frame, in every play window, the game draws three small views into EDRAM targets of their own, with 4 guest samples
// each, and resolves them into textures that the HUD and the 3D UI draw as bitmaps:
//   colour k_8_8_8_8 at tile 224, pitch 160, 4 samples + depth kD24FS8 at tile 0, pitch 160, 4 samples, declared 128x128   (about 25 draws a frame)
//   colour k_8_8_8_8 at tile 224, pitch 280, 4 samples + depth kD24FS8 at tile 0, pitch 280, 4 samples, declared 256x256   (about 37 draws a frame)
//   colour k_8_8_8_8 at tile 168, pitch 240, 4 samples, no depth,                                       declared 220x220   (43 to 84 draws a frame)
// The 220x220 one is read by xrage_im PS_Textured after the composite marker as an unnamed 220x220 k_8_8_8_8 texture: the HUD's minimap (the radar disc the
// player sees at the corner of the screen: roads, blips, the player's arrow). The 256x256 one is read by xUnlitCustomDepth and xGloss shaders as an unnamed
// 256x256 k_8_8_8_8 texture: a Flash movie drawn into a texture for the 3D UI (the Sidekick phone's screen: the contact's photo, the message text).
// The 128x128 pair has no reader in the UI census; it is the third small view of the same family (CHECK: what reads its product).
//
// None of them is "an image of the screen" (their sizes are not the screen's halvings), so at every render scale they were made at the guest's size: a 220x220
// disc stretched over about 230 pixels at 720p and three times that at 4K, a phone screen of 256x256 texels over a quarter of the 4K picture. The soft minimap and
// phone text of the owner's report (perf\out\uisharp-sheets\4k.png) are those texels, not the HUD's own drawing, so no sharper filter of the UI's fetch can fix them
// (round 1's variant cannot): the views have to be drawn at more pixels (UI-SHARP.md, lever C).
//
// The class: all five identities, at the SCREEN's scale (the render scale the images of the screen are made at; never below the guest's size, the targets are
// small and a render scale below 1x is for the scene's cost), with the guest's samples (4: the same pipelines as at 1x, no new sample count to build), moved
// live by the debug setting mcla_ui_minimap_scale. At `off` (the default) the class is at 1x and changes nothing: the game's own targets, shader for shader.
// Everything else is the reflection class's machinery (side_views.h): the viewport, scissor, clears and the resolves' rectangles follow the target's scale, the
// products are read at their own scale (Product::own_scale), a class member that cannot be made for want of video memory steps down.
//
// WHAT READS THEM, AND WHAT COULD BREAK (read in the code, not run):
//  - The HUD's draw of the 220x220 product: PS_Textured with UV from the vertices (normalised): the finer product is read as it is, at any scale. The 256x256
//    product by the 3D UI shaders: UV too. No shader of the census reads these textures with a texel size constant (xrage_im PS_Textured, xGloss and xUnlit shaders:
//    one fetch each, the offset is float2(0, 0)).
//  - The views' own draws (the roads, the blips, the clipping quads) use the guest's viewport and scissor in guest pixels, which every target scale scales
//    (ComputeViewport(state, depth, scale), Scale::Edge); lines and points whose width is set in guest pixels (RB/PA registers) would come out thinner against the
//    map at a larger scale (CHECK: the minimap's road lines at 2.25x against 1x: sharper, not thinner or broken).
//  - The resolves: a resolve of a member makes a product at the member's scale; a product that a non-member also writes would be made again at each of the two scales
//    (ResolveCache::Activate, "a class has to take all of its sources"): the 128x128 target is a member of the class but the gamma 128x128 colour target
//    (k_8_8_8_8_GAMMA at tile 1601, pitch 160, 1 sample) that the census also lists is not, so if the game copies the 128x128 view into it (CHECK) that chain
//    stays at 1x and the gain is lost there; and a product the guest CPU reads back would be reduced to the guest's size (the unscaled copy of the resolve rules).
//  - The identities share EDRAM tiles with other passes (tile 224 is also the 8x8 target at pitch 80; tile 0 is the depth of several): the declared size tells the
//    uses apart (Matches asks for 128, 256 or 220 whenever it is known), and another pass's use of the depth identity is a foreign use with a target of its own.
//  - Cost: 2.25x makes 288x288, 576x576 and 495x495 targets at 4 samples (a few MiB each); the draws a frame are the game's.
//
// CHECK (nothing here was built or run; D3D12 and Vulkan, debug layer / validation on, the ui-sharp.tour minimap pair at 1080p and 4K):
//  1. The log has "[gpu] edram: ... made at 2.25x (the UI map views side view, 4 host samples; declared 220x220 ...)" lines for each identity while the setting is on, and
//     "[gpu] resolve: ... is a product of the UI map views side view: 495x495 host texels (2.25x the guest's) ... read at its own scale".
//  2. mcla_ui_minimap_scale off: pictures identical to a build without this class (the class is at 1x: nothing changes).
//  3. on, 1080p and 4K: the minimap's roads, blips and arrow and the phone's text are sharper in the same frame; no missing disc, no shifted or clipped radar, the
//     minimap's circle edge and the player's arrow unchanged in place.
//  4. A live off -> on -> off: no validation message, a few frames at most of a cleared view while the targets are remade ("the UI map views side view ... is now made at").
//  5. What reads the 128x128 pair (the census of the same run: the log's resolve line names its product and the draws that read it).

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>

#include "edram.h"
#include "side_views.h"

REXCVAR_DEFINE_BOOL(
    mcla_ui_minimap_scale, false, "MCLA/GPU",
    "Debug: draw the HUD's small views (the minimap's 220x220 picture, the phone screen's 256x256 and a 128x128 one, which the game always draws at those sizes and the "
    "HUD stretches) at the render scale, as the images of the screen are, so that the minimap and the phone's text come out as sharp as the rest of the picture at "
    "1080p and 4K. Off: the game's own sizes. Never below the game's size; costs a few MiB of video memory. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

namespace xe = rex::graphics::xenos;

constexpr uint8_t kSamples = 4;

// One identity of the class: the surface it is bound through is declared `declared` x `declared`.
struct Identity {
  bool depth;
  uint16_t base_tiles;
  uint16_t pitch;
  uint32_t declared;
};
constexpr Identity kIdentities[] = {
    {false, 224, 160, 128},  // colour k_8_8_8_8, with the depth below
    {true, 0, 160, 128},     // depth kD24FS8
    {false, 224, 280, 256},
    {true, 0, 280, 256},
    {false, 168, 240, 220},  // the minimap: colour only
};

bool Matches(const EdramKey& key, uint32_t size_bits) {
  if (key.samples != kSamples || key.reserved != 0) {
    return false;
  }
  for (const Identity& identity : kIdentities) {
    if (key.pitch != identity.pitch || key.base_tiles != identity.base_tiles || (key.depth != 0) != identity.depth) {
      continue;
    }
    // The declared size, where it is known: any other size at these keys is another pass's use of them.
    if (size_bits != 0 && !side_views::DeclaredSize(size_bits, identity.declared, identity.declared)) {
      return false;
    }
    return key.format == (identity.depth ? uint8_t(xe::DepthRenderTargetFormat::kD24FS8) : uint8_t(xe::ColorRenderTargetFormat::k_8_8_8_8));
  }
  return false;
}

std::string Setting() {
  return REXCVAR_GET(mcla_ui_minimap_scale) ? "on" : "off";
}

bool Enabled() {
  return REXCVAR_GET(mcla_ui_minimap_scale);
}

uint32_t HostSamples(Scale, uint32_t guest_samples) {
  // The guest's own samples at any scale: the pipelines of these draws exist at that count, and the resolve reads every one of them.
  return guest_samples;
}

}  // namespace

const SideViewRule kUiMapRule = {
    .view = SideView::kUiMap,
    .name = "UI map views",
    .matches = Matches,
    .setting = Setting,
    .effect = gpu_class::Effect::kReflectionResolution,  // unused: the class has no `auto` row (follows_output)
    .test_scale = nullptr,
    .host_samples = HostSamples,
    .follows_output = true,
    .enabled = Enabled,
};

}  // namespace mcla::gpu

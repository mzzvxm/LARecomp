// MCLA GPU renderer -- the HUD's and the menus' render-to-texture views (the minimap, the quick map, the phone's screen, and the page of the pause menu,
// the garage and the GPS map) as a side view class that follows the render scale: its identities and its setting. See side_views.h.
//
// What the game draws (VERIFIED in the 3x logs, ports\run\p10-4k-hdr "edram: ... made at 1x (the guest's size; declared ...)" and perf\out\GFXCENSUS2.txt;
// INFERENCE for what each one is): in play the game draws three small views into EDRAM targets of their own, with 4 guest samples each, and resolves them into
// textures that the HUD and the 3D UI draw as bitmaps:
//   colour k_8_8_8_8 at tile 224, pitch 160, 4 samples + depth kD24FS8 at tile 0, pitch 160, 4 samples, declared 128x128   (about 25 draws a frame)
//     and, AT NIGHT, a second colour of the same pass, k_8_8_8_8 at tile 288, pitch 160, 4 samples, declared 128x128 (the rthit4 night run's log: "colour k_8_8_8_8@288 pitch 160 4x made at 3x
//     (an image of the screen; declared 128x128, first use 128 rows)", right after @224 and the depth; no such target in a day run) -- the same draws bind it as their second render target
//   colour k_8_8_8_8 at tile 224, pitch 280, 4 samples + depth kD24FS8 at tile 0, pitch 280, 4 samples, declared 256x256   (about 37 draws a frame)
//   colour k_8_8_8_8 at tile 168, pitch 240, 4 samples, no depth,                                       declared 220x220   (43 to 84 draws a frame)
// The D-pad up quick map replaces the 220x220 minimap with a larger overlay:
//   colour k_8_8_8_8 at tile   0, pitch 760, 4 samples, no depth,                                       declared 749x430   (216 draws a frame)
// GFXCENSUS2 shows it under map-overlay and later roam/end tags. The 26 matching target creations in ports\run\*\larecomp.log all have this same
// colour format, sample count and declared size. Other passes use tile 0 for depth at other pitches (e.g. 1280 for the scene, 520 for environment maps).
// and in the menus that are drawn over a world (the pause menu and its pages, the race pause, the garage and the showroom, the GPS map: the tags pause-tab0, pause-rb0,
// pause-rb1 and map-full of perf\out\GFXCENSUS2.txt, never free roam; p10-garage and p10-menu list the target):
//   colour k_8_8_8_8 at tile 960, pitch 960, 2 samples + depth kD24FS8 at tile 0, pitch 960, 2 samples, declared 960x640   (57 to 103 draws a frame)
// The 220x220 one is read by xrage_im PS_Textured after the composite marker as an unnamed 220x220 k_8_8_8_8 texture: the HUD's minimap (the radar disc the
// player sees at the corner of the screen: roads, blips, the player's arrow). The 256x256 one is read by xUnlitCustomDepth and xGloss shaders as an unnamed
// 256x256 k_8_8_8_8 texture: a Flash movie drawn into a texture for the 3D UI (the Sidekick phone's screen: the contact's photo, the message text).
// The 128x128 pair has no reader in the UI census; it is the third small view of the same family (CHECK: what reads its product).
// The 960x640 one (round 3) is read by xGloss PS_TexturedGloss, xUnlitRuntimeAlpha PS_Textured and xUnlitRuntimeColor PS_Textured, after the composite marker, as an
// unnamed 960x640 k_8_8_8_8 texture (perf\out\UISHARP2.txt, the 1080p census: 3,310 to 4,520 reads each, only in the windows that had the pause menu, the garage or the map
// up): a Flash movie drawn into a texture that a 3D card shows, like the phone's screen. INFERENCE (about 80%): the page of the pause menu and the GPS map's panel. What
// says it is a view at the guest's size: no draw INTO it is ever in the UI census with a render scale above 1 (the target scales seen are the HUD's 2.25 only), which is
// what a target made at the guest's size would give (those draws are before the marker), and "pause x1.03" (UISHARP2.txt) says the sharper fetch of its bitmap does nothing.
//
// None of them is "an image of the screen" (their sizes are not the screen's halvings), so at every render scale they were made at the guest's size: a 220x220
// disc stretched over about 230 pixels at 720p and three times that at 4K, a phone screen of 256x256 texels over a quarter of the 4K picture, a 960x640 page over
// the card's 1000 or more pixels at 1080p and three times that at 4K. The soft minimap, phone and menu text of the owner's report (perf\out\uisharp-sheets\4k.png) are
// those texels, not the UI's own drawing, so no sharper filter of the UI's fetch can fix them (round 1's variant cannot): the views have to be drawn at more pixels
// (UI-SHARP.md, lever C). The Flash draws INTO a member are themselves draws of the bitmaps the UI sharpening is for, at the member's scale: Scene::UiSharpContext
// calls them UI draws too (ui_sharp.h Context::kMovie).
//
// The class: the identities in side_view_ui_map.h, at the SCREEN's scale (the render scale the images of the screen are made at) when that is at least ui_sharp's kMinMagnification
// (1.25x of the console's picture: below it the class is at the guest's size, so 720p at a render scale of 1 is the game's own, target for target; side_views.h
// OutputScale), with the guest's samples (4 or 2: the same pipelines as at 1x, no new sample count to build), moved live by mcla_ui_minimap_scale. Off the class is at 1x
// and changes nothing: the game's own targets, shader for shader. Everything else is the reflection class's machinery (side_views.h): the viewport, scissor, clears and
// the resolves' rectangles follow the target's scale, the products are read at their own scale (Product::own_scale), a class member that cannot be made for want of video
// memory steps down.
//
// WHAT READS THEM, AND WHAT COULD BREAK (read in the code, not run):
//  - The HUD's draw of the 220x220 product: PS_Textured with UV from the vertices (normalised): the finer product is read as it is, at any scale. The 256x256
//    product by the 3D UI shaders: UV too. No shader of the census reads these textures with a texel size constant (xrage_im PS_Textured, xGloss and xUnlit shaders:
//    one fetch each, the offset is float2(0, 0)). The 960x640 product is read by the same three shaders (xGloss, xUnlitRuntimeAlpha, xUnlitRuntimeColor: one bitmap fetch each).
//  - The quick map is read after the marker by the same xrage_im PS_Textured (5CA2EECD341441FF, ports\run\r5c-high-ui-d3d12\larecomp.log):
//    one normalized UV fetch, so it can use the product at its own scale. The logs show no lock write-back for the 749x430 texture at 06acd000.
//    CHECK: confirm the scaled resolve survives the shared guest address and check for guest readback while the quick map is open. The guest-memory encode uses guest texels.
//  - The views' own draws (the roads, the blips, the clipping quads, the page's text and panels) use the guest's viewport and scissor in guest pixels, which every target
//    scale scales (ComputeViewport(state, depth, scale), Scale::Edge); lines and points whose width is set in guest pixels (RB/PA registers) would come out thinner against the
//    map at a larger scale (CHECK: the minimap's road lines at 2.25x against 1x: sharper, not thinner or broken).
//  - The resolves: a resolve of a member makes a product at the member's scale; a product that a non-member also writes would be made again at each of the two scales
//    (ResolveCache::Activate, "a class has to take all of its sources"): the 128x128 target is a member of the class but the gamma 128x128 colour target
//    (k_8_8_8_8_GAMMA at tile 1601, pitch 160, 1 sample) that the census also lists is not, so if the game copies the 128x128 view into it (CHECK) that chain
//    stays at 1x and the gain is lost there; and a product the guest CPU reads back would be reduced to the guest's size (the unscaled copy of the resolve rules).
//  - ONE PASS, ONE CLASS (the second colour of the 128x128 pass, rthit4 night): a draw's targets must all be at one scale, and a target that is no class's is made at the draw's scale the first time
//    (Get: the scale a use asks for, here the class's 3x) but is moved by the next live change of any scale (Retarget) to what its declared size classifies at (EdramTargets::ScaleAfter: a 128x128 surface is no
//    image of the screen, so 1x). The pass's colour at tile 288 was left out of this class, so after the first live change of a side view (the tour's ULTRA -> game at 8 s) its two companions were at 3x and it
//    at 1x: "targets of different resolution scales", all 25 draws a frame of the pass left out for the rest of the run (15,000 skipped draws every 600 frames in the log; CHECK: what the view
//    shows on screen). On main too: any live change of the shadows', the reflections' or the road reflection's resolution at 1.25x and over, at night. Every identity that the pass binds is in the table.
//  - The identities share EDRAM tiles with other passes (tile 224 is also the 8x8 target at pitch 80; tile 0 is the depth of several): the declared size tells the
//    uses apart (Matches checks the declared size when known), and another pass's use of the depth identity is a foreign use with a target of its own.
//  - Cost: 2.25x makes 288x288, 576x576 and 495x495 targets at 4 samples (a few MiB each) and a 2160x1440 page at 2 samples with its depth (about 50 MiB; 88 MiB at 3x),
//    made when a menu first uses it; the draws a frame are the game's. A target that cannot be made steps the class down (ONE STEP FOR ALL).
//
// CHECK (nothing here was built or run; D3D12 and Vulkan, debug layer / validation on, the ui-sharp.tour pairs at 1080p and 4K):
//  1. The log has "[gpu] edram: ... made at 2.25x (the UI map views side view, 4 host samples; declared 220x220 ...)" lines for each identity while the setting is on (and
//     "colour k_8_8_8_8@960 pitch 960 2x made at 2.25x (... 2 host samples; declared 960x640 ...)"), and "[gpu] resolve: ... is a product of the UI map views side view: 495x495
//     host texels (2.25x the guest's) ... read at its own scale" (2160x1440 for the page).
//  2. mcla_ui_minimap_scale off, or the render scale below 1.25: pictures identical to a build without this class (the class is at 1x: nothing changes).
//  3. on, 1080p and 4K: the minimap's roads, blips and arrow, the phone's text, the pause menu's rows and capitals, the garage's lists and the GPS map's panel are sharper in the
//     same frame; no missing disc, no shifted or clipped radar, the page's panels and the card unchanged in place, the card's text not thinner.
//  4. A live off -> on -> off: no validation message, a few frames at most of a cleared view while the targets are remade ("the UI map views side view ... is now made at").
//  5. What reads the 128x128 pair (the census of the same run: the log's resolve line names its product and the draws that read it).
//  6. The 960x640 page at 2x and 3x on a small card (VRAM): the class steps down instead of failing; the log says so.
//  7. CHECK: with the quick map open at 1080p and 4K, compare its roads and pins, position and clipping through off -> on -> off. Check the 749x430 product's logged scale on both APIs.

#include <cstdint>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/xenos/format.h>

#include "edram.h"
#include "side_view_ui_map.h"
#include "side_views.h"
#include "ui_sharp.h"

namespace mcla::gpu {

namespace {

namespace xe = rex::xenos;

// The class and the UI sharpening move at the same render scale (side_views.h kFollowMinScale).
static_assert(side_views::kFollowMinScale == uint32_t(ui_sharp::kMinMagnification * float(Scale::kDenominator) + 0.5f),
              "the UI map views follow the render scale from the scale at which the UI sharpening starts");

static_assert(ui_map_views::kColorFormat == uint8_t(xe::ColorRenderTargetFormat::k_8_8_8_8));
static_assert(ui_map_views::kDepthFormat == uint8_t(xe::DepthRenderTargetFormat::kD24FS8));

std::string Setting() {
  return larecomp::settings::mcla_ui_minimap_scale.Get() ? "on" : "off";
}

bool Enabled() {
  return larecomp::settings::mcla_ui_minimap_scale.Get();
}

uint32_t HostSamples(Scale, uint32_t guest_samples) {
  // The guest's own samples at any scale: the pipelines of these draws exist at that count, and the resolve reads every one of them.
  return guest_samples;
}

}  // namespace

const SideViewRule kUiMapRule = {
    .view = SideView::kUiMap,
    .name = "UI map views",
    .matches = ui_map_views::Matches<EdramKey>,
    .setting = Setting,
    .effect = gpu_class::Effect::kReflectionResolution,  // unused: the class has no `auto` row (follows_output)
    .test_scale = nullptr,
    .host_samples = HostSamples,
    .follows_output = true,
    .enabled = Enabled,
};

}  // namespace mcla::gpu

// MCLA GPU renderer -- screen-space ambient occlusion. See ao.h.

#include "ao.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "format.h"
#include "frame.h"
#include "gpu_class.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_ao, "high", "MCLA/GPU",
    "Ambient occlusion: darkens creases, kerbs, the ground under cars and where objects meet, "
    "from the picture's depth, only on the sky light and not on the sun's (the game's own "
    "constants say how much of a surface's light is which). off, low (half resolution, 2 slices) "
    "or high (full resolution, 3 slices; the default). Costs graphics card time, high more than "
    "low. Also drawn behind the pause menu and on the map, so a change made there shows at once. "
    "Live.")
    .allowed({"off", "low", "high"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_ao_intensity, "default", "MCLA/GPU",
    "How strong ambient occlusion looks: light, default, strong or max. Each is a set of four "
    "numbers: how strongly it darkens, how far it looks for things that block the sky light, how "
    "hard the measure is pushed, and how much it also darkens the part of a surface the sun lights; "
    "the stronger ones reach farther and make every crease, corner and under-car shadow darker. "
    "Only used while mcla_gpu_ao is not off (the pause menu's AO INTENSITY row). Live.")
    .allowed({"light", "default", "strong", "max"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ao_fade_end, 160.0, "MCLA/GPU",
    "The distance, in the game's world units (about metres), where ambient occlusion has faded "
    "out entirely; it starts to fade at half of it. Fog, rain and particles are not darkened "
    "either way: they are drawn over the picture afterwards. Live.")
    .debug_only()
    .range(20.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the ambient occlusion as a grey picture instead of applying it "
    "(only while it is on): 1 the factor the picture is multiplied by (white: none), 2 the "
    "visibility, 3 the share of the light that is ambient, 4 the surface normals, 5 the distance "
    "(a log scale), 6 how much of the last frames each pixel kept (white: all it can, black: none, "
    "so it is new, a vehicle whose own motion is not known, or has no history yet; the history itself "
    "goes on as usual). 0: "
    "off. Live.")
    .debug_only()
    .range(0, 6)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_history, 12, "MCLA/GPU",
    "Diagnostic: how many frames ambient occlusion averages to take its sampling noise out, 1 to "
    "64. Each frame's occlusion is found again in the next by the camera's motion and kept only "
    "where it is the same surface; more frames is smoother and slower to follow a moving shadow "
    "(a car's), fewer is noisier. 1: no averaging (two blur passes on every frame instead). Live.")
    .debug_only()
    .range(1, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_contact_shadows, "auto", "MCLA/GPU",
    "Contact shadows: the short sun shadows the game does not draw (under a car's sills, at the foot "
    "of a kerb or a wall, under props on the pavement), found from the picture's depth and put only "
    "on the sun's light, so they never darken what the game's own shadows already cover. Early in the "
    "morning and in the evening, when the game draws no sun shadows at all, they are the only ones. "
    "They are left out at night and in rain. off, low (half resolution, 8 steps), high (full "
    "resolution, 16 steps) or auto (the default): off on an integrated graphics card, low on a "
    "discrete one with less than 5.5 GiB of video memory, high on the rest. Costs graphics card time, "
    "high more than low. Also drawn behind the pause menu and on the map. Live.")
    .allowed({"auto", "off", "low", "high"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_length, 0.6, "MCLA/GPU",
    "Diagnostic: how far toward the sun contact shadows look, in the game's world units (about "
    "metres), with the sun high. The length grows with a low sun (0.7 / tan of its elevation, 1 to 3 "
    "times). Live.")
    .debug_only()
    .range(0.1, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_strength, 1.0, "MCLA/GPU",
    "Diagnostic: how strongly contact shadows darken the sun's light where they apply, 0 to 2 "
    "(the sun's elevation and the wetness scale it further). Live.")
    .debug_only()
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_thickness, 0.3, "MCLA/GPU",
    "Diagnostic: how far in front of a step of the ray a surface may be and still hide it, in the "
    "game's world units: a thing nearer to the camera than that only overlaps on screen and hides "
    "nothing. Live.")
    .debug_only()
    .range(0.05, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_contact_shadows_mask_gain, 2.0, "MCLA/GPU",
    "Diagnostic: what the game's sun shadow mask's stored value is multiplied by to give the sun's "
    "visibility (1 lit): the game's collector stores half of it and the lit shaders multiply by 2. "
    "Look at mcla_gpu_contact_shadows_view 3 (white where lit) if shadows are darkened twice or not "
    "at all. Live.")
    .debug_only()
    .range(0.5, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_contact_shadows_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the contact shadows as a grey picture instead of applying them "
    "(only while they are on, and instead of mcla_gpu_ao_view): 1 the factor the picture is "
    "multiplied by (white: none), 2 the contact visibility (black where the ray is hidden), 3 the "
    "sun's visibility (the game's shadow mask: white lit; white everywhere where the frame has none), "
    "4 the sun's share of the light. 0: off. Live.")
    .debug_only()
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    mcla_gpu_ssgi, "auto", "MCLA/GPU",
    "Bounce light: the light a lit wall or the ground sends back onto the road, the sides of cars and "
    "the walls beside it, found from the picture, so a red wall tints the pavement by it and a street "
    "lamp's pool lights the wall next to it. It takes the place of part of the sky light the ambient "
    "occlusion darkens, so a lit area does not get brighter overall, and a flat surface stays as it "
    "was. Needs ambient occlusion (mcla_gpu_ao) on. off, low (a coarser search: 2 directions and 2 steps "
    "each; half resolution where the occlusion is low too), high (3 and 3 at full resolution) or auto (the "
    "default): off on "
    "an integrated graphics card and on a discrete one with less than 5.5 GiB of video memory, low on "
    "the rest. Costs graphics card time, high more than low. Not drawn behind the pause menu or on "
    "the map. Live.")
    .allowed({"auto", "off", "low", "high"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_strength, 0.75, "MCLA/GPU",
    "Diagnostic: how much of the bounce light the picture gets, 0 to 2 (1: all that the surroundings "
    "send back by the picture's own colours; 0 runs the passes and adds nothing). Live.")
    .debug_only()
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_radius, 1.0, "MCLA/GPU",
    "Diagnostic: how far the bounce light looks, as a multiple of the ambient occlusion's radius "
    "(mcla_gpu_ao_intensity); the same samples give the occlusion's visibility, so a change moves "
    "that too. 1: the occlusion's own. Live.")
    .debug_only()
    .range(0.5, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_clamp, 0.5, "MCLA/GPU",
    "Diagnostic: the most the bounce light may add to a pixel, as a share of that pixel's own colour "
    "(its largest channel; the hue stays), 0.05 to 4: a bright lamp's pool or a neon sign beside a wall "
    "cannot light it white. (What a sample of the scene may carry into the bounce is "
    "mcla_gpu_ssgi_test_clamp.) Live.")
    .debug_only()
    .range(0.05, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_low, 0, "MCLA/GPU",
    "Diagnostic: what the bounce light's low does (mcla_gpu_ssgi low or auto that comes to low) to be cheaper "
    "than high: 0 fewer directions (2 slices and 2 steps each instead of 3 and 3) at the occlusion's own "
    "size, 1 half the picture's size (the occlusion's slices and steps) even where the occlusion is high, 2 "
    "both. The same samples give the occlusion's visibility, so 1 and 2 also make the occlusion's own "
    "picture half resolution while the bounce light is on. Live.")
    .debug_only()
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the bounce light's apply as a picture instead of applying it (only "
    "while the bounce light is on, and instead of the occlusion's and the contact shadows' views; the "
    "test's own views come first): 1 the factor the picture is multiplied by, in colour (the "
    "occlusion's plus the bounce), 2 the bounce each pixel gets (four times as bright as the others), "
    "3 the albedo estimate (the colour over the light the game's constants say fell on it, up to 1), "
    "4 that light, 5 how much of the colour is a surface (black: a source of light, which gets no "
    "bounce), 6 the change of the picture against the game's own (grey: none; lighter: brighter; "
    "darker). Shown at 0.3 of the value, except 6. 0: off. Live.")
    .debug_only()
    .range(0, 6)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_ssgi_test, false, "MCLA/GPU",
    "Diagnostic: the debug override of the bounce light (mcla_gpu_ssgi): forces its passes on, at the "
    "occlusion's own quality and whatever mcla_gpu_ssgi says (only while mcla_gpu_ao is not off). The "
    "scene colour at the occlusion's trigger is kept in a chain of images and the occlusion's main "
    "pass becomes a visibility bitmask that also finds what colour the blockers of each pixel's sky "
    "light are, then accumulated over frames and filtered. The occlusion's own look stays (its "
    "visibility now comes from the bitmask); the bounce is added to the picture as the setting adds it "
    "(mcla_gpu_ssgi_strength 0: not added), or shown by mcla_gpu_ssgi_test_view. Costs about as much "
    "as the occlusion again. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_thickness, 0.0, "MCLA/GPU",
    "Diagnostic: how thick the surfaces the bitmask finds are assumed to be, in the game's world units "
    "(about metres): a sample blocks the directions between its front and its back, the back being "
    "this far behind the front along the view ray. 0: solid, a sample blocks everything below its "
    "horizon (the occlusion's own horizon search, to within 1/32). Live.")
    .debug_only()
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_clamp, 2.0, "MCLA/GPU",
    "Diagnostic: the brightest radiance a sample of the scene colour may carry into the bounce, as a "
    "multiple of the frame's ambient plus sun light (never under 0.5): a street lamp, neon or the sun's "
    "disc is a source, not a surface that bounces light, and would light its whole neighbourhood "
    "white. Live.")
    .debug_only()
    .range(0.25, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_facing, 1.0, "MCLA/GPU",
    "Diagnostic: how much a sample's surface must face the pixel to light it, 0 to 1: 0 takes every "
    "surface to face the pixel, 1 weighs each by the slope of its depth along the slice (a roof seen "
    "from the ground, a wall seen from behind: nothing). Costs one more depth read per lit sector. "
    "Live.")
    .debug_only()
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_test_view, 0, "MCLA/GPU",
    "Diagnostic: shows a stage of the bounce light test as a picture instead of the scene colour "
    "(only while mcla_gpu_ssgi_test is on, and instead of the other debug views): 1 the bounce light "
    "the main pass found (one frame, noisy), 2 the scene colour chain's level 0 (the colour the "
    "blockers are taken from: averaged over the samples, capped), 3 its level 1, 4 its level 2, 5 the "
    "bounce light cleaned by a 5x5 mean, 6 the visibility the bitmask found (one frame, noisy: the "
    "occlusion's own view 2 shows it after the denoise and the history), 7 the bounce light after the "
    "accumulation over frames (the history), 8 the bounce light after the a-trous filter too (what the "
    "picture's bounce will be made of), 9 how much of the history each pixel kept (white: all it can, "
    "black: none, so it is new, a vehicle whose own motion is not known, or has no history yet). Shown at "
    "0.3 of its value times mcla_gpu_ssgi_test_view_gain (view 9 at full white). 0: off. Live.")
    .debug_only()
    .range(0, 9)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_view_gain, 1.0, "MCLA/GPU",
    "Diagnostic: the brightness of mcla_gpu_ssgi_test_view's picture, 0.1 to 16 times. Live.")
    .debug_only()
    .range(0.1, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_test_history, 16, "MCLA/GPU",
    "Diagnostic: how many frames the bounce light test averages the bounce over, 2 to 64 (the "
    "occlusion's own visibility keeps mcla_gpu_ao_history). The bounce is found again in the last frame "
    "by the camera's and the vehicles' motion and kept only where it is the same surface; more frames is "
    "smoother and slower to follow a light that changed, fewer is noisier. Live.")
    .debug_only()
    .range(2, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_ssgi_test_history_clamp, 1.25, "MCLA/GPU",
    "Diagnostic: how far the bounce's history may be from this frame's 3x3 neighbourhood of the bounce, in "
    "standard deviations of that neighbourhood (and never under 5% of its mean): the history is cut to that "
    "box. Narrow follows a changed light at once and keeps more noise; wide keeps the history longer and "
    "leaves a ghost of a car that went by. Live.")
    .debug_only()
    .range(0.25, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ssgi_test_atrous, 2, "MCLA/GPU",
    "Diagnostic: the iterations of the a-trous filter that takes the noise out of the bounce light after the "
    "history, 0 to 3 (0: none; each iteration reaches twice as far: 5x5, 9x9, 17x17 pixels, and stops at "
    "depth edges and corners). The history itself is never filtered. Live.")
    .debug_only()
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_census, 0, "MCLA/GPU",
    "Diagnostic: every Nth guest frame (and the first three frames with the main view), one block "
    "of log lines with the main view's draws around the game's mid-scene depth resolve, that "
    "resolve (where it is, how big, inside EndTiling or not), the light constants the occlusion "
    "uses (PS c26 ambient, PS c39 sun colour, VS c16 sun direction), the depth clear value, the "
    "lens the game's motion blur draw gives, what the history did and a statistic of the linear "
    "depth (read back from the graphics card, so the first three frames and every Nth one run the "
    "depth passes even with the occlusion off). Frames where the main view does not have exactly "
    "one qualifying resolve are logged as they come (the first 40). 0: off. Live.")
    .debug_only()
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[ao census]";
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) |
                              uint8_t(plume::RenderColorWriteEnable::GREEN) |
                              uint8_t(plume::RenderColorWriteEnable::BLUE);

// The registers the occlusion reads: PS c26 gLightAmbient, PS c39 gLightColor, VS c16 gLightPosDir
// (the direction the sunlight travels: at noon y is -0.867).
constexpr uint32_t kPsAmbient = 26;
constexpr uint32_t kPsSunColor = 39;
constexpr uint32_t kVsSunDirection = 16;
// VS c43: gReflectionParams, whose x is the road's wetness (0.095 dry, 1.17 in rain).
constexpr uint32_t kVsReflectionParams = 43;

// The settings the passes use that no cvar sets: the effect's falloff range as a share of the
// radius, the floor of the final visibility (no black blotches). The power of the visibility is
// part of the look (mcla_gpu_ao_intensity).
constexpr float kFalloffRatio = 0.615f;
constexpr float kMinVisibility = 0.1f;
// The frames the occlusion has to stay off before its images are given back.
constexpr uint32_t kReleaseAfterFrames = 60;
// Sizes below this (host pixels, either way) are not worth the passes.
constexpr uint32_t kMinRenderSize = 64;
// The history's flag in the push block's flags (hlsl/ao_temporal_ps.hlsl AoHistoryOn), and the flag
// of the pause/map view, whose vehicle pixels take the camera's motion like the world
// (AoVehiclesAsWorld).
constexpr uint32_t kHistoryFlag = 1u << 20;
constexpr uint32_t kVehiclesAsWorldFlag = 1u << 21;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
// The vehicle table (hlsl/ao_temporal_ps.hlsl): a row an ID, four texels a row (the three columns of
// the matrix and the usable flag, hlsl/temporal_vehicle_table_ps.hlsl).
constexpr plume::RenderFormat kVehicleTableFormat = plume::RenderFormat::R32G32B32A32_FLOAT;
constexpr uint32_t kVehicleTableWidth = 4;
// The vehicle summary line is printed every this many guest frames (and once, the first time a table
// is drawn).
constexpr uint64_t kVehicleLogEvery = 3600;
// The vertex constants' view inverse is taken as the frame's own camera once it matched the
// anchor's within kVsMatch in a frame whose camera moved by at least kVsMoved (an entry of the
// view inverse: a step of that size shows in it; a stale copy of the last frame's would not match).
constexpr double kVsMatch = 5e-4;
constexpr double kVsMoved = 5e-3;
// The sky and anything the linearisation could not place (hlsl/ao_common.hlsli kAoSky).
constexpr float kSkyDistance = 60000.0f;

// The flags of the apply pass (hlsl/ao_apply_ps.hlsl): the occlusion's term, the contact shadows', the
// game's shadow mask.
constexpr uint32_t kApplyOcclusionFlag = 1u << 22;
constexpr uint32_t kApplyContactFlag = 1u << 23;
constexpr uint32_t kApplyMaskFlag = 1u << 24;
// The bounce light's apply (hlsl/ao_apply_ps.hlsl, AO_GI): its limit rides in flags bits 8-15 (the slices'
// and steps' place, which the apply does not read) in units of kGiLimitUnit, and its debug views are 16 to
// 21 (mcla_gpu_ssgi_view 1 to 6).
constexpr uint32_t kGiLimitShift = 8;
constexpr uint32_t kGiLimitMask = 0xFFu << kGiLimitShift;
constexpr float kGiLimitUnit = 0.02f;
constexpr uint32_t kGiApplyViewBase = 15;
// The frames the bounce light's images are kept after the last frame that used them: a pause spell (the
// pause menu's and the map's world runs the plain occlusion) does not make them again.
constexpr uint32_t kGiReleaseAfterFrames = 600;
// The debug views of the contact shadows are 8 to 11 in the apply pass's view field (the occlusion's
// are 1 to 6).
constexpr uint32_t kContactViewBase = 7;

// Contact shadows. The game's shadow mask: the page of the resolve that makes it (the 1280x720
// k_8_8_8_8 collector output, resolved at the main view's phase 0x4000), a texture key's base page.
constexpr uint32_t kMaskPage = 0x06ACD000u >> 12;
constexpr uint8_t kTextureFormat8888 = 6;  // xenos::TextureFormat::k_8_8_8_8
constexpr uint32_t kMaskMinWidth = 640;
// The sun's elevation gate: no contact shadows at this many degrees and below (the night's light
// points up, 19 h is at 2.5 degrees with a grey sun), all of them from kGateFullDegrees.
constexpr double kGateOffDegrees = 3.0;
constexpr double kGateFullDegrees = 12.0;
// The wetness gate (VS c43.x: 0.095 dry, 1.17 in rain): full strength up to kWetDry, none from
// kWetDry + kWetSpan.
constexpr float kWetDry = 0.2f;
constexpr float kWetSpan = 0.5f;
// The ray grows with a low sun, 0.7 / tan(elevation), kept between 1 and 3 times the setting.
constexpr double kLengthTan = 0.7;
constexpr double kLengthMaxScale = 3.0;
// The distance fade of the contact shadows, in world units: a ray of a few tenths of a metre is
// under a pixel far away.
constexpr float kContactFadeStart = 40.0f;
constexpr float kContactFadeEnd = 80.0f;
// The steps of the ray at low and at high.
constexpr uint32_t kContactStepsLow = 8;
constexpr uint32_t kContactStepsHigh = 16;

// The SSGI test (hlsl/ao_color_ps.hlsl, the bitmask pass of hlsl/ao_main_ps.hlsl): the images are RGBA16F
// (the colour chain's three, and the main pass's visibility + bounce); a sample's radiance is capped at
// a multiple of the frame's ambient + sun and never under this, so that a night's dim constants
// (ambient 0.05) do not cap a lamp's pool to nothing.
constexpr plume::RenderFormat kGiFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr float kGiCapFloor = 0.5f;
// The debug pictures of mcla_gpu_ssgi_test_view, as the colour view pass numbers them (ao_color_ps.hlsl
// pass 2): the image's rgb, its yzw, its x, its yzw cleaned by a 5x5 mean.
constexpr uint32_t kGiViewRgb = 1;
constexpr uint32_t kGiViewBounce = 2;
constexpr uint32_t kGiViewGrey = 3;
constexpr uint32_t kGiViewBounceMean = 4;
// What the colour view pass multiplies a debug picture by (ao_color_ps.hlsl kViewScale): a view that is
// a weight asks for it back so that 1 is white.
constexpr float kGiViewScale = 0.3f;

float Smoothstep(double edge0, double edge1, double x) {
  const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return float(t * t * (3.0 - 2.0 * t));
}

// The looks of mcla_gpu_ao_intensity (the pause menu's AO INTENSITY row): the darkening's strength,
// the radius in the game's world units, the power of the visibility and the direct share (how much
// of the sun-lit part is darkened too). "default" is the setting's default.
struct LookDef {
  const char* name;
  float strength;
  float radius;
  float power;
  float direct;
};
constexpr LookDef kLooks[] = {
    {"light", 1.0f, 2.5f, 3.2f, 0.25f},
    {"default", 1.0f, 3.0f, 3.6f, 0.375f},
    {"strong", 1.0f, 3.5f, 4.0f, 0.5f},
    {"max", 1.0f, 3.75f, 4.5f, 0.75f},
};
constexpr uint32_t kLookCount = sizeof(kLooks) / sizeof(kLooks[0]);
constexpr uint32_t kDefaultLook = 1;

// The index of mcla_gpu_ao_intensity's look; anything the list does not know is the default.
uint32_t ReadLookIndex() {
  std::string value = REXCVAR_GET(mcla_gpu_ao_intensity);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  for (uint32_t i = 0; i < kLookCount; ++i) {
    if (value == kLooks[i].name) {
      return i;
    }
  }
  return kDefaultLook;
}

// A matrix that can be a camera's view inverse: finite, the last column (0, 0, 0, 1), unit axes.
bool PoseSane(const temporal::Mat4& w) {
  if (!temporal::Finite(w)) {
    return false;
  }
  if (std::fabs(w.m[0][3]) > 1e-3 || std::fabs(w.m[1][3]) > 1e-3 || std::fabs(w.m[2][3]) > 1e-3 ||
      std::fabs(w.m[3][3] - 1.0) > 1e-3) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    const double length = std::sqrt(w.m[r][0] * w.m[r][0] + w.m[r][1] * w.m[r][1] +
                                    w.m[r][2] * w.m[r][2]);
    if (!(length > 0.98 && length < 1.02)) {
      return false;
    }
  }
  return true;
}

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

double Length3(const float v[3]) {
  return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
}

std::string Vec3(const float v[3]) {
  return std::format("({:.4g}, {:.4g}, {:.4g})", v[0], v[1], v[2]);
}

std::string RefusalNames(uint32_t refused) {
  static const char* const kNames[] = {
      "not a depth copy",
      "not the main view's depth",
      "inside EndTiling",
      "not the first qualifying one of the frame",
      "after the anchor or the composite marker",
      "not into 078B0000",
      "the same call clears the depth",
      "the rectangle does not start at the origin or leaves the picture",
      "another tile of the picture the trigger covered whole",
      "the destination is smaller than the main target (a 4x view's tile texture)"};
  std::string out;
  for (uint32_t i = 0; i < 10; ++i) {
    if (refused & (1u << i)) {
      out += std::format("{}{}", out.empty() ? "" : ", ", kNames[i]);
    }
  }
  return out;
}

const char* ModeName(AmbientOcclusion::Mode mode) {
  switch (mode) {
    case AmbientOcclusion::Mode::kOff: return "off";
    case AmbientOcclusion::Mode::kLow: return "low";
    case AmbientOcclusion::Mode::kHigh: return "high";
  }
  return "?";
}

AmbientOcclusion::Mode ReadMode() {
  std::string value = REXCVAR_GET(mcla_gpu_ao);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (value == "low") {
    return AmbientOcclusion::Mode::kLow;
  }
  if (value == "high") {
    return AmbientOcclusion::Mode::kHigh;
  }
  return AmbientOcclusion::Mode::kOff;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return value;
}

// mcla_gpu_contact_shadows with `auto` resolved for this GPU (gpu_class.h).
AmbientOcclusion::Mode ReadContactMode() {
  const std::string value = gpu_class::Resolve(gpu_class::Effect::kContactShadows,
                                               Lower(REXCVAR_GET(mcla_gpu_contact_shadows)));
  if (value == "low") {
    return AmbientOcclusion::Mode::kLow;
  }
  if (value == "high") {
    return AmbientOcclusion::Mode::kHigh;
  }
  return AmbientOcclusion::Mode::kOff;
}

// mcla_gpu_ssgi with the auto value resolved for this GPU (gpu_class.h): off, low or high.
AmbientOcclusion::Mode ReadSsgiMode() {
  const std::string value =
      gpu_class::Resolve(gpu_class::Effect::kSsgi, Lower(REXCVAR_GET(mcla_gpu_ssgi)));
  if (value == "low") {
    return AmbientOcclusion::Mode::kLow;
  }
  if (value == "high") {
    return AmbientOcclusion::Mode::kHigh;
  }
  return AmbientOcclusion::Mode::kOff;
}

// A binary16 as a float.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1Fu;
  uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

// A light constant that is a usable amount (a draw whose shaders do not use the register can leave
// anything there).
float Usable(float x) {
  return std::isfinite(x) && x > 0.0f ? x : 0.0f;
}

float Luma(const float c[3]) {
  return 0.2126f * Usable(c[0]) + 0.7152f * Usable(c[1]) + 0.0722f * Usable(c[2]);
}

}  // namespace

bool AmbientOcclusion::LightRaw::operator==(const LightRaw& o) const {
  return std::memcmp(dwords, o.dwords, sizeof(dwords)) == 0;
}

void AmbientOcclusion::DecodeLight(const LightRaw& raw, float ambient[3], float sun[3],
                                   float dir[3]) {
  for (int i = 0; i < 3; ++i) {
    ambient[i] = std::bit_cast<float>(raw.dwords[i]);
    sun[i] = std::bit_cast<float>(raw.dwords[3 + i]);
    dir[i] = std::bit_cast<float>(raw.dwords[6 + i]);
  }
}

AmbientOcclusion::~AmbientOcclusion() {
  Shutdown();
}

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

uint32_t AmbientOcclusion::ApplyVariant(bool debug, uint32_t host_samples) {
  const uint32_t log2_samples = std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
  return kVApplyBase + (debug ? 4u : 0u) + log2_samples;
}

uint32_t AmbientOcclusion::GiApplyVariant(bool debug, uint32_t host_samples) {
  const uint32_t log2_samples = std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
  return kVApplyGiBase + (debug ? 4u : 0u) + log2_samples;
}

uint32_t AmbientOcclusion::GiViewVariant(uint32_t host_samples) {
  const uint32_t log2_samples = std::min(uint32_t(std::countr_zero(std::max(host_samples, 1u))), 3u);
  return kVGiView + log2_samples;
}

std::string AmbientOcclusion::VariantName(uint32_t variant) {
  switch (variant) {
    case kVDepth: return "ao depth";
    case kVDepthMs: return "ao depth (multisampled)";
    case kVMip: return "ao depth level";
    case kVMain: return "ao visibility";
    case kVDenoise: return "ao denoise";
    case kVTemporal: return "ao temporal";
    case kVTemporalMs: return "ao temporal (multisampled)";
    case kVVehicleTable: return "ao vehicle table";
    case kVContact: return "ao contact shadows";
    case kVMainGi: return "ssgi visibility bitmask";
    case kVColor: return "ssgi colour";
    case kVColorMs: return "ssgi colour (multisampled)";
    case kVColorMip: return "ssgi colour level";
    case kVDenoiseGi: return "ssgi denoise";
    case kVTemporalGi: return "ssgi temporal";
    case kVTemporalGiMs: return "ssgi temporal (multisampled)";
    case kVAtrous: return "ssgi a-trous";
    default: break;
  }
  if (variant >= kVGiView && variant < kVGiView + 4) {
    return std::format("ssgi view {}x", 1u << (variant - kVGiView));
  }
  if (variant >= kVApplyGiBase) {
    const uint32_t gi_index = variant - kVApplyGiBase;
    return std::format("ssgi {} {}x", (gi_index & 4) ? "apply view" : "apply", 1u << (gi_index & 3));
  }
  const uint32_t index = variant - kVApplyBase;
  return std::format("ao {} {}x", (index & 4) ? "view" : "apply", 1u << (index & 3));
}

void AmbientOcclusion::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                                  HostPasses& passes, EdramTargets& edram,
                                  PassTiming& pass_timing, ResolveCache& resolve) {
  resolve_ = &resolve;
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  // The host pass layout (host_passes.h) with a push block of 128 bytes instead of 48: the four
  // texture sets, the sampler set, the push range at (b0, space4), for both stages as there.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap.texture_set_builder());
  }
  builder.addDescriptorSet(heap.sampler_set_builder());
  builder.addPushConstant(0, 4, sizeof(Push),
                          plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "ao layout");
  bool ok = layout_ != nullptr;
  for (uint32_t i = 0; i < kCorePassCount && ok; ++i) {
    const HostShaderBlob blob = AmbientOcclusionPixelShader(i);
    shaders_[i] = passes.CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[gpu] ao: the layout or a shader could not be made: no ambient occlusion this "
                 "session");
    for (auto& shader : shaders_) {
      shader.reset();
    }
    layout_.reset();
    return;
  }
  // The vehicle table's rows are drawn with the temporal system's own shader (three float4 of push
  // constants as the three columns, a flag in the fourth texel) into an image of the occlusion's own.
  // Without it the vehicles keep no history (the wide blur, as before the vehicles' motion was used).
  const HostShaderBlob table = TemporalVehicleTablePixelShader();
  vehicle_table_shader_ = passes.CreateShader(table.data, table.size, table.name);
  if (!vehicle_table_shader_) {
    REXLOG_ERROR("[gpu] ao: the vehicle table's shader could not be made: the vehicles' pixels keep "
                 "no history this session (the wide blur)");
  }
  ready_ = true;
  ListPrecache();
}

bool AmbientOcclusion::EnsureGiShaders() {
  if (gi_ready_) {
    return true;
  }
  if (!ready_ || gi_shaders_failed_) {
    return false;
  }
  // The bounce light test's shaders are made the first time it is asked for (a player never asks, and
  // the shaders of the occlusion are all a player's session holds); a failure takes the test only.
  bool ok = true;
  for (uint32_t i = kCorePassCount; i < kPassCount && ok; ++i) {
    const HostShaderBlob blob = AmbientOcclusionPixelShader(i);
    shaders_[i] = passes_->CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    for (uint32_t i = kCorePassCount; i < kPassCount; ++i) {
      shaders_[i].reset();
    }
    gi_shaders_failed_ = true;
    REXLOG_ERROR("[gpu] ao: a shader of the bounce light test could not be made: no such test this "
                 "session (the occlusion is not affected)");
    return false;
  }
  gi_ready_ = true;
  return true;
}

void AmbientOcclusion::Shutdown() {
  // The host builder's functions name this object's layout and shaders: stop it first.
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (PostAa::Shutdown).
  ++generation_;
  probe_buffer_.reset();
  probe_busy_ = false;
  for (HostImage& image : depth_) {
    image = HostImage{};
  }
  raw_ = HostImage{};
  denoised_ = HostImage{};
  for (HostImage& image : history_) {
    image = HostImage{};
  }
  weight_ = HostImage{};
  for (HostImage& image : colour_) {
    image = HostImage{};
  }
  gi_ = HostImage{};
  gi_dn_ = HostImage{};
  for (HostImage& image : gi_hist_) {
    image = HostImage{};
  }
  depth_prev_ = HostImage{};
  gi_weight_ = HostImage{};
  vehicle_table_ = HostImage{};
  history_valid_ = false;
  history_gi_ = false;
  for (auto& pipeline : pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : shaders_) {
    shader.reset();
  }
  vehicle_table_shader_.reset();
  layout_.reset();
  ready_ = false;
  gi_ready_ = false;
  gi_shaders_failed_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
  resolve_ = nullptr;
}

void AmbientOcclusion::ListPrecache() {
  HostPrecache& precache = passes_->precache();
  // The apply pass for the play view's colour target (1x, 2x) and the pause/map view's (4x), so
  // that opening the pause menu with the occlusion on waits for no pipeline.
  std::vector<uint32_t> listed = {kVDepth,           kVDepthMs,          kVMip,
                                  kVMain,            kVDenoise,          kVTemporal,
                                  kVTemporalMs,      ApplyVariant(false, 1), ApplyVariant(false, 2),
                                  ApplyVariant(false, 4), kVContact};
  // The host's MSAA level changes while the game runs (msaa_level.h) and gives the main view's
  // colour target the new level's samples: the apply pass for 8 samples is listed whatever the
  // level is at the start, so a switch to 8x finds it made instead of making it on the render
  // thread in the first frame at that level (the guest's own pipelines are made ahead for the new
  // level by Pipelines::PrebuildForLevel; this pass is not one of them). 2x and 4x are listed above
  // (the pause and map view's colour target has the game view's samples, or 1 without MSAA). A level
  // the device cannot do is not listed.
  if (edram_ && edram_->SupportedMsaaLevel(8) == 8) {
    listed.push_back(ApplyVariant(false, 8));
  }
  if (vehicle_table_shader_) {
    listed.push_back(kVVehicleTable);
  }
  // The bounce light's pipelines, when the setting says it runs from the start (auto on a high-end graphics
  // card): made on the builder thread with the others, so that its first frame does not make a dozen of them
  // on the render thread. A setting changed while the game runs makes them when it is first used.
  if (ReadSsgiMode() != Mode::kOff && EnsureGiShaders()) {
    for (const uint32_t variant : {uint32_t(kVMainGi), uint32_t(kVColor), uint32_t(kVColorMs),
                                   uint32_t(kVColorMip), uint32_t(kVDenoiseGi), uint32_t(kVTemporalGi),
                                   uint32_t(kVTemporalGiMs), uint32_t(kVAtrous),
                                   GiApplyVariant(false, 1), GiApplyVariant(false, 2),
                                   GiApplyVariant(false, 4)}) {
      listed.push_back(variant);
    }
    if (edram_ && edram_->SupportedMsaaLevel(8) == 8) {
      listed.push_back(GiApplyVariant(false, 8));
    }
  }
  for (const uint32_t variant : listed) {
    precache.Add(HostPrecache::Key(HostPrecache::kAmbientOcclusion, variant),
                 VariantName(variant), [this, variant] { return MakePipeline(variant); });
  }
}

std::unique_ptr<plume::RenderPipeline> AmbientOcclusion::MakePipeline(uint32_t variant) {
  const std::string name = VariantName(variant);
  const plume::RenderBlendDesc copy = plume::RenderBlendDesc::Copy();
  const auto make = [&](Pass pass, plume::RenderFormat format, const plume::RenderBlendDesc& blend,
                        uint32_t samples) {
    return passes_->CreatePipeline(*layout_, *shaders_[pass], format, blend, samples, name.c_str());
  };
  switch (variant) {
    case kVDepth: return make(kPassDepth, plume::RenderFormat::R32_FLOAT, copy, 1);
    case kVDepthMs: return make(kPassDepthMs, plume::RenderFormat::R32_FLOAT, copy, 1);
    case kVMip: return make(kPassMip, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVMain: return make(kPassMain, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVDenoise: return make(kPassDenoise, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVTemporal: return make(kPassTemporal, plume::RenderFormat::R16G16_FLOAT, copy, 1);
    case kVTemporalMs: return make(kPassTemporalMs, plume::RenderFormat::R16G16_FLOAT, copy, 1);
    case kVContact: return make(kPassContact, plume::RenderFormat::R16_FLOAT, copy, 1);
    case kVMainGi: return make(kPassMainGi, kGiFormat, copy, 1);
    case kVColor: return make(kPassColor, kGiFormat, copy, 1);
    case kVColorMs: return make(kPassColorMs, kGiFormat, copy, 1);
    case kVColorMip: return make(kPassColorMip, kGiFormat, copy, 1);
    case kVDenoiseGi: return make(kPassDenoiseGi, kGiFormat, copy, 1);
    case kVTemporalGi: return make(kPassTemporalGi, kGiFormat, copy, 1);
    case kVTemporalGiMs: return make(kPassTemporalGiMs, kGiFormat, copy, 1);
    case kVAtrous: return make(kPassAtrous, kGiFormat, copy, 1);
    case kVVehicleTable:
      // The host pass layout (the row's 48 bytes of push constants), as the temporal system's own table.
      return vehicle_table_shader_ ? passes_->CreatePipeline(*vehicle_table_shader_,
                                                             kVehicleTableFormat, name.c_str())
                                   : nullptr;
    default: break;
  }
  if (variant >= kVGiView && variant < kVGiView + 4) {
    // The test's debug picture: into the scene colour with the target's sample count, the rgb replaced
    // (the pass writes one colour for every sample of a pixel).
    plume::RenderBlendDesc view = copy;
    view.renderTargetWriteMask = kWriteRgb;
    return make(kPassColorView, plume::RenderFormat::R16G16B16A16_FLOAT, view,
                1u << (variant - kVGiView));
  }
  // The apply pass: into the scene colour (float16 RGBA) with the target's sample count. The
  // multiply is dst.rgb * src.rgb (the shader writes the factor); a debug view replaces the rgb.
  // Alpha is never written. The bounce light's apply (G7c) is the same pipeline with its own shader:
  // the factor is then one a colour channel, and may be above 1 (the target is a float one).
  const bool gi_apply = variant >= kVApplyGiBase;
  const uint32_t index = variant - (gi_apply ? kVApplyGiBase : kVApplyBase);
  const bool debug = (index & 4) != 0;
  const uint32_t samples = 1u << (index & 3);
  plume::RenderBlendDesc blend = copy;
  if (!debug) {
    blend.blendEnabled = true;
    blend.srcBlend = plume::RenderBlend::ZERO;
    blend.dstBlend = plume::RenderBlend::SRC_COLOR;
    blend.blendOp = plume::RenderBlendOperation::ADD;
    blend.srcBlendAlpha = plume::RenderBlend::ZERO;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
    blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  }
  blend.renderTargetWriteMask = kWriteRgb;
  if (gi_apply) {
    return make(samples > 1 ? kPassApplyGiMs : kPassApplyGi,
                plume::RenderFormat::R16G16B16A16_FLOAT, blend, samples);
  }
  return make(samples > 1 ? kPassApplyMs : kPassApply,
              plume::RenderFormat::R16G16B16A16_FLOAT, blend, samples);
}

plume::RenderPipeline* AmbientOcclusion::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (pipeline_failed_[variant]) {
    return nullptr;
  }
  // Made ahead when listed; a variant nobody listed (a debug view, a 4x target) is made here.
  pipelines_[variant] = passes_->precache().Acquire(
      HostPrecache::Key(HostPrecache::kAmbientOcclusion, variant), VariantName(variant),
      [&] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    pipeline_failed_[variant] = true;
    REXLOG_ERROR("[gpu] ao: the pipeline '{}' could not be made: no {} this session in that "
                 "configuration",
                 VariantName(variant),
                 variant >= kVMainGi ? "bounce light" : "ambient occlusion");
  }
  return pipelines_[variant].get();
}

void AmbientOcclusion::BeginFrame() {
  census_every_ = uint32_t(std::max(0, REXCVAR_GET(mcla_gpu_ao_census)));
  Mode mode = ReadMode();
  if (!ready_ && mode != Mode::kOff) {
    mode = Mode::kOff;  // the passes could not be made (Initialize said so)
  }
  if (uint32_t(mode) != last_mode_logged_) {
    last_mode_logged_ = uint32_t(mode);
    REXLOG_INFO("[gpu] ao: ambient occlusion {} (mcla_gpu_ao, live)", ModeName(mode));
  }
  mode_ = mode;
  Mode contact = ReadContactMode();
  if (!ready_ || !resolve_) {
    contact = Mode::kOff;  // the passes could not be made (Initialize said so)
  }
  if (uint32_t(contact) != last_contact_logged_) {
    last_contact_logged_ = uint32_t(contact);
    const std::string setting = Lower(REXCVAR_GET(mcla_gpu_contact_shadows));
    REXLOG_INFO("[gpu] contact shadows {}{} (mcla_gpu_contact_shadows {}, live)", ModeName(contact),
                setting == "auto" ? " (auto on this GPU)" : "", setting);
  }
  contact_mode_ = contact;
  // The bounce light: the setting (its auto value resolved for this GPU) or the debug override
  // mcla_gpu_ssgi_test. It needs the occlusion's own images and passes (PlanGi says so again per frame).
  Mode gi_mode = ReadSsgiMode();
  if (!ready_) {
    gi_mode = Mode::kOff;  // the passes could not be made (Initialize said so)
  }
  if (uint32_t(gi_mode) != last_gi_logged_) {
    last_gi_logged_ = uint32_t(gi_mode);
    const std::string setting = Lower(REXCVAR_GET(mcla_gpu_ssgi));
    REXLOG_INFO("[gpu] ssgi: bounce light {}{} (mcla_gpu_ssgi {}, live; it needs mcla_gpu_ao on)",
                ModeName(gi_mode), setting == "auto" ? " (auto on this GPU)" : "", setting);
  }
  gi_mode_ = gi_mode;
  const bool test = REXCVAR_GET(mcla_gpu_ssgi_test);
  gi_active_ = (test || gi_mode_ != Mode::kOff) && mode_ != Mode::kOff && EnsureGiShaders();
  const bool gi_test = test && gi_active_;
  if (gi_test != gi_test_) {
    gi_test_ = gi_test;
    REXLOG_INFO("[gpu] ssgi: the bounce light test is {} (mcla_gpu_ssgi_test, live; it needs "
                "mcla_gpu_ao on)",
                gi_test ? "on" : "off");
  }
}

uint64_t AmbientOcclusion::HeldBytes() const {
  uint64_t bytes = 0;
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  for (const HostImage& image : depth_) {
    add(image);
  }
  add(raw_);
  add(denoised_);
  for (const HostImage& image : history_) {
    add(image);
  }
  add(weight_);
  for (const HostImage& image : colour_) {
    add(image);
  }
  add(gi_);
  add(gi_dn_);
  for (const HostImage& image : gi_hist_) {
    add(image);
  }
  add(depth_prev_);
  add(gi_weight_);
  add(vehicle_table_);
  return bytes;
}

void AmbientOcclusion::ReleaseImages() {
  if (!passes_) {
    return;
  }
  for (HostImage& image : depth_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(raw_);
  passes_->ReleaseImage(denoised_);
  for (HostImage& image : history_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(weight_);
  ReleaseGiImages();
  passes_->ReleaseImage(vehicle_table_);
  history_valid_ = false;
  history_cut_ = false;
  ++generation_;
}

// ---------------------------------------------------------------------------
// what the draws tell
// ---------------------------------------------------------------------------

void AmbientOcclusion::OnMainDraw(const MainDraw& draw, const ConstantBanks::Banks& banks) {
  frame_had_main_view_ = true;
  if (draw.match.samples == temporal::kPauseSamples) {
    ++view4x_draws_;  // the pause menu's, the race pause's and the map's world
    pause_view_ = true;
  } else {
    play_view_ = true;
  }
  if (anchor_seen_ || composite_seen_) {
    ++draws_at_anchor_;
    return;
  }
  // The vehicle IDs the main view's draws write before the anchor (the anchor's constants hold the
  // motion of those only).
  stencil_ids_ |= draw.stencil_ids;
  if (qualifying_resolves_ != 0) {
    ++draws_after_;
    if (draw.match.depth && draw.writes_depth) {
      ++depth_writes_after_;
    }
    return;
  }
  ++draws_before_;
  if (!draw.match.color) {
    return;
  }
  ++color_draws_before_;
  // The registers the banks of this draw hold (a draw whose shaders do not use them leaves what an
  // earlier one set: the value most colour draws carry is the frame's).
  LightRaw light;
  const uint8_t* ps = banks.ps.data();
  const uint8_t* vs = banks.vs.data();
  // The wetness, for the contact shadows' gate: counted in bins of 0.1 (PickWet).
  if (contact_mode_ != Mode::kOff) {
    const float wet = BankFloat(vs, kVsReflectionParams, 0);
    if (std::isfinite(wet) && wet >= 0.0f && wet <= 4.0f) {
      ++wet_bins_[uint32_t(wet * 10.0f + 0.5f)];
      ++wet_draws_;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    light.dwords[i] = LoadBe32(ps + kPsAmbient * 16 + i * 4);
    light.dwords[3 + i] = LoadBe32(ps + kPsSunColor * 16 + i * 4);
    light.dwords[6 + i] = LoadBe32(vs + kVsSunDirection * 16 + i * 4);
  }
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      vs_view_inverse_[r * 4 + c] = BankFloat(vs, 12 + r, c);
    }
  }
  ++vs_view_inverse_draws_;
  if (light_draws_ == 0) {
    first_light_ = light;
  }
  last_light_ = light;
  ++light_draws_;
  for (uint32_t i = 0; i < light_count_; ++i) {
    if (lights_[i].raw == light) {
      ++lights_[i].draws;
      return;
    }
  }
  if (light_count_ < kLightSlots) {
    lights_[light_count_].raw = light;
    lights_[light_count_].draws = 1;
    ++light_count_;
  } else {
    ++light_overflow_;
  }
}

bool AmbientOcclusion::PickLight(LightRaw* out, uint32_t* draws_with_it) const {
  if (light_draws_ == 0) {
    return false;
  }
  if (light_count_ == 0) {
    *out = last_light_;
    *draws_with_it = 0;
    return true;
  }
  uint32_t best = 0;
  for (uint32_t i = 1; i < light_count_; ++i) {
    if (lights_[i].draws >= lights_[best].draws) {
      best = i;
    }
  }
  *out = lights_[best].raw;
  *draws_with_it = lights_[best].draws;
  return true;
}

void AmbientOcclusion::OnDepthClear(const EdramKey& key, float z) {
  if (temporal::MainDepthSamples(key) == 0) {
    return;
  }
  ++depth_clears_;
  depth_clear_z_ = z;
}

void AmbientOcclusion::OnAnchor(const ConstantBanks::Banks& banks) {
  anchor_seen_ = true;
  have_camera_ = false;
  vehicle_motion_.valid = 0;  // read again below, once this frame gives a camera
  lens_step_valid_ = false;
  vs_compare_valid_ = false;
  // The play view's camera and the pause/map world's (4x samples) are kept apart: the lens differs
  // (a field of view of 25 to 54 degrees), and the anchor draw runs in the 4x frames too with a
  // valid camera (918 of 918 frames in a recorded pause and map session). A frame with draws of
  // both kinds or of neither (the anchor without a main view) gives no camera; the frame after it
  // has none to use.
  if (play_view_ == pause_view_) {
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  const bool pause = pause_view_;
  const uint8_t* ps = banks.ps.data();
  // The fetch size the camera test compares c62 against is c62 itself here: only the temporal
  // system knows the anchor's fetches, and a frame size that is not the main view's is caught by
  // the rectangle of the trigger (ao.cpp Judge).
  const float width = BankFloat(ps, 62, 0);
  const float height = BankFloat(ps, 62, 1);
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    reject_ = temporal::Reject::kFrameSize;
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    return;
  }
  temporal::Camera camera;
  reject_ = temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_,
                                    &camera);
  if (reject_ != temporal::Reject::kNone) {
    have_previous_ = false;
    have_last_step_ = false;
    vs_pose_trusted_ = false;
    if (rejects_logged_ < 8) {
      ++rejects_logged_;
      REXLOG_INFO("[gpu] ao: the anchor's constants gave no camera: {} (frame {}, logged {} of 8)",
                  temporal::RejectName(reject_), frame_, rejects_logged_);
    }
    return;
  }
  // The lens the previous frame's camera gave the trigger, against this frame's own (the trigger
  // works with the previous frame's lens, which is this frame's outside cuts).
  lens_delta_valid_ = have_previous_ && previous_frame_ + 1 == frame_ && previous_pause_ == pause;
  if (lens_delta_valid_) {
    const auto rel = [](double a, double b) {
      return std::fabs(a - b) / std::max(1e-30, std::fabs(b));
    };
    lens_delta_[0] = rel(camera.fov_y, previous_.fov_y);
    lens_delta_[1] = rel(camera.aspect, previous_.aspect);
    lens_delta_[2] = rel(camera.near_plane, previous_.near_plane);
    lens_delta_[3] = rel(camera.far_plane, previous_.far_plane);
    double dx = 0.0;
    double dot = 0.0;
    for (int i = 0; i < 3; ++i) {
      const double d = camera.position[i] - previous_.position[i];
      dx += d * d;
      dot += camera.forward[i] * previous_.forward[i];
    }
    lens_delta_[4] = std::sqrt(dx);
    lens_delta_[5] = dot;
    // The next frame's trigger uses this camera's lens: how fast the lens was moving is the best
    // guess of how far it is from that frame's own (a pause/map view only: see OnResolve).
    lens_step_ = std::max({lens_delta_[0], lens_delta_[1], lens_delta_[2], lens_delta_[3]});
    lens_step_valid_ = std::isfinite(lens_step_);
  }
  // The camera's motion from the frame before to this one, for the occlusion's history in the next
  // frame (FrameMotion): that frame is guessed to move the same way. A cut starts the history
  // again, and the step of a cut is not a motion to repeat.
  if (lens_delta_valid_) {
    const temporal::Cut cut =
        temporal::TestCut(camera, previous_, have_last_step_ ? last_step_ : nullptr);
    if (cut.fires) {
      history_valid_ = false;
      history_cut_ = true;
      have_last_step_ = false;
      last_motion_valid_ = false;
    } else {
      last_motion_ = temporal::Mul(previous_.view_inverse, camera.view);
      last_motion_valid_ = temporal::Finite(last_motion_);
      last_motion_frame_ = frame_;
      for (int i = 0; i < 3; ++i) {
        last_step_[i] = cut.step[i];
      }
      have_last_step_ = true;
    }
    // Whether the vertex constants of this frame's draws held this frame's view inverse (what the
    // census compares below): trusted once they did in a frame the camera moved in, dropped as
    // soon as they differ.
    if (vs_view_inverse_draws_ != 0) {
      temporal::Mat4 vertex;
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          vertex.m[r][c] = double(vs_view_inverse_[r * 4 + c]);
        }
      }
      const double differs = temporal::MaxAbsDiff(vertex, camera.view_inverse);
      const double moved = temporal::MaxAbsDiff(camera.view_inverse, previous_.view_inverse);
      if (!(differs <= kVsMatch)) {
        vs_pose_trusted_ = false;
      } else if (moved >= kVsMoved) {
        vs_pose_trusted_ = true;
      }
    }
  } else {
    have_last_step_ = false;
    last_motion_valid_ = false;
    vs_pose_trusted_ = false;
  }
  // The vehicles' own motion over this frame, from the game's matrices in these constants, for the
  // next frame's history. The pause/map world has nothing that moves by itself: its vehicle pixels
  // take the camera's motion.
  if (!pause) {
    NoteVehicles(ps);
  }
  camera_ = camera;
  have_camera_ = true;
  previous_ = camera;
  previous_pause_ = pause;
  have_previous_ = true;
  previous_frame_ = frame_;

  // The main view's vertex bank against the camera just read, for the census. If c12-c15 of a
  // vertex bank are the view inverse, the frame's own lens exists before the anchor.
  if (vs_view_inverse_draws_ != 0) {
    double worst = 0.0;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        const double d =
            std::fabs(double(vs_view_inverse_[r * 4 + c]) - camera.view_inverse.m[r][c]);
        worst = std::isfinite(d) ? std::max(worst, d) : HUGE_VAL;
      }
    }
    vs_compare_valid_ = true;
    vs_compare_diff_ = worst;
    ++vs_compared_[pause ? 1 : 0];
    if (worst <= 1e-4) {
      ++vs_equal_[pause ? 1 : 0];
    }
  }
}

// ---------------------------------------------------------------------------
// the trigger
// ---------------------------------------------------------------------------

uint32_t AmbientOcclusion::Judge(const ResolveRecord& record,
                                 const ResolveCache::Summary& summary) const {
  if (!summary.depth_copy) {
    return kRefusedNotDepthCopy;
  }
  uint32_t refused = 0;
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  if (samples == 0) {
    refused |= kRefusedNotMainDepth;
  }
  const bool pause = samples == temporal::kPauseSamples;
  if (record.flags & kEventInsideEndTiling) {
    refused |= kRefusedEndTiling;
  }
  // The game resolves the picture in tiles. The first is the trigger, which covers the whole
  // picture (the frame is drawn once: the depth target is complete); the others of the same
  // picture are not triggers, whatever their rectangle.
  const bool later_tile = IsLaterTile(summary);
  if (later_tile) {
    refused |= kRefusedLaterTile;
  } else if (qualifying_resolves_ != 0) {
    refused |= kRefusedNotFirst;
  }
  if (anchor_seen_ || composite_seen_) {
    refused |= kRefusedAfterAnchor;
  }
  if (summary.dest_address != kTriggerDestination) {
    refused |= kRefusedDestination;
  }
  if (summary.clears_depth) {
    refused |= kRefusedClearsDepth;
  }
  if (!later_tile) {
    uint32_t width = 0;
    uint32_t height = 0;
    PictureSize(summary, &width, &height);
    // The play view's first tile starts at the origin (the rows 0-512 of two). The pause/map
    // view's three tiles come in an order nobody has looked at: any rectangle inside the picture
    // will do for it, the occlusion covers the whole picture whichever tile triggers.
    const bool origin = summary.x0 == 0 && summary.y0 == 0;
    if ((!pause && !origin) || summary.x0 < 0 || summary.y0 < 0 || summary.x1 <= summary.x0 ||
        summary.y1 <= summary.y0 || uint32_t(summary.x1) > width || uint32_t(summary.y1) > height) {
      refused |= kRefusedRectangle;
    }
    // The pause/map view's destination must be the whole picture (1280x720 within the 1280x736
    // target), not a tile's own texture: the passes cover what the destination's size says.
    // (mcla_gpu_ao_census prints every depth copy.)
    if (pause && summary.source &&
        (width != summary.source->width ||
         height + EdramTargets::kHeightAlignment <= summary.source->height)) {
      refused |= kRefusedNotWholePicture;
    }
  }
  return refused;
}

void AmbientOcclusion::PictureSize(const ResolveCache::Summary& summary, uint32_t* width,
                                   uint32_t* height) {
  *width = summary.dest_width;
  *height = summary.dest_height;
  if (summary.source) {
    *width = std::min(*width, summary.source->width);
    *height = std::min(*height, summary.source->height);
  }
}

bool AmbientOcclusion::IsLaterTile(const ResolveCache::Summary& summary) const {
  // The same picture (source, destination texture) as the frame's first qualifying resolve, and a
  // rectangle of its own: the same rectangle again is a second copy, not a tile.
  return qualifying_resolves_ != 0 && summary.source_key == trigger_source_ &&
         summary.dest_address == trigger_destination_ && summary.dest_format == trigger_format_ &&
         summary.dest_width == trigger_size_[0] && summary.dest_height == trigger_size_[1] &&
         (summary.x0 != trigger_rect_[0] || summary.y0 != trigger_rect_[1] ||
          summary.x1 != trigger_rect_[2] || summary.y1 != trigger_rect_[3]);
}

bool AmbientOcclusion::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary,
                                 plume::RenderCommandList* list, bool upscaler_running) {
  if (contact_mode_ != Mode::kOff || gi_active_) {
    NoteMaskResolve(record);  // the bounce light reads the sun's shadow mask too (the albedo estimate)
  }
  if (!summary.depth_copy) {
    ++other_resolves_;
    return false;
  }
  const uint32_t refused = Judge(record, summary);
  const bool qualifies = (refused & ~uint32_t(kRefusedNotFirst)) == 0;
  const bool pause = temporal::MainDepthSamples(summary.source_key) == temporal::kPauseSamples;
  if (census_every_ != 0 && depth_resolves_.size() < 16) {
    DepthResolveNote note;
    note.caller = record.caller;
    note.flags = record.flags;
    note.source = summary.source_key;
    note.dest_address = summary.dest_address;
    note.dest_format = summary.dest_format;
    note.dest_width = summary.dest_width;
    note.dest_height = summary.dest_height;
    note.rect[0] = summary.x0;
    note.rect[1] = summary.y0;
    note.rect[2] = summary.x1;
    note.rect[3] = summary.y1;
    note.host_sample = summary.host_sample;
    note.host_samples = summary.source ? summary.source->host_samples : 0;
    note.inside_end_tiling = (record.flags & kEventInsideEndTiling) != 0;
    note.clears_depth = summary.clears_depth;
    note.refused = refused;
    note.draws_before = draws_before_ + draws_after_;
    note.anchor_before = anchor_seen_;
    depth_resolves_.push_back(note);
  }
  // A pause/map view's copy of the main depth that fails only on its destination: a build that
  // copies it elsewhere has no occlusion in that view, and the log says so once.
  if (pause && qualifying_resolves_ == 0 && refused == kRefusedDestination &&
      !pause_destination_logged_) {
    pause_destination_logged_ = true;
    REXLOG_INFO("[gpu] ao: a pause/map frame copies its main depth into {:08x} (format {}, {}x{}), "
                "not into {:08x} where the play view's mid-scene copy goes: no occlusion in that "
                "view (reported once; mcla_gpu_ao_census prints every copy)",
                summary.dest_address, summary.dest_format, summary.dest_width, summary.dest_height,
                kTriggerDestination);
  }
  if (qualifies) {
    if (qualifying_resolves_ == 0) {
      trigger_source_ = summary.source_key;
      trigger_destination_ = summary.dest_address;
      trigger_format_ = summary.dest_format;
      trigger_size_[0] = summary.dest_width;
      trigger_size_[1] = summary.dest_height;
      trigger_rect_[0] = summary.x0;
      trigger_rect_[1] = summary.y0;
      trigger_rect_[2] = summary.x1;
      trigger_rect_[3] = summary.y1;
    }
    ++qualifying_resolves_;
  } else if (refused & kRefusedLaterTile) {
    ++later_tiles_;
  }
  if (refused != 0 || !list || !ready_ || !summary.source) {
    return false;
  }

  // The trigger. The depth chain also runs, with the occlusion off, at the census's frames: its
  // statistic shows whether the depth convention holds (1 near, 0 far).
  trigger_pause_ = pause;
  trigger_camera_valid_ = false;
  outcome_ = Outcome::kIdle;
  Plan plan;
  plan.mode = mode_;
  plan.occlusion = mode_ != Mode::kOff;
  PlanContact(&plan);
  const bool contact = plan.contact.mode != Mode::kOff;
  plan.probe = census_every_ != 0 && (probes_done_ < 3 || frame_ % census_every_ == 0);
  if (!plan.occlusion && !contact) {
    if (!plan.probe) {
      return false;
    }
    plan.mode = Mode::kLow;  // the cheaper size is enough for a statistic
    plan.apply = false;
  }
  plan.upscaler = upscaler_running;
  plan.view = plan.occlusion ? uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ao_view), 0, 6)) : 0u;
  if (contact) {
    const uint32_t contact_view = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_view), 0, 4));
    if (contact_view != 0) {
      plan.view = kContactViewBase + contact_view;  // instead of the occlusion's
    }
  }
  PlanGi(&plan);
  // The passes run at half the picture's size when nothing that is on asks for the whole of it (the bounce
  // light's high does, as the occlusion's).
  plan.low = !(plan.occlusion && plan.mode == Mode::kHigh) &&
             !(contact && plan.contact.mode == Mode::kHigh) &&
             !(plan.gi.on && plan.gi.mode == Mode::kHigh);
  if (plan.gi.on && plan.gi.half && !(contact && plan.contact.mode == Mode::kHigh)) {
    plan.low = true;  // the bounce light's low at half the size, whatever the occlusion's own quality
  }
  // The lens is the previous frame's (the camera exists at the anchor, after this), of the same
  // kind of view: none, no occlusion this frame (a cut's first frame, the first frame after
  // turning it on, the first frame of a pause spell: the play view's lens is not the pause
  // camera's).
  if (!have_previous_ || previous_frame_ + 1 != frame_ || previous_pause_ != pause) {
    ++skipped_no_lens_;
    outcome_ = Outcome::kNoCamera;
    if (pause) {
      ++pause_skipped_no_lens_;
      if (!no_lens_pause_logged_) {
        no_lens_pause_logged_ = true;
        REXLOG_INFO("[gpu] ao: no occlusion in a pause/map frame that has no previous pause/map "
                    "frame's camera (the first frame of a pause spell, a frame after one whose "
                    "anchor draw gave no camera); counted, logged once");
      }
    } else if (!no_lens_logged_) {
      no_lens_logged_ = true;
      REXLOG_INFO("[gpu] ao: no occlusion in a frame that has no previous frame's camera (the "
                  "first frame, a cut, a frame whose anchor draw gave no camera); counted, "
                  "logged once");
    }
    return false;
  }
  // A pause/map view whose lens was changing fast between the last two frames (the pause camera's
  // swing when the menu opens, the map's zoom), or whose rate of change is not known yet (the
  // second frame of a spell, the second after the setting was turned on): this frame's lens may be
  // as far from the previous one's, and a wrong lens is worse than none for a frame, so no
  // occlusion until it is known to be steady. The play view keeps its rule: its lens moves by
  // about 1e-4 a frame.
  trigger_lens_step_ = lens_step_valid_ ? lens_step_ : 0.0;
  if (pause && (!lens_step_valid_ || lens_step_ > temporal::kCutFov)) {
    ++pause_skipped_lens_;
    outcome_ = lens_step_valid_ ? Outcome::kLensChanging : Outcome::kLensUnknown;
    if (!lens_step_logged_) {
      lens_step_logged_ = true;
      REXLOG_INFO("[gpu] ao: no occlusion in a pause/map frame whose lens is not known to be "
                  "steady (the second frame of a spell, or a change of more than {:.0f}% between "
                  "the last two frames: the pause camera's swing, the map); counted, logged once",
                  100.0 * temporal::kCutFov);
    }
    return false;
  }
  trigger_camera_ = previous_;
  trigger_camera_valid_ = true;
  // The whole picture, not the tile this resolve copies (the game resolves it in two: the rows
  // 0-512 and 512-720), at the source's own host scale (the render scale, an upscaler's render
  // size): the target is complete now, so the occlusion covers all of it and the lens's field of
  // view spans the rows it was made for.
  uint32_t picture_width = 0;
  uint32_t picture_height = 0;
  PictureSize(summary, &picture_width, &picture_height);
  const Scale scale = summary.source->scale;
  plan.render[0] = scale.Edge(picture_width);
  plan.render[1] = scale.Edge(picture_height);
  if (plan.render[0] < kMinRenderSize || plan.render[1] < kMinRenderSize) {
    outcome_ = Outcome::kTooSmall;
    return false;
  }
  const bool low = plan.low;
  for (uint32_t axis = 0; axis < 2; ++axis) {
    plan.ao[axis] = low ? (plan.render[axis] + 1) / 2 : plan.render[axis];
    plan.level[0][axis] = (plan.ao[axis] + 1) / 2;
    plan.level[1][axis] = (plan.ao[axis] + 3) / 4;
  }
  const bool recorded = Run(summary, list, plan);
  outcome_ = recorded ? Outcome::kRecorded : Outcome::kNotRecorded;
  return recorded;
}

// ---------------------------------------------------------------------------
// the passes
// ---------------------------------------------------------------------------

bool AmbientOcclusion::EnsureImages(const Plan& plan) {
  // The coarser depth levels are the occlusion's (and the census probe's); the contact shadows read
  // level 0 only. The two scratch images hold the occlusion's visibility passes and, after those,
  // the contact shadows'; the history is the occlusion's alone.
  const bool levels = plan.occlusion || plan.probe;
  const bool scratch = plan.apply;
  bool ok = passes_->EnsureImage(depth_[0], plan.ao[0], plan.ao[1], plume::RenderFormat::R32_FLOAT,
                                 "ao depth");
  if (ok && levels) {
    ok = passes_->EnsureImage(depth_[1], plan.level[0][0], plan.level[0][1],
                              plume::RenderFormat::R16_FLOAT, "ao depth 1/2") &&
         passes_->EnsureImage(depth_[2], plan.level[1][0], plan.level[1][1],
                              plume::RenderFormat::R16_FLOAT, "ao depth 1/4");
  }
  if (ok && scratch) {
    ok = passes_->EnsureImage(raw_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16_FLOAT,
                              "ao visibility") &&
         passes_->EnsureImage(denoised_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16_FLOAT,
                              "ao denoised visibility");
  }
  // The bounce light test keeps its own history (RGBA16F, EnsureGiImages) and the occlusion's is not made
  // while it runs: a frame that turns out not to have the test asks again with the test off (Run).
  if (ok && plan.occlusion && !plan.gi.on) {
    ok = passes_->EnsureImage(history_[0], plan.ao[0], plan.ao[1],
                              plume::RenderFormat::R16G16_FLOAT, "ao history 0") &&
         passes_->EnsureImage(history_[1], plan.ao[0], plan.ao[1],
                              plume::RenderFormat::R16G16_FLOAT, "ao history 1");
  }
  // The picture of debug view 6, made only while that view is shown.
  if (ok && plan.occlusion && !plan.gi.on && plan.view == 6u) {
    ok = passes_->EnsureImage(weight_, plan.ao[0], plan.ao[1], plume::RenderFormat::R16G16_FLOAT,
                              "ao history weight");
  } else if (weight_.texture) {
    passes_->ReleaseImage(weight_);
  }
  if (ok && (images_made_for_[0] != plan.ao[0] || images_made_for_[1] != plan.ao[1])) {
    images_made_for_[0] = plan.ao[0];
    images_made_for_[1] = plan.ao[1];
    const char* what = plan.apply ? (plan.occlusion ? ModeName(plan.mode) : "contact shadows only")
                                  : "census probe only";
    REXLOG_INFO("[gpu] ao: images at {}x{} for a {}x{} picture ({}): linear depth R32_FLOAT{}{}{}, "
                "{:.1f} MiB",
                plan.ao[0], plan.ao[1], plan.render[0], plan.render[1], what,
                levels ? " with two coarser levels" : "",
                scratch ? ", visibility R16_FLOAT x2" : "",
                plan.occlusion && !plan.gi.on ? ", history R16G16_FLOAT x2" : "",
                double(HeldBytes()) / (1024.0 * 1024.0));
  }
  return ok;
}

void AmbientOcclusion::Bind(plume::RenderCommandList* list) const {
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
}

void AmbientOcclusion::DrawPass(plume::RenderCommandList* list, HostImage& image,
                                const plume::RenderPipeline* pipeline, const Push& push) {
  // The same sequence as HostPasses::DrawInto, with this pass's own layout: 128 bytes of push
  // constants for the vertex and the pixel stage on both backends (the temporal motion pass uses
  // 80, the host passes' layout 48; Vulkan guarantees 128), and the host layout's texture sets and
  // sampler set bound again by Bind.
  const bool first_use = image.layout == plume::RenderTextureLayout::UNKNOWN;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  if (first_use) {
    // A new colour target is initialised before it is drawn into (D3D12 debug layer id 1422).
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(image.width), float(image.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(image.width), int32_t(image.height));
  list->setScissors(&scissor, 1);
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

void AmbientOcclusion::UpdateLook() {
  const uint32_t index = ReadLookIndex();
  const LookDef& def = kLooks[index];
  look_.strength = def.strength;
  look_.radius = def.radius;
  look_.power = def.power;
  look_.direct = def.direct;
  if (index != look_logged_index_) {
    look_logged_index_ = index;
    REXLOG_INFO("[gpu] ao: intensity {} (mcla_gpu_ao_intensity, live): strength {:.3g}, radius {:.3g} "
                "m, power {:.3g}, direct {:.3g}",
                def.name, look_.strength, look_.radius, look_.power, look_.direct);
  }
}

void AmbientOcclusion::FillPush(Push* push, const ResolveCache::Summary& summary, const Plan& plan,
                                const LightRaw* light) const {
  std::memset(push, 0, sizeof(*push));
  push->slots[0] = summary.sample_slot;
  push->slots[1] = summary.host_sample;
  // The occlusion's own quality picks the slices and steps; the images are at half the picture's size
  // (the flag the shaders read) when nothing that is on asks for more (Plan::low).
  // (the bounce light's own quality when it runs: its main pass is the occlusion's)
  const bool ao_low = plan.gi.on ? plan.gi.fewer : plan.mode == Mode::kLow;
  const bool low = plan.low;
  const uint32_t slices = ao_low ? 2 : 3;
  const uint32_t steps = ao_low ? 2 : 3;
  // The noise changes every frame (hlsl/ao_main_ps.hlsl); the denoise is a 3x3 each time.
  const uint32_t denoise_radius = 1;
  push->slots2[1] = uint32_t(frame_ & 1023);
  push->slots2[2] = plan.view | (slices << 8) | (steps << 12) | ((low ? 1u : 0u) << 16) |
                    (denoise_radius << 17);

  // The lens: the previous frame's projection (row vectors, view space to clip, the camera looking
  // down -z): tan of the half field of view from its x and y scales, the depth rows for the
  // linearisation (t = (P32 - ndc P33) / (P22 - ndc P23), ndc = 1 - stored depth).
  const temporal::Mat4& p = previous_.projection;
  const float radius = look_.radius * (plan.gi.on ? plan.gi.radius : 1.0f);
  const float fade_end = float(std::clamp(REXCVAR_GET(mcla_gpu_ao_fade_end), 20.0, 1000.0));
  push->lens[0] = float(1.0 / std::max(std::fabs(p.m[0][0]), 1e-9));
  push->lens[1] = float(1.0 / std::max(std::fabs(p.m[1][1]), 1e-9));
  push->lens[2] = radius;
  push->lens[3] = kFalloffRatio;
  push->lin[0] = float(p.m[2][2]);
  push->lin[1] = float(p.m[2][3]);
  push->lin[2] = float(p.m[3][2]);
  push->lin[3] = float(p.m[3][3]);
  push->shape[0] = look_.power;
  push->shape[1] = kMinVisibility;
  push->shape[2] = look_.strength;
  push->shape[3] = 0.5f * fade_end;
  push->extra[1] = fade_end;
  push->extra[2] = look_.direct;

  // The light: the ambient and the sun as lumas, and the direction to the sun in this renderer's
  // view space (x right, y up, z forward). The game's view space looks along -z, so z changes
  // sign. c16 is the direction the light travels (at noon y is -0.867: the sun is overhead), so the
  // direction to the sun is -c16. No light constants or no direction: no sun, the whole light is
  // ambient.
  if (light) {
    float ambient[3], sun[3], dir[3];
    DecodeLight(*light, ambient, sun, dir);
    push->light[3] = Luma(ambient);
    push->extra[0] = Luma(sun);
    double view[3] = {};
    for (int j = 0; j < 3; ++j) {
      for (int i = 0; i < 3; ++i) {
        view[j] += double(std::isfinite(dir[i]) ? dir[i] : 0.0f) * previous_.view.m[i][j];
      }
    }
    view[2] = -view[2];
    const double length = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
    if (std::isfinite(length) && length > 1e-6) {
      for (int i = 0; i < 3; ++i) {
        push->light[i] = float(-view[i] / length);
      }
    }
  }
}

const char* AmbientOcclusion::HistoryWhyName(HistoryWhy why) {
  switch (why) {
    case HistoryWhy::kUsed: return "used";
    case HistoryWhy::kFirst:
      return "none yet (the first frame, the occlusion just turned on)";
    case HistoryWhy::kGap: return "not the frame after the one that made it";
    case HistoryWhy::kView: return "the other kind of view (play against pause/map)";
    case HistoryWhy::kSize: return "another image size";
    case HistoryWhy::kKind: return "the history of the other kind (the bounce light test switched)";
    case HistoryWhy::kCut: return "a cut of the camera";
    case HistoryWhy::kCap: return "mcla_gpu_ao_history is 1";
    case HistoryWhy::kCount: break;
  }
  return "?";
}

const char* AmbientOcclusion::PoseName(Pose pose) {
  switch (pose) {
    case Pose::kStill: return "standing still (no step measured)";
    case Pose::kPredicted: return "the last step repeated";
    case Pose::kVertexBank: return "the frame's own view inverse from the vertex constants";
  }
  return "?";
}

bool AmbientOcclusion::FrameMotion(temporal::Mat4* motion, Pose* source, bool* cut) const {
  *cut = false;
  // The frame's own camera, from the vertex constants of the draws before the trigger, once it is
  // known that they hold it (OnAnchor): the motion is exact then, as in any renderer with a
  // camera of its own. v_history = v_world * V_history, v_world = v_now * W_now.
  if (vs_pose_trusted_ && vs_view_inverse_draws_ != 0) {
    temporal::Mat4 world;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        world.m[r][c] = double(vs_view_inverse_[r * 4 + c]);
      }
    }
    if (PoseSane(world)) {
      *motion = temporal::Mul(world, previous_.view);
      *source = Pose::kVertexBank;
      temporal::Camera now;
      for (int i = 0; i < 3; ++i) {
        now.position[i] = world.m[3][i];
        now.forward[i] = -world.m[2][i];
      }
      now.fov_y = previous_.fov_y;
      *cut = temporal::TestCut(now, previous_, have_last_step_ ? last_step_ : nullptr).fires;
      return temporal::Finite(*motion);
    }
  }
  // The last step repeated: T took the view space of the frame before the previous one to the
  // previous one's, and the frame now is guessed to take the previous one's to its own, so what
  // moves a point of this frame's view space back into the previous one's is the inverse of T.
  if (last_motion_valid_ && last_motion_frame_ == previous_frame_) {
    temporal::Mat4 inverse;
    if (temporal::Inverse(last_motion_, &inverse)) {
      *motion = inverse;
      *source = Pose::kPredicted;
      return true;
    }
  }
  *motion = temporal::Identity();
  *source = Pose::kStill;
  return true;
}

AmbientOcclusion::HistoryUse AmbientOcclusion::ChooseHistory(const Plan& plan) const {
  HistoryUse use;
  use.cap = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ao_history), 1, 64));
  use.motion = temporal::Identity();
  if (use.cap < 2) {
    use.why = HistoryWhy::kCap;
  } else if (!history_valid_) {
    use.why = history_cut_ ? HistoryWhy::kCut : HistoryWhy::kFirst;
  } else if (history_frame_ + 1 != frame_) {
    use.why = HistoryWhy::kGap;
  } else if (history_pause_ != trigger_pause_) {
    use.why = HistoryWhy::kView;
  } else if (history_size_[0] != plan.ao[0] || history_size_[1] != plan.ao[1]) {
    use.why = HistoryWhy::kSize;
  } else if (history_gi_ != plan.gi.on) {
    use.why = HistoryWhy::kKind;
  } else {
    bool cut = false;
    if (!FrameMotion(&use.motion, &use.pose, &cut)) {
      use.why = HistoryWhy::kFirst;
      use.pose = Pose::kStill;
      use.motion = temporal::Identity();
    } else if (cut) {
      use.why = HistoryWhy::kCut;
    } else {
      use.on = true;
      use.why = HistoryWhy::kUsed;
      use.age = history_age_;
    }
  }
  return use;
}

void AmbientOcclusion::NoteHistory(const HistoryUse& use) {
  history_why_ = use.why;
  history_pose_ = use.pose;
  if (use.on) {
    ++history_used_;
    ++pose_frames_[size_t(use.pose)];
    if (!history_logged_) {
      history_logged_ = true;
      REXLOG_INFO("[gpu] ao: the first frame that averages over a history: {} frames at most "
                  "(mcla_gpu_ao_history), the camera's motion into the last frame from {}; one "
                  "denoise pass instead of two, the wide blur only where the history does not "
                  "hold (reported once)",
                  use.cap, PoseName(use.pose));
    }
  } else {
    ++history_resets_[size_t(use.why)];
  }
}

void AmbientOcclusion::NoteVehicles(const uint8_t* ps_bank) {
  const temporal::VehicleStats s =
      temporal::ReadVehicleMotion(ps_bank, stencil_ids_, &vehicle_motion_);
  vehicle_motion_frame_ = frame_;
  VehicleCounts& v = vehicles_;
  ++v.anchors;
  if (s.asked != 0) {
    ++v.anchors_with_ids;
    v.ids_asked += s.asked;
    v.ids_taken += s.taken;
    v.not_sane += s.not_sane;
    v.not_rigid += s.not_rigid;
    v.max_asked = std::max(v.max_asked, s.asked);
  }
  v.no_world += s.no_world ? 1 : 0;
}

uint32_t AmbientOcclusion::RecordVehicleTable(plume::RenderCommandList* list, const HistoryUse& use,
                                              uint32_t* slot) {
  *slot = kNoSlot;
  // The pause/map view's vehicles take the camera's motion (no table); a motion that is not the last
  // frame's anchor's (a frame with no camera in between) is not used.
  if (!use.on || trigger_pause_ || vehicle_motion_.valid == 0 ||
      vehicle_motion_frame_ + 1 != frame_) {
    return 0;
  }
  plume::RenderPipeline* pipeline = Pipeline(kVVehicleTable);
  if (!pipeline ||
      !passes_->EnsureImage(vehicle_table_, kVehicleTableWidth, temporal::kVehicleIds,
                            kVehicleTableFormat, "ao vehicle table")) {
    return 0;
  }
  // For a point of vehicle k, this frame's game view space into the last frame's: the camera's motion
  // `use.motion` is v_now W_now V_prev for the world; the vehicle's own step T_k takes this frame's world
  // position of the point to its position in the last frame (vehicle_motion.h), so the point goes
  // v_now W_now T_k V_prev = use.motion * W_prev * T_k * V_prev (V_prev W_prev is the identity: previous_
  // is the camera the history was made with). Then into this renderer's view space (x right, y up, z
  // forward: S M S with S = diag(1, 1, -1, 1), as the camera's rows of the push block), three columns of
  // four floats the shader takes as rows.
  // CHECK: the direction of T_k (this frame's world to the last frame's, as BuildVehicleTable uses it
  // between clip_now and clip_prev) and the repeat of last frame's step for this frame; a car that
  // drives away from its own history in debug view 6 (black on a moving car) says one of them is wrong.
  const temporal::Mat4 to_world = temporal::Mul(use.motion, previous_.view_inverse);
  struct Row {
    uint32_t id;
    float columns[12];
  };
  static_assert(sizeof(Row::columns) == 48);  // the host pass layout's push range is 64 bytes
  Row rows[temporal::kVehicleIds];
  uint32_t count = 0;
  uint32_t mask = 0;
  for (uint32_t k = 1; k < temporal::kVehicleIds; ++k) {
    if (!(vehicle_motion_.valid & (1u << k))) {
      continue;
    }
    const temporal::Mat4 m =
        temporal::Mul(temporal::Mul(to_world, vehicle_motion_.world[k]), previous_.view);
    if (!temporal::Finite(m)) {
      continue;
    }
    Row& row = rows[count++];
    row.id = k;
    for (int c = 0; c < 3; ++c) {
      for (int r = 0; r < 4; ++r) {
        const double sign = ((r == 2) != (c == 2)) ? -1.0 : 1.0;
        row.columns[c * 4 + r] = float(sign * m.m[r][c]);
      }
    }
    mask |= 1u << k;
  }
  if (count == 0) {
    return 0;
  }
  // One draw a vehicle over the four texels of its row (hlsl/temporal_vehicle_table_ps.hlsl); the
  // image is cleared first, so the row of an ID nobody drew reads as not usable.
  // CHECK: the same sequence as TemporalMotion::RecordVehicleTable (host pass layout, 4 x 1 viewports
  // and scissors, a clear every frame) in this frame's list between two of the occlusion's own passes.
  HostImage& image = vehicle_table_;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  passes_->Bind(list);
  list->setPipeline(pipeline);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = rows[i].id;
    const plume::RenderViewport viewport{0.0f, float(id), float(kVehicleTableWidth), 1.0f};
    list->setViewports(&viewport, 1);
    const plume::RenderRect scissor(0, int32_t(id), int32_t(kVehicleTableWidth), int32_t(id + 1));
    list->setScissors(&scissor, 1);
    list->setGraphicsPushConstants(0, rows[i].columns, 0, sizeof(rows[i].columns));
    list->drawInstanced(3, 1, 0, 0);
  }
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
  *slot = image.slot;
  return mask;
}

void AmbientOcclusion::LogVehicleSummary() {
  const VehicleCounts& v = vehicles_;
  if (v.anchors_with_ids == 0 && v.frames_with_ids == 0 && v.pause_with_ids == 0) {
    return;  // no vehicle seen yet
  }
  const uint64_t seen = v.history_frames + v.pause_frames;
  if (seen == vehicles_logged_at_) {
    return;
  }
  vehicles_logged_at_ = seen;
  REXLOG_INFO("[gpu] ao: vehicle history, totals so far: of {} play frames that averaged over a history, "
              "{} had vehicle IDs in the stencil; {} of those {} IDs had a motion row ({:.1f}%), a "
              "table was drawn in {} frames ({} rows); the anchors' own constants gave a usable "
              "matrix to {} of {} IDs in {} of {} anchors (up to {} IDs an anchor; not sane {}, not "
              "rigid {}, c69 unusable in {} anchors); pause/map view: {} of {} history frames had "
              "vehicle IDs (up to {} a frame), which take the camera's motion like the world",
              v.history_frames, v.frames_with_ids, v.ids_with_row, v.ids_now,
              v.ids_now ? 100.0 * double(v.ids_with_row) / double(v.ids_now) : 0.0, v.table_frames,
              v.table_rows, v.ids_taken, v.ids_asked, v.anchors_with_ids, v.anchors, v.max_asked,
              v.not_sane, v.not_rigid, v.no_world, v.pause_with_ids, v.pause_frames,
              v.pause_max_ids);
}

bool AmbientOcclusion::Run(const ResolveCache::Summary& summary, plume::RenderCommandList* list,
                           const Plan& requested) {
  // The frame's plan, which the SSGI test may take its part out of (PrepareGi).
  Plan plan = requested;
  EdramTargets::Target& depth_target = *summary.source;
  const bool ms = depth_target.host_samples > 1;
  // Everything that can fail before anything is recorded. The coarser depth levels are the
  // occlusion's (and the census probe's); the contact shadows need the depth pass, their own march
  // and the denoise pass the occlusion uses.
  const bool levels = plan.occlusion || plan.probe;
  const bool contact = plan.contact.mode != Mode::kOff;
  plume::RenderPipeline* depth_pipeline = Pipeline(ms ? kVDepthMs : kVDepth);
  plume::RenderPipeline* mip_pipeline = levels ? Pipeline(kVMip) : nullptr;
  plume::RenderPipeline* main_pipeline = nullptr;
  plume::RenderPipeline* denoise_pipeline = nullptr;
  plume::RenderPipeline* temporal_pipeline = nullptr;
  plume::RenderPipeline* contact_pipeline = nullptr;
  if (plan.occlusion) {
    main_pipeline = Pipeline(kVMain);
    temporal_pipeline = Pipeline(ms ? kVTemporalMs : kVTemporal);
  }
  if (plan.apply) {
    denoise_pipeline = Pipeline(kVDenoise);
  }
  if (contact) {
    contact_pipeline = Pipeline(kVContact);
  }
  if (!depth_pipeline || (levels && !mip_pipeline) ||
      (plan.occlusion && (!main_pipeline || !temporal_pipeline)) ||
      (plan.apply && !denoise_pipeline) || (contact && !contact_pipeline)) {
    return false;
  }
  if (!EnsureImages(plan)) {
    if (!images_failed_logged_) {
      images_failed_logged_ = true;
      REXLOG_ERROR("[gpu] ao: the images could not be made: no occlusion while that lasts (asked "
                   "again every frame)");
    }
    return false;
  }
  // The bounce light test: its pipelines, the main view's colour target and its images. A frame
  // that cannot have it is the plain occlusion; with the test off its images go back.
  EdramTargets::Target* gi_colour = nullptr;
  uint32_t gi_colour_slot = kNoSlot;
  if (plan.gi.on) {
    if (PrepareGi(summary, list, plan, &gi_colour, &gi_colour_slot)) {
      gi_idle_frames_ = 0;
      // The test has a history of its own: the occlusion's is given back (made again if the test goes).
      for (HostImage& image : history_) {
        if (image.texture) {
          passes_->ReleaseImage(image);
        }
      }
      if (weight_.texture) {
        passes_->ReleaseImage(weight_);
      }
    } else {
      // This frame is the plain occlusion: its history is what EnsureImages left out for the test.
      plan.gi = GiPlan{};
      if (!EnsureImages(plan)) {
        return false;
      }
    }
  } else if (++gi_idle_frames_ >= kGiReleaseAfterFrames) {
    // A frame without the bounce light (the pause menu's world, a setting turned off): its images are kept
    // for a while, so that a pause spell does not make them again.
    ReleaseGiImages();
  }
  LightRaw light;
  uint32_t with_it = 0;
  const bool have_light = PickLight(&light, &with_it);
  UpdateLook();
  Push base;
  FillPush(&base, summary, plan, have_light ? &light : nullptr);

  // The depth plane is read as a texture by every pass and by the apply (after the resolve it is
  // already there: the call records a barrier only when it is elsewhere).
  edram_->Transition(list, depth_target, plume::RenderTextureLayout::SHADER_READ);

  Push push = base;
  pass_timing_->Mark(list, "ao depth");
  push.sizes[0] = float(plan.ao[0]);
  push.sizes[1] = float(plan.ao[1]);
  push.sizes[2] = float(plan.render[0]);
  push.sizes[3] = float(plan.render[1]);
  DrawPass(list, depth_[0], depth_pipeline, push);
  if (levels) {
    // The coarser levels.
    push.slots[2] = depth_[0].slot;
    push.sizes[0] = float(plan.level[0][0]);
    push.sizes[1] = float(plan.level[0][1]);
    push.sizes[2] = float(plan.ao[0]);
    push.sizes[3] = float(plan.ao[1]);
    DrawPass(list, depth_[1], mip_pipeline, push);
    push.slots[2] = depth_[1].slot;
    push.sizes[0] = float(plan.level[1][0]);
    push.sizes[1] = float(plan.level[1][1]);
    push.sizes[2] = float(plan.level[0][0]);
    push.sizes[3] = float(plan.level[0][1]);
    DrawPass(list, depth_[2], mip_pipeline, push);
  }
  if (plan.gi.on) {
    RecordColourChain(list, plan, base, *gi_colour, gi_colour_slot);
  }

  // The visibility image the apply pass reads for the occlusion's term (the depth image stands in
  // while the term is off: the apply pass does not read it then).
  uint32_t ao_slot = depth_[0].slot;
  // The test's images the debug views show: the bounce's history, and the bounce after the a-trous
  // filter (the history itself when there is no filtering).
  HostImage* gi_history = nullptr;
  HostImage* gi_bounce = nullptr;
  if (plan.occlusion) {
    // The visibility from the chain.
    pass_timing_->Mark(list, plan.gi.on ? "ssgi visibility" : "ao visibility");
    push = base;
    push.slots[2] = depth_[0].slot;
    push.slots[3] = depth_[1].slot;
    push.slots2[0] = depth_[2].slot;
    push.sizes[0] = push.sizes[2] = float(plan.ao[0]);
    push.sizes[1] = push.sizes[3] = float(plan.ao[1]);
    // The bounce light test's main pass is the same pass as a visibility bitmask: it reads the colour
    // chain too (slots.x, slots.y, slots2.w; the plain pass leaves them alone), the thickness and the
    // facing weight (shape.z, shape.w), and writes (visibility, bounce) into gi_, whose x channel the
    // first denoise pass below reads in place of raw_.
    HostImage* main_out = &raw_;
    if (plan.gi.on) {
      Push gi_push = push;
      gi_push.slots[0] = colour_[0].slot;
      gi_push.slots[1] = colour_[1].slot;
      gi_push.slots2[3] = colour_[2].slot;
      gi_push.shape[2] = plan.gi.thickness;
      gi_push.shape[3] = plan.gi.facing;
      DrawPass(list, gi_, Pipeline(kVMainGi), gi_push);
      main_out = &gi_;
    } else {
      DrawPass(list, raw_, main_pipeline, push);
    }
    // The history this frame can lean on, and the camera's motion into it.
    const HistoryUse use = ChooseHistory(plan);
    NoteHistory(use);
    // The history's two images take turns as the one this frame reads and the one it writes: the
    // test's (RGBA16F) or the occlusion's (RG16F).
    const uint32_t out_index = 1 - history_index_;
    HostImage* history_pair = plan.gi.on ? gi_hist_ : history_;
    HostImage& history_in = history_pair[history_index_];
    HostImage& history_out = history_pair[out_index];
    // The denoise: once with a history to average into, twice (the second pass back into the first
    // one's input image) without. It is never left out, whatever the quality or the upscaler.
    pass_timing_->Mark(list, plan.gi.on ? "ssgi denoise" : "ao denoise");
    push.slots[2] = main_out->slot;
    push.slots[3] = depth_[0].slot;
    HostImage* visibility = nullptr;
    if (plan.gi.on) {
      // The same blur over the four channels. The test's second pass (a frame with no history) goes into
      // the history's idle image: nothing reads it this frame before the accumulation writes the other.
      plume::RenderPipeline* denoise_gi = Pipeline(kVDenoiseGi);
      DrawPass(list, gi_dn_, denoise_gi, push);
      visibility = &gi_dn_;
      if (!use.on) {
        push.slots[2] = gi_dn_.slot;
        DrawPass(list, history_in, denoise_gi, push);
        visibility = &history_in;
      }
    } else {
      DrawPass(list, denoised_, denoise_pipeline, push);
      visibility = &denoised_;
      if (!use.on) {
        push.slots[2] = denoised_.slot;
        DrawPass(list, raw_, denoise_pipeline, push);
        visibility = &raw_;
      }
    }
    // The accumulation over frames: this frame's visibility with the history's into the other
    // history image, which is the next frame's history and what the apply reads.
    // The two R16G16_FLOAT history images are render targets and shader resources in turn
    // (DrawPass's barriers and first use clear); the pass also reads the stencil plane view of the
    // depth target (Texture2D<uint4> / Texture2DMS<uint4>) in the depth target's SHADER_READ
    // layout. The census line "history:" counts the frames that used a history against the resets
    // and where the camera's motion came from.
    pass_timing_->Mark(list, plan.gi.on ? "ssgi temporal" : "ao temporal");
    push = base;
    push.slots[2] = visibility->slot;
    push.slots[3] = depth_[0].slot;
    // Without a history the slot is any readable image: the shader does not read it.
    push.slots2[0] = use.on ? history_in.slot : depth_[0].slot;
    if (plan.gi.on) {
      // The test's accumulation: the last frame's depth is the image the depth pass wrote then (slots2.y
      // carried the frame number, which this pass does not read), the weight the bounce's history keeps
      // (its own number of frames) and the width of its clip.
      push.slots2[1] = use.on ? depth_prev_.slot : depth_[0].slot;
      push.lens[2] =
          use.on ? float(1.0 - 1.0 / double(std::min(use.age + 2, plan.gi.history))) : 0.0f;
      push.lens[3] = plan.gi.clamp_width;
    }
    push.slots2[2] |= use.on ? kHistoryFlag : 0u;
    push.slots2[3] = summary.stencil_slot;
    // The vehicles' rows: the vehicle table is drawn first (the list is outside any pass here), and the
    // pause/map view's vehicle pixels take the camera's motion instead.
    // CHECK: that nothing moves by itself in the pause/map view (the cars there are still, as the
    // world is): a car that did move in a pause spell would be reprojected as still and fail the
    // plane, depth or clamp test (the wide blur, ghosts held to the clamp's margin). The census's
    // "vehicles" line and the summary line count the pause frames that had vehicle IDs.
    uint32_t vehicle_slot = kNoSlot;
    const uint32_t vehicle_rows = RecordVehicleTable(list, use, &vehicle_slot);
    push.slots[0] = vehicle_slot;
    push.slots2[2] |= trigger_pause_ ? kVehiclesAsWorldFlag : 0u;
    if (use.on) {
      // What the log says: the vehicle IDs the main view had written by now and how many had a row.
      const uint32_t ids_now = stencil_ids_ & ~1u & ((1u << temporal::kVehicleIds) - 1u);
      const uint32_t count_now = uint32_t(std::popcount(ids_now));
      VehicleCounts& v = vehicles_;
      if (trigger_pause_) {
        ++v.pause_frames;
        if (count_now != 0) {
          ++v.pause_with_ids;
          v.pause_max_ids = std::max(v.pause_max_ids, count_now);
        }
      } else {
        ++v.history_frames;
        if (count_now != 0) {
          ++v.frames_with_ids;
          v.ids_now += count_now;
          v.ids_with_row += uint32_t(std::popcount(ids_now & vehicle_rows));
        }
        if (vehicle_rows != 0) {
          ++v.table_frames;
          v.table_rows += uint32_t(std::popcount(vehicle_rows));
        }
      }
    }
    push.sizes[0] = push.sizes[2] = float(plan.ao[0]);
    push.sizes[1] = push.sizes[3] = float(plan.ao[1]);
    // The motion in this renderer's view space (x right, y up, z forward): the game's looks along
    // -z, so the matrix is S G S with S = diag(1, 1, -1, 1). The shader takes the columns as rows.
    const auto motion = [&use](int i, int j) {
      const double sign = ((i == 2) != (j == 2)) ? -1.0 : 1.0;
      return float(sign * use.motion.m[i][j]);
    };
    for (int k = 0; k < 4; ++k) {
      push.lin[k] = motion(k, 0);
      push.shape[k] = motion(k, 1);
      push.light[k] = motion(k, 2);
    }
    push.extra[0] = history_tan_[0];
    push.extra[2] = history_tan_[1];
    push.extra[3] = use.on ? float(1.0 - 1.0 / double(std::min(use.age + 2, use.cap))) : 0.0f;
    // Debug view 6 shows how much history each pixel kept: the pass runs once more into an image of
    // its own, so that the history that goes on to the next frame is the normal one whatever the
    // view.
    // The test's weight picture is also what its debug view 9 shows.
    plume::RenderPipeline* accumulate =
        plan.gi.on ? Pipeline(ms ? kVTemporalGiMs : kVTemporalGi) : temporal_pipeline;
    HostImage& weight_image = plan.gi.on ? gi_weight_ : weight_;
    if (plan.view == 6u || (plan.gi.on && plan.gi.view == 9u)) {
      push.slots2[2] = (push.slots2[2] & ~0xFFu) | 6u;
      DrawPass(list, weight_image, accumulate, push);
    }
    push.slots2[2] &= ~0xFFu;  // the frame's own run writes no debug picture
    DrawPass(list, history_out, accumulate, push);
    if (plan.gi.on) {
      // The a-trous filter of the bounce over the history, into the two images nothing needs now (the
      // denoised image and the history's idle one), each iteration reading the last one's output: the
      // history itself is never filtered.
      gi_history = &history_out;
      gi_bounce = &history_out;
      if (plan.gi.atrous != 0) {
        pass_timing_->Mark(list, "ssgi a-trous");
        plume::RenderPipeline* atrous = Pipeline(kVAtrous);
        HostImage* scratch[2] = {&gi_dn_, &history_in};
        for (uint32_t i = 0; i < plan.gi.atrous; ++i) {
          Push filter = base;
          filter.slots2[2] &= ~0xFFu;
          filter.slots[2] = gi_bounce->slot;
          filter.slots[3] = depth_[0].slot;
          filter.slots2[0] = 1u << i;  // the taps' distance in pixels
          filter.sizes[0] = filter.sizes[2] = float(plan.ao[0]);
          filter.sizes[1] = filter.sizes[3] = float(plan.ao[1]);
          HostImage& target = *scratch[i & 1];
          DrawPass(list, target, atrous, filter);
          gi_bounce = &target;
        }
      }
      if (use.on) {
        ++gi_history_runs_;
      }
    }
    history_index_ = out_index;
    history_valid_ = true;
    history_cut_ = false;
    history_frame_ = frame_;
    history_pause_ = trigger_pause_;
    history_size_[0] = plan.ao[0];
    history_size_[1] = plan.ao[1];
    history_tan_[0] = base.lens[0];
    history_tan_[1] = base.lens[1];
    // The test's bounce averages over more frames than the visibility does: its age runs on to its cap.
    history_age_ =
        use.on ? std::min(use.age + 1, plan.gi.on ? std::max(use.cap, plan.gi.history) : use.cap)
               : 0u;
    history_gi_ = plan.gi.on;
    ao_slot = (plan.view == 6u ? weight_image : history_out).slot;
  }
  // The contact shadows: their march and its denoise, into the two scratch images the occlusion has
  // finished with.
  uint32_t contact_slot = kNoSlot;
  uint32_t mask_slot = kNoSlot;
  if (contact) {
    contact_slot = RecordContact(list, plan, base);
    if (contact_slot != kNoSlot) {
      mask_slot = FindMaskSlot();
    }
  }
  if (mask_slot == kNoSlot && plan.gi.apply) {
    mask_slot = FindMaskSlot();  // the bounce light's albedo estimate takes the sun's shadow into account
  }
  const bool contact_applied = contact_slot != kNoSlot;
  bool applied = false;
  if (plan.apply) {
    // The apply into the scene colour: the occlusion's term and the contact shadows' in one factor.
    push = base;
    push.slots[2] = ao_slot;
    push.slots[3] = depth_[0].slot;
    push.sizes[0] = float(plan.render[0]);
    push.sizes[1] = float(plan.render[1]);
    push.sizes[2] = float(plan.ao[0]);
    push.sizes[3] = float(plan.ao[1]);
    push.slots2[2] |= (plan.occlusion ? kApplyOcclusionFlag : 0u) |
                      (contact_applied ? kApplyContactFlag : 0u) |
                      (mask_slot != kNoSlot ? kApplyMaskFlag : 0u);
    push.slots2[0] = contact_applied ? contact_slot : 0u;
    push.slots2[3] = mask_slot != kNoSlot ? mask_slot : 0u;
    push.lens[2] = plan.contact.fade_start;
    push.lens[3] = plan.contact.fade_end;
    push.shape[0] = plan.contact.mask_gain;
    push.extra[3] = plan.contact.strength;
    if (plan.gi.apply && gi_bounce) {
      // The bounce light's apply (hlsl/ao_apply_ps.hlsl, AO_GI): the filtered image holds the visibility
      // (x, as the history kept it) and the bounce, the colour chain's level 0 the pixel's own colour (and
      // whether it is a source of light); the strength rides in shape.y, the limit in the flags' bits 8-15.
      push.slots[2] = gi_bounce->slot;
      push.slots2[1] = colour_[0].slot;
      push.shape[1] = plan.gi.strength;
      push.slots2[2] = (push.slots2[2] & ~kGiLimitMask) |
                       ((uint32_t(std::lround(plan.gi.limit / kGiLimitUnit)) << kGiLimitShift) &
                        kGiLimitMask);
      if (plan.gi.apply_view != 0) {
        push.slots2[2] = (push.slots2[2] & ~0xFFu) | (kGiApplyViewBase + plan.gi.apply_view);
      }
    }
    if (plan.gi.on && plan.gi.view != 0) {
      // The test's debug picture in place of the apply: an image of the test over the colour target
      // (hlsl/ao_color_ps.hlsl pass 2): 1 the bounce, 2-4 the chain's levels, 5 the bounce cleaned,
      // 6 the visibility.
      const uint32_t v = plan.gi.view;
      const HostImage& shown = (v >= 2 && v <= 4) ? colour_[v - 2]
                               : v == 7           ? *gi_history
                               : v == 8           ? *gi_bounce
                               : v == 9           ? gi_weight_
                                                  : gi_;
      push = base;
      push.slots[2] = shown.slot;
      push.sizes[0] = float(plan.render[0]);
      push.sizes[1] = float(plan.render[1]);
      push.sizes[2] = float(shown.width);
      push.sizes[3] = float(shown.height);
      push.slots2[2] = (v == 1 || v == 7 || v == 8) ? kGiViewBounce
                       : v == 5                      ? kGiViewBounceMean
                       : (v == 6 || v == 9)          ? kGiViewGrey
                                                     : kGiViewRgb;
      // The weight is shown at full white for 1 (the other pictures at 0.3 of their value).
      push.shape[0] = plan.gi.view_gain * (v == 9 ? 1.0f / kGiViewScale : 1.0f);
    }
    applied = RecordApply(list, summary, plan, push);
    applied_this_frame_ = applied;
  }
  if (plan.gi.on) {
    if (gi_runs_ == 0) {
      REXLOG_INFO("[gpu] ssgi: the first frame of the bounce light test: the colour chain {}x{} and "
                  "two coarser levels (RGBA16F) from the {}-sample main colour target, the "
                  "visibility bitmask in place of the GTAO horizons ({}), thickness {:.2f} ({}), a "
                  "sample's radiance capped at {:.2f} x (ambient + sun) and at least {:.2f}, facing "
                  "weight {:.2f}, debug view {}, {:.1f} MiB held by all the occlusion's images",
                  plan.ao[0], plan.ao[1], depth_target.host_samples, ModeName(plan.mode),
                  plan.gi.thickness, plan.gi.thickness > 0.0f ? "a slab" : "a solid", plan.gi.cap,
                  kGiCapFloor, plan.gi.facing, plan.gi.view, double(HeldBytes()) / (1024.0 * 1024.0));
      REXLOG_INFO("[gpu] ssgi: the bounce has its own history and denoise: the occlusion's 3x3 blur over "
                  "the four channels, an accumulation over frames (RGBA16F history, the last frame's depth "
                  "in an R32_FLOAT image of its own; the same camera motion, vehicle rows and tests as the "
                  "occlusion's history; the bounce over {} frames at most, its history clipped to {:.2f} "
                  "deviations of this frame's 3x3 neighbourhood) and {} a-trous iteration(s) of the "
                  "history's copy (the picture's bounce comes from it; the history is never filtered). "
                  "The occlusion's own RG16F history is not made while the test is on; the visibility "
                  "comes from the x channel of the new history (reported once)",
                  plan.gi.history, plan.gi.clamp_width, plan.gi.atrous);
    }
    if (plan.gi.apply && !gi_apply_logged_) {
      gi_apply_logged_ = true;
      REXLOG_INFO("[gpu] ssgi: the bounce light is added to the picture ({} from {}): {} slices and {} steps "
                  "at {}x{} for the {}x{} picture, strength {:.2f}, at most {:.2f} of a pixel's own colour, "
                  "search radius x{:.2f} of the occlusion's; the sun's shadow mask {} (without it the "
                  "albedo estimate takes the sun as unshadowed); the pause menu's and the map's world "
                  "runs the plain occlusion",
                  ModeName(plan.gi.mode), plan.gi.test ? "the debug override mcla_gpu_ssgi_test" : "mcla_gpu_ssgi",
                  plan.gi.fewer ? 2 : 3, plan.gi.fewer ? 2 : 3, plan.ao[0],
                  plan.ao[1], plan.render[0], plan.render[1], plan.gi.strength, plan.gi.limit,
                  plan.gi.radius, mask_slot != kNoSlot ? "found" : "not found in this frame");
    }
    ++gi_runs_;
  }
  if (contact_applied) {
    contact_mask_ = mask_slot != kNoSlot;
    if (contact_runs_ == 0) {
      REXLOG_INFO("[gpu] contact shadows: the first frame: {} at {}x{} (images {}x{}), {} steps over "
                  "{:.2f} m (thickness {:.2f} m), strength {:.2f}, the game's shadow mask {} (the "
                  "sun's visibility is 1 where it is not found), {}",
                  ModeName(plan.contact.mode), plan.render[0], plan.render[1], plan.ao[0],
                  plan.ao[1], plan.contact.steps, plan.contact.length, plan.contact.thickness,
                  plan.contact.strength, contact_mask_ ? "found" : "not found",
                  plan.occlusion ? "with the occlusion" : "without the occlusion");
    }
    ++contact_runs_;
    ++(contact_mask_ ? contact_mask_frames_ : contact_no_mask_frames_);
  }
  if (plan.probe) {
    RecordProbe(list, plan);
  }
  if (plan.gi.on) {
    // The depth image of this frame is the next frame's depth history: the two trade places now that
    // nothing of this frame reads depth_[0] again (the next frame's depth pass writes the other).
    std::swap(depth_[0], depth_prev_);
  }
  ran_this_frame_ = true;
  if (plan.occlusion && applied) {
    if (runs_ == 0) {
      REXLOG_INFO("[gpu] ao: the first occlusion pass: {} at {}x{} (visibility {}x{}), {} depth, "
                  "{}{}",
                  ModeName(plan.mode), plan.render[0], plan.render[1], plan.ao[0], plan.ao[1],
                  ms ? "multisampled" : "single-sampled",
                  plan.upscaler ? "noise rotating every frame, a temporal upscaler running too"
                                : "noise rotating every frame",
                  plan.view ? ", debug view" : "");
    }
    ++runs_;
    if (trigger_pause_) {
      if (pause_runs_ == 0) {
        const temporal::Camera& c = previous_;
        REXLOG_INFO("[gpu] ao: the first occlusion pass in a pause/map view: {} at {}x{} "
                    "(visibility {}x{}), {} host samples of the depth (guest sample 0 = host "
                    "sample {}), the lens of the previous pause/map frame's anchor draw: vertical "
                    "field of view {:.3f} deg, aspect {:.4f}, near {:.4f}, far {:.1f}",
                    ModeName(plan.mode), plan.render[0], plan.render[1], plan.ao[0], plan.ao[1],
                    depth_target.host_samples, summary.host_sample,
                    c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane, c.far_plane);
      }
      ++pause_runs_;
    }
  }
  return true;
}

bool AmbientOcclusion::RecordApply(plume::RenderCommandList* list,
                                   const ResolveCache::Summary& summary, const Plan& plan,
                                   const Push& push) {
  EdramTargets::Target& depth = *summary.source;
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and
  // scale (the depth key's layer is 0 while the scene phase is open, the colour's is always 0).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != depth.host_samples ||
      found->scale != depth.scale) {
    if (!refused_target_logged_) {
      refused_target_logged_ = true;
      REXLOG_ERROR("[gpu] ao: the main view's colour target {} {} (depth: {} host samples at {}x): "
                   "no occlusion applied (reported once)",
                   DescribeEdramKey(key),
                   !found ? "does not exist" : (!found->texture ? "has no texture" : "differs"),
                   depth.host_samples, depth.scale);
    }
    return false;
  }
  uint32_t variant = ApplyVariant(plan.view != 0, depth.host_samples);
  if (plan.gi.on && plan.gi.view != 0) {
    variant = GiViewVariant(depth.host_samples);
  } else if (plan.gi.apply) {
    variant = GiApplyVariant(plan.gi.apply_view != 0, depth.host_samples);
  }
  plume::RenderPipeline* pipeline = Pipeline(variant);
  if (!pipeline) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }
  // The scene's own sequence for a target it draws into: the layout, a framebuffer of that target
  // alone (no depth: nothing here tests or writes it). It is a colour-only render pass on the main
  // target in the middle of the frame: the depth plane stays in SHADER_READ, the colour goes to
  // COLOR_WRITE as for any draw, and the next draw of the game moves the depth back to DEPTH_WRITE
  // itself.
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  pass_timing_->Mark(list, plan.gi.apply ? "ssgi apply" : "ao apply");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(plan.render[0]), float(plan.render[1])};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(plan.render[0]), int32_t(plan.render[1]));
  list->setScissors(&scissor, 1);
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

// ---------------------------------------------------------------------------
// the contact shadows
// ---------------------------------------------------------------------------

bool AmbientOcclusion::PickWet(float* value) const {
  if (wet_draws_ == 0) {
    return false;
  }
  uint32_t best = 0;
  for (uint32_t i = 1; i < kWetBins; ++i) {
    if (wet_bins_[i] > wet_bins_[best]) {
      best = i;
    }
  }
  *value = 0.1f * float(best);
  return true;
}

void AmbientOcclusion::PlanContact(Plan* plan) {
  plan->contact = ContactPlan{};
  ContactState state = ContactState::kOff;
  const auto finish = [&] {
    contact_state_ = state;
    if (uint32_t(state) != contact_state_logged_) {
      contact_state_logged_ = uint32_t(state);
      switch (state) {
        case ContactState::kOff:
          break;
        case ContactState::kNoLight:
          REXLOG_INFO("[gpu] contact shadows: no usable light constants before the trigger: none "
                      "this frame (logged when the state changes)");
          break;
        case ContactState::kGated:
          REXLOG_INFO("[gpu] contact shadows: none now: the sun is at {:.1f} degrees (gate {:.2f}, "
                      "off at {:.0f} and below, full from {:.0f}) and the wetness is {:.1f} (gate "
                      "{:.2f}) (logged when the state changes)",
                      contact_elevation_, contact_gate_sun_, kGateOffDegrees, kGateFullDegrees,
                      contact_wet_, contact_gate_wet_);
          break;
        case ContactState::kActive:
          REXLOG_INFO("[gpu] contact shadows: on: the sun is at {:.1f} degrees (gate {:.2f}), the "
                      "wetness {:.1f} (gate {:.2f}), strength {:.2f} (logged when the state "
                      "changes)",
                      contact_elevation_, contact_gate_sun_, contact_wet_, contact_gate_wet_,
                      contact_strength_);
          break;
      }
    }
  };
  if (contact_mode_ == Mode::kOff) {
    finish();
    return;
  }
  LightRaw light;
  uint32_t with_it = 0;
  float ambient[3], sun[3], dir[3];
  if (!PickLight(&light, &with_it)) {
    state = ContactState::kNoLight;
    finish();
    return;
  }
  DecodeLight(light, ambient, sun, dir);
  const double length = Length3(dir);
  if (!std::isfinite(length) || length < 0.5) {
    state = ContactState::kNoLight;
    finish();
    return;
  }
  // VS c16 is the direction the light travels, so the sun's height above the horizon is -c16.y: noon
  // is +0.867 (60 degrees), 19 h 2.5 degrees, the night's light points up (negative here).
  const double sin_elevation = std::clamp(-double(dir[1]) / length, -1.0, 1.0);
  const double elevation = std::asin(sin_elevation) * 180.0 / 3.14159265358979323846;
  float wet = 0.0f;
  PickWet(&wet);  // none readable: dry
  contact_elevation_ = elevation;
  contact_wet_ = wet;
  contact_gate_sun_ = Smoothstep(kGateOffDegrees, kGateFullDegrees, elevation);
  contact_gate_wet_ = 1.0f - std::clamp((wet - kWetDry) / kWetSpan, 0.0f, 1.0f);
  const float setting = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_strength), 0.0, 2.0));
  contact_strength_ = setting * contact_gate_sun_ * contact_gate_wet_;
  if (!(contact_strength_ > 0.002f) || !(Luma(sun) > 1e-4f)) {
    state = ContactState::kGated;
    finish();
    return;
  }
  state = ContactState::kActive;
  ContactPlan& c = plan->contact;
  c.mode = contact_mode_;
  c.strength = contact_strength_;
  // The ray grows with a low sun (its shadows are long): 0.7 / tan(elevation), 1 to 3 times.
  const double tan_elevation = std::tan(elevation * 3.14159265358979323846 / 180.0);
  const double grow = tan_elevation > 1e-3
                          ? std::clamp(kLengthTan / tan_elevation, 1.0, kLengthMaxScale)
                          : kLengthMaxScale;
  c.length = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_length), 0.1, 3.0) * grow);
  c.thickness = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_thickness), 0.05, 3.0));
  c.steps = contact_mode_ == Mode::kLow ? kContactStepsLow : kContactStepsHigh;
  c.fade_start = kContactFadeStart;
  c.fade_end = kContactFadeEnd;
  c.mask_gain = float(std::clamp(REXCVAR_GET(mcla_gpu_contact_shadows_mask_gain), 0.5, 4.0));
  finish();
}

void AmbientOcclusion::NoteMaskResolve(const ResolveRecord& record) {
  // A colour copy (not a depth one, not a clear alone) into the mask's page, as the lit shaders fetch
  // it: k_8_8_8_8, the picture's size, level 0.
  const uint32_t source = record.resolve_flags & 7;
  if (source >= 4 || (record.resolve_flags & 0x2000) != 0 || record.dest_level != 0 ||
      !(record.flags & kEventHasDest) || (record.flags & kEventDestUnreadable)) {
    return;
  }
  uint32_t dwords[6];
  for (uint32_t i = 0; i < 6; ++i) {
    dwords[i] = LoadBe32(record.dest_header + guest::kTextureFetchOffset + 4 * i);
  }
  const TextureFetch dest = ReadTextureFetch(dwords);
  if (!dest.is_texture || dest.key.base_page != kMaskPage ||
      dest.key.format != kTextureFormat8888 || dest.key.width < kMaskMinWidth) {
    return;
  }
  mask_key_ = dest.key;
  mask_frame_ = frame_;
}

uint32_t AmbientOcclusion::FindMaskSlot() {
  // Only a resolve of this frame made the mask: at night, at 6 h and 18 h and in rain none comes,
  // and the page then holds the last frame's depth copy (the same key, a depth source), which is
  // not a mask. The game's sun shadow is then not there to leave out: the visibility is 1.
  if (!resolve_ || mask_frame_ != frame_) {
    return kNoSlot;
  }
  ResolveCache::Product* product = resolve_->Find(mask_key_, mask_key_.base_page);
  if (!product || !product->texture || product->depth_source ||
      product->layout != plume::RenderTextureLayout::SHADER_READ) {
    if (!mask_why_logged_) {
      mask_why_logged_ = true;
      REXLOG_INFO("[gpu] contact shadows: a resolve into the mask's page came this frame but the "
                  "product is {}: no mask, the sun's visibility is 1 (reported once)",
                  !product ? "not found"
                           : (!product->texture ? "without a texture"
                              : (product->depth_source ? "a depth copy"
                                                       : "not in the shader-read layout")));
    }
    return kNoSlot;
  }
  const uint32_t slot =
      product->views.Slot(*device_, *heap_, *product->texture, product->format,
                          mask_key_.levels(), format::kHostSwizzleRGBA, "ao contact mask view");
  return slot == kInvalidBindlessSlot ? kNoSlot : slot;
}

uint32_t AmbientOcclusion::RecordContact(plume::RenderCommandList* list, const Plan& plan,
                                         const Push& base) {
  plume::RenderPipeline* march = Pipeline(kVContact);
  plume::RenderPipeline* denoise = Pipeline(kVDenoise);
  if (!march || !denoise) {
    return kNoSlot;
  }
  const ContactPlan& c = plan.contact;
  // The march: the linear depth in, the contact visibility out (raw_). Its constants are in the
  // header of hlsl/ao_contact_ps.hlsl.
  pass_timing_->Mark(list, "ao contact");
  Push push = base;
  push.slots[2] = depth_[0].slot;
  push.sizes[0] = push.sizes[2] = float(plan.ao[0]);
  push.sizes[1] = push.sizes[3] = float(plan.ao[1]);
  push.lens[2] = c.length;
  push.lens[3] = c.thickness;
  push.shape[1] = float(c.steps);
  push.extra[1] = c.fade_end;
  DrawPass(list, raw_, march, push);
  // The occlusion's bilateral pass twice (the way the occlusion has no history of its own): the
  // noise of the ray's start goes, the edges of the depth stay.
  pass_timing_->Mark(list, "ao contact denoise");
  push.slots[2] = raw_.slot;
  push.slots[3] = depth_[0].slot;
  DrawPass(list, denoised_, denoise, push);
  push.slots[2] = denoised_.slot;
  DrawPass(list, raw_, denoise, push);
  return raw_.slot;
}

// ---------------------------------------------------------------------------
// the bounce light test (screen-space global illumination, first step)
// ---------------------------------------------------------------------------

void AmbientOcclusion::PlanGi(Plan* plan) {
  plan->gi = GiPlan{};
  // Not in the pause menu's and the map's world (the 4x view): the plain occlusion runs there.
  if (!gi_active_ || !plan->occlusion || !plan->apply || trigger_pause_) {
    return;
  }
  GiPlan& g = plan->gi;
  g.on = true;
  g.test = gi_test_;
  g.mode = gi_test_ ? plan->mode : gi_mode_;
  // What the quality means: the debug override is the occlusion's own (as it was); the setting's high is 3 and
  // 3 at the picture's size and its low is cheaper by fewer directions, by half the size, or both
  // (mcla_gpu_ssgi_low).
  const int low_how = std::clamp(REXCVAR_GET(mcla_gpu_ssgi_low), 0, 2);
  g.fewer = gi_test_ ? plan->mode == Mode::kLow : (g.mode == Mode::kLow && low_how != 1);
  g.half = !gi_test_ && g.mode == Mode::kLow && low_how >= 1;
  g.strength = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_strength), 0.0, 2.0));
  g.limit = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_clamp), 0.05, 4.0));
  g.radius = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_radius), 0.5, 3.0));
  g.apply_view = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_view), 0, 6));
  g.thickness = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_thickness), 0.0, 4.0));
  g.cap = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_clamp), 0.25, 16.0));
  g.facing = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_facing), 0.0, 1.0));
  g.view_gain = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_view_gain), 0.1, 16.0));
  g.view = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_view), 0, 9));
  g.history = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_history), 2, 64));
  g.clamp_width = float(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_history_clamp), 0.25, 8.0));
  g.atrous = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_atrous), 0, 3));
  if (g.view != 0) {
    plan->view = 0;  // instead of the other debug views: they are of the plain passes
    g.apply_view = 0;  // the test's own pictures come first
  } else if (g.apply_view != 0) {
    plan->view = 0;
  }
  // The apply with the bounce: always (the visibility comes from the same image), except where the
  // occlusion's or the contact shadows' debug view asks for the plain apply, or the test shows a picture
  // of its own; a strength of 0 is the test as it was (the plain apply, the bitmask's visibility).
  g.apply = g.view == 0 && (g.apply_view != 0 || (g.strength > 0.0f && plan->view == 0));
}

bool AmbientOcclusion::PrepareGi(const ResolveCache::Summary& summary,
                                 plume::RenderCommandList* list, const Plan& plan,
                                 EdramTargets::Target** colour, uint32_t* colour_slot) {
  EdramTargets::Target& depth = *summary.source;
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and
  // scale (RecordApply finds it the same way).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  std::string why;
  const EdramTargets::Target* found = edram_->Find(key);
  EdramTargets::Target* target = nullptr;
  uint32_t slot = kNoSlot;
  if (!found || !found->texture) {
    why = std::format("the main view's colour target {} {}", DescribeEdramKey(key),
                      !found ? "does not exist" : "has no texture");
  } else if (found->host_samples != depth.host_samples || found->scale != depth.scale) {
    why = std::format("the main view's colour target {} differs from the depth (samples {} against "
                      "{}, scale {}x against {}x)",
                      DescribeEdramKey(key), found->host_samples, depth.host_samples, found->scale,
                      depth.scale);
  } else {
    bool unset = false;
    uint32_t stencil = 0;
    target = edram_->Get(key, found->height, found->scale, 0, list, &unset);
    if (!target || !edram_->SampleSlots(*target, &slot, &stencil)) {
      why = "the main view's colour target could not be read as a texture";
    }
  }
  if (why.empty()) {
    // The pipelines (made here on first use: the test's are never listed ahead) and the images.
    const bool ms = depth.host_samples > 1;
    plume::RenderPipeline* main = Pipeline(kVMainGi);
    plume::RenderPipeline* level0 = Pipeline(ms ? kVColorMs : kVColor);
    plume::RenderPipeline* mip = Pipeline(kVColorMip);
    plume::RenderPipeline* view = plan.gi.view != 0 ? Pipeline(GiViewVariant(depth.host_samples)) : mip;
    plume::RenderPipeline* denoise = Pipeline(kVDenoiseGi);
    plume::RenderPipeline* temporal = Pipeline(ms ? kVTemporalGiMs : kVTemporalGi);
    plume::RenderPipeline* atrous = plan.gi.atrous != 0 ? Pipeline(kVAtrous) : mip;
    plume::RenderPipeline* apply =
        plan.gi.apply ? Pipeline(GiApplyVariant(plan.gi.apply_view != 0, depth.host_samples)) : mip;
    if (!main || !level0 || !mip || !view || !denoise || !temporal || !atrous || !apply) {
      why = "a pipeline of the test could not be made";
    } else if (!EnsureGiImages(plan)) {
      why = "an image of the test could not be made";
    }
  }
  if (!why.empty()) {
    if (!gi_failed_logged_) {
      gi_failed_logged_ = true;
      REXLOG_ERROR("[gpu] ssgi: no bounce light test in the frames that cannot have it (the occlusion "
                   "runs as usual): {} (reported once)",
                   why);
    }
    return false;
  }
  *colour = target;
  *colour_slot = slot;
  return true;
}

bool AmbientOcclusion::EnsureGiImages(const Plan& plan) {
  bool ok =
      passes_->EnsureImage(colour_[0], plan.ao[0], plan.ao[1], kGiFormat, "ssgi colour") &&
      passes_->EnsureImage(colour_[1], plan.level[0][0], plan.level[0][1], kGiFormat,
                           "ssgi colour 1/2") &&
      passes_->EnsureImage(colour_[2], plan.level[1][0], plan.level[1][1], kGiFormat,
                           "ssgi colour 1/4") &&
      passes_->EnsureImage(gi_, plan.ao[0], plan.ao[1], kGiFormat, "ssgi visibility and bounce") &&
      passes_->EnsureImage(gi_dn_, plan.ao[0], plan.ao[1], kGiFormat, "ssgi denoised") &&
      passes_->EnsureImage(gi_hist_[0], plan.ao[0], plan.ao[1], kGiFormat, "ssgi history 0") &&
      passes_->EnsureImage(gi_hist_[1], plan.ao[0], plan.ao[1], kGiFormat, "ssgi history 1") &&
      passes_->EnsureImage(depth_prev_, plan.ao[0], plan.ao[1], plume::RenderFormat::R32_FLOAT,
                           "ssgi depth history");
  // The picture of the history weight (debug view 9, or the occlusion's own view 6 while the test runs),
  // made only while one of them is shown.
  if (ok && (plan.gi.view == 9u || plan.view == 6u)) {
    ok = passes_->EnsureImage(gi_weight_, plan.ao[0], plan.ao[1], kGiFormat, "ssgi history weight");
  } else if (gi_weight_.texture) {
    passes_->ReleaseImage(gi_weight_);
  }
  return ok;
}

void AmbientOcclusion::ReleaseGiImages() {
  if (!passes_) {
    return;
  }
  for (HostImage& image : colour_) {
    if (image.texture) {
      passes_->ReleaseImage(image);
    }
  }
  for (HostImage* image : {&gi_, &gi_dn_, &gi_hist_[0], &gi_hist_[1], &depth_prev_, &gi_weight_}) {
    if (image->texture) {
      passes_->ReleaseImage(*image);
    }
  }
  // A history of the test's kind goes with its images (a plain history is made again when it is asked for).
  if (history_gi_) {
    history_valid_ = false;
    history_gi_ = false;
  }
}

void AmbientOcclusion::RecordColourChain(plume::RenderCommandList* list, const Plan& plan,
                                         const Push& base, EdramTargets::Target& colour,
                                         uint32_t colour_slot) {
  // The colour target is read as a texture: after the opaque draws it is in its write layout; the
  // apply pass (or the game's next draw) moves it back.
  edram_->Transition(list, colour, plume::RenderTextureLayout::SHADER_READ);
  pass_timing_->Mark(list, "ssgi colour chain");
  const bool ms = colour.host_samples > 1;
  // Level 0: the colour target's mean over the pixels and samples a texel covers, each sample capped at
  // a multiple of the frame's ambient + sun (ao_color_ps.hlsl).
  Push push = base;
  push.slots2[2] &= ~0xFFu;  // no debug view
  push.slots[0] = colour_slot;
  push.slots[1] = colour.host_samples;
  push.sizes[0] = float(plan.ao[0]);
  push.sizes[1] = float(plan.ao[1]);
  push.sizes[2] = float(plan.render[0]);
  push.sizes[3] = float(plan.render[1]);
  push.shape[0] = std::max(plan.gi.cap * (base.light[3] + base.extra[0]), kGiCapFloor);
  DrawPass(list, colour_[0], Pipeline(ms ? kVColorMs : kVColor), push);
  // The coarser levels, one pass each, as the depth chain's.
  plume::RenderPipeline* mip = Pipeline(kVColorMip);
  push = base;
  push.slots2[2] &= ~0xFFu;
  push.slots[2] = colour_[0].slot;
  push.sizes[0] = float(plan.level[0][0]);
  push.sizes[1] = float(plan.level[0][1]);
  push.sizes[2] = float(plan.ao[0]);
  push.sizes[3] = float(plan.ao[1]);
  DrawPass(list, colour_[1], mip, push);
  push.slots[2] = colour_[1].slot;
  push.sizes[0] = float(plan.level[1][0]);
  push.sizes[1] = float(plan.level[1][1]);
  push.sizes[2] = float(plan.level[0][0]);
  push.sizes[3] = float(plan.level[0][1]);
  DrawPass(list, colour_[2], mip, push);
}

// ---------------------------------------------------------------------------
// the census's depth statistic
// ---------------------------------------------------------------------------

void AmbientOcclusion::RecordProbe(plume::RenderCommandList* list, const Plan&) {
  if (probe_busy_) {
    return;
  }
  HostImage& level = depth_[2];
  const uint32_t width = level.width;
  const uint32_t height = level.height;
  const uint32_t row_bytes = (width * 2 + 255) / 256 * 256;  // R16_FLOAT, rows 256-aligned
  const uint64_t bytes = uint64_t(row_bytes) * height;
  if (!probe_buffer_ || probe_buffer_bytes_ < bytes) {
    probe_buffer_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes),
                                          "ao census readback");
    probe_buffer_bytes_ = probe_buffer_ ? bytes : 0;
    if (!probe_buffer_) {
      return;
    }
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(level.texture.get(),
                                             plume::RenderTextureLayout::COPY_SOURCE));
  level.layout = plume::RenderTextureLayout::COPY_SOURCE;  // the next DrawPass moves it on
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(probe_buffer_.get(),
                                                        plume::RenderFormat::R16_FLOAT, width,
                                                        height, 1, row_bytes / 2, 0),
      plume::RenderTextureCopyLocation::Subresource(level.texture.get()));
  probe_busy_ = true;
  ++probes_done_;
  const uint64_t frame = frame_;
  const uint64_t generation = generation_;
  const uint32_t row_texels = row_bytes / 2;
  frames_->RetireAction([this, frame, generation, width, height, row_texels] {
    ProcessProbe(frame, generation, width, height, row_texels, 2);
  });
}

void AmbientOcclusion::ProcessProbe(uint64_t frame, uint64_t generation, uint32_t width,
                                    uint32_t height, uint32_t row_texels, uint32_t level_shift) {
  probe_busy_ = false;
  if (generation != generation_ || !probe_buffer_ || width == 0 || height == 0) {
    return;
  }
  const void* mapped = probe_buffer_->map();
  if (!mapped) {
    REXLOG_ERROR("{} depth statistic of frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  std::vector<float> values(size_t(width) * height);
  const uint16_t* source = static_cast<const uint16_t*>(mapped);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      values[size_t(y) * width + x] = HalfToFloat(source[size_t(y) * row_texels + x]);
    }
  }
  probe_buffer_->unmap();

  // Sky and distances over the whole picture, and over its thirds: outdoors the road at the bottom
  // of the picture is nearer than the middle (the guest's 1 near / 0 far, linearised with the
  // lens, gives that), and the sky is at the top.
  struct Third {
    uint64_t count = 0;
    uint64_t sky = 0;
    double sum = 0.0;
  };
  Third thirds[3];
  std::vector<float> sample;
  sample.reserve(16384);
  const size_t stride = std::max<size_t>(values.size() / 16384, 1);
  uint64_t sky = 0;
  for (uint32_t y = 0; y < height; ++y) {
    Third& third = thirds[std::min(3u * y / height, 2u)];
    for (uint32_t x = 0; x < width; ++x) {
      const float t = values[size_t(y) * width + x];
      ++third.count;
      if (!(t < 0.5f * kSkyDistance)) {
        ++third.sky;
        ++sky;
      } else {
        third.sum += double(t);
        if ((size_t(y) * width + x) % stride == 0) {
          sample.push_back(t);
        }
      }
    }
  }
  const auto share = [](uint64_t part, uint64_t whole) {
    return whole ? 100.0 * double(part) / double(whole) : 0.0;
  };
  const auto mean = [](const Third& t) {
    return t.count > t.sky ? t.sum / double(t.count - t.sky) : 0.0;
  };
  float p[4] = {};  // min, median, 90th percentile, max
  if (!sample.empty()) {
    std::sort(sample.begin(), sample.end());
    p[0] = sample.front();
    p[1] = sample[sample.size() / 2];
    p[2] = sample[sample.size() * 9 / 10];
    p[3] = sample.back();
  }
  const double middle = mean(thirds[1]);
  const double bottom = mean(thirds[2]);
  const char* verdict =
      thirds[1].count == thirds[1].sky || thirds[2].count == thirds[2].sky
          ? "no ground to compare (an all-sky third)"
      : bottom < middle ? "the bottom of the picture is nearer than the middle: the guest's "
                          "1 near / 0 far holds"
                        : "the bottom of the picture is NOT nearer than the middle: look at the "
                          "convention (or this is not a street view)";
  REXLOG_INFO("{} depth statistic of frame {} (the linear depth chain's 1/{} level, {}x{}): sky "
              "{:.1f}%, distance min {:.2f} median {:.2f} 90th percentile {:.1f} max {:.1f}; top "
              "third {:.0f}% sky, mean distance of the middle third {:.1f}, of the bottom third "
              "{:.1f}: {}",
              kPrefix, frame, 1u << level_shift, width, height, share(sky, values.size()), p[0],
              p[1], p[2], p[3], share(thirds[0].sky, thirds[0].count), middle, bottom, verdict);
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void AmbientOcclusion::OnPresent() {
  pause_spell_ = pause_view_ && !play_view_ ? pause_spell_ + 1 : 0;
  if (frame_ % kVehicleLogEvery == 0 || (!vehicles_first_logged_ && vehicles_.table_frames != 0)) {
    vehicles_first_logged_ = vehicles_first_logged_ || vehicles_.table_frames != 0;
    LogVehicleSummary();
  }
  if (census_every_ != 0 && frame_had_main_view_) {
    Census();
  }
  // Nothing to compute for a second (the occlusion off, the contact shadows off or gated to nothing):
  // the images go back (they are made again by the first trigger after).
  const bool contact_working = contact_mode_ != Mode::kOff && contact_state_ == ContactState::kActive;
  if (mode_ == Mode::kOff && !contact_working) {
    if (off_frames_ < kReleaseAfterFrames && ++off_frames_ == kReleaseAfterFrames &&
        census_every_ == 0) {
      ReleaseImages();
      images_made_for_[0] = images_made_for_[1] = 0;
    }
  } else {
    off_frames_ = 0;
  }
  anchor_seen_ = false;
  composite_seen_ = false;
  play_view_ = false;
  pause_view_ = false;
  stencil_ids_ = 0;
  frame_had_main_view_ = false;
  qualifying_resolves_ = 0;
  later_tiles_ = 0;
  wet_bins_.fill(0);
  wet_draws_ = 0;
  draws_before_ = 0;
  color_draws_before_ = 0;
  draws_after_ = 0;
  depth_writes_after_ = 0;
  draws_at_anchor_ = 0;
  view4x_draws_ = 0;
  light_count_ = 0;
  light_overflow_ = 0;
  light_draws_ = 0;
  depth_clears_ = 0;
  depth_resolves_.clear();
  other_resolves_ = 0;
  lens_delta_valid_ = false;
  vs_view_inverse_draws_ = 0;
  vs_compare_valid_ = false;
  have_camera_ = false;
  trigger_camera_valid_ = false;
  trigger_pause_ = false;
  outcome_ = Outcome::kNoTrigger;
  ran_this_frame_ = false;
  applied_this_frame_ = false;
  ++frame_;
}

const char* AmbientOcclusion::ViewName() const {
  if (play_view_ && pause_view_) {
    return "play + pause/map (mixed)";
  }
  return pause_view_ ? "pause/map (4x)" : play_view_ ? "play" : "no main";
}

std::string AmbientOcclusion::TriggerText() const {
  const char* kind = trigger_pause_ ? "pause/map" : "play";
  switch (outcome_) {
    case Outcome::kNoTrigger:
      return "no resolve of the frame was the trigger: no occlusion";
    case Outcome::kIdle:
      return "a trigger, with the occlusion off and no depth statistic due: nothing recorded";
    case Outcome::kNoCamera:
      return std::format("a {} trigger, but no previous frame's camera of a {} view (the frame "
                         "before was not a consecutive {} frame with a camera): no occlusion",
                         kind, kind, kind);
    case Outcome::kLensUnknown:
      return "a pause/map trigger in the second frame of a spell (or of the setting being on): the "
             "lens' rate of change is not known yet, no occlusion for this frame";
    case Outcome::kLensChanging:
      return std::format("a pause/map trigger, but the lens changed by {:.1f}% between the last "
                         "two frames (limit {:.0f}%): no occlusion until it settles",
                         100.0 * trigger_lens_step_, 100.0 * temporal::kCutFov);
    case Outcome::kTooSmall:
      return std::format("a {} trigger, but the picture is smaller than {} pixels: no occlusion",
                         kind, kMinRenderSize);
    case Outcome::kNotRecorded:
      return std::format("a {} trigger, but a pipeline or an image could not be made (see the "
                         "errors above): no occlusion", kind);
    case Outcome::kRecorded:
      break;
  }
  const char* done = applied_this_frame_ ? "passes recorded and applied to the main view's colour"
                     : mode_ == Mode::kOff   ? "the depth passes only (the census probe), nothing "
                                               "applied"
                                             : "passes recorded but the apply pass could not be "
                                               "drawn (see the errors above)";
  if (!trigger_camera_valid_) {
    return std::format("a {} trigger, {}", kind, done);
  }
  const temporal::Camera& c = trigger_camera_;
  return std::format("a {} trigger, {} with the lens of the PREVIOUS frame's motion blur draw "
                     "(frame {}, a {} view, the step from the frame before {:.2e}): vertical field "
                     "of view {:.3f} deg, aspect {:.4f}, near {:.4f}, far {:.1f}, camera at "
                     "({:.1f}, {:.1f}, {:.1f}); the sun direction uses that frame's view rotation",
                     kind, done, c.frame, kind, trigger_lens_step_,
                     c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane, c.far_plane,
                     c.position[0], c.position[1], c.position[2]);
}

void AmbientOcclusion::Census() {
  const bool main_view = play_view_ || pause_view_;
  const bool anomaly = main_view && qualifying_resolves_ != 1 && census_anomalies_logged_ < 40;
  // The first four frames of every pause/map spell are logged whatever the interval: a spell is
  // short, and its first frames are where its camera comes from.
  const bool pause_entry =
      pause_view_ && !play_view_ && pause_spell_ <= 4 && census_pause_logged_ < 40;
  const bool periodic = census_frames_logged_ < 3 || frame_ % census_every_ == 0;
  if (!anomaly && !periodic && !pause_entry) {
    return;
  }
  if (anomaly) {
    ++census_anomalies_logged_;
  }
  if (pause_entry) {
    ++census_pause_logged_;
  }
  ++census_frames_logged_;
  const std::string spell = pause_view_ && !play_view_
                                ? std::format(" (frame {} of a pause/map spell)", pause_spell_)
                                : std::string();
  REXLOG_INFO("{} frame {}{}{}: {} view, main view draws {} before the first qualifying depth "
              "resolve ({} with colour), {} after it before the anchor ({} of them write the main "
              "depth), {} after the anchor, 4x (pause/map) draws {}; qualifying resolves {} "
              "(expect exactly 1), later tiles of its picture {}; anchor draw {}; clears of the "
              "main depth {} (last z {}); resolves of other kinds {}",
              kPrefix, frame_, anomaly ? " (UNEXPECTED trigger count)" : "", spell, ViewName(),
              draws_before_, color_draws_before_, draws_after_, depth_writes_after_,
              draws_at_anchor_, view4x_draws_, qualifying_resolves_, later_tiles_,
              anchor_seen_ ? "seen" : "NOT seen", depth_clears_,
              depth_clears_ ? std::format("{}", depth_clear_z_) : std::string("none"),
              other_resolves_);
  uint32_t index = 0;
  for (const DepthResolveNote& n : depth_resolves_) {
    REXLOG_INFO("{}   depth resolve {}: caller {:#010x}, source {}, host sample {} of {}, "
                "destination {:08x} format {} {}x{}, rect ({}, {})-({}, {}), {}, {}, after {} main "
                "view draws, anchor {}: {}",
                kPrefix, index++, n.caller, DescribeEdramKey(n.source), n.host_sample,
                n.host_samples, n.dest_address, n.dest_format, n.dest_width, n.dest_height,
                n.rect[0], n.rect[1], n.rect[2], n.rect[3],
                n.inside_end_tiling ? "inside EndTiling" : "plain resolve",
                n.clears_depth ? "clears the depth after" : "no clear", n.draws_before,
                n.anchor_before ? "already drawn" : "not yet",
                n.refused == 0 ? "THE TRIGGER"
                               : std::format("not the trigger ({})", RefusalNames(n.refused)));
  }
  LightRaw picked;
  uint32_t with_it = 0;
  if (PickLight(&picked, &with_it)) {
    float ambient[3], sun[3], dir[3];
    DecodeLight(picked, ambient, sun, dir);
    float first_a[3], first_s[3], first_d[3], last_a[3], last_s[3], last_d[3];
    DecodeLight(first_light_, first_a, first_s, first_d);
    DecodeLight(last_light_, last_a, last_s, last_d);
    REXLOG_INFO("{}   light constants: {} distinct values over {} colour draws before the trigger "
                "({} draws past the table); the commonest ({} draws): PS c26 ambient {}, PS c39 sun "
                "colour {}, VS c16 the direction the light travels {} (length {:.4f}); first draw: "
                "{} {} {}; last draw: {} {} {}",
                kPrefix, light_count_ + (light_overflow_ ? 1 : 0), light_draws_, light_overflow_,
                with_it, Vec3(ambient), Vec3(sun), Vec3(dir), Length3(dir), Vec3(first_a),
                Vec3(first_s), Vec3(first_d), Vec3(last_a), Vec3(last_s), Vec3(last_d));
    if (have_camera_) {
      // The direction to the sun, -c16, through this frame's view (row vectors, v' = v M, the
      // game's view space looks along -z): at noon the sun is overhead, so it is up in the world
      // (y > 0) when c16 is the direction the light travels.
      double view[3] = {};
      for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
          view[j] -= double(dir[i]) * camera_.view.m[i][j];
        }
      }
      const double ambient_luma = Luma(ambient);
      const double sun_luma = Luma(sun);
      REXLOG_INFO("{}   sun: c16 is the direction the light travels, y {:+.3f}, so the direction to "
                  "the sun (-c16) has world y {:+.3f} (+ is up: a sun overhead at noon gives +) and, "
                  "in the game's view space, ({:+.3f}, {:+.3f}, {:+.3f}); luma ambient {:.4g}, "
                  "sun {:.4g}: ambient share for a surface facing the sun {:.3f}, a surface in "
                  "shade 1.000",
                  kPrefix, double(dir[1]), -double(dir[1]), view[0], view[1], view[2],
                  ambient_luma, sun_luma,
                  ambient_luma + sun_luma > 1e-9 ? ambient_luma / (ambient_luma + sun_luma) : 1.0);
    }
  } else {
    REXLOG_INFO("{}   light constants: no colour draw of the main view before the trigger", kPrefix);
  }
  if (have_camera_) {
    const temporal::Camera& c = camera_;
    REXLOG_INFO("{}   lens (the game's motion blur draw, this frame): vertical field of view {:.3f} "
                "deg, aspect {:.4f}, near {:.4f}, far {:.1f}, c62 {}x{}, camera at ({:.1f}, {:.1f}, "
                "{:.1f}) looking ({:+.3f}, {:+.3f}, {:+.3f})",
                kPrefix, c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane,
                c.far_plane, c.width, c.height, c.position[0], c.position[1], c.position[2],
                c.forward[0], c.forward[1], c.forward[2]);
    if (lens_delta_valid_) {
      REXLOG_INFO("{}   the previous frame's lens (what the trigger uses) against this one's: "
                  "relative change fov {:.2e}, aspect {:.2e}, near {:.2e}, far {:.2e}; camera moved "
                  "{:.3f}, cosine of the turn {:.6f}",
                  kPrefix, lens_delta_[0], lens_delta_[1], lens_delta_[2], lens_delta_[3],
                  lens_delta_[4], lens_delta_[5]);
    } else {
      REXLOG_INFO("{}   no previous frame's camera of this kind of view (the frame before was "
                  "not a consecutive {} frame with a camera): the trigger gives no occlusion this "
                  "frame",
                  kPrefix, pause_view_ ? "pause/map" : "play");
    }
  } else {
    REXLOG_INFO("{}   no camera this frame ({})", kPrefix,
                anchor_seen_ ? temporal::RejectName(reject_) : "no anchor draw");
  }
  if (vs_compare_valid_) {
    REXLOG_INFO("{}   vertex bank: VS c12-c15 of the last colour draw before the trigger "
                "against the view inverse the anchor reads from PS c12-c15: largest entry "
                "difference {:.3e} ({}); frames compared so far: play {} of {} equal, pause/map {} "
                "of {} equal (where they agree, the frame's own camera exists in the vertex bank "
                "before the anchor)",
                kPrefix, vs_compare_diff_,
                vs_compare_diff_ <= 1e-4 ? "equal" : "DIFFERENT", vs_equal_[0], vs_compared_[0],
                vs_equal_[1], vs_compared_[1]);
  }
  REXLOG_INFO("{}   trigger: {}", kPrefix, TriggerText());
  REXLOG_INFO("{}   history: this frame's trigger {}{}; frames that used a history {}, did not: first {}, "
              "gap {}, view {}, size {}, kind {}, cut {}, cap {}; motion from the vertex constants {}, the "
              "last step {}, standing still {}; the vertex constants {} as the frame's camera",
              kPrefix,
              history_why_ == HistoryWhy::kUsed
                  ? std::format("used the history, camera motion from {}", PoseName(history_pose_))
                  : std::format("had no history: {}", HistoryWhyName(history_why_)),
              ran_this_frame_ ? "" : " (no passes recorded this frame)", history_used_,
              history_resets_[size_t(HistoryWhy::kFirst)], history_resets_[size_t(HistoryWhy::kGap)],
              history_resets_[size_t(HistoryWhy::kView)], history_resets_[size_t(HistoryWhy::kSize)],
              history_resets_[size_t(HistoryWhy::kKind)], history_resets_[size_t(HistoryWhy::kCut)],
              history_resets_[size_t(HistoryWhy::kCap)],
              pose_frames_[size_t(Pose::kVertexBank)], pose_frames_[size_t(Pose::kPredicted)],
              pose_frames_[size_t(Pose::kStill)],
              vs_pose_trusted_ ? "are trusted" : "are not trusted yet");
  REXLOG_INFO("{}   vehicles: IDs the main view wrote before this point {:#08x}; the last anchor "
              "(frame {}) gave a usable motion for {:#08x}; table rows drawn so far {} in {} frames",
              kPrefix, stencil_ids_ & ~1u, vehicle_motion_frame_,
              vehicle_motion_.valid, vehicles_.table_rows, vehicles_.table_frames);
  REXLOG_INFO("{}   occlusion: {} (mcla_gpu_ao), passes {} this frame, {} runs in all, {} frames "
              "without a previous frame's camera; pause/map view: {} runs, {} triggers without "
              "a pause/map camera, {} held back for the lens (second frame or a sweep); the depth "
              "statistic follows when the GPU has run its copy",
              kPrefix, ModeName(mode_), ran_this_frame_ ? "recorded" : "not recorded", runs_,
              skipped_no_lens_, pause_runs_, pause_skipped_no_lens_, pause_skipped_lens_);
  if (contact_mode_ != Mode::kOff) {
    float wet = 0.0f;
    const bool have_wet = PickWet(&wet);
    const char* state = contact_state_ == ContactState::kActive    ? "active"
                        : contact_state_ == ContactState::kGated   ? "gated to nothing"
                        : contact_state_ == ContactState::kNoLight ? "no light constants"
                                                                   : "off";
    REXLOG_INFO("{}   contact shadows: {} (mcla_gpu_contact_shadows), {}; sun at {:.1f} degrees (gate "
                "{:.2f}), wetness VS c43.x {} (gate {:.2f}), strength {:.2f}; the game's shadow "
                "mask: a resolve into its page {} this frame; frames with contact shadows so far: "
                "{} with the mask, {} without it, {} in all",
                kPrefix, ModeName(contact_mode_), state, contact_elevation_, contact_gate_sun_,
                have_wet ? std::format("{:.1f} ({} draws)", wet, wet_draws_) : std::string("not read"),
                contact_gate_wet_, contact_strength_,
                mask_frame_ == frame_ ? "came" : "did not come", contact_mask_frames_,
                contact_no_mask_frames_, contact_runs_);
  }
  if (gi_active_ || gi_runs_ != 0) {
    REXLOG_INFO("{}   bounce light: {} (mcla_gpu_ssgi), the test {} (mcla_gpu_ssgi_test), frames recorded "
                "so far {}, {} of them with a history to lean on ({} frames at most, clip {:.2f} "
                "deviations, {} a-trous iterations); images held {:.1f} MiB",
                kPrefix, ModeName(gi_mode_), gi_test_ ? "on" : "off", gi_runs_, gi_history_runs_,
                uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_history), 2, 64)),
                std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_history_clamp), 0.25, 8.0),
                uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_ssgi_test_atrous), 0, 3)),
                double(HeldBytes()) / (1024.0 * 1024.0));
  }
}

}  // namespace mcla::gpu

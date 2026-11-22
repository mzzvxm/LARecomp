// The pages of the hidden DEBUG menu. See debug_menu_pages.h.

#include "debug_menu_pages.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ui/ui_system.h>

#include "mc_engine/sdk_access.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "debug_menu.h"

namespace mcla_menu {

namespace {

// ── Value tables ───────────────────────────────────────────────────────
//
// Names stay inside A-Z, 0-9, space, '+', '.' and parentheses like every row of the pause menu, and
// no ':' or '/' (the value's text is also the name of a Flash element, pause_menu.cpp "speed-unit note").

template <typename T, std::size_t N>
constexpr int Len(const T (&)[N]) {
    return int(N);
}

// An INT32 cvar of 0 and 1 (frame_pacing): a Bool row would write "true" and "false".
constexpr const char* kOffOnVals[]  = {"0", "1"};
constexpr const char* kOffOnNames[] = {"OFF", "ON"};

constexpr const char* kFreecamVals[]  = {"off", "free"};
constexpr const char* kFreecamNames[] = {"OFF", "ON"};
constexpr double kCamSpeedVals[]      = {10.0, 20.0, 40.0, 80.0, 150.0, 300.0};
constexpr double kCamSensVals[]       = {0.5, 1.0, 1.5, 2.5, 4.0, 6.0, 10.0};

// The 64 shots menu_cam_slot can pin (0-63).
struct SlotTable {
    double v[64];
};
constexpr SlotTable MakeSlotTable() {
    SlotTable t{};
    for (int i = 0; i < 64; ++i) t.v[i] = double(i);
    return t;
}
constexpr SlotTable kMenuCamSlots = MakeSlotTable();

// Which render phase bits SHADOWS clears (SHADOW PHASES).
//
// STOCK is what the game's own `noshadows` switch clears: the sun cascade phases
// 0x20/0x40/0x80/0x100 plus 0x2000/0x4000. Measured at midnight those are not even in
// renderer+364, so clearing them does nothing: the shadow that is actually drawn comes from phase
// 0x400, which sub_823112C0 routes to the shadowNight / shadowFastBlend technique group and which
// `noshadows` leaves alone. Hence BLEND.
//
// ALL also takes 0x10/0x200/0x800, which are live passes unrelated to shadows; it corrupts the
// frame and is only useful for narrowing a phase down. (These lists were rows of the normal
// PERFORMANCE pages until SX-G5.)
constexpr const char* kShadowBitVals[]  = {"0x61E0", "0x400", "0x65E0", "0x7DF0"};
constexpr const char* kShadowBitNames[] = {"STOCK", "BLEND ONLY", "STOCK+BLEND", "ALL (BREAKS)"};
constexpr double kBreakFpsVals[]        = {0.0, 5.0, 10.0, 15.0, 20.0, 30.0, 60.0};
// Capture mode (the trailer toolkit): frames a second of the fixed step and of a take; 0 is off.
constexpr double kFixedStepVals[]       = {0.0, 24.0, 30.0, 60.0, 120.0, 240.0};
constexpr double kCaptureFpsVals[]      = {0.0, 24.0, 30.0, 60.0, 120.0};

#if defined(LARECOMP_HAS_GPU)
constexpr const char* kAoViewVals[]  = {"0", "1", "2", "3", "4", "5", "6"};
constexpr const char* kAoViewNames[] = {"OFF", "FACTOR", "VISIBILITY", "AMBIENT SHARE",
                                        "NORMALS", "DISTANCE", "HISTORY"};
constexpr double kAoHistoryVals[]    = {1.0, 2.0, 4.0, 8.0, 12.0, 16.0, 32.0, 64.0};
constexpr double kAoFadeEndVals[]    = {20.0, 40.0, 80.0, 160.0, 320.0, 640.0, 1000.0};
constexpr const char* kHdrViewVals[]  = {"0", "1", "2", "3", "4", "5"};
constexpr const char* kHdrViewNames[] = {"OFF", "UI ONLY", "SCENE ONLY", "UI COVERAGE",
                                         "OVER WHITE", "DIFFERENCE"};
// What the deferred local light pass lights (mcla_gpu_local_lights_sources, a sum of bits: gpu/local_lights_rules.h): none, the frame's surface lights, the police light bars, the
// head lamps of the cars the headlight stage does not light, the cars' rear lamps, the lamps of mods outside their grid, and the police bars + the cars' lamps + the mods' (4 + 8 + 16 + 32).
// The test lamp (2) and the lamp A/B (mcla_gpu_local_lights_ab, read at start) stay cvars of the lamp tests.
constexpr const char* kLocalLightSourceVals[]  = {"0", "1", "4", "8", "16", "32", "60"};
constexpr const char* kLocalLightSourceNames[] = {"NONE", "FRAME LIGHTS", "POLICE BARS", "HEAD LAMPS", "REAR LAMPS", "MOD LAMPS", "CARS + MODS"};
// Which of the two lights the cars' headlights (mcla_gpu_headlight_pass, gpu/headlight_pass_rules.h, GB-H2): the lit shaders' own variant (FORWARD), the deferred local light pass with the
// atlas shadows (PASS), or both added together (BOTH: the difference pictures).
constexpr const char* kHeadlightRouteVals[]  = {"forward", "pass", "both"};
constexpr const char* kHeadlightRouteNames[] = {"FORWARD", "PASS", "BOTH"};
constexpr const char* kMarkerLevelVals[]  = {"0", "1", "2"};
constexpr const char* kMarkerLevelNames[] = {"FRAME", "PHASES", "DETAIL"};
constexpr double kGhostLogVals[]          = {0.0, 1.0, 10.0, 60.0, 300.0};
constexpr double kMipBiasVals[]           = {-2.0, -1.5, -1.0, -0.5, 0.0, 0.5};
constexpr double kMemoryLogVals[]         = {0.0, 60.0, 300.0, 1800.0};
constexpr const char* kGlowVisibilityVals[]  = {"game", "soft"};
constexpr const char* kGlowVisibilityNames[] = {"GAME", "SOFT"};
// The UI sharpening's strength (ui_sharp.h, mcla_ui_sharp_strength): 1 is the default, 0.5 half of it, 2 a harder edge; 0 does nothing (the Dbl row shows it as NONE).
constexpr double kUiSharpVals[]           = {0.0, 0.5, 1.0, 1.5, 2.0};
// The share of the game's own UI the picture shows (the trailer toolkit's HUD fade); 1 is the game's frame.
constexpr double kHudAlphaVals[]          = {1.0, 0.75, 0.5, 0.25, 0.0};
#endif

// ── The tables ─────────────────────────────────────────────────────────
//
// Every cvar named here is tagged .debug_only() and live. tools/check_settings_exposure.py reads these
// tables (they start with kDebug) and fails on a cvar that is not, or that is restart-only: a setting
// that is only read at start belongs to a launch argument, not a row here (a debug row is never saved,
// so it could not be kept for the next start).

// The hub, in the order of SETTINGS-REVIEW.md 5.3. The rows are Submenu rows: each opens its page.
const ItemDef kDebugHubItems[] = {
    Submenu("PM_RxDbgSubFixes", "FIXES",       "RxDbgFixesMenu"),
    Submenu("PM_RxDbgSubPerf",  "PERFORMANCE", "RxDbgPerfMenu"),
    Submenu("PM_RxDbgSubRend",  "RENDERING",   "RxDbgRendMenu"),
    Submenu("PM_RxDbgSubCam",   "CAMERA",      "RxDbgCamMenu"),
    Submenu("PM_RxDbgSubTools", "TOOLS",       "RxDbgToolsMenu"),
    Submenu("PM_RxDbgSubLog",   "LOGGING",     "RxDbgLogMenu"),
    Submenu("PM_RxDbgSubInfo",  "INFO",        "RxDbgInfoMenu"),
};

// FIXES: each fix of the recomp with the switch that turns it off, to see whether it caused a bug
// (owner's answer 2026-11-13: kept as triage switches). The rows are the cvars as they are: the page makes
// no new switch.
const ItemDef kDebugFixesItems[] = {
    Bool("PM_RxDbgRealDelta",  "real_frame_delta",          "REAL FRAME DELTA: "),
    // The Plume renderer's frame pacing (frame_pacing.cpp); the cvar is 0 or 1.
    NativeOnly(Str("PM_RxDbgFramePacing", "frame_pacing",   "FRAME PACING: ",
                   kOffOnVals, kOffOnNames, Len(kOffOnVals))),
    Bool("PM_RxDbgChassis",    "smooth_chassis_depth",      "SUSPENSION FIX: "),
    Bool("PM_RxDbgChaseCam",   "smooth_chase_cam",          "SMOOTH CHASE CAM: "),
    Bool("PM_RxDbgVehPhysics", "smooth_vehicle_physics",    "SMOOTH VEHICLE PHYSICS: "),
    Bool("PM_RxDbgCamTrans",   "smooth_camera_transitions", "SMOOTH CAMERA TRANSITIONS: "),
    Bool("PM_RxDbgShakeFix",   "scale_camera_shake",        "CAMERA SHAKE FIX: "),
    Bool("PM_RxDbgCountdown",  "ui_countdown_fix",          "COUNTDOWN FIX: "),
    Bool("PM_RxDbgKeyRepeat",  "ui_input_repeat_30hz",      "MENU KEY REPEAT: "),
    Bool("PM_RxDbgMenuInput",  "ui_advance_on_input",       "MENU INPUT: "),
    Bool("PM_RxDbgMenu3d",     "ui_interpolate_camera",     "SMOOTH MENU 3D: "),
    // Smooth UI: every menu, pop-up, map panel and the HUD steps at the console's 30 a second and is drawn between
    // its last two steps (hooks/ui_smooth.cpp). It was a row of DISPLAY; it is a switch to look for a UI bug with now.
    Bool("PM_RxDbgSmoothUi",   "ui_interpolate",            "SMOOTH UI: "),
    // The neon underglow's pulse held on the console's 30 Hz clock (hooks/neon_pulse.cpp).
    Bool("PM_RxDbgNeonPulse",  "mcla_fix_neon_30hz",        "NEON PULSE 30 HZ: "),
    // The smoke of a closed race checkpoint fades at the console's rate at every frame rate (hooks/checkpoint_effect_fade.cpp).
    Bool("PM_RxDbgCpFade",     "mcla_fix_checkpoint_fade",  "CHECKPOINT SMOKE: "),
    // The native keyboard (keyboard_native.cpp) is always on; this turns it off to tell whether a bug is its (off = the keyboard does nothing).
    Bool("PM_RxDbgNativeKb",   "keyboard_native",           "NATIVE KEYBOARD: "),
    // Button prompts follow the device in use on every screen (hooks/ui.cpp, keyboard_native.cpp): the glyph bitmaps are painted, the movies' platform flag stays on Xbox. Off = the
    // older way (the flag shows the PlayStation set; screens already open keep their glyphs until reopened; only the key caps are painted).
    Bool("PM_RxDbgPromptsLive", "prompts_live",             "LIVE BUTTON PROMPTS: "),
    // Keyboard steering builds up to full lock and comes back over a short time (keyboard_native.cpp KeySteerScope); off = the old on / off key pair, full lock at once.
    Bool("PM_RxDbgSteerRamp",  "kb_steer_ramp",             "KEYBOARD STEER RAMP: "),
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Bool("PM_RxDbgDither", "mcla_gpu_present_dither", "PRESENT DITHER: ")),
    NativeOnly(Bool("PM_RxDbgA2c",    "mcla_gpu_a2c_dither_all", "ALPHA TO MASK FIX: ")),
    // The glow of lights keeps its size above a render scale of 1x: the bloom's Gaussian taps follow the
    // scale. The renderer reads the cvar at each draw, so the row is live.
    NativeOnly(Bool("PM_RxDbgBloomScale", "mcla_gpu_bloom_scale", "BLOOM SIZE FIX: ")),
    // The auto-exposure measures the same screen area above a render scale of 1x as at 1x (its sample pass's
    // taps follow the scale). Read at each draw, so the row is live; no change at a render scale of 1x.
    NativeOnly(Bool("PM_RxDbgLumScale", "mcla_gpu_luminance_scale_fix", "EXPOSURE SAMPLE FIX: ")),
    // How hard the UI sharpening works (gpu/ui_sharp.h): the SHARP UI row on GRAPHICS > IMAGE QUALITY turns it on and off (mcla_ui_sharp, on by default,
    // and SHARP MINIMAP is mcla_ui_minimap_scale); this is the strength for tuning, 1 the default, 2 a harder alpha edge. Read at each UI draw, so live.
    NativeOnly(Dbl("PM_RxDbgUiSharp", "mcla_ui_sharp_strength", "UI SHARP STRENGTH: ",
                   kUiSharpVals, Len(kUiSharpVals), "%g", "", "NONE")),
    // The chrome of the cars (the tailpipe tips, badges and trim) is not lit by what the car's own body covers: short rays through the car's picture of the frame
    // before (the ray traced one where the card can trace) darken it under the bumper. Off = the game's own chrome, which glows there. The variant is chosen at each
    // draw (pipelines.cpp WantedFeatures), so the row is live; the first draw after turning it on builds the shader (a short stutter).
    NativeOnly(Bool("PM_RxDbgCarChrome", "mcla_gpu_car_chrome_occlusion", "CAR CHROME FIX: ")),
#endif
    Action("PM_RxDbgFixReset", kActionDebugReset),
};

// PERFORMANCE: the switches that trade frame pacing, CPU time and picture for speed, and that have no row
// in the normal menus (SX-G5; owner's answer 3: debug for now; a switch comes back after it is
// measured as a safe win on weak hardware). Not here: TREE IMPOSTORS and SCREEN BLUR
// (perf_no_impostors, perf_no_fullscreen_blur) are read once at start, so they are launch arguments,
// not rows. CHEAP CAR SHADOW (perf_fast_vehicle_shadows) is here since the game was found to read its
// switch at every car shadow draw (render_phases.cpp).
const ItemDef kDebugPerformanceItems[] = {
    Bool("PM_RxDbgPaceSwap",   "pace_at_swap",           "PACE AT PRESENTATION: "),
    // Patch_FenceSpinThrottle, live: the hook reads the cvar on every call, so the next fence poll
    // already sees a change made here. Whether it helps or hurts depends on the host CPU, not on the
    // game. It replaces the guest's fence-poll pause with YieldProcessor plus a SwitchToThread every
    // 16th call, and the guest reaches that poll about 271k times per frame. Measured on an 8C/16T with
    // three threads near 100%: 644k SwitchToThread a second costing 10 ms/s in total, because with cores
    // to spare each one returns without switching -- no gain, no loss. On a machine with fewer cores
    // those same calls switch for real and the poll thread pays microseconds each time, so ON can cost
    // far more than it saves. Leave it ON, turn it OFF if the game stutters.
    Bool("PM_RxDbgFenceSpin",  "fence_spin_throttle",    "FENCE SPIN YIELD: "),
    Bool("PM_RxDbgLimitSpin",  "frame_limit_spin",       "FPS LIMIT SPIN: "),
    Bool("PM_RxDbgPumpWake",   "pump_wake_on_demand",    "FRAME PACING WAKE: "),
    // CHECK: flipped on after the game made its surfaces, are they past the 2048 EDRAM tiles
    // Patch_EdramLimit lets through only while this is on (graphics.cpp)? A live ON may misbehave.
    Bool("PM_RxDbgSingleTile", "single_tile",            "SINGLE TILE: "),
    Str ("PM_RxDbgShadowBits", "perf_shadow_phase_bits", "SHADOW PHASES: ",
         kShadowBitVals, kShadowBitNames, Len(kShadowBitVals)),
    // Live: the game reads the switch each time it draws a car into a shadow pass (the daylight
    // cascades), and the dev-option node is kept equal to the cvar every frame. Nothing changes at night.
    // CHECK: the car's shadow before and after a flip in daylight (chain-live3b.ps1).
    Bool("PM_RxDbgCarShadow",  "perf_fast_vehicle_shadows", "CHEAP CAR SHADOW: "),
    // The cvar is live; the prop parse it skips runs when a district loads.
    // CHECK: whether that parse runs again after a district change or only at start-up (render_phases.cpp).
    Bool("PM_RxDbgNoTrees",    "perf_no_trees",          "FOLIAGE: ", true, " (ON RELOAD)"),
    Dbl ("PM_RxDbgBreakFps",   "breaking_frame_rate_limit", "BREAK FPS FLOOR: ",
         kBreakFpsVals, Len(kBreakFpsVals), "%g", "", "STOCK"),
};

// RENDERING: the Plume renderer's debug views and logs, and the picture's shape.
const ItemDef kDebugRenderingItems[] = {
#if defined(LARECOMP_HAS_GPU)
    // RENDERER: NATIVE (LARecomp's own, Plume), XENIA D3D12 or XENIA VULKAN (the SDK's Xenia GPU, which stays in the program as a hidden
    // fallback: if the native renderer will not run on a PC, this is how a tester or a player who was told to gets the game going; the
    // launcher and the normal menus do not offer it). The choice is gpu.driver, which is read at start: the row HOLDS it, the next save
    // writes it to larecomp.toml (the one debug row that is kept) and leaving the menu offers RESTART NOW. Both renderers list it. Grey
    // while a command line option or an environment variable sets gpu.driver (it beats the file). Its words are pause_menu.cpp's
    // (kDebugRendererDynamics); the mapping is gpu/renderer_choice.h.
    WithDynamics(Str("PM_RxDbgRenderer", "gpu.driver", "RENDERER: ", nullptr, nullptr, 0), &kDebugRendererDynamics),
    NativeOnly(Str("PM_RxDbgAoView",     "mcla_gpu_ao_view",       "AO VIEW: ",
                   kAoViewVals, kAoViewNames, Len(kAoViewVals))),
    NativeOnly(Dbl("PM_RxDbgAoHistory",  "mcla_gpu_ao_history",    "AO HISTORY: ",
                   kAoHistoryVals, Len(kAoHistoryVals), "%g")),
    NativeOnly(Dbl("PM_RxDbgAoFade",     "mcla_gpu_ao_fade_end",   "AO FADE END: ",
                   kAoFadeEndVals, Len(kAoFadeEndVals), "%g")),
    NativeOnly(Str("PM_RxDbgHdrView",    "mcla_gpu_hdr_layers_view", "HDR LAYERS VIEW: ",
                   kHdrViewVals, kHdrViewNames, Len(kHdrViewVals))),
    // The deferred local light pass (gpu/local_lights.h, GB-H1 and GB-H3): one additive draw per light from the G-buffer's planes (the planes are made only while this is on:
    // gbuffer_rules.h AutoPlanes). OFF by default, and a DEBUG row only: there is no LOCAL LIGHTS row for players (owner, 2026-11-21: the pass is a way to light the game's lamps and the
    // cars' lights, not a setting). Both are live; with SOURCES at NONE the pass draws nothing (the lamp A/B's lamps apart).
    // CHECK: the SOURCES row writes its words ("0", "4", ...) into an integer cvar as the other 0/1 rows do (frame_pacing); a value that is not one of the seven (a sum a command line set, 28)
    // shows as the first word until the row is stepped.
    NativeOnly(Bool("PM_RxDbgLocalLights", "mcla_gpu_local_lights", "LOCAL LIGHT PASS: ")),
    NativeOnly(Str("PM_RxDbgLocalSrc",   "mcla_gpu_local_lights_sources", "LIGHT PASS SOURCES: ",
                   kLocalLightSourceVals, kLocalLightSourceNames, Len(kLocalLightSourceVals))),
    // The headlights' A/B (GB-H2, live; the forward stage is deleted only after the owner has seen the pictures): FORWARD is the game as it is, PASS takes the lit shaders' headlight variant away and
    // lights the same cars' headlights in the deferred pass with their shadows (it also reaches what the variant leaves out: the cars' paint, the pedestrians, the interiors; the pass runs for it
    // whatever LOCAL LIGHT PASS says and needs HEADLIGHTS on), BOTH lights twice. The words are the cvar's, written as the other word rows do.
    NativeOnly(Str("PM_RxDbgHeadlightVia", "mcla_gpu_headlight_pass", "HEADLIGHTS VIA: ",
                   kHeadlightRouteVals, kHeadlightRouteNames, Len(kHeadlightRouteVals))),
    // The headlight route PASS keeps the lit shaders' forward headlight on the draws the deferred pass does not light: the neon, the store sign and the lamp glass (the pass skips them as emissive),
    // a draw with no planes, a draw that clears them; the draws the pass does light drop it, so a pixel is lit once. Off = the route's first version: those draws get no headlight. Read at each
    // draw, so live; no effect in the routes FORWARD and BOTH. The A/B of GB-H2-FOLLOW (headlight_pass_rules.h). (Not on the FIXES page: that one is at the 24 row cap of a native page.)
    NativeOnly(Bool("PM_RxDbgHeadlightKeep", "mcla_gpu_headlight_pass_keep_forward", "PASS FORWARD HEADLIGHT: ")),
    // The glow of street lamps (halo and star): the game's one depth test at the glow's centre (GAME), or the
    // share of 24 tests on a small disc (SOFT: it fades at an edge instead of popping). The renderer reads the
    // cvar at each draw, so the row is live; the first time SOFT shows, the shaders are built.
    NativeOnly(Str("PM_RxDbgGlowVis",    "mcla_gpu_glow_visibility", "GLOW VISIBILITY: ",
                   kGlowVisibilityVals, kGlowVisibilityNames, Len(kGlowVisibilityVals))),
    // The upscalers' A/B levers (a shimmer on fine detail at a low base resolution): the extra texture
    // detail bias, the masks, and MSAA under the upscaler.
    NativeOnly(Dbl("PM_RxDbgUpMipBias",  "mcla_gpu_scene_extra_bias", "UPSCALER MIP BIAS: ",
                   kMipBiasVals, Len(kMipBiasVals), "%g")),
    NativeOnly(Bool("PM_RxDbgUpMasks",   "mcla_gpu_upscaler_masks", "UPSCALER MASKS: ")),
    NativeOnly(Bool("PM_RxDbgUpMsaa",    "mcla_gpu_msaa_with_upscaler", "MSAA WITH UPSCALER: ")),
    // The static-scene flicker levers (FSR's own tuning constants, mcla_gpu_fsr_velocity_factor and the two
    // after it, are cvars only): no jitter at all, FSR's own debug views, and the post chain on the
    // upscaled picture at the output's size.
    NativeOnly(Bool("PM_RxDbgUpZeroJit", "mcla_gpu_upscaler_zero_jitter", "UPSCALER NO JITTER: ")),
    NativeOnly(Bool("PM_RxDbgFsrView",   "mcla_gpu_fsr_debug_view", "FSR DEBUG VIEW: ")),
    NativeOnly(Bool("PM_RxDbgPostOut",   "mcla_gpu_upscaler_post_reads_output",
                    "POST CHAIN ON UPSCALED: ")),
    NativeOnly(Bool("PM_RxDbgMarkers",   "mcla_gpu_perf_markers",  "PERF MARKERS: ")),
    NativeOnly(Str("PM_RxDbgMarkerLvl",  "mcla_gpu_perf_markers_level", "PERF MARKERS LEVEL: ",
                   kMarkerLevelVals, kMarkerLevelNames, Len(kMarkerLevelVals))),
    NativeOnly(Dbl("PM_RxDbgGhostLog",   "mcla_gpu_ghost_log_interval", "GHOST LOG EVERY: ",
                   kGhostLogVals, Len(kGhostLogVals), "%g", "", "OFF")),
    NativeOnly(Dbl("PM_RxDbgMemLog",     "mcla_gpu_process_memory_log", "MEMORY LOG EVERY: ",
                   kMemoryLogVals, Len(kMemoryLogVals), "%g", "", "OFF")),
    NativeOnly(Dbl("PM_RxDbgVramLog",    "mcla_gpu_vram_log",      "VRAM LOG EVERY: ",
                   kMemoryLogVals, Len(kMemoryLogVals), "%g", "", "OFF")),
    // Live: Renderer::ExecutePresent asks FrameGeneration::WantsVsync at every present (the same SetVsync that follows mcla_gpu_vsync), so a
    // change reaches the swap chain at the next frame: Direct3D 12 reads it in the present call, Vulkan changes its present mode with
    // the chain's next rebuild. It said "(ON NEXT USE)" before: that was the old reading of the setting, which only the log line at start-up still does.
    // CHECK: with frame generation on, a flip shows in the PRESENT line of the frame log (immediate against FIFO) within a second, no restart of frame generation.
    NativeOnly(Bool("PM_RxDbgFgTearing", "mcla_gpu_frame_generation_tearing", "FRAME GEN TEARING: ")),
#endif
    // The SDK's vsync on the Xenia renderer: it sets the guest's vblank interval (the game then paces
    // to it, about 30 % slower) but does not stop tearing, and the start-up flags set it again at every
    // start; a test switch, not a player setting.
    XeniaOnly(Bool("PM_RxDbgXeVsync", "vsync", "XENIA VSYNC: ")),
};

// CAMERA: the developer free camera, the free-look collision test and the camera diagnostics.
const ItemDef kDebugCameraItems[] = {
    Str ("PM_RxDbgFreecam",   "debug_cam",              "FREECAM: ",
         kFreecamVals, kFreecamNames, Len(kFreecamVals)),
    Dbl ("PM_RxDbgCamSpeed",  "debug_cam_speed",        "CAM SPEED: ", kCamSpeedVals,
         Len(kCamSpeedVals), "%g"),
    Dbl ("PM_RxDbgCamSens",   "debug_cam_sens",         "FREECAM SENS: ", kCamSensVals,
         Len(kCamSensVals), "%g"),
    Bool("PM_RxDbgCamMouse",  "debug_cam_mouse",        "FREECAM MOUSE: "),
    Bool("PM_RxDbgCamCollide", "cam_freelook_collide",  "CAMERA COLLISION: "),
    Bool("PM_RxDbgCamFlip",   "cam_freelook_cs_invert", "COLLISION FLIP: "),
    Bool("PM_RxDbgCamTrace",  "cam_trace",              "CAM TRACE: "),
    Bool("PM_RxDbgMenuCamLock", "menu_cam_lock",        "MENU CAM LOCK: "),
    Dbl ("PM_RxDbgMenuCamSlot", "menu_cam_slot",        "MENU CAM SLOT: ", kMenuCamSlots.v, 64,
         "%g"),
};

// TOOLS: the modder's texture tools, the panels, a leftover test button, the trailer toolkit's HUD fade.
const ItemDef kDebugToolsItems[] = {
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Action("PM_RxDbgTexDump",   kActionDumpTextures)),
    NativeOnly(Action("PM_RxDbgTexReload", kActionReloadTextures)),
    NativeOnly(Bool("PM_RxDbgTexDumpAll",  "mcla_gpu_texture_dump", "DUMP ALL TEXTURES: ")),
#endif
    // The game's own TestMovie button on the pause screen (pause_menu.cpp Hook_EnablePMTeste).
    Bool("PM_RxDbgTestMovie", "show_test_movie", "TEST MOVIE BUTTON: "),
    // Capture mode (src/mc_engine/cinematic/capture_mode.h): the game steps by exactly 1 / this many frames a second whatever a frame takes
    // to render (the FPS limit, the step cap and frame pacing stand aside). No take and no frame gate: the world runs fast or slow.
    Dbl("PM_RxDbgFixedStep", "mcla_fixed_step_fps", "FIXED STEP: ", kFixedStepVals, Len(kFixedStepVals), "%g", "", "OFF"),
#if defined(LARECOMP_HAS_GPU)
    // A take: the fixed step at this rate, every frame read back and the game held at the frame gate until it is. It makes a take
    // folder under mcla_capture_dir (mcla_capture_out names one exactly); the sink is mcla_capture_sink (none until set).
    // A scene's capture key does the same.
    NativeOnly(Dbl("PM_RxDbgCapture", "mcla_capture_fps", "CAPTURE FPS: ", kCaptureFpsVals, Len(kCaptureFpsVals), "%g", "", "OFF")),
#endif
    // The trailer toolkit's free camera and flight recorder (cinematic/freecam.h): the pad flies the camera and the game gets no pad input
    // but Start, so close this menu to fly; Back + A on the pad records too. The file name is mcla_freecam_out.
    Bool("PM_RxDbgFlyCam",    "mcla_freecam",        "TRAILER FREE CAMERA: "),
    Bool("PM_RxDbgFlyRec",    "mcla_freecam_record", "RECORD FLIGHT: "),
    // The Director's panel (ui/director_panel.h): the timeline, the camera keys by framing, the rigs, the cars and the routes over the picture, every action a command of
    // the Director and a line of the scene file. Close this menu to use it; Ctrl+F12 flips it too (ui.bind.mcla_director). It switches the Director on itself.
    Bool("PM_RxDbgDirPanel",  "mcla_director_panel", "DIRECTOR PANEL: "),
    Action("PM_RxDbgOpenF4",     kActionDebugOpenSettings),
    Action("PM_RxDbgTelemetry",  kActionDebugTelemetry),
    Bool("PM_RxDbgSquash",       "squash_stretch", "SQUASH AND STRETCH: "),
#if defined(LARECOMP_HAS_GPU)
    // The game's own UI (HUD, map, phone, prompts, gamertags, world icons) faded in the picture; the
    // pause menu, this menu and the F3 card are never faded, so the effect shows once the menu is
    // closed. Applies to the next frame and costs nothing at 1 (src/gpu/ui_fade.h).
    NativeOnly(Dbl("PM_RxDbgHudAlpha", "mcla_cin_hud_alpha", "HUD ALPHA: ", kHudAlphaVals,
                   Len(kHudAlphaVals), "%g")),
#endif
};

// LOGGING: the log switches that can be flipped while the game runs. The restart-only diagnostics are
// launch arguments (SETTINGS-REVIEW.md B.2). Each one writes more lines (some a lot) to the log.
const ItemDef kDebugLoggingItems[] = {
    Bool("PM_RxDbgUiColorLog",  "ui_color_log",          "UI COLOR LOG: "),
    Bool("PM_RxDbgMapMouseLog", "map_mouse_diag",        "MAP MOUSE DIAG: "),
    Bool("PM_RxDbgSlipLog",     "trigger_slip_log",      "TRIGGER SLIP LOG: "),
    Bool("PM_RxDbgWheelTrace",  "wheel_trace",           "WHEEL TRACE: "),
    Bool("PM_RxDbgFfbTrace",    "wheel_ffb_trace",       "WHEEL FFB TRACE: "),
    Bool("PM_RxDbgRearDiag",    "rearview_diag",         "REARVIEW DIAG: "),
    Bool("PM_RxDbgMusicLog",    "music_bank_log",        "MUSIC BANK LOG: "),
    Bool("PM_RxDbgLzxStats",    "lzx_stats",             "LZX STATS: "),
    Bool("PM_RxDbgWarpTrace",   "warp_transition_trace", "WARP TRACE: "),
    Bool("PM_RxDbgCutProbe",    "cutscene_probe_log",    "CUTSCENE PROBE LOG: "),
    Bool("PM_RxDbgPhotoTrace",  "photo_ui_trace",        "PHOTO UI TRACE: "),
    Bool("PM_RxDbgOnlineDiag",  "online_diag",           "ONLINE DIAG: "),
    Bool("PM_RxDbgLbDiag",      "leaderboards_diag",     "LEADERBOARDS DIAG: "),
    Bool("PM_RxDbgRmrDiag",     "rmr_diag",              "RMR DIAG: "),
    Bool("PM_RxDbgLowCpuProbe", "lowcpu_probe",          "LOW CPU PROBE: "),
#if defined(LARECOMP_HAS_GPU)
    // Every 600 frames, the textures the HUD and menus read (what UI SHARPEN is tuned from).
    NativeOnly(Bool("PM_RxDbgUiSharpCensus", "mcla_ui_sharp_census", "UI SHARPEN CENSUS: ")),
#endif
};

// INFO: what differs from the defaults, and the way back.
const ItemDef kDebugInfoItems[] = {
    Action("PM_RxDbgShowChanged", kActionDebugShowChanged),
    Action("PM_RxDbgCopyArgs",    kActionDebugCopyArgs),
    Action("PM_RxDbgInfoReset",   kActionDebugReset),
};

template <std::size_t N>
const ItemDef* Rows(const ItemDef (&table)[N], int& count) {
    count = int(N);
    return table;
}

// ── The actions ────────────────────────────────────────────────────────
//
// All of this runs on the guest thread (the menu's), so the state below needs no lock.

using Clock = std::chrono::steady_clock;

// The short answer an action row shows after a click ("COPIED (3)"), for a moment.
constexpr std::chrono::milliseconds kAnswerTime{2500};
struct RowAnswer {
    ActionId id = ActionId(0);
    std::string text;
    Clock::time_point until{};
};
RowAnswer g_answer;

void Answer(ActionId id, std::string text) {
    g_answer.id = id;
    g_answer.text = std::move(text);
    g_answer.until = Clock::now() + kAnswerTime;
}

// How many debug settings are not at their default. The row asks on every draw, and the count walks
// the whole cvar registry, so it is taken at most twice a second.
// CHECK: the walk is cheap enough (about 600 cvars) that this throttle is all it needs.
constexpr std::chrono::milliseconds kCountEvery{500};
std::size_t g_changed_count = 0;
Clock::time_point g_changed_at{};
bool g_changed_valid = false;

std::size_t ChangedCount() {
    const Clock::time_point now = Clock::now();
    if (!g_changed_valid || now - g_changed_at >= kCountEvery) {
        g_changed_count = larecomp::NonDefaultDebugCvars().size();
        g_changed_at = now;
        g_changed_valid = true;
    }
    return g_changed_count;
}

void ForgetChangedCount() { g_changed_valid = false; }

// The key a bind is on, for the label: "F4" unless it was rebound. Only A-Z, 0-9, space and '+'.
std::string BindKeyLabel(const char* bind_cvar, const char* fallback) {
    const std::string key = larecomp::cvar::GetFlagByName(bind_cvar);
    std::string out;
    for (const char c : key) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u))
            out += char(std::toupper(u));
        else if (c == '+' || c == ' ')
            out += c;
    }
    return out.empty() ? std::string(fallback) : out;
}

// The text of one `--name=value` argument: the value is quoted when it has anything but plain
// characters in it.
std::string QuoteArgument(const std::string& value) {
    const bool plain = std::all_of(value.begin(), value.end(), [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '_' || c == '.' || c == '-' || c == '+' || c == ':' ||
               c == '/' || c == '\\';
    });
    if (plain) return value;
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// `--debug_menu` then `--name=value` for every debug setting that is not at its default, whichever way
// it got there: the line that brings a run back to this state (a bug report, a shortcut). A setting
// whose value is empty is left out and counted in `skipped`: the game drops its whole command line for
// an empty `--name=` (BACKLOG UW note).
std::string LaunchArguments(int& used, int& skipped) {
    std::string line = "--debug_menu";
    used = 0;
    skipped = 0;
    for (const larecomp::DebugCvarChange& change : larecomp::NonDefaultDebugCvars()) {
        if (change.value.empty()) {
            ++skipped;
            continue;
        }
        line += " --" + change.name + "=" + QuoteArgument(change.value);
        ++used;
    }
    return line;
}

// CHECK: Windows only, and from the guest thread (the clipboard has no owner window here, so it is
// handed the data at once). Elsewhere the line is in the log only.
bool CopyToClipboard(const std::string& text) {
#if defined(_WIN32)
    const int wide_count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wide_count <= 0) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, SIZE_T(wide_count) * sizeof(wchar_t));
    if (!mem) return false;
    void* locked = GlobalLock(mem);
    if (!locked) {
        GlobalFree(mem);
        return false;
    }
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, static_cast<wchar_t*>(locked), wide_count);
    GlobalUnlock(mem);
    if (!OpenClipboard(nullptr)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);  // the clipboard owns the memory only once it took it
    return ok;
#else
    (void)text;
    return false;
#endif
}

void ShowChanged() {
    const std::vector<larecomp::DebugCvarChange> changes = larecomp::NonDefaultDebugCvars();
    if (changes.empty()) {
        REXLOG_INFO("[settings] no debug setting is different from its default");
    } else {
        REXLOG_INFO("[settings] {} debug setting(s) are not at their default:", changes.size());
        for (const larecomp::DebugCvarChange& change : changes) {
            REXLOG_INFO("[settings] {} = {} ({})", change.name, change.value,
                        larecomp::CvarSourceName(change.source));
        }
    }
    ForgetChangedCount();
    Answer(kActionDebugShowChanged, "IN THE LOG (" + std::to_string(changes.size()) + ")");
}

void CopyArguments() {
    int used = 0;
    int skipped = 0;
    const std::string line = LaunchArguments(used, skipped);
    REXLOG_INFO("[debug] launch arguments for this state: {}", line);
    if (skipped > 0) {
        REXLOG_WARN("[debug] {} setting(s) with an empty value are left out of the arguments: "
                    "an empty --name= makes the game drop its whole command line",
                    skipped);
    }
    const bool ok = CopyToClipboard(line);
    if (!ok) REXLOG_WARN("[debug] the launch arguments could not be put on the clipboard");
    Answer(kActionDebugCopyArgs,
           ok ? "COPIED (" + std::to_string(used) + ")" : std::string("NOT COPIED, SEE THE LOG"));
}

// Every debug setting that is not at its default goes back to it, whichever way it got there (this
// session, the command line, larecomp.toml): this session only, the file is not touched. Left alone:
// debug_menu itself (NonDefaultDebugCvars does not list it) and any setting that is only read at start,
// which a live write would not change (or would change under the code that read it).
// The value is written through SetFlagByName so the cvar's change callback runs (aspect_ratio, hud_width
// follow their cvar through one; ResetToDefault, which writes the storage directly, would leave them on
// the old value), then ResetToDefault puts the cvar's source back to "default".
void ResetDebugSettings() {
    int reset = 0;
    int skipped = 0;
    for (const larecomp::DebugCvarChange& change : larecomp::NonDefaultDebugCvars()) {
        if (change.name == "debug_menu") continue;
        const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(change.name);
        if (!info) continue;
        if (info->lifecycle != larecomp::cvar::Lifecycle::kHotReload) {
            ++skipped;
            REXLOG_INFO("[settings] {} is only read at start: left at {} ({})", change.name,
                        change.value, larecomp::CvarSourceName(change.source));
            continue;
        }
        const std::string default_value = info->default_value;  // the entry may move once we write
        REXLOG_INFO("[settings] {} = {} -> {} (reset)", change.name, change.value, default_value);
        larecomp::cvar::SetFlagByName(change.name, default_value);
        larecomp::cvar::ResetToDefault(change.name);
        ++reset;
    }
    REXLOG_INFO("[debug] {} debug setting(s) reset to their default, {} left (read at start only)",
                reset, skipped);
    ForgetChangedCount();
    Answer(kActionDebugReset, "RESET (" + std::to_string(reset) + ")");
}

// Shows or hides an overlay (LarecompApp::StartOverlays added it as `overlay`: "mcla_settings", "mcla_telemetry"), on the UI thread: the panels are
// ImGui dialogs and are made and drawn there. 0.10 pressed the key the bind was on, so a key the player rebound was followed; the overlay's own
// toggle (IUISystem::ToggleOverlay) is the same thing without the key.
// CHECK: the dialog opens over the pause menu (the menu keeps the pad until it is closed).
void ToggleOverlay(const char* overlay) {
    rex::ui::IUISystem* ui = larecomp::sdk::UI();
    if (!ui || !ui->HasWindow()) {
        REXLOG_WARN("[debug] {} cannot be opened: the game has no window yet", overlay);
        return;
    }
    ui->PostTask([ui, name = std::string(overlay)] { ui->ToggleOverlay(name); });
}

}  // namespace

const ItemDef* DebugHubRows(int& count)         { return Rows(kDebugHubItems, count); }
const ItemDef* DebugFixesRows(int& count)       { return Rows(kDebugFixesItems, count); }
const ItemDef* DebugPerformanceRows(int& count) { return Rows(kDebugPerformanceItems, count); }
const ItemDef* DebugRenderingRows(int& count)   { return Rows(kDebugRenderingItems, count); }
const ItemDef* DebugCameraRows(int& count)      { return Rows(kDebugCameraItems, count); }
const ItemDef* DebugToolsRows(int& count)       { return Rows(kDebugToolsItems, count); }
const ItemDef* DebugLoggingRows(int& count)     { return Rows(kDebugLoggingItems, count); }
const ItemDef* DebugInfoRows(int& count)        { return Rows(kDebugInfoItems, count); }

std::string DebugActionLabel(ActionId id) {
    if (g_answer.id == id && Clock::now() < g_answer.until) return g_answer.text;
    switch (id) {
    case kActionDebugShowChanged:
        return "SHOW CHANGED SETTINGS (" + std::to_string(ChangedCount()) + ")";
    case kActionDebugCopyArgs:
        return "COPY LAUNCH ARGUMENTS";
    case kActionDebugReset:
        return "RESET DEBUG SETTINGS";
    case kActionDebugOpenSettings:
        return "OPEN SETTINGS PANEL (" + BindKeyLabel("bind_mcla_settings", "F4") + ")";
    case kActionDebugTelemetry:
        return "TELEMETRY CARD (" + BindKeyLabel("bind_mcla_telemetry", "F3") + ")";
    default:
        break;
    }
    return "";
}

void DebugActionRun(ActionId id) {
    switch (id) {
    case kActionDebugShowChanged:    ShowChanged(); break;
    case kActionDebugCopyArgs:       CopyArguments(); break;
    case kActionDebugReset:          ResetDebugSettings(); break;
    case kActionDebugOpenSettings:   ToggleOverlay("mcla_settings"); break;
    case kActionDebugTelemetry:      ToggleOverlay("mcla_telemetry"); break;
    default:                         break;
    }
}

}  // namespace mcla_menu

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include <rex/chrono/clock.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif
#include "imgui.h"
#include "logging.h"
#include "hooks.h"
#include "discord_rpc/discord_rpc.h"
#include "graphics_button.h"
#include "larecomp_log.h"
#include "menu_camera.h"
#include "modloader/modloader.h"
#include "mp3custom/mp3custom.h"
#include "hud_units.h"
#include "online/online_common.h"  // shared guest-memory helpers (IsGuestPtr, ...)

// CVAR DEFINITIONS (Will appear in F4 menu)
// The '.lifecycle(kRequiresRestart)' forces the user to restart the game if they change the value.

REXCVAR_DEFINE_BOOL(skip_intro, false, "MCLA/Patches", "Skip the intro videos to prevent graphical issues.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// BadassBaboon's Recomp Adjustments: Enable 60 FPS by default
REXCVAR_DEFINE_BOOL(fps_60, true, "MCLA/Patches", "Increases vsync target to 60 FPS and enables deltatime.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(disable_motion_blur, false, "MCLA/Patches", "Disable Motion Blur completely.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_imposter_shadows, true, "MCLA/Patches", "Performance Mode: Foliage won't cast shadows.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Diagnostic: dumps every tune field as it registers. Off by default -- it fires for
// every tune in the game (about 1100 unique names across ~28 classes) -- but it is the
// only way to see a tune's live layout, so it stays.
REXCVAR_DEFINE_BOOL(tune_field_probe, false, "MCLA/Diagnostics",
                    "Log every tune field name as it registers.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(disable_msaa, false, "MCLA/Patches", "Disable Anti-Aliasing (MSAA).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// NOTE: the online cvars (online_ignore_content_check, online_diag) moved to
// src/mc_engine/online/system_link.cpp along with the hooks that use them.

REXCVAR_DEFINE_BOOL(break_pairwise_collision, false, "MCLA/Patches", "Disables pairwise collision resolution.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_rubberbanding, false, "MCLA/Patches",
    "Disable the AI RubberBand system outright. Note this also removes MinThrottle, the "
    "leash that slows the AI when it is AHEAD — so it makes races harder, not fairer. "
    "Prefer the rubberband_* scales under MCLA/Difficulty.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// ── AI rubberband scales ────────────────────────────────────────────────────
// Applied on top of whatever $/tune/career/RubberBandTune00..10 loaded, to all
// 11 entries. 1.0 leaves the tune exactly as shipped.
REXCVAR_DEFINE_BOOL(rubberband_dump, false, "MCLA/Difficulty",
    "Write every RubberBandTune entry to <exe>/rubberband_dump.txt and flip back off. "
    "Read it before picking the scales below.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_catchup_scale, 1.0, "MCLA/Difficulty",
    "Scales how hard the AI pulls when it is BEHIND: MaxThrottle, BehindMaxThrottle, "
    "HomeStretchBehindMaxThrottle, PlayerCloseMaxThrottle. Below 1 = less catch-up.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_holdback_scale, 1.0, "MCLA/Difficulty",
    "Scales MinThrottle, the throttle cap while the AI is AHEAD. Below 1 = the leader "
    "backs off harder; above 1 = it runs away more freely.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The 11 RubberBandTune entries are difficulty levels. sub_82743B30
// (RubberBandMgr_SetupRace) picks one into mgr+60: it starts from the race's
// base (mgr+56) and then, when the race got clipped down to the racer's own
// level, subtracts mgr+68 (2) — or adds mgr+72 (3) when the racer is below what
// the race asked for. The game's own `rubberband` dev switch (node 0x8290AC10)
// overrides that index outright, clamped 0..10, which is what this reproduces —
// written every frame so it also survives the restore path at the end of setup.
REXCVAR_DEFINE_INT32(rubberband_level, -1, "MCLA/Difficulty",
    "Force which RubberBandTune level the AI races at: -1 leaves the game's own choice, "
    "0 = easiest, 10 = hardest. This is the direct way to make races harder — the tune "
    "index the game normally derives from your racer level and the race's base.")
    .range(-1, 10)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_distance_scale, 1.0, "MCLA/Difficulty",
    "Scales the distance bands the rubberband reacts within: MinDist, MaxDist, "
    "BehindMinDist, BehindMaxDist. Larger = the band engages from further away.")
    .range(0.1, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// AI nitrous policy. The tune gates nitrous on four distances; raising them
// makes the AI hold it instead of dumping the bottle off the line.
//
// Direction of each comparison is NOT confirmed by RE — the names and offsets
// are (parser registration sub_823990E0), and LoadTuning rescales two of them
// (+48 *= 2, +56 *= 2.5) after parsing. Read rubberband_dump.txt for the real
// numbers and try one knob at a time.
// Absolute, not a scale: measured across all 11 levels, UseNitroMinDistFromStart
// is a flat 100 at every one of them — the only field in the struct with no
// difficulty ramp. Scaling a constant tells you nothing about what you asked
// for, and 100 is short enough that even x8 lands inside the opening straight.
REXCVAR_DEFINE_DOUBLE(rubberband_nitro_start_dist, -1.0, "MCLA/Difficulty",
    "UseNitroMinDistFromStart in world units: how far into the race the AI must be "
    "before it may touch nitrous. Stock is 100 at every difficulty level, which is why "
    "they empty the bottle off the line. -1 keeps stock.")
    .range(-1.0, 8000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_nitro_finish_scale, 1.0, "MCLA/Difficulty",
    "Scales UseNitroMinDistFromFinish, the gate tied to the run-in to the finish.")
    .range(0.0, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_nitro_behind_scale, 1.0, "MCLA/Difficulty",
    "Scales UseNitroMinDistBehindPlayer and UnlimitedNitroMinDistBehindPlayer — how far "
    "behind you the AI has to be before it spends nitrous, and before it gets unlimited.")
    .range(0.0, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(dbg_print, false, "MCLA/Patches", "Enable DbgPrint console outputs.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(unlock_ride_height, false, "MCLA/Patches", "Allow the full stock ride-height table in the shop (down to rh_800 / -8).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(unlock_wheel_fit, false, "MCLA/Patches", "Allow every stock rim size, tire profile, tire width and ride height regardless of the car's clearance metrics.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(physics_noclip, true, "MCLA/Physics", "Disable CCD/Pairwise Collision (Noclip)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(disable_dof, false, "MCLA/Patches", "Disable Depth of Field (DoF) completely.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(fov_1p_scale, 1.0, "MCLA/Camera", "FOV scale — 1st person / cockpit (0.5 = narrower, 2.0 = wider)")
    .range(0.5, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(fov_3p_scale, 1.0, "MCLA/Camera", "FOV scale — 3rd person / chase cam (0.5 = narrower, 2.0 = wider)")
    .range(0.5, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Continuous exponential camera boom smoothing at 60 FPS
REXCVAR_DEFINE_BOOL(smooth_chase_cam, true, "MCLA/Camera",
    "Fix: Smooth chase camera boom interpolation at 60 FPS using continuous-time exponential decay.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(chase_cam_smoothing_factor, 1.0, "MCLA/Camera",
    "Chase camera boom smoothing factor multiplier (0.1 - 3.0).")
    .range(0.1, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Vehicle chassis suspension damping & ground depth continuous filter
REXCVAR_DEFINE_BOOL(smooth_chassis_depth, true, "MCLA/Physics",
    "Fix: Smooth vehicle chassis suspension and ground depth damping at 60 FPS.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Ambient traffic & pedestrian density tuning for city performance
REXCVAR_DEFINE_BOOL(enable_ambient_tuning, true, "MCLA/Performance",
    "Enable ambient traffic and pedestrian density tuning.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(traffic_unspawn_dist, 250.0, "MCLA/Performance",
    "Traffic vehicle unspawn radius in meters (default 400.0, lower = higher FPS in city).")
    .range(100.0, 600.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(ped_density_scale, 0.5, "MCLA/Performance",
    "Pedestrian density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(parked_car_scale, 0.5, "MCLA/Performance",
    "Parked car density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ── Render phase culling ────────────────────────────────────────────────────
// These reactivate the game's own dev command-line switches (see the debug
// option block further down). They are not new code paths: the retail renderer
// still contains every branch, only the switch that reaches it was stripped.
//
// perf_no_shadows is the big one. sub_822E47E0 (the renderer ctor) reacts to it
// with `phase_mask &= 0xFFFF9E1F`, clearing render phase bits 0x20 0x40 0x80
// 0x100 0x2000 0x4000 in the enable mask at renderer+448. The phase loop in
// sub_822E6408 only dispatches a phase's draw lists when its bit is set, so
// those passes stop existing entirely: no shadow-map traversal, no
// shadowDepth/shadowAlphaDepth/shadowBlend technique draws, no shadow render
// targets. Restart-only because the mask is computed once, at renderer init.
REXCVAR_DEFINE_BOOL(perf_no_shadows, false, "MCLA/Performance",
    "Drop every real-time shadow render phase. Largest single framerate win; the world "
    "loses cast shadows. Applied straight to the renderer's phase enable mask, so it "
    "takes effect immediately.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Which render phase bits perf_no_shadows clears.
//
// The game's own `noshadows` clears 0x61E0 (0x20 0x40 0x80 0x100 0x2000 0x4000)
// — the sun cascade phases. Measured with the clock held at midnight, none of
// those appear in renderer+364 at all, so clearing them changes nothing. The
// shadow that is actually drawn comes from phase 0x400: sub_823120C8's case 4
// runs when `(mask & 0x20)` OR `((mask & 0x400) && night)`, and sub_823112C0
// routes 0x400 to shadowNight / shadowFastBlend. `noshadows` never touches it.
//
// Default is therefore 0x65E0 = the stock set plus 0x400, so one setting covers
// both the day (cascade) and night (blend) path.
//   0x61E0  stock noshadows — sun cascades only
//   0x400   the night/blend shadow phase only
//   0x65E0  both
//   0x7DF0  everything sub_823120C8 dispatches on; also takes 0x10/0x200/0x800,
//           which are unrelated live passes, so it corrupts the frame
static constexpr uint32_t kShadowPhaseBitsDefault = 0x65E0u;

REXCVAR_DEFINE_STRING(perf_shadow_phase_bits, "0x65E0", "MCLA/Performance",
    "Render phase bits perf_no_shadows clears in renderer+448 (hex). 0x65E0 = sun cascades "
    "(0x61E0, what the game's own 'noshadows' clears) plus the night/blend shadow phase "
    "0x400 that it misses. 0x400 alone isolates the night path.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(perf_no_race_shadows, false, "MCLA/Performance",
    "Drop the shadow pass during races only (dev switch 'noraceshadows').")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(perf_fast_vehicle_shadows, false, "MCLA/Performance",
    "Cheap blob shadow under vehicles instead of the real-time one (dev switch "
    "'fastVehShadows'). Use instead of perf_no_shadows to keep world shadows.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Load-time, not per-frame: sub_8230B778 skips allocating the ImpostorDepth /
// ShadowImpostor / ImpostorColor / ImpostorNormal render targets. node+4 is kept
// in sync with this cvar every frame, so the change lands the next time the
// impostor system loads rather than needing a process restart.
REXCVAR_DEFINE_BOOL(perf_no_impostors, false, "MCLA/Performance",
    "Do not allocate the foliage impostor render targets (dev switch 'noimpostors'). "
    "Distant trees lose their billboards. Applies on the next load of that system.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Same shape: sub_82310478 skips the prop parse for $/city/<district> entirely.
REXCVAR_DEFINE_BOOL(perf_no_trees, false, "MCLA/Performance",
    "Skip loading the prop/foliage set (dev switch 'notrees'). Last resort. Applies on "
    "the next district load.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(perf_no_fullscreen_blur, false, "MCLA/Performance",
    "Skip the full-screen blur pass (dev switch 'nofsblur').")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// ── rage::fragTuneStruct overrides ──────────────────────────────────────────
// Both values live in the fragment tune the game parses out of
// $/tune/types/fragments (fragment = breakable prop: poles, signs, fences,
// barriers). They are patched in the parsed struct at runtime, so no RPF edit
// and no decryption is involved. 0 keeps whatever the tune file loaded.
REXCVAR_DEFINE_DOUBLE(global_max_draw_distance, 0.0, "MCLA/Performance",
    "fragTuneStruct::GlobalMaxDrawingDistance — draw distance for breakable props "
    "(poles, signs, fences). 0 = keep the tune file's value (3000); the engine's own "
    "constructor default is 250.")
    .range(0.0, 6000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(breaking_frame_rate_limit, 0.0, "MCLA/Performance",
    "fragTuneStruct::BreakingFrameRateLimit — framerate floor under which the engine "
    "stops spawning new fragment breaks. 0 = keep the tune file's value (10); the "
    "engine's own constructor default is 30.")
    .range(0.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Steering physics and frame rate limiter CVARs
REXCVAR_DEFINE_BOOL(scale_steering_with_fps, true, "MCLA/Controls",
    "Scale vehicle steering delta to maintain consistent handling response at 60 FPS.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(steering_sensitivity, 1.0, "MCLA/Controls",
    "Vehicle steering sensitivity multiplier (0.2 = tighter, 1.0 = stock, 2.0 = faster).")
    .range(0.2, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Default to 60 FPS: with vsync=false for low input lag and fast pacing,
// the precision frame limiter caps to 60 FPS out-of-the-box.
REXCVAR_DEFINE_INT32(fps_limit, 60, "MCLA/Performance",
    "Frame rate cap (0 = uncapped, 60 = 60 FPS, 120 = 120 FPS, 144 = 144 FPS).")
    .range(0, 360)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(lod_traffic_scale, 1.0, "MCLA/LOD", "Escala de LOD do Tráfego (0.1 - 10.0)")
    .range(0.1, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(lod_city_scale, 1.0, "MCLA/LOD", "Escala de LOD da Cidade (0.1 - 10.0)")
    .range(0.1, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "MCLA/Patches", "Screen Aspect Ratio")
    .allowed({"16:9", "16:10", "21:9", "32:9"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(rexglue_settings_in_gameoptions, false, "MCLA/Settings",
    "Replace Game Options with ReXGlue Settings overlay (F4)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(single_tile, false, "MCLA/Performance",
    "Render the scene in a single predicated-tiling tile instead of two. Halves draw calls "
    "and state traffic with MSAA on. Requires the enlarged virtual EDRAM (SDK >= this build).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(debug_cam, "off", "MCLA/Camera",
    "Free-fly camera during live gameplay: left stick moves, right stick looks, triggers change "
    "speed. Gameplay keeps running underneath (drive, traffic, physics).")
    .allowed({"off", "free"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_speed, 40.0, "MCLA/Camera",
    "Free-fly camera move speed (world units/second).")
    .range(1.0, 500.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_sens, 2.5, "MCLA/Camera",
    "Free-fly camera look sensitivity (radians/second at full stick).")
    .range(0.2, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(debug_cam_mouse, true, "MCLA/Camera",
    "Free-fly camera: use the mouse to look (captures the cursor while active).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_mouse_sens, 0.0025, "MCLA/Camera",
    "Free-fly mouse look sensitivity (radians per mouse pixel).")
    .range(0.0002, 0.02)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(speed_units, "kmh", "MCLA/HUD",
    "Speedometer / distance units. Drives the game's own metric formatter "
    "(sub_8238DDF0): game = console profile default (mph on NTSC), kmh = metric "
    "(km/h, km, m), mph = imperial (mph, miles, ft). Converts both the number and "
    "the unit label; the analog dial tick art stays as authored.\n"
    "kmh also converts the live HUD speedometer, which does NOT go through that "
    "formatter: the HUD movie multiplies m/s by 2.237 in its own ActionScript, so "
    "hud_units.cpp rewrites that constant (and the per-glyph unit label) in guest "
    "memory. The radar detector's speed-limit sign stays in mph -- its number comes "
    "from the guest and the game's metric branch for it is dead code.")
    .allowed({"game", "kmh", "mph"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(button_prompts, "xbox", "MCLA/UI",
    "On-screen button glyphs. xbox = A/B/X/Y + LB/RB/LT/RT, playstation = "
    "cross/circle/square/triangle + L1/R1/L2/R2. Both sets already ship inside "
    "the game's own UI movies and texture dictionary, so this only flips the "
    "'platform' flag their ActionScript reads - no asset is swapped or reloaded. "
    "Menus already on screen keep their old glyphs until reopened, and the boot "
    "legal screen switches to its PS3 text page.")
    .allowed({"xbox", "playstation"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(lzx_stats, false, "MCLA/Debug",
    "Measure pgStreamer LZX decompression (XMemDecompressStream): per-2s window stats "
    "appended to <exe>/lzx_stats.txt. For diagnosing streaming stutter (South Central).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(extra_vinyl_layers, false, "MCLA/Garage",
    "Raise the front/rear BUMPER vinyl caps from 16 to 31 layers each "
    "(64/64/64/31/31). Top and side caps stay at 64 because MCLA's vinyl "
    "composite pipeline hard-caps a single surface at 64 layers (fixed-64 "
    "work-area buffers; >64 overflows and crashes). Bumpers stay <=64 and keep "
    "their 5-bit save field, so existing garage/online cars are 100% compatible.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(export_vinyl, false, "MCLA/Garage",
    "Export the current car's vinyl layers to a .vgp file in <exe dir>/vinyls/ "
    "(auto-named vinyl_<car>_<timestamp>.vgp). Toggle ON while in the garage with "
    "a car loaded; it fires once and flips back OFF. Share the file; import later.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(import_vinyl, "", "MCLA/Garage",
    "Import a .vgp vinyl package onto the current car. Type the file name (found "
    "in <exe dir>/vinyls/, with or without the .vgp extension) and press Enter "
    "while in the garage with a car loaded. It overwrites the car's current vinyl "
    "layers and re-composites, then clears the field. Layers past the active "
    "per-surface cap are dropped (enable extra_vinyl_layers first for 31 bumper "
    "slots).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vinyl_auto_readback, true, "MCLA/Garage",
    "Fix: car vinyls only load outside the Vinyl Editor when GPU readback is on "
    "(the game builds the decal texture on the CPU from a GPU composite). This "
    "briefly enables d3d12_readback_resolve for ~1.5s around each vinyl "
    "(re)composite (garage entry / edits) so decals appear everywhere, while "
    "keeping readback OFF during racing for full performance. Leave ON.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(photo_auto_readback, true, "MCLA/PhotoMode",
    "Fix: photo mode previews show an old frame or garbage. The game takes its "
    "picture on the CPU, by locking the front buffer and JPEG-encoding it, so it "
    "needs the GPU resolve copied back to guest memory. This asks for a readback "
    "of just the two front buffers for a few frames around each shot, instead of "
    "the global readback_resolve cvar which stalls every resolve of every frame. "
    "Leave ON.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(photo_readback_debug, false, "MCLA/PhotoMode",
    "Diagnostics for photo_auto_readback: logs the photo album state machine's "
    "phase transitions and the front buffer addresses the readback is armed for. "
    "Pair with the GPU-side gpu_log_resolve_readback_misses to see whether the "
    "resolve that fills the front buffer actually lands in the armed range.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(dump_vinyl_shapes, false, "MCLA/Garage",
    "Dump the vinyl shape catalog (every ShapeIdx -> source bitmap filename + "
    "texture header) to <exe dir>/vinyls/vinyl_shapes.json. Enter the Vinyl editor "
    "once so the shape library loads, then toggle ON; it fires once and flips back "
    "OFF. Used to build the image->vinyl decomposer's brush set.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(capture_vinyl_shapes, false, "MCLA/Garage",
    "Hands-free: force-load every vinyl shape a few at a time, hash each, and "
    "build the full ShapeIdx->hash map in vinyls/vinyl_shape_hashes.txt (+ .json) "
    "— no manual browsing. Enter the Vinyl editor (main menu, not a picker grid), "
    "then toggle ON; it sweeps all ~894 shapes over a few seconds and logs when "
    "done. Join the hashes to the DDS the texture dumper already wrote.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Function to apply/revert the Aspect Ratio patch in GPU memory
static void ApplyAspectRatioPatch(std::string_view ratio) {
    extern uint8_t* g_guest_mem;
    if (!g_guest_mem) return;
    
    uint8_t* patch_ptr = g_guest_mem + 0x8201E7EC;
    
    LARECOMP_APP_INFO("ApplyAspectRatioPatch called! Ratio: {}, Memory Before: {:02X} {:02X} {:02X} {:02X}", 
        ratio, patch_ptr[0], patch_ptr[1], patch_ptr[2], patch_ptr[3]);
    
    uint32_t val = 0x3FE38E39; // 16:9 Default (1.777777f)
    if (ratio == "16:10") {
        val = 0x3FCCCCCD; // 16:10 (1.600000f) -> Adicionado aqui
    } else if (ratio == "21:9") {
        val = 0x40155555; // 21:9 (2.333333f)
    } else if (ratio == "32:9") {
        val = 0x40638E39; // 32:9 (3.555555f)
    }
    
    // Write the 4 bytes in Big-Endian at the correct address
    patch_ptr[0] = (val >> 24) & 0xFF; // MSB
    patch_ptr[1] = (val >> 16) & 0xFF;
    patch_ptr[2] = (val >> 8)  & 0xFF;
    patch_ptr[3] = val         & 0xFF; // LSB
    
    LARECOMP_APP_INFO("Memory After: {:02X} {:02X} {:02X} {:02X}", 
        patch_ptr[0], patch_ptr[1], patch_ptr[2], patch_ptr[3]);
}

// Vinyl (decal) layer caps. MCLA stores car decals in a fixed 288-slot layer
// array (each layer = 20 bytes) partitioned across 5 surfaces by two parallel
// 5-entry tables in guest .data/.rdata:
//   dword_820510B0 @ 0x820510B0 = per-surface capacity  {64,64,64,16,16}
//   dword_827E9770 @ 0x827E9770 = per-surface start slot {0,64,128,192,208}
// with start[i+1] = start[i] + cap[i]. Every accessor (sub_82393118 &c.) reads
// these live, and the work-area ctor (sub_82370C38) copies the caps + allocates
// per-surface used-buffers of cap[i] bytes ONCE at garage init — so this patch
// must land before the garage is entered (InitHooks, at startup) and requires a
// restart to take effect.
//
// New layout 85/85/86/16/16 (offsets 0/85/170/256/272) sums to exactly 288, so
// it fills the existing array without any reallocation or struct growth. The
// per-surface save COUNT is serialized in bit-width(cap) bits (sub_82395A50 /
// sub_823955C8): 85/86 keep the 7-bit field of 64, and the bumpers stay 16 (5
// bits), so the save/online bitstream layout is byte-identical -> existing
// garage cars still load. Only the raw "UserVinylData" package blob is stored by
// absolute slot; saved packages read shifted until re-saved (garage cars are fine).
static void ApplyVinylLayerCaps() {
    if (!REXCVAR_GET(extra_vinyl_layers)) return;

    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // dword_820510B0 (the capacity table) lives in the XEX read-only data
    // section — the loader maps XEX_SECTION_READONLY_DATA read-only
    // (xex_module.cpp), so a raw store there faults (0xC0000005). Flip the
    // containing page to read/write first. dword_827E9770 is in .data and
    // already writable; unprotecting it too is a harmless no-op. (Switching the
    // base pointer does NOT help — the fault is page protection, not the base.)
    auto make_writable = [&](uint32_t addr) {
        if (auto* heap = mem->LookupHeap(addr)) {
            heap->Protect(addr, 5 * sizeof(uint32_t),
                          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
        }
    };

    auto write_table = [&](uint32_t addr, const uint32_t (&vals)[5]) {
        auto* p = mem->TranslateVirtual<uint8_t*>(addr);
        for (int i = 0; i < 5; ++i) {
            uint32_t v = vals[i];
            p[i * 4 + 0] = (v >> 24) & 0xFF;  // guest memory is big-endian
            p[i * 4 + 1] = (v >> 16) & 0xFF;
            p[i * 4 + 2] = (v >> 8) & 0xFF;
            p[i * 4 + 3] = v & 0xFF;
        }
    };

    // Per-surface caps. IMPORTANT: MCLA's vinyl COMPOSITE pipeline has a hard
    // 64-layers-per-surface limit — the work-area buffers at wa+48 (malloc 256 =
    // 64 dwords) and wa+52 (malloc 64) are fixed-64 and indexed by layer up to
    // cap[surf] (sub_8236EA38), so any surface with cap > 64 overflows them and
    // crashes on the vinyl-layer menu / regen. So top/sides stay at 64. Bumpers
    // (surfaces 3,4) go 16 -> 31: still <= 64 (composite-safe) and still 5-bit
    // (16..31), so the save bitstream stays identical -> fully compatible.
    static constexpr uint32_t kCaps[5]    = {64, 64, 64, 31, 31};   // dword_820510B0
    static constexpr uint32_t kOffsets[5] = {0, 64, 128, 192, 223}; // dword_827E9770

    make_writable(0x820510B0);
    make_writable(0x827E9770);
    write_table(0x820510B0, kCaps);
    write_table(0x827E9770, kOffsets);

    LARECOMP_APP_INFO(
        "[Vinyl] Raised bumper layer caps to 64/64/64/31/31 (composite-safe, save-compatible).");
}

// ===========================================================================
// Vinyl exporter (phase 1 of custom-package import/export)
// ===========================================================================
//
// Layout (verified in IDA, default.xex): the current car's vinyl layers live in
// a heap "config block" = *(paint + 132); container = block + 64; the layer
// array starts at container + 2244. 5 surfaces (top, side1, side2, front bumper,
// rear bumper). Per-surface capacity table dword_820510B0 @ 0x820510B0, start-
// offset table dword_827E9770 @ 0x827E9770. Layer slot = array + 20*(offset[s]
// + localIdx); a slot is USED when its u16 shape-id at +16 != 0xFFFF. Each layer
// is a fixed 20-byte struct (half-float pos/scale/rot/skew + packed RGBA + flags)
// — copied verbatim, so export/import is lossless within the same game build.

// Current car's vinyl regen args, cached by Hook_CacheVinylPaint at the regen
// entry (sub_8236D850): work-area, paint object, composite texture, player index.
static std::atomic<uint32_t> g_vinyl_wa{0};
static std::atomic<uint32_t> g_vinyl_paint{0};
static std::atomic<uint32_t> g_vinyl_tex{0};
static std::atomic<uint32_t> g_vinyl_player{0};

// Windowed GPU readback around vinyl (re)composites. MCLA builds the car's decal
// texture on the CPU from a GPU composite; without readback the CPU reads stale
// physical RAM, so vinyls only show inside the Vinyl Editor. We flip
// d3d12_readback_resolve on for ~1.5s around each regen (the composite + CPU copy
// finish well within that), then off again — so racing keeps full performance.
static std::atomic<int64_t> g_vinyl_rb_deadline_ns{0};
static std::atomic_bool g_vinyl_rb_forced{false};

static int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Returns whether the guest vinyl composite is still running. The per-frame
// driver sub_8235AC78 advances a state machine on the car-model flags
// (+604/+605/+606/+607/+608/+6486) until they all clear = done, and each stage
// waits on the work-area "stage pending" byte wa+248. The full composite of all
// surfaces/layers spans many frames, so we hold readback until it's actually
// idle instead of a fixed guess. Self-contained (direct guest reads).
static bool VinylCompositeBusy() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return false;
    auto* mem = rt->memory();
    if (!mem) return false;
    auto rd8 = [&](uint32_t a) -> uint8_t { return *mem->TranslateVirtual<const uint8_t*>(a); };
    auto is_ptr = [](uint32_t ea) { return ea >= 0x10000u && ea < 0xFFFF0000u; };

    uint32_t wa = g_vinyl_wa.load(std::memory_order_relaxed);
    // wa+248 = a regen stage's GPU work pending; wa+1696 = the composite-busy
    // byte the driver itself gates stages on (sub_8236DB68).
    if (is_ptr(wa) && (rd8(wa + 248) || rd8(wa + 1696))) return true;

    const auto* p = mem->TranslateVirtual<const uint8_t*>(0x8288DCF8);  // current mcCarModel
    uint32_t car = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (!is_ptr(car)) return false;
    // Pipeline-stage flags (sub_8235AC78): full +608/+604/+605/+607/+6486 and the
    // partial-update path +606 (sub_8236D2A0, which we don't hook) — polling here
    // catches every composite trigger, including the last surface / rear bumper.
    return rd8(car + 604) || rd8(car + 605) || rd8(car + 606) || rd8(car + 607) ||
           rd8(car + 608) || rd8(car + 6486);
}

// Called from the regen hook: (re)arm the readback window. Only manages the cvar
// when the user hasn't already turned global readback on themselves.
static void ArmVinylReadbackWindow() {
    if (!REXCVAR_GET(vinyl_auto_readback)) return;
    if (!g_vinyl_rb_forced.load(std::memory_order_relaxed)) {
        if (rex::cvar::GetFlagByName("d3d12_readback_resolve") == "true") return;  // user's choice
        rex::cvar::SetFlagByName("d3d12_readback_resolve", "true");
        g_vinyl_rb_forced.store(true, std::memory_order_relaxed);
    }
    // Generous bridge until the state machine spins up; TickVinylReadbackWindow
    // then keeps it alive for as long as the composite actually runs.
    g_vinyl_rb_deadline_ns.store(NowNs() + 2'000'000'000LL, std::memory_order_relaxed);  // +2s
}

// Called every frame from Patch_DeltaTimePre. Runs regardless of how the
// composite was triggered: whenever the guest state machine is busy it (re)opens
// the readback window; it closes ~1.5s after the composite goes idle. This
// catches partial updates and late/last-surface composites (e.g. rear bumper)
// that the sub_8236D850 hook alone can miss, without leaving readback on during
// racing (flags stay 0 when no vinyl work is queued).
static void TickVinylReadbackWindow() {
    if (VinylCompositeBusy() && REXCVAR_GET(vinyl_auto_readback)) {
        if (!g_vinyl_rb_forced.load(std::memory_order_relaxed) &&
            rex::cvar::GetFlagByName("d3d12_readback_resolve") != "true") {
            rex::cvar::SetFlagByName("d3d12_readback_resolve", "true");
            g_vinyl_rb_forced.store(true, std::memory_order_relaxed);
        }
        g_vinyl_rb_deadline_ns.store(NowNs() + 1'500'000'000LL, std::memory_order_relaxed);
    }
    if (g_vinyl_rb_forced.load(std::memory_order_relaxed) &&
        NowNs() >= g_vinyl_rb_deadline_ns.load(std::memory_order_relaxed)) {
        rex::cvar::SetFlagByName("d3d12_readback_resolve", "false");
        g_vinyl_rb_forced.store(false, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Photo mode: front buffer readback
// ---------------------------------------------------------------------------
// Photo mode never touches the GPU for its picture. sub_82178B20 locks the front
// buffer texture dword_828393AC[dword_82839374] with LockRect, untiles it with
// XGUntileSurface, endian-swaps it, and hands the raw pixels to libjpeg
// (sub_8263C728, quality 85). Both the discard/save preview and the saved slot
// are decoded back from that one JPEG, so if the locked guest memory is stale
// the preview shows an old frame or garbage.
//
// The front buffer is only ever filled by the end-of-frame EDRAM resolve in
// sub_8217B7B0, which the emulator keeps GPU-side. Instead of turning on the
// global readback_resolve cvar (every resolve, every frame, full GPU drain), arm
// an address-scoped request for just the two front buffers while the photo state
// machine winds up to the capture.

// Reads the base address out of a guest D3DTexture's GPU fetch constant, the
// same field sub_82410440 feeds to LockRect: dword 1 of the fetch constant
// (texture + 32), whose top 20 bits are the base address in 4 KB pages.
//
// That address lives in the 0xE0000000 physical alias window, not in the
// physical space the resolve reports, so it still has to go through
// GetPhysicalAddress: 0xE7C47000 -> 0x07C48000 (mask to 0x1FFFFFFF plus the
// 0x1000 offset the 0xE0 heap is mapped at). Returns 0 if the pointer isn't a
// plausible object or the address isn't in a physical heap.
static uint32_t GuestTextureBaseAddress(uint32_t texture_ea) {
    if (texture_ea < 0x10000u || texture_ea >= 0xFFFF0000u) return 0;
    auto* rt = rex::Runtime::instance();
    if (!rt) return 0;
    auto* mem = rt->memory();
    if (!mem) return 0;
    const auto* p = mem->TranslateVirtual<const uint8_t*>(texture_ea + 32);
    if (!p) return 0;
    uint32_t fetch_dword =
        (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    uint32_t base_ea = fetch_dword & 0xFFFFF000u;
    if (!base_ea) return 0;
    uint32_t physical = mem->GetPhysicalAddress(base_ea);
    return physical == UINT32_MAX ? 0 : physical;
}

// Arms readback for both entries of the front buffer array. The game alternates
// dword_82839374 on every swap and the capture locks whichever is current, so
// covering only one of them would be a coin flip.
static void ArmPhotoFrontBufferReadback() {
    if (!REXCVAR_GET(photo_auto_readback)) return;
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    auto* gfx = rt->graphics_system();
    if (!mem || !gfx) return;

    const auto* array = mem->TranslateVirtual<const uint8_t*>(0x828393ACu);  // front buffers[2]
    if (!array) return;

    // 1280x720 at 32 bpp tiled is 3768320 bytes (the resolve length the GPU
    // reports); 4 MB covers it without decoding the pitch out of the fetch
    // constant. The slack matters: MCLA resolves each frame as three tiled
    // bands into the same buffer (predicated tiling, sub_8241C308), so the
    // request has to span all three, not just the base one.
    constexpr uint32_t kFrontBufferSpan = 4u * 1024u * 1024u;
    // The grab fires a few frames after the phase we arm on, and the resolve
    // that fills the buffer happened the frame before that.
    constexpr uint32_t kArmedFrames = 8;

    uint32_t bases[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const uint8_t* e = array + i * 4;
        uint32_t texture_ea =
            (uint32_t(e[0]) << 24) | (uint32_t(e[1]) << 16) | (uint32_t(e[2]) << 8) | e[3];
        bases[i] = GuestTextureBaseAddress(texture_ea);
        if (bases[i]) {
            gfx->RequestResolveReadback(bases[i], kFrontBufferSpan, kArmedFrames);
        }
    }
    if (REXCVAR_GET(photo_readback_debug)) {
        static uint32_t last_logged[2] = {0, 0};
        if (bases[0] != last_logged[0] || bases[1] != last_logged[1]) {
            last_logged[0] = bases[0];
            last_logged[1] = bases[1];
            MC_INFO("photo: armed front buffer readback, base0={:08X} base1={:08X} span={} KB",
                    bases[0], bases[1], kFrontBufferSpan >> 10);
        }
    }
}

// Entry hook on sub_8263CB78, the photo album vhsm update. r3 is the state
// object (photo album object + 456); its phase lives at +132:
//   3 = capture pipeline running (substate 9 grabs, 12 encodes, 10 finishes)
//   4 = decode the fresh JPEG into the PreviewPicture texture
//   5/6 = fade/settle frames before the grab is triggered
// Arming from 3/5/6 puts the readback window several frames ahead of the
// LockRect in sub_82178B20, which is what the front buffer resolve needs.
void Hook_PhotoModeCapture(PPCRegister& r3) {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t state_ea = static_cast<uint32_t>(r3.u64);
    if (state_ea < 0x10000u || state_ea >= 0xFFFF0000u) return;
    const auto* p = mem->TranslateVirtual<const uint8_t*>(state_ea + 132);
    if (!p) return;
    uint32_t phase = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (REXCVAR_GET(photo_readback_debug)) {
        static uint32_t last_phase = 0xFFFFFFFFu;
        if (phase != last_phase) {
            last_phase = phase;
            const auto* s = mem->TranslateVirtual<const uint8_t*>(state_ea + 136);
            uint32_t substate =
                s ? ((uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 8) | s[3])
                  : 0;
            MC_INFO("photo: vhsm phase={} substate={} (state={:08X})", phase, substate, state_ea);
        }
    }
    // Arm across the whole snapshot approach, not just the phase that grabs.
    // The grab in phase 3 substate 9 runs inside this very call, so a request
    // armed there is already too late for it - only the resolves of earlier
    // frames can still fill the buffer it locks.
    if (phase >= 2 && phase <= 6) {
        ArmPhotoFrontBufferReadback();
    }
}

void Hook_CacheVinylPaint(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    g_vinyl_wa.store(static_cast<uint32_t>(r3.u64), std::memory_order_relaxed);
    g_vinyl_paint.store(static_cast<uint32_t>(r4.u64), std::memory_order_relaxed);
    g_vinyl_tex.store(static_cast<uint32_t>(r5.u64), std::memory_order_relaxed);
    g_vinyl_player.store(static_cast<uint32_t>(r6.u64), std::memory_order_relaxed);
    ArmVinylReadbackWindow();
    static std::atomic_bool logged{false};
    if (!logged.exchange(true)) {
        LARECOMP_APP_INFO("[Vinyl] regen hook fired, wa=0x{:08X} paint=0x{:08X} tex=0x{:08X} player={}",
                          static_cast<uint32_t>(r3.u64), static_cast<uint32_t>(r4.u64),
                          static_cast<uint32_t>(r5.u64), static_cast<uint32_t>(r6.u64));
    }
}

namespace {

constexpr uint32_t kVgpMagic = 0x47565852;  // "RXVG"
constexpr uint32_t kVgpVersion = 1;
constexpr uint32_t kVinylSurfaces = 5;
constexpr uint32_t kLayerStride = 20;
constexpr uint32_t kCapsTable = 0x820510B0;
constexpr uint32_t kOffsetsTable = 0x827E9770;

// mcCarVinylShapeLibrary (dword_8288DFC4) + its shape database (dword_828CD0F8).
// Per raster category cat (0..22): count = lib[3+cat] (dword @ lib+12+4*cat),
// descriptor table = lib[26+cat] (16 bytes/entry: +0 shapeObj, +8 flags), source
// filename = names[local] where names = *(db + 96 + 8*cat). ShapeIdx = cat*1000+local.
constexpr uint32_t kShapeLib = 0x8288DFC4;
constexpr uint32_t kShapeDb = 0x828CD0F8;
constexpr uint32_t kShapeCats = 23;

// The guest-memory helpers (IsGuestPtr, GuestRead/Write*, GuestCStr) moved to
// online/online_common.cpp so the online translation units can link them too;
// online_common.h (included above) declares them. They keep external linkage.

// Resolves the current garage car's paint object: the hook-cached value first,
// else the live mcCarModel global (dword_8288DCF8; carModel+20 = paint). Returns
// 0 if no car is loaded. `src` (optional) is set to which source succeeded.
uint32_t ResolveCurrentPaint(rex::memory::Memory* mem, const char** src = nullptr) {
    uint32_t paint = g_vinyl_paint.load(std::memory_order_relaxed);
    if (IsGuestPtr(paint)) { if (src) *src = "hook"; return paint; }
    uint32_t car_model = GuestRead32(mem, 0x8288DCF8);
    if (IsGuestPtr(car_model)) {
        paint = GuestRead32(mem, car_model + 20);
        if (IsGuestPtr(paint)) { if (src) *src = "carmodel"; return paint; }
    }
    return 0;
}

// Reads the car name C-string at *(paint+76)+8; sanitizes to a filename token.
std::string ReadCarName(rex::memory::Memory* mem, uint32_t paint) {
    std::string name;
    uint32_t p76 = GuestRead32(mem, paint + 76);
    if (IsGuestPtr(p76)) {
        uint32_t str_addr = p76 + 8;
        const auto* s = mem->TranslateVirtual<const char*>(str_addr);
        for (int i = 0; i < 48 && s[i]; ++i) {
            char c = s[i];
            name += (std::isalnum(static_cast<unsigned char>(c))) ? c : '_';
        }
    }
    return name.empty() ? "car" : name;
}

std::string TimestampToken() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

}  // namespace

void ExportVinyl() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // Prefer the paint cached by the regen hook; fall back to the current
    // mcCarModel global (dword_8288DCF8, set every frame while the car renders;
    // carModel+20 = paint) so export works even if the hook hasn't fired.
    uint32_t paint = g_vinyl_paint.load(std::memory_order_relaxed);
    const char* src = "hook";
    if (!IsGuestPtr(paint)) {
        uint32_t car_model = GuestRead32(mem, 0x8288DCF8);
        if (IsGuestPtr(car_model)) {
            paint = GuestRead32(mem, car_model + 20);
            src = "carmodel";
        }
    }
    if (!IsGuestPtr(paint)) {
        LARECOMP_APP_ERROR("[Vinyl] Export: no car found (hook cache & mcCarModel both empty) "
                           "— be in the garage with the car visible.");
        return;
    }
    LARECOMP_APP_INFO("[Vinyl] Export: paint=0x{:08X} (via {})", paint, src);
    uint32_t block = GuestRead32(mem, paint + 132);
    if (!IsGuestPtr(block)) {
        LARECOMP_APP_ERROR("[Vinyl] Export: invalid vinyl block (0x{:08X}).", block);
        return;
    }
    uint32_t array = block + 64 + 2244;

    // Serialize: magic, version, carname, then per surface {cap, usedCount,
    // [localIdx u32 + 20 raw layer bytes]...}. Layer bytes are copied verbatim
    // (already big-endian in guest memory) for lossless round-trip.
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) {
        out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF); out.push_back((v >> 24) & 0xFF);
    };

    std::string car = ReadCarName(mem, paint);
    put32(kVgpMagic);
    put32(kVgpVersion);
    put32(static_cast<uint32_t>(car.size()));
    out.insert(out.end(), car.begin(), car.end());
    put32(kVinylSurfaces);

    uint32_t total_layers = 0;
    for (uint32_t s = 0; s < kVinylSurfaces; ++s) {
        uint32_t cap = GuestRead32(mem, kCapsTable + s * 4);
        uint32_t off = GuestRead32(mem, kOffsetsTable + s * 4);
        if (cap > 4096) cap = 0;  // sanity guard

        // Collect used slots first (shape-id != 0xFFFF).
        std::vector<uint32_t> used;
        for (uint32_t k = 0; k < cap; ++k) {
            uint32_t slot = array + kLayerStride * (off + k);
            if (GuestRead16(mem, slot + 16) != 0xFFFF) used.push_back(k);
        }

        put32(cap);
        put32(static_cast<uint32_t>(used.size()));
        for (uint32_t k : used) {
            uint32_t slot = array + kLayerStride * (off + k);
            const auto* p = mem->TranslateVirtual<const uint8_t*>(slot);
            put32(k);
            out.insert(out.end(), p, p + kLayerStride);
            ++total_layers;
        }
    }

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path file = dir / ("vinyl_" + car + "_" + TimestampToken() + ".vgp");

    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) {
        LARECOMP_APP_ERROR("[Vinyl] Export: cannot open {} for writing.", file.string());
        return;
    }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    LARECOMP_APP_INFO("[Vinyl] Exported {} layers to {}", total_layers, file.string());
}

// ===========================================================================
// Vinyl shape catalog dump (ShapeIdx -> source name + content hash of the GPU
// texture, matching the SDK texture dumper so the DDS files can be joined).
// Verified in IDA (default.xex, sub_8236E400 / sub_82371368):
//   lib = mcCarVinylShapeLibrary* @ kShapeLib; db = shape DB @ kShapeDb.
//   count[cat]     = lib[3+cat]   (dword @ lib + 12 + 4*cat)
//   descTable[cat] = lib[26+cat]  (16 bytes/entry: +0 shapeObj, +8 flags)
//   refcount[cat]  = *(lib[49+cat]); request-load count per shape at +4*local
//   names[cat]     = *(db + 96 + 8*cat); filename = names[local] (char*)
//   shapeObj+8     = engine tex wrapper; wrapper+0x1C -> D3DTexture; fetch @ +0x1C
// A descriptor is "loaded" when (flags & 0x30000000) == 0x30000000.
// ===========================================================================

struct ShapeRec {
    uint64_t hash = 0;
    uint32_t w = 0, h = 0;
    std::string fmt;
};

// Resident-shape probe: parse the shape's texture and content-hash its guest
// texels exactly like the SDK dumper. hash == 0 if not loaded / not decodable.
struct ShapeProbe {
    bool loaded = false;
    uint32_t w = 0, h = 0, base = 0, size = 0, pitch = 0;
    bool tiled = false;
    std::string fmt;
    rex::graphics::xenos::TextureFormat format{};
    rex::graphics::xenos::Endian endian{};
    uint64_t hash = 0;
};

static ShapeProbe ProbeShape(rex::memory::Memory* mem, uint32_t lib, uint32_t cat, uint32_t local) {
    ShapeProbe p;
    uint32_t desc_table = GuestRead32(mem, lib + 4 * (26 + cat));
    if (!IsGuestPtr(desc_table)) return p;
    uint32_t desc = desc_table + 16 * local;
    uint32_t shape_obj = GuestRead32(mem, desc + 0);
    uint32_t flags = GuestRead32(mem, desc + 8);
    p.loaded = (flags & 0x30000000u) == 0x30000000u;
    if (!p.loaded || !IsGuestPtr(shape_obj)) return p;
    uint32_t tex = GuestRead32(mem, shape_obj + 8);
    if (!IsGuestPtr(tex)) return p;
    // wrapper+0x20 = real dims (hi16 = width, lo16 = height); the D3DTexture fetch
    // reports width/height minus 1, so use the wrapper's for the DDS name/decode
    // (matches what the SDK dumper wrote for the already-browsed shapes).
    uint32_t dims = GuestRead32(mem, tex + 0x20);
    uint32_t d3dtex = GuestRead32(mem, tex + 0x1C);
    if (!IsGuestPtr(d3dtex)) return p;
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.dword_0 = GuestRead32(mem, d3dtex + 0x1C);
    fetch.dword_1 = GuestRead32(mem, d3dtex + 0x20);
    fetch.dword_2 = GuestRead32(mem, d3dtex + 0x24);
    fetch.dword_3 = GuestRead32(mem, d3dtex + 0x28);
    fetch.dword_4 = GuestRead32(mem, d3dtex + 0x2C);
    fetch.dword_5 = GuestRead32(mem, d3dtex + 0x30);
    rex::graphics::TextureInfo ti{};
    if (rex::graphics::TextureInfo::Prepare(fetch, &ti) && ti.width >= 1 && ti.width <= 4096 &&
        ti.height >= 1 && ti.height <= 4096) {
        p.w = dims >> 16;
        p.h = dims & 0xFFFF;
        if (p.w < 1 || p.w > 4096 || p.h < 1 || p.h > 4096) {
            p.w = ti.width;
            p.h = ti.height;
        }
        p.base = ti.memory.base_address;
        p.size = ti.memory.base_size;
        p.pitch = fetch.pitch;
        p.tiled = ti.is_tiled;
        p.format = ti.format;
        p.endian = ti.endianness;
        const auto* finfo = ti.format_info();
        if (finfo && finfo->name) p.fmt = finfo->name;
        const uint8_t* bytes = mem->TranslatePhysical<const uint8_t*>(p.base);
        if (bytes && p.size)
            p.hash = rex::graphics::TextureReplacement::HashGuestData(bytes, p.size);
    }
    return p;
}

static std::string ShapeName(rex::memory::Memory* mem, uint32_t db, uint32_t cat, uint32_t local) {
    uint32_t names = GuestRead32(mem, db + 96 + 8 * cat);
    if (!IsGuestPtr(names)) return {};
    return GuestCStr(mem, GuestRead32(mem, names + 4 * local), 128);
}

static std::filesystem::path VinylDir() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static std::map<uint32_t, ShapeRec> LoadShapeRecs(const std::filesystem::path& dir) {
    std::map<uint32_t, ShapeRec> recs;
    std::ifstream mf(dir / "vinyl_shape_hashes.txt");
    uint32_t midx = 0, mw = 0, mh = 0;
    uint64_t mhash = 0;
    std::string mfmt;
    while (mf >> midx >> std::hex >> mhash >> std::dec >> mw >> mh >> mfmt) {
        if (mhash) recs[midx] = ShapeRec{mhash, mw, mh, mfmt};
    }
    return recs;
}

// Writes vinyl_shape_hashes.txt (accumulated ShapeIdx->hash map) and
// vinyl_shapes.json (the full catalog joined to whatever hashes exist so far).
static void SaveShapeManifest(rex::memory::Memory* mem, uint32_t lib, uint32_t db,
                              const std::map<uint32_t, ShapeRec>& recs,
                              const std::filesystem::path& dir) {
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') o.push_back('\\');
            o.push_back(c);
        }
        return o;
    };
    std::string json = "{\n  \"shapes\": [\n";
    uint32_t total = 0;
    bool first = true;
    for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
        uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
        if (count > 100000) continue;
        for (uint32_t local = 0; local < count; ++local) {
            uint32_t idx = cat * 1000 + local;
            std::string name = ShapeName(mem, db, cat, local);
            auto it = recs.find(idx);
            uint64_t hash = it != recs.end() ? it->second.hash : 0;
            uint32_t w = it != recs.end() ? it->second.w : 0;
            uint32_t h = it != recs.end() ? it->second.h : 0;
            std::string fmt = it != recs.end() ? it->second.fmt : std::string();
            char hbuf[19];
            std::snprintf(hbuf, sizeof(hbuf), "0x%016llX", static_cast<unsigned long long>(hash));
            if (!first) json += ",\n";
            first = false;
            json += "    {\"idx\": " + std::to_string(idx) + ", \"cat\": " + std::to_string(cat) +
                    ", \"local\": " + std::to_string(local) + ", \"name\": \"" + esc(name) +
                    "\", \"w\": " + std::to_string(w) + ", \"h\": " + std::to_string(h) +
                    ", \"fmt\": \"" + fmt + "\", \"hash\": \"" + std::string(hbuf) + "\"}";
            ++total;
        }
    }
    json += "\n  ],\n  \"total\": " + std::to_string(total) +
            ",\n  \"mapped\": " + std::to_string(static_cast<uint32_t>(recs.size())) + "\n}\n";

    {
        std::ofstream mf(dir / "vinyl_shape_hashes.txt", std::ios::trunc);
        for (const auto& [rid, r] : recs) {
            char hb[17];
            std::snprintf(hb, sizeof(hb), "%016llx", static_cast<unsigned long long>(r.hash));
            mf << rid << ' ' << hb << ' ' << r.w << ' ' << r.h << ' ' << r.fmt << '\n';
        }
    }
    std::ofstream f(dir / "vinyl_shapes.json", std::ios::binary | std::ios::trunc);
    if (f) f.write(json.data(), static_cast<std::streamsize>(json.size()));
}

// Single-pass dump: hash every currently-resident shape, merge into the
// persistent map, rewrite the manifest. Use after browsing categories.
void DumpVinylShapes() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t lib = GuestRead32(mem, kShapeLib);
    uint32_t db = GuestRead32(mem, kShapeDb);
    if (!IsGuestPtr(lib) || !IsGuestPtr(db)) {
        LARECOMP_APP_ERROR("[Vinyl] Shape dump: catalog not ready (lib=0x{:08X} db=0x{:08X}) "
                           "— enter the Vinyl editor once, then retry.", lib, db);
        return;
    }
    std::filesystem::path dir = VinylDir();
    auto recs = LoadShapeRecs(dir);
    uint32_t resident = 0;
    for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
        uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
        if (count > 100000) continue;
        for (uint32_t local = 0; local < count; ++local) {
            ShapeProbe p = ProbeShape(mem, lib, cat, local);
            if (p.loaded && p.hash) {
                recs[cat * 1000 + local] = ShapeRec{p.hash, p.w, p.h, p.fmt};
                ++resident;
            }
        }
    }
    SaveShapeManifest(mem, lib, db, recs, dir);
    LARECOMP_APP_INFO("[Vinyl] Shapes: {} mapped total ({} resident this pass).", recs.size(),
                      resident);
}

// ---------------------------------------------------------------------------
// Hands-free capture: force-load every shape a few at a time via the game's own
// request-refcount (lib[49+cat][local]), hash it while resident, release it,
// advance. Runs off Patch_DeltaTimePre so async loads have frames to complete.
// Bounds memory to a small sliding window; the hash matches the DDS the SDK
// dumper already wrote, so no re-browsing / re-dumping is needed.
// ---------------------------------------------------------------------------
static std::atomic_bool g_scap_request{false};  // set by cvar callback (any thread)
static bool g_scap_active = false;               // tick-thread only below
static std::vector<std::pair<uint32_t, uint32_t>> g_scap_list;
static std::map<uint32_t, ShapeRec> g_scap_recs;
static size_t g_scap_cursor = 0;
static size_t g_scap_requested = 0;
static int g_scap_wait = 0;
static uint32_t g_scap_captured = 0;
static constexpr size_t kScapWindow = 6;   // max shapes we hold resident at once
static constexpr int kScapMaxWait = 120;   // frames to await one load before skip

void RequestVinylShapeCapture() { g_scap_request.store(true, std::memory_order_relaxed); }

// Adjust a shape's request-refcount by delta (big-endian RMW). +1 asks the
// per-frame loader to load it; -1 lets it be released. Increment (not set) so we
// never clobber the game's own reference for shapes it currently wants.
static void ShapeRefAdjust(rex::memory::Memory* mem, uint32_t lib, uint32_t cat, uint32_t local,
                           int delta) {
    uint32_t arr = GuestRead32(mem, lib + 4 * (49 + cat));
    if (!IsGuestPtr(arr)) return;
    uint32_t a = arr + 4 * local;
    int64_t v = static_cast<int32_t>(GuestRead32(mem, a));
    v += delta;
    if (v < 0) v = 0;
    GuestWrite32(mem, a, static_cast<uint32_t>(v));
}

void TickVinylShapeCapture() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t lib = GuestRead32(mem, kShapeLib);
    uint32_t db = GuestRead32(mem, kShapeDb);

    if (g_scap_request.exchange(false, std::memory_order_relaxed)) {
        if (!IsGuestPtr(lib) || !IsGuestPtr(db)) {
            LARECOMP_APP_ERROR("[Vinyl] Capture: catalog not ready — enter the Vinyl editor first.");
        } else if (!g_scap_active) {
            g_scap_list.clear();
            for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
                uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
                if (count > 100000) continue;
                for (uint32_t local = 0; local < count; ++local)
                    g_scap_list.emplace_back(cat, local);
            }
            g_scap_recs = LoadShapeRecs(VinylDir());
            g_scap_cursor = 0;
            g_scap_requested = 0;
            g_scap_wait = 0;
            g_scap_captured = 0;
            g_scap_active = true;
            LARECOMP_APP_INFO("[Vinyl] Capture: sweeping {} shapes...", g_scap_list.size());
        }
    }

    if (!g_scap_active) return;
    if (!IsGuestPtr(lib) || !IsGuestPtr(db)) return;  // catalog gone; pause this frame

    // Keep a sliding window of load requests ahead of the cursor.
    size_t want = std::min(g_scap_cursor + kScapWindow, g_scap_list.size());
    while (g_scap_requested < want) {
        auto [c, l] = g_scap_list[g_scap_requested];
        ShapeRefAdjust(mem, lib, c, l, +1);
        ++g_scap_requested;
    }

    if (g_scap_cursor < g_scap_list.size()) {
        auto [c, l] = g_scap_list[g_scap_cursor];
        ShapeProbe p = ProbeShape(mem, lib, c, l);
        if (p.loaded && p.hash) {
            g_scap_recs[c * 1000 + l] = ShapeRec{p.hash, p.w, p.h, p.fmt};
            // Force-loaded shapes are never drawn, so the SDK dump-on-sample path
            // misses them. Write the DDS ourselves via the SDK's own (proven)
            // dumper — same dump/<hash>_<w>x<h>_<fmt>.dds naming, and it skips any
            // file that already exists (already dumped while browsing).
            static rex::graphics::TextureReplacement s_repl([] {
                std::string tf = rex::cvar::GetFlagByName("texture_folder");
                return std::filesystem::path(tf.empty() ? std::string("textures") : tf);
            }());
            const uint8_t* bytes = mem->TranslatePhysical<const uint8_t*>(p.base);
            if (bytes && p.size)
                s_repl.DumpTexture(p.hash, p.w, p.h, p.pitch, p.tiled, p.format, p.endian, bytes,
                                   p.size);
            ++g_scap_captured;
            ShapeRefAdjust(mem, lib, c, l, -1);
            ++g_scap_cursor;
            g_scap_wait = 0;
        } else if (++g_scap_wait > kScapMaxWait) {
            ShapeRefAdjust(mem, lib, c, l, -1);  // give up on this one
            ++g_scap_cursor;
            g_scap_wait = 0;
        }
        return;
    }

    SaveShapeManifest(mem, lib, db, g_scap_recs, VinylDir());
    LARECOMP_APP_INFO("[Vinyl] Capture done: {} shapes mapped ({} newly captured this run).",
                      g_scap_recs.size(), g_scap_captured);
    g_scap_active = false;
    g_scap_list.clear();
    g_scap_recs.clear();
}

// Imports a .vgp package (from <exe dir>/vinyls/) onto the current garage car,
// mirroring the game's own package loader (sub_826A2520): for each surface,
// begin-edit (write container+2072=surf, +8004=1), empty every cap slot, drop in
// the imported layers verbatim, end-edit (container+2072=-1); then trigger one
// full regen by replicating sub_8236D850's work-area writes. All pure guest-
// memory writes (the block is a writable heap object) — no guest calls, so this
// is safe to run from the F4/cvar-callback thread.
void ImportVinyl(const std::string& name_in) {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // Trim surrounding whitespace/quotes from the typed name.
    std::string name = name_in;
    auto trim = [](std::string& s) {
        auto notspace = [](unsigned char c) { return !std::isspace(c) && c != '"'; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    };
    trim(name);
    if (name.empty()) return;

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::path file = dir / name;
    if (!std::filesystem::exists(file, ec))
        if (std::filesystem::exists(dir / (name + ".vgp"), ec)) file = dir / (name + ".vgp");
    if (!std::filesystem::exists(file, ec)) {
        LARECOMP_APP_ERROR("[Vinyl] Import: '{}' not found in {}.", name, dir.string());
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); ++it)
            if (it->path().extension() == ".vgp")
                LARECOMP_APP_INFO("[Vinyl]   available: {}", it->path().filename().string());
        return;
    }

    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t o = 0;
    auto rd32 = [&](uint32_t& v) -> bool {
        if (o + 4 > buf.size()) return false;
        v = uint32_t(buf[o]) | (uint32_t(buf[o + 1]) << 8) | (uint32_t(buf[o + 2]) << 16) |
            (uint32_t(buf[o + 3]) << 24);
        o += 4;
        return true;
    };

    uint32_t magic = 0, ver = 0, nlen = 0, nsurf = 0;
    if (!rd32(magic) || magic != kVgpMagic) {
        LARECOMP_APP_ERROR("[Vinyl] Import: {} is not a RexGlue .vgp (bad magic).",
                           file.filename().string());
        return;
    }
    rd32(ver);
    if (ver != kVgpVersion)
        LARECOMP_APP_INFO("[Vinyl] Import: file format v{} (importer is v{}); reading anyway.",
                          ver, kVgpVersion);
    if (!rd32(nlen) || o + nlen > buf.size()) {
        LARECOMP_APP_ERROR("[Vinyl] Import: {} is truncated.", file.filename().string());
        return;
    }
    std::string src_car(reinterpret_cast<const char*>(buf.data() + o), nlen);
    o += nlen;
    if (!rd32(nsurf) || nsurf != kVinylSurfaces) {
        LARECOMP_APP_ERROR("[Vinyl] Import: unexpected surface count ({}).", nsurf);
        return;
    }

    // Parse per-surface used layers: {cap, used, [localIdx u32 + 20 raw bytes]...}.
    struct Layer { uint32_t idx; uint8_t bytes[kLayerStride]; };
    std::vector<std::vector<Layer>> surf_layers(kVinylSurfaces);
    for (uint32_t s = 0; s < nsurf; ++s) {
        uint32_t fcap = 0, used = 0;
        if (!rd32(fcap) || !rd32(used)) {
            LARECOMP_APP_ERROR("[Vinyl] Import: truncated surface {}.", s);
            return;
        }
        (void)fcap;  // layers are placed by live caps, not the file's
        for (uint32_t i = 0; i < used; ++i) {
            uint32_t idx = 0;
            if (!rd32(idx) || o + kLayerStride > buf.size()) {
                LARECOMP_APP_ERROR("[Vinyl] Import: truncated layer data (surface {}).", s);
                return;
            }
            Layer L;
            L.idx = idx;
            std::memcpy(L.bytes, buf.data() + o, kLayerStride);
            o += kLayerStride;
            surf_layers[s].push_back(L);
        }
    }

    // Resolve the current car and its writable vinyl block.
    const char* psrc = "?";
    uint32_t paint = ResolveCurrentPaint(mem, &psrc);
    if (!paint) {
        LARECOMP_APP_ERROR("[Vinyl] Import: no car loaded — be in the garage with the car visible.");
        return;
    }
    uint32_t block = GuestRead32(mem, paint + 132);
    if (!IsGuestPtr(block)) {
        LARECOMP_APP_ERROR("[Vinyl] Import: invalid vinyl block (0x{:08X}).", block);
        return;
    }
    uint32_t container = block + 64;
    uint32_t array = container + 2244;

    uint32_t written = 0, dropped = 0;
    for (uint32_t s = 0; s < kVinylSurfaces; ++s) {
        uint32_t cap = GuestRead32(mem, kCapsTable + s * 4);
        uint32_t off = GuestRead32(mem, kOffsetsTable + s * 4);
        if (cap > 4096) cap = 0;  // sanity guard

        GuestWrite32(mem, container + 2072, s);  // sub_82392538: begin surface edit
        GuestWrite8(mem, container + 8004, 1);

        for (uint32_t k = 0; k < cap; ++k)  // empty every slot first
            GuestWrite16(mem, array + kLayerStride * (off + k) + 16, 0xFFFF);

        for (const auto& L : surf_layers[s]) {
            if (L.idx >= cap) { ++dropped; continue; }  // past the active cap
            auto* p = mem->TranslateVirtual<uint8_t*>(array + kLayerStride * (off + L.idx));
            std::memcpy(p, L.bytes, kLayerStride);
            ++written;
        }

        GuestWrite32(mem, container + 2072, 0xFFFFFFFFu);  // sub_82392548: end surface edit
    }

    // Trigger a full vinyl regen by replicating sub_8236D850(wa, paint, tex,
    // player) with plain memory writes. Work-area comes from the global
    // dword_8288DFC0 (fall back to the hook-cached r3); tex/player are the values
    // the hook captured at the last regen for this car.
    uint32_t wa = GuestRead32(mem, 0x8288DFC0);
    if (!IsGuestPtr(wa)) wa = g_vinyl_wa.load(std::memory_order_relaxed);
    uint32_t tex = g_vinyl_tex.load(std::memory_order_relaxed);
    uint32_t player = g_vinyl_player.load(std::memory_order_relaxed);
    if (IsGuestPtr(wa)) {
        GuestWrite32(mem, wa + 256, tex);     // a3: composite target texture
        GuestWrite8(mem, wa + 248, 1);
        GuestWrite32(mem, wa + 244, paint);   // a2: paint object
        GuestWrite32(mem, wa + 220, 0);
        GuestWrite32(mem, wa + 224, 0);
        GuestWrite8(mem, wa + 20, 1);
        GuestWrite8(mem, wa + 21, 1);
        GuestWrite8(mem, wa + 251, 1);
        GuestWrite32(mem, wa + 16, player);   // a4: player index
    } else {
        LARECOMP_APP_ERROR("[Vinyl] Import: work-area not ready — layers written but not "
                           "re-composited. Nudge a vinyl edit or re-enter the garage.");
    }

    LARECOMP_APP_INFO("[Vinyl] Imported {} layers ({} over-cap dropped) from '{}' (made for '{}') "
                      "onto car paint=0x{:08X} via {}.",
                      written, dropped, file.filename().string(), src_car, paint, psrc);
}

// =============================================================================
// Debug/dev command-line options (reactivated)
// -----------------------------------------------------------------------------
// MCLA retail keeps ~540 dev options (unlockall, timeofday, noshadows, money…)
// registered as dead stubs at 0x827A76E0..0x827B8598 that branch to the list
// registrar 0x821C06C8. Each stub owns a node; node+4 is the "value" slot: a
// guest char* to an ASCII value string. The stripped retail parser never fills
// it, so every consumer's guard (`if (node+4) …`) is false and the getter
// sub_821C0750 (which derefs node+4 and atois it) is never reached.
//
// Consumers read node+4 exactly once, at init/level-load (verified in IDA:
// timeofday -> mcLightingManager ctor sub_822F3498; money -> profile
// deserializer sub_826BACF0; showframerate -> render init sub_822D68F8). An
// external process that writes after boot is always too late — which is why
// post-boot injection changed nothing. InitHooks runs at startup, before those
// consumers, so populating node+4 here is equivalent to the dev command line.
//
// A pointer to "1" satisfies both consumer shapes: bool guards see nonzero, and
// the atoi getter parses the string. Value strings are parked in the dead stub
// region itself (mapped, never executed after the strip).
struct DebugOption {
    const char* name;
    uint32_t value_addr;  // guest address of node+4
};
#include "debug_options_table.inc"

// Dead registration-stub bytes double as scratch for the value strings.
static constexpr uint32_t kDbgScratchStart = 0x827A76E0u;
static constexpr uint32_t kDbgScratchEnd   = 0x827B8500u;

static const DebugOption* FindDebugOption(const std::string& name) {
    for (const auto& opt : kDebugOptions)
        if (name == opt.name) return &opt;
    return nullptr;
}

// Bump allocator over the dead stub region, shared by every writer of an option
// value so two callers never hand out the same bytes.
static uint32_t g_dbg_scratch_cursor = kDbgScratchStart;

// The stub region is part of the XEX code image, so it is mapped read+execute
// and writing a value string into it faults (guest AV at 0x827A76E0). Nothing
// ever executes from guest memory in a recomp — the code is native — so making
// these pages writable costs nothing. Done once, lazily, and a failure disables
// the whole mechanism instead of crashing.
static bool EnsureDebugScratchWritable() {
    enum class State { kUnknown, kOk, kFailed };
    static State state = State::kUnknown;
    if (state != State::kUnknown) return state == State::kOk;
    state = State::kFailed;

    constexpr uint32_t kPage = 0x1000u;
    const uint32_t lo = kDbgScratchStart & ~(kPage - 1u);
    const uint32_t hi = (kDbgScratchEnd + kPage - 1u) & ~(kPage - 1u);
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return false;

    // Reports whether the host pages backing the range really are writable now.
    // BaseHeap::Protect can return true having applied something else entirely
    // (it takes rex::memory::kMemoryProtect* flags, not the X_PAGE_* family —
    // handing it an X_PAGE_ value silently maps to kNoAccess), so the return
    // value is not evidence on its own.
    auto range_is_writable = [&]() -> bool {
#if defined(_WIN32)
        for (uint32_t a = lo; a < hi;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) return false;
            constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY |
                                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (mbi.State != MEM_COMMIT || !(mbi.Protect & kWritable)) return false;
            const uint64_t end =
                uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
            if (end <= a) return false;
            a = uint32_t(end);
        }
        return true;
#else
        return false;
#endif
    };

    auto* memory = rex::Runtime::instance()->memory();
    auto* heap = memory ? memory->LookupHeap(kDbgScratchStart) : nullptr;
    if (heap) {
        constexpr uint32_t kRW =
            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        if (heap->Protect(lo, hi - lo, kRW) && range_is_writable()) {
            state = State::kOk;
            LARECOMP_APP_INFO("[DbgOpt] scratch 0x{:08X}-0x{:08X} writable (guest heap)", lo, hi);
            return true;
        }
    }

#if defined(_WIN32)
    // The image is not always tracked by a heap whose Protect reaches the host
    // mapping. The arena is an ordinary host reservation, so protect it directly.
    DWORD old = 0;
    if (VirtualProtect(base + lo, hi - lo, PAGE_EXECUTE_READWRITE, &old) &&
        range_is_writable()) {
        state = State::kOk;
        LARECOMP_APP_INFO("[DbgOpt] scratch 0x{:08X}-0x{:08X} writable (host, was 0x{:X})",
                          lo, hi, uint32_t(old));
        return true;
    }
#endif

    LARECOMP_APP_ERROR("[DbgOpt] cannot make scratch 0x{:08X}-0x{:08X} writable; "
                       "dev options disabled", lo, hi);
    return false;
}

// Parks `value` in guest scratch and points node+4 at it — exactly the state the
// dev command line would have left. Returns false if the scratch region is
// exhausted or could not be made writable.
static bool SetDebugOptionValue(uint32_t value_addr, const std::string& value) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) {
        LARECOMP_APP_ERROR("[DbgOpt] no guest membase");
        return false;
    }
    if (!EnsureDebugScratchWritable()) return false;

    uint32_t need = static_cast<uint32_t>(value.size()) + 1;
    if (g_dbg_scratch_cursor + need > kDbgScratchEnd) {
        LARECOMP_APP_ERROR("[DbgOpt] scratch full, dropping node+4 @ 0x{:08X}", value_addr);
        return false;
    }

    const uint32_t at = g_dbg_scratch_cursor;
    for (size_t i = 0; i < value.size(); ++i)
        base[at + i] = static_cast<uint8_t>(value[i]);
    base[at + value.size()] = 0;

    // node+4 = big-endian guest pointer to that string.
    base[value_addr + 0] = static_cast<uint8_t>(at >> 24);
    base[value_addr + 1] = static_cast<uint8_t>(at >> 16);
    base[value_addr + 2] = static_cast<uint8_t>(at >> 8);
    base[value_addr + 3] = static_cast<uint8_t>(at);

    g_dbg_scratch_cursor += (need + 3u) & ~3u;
    return true;
}

// Writing node+4 from InitHooks does not survive. The registration stubs are
// ordinary guest static initializers: they run when the guest starts, which is
// after InitHooks, and the registrar sub_821C06C8 clears the slot itself —
//
//   821C06CC  stw  r4, 8(r3)        node+8   = 0
//   821C06D4  stw  r5, 0(r3)        node+0   = name
//   821C06D8  stb  r6, 0x10(r3)     node+0x10= 0
//   821C06DC  stw  r9, 4(r3)        node+4   = 0     <- wipes an early write
//   821C06E0  lwz  r11, 0x59E4(r10) list head
//   821C06E4  stw  r11, 0xC(r3)     node+0xC = next
//   821C06E8  stw  r3, 0x59E4(r10)  head     = node
//   821C06EC  blr
//
// So the requests are only collected at InitHooks time and applied from
// Patch_DevOptionRegistered, hooked on that blr with r3 still holding the node.
// That is after the game has finished building the node and before any consumer
// can read it, for every option, which is the only point where both hold.
static uint32_t ReadGuestU32(const uint8_t* base, uint32_t addr);
static void WriteGuestU32(uint8_t* base, uint32_t addr, uint32_t val);

struct PendingDebugOption {
    uint32_t value_addr;  // node+4
    std::string value;
};

static std::vector<PendingDebugOption> g_pending_options;

static void QueueDebugOption(const std::string& name, const std::string& value) {
    const DebugOption* opt = FindDebugOption(name);
    if (!opt) {
        LARECOMP_APP_ERROR("[DbgOpt] unknown option '{}'", name);
        return;
    }
    const std::string v = value.empty() ? std::string("1") : value;
    for (auto& p : g_pending_options) {
        if (p.value_addr == opt->value_addr) {  // last writer wins
            p.value = v;
            return;
        }
    }
    g_pending_options.push_back({opt->value_addr, v});
    LARECOMP_APP_INFO("[DbgOpt] queued {} = {} (node+4 @ 0x{:08X})", name, v, opt->value_addr);
}

// The MCLA/Performance cvars backed by a dev switch. perf_no_shadows is NOT
// here: its only effect is a mask edit the renderer does once, at construction,
// so it is applied directly and per-frame in ApplyRenderPhaseMask() instead —
// which also makes it take hold without a restart.
struct PerfDebugOption {
    const char* cvar;
    const char* option;
};

static constexpr PerfDebugOption kPerfDebugOptions[] = {
    {"perf_no_race_shadows",      "noraceshadows"},
    {"perf_fast_vehicle_shadows", "fastVehShadows"},
    {"perf_no_impostors",         "noimpostors"},
    {"perf_no_trees",             "notrees"},
    {"perf_no_fullscreen_blur",   "nofsblur"},
};

static void ApplyPerfDebugOptions() {
    for (const auto& p : kPerfDebugOptions) {
        if (rex::cvar::GetFlagByName(p.cvar) != "true") continue;
        QueueDebugOption(p.option, "1");
    }
}

// These switches are load-time, not per-frame state: noimpostors skips creating
// the ImpostorDepth/ShadowImpostor/ImpostorColor/ImpostorNormal render targets
// in sub_8230B778, and notrees skips the whole prop parse in sub_82310478. The
// game has no path to build or tear those down while running, so they cannot be
// toggled instantly.
//
// What this does buy: node+4 is kept in sync with the cvar every frame, so the
// consumers pick the new value up the next time their system loads — a district
// change or a world reload — instead of needing the process restarted.
//
// The scratch pointer for "1" is allocated once per option and reused, so
// flipping a switch repeatedly does not walk the bump allocator forward.
static void ApplyLoadTimeDevOptions() {
    static bool primed = false;
    static bool last[std::size(kPerfDebugOptions)] = {};
    static uint32_t value_ptr[std::size(kPerfDebugOptions)] = {};

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    for (size_t i = 0; i < std::size(kPerfDebugOptions); ++i) {
        const auto& p = kPerfDebugOptions[i];
        const bool want = rex::cvar::GetFlagByName(p.cvar) == "true";
        if (primed && want == last[i]) continue;
        last[i] = want;

        const DebugOption* opt = FindDebugOption(p.option);
        if (!opt) continue;

        if (!want) {
            WriteGuestU32(base, opt->value_addr, 0);  // exactly what the registrar leaves
        } else {
            if (value_ptr[i] == 0) {
                if (!SetDebugOptionValue(opt->value_addr, "1")) continue;
                value_ptr[i] = ReadGuestU32(base, opt->value_addr);
            } else {
                WriteGuestU32(base, opt->value_addr, value_ptr[i]);
            }
        }
        if (primed) {
            LARECOMP_APP_INFO("[DbgOpt] {} -> {} (applies on next load of that system)",
                              p.option, want ? "on" : "off");
        }
    }
    primed = true;
}

// Reads <exe dir>/debug_options.txt: one `name` or `name=value` per line,
// '#'/';' comments and blank lines ignored. Bare name means value "1".
void ApplyDebugOptions() {
    std::error_code ec;
    std::filesystem::path file = std::filesystem::current_path(ec) / "debug_options.txt";
    if (ec || !std::filesystem::exists(file, ec)) return;

    std::ifstream in(file);
    if (!in) {
        LARECOMP_APP_ERROR("[DbgOpt] cannot open {}", file.string());
        return;
    }

    int applied = 0, unknown = 0;
    std::string line;
    while (std::getline(in, line)) {
        // strip whitespace + inline comments
        auto cut = line.find_first_of("#;");
        if (cut != std::string::npos) line.erase(cut);
        auto b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t\r\n");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        std::string name = line, value = "1";
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            name = line.substr(0, eq);
            value = line.substr(eq + 1);
            auto nb = name.find_last_not_of(" \t");
            if (nb != std::string::npos) name.erase(nb + 1);
            auto vb = value.find_first_not_of(" \t");
            value = (vb == std::string::npos) ? std::string() : value.substr(vb);
        }
        if (name.empty()) continue;

        if (!FindDebugOption(name)) {
            LARECOMP_APP_ERROR("[DbgOpt] unknown option '{}'", name);
            ++unknown;
            continue;
        }
        QueueDebugOption(name, value);
        ++applied;
    }
    LARECOMP_APP_INFO("[DbgOpt] queued {} option(s) from file, {} unknown", applied, unknown);
}

static void StartFreezeWatchdog();
static void DumpRubberBandTuning();

void InitHooks() {
    // Builds xarchive_mods.rpf from models/*.obj. Must run before guest code
    // reaches sub_822C4630 and mounts the archives.
    mc::modloader::Init();

    // Scans <exe>/music and the User Music folder. The tracks are handed to
    // mcMusicManager later, from the ctor hook MCLA_CustomMusic_Install.
    InitCustomMusic();

    ApplyAspectRatioPatch(REXCVAR_GET(aspect_ratio));

    rex::cvar::RegisterChangeCallback("aspect_ratio",
        [](std::string_view name, std::string_view new_value) {
            ApplyAspectRatioPatch(new_value);
        }
    );

    ApplyVinylLayerCaps();

    // export_vinyl acts as a button: toggling it ON runs the export, then it
    // flips back OFF so it can be triggered again.
    rex::cvar::RegisterChangeCallback("export_vinyl",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                ExportVinyl();
                rex::cvar::SetFlagByName("export_vinyl", "false");
            }
        }
    );

    // rubberband_dump: button — writes the 11 parsed tune entries out and flips
    // itself back off so it can be triggered again.
    rex::cvar::RegisterChangeCallback("rubberband_dump",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                DumpRubberBandTuning();
                rex::cvar::SetFlagByName("rubberband_dump", "false");
            }
        }
    );

    // import_vinyl: type a .vgp file name + Enter to apply it, then the field
    // clears itself. The empty write re-fires this callback, hence the guard.
    rex::cvar::RegisterChangeCallback("import_vinyl",
        [](std::string_view name, std::string_view new_value) {
            if (new_value.empty()) return;
            ImportVinyl(std::string(new_value));
            rex::cvar::SetFlagByName("import_vinyl", "");
        }
    );

    // dump_vinyl_shapes: button — toggling ON writes the shape catalog manifest,
    // then flips back OFF so it can be triggered again.
    rex::cvar::RegisterChangeCallback("dump_vinyl_shapes",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                DumpVinylShapes();
                rex::cvar::SetFlagByName("dump_vinyl_shapes", "false");
            }
        }
    );

    // capture_vinyl_shapes: button — starts the hands-free sweep, which runs on
    // the per-frame tick (Patch_DeltaTimePre) and finalizes itself.
    rex::cvar::RegisterChangeCallback("capture_vinyl_shapes",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                RequestVinylShapeCapture();
                rex::cvar::SetFlagByName("capture_vinyl_shapes", "false");
            }
        }
    );

    // Reactivate the game's dev command-line options from <exe>/debug_options.txt.
    // Must run before the game's option consumers (all init/level-load reads).
    ApplyDebugOptions();

    // The MCLA/Performance cvars that map onto those same dev switches. Runs
    // after the file, so a cvar that is on overrides the same name coming from
    // debug_options.txt; a cvar that is off leaves the file's value alone.
    ApplyPerfDebugOptions();

    StartFreezeWatchdog();
}

// HOOK FUNCTIONS (Called in the middle of translated Assembly execution)

bool SkipIntro() {
    return REXCVAR_GET(skip_intro);
}

// 0x821315E4 in sub_82131508, the branch taken when dword_82830B14 (the legals
// screen object) is null:
//
//     if (dword_82830B14) { ...ordered teardown... }
//     else                  sub_822E5B00(dword_8287E064, -1);
//
// sub_822E5B00 ORs its argument into *(mgr + 424), which sub_822E5B60 folds into
// *(mgr + 364) on the next frame. That word is the render pass bitmask that
// sub_822E6408 walks bit by bit (v12 = 1; ...; v12 *= 2; while v12 != 0x40000000),
// dispatching the 13 renderers of every set bit.
//
// Every normal caller ORs 0xFEFFFFFF instead: sub_821F9918 (0x821F99BC) and
// sub_821FCED8 (0x821FCF10). Bit 24 is deliberately excluded and the game never
// sets it anywhere else. This branch's -1 does set it, so skipping the intro
// enables a pass whose stage (mgr[24]) was never prepared, and it draws with
// uninitialised state - corrupt geometry and shading across the frame.
//
// Rewrite the argument to the same mask the rest of the engine uses. Applies to
// any way of reaching a null legals object, so it is independent of which
// SkipIntro hook is active.
void MCLA_SkipIntroRenderPassMask(PPCRegister& r4) {
    if (REXCVAR_GET(skip_intro)) {
        r4.u32 = 0xFEFFFFFFu;
    }
}

static bool GetAspectRatio(double& out_val) {
    std::string ratio = REXCVAR_GET(aspect_ratio);
    if (ratio == "4:3") {
        out_val = 1.3333333;
        return true;
    } else if (ratio == "16:10") {
        out_val = 1.6000000;
        return true;
    } else if (ratio == "21:9") {
        out_val = 2.3333333;
        return true;
    } else if (ratio == "32:9") {
        out_val = 3.5555556;
        return true;
    }
    return false;
}


bool Patch_AspectRatio_82233EB4(PPCRegister& f0) {
    return GetAspectRatio(f0.f64);
}
bool Patch_AspectRatio_82214BB8(PPCRegister& f10) {
    return GetAspectRatio(f10.f64);
}
bool Patch_AspectRatio_822E5E68(PPCRegister& f12) {
    return GetAspectRatio(f12.f64);
}
bool Patch_AspectRatio_8223E5E0(PPCRegister& f13) {
    return GetAspectRatio(f13.f64);
}

bool Patch_60FPS_Jump() {
    return REXCVAR_GET(fps_60);
}

// Single-tile predicated tiling — hook at 0x8217A700 in
// grcDevice::BeginTiledRendering (sub_8217A470), the convergence point right
// after the per-orientation tile size math and before the tile rect loop.
// r7 = tile width, r8 = tile height (both feed the 160/32-aligned dimensions,
// the tile rect array and the PredictedTile RT allocation); r28/r25 = screen
// width/height. Forcing tile size = screen size makes the tile count land on
// 1, so the scene is submitted once instead of once per tile. Tile count is
// also stored at 0x827D42A4 before this point — overwrite it to 1 for the
// resolve/end path that reads the global. Needs the SDK's enlarged virtual
// EDRAM (720p 2xMSAA color+depth = 2880 tiles > the real 2048).
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r25, PPCRegister& r28) {
    if (!REXCVAR_GET(single_tile)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    r7.u64 = r28.u64;  // tile width  = screen width
    r8.u64 = r25.u64;  // tile height = screen height

    // dword_827D42A4 = tile count (already computed and stored) -> 1
    base[0x827D42A4 + 0] = 0;
    base[0x827D42A4 + 1] = 0;
    base[0x827D42A4 + 2] = 0;
    base[0x827D42A4 + 3] = 1;
}

// EDRAM capacity check bypass — sub_82410D70 (guest D3D CreateSurface,
// auto-allocation path) validates alloc_base + size_in_tiles <= 0x800 (2048,
// the real console EDRAM) at 0x82410E34 and destroys the surface / returns
// NULL past it. Single-tile 720p 2xMSAA needs 2880 tiles, so every render
// target fails and the screen collapses into aliased EDRAM bands. The SDK's
// virtual EDRAM is 4096 tiles and the guest surface header keeps 12-bit base
// fields (max 4095), so allocations up to 4096 are safe. r11 = base + size;
// returning true jumps to the success branch (0x82410E48).
bool Patch_EdramLimit(PPCRegister& r11) {
    if (!REXCVAR_GET(single_tile)) return false;
    return r11.u64 <= 4096;
}

static float ReadGuestF32(const uint8_t* base, uint32_t addr);
static void WriteGuestF32(uint8_t* base, uint32_t addr, float val);
static void ApplyAmbientDensityTuning();
static void ApplyFragTuneOverrides();
static void ApplyRenderPhaseMask();
static void ApplyRubberBandLevel();
static void ApplyRubberBandScales();
static void DumpRubberBandTuning();
static void LogRenderPhaseMaskOnce();
static void StartFreezeWatchdog();

// Bumped once per frame from Patch_DeltaTimePre; read by the freeze watchdog.
static std::atomic<uint64_t> g_frame_heartbeat{0};

// Free-fly camera host state (see Patch_DebugCam). Angles are seeded from the
// camera on activation, then integrated from the right stick each frame. The
// world position lives in guest memory at +0x50 and is read/written in place.
static bool g_freecam_seeded = false;
static float g_freecam_yaw = 0.0f;
static float g_freecam_pitch = 0.0f;

// Mouse-look for the free-fly camera. Self-contained OS mouse capture (Win32):
// while active we hide + confine the cursor, recenter it every frame, and feed
// the raw pixel delta into yaw/pitch as a displacement (no dt scaling — mouse
// deltas are already per-frame). Independent of mnk_mode, so it does not hijack
// the pad. Sign matches the right-stick path: mouse right = look right (yaw
// down), mouse up = look up (pitch down). Caller clamps pitch afterward.
#if defined(_WIN32)
static bool g_freecam_mouse_captured = false;
static void FreecamMouseUpdate(bool active, float& yaw, float& pitch) {
    auto* win = rex::Runtime::instance()->display_window();
    if (!win) return;
    HWND hwnd = static_cast<HWND>(win->GetNativeWindowHandle());
    if (!hwnd) return;

    // Release the cursor whenever an ImGui overlay (F4 RexGlue Settings, console,
    // etc.) wants the mouse, so the menu is usable; recapture once it is closed.
    // Deterministic from the live overlay state — no key-toggle to drift out of
    // sync with the menu. debug_cam_mouse is the master on/off.
    bool menu_open = ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    bool want = active && REXCVAR_GET(debug_cam_mouse) && !menu_open;

    // Client-area center in screen coordinates (recenter target).
    RECT rc;
    GetClientRect(hwnd, &rc);
    POINT center = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    ClientToScreen(hwnd, &center);

    if (want && !g_freecam_mouse_captured) {
        g_freecam_mouse_captured = true;
        win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
        win->CaptureMouse();
        SetCursorPos(center.x, center.y);  // seed center, skip first-frame spike
        return;
    }
    if (!want) {
        if (g_freecam_mouse_captured) {
            g_freecam_mouse_captured = false;
            win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
            win->ReleaseMouse();
        }
        return;
    }

    POINT cur;
    GetCursorPos(&cur);
    int dx = cur.x - center.x;
    int dy = cur.y - center.y;
    float s = static_cast<float>(REXCVAR_GET(debug_cam_mouse_sens));
    yaw -= dx * s;
    pitch += dy * s;
    SetCursorPos(center.x, center.y);
}
#else
static void FreecamMouseUpdate(bool, float&, float&) {}
#endif

static uint32_t ReadGuestU32(const uint8_t* base, uint32_t addr) {
    return (uint32_t(base[addr + 0]) << 24) | (uint32_t(base[addr + 1]) << 16) |
           (uint32_t(base[addr + 2]) << 8) | uint32_t(base[addr + 3]);
}

static void WriteGuestU32(uint8_t* base, uint32_t addr, uint32_t val) {
    base[addr + 0] = (val >> 24) & 0xFF;
    base[addr + 1] = (val >> 16) & 0xFF;
    base[addr + 2] = (val >> 8) & 0xFF;
    base[addr + 3] = val & 0xFF;
}

// Normalize an XInput thumbstick axis (int16, -32768..32767, centered at 0) to
// [-1, 1] with a small radial deadzone. XInput convention: up/right = positive.
static float NormalizeStick(int16_t raw) {
    float v = float(raw) / 32767.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (v < 0.15f && v > -0.15f) return 0.0f;
    return v;
}

// Gate bypass so the free-fly camera runs during live gameplay. In sub_822C0320
// the dcam manager block only executes when there is no gameplay camera source
// (r29 == 0, menus) or the photo-mode-active byte is set; in normal gameplay it
// is skipped, so Patch_DebugCam never fires. Hook at the gate compare
// (0x822C0644); returning true jumps straight to the manager block
// (0x822C065C), the same target as the gate-passed path (skipped instructions
// are loads/compares only). Only bypasses when the free camera is requested, so
// photo mode is untouched when debug_cam = off.
bool Patch_DebugCamGate() {
    return REXCVAR_GET(debug_cam) == "free";
}

// Free-fly camera driver. Hook at 0x822C0668: r3 = dcam manager (from
// camsys+0x33C, already null-checked), right before the manager's per-frame
// update (sub_82502E18) which in turn updates the active camera.
//
// The manager's active camera index (mgr+0, big-endian u32) selects slot
// mgr+8+index*4; index 0 (mgr+8) is the free-fly camera — its update
// (sub_82536288 -> sub_82537450) integrates position from velocity and rebuilds
// its orientation matrix every frame from scalar angles, so driving those
// scalars + position is enough to fly it. Camera object layout (offsets from
// the object pointer):
//   +0x3C float  yaw   (added to +0x38, which we keep 0)
//   +0x40 float  pitch
//   +0x50 vec4   velocity (added into position each frame; we zero it and set
//                 position directly)
//   +0x100 vec4  position [x, y, z, w]
// The game rebuilds orientation as a yaw about +Y (sub_82202E38), i.e. Y is up.
// We keep yaw/pitch/position in host state and drive them from the sticks.
void Patch_DebugCam(PPCRegister& r3) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    uint32_t mgr = static_cast<uint32_t>(r3.u64);

    if (REXCVAR_GET(debug_cam) != "free") {
        // Release: cut the blend back to the gameplay camera immediately.
        if (ReadGuestF32(base, mgr + 860) > 0.0f) {
            WriteGuestF32(base, mgr + 860, 0.0f);
        }
        g_freecam_seeded = false;
        FreecamMouseUpdate(false, g_freecam_yaw, g_freecam_pitch);  // release cursor
        return;
    }

    // Force the free-fly camera (index 0) active and fully blended in.
    base[mgr + 0] = 0;
    base[mgr + 1] = 0;
    base[mgr + 2] = 0;
    base[mgr + 3] = 0;
    WriteGuestF32(base, mgr + 860, 1.0f);
    // mgr+856 gates the per-frame camera update in sub_82502E18 (the call that
    // rebuilds the orientation matrix at +0xD0 from our angles). It is 1 in the
    // ctor but cleared outside photo mode, freezing the orientation. Force it on.
    base[mgr + 856] = 1;

    uint32_t cam = ReadGuestU32(base, mgr + 8);
    if (!cam) return;

    // Kill the camera's internal look input so the update (sub_82536288) does
    // not overwrite our yaw at +0x3C from the (unfed) photo-mode input source.
    WriteGuestU32(base, cam + 0x68, 0);

    // Seed our angle state from the camera's current orientation on activation
    // so the view does not jump. Position lives at +0x50 and is read fresh each
    // frame (below), so it needs no host-side seed.
    if (!g_freecam_seeded) {
        g_freecam_yaw = ReadGuestF32(base, cam + 0x3C);
        g_freecam_pitch = ReadGuestF32(base, cam + 0x40);
        g_freecam_seeded = true;
    }

    float dt = ReadGuestF32(base, 0x827D755C);  // clock+0x5C: raw frame dt
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

    // Read the pad straight from the host input system (the guest ioPad globals
    // are ambiguous). XInput axes: up/right positive, int16 range.
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    auto* isys = static_cast<rex::input::InputSystem*>(
        rex::Runtime::instance()->input_system());
    if (isys) {
        rex::input::X_INPUT_STATE state{};
        if (isys->GetState(0, &state) == 0) {
            lx = NormalizeStick(state.gamepad.thumb_lx);
            ly = -NormalizeStick(state.gamepad.thumb_ly);  // stick up = forward
            rx = -NormalizeStick(state.gamepad.thumb_rx);  // stick right = look right
            ry = -NormalizeStick(state.gamepad.thumb_ry);  // stick up = look up
        }
    }

    float sens = static_cast<float>(REXCVAR_GET(debug_cam_sens));
    float speed = static_cast<float>(REXCVAR_GET(debug_cam_speed));

    // Look: integrate the right stick into yaw/pitch and write them back; the
    // game rebuilds the orientation matrix (+0xD0) from these each frame.
    g_freecam_yaw += rx * sens * dt;
    g_freecam_pitch += ry * sens * dt;
    // Add mouse look on top of the stick (displacement, no dt).
    FreecamMouseUpdate(true, g_freecam_yaw, g_freecam_pitch);
    if (g_freecam_pitch > 1.5f) g_freecam_pitch = 1.5f;
    if (g_freecam_pitch < -1.5f) g_freecam_pitch = -1.5f;
    WriteGuestF32(base, cam + 0x38, 0.0f);
    WriteGuestF32(base, cam + 0x3C, g_freecam_yaw);
    WriteGuestF32(base, cam + 0x40, g_freecam_pitch);

    // Move: the world position is the vec3 at +0x50 (x, y=height, z). Read it
    // fresh (so the native button controls LB/RB/LT/RT still add in), advance it
    // along the view direction from the left stick, and write it back. Y is up
    // (orientation is a yaw about Y).
    float cy = std::cos(g_freecam_yaw), sy = std::sin(g_freecam_yaw);
    float cp = std::cos(g_freecam_pitch), sp = std::sin(g_freecam_pitch);
    float fwd_x = cp * sy, fwd_y = sp, fwd_z = cp * cy;  // forward (yaw+pitch)
    float right_x = cy, right_z = -sy;                   // horizontal strafe
    float step = speed * dt;

    float px = ReadGuestF32(base, cam + 0x50);
    float py = ReadGuestF32(base, cam + 0x54);
    float pz = ReadGuestF32(base, cam + 0x58);
    px += (fwd_x * ly + right_x * lx) * step;
    py += (fwd_y * ly) * step;
    pz += (fwd_z * ly + right_z * lx) * step;
    WriteGuestF32(base, cam + 0x50, px);
    WriteGuestF32(base, cam + 0x54, py);
    WriteGuestF32(base, cam + 0x58, pz);

    // Hand the pose to the menu-camera module so menu_cam_dump can emit the spot
    // you are flying at (position + look direction) as a front-end camera line.
    MenuCam_NoteFreecam(px, py, pz, g_freecam_yaw, g_freecam_pitch);
}

// Intro/legals pacing at 60 FPS. The intro SWF is advanced by sub_821F9918
// with a HARDCODED 1/60s per call (immune to any dt patch), and that function
// runs twice per frame (present callback sub_821FC008 + main tick
// sub_821FC588). At the console's 30 Hz that totals real time; at 60 Hz it's
// exactly 2x. Skipping every other advance (jump to the 0x821F9A00 epilogue)
// restores the original rate.
bool Hook_IntroHalfRate() {
    if (!REXCVAR_GET(fps_60)) return false;
    static uint32_t call_count = 0;
    return (call_count++ & 1) != 0;  // true = skip this advance
}

// 0x82725100, in sub_827250A8's per-movie lighting pass. The game asks the movie
// for its "lights" node and then uses the answer without checking it:
//
//   827250FC  bl sub_825ED480      r3 = the member, 0 when the movie has none
//   82725100  bl sub_825EF9F0      `return a1[2] == 5 ? *a1 : 0`  -- reads a1[2]
//   82725104  mr r26, r3
//   82725108  lwz r10, 0(r26)      -- and reads r26[0]
//
// Either read faults on a null. sub_825ED480 returns 0 while the movie's member
// table at +128 is still null, which is the state a movie is in before it has
// finished being built; on the 360 the lighting pass never runs that early, but
// under the recomp's thread timing it sometimes does. That is the intermittent
// "read of guest 0x00000008 in sub_825EF9F0" crash, and it is a null check the
// game simply does not have.
//
// So answer the question sub_825EF9F0 would have answered, and when the answer
// is null skip the block it feeds -- the hook jumps to 0x82725144, the `li r3, 1`
// that closes the scope. A movie with no lights node has nothing to light.
bool MCLA_UI_SkipMissingLights(PPCRegister& r3) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    const uint32_t node = static_cast<uint32_t>(r3.u32);
    if (base && node) {
        const uint8_t* p = base + node + 8;
        const uint32_t kind =
            (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        if (kind == 5) return false;  // a real lights node: let the game run
    }

    static uint32_t skipped = 0;
    if (++skipped <= 4) {
        MC_WARN("[ui] movie has no lights node yet (r3={:#010x}), skipping the "
                "lighting pass this frame; the game would have dereferenced it",
                node);
    }
    return true;
}

// Swap-interval patch at 0x82419AA0 ("li r11, 2"): the game always requests
// D3D interval TWO (30 FPS); replacing with 1 requests 60 Hz. Note the current
// RexGlue command processor ignores the guest swap interval (host vblank is a
// fixed 60 Hz timer), so this is kept only for correctness of the swap packet.
bool Patch_60FPS_Byte(PPCRegister& r11) {
    if (REXCVAR_GET(fps_60)) {
        r11.u64 = 1; // Replaces the original value with 1 (li r11, 1)
        return true; // Skips the original instruction
    }
    return false;
}

// The same hook works for both Motion Blur instructions!
bool Patch_DisableMotionBlur(PPCRegister& r3) {
    if (REXCVAR_GET(disable_motion_blur)) {
        r3.u64 = 0; // li r3, 0
        return true;
    }
    return false;
}

bool Patch_DisableMSAA(PPCRegister& r11) {
    if (REXCVAR_GET(disable_msaa)) {
        r11.u64 = 1; // li r11, 1
        return true;
    }
    return false;
}

// Traffic (va_) vehicles turned into player cars: chassis-bound substitution.
//
// A vp_ vehicle's physics bound is a phBoundComposite (type 12) whose five children are
// the chassis phBoundGeometry plus the four wheels. A va_ vehicle's bound is a bare
// phBoundGeometry (type 4) with no composite around it -- the split is total: none of
// the 41 va_ cars has a composite, all 65 vp_/vpd_ cars do. The bound lives in the
// car's .xtl/.xtp, not the .xct.
//
// The player-vehicle code reads *(root + 0x80) as the composite's child array without
// checking the type. On a geometry, +0x80 is m_Vertices, so the first float4 of vertex
// data is used as a phBound*; the resulting garbage object reports a polygon count > 0
// with a NULL polygon array and sub_8259FF88 faults at 0x8.
//
// Substituting the root itself when it is not a composite makes the deform pass work on
// the traffic car's own 16-vertex hull. It is a no-op for every shipped player car,
// which is why it is unconditional rather than cvar-gated.
static void SubstituteChassisBound(uint32_t root, PPCRegister& child) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base || root == 0) return;
    if (base[root + 4] != 12) child.u64 = root;
}

// 0x8232D048 in sub_8232CFF0, before `addi r6, r11, 0x10`. r9 holds root + 0x80.
void MCLA_TrafficChassisBound_8232D048(PPCRegister& r9, PPCRegister& r11) {
    SubstituteChassisBound(static_cast<uint32_t>(r9.u64) - 0x80u, r11);
}

// 0x8232D900 in sub_8232D8C8 (hull-index search), after `lwz r11, 0(r11)`. r3 is
// still the root bound returned by sub_8255B9A8.
void MCLA_TrafficChassisBound_8232D900(PPCRegister& r3, PPCRegister& r11) {
    SubstituteChassisBound(static_cast<uint32_t>(r3.u64), r11);
}

// 0x8232E274 in sub_8232E238 (suspension deform), after `lwz r31, 0(r10)`. r3 is
// still the root bound. This is the site that actually crashed.
void MCLA_TrafficChassisBound_8232E274(PPCRegister& r3, PPCRegister& r31) {
    SubstituteChassisBound(static_cast<uint32_t>(r3.u64), r31);
}

// 0x8259AA40, entry of sub_8259AA28 = phBoundComposite::ReleaseChildren, before
// `lhz r11, 0x92(r30)`. mcCarSim's destructor (sub_8232CDA8) calls it on the bound
// unconditionally, and sub_8232CEB0 does the same before re-attaching one. On a
// traffic car's bare geometry, +146 lands in the middle of the quantum-offset float
// and +128 is m_Vertices, so it walks vertex data as a child pointer array. Returning
// true jumps to the epilogue at 0x8259AA90, which is exactly right: a non-composite
// has no children to release.
bool MCLA_TrafficBoundRelease_8259AA40(PPCRegister& r30) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    const auto bound = static_cast<uint32_t>(r30.u64);
    if (!base) return false;
    return bound == 0 || base[bound + 4] != 12;
}

// Tune field registration probe. sub_824DF200(owner, type, name, &field, ...).
//
// `owner` is the class descriptor for a top-level field, but a field of type 13 is a
// nested sub-object and everything registered after it reports that sub-object as its
// owner -- so owner/field together give the tree, not a flat list.
//
// Known limit: the vehicle handling tune does NOT come through here. A full capture
// (boot through gameplay) yields ~1100 fields across cameras, HUD, effects, AI, input
// and cop lights, and no vehicle physics class at all -- no SteeringLimit, TurnBias,
// SlidingFric or OptSlipPercent. Do not spend another session looking for them here.
void MCLA_TuneFieldProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    if (!REXCVAR_GET(tune_field_probe)) return;
    const auto* base = rex::Runtime::instance()->virtual_membase();
    const auto name_addr = static_cast<uint32_t>(r5.u64);
    if (!base || !name_addr) return;

    char name[64];
    size_t n = 0;
    while (n < sizeof(name) - 1) {
        const char c = static_cast<char>(base[name_addr + n]);
        if (!c) break;
        name[n++] = c;
    }
    name[n] = '\0';
    if (n == 0) return;

    static std::set<std::string> seen;
    if (!seen.insert(name).second) return;

    LARECOMP_APP_INFO("[TuneField] {:<26} type={} owner={:#010x} field={:#010x}", name,
                      static_cast<uint32_t>(r4.u64), static_cast<uint32_t>(r3.u64),
                      static_cast<uint32_t>(r6.u64));
}

// BadassBaboon's Recomp Adjustments: Foliage imposter shadow bypass
bool Patch_DisableImposterShadows(PPCRegister& r11) {
    if (REXCVAR_GET(disable_imposter_shadows)) {
        r11.u64 = 0; // li r11, 0
        return true;
    }
    return false;
}

// Returns 'true' to inject a 'blr' (return from collision function)
bool Patch_PhysicsCollision() {
    return REXCVAR_GET(break_pairwise_collision);
}

bool Patch_DisableRubberBanding() {
    return REXCVAR_GET(disable_rubberbanding);
}

bool Patch_DisableDoF() {
    return REXCVAR_GET(disable_dof);
}

// Disable DoF at the composite. Hooked at sub_8260EBB8 entry where r3 = dofObj
// (dword_829054A0). Zeroing the circle-of-confusion vector at dofObj+0xF0 collapses
// the per-pixel blur to sharp with the scene fully intact — verified in gameplay,
// menu AND freecam. The composite runs every frame DoF is drawn, so this covers
// every state without touching the setters (blocking those left stale DoF in the
// menu). The other composite inputs (+0x158/+0x128/+0x138/+0x1B0) are NOT safe to
// zero — they white-out / desaturate the frame — so only +0xF0 is touched.
void Patch_DofComposite(PPCRegister& r3) {
    if (!REXCVAR_GET(disable_dof)) return;
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    uint32_t o = static_cast<uint32_t>(r3.u64);  // dofObj guest address
    if (!o) return;
    for (int i = 0; i < 16; ++i) base[o + 0xF0 + i] = 0;  // CoC vector = 0 -> no blur
}

void Patch_ScaleTrafficLOD(PPCRegister& f0) {
    f0.f64 = f0.f64 * REXCVAR_GET(lod_traffic_scale);
}

// Archive list injection for the modloader. sub_822C4630 mounts a ';'-separated
// list of packfiles, all at "a:/archive/". Which list it uses is decided across
// three branches (the caller's argument, the "audlo" fallback, the
// dword_8288BA44 override) that merge at 0x822C4858, and 0x822C4860 copies the
// winner into a 511-byte stack buffer at r1+0xD0. This hook sits on the
// instruction right after that copy and appends to the buffer in place.
//
// Appending rather than repointing matters twice over: it keeps whichever list
// the game picked, and it writes to the guest stack. Parking a string in the
// dead stub region instead is not an option -- those pages belong to the XEX
// image and are mapped read-only, so writing there faults.
//
// xarchive_mods.rpf ends up mounted last, and fiDevice::GetDevice
// (sub_821CB488) searches a mount point's devices last-registered-first,
// falling through when one does not hold the file -- so the mod archive
// overrides per file and everything else still comes from the shipped ones.
void Patch_ArchiveList(PPCRegister& r1) {
    constexpr uint32_t kListBufferOffset = 0xD0;  // v43 in sub_822C4630's frame
    constexpr size_t kListBufferSize = 512;       // copied with a 511-byte bound
    mc::modloader::AppendModArchiveTo(static_cast<uint32_t>(r1.u64) + kListBufferOffset,
                                      kListBufferSize);
}

// Ride height range. sub_82392F68 is the wheel-fit validator: with
//   f31 = TireRadius, f30 = RideHeight (negative = lowered)
// it rejects a setup when f31 + f30 < AxleToFloorboards ("ride too low
// (grinding floor)", result 1) or f31 - f30 > AxleToWheelwell ("tire too big
// (hitting wheel well)", result 2). Both AxleTo* values are per-car floats, so
// every car stops lowering at a different notch — typically -2 (rh_200), even
// though the stock table at off_820511C4/unk_820511F4 runs all the way to
// rh_800 (-0.15 m, i.e. -8) and the stepper sub_8269EED8 already clamps to
// 0..11. Zeroing f30 right after it is loaded (0x82392FF0) takes ride height
// out of both comparisons while leaving the rim/tire size checks - which only
// depend on f31 - exactly as shipped.
void Patch_RideHeightFit(PPCRegister& f30) {
    if (REXCVAR_GET(unlock_ride_height)) f30.f64 = 0.0;
}

// Wheel sizing range. The same validator gates all four wheel mods: the shop
// writes the picked byte (rim +2046, profile +2048, width +2044, ride +2042),
// calls sub_8269EFE0, and on a bad fit puts all four bytes back - which is why a
// car refuses larger rims or fatter tires long before the stock tables run out
// (RimSize 12..28, TireProfile 0..13, TireWidth 0..16, RideHeight 0..11). The
// three comparisons in sub_82392F68 start at 0x82392FFC; jumping straight to
// the "fits" tail at 0x823930FC reports success for every combination, so the
// menus expose their full stock lists. Cosmetic only - no geometry is created,
// the tires just clip the arches at the extremes.
bool Patch_WheelFitBypass() {
    return REXCVAR_GET(unlock_wheel_fit);
}

// Speedometer / distance units. sub_8238DDF0 is the game's unit formatter; at
// 0x8238DE54 it loads the metric flag dword_8288E5A0 into r11, then r11==0
// selects imperial ("%.1fmph"/miles/ft), r11!=0 selects metric ("%.1fk/h" with
// value*1.609344 / km / m). We replace that load with our forced value so the
// game's own metric path renders km/h — number and label both. "game" leaves
// the console profile default in place (original lwz runs).
bool Patch_SpeedUnits(PPCRegister& r11) {
    std::string units = REXCVAR_GET(speed_units);
    if (units == "kmh") { r11.u64 = 1; return true; }
    if (units == "mph") { r11.u64 = 0; return true; }
    return false;  // "game": keep the profile/region default
}

// ── Button prompt glyphs (Xbox <-> PlayStation) ──────────────────────────
//
// Every MCLA UI movie ships BOTH glyph sets, and so does the texture dictionary
// the 360 build actually loads (resources/ui/shared_latin.xtd): shared_13 is the
// PS3 circle, shared_15 the square, shared_24 the cross and shared_36 the
// triangle, sitting right next to the 360 A/B/X/Y and LB/RB/LT/RT art. The
// ActionScript picks a strip with `icon.gotoAndStop(iconID + platform * 49)`
// (pause, popup, prompt, garage, navsys, raceeditor; legals uses platform * 39
// for its text pages). Native code only ever hands the movie an abstract iconID
// (sub_82637E68: 1 = accept, 2 = cancel), so nothing on the C++ side is bound to
// the 360 art — the whole switch is one flag.
//
// (The stray resources/ui/0x989F63FD.xtd is "shared.xtd", the PS3 build's own
// sprite pack. It is dead weight: the XEX only ever loads shared_latin/shared_jp
// (sub_821FE648), and its shared_N indices do not line up with the ones the .xsf
// files import by name, so swapping it in would scramble the UI. We don't.)
//
// `platform` is an mcRegistry int the movies read at frame 1 through the
// FSCommand getvar handler (sub_82721728 -> mcVariant::asInt sub_822031A8, which
// for type 3 returns *(entry + 88)). mcUIManager's ctor builds it with a
// hard-coded 0 (sub_821FDED8, 0x821FE204..0x821FE228) and sub_821F8038 re-pushes
// the same 0 into every movie as it is created. We own both: capture the entry
// as it is built and keep +88/+92 in sync with the cvar, and feed the push.
// 0 = Xbox 360, 1 = PS3.

// mcRegistry "platform" entry, captured by Hook_PlatformVarInit.
static std::atomic<uint32_t> g_platform_var_ea{0};
// Last value written, so the tick only touches guest memory on a real change.
static std::atomic<int> g_platform_applied{-1};

static int WantedPlatformValue() {
    std::string mode = REXCVAR_GET(button_prompts);
    return mode == "playstation" ? 1 : 0;
}

// +88 (0x58) is the int mcVariant::asInt returns for type 3, +92 (0x5C) the
// plain "%d" char buffer sub_823DC018 formatted into for the string readers.
static void WritePlatformVar(uint32_t entry, int value) {
    auto* rt = rex::Runtime::instance();
    uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    if (!base || !entry) return;

    base[entry + 88] = 0;
    base[entry + 89] = 0;
    base[entry + 90] = 0;
    base[entry + 91] = static_cast<uint8_t>(value & 0xFF);

    base[entry + 92] = static_cast<uint8_t>('0' + (value & 1));
    base[entry + 93] = 0;
}

// Called once per frame from Patch_DeltaTimePre so the cvar can be flipped live
// from the F4 menu. Movies already on screen keep the glyphs they resolved at
// their own frame 1; anything opened after this picks up the new set.
void TickButtonPrompts() {
    const uint32_t entry = g_platform_var_ea.load(std::memory_order_relaxed);
    if (!entry) return;

    const int want = WantedPlatformValue();
    if (want == g_platform_applied.load(std::memory_order_relaxed)) return;

    WritePlatformVar(entry, want);
    g_platform_applied.store(want, std::memory_order_relaxed);
    MC_INFO("[buttons] prompt glyphs -> {} (platform={})",
            want ? "PlayStation" : "Xbox 360", want);
}

// mcUIManager ctor, right after the "platform" mcVariant has been created and
// its value/text written with 0 — r27 is the entry, still live for the game's
// own `stw r23, 0x9C(r27)` (type = 3) on the next instruction. Void hook: the
// original `lis r6` still runs. Applying here rather than waiting for the tick
// matters, because the same ctor goes on to load raceeditor/garage/policecam/
// credits a few instructions later.
void Hook_PlatformVarInit(PPCRegister& r27) {
    const uint32_t entry = static_cast<uint32_t>(r27.u64);
    if (!IsGuestPtr(entry)) return;

    const int want = WantedPlatformValue();
    g_platform_var_ea.store(entry, std::memory_order_relaxed);
    WritePlatformVar(entry, want);
    g_platform_applied.store(want, std::memory_order_relaxed);
    MC_INFO("[buttons] mcRegistry 'platform' @0x{:08X}, glyphs = {}", entry,
            want ? "PlayStation" : "Xbox 360");
}

// sub_821F8038 pushes aspect/lang/zone/platform into a movie as it is created.
// Replaces the `li r5, 0` at 0x821F8160 that feeds the platform push; returning
// true skips it and resumes at 0x821F8164. Measured: sub_822C2EA8 calls this
// once at boot for the intro movie only, so it is not the path the menus use —
// see Hook_SwfContextEnter below for those.
bool Patch_PlatformPush(PPCRegister& r5) {
    r5.u64 = static_cast<uint64_t>(WantedPlatformValue());
    return true;
}

// Live switching.
//
// Every UI movie reads `platform` exactly once, on its own frame 1:
//
//   _global.d_platform = 0
//   FSCommand:getvar "platform"        -> sub_82721728 writes _global.d_platform
//   _global.platform   = _global.d_platform
//
// and the movies are loaded once and kept, so the registry value alone only
// takes effect on a fresh boot. What the frame code actually evaluates on every
// render, though, is `_global.platform` itself (`icon.gotoAndStop(iconID +
// platform * 49)` reads the member each time), so writing that variable in a
// live movie switches the glyphs immediately.
//
// sub_825EE970 is the AVM's "run this action buffer" entry: it parks its
// argument in dword_828FF970 for the duration, which makes r3 a context that is
// live by definition. Pushing from there is what keeps this free of dangling
// pointers — we never store a context to write to later, we only write to the
// one being handed to us, and only when its last known value differs.
constexpr uint32_t kSwfSetVarIntFn = 0x825EE0E0;  // (ctx, name, int)
constexpr uint32_t kStrPlatform    = 0x8201ABDC;  // the XEX's own "platform"

struct SwfPlatformCtx {
    uint32_t ctx = 0;
    int value = -1;
};
constexpr int kMaxSwfCtx = 16;
static SwfPlatformCtx g_swf_ctx[kMaxSwfCtx];
static int g_swf_ctx_count = 0;

static uint32_t CallGuestFn3(uint32_t fn_addr, uint32_t a0, uint32_t a1, uint32_t a2) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return 0;
    PPCFunc* fn = rt->function_dispatcher()->GetFunction(fn_addr);
    if (!fn) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, a0, a1, a2);
}

void Hook_SwfContextEnter(PPCRegister& r3) {
    const uint32_t ctx = static_cast<uint32_t>(r3.u64);
    if (!ctx || !g_platform_var_ea.load(std::memory_order_relaxed)) return;

    const int want = WantedPlatformValue();

    int slot = -1;
    for (int i = 0; i < g_swf_ctx_count; ++i) {
        if (g_swf_ctx[i].ctx != ctx) continue;
        if (g_swf_ctx[i].value == want) return;  // already current
        slot = i;
        break;
    }
    if (slot < 0) {
        // Full is not expected (a handful of movies exist); recycling slot 0
        // just costs one redundant push if it ever happens.
        slot = g_swf_ctx_count < kMaxSwfCtx ? g_swf_ctx_count++ : 0;
        g_swf_ctx[slot].ctx = ctx;
    }

    CallGuestFn3(kSwfSetVarIntFn, ctx, kStrPlatform, uint32_t(want));
    g_swf_ctx[slot].value = want;
}

void UpdateCityLODMemory() {
    extern uint8_t* g_guest_mem;
    if (!g_guest_mem) return;

    float scale = static_cast<float>(REXCVAR_GET(lod_city_scale));
    float final_lod = scale * 300.0f;

    uint32_t int_val;
    std::memcpy(&int_val, &final_lod, sizeof(float));

    uint32_t city_lod_addr = 0x827E0DE0; 
    
    // Injeção Big-Endian segura
    g_guest_mem[city_lod_addr + 0] = (int_val >> 24) & 0xFF;
    g_guest_mem[city_lod_addr + 1] = (int_val >> 16) & 0xFF;
    g_guest_mem[city_lod_addr + 2] = (int_val >> 8)  & 0xFF;
    g_guest_mem[city_lod_addr + 3] = int_val         & 0xFF;
}

void Patch_ScaleCityLOD(PPCRegister& f13) {
    // Multiplicamos o valor que a engine acabou de ler da memória pelo nosso slider
    f13.f64 = f13.f64 * REXCVAR_GET(lod_city_scale);
}

bool OpenRexGraphicsFromGameOptions_826686D4(PPCRegister& r3) {
    if (!REXCVAR_GET(rexglue_settings_in_gameoptions)) {
        return false;
    }
    mc::ui::RequestOpenRexGraphicsMenu();
    r3.u64 = 1;
    return true;
}

void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24) {
    int cam_idx = static_cast<int>(r24.u64);
    double scale = (cam_idx == 1) ? REXCVAR_GET(fov_1p_scale) : REXCVAR_GET(fov_3p_scale);
    if (scale != 1.0) {
        f1.f64 = f1.f64 * scale;
    }
}

static float ReadGuestF32(const uint8_t* base, uint32_t addr) {
    uint32_t be = (uint32_t(base[addr + 0]) << 24) | (uint32_t(base[addr + 1]) << 16) |
                  (uint32_t(base[addr + 2]) << 8) | uint32_t(base[addr + 3]);
    float val;
    std::memcpy(&val, &be, sizeof(float));
    return val;
}

static void WriteGuestF32(uint8_t* base, uint32_t addr, float val) {
    uint32_t be;
    std::memcpy(&be, &val, sizeof(float));
    base[addr + 0] = (be >> 24) & 0xFF;
    base[addr + 1] = (be >> 16) & 0xFF;
    base[addr + 2] = (be >> 8) & 0xFF;
    base[addr + 3] = be & 0xFF;
}

// BadassBaboon's Recomp Adjustments: Rock-solid thread-pinned frame rate limiter
static void EnforceFrameLimit() {
    int32_t limit = REXCVAR_GET(fps_limit);
    if (const char* cap_env = std::getenv("MCLA_FPS_CAP")) {
        limit = std::atoi(cap_env);
    }
    if (limit <= 0) return;

    const double period_us = 1000000.0 / static_cast<double>(limit);

    // sub_821BDA90 has multiple callers across threads; bind limiter to main thread
    static const std::thread::id owner = std::this_thread::get_id();
    if (std::this_thread::get_id() != owner) return;

    static uint64_t next_us = 0;
    auto now_us = [] {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };

    uint64_t now = now_us();
    if (next_us == 0) {
        next_us = now + static_cast<uint64_t>(period_us);
        return;
    }

    if (now < next_us) {
        uint64_t remaining = next_us - now;
        if (remaining > 1500) {
            std::this_thread::sleep_for(std::chrono::microseconds(remaining - 1500));
        }
        while (now_us() < next_us) {
            std::this_thread::yield();
        }
    }

    uint64_t after = now_us();
    next_us += static_cast<uint64_t>(period_us);
    if (next_us < after) next_us = after + static_cast<uint64_t>(period_us);
}

// BadassBaboon's Recomp Adjustments: Core 60 FPS Clock Delta Pipeline
// 0x821BDAB0: runs after subf r8,r10,r11 in sub_821BDA90.
// Clamps max ticks and runs precision limiter.
void MCLAFrameDelta(PPCRegister& r8) {
    if (!REXCVAR_GET(fps_60)) return;
    EnforceFrameLimit();
    UpdateCityLODMemory();
    uint64_t hz = rex::chrono::Clock::guest_tick_frequency();
    if (hz == 0) hz = 50000000;
    uint64_t max_ticks = static_cast<uint64_t>(0.125 * static_cast<double>(hz));
    if (r8.u64 > max_ticks) {
        r8.u64 = max_ticks;
    }
}

// BadassBaboon's Recomp Adjustments: real delta instead of the fixed timestep.
//
// By 0x821BDAF8 sub_821BDA90 has already stored the measured unscaled delta at
// [r3+0x58] and the scaled one at [r3+0x08]. Two separate blocks downstream then
// throw that away and substitute the fixed timestep at [r3+0x20]; both have to
// be handled, and which one runs depends on [r3+0x3A] / [r3+0x3C]:
//
//   loc_821BDB58  reached when both are zero, after the +0x14 / +0x18
//                 accumulator updates. Loads [r3+0x20], and if the real delta is
//                 at least that big writes the FIXED value over +0x58 and +0x08
//                 (0x821BDB84/0x821BDB88). Skipped wholesale by jumping to
//                 loc_821BDC34 -- the accumulators are already updated by then,
//                 so nothing else is lost.
//   loc_821BDB90  reached from 0x821BDB1C / 0x821BDB28 when either flag is set.
//                 Not covered by the jump above, since it sits before it in the
//                 flow. Does the same substitution out of f11, so f11 is
//                 rewritten with the real delta instead.
//
// Returns true to take the jump. Baboon's build jumped unconditionally; gating
// it on fps_60 is the only change, and it makes the cvar actually turn the whole
// thing off instead of leaving half of it live.
bool MCLAUseRealDelta() {
    return REXCVAR_GET(fps_60);
}

// 0x821BDB90, after `lfs f11, 0x20(r3)` has loaded the fixed timestep. f11 feeds
// both `stfs f11, 0x58(r3)` and `fmuls f0, f11, f13` -> `stfs f0, 8(r3)`, so
// replacing it with [r3+0x58] (the measured unscaled delta stored at 0x821BDAF8)
// publishes the real frame time down this path too.
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11) {
    if (!REXCVAR_GET(fps_60)) return;
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    f11.f64 = static_cast<double>(ReadGuestF32(base, static_cast<uint32_t>(r3.u64) + 0x58));
}

// Fires at 0x822C22EC, right after the clock update (sub_821BDA90). The delta
// time itself is delivered at the clock source (MCLAFrameDelta /
// MCLAUseRealDelta / MCLAFixedStepPath) rather than overwritten late in the
// frame, which is what caused traffic jitter and physics stutter. What is left
// here is the per-frame housekeeping.
void Patch_DeltaTimePre() {
    TickVinylReadbackWindow();  // runs every frame regardless of fps_60
    TickVinylShapeCapture();    // hands-free shape-catalog sweep, if requested
    TickButtonPrompts();        // picks up a live button_prompts change
    TickCustomMusic();          // custom radio: volume + end-of-track advance
    TickHudUnits();             // hud_speed_units: mph -> km/h, live
    ApplyAmbientDensityTuning();  // no-op unless an ambient cvar moved
    ApplyFragTuneOverrides();     // re-asserts the fragment tune overrides
    ApplyRenderPhaseMask();       // perf_no_shadows, live
    ApplyLoadTimeDevOptions();    // keeps node+4 in sync for the load-time switches
    ApplyRubberBandLevel();       // forced AI difficulty tune index
    ApplyRubberBandScales();      // AI catch-up / hold-back limits
    LogRenderPhaseMaskOnce();     // a few samples of the real per-frame masks
    g_frame_heartbeat.fetch_add(1, std::memory_order_relaxed);
}

// Loop-entry anchor. r24 must NOT be modified: the game divides game_dt by it
// on its own (sub_821BD910) and r24 = 0 would clear the sub-tick gate at
// 0x827D754C and freeze physics.
void Patch_DeltaTime(PPCRegister& r24) {
    (void)r24;
}

// 0x823126A4: `bl sub_8230D988` inside the sun-cascade shadow phase (phase bit
// 0x20) of sub_823120C8, with r3 already loaded from dword_8288D0A0.
//
// sub_8230D988 picks the next impostor to refresh with an unbounded round-robin
// search:
//
//   do { do { v6 = (v6 + 1) % count; } while (!entry[v6].f168); } while (!entry[v6].f196);
//
// Neither loop has an iteration bound, and the caller only checks that the array
// exists and count != 0 — not that any entry qualifies. With notrees or
// noimpostors on, the array is allocated with a non-zero count but nothing is
// ever filled in, so the search spins forever the first time the phase runs.
// That only happens in daylight, since the call sits behind `mask & 0x20`.
//
// Returning true jumps to 0x823126A8, which is the exact target of the game's
// own `beq` at 0x8231268C for the null-manager case — not a made-up exit.
//
// Gated on the array's real contents rather than on the cvars, so it covers any
// other way of ending up with an empty impostor pool.
bool Patch_ImpostorShadowGuard(PPCRegister& r3) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return false;

    const uint32_t mgr = static_cast<uint32_t>(r3.u64);
    if (mgr == 0) return true;

    const uint32_t arr = ReadGuestU32(base, mgr + 16);
    if (arr == 0) return true;

    const uint32_t count = (uint32_t(base[arr + 12]) << 8) | uint32_t(base[arr + 13]);
    const uint32_t entries = ReadGuestU32(base, arr + 8);
    if (count == 0 || entries == 0) return true;

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = entries + i * 224u;
        if (ReadGuestU32(base, e + 168) && ReadGuestU32(base, e + 196)) return false;
    }

    static bool logged = false;
    if (!logged) {
        logged = true;
        LARECOMP_APP_INFO(
            "[Impostor] no refreshable entry in {} slot(s) — skipping sub_8230D988, which "
            "would search for one forever", count);
    }
    return true;
}

// 0x821C06EC, the blr of the dev-option registrar sub_821C06C8. r3 still holds
// the node the game just finished building, and the instruction right before us
// (`stw r9, 4(r3)`) has already cleared node+4 — so this is the first and only
// moment a value can be planted where nothing will wipe it and every consumer
// still reads it later.
void Patch_DevOptionRegistered(PPCRegister& r3) {
    if (g_pending_options.empty()) return;

    const uint32_t value_addr = static_cast<uint32_t>(r3.u64) + 4;
    for (auto it = g_pending_options.begin(); it != g_pending_options.end(); ++it) {
        if (it->value_addr != value_addr) continue;
        if (SetDebugOptionValue(value_addr, it->value)) {
            LARECOMP_APP_INFO("[DbgOpt] applied node+4 @ 0x{:08X} = {}", value_addr, it->value);
        }
        g_pending_options.erase(it);
        return;
    }
}

void Patch_BypassVehicleDLC(PPCRegister& r30) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    uint32_t struct_addr = static_cast<uint32_t>(r30.u64);
    if (struct_addr == 0) return;

    uint8_t* ptr = base + struct_addr;

    // Preço em offset 0x2C, big-endian
    uint32_t price = (uint32_t(ptr[0x2C]) << 24) | (uint32_t(ptr[0x2D]) << 16) |
                     (uint32_t(ptr[0x2E]) << 8)  | uint32_t(ptr[0x2F]);

    if (price == 118000) {
        LARECOMP_APP_INFO("[Audi R8] Restaurando ContentFlags/PortalRewardIdx para valores padrão");

        // ContentDownloadFlags (0x30) = 1, ContentFlags (0x34) = 1 — big-endian
        ptr[0x30] = 0; ptr[0x31] = 0; ptr[0x32] = 0; ptr[0x33] = 1;
        ptr[0x34] = 0; ptr[0x35] = 0; ptr[0x36] = 0; ptr[0x37] = 1;
        // PortalRewardIdx (0x38) = -1 (0xFFFFFFFF)
        ptr[0x38] = 0xFF; ptr[0x39] = 0xFF; ptr[0x3A] = 0xFF; ptr[0x3B] = 0xFF;
    }
}

// BadassBaboon's Recomp Adjustments:
// Continuous-time exponential decay for chase camera smoothing factors.
// In sub_82320298 (mcPlayerCamera::Update):
//   0x82320468 - f13 is the camera position chase/lag factor S1
//   0x823204F4 - f0  is the camera look-at / orientation factor S2
//
// On 30 FPS console the engine multiplied the raw profile factor by 0.5 and stepped once per update:
//   S(dt) = 1 - (1 - 0.5 * S_raw) ^ (30 * dt * scale)
static void ApplyCameraSmoothing(PPCRegister& reg) {
    if (!REXCVAR_GET(smooth_chase_cam)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float raw_dt = ReadGuestF32(base, 0x827D7508);
    const double raw_k = reg.f64;
    if (raw_k <= 0.0 || raw_k >= 1.0 || raw_dt <= 0.0f) return;

    const float dt = std::clamp(raw_dt, 0.001f, 0.05f);
    const double k30 = 0.5 * raw_k;
    const double scale = REXCVAR_GET(chase_cam_smoothing_factor);
    reg.f64 = 1.0 - std::pow(1.0 - k30, static_cast<double>(dt) * 30.0 * scale);
}

void MCLACameraPosSmoothing(PPCRegister& f13) {
    ApplyCameraSmoothing(f13);
}

void MCLACameraLookAtSmoothing(PPCRegister& f0) {
    ApplyCameraSmoothing(f0);
}

// BadassBaboon's Recomp Adjustments: Vehicle chassis suspension damping & ground depth filter continuous-time scaling
// 0x82563720: lis r11, flt_82001D14@ha in sub_82563298.
// f0 is the chassis ground depth filter coefficient alpha (0.10 at 30 FPS, 0.05 at 60 FPS).
void MCLAChassisDepthSmoothing(PPCRegister& f0) {
    if (!REXCVAR_GET(smooth_chassis_depth)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float raw_dt = ReadGuestF32(base, 0x827D7508);
    if (raw_dt > 0.0f) {
        const float dt = std::clamp(raw_dt, 0.001f, 0.05f);
        f0.f64 = 1.0 - std::pow(0.90, static_cast<double>(dt) * 30.0);
    }
}

// BadassBaboon's Recomp Adjustments: ambient traffic / pedestrian density.
//
// mcAmbientDensityTuning is not a separate object: sub_826F5CB0 calls its
// constructor sub_826F5B18 with its OWN `this` (`sub_826F5B18(a1)`), so the
// tuning fields are the base of the ~5936-byte ambient zone. The zone array is
// built in sub_826D8E70 -- 31 zones, `v5 = manager + 56476`, stride 1484 dwords
// -- and each one re-parses $/tune/ambients/density_tuning.xml through
// sub_826F4CB8, whose r31 is that zone. r31 is therefore the tuning object,
// and 31 hook firings per load is expected, one per zone.
//
// (The value passed as the parse's 4th argument is NOT the instance: it is the
// shared class descriptor returned by vtable slot 1, identical across all 31
// zones. Writing tuning fields through it corrupts a live RAGE structure.)
//
// The override has to land AFTER the parse -- hooking the constructor is
// pointless because the parse overwrites every field it set.
//
// Per-zone originals are captured from what the parse left behind, and every
// override is computed from those, so re-parsing a zone never compounds the
// scale the way the original code did.
struct DensityTuningValues {
    float spawn = 0.0f;
    float unspawn = 0.0f;
    float cull = 0.0f;
    float ped = 0.0f;
    float parked = 0.0f;
};

static std::mutex g_density_mutex;
static std::map<uint32_t, DensityTuningValues> g_density_orig;  // zone address -> XML values

// Last state pushed into guest memory, so the per-frame tick only writes when a
// cvar actually moved.
static bool g_density_applied_valid = false;
static bool g_density_applied_enabled = false;
static DensityTuningValues g_density_applied;

// Writes one zone. Caller holds g_density_mutex.
// Offsets verified via rage::mcAmbientDensityTuning in mcla_rage_types.h:
//   +0x08 = spawn_max
//   +0x10 = unspawn_max
//   +0x14 = cull_max
//   +0x60 = ped_density (96)
//   +0x98 = parked_factor (152)
static void WriteDensityZone(uint8_t* base, uint32_t a, const DensityTuningValues& orig,
                             bool enabled, const DensityTuningValues& want) {
    if (!enabled) {
        WriteGuestF32(base, a + 8, orig.spawn);
        WriteGuestF32(base, a + 16, orig.unspawn);
        WriteGuestF32(base, a + 20, orig.cull);
        WriteGuestF32(base, a + 96, orig.ped);
        WriteGuestF32(base, a + 152, orig.parked);
        return;
    }
    float unspawn_val = want.unspawn > 0.0f ? want.unspawn : orig.unspawn;
    WriteGuestF32(base, a + 16, unspawn_val);
    if (orig.spawn > 0.0f) {
        WriteGuestF32(base, a + 8, orig.spawn * 0.75f);
    }
    if (orig.cull > 0.0f) {
        WriteGuestF32(base, a + 20, orig.cull * 0.75f);
    }
    WriteGuestF32(base, a + 96, orig.ped * want.ped);
    WriteGuestF32(base, a + 152, orig.parked * want.parked);
}

static void ReadDensityCvars(bool& enabled, DensityTuningValues& want) {
    enabled = REXCVAR_GET(enable_ambient_tuning);
    want.unspawn = static_cast<float>(REXCVAR_GET(traffic_unspawn_dist));
    want.ped = static_cast<float>(REXCVAR_GET(ped_density_scale));
    want.parked = static_cast<float>(REXCVAR_GET(parked_car_scale));
}

// 0x826F4E3C, the instruction after the density_tuning.xml parse returns.
// r31 is the ambient zone, i.e. the tuning object.
void MCLAAmbientDensityTuning(PPCRegister& r31) {
    const uint32_t a = static_cast<uint32_t>(r31.u64);
    if (a == 0) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    bool enabled = false;
    DensityTuningValues want;
    ReadDensityCvars(enabled, want);

    std::lock_guard<std::mutex> lock(g_density_mutex);

    // The parse just restored this zone to its XML values, so re-reading here
    // is what keeps the scale from compounding across reloads.
    DensityTuningValues orig;
    orig.spawn = ReadGuestF32(base, a + 8);
    orig.unspawn = ReadGuestF32(base, a + 16);   // 400.0 stock
    orig.cull = ReadGuestF32(base, a + 20);
    orig.ped = ReadGuestF32(base, a + 96);       // 15.0 / XML stock
    orig.parked = ReadGuestF32(base, a + 152);   // 0.25 stock
    const bool first = g_density_orig.find(a) == g_density_orig.end();
    g_density_orig[a] = orig;

    WriteDensityZone(base, a, orig, enabled, want);

    // One line per zone the first time it is seen; reloads are silent, since 31
    // identical lines every district change is noise.
    if (first) {
        LARECOMP_APP_INFO(
            "[Ambient Tuning] zone {} at 0x{:08X}: unspawn {:.1f} -> {:.1f}, "
            "ped {:.4f} -> {:.4f}, parked {:.2f} -> {:.2f}",
            g_density_orig.size(), a, orig.unspawn, ReadGuestF32(base, a + 16), orig.ped,
            ReadGuestF32(base, a + 96), orig.parked, ReadGuestF32(base, a + 152));
    }
}

// Per-frame, so the cvars behave as the kHotReload they are declared to be.
// Costs a compare per frame and touches guest memory only when one moved.
static void ApplyAmbientDensityTuning() {
    bool enabled = false;
    DensityTuningValues want;
    ReadDensityCvars(enabled, want);

    std::lock_guard<std::mutex> lock(g_density_mutex);
    if (g_density_applied_valid && g_density_applied_enabled == enabled &&
        g_density_applied.unspawn == want.unspawn && g_density_applied.ped == want.ped &&
        g_density_applied.parked == want.parked) {
        return;
    }
    g_density_applied_enabled = enabled;
    g_density_applied = want;
    g_density_applied_valid = true;

    if (g_density_orig.empty()) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    for (const auto& [addr, orig] : g_density_orig) {
        WriteDensityZone(base, addr, orig, enabled, want);
    }
    LARECOMP_APP_INFO("[Ambient Tuning] {} zones updated (enabled={}, unspawn={:.1f}, ped={:.2f}, parked={:.2f})",
                      g_density_orig.size(), enabled, want.unspawn, want.ped, want.parked);
}

// ── rage::fragTuneStruct overrides ──────────────────────────────────────────
//
// sub_82729B10 allocates the 1448-byte singleton and parks the pointer in
// dword_828D4CE4; sub_8275E518 is its constructor. The field offsets come from
// the parser registration in sub_8275E308, which writes each parMember's offset
// slot: the record naming "GlobalMaxDrawingDistance" (0x8282F370) gets 8 and the
// one naming "BreakingFrameRateLimit" (0x8282F400) gets 32. Both line up with
// the constructor defaults (+8 = 250.0f, +32 = 30.0f).
//
// The game overwrites both from $/tune/types/fragments after construction, so
// this runs per-frame and re-asserts the override on top of whatever the tune
// file loaded — no RPF edit, no decryption. A cvar left at 0 writes nothing and
// instead keeps re-reading the guest value, so switching back to 0 restores
// exactly what the tune file had.
static constexpr uint32_t kFragTunePtr        = 0x828D4CE4u;  // rage::fragTuneStruct*
static constexpr uint32_t kFragTuneDrawDist   = 8u;           // GlobalMaxDrawingDistance
static constexpr uint32_t kFragTuneBreakLimit = 32u;          // BreakingFrameRateLimit

struct FragTuneField {
    uint32_t offset;
    const char* cvar;
    const char* label;
    float stock = 0.0f;   // last value seen while the cvar was 0
    bool have_stock = false;
    float applied = 0.0f;
    bool have_applied = false;
};

static FragTuneField g_frag_tune[] = {
    {kFragTuneDrawDist,   "global_max_draw_distance",  "GlobalMaxDrawingDistance"},
    {kFragTuneBreakLimit, "breaking_frame_rate_limit", "BreakingFrameRateLimit"},
};

static void ApplyFragTuneOverrides() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const uint32_t obj = ReadGuestU32(base, kFragTunePtr);
    if (obj == 0) return;

    for (auto& f : g_frag_tune) {
        const double want = std::strtod(rex::cvar::GetFlagByName(f.cvar).c_str(), nullptr);

        if (!(want > 0.0)) {
            // Auto: track whatever the tune file left there, and put it back once
            // if we had been overriding.
            if (f.have_applied) {
                if (f.have_stock) {
                    WriteGuestF32(base, obj + f.offset, f.stock);
                    LARECOMP_APP_INFO("[FragTune] {} restored to {:.1f}", f.label, f.stock);
                }
                f.have_applied = false;
            }
            f.stock = ReadGuestF32(base, obj + f.offset);
            f.have_stock = true;
            continue;
        }

        const float v = static_cast<float>(want);
        const float cur = ReadGuestF32(base, obj + f.offset);
        if (f.have_applied && f.applied == v && cur == v) continue;

        // If the field moved out from under us the game wrote it itself — the
        // tune file landing after construction — so that is the real stock
        // value, not whatever was in the struct before the parse.
        if (!f.have_stock || (f.have_applied && cur != f.applied)) {
            f.stock = cur;
            f.have_stock = true;
        }
        WriteGuestF32(base, obj + f.offset, v);
        if (!f.have_applied || f.applied != v) {
            LARECOMP_APP_INFO("[FragTune] {} {:.1f} -> {:.1f} (fragTuneStruct 0x{:08X}+{})",
                              f.label, f.stock, v, obj, f.offset);
        }
        f.applied = v;
        f.have_applied = true;
    }
}

// ── Render phase enable mask ────────────────────────────────────────────────
//
// perf_no_shadows reproduces what the game's own `noshadows` switch does inside
// the renderer constructor sub_822E47E0:
//
//   822e49c8  lwz    r11, 0x1C0(r31)
//   822e49cc  rlwinm r10, r11, 0,27,22   ; clears 0x20 0x40 0x80 0x100
//   822e49d0  rlwinm r10, r10, 0,19,16   ; clears 0x2000 0x4000
//   822e49d4  stw    r10, 0x1C0(r31)
//
// Doing it here instead of through the dev switch means it does not depend on
// hitting the one instant between the node being registered and the constructor
// reading it, and it applies without a restart. sub_822E6408 re-reads
// renderer+448 every frame, and the only other writers are that constructor and
// the runtime setter at 0x822E51F8, so re-asserting per frame is idempotent and
// heals itself if the game turns the phases back on.
static constexpr uint32_t kRendererPtr = 0x8287E064u;
static constexpr uint32_t kPhaseEnableMask = 448u;   // enable mask, written by the ctor
static constexpr uint32_t kPhaseRequested = 364u;    // phases asked for this frame
static constexpr uint32_t kPhaseEffective = 360u;    // requested & enable, what consumers read

static uint32_t ShadowPhaseBits() {
    const std::string s = rex::cvar::GetFlagByName("perf_shadow_phase_bits");
    if (s.empty()) return kShadowPhaseBitsDefault;
    const uint32_t v = uint32_t(std::strtoul(s.c_str(), nullptr, 0));
    return v ? v : kShadowPhaseBitsDefault;
}

static void ApplyRenderPhaseMask() {
    static bool was_applied = false;
    static uint32_t applied_bits = 0;
    static uint32_t stock_bits = 0;  // of applied_bits, which were set before we touched them
    static bool have_stock = false;

    const bool want = rex::cvar::GetFlagByName("perf_no_shadows") == "true";
    const uint32_t bits = ShadowPhaseBits();
    if (!want && !was_applied) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (renderer == 0) return;

    const uint32_t cur = ReadGuestU32(base, renderer + kPhaseEnableMask);

    // Turning off, or the bit set changed under us: put back what we took before
    // taking anything else, so the restore is never wider than the capture.
    if (was_applied && (!want || bits != applied_bits)) {
        const uint32_t restored = (cur & ~applied_bits) | stock_bits;
        if (restored != cur) WriteGuestU32(base, renderer + kPhaseEnableMask, restored);
        LARECOMP_APP_INFO("[PhaseMask] restored 0x{:08X}: renderer+448 0x{:08X} -> 0x{:08X}",
                          applied_bits, cur, restored);
        was_applied = false;
        have_stock = false;
        if (!want) return;
    }

    const uint32_t now = ReadGuestU32(base, renderer + kPhaseEnableMask);
    if (!have_stock) {
        stock_bits = now & bits;
        applied_bits = bits;
        have_stock = true;
    }

    const uint32_t next = now & ~bits;
    if (next != now) {
        WriteGuestU32(base, renderer + kPhaseEnableMask, next);
        if (!was_applied) {
            LARECOMP_APP_INFO("[PhaseMask] clearing 0x{:08X}: renderer+448 0x{:08X} -> 0x{:08X}",
                              bits, now, next);
        }
    }
    was_applied = true;
}

// One-shot probe for the render phase switches. Prints, the first frame the
// renderer exists, whether our dev-option writes survived to that point and what
// the phase enable mask actually ended up as.
//
//   node+4 != 0            -> sub_822E47E0 saw the switch
//   mask & 0x61E0 == 0     -> noshadows took (bits 20/40/80/100/2000/4000)
//
// renderer = dword_8287E064, +448 enable mask, +364 requested, +360 effective.
// ── Freeze watchdog ─────────────────────────────────────────────────────────
//
// A hang leaves nothing in the log: no exception, no crash handler, just the
// last line before it stopped. This bumps a counter every frame and a host
// thread reports the guest's render state when the counter stalls, so the next
// freeze says which render phase it died in instead of nothing at all.
//
// renderer+356 is the phase bit sub_822E6408 was on when it stopped advancing.
static constexpr uint32_t kPhaseCurrent = 356u;

static void StartFreezeWatchdog() {
    std::thread([] {
        uint64_t last = 0;
        int stalled = 0;
        bool reported = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint64_t now = g_frame_heartbeat.load(std::memory_order_relaxed);
            if (now != last) {
                last = now;
                stalled = 0;
                reported = false;
                continue;
            }
            if (now == 0) continue;  // not running yet
            if (++stalled < 5 || reported) continue;
            reported = true;

            auto* base = rex::Runtime::instance()->virtual_membase();
            if (!base) {
                LARECOMP_APP_ERROR("[Watchdog] frame stalled {}s, no guest membase", stalled);
                continue;
            }
            const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
            if (renderer == 0) {
                LARECOMP_APP_ERROR("[Watchdog] frame stalled {}s, renderer null", stalled);
                continue;
            }
            LARECOMP_APP_ERROR(
                "[Watchdog] frame stalled {}s at phase 0x{:08X} | enable=0x{:08X} "
                "requested=0x{:08X} effective=0x{:08X}",
                stalled, ReadGuestU32(base, renderer + kPhaseCurrent),
                ReadGuestU32(base, renderer + kPhaseEnableMask),
                ReadGuestU32(base, renderer + kPhaseRequested),
                ReadGuestU32(base, renderer + kPhaseEffective));
            LARECOMP_APP_ERROR("[Watchdog] perf_no_shadows={} bits={} no_trees={} no_impostors={}",
                               rex::cvar::GetFlagByName("perf_no_shadows"),
                               rex::cvar::GetFlagByName("perf_shadow_phase_bits"),
                               rex::cvar::GetFlagByName("perf_no_trees"),
                               rex::cvar::GetFlagByName("perf_no_impostors"));
        }
    }).detach();
}

static void LogRenderPhaseMaskOnce() {
    // The one-shot version read renderer+360 before the first RenderPhases call
    // and got 0xCDCDCDCD (uninitialised heap fill), which said nothing. Sample a
    // handful of real frames instead: +364 is what the frame asked for and +360
    // is what actually reached the consumers.
    static int samples = 0;
    static int frame = 0;
    static uint32_t last_effective = 0xFFFFFFFFu;
    if (samples >= 8) return;
    if (++frame % 60) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (renderer == 0) return;

    const uint32_t effective = ReadGuestU32(base, renderer + kPhaseEffective);
    if (effective == last_effective) return;
    last_effective = effective;
    ++samples;

    const uint32_t bits = ShadowPhaseBits();
    LARECOMP_APP_INFO(
        "[PhaseMask] enable=0x{:08X} requested=0x{:08X} effective=0x{:08X} | 0x{:X} in effective: "
        "0x{:X}",
        ReadGuestU32(base, renderer + kPhaseEnableMask),
        ReadGuestU32(base, renderer + kPhaseRequested), effective, bits, effective & bits);

    if (samples == 1) {
        LARECOMP_APP_INFO(
            "[PhaseMask] node+4: noimpostors(0x8288D0DC)=0x{:08X} notrees(0x8288D0F0)=0x{:08X} "
            "nofsblur(0x8288BA84)=0x{:08X}",
            ReadGuestU32(base, 0x8288D0DCu), ReadGuestU32(base, 0x8288D0F0u),
            ReadGuestU32(base, 0x8288BA84u));
    }
}

// ── mcRubberBandMgr tuning ──────────────────────────────────────────────────
//
// sub_827440E8 allocates the 1876-byte manager and parks it in dword_8290AC0C;
// sub_82743A10 (RubberBandMgr_LoadTuning) fills 11 mcRubberBandTuning entries at
// mgr+76, stride 140, from $/tune/career/RubberBandTune00..10 — then rescales a
// few of them in code (+48 *= 2, +56 *= 2.5, +64 *= 2), so the file value is not
// the final value. Field names and offsets come from the parser registration in
// sub_823990E0, same shape as fragTuneStruct.
//
// disable_rubberbanding kills the whole system, which also removes MinThrottle —
// the leash that slows the AI down when it is AHEAD. That is why turning it off
// makes races harder rather than fairer. Scaling the individual limits keeps
// both halves and just changes how hard they pull.
static constexpr uint32_t kRubberBandMgrPtr = 0x8290AC0Cu;
static constexpr uint32_t kRubberBandFirst = 76u;
static constexpr uint32_t kRubberBandStride = 140u;
static constexpr int kRubberBandCount = 11;

struct RubberBandField {
    uint32_t offset;
    const char* name;
};

// The scalar members. +76 AheadDistOffsets and +108 BehindDistOffsets are
// 8-float arrays and are dumped separately.
static constexpr RubberBandField kRubberBandFields[] = {
    {0,  "MaxThrottle"},
    {4,  "MinSpeed"},
    {8,  "MinDist"},
    {12, "MaxDist"},
    {16, "MinThrottle"},
    {20, "BehindMinDist"},
    {24, "BehindMaxDist"},
    {28, "BehindMaxThrottle"},
    {32, "OutsideTrafficBubbleThresholdSpeed"},
    {36, "OutsideTrafficBubbleThrottlePenalty"},
    {40, "DistBehindBeforeTakingShortcuts"},
    {44, "UseNitroMinDistFromStart"},
    {48, "UseNitroMinDistFromFinish"},
    {52, "UseNitroMinDistBehindPlayer"},
    {56, "UnlimitedNitroMinDistBehindPlayer"},
    {60, "HomeStretchBehindMaxThrottle"},
    {64, "HomeStretchDistance"},
    {68, "PlayerCloseMaxThrottle"},
    {72, "PlayerCloseDistance"},
};

// Writes every entry to <exe>/rubberband_dump.txt. The knobs below are meant to
// be chosen from these numbers rather than guessed at.
static void DumpRubberBandTuning() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) {
        LARECOMP_APP_ERROR("[RubberBand] manager not created yet");
        return;
    }

    std::error_code ec;
    std::ofstream out(std::filesystem::current_path(ec) / "rubberband_dump.txt");
    if (!out) {
        LARECOMP_APP_ERROR("[RubberBand] cannot open rubberband_dump.txt");
        return;
    }
    out << "mcRubberBandMgr 0x" << std::hex << mgr << std::dec
        << "  (11 x mcRubberBandTuning, 140 bytes, at mgr+76)\n"
        << "values are post-LoadTuning: +48 *= 2, +56 *= 2.5, +64 *= 2\n\n";

    for (int i = 0; i < kRubberBandCount; ++i) {
        const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
        out << "RubberBandTune" << (i < 10 ? "0" : "") << i << "  @0x" << std::hex << e
            << std::dec << "\n";
        for (const auto& f : kRubberBandFields) {
            char line[128];
            std::snprintf(line, sizeof(line), "  +%-3u %-36s %g\n", f.offset, f.name,
                          double(ReadGuestF32(base, e + f.offset)));
            out << line;
        }
        for (const char* which : {"AheadDistOffsets", "BehindDistOffsets"}) {
            const uint32_t off = (which[0] == 'A') ? 76u : 108u;
            out << "  +" << off << " " << which << " ";
            for (int k = 0; k < 8; ++k) out << ReadGuestF32(base, e + off + 4u * k) << " ";
            out << "\n";
        }
        out << "\n";
    }
    LARECOMP_APP_INFO("[RubberBand] dumped {} entries to rubberband_dump.txt", kRubberBandCount);
}

// Which fields each scale drives. `throttle` marks the normalised 0..1 fields,
// which are not pushed past 1.0 when scaling up.
struct RubberBandScale {
    const char* cvar;
    uint32_t offsets[4];  // 0-terminated
    bool throttle;
    // Scales multiply the loaded value and are neutral at 1.0. Absolute knobs
    // write the value straight in and are neutral at -1 — used where the shipped
    // tune is the same at every difficulty level, so a multiplier would be
    // meaningless to reason about.
    bool absolute;
};

static constexpr RubberBandScale kRubberBandScales[] = {
    {"rubberband_catchup_scale",      {0, 28, 60, 68}, true,  false},  // *MaxThrottle
    {"rubberband_holdback_scale",     {16, 0, 0, 0},   true,  false},  // MinThrottle
    {"rubberband_distance_scale",     {8, 12, 20, 24}, false, false},  // *Dist bands
    {"rubberband_nitro_start_dist",   {44, 0, 0, 0},   false, true},   // UseNitroMinDistFromStart
    {"rubberband_nitro_finish_scale", {48, 0, 0, 0},   false, false},  // UseNitroMinDistFromFinish
    {"rubberband_nitro_behind_scale", {52, 56, 0, 0},  false, false},  // *MinDistBehindPlayer
};

static bool RubberBandGroupNeutral(const RubberBandScale& g, double v) {
    return g.absolute ? (v < 0.0) : (v == 1.0);
}

// Offset 0 is a real field (MaxThrottle) and only ever appears first, so a zero
// past index 0 is the terminator.
static constexpr size_t RubberBandFieldTotal() {
    size_t n = 0;
    for (const auto& g : kRubberBandScales)
        for (size_t i = 0; i < 4; ++i)
            if (i == 0 || g.offsets[i] != 0) ++n;
    return n;
}

// Same shape as the fragTune override: while every scale is 1.0 the loaded
// values are re-read as the baseline, so the scales always multiply the tune the
// game actually parsed rather than compounding on themselves. Throttle fields
// are clamped back to their original ceiling when that ceiling was <= 1.0, since
// those read as normalised throttle and overshooting them is not something the
// shipped tune ever does.
// mgr+60 holds the index of the RubberBandTune entry in force. Writing it every
// frame beats both places the game sets it: sub_82743B30 computes it at race
// setup, and the tail of that function can overwrite it again from the stored
// value at dword_8286EC70+2616.
static constexpr uint32_t kRubberBandLevelField = 60u;

static void ApplyRubberBandLevel() {
    static int last_logged = -2;
    const int want = int(std::strtol(rex::cvar::GetFlagByName("rubberband_level").c_str(),
                                     nullptr, 10));
    if (want < 0) {
        last_logged = -2;
        return;
    }

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) return;

    const uint32_t clamped = uint32_t(want > 10 ? 10 : want);
    if (ReadGuestU32(base, mgr + kRubberBandLevelField) != clamped)
        WriteGuestU32(base, mgr + kRubberBandLevelField, clamped);

    if (last_logged != want) {
        last_logged = want;
        LARECOMP_APP_INFO("[RubberBand] forcing tune level {} (mgr+60)", clamped);
    }
}

static void ApplyRubberBandScales() {
    static constexpr size_t kFieldCount = RubberBandFieldTotal();
    static float baseline[kRubberBandCount][kFieldCount] = {};
    static bool have_base = false;
    static bool was_scaled = false;

    double scale[std::size(kRubberBandScales)];
    bool neutral = true;
    for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
        scale[g] = std::strtod(rex::cvar::GetFlagByName(kRubberBandScales[g].cvar).c_str(),
                               nullptr);
        if (!RubberBandGroupNeutral(kRubberBandScales[g], scale[g])) neutral = false;
    }

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) return;
    // Entry 0 is still zero until LoadTuning has run; do not snapshot that.
    if (ReadGuestF32(base, mgr + kRubberBandFirst + 12) == 0.0f) return;

    // Dump once, automatically, the first frame the tune is actually parsed —
    // the scales below are only meaningful against these numbers, and waiting
    // for someone to press a button means the file is usually missing when it
    // is needed. The rubberband_dump cvar still forces a fresh one on demand.
    static bool auto_dumped = false;
    if (!auto_dumped) {
        auto_dumped = true;
        DumpRubberBandTuning();
    }

    // Every field this touches, in the order the baseline stores them.
    auto for_each_field = [&](auto&& fn) {
        size_t slot = 0;
        for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
            const auto& grp = kRubberBandScales[g];
            for (size_t i = 0; i < 4; ++i) {
                if (i != 0 && grp.offsets[i] == 0) break;
                fn(slot++, grp.offsets[i], scale[g], grp.throttle, grp.absolute);
            }
        }
    };

    if (neutral) {
        if (was_scaled) {
            for (int i = 0; i < kRubberBandCount; ++i) {
                const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
                for_each_field([&](size_t slot, uint32_t off, double, bool, bool) {
                    WriteGuestF32(base, e + off, baseline[i][slot]);
                });
            }
            was_scaled = false;
            LARECOMP_APP_INFO("[RubberBand] scales cleared, shipped tune restored");
            return;
        }
        // Keep the baseline tracking whatever LoadTuning last produced.
        for (int i = 0; i < kRubberBandCount; ++i) {
            const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
            for_each_field([&](size_t slot, uint32_t off, double, bool, bool) {
                baseline[i][slot] = ReadGuestF32(base, e + off);
            });
        }
        have_base = true;
        return;
    }

    for (int i = 0; i < kRubberBandCount; ++i) {
        const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
        for_each_field([&](size_t slot, uint32_t off, double v, bool is_throttle,
                           bool is_absolute) {
            if (!have_base) baseline[i][slot] = ReadGuestF32(base, e + off);
            const float orig = baseline[i][slot];
            if (is_absolute) {
                // Neutral for this group: leave the shipped value in place.
                if (v < 0.0) {
                    WriteGuestF32(base, e + off, orig);
                    return;
                }
                WriteGuestF32(base, e + off, float(v));
                return;
            }
            double out = double(orig) * v;
            // Throttles are normalised — measured 0.75..1.0 across the 11 levels
            // — so do not push them past 1.0 when scaling up.
            if (is_throttle && orig <= 1.0f && out > 1.0) out = 1.0;
            WriteGuestF32(base, e + off, float(out));
        });
    }
    have_base = true;

    if (!was_scaled) {
        std::string what;
        for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
            const auto& grp = kRubberBandScales[g];
            if (RubberBandGroupNeutral(grp, scale[g])) continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s %s%g", what.empty() ? "" : ", ",
                          grp.cvar + 11,  // skip "rubberband_"
                          grp.absolute ? "= " : "x", scale[g]);
            what += buf;
        }
        LARECOMP_APP_INFO("[RubberBand] {} on {} entries", what, kRubberBandCount);
    }
    was_scaled = true;
}

// BadassBaboon's Recomp Adjustments:
// 0x822A2ED4 in sub_822A2988: `lfs f0, 0xC(r20)` with r20 = 0x827D7500, so f0
// is the clock's inv_game_dt, and the next lines turn the steering delta into a
// per-second rate with it. Halving it at 60 FPS reproduces the 30 FPS response
// the handling was tuned against.
void Patch_SteeringSensitivity(PPCRegister& f0) {
    double sens = REXCVAR_GET(steering_sensitivity);
    if (REXCVAR_GET(scale_steering_with_fps) && REXCVAR_GET(fps_60)) {
        sens *= 0.5;
    }
    f0.f64 *= sens;
}

// The player's current district (return of Racer_GetCurrentDistrict). Fires on
// the game's own district queries -> the RPC updates the area live while driving.
void Hook_CaptureDistrict(PPCRegister& r3) {
    // rpc-diag: confirm the hook fires + what district it sees. Remove later.
    static int last_diag = -999;
    int idx = static_cast<int>(r3.u64);
    if (idx != last_diag) {
        last_diag = idx;
        LARECOMP_APP_INFO("[rpc-diag] district hook fired, idx={}", idx);
    }
    RpcOnDistrictChanged(idx);
}


// LZX streaming decompression probe. pgStreamer worker threads decompress
// world resources through zlibInflater::InflateBegin (sub_821D5E10), which
// wraps the statically linked XMemDecompressStream (sub_8244FF20, XCompress
// LZX, 128KB window) — all of it recompiled guest code. The pair of hooks
// brackets that call: Pre fires at 0x821D5EB4 (just before the bl), Post at
// 0x821D5EBC (first instruction after it). The wrapper keeps its in/out sizes
// in stack slots: [r1+0x50] holds the source bytes offered (consumed after the
// call) and [r1+0x54] the destination capacity (bytes produced after the
// call). Two worker threads run this concurrently, hence thread_local pairing
// and atomic totals. Results append to <exe>/lzx_stats.txt every 2 seconds
// while the lzx_stats cvar is on.
namespace {

struct LzxWindow {
    uint64_t calls = 0;
    uint64_t ns = 0;
    uint64_t src_bytes = 0;
    uint64_t dst_bytes = 0;
    uint64_t errors = 0;
};

std::atomic<uint64_t> g_lzx_calls{0};
std::atomic<uint64_t> g_lzx_ns{0};
std::atomic<uint64_t> g_lzx_src_bytes{0};
std::atomic<uint64_t> g_lzx_dst_bytes{0};
std::atomic<uint64_t> g_lzx_max_ns{0};
std::atomic<uint64_t> g_lzx_errors{0};
std::atomic<int64_t> g_lzx_last_dump_ns{0};
std::mutex g_lzx_dump_mutex;
LzxWindow g_lzx_prev;

thread_local int64_t tl_lzx_start_ns = 0;

int64_t LzxNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void LzxDump(int64_t now_ns, int64_t prev_dump_ns) {
    std::lock_guard<std::mutex> lock(g_lzx_dump_mutex);

    LzxWindow cur;
    cur.calls = g_lzx_calls.load(std::memory_order_relaxed);
    cur.ns = g_lzx_ns.load(std::memory_order_relaxed);
    cur.src_bytes = g_lzx_src_bytes.load(std::memory_order_relaxed);
    cur.dst_bytes = g_lzx_dst_bytes.load(std::memory_order_relaxed);
    cur.errors = g_lzx_errors.load(std::memory_order_relaxed);
    uint64_t max_ns = g_lzx_max_ns.exchange(0, std::memory_order_relaxed);

    double wall_ms = double(now_ns - prev_dump_ns) / 1e6;
    double busy_ms = double(cur.ns - g_lzx_prev.ns) / 1e6;
    double out_mb = double(cur.dst_bytes - g_lzx_prev.dst_bytes) / (1024.0 * 1024.0);
    double in_mb = double(cur.src_bytes - g_lzx_prev.src_bytes) / (1024.0 * 1024.0);
    uint64_t calls = cur.calls - g_lzx_prev.calls;
    uint64_t errors = cur.errors - g_lzx_prev.errors;

    std::error_code ec;
    std::filesystem::path file = std::filesystem::current_path(ec) / "lzx_stats.txt";
    if (ec) return;
    std::ofstream out(file, std::ios::app);
    if (!out) return;

    char line[320];
    std::snprintf(line, sizeof(line),
                  "wall=%.0fms calls=%llu busy=%.2fms busy_pct=%.1f%% in=%.2fMB out=%.2fMB "
                  "out_rate=%.1fMB/s max_call=%.0fus errors=%llu | total: calls=%llu busy=%.0fms "
                  "out=%.1fMB\n",
                  wall_ms, static_cast<unsigned long long>(calls), busy_ms,
                  wall_ms > 0.0 ? busy_ms * 100.0 / wall_ms : 0.0, in_mb, out_mb,
                  wall_ms > 0.0 ? out_mb * 1000.0 / wall_ms : 0.0, double(max_ns) / 1e3,
                  static_cast<unsigned long long>(errors),
                  static_cast<unsigned long long>(cur.calls), double(cur.ns) / 1e6,
                  double(cur.dst_bytes) / (1024.0 * 1024.0));
    out << line;

    g_lzx_prev = cur;
}

}  // namespace

void Hook_LzxDecompressPre(PPCRegister& r1) {
    (void)r1;
    if (!REXCVAR_GET(lzx_stats)) {
        tl_lzx_start_ns = 0;
        return;
    }
    tl_lzx_start_ns = LzxNowNs();
}

void Hook_LzxDecompressPost(PPCRegister& r1, PPCRegister& r3) {
    if (!tl_lzx_start_ns) return;
    int64_t now = LzxNowNs();
    uint64_t dur = uint64_t(now - tl_lzx_start_ns);
    tl_lzx_start_ns = 0;

    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    uint32_t sp = static_cast<uint32_t>(r1.u64);
    // After XMemDecompressStream returns: [sp+0x50] = source bytes consumed,
    // [sp+0x54] = destination bytes produced (the wrapper advances its
    // pointers by exactly these values right after the call).
    uint32_t consumed = GuestRead32(mem, sp + 0x50);
    uint32_t produced = GuestRead32(mem, sp + 0x54);

    g_lzx_calls.fetch_add(1, std::memory_order_relaxed);
    g_lzx_ns.fetch_add(dur, std::memory_order_relaxed);
    g_lzx_src_bytes.fetch_add(consumed, std::memory_order_relaxed);
    g_lzx_dst_bytes.fetch_add(produced, std::memory_order_relaxed);

    uint64_t prev_max = g_lzx_max_ns.load(std::memory_order_relaxed);
    while (dur > prev_max &&
           !g_lzx_max_ns.compare_exchange_weak(prev_max, dur, std::memory_order_relaxed)) {
    }

    // 0x81DE2001 is the "needs more input" status the game itself tolerates.
    int32_t status = static_cast<int32_t>(r3.u64);
    if (status < 0 && status != int32_t(0x81DE2001)) {
        g_lzx_errors.fetch_add(1, std::memory_order_relaxed);
    }

    int64_t last = g_lzx_last_dump_ns.load(std::memory_order_relaxed);
    if (now - last >= 2'000'000'000 &&
        g_lzx_last_dump_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        // First window after enabling has no baseline timestamp — skip the dump,
        // the totals still carry into the next one.
        if (last != 0) LzxDump(now, last);
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_CacheVinylPaint(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
void Hook_PhotoModeCapture(PPCRegister& r3) {}
void ExportVinyl() {}
void DumpVinylShapes() {}
void RequestVinylShapeCapture() {}
void TickVinylShapeCapture() {}
void ApplyDebugOptions() {}
void InitHooks() {}
bool SkipIntro() { return false; }
void MCLA_SkipIntroRenderPassMask(PPCRegister& r4) {}
bool Patch_AspectRatio_82233EB4(PPCRegister& f0) { return false; }
bool Patch_AspectRatio_82214BB8(PPCRegister& f10) { return false; }
bool Patch_AspectRatio_822E5E68(PPCRegister& f12) { return false; }
bool Patch_AspectRatio_8223E5E0(PPCRegister& f13) { return false; }
bool Patch_60FPS_Jump() { return false; }
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r25, PPCRegister& r28) {}
bool Patch_EdramLimit(PPCRegister& r11) { return false; }
bool Patch_DebugCamGate() { return false; }
void Patch_DebugCam(PPCRegister& r3) {}
bool Hook_IntroHalfRate() { return false; }
bool MCLA_UI_SkipMissingLights(PPCRegister& r3) { return false; }
bool Patch_60FPS_Byte(PPCRegister& r11) { return false; }
bool Patch_DisableMotionBlur(PPCRegister& r3) { return false; }
bool Patch_DisableMSAA(PPCRegister& r11) { return false; }
bool Patch_PhysicsCollision() { return false; }
bool Patch_DisableRubberBanding() { return false; }
bool Patch_DisableDoF() { return false; }
void Patch_DofComposite(PPCRegister& r3) {}
void Patch_ScaleTrafficLOD(PPCRegister& f0) {}
void Patch_RideHeightFit(PPCRegister& f30) {}
bool Patch_WheelFitBypass() { return false; }
bool Patch_SpeedUnits(PPCRegister& r11) { return false; }
void TickButtonPrompts() {}
void Hook_PlatformVarInit(PPCRegister& r27) {}
bool Patch_PlatformPush(PPCRegister& r5) { return false; }
void Hook_SwfContextEnter(PPCRegister& r3) {}
void UpdateCityLODMemory() {}
void Patch_ScaleCityLOD(PPCRegister& f13) {}
bool OpenRexGraphicsFromGameOptions_826686D4(PPCRegister& r3) { return false; }
void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24) {}
void Patch_DeltaTimePre() {}
void Patch_DeltaTime(PPCRegister& r24) {}
void Patch_BypassVehicleDLC(PPCRegister& r30) {}
void Patch_DevOptionRegistered(PPCRegister& r3) {}
bool Patch_ImpostorShadowGuard(PPCRegister& r3) { return false; }
void Hook_CaptureDistrict(PPCRegister& r3) {}
void Hook_LzxDecompressPre(PPCRegister& r1) {}
void Hook_LzxDecompressPost(PPCRegister& r1, PPCRegister& r3) {}
void MCLACameraPosSmoothing(PPCRegister& f13) {}
void MCLACameraLookAtSmoothing(PPCRegister& f0) {}
void MCLAChassisDepthSmoothing(PPCRegister& f0) {}
void MCLAAmbientDensityTuning(PPCRegister& r31) {}
bool Patch_DisableImposterShadows(PPCRegister& r11) { return false; }
void MCLA_TrafficChassisBound_8232D048(PPCRegister& r9, PPCRegister& r11) {}
void MCLA_TrafficChassisBound_8232D900(PPCRegister& r3, PPCRegister& r11) {}
void MCLA_TrafficChassisBound_8232E274(PPCRegister& r3, PPCRegister& r31) {}
bool MCLA_TrafficBoundRelease_8259AA40(PPCRegister& r30) { return false; }
void MCLA_TuneFieldProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
void Patch_SteeringSensitivity(PPCRegister& f0) {}
void MCLAFrameDelta(PPCRegister& r8) {}
bool MCLAUseRealDelta() { return false; }
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11) {}
#endif // REXGLUE_HAS_XEO3_TARGET

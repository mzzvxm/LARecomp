#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class PerformanceSettings final : public rex::SettingOwner {
 public:
  PerformanceSettings() : SettingOwner("larecomp") {}

  REX_SETTING(f64, breaking_frame_rate_limit, 0.0, "fragTuneStruct::BreakingFrameRateLimit — framerate floor under which the engine "
    "stops spawning new fragment breaks. 0 = keep the tune file's value (10); the "
    "engine's own constructor default is 30.", rex::Range{0.0, 120.0}, rex::kDebugOnly);

  // BadassBaboon's Recomp Adjustments: Ambient traffic & pedestrian density tuning for city performance
  REX_SETTING(bool, enable_ambient_tuning, true, "Enable ambient traffic and pedestrian density tuning.", rex::kDebugOnly);

  REX_SETTING(bool, fence_spin_throttle, true, "Yield the host CPU inside the guest's D3D fence poll (sub_82412F98) instead of running "
    "its 32 pipeline-throttle NOPs. Off restores the stock guest spin.", rex::kDebugOnly);

  REX_SETTING(bool, fma_float_fast, true, "On a CPU without FMA3 (Intel before Haswell, AMD before Piledriver), fused multiply-adds "
    "whose operands are single-precision values are computed with a double multiply and add, "
    "which gives the same bits, instead of the C runtime's software fma (157 to 165 cycles a "
    "call). Nothing changes on a CPU with FMA3. Off passes every call to the runtime. Takes "
    "effect at the next start.", rex::kDebugOnly, rex::kRestart);  // was inside a #if in the .cpp: check

  // Default to 60 FPS: with vsync=false for low input lag and fast pacing,
  // the precision frame limiter caps to 60 FPS out-of-the-box.
  REX_SETTING(i32, fps_limit, 60, "Frame rate cap in FPS (0 = uncapped). The pause menu offers 30, 40, 45, 50, 60, 120 and 144. "
    "While frame generation runs the cap counts the frames the game renders (twice as many are "
    "shown) and mcla_gpu_frame_generation_fps_limit is the cap instead.", rex::Range{0, 360});

  REX_SETTING(bool, frame_limit_spin, true, "Hit the frame-limit deadline with a PAUSE spin. Off restores the sleep-then-yield wait, "
    "which gives the core back instead of burning it.", rex::kDebugOnly);

  REX_SETTING(i32, frame_pacing, 1, "1: every frame stays on screen for exactly the time step the game simulated it with -- the "
    "renderer holds a frame that is ready early until its planned time, and the game steps by the "
    "planned interval. Stops the camera's back-and-forth jumps when the game does not hold the FPS "
    "LIMIT, for a few milliseconds of latency. 0: the step is the measured frame time and frames are "
    "shown when ready, as before. Needs the Plume renderer, and is idle with mcla_gpu_vsync.", rex::Range{0, 1}, rex::kDebugOnly);

  // ── rage::fragTuneStruct overrides ──────────────────────────────────────────
  // Both values live in the fragment tune the game parses out of
  // $/tune/types/fragments (fragment = breakable prop: poles, signs, fences,
  // barriers). They are patched in the parsed struct at runtime, so no RPF edit
  // and no decryption is involved. 0 keeps whatever the tune file loaded.
  REX_SETTING(f64, global_max_draw_distance, 0.0, "fragTuneStruct::GlobalMaxDrawingDistance — draw distance for breakable props "
    "(poles, signs, fences). 0 = keep the tune file's value (3000); the engine's own "
    "constructor default is 250.", rex::Range{0.0, 6000.0});

  REX_SETTING(bool, lzx_native, true, "Decompress the game's streamed resources (LZX) with native code instead of the recompiled "
    "XMemDecompressStream.\n"
    "\n"
    "The first streams are decoded by both and compared; any difference switches the native decoder "
    "off for the rest of the session. Read at start.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(i32, host_helpers_mask, 63, "Which of the game's small helper functions run as native host code instead of the recompiled "
    "version, as bits: 1 memcpy, 2 memset, 4 the row writer, 8 the shader-constant copy, 16 the "
    "constant-bank flush, 32 the C runtime's memcpy. 0 runs all of them recompiled (the A/B switch); 63 is the default. A call "
    "the native version cannot reproduce exactly (overlapping ranges, register or physical-window "
    "addresses, a command-ring wrap) always goes to the recompiled code. Read at start.", rex::Range{0, 63}, rex::kDebugOnly, rex::kRestart);

  // Where the frame limiter waits. The game updates on its main loop
  // (sub_82131508) and presents from "[MC] Render Thread" (sub_82306238), one
  // frame behind. Waiting before the update leaves the present wherever the
  // render work ends it, so frames reach the screen at uneven intervals; waiting
  // just before the swap presents them evenly, and the queue between the two
  // threads holds the update back to the same interval. The cost is latency: the
  // update then runs up to a frame ahead of what is on screen.
  REX_SETTING(bool, pace_at_swap, true, "Hold each frame to the FPS limit right before it is presented instead of before the "
    "game's update: even present intervals (best on a variable refresh display), at the cost "
    "of up to one more frame of input latency.", rex::kDebugOnly);

  // Below 0 is auto (gpu/gpu_class.h kScales): 1.0 (the game's own) on a high-end graphics card, 0.75 on a mid-range one, 0.5 on
  // integrated graphics and until the card is known. 0 is none, so it cannot be the sentinel as it is for the LOD scales.
  REX_SETTING(f64, parked_car_scale, -1.0, "How many cars are parked along the roads, as a multiple of the game's own amount (0 = none, 1 = the game's). "
    "-1 (auto, the default) is 1.0 on a high-end graphics card, 0.75 on a mid-range one and 0.5 on integrated "
    "graphics. Fewer is faster (0 to 2; the menu has auto, 0, 0.25, 0.5, 0.75, 1, 1.5 and 2).", rex::Range{-1.0, 2.0});

  // Below 0 is auto, as for parked_car_scale.
  REX_SETTING(f64, ped_density_scale, -1.0, "How many people walk the streets, as a multiple of the game's own amount (0 = none, 1 = the game's). "
    "-1 (auto, the default) is 1.0 on a high-end graphics card, 0.75 on a mid-range one and 0.5 on integrated "
    "graphics. Fewer is faster (0 to 2; the menu has auto, 0, 0.25, 0.5, 0.75, 1, 1.5 and 2).", rex::Range{-1.0, 2.0});

  // Live. The one reader of the dev switch is sub_8235FA90, Vehicle::Draw's phase 0x10 / shadow-pass
  // routine (feature-parity/GFX-UPGRADES-DESIGN.md 13.2). The recompiled code reads node+4 with
  // `lwz r11,4(r9)` at 0x8235FB20 on every call and keeps no copy. The routine sets the car up at
  // LOD 2 and, only while the switch is OFF, sets it up once more at the forced LOD
  // (dword_827E624C, 1) before the draw: with the switch on the car is drawn at LOD 2. (That LOD 2 is
  // the coarser one is the usual order of LODs, not checked.) ApplyLoadTimeDevOptions already keeps
  // node+4 equal to this cvar every frame, so a flip reaches the game at the next shadow draw; the
  // restart tag and "applies at the next start" came from the fork, which listed the switch among
  // the options read at start-up (true for TREE IMPOSTORS and SCREEN BLUR, below, not for this one).
  // CHECK: perf/scripts/chain-live3b.ps1 flips it in free roam in daylight and compares the car's
  // shadow before and after. At night the 0x400 phase does not go through this routine, so nothing
  // changes there.
  REX_SETTING(bool, perf_fast_vehicle_shadows, false, "Draw cars into the shadow passes with less detail (dev switch 'fastVehShadows'): the game "
    "skips the extra, more detailed draw it adds for each car's shadow. Use instead of "
    "perf_no_shadows to keep world shadows. Takes effect at the next frame that draws shadows: "
    "no restart.", rex::kDebugOnly);

  // Read once, at start-up. The recompiled code reads the switch in sub_82306860, the constructor of
  // the post-effect object with vtable 0x820424B0: it stores `nofsblur == 0` as a byte at object+5
  // (and `fastdraw == 0 && nomotionblur == 0` at object+4); no other reader of the node was found. The
  // constructor is reached from the game's start-up chain (direct calls: sub_821305E8 -> sub_821BD7C0
  // -> sub_82131790 -> sub_82131508 -> sub_822C2EA8 -> sub_823047D8; the same chain makes the other
  // renderer clients), so ApplyLoadTimeDevOptions keeping node+4 equal to the cvar reaches nothing
  // until the object is made again: a flip during play does nothing, as the owner saw, and a district
  // load does not make it again. (An earlier survey read the harness's "applies on the next load of
  // that system" as "applies at the next load"; the code says start.)
  // Not a candidate for the phase mask (ApplyRenderPhaseMask): the fork's frame-structure survey puts
  // the 0x2 phase on the 256x256 reflection view, and the blur passes (motion blur, the bloom blurs)
  // are inside the post phase (0x08000000) with the bright pass and the luminance chain, so no phase
  // bit drops a blur alone. A live switch would write object+5 of the live object (sub_823047D8
  // stores the new object at `[r3 + 20]` of its argument) or hook the loads of that byte.
  // CHECK: where the object is kept, which pass reads byte +5 and what it skips; none of that was
  // run, and the row stays a launch argument until it is.
  REX_SETTING(bool, perf_no_fullscreen_blur, false, "Skip the full-screen blur pass (dev switch 'nofsblur'). Applies at the next start of "
    "the game.", rex::kDebugOnly, rex::kRestart);

  // Load-time, not per-frame: sub_8230B778 skips allocating the ImpostorDepth /
  // ShadowImpostor / ImpostorColor / ImpostorNormal render targets. node+4 is kept
  // in sync with this cvar every frame, but the game makes these targets once, with
  // the world, at start-up (the fork logged one world setup and 53 impostor setups
  // at start-up and none when a race started or was quit: sub_82305230 ->
  // sub_8230C388 -> sub_8230B778), so the change is seen at the next start. This
  // said "the next load of that system" before, and the pause row "(ON RELOAD)".
  // CHECK: toggled in the pause menu, then a district change: the impostors do not
  // change until the game is started again.
  REX_SETTING(bool, perf_no_impostors, false, "Do not allocate the foliage impostor render targets (dev switch 'noimpostors'). "
    "Distant trees lose their billboards. Applies at the next start of the game.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, perf_no_race_shadows, false, "Drop the shadow pass during races only (dev switch 'noraceshadows').", rex::kDebugOnly, rex::kRestart);

  // ── Render phase culling ────────────────────────────────────────────────────
  // These reactivate the game's own dev command-line switches (see
  // dev_options.cpp). They are not new code paths: the retail renderer still
  // contains every branch, only the switch that reaches it was stripped.
  //
  // perf_no_shadows is the big one. sub_822E47E0 (the renderer ctor) reacts to it
  // with `phase_mask &= 0xFFFF9E1F`, clearing render phase bits 0x20 0x40 0x80
  // 0x100 0x2000 0x4000 in the enable mask at renderer+448. The phase loop in
  // sub_822E6408 only dispatches a phase's draw lists when its bit is set, so
  // those passes stop existing entirely: no shadow-map traversal, no
  // shadowDepth/shadowAlphaDepth/shadowBlend technique draws, no shadow render
  // targets. Restart-only because the mask is computed once, at renderer init.
  REX_SETTING(bool, perf_no_shadows, false, "Drop every real-time shadow render phase. Largest single framerate win; the world "
    "loses cast shadows. Applied straight to the renderer's phase enable mask, so it "
    "takes effect immediately.");

  // Same shape: sub_82310478 skips the prop parse for $/city/<district> entirely.
  // CHECK: when that parse runs. If it is part of the world's set-up at start-up, like the
  // impostor targets, this too is seen only at the next start (tag, description and the pause
  // row's "(ON RELOAD)" would then say restart).
  REX_SETTING(bool, perf_no_trees, false, "Skip loading the prop/foliage set (dev switch 'notrees'). Last resort. Applies on "
    "the next district load.", rex::kDebugOnly);

  REX_SETTING(std::string, perf_shadow_phase_bits, "0x65E0", "Render phase bits perf_no_shadows clears in renderer+448 (hex). 0x65E0 = sun cascades "
    "(0x61E0, what the game's own 'noshadows' clears) plus the night/blend shadow phase "
    "0x400 that it misses. 0x400 alone isolates the night path.", rex::kDebugOnly);

  REX_SETTING(bool, pump_wake_on_demand, true, "The game's frame-pacing thread (it frees the draw command ring's slots and sleeps out "
    "7 ms a round) comes round at once when the main thread is waiting for a slot, instead of "
    "after its Sleep. Lifts the frame rate an uncapped game was held to when a frame takes "
    "less than 7 ms; changes nothing under a frame rate cap below that. Needs 4 or more "
    "logical processors, and a watchdog on the audio mixer turns it off if the audio runs "
    "dry. Off is the game's own Sleep.", rex::kDebugOnly);

  REX_SETTING(bool, single_tile, false, "Render the scene in a single predicated-tiling tile instead of two. Halves draw calls "
    "and state traffic with MSAA on. Requires the enlarged virtual EDRAM (SDK >= this build).", rex::kDebugOnly);

  // Off, because it was measured off. e0d9e94 turned the SDK flag on for the
  // overlap it is supposed to buy, but on the bench (menu_cam slot 8, 40-116 s,
  // three interleaved pairs against a build of ac2453d - the commit before that
  // batch - with the same SDK DLLs) it costs about 7%:
  //
  //   pre-batch build                40.26 fps  (40.47 / 40.25 / 40.07)
  //   this build, flag off           40.88 fps  (40.75 / 40.68 / 41.20)
  //   this build, flag on            38.04 fps  (37.72 / 38.25 / 38.15)
  //
  // Ending a submission at every PM4 primary buffer end trades one batched
  // submit per frame for many small ones; the driver overhead of the extra
  // ExecuteCommandLists and fence signals outweighs the overlap here. It may
  // still win on a faster GPU, which is why this stays a cvar instead of a
  // deletion - set it true in larecomp.toml to measure that.
  REX_SETTING(bool, submit_on_primary_buffer_end, false, "Drive the SDK's d3d12_submit_on_primary_buffer_end. Ends a D3D12 submission at every PM4 "
    "primary buffer end instead of batching the frame. Measured ~7% slower on a GTX 1650. "
    "With graphics_backend = vulkan the SDK's own vulkan_submit_on_primary_buffer_end (on by "
    "default) applies instead; not measured here, so it is left alone.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(f64, traffic_density_scale, 1.0, "Moving traffic density scale (0.0 = no new traffic cars, 1.0 = the game's own, 2.0 = double). It scales the "
    "zones' density at low and at high speed, so it holds at every speed. Live, for the cars that spawn from the "
    "next frame on; cars already on the road stay until they leave by the unspawn radius (traffic_unspawn_dist). "
    "Needs enable_ambient_tuning (on by default). Debug setting: trailers and repeatable tests, where the road is "
    "to stay empty (with ped_density_scale and parked_car_scale at 0).", rex::Range{0.0, 2.0}, rex::kDebugOnly);

  // 0 is auto (gpu/gpu_class.h kScales): 400 (the game's own) on a high-end graphics card, 300 on a mid-range one, 250 on
  // integrated graphics and until the card is known. A value set by hand is held to 100 to 600.
  REX_SETTING(f64, traffic_unspawn_dist, 0.0, "How far from you traffic cars are kept, in metres: a car further away is removed. "
    "0 (auto, the default) is 400 (the game's own) on a high-end graphics card, 300 on a mid-range one and 250 on "
    "integrated graphics. Less is faster (100 to 600; the menu has auto, 100, 150, 200, 250, 300, 400, 500 and 600).", rex::Range{0.0, 600.0});

  REX_SETTING(bool, vsync_fast_poll, true, "Drive the SDK's gpu_vsync_fast_poll. With vsync off the guest vblank interval is 1 ms, so "
    "fast poll keeps the GPU VSync worker spinning a whole core. Off restores the 1 ms sleep.", rex::kDebugOnly, rex::kRestart);

};

inline PerformanceSettings g_performance;

inline auto& breaking_frame_rate_limit = g_performance.breaking_frame_rate_limit;
inline auto& enable_ambient_tuning = g_performance.enable_ambient_tuning;
inline auto& fence_spin_throttle = g_performance.fence_spin_throttle;
inline auto& fma_float_fast = g_performance.fma_float_fast;
inline auto& fps_limit = g_performance.fps_limit;
inline auto& frame_limit_spin = g_performance.frame_limit_spin;
inline auto& frame_pacing = g_performance.frame_pacing;
inline auto& global_max_draw_distance = g_performance.global_max_draw_distance;
inline auto& host_helpers_mask = g_performance.host_helpers_mask;
inline auto& lzx_native = g_performance.lzx_native;
inline auto& pace_at_swap = g_performance.pace_at_swap;
inline auto& parked_car_scale = g_performance.parked_car_scale;
inline auto& ped_density_scale = g_performance.ped_density_scale;
inline auto& perf_fast_vehicle_shadows = g_performance.perf_fast_vehicle_shadows;
inline auto& perf_no_fullscreen_blur = g_performance.perf_no_fullscreen_blur;
inline auto& perf_no_impostors = g_performance.perf_no_impostors;
inline auto& perf_no_race_shadows = g_performance.perf_no_race_shadows;
inline auto& perf_no_shadows = g_performance.perf_no_shadows;
inline auto& perf_no_trees = g_performance.perf_no_trees;
inline auto& perf_shadow_phase_bits = g_performance.perf_shadow_phase_bits;
inline auto& pump_wake_on_demand = g_performance.pump_wake_on_demand;
inline auto& single_tile = g_performance.single_tile;
inline auto& submit_on_primary_buffer_end = g_performance.submit_on_primary_buffer_end;
inline auto& traffic_density_scale = g_performance.traffic_density_scale;
inline auto& traffic_unspawn_dist = g_performance.traffic_unspawn_dist;
inline auto& vsync_fast_poll = g_performance.vsync_fast_poll;

}  // namespace larecomp::settings

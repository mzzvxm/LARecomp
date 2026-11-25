#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class PatchesSettings final : public rex::SettingOwner {
 public:
  PatchesSettings() : SettingOwner("larecomp") {}

  // "auto" is the window's own shape, whatever it is (mc_engine/ultrawide.h): wider than 16:9 widens the
  // view and keeps the HUD 16:9 in the middle (hud_width, below, moves its corner pieces out); taller than
  // 16:9 (16:10 such as 1920x1200 or the Steam Deck's 1280x800, 4:3, 5:4, a portrait window) keeps the
  // game's width and shows more above and below, the HUD again 16:9 in the middle. The others pick a shape
  // whatever the window is: the picture lies in the largest frame of that shape in the window, centred,
  // with pure black bars where the window is not that shape (mc_engine/frame_fit.h). Live: the pause
  // menu's DISPLAY > ASPECT RATIO and the launcher write it while the game runs. The Xenia renderer
  // always follows the window, and keeps 16:9 with bars in a window taller than that.
  REX_SETTING(std::string, aspect_ratio, "auto", "Shape of the picture: auto fills the window at any shape (a 21:9 or 32:9 window gets a wider view, "
    "a 16:10, 4:3 or taller one more above and below; the HUD stays 16:9 in the middle), or 16:9, "
    "16:10, 21:9 or 32:9 whatever the window is, with black bars above and below where the window is "
    "narrower than that shape and at the sides where it is wider.", rex::Allowed("auto", "16:9", "16:10", "21:9", "32:9"));

  // Live: Patch_PhysicsCollision (vehicle.cpp) is a return_on_true hook on the collision function's first instruction and reads this on every
  // call, with no copy. It was tagged kRequiresRestart; nothing reads it at start.
  // CHECK: flipped on and off in a race with cars close together, the cars stop pushing each other and push again, nothing else changes.
  REX_SETTING(bool, break_pairwise_collision, false, "Disables pairwise collision resolution. Live: the game's collision function is skipped "
    "or run again at its next call.", rex::kDebugOnly);

  REX_SETTING(bool, carbon_all_parts, true, "Carbon fiber can be applied to any body part, not just the hood.", rex::kDebugOnly);

  REX_SETTING(bool, dbg_print, false, "Enable DbgPrint console outputs.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, disable_dof, true, "Disable Depth of Field (DoF) completely.");

  // Live: Patch_DisableImposterShadows reads this on every pass through its site in the imposter
  // shadow submission (sub_8230C860) and nothing keeps a copy; the same hook is a plain, switchable
  // toggle in the fork (its FOLIAGE SHADOWS row has no restart mark and was measured switching during
  // play). It was tagged kRequiresRestart, and its pause row said "(RESTART)".
  // CHECK: in daylight, FOLIAGE SHADOWS flipped in the pause menu shows after resuming, no restart.
  // The row is on LIGHTING AND SHADOWS right after SHADOWS and is grey while SHADOWS is off (the sun's shadow maps are not drawn then).
  // CHECK: the hook only skips the refresh, so flipped to true while playing the silhouettes already baked may stay in use (perf\LEGACY-GFX-AUDIT.md B9).
  REX_SETTING(bool, disable_imposter_shadows, true, "Far trees are drawn as flat cards, and each casts a baked shadow silhouette into the sun's shadows. "
    "False refreshes those silhouettes, so far trees cast shadows; true skips the refresh, so they cast none. "
    "Daylight only, and only while perf_no_shadows is false (the pause menu's SHADOWS is on).");

  // Live: Patch_DisableMotionBlur reads this at the two sites that load the blur's enable flag, on
  // every pass through them (nothing keeps a copy), and the pause menu's MOTION BLUR row flips it
  // without a restart. It was tagged kRequiresRestart, which made the F4 panel and the launcher show
  // it as a restart setting.
  REX_SETTING(bool, disable_motion_blur, false, "Disable Motion Blur completely.");

  REX_SETTING(bool, disable_msaa, false, "Disable Anti-Aliasing (MSAA).", rex::kRequiresRestart);

  REX_SETTING(bool, disable_rubberbanding, false, "Disable the AI RubberBand system outright. Note this also removes MinThrottle, the "
    "leash that slows the AI when it is AHEAD — so it makes races harder, not fairer. "
    "Prefer the rubberband_* scales under MCLA/Difficulty.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, garage_tire_offset, true, "Add a TIRE OFFSET row to the garage wheels > dimensions screen, editing "
    "the per-car TireOffset0/1 bytes retail parses but never uses.", rex::kDebugOnly);

  // Live, and only on a picture wider than 16:9 (mc_engine/ultrawide.h, "the HUD's corner pieces"): the
  // guest hooks read it when a piece is drawn and the renderer when it squeezes the piece's draws.
  // The pause menu (INTERFACE > UI, HUD WIDTH) and the launcher name three steps: CENTERED 0, WIDER 50,
  // FULL 100.
  REX_SETTING(i32, hud_width, 0, "On a wide picture, how far the HUD's corner pieces (the minimap, speedometer, race position, lap "
    "and time, and the phone) move out toward the screen edges, in percent: 0 (Centered) keeps the whole HUD in the "
    "middle 16:9, 50 (Wider) is halfway, 100 (Full) puts each piece as far from its edge as on a 16:9 "
    "screen.", rex::Range{0, 100});

  REX_SETTING(bool, intro_original_speed, true, "Present the start-up legal screens and logos 30 times a second, as the console does. "
    "They are drawn by a loop that never runs the engine timer, so fps_limit never reached "
    "them and they played as fast as the host could present. The screens play before the "
    "pause menu can be opened, so a change made there is seen at the next start.");

  REX_SETTING(bool, mcla_fast_exit, true, "Fix: the game ends its process right after its orderly shutdown (saves, settings and logs written, the renderer "
    "and audio stopped), without running the destructors of its global objects. The SDK leaves a game thread that is "
    "slow to leave running while the game ends; a thread like that could still use an object the exit had just "
    "destroyed, and the game crashed a few milliseconds after it said it was exiting (about once in 1400 exits). On: "
    "the process ends at once, with the same exit code. Off: the old end through the C runtime's exit.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_skip_gpu_teardown, true, "Leave the final GPU objects for the OS to reclaim at process exit. Requires mcla_fast_exit. "
    "GPU validation, debug builds and device loss use full teardown. Read at quit.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_fix_checkpoint_fade, true, "Fix: the smoke of a closed race checkpoint dissipates over about half a second at every frame "
    "rate. The game fades it by dt x 25 at every update, which is 0.83 per update on the console's 30 updates a "
    "second but 0.42 at 60 FPS and 0.17 at 144 FPS, so above 30 FPS the smoke vanished with the checkpoint. On: the "
    "same decay per second as the console at any frame rate. Off: the game's own number at every update.", rex::kDebugOnly);

  REX_SETTING(bool, mcla_fix_neon_30hz, true, "Fix: the neon underglow's pulse follows the console's 30 Hz clock, not every frame. A fast flash "
    "rate is a wave of about 30 Hz; the console sampled it 30 times a second and showed a slow throb, "
    "and at 60 FPS or more the same wave blinks about 11 times a second (a strobe). On: every frame "
    "rate shows the console's throb. Off: the pulse is worked out at every frame.", rex::kDebugOnly);

  REX_SETTING(f64, mcla_fix_neon_pulse_pin, -1.0, "Diagnostic (the neon underglow's pulse, for tests): a number from 0 to 1 that every flashing neon shows at every update, whatever the game's clock, so that two runs of a scene "
    "show the same underglow (the pulse is a function of the clock, and a picture A/B of the neon's light cannot be judged when it differs by run). Below 0 (the default): the game's own pulse. Live.", rex::kDebugOnly, rex::Range{-1.0, 1.0});

  // BadassBaboon's Recomp Adjustments: Enable 60 FPS by default
  REX_SETTING(bool, real_frame_delta, true, "Feeds the simulation the measured frame time instead of the engine's fixed "
    "30 Hz timestep, and unlocks presentation from every-other-vblank. Required "
    "for correct physics, camera and traffic above 30 FPS. This is NOT a frame "
    "rate cap - see fps_limit for that.", rex::kDebugOnly);

  REX_SETTING(bool, showroom_unlimited, true, "The garage's showroom lists every vehicle on sale. Off = the game's own showroom: 70 rows, "
    "and a picture for at most 7 vehicles past the game's own.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, skip_intro, false, "Skip the intro videos to prevent graphical issues.", rex::kRequiresRestart);

  REX_SETTING(f64, tire_offset_step, 0.012, "Metres of lateral wheel movement per TIRE OFFSET notch (17 notches, "
    "-8..+8, so the default spans about +/-10 cm). 0 disables the effect while "
    "leaving the garage row usable.", rex::kDebugOnly);

  REX_SETTING(u32, tire_offset_targets, 31, "Bitmask (decimal) of the per-wheel matrix blocks TIRE OFFSET moves: 1 = "
    "model+1088 (the tyre), 2 = +1344, 4 = +1600, 8 = +1856, 16 = +1920. "
    "31 moves all five; drop bits to find which block owns which part.", rex::kDebugOnly);

  REX_SETTING(bool, unlock_ride_height, false, "Allow the full stock ride-height table in the shop (down to rh_800 / -8).");

  REX_SETTING(bool, unlock_wheel_fit, false, "Allow every stock rim size, tire profile, tire width and ride height regardless of the car's clearance metrics.");

  REX_SETTING(bool, vehicle_records_relocate, true, "Keep the parts bought for each car model in a table sized to the roster, so the game "
    "takes more than 77 vehicles. Off = the game's own 77-slot table: past 77 vehicles it "
    "overwrites other profile data, and a large roster crashes the boot.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, warp_transition_fix, true, "Finish a cinematic warp (pause menu -> garage from far away) whose camera "
    "flight ended between two frames. Without it the game can sit on the garage "
    "facade forever: the warper only notices the flight's end by predicting it one "
    "frame early from a duration measured at a fixed 1/30 s step, and real, varying "
    "frame times slip past that prediction.", rex::kDebugOnly);

  REX_SETTING(bool, car_mesh_fixes, true, "Fix car part models the game ships broken: the Impala 64's second rear "
    "bumper, which sits lower than the others.");

  REX_SETTING(bool, garage_camber, true, "Adds a CAMBER row to the garage wheel dimensions screen, editing the "
    "per-car Camber0/1 floats retail saves but was not found to use.");

  REX_SETTING(bool, garage_menu_fixes, true, "Fix garage tune files the game ships broken: the low riders' side "
    "exhausts, the CLK63 AMG's missing 3rd and 4th parts, and the city bus's "
    "missing performance list.");

  REX_SETTING(bool, garage_unlock_parts, false, "For testing: every entry of the garage part menus can be picked, whatever "
    "the career has unlocked. Parts bought with it on stay bought.", rex::kDebugOnly);

  REX_SETTING(bool, owned_cars_keep_all_generations, false, "Keep every set-aside owned-car file, also those a later load proved the save on disk "
    "does not use.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(i32, owned_cars_limit, 30, "How many cars a save can own (30 to 255). 30 = the game's own limit and its own storage; "
    "above 30 the cars past the 30th are kept in mc4_owned_cars.bin beside the save, "
    "and a build without this option shows only the first 30. Saving while it is 30 (or with "
    "a build without it) leaves the cars past the 30th out of that save; they stay on disk "
    "and come back with owned_cars_recover_unmatched.", rex::kRequiresRestart);

  REX_SETTING(bool, owned_cars_poison_static, false, "Test: fill the profile's own 30 car slots with a pattern while the moved block "
    "is in use, so a read that still goes to them shows at once.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, owned_cars_recover_unmatched, false, "For one start: when no owned-car file matches the save loaded and the save owns exactly 30 "
    "cars, add the cars past the 30th from the newest such file beside it (for a save made "
    "while owned_cars_limit was 30). Turn it off again afterwards.", rex::kRequiresRestart);

  REX_SETTING(i32, owned_cars_test_fill, 0, "Test: after a load, copy the current car until the save owns this many cars.", rex::kDebugOnly, rex::kRequiresRestart);

};

inline PatchesSettings& g_patches = larecomp::Leak<PatchesSettings>();

inline auto& aspect_ratio = g_patches.aspect_ratio;
inline auto& break_pairwise_collision = g_patches.break_pairwise_collision;
inline auto& carbon_all_parts = g_patches.carbon_all_parts;
inline auto& dbg_print = g_patches.dbg_print;
inline auto& disable_dof = g_patches.disable_dof;
inline auto& disable_imposter_shadows = g_patches.disable_imposter_shadows;
inline auto& disable_motion_blur = g_patches.disable_motion_blur;
inline auto& disable_msaa = g_patches.disable_msaa;
inline auto& disable_rubberbanding = g_patches.disable_rubberbanding;
inline auto& garage_tire_offset = g_patches.garage_tire_offset;
inline auto& hud_width = g_patches.hud_width;
inline auto& intro_original_speed = g_patches.intro_original_speed;
inline auto& mcla_fast_exit = g_patches.mcla_fast_exit;
inline auto& mcla_skip_gpu_teardown = g_patches.mcla_skip_gpu_teardown;
inline auto& mcla_fix_checkpoint_fade = g_patches.mcla_fix_checkpoint_fade;
inline auto& mcla_fix_neon_30hz = g_patches.mcla_fix_neon_30hz;
inline auto& mcla_fix_neon_pulse_pin = g_patches.mcla_fix_neon_pulse_pin;
inline auto& real_frame_delta = g_patches.real_frame_delta;
inline auto& showroom_unlimited = g_patches.showroom_unlimited;
inline auto& skip_intro = g_patches.skip_intro;
inline auto& tire_offset_step = g_patches.tire_offset_step;
inline auto& tire_offset_targets = g_patches.tire_offset_targets;
inline auto& unlock_ride_height = g_patches.unlock_ride_height;
inline auto& unlock_wheel_fit = g_patches.unlock_wheel_fit;
inline auto& vehicle_records_relocate = g_patches.vehicle_records_relocate;
inline auto& warp_transition_fix = g_patches.warp_transition_fix;
inline auto& car_mesh_fixes = g_patches.car_mesh_fixes;
inline auto& garage_camber = g_patches.garage_camber;
inline auto& garage_menu_fixes = g_patches.garage_menu_fixes;
inline auto& garage_unlock_parts = g_patches.garage_unlock_parts;
inline auto& owned_cars_keep_all_generations = g_patches.owned_cars_keep_all_generations;
inline auto& owned_cars_limit = g_patches.owned_cars_limit;
inline auto& owned_cars_poison_static = g_patches.owned_cars_poison_static;
inline auto& owned_cars_recover_unmatched = g_patches.owned_cars_recover_unmatched;
inline auto& owned_cars_test_fill = g_patches.owned_cars_test_fill;

}  // namespace larecomp::settings

#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

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

  REX_SETTING(bool, break_pairwise_collision, false, "Disables pairwise collision resolution.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, carbon_all_parts, true, "Carbon fiber can be applied to any body part, not just the hood.", rex::kDebugOnly);

  REX_SETTING(bool, dbg_print, false, "Enable DbgPrint console outputs.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, disable_dof, true, "Disable Depth of Field (DoF) completely.");

  // Live: Patch_DisableImposterShadows reads this on every pass through its site in the imposter
  // shadow submission (sub_8230C860) and nothing keeps a copy; the same hook is a plain, switchable
  // toggle in the fork (its FOLIAGE SHADOWS row has no restart mark and was measured switching during
  // play). It was tagged kRequiresRestart, and its pause row said "(RESTART)".
  // CHECK: in daylight, FOLIAGE SHADOWS flipped in the pause menu shows after resuming, no restart.
  // The default is false (foliage casts shadows) since 2026-11-20, the owner's LOD defaults pass: the fork measured foliage shadows at -0.07 ms of 8.3 on an RTX 5080 and
  // -0.09 to -0.10 ms of 39 to 43 on the 2-CU integrated GPU (launcher/dev/aniso-2026-10-27.md, the detail options table), so the shadows of
  // trees are on for every graphics card. CHECK: a tree-heavy scene on the integrated GPU (perf\scripts\chain-lod-defaults.ps1, arm foliage).
  REX_SETTING(bool, disable_imposter_shadows, false, "Trees and plants do not cast shadows. Off (the default) lets them cast shadows; on is a little faster "
    "on a weak graphics card.");

  // Live: Patch_DisableMotionBlur reads this at the two sites that load the blur's enable flag, on
  // every pass through them (nothing keeps a copy), and the pause menu's MOTION BLUR row flips it
  // without a restart. It was tagged kRequiresRestart, which made the F4 panel and the launcher show
  // it as a restart setting.
  REX_SETTING(bool, disable_motion_blur, false, "Disable Motion Blur completely.");

  REX_SETTING(bool, disable_msaa, false, "Disable Anti-Aliasing (MSAA).", rex::kRestart);

  REX_SETTING(bool, disable_rubberbanding, false, "Disable the AI RubberBand system outright. Note this also removes MinThrottle, the "
    "leash that slows the AI when it is AHEAD — so it makes races harder, not fairer. "
    "Prefer the rubberband_* scales under MCLA/Difficulty.", rex::kDebugOnly, rex::kRestart);

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

  REX_SETTING(bool, mcla_fix_neon_30hz, true, "Fix: the neon underglow's pulse follows the console's 30 Hz clock, not every frame. A fast flash "
    "rate is a wave of about 30 Hz; the console sampled it 30 times a second and showed a slow throb, "
    "and at 60 FPS or more the same wave blinks about 11 times a second (a strobe). On: every frame "
    "rate shows the console's throb. Off: the pulse is worked out at every frame.", rex::kDebugOnly);

  // BadassBaboon's Recomp Adjustments: Enable 60 FPS by default
  REX_SETTING(bool, real_frame_delta, true, "Feeds the simulation the measured frame time instead of the engine's fixed "
    "30 Hz timestep, and unlocks presentation from every-other-vblank. Required "
    "for correct physics, camera and traffic above 30 FPS. This is NOT a frame "
    "rate cap - see fps_limit for that.", rex::kDebugOnly);

  REX_SETTING(bool, showroom_unlimited, true, "The garage's showroom lists every vehicle on sale. Off = the game's own showroom: 70 rows, "
    "and a picture for at most 7 vehicles past the game's own.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, skip_intro, false, "Skip the intro videos to prevent graphical issues.", rex::kRestart);

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
    "overwrites other profile data, and a large roster crashes the boot.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, warp_transition_fix, true, "Finish a cinematic warp (pause menu -> garage from far away) whose camera "
    "flight ended between two frames. Without it the game can sit on the garage "
    "facade forever: the warper only notices the flight's end by predicting it one "
    "frame early from a duration measured at a fixed 1/30 s step, and real, varying "
    "frame times slip past that prediction.", rex::kDebugOnly);

};

inline PatchesSettings g_patches;

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
inline auto& mcla_fix_neon_30hz = g_patches.mcla_fix_neon_30hz;
inline auto& real_frame_delta = g_patches.real_frame_delta;
inline auto& showroom_unlimited = g_patches.showroom_unlimited;
inline auto& skip_intro = g_patches.skip_intro;
inline auto& tire_offset_step = g_patches.tire_offset_step;
inline auto& tire_offset_targets = g_patches.tire_offset_targets;
inline auto& unlock_ride_height = g_patches.unlock_ride_height;
inline auto& unlock_wheel_fit = g_patches.unlock_wheel_fit;
inline auto& vehicle_records_relocate = g_patches.vehicle_records_relocate;
inline auto& warp_transition_fix = g_patches.warp_transition_fix;

}  // namespace larecomp::settings

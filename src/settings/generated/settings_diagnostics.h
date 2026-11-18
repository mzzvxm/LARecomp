#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class DiagnosticsSettings final : public rex::SettingOwner {
 public:
  DiagnosticsSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, cam_trace, false, "Write cam_trace.csv: every game update's camera (position, direction, shake), the player "
    "car's position and the clock's time step, every guest swap, and when each frame was "
    "presented and reached the screen. For measuring camera judder.", rex::kDebugOnly);

  REX_SETTING(bool, lowcpu_probe, false, "Every 10 s, two lines in the log about a PC with few cores: how late the game's audio callback "
    "and its 1 ms clock tick were (counts of gaps over 8, 12 and 20 ms, clock staleness) and the "
    "CPU time of the audio worker and the timer-queue thread. Only reads. For crackling sound and "
    "stutter when the processor is busy: start the game with it, reproduce the problem, send the "
    "log.", rex::kDebugOnly);

  REX_SETTING(bool, lzx_native_check_all, false, "Run the recompiled LZX decoder next to the native one for every stream, not only the first few, "
    "and compare every call. Slower than either alone: for validating a whole session. Read at start.", rex::kDebugOnly, rex::kRestart);

  REX_SETTING(bool, photo_ui_trace, false, "Log the UI statechart while the photo album scene is up (photo mode, the album, "
    "the Rate My Ride snapshots): events posted and dispatched, delayed events, the "
    "UI.* and photoAlbum.* commands with the states they touch, and the paced movies' "
    "frame counts every 10 seconds. Silent everywhere else.", rex::kDebugOnly);

  // Diagnostic: dumps every tune field as it registers. Off by default -- it fires for
  // every tune in the game (about 1100 unique names across ~28 classes) -- but it is the
  // only way to see a tune's live layout, so it stays.
  REX_SETTING(bool, tune_field_probe, false, "Log every tune field name as it registers.", rex::kDebugOnly);

  REX_SETTING(bool, ui_color_log, false, "Log every distinct Flash UI color (movie, draw site, ARGB, bound texture) "
    "with a running count, to measure the families ui_theme_* recolors.", rex::kDebugOnly);

  REX_SETTING(bool, warp_transition_test_miss, false, "Testing aid: hide the running transition from the warper's own end check, so "
    "every cinematic warp ends the way the stuck ones do -- the transition finishes "
    "and reports it while the warper is not looking. With warp_transition_fix off "
    "this reproduces the stuck garage facade on demand.", rex::kDebugOnly);

  REX_SETTING(bool, warp_transition_trace, false, "Log every frame the warper spends waiting on a cinematic warp transition: the "
    "transition's time, duration, active flag and the frame delta.", rex::kDebugOnly);

};

inline DiagnosticsSettings g_diagnostics;

inline auto& cam_trace = g_diagnostics.cam_trace;
inline auto& lowcpu_probe = g_diagnostics.lowcpu_probe;
inline auto& lzx_native_check_all = g_diagnostics.lzx_native_check_all;
inline auto& photo_ui_trace = g_diagnostics.photo_ui_trace;
inline auto& tune_field_probe = g_diagnostics.tune_field_probe;
inline auto& ui_color_log = g_diagnostics.ui_color_log;
inline auto& warp_transition_test_miss = g_diagnostics.warp_transition_test_miss;
inline auto& warp_transition_trace = g_diagnostics.warp_transition_trace;

}  // namespace larecomp::settings

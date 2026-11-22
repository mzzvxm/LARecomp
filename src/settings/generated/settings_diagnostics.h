#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

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
    "and compare every call. Slower than either alone: for validating a whole session. Read at start.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(i32, mcla_traffic_probe, 0, "Debug, live, never saved: every this many updates log the ambient (traffic) cars one by one (model, position, heading, speed, the rail, "
    "the LOD), the ambient and layout pools (capacity, live count, free slots), the opponent and racer pools and the police patrols, and one "
    "SUMMARY line, all as '[traffic-probe]' lines. It reads the game and changes nothing. 0 = off.", rex::kDebugOnly, rex::Range{0, 36000});

  REX_SETTING(f64, mcla_traffic_probe_ahead, 25.0, "Debug: how far in front of the player's car, in metres, the spawn test places the car.", rex::kDebugOnly, rex::Range{5.0, 100.0});

  REX_SETTING(i32, mcla_traffic_probe_detail, 1, "Debug: how much of each traffic probe sample is logged. 0 = the pools, the census and the SUMMARY line; 1 = and the 20 nearest cars; 2 = "
    "and every car (up to 200). The census always counts all of them.", rex::kDebugOnly, rex::Range{0, 2});

  REX_SETTING(std::string, mcla_traffic_probe_model, "", "Debug: the ambient type (the car's model name in the game's own list, logged by the traffic probe as 'types') the spawn test asks for. "
    "Empty = the first regular car type of the game's list.", rex::kDebugOnly);

  REX_SETTING(i32, mcla_traffic_probe_spawn, 0, "Debug, live, never saved: a set to a number other than 0 (and other than the last one acted on) makes ONE call to the game's "
    "AmbientMgr_AddAmbientToLayout, asking it to place an ambient car of mcla_traffic_probe_model mcla_traffic_probe_ahead metres in front "
    "of the player, and follows what comes of it in the log for two minutes. Free roam only, with a layout slot free, at most three calls "
    "in a run of the game; a refusal is logged with its reason. 0 = off and re-arms (the same number again does nothing).", rex::kDebugOnly, rex::Range{0, 1000000});

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

inline DiagnosticsSettings& g_diagnostics = larecomp::Leak<DiagnosticsSettings>();

inline auto& cam_trace = g_diagnostics.cam_trace;
inline auto& lowcpu_probe = g_diagnostics.lowcpu_probe;
inline auto& lzx_native_check_all = g_diagnostics.lzx_native_check_all;
inline auto& mcla_traffic_probe = g_diagnostics.mcla_traffic_probe;
inline auto& mcla_traffic_probe_ahead = g_diagnostics.mcla_traffic_probe_ahead;
inline auto& mcla_traffic_probe_detail = g_diagnostics.mcla_traffic_probe_detail;
inline auto& mcla_traffic_probe_model = g_diagnostics.mcla_traffic_probe_model;
inline auto& mcla_traffic_probe_spawn = g_diagnostics.mcla_traffic_probe_spawn;
inline auto& photo_ui_trace = g_diagnostics.photo_ui_trace;
inline auto& tune_field_probe = g_diagnostics.tune_field_probe;
inline auto& ui_color_log = g_diagnostics.ui_color_log;
inline auto& warp_transition_test_miss = g_diagnostics.warp_transition_test_miss;
inline auto& warp_transition_trace = g_diagnostics.warp_transition_trace;

}  // namespace larecomp::settings

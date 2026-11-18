#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class CutscenesSettings final : public rex::SettingOwner {
 public:
  CutscenesSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, cutscene_bind_coordsys, false, "Anchor the scene to the player's car before it plays, through the game's "
    "own CineScript_SetGameCoordSysBindingCopyRacer. A cutscene is authored "
    "around its own origin, not in world space; the gameplay script that "
    "normally plays one WARPS the player to the scene and leaves the binding "
    "at zero, which is why an all-zero binding looked correct when this was "
    "measured on a real garage entry. A replay does not warp, so with no "
    "anchor the scene plays at the world origin -- measured: the player's car "
    "went from (819.6, 17.5, -808.4) to (-0.1, -2.8, -5.6) across one replay, "
    "which is both the car that will not drive afterwards and the actors that "
    "walk on nothing. Anchoring brings the scene to the car instead, which is "
    "what the reward and cop-pullover scenes do natively. Off by default: it "
    "made things worse in play, so the anchor stays behind this flag until "
    "there is a measurement that says what it broke.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_debug, false, "Dump the whole script-thread tree and the scrThread array with each "
    "cutscene gate report. Off by default: it is ~200 lines a shot.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_force_camera, false, "Run the camera event's own start handler (sub_823D7280) on the event that "
    "Camera_LaunchEvent queued, if the cutscene camera is still idle a frame "
    "later. That handler is the only thing in the game that writes the camera's "
    "clock (cineCam+120), and mcCineDirector::IsCameraActive (sub_823CDC90) "
    "refuses to hand the screen to the cutscene camera while that clock is "
    "negative -- so if the event is queued but never dispatched, this is exactly "
    "the work the game skipped, done with the game's own function and the game's "
    "own event object. It is a no-op when the camera is already running.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_force_events, false, "Diagnostic only, and it does NOT help a replay: mgr+305 turned out to be "
    "the SKIP request, not a 'scene is playing' flag. sub_823BEEB0 reads it, "
    "asks sub_823BBB98 for the time of the next cut event, and jumps the scene "
    "clock there -- measured: the garage scene picked 2.833s and ended in four "
    "frames, and district champs picked 74.233s and its clock went 0.036 -> "
    "74.254 in one frame. Leave it off; it is kept only to reproduce that.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_keep_movies, true, "Clear mgr+316 before the scene tears down, so FinishScene does not run its "
    "movie teardown. That branch is `if (mgr+316) { sub_821F6238(movies); "
    "sub_821F15C8(...); }`, and sub_821F6238 walks four movie slots calling "
    "sub_821F4C18, which does `v4 = slot[2]; if (v4) { *v4 = 0; slot[2] = 0; }` "
    "-- it writes zero over the first dword of whatever that slot points at, and "
    "the first dword of an object is its vtable. The pause menu is a Flash movie "
    "(PAUSEMOVIE), and reopening it after a replay died on exactly that: a vfunc "
    "call through an object whose vtable had been zeroed. mgr+316 is set by "
    "SetSceneName, so every replay took that branch. A mission survives it "
    "because the flow rebuilds the movies on the way back to gameplay; a replay "
    "has no flow. The cost of skipping it is the scene's subtitle movie not "
    "being released -- a leak, not a crash.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_menu_autoclose, true, "Let a scene picked from the pause menu start by itself: unpause the game "
    "with the pause manager's own ResumeOnly request, play, then hand the menu "
    "by calling the same function the Continue item calls (sub_8265F428), so the "
    "menu closes through the game's own transition -- the one that unpauses and "
    "emits resume/ENTERNORMAL/HUDMOVIE. Turn it off to arm the scene and close "
    "the menu by hand instead.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_pad_racers, true, "Repeat the player into the cutscene's remaining car slots when the world "
    "has fewer racers than the scene wants. Wrong on screen -- several cars end "
    "up being the same one -- but it is what gets a scene past its "
    "'insufficient number of racers slotted' guard in free roam.", rex::kDebugOnly);

  REX_SETTING(std::string, cutscene_play_now, "", "Set to a cutscene name to play it immediately, without going through the "
    "menu. Clears itself once the scene starts.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_probe_log, false, "Print the per-frame trace of a replay: which characters stream in, every "
    "actor launch with its clip and entity, the camera hand-over, and each cine "
    "event as it fires with its name from the game's own table. Off by default "
    "because a 74 second scene is a few hundred lines. The things that mean "
    "something went wrong -- a bailed script, a wedged scene, a stuck release "
    "counter -- are always printed regardless.", rex::kDebugOnly);

  REX_SETTING(std::string, cutscene_replay, "cut_story_district_champs", "Which cutscene the pause menu's CUTSCENES tab replays. Names match the "
    "scene's animation pack under $/resources/animation/cineanimpacks. The "
    "default is a hangout exit because those need no cars: an offline scan of "
    "the generated scripts shows the cut_hangout_* scenes make zero Cars_* and "
    "zero AssignNameToSlot calls, while the story scenes want four racers that "
    "free roam does not have.");

  REX_SETTING(std::string, cutscene_run_script, "", "Start any script object by name, relative to $/script_obj, on the next "
    "frame. Meant for the game's own cutscene harness -- "
    "'tools/CutsceneTests/SETUP_GENERIC_CUTSCENE' slots the racers and "
    "characters a scene needs. Clears itself once the script is mounted.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_setup, true, "Run $/script_obj/tools/CutsceneTests/SETUP_GENERIC_CUTSCENE before the "
    "scene. It is the game's own standalone-cutscene harness: it slots four "
    "racers and nine character sets, which is what a generated cutscene script "
    "assumes and bails out without.", rex::kDebugOnly);

  REX_SETTING(bool, cutscene_skip_uses_teardown, false, "Make the skip button end a REPLAY through our own teardown instead of the "
    "game's skip path. Off, because the native path is the one that restores "
    "anything: it does not kill the scene, it pushes the scene clock to the next "
    "cut event, and SetTime then runs every event in between -- including the "
    "tail (fade_to_black, bookmark_cut_to_game, fade_to_scene) that hands the "
    "camera back, brings the HUD in and stops the speech. Our teardown skips all "
    "of that, which is why it left the audio playing and nothing restored. Kept "
    "only as an escape hatch if a scene wedges with no cut event to jump to.", rex::kDebugOnly);

  REX_SETTING(std::string, cutscene_stop, "", "Abort the running cutscene and dump which of mcCineScript's gates never "
    "opened. 'kill' raises the script's own kill flag; 'finish' runs the "
    "teardown directly, which is what gets the HUD back from a wedged scene.", rex::Allowed("", "kill", "finish"), rex::kDebugOnly);

};

inline CutscenesSettings g_cutscenes;

inline auto& cutscene_bind_coordsys = g_cutscenes.cutscene_bind_coordsys;
inline auto& cutscene_debug = g_cutscenes.cutscene_debug;
inline auto& cutscene_force_camera = g_cutscenes.cutscene_force_camera;
inline auto& cutscene_force_events = g_cutscenes.cutscene_force_events;
inline auto& cutscene_keep_movies = g_cutscenes.cutscene_keep_movies;
inline auto& cutscene_menu_autoclose = g_cutscenes.cutscene_menu_autoclose;
inline auto& cutscene_pad_racers = g_cutscenes.cutscene_pad_racers;
inline auto& cutscene_play_now = g_cutscenes.cutscene_play_now;
inline auto& cutscene_probe_log = g_cutscenes.cutscene_probe_log;
inline auto& cutscene_replay = g_cutscenes.cutscene_replay;
inline auto& cutscene_run_script = g_cutscenes.cutscene_run_script;
inline auto& cutscene_setup = g_cutscenes.cutscene_setup;
inline auto& cutscene_skip_uses_teardown = g_cutscenes.cutscene_skip_uses_teardown;
inline auto& cutscene_stop = g_cutscenes.cutscene_stop;

}  // namespace larecomp::settings

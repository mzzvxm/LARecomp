#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Cutscene gallery. See cutscene_gallery.h for the RE map; this file is the
// state machine that gets the pause menu off screen and starts the scene's
// script thread.
//

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/function_dispatcher.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "cutscene_gallery.h"
#include "pause_menu.h"
#include "logging.h"

REXCVAR_DEFINE_STRING(cutscene_replay, "cut_story_district_champs", "MCLA/Cutscenes",
    "Which cutscene the pause menu's CUTSCENES tab replays. Names match the "
    "scene's animation pack under $/resources/animation/cineanimpacks. The "
    "default is a hangout exit because those need no cars: an offline scan of "
    "the generated scripts shows the cut_hangout_* scenes make zero Cars_* and "
    "zero AssignNameToSlot calls, while the story scenes want four racers that "
    "free roam does not have.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(cutscene_play_now, "", "MCLA/Cutscenes",
    "Set to a cutscene name to play it immediately, without going through the "
    "menu. Clears itself once the scene starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_force_events, false, "MCLA/Cutscenes",
    "Diagnostic only, and it does NOT help a replay: mgr+305 turned out to be "
    "the SKIP request, not a 'scene is playing' flag. sub_823BEEB0 reads it, "
    "asks sub_823BBB98 for the time of the next cut event, and jumps the scene "
    "clock there -- measured: the garage scene picked 2.833s and ended in four "
    "frames, and district champs picked 74.233s and its clock went 0.036 -> "
    "74.254 in one frame. Leave it off; it is kept only to reproduce that.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_force_camera, false, "MCLA/Cutscenes",
    "Run the camera event's own start handler (sub_823D7280) on the event that "
    "Camera_LaunchEvent queued, if the cutscene camera is still idle a frame "
    "later. That handler is the only thing in the game that writes the camera's "
    "clock (cineCam+120), and mcCineDirector::IsCameraActive (sub_823CDC90) "
    "refuses to hand the screen to the cutscene camera while that clock is "
    "negative -- so if the event is queued but never dispatched, this is exactly "
    "the work the game skipped, done with the game's own function and the game's "
    "own event object. It is a no-op when the camera is already running.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_menu_autoclose, true, "MCLA/Cutscenes",
    "Let a scene picked from the pause menu start by itself: unpause the game "
    "with the pause manager's own ResumeOnly request, play, then hand the menu "
    "by calling the same function the Continue item calls (sub_8265F428), so the "
    "menu closes through the game's own transition -- the one that unpauses and "
    "emits resume/ENTERNORMAL/HUDMOVIE. Turn it off to arm the scene and close "
    "the menu by hand instead.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_skip_uses_teardown, false, "MCLA/Cutscenes",
    "Make the skip button end a REPLAY through our own teardown instead of the "
    "game's skip path. Off, because the native path is the one that restores "
    "anything: it does not kill the scene, it pushes the scene clock to the next "
    "cut event, and SetTime then runs every event in between -- including the "
    "tail (fade_to_black, bookmark_cut_to_game, fade_to_scene) that hands the "
    "camera back, brings the HUD in and stops the speech. Our teardown skips all "
    "of that, which is why it left the audio playing and nothing restored. Kept "
    "only as an escape hatch if a scene wedges with no cut event to jump to.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_keep_movies, true, "MCLA/Cutscenes",
    "Clear mgr+316 before the scene tears down, so FinishScene does not run its "
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
    "being released -- a leak, not a crash.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_probe_log, false, "MCLA/Cutscenes",
    "Print the per-frame trace of a replay: which characters stream in, every "
    "actor launch with its clip and entity, the camera hand-over, and each cine "
    "event as it fires with its name from the game's own table. Off by default "
    "because a 74 second scene is a few hundred lines. The things that mean "
    "something went wrong -- a bailed script, a wedged scene, a stuck release "
    "counter -- are always printed regardless.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(cutscene_run_script, "", "MCLA/Cutscenes",
    "Start any script object by name, relative to $/script_obj, on the next "
    "frame. Meant for the game's own cutscene harness -- "
    "'tools/CutsceneTests/SETUP_GENERIC_CUTSCENE' slots the racers and "
    "characters a scene needs. Clears itself once the script is mounted.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_setup, true, "MCLA/Cutscenes",
    "Run $/script_obj/tools/CutsceneTests/SETUP_GENERIC_CUTSCENE before the "
    "scene. It is the game's own standalone-cutscene harness: it slots four "
    "racers and nine character sets, which is what a generated cutscene script "
    "assumes and bails out without.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_bind_coordsys, false, "MCLA/Cutscenes",
    "Anchor the scene to the player's car before it plays, through the game's "
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
    "there is a measurement that says what it broke.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_pad_racers, true, "MCLA/Cutscenes",
    "Repeat the player into the cutscene's remaining car slots when the world "
    "has fewer racers than the scene wants. Wrong on screen -- several cars end "
    "up being the same one -- but it is what gets a scene past its "
    "'insufficient number of racers slotted' guard in free roam.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cutscene_debug, false, "MCLA/Cutscenes",
    "Dump the whole script-thread tree and the scrThread array with each "
    "cutscene gate report. Off by default: it is ~200 lines a shot.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(cutscene_stop, "", "MCLA/Cutscenes",
    "Abort the running cutscene and dump which of mcCineScript's gates never "
    "opened. 'kill' raises the script's own kill flag; 'finish' runs the "
    "teardown directly, which is what gets the HUD back from a wedged scene.")
    .allowed({"", "kill", "finish"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// ── Guest addresses ───────────────────────────────────────────────────

// mcScriptManager singleton, stored by its ctor sub_827279A0.
constexpr uint32_t kScriptMgrPtr    = 0x8290A9F8;
// sub_82727908(name, parent, flag, argA, argB, stackSize)
constexpr uint32_t kStartThreadFn   = 0x82727908;
constexpr uint32_t kGuestMallocFn   = 0x82130528;
// mcCineScript::IsScenePlaying, sub_823BBF80. True from SetSceneName until
// FinishScene, which is exactly the window a replay occupies.
constexpr uint32_t kIsScenePlayingFn = 0x823BBF80;
// mcCineScript::FinishScene, sub_823C37F8 -- the teardown the script itself
// runs last. Used only as the escape hatch when a scene wedges.
constexpr uint32_t kFinishSceneFn   = 0x823C37F8;
// mcCineScript's camera-animation lookup, sub_823C1260. It builds the
// dictionary name "cutscene/<scene name>" and returns the `CSCamera` clip in
// the streamed animpack, or 0 when there is none. A getter with no side
// effects, so it is safe to poll.
//
// This is the one thing that explains audio and subtitles playing while
// nothing renders and the scene never ends: the speech stream and the subtitle
// movie run on their own, but without a camera clip there is no framing and no
// duration for the scene to run out.
constexpr uint32_t kCameraAnimFn    = 0x823C1260;
// mcCineScript singleton. Null before the city is up, which is also the point
// before which no cutscene can run.
constexpr uint32_t kCineScriptPtr   = 0x8286D8F0;
// Game_IsPaused (sub_822BF228) reads *(u8*)(p+4) && !*(u8*)(p+10).
constexpr uint32_t kCineEventState = 0x828CD53C;  // 0 idle, 1..3 transitioning
constexpr uint32_t kCineDirector   = 0x8286D8E8;

// sub_823CC200 does ++dword_828CD538 when a hand-over starts, and the only
// thing that puts it back to zero is sub_823CE098's state 3 -- the far end of
// the transition. A replay that ends early never gets there, so the counter is
// left standing and the game goes on believing a cutscene hand-over is in
// progress: sub_823CC2E8 stops caching the camera, and the HUD and the pause
// menu stay away. Measured: driving comes back once the racers are resumed, but
// the HUD never returns and Start does nothing.
constexpr uint32_t kCineTransitions = 0x828CD538;

constexpr uint32_t kPauseStatePtr   = 0x828CCEA0;

// The pause manager takes requests through a ten-deep ring rather than having
// its state poked directly: sub_822D26D0(mgr, code) queues one and sub_822D29E8
// drains it. The codes are named in the game's own table at off_827E91A8, and
// the switch in the pump says what each one does:
//
//   1 PauseSimulation / 7 PauseOnly   -> paused = 1
//   2 ResumeSimulation / 8 ResumeOnly -> paused = 0
//   3 PauseLocally / 4 ResumeLocally  -> also touch the racers
//   5 FastForward   6 FrameAdvance    -> let the world tick once while paused
//
// PauseOnly/ResumeOnly are the pair that leaves everything else alone, which is
// what a replay wants: unpause to let the scene run, pause again to hand the
// menu back.
// The game's own "close the pause menu". sub_82668660 is the pause menu's key
// dispatcher (a1 = controller, a2 = key); on key 55 (accept) it walks the item
// names, and for PM_Continue it does exactly one thing:
//
//     if (sub_82661508("PM_Continue", 1)) { sub_8265F428(); return 1; }
//
// So this is the whole of Continue. It takes no arguments and it is the only
// supported way out of the menu: it runs the game's own transition, which is
// what emits 'resume' / 'ENTERNORMAL' / 'HUDMOVIE' and unpauses. Everything this
// file tried before -- popping the navigation stack, queueing ResumeOnly -- was
// an imitation of it, and each imitation broke something else (the HUD lives on
// the screens a stray pop unwinds; unpausing alone leaves the menu drawn).
constexpr uint32_t kPauseMenuContinueFn = 0x8265F428;

constexpr uint32_t kPauseRequestFn  = 0x822D26D0;
constexpr uint32_t kReqPauseSim      = 1;
constexpr uint32_t kReqResumeSim     = 2;
constexpr uint32_t kReqPauseLocally  = 3;
constexpr uint32_t kReqResumeLocally = 4;
constexpr uint32_t kReqPauseOnly    = 7;
constexpr uint32_t kReqResumeOnly   = 8;
// Dev switches that would make the scene fail silently: `nocutscenes` skips
// them outright and `nocineanimpacks` makes DoesCutsceneAnimPackExist return 0.
constexpr uint32_t kNoCutscenesFlag = 0x828CD45C;
constexpr uint32_t kNoAnimPacksFlag = 0x828CD498;
// `bypasscutscenecam`: the game's own switch for running a cutscene with the
// gameplay camera instead of the scene's. If it is on, the cutscene camera is
// never installed -- which looks exactly like what is happening.
constexpr uint32_t kBypassCutsceneCam = 0x828CD55C;

// StartNewThread only accepts these three; anything else is logged and forced
// to 1500. core_rolling_prototype is a deep script, so take the big one.
constexpr uint32_t kScriptStackSize = 3800;

// ── Cutscene table ────────────────────────────────────────────────────
//
// Produced by `tools/sco.py <archive> cutscenes`, which cross-references the
// 244 animation packs against the generated scripts and prints the pair. Only
// the scenes worth watching are listed: the race-start grid stingers
// (gen_prerace_*), the cop stop/arrest beats (cut_cop_*) and the safety cam
// are gameplay furniture, and several of them assume racer slots the free
// roam has no way to fill.
//
// `dir` is the subdirectory the script sits in, relative to
// Game/CineScripts/generated. The build put them in three different places and
// the name alone does not tell you which.
//
// Only scenes whose script fires a `fade_to_scene` event are listed -- that is
// the event a cutscene that stands on its own opens with. The others were tried
// and do not belong in a gallery:
//
//   * the 11 cut_hangout_*_ext scenes open with `menu_to_cam`, a transition out
//     of the hangout menu. Played from free roam the camera starts from a state
//     that does not exist and the screen shows nothing, even though the script
//     runs correctly for its full ~130 frames.
//   * the cut_hangout_*_ent_* scripts and cut_hangout_02_ext have no
//     Events_LaunchEvent calls at all, so there is nothing in them to play.
//
// `cars` is how many Cars_* call sites the script has, which tracks how many
// racers it expects slotted. Free roam only ever has the player, so the zero-car
// scenes are the ones that can work today; the rest need vehicles spawned first,
// the way the per-cutscene test scripts in tools/CutsceneTests do.

struct Cutscene {
    const char* id;     // animation pack / cvar value
    const char* label;  // menu text
    const char* dir;    // "" | "Story" | "hangout" | "RaceStart"
    uint8_t cars;       // Cars_* call sites in the script, i.e. how many
                        // racers it expects to find slotted
};

constexpr Cutscene kCutscenes[] = {
    // No cars at all: these are the ones a free-roam replay can satisfy today.
    {"cut_story_district_champs",             "DISTRICT CHAMPS",           "Story", 0},
    {"cut_story_district_champs_night",       "DISTRICT CHAMPS (NIGHT)",   "Story", 0},
    {"cut_la_hw_oh_garage_01x_ext",           "HOLLYWOOD GARAGE",          "", 0},
    {"cut_la_be_smb_garage_01x_ext",          "BEACH GARAGE",              "", 0},

    // Needs 1 car slotted.
    {"cut_intro",                             "INTRO",                     "Story", 1},
    {"cut_story_find_sh_04",                  "FIND SH 04",                "Story", 1},
    {"cut_story_hollywood_garage_intro",      "HOLLYWOOD GARAGE INTRO",    "Story", 1},
    {"cut_loss_chung_hee",                    "LOSS: CHUNG HEE",           "", 1},
    {"cut_loss_jin",                          "LOSS: JIN",                 "", 1},
    {"cut_loss_lester",                       "LOSS: LESTER",              "", 1},
    {"cut_loss_pete",                         "LOSS: PETE",                "", 1},
    {"sc_cut_garage_01_ext",                  "SC GARAGE",                 "", 1},
    {"sc_cut_loss_ONE",                       "SC LOSS ONE",               "", 1},
    {"sc_cut_loss_TWO",                       "SC LOSS TWO",               "", 1},

    // Needs 2 cars slotted.
    {"cut_reward_gen_01",                     "REWARD 01",                 "", 2},
    {"cut_reward_gen_02",                     "REWARD 02",                 "", 2},
    {"cut_reward_gen_03",                     "REWARD 03",                 "", 2},
    {"cut_reward_gen_04",                     "REWARD 04",                 "", 2},
    {"cut_reward_karol",                      "REWARD: KAROL",             "", 2},
    {"cut_reward_marcel",                     "REWARD: MARCEL",            "", 2},
    {"cut_reward_nikolai",                    "REWARD: NIKOLAI",           "", 2},
    {"cut_story_hangout_series_downtown",     "HANGOUT SERIES DOWNTOWN",   "Story", 2},
    {"cut_story_motorcycle_rep",              "MOTORCYCLE REP",            "Story", 2},
    {"sc_cut_reward_gen_01",                  "SC REWARD 01",              "", 2},
    {"sc_cut_reward_hookman_01",              "SC REWARD: HOOKMAN",        "", 2},

    // Needs 3 cars slotted.
    {"cut_story_payback_intro",               "PAYBACK INTRO",             "Story", 3},
};
constexpr int kCutsceneCount = int(sizeof(kCutscenes) / sizeof(kCutscenes[0]));

// Flattened columns for the pause menu's value row, which takes two parallel
// const char* arrays rather than a table of structs.
const char* g_ids[kCutsceneCount];
const char* g_labels[kCutsceneCount];

struct ColumnInit {
    ColumnInit() {
        for (int i = 0; i < kCutsceneCount; ++i) {
            g_ids[i] = kCutscenes[i].id;
            g_labels[i] = kCutscenes[i].label;
        }
    }
};
const ColumnInit g_column_init;

// ── Guest memory / call helpers ───────────────────────────────────────

uint8_t* Membase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

uint32_t ReadGuestBE32(uint32_t ea) {
    uint8_t* base = Membase();
    if (!base || !ea) return 0;
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

float ReadGuestFloat(uint32_t ea) {
    const uint32_t bits = ReadGuestBE32(ea);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

uint8_t ReadGuestU8(uint32_t ea) {
    uint8_t* base = Membase();
    return (base && ea) ? base[ea] : 0;
}

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return nullptr;
    return rt->function_dispatcher()->GetFunction(addr);
}

void WriteGuestBE32(uint32_t ea, uint32_t v) {
    uint8_t* base = Membase();
    if (!base || !ea) return;
    base[ea + 0] = uint8_t(v >> 24);
    base[ea + 1] = uint8_t(v >> 16);
    base[ea + 2] = uint8_t(v >> 8);
    base[ea + 3] = uint8_t(v);
}

uint32_t CallGuestMalloc(uint32_t size) {
    PPCFunc* fn = GuestFn(kGuestMallocFn);
    return fn ? rex::ppc::GuestToHostFunction<uint32_t>(fn, size) : 0;
}

uint32_t AllocGuestString(const char* str) {
    uint8_t* base = Membase();
    const uint32_t len = uint32_t(std::strlen(str) + 1);
    uint32_t buf = CallGuestMalloc(len);
    if (base && buf) std::memcpy(base + buf, str, len);
    return buf;
}

// ── Game-state probes ─────────────────────────────────────────────────

bool GameIsPaused() {
    const uint32_t p = ReadGuestBE32(kPauseStatePtr);
    if (!p) return false;
    return ReadGuestU8(p + 4) != 0 && ReadGuestU8(p + 10) == 0;
}

// +4 paused, +6 and +8 are the two "locally" flags the Pause/ResumeLocally and
// FastForward requests move, +10 is the run-one-frame-while-paused bit. Printed
// whole because a replay that ends early has left the game running with dead
// input three times now, and this is the state that decides whether the player
// is allowed to drive.
// The hand-over is a small state machine, not one flag, and an early teardown
// can leave every part of it mid-flight:
//
//   dword_828CD538  how many hand-overs are counted as running
//   dword_828CD53C  0 idle, 1..3 the transition itself
//   director+12/+16 the request sub_823CC200 filed, 3 = handed back
//
// Measured after a skipped replay: counter 1 and state 3. State 3 only leaves
// through sub_823CE098, and only when director+12 is 3 -- which the teardown has
// already cleared, so it can never resolve. Put the whole thing back to the
// values the constructor uses.
void UnstickCineTransition(const char* why) {
    const uint32_t n = ReadGuestBE32(kCineTransitions);
    const uint32_t st = ReadGuestBE32(kCineEventState);
    const uint32_t dir = ReadGuestBE32(kCineDirector);
    const uint32_t dir12 = dir ? ReadGuestBE32(dir + 12) : 0;
    if (!n && !st && !dir12) return;

    WriteGuestBE32(kCineTransitions, 0);
    WriteGuestBE32(kCineEventState, 0);
    if (dir) {
        WriteGuestBE32(dir + 12, 0);
        WriteGuestBE32(dir + 16, 0);
    }
    MC_WARN("[cutscene] cine hand-over left mid-flight at '{}' (count={} "
            "state={} director+12={}) -- put back to idle, otherwise the HUD "
            "and the pause menu never come back", why, n, st, dir12);
}

void LogPauseManager(const char* why) {
    const uint32_t mgr = ReadGuestBE32(kPauseStatePtr);
    if (!mgr) return;
    MC_INFO("[cutscene] pause manager at '{}': paused={} +6={} +8={} +10={} "
            "queued={}", why, ReadGuestU8(mgr + 4), ReadGuestU8(mgr + 6),
            ReadGuestU8(mgr + 8), ReadGuestU8(mgr + 10),
            ReadGuestBE32(mgr + 68));
}

bool ClosePauseMenuLikeContinue() {
    PPCFunc* fn = GuestFn(kPauseMenuContinueFn);
    if (!fn) return false;
    rex::ppc::GuestToHostFunction<void>(fn);
    MC_INFO("[cutscene] closed the pause menu the way PM_Continue does");
    return true;
}

void RequestPauseState(uint32_t code, const char* why) {
    const uint32_t mgr = ReadGuestBE32(kPauseStatePtr);
    PPCFunc* fn = GuestFn(kPauseRequestFn);
    if (!mgr || !fn) return;
    rex::ppc::GuestToHostFunction<void>(fn, mgr, code);
    // Names straight from the game's own table at off_827E91A8, so the log says
    // what was actually sent instead of guessing between two of them.
    static const char* kNames[] = {"None", "PauseSimulation", "ResumeSimulation",
                                   "PauseLocally", "ResumeLocally", "FastForward",
                                   "FrameAdvance", "PauseOnly", "ResumeOnly"};
    MC_INFO("[cutscene] pause request {} ({})",
            code < (sizeof(kNames) / sizeof(kNames[0])) ? kNames[code] : "?", why);
}

bool SceneIsPlaying() {
    if (!ReadGuestBE32(kCineScriptPtr)) return false;
    PPCFunc* fn = GuestFn(kIsScenePlayingFn);
    if (!fn) return false;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn) != 0;
}

bool CineScriptReady() { return ReadGuestBE32(kCineScriptPtr) != 0; }

// ── Where a stalled scene is stuck ────────────────────────────────────
//
// core_rolling_prototype does not start the scene until a run of gates all
// read true, and every one of them is a field of the mcCineScript singleton.
// Read straight out of the disassembly:
//
//   +0x120 (288)  scene selected. SetSceneName (sub_823BCC58) sets it, and
//                 IsScenePlaying (sub_823BBF80) returns 1 on it alone -- which
//                 is why naming a scene is enough to hide the HUD even if
//                 nothing else ever happens.
//   +0x121 (289)  SetSceneReadyForGame (0x823BE350) sets it.
//   +0x124 (292)  }  the two "GPS transition" interlocks.
//   +0x125 (293)  }  IsGameReadyForScene  = +289 && !+293 && !+292 (0x823BE368)
//                    IsGameReadyForReplay = +289 && !+292          (0x823BE3B0)
//                    EndDescriptionAndStartClock raises them via sub_823BF0D0;
//                    the UI/GPS code at sub_8221A440 / sub_8221BE98 clears them.
//   +0x12C (300)  race-start slot; also gates the animpack release.
//   +0x130 (304)  kill request. KillScene (sub_823BBF48) is only this byte, so
//                 it does nothing unless the script gets far enough to poll it.
//   +0x170 (368)  m_RefStreamableCutsceneAnimPack. IsLoadedCutsceneAnimPack
//                 (sub_823BFBE0) wants ref->+12 non-null with
//                 (that->+8 & 0x30000000) == 0x30000000, i.e. fully streamed.
//                 A null ref means the request was never even issued.
//   +0x194 (404)  leaf scene name StartLoadCutsceneAnimPack stored.
//
// byte_828CD3B0 is the scene name itself, so the dump also proves which scene
// the manager thinks it is on.

constexpr uint32_t kSceneNameStr = 0x828CD3B0;

std::string GuestStringAt(uint32_t ea, size_t cap = 128) {
    uint8_t* base = Membase();
    if (!base || !ea) return {};
    std::string s;
    for (size_t i = 0; i < cap && base[ea + i]; ++i)
        s.push_back(char(base[ea + i]));
    return s;
}

void LogScriptState();
bool LaunchedThreadAlive(uint32_t thread);

// The thread StartCutsceneScript last handed to the script manager.
std::atomic<uint32_t> g_launched_thread{0};


// ── The other state machine: the cine event director ──────────────────
//
// The camera event does get queued (the Camera_LaunchEvent hooks prove it), so
// the question moved to who plays that queue. sub_823BBB98 is what walks it,
// picking the "CineEvents" instances out of mgr+0x108 -- and its very first line
// is `if (dword_828CD53C) return`, a transition state machine driven by
// sub_823CE098 off the director singleton at dword_8286D8E8 (+12 state, +16).
//
// So dump those three, plus how many events are actually sitting in the queue,
// and only when something changes -- a replay is ~170 frames and a per-frame
// line would bury the interesting transition.


// ── The camera hand-over, read out of the XEX ─────────────────────────
//
// sub_822C0320 is the function that decides, every frame, which camera the
// frame is rendered from. It takes the cutscene camera when, and only when,
//
//     mcCineDirector::IsCameraActive(director)   (sub_823CDC90)
//         = !director[20]            <- set once from the bypasscutscenecam cvar
//        && director[12] != 3        <- 3 means "handed back to the game"
//        && *(float*)(director[8] + 120) >= 0.0
//
// director+8 is the mcCineCamera (sub_823CE810 builds it), and its +120 is the
// camera's own clock. -1 there means "no camera running", and the ONLY code
// that clears it is the camera event's start handler:
//
//     sub_823D7280 (event start) -> sub_823CEE48 -> sub_823CECF8
//         cam+116 = the CSCamera clip, cam+80.. = the coord-sys binding,
//         cam+120 = 0.0
//
// So the whole thing hangs off one float. sub_823CEBD8 (called every frame by
// the cine tick at sub_822D3030) evaluates the clip into cam+16..+64 while that
// clock is >= 0 -- +64 is the translation row, i.e. where the camera is.
constexpr uint32_t kCamEventStartFn = 0x823D7280;

// off_827EAD60 is the game's own table of 97 cine-event names, indexed by the
// id at event+32 -- sub_823D76A8's "Unhandled CineEvent value %d name %s" print
// is what gives the indexing away. Handy ones: 0x44 fade_to_black,
// 0x45 fade_to_scene, 0x48 bookmark_cut_to_game, 0x4A cut_to_game,
// 0x4E cam_to_game, 0x53 menu_to_cam.
constexpr uint32_t kCineEventNames  = 0x827EAD60;
constexpr uint32_t kCineEventNameCount = 97;
constexpr uint32_t kCineCamTime     = 120;  // mcCineCamera clock, -1 = idle
constexpr uint32_t kCineCamClip     = 116;
constexpr uint32_t kCineCamPos      = 64;   // translation row of the built matrix
constexpr uint32_t kCineCamFov      = 104;

// The camera event Camera_LaunchEvent queued, captured by MCLA_CamEventQueued.
uint32_t g_cam_event  = 0;
bool     g_cam_forced = false;

// One entry per character the scene asks for, so the probe below can talk only
// when an answer changes instead of nine times a frame.
constexpr int kCharSlotStates = 20;
struct CharSlotState { uint32_t key; bool loaded; bool seen; };
CharSlotState g_char_state[kCharSlotStates] = {};
std::string g_char_pending_path;
std::string g_char_pending_name;

void LogCineDirector(const std::string& why, bool force) {
    static uint32_t last_state = 0xFFFFFFFFu;
    static uint32_t last_dir = 0xFFFFFFFFu;
    static uint32_t last_count = 0xFFFFFFFFu;

    const uint32_t state = ReadGuestBE32(kCineEventState);
    const uint32_t director = ReadGuestBE32(kCineDirector);
    const uint32_t dir_state = director ? ReadGuestBE32(director + 12) : 0;
    const uint32_t dir_16 = director ? ReadGuestBE32(director + 16) : 0;

    // the event queue itself: mgr+0x108 -> atArray, count is the u16 at +4
    const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
    const uint32_t queue = mgr ? ReadGuestBE32(mgr + 0x108) : 0;
    uint32_t count = 0;
    if (queue) {
        uint8_t* qbase = Membase();
        if (qbase) count = uint32_t((uint16_t(qbase[queue + 4]) << 8) | qbase[queue + 5]);
    }

    const uint32_t packed = (dir_state << 8) | dir_16;
    if (!force && state == last_state && packed == last_dir && count == last_count) return;
    last_state = state;
    last_dir = packed;
    last_count = count;

    // mgr+305 is the gate: sub_823BEEB0 only calls the queue walker (sub_823BBB98)
    // when it is set, and sub_823CE098 needs it clear to push the director on.
    uint8_t* base = Membase();
    const uint32_t playing = (base && mgr) ? base[mgr + 305] : 0;
    const uint32_t killing = (base && mgr) ? base[mgr + 304] : 0;
    const uint32_t consumed = (base && mgr) ? base[mgr + 316] : 0;

    MC_INFO("[cutscene][dir] {}: eventState(0x828CD53C)={} director+12={} director+16={} "
            "queued events={} mgr+305(gate)={} mgr+304(kill)={} mgr+316={}",
            why, state, dir_state, dir_16, count, playing, killing, consumed);
}

void LogCutsceneGates(const char* why) {
    const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
    if (!mgr) {
        MC_WARN("[cutscene] {}: mcCineScript singleton is null", why);
        return;
    }

    const uint32_t ref = ReadGuestBE32(mgr + 368);
    const uint32_t res = ref ? ReadGuestBE32(ref + 12) : 0;
    const uint32_t flags = res ? ReadGuestBE32(res + 8) : 0;

    // +0x194 is the *pending* request and the pump at 0x822D2FE0 moves it to
    // +0x19C, so an empty pending slot means it was picked up, not that nothing
    // was ever requested. Print both.
    MC_INFO("[cutscene] {}: scene='{}' pending='{}' active='{}'", why,
            GuestStringAt(kSceneNameStr),
            GuestStringAt(ReadGuestBE32(mgr + 0x194)),
            GuestStringAt(ReadGuestBE32(mgr + 0x19C)));
    // +0x123 is CineScript_SKIPPING_CUTSCENE (0x823BED38 is `mgr[0x123] = 1`
    // and nothing else) -- the script's own "I gave up" flag, and the one thing
    // that separates a scene still loading from one the script already bailed
    // out of. The raw row covers the bytes either side of it too, since this
    // block of flags is dense and mostly unnamed.
    MC_INFO("[cutscene]   selected={} readyForGame={} SKIPPED={} lock292={} "
            "lock293={} slot300={} kill304={}",
            ReadGuestU8(mgr + 0x120), ReadGuestU8(mgr + 0x121),
            ReadGuestU8(mgr + 0x123), ReadGuestU8(mgr + 0x124),
            ReadGuestU8(mgr + 0x125), ReadGuestBE32(mgr + 300),
            ReadGuestU8(mgr + 304));
    {
        char row[3 * 16 + 1];
        for (int i = 0; i < 16; ++i)
            std::snprintf(row + i * 3, 4, "%02X ", ReadGuestU8(mgr + 0x120 + i));
        MC_INFO("[cutscene]   mgr+0x120: {}", row);
    }
    MC_INFO("[cutscene]   animpack ref=0x{:08X} resource=0x{:08X} "
            "flags=0x{:08X} streamed={}",
            ref, res, flags, (flags & 0x30000000u) == 0x30000000u);
    // +0x174/+0x184/+0x194 are the pending request slots (hangout, auxiliary,
    // cutscene) and +0x17C/+0x18C/+0x19C the active ones the pump at
    // 0x822D2FE0 moves them into. A pending slot that never empties means the
    // pump is not running; both empty with a null ref means the request was
    // never queued in the first place.
    MC_INFO("[cutscene]   req pend(hang=0x{:08X} aux=0x{:08X} cut=0x{:08X}) "
            "active(hang=0x{:08X} aux=0x{:08X} cut=0x{:08X})",
            ReadGuestBE32(mgr + 0x174), ReadGuestBE32(mgr + 0x184),
            ReadGuestBE32(mgr + 0x194), ReadGuestBE32(mgr + 0x17C),
            ReadGuestBE32(mgr + 0x18C), ReadGuestBE32(mgr + 0x19C));
    // The manager tick (sub_823C6538) only calls StartLoadAnimPack
    // (sub_823C6460, which resolves the name and does pgStreamableRef::Set)
    // when the active request is set AND its delayed-release counter has gone
    // back to -1:
    //     if (v1[103] /* +0x19C */ && v1[110] /* +0x1B8 */ <= -1) ...
    // so an active request that never clears means one of these counters is
    // still holding the slot.
    MC_INFO("[cutscene]   release counters hang(+0x1A8)={} aux(+0x1B0)={} "
            "cut(+0x1B8)={}",
            int32_t(ReadGuestBE32(mgr + 0x1A8)),
            int32_t(ReadGuestBE32(mgr + 0x1B0)),
            int32_t(ReadGuestBE32(mgr + 0x1B8)));
    {
        char row[3 * 32 + 1];
        for (int i = 0; i < 32; ++i)
            std::snprintf(row + i * 3, 4, "%02X ",
                          ReadGuestU8(mgr + 0x1A0 + uint32_t(i)));
        MC_INFO("[cutscene]   mgr+0x1A0: {}", row);
    }
    if (PPCFunc* cam = GuestFn(kCameraAnimFn)) {
        const uint32_t clip = rex::ppc::GuestToHostFunction<uint32_t>(cam);
        // sub_823C4FA8 reads *(float*)(clip + 12) as the camera's duration
        // when it places the cut_to_game / fade_to_scene events, so that float
        // is the scene's length. Zero means the camera has nothing to play.
        float dur = 0.0f;
        if (clip) {
            const uint32_t raw = ReadGuestBE32(clip + 12);
            std::memcpy(&dur, &raw, sizeof(dur));
        }
        MC_INFO("[cutscene]   CSCamera clip in 'cutscene/{}': 0x{:08X} "
                "duration={:.2f}s{}",
                GuestStringAt(kSceneNameStr), clip, dur,
                clip ? "" : "  <-- MISSING");
    }
    MC_INFO("[cutscene]   nocutscenes={} nocineanimpacks={} "
            "bypasscutscenecam={} paused={}",
            ReadGuestBE32(kNoCutscenesFlag), ReadGuestBE32(kNoAnimPacksFlag),
            ReadGuestBE32(kBypassCutsceneCam), GameIsPaused());
    LogScriptState();
}

// ── Script threads ────────────────────────────────────────────────────
//
// mcScriptThread, from its ctor sub_82727E88 and sub_82727DA8 (add child):
//
//   +4  next sibling   +8  first child   +12 parent
//   +16 scr id, written by mcScript::Load (a1[4] in sub_82728010). This is the
//       handle the script's own launch returns (sub_82618698) and the one
//       IsChildFinished (sub_82727E38) resolves.
//   +24 stack size     +28 name (strdup'd by sub_821378B8)
//
// The VM side is a flat array: dword_828DA3C8 holds `word_828DA3CC` scrThread
// pointers, each with [1] = id and [3] = state (2 = finished, per
// sub_82727E38). dword_828DA3BC is whichever one is executing.
//
// Walking both is what says whether a launch actually produced a running
// script -- a generated cutscene script spawns core_rolling_prototype as a
// child and then just waits on it, so "did the child appear" is the whole
// question when a scene never starts.

constexpr uint32_t kScrThreadArray = 0x828DA3C8;
constexpr uint32_t kScrThreadCount = 0x828DA3CC;
constexpr uint32_t kScrThreadCur   = 0x828DA3BC;

void LogScriptThread(uint32_t thread, int depth) {
    for (int guard = 0; thread && guard < 64; ++guard) {
        MC_INFO("[cutscene]   {}thread=0x{:08X} id={} stack={} name='{}'",
                depth ? std::string(size_t(depth) * 2, ' ') : std::string(),
                thread, ReadGuestBE32(thread + 16), ReadGuestBE32(thread + 24),
                GuestStringAt(ReadGuestBE32(thread + 28)));
        const uint32_t child = ReadGuestBE32(thread + 8);
        if (child && depth < 6) LogScriptThread(child, depth + 1);
        thread = ReadGuestBE32(thread + 4);
    }
}

// Is `thread` still anywhere in the manager's tree? A generated cutscene script
// spawns core_rolling_prototype and waits on it, so it should outlive the whole
// scene; if it is gone while the scene is still selected, it ran to completion
// instead -- which is what bailing out looks like from the outside.
bool ThreadAlive(uint32_t thread, uint32_t node, int depth) {
    for (int guard = 0; node && guard < 256; ++guard) {
        if (node == thread) return true;
        const uint32_t child = ReadGuestBE32(node + 8);
        if (child && depth < 8 && ThreadAlive(thread, child, depth + 1))
            return true;
        node = ReadGuestBE32(node + 4);
    }
    return false;
}

bool LaunchedThreadAlive(uint32_t thread) {
    const uint32_t mgr = ReadGuestBE32(kScriptMgrPtr);
    const uint32_t root = mgr ? ReadGuestBE32(mgr) : 0;
    return thread && root && ThreadAlive(thread, root, 0);
}

void LogScriptState() {
    const uint32_t mgr = ReadGuestBE32(kScriptMgrPtr);
    const uint32_t root = mgr ? ReadGuestBE32(mgr) : 0;
    const uint32_t launched = g_launched_thread.load(std::memory_order_relaxed);
    MC_INFO("[cutscene]   script mgr=0x{:08X} root=0x{:08X} launched=0x{:08X} "
            "alive={}",
            mgr, root, launched,
            launched ? LaunchedThreadAlive(launched) : false);
    // The full tree is ~200 threads, which is unreadable at the two-second
    // cadence; it is only worth printing when someone is chasing a launch.
    if (root && REXCVAR_GET(cutscene_debug)) LogScriptThread(root, 1);

    uint8_t* base = Membase();
    const uint32_t arr = ReadGuestBE32(kScrThreadArray);
    const uint16_t n = base ? uint16_t((uint16_t(base[kScrThreadCount]) << 8) |
                                       base[kScrThreadCount + 1])
                            : 0;
    MC_INFO("[cutscene]   scrThreads={} current=0x{:08X}", n,
            ReadGuestBE32(kScrThreadCur));
    if (!REXCVAR_GET(cutscene_debug)) return;
    for (uint16_t i = 0; i < n && i < 32; ++i) {
        const uint32_t t = ReadGuestBE32(arr + uint32_t(4 * i));
        if (!t) continue;
        const uint32_t id = ReadGuestBE32(t + 4);
        if (!id) continue;  // free slot
        MC_INFO("[cutscene]     scr[{}]=0x{:08X} id={} state={}", i, t, id,
                ReadGuestBE32(t + 12));
    }
}

// ── Resolving native commands ─────────────────────────────────────────
//
// The script VM calls natives by hash, and a compiled .sco carries nothing but
// those hashes -- so reading a script offline leaves every call site unnamed.
// The registry that scrThread::Register (sub_82554798) fills is an ordinary
// open-addressed table, and walking it turns a hash back into the handler
// address, which IDA can then name:
//
//   dword_828DA3E0[0] entries   [1] capacity   [2] initial size   [3] count
//   entry = { u32 hash, u32 handler }, 8 bytes, empty when hash <= 1
//       (sub_82554500 allocates and zeroes it, sub_825578C8 inserts)
//
// The hash itself is sub_82557F78: the RAGE filename hash of the lowercased
// name, bumped to 2 if it would come out below that.

constexpr uint32_t kNativeTable = 0x828DA3E0;

uint32_t FindNativeHandler(uint32_t hash) {
    const uint32_t entries = ReadGuestBE32(kNativeTable);
    const uint32_t cap = ReadGuestBE32(kNativeTable + 4);
    if (!entries || !cap) return 0;
    for (uint32_t i = 0; i < cap; ++i) {
        if (ReadGuestBE32(entries + i * 8) == hash)
            return ReadGuestBE32(entries + i * 8 + 4);
    }
    return 0;
}

// Every hash cut_intro_generated and core_rolling_prototype call that the name
// list could not resolve. The one that matters is 0x4A2100E4: four arguments,
// one return, called immediately before IsChildFinished, which is where the
// generated script hands over to core_rolling_prototype.
constexpr uint32_t kUnknownNatives[] = {
    0x4A2100E4, 0xE7A66957, 0x0D68D93C, 0x8C60E81A, 0x266DBE5B, 0xECF8EB5F,
    0x63651F03, 0xCD033E2F, 0x4402D949, 0x37F300B9, 0xC0394E05, 0xD4E822E1,
    0x868997DA, 0x45C8C188, 0x01185F9B, 0x6CE6702A, 0x422DF6CB, 0x65552629,
    0x02E89308, 0xCA47ABA6, 0xB2D4FCFE, 0x84B0649B, 0xAD7AD071, 0xA18BBE50,
    0x35785333, 0x74574858, 0x0F902C89, 0xBA6C81F4, 0x66A532AB, 0x87AA640D,
    0x67116627, 0xD48B90B6, 0x002E2800, 0x90837EAD,
};

void LogNativeHandlers() {
    const uint32_t cap = ReadGuestBE32(kNativeTable + 4);
    MC_INFO("[cutscene] native table entries=0x{:08X} cap={} count={}",
            ReadGuestBE32(kNativeTable), cap,
            ReadGuestBE32(kNativeTable + 12));
    for (uint32_t h : kUnknownNatives)
        MC_INFO("[cutscene]   native {:08X} -> 0x{:08X}", h,
                FindNativeHandler(h));
}

// ── The scrThread behind a launched script ────────────────────────────
//
// START_NEW_SCRIPT_WITH_ARGS (sub_82556AC8, hash 0x4A2100E4 -- the call a
// generated cutscene script makes to hand over to core_rolling_prototype) ends
// up in sub_82727930:
//
//   v8 = *(scrThread::GetActive() + 48);          // the running script's mcScript
//   if (*(v8 + 28) && mcScriptManager::StartNewThread(..., v8, ...))
//       return newThread + 16;
//   return 0;
//
// scrThread+48 is the link back to the mcScriptThread, written by
// mcScript::Load (sub_82728010) through sub_82554D48(id). If it is zero for our
// thread, v8 is zero, `*(v8 + 28)` reads guest address 28, and the launch
// returns 0 without trying -- and a returned 0 makes the parent's
// IsChildFinished true immediately, which is the bail-out we are seeing.
//
// The scrThread array is dword_828DA3C8 x word_828DA3CC, entry [1] = id and
// [3] = state; +48 is the mcScript link and +64 the stack size the slot was
// allocated with.

uint32_t FindScrThread(uint32_t id) {
    uint8_t* base = Membase();
    const uint32_t arr = ReadGuestBE32(kScrThreadArray);
    if (!base || !arr || !id) return 0;
    const uint16_t n = uint16_t((uint16_t(base[kScrThreadCount]) << 8) |
                                base[kScrThreadCount + 1]);
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t t = ReadGuestBE32(arr + uint32_t(4 * i));
        if (t && ReadGuestBE32(t + 4) == id) return t;
    }
    return 0;
}

void LogLaunchedScrThread(const char* why) {
    const uint32_t thread = g_launched_thread.load(std::memory_order_relaxed);
    if (!thread) return;
    const uint32_t id = ReadGuestBE32(thread + 16);
    const uint32_t scr = FindScrThread(id);
    if (!scr) {
        MC_INFO("[cutscene]   {}: mcThread=0x{:08X} id={} -- no scrThread slot",
                why, thread, id);
        return;
    }
    MC_INFO("[cutscene]   {}: mcThread=0x{:08X} id={} scr=0x{:08X} state={} "
            "mcScriptLink(+48)=0x{:08X} stack(+64)={}",
            why, thread, id, scr, ReadGuestBE32(scr + 12),
            ReadGuestBE32(scr + 48), ReadGuestBE32(scr + 64));
}

// KillScene, sub_823BBF48. Only sets +304, which the script has to notice.
void RequestKillScene() {
    const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
    uint8_t* base = Membase();
    if (mgr && base) base[mgr + 304] = 1;
}

// FinishScene, sub_823C37F8: the full teardown the script would run at the
// end. Blunt -- it tears the scene down under a thread that is still in it --
// but it is the only thing that gets the HUD back when the script is wedged
// before its own kill check.
// See cutscene_keep_movies: mgr+316 decides whether FinishScene tears the movie
// slots down, and that teardown zeroes a vtable the pause menu still uses.
void ClearSceneMovieTeardownFlag() {
    if (!REXCVAR_GET(cutscene_keep_movies)) return;
    const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
    uint8_t* base = Membase();
    if (!mgr || !base || !base[mgr + 316]) return;
    base[mgr + 316] = 0;
    MC_INFO("[cutscene] cleared mgr+316 so FinishScene keeps the movie slots "
            "(that teardown zeroes the pause menu's vtable)");
}

// The scene's own set dressing -- the interior a cutscene plays inside.
//
// A garage cutscene is not staged in the world: the props it stands in come
// from resources/cutsceneitems/<name>.xdr, and the script that plays the scene
// does NOT load them. The parent gameplay script does, right before it launches
// the generated script (measured in the garage flow script, which calls
// CineScript_Prop_LoadSetDressing("cut_la_hw_oh_garage_set_items") and only
// then starts sc_cut_garage_01_ent_generated). Replaying such a scene on its
// own therefore plays it in an empty world -- the "limbo" the garage intro
// shows.
constexpr uint32_t kPropMgrPtr       = 0x828794D4;
constexpr uint32_t kLoadSetDressFn   = 0x823D3DF8;  // (propMgr, name, bool)
constexpr uint32_t kUnloadSetDressFn = 0x823D3818;  // (propMgr, 0)
constexpr uint32_t kPropLoadState    = 32;          // propMgr + this; 1|2 = busy

// Scene -> set dressing, read straight off the CineScript_Prop_LoadSetDressing
// call sites: every parent script that loads one, paired with the generated
// script it goes on to launch. Only these scenes have one at all.
struct SetDressing {
    const char* scene;
    const char* items;
};
constexpr SetDressing kSetDressing[] = {
    {"cut_la_hw_oh_garage_01x_ent",      "cut_la_hw_oh_garage_set_items"},
    {"cut_story_hollywood_garage_intro", "cut_la_hw_oh_garage_set_items"},
    {"sc_cut_garage_01_ent",             "cut_la_hw_oh_garage_set_items"},
    {"cut_la_be_smb_garage_01x_ent",     "cut_la_be_smb_garage_set_items"},
    {"cut_story_payback_intro",          "cut_la_be_smb_garage_set_items"},
    {"cut_story_find_sh_02c",            "cut_hk2_24_set_items"},
};

const char* SetDressingFor(const char* scene) {
    for (const auto& d : kSetDressing)
        if (std::strcmp(d.scene, scene) == 0) return d.items;
    return nullptr;
}

// Fading back in after a scene that faded out.
//
// "fade_to_black" is not something the cutscene script does -- it is a scene
// EVENT. sub_823D76A8, the event dispatcher, answers it by starting the misc
// task script "game/misctasks/screen_fade_to_black", and it answers
// "fade_to_scene" with "game/misctasks/screen_fade_to_scene", which is the one
// that brings the picture back. Both are started the same way: the duration
// float goes into a static 24-byte argument block and the script is launched
// with that block as its argument.
//
// A replay that is skipped, or cut short by the watchdog, stops between those
// two events -- the screen has faded out and nothing is left to fade it back.
// That is the black screen after skipping the garage intro. Teardown fires the
// second half itself.
constexpr uint32_t kFadeArgBlock  = 0x828CD5E4;
constexpr uint32_t kFadeDurAt     = 0x14;  // kFadeArgBlock + this
constexpr uint32_t kFadeArgSize   = 0x18;
constexpr uint32_t kFadeStack     = 0x5DC;
constexpr float    kFadeSeconds   = 0.5f;
constexpr const char* kFadeToScene = "game/misctasks/screen_fade_to_scene";

void WriteGuestFloat(uint32_t ea, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    WriteGuestBE32(ea, bits);
}

void FadeBackToScene() {
    PPCFunc* fn = GuestFn(kStartThreadFn);
    if (!fn) return;
    const uint32_t name = AllocGuestString(kFadeToScene);
    if (!name) return;
    WriteGuestFloat(kFadeArgBlock + kFadeDurAt, kFadeSeconds);
    // Same call shape the dispatcher uses: no parent, flag 1, the argument
    // block and its size, and the misc task's own stack size.
    rex::ppc::GuestToHostFunction<uint32_t>(fn, name, 0u, 1u, kFadeArgBlock,
                                           kFadeArgSize, kFadeStack);
    MC_INFO("[cutscene] fading back to scene over {:.2f}s", kFadeSeconds);
}

// Split-screen never happens here, but asking for a couple of player slots
// costs nothing and does not assume the player is index 0.
constexpr uint32_t kPlayerSweep = 4;

// Racer UIDs, the way the game hands them out.
//
// A racer's UID lives at racer+140 -- that is the field Racer_FindFromUID
// (sub_822B6238) matches on and the one Racer_SetUID (sub_822ACF88) writes.
// The gameplay scripts do this before every cutscene: read the racer's UID and,
// if it is still 0, take 5000 + slot and walk upward until FindFromUID says
// nothing is using it, then set it on the racer and put it in the car slot.
constexpr uint32_t kRacerUidAt   = 140;
constexpr uint32_t kUidFirstFree = 5000;

// Player_FindRacerObject(index), which is how every gameplay script gets the
// racer it is about to slot: sub_821E80F8 says whether that player slot is
// live, sub_822A3998 hands back the racer.
constexpr uint32_t kPlayerMgrPtr   = 0x82874374;
constexpr uint32_t kPlayerValidFn  = 0x821E80F8;  // (playerMgr, index) -> bool
constexpr uint32_t kPlayerRacerFn  = 0x822A3998;  // (playerMgr, index) -> racer
// CineScript_SetGameCoordSysBindingCopyRacer's real body: (racer) -> copies the
// racer's matrix into the scene's coordinate system and binds the scene to it.
constexpr uint32_t kBindCoordSysToRacerFn = 0x823BFAA8;

// The player's racer, and enough of its state to tell apart the two ways a
// replay can leave the car unable to drive.
//
// A cutscene touches the player's racer in two very different ways, and after a
// replay that ends abnormally either one could be what is holding the car:
//   * the stream lock. The cutscene script does Racer_GetStreamLockState,
//     Racer_SetStreamingLockedIn (which is only "racer+660 = 1"), and restores
//     it after the scene. A replay torn down early never reaches the restore.
//   * position. The gameplay script that normally plays a scene WARPS the
//     player to it (Racer_InitializeResetPosition) and the scene then drives
//     cars as animated movers. We never warp, so if the scene moved the racer
//     it is left wherever the animation put it -- which can be inside geometry
//     or in the air.
// Logging the racer's position and lock either side of a replay separates them
// in one run: a car that is stuck without having moved is not a position
// problem, and a car that ended up somewhere else is not a lock problem.
constexpr uint32_t kRacerStreamLockAt = 660;
// Where a racer's position actually lives, read off Racer_GetPosition's handler
// (sub_822AEF88): racer+48 is the entity, entity+28 points at its matrix, and
// the translation is 48 bytes past the matrix's +16 -- with a second spelling,
// entity+272+48, chosen by the same global "which half" flag that shows up all
// over this engine. Both are printed because picking the wrong one silently
// reads zeros, which is exactly what the first version of this probe did.
constexpr uint32_t kRacerEntityAt   = 48;
constexpr uint32_t kEntityMatrixAt  = 28;
constexpr uint32_t kMatrixTransAt   = 16 + 48;
constexpr uint32_t kEntityTransAlt  = 272 + 48;
constexpr uint32_t kRacerTransNoEnt = 288 + 48;

uint32_t PlayerRacer() {
    const uint32_t player_mgr = ReadGuestBE32(kPlayerMgrPtr);
    PPCFunc* valid_fn = GuestFn(kPlayerValidFn);
    PPCFunc* racer_fn = GuestFn(kPlayerRacerFn);
    if (!player_mgr || !valid_fn || !racer_fn) return 0;
    for (uint32_t idx = 0; idx < kPlayerSweep; ++idx) {
        if (!(rex::ppc::GuestToHostFunction<uint32_t>(valid_fn, player_mgr, idx)
              & 0xFF))
            continue;
        if (const uint32_t r = rex::ppc::GuestToHostFunction<uint32_t>(
                racer_fn, player_mgr, idx))
            return r;
    }
    return 0;
}

void LogPlayerRacer(const char* when) {
    const uint32_t racer = PlayerRacer();
    if (!racer) {
        MC_INFO("[cutscene][racer] {}: no player racer", when);
        return;
    }
    const uint32_t ent = ReadGuestBE32(racer + kRacerEntityAt);
    uint32_t a = 0, b = 0;
    if (ent) {
        const uint32_t mtx = ReadGuestBE32(ent + kEntityMatrixAt);
        if (mtx) a = mtx + kMatrixTransAt;
        b = ent + kEntityTransAlt;
    } else {
        a = racer + kRacerTransNoEnt;
    }
    auto vec = [](uint32_t at) {
        if (!at) return std::string("-");
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(%.1f, %.1f, %.1f)",
                      ReadGuestFloat(at), ReadGuestFloat(at + 4),
                      ReadGuestFloat(at + 8));
        return std::string(buf);
    };
    MC_INFO("[cutscene][racer] {}: racer=0x{:08X} uid={} streamLock={} "
            "entity=0x{:08X} pos={} alt={}",
            when, racer, ReadGuestBE32(racer + kRacerUidAt),
            ReadGuestBE32(racer + kRacerStreamLockAt), ent, vec(a), vec(b));
}

// Whether we loaded a set dressing for the scene now playing -- so teardown
// only unloads what it put there.
std::atomic<bool> g_set_dressing_up{false};

bool LoadSetDressing(const char* items) {
    const uint32_t prop_mgr = ReadGuestBE32(kPropMgrPtr);
    PPCFunc* fn = GuestFn(kLoadSetDressFn);
    if (!prop_mgr || !fn) {
        MC_WARN("[cutscene] set dressing '{}' skipped: prop manager 0x{:08X} / "
                "fn missing", items, prop_mgr);
        return false;
    }
    const uint32_t name = AllocGuestString(items);
    if (!name) return false;
    // Second argument is the flag the garage flow script passes as 0.
    const uint32_t ok = rex::ppc::GuestToHostFunction<uint32_t>(
        fn, prop_mgr, name, 0u);
    g_set_dressing_up.store(ok != 0, std::memory_order_relaxed);
    MC_INFO("[cutscene] set dressing '{}' requested: {}", items,
            ok ? "accepted" : "refused");
    return ok != 0;
}

bool SetDressingLoading() {
    const uint32_t prop_mgr = ReadGuestBE32(kPropMgrPtr);
    if (!prop_mgr) return false;
    const uint32_t state = ReadGuestBE32(prop_mgr + kPropLoadState);
    return state == 1 || state == 2;
}

void UnloadSetDressing() {
    if (!g_set_dressing_up.exchange(false, std::memory_order_relaxed)) return;
    const uint32_t prop_mgr = ReadGuestBE32(kPropMgrPtr);
    PPCFunc* fn = GuestFn(kUnloadSetDressFn);
    if (!prop_mgr || !fn) return;
    rex::ppc::GuestToHostFunction<uint32_t>(fn, prop_mgr, 0u);
    MC_INFO("[cutscene] set dressing unloaded");
}

void ForceFinishScene() {
    PPCFunc* fn = GuestFn(kFinishSceneFn);
    if (!fn) {
        MC_WARN("[cutscene] FinishScene 0x{:08X} not in dispatcher",
                kFinishSceneFn);
        return;
    }
    rex::ppc::GuestToHostFunction<uint32_t>(fn);
    MC_INFO("[cutscene] FinishScene forced");
    // A skip lands between fade-out and fade-in; unconditional here because a
    // fade to the scene when the scene is already visible is what the game
    // itself runs at the end of every fade pair.
    FadeBackToScene();
    // The props came in for this scene only; leaving a garage interior standing
    // in the middle of free roam is worse than the empty scene was.
    UnloadSetDressing();
}

// ── Launch ────────────────────────────────────────────────────────────

// "Game/CineScripts/generated[/<dir>]/<id>_generated", the path mcScript::Load
// resolves under $\script_obj.
std::string ScriptPath(const Cutscene& cs) {
    std::string p = "Game/CineScripts/generated";
    if (cs.dir[0]) {
        p += '/';
        p += cs.dir;
    }
    p += '/';
    p += cs.id;
    p += "_generated";
    return p;
}

// Rockstar's own standalone-cutscene harness, found at
// $/script_obj/tools/CutsceneTests/. SETUP_GENERIC_CUTSCENE.sco is 510 bytes
// and does exactly the prerequisite a generated cutscene script assumes:
//
//   CineScript_SetGameCoordSysBinding(...)
//   loop { id = <0x822ACF40>(i); CineScript_Cars_AssignIdToSlot(slot, id); }
//       -- its own error string is
//          "PROC 'SlotFourAvailableRacers' Failed to slot all four racers."
//   CineScript_Characters_AssignNameToSlot(0, "drv_fh_001_set")
//   CineScript_Characters_AssignNameToSlot(1..7, "drv_mc_07_set")
//   CineScript_Characters_AssignNameToSlot(8, "drv_mp_01_set")
//
// Without it the generated script finds no racers slotted, jumps to its
// "insufficient number of racers slotted for cutscene" block and calls
// CineScript_SKIPPING_CUTSCENE -- which is precisely the one-frame bail-out
// this was doing. It is a plain script with no children of its own, so it
// finishes in a frame or two.
constexpr const char* kSetupScript = "tools/CutsceneTests/SETUP_GENERIC_CUTSCENE";

// ── Does a script object exist? ───────────────────────────────────────
//
// The same read-only probe DoesCutsceneAnimPackExist uses (sub_823BD0A0):
// push a directory onto the shared fiDevice search path, ask whether
// <name>.<ext> resolves, pop. No side effects, unlike trying to mount it.

constexpr uint32_t kSearchPath  = 0x827D7770;  // unk_827D7770
constexpr uint32_t kPushDirFn   = 0x821CA540;
constexpr uint32_t kFileExistFn = 0x821CA778;  // (searchPath, name, ext)
constexpr uint32_t kPopDirFn    = 0x821C9A90;

bool ScriptObjectExists(const char* name) {
    PPCFunc* push = GuestFn(kPushDirFn);
    PPCFunc* exists = GuestFn(kFileExistFn);
    PPCFunc* pop = GuestFn(kPopDirFn);
    if (!push || !exists || !pop) return false;
    const uint32_t dir = AllocGuestString("$/script_obj");
    const uint32_t nm = AllocGuestString(name);
    const uint32_t ext = AllocGuestString("sco");
    if (!dir || !nm || !ext) return false;
    rex::ppc::GuestToHostFunction<uint32_t>(push, kSearchPath, dir);
    const uint32_t hit =
        rex::ppc::GuestToHostFunction<uint32_t>(exists, kSearchPath, nm, ext);
    rex::ppc::GuestToHostFunction<uint32_t>(pop, kSearchPath);
    return hit != 0;
}

// ── Reproducing SETUP_GENERIC_CUTSCENE natively ───────────────────────
//
// The archive this build reads has no tools/ directory, so the dev harness is
// not available and its three steps have to be done directly. Each native is a
// thin wrapper over a plain function, and calling the inner one avoids having
// to build the VM's argument frame at all:
//
//   CineScript_Characters_AssignNameToSlot -> loc_823C08F0:
//       r3 = argv[0] (name), r4 = argv[1] (slot), b sub_823BC4E0
//       sub_823BC4E0(const char* name, int slot) writes
//       mgr+1232+slot*64 = "character/<name>" and mgr+2512+slot*64 = "<name>"
//
//   CineScript_Cars_AssignIdToSlot -> loc_823BEB70, which has no inner
//       function at all -- it is one store:
//           *(mgr + (argv[1] + 953) * 4) = argv[0]
//       i.e. mgr + 3812 + slot*4 = id, a 16-entry table. FinishScene
//       (sub_823C37F8) resets exactly that table to -1, which confirms it.
//
//   the unnamed 0x0D68D93C -> sub_822ACF40:
//       **a1 = sub_822B6238(dword_828745E4 /* mcRacerManager */, *a1[2])
//       so sub_822B6238(racerMgr, index) is "id of racer <index>".
//
// Argument order is the order the script pushes them: argv[0] is the first.
// Getting that backwards is what made the first attempt write through a string
// pointer used as a slot index.

// The step the harness does first and the one skipped until now: without a
// coordinate system the scene has no frame to play in, so it runs and finishes
// with nothing on screen.
//
//   CineScript_Special_GetPlayerCoordSysBinding -> sub_823BED50:
//       r3 = argv[0]; bl sub_823BDD90; return (bool)
//       so sub_823BDD90(dst) fills dst with the player's binding.
//   CineScript_SetGameCoordSysBinding -> loc_823BE558, no inner function:
//       copies 6 dwords from argv[0] to mgr + 0x3C.
constexpr uint32_t kPlayerCoordSysFn = 0x823BDD90;  // (dst) -> bool
constexpr uint32_t kGameCoordSysAt   = 0x3C;        // mgr + this, 6 dwords
constexpr uint32_t kCoordSysDwords   = 6;

// There are TWO coord-sys bindings on mcCineScript and they are not the same
// one. CineScript_SetGameCoordSysBinding (loc_823BE558) copies six dwords to
// mgr+0x3C -- that is the *game* binding, and it is the one the harness was
// filling. The camera uses the other: Camera_LaunchEvent hands
// `mgr + 0x24` to the event (sub_823D6CF8 copies it to event+40), and the
// native that writes that block is CineScript_SetSceneCoordSysBinding
// (loc_823BEE6C, the same six-dword loop with 0x24 instead of 0x3C).
//
// Nothing in a bare replay ever writes it, and the only code in the game that
// clears it is sub_823C3620, which runs on system teardown, not per scene. So
// it sits at the allocator's 0xCD fill from boot, and sub_823A3F00 -- which
// reads binding[0] as an entity pointer, binding[1] as a matrix and
// binding[2..4] as a translation -- turns that fill into a garbage transform.
// Measured on hardware: binding[0]=0xCDCDCDCD, offsets -431602080.0, and the
// cutscene camera evaluated to (1.1e10, 1.1e10, 1.1e10).
//
// Zeroed, sub_823A3F00 takes its third branch and returns identity with a zero
// translation, which is what a CSCamera clip wants: those clips are authored in
// world space (cut_la_hw_oh_garage_01x_ext starts at 803.5, 22.6, -799.2,
// cut_story_district_champs at 1658.8, 1.4, 230.9).
constexpr uint32_t kSceneCoordSysAt = 0x24;  // mgr + this, 6 dwords

constexpr uint32_t kAssignNameToSlotFn = 0x823BC4E0;  // (const char*, int slot)
constexpr uint32_t kRacerIdByIndexFn   = 0x822B6238;  // (racerMgr, index) -> id
constexpr uint32_t kRacerMgrPtr        = 0x828745E4;
constexpr uint32_t kCarSlotTable       = 3812;  // mgr + this + slot*4, 16 slots
constexpr int kCarSlots = 4;                    // what the harness fills
constexpr int kCarSlotMax = 16;
// Racer UIDs are handed out small in free roam, but the sweep is cheap -- a
// handful of guest calls once per replay -- so it does not have to be tight.
constexpr uint32_t kUidSweep = 64;
constexpr int kCharSlotMax = 20;

constexpr const char* kSetupCharacters[] = {
    "drv_fh_001_set", "drv_mc_07_set", "drv_mc_07_set", "drv_mc_07_set",
    "drv_mc_07_set", "drv_mc_07_set", "drv_mc_07_set", "drv_mc_07_set",
    "drv_mp_01_set",
};
constexpr int kSetupCharCount =
    int(sizeof(kSetupCharacters) / sizeof(kSetupCharacters[0]));

// core_rolling_prototype decides the scene binding like this:
//
//     if (!static[0x504] && static[16] >= 1 && static[20][0]+20 >= 0)
//           SetSceneCoordSysBinding(local[56])   <- filled by Cars_GetCoordSysBinding
//     else  GetGameCoordSysBinding(local[133]); SetSceneCoordSysBinding(local[133])
//
// The loop that fills local[56] runs static[0x1F8] times -- zero for a scene
// that needs no cars, like the garage one. But the second test passes anyway,
// because the harness pads the car slots with the player racer to get past the
// "insufficient number of racers slotted" guard. So the script hands
// SetSceneCoordSysBinding a local it never wrote: script stack, which the
// game's allocator fills with 0xCD.
//
// That is where the 0xCDCDCDCD binding came from, and why clearing mgr+0x24
// once during setup did nothing -- the script dirties it afterwards. Scrub it
// whenever it still looks like fill; a real binding never does. Measured on a
// genuine garage entry, the game's own value is all zeros.
void ScrubSceneCoordSys(const char* why) {
    const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
    if (!mgr) return;
    bool fill = false;
    for (uint32_t i = 0; i < kCoordSysDwords; ++i)
        if (ReadGuestBE32(mgr + kSceneCoordSysAt + i * 4) == 0xCDCDCDCDu) fill = true;
    if (!fill) return;
    for (uint32_t i = 0; i < kCoordSysDwords; ++i)
        WriteGuestBE32(mgr + kSceneCoordSysAt + i * 4, 0);
    MC_WARN("[cutscene] scene coord sys (mgr+0x24) was uninitialised heap fill "
            "at '{}' -- zeroed, so the camera plays at its authored world "
            "position", why);
}

// The animpack release counters (+0x1A8 hangout, +0x1B0 auxiliary, +0x1B8
// cutscene) deadlock at zero, and once one does no scene can ever load its pack
// again. sub_823C6538 is the whole story:
//
//     if (active_request && counter <= -1) StartLoadAnimPack();
//     ...
//     if (counter > 0 && --counter == 0)
//         if (sub_823BDFD8(ref))  { release(); counter = -1; }
//
// and sub_823BDFD8's first line is `if (!ref[3]) return 0` -- it refuses to
// finish the release while the resource is null. So a scene that dies before
// its pack ever streamed (the "insufficient racers" bail, for one) leaves the
// counter at zero with a null resource: the release can never complete because
// there is nothing to release, and the load can never start because the counter
// is not -1. Measured on cut_story_district_champs_night: cut(+0x1B8)=0,
// resource=0x00000000, scene stuck until the game is restarted.
//
// -1 is the game's own idle value, so putting it back is exactly what the
// release would have done.
constexpr uint32_t kReleaseCounters[] = {0x1A8, 0x1B0, 0x1B8};

void UnstickAnimPackRelease(const char* why) {
    const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
    if (!mgr) return;
    for (uint32_t off : kReleaseCounters) {
        if (int32_t(ReadGuestBE32(mgr + off)) != 0) continue;
        WriteGuestBE32(mgr + off, 0xFFFFFFFFu);
        MC_WARN("[cutscene] animpack release counter +0x{:X} was stuck at 0 at "
                "'{}' -- put back to -1, otherwise no scene can load its pack "
                "again", off, why);
    }
}

// Every probe below is a trace, not a diagnosis, so it answers to a cvar. The
// warnings that mean something is broken are deliberately left outside it.
bool ProbeLog() {
    return REXCVAR_GET(cutscene_probe_log) && CutsceneReplayBusy();
}

void RunNativeSetup() {
    const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
    const uint32_t racer_mgr = ReadGuestBE32(kRacerMgrPtr);
    if (!mgr) {
        MC_WARN("[cutscene] native setup: no mcCineScript");
        return;
    }

    // Anchor the scene to the player's car, the way the reward and cop scenes
    // do. sub_823BFAA8 is what CineScript_SetGameCoordSysBindingCopyRacer
    // reaches: it copies the racer's matrix into mcCineScript+96 and points the
    // binding at mcCineScript+64 at it. Doing it by hand into a different field
    // is what the old copy here did, and it never took.
    bool bound = false;
    if (!REXCVAR_GET(cutscene_bind_coordsys)) {
        MC_INFO("[cutscene] native setup: coord sys bind disabled");
    } else if (PPCFunc* bind_fn = GuestFn(kBindCoordSysToRacerFn)) {
        if (const uint32_t racer = PlayerRacer()) {
            rex::ppc::GuestToHostFunction<void>(bind_fn, racer);
            bound = true;
        }
    }
    if (REXCVAR_GET(cutscene_bind_coordsys))
        MC_INFO("[cutscene] native setup: scene anchored to the player's car: "
                "{}", bound);

    ScrubSceneCoordSys("native setup");
    UnstickAnimPackRelease("native setup");

    // sub_822B6238 searches the racer array for the entry whose +140 equals the
    // key, so sweep a range of keys rather than assuming they are 0..3: free
    // roam numbers the player and whatever AI happens to exist however it
    // likes.
    // What goes in a car slot is the racer's UID, not the racer.
    //
    // The generated scripts read a slot with CineScript_Cars_ReadSlotId and
    // hand the result straight to Racer_FindFromUID, which is
    // sub_822B6238(racerMgr, uid): it walks the racer array looking for the
    // entry whose +140 equals what it was given, and returns the racer. The
    // game's own CineScript_Cars_AssignIdToSlot (sub_823BEB70) writes the id
    // into mgr + 4*(slot+953), the same table -- an id in, an id out.
    //
    // This used to store sub_822B6238's RETURN value, which is the racer
    // pointer. Racer_FindFromUID then searched for a racer whose +140 equalled
    // a pointer, found nothing, and the script jumped to its "insufficient
    // number of racers slotted for cutscene" block and called
    // CineScript_SKIPPING_CUTSCENE -- which is exactly the five-frame bail with
    // SKIPPED=1 in the log. Measured over all 316 generated cutscene scripts:
    // 96 of them gate on Racer_FindFromUID and so could never start, while the
    // ones that pass instead through Player_GetRacer (cut_story_district_champs,
    // cut_la_hw_oh_garage_01x_ext) never touched this table and always worked.
    // That split is what made the table look correct for so long.
    //
    // So sweep candidate UIDs, keep the UID rather than the racer, and let
    // sub_822B6238 say which ones exist.
    uint32_t racers[kCarSlots] = {};
    int found = 0;
    PPCFunc* racer_fn = GuestFn(kRacerIdByIndexFn);
    if (racer_mgr && racer_fn) {
        // Ask for the players' racers by name rather than sweeping. A sweep
        // over sub_822B6238 is a sweep over UIDs, not over the array, so it can
        // only ever turn up racers that already have one -- and in free roam
        // the player's is still 0, which is why sweeping from 0 appeared to
        // find a racer and slotted a value that meant nothing.
        const uint32_t player_mgr = ReadGuestBE32(kPlayerMgrPtr);
        PPCFunc* valid_fn = GuestFn(kPlayerValidFn);
        PPCFunc* prcr_fn  = GuestFn(kPlayerRacerFn);
        for (uint32_t idx = 0; player_mgr && valid_fn && prcr_fn &&
                               idx < kPlayerSweep && found < kCarSlots; ++idx) {
            if (!(rex::ppc::GuestToHostFunction<uint32_t>(valid_fn, player_mgr,
                                                          idx) & 0xFF))
                continue;
            const uint32_t racer = rex::ppc::GuestToHostFunction<uint32_t>(
                prcr_fn, player_mgr, idx);
            if (!racer) continue;
            uint32_t uid = ReadGuestBE32(racer + kRacerUidAt);
            if (!uid) {
                uid = kUidFirstFree + uint32_t(found);
                while (rex::ppc::GuestToHostFunction<uint32_t>(racer_fn,
                                                               racer_mgr, uid))
                    ++uid;
                WriteGuestBE32(racer + kRacerUidAt, uid);
            }
            bool dup = false;
            for (int i = 0; i < found; ++i) dup = dup || racers[i] == uid;
            if (!dup) racers[found++] = uid;
        }
    } else {
        MC_WARN("[cutscene] native setup: racer manager 0x{:08X} / fn missing",
                racer_mgr);
    }

    // Free roam usually yields exactly one racer -- the player -- and every
    // generated scene wants four ("Failed to slot all four racers" is the
    // harness's own wording). Repeating the one we have is wrong on screen but
    // it answers whether this table is the only thing standing in the way.
    int slotted = 0;
    const bool pad = REXCVAR_GET(cutscene_pad_racers) && found > 0;
    static_assert(kCarSlots <= kCarSlotMax, "car slot table is 16 wide");
    for (int slot = 0; slot < kCarSlots; ++slot) {
        const bool have = slot < found || pad;
        // -1 is the table's own empty marker: Cars_ReadSlotId tests for it by
        // name ("returning id -1 on slot ...") before handing the value on.
        // Writing a plain 0 into an unfilled slot would read back as a racer id
        // of zero and send the script looking for a racer that is not there.
        const uint32_t uid =
            have ? (slot < found ? racers[slot] : racers[0]) : 0xFFFFFFFFu;
        WriteGuestBE32(mgr + kCarSlotTable + uint32_t(slot) * 4, uid);
        if (have) ++slotted;
    }
    std::string uid_list;
    for (int i = 0; i < found; ++i) {
        if (!uid_list.empty()) uid_list += ' ';
        uid_list += std::to_string(racers[i]);
    }
    MC_INFO("[cutscene] native setup: {} racer uid(s) [{}], {} slot(s) filled "
            "(padding {})", found, uid_list, slotted, pad ? "on" : "off");

    int named = 0;
    PPCFunc* name_fn = GuestFn(kAssignNameToSlotFn);
    if (name_fn) {
        static_assert(kSetupCharCount <= kCharSlotMax,
                      "character slot table is 20 wide");
        for (int slot = 0; slot < kSetupCharCount; ++slot) {
            const uint32_t str = AllocGuestString(kSetupCharacters[slot]);
            if (!str) break;
            rex::ppc::GuestToHostFunction<uint32_t>(name_fn, str,
                                                    uint32_t(slot));
            ++named;
        }
    }
    MC_INFO("[cutscene] native setup: {} character slot(s)", named);
}

uint32_t StartScriptByName(const char* name, uint32_t stack) {
    const uint32_t mgr = ReadGuestBE32(kScriptMgrPtr);
    PPCFunc* fn = GuestFn(kStartThreadFn);
    if (!mgr || !fn) return 0;
    const uint32_t guest_name = AllocGuestString(name);
    if (!guest_name) return 0;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, guest_name, 0u, 1u, 0u,
                                                   0u, stack);
}

bool StartCutsceneScript(const Cutscene& cs) {
    const uint32_t mgr = ReadGuestBE32(kScriptMgrPtr);
    const uint32_t mgr_cine = ReadGuestBE32(kCineScriptPtr);
    if (!mgr) {
        MC_WARN("[cutscene] script manager not up yet");
        return false;
    }

    // The dev switches would make the scene bail with nothing on screen; clear
    // them rather than let a replay look like it silently did nothing.
    if (ReadGuestBE32(kNoCutscenesFlag) || ReadGuestBE32(kNoAnimPacksFlag)) {
        MC_WARN("[cutscene] nocutscenes/nocineanimpacks are set — the scene "
                "would be skipped; not starting");
        return false;
    }

    PPCFunc* fn = GuestFn(kStartThreadFn);
    if (!fn) {
        MC_WARN("[cutscene] StartNewThread 0x{:08X} not in dispatcher",
                kStartThreadFn);
        return false;
    }

    // CineScript_SKIPPING_CUTSCENE (0x823BED38) only ever sets mgr+0x123, and
    // nothing clears it -- not even FinishScene. Left alone it reports the
    // previous attempt's failure on this one.
    if (uint8_t* base = Membase()) base[mgr_cine + 0x123] = 0;

    // The interior the scene stands in, if it has one. The parent gameplay
    // script loads it before launching the generated script, so this goes in
    // the same place. It streams in the background -- the scene's own script
    // waits on its assets anyway, and the props arriving a frame or two late is
    // what the game itself lives with.
    LogPlayerRacer("before the replay");
    if (const char* items = SetDressingFor(cs.id)) LoadSetDressing(items);

    const std::string path = ScriptPath(cs);
    // sub_82727668 copies the name into its own 256-byte buffer (sub_823DB670)
    // before anything else, so a scratch guest allocation is enough.
    const uint32_t name = AllocGuestString(path.c_str());
    if (!name) return false;

    // Same shape the game uses for its own native script launches
    // (sub_822C4BD8): root thread as the parent, flag 1, no load args.
    const uint32_t thread = rex::ppc::GuestToHostFunction<uint32_t>(
        fn, name, 0u, 1u, 0u, 0u, kScriptStackSize);
    if (!thread) {
        MC_WARN("[cutscene] '{}' failed to mount", path);
        return false;
    }

    g_launched_thread.store(thread, std::memory_order_relaxed);
    MC_INFO("[cutscene] '{}' started, thread 0x{:08X}", path, thread);
    // Once per session: the registry is fixed after boot, and this is what
    // lets the offline .sco disassembly name its call sites.
    static bool natives_logged = false;
    if (!natives_logged) {
        natives_logged = true;
        LogNativeHandlers();
    }
    LogLaunchedScrThread("at launch");
    return true;
}

// ── Replay state machine ──────────────────────────────────────────────
//
// Idle -> Closing -> Starting -> Playing -> Idle. Closing exists because the
// pause menu holds the game paused, and core_rolling_prototype's load loop
// polls Game_IsPaused and gives up after ten seconds of it.

enum class State { kIdle, kClosing, kSetup, kStarting, kPlaying };

std::atomic<int> g_state{int(State::kIdle)};
std::atomic<int> g_pending{-1};   // table index armed by CutsceneRequestPlay
int  g_wait_frames = 0;
int  g_pop_attempts = 0;
int  g_playing_frames = 0;
int  g_stall_frames = 0;
float g_last_cam_clock = -2.0f;
float g_last_scene_clock = -2.0f;
int  g_close_wait = 0;
// Set when we unpaused the game ourselves, so the menu is handed back the same
// way once the scene is over.
bool g_resumed_for_replay = false;

// How often the gate dump goes out while a scene is up, and how long a scene
// may sit there before it is treated as wedged. 60 fps nominal.
constexpr int kGateLogPeriod = 120;
// How long a scene may make NO progress before it is treated as wedged. This is
// not a cap on scene length: cut_story_district_champs is 74.33s, and a flat
// 1800-frame cap tore it down at t=30.0s with the camera still moving, which
// looked exactly like the game cutting back on its own -- with the speech still
// playing, because a forced teardown does not stop it. Progress is measured off
// the cutscene camera's own clock and the scene clock, so a scene stays alive
// for as long as either is advancing.
constexpr int kStallLimit = 600;

// How many VirtualHSM levels to pop before giving up on closing the menu for
// the player and just waiting for them to do it, and how long to wait after
// that.
// Exactly one. The navigation stack is popped level by level, and there are
// only two of ours to leave: our own submenu, which PauseMenuLeaveSubmenu takes
// off, and the pause screen under it, which is this one. Four pops took two
// levels too many -- the screens the HUD and the pause button live on -- and the
// game came back with neither, which is the whole "HUD nao volta" story. Zero
// pops leaves the pause screen drawn over the scene. GuestStackPop logs the new
// top after each one, so the log shows exactly where this lands.
// Zero until the game's own close sequence is known. Every pop here is a guess
// about how many levels of navigation stack the pause screen occupies, and a
// guess that is too high takes the HUD and the pause button down with it -- the
// stack is popped level by level and does not care what it is unwinding. The
// menu item arms the replay; the player closes the menu the way the game
// intends, and the scene starts the moment the game is unpaused.
constexpr int kMaxPops = 0;
constexpr int kCloseTimeout = 1800;
// Frames to let the unpause settle before the script goes in.
constexpr int kSettleFrames = 4;
// Frames to let SETUP_GENERIC_CUTSCENE slot the racers and characters.
constexpr int kSetupFrames = 8;
// Frames to wait for IsScenePlaying to come up before calling it a failure.
constexpr int kStartTimeout = 600;

int FindCutscene(const std::string& id) {
    for (int i = 0; i < kCutsceneCount; ++i)
        if (id == kCutscenes[i].id) return i;
    return -1;
}

}  // namespace

// ── Starting the camera the queue did not start ───────────────────────
//
// Camera_LaunchEvent queues an event; the event's start handler (sub_823D7280)
// is what hands the clip to the camera and starts its clock, and until that
// clock leaves -1 the game's camera picker will not use the cutscene camera.
// If the event has been sitting in the queue for a few frames with the camera
// still idle, run that handler on the game's own event object -- same function,
// same argument, just called from here instead of from the event timeline.
void ForceCameraEventIfIdle() {
    if (!REXCVAR_GET(cutscene_force_camera) || g_cam_forced || !g_cam_event) return;
    const uint32_t director = ReadGuestBE32(kCineDirector);
    const uint32_t cam = director ? ReadGuestBE32(director + 8) : 0;
    if (!cam || ReadGuestFloat(cam + kCineCamTime) >= 0.0f) return;
    PPCFunc* fn = GuestFn(kCamEventStartFn);
    if (!fn) return;
    g_cam_forced = true;
    MC_WARN("[cutscene][cam] the queued camera event 0x{:08X} was never "
            "dispatched -- running its start handler by hand", g_cam_event);
    rex::ppc::GuestToHostFunction<void>(fn, uint32_t(0), g_cam_event);
    MC_WARN("[cutscene][cam] camera clock is now {:.3f} (clip 0x{:08X})",
            ReadGuestFloat(cam + kCineCamTime), ReadGuestBE32(cam + kCineCamClip));
}

// ── Public surface ────────────────────────────────────────────────────

int CutsceneCount() { return kCutsceneCount; }

const char* CutsceneId(int index) {
    if (index < 0 || index >= kCutsceneCount) return "";
    return kCutscenes[index].id;
}

const char* CutsceneLabel(int index) {
    if (index < 0 || index >= kCutsceneCount) return "";
    return kCutscenes[index].label;
}

const char* const* CutsceneIds() { return g_ids; }
const char* const* CutsceneLabels() { return g_labels; }

int CutsceneSelectedIndex() {
    const int i = FindCutscene(REXCVAR_GET(cutscene_replay));
    return i < 0 ? 0 : i;
}

bool CutsceneReplayBusy() {
    return State(g_state.load(std::memory_order_relaxed)) != State::kIdle;
}

void CutsceneRequestPlay() {
    if (CutsceneReplayBusy()) return;
    g_pending.store(CutsceneSelectedIndex(), std::memory_order_relaxed);
}

void TickCutsceneGallery() {
    // Escape hatch. `cutscene_stop = kill` asks politely (the script has to
    // poll +304); `= finish` runs the teardown itself and is what gets the HUD
    // back when the script is stuck before its own kill check.
    {
        const std::string stop = REXCVAR_GET(cutscene_stop);
        if (!stop.empty()) {
            rex::cvar::SetFlagByName("cutscene_stop", "");
            LogCutsceneGates("stop requested");
            if (!SceneIsPlaying())
                MC_WARN("[cutscene] nothing to stop — no scene is selected "
                        "(a hidden HUD here is the pause menu, not a cutscene)");
            RequestKillScene();
            if (stop == "finish") ForceFinishScene();
            g_playing_frames = 0;
            g_pending.store(-1, std::memory_order_relaxed);
            g_state.store(int(State::kIdle), std::memory_order_relaxed);
            return;
        }
    }

    // Run an arbitrary script object. Independent of the replay state machine
    // so it also works while a scene is up.
    {
        const std::string script = REXCVAR_GET(cutscene_run_script);
        if (!script.empty()) {
            rex::cvar::SetFlagByName("cutscene_run_script", "");
            const uint32_t t = StartScriptByName(script.c_str(), 1500);
            if (t)
                MC_INFO("[cutscene] '{}' started, thread 0x{:08X}",
                        script, t);
            else
                MC_WARN("[cutscene] '{}' failed to mount", script);
        }
    }

    // `cutscene_play_now` is the hands-off entry point: set it in
    // larecomp.toml or from the console and the scene starts on the next
    // frame, no menu involved.
    {
        const std::string now = REXCVAR_GET(cutscene_play_now);
        if (!now.empty()) {
            rex::cvar::SetFlagByName("cutscene_play_now", "");
            const int i = FindCutscene(now);
            if (i < 0) {
                MC_WARN("[cutscene] '{}' is not in the gallery", now);
            } else if (!CutsceneReplayBusy()) {
                rex::cvar::SetFlagByName("cutscene_replay", kCutscenes[i].id);
                g_pending.store(i, std::memory_order_relaxed);
            }
        }
    }

    const int pending = g_pending.load(std::memory_order_relaxed);
    State state = State(g_state.load(std::memory_order_relaxed));

    // Do NOT pause again here. Measured: unpausing to start the scene also takes
    // the pause menu off screen, so a PauseOnly afterwards leaves the game
    // frozen with no interface to unpause it -- a soft lock, and the player has
    // to kill the process. The scene ends, the game keeps running, and the
    // player opens the menu again themselves if they want it.
    if (state == State::kIdle && g_resumed_for_replay) {
        g_resumed_for_replay = false;
        LogPauseManager("replay over");
        MC_INFO("[cutscene] cine transitions={} eventState={}",
                ReadGuestBE32(kCineTransitions), ReadGuestBE32(kCineEventState));
        UnstickCineTransition("replay over");
        // A scene freezes the player while it runs and the flow is what lets
        // them drive again; a replay that ends early never gets there, so say it
        // explicitly. Resuming something already running is a no-op.
        RequestPauseState(kReqResumeLocally, "restoring player control");
        RequestPauseState(kReqResumeSim, "restoring the simulation");
        MC_INFO("[cutscene] replay over, leaving the game running");
    }

    if (state == State::kIdle) {
        if (pending < 0) return;
        if (!CineScriptReady()) {
            MC_WARN("[cutscene] no city loaded — nothing to play a scene in");
            g_pending.store(-1, std::memory_order_relaxed);
            return;
        }
        if (SceneIsPlaying()) {
            MC_WARN("[cutscene] a cutscene is already running");
            g_pending.store(-1, std::memory_order_relaxed);
            return;
        }
        g_pop_attempts = 0;
        g_close_wait = 0;
        g_wait_frames = 0;
        g_state.store(int(State::kClosing), std::memory_order_relaxed);
        return;
    }

    if (state == State::kClosing) {
        if (!GameIsPaused()) {
            g_wait_frames = kSettleFrames;
            g_state.store(int(State::kSetup), std::memory_order_relaxed);
            return;
        }
        // Do NOT pop the VirtualHSM stack blind. The pause tab is not on it --
        // measured long ago: eight pops and the game was still paused -- so the
        // pops never closed anything, they just unwound whatever WAS on it. That
        // is where the HUD lives: after a replay the game drove fine, the cine
        // state was idle and the pause manager was clean, and still no HUD and
        // no pause, because we had popped their screens off. Unpausing is what
        // takes the menu away; nothing needs popping.
        if (g_pop_attempts < kMaxPops) {
            ++g_pop_attempts;
            PauseMenuPopStack();
            return;
        }
        if (++g_close_wait == 1) {
            // Always step out of our own submenu first. While g_active_menu is
            // set our hooks consume every action press so the game never
            // resolves a click against the wrong list -- which means that if we
            // just sit here armed, the player cannot even close the pause menu:
            // the game looks frozen with the menu up. One pop, the same one a B
            // press would do, hands the input and the screen back.
            PauseMenuLeaveSubmenu();
            LogPauseManager("before the replay");
            if (!REXCVAR_GET(cutscene_menu_autoclose)) {
                MC_INFO("[cutscene] armed — close the pause menu and the scene "
                        "starts");
            } else if (!ClosePauseMenuLikeContinue()) {
                MC_WARN("[cutscene] could not reach the game's pause-menu close "
                        "— close it by hand and the scene starts");
            }
        }
        // Deliberately no timeout. The request used to be dropped after
        // kCloseTimeout frames, so picking a cutscene and taking a moment to
        // close the menu looked like the menu item did nothing at all. It waits
        // now; CutsceneRequestPlay refuses to arm a second one meanwhile.
        if (g_close_wait % kCloseTimeout == 0)
            MC_INFO("[cutscene] still armed — close the pause menu and the "
                    "scene starts");
        return;
    }

    if (state == State::kSetup) {
        if (g_wait_frames > 0) {
            --g_wait_frames;
            return;
        }
        if (REXCVAR_GET(cutscene_setup)) {
            // The dev harness is the better path when the build has it: it is
            // the game's own code. Retail archives may not ship tools/, so
            // check before trying and reproduce it natively otherwise.
            const bool have_script = ScriptObjectExists(kSetupScript);
            MC_INFO("[cutscene] '{}' present in the archive: {}", kSetupScript,
                    have_script);
            uint32_t t = 0;
            if (have_script) t = StartScriptByName(kSetupScript, 1500);
            if (t) {
                MC_INFO("[cutscene] '{}' started, thread 0x{:08X}",
                        kSetupScript, t);
            } else {
                if (have_script)
                    MC_WARN("[cutscene] '{}' exists but would not mount — "
                            "falling back to the native setup", kSetupScript);
                RunNativeSetup();
            }
        }
        // It has no children and no WAIT, so a few frames is plenty for it to
        // run and retire before the scene script reads the slots.
        g_wait_frames = kSetupFrames;
        g_state.store(int(State::kStarting), std::memory_order_relaxed);
        return;
    }

    if (state == State::kStarting) {
        if (g_wait_frames > 0) {
            --g_wait_frames;
            return;
        }
        const int idx = pending;
        g_pending.store(-1, std::memory_order_relaxed);
        if (idx < 0 || idx >= kCutsceneCount ||
            !StartCutsceneScript(kCutscenes[idx])) {
            g_state.store(int(State::kIdle), std::memory_order_relaxed);
            return;
        }
        g_wait_frames = kStartTimeout;
        g_playing_frames = 0;
        g_stall_frames = 0;
        g_last_cam_clock = -2.0f;
        g_last_scene_clock = -2.0f;
        g_cam_event = 0;
        g_cam_forced = false;
        for (auto& st : g_char_state) st = {};
        g_state.store(int(State::kPlaying), std::memory_order_relaxed);
        return;
    }

    // kPlaying. The script sets the scene name on its first tick, which is what
    // flips IsScenePlaying; until then wait, and once the scene has been up and
    // has gone down again the replay is over.
    if (SceneIsPlaying()) {
        g_wait_frames = -1;  // seen playing at least once
        // IsScenePlaying goes true the moment the scene is *named*, long before
        // anything is on screen, so it cannot tell a playing scene from one
        // wedged in core_rolling_prototype's load loop. Watch the gates instead:
        // they say which one never opened.
        // The script exiting while the scene is still selected is not a stall,
        // it is a bail-out: nothing is ever going to load, and the HUD stays
        // hidden until someone tears the scene down. Catch it directly.
        const uint32_t launched =
            g_launched_thread.load(std::memory_order_relaxed);
        if (launched && g_playing_frames > kSettleFrames &&
            !LaunchedThreadAlive(launched)) {
            MC_WARN("[cutscene] the script exited after {} frames without the "
                    "scene ever loading — it bailed out", g_playing_frames);
            LogCutsceneGates("script exited");
            g_launched_thread.store(0, std::memory_order_relaxed);
            RequestKillScene();
            ForceFinishScene();
            g_playing_frames = 0;
            g_state.store(int(State::kIdle), std::memory_order_relaxed);
            return;
        }
        // It bails within a handful of frames, so the interesting window is
        // right at the start.
        if (g_playing_frames < 8) LogLaunchedScrThread("running");
        // One line per change, so the whole replay costs a handful of lines.
        LogCineDirector("frame " + std::to_string(g_playing_frames), false);

        if (REXCVAR_GET(cutscene_force_events)) {
            const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
            uint8_t* base = Membase();
            if (mgr && base && base[mgr + 305] == 0) base[mgr + 305] = 1;
        }
        // The player asking to skip: mgr+305 is the request byte the game's own
        // skip path reads. Take it before sub_823BEEB0 sees it and end the
        // scene the way the stop cvar does.
        if (REXCVAR_GET(cutscene_skip_uses_teardown)) {
            const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
            uint8_t* base = Membase();
            if (mgr && base && base[mgr + 305]) {
                base[mgr + 305] = 0;
                MC_INFO("[cutscene] skip pressed — tearing the replay down "
                        "instead of using the game's skip path");
                RequestKillScene();
                ForceFinishScene();
                g_playing_frames = 0;
                g_state.store(int(State::kIdle), std::memory_order_relaxed);
                return;
            }
        }

        // Every frame, because the natural end is torn down by the script's own
        // FinishScene call, not by ours -- the flag has to be clear whenever
        // that happens. Nothing else reads mgr+316: SetSceneName sets it,
        // sub_823BEEB0 sets it on a skip, and FinishScene is the only reader.
        ClearSceneMovieTeardownFlag();
        ScrubSceneCoordSys("replay tick");
        UnstickAnimPackRelease("replay tick");
        ForceCameraEventIfIdle();
        // Liveness: the camera clock while a camera event is up, the scene clock
        // otherwise. Either one moving means the scene is still playing.
        {
            const uint32_t director = ReadGuestBE32(kCineDirector);
            const uint32_t cam = director ? ReadGuestBE32(director + 8) : 0;
            const uint32_t owner = ReadGuestBE32(kCineScriptPtr)
                ? ReadGuestBE32(ReadGuestBE32(kCineScriptPtr) + 268) : 0;
            const float cam_t = cam ? ReadGuestFloat(cam + kCineCamTime) : -1.0f;
            const float scene_t = owner ? ReadGuestFloat(owner + 364) : 0.0f;
            if (cam_t != g_last_cam_clock || scene_t != g_last_scene_clock) {
                g_last_cam_clock = cam_t;
                g_last_scene_clock = scene_t;
                g_stall_frames = 0;
            } else {
                ++g_stall_frames;
            }
        }

        if (++g_playing_frames % kGateLogPeriod == 0) {
            LogCutsceneGates("still waiting");
            LogCineDirector("still waiting", true);
            if (g_stall_frames >= kStallLimit) {
                MC_WARN("[cutscene] neither the camera nor the scene clock moved "
                        "for {} frames (scene ran {}) — tearing it down so the "
                        "game is playable again", g_stall_frames, g_playing_frames);
                RequestKillScene();
                ForceFinishScene();
                g_playing_frames = 0;
                g_state.store(int(State::kIdle), std::memory_order_relaxed);
            }
        }
        return;
    }
    const int played = g_playing_frames;
    g_playing_frames = 0;
    if (g_wait_frames > 0) {
        --g_wait_frames;
        return;
    }
    if (g_wait_frames == 0) {
        MC_WARN("[cutscene] scene never started — check the log for "
                "'SKIPPING CUTSCENE'");
        LogCutsceneGates("never started");
    } else {
        // core_rolling_prototype reaches Camera_Kill / PopKillBuffer /
        // FinishScene whether it played the scene or gave up on it, so the end
        // of the scene says nothing on its own. mgr+0x123 does: it is cleared
        // before every start, so if it is set now this run skipped.
        const uint32_t cine = ReadGuestBE32(kCineScriptPtr);
        const bool skipped = cine && ReadGuestU8(cine + 0x123);
        if (skipped)
            MC_WARN("[cutscene] scene ended after {} frames having SKIPPED "
                    "itself — it tore down without playing", played);
        else
            MC_INFO("[cutscene] replay finished after {} frames", played);
        LogCutsceneGates(skipped ? "skipped" : "finished");
        LogCineDirector("finished", true);
        LogPlayerRacer("after the replay");
    }
    g_state.store(int(State::kIdle), std::memory_order_relaxed);
}


// ── Why the cutscene camera never appears ─────────────────────────────
//
// mcCineScript::Camera_LaunchEvent (sub_823C3BA8) is what puts the cutscene
// camera on screen. Disassembling core_rolling_prototype shows the call sits at
// the exit of a loop, unconditional, immediately before
// EndDescriptionAndStartClock -- and a replay that reaches the clock (which is
// what raises readyForGame) has therefore already been through here. So "the
// game side never installs the camera" is wrong; it does.
//
// The handler has four ways out and each one means something different:
//
//   the animpack is not streamed  -> it looks the clip up in the global anim
//                                    dictionary instead of the pack
//   the event pool is empty       -> "camera event pool is depleted"
//   the clip is not in the pack   -> "could not find anim %s/%s in animpack %s",
//                                    and it carries on with a one second event
//   success                       -> the event is queued onto mgr+0x108
//
// Only the last one puts a camera anywhere, and even then something else has to
// consume the queue. These hooks say which of the four happened, once per
// launch, so the next step lands on the right thing.


// sub_823BEEB0 is the cutscene SKIP path, not the player: gated on mgr+305 it
// asks sub_823BBB98 for the time of the next cut event and jumps the scene clock
// to it. Measured with mgr+305 forced: garage picked t=2.833s (its whole length)
// and ended in 4 frames; district champs picked t=74.233s and the clock went
// 0.036 -> 74.254 in a single frame. The clock itself ticks fine on its own.
void MCLA_CineEventWalk(PPCRegister& r31) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged >= 3) return;   // just proof it runs; the result is the interesting half
    ++logged;
    MC_INFO("[cutscene][evt] walking the queue (mgr 0x{:08X})", r31.u32);
}

void MCLA_CineEventTime(PPCRegister& f1, PPCRegister& r31) {
    const uint32_t mgr = r31.u32;
    const uint32_t owner = ReadGuestBE32(mgr + 268);
    const uint32_t clock_bits = owner ? ReadGuestBE32(owner + 364) : 0;
    float clock = 0.0f;
    std::memcpy(&clock, &clock_bits, sizeof(clock));
    const float picked = float(f1.f64);

    // Only two things are worth a line: the frame that actually schedules the cut
    // (picked > 0 is what sub_823BEEB0 acts on), and the scene clock moving at all.
    static float last_clock = -1.0f;
    static int scheduled = 0;
    const bool clock_moved = clock != last_clock;
    last_clock = clock;

    if (picked > 0.0f && scheduled < 8) {
        ++scheduled;
        MC_WARN("[cutscene][evt] SCHEDULING the cut: picked t={:.3f}s with scene clock={:.3f}s "
                "-- the caller writes mgr+0x134 and ends the scene",
                picked, clock);
        return;
    }
    if (clock_moved && ProbeLog()) {
        MC_INFO("[cutscene][evt] scene clock now {:.3f}s (picked {:.3f}s)", clock, picked);
    }
}

void MCLA_CamEventEnter(PPCRegister& r3, PPCRegister& r4) {
    if (!ProbeLog()) return;
    MC_INFO("[cutscene][cam] Camera_LaunchEvent('{}', '{}')",
            GuestStringAt(r3.u32), GuestStringAt(r4.u32));
}

void MCLA_CamEventNoPack() {
    if (!ProbeLog()) return;
    MC_WARN("[cutscene][cam]   animpack not streamed -- looking the clip up in "
            "the global anim dictionary instead");
}

void MCLA_CamEventPool(PPCRegister& r29) {
    if (!ProbeLog()) return;
    if (r29.u32 == 0)
        MC_WARN("[cutscene][cam]   camera event pool is depleted, nothing was launched");
}

void MCLA_CamEventClip(PPCRegister& r30, PPCRegister& r28, PPCRegister& r27) {
    if (!ProbeLog()) return;
    if (r30.u32 != 0)
        MC_INFO("[cutscene][cam]   clip 0x{:08X} found", r30.u32);
    else
        MC_WARN("[cutscene][cam]   clip '{}/{}' is NOT in the animpack -- the event "
                "runs for one second with no camera",
                GuestStringAt(r28.u32), GuestStringAt(r27.u32));
}

void MCLA_CamEventQueued(PPCRegister& r3, PPCRegister& r4) {
    g_cam_event = r4.u32;
    if (!ProbeLog()) return;
    MC_INFO("[cutscene][cam]   event 0x{:08X} queued on manager 0x{:08X} -- from here "
            "the queue has to be played by the game",
            r4.u32, r3.u32);
}

// ── The hand-over itself ──────────────────────────────────────────────
//
// Four probes that together answer, in one run, why the screen is not the
// cutscene's. Read them in this order:
//
//   [handover] the game's own camera picker (sub_822C0320), one line per
//              change: whether it even asked, and what IsCameraActive said.
//   [cinecam]  the cutscene camera itself: its clock, its clip, and where the
//              evaluated matrix puts it.
//   [install]  the camera event's start handler running -- the only thing that
//              starts that clock. If this line never appears, the event was
//              queued and never dispatched, and that is the whole bug.

void MCLA_CineEventFired(PPCRegister& r4) {
    if (!ProbeLog()) return;
    const uint32_t evt = r4.u32;
    const uint32_t id = ReadGuestBE32(evt + 32);
    std::string name = "(out of range)";
    if (id < kCineEventNameCount)
        name = GuestStringAt(ReadGuestBE32(kCineEventNames + id * 4));
    const uint32_t mgr = ReadGuestBE32(kCineScriptPtr);
    const uint32_t owner = mgr ? ReadGuestBE32(mgr + 268) : 0;
    MC_WARN("[cutscene][event] '{}' (id {}) fired at scene t={:.3f}s "
            "(window {:.3f}..{:.3f})",
            name, id, owner ? ReadGuestFloat(owner + 364) : -1.0f,
            ReadGuestFloat(evt + 4), ReadGuestFloat(evt + 8));
}

// CineScript_Characters_LoadType (handler sub_823C0598) is what
// core_rolling_prototype calls once per character slot, looping static[16]
// times -- nine for cut_story_district_champs, whose own generated script
// assigns drv_mb_04_set, drv_mp_01_set, drv_mc_003_set, drv_fc_003_set,
// ped_fc_001_set, ped_mb_001_set, ped_ma_004_set and prp_sidekick_001_set
// twice. It forwards to sub_823CA198(charMgr, path, name) and the script ANDs
// every result into its "everyone is ready" local, so a false here is a
// character that will not be on screen.
// The script re-runs the whole loop every frame until every character is in, so
// a line per call is nine lines a frame for the second or so the streaming
// takes. Only say something when a given character's answer changes.
void MCLA_CineCharLoad(PPCRegister& r4, PPCRegister& r5) {
    if (!ProbeLog()) return;
    g_char_pending_path = GuestStringAt(r4.u32);
    g_char_pending_name = GuestStringAt(r5.u32);
}

void MCLA_CineCharLoaded(PPCRegister& r8) {
    if (!CutsceneReplayBusy() || g_char_pending_name.empty()) return;
    const bool loaded = r8.u32 != 0;

    // Keyed by name pointer: the script hands the same string object back every
    // frame, and a scene can ask for one character twice (two sidekicks).
    const uint32_t key = std::hash<std::string>{}(g_char_pending_name) & 0xFFFFFFFFu;
    CharSlotState* slot = nullptr;
    for (auto& st : g_char_state) {
        if (st.seen && st.key == key) { slot = &st; break; }
        if (!st.seen && !slot) slot = &st;
    }
    if (!slot) return;
    if (slot->seen && slot->loaded == loaded) return;
    const bool first = !slot->seen;
    *slot = {key, loaded, true};

    if (loaded && !first)
        MC_INFO("[cutscene][char] '{}' streamed in", g_char_pending_name);
    else if (loaded)
        MC_INFO("[cutscene][char] '{}' was already resident", g_char_pending_name);
    else
        MC_INFO("[cutscene][char] '{}' still streaming ({})",
                g_char_pending_name, g_char_pending_path);
}

// Characters_LaunchAnimEventWithFace -> sub_823C41D8 is the character half of
// Camera_LaunchEvent, and it has the same shape: take an event from a pool,
// resolve things, queue it on mgr+0x108. Its failure paths are:
//
//   pool empty          -> "character event pool is depleted"
//   sub_823CA248 == 0   -> SILENT. The event goes straight back to the pool and
//                          no actor is ever placed. This is the one to watch:
//                          it resolves the character instance for the slot, and
//                          nothing in the game prints when it fails.
//   clip not in pack    -> "could not find anim %s/%s in animpack %s"
//   success             -> sub_823D6A68 fills it, sub_824A3410 queues it
void MCLA_CineCharAnim(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                       PPCRegister& r6) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged++ >= 40) return;
    MC_INFO("[cutscene][actor] LaunchAnimEvent slot={} actor='{}' anim='{}/{}'",
            r3.u32, GuestStringAt(r4.u32), GuestStringAt(r5.u32),
            GuestStringAt(r6.u32));
}

void MCLA_CineCharActor(PPCRegister& r18) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged++ >= 40) return;
    if (r18.u32)
        MC_INFO("[cutscene][actor]   character instance 0x{:08X}", r18.u32);
    else
        MC_WARN("[cutscene][actor]   NO character instance -- the event is "
                "dropped silently and this actor never appears");
}

void MCLA_CineCharQueued(PPCRegister& r4) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged++ >= 40) return;
    MC_INFO("[cutscene][actor]   event 0x{:08X} queued", r4.u32);
}

// The character event's start handler is sub_823D7120 -> sub_823CA2E8 ->
// sub_823CA370 -> sub_823D24A8 (LaunchAnimAt), which puts the actor on screen:
//
//   v17 = sub_822E6A80(charMgr, "<name><suffix>")   <- find/create the entity
//   if (v17 && sub_823D1F48(v17, ...)) { ref++; return 1; }
//   return 0;
//
// Nobody checks that return, so both halves fail in silence: no entity, or an
// entity that never gets the animation bound. One line each.
// The clip sub_823BF2F0 resolved for the actor's body. When it comes back zero
// the launch carries on anyway -- the "could not find anim" complaint next to it
// goes through nullsub_1, which is a no-op in retail -- and the event is queued
// with no clip. Its start handler (sub_823D7120) then reads evt+40, sees zero,
// tries evt+48, sees zero, and returns without spawning anything.
void MCLA_CineActorClip(PPCRegister& r20) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged++ >= 40) return;
    if (r20.u32)
        MC_INFO("[cutscene][actor]   body clip 0x{:08X}", r20.u32);
    else
        MC_WARN("[cutscene][actor]   body clip NOT FOUND in the animpack -- the "
                "event is queued with no animation and spawns nothing");
}

// The character event's start, the twin of sub_823D7280. evt+40 is the body
// clip, evt+48 the alternate; both zero means this returns silently.
void MCLA_CineActorStart(PPCRegister& r4) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged++ >= 40) return;
    // dword_828CD5C0 picks which half of sub_823D1F48 runs, and it also changes
    // the name suffix in sub_823D24A8. Zero = create a fresh actor instance
    // (sub_823D1300, then enable + place + play). Non-zero = hunt for an
    // instance that already exists and hang the animation on that one instead.
    MC_WARN("[cutscene][actor] start handler ran for event 0x{:08X} "
            "(instance 0x{:08X}, clip 0x{:08X}, alt 0x{:08X}, mode={})",
            r4.u32, ReadGuestBE32(r4.u32 + 36), ReadGuestBE32(r4.u32 + 40),
            ReadGuestBE32(r4.u32 + 48), ReadGuestBE32(0x828CD5C0));
}

void MCLA_CineActorName(PPCRegister& r4) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged++ >= 40) return;
    MC_INFO("[cutscene][actor]   looking the entity up as '{}'",
            GuestStringAt(r4.u32));
}

void MCLA_CineActorEntity(PPCRegister& r31) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged++ >= 40) return;
    if (r31.u32)
        MC_INFO("[cutscene][actor]   entity 0x{:08X}", r31.u32);
    else
        MC_WARN("[cutscene][actor]   NO entity for this actor -- the character "
                "manager did not create one");
}

void MCLA_CineActorBound(PPCRegister& r11) {
    if (!ProbeLog()) return;
    static int logged = 0;
    if (logged++ >= 40) return;
    if (r11.u32) MC_INFO("[cutscene][actor]   animation bound, actor is live");
    else         MC_WARN("[cutscene][actor]   animation NOT bound -- entity "
                         "exists but plays nothing");
}

void MCLA_GameCamInGame(PPCRegister& r19) {
    static uint32_t last = 0xFFFFFFFFu;
    if (!CutsceneReplayBusy() || r19.u32 == last) return;
    last = r19.u32;
    if (r19.u32)
        MC_WARN("[cutscene][handover] the picker took the in-game override branch "
                "-- it never asks about the cutscene camera this frame");
    else
        MC_INFO("[cutscene][handover] picker is asking the director for a camera");
}

void MCLA_GameCamActive(PPCRegister& r11) {
    static uint32_t last = 0xFFFFFFFFu;
    if (!CutsceneReplayBusy() || r11.u32 == last) return;
    last = r11.u32;
    const uint32_t director = ReadGuestBE32(kCineDirector);
    const uint32_t cam = director ? ReadGuestBE32(director + 8) : 0;
    MC_WARN("[cutscene][handover] IsCameraActive -> {} (bypass={} state={} "
            "camClock={:.3f})",
            r11.u32, ReadGuestU8(director + 20), ReadGuestBE32(director + 12),
            cam ? ReadGuestFloat(cam + kCineCamTime) : -1.0f);
}

void MCLA_CineCamUpdate(PPCRegister& r3) {
    if (!ProbeLog()) return;
    const uint32_t cam = r3.u32;
    const float t = ReadGuestFloat(cam + kCineCamTime);
    static int period = 0;
    static bool was_running = false;
    const bool running = t >= 0.0f;
    if (running != was_running) {
        was_running = running;
        period = 0;
        MC_WARN("[cutscene][cinecam] camera clock {} (cam 0x{:08X}, clip 0x{:08X})",
                running ? "STARTED" : "went idle", cam,
                ReadGuestBE32(cam + kCineCamClip));
    }
    if (!running || period++ % 30 != 0) return;
    MC_INFO("[cutscene][cinecam] t={:.3f}s pos=({:.1f}, {:.1f}, {:.1f}) fovRad={:.3f}",
            t, ReadGuestFloat(cam + kCineCamPos),
            ReadGuestFloat(cam + kCineCamPos + 4),
            ReadGuestFloat(cam + kCineCamPos + 8),
            ReadGuestFloat(cam + kCineCamFov));
}

void MCLA_CineCamInstall(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    if (!ProbeLog()) return;
    MC_WARN("[cutscene][install] camera event start ran: director 0x{:08X} clip "
            "0x{:08X} binding 0x{:08X} (binding[0]=0x{:08X}, offset {:.1f}, "
            "{:.1f}, {:.1f})",
            r3.u32, r4.u32, r5.u32, ReadGuestBE32(r5.u32),
            ReadGuestFloat(r5.u32 + 8), ReadGuestFloat(r5.u32 + 12),
            ReadGuestFloat(r5.u32 + 16));
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include <rex/ppc/context.h>

#include "cutscene_gallery.h"

int  CutsceneCount() { return 0; }
const char* CutsceneId(int) { return ""; }
const char* CutsceneLabel(int) { return ""; }
const char* const* CutsceneIds() { return nullptr; }
const char* const* CutsceneLabels() { return nullptr; }
int  CutsceneSelectedIndex() { return 0; }
void CutsceneRequestPlay() {}
bool CutsceneReplayBusy() { return false; }
void TickCutsceneGallery() {}

void MCLA_CamEventEnter(PPCRegister&, PPCRegister&) {}
void MCLA_CineEventTime(PPCRegister&, PPCRegister&) {}
void MCLA_CineEventWalk(PPCRegister&) {}
void MCLA_CamEventNoPack() {}
void MCLA_CamEventPool(PPCRegister&) {}
void MCLA_CamEventClip(PPCRegister&, PPCRegister&, PPCRegister&) {}
void MCLA_CamEventQueued(PPCRegister&, PPCRegister&) {}
void MCLA_CineEventFired(PPCRegister&) {}
void MCLA_CineCharLoad(PPCRegister&, PPCRegister&) {}
void MCLA_CineCharLoaded(PPCRegister&) {}
void MCLA_CineCharAnim(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void MCLA_CineCharActor(PPCRegister&) {}
void MCLA_CineCharQueued(PPCRegister&) {}
void MCLA_CineActorClip(PPCRegister&) {}
void MCLA_CineActorStart(PPCRegister&) {}
void MCLA_CineActorName(PPCRegister&) {}
void MCLA_CineActorEntity(PPCRegister&) {}
void MCLA_CineActorBound(PPCRegister&) {}
void MCLA_GameCamInGame(PPCRegister&) {}
void MCLA_GameCamActive(PPCRegister&) {}
void MCLA_CineCamUpdate(PPCRegister&) {}
void MCLA_CineCamInstall(PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif  // REXGLUE_HAS_XEO3_TARGET

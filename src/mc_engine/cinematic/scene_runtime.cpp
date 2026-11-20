#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The scene runner's glue (trailer toolkit, work package M1a; feature-parity/CINEMATIC-DESIGN.md section 7). The format, the
// timeline and the clock are in scene.h; what is here is arming, reading the file, and putting the world tracks into the game.
//
// Cvars (all live, all debug-only):
//   mcla_scene           the scene file to run: a full path, or a name that is looked up next to the game and in its
//                        scenes folder (".scene" may be left off). Empty: off and nothing runs. Read again when its time
//                        stamp changes; a reload runs the scene again from its start.
//   mcla_scene_restart   a change of this number runs the scene again from its start (a take, again).
//   mcla_scene_t         0 or more: scene time IS this value on every update (stills, scrubbing); -1: it runs.
//
// Armed, the runner waits until a car is on the road and the ambient manager exists (30 updates in a row), then starts scene
// time at 0. Scene time is the engine timer's UNSCALED frame delta (timer + 0x58), so a time scale track slows the world and
// leaves the scene's own clock alone; it holds while the game is paused. Every update, from Patch_DeltaTimePre on the game thread:
//
//   hour        TimeOfDay_SceneHour (time_of_day.cpp: the scene's own channel, which outranks a menu shot's request,
//               time_of_day_realtime and time_of_day_hold, and which menu_camera.cpp never touches; Hook_TimeOfDay pins the
//               hour at the next lighting update). Released with a negative hour. The shared TimeOfDay_RequestHour pair was
//               what the first version used: menu_camera.cpp clears it on every gameplay frame, so no hour ever applied.
//   weather     Weather_SceneIndex, the same contract. Released with a negative index.
//   traffic, peds, parked cars
//               AmbientMgr_SetTrafficEnable / SetPedsEnable / SetParkedCarsEnable, the game's own script natives, called through
//               native_call.h: the native stores the enable byte (0x8282A7C8 + 0, 1, 2) and does what a script's call does. The
//               byte is read back to see whether it took; a byte the game put back is written again (three times at most).
//               E1b measured that the byte alone only stops NEW spawns; whether the native also clears what is on the road is
//               what the first run says (CHECK). The traffic native (SetTrafficEnable(false)) is read in the generated code to send the
//               despawn message 0x60 as well, which clears the road.
//   traffic off ALSO holds the setting traffic_density_scale at 0 (world_tuning.cpp: the zones' traffic density 0, and its gate holds the enable
//               byte at 0 every update whatever writes it); `traffic on` and the scene's end give both back. ORDER: that gate saves the byte it finds when it
//               closes, and puts it back when it opens, so the native is called one update AFTER the setting was taken (the gate runs at the top of
//               every update, before this code): called before, the gate would save the 0 the native wrote and "restore" it at the end. The byte the
//               game had before the scene is read when the setting is taken, so the end gives the native the right value.
//   peds <scale>, parked <scale>
//               the settings ped_density_scale / parked_car_scale held for the scene (the old value and its origin come back, as for `set`): the
//               timeline's numbers, 0 to 2. (A 0 closes that spawner for new spawns; what is on the road stays until it leaves: `peds off` also
//               calls the native.) A world key holds a setting through the same list as a `set` key and lets go of it when the key does.
//   police      PoliceManager_SetAllPoliceActive (a native: no byte to read back).
//   timescale   MCLATimeScaleOverride (the global function at the end of this file, a midasm hook in larecomp_config.toml): after
//               `lfs f13,84(r3)` in the engine timer sub_821BDA90 it puts the scene's scale into f13, the register the timer
//               multiplies the frame's step by. The word at timer + 0x54 (0x827D7554) is NOT the control, it is a copy: the main
//               loop (sub_822C1FA8) overwrites it every frame with the game's global 0x827E1C34 (floor 0.1 from the image
//               0x82004288; forced back to 1.0 when sub_82387A18 is true, probably a network game [CHECK]; the time-warp
//               effects of sub_82653920 and the replay camera sub_822A76B8 write that global), so a scene's write to the word
//               was undone before the timer read it (the first run: "something else set the time scale" 100 times, engine
//               step ratio 1.000). Overriding in the timer beats all of that and goes below 0.1 (the timer's own floor, 1e-4 s on the scaled step, is what is left).
//               The state line says what the engine really did: the scaled step over the unscaled one (timer + 0x08 / + 0x58);
//               "game" is the scale the game's own code gave the timer. Not applied while paused, in a menu or a load.
//   set         a setting through larecomp::SetCvarForScene (SetFlagByName, and the old value and its origin are kept: the value comes back
//               when the scene ends, and a save of the settings in the middle of the scene does not write the scene's value). Only live
//               (hot reload) settings, and not the ones CvarRefusal (scene.h) names: a scene cannot start a program or write a file.
//   label, mark logged. Labels are data for the compositor: nothing is drawn in the game.
//   capture     nothing here: capture_mode.cpp registers the applier that asks for a take (a fixed step, the frame gate and the readback)
//               from the `capture fps` key to `capture off` or the scene's end, with the take's name, sink and state log (M2b). The
//               take copies the scene file (SceneFilePath) and reads the game's state for its state log (SampleGameState).
//   split       the renderer's before/after split position (split_runtime.cpp: a TrackApplier that writes mcla_gpu_split).
//   camera, cut, hud, car
//               nothing here: camera_runtime.cpp, hud_runtime.cpp and cars.cpp register a TrackApplier (scene_runtime.h) each.
//   start       no applier yet (the preamble: save, seed, warp, car slot); the runner says so in the log.
//
// The scene ends at its `end` (a scene with none runs until the cvar is cleared): everything it changed goes back, the world
// tracks to the game's own, the settings to their old values, the toggles to what the bytes were.
//
// Written without a build or a run. Every guest address is from the generated code and the notes of the repo (hooks_internal.h,
// cutscene_gallery.cpp, time_of_day.cpp, cin_fixedstep.cpp of test/cin-base, E4); CHECK where nothing has shown it in a running game.
//

#include "scene_runtime.h"
#include "scene_want.h"
#include "spots.h"

#include "settings/larecomp_settings.h"
#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "mc_engine/core/config_save.h"
#include "mc_engine/gameplay/time_of_day.h"
#include "larecomp_log.h"
#include "capture_mode.h"
#include "native_call.h"
#include "state_log.h"

#include "../hooks/hooks.h"  // MCLATimeScaleOverride, defined at the end of this file
#include "larecomp_guest.h"

// ---------------------------------------------------------------- cvars

namespace mc::cinematic {

namespace {

// ---------------------------------------------------------------- guest addresses

constexpr uint32_t kGuestScaledDelta = 0x827D7508;    // timer + 0x08: the frame's delta after the time scale (hooks_internal.h)
constexpr uint32_t kGuestUnscaledDelta = 0x827D7558;  // timer + 0x58: the frame's delta before the time scale
constexpr uint32_t kGuestTimeScale = 0x827D7554;      // timer + 0x54: the scale the game's own code gave the timer (read only here)
constexpr uint32_t kPauseStatePtr = 0x828CCEA0;       // +4 paused, +10 run-one-frame (map_warp.cpp, cutscene_gallery.cpp)
constexpr uint32_t kAmbientMgrPtr = 0x8286D8E4;       // the ambient manager (world_tuning.cpp)
constexpr uint32_t kPlayerMgrPtr = 0x82874374;
constexpr uint32_t kRacerEntityAt = 48;    // racer + 48: its entity (map_warp.cpp)
constexpr uint32_t kEntityMatrixAt = 28;   // entity + 28: a pointer to the live matrix, whose 16 floats start at +16
constexpr uint32_t kPlayerValidFn = 0x821E80F8;       // (playerMgr, index) -> bool
constexpr uint32_t kPlayerRacerFn = 0x822A3998;       // (playerMgr, index) -> racer
constexpr uint32_t kPlayerSweep = 4;

struct ToggleDef {
    const char* name;
    uint32_t wrapper;  // the script native's wrapper (feature-parity/natives/natives.tsv)
    uint32_t byte;     // the enable byte the native stores first (cin_fixedstep.cpp of test/cin-base); 0 = none to read
    int WorldState::*field;
};
constexpr ToggleDef kToggles[] = {
    {"traffic", 0x826D67F0, 0x8282A7C8, &WorldState::traffic},  // AmbientMgr_SetTrafficEnable
    {"peds", 0x826D6810, 0x8282A7C9, &WorldState::peds},        // AmbientMgr_SetPedsEnable
    {"parked", 0x826D6830, 0x8282A7CA, &WorldState::parked},    // AmbientMgr_SetParkedCarsEnable
    {"police", 0x82703240, 0, &WorldState::police},             // PoliceManager_SetAllPoliceActive
};
constexpr size_t kToggleCount = sizeof(kToggles) / sizeof(kToggles[0]);

uint8_t* Membase() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

uint32_t R32(const uint8_t* base, uint32_t ea) {
    return (uint32_t(base[ea + 0]) << 24) | (uint32_t(base[ea + 1]) << 16) | (uint32_t(base[ea + 2]) << 8) | uint32_t(base[ea + 3]);
}

float RF(const uint8_t* base, uint32_t ea) {
    const uint32_t v = R32(base, ea);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = REX_RUNTIME();
    if (!rt) return nullptr;
    return rt->FindFunction(addr).value_or(nullptr);
}

bool GamePaused(const uint8_t* base) {
    const uint32_t p = R32(base, kPauseStatePtr);
    return p != 0 && base[p + 4] != 0 && base[p + 10] == 0;  // the test map_warp.cpp uses
}

// The player's racer (0 in a menu, the garage, a loading screen).
uint32_t PlayerRacer(const uint8_t* base) {
    const uint32_t mgr = R32(base, kPlayerMgrPtr);
    PPCFunc* valid_fn = GuestFn(kPlayerValidFn);
    PPCFunc* racer_fn = GuestFn(kPlayerRacerFn);
    if (!mgr || !valid_fn || !racer_fn) return 0;
    for (uint32_t i = 0; i < kPlayerSweep; ++i) {
        if (!(larecomp::guest::Call<uint32_t>(valid_fn, mgr, i) & 0xFF)) continue;
        if (const uint32_t r = larecomp::guest::Call<uint32_t>(racer_fn, mgr, i)) return r;
    }
    return 0;
}

// A car on the road and the ambient manager built: the world the tracks apply to.
bool WorldReady(const uint8_t* base) {
    return R32(base, kAmbientMgrPtr) != 0 && PlayerRacer(base) != 0;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path PathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string Utf8FromPath(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string Fmt(double v, const char* spec = "%.2f") {
    char buf[40];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

const char* EaseName(Ease e) {
    switch (e) {
        case Ease::kLinear: return "linear";
        case Ease::kIn: return "in";
        case Ease::kOut: return "out";
        case Ease::kInOut: return "inout";
    }
    return "?";
}

// One key as the log writes it.
std::string Describe(const Event& e) {
    std::string out = std::string(TrackName(e.track)) + " ";
    switch (e.track) {
        case Track::kHour:
            if (e.release) return out + "free (the game's own day cycle)";
            if (e.has_from) return out + Fmt(e.from) + ".." + Fmt(e.to) + " over " + Fmt(e.over) + " s, ease " + EaseName(e.ease);
            return out + Fmt(e.to) + " (held)";
        case Track::kWeather:
            if (e.release) return out + "game (the game's own weather)";
            return out + (Weather_IndexToName(e.index) ? Weather_IndexToName(e.index) : "?");
        case Track::kPeds:
        case Track::kParked:
            if (e.state == kStateScale) return out + Fmt(e.to) + " (the setting " + (e.track == Track::kPeds ? "ped_density_scale" : "parked_car_scale") + ", held for the scene)";
            return out + (e.state ? "on" : "off");
        case Track::kTraffic:
            return out + (e.state ? "on" : "off (the native, and the setting traffic_density_scale held at 0)");
        case Track::kPolice:
            return out + (e.state ? "on" : "off");
        case Track::kTimeScale:
            out += e.has_from ? Fmt(e.from) + ".." + Fmt(e.to) : Fmt(e.to);
            if (e.over > 0.0) out += " over " + Fmt(e.over) + " s, ease " + EaseName(e.ease);
            return out;
        case Track::kCvar:
            return out + e.name + " = " + e.text;
        case Track::kLabel:
            out += "\"" + e.text + "\"";
            if (!e.side.empty()) out += " side " + e.side;
            if (e.fade > 0.0) out += " fade " + Fmt(e.fade) + " s";
            if (e.over > 0.0) out += " for " + Fmt(e.over) + " s";
            return out + " (data for the compositor: nothing is drawn in the game)";
        case Track::kMark:
            return out + e.name;
        case Track::kCapture:
            if (!e.state) return out + "off (the take ends)";
            out += "fps " + Fmt(e.to, "%.6g") + " (a take: the game steps by 1/fps and every frame is read back)";
            if (!e.name.empty()) out += " name " + e.name;
            if (!e.text.empty()) out += " sink " + e.text;
            if (e.index >= 0) out += std::string(" statelog ") + (e.index ? "on" : "off");
            return out;
        case Track::kSplit:
            if (e.release) return out + "off (the effects are applied everywhere)";
            if (e.has_from) return out + Fmt(e.from) + ".." + Fmt(e.to) + " over " + Fmt(e.over) + " s, ease " + EaseName(e.ease);
            return out + Fmt(e.to) + " (held)";
        default:
            break;
    }
    for (const std::string& w : e.words) out += w + " ";
    if (!e.words.empty()) out.pop_back();
    return out;
}

// ---------------------------------------------------------------- the runner

struct SavedCvar {
    std::string name;
    std::string before;
};

// The density settings a world key holds (WantedScale): the cvar, a name for the log.
constexpr size_t kScaleCount = 3;
constexpr const char* kScaleCvars[kScaleCount] = {"traffic_density_scale", "ped_density_scale", "parked_car_scale"};
constexpr const char* kScaleKeys[kScaleCount] = {"traffic off", "peds", "parked"};

struct ToggleState {
    int orig = -1;     // the byte before the scene touched it (police: on)
    int applied = -1;  // what the scene made it, -1 = untouched
    int retries = 0;
    int failures = 0;
};

constexpr size_t kMaxAppliers = 8;  // camera, capture, cars, hud, and room for the free-camera recorder and the audio take
const TrackApplier* g_appliers[kMaxAppliers] = {};
size_t g_applier_count = 0;

// The scale the engine timer's hook hands the engine; negative = the game's own. Set on the game thread by the runner, read by the
// hook on whichever thread calls the timer.
std::atomic<float> g_timescale_override{-1.0f};

struct Runner {
    bool armed = false;
    std::string requested;  // the value of mcla_scene this run was armed for
    std::string path;       // where it was looked for
    std::string file_name;  // for the diagnostics
    int restart_seen = 0;
    SceneState state = SceneState::kOff;

    ReloadWatcher watcher;
    int64_t next_poll_ms = 0;
    bool have_scene = false;
    Scene scene;
    EventCursor cursor;
    SceneClock clock;
    int started_ticks = 0;  // updates spent waiting for the world, for the log

    bool hour_active = false;
    bool weather_active = false;
    bool timescale_active = false;
    bool paused = false;          // this update: the game is paused (the time scale override stands down)
    double ts_used = -1.0;        // the scale the timer was given for the frame it just timed (the previous update's), -1 = none
    int ts_unfollowed = 0;        // updates in a row in which the engine's step ratio was not that scale
    bool ts_warned = false;
    ToggleState toggles[kToggleCount];
    // The density settings the timeline holds (traffic_density_scale, ped_density_scale, parked_car_scale): the value held (-1 = not held), whether the
    // cvar took it, and the update that took it (the traffic native waits one update after it).
    double scale_applied[kScaleCount] = {-1.0, -1.0, -1.0};
    bool scale_held[kScaleCount] = {false, false, false};
    uint64_t scale_tick[kScaleCount] = {0, 0, 0};
    uint64_t world_tick = 0;  // updates run with the world in force
    std::vector<SavedCvar> saved;
    int unapplied[32] = {};  // keys of tracks nobody applies, by Track (a bit each in TrackBit: 32 at most)
    double next_state_log = 0.0;
    bool state_pending = false;  // a state line queued by the last update, written by the next (see RunAt)
    double state_t = 0.0;
    WorldState state_w;
    int calls_this_tick = 0;
    int waiting_ticks = 0;

    // The Director's handles (SceneHold, SceneSeekTo, ... in scene_runtime.h). Reset with the rest when a scene is armed.
    bool dir_held = false;      // the transport holds the clock (a pause or a seek)
    bool dir_frozen = false;    // ... and the world is held at the engine's time scale floor
    double dir_pin = -1.0;      // >= 0: the clock is put at this time every update (a seek; it stays until play, pause or another seek)
    int dir_steps = 0;          // updates still to run of a step: the clock moves and the world runs for them, then the hold is back
    bool dir_override_on = false;
    WorldState dir_override;    // the live preview of the world (world.set without commit): its set fields replace the timeline's
    bool ready_open = true;     // the ready gate let the clock go from t = 0 (true when no gate is installed)
    int64_t ready_since_ms = 0;
    bool ready_warned = false;
    uint64_t updates = 0;       // updates run with a scene loaded
    uint64_t begin_update = 0;  // the update count when the scene (re)started: the ready gate waits a few updates after it

    // The scene's named places (spots.h): the table of the spots files read for the last parse and the stamps they had, so a changed spots file reloads the scene.
    LoadedSpots spots;
};
Runner g;

// What the Director installed: the observer of loads and keys, the gate that holds t = 0 until the cars and the streaming are ready, and whether a changed
// file is loaded in place. Not part of the Runner: they outlive a scene that is armed again.
const SceneObserver* g_observer = nullptr;
bool (*g_ready_gate)() = nullptr;
std::atomic<bool> g_soft_reload{false};
constexpr int64_t kReadyGateMaxMs = 30000;  // the gate opens after this long whatever it says: a car that never comes must not hold a scene for ever

bool HandledByApplier(Track t) {
    for (size_t i = 0; i < g_applier_count; ++i) {
        if (g_appliers[i]->tracks & TrackBit(t)) return true;
    }
    return false;
}

// "scene.scene" -> as given when it exists, else in scenes/, with .scene added when it has no extension.
std::string ResolvePath(const std::string& want) {
    std::vector<std::string> tries{want};
    const bool has_ext = PathFromUtf8(want).has_extension();
    if (!has_ext) tries.push_back(want + ".scene");
    const bool absolute = PathFromUtf8(want).is_absolute();
    if (!absolute) {
        tries.push_back("scenes/" + want);
        if (!has_ext) tries.push_back("scenes/" + want + ".scene");
    }
    std::error_code ec;
    for (const std::string& t : tries) {
        if (std::filesystem::is_regular_file(PathFromUtf8(t), ec)) return t;
    }
    return want;
}

std::string LeafName(const std::string& path) {
    return Utf8FromPath(PathFromUtf8(path).filename());
}

// ---------------------------------------------------------------- applying

void ApplyToggle(size_t i, int want, uint8_t* base, int budget) {
    const ToggleDef& d = kToggles[i];
    ToggleState& s = g.toggles[i];
    const int byte = d.byte ? static_cast<int>(base[d.byte] != 0) : -1;
    int target = want;
    if (want < 0) {
        if (s.applied < 0) return;  // the scene never touched it
        target = s.orig >= 0 ? s.orig : 1;
    } else if (s.applied == want && (byte < 0 || byte == want)) {
        return;  // in force
    }
    if (g.calls_this_tick >= budget) return;  // one native an update while running; the next update takes the rest
    if (want >= 0 && s.applied == want) {
        // In force once and the byte went back: the game wrote it (a cutscene, a hangout).
        if (s.retries >= 3) return;
        ++s.retries;
        LARECOMP_APP_WARN("[scene] {}: the game set {} back to {} after the scene made it {}: calling the native again ({} of 3)",
                          d.name, d.name, byte, want, s.retries);
    } else if (want >= 0) {
        s.retries = 0;
    }
    if (s.failures >= 3) return;
    if (s.orig < 0) s.orig = byte >= 0 ? byte : 1;
    ++g.calls_this_tick;
    const NativeResult r = CallNative(d.wrapper, {static_cast<uint32_t>(target)});
    if (!r.called) {
        ++s.failures;
        LARECOMP_APP_WARN("[scene] {} {}: the native at 0x{:08X} could not be called ({}){}", d.name, target ? "on" : "off", d.wrapper,
                          "not on a guest thread, no such function, or no block for the context",
                          s.failures >= 3 ? "; giving up on it" : "");
        return;
    }
    const int after = d.byte ? static_cast<int>(base[d.byte] != 0) : -1;
    LARECOMP_APP_INFO("[scene] {} {}{}: native 0x{:08X} called in {:.2f} ms; enable byte {} -> {}{}", d.name, target ? "on" : "off",
                      want < 0 ? " (given back)" : "", d.wrapper, r.ms, byte, after,
                      (d.byte && after != target) ? "  <-- the byte is not what was asked" : "");
    if (want < 0) {
        s.applied = -1;
        s.orig = -1;
        s.retries = 0;
    } else {
        s.applied = want;
    }
}

// The time scale track. The scale itself is put into the engine timer by MCLATimeScaleOverride (the end of this file); here it is
// decided what that hook hands over, and checked that the engine took it.
void ApplyTimeScale(const WorldState& w, const uint8_t* base) {
    // What the engine made of the scale it was given for the frame it just timed (the previous update's value, set below).
    if (g.ts_used >= 0.0) {
        const double ratio = EngineStepRatio(RF(base, kGuestScaledDelta), RF(base, kGuestUnscaledDelta));
        if (TimeScaleFollowed(g.ts_used, ratio)) {
            g.ts_unfollowed = 0;
        } else if (++g.ts_unfollowed >= 120 && !g.ts_warned) {
            g.ts_warned = true;
            LARECOMP_APP_WARN("[scene] timescale: the engine's step ratio is {:.3f} and the scene asks for {:.3f}, {} updates in a row: "
                              "the timer hook (MCLATimeScaleOverride) may not be in this build, or the engine's own floor of 1e-4 s "
                              "under the scaled step holds a very small scale at this frame rate",
                              ratio, g.ts_used, g.ts_unfollowed);
        }
    }
    // The Director's hold freezes the world: the scale goes to 0 (the engine's own floor of 1e-4 s under the scaled step is what is left). A step lets it run.
    const bool frozen = g.dir_frozen && g.dir_steps == 0;
    const double want = g.paused ? -1.0 : (frozen ? TimeScaleForEngine(0.0) : (w.timescale_set ? TimeScaleForEngine(w.timescale) : -1.0));
    g_timescale_override.store(static_cast<float>(want), std::memory_order_relaxed);
    g.ts_used = want;
    if (w.timescale_set || frozen) {
        if (!g.timescale_active) {
            g.timescale_active = true;
            LARECOMP_APP_INFO("[scene] timescale: the scene's scale goes into the engine timer from now on (the game's own is {:.3f}); "
                              "the word at 0x827D7554 is only a copy the main loop rewrites every frame", RF(base, kGuestTimeScale));
        }
    } else if (g.timescale_active) {
        g.timescale_active = false;
        g.ts_unfollowed = 0;
        LARECOMP_APP_INFO("[scene] timescale: back to the game's own");
    }
}

// Not running now (a menu, a load): the engine runs at its own scale until the next update that runs the scene.
void StandDownTimeScale() {
    g_timescale_override.store(-1.0f, std::memory_order_relaxed);
    g.ts_used = -1.0;
    g.ts_unfollowed = 0;
}

// What the timeline wants a density setting at: -1 = not held. `traffic off` holds traffic_density_scale at 0 (the world's traffic key has no scale of its own).
double WantedScale(size_t i, const WorldState& w) {
    switch (i) {
        case 0: return w.traffic == 0 ? 0.0 : -1.0;
        case 1: return w.peds_scale;
        default: return w.parked_scale;
    }
}

// A setting held for a world key, through the same hold as a `set` key's (SetCvarForScene: the old value and where it came from are kept; a save in the middle
// of the scene does not write the scene's value). False when this build has no such setting, or it does not take the value.
bool HoldCvar(const char* name, const std::string& value, const char* why) {
    const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(name);
    if (!info) {
        LARECOMP_APP_WARN("[scene] {}: no setting called '{}' in this build: only the switch applies", why, name);
        return false;
    }
    if (info->lifecycle != larecomp::cvar::Lifecycle::kHotReload) {
        LARECOMP_APP_WARN("[scene] {}: '{}' is read only at start; the scene cannot hold it", why, name);
        return false;
    }
    const std::string before = larecomp::cvar::GetFlagByName(name);
    const bool known = std::any_of(g.saved.begin(), g.saved.end(), [&](const SavedCvar& s) { return s.name == name; });
    if (!larecomp::SetCvarForScene(name, value)) {
        LARECOMP_APP_WARN("[scene] {}: '{}' did not take '{}' (a value out of its range, or not of its kind)", why, name, value);
        return false;
    }
    if (!known) g.saved.push_back({name, before});
    LARECOMP_APP_INFO("[scene] {}: {} = {} (was {})", why, name, larecomp::cvar::GetFlagByName(name), before);
    return true;
}

void ReleaseCvar(const char* name, const char* why) {
    const auto it = std::find_if(g.saved.begin(), g.saved.end(), [&](const SavedCvar& s) { return s.name == name; });
    if (it == g.saved.end()) return;
    if (larecomp::RestoreCvarForScene(name)) LARECOMP_APP_INFO("[scene] {}: {} back to {}", why, name, it->before);
    else LARECOMP_APP_WARN("[scene] {}: {} could not be put back to {}", why, name, it->before);
    g.saved.erase(it);
}

// The density settings the world keys hold: taken when a key asks, given back when it lets go (and by RestoreCvars at the end).
void ApplyScales(const WorldState& w, const uint8_t* base) {
    for (size_t i = 0; i < kScaleCount; ++i) {
        const double want = WantedScale(i, w);
        if (want == g.scale_applied[i]) continue;
        if (want >= 0.0) {
            if (i == 0 && g.toggles[0].orig < 0 && base) {
                g.toggles[0].orig = static_cast<int>(base[kToggles[0].byte] != 0);  // the game's own byte, before the setting's gate can close and zero it
            }
            g.scale_held[i] = HoldCvar(kScaleCvars[i], Fmt(want, "%.6g"), kScaleKeys[i]);
            g.scale_applied[i] = want;
            g.scale_tick[i] = g.world_tick;
        } else {
            if (g.scale_held[i]) ReleaseCvar(kScaleCvars[i], kScaleKeys[i]);
            g.scale_held[i] = false;
            g.scale_applied[i] = -1.0;
        }
    }
}

// The world tracks as they should be now. `w` all unset hands everything back. budget: natives to call this update.
void ApplyWorld(const WorldState& w, uint8_t* base, int budget) {
    if (w.hour_set) {
        TimeOfDay_SceneHour(static_cast<float>(w.hour));
        g.hour_active = true;
    } else if (g.hour_active) {
        TimeOfDay_SceneHour(-1.0f);
        g.hour_active = false;
        LARECOMP_APP_INFO("[scene] hour: the game's own day cycle again");
    }
    if (w.weather >= 0) {
        Weather_SceneIndex(w.weather);
        g.weather_active = true;
    } else if (g.weather_active) {
        Weather_SceneIndex(-1);
        g.weather_active = false;
        LARECOMP_APP_INFO("[scene] weather: the game's own again");
    }
    ApplyTimeScale(w, base);
    ApplyScales(w, base);
    for (size_t i = 0; i < kToggleCount; ++i) {
        const int want = w.*(kToggles[i].field);
        // traffic off: the setting was taken in this update or an earlier one; the native comes one update after it, when the setting's gate has closed and saved the
        // game's own byte (see the header). Not held (a build without the setting): no wait.
        if (i == 0 && want == 0 && g.scale_held[0] && g.world_tick <= g.scale_tick[0]) continue;
        ApplyToggle(i, want, base, budget);
    }
}

void ApplyCvar(const Event& e) {
    // The parser already refuses these (the scene does not load); this stays because nothing else stands between a scene and the
    // program that ffmpeg's settings would start.
    if (const std::string why = CvarRefusal(e.name); !why.empty()) {
        LARECOMP_APP_ERROR("[scene] line {}: {}", e.line, why);
        return;
    }
    const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(e.name);
    if (!info) {
        LARECOMP_APP_WARN("[scene] line {}: no setting called '{}'", e.line, e.name);
        return;
    }
    if (info->type == larecomp::cvar::FlagType::Command) {
        LARECOMP_APP_WARN("[scene] line {}: '{}' is a command, not a setting", e.line, e.name);
        return;
    }
    if (info->lifecycle != larecomp::cvar::Lifecycle::kHotReload) {
        LARECOMP_APP_WARN("[scene] line {}: '{}' is read only at start; a scene cannot change it", e.line, e.name);
        return;
    }
    const std::string before = larecomp::cvar::GetFlagByName(e.name);
    const bool known = std::any_of(g.saved.begin(), g.saved.end(), [&](const SavedCvar& s) { return s.name == e.name; });
    if (!larecomp::SetCvarForScene(e.name, e.text)) {
        LARECOMP_APP_WARN("[scene] line {}: '{}' did not take '{}' (a value out of its range, or not of its kind)", e.line, e.name, e.text);
        return;
    }
    if (!known) g.saved.push_back({e.name, before});
    LARECOMP_APP_INFO("[scene] set {} = {} (was {})", e.name, larecomp::cvar::GetFlagByName(e.name), before);
}

// Every setting the scene took goes back to its value and its origin (default, file, command line, run time): a command-line value
// is not a change a later save would write.
void RestoreCvars() {
    for (auto it = g.saved.rbegin(); it != g.saved.rend(); ++it) {
        if (larecomp::RestoreCvarForScene(it->name)) {
            LARECOMP_APP_INFO("[scene] set {} back to {}", it->name, it->before);
        } else {
            LARECOMP_APP_WARN("[scene] {} could not be put back to {}", it->name, it->before);
        }
    }
    g.saved.clear();
    for (size_t i = 0; i < kScaleCount; ++i) {  // every setting a world key held went back with the list
        g.scale_applied[i] = -1.0;
        g.scale_held[i] = false;
    }
}

void EndAppliers() {
    for (size_t i = 0; i < g_applier_count; ++i) {
        if (g_appliers[i]->end) g_appliers[i]->end();
    }
}

void LogUnapplied() {
    std::string parts;
    for (int t = 0; t < 32; ++t) {
        if (g.unapplied[t] == 0) continue;
        if (!parts.empty()) parts += ", ";
        parts += std::string(TrackName(static_cast<Track>(t))) + " " + std::to_string(g.unapplied[t]);
    }
    if (!parts.empty()) LARECOMP_APP_INFO("[scene] keys nobody applies yet (no applier registered for them): {}", parts);
    std::memset(g.unapplied, 0, sizeof(g.unapplied));
}

void FlushState(const uint8_t* base);  // below, with the state line

// Everything the scene changed goes back: the world, the settings, the other packages' tracks.
void ReleaseAll(const char* why) {
    uint8_t* base = Membase();
    if (base) FlushState(base);  // the last update's state line, before the hand-back line
    LARECOMP_APP_INFO("[scene] handing the game back ({})", why);
    statelog::SceneEnd();
    if (base) {
        g.calls_this_tick = 0;
        ApplyWorld(WorldState{}, base, 8);
    }
    RestoreCvars();
    EndAppliers();
    LogUnapplied();
}

// The cvars and the other packages' tracks of the scene that is going; the world stays as it is until the next one says.
void ReleaseEvents() {
    statelog::SceneEnd();
    RestoreCvars();
    EndAppliers();
    LogUnapplied();
}

void BeginScene(const char* why) {
    g.cursor.Reset();
    g.next_state_log = 0.0;
    g.state_pending = false;
    g.ready_open = (g_ready_gate == nullptr);
    g.ready_since_ms = NowMs();
    g.ready_warned = false;
    g.begin_update = g.updates;
    std::memset(g.unapplied, 0, sizeof(g.unapplied));
    LARECOMP_APP_INFO("[scene] start ({}): '{}', {}", why, g.scene.name, Summarize(g.scene));
    statelog::SceneBegin(g.scene);  // opens the state log when the scene asks for one (`statelog on`)
    for (size_t i = 0; i < g_applier_count; ++i) {
        if (g_appliers[i]->begin) g_appliers[i]->begin(g.scene);
    }
}

void LogState(double t, const uint8_t* base, const WorldState& w) {
    const float hour = TimeOfDay_GetHour();
    // What the engine really did with the time scale: the frame's scaled step over its unscaled one. "game" is the scale the game's own
    // code gave the timer (the scene's replaces it inside the timer, so the two differ while a scene's time scale runs).
    const double ratio = EngineStepRatio(RF(base, kGuestScaledDelta), RF(base, kGuestUnscaledDelta));
    const auto scale = [](double s) { return s >= 0.0 ? " x" + Fmt(s) : std::string(); };  // " x0.30" while a density setting is held
    LARECOMP_APP_INFO("[scene] t={:.2f} state: hour {} (game {:.2f}), weather {} (game {}), timescale {} (game {:.3f}, engine step ratio {:.3f}), "
                      "traffic {} peds {}{} parked {}{} police {}",
                      t, w.hour_set ? Fmt(w.hour) : std::string("-"), hour, w.weather >= 0 ? w.weather : -1, Weather_GetIndex(),
                      w.timescale_set ? Fmt(w.timescale, "%.3f") : std::string("-"), RF(base, kGuestTimeScale), ratio, w.traffic, w.peds,
                      scale(w.peds_scale), w.parked, scale(w.parked_scale), w.police);
}

// Writes the state line the last update queued, if there is one.
void FlushState(const uint8_t* base) {
    if (!g.state_pending) return;
    g.state_pending = false;
    LogState(g.state_t, base, g.state_w);
}

// One update of a running scene (the world is there): the keys that came due, the world tracks, the other packages' tracks.
void RunAt(double t, uint8_t* base) {
    ++g.world_tick;
    std::vector<const Event*> due;
    g.cursor.Take(g.scene, t, &due);
    for (const Event* e : due) {
        LARECOMP_APP_INFO("[scene] t={:.2f} (line {}) {}", t, e->line, Describe(*e));
        if (g_observer && g_observer->key) g_observer->key(*e, t);
        switch (e->track) {
            case Track::kCvar:
                ApplyCvar(*e);
                break;
            case Track::kHour:
            case Track::kWeather:
            case Track::kTraffic:
            case Track::kPeds:
            case Track::kParked:
            case Track::kPolice:
            case Track::kTimeScale:
            case Track::kLabel:
                break;  // the world is read from the timeline below; labels are only logged
            case Track::kMark:
                statelog::NoteMark(e->name);  // the state log's next row carries it (a no-op when no log is open)
                break;
            default:
                if (!HandledByApplier(e->track)) ++g.unapplied[static_cast<int>(e->track)];
                break;
        }
    }
    g.calls_this_tick = 0;
    WorldState w = EvaluateWorld(g.scene, t);
    if (g.dir_override_on) {  // the Director's live preview: what it set replaces the timeline's value
        const WorldState& o = g.dir_override;
        if (o.hour_set) w.hour_set = true, w.hour = o.hour;
        if (o.weather >= 0) w.weather = o.weather;
        if (o.traffic >= 0) w.traffic = o.traffic;
        if (o.peds >= 0) w.peds = o.peds, w.peds_scale = -1.0;
        if (o.parked >= 0) w.parked = o.parked, w.parked_scale = -1.0;
        if (o.peds_scale >= 0.0) w.peds_scale = o.peds_scale, w.peds = -1;
        if (o.parked_scale >= 0.0) w.parked_scale = o.parked_scale, w.parked = -1;
        if (o.police >= 0) w.police = o.police;
        if (o.timescale_set) w.timescale_set = true, w.timescale = o.timescale;
    }
    ApplyWorld(w, base, 2);
    for (size_t i = 0; i < g_applier_count; ++i) {
        if (g_appliers[i]->apply) g_appliers[i]->apply(g.scene, t);
    }
    // The state line goes out one update late. What the scene sets now (the hour, the sky, the time scale) reaches the game on its next
    // lighting update and its next timer read, so a line written in this update would show the game one step behind at every key
    // ("weather 1 (game 0)" at the very update the weather key came due).
    FlushState(base);
    if (t >= g.next_state_log) {
        g.state_pending = true;
        g.state_t = t;
        g.state_w = w;
        g.next_state_log = std::floor(t / 2.0) * 2.0 + 2.0;
    }
}

// ---------------------------------------------------------------- the file

void NoteLoaded(const std::string& text, bool ok, int errors, bool soft, const char* by) {
    if (g_observer && g_observer->loaded) g_observer->loaded(g.path, text, ok, errors, soft, by);
}

// A changed scene replaces the one that runs IN PLACE (feature-parity/DIRECTOR-DESIGN.md 2.5): the clock stays where it is, the settings the old scene held go
// back and those of the new scene that are due by now are taken, the keys that were due keep their effect and are not played again, and the track appliers
// that can reload (the cars: only a car whose statement changed is made again) keep what they have. An applier with no `reload` is ended and begun again with
// the new scene, which is what a restart did for it before (the camera's rig starts over: one cut).
void SoftReload(Scene&& next, const char* by) {
    const double t = g.clock.time();
    RestoreCvars();
    g.scene = std::move(next);
    g.have_scene = true;
    for (const Event& e : g.scene.events) {
        if (e.track == Track::kCvar && e.t <= t + 1e-9) ApplyCvar(e);
    }
    g.cursor.Reset();
    std::vector<const Event*> past;
    g.cursor.Take(g.scene, t, &past);  // the keys up to now are in force already: not due again
    std::memset(g.unapplied, 0, sizeof(g.unapplied));
    for (size_t i = 0; i < g_applier_count; ++i) {
        const TrackApplier* a = g_appliers[i];
        if (a->reload) {
            a->reload(g.scene, t);
        } else {
            if (a->end) a->end();
            if (a->begin) a->begin(g.scene);
        }
    }
    LARECOMP_APP_INFO("[scene] reloaded in place by {} at t={:.2f}: '{}', {} (the clock and the cars are kept)", by, t, g.scene.name, Summarize(g.scene));
}

// The parsed scene becomes the scene (a file that was read, or the Director's own text). True when it was loaded in place.
bool ApplyScene(ParseResult&& r, const char* by) {
    const bool was_running = g.have_scene && (g.clock.state() == SceneClock::State::kRunning || g.clock.state() == SceneClock::State::kFinished);
    const bool soft = g_soft_reload.load(std::memory_order_relaxed) && g.have_scene && g.clock.state() == SceneClock::State::kRunning && !CaptureTakeActive();
    LARECOMP_APP_INFO("[scene] loaded {}: '{}', {}; {} warning(s)", g.file_name, r.scene.name, Summarize(r.scene), r.WarningCount());
    if (soft) {
        SoftReload(std::move(r.scene), by);
        return true;
    }
    if (g.have_scene) ReleaseEvents();
    g.scene = std::move(r.scene);
    g.have_scene = true;
    if (was_running) {
        g.clock.Restart();
        g.state = SceneState::kRunning;
        BeginScene("reloaded");
    } else {
        g.clock.Arm();
        g.state = SceneState::kWaiting;
        g.waiting_ticks = 0;
    }
    return false;
}

// The scene text parsed with the spots tables that apply to the scene's file (spots.h INTEGRATION): `spot:<name>` words become numbers, and the scene knows
// where it lies (a camera rig's route is looked for beside it). What was read stays in g.spots for the change check in PollFile.
ParseResult ParseSceneText(const std::string& text) {
    ParseResult r = ParseSceneWithSpots(text, g.path, &g.spots);
    for (const std::string& problem : g.spots.problems) LARECOMP_APP_WARN("[scene] spots: {}", problem);
    return r;
}

void LogDiagnostics(const ParseResult& r) {
    for (const Diagnostic& d : r.diagnostics) {
        const std::string line = FormatDiagnostic(d, g.file_name);
        if (d.severity == Severity::kError) LARECOMP_APP_ERROR("[scene] {}", line);
        else LARECOMP_APP_WARN("[scene] {}", line);
    }
}

void Load(const FileStamp& stamp) {
    std::ifstream in(PathFromUtf8(g.path), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    g.watcher.Loaded(stamp);
    ParseResult r = ParseSceneText(text);
    LogDiagnostics(r);
    if (!r.Ok()) {
        LARECOMP_APP_ERROR("[scene] {}: {} error(s), {}", g.file_name, r.ErrorCount(),
                           g.have_scene ? "the scene that was loaded stays" : "nothing to run");
        if (!g.have_scene) g.state = SceneState::kFailed;
        NoteLoaded(text, false, r.ErrorCount(), false, "file");
        return;
    }
    const bool soft = ApplyScene(std::move(r), "file");
    NoteLoaded(text, true, 0, soft, "file");
}

void PollFile() {
    const int64_t now = NowMs();
    if (now < g.next_poll_ms) return;
    g.next_poll_ms = now + 250;
    const std::filesystem::path p = PathFromUtf8(g.path);
    std::error_code ec;
    FileStamp stamp;
    stamp.exists = std::filesystem::is_regular_file(p, ec);
    if (stamp.exists) {
        stamp.size = static_cast<uint64_t>(std::filesystem::file_size(p, ec));
        stamp.mtime = static_cast<int64_t>(std::filesystem::last_write_time(p, ec).time_since_epoch().count());
    }
    switch (g.watcher.Poll(stamp, now)) {
        case ReloadWatcher::Action::kNone:
            // A spots file of the scene changed (a spot moved, one was added): the scene is read again, so the shot moves at once (soft: the clock and the cars stay).
            if (g.have_scene && stamp.exists && SpotsChanged(g.path, g.spots)) {
                LARECOMP_APP_INFO("[scene] a spots file of {} changed: reading the scene again", g.file_name);
                Load(stamp);
            }
            return;
        case ReloadWatcher::Action::kMissing:
            LARECOMP_APP_WARN("[scene] {}: no such file", g.path);
            if (!g.have_scene) g.state = SceneState::kFailed;
            return;
        case ReloadWatcher::Action::kLoad:
            Load(stamp);
            return;
    }
}

void Arm(const std::string& want) {
    if (g.armed) ReleaseAll("another scene was armed");
    g = Runner{};
    g.armed = true;
    g.requested = want;
    g.path = ResolvePath(want);
    g.file_name = LeafName(g.path);
    g.restart_seen = larecomp::settings::mcla_scene_restart.Get();
    g.state = SceneState::kPending;
    LARECOMP_APP_INFO("[scene] armed: {}", g.path);
}

void Disarm(const char* why) {
    ReleaseAll(why);
    g = Runner{};
    LARECOMP_APP_INFO("[scene] off ({})", why);
}

void Restart(const char* why, bool world_ready) {
    ReleaseEvents();
    if (world_ready) {
        g.clock.Restart();
        g.state = SceneState::kRunning;
        BeginScene(why);
    } else {
        g.clock.Arm();
        g.state = SceneState::kWaiting;
        g.waiting_ticks = 0;
    }
}

// One update of a loaded scene.
void Step(uint8_t* base) {
    if (g.state == SceneState::kFinished && g.restart_seen == larecomp::settings::mcla_scene_restart.Get()) return;  // handed back; nothing more to do
    const bool ready = WorldReady(base);
    const bool paused = GamePaused(base);
    const float dt = RF(base, kGuestUnscaledDelta);
    const double duration = g.scene.Duration();

    if (g.restart_seen != larecomp::settings::mcla_scene_restart.Get()) {
        g.restart_seen = larecomp::settings::mcla_scene_restart.Get();
        Restart("restart requested", ready);
    }

    ++g.updates;
    // The Director's hold: the clock does not move for a pause or a seek (except in the updates of a step). Before t = 0 the clock also waits until the scene's
    // cars are made and the streaming is done (the ready gate: a camera attached to a car that is still being made would hold the origin).
    const bool hold_clock = g.dir_held && g.dir_steps == 0;
    bool gate_wait = false;
    if (!g.ready_open && g.clock.state() == SceneClock::State::kRunning && g.clock.time() <= 0.0 && g.dir_pin < 0.0) {
        const bool settled = g.updates - g.begin_update >= 4 && g_ready_gate && g_ready_gate();
        const bool overdue = NowMs() - g.ready_since_ms > kReadyGateMaxMs;
        if (settled || overdue) {
            g.ready_open = true;
            if (overdue && !settled && !g.ready_warned) {
                g.ready_warned = true;
                LARECOMP_APP_WARN("[scene] the cars or the streaming were not ready after {} s: the clock goes on anyway", kReadyGateMaxMs / 1000);
            }
        } else {
            gate_wait = true;
        }
    }
    SceneClock::Step st = g.clock.Advance(ready, paused || hold_clock || gate_wait, (std::isfinite(dt) && dt > 0.0f) ? static_cast<double>(dt) : 0.0, duration);
    double still = larecomp::settings::mcla_scene_t.Get();
    if (still < 0.0) still = g.dir_pin;
    if (still >= 0.0 && g.clock.state() == SceneClock::State::kRunning) g.clock.SetTime(still, duration);

    switch (g.clock.state()) {
        case SceneClock::State::kWaiting:
            g.state = SceneState::kWaiting;
            ++g.waiting_ticks;
            return;
        case SceneClock::State::kRunning:
        case SceneClock::State::kFinished:
            break;
        case SceneClock::State::kIdle:
            return;
    }
    if (st.started) BeginScene("a car on the road and the ambient manager up");
    if (g.state == SceneState::kFinished && !st.finished) return;  // already handed back
    g.state = SceneState::kRunning;
    g.paused = paused;
    if (ready) RunAt(g.clock.time(), base);
    else StandDownTimeScale();
    if (g.dir_steps > 0 && ready) --g.dir_steps;  // one update of a step is done
    if (st.finished) {
        LARECOMP_APP_INFO("[scene] finished at t={:.2f}", g.clock.time());
        ReleaseAll("the scene's end");
        g.state = SceneState::kFinished;
    }
}

// ---------------------------------------------------------------- mcla_scene, read from two threads
//
// What scene is asked for is kept by SceneWant (scene_want.h: no lock and no allocation on a quiet tick, and why the change callback is
// hooked up on the first ask and not from a static initializer here: on 0.11 the setting does not exist yet at static initialization).
SceneWant g_want(SceneWant::Source{
    [](SceneWant::Notify notify) {
        return larecomp::cvar::RegisterChangeCallback(
            "mcla_scene", [notify = std::move(notify)](std::string_view, std::string_view value) { notify(value); });
    },
    [] { return larecomp::cvar::GetFlagByName("mcla_scene"); }});  // the cvar system's own locked read; none of our locks is held

}  // namespace

// ---------------------------------------------------------------- the public side

void TickScene() {
    statelog::Tick();  // the per-update state log (mcla_state_log or a scene's `statelog on`): the row is the state after the update before; one string test when off
    const std::string& want = g_want.Wanted();
    if (want.empty()) {
        if (g.armed) Disarm("mcla_scene was cleared");
        return;
    }
    uint8_t* base = Membase();
    if (!base) return;
    if (!g.armed || want != g.requested) Arm(want);
    PollFile();
    if (g.have_scene) Step(base);
}

SceneState CurrentSceneState() {
    if (!g.armed) return g_want.IsSet() ? SceneState::kPending : SceneState::kOff;
    return g.state;
}

const char* SceneStateName(SceneState state) {
    switch (state) {
        case SceneState::kOff: return "off";
        case SceneState::kPending: return "pending";
        case SceneState::kWaiting: return "waiting";
        case SceneState::kRunning: return "running";
        case SceneState::kFinished: return "finished";
        case SceneState::kFailed: return "failed";
    }
    return "?";
}

bool SceneDone() {
    const SceneState s = CurrentSceneState();
    return s == SceneState::kFinished || s == SceneState::kFailed;
}

double SceneTime() {
    return g.armed && g.have_scene ? g.clock.time() : 0.0;
}

std::string SceneFilePath() {
    return g.armed && g.have_scene ? g.path : std::string();
}

std::string SceneName() {
    return g.armed && g.have_scene ? g.scene.name : std::string();
}

bool SampleGameState(GameStateSample* out) {
    *out = GameStateSample{};
    const uint8_t* base = Membase();
    if (!base) return false;
    out->hour = TimeOfDay_GetHour();
    out->time_scale = RF(base, kGuestTimeScale);
    out->dt = RF(base, kGuestScaledDelta);
    out->dt_unscaled = RF(base, kGuestUnscaledDelta);
    out->paused = GamePaused(base);
    out->player = PlayerRacer(base);
    out->world_ready = R32(base, kAmbientMgrPtr) != 0 && out->player != 0;
    if (out->player != 0) {
        const uint32_t entity = R32(base, out->player + kRacerEntityAt);
        const uint32_t matrix = entity ? R32(base, entity + kEntityMatrixAt) : 0;
        if (matrix != 0) {
            for (int i = 0; i < 16; ++i) out->matrix[i] = RF(base, matrix + 16 + 4 * static_cast<uint32_t>(i));
            out->has_matrix = true;
        }
    }
    return true;
}

bool RegisterTrackApplier(const TrackApplier* applier) {
    if (!applier || g_applier_count >= kMaxAppliers) return false;
    g_appliers[g_applier_count++] = applier;
    return true;
}

// ---------------------------------------------------------------- the Director's handles

namespace {
FileStamp CurrentStamp() {
    const std::filesystem::path p = PathFromUtf8(g.path);
    std::error_code ec;
    FileStamp stamp;
    stamp.exists = std::filesystem::is_regular_file(p, ec);
    if (stamp.exists) {
        stamp.size = static_cast<uint64_t>(std::filesystem::file_size(p, ec));
        stamp.mtime = static_cast<int64_t>(std::filesystem::last_write_time(p, ec).time_since_epoch().count());
    }
    return stamp;
}
}  // namespace

const SpotTable* SceneSpotTable() {
    return g.armed ? &g.spots.table : nullptr;
}

const Scene* LoadedScene() {
    return g.armed && g.have_scene ? &g.scene : nullptr;
}

SceneTransport GetSceneTransport() {
    SceneTransport t;
    t.state = CurrentSceneState();
    if (!g.armed || !g.have_scene) return t;
    t.t = g.clock.time();
    t.duration = g.scene.Duration();
    t.held = g.dir_held;
    t.frozen = g.dir_frozen;
    t.pending_steps = g.dir_steps;
    t.pinned = g.dir_pin >= 0.0;
    t.holding_for_ready = !g.ready_open && g.clock.state() == SceneClock::State::kRunning;
    t.updates = g.updates;
    return t;
}

void SceneHold(bool hold) {
    if (!g.armed) return;
    g.dir_steps = 0;
    if (hold) {
        g.dir_held = true;
        g.dir_frozen = true;
    } else {
        g.dir_held = false;
        g.dir_frozen = false;
        g.dir_pin = -1.0;
    }
}

void SceneSeekTo(double t) {
    if (!g.armed || !g.have_scene) return;
    t = std::max(0.0, t);
    if (g.scene.Duration() >= 0.0) t = std::min(t, g.scene.Duration());
    if (g.state == SceneState::kFinished) {  // handed back: it has to run again before it can show a time
        SceneRestartAt(t);
        return;
    }
    g.dir_held = true;
    g.dir_frozen = true;
    g.dir_pin = t;
    g.dir_steps = 0;
    if (g.clock.state() == SceneClock::State::kRunning) g.clock.SetTime(t, g.scene.Duration());
}

void SceneStepUpdates(int updates) {
    if (!g.armed || !g.have_scene || updates < 1) return;
    g.dir_steps = std::min(updates, 100000);
    g.dir_pin = -1.0;
}

void SceneRestartAt(double t) {
    if (!g.armed || !g.have_scene) return;
    uint8_t* base = Membase();
    g.dir_steps = 0;
    Restart("the Director", base && WorldReady(base));
    g.dir_pin = t > 0.0 ? t : -1.0;
    g.dir_held = t > 0.0;
    g.dir_frozen = t > 0.0;
    if (t > 0.0 && g.clock.state() == SceneClock::State::kRunning) g.clock.SetTime(t, g.scene.Duration());
}

SceneTextResult SceneLoadText(const std::string& text, const char* by) {
    SceneTextResult res;
    if (!g.armed) return res;
    ParseResult r = ParseSceneText(text);
    res.diagnostics = r.diagnostics;
    res.errors = r.ErrorCount();
    LogDiagnostics(r);
    if (!r.Ok()) {
        LARECOMP_APP_ERROR("[scene] {}: {} error(s) in the text the Director gave: the scene that was loaded stays", g.file_name, r.ErrorCount());
        NoteLoaded(text, false, res.errors, false, by);
        return res;
    }
    g.watcher.Loaded(CurrentStamp());  // the Director wrote the file itself: the watcher must not read it again
    res.soft = ApplyScene(std::move(r), by);
    res.ok = true;
    NoteLoaded(text, true, 0, res.soft, by);
    return res;
}

void SceneSetWorldOverride(const WorldState& w) {
    g.dir_override = w;
    g.dir_override_on = true;
}

void SceneClearWorldOverride() {
    g.dir_override = WorldState{};
    g.dir_override_on = false;
}

WorldState SceneWorldOverride(bool* on) {
    if (on) *on = g.dir_override_on;
    return g.dir_override;
}

void SetSceneObserver(const SceneObserver* observer) {
    g_observer = observer;
}

void SetSceneReadyGate(bool (*gate)()) {
    g_ready_gate = gate;
    if (g.armed) g.ready_open = (gate == nullptr) || g.ready_open;
}

void SetSceneSoftReload(bool on) {
    g_soft_reload.store(on, std::memory_order_relaxed);
}

}  // namespace mc::cinematic

// A running scene's time scale, put into the engine timer. Midasm hook after the instruction at 0x821BDAB4 (`lbz r7,56(r3)`, the one
// after the `subf r8,r10,r11` that MCLAFrameDelta follows) in sub_821BDA90: f13 holds the scale loaded by `lfs f13,84(r3)` and is not
// touched again until the `fmuls f6,f13,f8` that makes timer + 0x60 and the `fmuls f0,f7,f13` that makes the scaled step (timer + 0x08).
// Without a scene's time scale it is one atomic load. The timer has callers on more than one thread; all of them get the scale.
void MCLATimeScaleOverride(PPCRegister& f13) {
    const float scale = mc::cinematic::g_timescale_override.load(std::memory_order_relaxed);
    if (scale >= 0.0f) f13.f64 = static_cast<double>(scale);
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include <rex/ppc/context.h>

#include "scene_runtime.h"

void MCLATimeScaleOverride(PPCRegister& f13) {}

namespace mc::cinematic {
void TickScene() {}
SceneState CurrentSceneState() { return SceneState::kOff; }
const char* SceneStateName(SceneState) { return "off"; }
bool SceneDone() { return false; }
double SceneTime() { return 0.0; }
std::string SceneFilePath() { return std::string(); }
std::string SceneName() { return std::string(); }
bool SampleGameState(GameStateSample* out) { *out = GameStateSample{}; return false; }
bool RegisterTrackApplier(const TrackApplier*) { return false; }
const Scene* LoadedScene() { return nullptr; }
const SpotTable* SceneSpotTable() { return nullptr; }
SceneTransport GetSceneTransport() { return SceneTransport{}; }
void SceneHold(bool) {}
void SceneSeekTo(double) {}
void SceneStepUpdates(int) {}
void SceneRestartAt(double) {}
SceneTextResult SceneLoadText(const std::string&, const char*) { return SceneTextResult{}; }
void SetSceneSoftReload(bool) {}
void SceneSetWorldOverride(const WorldState&) {}
void SceneClearWorldOverride() {}
WorldState SceneWorldOverride(bool* on) {
    if (on) *on = false;
    return WorldState{};
}
void SetSceneObserver(const SceneObserver*) {}
void SetSceneReadyGate(bool (*)()) {}
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET

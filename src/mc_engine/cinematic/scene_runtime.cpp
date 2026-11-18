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
//               what the first run says (CHECK).
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

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

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
#include "native_call.h"
#include "state_log.h"

#include "../hooks/hooks.h"  // MCLATimeScaleOverride, defined at the end of this file

// ---------------------------------------------------------------- cvars

REXCVAR_DEFINE_STRING(mcla_scene, "", "MCLA/Cinematic",
    "TRAILER TOOLKIT: run the scene file at this path: a full path, or a name looked up next to the game and in its scenes "
    "folder (.scene may be left off). Hour, weather, traffic, time scale and settings follow the scene's timeline; the file is "
    "read again when it changes. Empty: off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_INT32(mcla_scene_restart, 0, "MCLA/Cinematic",
    "TRAILER TOOLKIT: a change of this number runs the armed scene again from its start.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(mcla_scene_t, -1.0, "MCLA/Cinematic",
    "TRAILER TOOLKIT: 0 or more holds the scene's clock at this many seconds on every update (stills, scrubbing); -1 lets "
    "it run.")
    .range(-1.0, 86400.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

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
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
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
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return nullptr;
    return rt->function_dispatcher()->GetFunction(addr);
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
        if (!(rex::ppc::GuestToHostFunction<uint32_t>(valid_fn, mgr, i) & 0xFF)) continue;
        if (const uint32_t r = rex::ppc::GuestToHostFunction<uint32_t>(racer_fn, mgr, i)) return r;
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
        case Track::kTraffic:
        case Track::kPeds:
        case Track::kParked:
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
    std::vector<SavedCvar> saved;
    int unapplied[32] = {};  // keys of tracks nobody applies, by Track (a bit each in TrackBit: 32 at most)
    double next_state_log = 0.0;
    bool state_pending = false;  // a state line queued by the last update, written by the next (see RunAt)
    double state_t = 0.0;
    WorldState state_w;
    int calls_this_tick = 0;
    int waiting_ticks = 0;
};
Runner g;

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
    const double want = (w.timescale_set && !g.paused) ? TimeScaleForEngine(w.timescale) : -1.0;
    g_timescale_override.store(static_cast<float>(want), std::memory_order_relaxed);
    g.ts_used = want;
    if (w.timescale_set) {
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
    for (size_t i = 0; i < kToggleCount; ++i) ApplyToggle(i, w.*(kToggles[i].field), base, budget);
}

void ApplyCvar(const Event& e) {
    // The parser already refuses these (the scene does not load); this stays because nothing else stands between a scene and the
    // program that ffmpeg's settings would start.
    if (const std::string why = CvarRefusal(e.name); !why.empty()) {
        LARECOMP_APP_ERROR("[scene] line {}: {}", e.line, why);
        return;
    }
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(e.name);
    if (!info) {
        LARECOMP_APP_WARN("[scene] line {}: no setting called '{}'", e.line, e.name);
        return;
    }
    if (info->type == rex::cvar::FlagType::Command) {
        LARECOMP_APP_WARN("[scene] line {}: '{}' is a command, not a setting", e.line, e.name);
        return;
    }
    if (info->lifecycle != rex::cvar::Lifecycle::kHotReload) {
        LARECOMP_APP_WARN("[scene] line {}: '{}' is read only at start; a scene cannot change it", e.line, e.name);
        return;
    }
    const std::string before = rex::cvar::GetFlagByName(e.name);
    const bool known = std::any_of(g.saved.begin(), g.saved.end(), [&](const SavedCvar& s) { return s.name == e.name; });
    if (!larecomp::SetCvarForScene(e.name, e.text)) {
        LARECOMP_APP_WARN("[scene] line {}: '{}' did not take '{}' (a value out of its range, or not of its kind)", e.line, e.name, e.text);
        return;
    }
    if (!known) g.saved.push_back({e.name, before});
    LARECOMP_APP_INFO("[scene] set {} = {} (was {})", e.name, rex::cvar::GetFlagByName(e.name), before);
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
    LARECOMP_APP_INFO("[scene] t={:.2f} state: hour {} (game {:.2f}), weather {} (game {}), timescale {} (game {:.3f}, engine step ratio {:.3f}), "
                      "traffic {} peds {} parked {} police {}",
                      t, w.hour_set ? Fmt(w.hour) : std::string("-"), hour, w.weather >= 0 ? w.weather : -1, Weather_GetIndex(),
                      w.timescale_set ? Fmt(w.timescale, "%.3f") : std::string("-"), RF(base, kGuestTimeScale), ratio, w.traffic, w.peds,
                      w.parked, w.police);
}

// Writes the state line the last update queued, if there is one.
void FlushState(const uint8_t* base) {
    if (!g.state_pending) return;
    g.state_pending = false;
    LogState(g.state_t, base, g.state_w);
}

// One update of a running scene (the world is there): the keys that came due, the world tracks, the other packages' tracks.
void RunAt(double t, uint8_t* base) {
    std::vector<const Event*> due;
    g.cursor.Take(g.scene, t, &due);
    for (const Event* e : due) {
        LARECOMP_APP_INFO("[scene] t={:.2f} (line {}) {}", t, e->line, Describe(*e));
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
    const WorldState w = EvaluateWorld(g.scene, t);
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

void Load(const FileStamp& stamp) {
    std::ifstream in(PathFromUtf8(g.path), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    g.watcher.Loaded(stamp);
    ParseResult r = ParseScene(text);
    for (const Diagnostic& d : r.diagnostics) {
        const std::string line = FormatDiagnostic(d, g.file_name);
        if (d.severity == Severity::kError) LARECOMP_APP_ERROR("[scene] {}", line);
        else LARECOMP_APP_WARN("[scene] {}", line);
    }
    if (!r.Ok()) {
        LARECOMP_APP_ERROR("[scene] {}: {} error(s), {}", g.file_name, r.ErrorCount(),
                           g.have_scene ? "the scene that was loaded stays" : "nothing to run");
        if (!g.have_scene) g.state = SceneState::kFailed;
        return;
    }
    const bool was_running = g.have_scene && (g.clock.state() == SceneClock::State::kRunning || g.clock.state() == SceneClock::State::kFinished);
    if (g.have_scene) ReleaseEvents();
    g.scene = std::move(r.scene);
    g.have_scene = true;
    LARECOMP_APP_INFO("[scene] loaded {}: '{}', {}; {} warning(s)", g.file_name, g.scene.name, Summarize(g.scene), r.WarningCount());
    if (was_running) {
        g.clock.Restart();
        g.state = SceneState::kRunning;
        BeginScene("reloaded");
    } else {
        g.clock.Arm();
        g.state = SceneState::kWaiting;
        g.waiting_ticks = 0;
    }
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
    g.restart_seen = REXCVAR_GET(mcla_scene_restart);
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
    if (g.state == SceneState::kFinished && g.restart_seen == REXCVAR_GET(mcla_scene_restart)) return;  // handed back; nothing more to do
    const bool ready = WorldReady(base);
    const bool paused = GamePaused(base);
    const float dt = RF(base, kGuestUnscaledDelta);
    const double duration = g.scene.Duration();

    if (g.restart_seen != REXCVAR_GET(mcla_scene_restart)) {
        g.restart_seen = REXCVAR_GET(mcla_scene_restart);
        Restart("restart requested", ready);
    }

    SceneClock::Step st = g.clock.Advance(ready, paused, (std::isfinite(dt) && dt > 0.0f) ? static_cast<double>(dt) : 0.0, duration);
    const double still = REXCVAR_GET(mcla_scene_t);
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
    if (st.finished) {
        LARECOMP_APP_INFO("[scene] finished at t={:.2f}", g.clock.time());
        ReleaseAll("the scene's end");
        g.state = SceneState::kFinished;
    }
}

// ---------------------------------------------------------------- mcla_scene, read from two threads
//
// The cvar's string is written by whoever edits it (the F4 panel, the console: SetFlagByName, under the cvar registry's lock) and read
// by the game thread (TickScene) and by the thread that runs the camera hook (CurrentSceneState). A std::string read while it is
// assigned is a data race, so nothing here reads the cvar's own storage:
//   - the cvar's change callback (it runs on the writer's thread, under the registry's lock) copies every new value into g_want_text
//     under g_want_mutex and bumps a generation;
//   - a value that arrives with no write through SetFlagByName (the command line, larecomp.toml) is read once, by the cvar system's
//     own locked getter, the first time anyone asks (on a guest thread, long after the command line and the file were read);
//   - the game thread keeps a copy of its own and takes the lock only when the generation moved. With no scene and no edit a tick is
//     two atomic loads and an emptiness test: no lock, no allocation. CurrentSceneState reads one atomic.
std::mutex g_want_mutex;
std::string g_want_text;                 // guarded by g_want_mutex
bool g_want_known = false;               // guarded by g_want_mutex: g_want_text holds a value (from the callback or the first read)
std::atomic<uint32_t> g_want_gen{0};     // bumped under g_want_mutex with every new value
std::atomic<bool> g_want_set{false};     // the value is not empty
std::atomic<bool> g_want_seeded{false};  // the first read is done
std::string g_want_mine;                 // the game thread's own copy (TickScene only)
uint32_t g_want_mine_gen = 0;

void StoreWantLocked(std::string_view value) {
    g_want_text.assign(value);
    g_want_known = true;
    g_want_set.store(!value.empty(), std::memory_order_release);
    g_want_gen.fetch_add(1, std::memory_order_release);
}

void OnSceneCvarChanged(std::string_view, std::string_view value) {
    std::lock_guard<std::mutex> lock(g_want_mutex);
    StoreWantLocked(value);
}

// After the cvar's own definition at the top of the file, so the cvar exists when the callback is added.
[[maybe_unused]] const bool g_want_watching = (rex::cvar::RegisterChangeCallback("mcla_scene", OnSceneCvarChanged), true);

void EnsureWantKnown() {
    if (g_want_seeded.load(std::memory_order_acquire)) return;
    const std::string value = rex::cvar::GetFlagByName("mcla_scene");  // the cvar system's own locked read; none of our locks is held
    {
        std::lock_guard<std::mutex> lock(g_want_mutex);
        if (!g_want_known) StoreWantLocked(value);  // a callback that ran meanwhile holds a newer value
    }
    g_want_seeded.store(true, std::memory_order_release);
}

// The scene asked for, as of this update. The game thread only.
const std::string& WantedScene() {
    EnsureWantKnown();
    if (g_want_gen.load(std::memory_order_acquire) != g_want_mine_gen) {
        std::lock_guard<std::mutex> lock(g_want_mutex);
        g_want_mine = g_want_text;
        g_want_mine_gen = g_want_gen.load(std::memory_order_relaxed);
    }
    return g_want_mine;
}

}  // namespace

// ---------------------------------------------------------------- the public side

void TickScene() {
    statelog::Tick();  // the per-update state log (mcla_state_log or a scene's `statelog on`): the row is the state after the update before; one string test when off
    const std::string& want = WantedScene();
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
    if (!g.armed) {
        EnsureWantKnown();
        return g_want_set.load(std::memory_order_acquire) ? SceneState::kPending : SceneState::kOff;
    }
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
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET

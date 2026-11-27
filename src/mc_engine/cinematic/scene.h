#pragma once
//
// The scene file of the trailer toolkit (feature-parity/CINEMATIC-DESIGN.md section 7.1, work package M1a).
//
// A scene is a timeline in scene time (seconds): a text file of one statement a line that says what the world, the
// settings and, later, the camera, the HUD and the cars do and when. This file is the pure half: the parser, the
// timeline maths, the clock that turns game frames into scene time and the rule that decides when a changed file is
// loaded again. It knows nothing of the game, the SDK or the log, so scene_test.cpp builds it on its own.
// scene_runtime.cpp is the glue that arms a scene and applies it to the game.
//
// FORMAT. One statement a line; '#' starts a comment (at the start of a line or after a blank, outside "quotes");
// words are split on blanks, "double quotes" keep one word. Times are seconds (3, 3.5) or minutes:seconds (1:30.5).
// A statement is `at <time> <statement>`, or a bare statement, which is at time 0. `cam`, `cut` and `end` take their
// time as the first word instead of `at`.
//
//   scene <name> [fps <n>] [step <seconds or 1/n>]        header (the capture rate and the simulation step, as notes)
//   end <time>                                            the scene finishes there and hands everything back
//   statelog on [dir <folder>] | off                      the scene writes the per-update state log (state_log.h): an option of the
//                                                         whole scene, so no time; the CSV goes to <folder>\state.csv (default: the
//                                                         take folder when one is set, else statelogs\ next to the game)
//   world <key> <value> ...                               a bundle at time 0 of: hour <h> [hold], weather <w>,
//                                                         traffic|peds|parked|police on|off, peds|parked <scale>, timescale <x>
//   at T hour <h>                                         the world sits at that hour (0..24) until the next key
//   at T hour <a>..<b> over <s> [ease <e>]                the hour moves from a to b (a value over 24 wraps)
//   at T hour free                                        the game's own day cycle again
//   at T weather nice|cloudy|stormy|foggy|game            game = the game's own weather again
//   at T traffic|peds|parked|police on|off                the ambient switches (police: every cop active or not): the game's own script
//                                                         natives. traffic off ALSO holds the setting traffic_density_scale at 0 (new traffic
//                                                         stops whatever writes the enable byte; the native clears the road); on gives both back
//   at T peds|parked <scale>                              the density SETTING (ped_density_scale / parked_car_scale), a number from 0 to 2:
//                                                         held for the scene, the old value comes back when it ends (a 0 or 1 here is a scale,
//                                                         not off or on: the switches are the words on and off)
//   at T timescale <x> [over <s>] [ease <e>]              the game's time scale; a ramp starts from the value before it
//   at T timescale <a>..<b> over <s> [ease <e>]
//   at T set <cvar> <value...>                            a setting (the old value comes back when the scene ends); not one that names a
//                                                         program, a path or arguments, or writes a file (CvarRefusal below)
//   at T preset vanilla|enhanced|enhanced_rt|custom      live graphics preset until the next key or unload
//   at T preset_label on|off                              show the preset in the Director overlay
//   at T label "<text>" [side <s>] [fade <s>] [for <s>]   an on-screen label: DATA for the compositor, never drawn in game
//   at T mark <name>                                      a named instant, logged
//   at T capture fps <n> [step 1/<n>] [name <take>] [sink <s>] [statelog on|off]
//                                                         a take: from this key the game steps by exactly 1/n a second and every
//                                                         frame is read back (capture mode, M2a); one capture rate in a scene.
//                                                         name: the take's folder (letters, digits, _ - .; a taken name gets -2, -3 ...),
//                                                         sink: none, ffmpeg (FFV1 in mkv), png (one picture a frame) or raw; statelog on:
//                                                         the game's state for every frame in state.csv (the take folder, M2b)
//   at T capture off                                      the take ends (it also ends with the scene's `end`)
//   at T capture fps <n> | capture off                    a take (M2a, capture_core.h); the parser reads it (Track::kCapture)
//   at T split <x>                                        the before/after split (M5b) sits at x, 0..1 of the picture's width: the
//                                                         effects it cuts are applied left of it only; until the next key
//   at T split <a>..<b> over <s> [ease <e>]               the split moves from a to b
//   at T split off                                        no split: the effects are applied everywhere again
//   cam <T> <...>   cut <T>   at T hud <...>   at T car <id> <...>   start <...>
//                                                         the camera, HUD, car and preamble tracks (M1b, M1c, M4):
//                                                         kept as words for their packages, which register a TrackApplier
//                                                         (camera.h, hud_track.h, car_verbs.h read them); `start` has none yet
//
// SPOT WORDS. In a `cam`, `car` or `start` statement a word `spot:<name>[@right,up,forward[,turn]]` is a named place of the spots file
// (spots.h): the parser turns it into numbers before the statement's words are kept (x y z, or after `heading` or `yaw` the heading in
// degrees), so the packages that read the words see coordinates only. It needs the table: ParseOptions below.
//
// ease: linear (default), in, out, inout. A key's value holds until the next key of the same track.
//
// Everything a runner needs to know to run a scene reads as data here. A parse never throws: it returns the scene and
// a list of diagnostics, each with its line number; a scene with an error is not run.
//

#include "gpu/gfx_preset.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::cinematic {

// ---------------------------------------------------------------------------------------------------------------- diagnostics

enum class Severity : uint8_t { kWarning, kError };

struct Diagnostic {
    int line = 0;  // 1-based; 0 = the file as a whole
    Severity severity = Severity::kError;
    std::string message;
};

// "name:12: error: unknown weather 'sunny' (nice, cloudy, stormy, foggy or game)"; line 0 prints without a number.
std::string FormatDiagnostic(const Diagnostic& d, std::string_view file_name);

// ---------------------------------------------------------------------------------------------------------------- the data

enum class Ease : uint8_t { kLinear, kIn, kOut, kInOut };

// Where u in 0..1 is on the curve (clamped outside it). Both ends map to themselves.
double ApplyEase(Ease ease, double u);

enum class Track : uint8_t {
    // world: applied by the runtime (M1a)
    kHour,
    kWeather,
    kTraffic,
    kPeds,
    kParked,
    kPolice,
    kTimeScale,
    // events: cvars are applied, labels and marks are logged
    kCvar,
    kLabel,
    kMark,
    // the renderer's before/after split (M5b): the value is `to`, a ramp starts at `from`, `release` = off
    kSplit,
    // other packages: kept as words
    kCamera,  // `cam`  (M1b)
    kCut,     // `cut`  (M1b)
    kHud,     // `hud`  (M1c)
    kCar,     // `car`  (M4)
    kStart,   // `start`: the preamble (save, seed, warp, car slot)
    // capture mode (M2a)
    kPreset,
    kPresetLabel,
    kCapture,  // `capture fps <n>` (state 1, fps in `to`) and `capture off` (state 0)
};
const char* TrackName(Track track);

// Event::state of a peds or parked key that carries a density scale (Event::to) instead of on or off.
inline constexpr int kStateScale = 2;

struct Event {
    double t = 0.0;  // scene time of the key, seconds
    int line = 0;    // where it was written
    Track track = Track::kMark;

    // hour, timescale: the value is `to`; `from` is the start of a ramp when has_from (else a timescale ramp starts from
    // the value in force before the key). over = 0 is a jump.
    double from = 0.0;
    double to = 0.0;
    bool has_from = false;
    double over = 0.0;  // seconds: a ramp's length; a label's time on screen (0 = to the end)
    Ease ease = Ease::kLinear;
    bool release = false;  // hour free / weather game: the game's own again

    int index = -1;  // weather: 0 nice, 1 cloudy, 2 stormy, 3 foggy; capture fps: statelog 1 on, 0 off, -1 not given
    int state = 0;   // traffic, peds, parked, police: 1 on, 0 off; peds, parked: kStateScale = a density scale, in `to`

    std::string name;                // cvar name; mark name; car id; capture fps: the take's name
    std::string text;                // cvar value; label text; capture fps: the sink
    std::string capture_rects;       // capture fps: authored display-pixel rectangles
    std::string side;                // label side
    double fade = 0.0;               // label fade, seconds
    std::vector<std::string> words;  // camera, cut, hud, car, start: the words after the verb, in the file's spelling
};

struct Scene {
    std::string name;
    double fps = 0.0;   // 0 = not given
    double step = 0.0;  // seconds; 0 = not given
    double end = -1.0;  // the `end` time; -1 = the scene never finishes
    // The take (`capture fps` .. `capture off` or the end), worked out when the file is read: 0 = the scene makes none.
    double capture_fps = 0.0;
    double capture_from = 0.0;
    double capture_to = -1.0;  // -1 = to the scene's end, which it never reaches
    // What the `capture fps` key asked for besides the rate (M2b): "" = not given (the debug settings and the scene's name decide).
    std::string capture_name;
    std::string capture_sink;
    bool capture_state_log = false;
    std::string capture_rects;
    std::vector<Event> events;  // in time order; the file's order within one time

    // When the last key and everything it started (a ramp, a label) is over. 0 for no events.
    double LastKeyEnd() const;
    // The scene's length: `end`, or -1 when it has none.
    double Duration() const { return end; }

    // Options of the whole scene (written without a time): `statelog on [dir <folder>]`.
    bool statelog = false;
    std::string statelog_dir;  // empty = the default place

    // Where the text came from (ParseOptions::path): the file and its folder. Empty when the parser was not told. The folder is where a
    // route named by a camera rig (camera.h `rig flyby path <route>`) is looked for, beside the scene and in its `routes` folder.
    std::string source_path;
    std::string source_dir;
};

struct ParseResult {
    Scene scene;
    std::vector<Diagnostic> diagnostics;
    int ErrorCount() const;
    int WarningCount() const;
    bool Ok() const { return ErrorCount() == 0; }
};

class SpotTable;  // spots.h

// What a parse may know beyond the text. A scene file is the same text everywhere; the table of spots and the folder it lies in are the
// machine's (spots.h INTEGRATION says how the runtime fills this in).
struct ParseOptions {
    const SpotTable* spots = nullptr;  // `spot:<name>` words resolve against it; null: none is loaded, a spot word is an error
    std::string path;                  // the scene file's path (Scene::source_path); empty: not known
};

// Parses a whole file. Never throws; on an error the scene holds what was understood and Ok() is false.
ParseResult ParseScene(std::string_view text);
ParseResult ParseScene(std::string_view text, const ParseOptions& options);

// "12 events: hour 2, weather 1, set 3 ...; ends at 24.0 s" for the log.
std::string Summarize(const Scene& scene);

// Weather names, in the game's index order (time_of_day.cpp): nice cloudy stormy foggy.
int WeatherIndexFromName(std::string_view name);  // -1 when it is not one of the four

// What a scene may `set`. A scene file is data that gets sent around, so `set` must never be a way to start a program or to write
// a file somewhere. This returns the sentence to show when a scene may not set the setting `name` (the parser gives it as the line's
// error, the runner as its warning), and an empty string when it may; the runner still asks the cvar system whether the setting exists,
// is live and takes the value. Not settable by a scene (names compared without regard to case):
//   mcla_scene*          a scene does not arm another one
//   mcla_director*       nor drive the Director (its command channel and the folder its commands may write to: DIRECTOR-DESIGN.md 2.2)
//   mcla_capture_*       every capture setting: the take's program (ffmpeg), its arguments, its sink and its folder. A take is started
//                        with the scene's own `capture` key; where it goes is chosen on the command line
//   a setting that names a program, a path, a server or arguments (a name ending in _path, _dir, _folder, _file, _root, _exe, _args,
//                        _command, _csv ..., and a few others by name) or that makes the game write a file of its own (a name with
//                        "dump" in it, or ending in _probe, _diag, _trace, _census, _telemetry ..., and a few others by name)
// A new setting of that kind is refused by naming it so; one that does not follow the pattern goes into kRefusedCvars (scene.cpp).
// The same rule is in ports\scripts\scene_check.py (the lint says it first); both are checked against the same names (scene_test.cpp,
// scene_check.py selftest).
std::string CvarRefusal(std::string_view name);

// The time scale of a scene as the engine timer is told it (the timer hook in scene_runtime.cpp replaces the scale the timer
// reads, f13 after `lfs f13,84(r3)` in sub_821BDA90): the scene's value held to the range the parser takes (0..10);
// -1 when it is not a number, which means no override.
double TimeScaleForEngine(double scene_value);
// What the engine did with it: the frame's scaled step over its unscaled one (timer + 0x08 over + 0x58). 0 when there is no
// usable step (zero, negative, not a number).
double EngineStepRatio(double scaled, double unscaled);
// Whether that ratio is what the scene asked for, to the one frame a ramp lags and the engine's own floor under the scaled
// step (1e-4 s): within 0.05 + 10% of the asked value. A time scale that is not followed for a while is logged once.
bool TimeScaleFollowed(double want, double ratio);

// ---------------------------------------------------------------------------------------------------------------- the timeline

// What the world tracks say at one moment. Pure: a function of the scene and the time, so a still at any time (a
// scrub) and a running scene agree.
struct WorldState {
    bool hour_set = false;  // the scene pins the hour
    double hour = 0.0;      // 0..24 (a value of 24 or more wraps)
    int weather = -1;       // 0..3, -1 = the game's own
    int traffic = -1;       // -1 = not touched, 0 off, 1 on
    int peds = -1;          // the game's switch: -1 = not touched, 0 off, 1 on
    int parked = -1;
    int police = -1;
    double peds_scale = -1.0;    // the density setting (ped_density_scale / parked_car_scale): -1 = not touched, else 0..2; a switch key and a scale key
    double parked_scale = -1.0;  // of one track replace each other (the later key wins, and clears the other)
    bool timescale_set = false;
    double timescale = 1.0;
    bool operator==(const WorldState&) const = default;
};
WorldState EvaluateWorld(const Scene& scene, double t);

// Whether the scene wants a take at scene time `t` and at what rate: from the `capture fps` key to `capture off` (or the end),
// the end itself not included (the frame of the update that reaches it is the last one of the take). Pure, like EvaluateWorld.
struct CaptureWindow {
    bool on = false;
    double fps = 0.0;
};
CaptureWindow EvaluateCapture(const Scene& scene, double t);
// What the split track says at one moment: `set` false = no split (before the first key, or after `split off`); else the
// position, 0..1 of the picture's width (a ramp's value at t). Pure, like EvaluateWorld.
struct SplitState {
    bool set = false;
    double position = 0.0;
    bool operator==(const SplitState&) const = default;
};
SplitState EvaluateSplit(const Scene& scene, double t);

struct PresetTrackState {
    std::optional<mcla::gpu::gfx_preset::Preset> preset;
    bool label = false;
    int line = 0;
};
PresetTrackState EvaluatePreset(const Scene& scene, double t);

// Walks the events once in time order: Take() hands over every event whose time has come (<= t) and was not given before.
// Reset() to start again from the first. The runtime acts on the discrete ones (cvar, label, mark, and the other
// packages' tracks) as they come; the world tracks it reads through EvaluateWorld.
class EventCursor {
  public:
    void Reset() { next_ = 0; }
    size_t Taken() const { return next_; }
    void Take(const Scene& scene, double t, std::vector<const Event*>* out);

  private:
    size_t next_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------- the clock

// Turns game frames into scene time. Armed, it waits until the world is ready (a car on the road, not paused) for
// `settle_ticks` frames in a row, then starts at 0. While running it adds each frame's unscaled game delta (never more than
// max_dt: a hitch does not skip the scene), holds still while the game is paused or the world is gone (a loading screen),
// and finishes when the scene's length is reached (a scene with no `end` never finishes).
class SceneClock {
  public:
    enum class State : uint8_t { kIdle, kWaiting, kRunning, kFinished };
    struct Config {
        int settle_ticks = 30;
        double max_dt = 0.25;
    };
    struct Step {
        State state = State::kIdle;
        double t = 0.0;
        bool started = false;   // this frame the clock went from waiting to running
        bool finished = false;  // this frame it reached the end
        bool advanced = false;  // the time moved
    };

    SceneClock() = default;
    explicit SceneClock(Config config) : config_(config) {}

    void Arm();      // start waiting for the world; time 0 when it is ready
    void Disarm();   // idle
    void Restart();  // running at time 0 at once (a reload while the world is already there)
    // `duration` < 0: no end.
    Step Advance(bool world_ready, bool paused, double dt, double duration);
    // The clock is this time from now on (a still, a scrub): only while running or finished (a finished scene runs again).
    void SetTime(double t, double duration);

    State state() const { return state_; }
    double time() const { return t_; }

  private:
    Config config_;
    State state_ = State::kIdle;
    double t_ = 0.0;
    int ready_ticks_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------- hot reload

// What the file looked like when it was last looked at.
struct FileStamp {
    bool exists = false;
    int64_t mtime = 0;  // any unit, as long as it is the same every time
    uint64_t size = 0;
    bool operator==(const FileStamp&) const = default;
};

// Decides when to read the scene file: the first time it exists, and again after its time stamp or size changed and then
// stayed the same for settle_ms (an editor writes a file in two steps; a half-written file is not read). A file that was
// read, good or bad, is not read again until its stamp changes. The caller polls (a few times a second is enough),
// reads when told to, and says which stamp it read.
class ReloadWatcher {
  public:
    enum class Action : uint8_t { kNone, kLoad, kMissing };

    ReloadWatcher() = default;
    explicit ReloadWatcher(int64_t settle_ms) : settle_ms_(settle_ms) {}

    void Reset();  // another file: forget everything
    Action Poll(const FileStamp& now, int64_t now_ms);
    void Loaded(const FileStamp& stamp);  // the stamp of the file as it was when it was read (parsed or not)

  private:
    int64_t settle_ms_ = 300;
    bool have_loaded_ = false;
    FileStamp loaded_;
    bool have_pending_ = false;
    FileStamp pending_;
    int64_t pending_since_ = 0;
    bool missing_reported_ = false;
};

}  // namespace mc::cinematic

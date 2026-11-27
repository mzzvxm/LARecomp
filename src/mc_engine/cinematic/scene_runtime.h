#pragma once
//
// The scene runner (trailer toolkit, work package M1a): arms a scene file, keeps it loaded (hot reload by time stamp), owns
// scene time, and applies the world tracks to the game: hour and weather through the time-of-day hooks, the ambient
// switches through the game's own script natives, the time scale through the engine timer, settings through the cvars.
// The parser, the timeline and the clock are in scene.h; this file is the glue.
//
// Nothing runs unless a scene is armed. A scene is armed by the cvar mcla_scene (a path; empty = off) or, in the test
// harness, by the tour command `scene <file>` that sets it. The camera, HUD and car tracks have no applier here: the packages
// that own them register one (RegisterTrackApplier) and get the scene and its time every update.
//

#include <cstdint>
#include <string>
#include <vector>

#include "scene.h"

namespace mc::cinematic {

// Once per game update on the game thread (Patch_DeltaTimePre). One string test when no scene is armed.
void TickScene();

// Where the runner is. kPending: armed, the file not read yet. kWaiting: read, waiting for a car on the road. kRunning:
// scene time moves. kFinished: reached its `end` and handed everything back. kFailed: the file is missing or has an error and
// no earlier version is loaded.
enum class SceneState : uint8_t { kOff, kPending, kWaiting, kRunning, kFinished, kFailed };
SceneState CurrentSceneState();
const char* SceneStateName(SceneState state);

// True when a scene was armed and has finished or failed: what a tour's `scene_wait` waits for.
bool SceneDone();

// Scene time in seconds while running (the last time it reached when finished), else 0.
double SceneTime();

// The armed scene's file, as it was looked up (a full path when one was found), and its name from the `scene` line; empty when none is armed or
// the file has not been read. A take copies the file into its folder (capture_mode.cpp, M2b). A file a scene names (a car's route) is looked for
// beside it. Game thread only (the appliers' begin / apply / end).
std::string SceneFilePath();
std::string SceneName();

// The game's state as the take's state log writes it (take_folder.h StateRow): the engine timer's two deltas and its time scale word, the
// day cycle's hour, the pause flag, and the player's car (the live matrix, 16 floats, read the way map_warp.cpp reads it). Game thread only,
// between updates (the hand-over of a frame). False when the runtime is not up; a field it cannot read stays 0.
struct GameStateSample {
    float hour = 0.0f;
    float time_scale = 0.0f;
    float dt = 0.0f;           // seconds: this update's step after the time scale
    float dt_unscaled = 0.0f;  // before it
    bool paused = false;
    bool world_ready = false;  // a car on the road and the ambient manager built
    uint32_t player = 0;       // the player's racer (guest address), 0 = none
    bool has_matrix = false;
    float matrix[16] = {};     // the player car's live matrix: right, up, back, position rows
};
bool SampleGameState(GameStateSample* out);

// What the other packages' tracks implement. The runner calls begin when a scene starts (and again when a reload restarts it),
// apply with the scene time on every game update while it runs, and end when it finishes, is replaced or is turned off:
// whatever the applier changed in the game goes back there. `tracks` lists the tracks it takes over (a bit per Track), so the
// log can say which keys of a scene nobody applies.
struct TrackApplier {
    const char* name;  // for the log: "camera", "hud", "cars"
    uint32_t tracks;   // 1u << static_cast<int>(Track::kCamera) | ...
    void (*begin)(const Scene& scene);
    void (*apply)(const Scene& scene, double t);
    void (*end)();
    // Optional (the Director's soft reload, feature-parity/DIRECTOR-DESIGN.md 2.5): the scene file changed while the scene runs at time `t`; the clock and the
    // game are kept. Re-read the keys from `scene` and keep what is running (the cars make again only a car whose statement changed). An applier with none is
    // ended and begun again with the new scene, as a restart did.
    void (*reload)(const Scene& scene, double t) = nullptr;
};
constexpr uint32_t TrackBit(Track t) {
    return 1u << static_cast<int>(t);
}
// Up to eight (camera, capture, cars and hud use four); the appliers are not owned (give it something that lives as long as the game).
bool RegisterTrackApplier(const TrackApplier* applier);

// ---------------------------------------------------------------------------------------------------------------- the Director's handles
//
// What src/mc_engine/cinematic/director uses to run the scene as a transport (DIRECTOR-DESIGN.md 2.4 and 2.5). Game thread only (Director::Tick runs after
// TickScene), and nothing here changes anything until the Director calls it.

// The parsed scene that runs, or null when none is armed or the file was never read. Valid until the next TickScene.
const Scene* LoadedScene();
// The named places the scene resolves `spot:<name>` words against (spots.h; the files beside the scene as of its last read), or null when no scene is armed.
// For the Director's snapshot and its query.spots. Valid until the next TickScene.
const SpotTable* SceneSpotTable();

struct SceneTransport {
    SceneState state = SceneState::kOff;
    double t = 0.0;                  // scene time
    double duration = -1.0;          // the scene's `end`, -1 = none
    bool held = false;               // the Director holds the clock (pause or seek)
    bool frozen = false;             // ... and the world at the engine's time scale floor
    int pending_steps = 0;           // updates left of a step
    bool pinned = false;             // a seek keeps the clock at its time
    bool holding_for_ready = false;  // before t = 0 the clock waits for the cars and the streaming (SetSceneReadyGate)
    uint64_t updates = 0;            // updates run with a scene loaded
};
SceneTransport GetSceneTransport();

// Pause (true): the clock holds and the world is frozen. Play (false): both are released and a seek's pin is dropped.
void SceneHold(bool hold);
// Pins the clock at `t` (0 to the scene's end), held and frozen: exact for the camera, kinematic cars, the hour and the other tracks that are functions of the
// time. A scene that has ended is run again first (its cars are made again).
void SceneSeekTo(double t);
// While held: lets this many updates run (the clock moves by their steps, the world runs), then holds again. Exact steps need a fixed step (mcla_fixed_step_fps).
void SceneStepUpdates(int updates);
// Runs the scene again from the start (the cars are made again) and, for t > 0, seeks there.
void SceneRestartAt(double t);

// A live Director override owned by the scene, with its value and save provenance restored on unload.
bool SceneSetLiveSetting(const std::string& name, const std::string& value);

// The scene is this text now (an edit by the Director, an undo): parsed, and, when a scene runs and the Director asked for it (SetSceneSoftReload), loaded in
// place. A text with an error changes nothing.
struct SceneTextResult {
    bool ok = false;
    bool soft = false;
    int errors = 0;
    std::vector<Diagnostic> diagnostics;
};
SceneTextResult SceneLoadText(const std::string& text, const char* by);
// Loaded in place (true) or restarted (false) when the file changes while the scene runs. The Director turns it on while it runs.
void SetSceneSoftReload(bool on);

// A live preview of the world over the timeline: the fields that are set (hour_set, weather >= 0, traffic >= 0, ...) replace what the timeline says.
void SceneSetWorldOverride(const WorldState& w);
void SceneClearWorldOverride();
WorldState SceneWorldOverride(bool* on);

// What the Director hears of the runner: a scene text was loaded (from the file or from SceneLoadText; `by` says which: "file", "agent", ...; soft: in
// place) or was refused (ok false), and a key came due.
struct SceneObserver {
    void (*loaded)(const std::string& path, const std::string& text, bool ok, int errors, bool soft, const char* by);
    void (*key)(const Event& event, double t);
};
void SetSceneObserver(const SceneObserver* observer);
// Before t = 0 the clock holds until this says true (the cars are made, the streaming is done), for at most 30 s. Null: no gate.
void SetSceneReadyGate(bool (*gate)());

}  // namespace mc::cinematic

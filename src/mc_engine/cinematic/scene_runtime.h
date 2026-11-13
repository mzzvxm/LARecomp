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
// the file has not been read. A take copies the file into its folder (capture_mode.cpp, M2b).
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
};
constexpr uint32_t TrackBit(Track t) {
    return 1u << static_cast<int>(t);
}
// Up to four; the appliers are not owned (give it something that lives as long as the game).
bool RegisterTrackApplier(const TrackApplier* applier);

}  // namespace mc::cinematic

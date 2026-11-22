#pragma once
//
// The per-update state log of the trailer toolkit (feature-parity/CINEMATIC-PLAN.md, work package M3a): the glue between the game
// and state_log_core.h. One CSV row per game update with the player's and the scene's cars, the camera block, the hour and weather,
// the clocks, the scene's clock and the random number state: the evidence that two takes of a scene matched, and the first row and
// column where they did not (ports/scripts/take_diff.py reads two of them).
//
// Two ways to ask for it, both debug-only:
//   * the setting mcla_state_log: a .csv file, or a folder that gets state.csv. Live: set it and rows start with the next update
//     that has a player car, clear it and the log is closed. It outranks the scene's own request.
//   * the scene option `statelog on [dir <folder>]` (scene.h): the log is opened when the scene starts (again on a restart: one file
//     a take), closed when it ends. The file goes to <folder>/state.csv, else the take folder (SetTakeFolder), else
//     statelogs/<scene>-<date>-<time>.csv next to the game. A folder that already holds state.csv gets state-2.csv, state-3.csv.
//
// Rows are only written while the player has a car (no menus, no loading screens). A row is made on the game thread (a few
// guest reads and one formatted line) and handed to a writer thread through a bounded queue: a stalled disk costs rows, which the
// next row reports in its `lost` column, never a wait. Nothing is read or written while no log is open: one string test an update.
//

#include <cstddef>
#include <cstdint>
#include <string>

#include "scene.h"

namespace mc::cinematic::statelog {

// Once per game update on the game thread (Patch_DeltaTimePre), before the scene runner's own step: the row is the state after the
// update before it.
void Tick();

// The scene runner calls these. SceneBegin when a scene starts (and again when a reload or a restart starts it over), SceneEnd when it
// finishes, is replaced or is turned off. NoteMark when a `mark` key comes due: the next row carries its name.
void SceneBegin(const Scene& scene);
void SceneEnd();
void NoteMark(const std::string& name);

// The folder of the take being written (M2b): where a scene's `statelog on` goes when it names no folder. Empty: the default.
void SetTakeFolder(const std::string& folder);

// The cars of the scene, by the id the scene gave them (M4's registry: car_registry.h, cars.h). Registered once; read on the game
// thread once an update while a log is open. `count` = the cars known now, the player not among them; `get` fills the id and the
// matrix (right, up, back, position, 16 floats: the entity's world matrix) of car `index` and says whether it has a pose this update.
struct CarSource {
    size_t (*count)();
    bool (*get)(size_t index, char* id, size_t id_size, float matrix[16]);
};
bool RegisterCarSource(const CarSource* source);

// A log is open.
bool Active();
// The game is ending: writes the rows that are still queued and the `#end` line and closes the file. The fast exit (exit_fast.h) runs no
// destructor, and the writer's destructor was what closed a log that was still open at the end.
void CloseForExit();
// The file of the log that is open, or empty.
std::string CurrentLogPath();

}  // namespace mc::cinematic::statelog

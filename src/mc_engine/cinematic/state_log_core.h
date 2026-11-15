#pragma once
//
// The per-update state log of the trailer toolkit (feature-parity/CINEMATIC-PLAN.md, work package M3a): the pure half.
//
// One CSV row per game update with what a second take of the same scene has to agree on: the player's car and the scene's cars
// (the entity's world matrix and the player's speed), the camera block the game publishes, the hour and weather, the game's
// clocks, the scene's own clock and the state of the random number generator. take_diff.py (ports/scripts) reads two of these
// and names the first row, column and values where they differ, with a tolerance per column.
//
// This file knows nothing of the game, the SDK or the log: the format of a row, the hashes, where the file goes, the bounded
// queue between the game thread and the writer thread, and the writer. state_log_test.cpp builds it on its own.
// state_log.cpp is the glue that reads the game and decides when to write.
//
// THE ROW. Row k is written at the start of update k (Patch_DeltaTimePre, after the engine timer ran), so it holds the state AFTER
// update k-1 and the step update k is about to take. Columns, in the file's order (one function names and fills them, so the
// header and the rows cannot drift apart):
//
//   tick mark wall_ms lost    the update number since the log opened (rows skipped for want of a player car still count, so a
//                             gap shows), the scene's mark that came due since the last row, the wall time, and the rows the
//                             writer could not take since the last row (the queue was full)
//   scene_state scene_t       the scene runner's state (0 off, 1 pending, 2 waiting, 3 running, 4 finished, 5 failed) and its
//                             clock in seconds (-1 when there is no scene)
//   dt dt_unscaled time_scale frame_ctr clock_total clock_game mode_timer
//                             the engine timer: the step the game takes (after the time scale), before it, the scale, the
//                             engine's own update counter, the two float clocks, the seconds since the mode loaded
//   rng hour weather paused live
//                             the generator behind RAND_* (0x82860890), the hour (-1: not up), the weather index, paused,
//                             free roam live
//   p_ok p_m0..p_m14 p_speed  the player's racer: the live world matrix, rows right, up, back, position (the fourth word of each
//                             row is padding, uninitialised memory in the game, and is not logged: p_m3, p_m7, p_m11, p_m15 do
//                             not exist), and its speed in m/s
//   cam_ok cam_m0..cam_m14    the camera block the game publishes after its camera tick, the same twelve words
//   n_cars c0_id c0_ok c0_m0..c0_m14 ... c5_*
//                             the scene's cars (not the player), in the order the car source lists them (the order the scene
//                             spawned them), each with its scene id, whether it has a pose and the same twelve words of its matrix
//   h_player h_cars h_world h_cam hash chain
//                             FNV-1a of the exact bits of each group's values, `hash` over the four of them (it leaves the
//                             clocks, the generator and the wall time out, so two takes that start a few updates apart still
//                             match row for row at a shift), and `chain`: a running hash of every `hash` so far (a whole take
//                             in one value). The hashes are bit-exact; take_diff.py's tolerances do not use them.
//
// Floats are written with 9 significant digits, which reads back as the same float. The file starts with comment lines (`#`), the
// column line comes next, and a log that was closed properly ends with `#end rows=N written=N dropped=N`.
//

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mc::cinematic::statelog {

constexpr int kMaxCars = 6;     // scene cars in the row (the player's car has its own columns)
constexpr int kIdChars = 23;    // the longest car id kept
constexpr int kMatrixWords = 16;

// The twelve words of a matrix that mean something (rows right, up, back, position without their fourth words).
constexpr int kPoseWords = 12;
// Index into the 16-float matrix of pose word k (0..11): 0,1,2, 4,5,6, 8,9,10, 12,13,14.
constexpr int PoseWordIndex(int k) {
    return k + k / 3;
}

struct CarSample {
    bool ok = false;
    char id[kIdChars + 1] = {};
    float m[kMatrixWords] = {};  // right, up, back, position (4 floats each)
};

struct Sample {
    uint64_t tick = 0;
    std::string mark;
    double wall_ms = 0.0;
    uint32_t lost = 0;
    // the scene runner
    int scene_state = 0;
    double scene_t = -1.0;
    // the clock
    float dt = 0, dt_unscaled = 0, time_scale = 0, clock_total = 0, clock_game = 0, mode_timer = 0;
    uint32_t frame_ctr = 0;
    // the world
    uint32_t rng = 0;
    float hour = 0;
    int32_t weather = 0;  // -1: not up
    uint32_t paused = 0, live = 0;
    // the player
    bool p_ok = false;
    float p_m[kMatrixWords] = {};
    float p_speed = 0;
    // the camera
    bool cam_ok = false;
    float cam[kMatrixWords] = {};
    // the scene's cars
    int n_cars = 0;
    CarSample car[kMaxCars];
};

// The running part of the hashes: the chain carries over from row to row.
struct Chain {
    uint64_t value = 0;
};

constexpr const char* kFormatVersion = "v2";

// The column names in the file's order.
std::vector<std::string> ColumnNames();
// "tick,mark,..." (no newline).
std::string ColumnLine();
// One row, with its newline. `chain` is advanced by this row's `hash`.
std::string RowText(const Sample& sample, Chain* chain);

// The first comment line: what the log is of. `fixed_step_fps` 0 = real time or unknown.
std::string PreambleLine(const std::string& scene_name, double fixed_step_fps, const std::string& started);
// The last line of a log that was closed properly.
std::string TrailerLine(uint64_t ticks, uint64_t written, uint64_t dropped);

// A string safe for one CSV field: a comma, a quote, a line break or any other control character becomes ';' (a mark's or a car id's).
std::string CleanField(const std::string& text, size_t max_chars);

// ------------------------------------------------------------------------------------------------------------ where the file goes

// `path` as a CSV file when it ends in .csv (any case), else as a folder that holds state.csv.
std::string CsvPathFor(const std::string& path);

// Which file a log is written to, from the first of these that is set:
//   cvar_value  mcla_state_log: a .csv file, or a folder (state.csv in it)
//   scene_dir   the scene's `statelog on dir <folder>`
//   take_dir    the folder of the take being written (SetTakeFolder: M2b's take folder)
//   else        statelogs/<scene name>-<stamp>.csv next to the game (stamp = "20261113-142530")
std::string ResolveLogPath(const std::string& cvar_value, const std::string& scene_dir, const std::string& take_dir,
                           const std::string& scene_name, const std::string& stamp);

// A file name part with only letters, digits, '_', '-' and '.' ("my scene" -> "my_scene"); "scene" when nothing is left.
std::string SafeFileStem(const std::string& name);

// `path` when nothing is there, else state-2.csv, state-3.csv ... (the stem and extension kept) up to 999; `exists` says whether a
// file is there. Used for the folder cases, so a take again in the same folder does not overwrite the take before it.
std::string FirstFreePath(const std::string& path, const std::function<bool(const std::string&)>& exists);

// ------------------------------------------------------------------------------------------------------------ queue and writer

// The text of the rows waiting for the writer, with a hard cap: a row that would go over it is refused (and counted), so a disk
// that stalls costs rows, never memory and never a wait on the game thread.
class RowQueue {
  public:
    explicit RowQueue(size_t max_bytes) : max_bytes_(max_bytes) {}
    // False when the row does not fit (it is dropped and counted). A row larger than the cap never fits.
    bool Push(std::string row);
    // Forget the pending rows and the counts (a new log).
    void Reset();
    // Everything pending, in order, appended to *out. Returns the bytes moved.
    size_t PopAll(std::string* out);
    size_t pending_bytes() const;
    uint64_t pushed() const;
    uint64_t dropped() const;
    size_t max_bytes() const { return max_bytes_; }

  private:
    const size_t max_bytes_;
    mutable std::mutex mutex_;
    std::string pending_;
    uint64_t pushed_ = 0;
    uint64_t dropped_ = 0;
};

// The file and its thread. Open() writes the head (the comment lines and the column line) at once, Push() hands a row to the
// thread (never blocks on the disk), Close() writes what is left and the trailer and joins the thread.
class LogWriter {
  public:
    explicit LogWriter(size_t max_pending_bytes = 32u << 20, int flush_ms = 100);
    ~LogWriter();
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    // Creates the folders and the file (UTF-8 path) and starts the thread. False when the file cannot be made.
    bool Open(const std::string& path, const std::string& head);
    bool IsOpen() const { return open_.load(std::memory_order_acquire); }
    // The game thread. False when the queue was full (or the log is not open).
    bool Push(std::string row);
    // Flushes everything, writes `#end ...` (rows, written and dropped counts) and closes. `ticks` = the updates that passed while it was open.
    void Close(uint64_t ticks);

    uint64_t written() const { return queue_.pushed(); }  // rows the queue took
    uint64_t dropped() const { return queue_.dropped(); } // rows it refused
    const std::string& path() const { return path_; }

  private:
    void Run();
    void Flush();

    RowQueue queue_;
    const int flush_ms_;
    std::string path_;
    std::atomic<bool> open_{false};
    std::ofstream file_;
    std::mutex file_mutex_;   // one flush at a time: they must write in the order they took the rows
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace mc::cinematic::statelog

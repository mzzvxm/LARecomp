#pragma once
//
// The take folder of the trailer toolkit (work package M2b), the pure half: the names of a take and of its files, the memory a take is
// allowed to hold, the slot pool of the host ring, take.json, the state log's rows, and the small helpers they need (a JSON writer, a
// UTC time stamp, an FNV hash). It knows nothing of the game, the SDK, the renderer or the log, so take_folder_test.cpp builds it on its
// own. capture_mode.cpp is the glue that makes the folder and writes the files; capture_core.h has the step, the gate and the take's books.
//
// A TAKE FOLDER holds everything that says what a take is, so that it can be kept with its scene and compared with another take:
//
//   take.json     what the take is: the scene, the exe, the cvars, the picture size, the rate, the sink, the frame count, the update map,
//                 the dropped and repeated counts (0 for a take that did what it should), the memory it held, how it ended. Written when
//                 the take starts (state "running": a crash leaves it) and again when it ends.
//   video.mkv     the frames as one FFV1 file (sink ffmpeg), or
//   frames/       one PNG for every frame, f000000.png ... (sink png), or
//   video.bgra    raw B8G8R8A8 frames, rows packed, width x height x 4 bytes a frame (sink raw); with sink none there is no picture
//   frames.csv    one row for every frame: the update it shows, its scene time and what it cost
//   state.csv     only when the scene asks (`statelog on`): one row for every frame of the game's own state, the evidence that two takes match
//   scene.scene   a copy of the scene file the take ran
//   spots.txt     only when the scene names places (`spot:<name>`): the spots files it read (spots.h), so the copy resolves them
//   ffmpeg.log    ffmpeg's own output (sinks ffmpeg and png)
//
// A take never overwrites another: a name that is taken gets -2, -3 ... after it.
//
// THE MEMORY A TAKE HOLDS is bounded by construction: the GPU readback buffers and the host ring are made once, at the picture's size,
// from a plan (PlanMemory) that has to fit a hard cap and the free memory of the PC; nothing grows with the length of the take or the
// speed of the writer. A writer that is slower than the render makes the ring full, the render thread waits for a free slot, and the
// frame gate holds the game's next update; a take that cannot fit, or whose writer stops, ends with an error instead of buffering.
//

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mc::cinematic::capture {

// ---------------------------------------------------------------------------------------------------------------- names

inline constexpr size_t kMaxTakeNameLength = 64;

// A name safe as a folder name on every system: ASCII letters, digits, `_`, `.` and `-`; anything else (blanks, quotes, slashes,
// non-ASCII bytes) becomes one `-`; no leading or trailing `.` or `-`; at most 64 characters; the names Windows keeps for devices
// (con, nul, com1 ...) get a `_` in front. `fallback` is used when nothing is left, then "take".
std::string SanitizeTakeName(std::string_view text, std::string_view fallback);

// `base`, else `base-2`, `base-3` ... the first that `taken` does not claim; empty when the first 999 are all taken.
std::string UniqueName(const std::string& base, const std::function<bool(const std::string&)>& taken);

// `a` and `b` with one separator between, the kind `a` already uses ('\\' when it has one and no '/', else '/').
std::string JoinPath(const std::string& a, const std::string& b);

// ---------------------------------------------------------------------------------------------------------------- the files

struct TakeLayout {
    std::string folder;
    std::string take_json;
    std::string frames_csv;
    std::string state_csv;
    std::string scene_copy;
    std::string ffmpeg_log;
    std::string video_mkv;
    std::string video_raw;
    std::string audio_wav;  // the take's sound (capture_audio.h): audio.wav, 32 bit float, 48 kHz
    std::string audio_csv;  // a row for every video frame of it: audio.csv
    std::string frames_dir;
    std::string frame_pattern;  // for ffmpeg: <frames_dir>/f%06d.png
};
TakeLayout MakeLayout(const std::string& folder);
// The file names inside the folder, for take.json (relative, with '/').
inline constexpr const char* kTakeJsonName = "take.json";
inline constexpr const char* kFramesCsvName = "frames.csv";
inline constexpr const char* kStateCsvName = "state.csv";
inline constexpr const char* kSceneCopyName = "scene.scene";
// The spots files the scene reads (spots.h), one after the other: written beside the copy when the scene has a `spot:` word, so the copy resolves the same places.
inline constexpr const char* kSpotsCopyName = "spots.txt";
inline constexpr const char* kFfmpegLogName = "ffmpeg.log";
inline constexpr const char* kVideoMkvName = "video.mkv";
inline constexpr const char* kAudioWavName = "audio.wav";
inline constexpr const char* kAudioCsvName = "audio.csv";
inline constexpr const char* kVideoRawName = "video.bgra";
inline constexpr const char* kFramesDirName = "frames";
inline constexpr const char* kFramePatternName = "frames/f%06d.png";
// "f000000.png" for frame 0 (six digits at least).
std::string FrameFileName(uint64_t frame);

// ---------------------------------------------------------------------------------------------------------------- memory

struct MemoryRequest {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t gpu_max = 4;      // GPU readback buffers (renderer: frames in flight + 2); one frame each, rows padded to 256 bytes
    uint32_t gpu_min = 2;      // the fewest that still work (2 in lock-step)
    uint32_t ring_wanted = 4;  // host buffers between the readback and the writer, as asked (mcla_capture_ring)
    uint32_t ring_min = 2;
    uint64_t cap_bytes = 0;        // the hard cap on all of them together; 0 = none
    uint64_t available_bytes = 0;  // free physical memory now; 0 = not known
    uint64_t reserve_bytes = 0;    // that much has to stay free
    uint64_t fixed_bytes = 0;      // other memory the take holds for sure (the pipe to ffmpeg), counted in the total
};
struct MemoryPlan {
    bool ok = false;
    uint32_t gpu_buffers = 0;
    uint32_t ring = 0;
    uint64_t gpu_frame_bytes = 0;   // one GPU readback buffer: row pitch x height
    uint64_t host_frame_bytes = 0;  // one host buffer: width x height x 4
    uint64_t total_bytes = 0;       // gpu_buffers x gpu + ring x host + fixed
    uint64_t limit_bytes = 0;       // what the plan had to fit (0 = no limit)
    std::string text;               // what was reduced, or why nothing fits
};
// The most buffers (ring first, then the GPU's, never below the minimums) that fit the cap and the free memory. A frame that does
// not fit even at the minimums is not ok, and `text` says which limit stopped it and what to change.
MemoryPlan PlanMemory(const MemoryRequest& request);
// Free memory fell to `available` while `floor` (0 = no floor) must stay free: the take is to stop. `available` 0 = not known.
bool MemoryFloorHit(uint64_t available_bytes, uint64_t floor_bytes);
// "253.1 MiB"
std::string FormatMib(uint64_t bytes);

// The slots of the host ring: a fixed number of buffers, each in use or free. The sink takes one for a frame and gives it back when the
// writer has written it. There is no way to get more than `capacity`, which is the point: the render thread waits instead.
class SlotPool {
  public:
    explicit SlotPool(uint32_t capacity = 0) { Reset(capacity); }
    void Reset(uint32_t capacity);
    // A free slot, or false when all are in use.
    bool TryAcquire(uint32_t* slot);
    // Gives a slot back; false when it was not in use (a slot given back twice, or one that never was).
    bool Release(uint32_t slot);
    uint32_t capacity() const { return static_cast<uint32_t>(in_use_.size()); }
    uint32_t in_use() const { return in_use_count_; }
    uint32_t free_slots() const { return capacity() - in_use_count_; }
    uint32_t max_in_use() const { return max_in_use_; }

  private:
    std::vector<uint8_t> in_use_;
    uint32_t in_use_count_ = 0;
    uint32_t max_in_use_ = 0;
};

// The host ring: `slots` buffers of one frame, a writer thread, and nothing else. The render thread takes a free buffer (Acquire), fills it and
// hands it over (Submit); the writer thread calls `write` for every handed-over buffer in order, then the buffer is free again. When every
// buffer is queued or being written, Acquire waits: that wait is the back-pressure, the render thread (and with it the frame gate and the
// game) stands still, and no frame is held anywhere else. A writer that finishes nothing for `timeout_ms` is stuck: Acquire gives up (and says so)
// instead of waiting for ever, and the caller ends the take with an error.
class HostRing {
  public:
    // The writer's job for the frame handed over as `row`: false when it could not write it (the ring goes on draining: frames that cannot be
    // written are counted by the caller, never kept). Runs on the writer thread.
    using WriteFn = std::function<bool(uint64_t row, const uint8_t* data, uint64_t bytes)>;
    struct Slot {
        uint8_t* data = nullptr;  // nullptr: no buffer (closed, or stuck)
        uint32_t index = 0;
        uint64_t bytes = 0;
    };

    HostRing() = default;
    ~HostRing() { Close(); }
    HostRing(const HostRing&) = delete;
    HostRing& operator=(const HostRing&) = delete;

    // Allocates the buffers (never throws: false and nothing allocated when the memory is not there) and starts the writer.
    bool Open(uint32_t slots, uint64_t frame_bytes, WriteFn write);
    bool is_open() const;
    // Render thread. Waits for a free buffer; `waited_ms` is how long it waited; `stuck` is set when `timeout_ms` passed with no write
    // finishing (and the return has no data).
    Slot Acquire(int timeout_ms, double* waited_ms, bool* stuck);
    // The buffer holds frame `row`.
    void Submit(const Slot& slot, uint64_t row);
    // The buffer is not needed (the frame was lost).
    void Cancel(const Slot& slot);
    // The writer writes everything handed over, then stops; the buffers are freed. Safe to call twice.
    void Close();

    uint32_t slots() const { return slots_; }
    uint32_t queued() const;      // handed over and not written yet
    uint32_t max_in_use() const;  // the most buffers that were in use at once
    uint32_t max_queued() const;  // the most that waited for the writer
    uint64_t written() const { return written_.load(std::memory_order_relaxed); }
    uint64_t write_failures() const { return failures_.load(std::memory_order_relaxed); }
    // The row the first failed write was for (valid when write_failures() > 0).
    uint64_t first_failed_row() const { return first_failed_row_.load(std::memory_order_relaxed); }
    // Counts every finished write (and every freed slot): a wait that sees it move is not stuck.
    uint64_t progress() const { return progress_.load(std::memory_order_relaxed); }

  private:
    struct Item {
        uint32_t slot;
        uint64_t row;
    };
    void WriterMain();

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::condition_variable free_;
    bool open_ = false;
    bool stop_ = false;
    uint32_t slots_ = 0;
    uint64_t frame_bytes_ = 0;
    std::vector<std::unique_ptr<uint8_t[]>> buffers_;
    SlotPool pool_;
    std::deque<Item> queue_;
    uint32_t max_queued_ = 0;
    std::thread writer_;
    WriteFn write_;
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> failures_{0};
    std::atomic<uint64_t> first_failed_row_{0};
    std::atomic<uint64_t> progress_{0};
};

// ---------------------------------------------------------------------------------------------------------------- JSON

std::string JsonEscape(std::string_view text);

// Writes indented JSON in the order it is told. Misuse (a value with no key inside an object ...) is the caller's bug and is not checked;
// the test reads the output back.
class JsonWriter {
  public:
    void BeginObject();
    void EndObject();
    void BeginArray();
    void EndArray();
    void Key(std::string_view name);
    void String(std::string_view value);
    void Int(int64_t value);
    void UInt(uint64_t value);
    void Double(double value);  // ten significant digits; not finite = null
    void Bool(bool value);
    void Null();
    // Key + value in one.
    void Field(std::string_view name, std::string_view value) { Key(name); String(value); }
    void Field(std::string_view name, const char* value) { Key(name); String(value); }
    void Field(std::string_view name, int64_t value) { Key(name); Int(value); }
    void Field(std::string_view name, int value) { Key(name); Int(value); }
    void Field(std::string_view name, uint64_t value) { Key(name); UInt(value); }
    void Field(std::string_view name, uint32_t value) { Key(name); UInt(value); }
    void Field(std::string_view name, double value) { Key(name); Double(value); }
    void Field(std::string_view name, bool value) { Key(name); Bool(value); }
    std::string Take();  // the text, a newline at the end

  private:
    void Before();
    void Newline();
    std::string out_;
    std::vector<bool> first_;  // for each open container: nothing written in it yet
    bool after_key_ = false;
};

// ---------------------------------------------------------------------------------------------------------------- take.json

struct TakeReport {
    // "running" (written at the start), "complete" (the take did what it should), "inexact" (it ended, but a frame was dropped or
    // repeated), "failed" (it could not run or stopped: `error` says why).
    std::string state = "running";
    std::string error;
    std::string name;
    std::string started_utc;
    std::string finished_utc;
    double wall_s = 0.0;

    bool has_scene = false;
    std::string scene_name;
    std::string scene_source;  // where the file was
    std::string scene_copy;    // its name in the take folder
    std::string scene_fnv64;   // hash of its bytes, hex
    double first_scene_t = 0.0;

    std::string exe_path;
    std::string exe_commit;  // from SET-SOURCE.txt next to the exe (the harness writes it), else ""
    std::string version;

    uint32_t width = 0;
    uint32_t height = 0;
    std::string pixel_format = "bgra8";  // the readback's channel order; the files are as the sink says
    double fps = 0.0;
    uint64_t step_ticks = 0;
    double step_ms = 0.0;

    std::string sink = "none";
    std::string output;  // the video file or the frame pattern, relative to the folder; "" for none
    uint64_t output_bytes = 0;
    uint64_t output_files = 0;  // png: the pictures in frames/
    int ffmpeg_exit = -2;       // -2: ffmpeg was not run
    std::string ffmpeg_command;

    uint64_t first_update = 0;
    uint64_t last_update = 0;  // of the frames read back; the update before the end gate
    uint64_t updates_run = 0;
    uint64_t frames = 0;  // rows: frames read back or reported lost
    uint64_t lost = 0;
    uint64_t dropped = 0;
    uint64_t repeated = 0;
    uint64_t out_of_order = 0;
    uint64_t skipped = 0;
    uint64_t handed = 0;
    bool exact = false;
    std::string verdict;

    uint64_t gate_timeouts = 0;
    bool gate_off = false;
    int lag = 0;

    uint32_t ring = 0;
    uint32_t gpu_buffers = 0;
    uint32_t ring_max_fill = 0;
    uint64_t ring_stalls = 0;
    uint64_t memory_plan_bytes = 0;
    uint64_t memory_cap_bytes = 0;
    std::string memory_note;
    double ram_free_start_mb = -1.0;  // free physical memory (-1: not known)
    double ram_free_min_mb = -1.0;
    double ram_free_end_mb = -1.0;

    std::string state_log;  // the file name, "" when the scene did not ask
    uint64_t state_rows = 0;

    std::vector<std::pair<std::string, std::string>> cvars;  // the settings that differ from their defaults
};
inline constexpr int kTakeJsonVersion = 1;
std::string BuildTakeJson(const TakeReport& report);
// The state of a finished take: failed when `error` is not empty or a sink failed, complete when exact, else inexact.
const char* FinalState(bool exact, bool failed);

// ---------------------------------------------------------------------------------------------------------------- the state log

// One row of state.csv: the game as frame `frame` shows it, read at the frame's hand-over (after its update ran). The player's car
// is read the way map_warp.cpp reads it; `matrix_hash` is the FNV-1a of its 16 floats' bytes (a take that matches another matches in
// it to the last bit).
struct StateRow {
    uint64_t frame = 0;
    uint64_t update = 0;
    double scene_t = -1.0;  // -1: no scene runs
    float hour = 0.0f;
    float time_scale = 0.0f;
    float dt_ms = 0.0f;           // the update's step after the time scale
    float dt_unscaled_ms = 0.0f;  // and before it
    bool paused = false;
    bool world_ready = false;
    uint32_t player = 0;  // the player's racer (guest address), 0 = none
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint64_t matrix_hash = 0;
};
std::string StateCsvHeader();
std::string StateCsvRow(const StateRow& row);

inline constexpr uint64_t kFnvOffset = 14695981039346656037ull;
uint64_t Fnv1a64(const void* data, size_t bytes, uint64_t seed = kFnvOffset);
std::string Hex64(uint64_t value);  // 16 digits

// ---------------------------------------------------------------------------------------------------------------- small things

// A setting whose name says it holds a secret (password, token, key ...): take.json keeps its name and not its value, so a take folder can
// be handed to someone else.
bool IsSecretCvarName(std::string_view name);
// "2026-11-13T09:05:01Z" for seconds since 1970 (UTC); no time zone database, no C library clock.
std::string FormatUtc(int64_t unix_seconds);
// The commit the harness's build-set wrote next to the exe: "ref=feat/x sha=3f1755e3... patches=..." -> "3f1755e3...". Empty when there is none.
std::string ParseSetSourceSha(std::string_view text);

}  // namespace mc::cinematic::capture

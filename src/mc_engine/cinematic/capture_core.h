#pragma once
//
// Capture mode of the trailer toolkit (work package M2a), the pure half: the fixed step in the game's own ticks, the tag that
// ties a picture to the update it shows, the books of the frame gate, the take's books, the scene clock's maths and the
// ffmpeg command line. It knows nothing of the game, the SDK, the renderer or the log, so capture_core_test.cpp builds it on
// its own. capture_mode.cpp is the glue (the cvars, the gate, the take, the sink); gpu/capture_readback.cpp is the render thread's
// half.
//
// THE FRAME GATE, in one place. The game's frame (hooks/frame_pacing.cpp describes it) is
//
//     game thread:    gate -> update k -> hand-over of frame k (the render thread is idle) -> gate -> update k + 1 ...
//     render thread:  (idle) -> draws frame k -> swap -> "render done" -> (idle) ...
//
// so update k + 1 normally runs while the render thread draws frame k. A take wants the opposite: the game does not start update
// k + 1 until frame k has been drawn and read back. The gate before every update waits for exactly that, and a frame is
// identified at the hand-over, on the game thread, where the render thread is idle: the hand-over stamps "this frame shows update
// k" (HandOverBook::Note), the swap that follows takes the stamp. The stamp cannot be late or early whatever the threads do,
// because the next hand-over only comes after the render thread's "render done" of this frame. (The first version of the
// capture tagged a frame at its swap with the number of updates then run; the swap of the frame before a take could come after
// the take's first gate, so the take began with a repeated picture and its last picture was cut off.)
//

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mc::cinematic::capture {

// ---------------------------------------------------------------------------------------------------------------- the step

// The game's tick: dt = ticks * this (a 49.875 MHz timebase, not the SDK's 50 MHz).
inline constexpr double kGameTickSeconds = 2.0050125115744777e-8;
// The engine timer clamps the scaled step to 0.0001 .. 0.1 s: 10 fps is the slowest step it can be given.
inline constexpr double kMinFps = 10.0;
inline constexpr double kMaxFps = 1000.0;

// The rate in range, or 0 when `fps` is not a positive number (nothing is set).
double ClampFps(double fps);
// The step as the game's ticks, rounded to the nearest; 0 when `fps` is not set. 60 fps is 831250 ticks.
uint64_t StepTicksForFps(double fps);
// The rate a step in ticks stands for, to six decimals (what the log and the take's files say).
double FpsForStepTicks(uint64_t ticks);
// The step in milliseconds for `ticks`.
double StepMsForTicks(uint64_t ticks);

// Scene time uses the nominal video/audio step. The guest timer receives that step rounded to guest ticks.
struct TakeSceneClock {
    double t0 = 0.0;
    double step = 0.0;

    double Time(uint64_t frame) const {
        const double offset = static_cast<double>(frame) * step;
        return t0 + offset;
    }
    // The Director's range is half-open. A scene's end can itself be sampled.
    bool PastEnd(uint64_t frame, double end, double duration) const {
        const double t = Time(frame);
        return (end >= 0.0 && t >= end) || (duration >= 0.0 && t > duration);
    }
};

// A request can name its exact first scene time. Otherwise use the first grid point strictly after the request.
TakeSceneClock PlanTakeSceneClock(double request_time, double fps, double requested_t0 = -1.0);
// Limit an ordinary scene update so it visits the authored capture key before the take's first update.
double StepToTakeKey(double scene_t, double dt, double key, bool ready_opened);

// ---------------------------------------------------------------------------------------------------------------- the tag

// A frame's tag, handed from the swap thread to the present record: the update the frame shows in the low 62 bits, and
// kTagCapture when the frame is a frame of the take. 0 = no tag.
inline constexpr uint64_t kTagCapture = 1ull << 63;
inline constexpr uint64_t kTagUpdateMask = (1ull << 62) - 1;
constexpr uint64_t MakeTag(uint64_t update, bool capture) {
    return (update & kTagUpdateMask) | (capture ? kTagCapture : 0);
}
constexpr bool TagIsCapture(uint64_t tag) {
    return (tag & kTagCapture) != 0;
}
constexpr uint64_t TagUpdate(uint64_t tag) {
    return tag & kTagUpdateMask;
}

// ---------------------------------------------------------------------------------------------------------------- the hand-over

// What a hand-over says about its frame.
struct HandOver {
    uint64_t tag = 0;        // 0: no frame to tag (a hand-over with no update before it)
    bool take_frame = false;
    bool repeat = false;     // the same update was handed over before: a screen the game draws without updating
};

// The game thread's books of the hand-overs. One per run; Begin() when a take starts.
class HandOverBook {
  public:
    // `update`: the number of updates that have passed the gate, the one that just ran counted (1-based). `take_active` and
    // `take_first`: whether a take is running and the update it started at. A frame is a take frame when the take is running
    // and the update is not before its first.
    HandOver Note(uint64_t update, bool take_active, uint64_t take_first);
    void BeginTake() { handed_ = 0; }
    // Frames handed over as the take's, since BeginTake.
    uint64_t Handed() const { return handed_; }
    // Hand-overs that repeated an update, over the whole run.
    uint64_t Repeats() const { return repeats_; }

  private:
    uint64_t last_update_ = 0;
    uint64_t handed_ = 0;
    uint64_t repeats_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------- the gate

// How many take frames must have been read back (or lost) before the game may start the next update. `handed`: frames handed
// over so far; `lag`: how many frames the game may run ahead of the readback; `ending`: the take ends at this gate, so it
// waits for every frame and the lag does not count.
uint64_t FramesNeeded(uint64_t handed, uint32_t lag, bool ending);

// The gate's books. `captured` counts take frames the render thread has read back or reported lost; `resync` is what a timeout
// added so that one lost wake-up does not make every later gate wait again.
struct GateBook {
    uint64_t captured = 0;
    int64_t resync = 0;
    uint32_t consecutive_timeouts = 0;
    uint64_t timeouts = 0;
    bool off = false;  // three timeouts in a row: the game is no longer held for the rest of the take

    bool Met(uint64_t needed) const { return static_cast<int64_t>(captured) + resync >= static_cast<int64_t>(needed); }
    // A wait that ran out: sets the books straight (returns how many frames it gave up on) and counts the timeout.
    int64_t TimedOut(uint64_t needed);
    // A wait that was met.
    void NoteMet() { consecutive_timeouts = 0; }
};

// How a wait at the gate ended.
enum class GateEnd {
    kMet,       // every frame needed has been read back or reported lost
    kClosing,   // the game is closing (CaptureCloseNow): nothing will read back the frames still to come, the game thread must get out at once
    kTimedOut,  // `timeout_ms` went by with nothing moving
};

// The game thread's wait at the gate (capture_mode.cpp WaitForFrames), with the lock, the condition variable and the three questions passed in so that
// capture_core_test.cpp can run it against threads. `lock` holds the mutex that guards the gate's books and is held on entry and on return; `met()` asks
// whether the frames needed are in; `progress()` is a counter that moves whenever a frame is read back, lost or written; `closing()` says the game is
// closing. A wait times out only when NOTHING moved for `timeout_ms`: a slow writer that keeps finishing frames never times the game out.
//
// WHY `closing`. The render thread that reads the frames back is stopped by the close (mcla::gpu::Shutdown) while the game thread is still running its
// updates until the SDK stops it, and a frame handed over just before that is never read back or reported lost. Without the third question the game
// thread sat here for the whole timeout (20 s by default), in a host wait that the SDK's stop request cannot reach, so the title's termination found its
// main thread still running and the close watchdog ended the process with its hang status (VERIFY-EXIT, V02: every close during a take). The question is
// asked first after `met()`, so a gate that is met is met whatever the close says. The notifier sets the flag, takes and releases the mutex, then notifies
// (CaptureCloseNow), so the wake-up cannot fall between this loop's question and its wait; `slice_ms` is the backstop if it ever does.
template <class MetFn, class ProgressFn, class ClosingFn>
GateEnd WaitAtGate(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, MetFn met, ProgressFn progress, ClosingFn closing, int timeout_ms,
                   int slice_ms = 200) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    auto seen = progress();
    for (;;) {
        if (met()) return GateEnd::kMet;
        if (closing()) return GateEnd::kClosing;
        const auto now = Clock::now();
        const auto p = progress();
        if (p != seen) {
            seen = p;
            deadline = now + std::chrono::milliseconds(timeout_ms);
        }
        if (now >= deadline) return GateEnd::kTimedOut;
        cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(slice_ms)));
    }
}

// ---------------------------------------------------------------------------------------------------------------- the take

// What a take's frames did, in the order the render thread finished them.
class TakeBook {
  public:
    void Begin(uint64_t first_update);
    // A frame of `update` was read back (`lost` false) or could not be. Returns false when the update is not the one after the
    // frame before (the first frame of a take must be its first update): a frame repeated or missing.
    bool OnFrame(uint64_t update, bool lost);

    uint64_t Rows() const { return rows_; }
    uint64_t Lost() const { return lost_; }
    uint64_t Anomalies() const { return anomalies_; }
    // Of the anomalies: frames of an update the take already had (or one before its first), and updates that no frame came for between
    // two frames (a gap's size, so three missing updates count three).
    uint64_t Repeated() const { return repeated_; }
    uint64_t Skipped() const { return skipped_; }
    uint64_t FirstUpdate() const { return first_; }
    uint64_t LastUpdate() const { return last_; }
    // Updates the take ran, `end_update` being the first update that did not (the gate it ended at).
    uint64_t UpdatesRun(uint64_t end_update) const { return end_update > first_ ? end_update - first_ : 0; }

  private:
    uint64_t first_ = 0;
    uint64_t last_ = 0;
    uint64_t rows_ = 0;
    uint64_t lost_ = 0;
    uint64_t anomalies_ = 0;
    uint64_t repeated_ = 0;
    uint64_t skipped_ = 0;
    bool have_last_ = false;
};

struct Verdict {
    bool exact = false;  // one picture for every update, none repeated, none lost
    std::string text;    // "240 frames for 240 updates (7397..7636): exact" or what is off
    // The take's own counts (take.json writes them): updates that have no picture (lost, skipped, or cut off at the end) and
    // pictures that are not the one of a new update. Both are 0 for an exact take.
    uint64_t dropped = 0;
    uint64_t repeated = 0;
};
// The take's verdict at its end: `end_update`, the first update that did not run in it; `handed`, the frames the hand-overs
// stamped as the take's.
Verdict Judge(const TakeBook& book, uint64_t end_update, uint64_t handed);

// ---------------------------------------------------------------------------------------------------------------- the scene clock

// The clock the algorithms that read the wall time read instead of it during a take (the upscaler's history rule, the UI
// movies' pacing). Wall time when idle; in a take, time is the frames times the step, so a slow capture does not look like a
// stall. Two counters are kept apart because two threads read it with their own: kind 0 the game thread's update count, kind 1
// the renderer's frame count. Each starts where the steady clock was when the clock was switched, so a value kept across the
// switch stays comparable. Not thread safe: the caller locks.
class VirtualClock {
  public:
    // Returns true when the state changed (the epoch moved).
    bool SetActive(bool active);
    bool active() const { return active_; }
    uint32_t epoch() const { return epoch_; }
    // `steady_ms`: the steady clock now; `frame`: the caller's own counter; `step_ms`: the take's step.
    double NowMs(int kind, uint64_t frame, double steady_ms, double step_ms);

  private:
    struct Anchor {
        uint32_t epoch = ~0u;
        uint64_t frame = 0;
        double ms = 0.0;
    };
    bool active_ = false;
    uint32_t epoch_ = 0;
    Anchor anchors_[2];
};

// ---------------------------------------------------------------------------------------------------------------- statistics

struct Stat {
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double max = 0.0;
    size_t n = 0;
};
// Of the values that are not negative (-1 marks "not measured").
Stat StatOf(std::vector<double> values);
// "mean/p95/max" with two decimals.
std::string FormatStat(const Stat& s);

// ---------------------------------------------------------------------------------------------------------------- the sink

enum class SinkKind : uint8_t { kNone, kFfmpeg, kRaw, kPng };
// "none", "ffmpeg", "raw" or "png"; false for anything else (and `*out` is kNone). A scene's `capture ... sink <name>` uses the same
// four names (scene.cpp keeps its own copy of the list, so that it needs nothing else to build).
bool ParseSinkKind(const std::string& text, SinkKind* out);
const char* SinkKindName(SinkKind kind);
// The sinks that run ffmpeg (a pipe of raw frames to it): the FFV1 video and the PNG pictures.
bool SinkUsesFfmpeg(SinkKind kind);
// ffmpeg's command line for a take of raw frames on its standard input: `program` (quoted), `args` (its output options),
// `out_file` (quoted). The rate is written as the shortest form with six significant digits.
std::string FfmpegCommandLine(const std::string& program, uint32_t width, uint32_t height, bool bgra, double fps,
                              const std::string& args, const std::string& out_file);
// The same input for a sequence of PNG pictures: one file for every frame, numbered from 0 (`out_pattern` has a %06d in it), no frame
// repeated or dropped by ffmpeg's own rate conversion (-fps_mode passthrough), RGB (the readback's alpha is not a picture). ffmpeg's
// PNG encoder is the writer: a frame costs about as long as it takes ffmpeg to compress it, and the ring holds the take back meanwhile.
std::string FfmpegPngCommandLine(const std::string& program, uint32_t width, uint32_t height, bool bgra, double fps,
                                 const std::string& out_pattern);

}  // namespace mc::cinematic::capture

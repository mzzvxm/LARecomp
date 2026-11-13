// Check of capture mode's pure half (capture_core.h): the fixed step in the game's ticks, the frame tag, the hand-over book, the
// gate's books, the take's books, the scene clock and the ffmpeg command line. A program of its own: it needs no game, no
// ReXGlue, no renderer and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_capture_test      (CMakeLists.txt)
//   <build dir>/larecomp_capture_test
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 src/mc_engine/cinematic/capture_core_test.cpp src/mc_engine/cinematic/capture_core.cpp -o capture_core_test.exe
//
// The part that matters is TestPipeline: the two threads of the game and the render thread's one-frame lead, replayed as an
// event list, with the first capture's way of tagging (a frame is whatever the update count was at its swap) and with the
// hand-over's stamp. The first fails exactly as the E7 take did (a repeated first frame, the last frame cut off); the second
// is exact whatever the render thread's lead was when the take began.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "capture_core.h"

using namespace mc::cinematic::capture;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

void CheckU(uint64_t got, uint64_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want));
    }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
    }
}

void CheckNum(double got, double want, const std::string& what, double tol = 1e-9) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

// ---------------------------------------------------------------------------------------------------------------- the step

void TestStep() {
    // The game's timebase is 49.875 MHz: a step of 1/n s is that many ticks, exactly for these rates.
    CheckU(StepTicksForFps(60.0), 831250, "60 fps is 831250 ticks");
    CheckU(StepTicksForFps(30.0), 1662500, "30 fps is 1662500 ticks");
    CheckU(StepTicksForFps(24.0), 2078125, "24 fps is 2078125 ticks");
    CheckU(StepTicksForFps(120.0), 415625, "120 fps is 415625 ticks");
    const uint64_t t240 = StepTicksForFps(240.0);
    Check(t240 == 207812 || t240 == 207813, "240 fps is 207812.5 ticks, rounded");
    CheckU(StepTicksForFps(0.0), 0, "0 is not set");
    CheckU(StepTicksForFps(-5.0), 0, "a negative rate is not set");
    CheckU(StepTicksForFps(std::nan("")), 0, "NaN is not set");
    CheckU(StepTicksForFps(5.0), StepTicksForFps(10.0), "below 10 fps the step is 10 fps's");
    CheckU(StepTicksForFps(5000.0), StepTicksForFps(1000.0), "above 1000 fps the step is 1000 fps's");
    CheckNum(ClampFps(0.0), 0.0, "ClampFps(0)");
    CheckNum(ClampFps(60.0), 60.0, "ClampFps(60)");
    CheckNum(ClampFps(1e9), kMaxFps, "ClampFps clamps up");
    // (the game's tick is a float constant, 2.0050125e-8, a hair under 1 / 49.875 MHz: the rate comes back to six decimals)
    CheckNum(FpsForStepTicks(831250), 60.0, "831250 ticks is 60 fps", 2e-6);
    CheckNum(FpsForStepTicks(2078125), 24.0, "2078125 ticks is 24 fps", 2e-6);
    CheckNum(FpsForStepTicks(0), 0.0, "no ticks, no rate");
    CheckNum(StepMsForTicks(831250), 1000.0 / 60.0, "the step in ms", 1e-6);
    for (const double fps : {10.0, 24.0, 25.0, 30.0, 48.0, 50.0, 60.0, 90.0, 120.0, 144.0}) {
        const double back = FpsForStepTicks(StepTicksForFps(fps));
        CheckNum(back, fps, "fps -> ticks -> fps at " + std::to_string(fps), fps * 1e-6);
    }
}

// ---------------------------------------------------------------------------------------------------------------- the tag

void TestTag() {
    const uint64_t t = MakeTag(7397, true);
    Check(TagIsCapture(t), "a take frame's tag says so");
    CheckU(TagUpdate(t), 7397, "and carries its update");
    const uint64_t n = MakeTag(7396, false);
    Check(!TagIsCapture(n), "a frame before the take is not one");
    CheckU(TagUpdate(n), 7396, "its update is kept");
    Check(n != 0, "a tag for update 1 or more is never 0 (0 means no tag)");
    CheckU(TagUpdate(MakeTag(~0ull, true)), kTagUpdateMask, "the update fits 62 bits");
}

// ---------------------------------------------------------------------------------------------------------------- the hand-over

void TestHandOver() {
    HandOverBook b;
    HandOver h = b.Note(1, false, 0);
    Check(h.tag != 0 && !h.take_frame && !h.repeat, "an update with no take: a tag, not a take frame");
    CheckU(TagUpdate(h.tag), 1, "its tag is its update");
    h = b.Note(1, false, 0);
    Check(h.repeat && h.tag == 0 && !h.take_frame, "the same update again is a repeat with no tag");
    CheckU(b.Repeats(), 1, "one repeat counted");
    h = b.Note(0, true, 1);
    Check(h.tag == 0 && !h.repeat, "update 0 (nothing has run) has no frame");

    b.BeginTake();
    h = b.Note(10, true, 11);
    Check(!h.take_frame, "an update before the take's first is not a take frame");
    h = b.Note(11, true, 11);
    Check(h.take_frame && TagIsCapture(h.tag) && TagUpdate(h.tag) == 11, "the take's first update is its first frame");
    h = b.Note(12, true, 11);
    Check(h.take_frame, "and the next");
    h = b.Note(12, true, 11);
    Check(h.repeat && !h.take_frame, "a repeat inside a take is not a take frame (one picture per update)");
    CheckU(b.Handed(), 2, "two frames handed over");
    h = b.Note(13, false, 11);
    Check(!h.take_frame && h.tag != 0, "after the take ended, frames are not take frames");
    CheckU(b.Handed(), 2, "and are not counted");
    b.BeginTake();
    CheckU(b.Handed(), 0, "a new take starts the count again");
}

// ---------------------------------------------------------------------------------------------------------------- the gate

void TestGate() {
    CheckU(FramesNeeded(0, 0, false), 0, "no frame handed, none needed");
    CheckU(FramesNeeded(5, 0, false), 5, "lock-step: every frame handed over");
    CheckU(FramesNeeded(5, 1, false), 4, "a lag of one: one frame behind is allowed");
    CheckU(FramesNeeded(1, 2, false), 0, "the lag never goes below zero");
    CheckU(FramesNeeded(5, 2, true), 5, "at the end the lag does not count");

    GateBook g;
    Check(g.Met(0), "nothing needed is met at once");
    Check(!g.Met(1), "a frame is needed");
    g.captured = 1;
    Check(g.Met(1), "met when it is read back");
    CheckU(static_cast<uint64_t>(g.TimedOut(3)), 2, "a timeout gives up on the two frames short");
    Check(g.Met(3), "and the books say it is met now");
    CheckU(g.timeouts, 1, "one timeout");
    Check(!g.off, "the gate is still on");
    g.NoteMet();
    CheckU(g.consecutive_timeouts, 0, "a met wait clears the run");
    g.TimedOut(4);
    g.TimedOut(5);
    Check(!g.off, "two in a row: still on");
    g.TimedOut(6);
    Check(g.off, "three in a row: the game is not held for the rest of the take");
    CheckU(g.timeouts, 4, "four timeouts in all");
    GateBook late;
    late.captured = 5;
    CheckU(static_cast<uint64_t>(late.TimedOut(4)), 0, "a timeout that finds the books ahead gives up on nothing");
}

// ---------------------------------------------------------------------------------------------------------------- the take

void TestTake() {
    TakeBook t;
    t.Begin(100);
    Check(t.OnFrame(100, false), "the first frame is the first update");
    Check(t.OnFrame(101, false), "the next update follows");
    Check(!t.OnFrame(101, false), "a repeated update is out of order");
    Check(!t.OnFrame(104, false), "a missing update is out of order");
    Check(t.OnFrame(105, true), "the next in a row after that is fine, lost or not");
    CheckU(t.Rows(), 5, "five rows");
    CheckU(t.Lost(), 1, "one lost");
    CheckU(t.Anomalies(), 2, "two anomalies");
    CheckU(t.FirstUpdate(), 100, "the first update");
    CheckU(t.LastUpdate(), 105, "the last update");
    CheckU(t.UpdatesRun(106), 6, "updates 100..105 ran");
    CheckU(t.UpdatesRun(100), 0, "no update ran before the first");

    TakeBook u;
    u.Begin(10);
    Check(!u.OnFrame(11, false), "a take whose first frame is not its first update is an anomaly");

    TakeBook ok;
    ok.Begin(7397);
    for (uint64_t k = 7397; k <= 7636; ++k) ok.OnFrame(k, false);
    const Verdict v = Judge(ok, 7637, 240);
    Check(v.exact, "240 frames for 240 updates is exact");
    CheckEq(v.text, "240 frames for 240 updates (7397..7636): exact", "the verdict text");

    TakeBook one_more;
    one_more.Begin(7397);
    one_more.OnFrame(7397, false);
    for (uint64_t k = 7397; k <= 7635; ++k) one_more.OnFrame(k, false);
    one_more.OnFrame(7636, true);
    const Verdict bad = Judge(one_more, 7637, 240);
    Check(!bad.exact, "a repeated first frame and a lost last one are not exact");
    Check(bad.text.find("241 frames captured (1 lost, 1 out of order) for 240 updates (7397..7636)") == 0, "the verdict says what is off: " + bad.text);
    TakeBook mismatch;
    mismatch.Begin(5);
    for (uint64_t k = 5; k <= 9; ++k) mismatch.OnFrame(k, false);
    Check(!Judge(mismatch, 10, 4).exact, "frames handed over that do not match the updates are not exact");
    Check(Judge(mismatch, 10, 5).exact, "five for five is");
}

// ---------------------------------------------------------------------------------------------------------------- the pipeline

// The two threads of the game as an event list. The game thread runs gate(u) -> update u -> hand-over(u) -> gate(u + 1) ...;
// the render thread draws frame k after the hand-over of k and finishes it (swap, "render done") before the hand-over of k + 1.
// With no take the game thread does not wait for it, so at a gate the render thread is a frame behind (frame u - 1 may be
// swapped after update u has started: the late swap). While a take runs the gate waits until every frame handed over is read
// back, as the runtime's gate does.
struct Pipeline {
    // what the take books say
    HandOverBook handover;
    GateBook gate;
    TakeBook take;
    bool stamp_at_handover = true;  // false: the first capture's tagging (the update count at the swap)
    bool eager = false;             // the render thread swaps a frame as soon as it is handed over (else only when it must)

    uint64_t updates = 0;        // the game thread's count (updates that passed the gate)
    bool active = false;
    uint64_t first = 0;
    std::deque<uint64_t> pending_stamp;  // frames the render thread has been handed and has not swapped (at most one: render done)
    std::deque<uint64_t> pending_update; // the update each of them shows
    std::vector<uint64_t> shown;         // the update each take frame really shows, in the order they were read back

    // The render thread finishes the frame it was given: its swap, then the readback if the tag says take frame.
    void RenderOne() {
        const uint64_t stamp = pending_stamp.front();
        const uint64_t shows = pending_update.front();
        pending_stamp.pop_front();
        pending_update.pop_front();
        uint64_t tag = stamp;
        if (!stamp_at_handover) {
            // swap-time tagging: whatever the update count is now; a take frame when it is not before the take's first update
            tag = MakeTag(updates, active && updates >= first);
        }
        if (TagIsCapture(tag)) {
            take.OnFrame(TagUpdate(tag), false);
            shown.push_back(shows);
            ++gate.captured;
        }
    }

    void RenderAll() {
        while (!pending_stamp.empty()) RenderOne();
    }

    // The gate of the next update; `want`: a take is wanted. Returns the update that runs, or 0 when the take ended here.
    uint64_t Gate(bool want) {
        const uint64_t u = updates + 1;
        if (!active && want) {
            active = true;
            first = u;
            handover.BeginTake();
            take.Begin(u);
            gate = GateBook{};
            shown.clear();
        } else if (active) {
            const bool ending = !want;
            // the old gate waited for u - first frames (the updates the take had run), the new one for the frames handed over
            const uint64_t needed = stamp_at_handover ? FramesNeeded(handover.Handed(), 0, ending) : (u - first);
            while (!gate.Met(needed) && !pending_stamp.empty()) RenderOne();
            if (ending) {
                active = false;
                updates = u;
                return 0;
            }
        }
        updates = u;
        return u;
    }

    // The hand-over of the update that just ran: the render thread must have finished the frame it was given before.
    void HandOverUpdate() {
        RenderAll();  // "render done" of the frame before
        const HandOver h = handover.Note(updates, active, first);
        pending_stamp.push_back(h.tag);
        pending_update.push_back(updates);
        if (eager) RenderOne();
    }

    // Updates start and end like the game's loop. The take is wanted from update `from` (its gate) to the gate of `to` (not run).
    void Run(uint64_t from, uint64_t to) { Run(1, from, to); }
    void Run(uint64_t start, uint64_t from, uint64_t to) {
        for (uint64_t u = start;; ++u) {
            const bool want = u >= from && u < to;
            if (Gate(want) == 0) break;
            HandOverUpdate();
            if (u > to + 5) break;  // safety
        }
        RenderAll();  // the game thread went on: the render thread finishes what it was given
    }
};

bool Consecutive(const std::vector<uint64_t>& v, uint64_t first, uint64_t count) {
    if (v.size() != count) return false;
    for (uint64_t i = 0; i < count; ++i) {
        if (v[i] != first + i) return false;
    }
    return true;
}

void TestPipeline() {
    // The take wanted at update 7397 (its gate), up to the gate of 7637: 240 updates, 7397 to 7636.
    {
        Pipeline p;
        p.stamp_at_handover = false;
        p.Run(7397, 7637);
        // the first capture: frame 7396 was still with the render thread when the take began, its swap came after the take's
        // first gate and counted as the take's; the last frame, 7636, was handed over after the count the gate waited for
        CheckU(p.take.Rows(), 240, "swap-time tagging: 240 rows, but...");
        Check(!p.shown.empty() && p.shown.front() == 7396, "swap-time tagging: the first picture is update 7396's, from before the take");
        Check(!p.shown.empty() && p.shown.back() == 7635, "swap-time tagging: the last picture is update 7635's (E7: 7636's was lost)");
        Check(!Consecutive(p.shown, 7397, 240), "swap-time tagging does not give pictures 7397..7636");
    }
    // The stamp, whichever way the render thread runs: behind (swaps a frame only when it has to) or eager.
    for (const bool eager : {false, true}) {
        const std::string how = eager ? " (eager render thread)" : " (lagging render thread)";
        Pipeline p;
        p.eager = eager;
        p.Run(7397, 7637);
        CheckU(p.take.Rows(), 240, "hand-over stamps: 240 rows for 240 updates" + how);
        CheckU(p.take.Anomalies(), 0, "no repeat, nothing out of order" + how);
        CheckU(p.take.Lost(), 0, "nothing lost" + how);
        CheckU(p.take.FirstUpdate(), 7397, "the take starts at 7397" + how);
        CheckU(p.take.LastUpdate(), 7636, "and its last picture is update 7636's" + how);
        Check(Consecutive(p.shown, 7397, 240), "the pictures are exactly updates 7397..7636" + how);
        CheckU(p.handover.Handed(), 240, "240 frames handed over" + how);
        Check(Judge(p.take, 7637, p.handover.Handed()).exact, "exact" + how);
        CheckU(p.gate.captured, 240, "the gate's books agree" + how);
        CheckU(p.pending_stamp.size(), 0, "no frame left with the render thread" + how);
    }
    {
        // a take of one frame, and a take that starts at the very first update
        Pipeline p;
        p.Run(5, 6);
        CheckU(p.take.Rows(), 1, "a one-frame take has one frame");
        CheckU(p.take.Anomalies(), 0, "in order");
        CheckU(p.take.FirstUpdate(), 5, "of update 5");
        Check(Consecutive(p.shown, 5, 1), "and it shows update 5");
        Pipeline q;
        q.Run(1, 31);
        CheckU(q.take.Rows(), 30, "a take from the first update: 30 frames");
        CheckU(q.take.Anomalies(), 0, "in order");
        Check(Consecutive(q.shown, 1, 30), "showing updates 1..30");
    }
    {
        // two takes in one run: each exact (the books start again)
        Pipeline p;
        p.Run(10, 20);
        CheckU(p.take.Rows(), 10, "the first take: 10 frames");
        const uint64_t before = p.updates;
        p.Run(before + 1, before + 5, before + 12);
        CheckU(p.take.Rows(), 7, "the second take: 7 frames");
        CheckU(p.take.Anomalies(), 0, "in order");
        CheckU(p.take.FirstUpdate(), before + 5, "it started where it was asked to");
        Check(Consecutive(p.shown, before + 5, 7), "and shows exactly its updates");
    }
    {
        // a hand-over with no update before it (a screen drawn without the timer) in the middle of a take is no take frame
        Pipeline p;
        p.active = true;
        p.first = 1;
        p.handover.BeginTake();
        p.take.Begin(1);
        p.updates = 1;
        p.HandOverUpdate();
        p.HandOverUpdate();  // the same update again
        p.updates = 2;
        p.HandOverUpdate();
        p.RenderAll();
        CheckU(p.take.Rows(), 2, "two frames for two updates, the repeated hand-over made none");
        CheckU(p.take.Anomalies(), 0, "nothing out of order");
        CheckU(p.handover.Repeats(), 1, "one repeat counted");
    }
}

// ---------------------------------------------------------------------------------------------------------------- the scene clock

void TestClock() {
    VirtualClock c;
    const double step = 1000.0 / 60.0;
    CheckNum(c.NowMs(0, 100, 5000.0, step), 5000.0, "idle: the steady clock");
    CheckNum(c.NowMs(1, 7, 5001.0, step), 5001.0, "idle, the other kind");
    Check(!c.active(), "not active");
    const uint32_t e0 = c.epoch();
    Check(c.SetActive(true), "switching on changes the epoch");
    Check(c.epoch() != e0, "a new epoch");
    Check(!c.SetActive(true), "switching on again does nothing");
    // the first read anchors at the steady clock
    CheckNum(c.NowMs(0, 100, 5010.0, step), 5010.0, "the first read starts where the steady clock is");
    CheckNum(c.NowMs(0, 101, 9999.0, step), 5010.0 + step, "one frame later: one step later, whatever the wall clock says");
    CheckNum(c.NowMs(0, 160, 123.0, step), 5010.0 + 60.0 * step, "sixty frames: a second");
    // the other kind has its own anchor
    CheckNum(c.NowMs(1, 7, 5020.0, step), 5020.0, "the renderer's counter is anchored where it is first read");
    CheckNum(c.NowMs(1, 9, 0.0, step), 5020.0 + 2.0 * step, "two frames on");
    // a gap that would have been a stall on the wall clock is one step on this one
    const double a = c.NowMs(1, 10, 10000.0, step);
    const double b = c.NowMs(1, 11, 10150.0, step);
    CheckNum(b - a, step, "a slow frame (150 ms of wall time) is still one step", 1e-9);
    Check(c.SetActive(false), "switching off changes the epoch again");
    CheckNum(c.NowMs(0, 500, 42.0, step), 42.0, "off: the steady clock");
    c.SetActive(true);
    CheckNum(c.NowMs(0, 500, 6000.0, step), 6000.0, "a new take anchors again, at the steady clock of that moment");
}

// ---------------------------------------------------------------------------------------------------------------- statistics, sink

void TestStats() {
    const Stat none = StatOf({});
    CheckU(none.n, 0, "no values");
    const Stat s = StatOf({4.0, 1.0, 2.0, 3.0, -1.0});
    CheckU(s.n, 4, "the -1 is not a value");
    CheckNum(s.mean, 2.5, "mean");
    CheckNum(s.max, 4.0, "max");
    CheckNum(s.p50, 3.0, "p50 of four is the third");
    CheckEq(FormatStat(s), "2.50/4.00/4.00", "formatted");
    std::vector<double> hundred;
    for (int i = 1; i <= 100; ++i) hundred.push_back(i);
    CheckNum(StatOf(hundred).p95, 96.0, "p95 of 1..100");
}

void TestSink() {
    SinkKind k = SinkKind::kRaw;
    Check(ParseSinkKind("none", &k) && k == SinkKind::kNone, "none");
    Check(ParseSinkKind("ffmpeg", &k) && k == SinkKind::kFfmpeg, "ffmpeg");
    Check(ParseSinkKind("raw", &k) && k == SinkKind::kRaw, "raw");
    Check(!ParseSinkKind("mp4", &k) && k == SinkKind::kNone, "anything else is refused and means none");
    CheckEq(SinkKindName(SinkKind::kFfmpeg), "ffmpeg", "its name");
    // the command E7's take ran (perf/out/CIN-E7.txt)
    CheckEq(FfmpegCommandLine("C:\\ffmpeg\\ffmpeg.exe", 3840, 2160, true, 60.0, "-c:v ffv1 -level 3 -g 1 -slices 24 -pix_fmt bgr0",
                              "C:\\take\\take.mkv"),
            "\"C:\\ffmpeg\\ffmpeg.exe\" -y -hide_banner -loglevel warning -f rawvideo -pixel_format bgra -video_size 3840x2160 "
            "-framerate 60 -i - -c:v ffv1 -level 3 -g 1 -slices 24 -pix_fmt bgr0 \"C:\\take\\take.mkv\"",
            "the ffmpeg command line");
    CheckEq(FfmpegCommandLine("ffmpeg", 1280, 720, false, 23.976, "", "o.mkv"),
            "\"ffmpeg\" -y -hide_banner -loglevel warning -f rawvideo -pixel_format rgba -video_size 1280x720 -framerate 23.976 -i - \"o.mkv\"",
            "rgba, a fractional rate, no output options");
}

}  // namespace

int main() {
    TestStep();
    TestTag();
    TestHandOver();
    TestGate();
    TestTake();
    TestPipeline();
    TestClock();
    TestStats();
    TestSink();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

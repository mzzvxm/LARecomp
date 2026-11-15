// Check of the take folder's pure half (take_folder.h): take names, the files' layout, the memory plan and its bound, the slot pool, the host ring
// (real threads: a fast render and a slow writer, a stuck writer, a writer that fails), take.json (written and read back), the state log's rows and the
// small helpers. A program of its own: it needs no game, no ReXGlue, no renderer and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_take_test      (CMakeLists.txt)
//   <build dir>/larecomp_take_test [sample.json]
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 src/mc_engine/cinematic/take_folder_test.cpp src/mc_engine/cinematic/take_folder.cpp src/mc_engine/cinematic/capture_core.cpp
//           -o take_folder_test.exe
//
// With a file name it also writes a take.json of a finished take there, so that ports/scripts/scene_run.py's reader can be checked against what the
// game's writer makes (the check chain does: `scene_run.py check-json <sample.json>`).
//
// The part that matters is TestPipeline: the game thread's frame gate, a render thread that reads frames back and a writer far slower than both, all
// real threads on the real ring, with the take's own books (TakeBook, Judge). The ring never holds more than its slots, the game never runs more than
// the lag ahead of the frames read back, and the take is exact: a slow writer costs time, never frames or memory.

#define _CRT_SECURE_NO_WARNINGS 1

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "capture_core.h"
#include "take_folder.h"

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
        std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
    }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
    }
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

using Ms = std::chrono::milliseconds;
double ElapsedMs(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------------------------------------------- names

void TestNames() {
    CheckEq(SanitizeTakeName("harbor-flyby", ""), "harbor-flyby", "a good name is kept");
    CheckEq(SanitizeTakeName("My Take 1", ""), "My-Take-1", "blanks become a dash");
    CheckEq(SanitizeTakeName("a  b\t c", ""), "a-b-c", "a run of blanks is one dash");
    CheckEq(SanitizeTakeName("../../etc", ""), "etc", "no way out of the folder");
    CheckEq(SanitizeTakeName("a/b\\c:d*e?f\"g<h>i|j", ""), "a-b-c-d-e-f-g-h-i-j", "every character a path does not allow");
    CheckEq(SanitizeTakeName(".hidden", ""), "hidden", "no leading dot");
    CheckEq(SanitizeTakeName("take...", ""), "take", "no trailing dots (Windows drops them)");
    CheckEq(SanitizeTakeName("  --x--  ", ""), "x", "dashes and blanks around it go");
    CheckEq(SanitizeTakeName("", "intro-sunset"), "intro-sunset", "the fallback when there is no name");
    CheckEq(SanitizeTakeName("???", "intro sunset"), "intro-sunset", "the fallback is cleaned too");
    CheckEq(SanitizeTakeName("", ""), "take", "and take when there is nothing");
    CheckEq(SanitizeTakeName("\xC3\xA9t\xC3\xA9", ""), "t", "non-ASCII bytes are not kept (a folder name that every tool reads)");
    CheckEq(SanitizeTakeName("con", ""), "_con", "a device name");
    CheckEq(SanitizeTakeName("NUL.txt", ""), "_NUL.txt", "a device name with an extension, any case");
    CheckEq(SanitizeTakeName("com3", ""), "_com3", "com3");
    CheckEq(SanitizeTakeName("com0", ""), "com0", "com0 is not one");
    CheckEq(SanitizeTakeName("console", ""), "console", "a name that starts like one is not one");
    const std::string longname(200, 'a');
    CheckU(SanitizeTakeName(longname, "").size(), kMaxTakeNameLength, "a long name is cut");
    CheckEq(SanitizeTakeName(std::string(63, 'a') + "-bbb", ""), std::string(63, 'a'), "cut where a dash would end it");

    std::set<std::string> taken{"a", "a-2", "a-3"};
    const auto is_taken = [&](const std::string& n) { return taken.count(n) != 0; };
    CheckEq(UniqueName("b", is_taken), "b", "a free name is kept");
    CheckEq(UniqueName("a", is_taken), "a-4", "the next free suffix");
    taken.clear();
    for (int i = 2; i < 1000; ++i) taken.insert("z-" + std::to_string(i));
    taken.insert("z");
    CheckEq(UniqueName("z", is_taken), "", "nothing left after 999");

    CheckEq(JoinPath("C:\\takes", "a"), "C:\\takes\\a", "backslash folder");
    CheckEq(JoinPath("C:\\takes\\", "a"), "C:\\takes\\a", "a separator already there");
    CheckEq(JoinPath("takes/x", "a"), "takes/x/a", "slash folder");
    CheckEq(JoinPath("takes", "a"), "takes/a", "neither");
    CheckEq(JoinPath("", "a"), "a", "empty");
    CheckEq(JoinPath("C:/takes\\x", "a"), "C:/takes\\x/a", "both kinds: slash");
}

void TestLayout() {
    const TakeLayout l = MakeLayout("C:\\takes\\t1");
    CheckEq(l.take_json, "C:\\takes\\t1\\take.json", "take.json");
    CheckEq(l.frames_csv, "C:\\takes\\t1\\frames.csv", "frames.csv");
    CheckEq(l.state_csv, "C:\\takes\\t1\\state.csv", "state.csv");
    CheckEq(l.scene_copy, "C:\\takes\\t1\\scene.scene", "scene copy");
    CheckEq(l.video_mkv, "C:\\takes\\t1\\video.mkv", "video.mkv");
    CheckEq(l.video_raw, "C:\\takes\\t1\\video.bgra", "video.bgra");
    CheckEq(l.audio_wav, "C:\\takes\\t1\\audio.wav", "audio.wav");
    CheckEq(l.audio_csv, "C:\\takes\\t1\\audio.csv", "audio.csv");
    CheckEq(l.frames_dir, "C:\\takes\\t1\\frames", "frames");
    CheckEq(l.frame_pattern, "C:\\takes\\t1\\frames\\f%06d.png", "the pattern ffmpeg numbers by");
    CheckEq(l.ffmpeg_log, "C:\\takes\\t1\\ffmpeg.log", "ffmpeg.log");
    CheckEq(FrameFileName(0), "f000000.png", "frame 0");
    CheckEq(FrameFileName(1199), "f001199.png", "frame 1199");
    CheckEq(FrameFileName(1234567), "f1234567.png", "more than six digits widen (as ffmpeg's %06d does)");
}

// ---------------------------------------------------------------------------------------------------------------- memory

void TestMemory() {
    const uint64_t kMiB = 1048576;
    // 4K: 3840x2160x4 = 31.64 MiB a frame, rows already a multiple of 256
    MemoryRequest r;
    r.width = 3840;
    r.height = 2160;
    r.gpu_max = 4;
    r.ring_wanted = 4;
    r.cap_bytes = 2048 * kMiB;
    MemoryPlan p = PlanMemory(r);
    Check(p.ok, "4K fits the default cap");
    CheckU(p.gpu_frame_bytes, 33177600, "a 4K GPU buffer");
    CheckU(p.host_frame_bytes, 33177600, "a 4K host buffer");
    CheckU(p.gpu_buffers, 4, "all GPU buffers");
    CheckU(p.ring, 4, "the whole ring");
    CheckU(p.total_bytes, 8 * 33177600ull, "eight buffers");
    CheckEq(FormatMib(p.total_bytes), "253.1 MiB", "what E7's 4K take held (8 x 31.6 MiB)");
    Check(p.text.empty(), "nothing cut: no note");

    // a width whose rows are not a multiple of 256: the GPU buffer is padded, the host buffer is not
    MemoryRequest odd = r;
    odd.width = 1000;
    odd.height = 10;
    const MemoryPlan po = PlanMemory(odd);
    CheckU(po.gpu_frame_bytes, 4096ull * 10, "1000 pixels = 4000 bytes, padded to 4096");
    CheckU(po.host_frame_bytes, 40000, "packed rows on the host");

    // 8K: 126.6 MiB a frame
    MemoryRequest k8 = r;
    k8.width = 7680;
    k8.height = 4320;
    p = PlanMemory(k8);
    Check(p.ok && p.gpu_buffers == 4 && p.ring == 4, "8K at the default cap: 8 x 126.6 MiB = 1012.5 MiB fits 2048");
    CheckEq(FormatMib(p.total_bytes), "1012.5 MiB", "8K total");
    k8.ring_wanted = 8;
    p = PlanMemory(k8);
    Check(p.ok && p.ring == 8, "a ring of 8 at 8K = 1519 MiB still fits 2048");
    k8.cap_bytes = 600 * kMiB;
    p = PlanMemory(k8);
    Check(p.ok, "a 600 MiB cap at 8K: ok");
    CheckU(p.ring, 2, "the ring is cut first, to its minimum");
    CheckU(p.gpu_buffers, 2, "then the GPU buffers");
    Check(p.total_bytes <= 600 * kMiB, "and it fits");
    Check(Has(p.text, "buffers cut to fit the memory cap"), "the note says what was cut: " + p.text);
    k8.cap_bytes = 400 * kMiB;
    p = PlanMemory(k8);
    Check(!p.ok, "400 MiB cannot hold four 8K frames");
    Check(Has(p.text, "needs at least 506.3 MiB") && Has(p.text, "over the memory cap (400.0 MiB)") && Has(p.text, "lower the render scale"),
          "the error says what is needed and what to do: " + p.text);

    // the free memory
    MemoryRequest f = r;
    f.cap_bytes = 0;
    f.available_bytes = 1024 * kMiB;
    f.reserve_bytes = 768 * kMiB;  // 256 MiB usable
    p = PlanMemory(f);
    Check(p.ok && p.gpu_buffers == 4 && p.ring == 4, "253 MiB fits 256 MiB of usable memory");
    f.available_bytes = 900 * kMiB;  // 132 MiB usable
    p = PlanMemory(f);
    Check(p.ok && p.gpu_buffers == 2 && p.ring == 2, "132 MiB usable: the minimums (4 x 31.6 = 126.6 MiB)");
    Check(Has(p.text, "the free memory"), "the note names the free memory: " + p.text);
    f.available_bytes = 700 * kMiB;  // less than the reserve
    p = PlanMemory(f);
    Check(!p.ok && Has(p.text, "the free memory"), "less free than the reserve: nothing fits, and it says so");
    f.available_bytes = 0;
    p = PlanMemory(f);
    Check(p.ok && p.limit_bytes == 0, "no cap and no reading: no limit");
    // the smaller of the two limits wins
    MemoryRequest both = r;
    both.available_bytes = 3000 * kMiB;
    both.reserve_bytes = 1024 * kMiB;  // 1976 usable, the cap 2048
    p = PlanMemory(both);
    CheckU(p.limit_bytes, 1976 * kMiB, "the free memory is the tighter limit");
    both.available_bytes = 8000 * kMiB;
    p = PlanMemory(both);
    CheckU(p.limit_bytes, 2048 * kMiB, "the cap is");
    // the fixed part is counted
    MemoryRequest fx = r;
    fx.fixed_bytes = 4 * kMiB;
    CheckU(PlanMemory(fx).total_bytes, 8 * 33177600ull + 4 * kMiB, "the pipe's buffer is in the total");

    // the bound as a property: for any size and limit, an ok plan fits the limit and never exceeds what was asked, and a plan that is not ok
    // does not fit even at the minimums
    uint64_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return seed >> 33;
    };
    int ok_plans = 0, refused = 0;
    for (int i = 0; i < 2000; ++i) {
        MemoryRequest q;
        q.width = static_cast<uint32_t>(320 + next() % 7000);
        q.height = static_cast<uint32_t>(180 + next() % 4000);
        q.gpu_max = static_cast<uint32_t>(2 + next() % 4);
        q.gpu_min = static_cast<uint32_t>(1 + next() % q.gpu_max);
        q.ring_wanted = static_cast<uint32_t>(2 + next() % 7);
        q.ring_min = static_cast<uint32_t>(1 + next() % q.ring_wanted);
        q.cap_bytes = (next() % 3 == 0) ? 0 : (64 + next() % 4000) * kMiB;
        q.available_bytes = (next() % 3 == 0) ? 0 : (256 + next() % 16000) * kMiB;
        q.reserve_bytes = (next() % 600) * kMiB;
        q.fixed_bytes = (next() % 8) * kMiB;
        const MemoryPlan m = PlanMemory(q);
        const uint64_t at_min = q.gpu_min * m.gpu_frame_bytes + q.ring_min * m.host_frame_bytes + q.fixed_bytes;
        if (m.ok) {
            ++ok_plans;
            Check(m.limit_bytes == 0 || m.total_bytes <= m.limit_bytes, "an ok plan fits its limit");
            Check(m.gpu_buffers >= q.gpu_min && m.gpu_buffers <= std::max(q.gpu_min, q.gpu_max), "GPU buffers between the minimum and the ask");
            Check(m.ring >= q.ring_min && m.ring <= std::max(q.ring_min, q.ring_wanted), "ring between the minimum and the ask");
        } else {
            ++refused;
            Check(m.limit_bytes > 0 && at_min > m.limit_bytes, "a refused plan does not fit even at the minimums");
        }
    }
    Check(ok_plans > 100 && refused > 20, "the property test saw both kinds (" + std::to_string(ok_plans) + " ok, " + std::to_string(refused) + " refused)");

    Check(!MemoryFloorHit(0, 512 * kMiB), "free memory not known: no stop");
    Check(!MemoryFloorHit(1024 * kMiB, 0), "no floor: no stop");
    Check(MemoryFloorHit(300 * kMiB, 512 * kMiB), "under the floor: stop");
    Check(!MemoryFloorHit(512 * kMiB, 512 * kMiB), "at the floor: not yet");
}

// ---------------------------------------------------------------------------------------------------------------- the slot pool and the ring

void TestSlotPool() {
    SlotPool p(3);
    CheckU(p.capacity(), 3, "capacity");
    uint32_t a = 9, b = 9, c = 9, d = 9;
    Check(p.TryAcquire(&a) && p.TryAcquire(&b) && p.TryAcquire(&c), "three slots");
    Check(a != b && b != c && a != c && a < 3 && b < 3 && c < 3, "all different, all in range");
    Check(!p.TryAcquire(&d), "no fourth: there is no way to get more than the capacity");
    CheckU(p.in_use(), 3, "three in use");
    CheckU(p.free_slots(), 0, "none free");
    Check(p.Release(b), "give one back");
    Check(!p.Release(b), "twice is refused");
    Check(!p.Release(7), "a slot that never was is refused");
    Check(p.TryAcquire(&d) && d == b, "the slot given back is the one handed out again");
    CheckU(p.max_in_use(), 3, "the most in use");
    p.Reset(2);
    CheckU(p.capacity(), 2, "reset to two");
    CheckU(p.in_use(), 0, "none in use");
    CheckU(p.max_in_use(), 0, "the high-water mark is reset too");
    SlotPool empty(0);
    Check(!empty.TryAcquire(&a), "a pool of none gives none");
}

void FillFrame(uint8_t* data, uint64_t bytes, uint64_t row) {
    for (uint64_t i = 0; i < bytes; ++i) data[i] = static_cast<uint8_t>((row * 31 + i) & 0xFF);
}
bool FrameIs(const uint8_t* data, uint64_t bytes, uint64_t row) {
    for (uint64_t i = 0; i < bytes; ++i) {
        if (data[i] != static_cast<uint8_t>((row * 31 + i) & 0xFF)) return false;
    }
    return true;
}

// A render far quicker than a writer: the ring is the only place a frame waits, and it holds at most its slots.
void TestRingBackPressure() {
    HostRing ring;
    std::mutex m;
    std::vector<uint64_t> order;
    bool content_ok = true;
    Check(ring.Open(3, 1024, [&](uint64_t row, const uint8_t* data, uint64_t bytes) {
              std::this_thread::sleep_for(Ms(3));  // the slow writer
              std::lock_guard<std::mutex> lock(m);
              order.push_back(row);
              if (!FrameIs(data, bytes, row)) content_ok = false;
              return true;
          }),
          "the ring opens");
    Check(ring.is_open(), "and is open");
    CheckU(ring.slots(), 3, "three slots");
    const auto t0 = std::chrono::steady_clock::now();
    double waited_total = 0.0;
    const uint64_t kFrames = 120;
    for (uint64_t row = 0; row < kFrames; ++row) {
        double waited = 0.0;
        bool stuck = false;
        const HostRing::Slot s = ring.Acquire(5000, &waited, &stuck);
        waited_total += waited;
        if (!s.data || stuck) {
            Check(false, "a slot for every frame");
            break;
        }
        CheckU(s.bytes, 1024, "the slot's size");
        FillFrame(s.data, s.bytes, row);
        ring.Submit(s, row);
    }
    ring.Close();
    const double elapsed = ElapsedMs(t0);
    CheckU(order.size(), kFrames, "every frame was written");
    bool in_order = true;
    for (size_t i = 0; i < order.size(); ++i) in_order = in_order && order[i] == i;
    Check(in_order, "in the order they were handed over");
    Check(content_ok, "with the bytes that were put in");
    Check(ring.max_in_use() <= 3, "never more than three buffers in use: " + std::to_string(ring.max_in_use()));
    Check(ring.max_queued() <= 3, "never more than three waiting for the writer: " + std::to_string(ring.max_queued()));
    Check(ring.max_in_use() == 3, "and the ring did fill (the writer was the slow one)");
    CheckU(ring.written(), kFrames, "the ring's own count");
    CheckU(ring.write_failures(), 0, "no failure");
    Check(waited_total > 100.0, "the render waited for the writer: " + std::to_string(waited_total) + " ms (back-pressure, not a bigger buffer)");
    Check(elapsed > 120 * 3 * 0.8, "the take took as long as the writer needed: " + std::to_string(elapsed) + " ms");
    Check(!ring.is_open(), "closed after Close");
    bool stuck = false;
    const HostRing::Slot after = ring.Acquire(100, nullptr, &stuck);
    Check(after.data == nullptr && !stuck, "a closed ring gives no buffer (and is not 'stuck')");
    ring.Close();  // twice is fine
}

// A writer that stops: the render does not wait for ever, it is told. A writer that is only slow is never called stuck.
void TestRingStuck() {
    {
        HostRing ring;
        std::mutex m;
        std::condition_variable cv;
        bool release = false;
        std::atomic<int> written{0};
        Check(ring.Open(2, 256, [&](uint64_t, const uint8_t*, uint64_t) {
                  std::unique_lock<std::mutex> lock(m);
                  cv.wait(lock, [&] { return release; });
                  ++written;
                  return true;
              }),
              "ring for the stuck writer");
        for (uint64_t row = 0; row < 2; ++row) {
            const HostRing::Slot s = ring.Acquire(1000, nullptr, nullptr);
            Check(s.data != nullptr, "the first two slots are free");
            ring.Submit(s, row);
        }
        double waited = 0.0;
        bool stuck = false;
        const HostRing::Slot none = ring.Acquire(250, &waited, &stuck);
        Check(none.data == nullptr && stuck, "the third waits, then is told the writer is stuck");
        Check(waited >= 240.0 && waited < 2000.0, "after about the timeout: " + std::to_string(waited) + " ms");
        {
            std::lock_guard<std::mutex> lock(m);
            release = true;
        }
        cv.notify_all();
        const HostRing::Slot again = ring.Acquire(2000, nullptr, &stuck);
        Check(again.data != nullptr && !stuck, "once the writer runs again a slot comes");
        ring.Cancel(again);
        ring.Close();
        CheckU(written.load(), 2, "both queued frames were written at the end");
    }
    {
        // a writer taking 50 ms a frame against a timeout of 150 ms: progress every 50 ms, so never stuck, however many frames
        HostRing ring;
        Check(ring.Open(1, 64, [&](uint64_t, const uint8_t*, uint64_t) {
                  std::this_thread::sleep_for(Ms(50));
                  return true;
              }),
              "ring for the slow writer");
        int stuck_count = 0;
        for (uint64_t row = 0; row < 6; ++row) {
            bool stuck = false;
            const HostRing::Slot s = ring.Acquire(150, nullptr, &stuck);
            if (stuck || !s.data) {
                ++stuck_count;
                continue;
            }
            ring.Submit(s, row);
        }
        ring.Close();
        CheckU(static_cast<uint64_t>(stuck_count), 0, "a slow writer that keeps finishing frames is not stuck");
        CheckU(ring.written(), 6, "all six written");
    }
}

void TestRingMisc() {
    {
        // Close writes everything that was handed over
        HostRing ring;
        std::atomic<int> n{0};
        Check(ring.Open(4, 128, [&](uint64_t, const uint8_t*, uint64_t) {
                  std::this_thread::sleep_for(Ms(5));
                  ++n;
                  return true;
              }),
              "ring for the drain");
        for (uint64_t row = 0; row < 4; ++row) ring.Submit(ring.Acquire(1000, nullptr, nullptr), row);
        ring.Close();
        CheckU(static_cast<uint64_t>(n.load()), 4, "Close drains the queue");
    }
    {
        // a write that fails is counted, the rest go on; Cancel gives a slot back
        HostRing ring;
        Check(ring.Open(2, 128, [&](uint64_t row, const uint8_t*, uint64_t) { return row != 3 && row != 5; }), "ring for the failures");
        for (uint64_t row = 0; row < 8; ++row) {
            const HostRing::Slot s = ring.Acquire(1000, nullptr, nullptr);
            if (row == 6) {
                ring.Cancel(s);
                continue;
            }
            ring.Submit(s, row);
        }
        ring.Close();
        CheckU(ring.written(), 7, "seven were handed over (one cancelled)");
        CheckU(ring.write_failures(), 2, "two writes failed");
        CheckU(ring.first_failed_row(), 3, "the first was frame 3");
    }
    {
        // memory that is not there: Open says so and leaves nothing behind
        HostRing ring;
        Check(!ring.Open(2, 1ull << 45, [](uint64_t, const uint8_t*, uint64_t) { return true; }), "32 TiB of buffers cannot be had");
        Check(!ring.is_open(), "and nothing is open");
        Check(!ring.Open(0, 128, [](uint64_t, const uint8_t*, uint64_t) { return true; }), "no slots");
        Check(!ring.Open(2, 0, [](uint64_t, const uint8_t*, uint64_t) { return true; }), "no bytes");
        Check(ring.Open(2, 128, [](uint64_t, const uint8_t*, uint64_t) { return true; }), "it opens afterwards");
        Check(ring.Open(3, 128, [](uint64_t, const uint8_t*, uint64_t) { return true; }), "and can be opened again (it closes the old one first)");
        CheckU(ring.slots(), 3, "three slots now");
    }
}

// ---------------------------------------------------------------------------------------------------------------- the pipeline

// What capture_mode.cpp does, with its books and the real ring: a game thread that stops at the frame gate, a render thread that reads each frame
// back into a ring buffer, and a writer that is much slower than either. Lag 0 and lag 2.
struct PipelineResult {
    uint64_t max_ahead = 0;     // frames handed over and not yet read back, seen at any gate
    uint32_t max_in_use = 0;
    uint32_t max_queued = 0;
    Verdict verdict;
    uint64_t written = 0;
    double elapsed_ms = 0.0;
    double render_waited_ms = 0.0;
};

PipelineResult RunPipeline(uint64_t updates, uint32_t lag, uint32_t slots, int writer_ms) {
    PipelineResult res;
    HostRing ring;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    GateBook gate;
    TakeBook book;
    std::mutex render_mutex;
    std::condition_variable render_cv;
    std::vector<uint64_t> render_queue;  // frames handed over, not read back (the hand-over of the next frame comes after "render done")
    bool game_done = false;
    std::vector<uint64_t> written_rows;
    std::mutex written_mutex;
    ring.Open(slots, 512, [&](uint64_t row, const uint8_t*, uint64_t) {
        std::this_thread::sleep_for(Ms(writer_ms));
        std::lock_guard<std::mutex> lock(written_mutex);
        written_rows.push_back(row);
        return true;
    });
    const uint64_t first = 1000;
    book.Begin(first);

    std::thread render([&] {
        uint64_t row = 0;
        for (;;) {
            uint64_t update = 0;
            {
                std::unique_lock<std::mutex> lock(render_mutex);
                render_cv.wait(lock, [&] { return game_done || !render_queue.empty(); });
                if (render_queue.empty()) return;
                update = render_queue.front();
                render_queue.erase(render_queue.begin());
            }
            double waited = 0.0;
            bool stuck = false;
            const HostRing::Slot s = ring.Acquire(10000, &waited, &stuck);  // blocks while the ring is full
            res.render_waited_ms += waited;
            if (s.data) {
                FillFrame(s.data, s.bytes, row);
                ring.Submit(s, row++);
            }
            std::lock_guard<std::mutex> lock(gate_mutex);
            book.OnFrame(update, !s.data);
            ++gate.captured;
            gate_cv.notify_all();
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    HandOverBook handover;
    handover.BeginTake();
    uint64_t update = first;
    for (uint64_t k = 0; k < updates; ++k, ++update) {
        // the gate before the update: wait until the frames handed over are read back, less the lag
        const uint64_t needed = FramesNeeded(handover.Handed(), lag, false);
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate_cv.wait(lock, [&] { return gate.Met(needed); });
        }
        const HandOver h = handover.Note(update, true, first);  // the update ran; its frame is handed over
        {
            // frames handed over and not read back, the one just handed over included
            std::lock_guard<std::mutex> lock(gate_mutex);
            const uint64_t ahead = handover.Handed() - std::min<uint64_t>(handover.Handed(), gate.captured);
            res.max_ahead = std::max(res.max_ahead, ahead);
        }
        std::lock_guard<std::mutex> lock(render_mutex);
        render_queue.push_back(TagUpdate(h.tag));
        render_cv.notify_one();
    }
    // the end gate: wait for every frame
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        const uint64_t needed = FramesNeeded(handover.Handed(), lag, true);
        gate_cv.wait(lock, [&] { return gate.Met(needed); });
    }
    {
        std::lock_guard<std::mutex> lock(render_mutex);
        game_done = true;
        render_cv.notify_all();
    }
    render.join();
    ring.Close();
    res.elapsed_ms = ElapsedMs(t0);
    res.max_in_use = ring.max_in_use();
    res.max_queued = ring.max_queued();
    res.written = ring.written();
    res.verdict = Judge(book, update, handover.Handed());
    return res;
}

void TestPipeline() {
    // lock-step, a 4-slot ring, a writer 4 ms a frame against a render that takes no time: 150 frames
    PipelineResult a = RunPipeline(150, 0, 4, 4);
    Check(a.verdict.exact, "lag 0: the take is exact: " + a.verdict.text);
    CheckU(a.verdict.dropped, 0, "lag 0: nothing dropped");
    CheckU(a.verdict.repeated, 0, "lag 0: nothing repeated");
    CheckU(a.written, 150, "lag 0: every frame reached the writer");
    Check(a.max_ahead <= 1, "lag 0: never more than the one frame just handed over was waiting for its readback: " + std::to_string(a.max_ahead));
    Check(a.max_in_use <= 4, "lag 0: the ring never held more than its four slots: " + std::to_string(a.max_in_use));
    Check(a.elapsed_ms > 150 * 4 * 0.8, "lag 0: the writer set the pace: " + std::to_string(a.elapsed_ms) + " ms");

    // a lag of 2 lets the game run two frames ahead of the readback, never more; the ring still holds only its two slots
    PipelineResult b = RunPipeline(150, 2, 2, 4);
    Check(b.verdict.exact, "lag 2: exact: " + b.verdict.text);
    Check(b.max_ahead <= 3, "lag 2: never more than lag + 1 frames ahead: " + std::to_string(b.max_ahead));
    Check(b.max_in_use <= 2, "lag 2: two slots are all the ring ever held: " + std::to_string(b.max_in_use));
    CheckU(b.written, 150, "lag 2: every frame written");
    Check(b.render_waited_ms > 50.0, "lag 2: the render thread waited for a slot (back-pressure): " + std::to_string(b.render_waited_ms) + " ms");
}

// ---------------------------------------------------------------------------------------------------------------- JSON

struct J {
    char type = 'n';  // n null, b bool, d number, s string, o object, a array
    bool b = false;
    double d = 0.0;
    std::string s;
    std::vector<std::pair<std::string, J>> o;
    std::vector<J> a;
    const J* Get(const std::string& key) const {
        for (const auto& kv : o) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

// A reader as strict as a standard one needs to be for the checks: it rejects trailing commas, bare words, unterminated strings and text after the end.
class JsonReader {
  public:
    explicit JsonReader(const std::string& t) : t_(t) {}
    bool Parse(J* out) {
        Ws();
        if (!Value(out)) return false;
        Ws();
        return p_ == t_.size();
    }

  private:
    void Ws() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\n' || t_[p_] == '\r' || t_[p_] == '\t')) ++p_;
    }
    bool Lit(const char* w) {
        const size_t n = std::strlen(w);
        if (t_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    bool Str(std::string* out) {
        if (p_ >= t_.size() || t_[p_] != '"') return false;
        ++p_;
        while (p_ < t_.size() && t_[p_] != '"') {
            unsigned char c = static_cast<unsigned char>(t_[p_]);
            if (c < 0x20) return false;  // a raw control character is not allowed
            if (c == '\\') {
                ++p_;
                if (p_ >= t_.size()) return false;
                const char e = t_[p_];
                if (e == 'u') {
                    if (p_ + 4 >= t_.size()) return false;
                    unsigned v = 0;
                    for (int i = 1; i <= 4; ++i) {
                        const char h = t_[p_ + i];
                        const bool hex = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
                        if (!hex) return false;
                        v = v * 16 + static_cast<unsigned>(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                    }
                    out->push_back(static_cast<char>(v));  // only the control characters come through here
                    p_ += 4;
                } else if (e == '"' || e == '\\' || e == '/') {
                    out->push_back(e);
                } else if (e == 'n') {
                    out->push_back('\n');
                } else if (e == 'r') {
                    out->push_back('\r');
                } else if (e == 't') {
                    out->push_back('\t');
                } else if (e == 'b' || e == 'f') {
                    out->push_back(e == 'b' ? '\b' : '\f');
                } else {
                    return false;
                }
            } else {
                out->push_back(static_cast<char>(c));
            }
            ++p_;
        }
        if (p_ >= t_.size()) return false;
        ++p_;
        return true;
    }
    bool Value(J* v) {
        if (p_ >= t_.size()) return false;
        const char c = t_[p_];
        if (c == '{') {
            ++p_;
            v->type = 'o';
            Ws();
            if (p_ < t_.size() && t_[p_] == '}') {
                ++p_;
                return true;
            }
            for (;;) {
                Ws();
                std::string key;
                if (!Str(&key)) return false;
                Ws();
                if (p_ >= t_.size() || t_[p_] != ':') return false;
                ++p_;
                Ws();
                J child;
                if (!Value(&child)) return false;
                v->o.emplace_back(std::move(key), std::move(child));
                Ws();
                if (p_ < t_.size() && t_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == '}') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            v->type = 'a';
            Ws();
            if (p_ < t_.size() && t_[p_] == ']') {
                ++p_;
                return true;
            }
            for (;;) {
                Ws();
                J child;
                if (!Value(&child)) return false;
                v->a.push_back(std::move(child));
                Ws();
                if (p_ < t_.size() && t_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == ']') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v->type = 's';
            return Str(&v->s);
        }
        if (Lit("true")) {
            v->type = 'b';
            v->b = true;
            return true;
        }
        if (Lit("false")) {
            v->type = 'b';
            return true;
        }
        if (Lit("null")) {
            v->type = 'n';
            return true;
        }
        const size_t start = p_;
        if (p_ < t_.size() && t_[p_] == '-') ++p_;
        while (p_ < t_.size() && ((t_[p_] >= '0' && t_[p_] <= '9') || t_[p_] == '.' || t_[p_] == 'e' || t_[p_] == 'E' || t_[p_] == '+' || t_[p_] == '-')) ++p_;
        if (p_ == start) return false;
        v->type = 'd';
        v->d = std::strtod(t_.substr(start, p_ - start).c_str(), nullptr);
        return true;
    }
    const std::string& t_;
    size_t p_ = 0;
};

TakeReport SampleReport() {
    TakeReport r;
    r.state = "complete";
    r.name = "harbor-flyby";
    r.started_utc = FormatUtc(1794560701);
    r.finished_utc = FormatUtc(1794560741);
    r.wall_s = 38.25;
    r.has_scene = true;
    r.scene_name = "take-20s";
    r.scene_source = "C:\\mcv\\larecomp\\ports\\scripts\\scenes\\take-20s.scene";
    r.scene_copy = kSceneCopyName;
    r.scene_fnv64 = Hex64(Fnv1a64("scene text", 10));
    r.first_scene_t = 0.016666667;
    r.exe_path = "C:\\mcv\\larecomp\\ports\\exe\\b-m2b\\larecomp.exe";
    r.exe_commit = "3f1755e339dfd8f9b9259c2d0d18514b41e4d4b7";
    r.version = "PATCH 10 - PREVIEW 2";
    r.width = 3840;
    r.height = 2160;
    r.fps = 60.0;
    r.step_ticks = 831250;
    r.step_ms = 16.6666666667;
    r.sink = "ffmpeg";
    r.output = kVideoMkvName;
    r.output_bytes = 5100000000ull;
    r.ffmpeg_exit = 0;
    r.ffmpeg_command = "\"C:\\ffmpeg.exe\" -y -i - \"C:\\takes\\t\\video.mkv\"";
    r.first_update = 7397;
    r.last_update = 8596;
    r.updates_run = 1200;
    r.frames = 1200;
    r.handed = 1200;
    r.exact = true;
    r.verdict = "1200 frames for 1200 updates (7397..8596): exact";
    r.ring = 4;
    r.gpu_buffers = 4;
    r.ring_max_fill = 3;
    r.ring_stalls = 212;
    r.memory_plan_bytes = 265420800;
    r.memory_cap_bytes = 2147483648ull;
    r.ram_free_start_mb = 31000.5;
    r.ram_free_min_mb = 29850.0;
    r.ram_free_end_mb = 30900.0;
    r.state_log = kStateCsvName;
    r.state_rows = 1200;
    r.cvars = {{"mcla_capture_sink", "ffmpeg"}, {"mcla_gpu_resolution_scale", "3"}, {"odd \"key\"", "back\\slash \"quote\"\ttab\n\x01 \xC3\xA9"}};
    return r;
}

void TestJson() {
    CheckEq(JsonEscape("a\"b\\c\n\r\t\x01"), "a\\\"b\\\\c\\n\\r\\t\\u0001", "escapes");
    CheckEq(JsonEscape("\xC3\xA9"), "\xC3\xA9", "UTF-8 passes through");
    CheckEq(JsonEscape("\xE2\x82\xAC \xF0\x9F\x9A\x97"), "\xE2\x82\xAC \xF0\x9F\x9A\x97", "3- and 4-byte UTF-8 pass through");
    CheckEq(JsonEscape("a\xFFz"), "a\\ufffdz", "a byte that is not UTF-8 becomes U+FFFD");
    CheckEq(JsonEscape("ab\xC3"), "ab\\ufffd", "a cut sequence at the end too");
    CheckEq(JsonEscape("\xC0\x80"), "\\ufffd\\ufffd", "an overlong form is not UTF-8");
    CheckEq(JsonEscape("\xED\xA0\x80"), "\\ufffd\\ufffd\\ufffd", "a surrogate is not UTF-8");
    CheckEq(JsonEscape("\xF4\x90\x80\x80"), "\\ufffd\\ufffd\\ufffd\\ufffd", "beyond U+10FFFF is not");

    JsonWriter w;
    w.BeginObject();
    w.Field("a", 1);
    w.Key("b");
    w.BeginArray();
    w.Int(1);
    w.Double(2.5);
    w.Null();
    w.EndArray();
    w.Key("c");
    w.BeginObject();
    w.EndObject();
    w.Field("d", true);
    w.Field("e", std::nan(""));
    w.EndObject();
    const std::string small = w.Take();
    J root;
    Check(JsonReader(small).Parse(&root), "the small document reads back: " + small);
    Check(root.Get("a") && root.Get("a")->d == 1, "a");
    Check(root.Get("b") && root.Get("b")->a.size() == 3 && root.Get("b")->a[1].d == 2.5 && root.Get("b")->a[2].type == 'n', "the array");
    Check(root.Get("c") && root.Get("c")->type == 'o' && root.Get("c")->o.empty(), "the empty object");
    Check(root.Get("d") && root.Get("d")->b, "true");
    Check(root.Get("e") && root.Get("e")->type == 'n', "a number that is not finite is null");

    // the reader is strict (so the checks below mean something)
    J junk;
    Check(!JsonReader("{\"a\": 1,}").Parse(&junk), "the reader refuses a trailing comma");
    Check(!JsonReader("{\"a\": nan}").Parse(&junk), "and nan");
    Check(!JsonReader("{\"a\": \"x\ny\"}").Parse(&junk), "and a raw newline in a string");

    // take.json of a finished take
    const TakeReport rep = SampleReport();
    const std::string text = BuildTakeJson(rep);
    J t;
    Check(JsonReader(text).Parse(&t), "take.json reads back as JSON");
    if (t.type != 'o') return;
    Check(t.Get("take_json_version") && t.Get("take_json_version")->d == kTakeJsonVersion, "version");
    Check(t.Get("state") && t.Get("state")->s == "complete", "state");
    Check(t.Get("name") && t.Get("name")->s == "harbor-flyby", "name");
    Check(t.Get("started_utc") && t.Get("started_utc")->s == "2026-11-13T09:05:01Z", "started_utc: " + (t.Get("started_utc") ? t.Get("started_utc")->s : "?"));
    const J* scene = t.Get("scene");
    Check(scene && scene->type == 'o' && scene->Get("copy") && scene->Get("copy")->s == "scene.scene" && scene->Get("source") &&
              scene->Get("source")->s == rep.scene_source,
          "scene: a path with backslashes survives");
    const J* exe = t.Get("exe");
    Check(exe && exe->Get("commit") && exe->Get("commit")->s == rep.exe_commit, "exe commit");
    const J* pic = t.Get("picture");
    Check(pic && pic->Get("width") && pic->Get("width")->d == 3840 && pic->Get("height")->d == 2160 && pic->Get("fps")->d == 60 &&
              pic->Get("step_ticks")->d == 831250,
          "picture");
    const J* fr = t.Get("frames");
    Check(fr && fr->Get("count")->d == 1200 && fr->Get("updates_run")->d == 1200 && fr->Get("dropped") && fr->Get("dropped")->d == 0 &&
              fr->Get("repeated") && fr->Get("repeated")->d == 0 && fr->Get("exact")->b && fr->Get("first_update")->d == 7397 &&
              fr->Get("last_update")->d == 8596 && fr->Get("frames_file")->s == "frames.csv",
          "frames: count, updates, dropped 0, repeated 0, the map");
    const J* sink = t.Get("sink");
    Check(sink && sink->Get("kind")->s == "ffmpeg" && sink->Get("output")->s == "video.mkv" && sink->Get("ffmpeg_exit")->d == 0 &&
              sink->Get("ffmpeg_command")->s == rep.ffmpeg_command,
          "sink, with the command's quotes");
    const J* mem = t.Get("memory");
    Check(mem && mem->Get("ring_buffers")->d == 4 && mem->Get("gpu_readback_buffers")->d == 4 && mem->Get("plan_bytes")->d == 265420800 &&
              mem->Get("cap_bytes")->d == 2147483648.0 && mem->Get("ram_free_mb_min")->d == 29850.0,
          "memory");
    const J* sl = t.Get("state_log");
    Check(sl && sl->type == 'o' && sl->Get("file")->s == "state.csv" && sl->Get("rows")->d == 1200, "state log");
    const J* cv = t.Get("cvars");
    Check(cv && cv->type == 'o' && cv->o.size() == 3 && cv->Get("mcla_gpu_resolution_scale")->s == "3", "cvars");
    const J* odd = cv ? cv->Get("odd \"key\"") : nullptr;
    Check(odd && odd->s == "back\\slash \"quote\"\ttab\n\x01 \xC3\xA9", "a key and a value full of quotes, backslashes and control characters survive");

    // the take that has just started
    TakeReport run;
    run.name = "t";
    run.has_scene = false;
    run.state_log.clear();
    const std::string running = BuildTakeJson(run);
    J rj;
    Check(JsonReader(running).Parse(&rj), "the running take.json is JSON too");
    Check(rj.Get("state") && rj.Get("state")->s == "running", "state running");
    Check(rj.Get("scene") && rj.Get("scene")->type == 'n', "no scene: null");
    Check(rj.Get("state_log") && rj.Get("state_log")->type == 'n', "no state log: null");
    Check(rj.Get("frames")->Get("frames_file")->s.empty(), "no frames file yet");

    CheckEq(FinalState(true, false), "complete", "exact: complete");
    CheckEq(FinalState(false, false), "inexact", "not exact: inexact");
    CheckEq(FinalState(true, true), "failed", "failed wins");

}

// ---------------------------------------------------------------------------------------------------------------- the state log and the small things

void TestSmall() {
    StateRow r;
    r.frame = 7;
    r.update = 7404;
    r.scene_t = 0.133333;
    r.hour = 17.5f;
    r.time_scale = 1.0f;
    r.dt_ms = 16.6667f;
    r.dt_unscaled_ms = 16.6667f;
    r.paused = false;
    r.world_ready = true;
    r.player = 0x4A2C1000;
    r.x = -2932.9f;
    r.y = 14.3f;
    r.z = 368.4f;
    r.matrix_hash = 0xCBF29CE484222325ull;
    CheckEq(StateCsvHeader(), "frame,update,scene_t,hour,time_scale,dt_ms,dt_unscaled_ms,paused,world_ready,player,x,y,z,matrix_fnv64\n", "the header");
    const std::string row = StateCsvRow(r);
    CheckEq(row, "7,7404,0.133333,17.50000,1.00000,16.66670,16.66670,0,1,4A2C1000,-2932.899902,14.300000,368.399994,cbf29ce484222325\n", "a row");
    size_t commas = 0;
    for (const char c : StateCsvHeader()) commas += c == ',';
    size_t row_commas = 0;
    for (const char c : row) row_commas += c == ',';
    CheckU(row_commas, commas, "a row has the header's columns");

    CheckEq(Hex64(Fnv1a64("", 0)), "cbf29ce484222325", "FNV-1a 64 of nothing");
    CheckEq(Hex64(Fnv1a64("a", 1)), "af63dc4c8601ec8c", "of a");
    CheckEq(Hex64(Fnv1a64("foobar", 6)), "85944171f73967e8", "of foobar");
    const float m1[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
    float m2[16];
    std::memcpy(m2, m1, sizeof(m1));
    CheckU(Fnv1a64(m1, sizeof(m1)), Fnv1a64(m2, sizeof(m2)), "the same matrix hashes the same");
    m2[12] = std::nextafter(5.0f, 6.0f);
    Check(Fnv1a64(m1, sizeof(m1)) != Fnv1a64(m2, sizeof(m2)), "one bit of one float changes the hash");

    CheckEq(FormatUtc(0), "1970-01-01T00:00:00Z", "the epoch");
    CheckEq(FormatUtc(86399), "1970-01-01T23:59:59Z", "the last second of the first day");
    CheckEq(FormatUtc(951782400), "2000-02-29T00:00:00Z", "a leap day");
    CheckEq(FormatUtc(1700000000), "2023-11-14T22:13:20Z", "1.7 billion");
    CheckEq(FormatUtc(4102444800), "2100-01-01T00:00:00Z", "2100 (not a leap year) begins right");
    CheckEq(FormatUtc(-1), "1969-12-31T23:59:59Z", "before the epoch");

    CheckEq(ParseSetSourceSha("ref=feat/debug-xenia-vsync sha=3f1755e339dfd8f9b9259c2d0d18514b41e4d4b7 patches=C:\\x.patch sdk=t2-sb built=2026-11-13T04:08:03"),
            "3f1755e339dfd8f9b9259c2d0d18514b41e4d4b7", "the commit of a SET-SOURCE.txt");
    CheckEq(ParseSetSourceSha("sha=ABCDEF1\n"), "abcdef1", "at the start, upper case made lower");
    CheckEq(ParseSetSourceSha("xsha=abcdef1234"), "", "only a word of its own");
    CheckEq(ParseSetSourceSha("sha=zz"), "", "not hex");
    CheckEq(ParseSetSourceSha("sha=abc12"), "", "too short to be a commit");
    CheckEq(ParseSetSourceSha("ref=a xsha=1234567 sha=89abcdef0"), "89abcdef0", "the second try");
    CheckEq(ParseSetSourceSha(""), "", "nothing");

    Check(IsSecretCvarName("relay_password") && IsSecretCvarName("Discord_Token") && IsSecretCvarName("api_key_x") && IsSecretCvarName("client_secret"),
          "settings that hold secrets");
    Check(!IsSecretCvarName("mcla_capture_sink") && !IsSecretCvarName("mcla_gpu_resolution_scale") && !IsSecretCvarName("window_width"),
          "settings that do not");

    // the sinks
    SinkKind k;
    Check(ParseSinkKind("png", &k) && k == SinkKind::kPng, "png is a sink");
    CheckEq(SinkKindName(SinkKind::kPng), "png", "its name");
    Check(SinkUsesFfmpeg(SinkKind::kFfmpeg) && SinkUsesFfmpeg(SinkKind::kPng) && !SinkUsesFfmpeg(SinkKind::kRaw) && !SinkUsesFfmpeg(SinkKind::kNone),
          "ffmpeg runs for the video and the pictures");
    CheckEq(FfmpegPngCommandLine("C:\\ffmpeg\\ffmpeg.exe", 3840, 2160, true, 60.0, "C:\\t\\frames\\f%06d.png"),
            "\"C:\\ffmpeg\\ffmpeg.exe\" -y -hide_banner -loglevel warning -f rawvideo -pixel_format bgra -video_size 3840x2160 -framerate 60 -i - "
            "-fps_mode passthrough -c:v png -compression_level 3 -pix_fmt rgb24 -start_number 0 \"C:\\t\\frames\\f%06d.png\"",
            "the PNG command line");

    // the take's counts: dropped and repeated
    TakeBook b;
    b.Begin(100);
    for (uint64_t u = 100; u < 110; ++u) b.OnFrame(u, false);
    Verdict v = Judge(b, 110, 10);
    Check(v.exact && v.dropped == 0 && v.repeated == 0, "an exact take drops and repeats nothing");
    TakeBook c;
    c.Begin(100);
    c.OnFrame(100, false);
    c.OnFrame(100, false);  // a repeat
    c.OnFrame(101, false);
    c.OnFrame(104, false);  // 102 and 103 are missing
    c.OnFrame(105, true);   // lost
    v = Judge(c, 106, 6);
    Check(!v.exact, "not exact");
    CheckU(c.Repeated(), 1, "one repeat");
    CheckU(c.Skipped(), 2, "two updates skipped");
    CheckU(v.repeated, 1, "the verdict's repeated");
    CheckU(v.dropped, 3, "the verdict's dropped: six updates, pictures for 100, 101 and 104 only (102 and 103 are missing, 105's was lost)");
}

}  // namespace

int main(int argc, char** argv) {
    TestNames();
    TestLayout();
    TestMemory();
    TestSlotPool();
    TestRingBackPressure();
    TestRingStuck();
    TestRingMisc();
    TestPipeline();
    TestJson();
    TestSmall();
    if (argc > 1) {
        const std::string text = BuildTakeJson(SampleReport());
        if (std::FILE* f = std::fopen(argv[1], "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        } else {
            std::printf("FAILED: cannot write %s\n", argv[1]);
            ++g_failed;
        }
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

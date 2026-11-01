// The game's frame-pacing thread, woken when the main thread is waiting for it.
//
// The engine hands its work to a second guest thread through a ring of three slots per
// context (0x82836814): a word at +0 that the main thread advances when it has written a
// slot, and one at +4 that the other side sets when it has used one. Before the main thread
// writes into the next slot it waits for the ring not to be full (sub_82148A10: up to 1,000
// microseconds of Sleep(0) and clock reads). The slots are freed by one thread only, the loop
// of sub_82144888, and exactly one per round (sub_82160088). Each round ends with a Sleep of
// what is left of 7 ms, in whole milliseconds (sub_821C91B8, which jumps to the Sleep
// native hooked in threading.cpp). So a main thread whose frame takes less than that rhythm
// waits for the next round, and the frame rate is held at about one frame per rhythm however
// fast the CPU and the GPU are.
//
// With pump_wake_on_demand that Sleep is a wait on an event with the same timeout, and the
// main thread's ring wait sets the event when it finds the thread waiting: the round comes at
// once. While the main thread never waits (a frame slower than the rhythm, or a frame rate
// cap below it) nothing sets the event and the thread keeps its rhythm, so a cap behaves as
// before.
//
// Audio is the known risk: a main thread that runs flat out leaves less CPU to the rest, and
// on a small CPU the game's audio ran dry. Two guards:
//  * it stays off below 6 logical processors (the process's affinity mask);
//  * a watchdog on the game's audio mixer thread (sub_823EC8C8), which finishes a frame about
//    every 5.3 ms, and the SDK queues 8 of them (audio_maxqframes). A mixer that finishes
//    none for 40 ms, with a round that came early less than 2 s before, is taken as the audio
//    running dry: the thread goes back to the game's own Sleep for 60 s, and a second time
//    for the rest of the session. The SDK does not count its underruns where this file can
//    read them, so the mixer's pace stands in for them.

#if defined(_WIN32) && !defined(REXGLUE_HAS_XEO3_TARGET)
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../threading.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(pump_wake_on_demand, true, "MCLA/Performance",
    "The game's frame-pacing thread (it frees the draw command ring's slots and sleeps out "
    "7 ms a round) comes round at once when the main thread is waiting for a slot, instead of "
    "after its Sleep. Lifts the frame rate an uncapped game was held to when a frame takes "
    "less than 7 ms; changes nothing under a frame rate cap below that. Needs 6 or more "
    "logical processors, and a watchdog on the audio mixer turns it off if the audio runs "
    "dry. Off is the game's own Sleep.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr unsigned kMinProcessors = 6;
constexpr int64_t kMixerGapNs = 40'000'000;       // the SDK's queue: 8 frames of 5.3 ms
constexpr int64_t kRecentNs = 2'000'000'000;      // a round that came early this recently is blamed
constexpr int64_t kPauseNs = 60'000'000'000;      // the first trip pauses the wake this long

std::atomic<bool> g_waiting{false};         // the freeing thread is inside its wait
std::atomic<int64_t> g_last_early_ns{0};    // its last round that came early (0 = none)
std::atomic<int64_t> g_mixer_ns{0};         // the mixer's last finished frame (0 = none yet)
std::atomic<int64_t> g_paused_until_ns{0};
std::atomic<uint32_t> g_trips{0};           // 1 = paused once, 2 = off for the session

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

HANDLE WakeEvent() {
    static HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset
    return event;
}

bool EnoughProcessors() {
    static const bool ok = [] {
        DWORD_PTR process = 0, system = 0;
        const unsigned n = GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process
                               ? static_cast<unsigned>(__builtin_popcountll(process))
                               : std::thread::hardware_concurrency();
        if (n != 0 && n < kMinProcessors) {
            LARECOMP_APP_INFO("[PumpWake] off: {} logical processors, {} needed", n, kMinProcessors);
        }
        return n == 0 || n >= kMinProcessors;
    }();
    return ok;
}

bool Allowed(int64_t now) {
    return REXCVAR_GET(pump_wake_on_demand) && EnoughProcessors() &&
           g_trips.load(std::memory_order_relaxed) < 2 &&
           now >= g_paused_until_ns.load(std::memory_order_relaxed);
}

}  // namespace

// 0x82148AB0 in sub_82148A10: `li r3,0` ahead of the Sleep(0) in the main thread's wait for a
// free slot. r31 = the context; its byte at +199 picks the ring (rlwinm r8,r11,30,2,28), and
// the freeing thread only frees the first.
void Hook_PumpWakeRingWait(PPCRegister& r31) {
    if (!g_waiting.load(std::memory_order_acquire)) return;
    static const uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (((base[r31.u32 + 199] >> 2) & 0x38) != 0) return;
    SetEvent(WakeEvent());
}

// 0x821449C8 in sub_82144888, after `lwz r3,84(r1)`: r3 is the milliseconds of the Sleep that
// ends the freeing thread's round, and the `bl` after it is jumped over when the wait is done
// here (nothing reads the Sleep's result).
bool Hook_PumpWake(PPCRegister& r3) {
    const uint32_t ms = r3.u32;
    const int64_t now = NowNs();
    if (ms == 0 || !Allowed(now)) return false;

    static std::once_flag announced;
    std::call_once(announced, [] {
        mc::EnableHighResTimer();
        LARECOMP_APP_INFO("[PumpWake] on: the frame-pacing thread comes round when the main thread waits for it");
    });
    ResetEvent(WakeEvent());  // a set from an earlier wait is not this wait's
    g_waiting.store(true, std::memory_order_seq_cst);
    const bool early = WaitForSingleObject(WakeEvent(), ms) == WAIT_OBJECT_0;
    g_waiting.store(false, std::memory_order_seq_cst);
    if (early) {
        if (g_last_early_ns.exchange(NowNs(), std::memory_order_relaxed) == 0) {
            LARECOMP_APP_INFO("[PumpWake] first round that came early");
        }
    }
    return true;
}

// 0x823EC978 in sub_823EC8C8, the `bl KeSetEvent` with which the audio mixer thread reports a
// frame done. A gap of 40 ms or more since the previous one is the audio running dry. It is
// logged (the first 50) with the wake on or off; with the wake on and a round that came early
// within 2 s, it also trips the watchdog.
void Hook_PumpWakeMixerDone() {
    const int64_t now = NowNs();
    const int64_t last = g_mixer_ns.exchange(now, std::memory_order_relaxed);
    if (last == 0) {
        LARECOMP_APP_INFO("[PumpWake] the audio watchdog sees the mixer");
        return;
    }
    if (now - last < kMixerGapNs) return;

    const int64_t gap_ms = (now - last) / 1'000'000;
    const int64_t early = g_last_early_ns.load(std::memory_order_relaxed);
    if (early != 0 && now - early < kRecentNs && Allowed(now)) {
        const uint32_t trips = g_trips.fetch_add(1, std::memory_order_relaxed) + 1;
        if (trips == 1) g_paused_until_ns.store(now + kPauseNs, std::memory_order_relaxed);
        LARECOMP_APP_WARN("[PumpWake] mixer idle {} ms while the thread was coming round early: {}", gap_ms,
                          trips == 1 ? "back to the game's own Sleep for 60 s, off for the session if it happens again"
                                     : "off for the rest of the session");
        return;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 50) {
        LARECOMP_APP_INFO("[PumpWake] mixer idle {} ms", gap_ms);
    }
}
#endif  // _WIN32 && !REXGLUE_HAS_XEO3_TARGET

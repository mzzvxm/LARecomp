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
//
// The ring wait is itself a poll: for up to 1,000 microseconds the main thread calls Sleep(0)
// (a SwitchToThread) and reads the clock and the word at +4 again, then gives up. The freeing
// thread comes round once per rhythm, so a wait often runs its whole length. On a PC with 5 or
// fewer logical processors that poll holds a core the game's other threads need, with the pump
// wake off as well as on. There the wait is a short spin on the word, a sleep on a
// high-resolution timer that the freeing thread ends early (it sets an event right after it
// stores the word: Hook_PumpWakeSlotFreed), and a spin again for the last 400 microseconds,
// since the timer wakes late. The loop stays the game's own: it reads the word and the clock
// after every wait, so its give-up is where it was, and a wake that never comes costs only the
// sleep's length. Bigger CPUs keep the Sleep(0).

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
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

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

const uint8_t* GuestBase() {
    static const uint8_t* base = rex::Runtime::instance()->virtual_membase();
    return base;
}

// The main thread is in its ring wait: if the freeing thread is inside its own wait, it comes
// round now.
void WakePumpIfWaiting(PPCRegister& r31) {
    if (!g_waiting.load(std::memory_order_acquire)) return;
    if (((GuestBase()[r31.u32 + 199] >> 2) & 0x38) != 0) return;
    SetEvent(WakeEvent());
}

// The ring wait sleeps instead of polling (see the top of the file).
bool PollWait() {
    return mc::FewLogicalProcessors();
}

constexpr int64_t kPollSpinNs = 40'000;       // spin on the word before sleeping
constexpr int64_t kPollTailNs = 400'000;      // the timer wakes about 0.3 ms late: spin this long before the deadline
constexpr int64_t kPollMinSleepNs = 200'000;  // a shorter sleep is not worth the two waits
constexpr DWORD kPollMaxSleepMs = 20;         // the timer fires well before; this only bounds a lost wake

// Set by Hook_PumpWakeSlotFreed once the word at +4 is stored.
HANDLE SlotFreedEvent() {
    static HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset
    return event;
}

// One high-resolution timer per waiting thread (null before Windows 10 1803: the game's poll stays).
struct WaitTimer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    ~WaitTimer() {
        if (handle) CloseHandle(handle);
    }
};

// The wait for a free slot, done here. True: the Sleep(0) is skipped and the game's loop reads the
// word and the clock again. False: no timer, the game polls.
bool WaitForSlot(PPCRegister& r31, PPCRegister& r30, PPCRegister& r29, PPCRegister& r26,
                 PPCRegister& r3) {
    static thread_local WaitTimer timer;
    if (!timer.handle) return false;
    const int32_t left_us = static_cast<int32_t>(r26.u32 - r3.u32);
    if (left_us <= 0) return true;  // the loop's next turn sees its deadline
    const uint8_t* base = GuestBase();
    const uint32_t ring = (base[r31.u32 + 199] >> 2) & 0x38;
    const uint32_t* word = reinterpret_cast<const uint32_t*>(base + r30.u32 + 4 + ring);
    const uint32_t expected = __builtin_bswap32(r29.u32);  // guest memory is big-endian
    const auto freed = [&] { return __atomic_load_n(word, __ATOMIC_RELAXED) != expected; };

    const int64_t entered = NowNs();
    const int64_t deadline = entered + static_cast<int64_t>(left_us) * 1000;
    HANDLE slot_freed = SlotFreedEvent();
    ResetEvent(slot_freed);  // before the word is read again: a store after the read sets it

    // 1. A short spin: the word often changes within it when a slot is nearly free.
    int64_t now = entered;
    for (;;) {
        for (int i = 0; i < 16; ++i) {
            if (freed()) return true;
            YieldProcessor();
        }
        now = NowNs();
        if (now - entered >= kPollSpinNs || now >= deadline) break;
    }
    // 2. Asleep until the tail before the deadline, or until the freeing thread stores the word.
    const int64_t sleep_ns = deadline - kPollTailNs - now;
    if (sleep_ns >= kPollMinSleepNs) {
        LARGE_INTEGER due;
        due.QuadPart = -(sleep_ns / 100);  // 100 ns units, relative
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0)) {
            const HANDLE handles[2] = {slot_freed, timer.handle};
            const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, kPollMaxSleepMs);
            if (woke != WAIT_OBJECT_0 + 1) CancelWaitableTimer(timer.handle);
            if (woke == WAIT_OBJECT_0) return true;  // a slot was freed: the loop reads the word
        }
    }
    // 3. The tail: spin until the word changes or the deadline.
    for (;;) {
        for (int i = 0; i < 16; ++i) {
            if (freed()) return true;
            YieldProcessor();
        }
        if (NowNs() >= deadline) return true;
    }
}

}  // namespace

// 0x82148AB0 in sub_82148A10: `li r3,0` ahead of the Sleep(0) in the main thread's wait for a
// free slot. r31 = the context; its byte at +199 picks the ring (rlwinm r8,r11,30,2,28), and
// the freeing thread only frees the first. r30 = 0x82836814, the rings (8 bytes each; the word
// at +4 is the one the freeing thread stores), r29 = that word as the loop last read it, r26 =
// the loop's deadline and r3 = its last clock reading, both in microseconds. It wakes the
// freeing thread if that waits (pump wake). On a PC with few logical processors it also does
// the wait, and returns true: the jump skips the Sleep(0) and goes on at the `lbz` that reads
// the word again.
bool Hook_PumpWakeRingWait(PPCRegister& r31, PPCRegister& r30, PPCRegister& r29, PPCRegister& r26,
                           PPCRegister& r3) {
    WakePumpIfWaiting(r31);
    if (!PollWait()) return false;
    static std::once_flag announced;
    std::call_once(announced, [] {
        LARECOMP_APP_INFO("[PumpWake] the main thread's ring wait sleeps on a timer instead of polling");
    });
    return WaitForSlot(r31, r30, r29, r26, r3);
}

// 0x82160140 in sub_82160088, after `stw r11,4(r6)` (r6 = 0x82836814): the freeing thread has
// stored the word at +4 of the first ring, the only store to it. Wakes a main thread asleep in
// its ring wait.
void Hook_PumpWakeSlotFreed() {
    if (PollWait()) SetEvent(SlotFreedEvent());
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
#elif !defined(REXGLUE_HAS_XEO3_TARGET)
// The wake is a Win32 event; elsewhere the game's own Sleep runs (false = not handled).
#include <rex/ppc.h>
void Hook_PumpWakeRingWait(PPCRegister&) {}
bool Hook_PumpWake(PPCRegister&) { return false; }
void Hook_PumpWakeMixerDone() {}
#endif  // _WIN32 && !REXGLUE_HAS_XEO3_TARGET

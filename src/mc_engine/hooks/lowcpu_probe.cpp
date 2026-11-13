// lowcpu_probe: what the game's audio and its 1 ms clock do on a PC with few cores, in the log.
//
// A player with crackling sound, or a game that stutters when the processor is busy, sends the log;
// this is the part of it that says whether the audio worker and the SDK's timer-queue thread got
// the processor when they needed it. Off by default (lowcpu_probe = false, a row on the debug
// LOGGING page, live). Run with log_level info (the default). Every 10 s two lines:
//
//   [LowCpuProbe] audio ...  the game's audio callback and the guest's tick
//   [LowCpuProbe] cpu ...    CPU time of the audio worker and of the timer-queue thread
//
// What is read, and how:
//  * Audio gaps. The override of rex_sub_823ECA40 runs on the SDK's audio worker once per
//    callback (sub_823F56D0 is a thunk that jumps to it; it is the only caller). The time since
//    the previous call is counted over 8, 12 and 20 ms, and the longest is kept. The output
//    device asks for 480 samples every 10 ms, so about half the gaps are ~10 ms by construction
//    and "over 8 ms" is about half of all calls on a healthy PC: read 12 ms and 20 ms. A starved
//    worker or mixer shows as gaps of 20 ms and more (the output runs dry once the 8 queued
//    frames, 42.7 ms, are used up), usually followed by several calls close together. The
//    always-on counter in audio_mixer.cpp ("[Audio] N gaps over 20 ms") counts the 20 ms ones
//    alone and leaves out pauses and a window that is not in front; this one counts everything.
//  * Tick lateness. KeTimeStampBundle (xboxkrnl ordinal 0xAD) has the guest's 1 ms tick count at
//    +16, written by the SDK's timer-queue thread. At each audio callback the tick is read, then
//    Clock::QueryGuestUptimeMillis() (what the thread writes); the difference is the staleness
//    in whole milliseconds. A tick that is on time reads 0 or 1; 2 and more means the thread was
//    more than one period late. Sampled 187 times a second, not 1000.
//  * CPU time of the audio worker and of the thread named "rex::thread::TimerQueue", by
//    GetThreadTimes (kernel + user; the system charges it by clock tick, so a thread that wakes
//    just after each tick can be undercounted) and by QueryThreadCycleTime divided by the TSC
//    rate measured over the same window (assumes the cycle time counts TSC ticks: compare the
//    two figures), and the cores the whole process used.
//
// It only observes: no priority, affinity or power setting of any thread is changed (the local
// test patch had three such knobs for the timer-queue thread; they stay out).
//
// Cost off: one read of the cvar per audio callback (about 190 a second). Switched on or off while
// the game runs: the 10 s window starts when it is switched on.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(lowcpu_probe, false, "MCLA/Diagnostics",
    "Every 10 s, two lines in the log about a PC with few cores: how late the game's audio callback "
    "and its 1 ms clock tick were (counts of gaps over 8, 12 and 20 ms, clock staleness) and the "
    "CPU time of the audio worker and the timer-queue thread. Only reads. For crackling sound and "
    "stutter when the processor is busy: start the game with it, reproduce the problem, send the "
    "log.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

#if defined(_WIN32)
#include <rex/chrono/clock.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include "larecomp_log.h"

REX_EXTERN(__imp__rex_sub_823ECA40);  // the audio callback's body, called by sub_823F56D0

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int64_t kReportSeconds = 10;

// Counters since the last report. The audio worker adds, the reporter reads and clears.
struct Window {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> gaps8{0}, gaps12{0}, gaps20{0};
    std::atomic<uint64_t> max_gap_us{0};
    std::atomic<uint64_t> ticks{0};
    std::atomic<uint64_t> stale2{0}, stale3{0}, stale5{0}, stale10{0};
    std::atomic<uint64_t> stale_sum{0};
    std::atomic<uint64_t> max_stale{0};

    void Clear() {
        calls.store(0);
        gaps8.store(0);
        gaps12.store(0);
        gaps20.store(0);
        max_gap_us.store(0);
        ticks.store(0);
        stale2.store(0);
        stale3.store(0);
        stale5.store(0);
        stale10.store(0);
        stale_sum.store(0);
        max_stale.store(0);
    }
};

Window g_win;
std::atomic<HANDLE> g_audio_thread{nullptr};
std::atomic<uint32_t> g_bundle{0};          // guest address of KeTimeStampBundle, 0 = not known
std::atomic<bool> g_reporter_running{false};

void RaiseMax(std::atomic<uint64_t>& max, uint64_t value) {
    uint64_t current = max.load(std::memory_order_relaxed);
    while (value > current &&
           !max.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

uint32_t FindBundle() {
    rex::Runtime* runtime = rex::Runtime::instance();
    auto* resolver = runtime ? runtime->export_resolver() : nullptr;
    auto* entry = resolver ? resolver->GetExportByOrdinal("xboxkrnl.exe", 0x00AD) : nullptr;
    return entry ? entry->variable_ptr : 0;
}

// A thread of this process by its description, opened for query; nullptr if none.
HANDLE FindThread(const wchar_t* wanted, DWORD* tid_out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    HANDLE found = nullptr;
    const DWORD me = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && !found; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != me) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) {
            continue;
        }
        PWSTR description = nullptr;
        const bool match = SUCCEEDED(GetThreadDescription(h, &description)) && description &&
                           std::wcscmp(description, wanted) == 0;
        if (description) {
            LocalFree(description);
        }
        if (match) {
            found = h;
            *tid_out = te.th32ThreadID;
        } else {
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return found;
}

// CPU time of one thread: GetThreadTimes (kernel + user, 100 ns) and QueryThreadCycleTime.
bool ReadThread(HANDLE h, uint64_t* times_100ns, uint64_t* cycles) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!h || !GetThreadTimes(h, &created, &exited, &kernel, &user)) {
        return false;
    }
    *times_100ns = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                   ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
    using QueryThreadCycleTimeFn = BOOL(WINAPI*)(HANDLE, PULONG64);
    static const auto fn = reinterpret_cast<QueryThreadCycleTimeFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryThreadCycleTime")));
    ULONG64 value = 0;
    *cycles = (fn && fn(h, &value)) ? value : 0;
    return true;
}

uint64_t ProcessTimes100ns() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0;
    }
    return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
           ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}

struct Watch {
    HANDLE handle = nullptr;
    DWORD tid = 0;
    uint64_t prev_times = 0;
    uint64_t prev_cycles = 0;
    double times_ms = 0.0;   // this window, by GetThreadTimes
    double cycles_ms = 0.0;  // this window, by cycle time over the TSC rate

    // The baseline the next Update measures from.
    void Rebase() {
        uint64_t times = 0, cycles = 0;
        if (handle && ReadThread(handle, &times, &cycles)) {
            prev_times = times;
            prev_cycles = cycles;
        }
    }

    void Update(double tsc_per_ms) {
        uint64_t times = 0, cycles = 0;
        if (!ReadThread(handle, &times, &cycles)) {
            times_ms = cycles_ms = 0.0;
            return;
        }
        times_ms = double(times - prev_times) / 10000.0;
        cycles_ms = tsc_per_ms > 0.0 ? double(cycles - prev_cycles) / tsc_per_ms : 0.0;
        prev_times = times;
        prev_cycles = cycles;
    }
};

// Runs while the cvar is on; ends when it is turned off (the next audio callback that sees it on
// starts it again).
void Reporter() {
    SetThreadDescription(GetCurrentThread(), L"LowCpuProbe");
    Watch audio;
    audio.handle = g_audio_thread.load();
    audio.tid = audio.handle ? GetThreadId(audio.handle) : 0;
    Watch queue;

    LARECOMP_APP_INFO("[LowCpuProbe] started: KeTimeStampBundle at guest 0x{:08X}, audio worker "
                      "thread {}; lines every {} s",
                      g_bundle.load(), audio.tid, kReportSeconds);

    // The first window starts now: what the audio worker counted before is dropped.
    g_win.Clear();
    audio.Rebase();
    SteadyClock::time_point prev_time = SteadyClock::now();
    uint64_t prev_tsc = __rdtsc();
    uint64_t prev_process = ProcessTimes100ns();

    while (REXCVAR_GET(lowcpu_probe)) {
        if (!queue.handle) {
            queue.handle = FindThread(L"rex::thread::TimerQueue", &queue.tid);
            queue.Rebase();
        }

        // In steps of a second so that turning the probe off is seen soon.
        for (int64_t s = 0; s < kReportSeconds && REXCVAR_GET(lowcpu_probe); ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!REXCVAR_GET(lowcpu_probe)) {
            break;
        }

        const SteadyClock::time_point now = SteadyClock::now();
        const uint64_t tsc = __rdtsc();
        const double window_ms =
            std::chrono::duration<double, std::milli>(now - prev_time).count();
        const double tsc_per_ms = window_ms > 0.0 ? double(tsc - prev_tsc) / window_ms : 0.0;
        const uint64_t process = ProcessTimes100ns();
        const double process_cores =
            window_ms > 0.0 ? (double(process - prev_process) / 10000.0) / window_ms : 0.0;
        prev_time = now;
        prev_tsc = tsc;
        prev_process = process;

        const uint64_t calls = g_win.calls.exchange(0);
        const uint64_t gaps8 = g_win.gaps8.exchange(0);
        const uint64_t gaps12 = g_win.gaps12.exchange(0);
        const uint64_t gaps20 = g_win.gaps20.exchange(0);
        const uint64_t max_gap_us = g_win.max_gap_us.exchange(0);
        const uint64_t ticks = g_win.ticks.exchange(0);
        const uint64_t stale2 = g_win.stale2.exchange(0);
        const uint64_t stale3 = g_win.stale3.exchange(0);
        const uint64_t stale5 = g_win.stale5.exchange(0);
        const uint64_t stale10 = g_win.stale10.exchange(0);
        const uint64_t stale_sum = g_win.stale_sum.exchange(0);
        const uint64_t max_stale = g_win.max_stale.exchange(0);

        LARECOMP_APP_INFO("[LowCpuProbe] audio {:.1f} s: {} callbacks ({:.0f}/s), gaps over 8 ms "
                          "{}, over 12 ms {}, over 20 ms {}, longest {:.1f} ms | tick staleness "
                          "(whole ms, 0 and 1 are on time): {} samples, 2 ms or more {}, 3 or "
                          "more {}, 5 or more {}, 10 or more {}, longest {} ms, mean {:.2f} ms",
                          window_ms / 1000.0, calls, window_ms > 0.0 ? calls * 1000.0 / window_ms : 0.0,
                          gaps8, gaps12, gaps20, double(max_gap_us) / 1000.0, ticks, stale2,
                          stale3, stale5, stale10, max_stale,
                          ticks ? double(stale_sum) / double(ticks) : 0.0);

        audio.Update(tsc_per_ms);
        queue.Update(tsc_per_ms);
        const double pct = window_ms > 0.0 ? 100.0 / window_ms : 0.0;
        LARECOMP_APP_INFO("[LowCpuProbe] cpu {:.1f} s: process {:.2f} cores | audio worker "
                          "(thread {}): {:.1f} ms by times, {:.1f} ms by cycles ({:.2f}% of a "
                          "core) | TimerQueue (thread {}, priority {}): {:.1f} ms by times, "
                          "{:.1f} ms by cycles ({:.2f}% of a core) | TSC {:.0f} MHz",
                          window_ms / 1000.0, process_cores, audio.tid, audio.times_ms,
                          audio.cycles_ms, audio.cycles_ms * pct, queue.tid,
                          queue.handle ? GetThreadPriority(queue.handle) : 0, queue.times_ms,
                          queue.cycles_ms, queue.cycles_ms * pct, tsc_per_ms / 1000.0);
    }

    if (queue.handle) {
        CloseHandle(queue.handle);
    }
    LARECOMP_APP_INFO("[LowCpuProbe] stopped");
    g_reporter_running.store(false);
}

// Runs on the audio worker, once per callback.
void OnAudioCallback(const uint8_t* base) {
    static thread_local int64_t last_us = 0;
    static std::once_flag found;
    std::call_once(found, [] {
        g_bundle.store(FindBundle());
        // A handle of its own: the pseudo handle GetCurrentThread() would mean the reporter's
        // thread when the reporter uses it.
        HANDLE self = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                                 FALSE, GetCurrentThreadId());
        g_audio_thread.store(self);
    });
    if (!g_reporter_running.exchange(true)) {
        std::thread(Reporter).detach();
        last_us = 0;  // the gap across the time the probe was off is not a gap
    }

    // The tick first, then the time: the time can only be the later of the two.
    const uint32_t bundle = g_bundle.load(std::memory_order_relaxed);
    uint32_t tick = 0;
    if (bundle) {
        uint32_t raw = 0;
        std::memcpy(&raw, base + bundle + 16, sizeof(raw));
        tick = __builtin_bswap32(raw);
    }
    const uint32_t now_ms = rex::chrono::Clock::QueryGuestUptimeMillis();

    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               SteadyClock::now().time_since_epoch()).count();
    g_win.calls.fetch_add(1, std::memory_order_relaxed);
    if (last_us) {
        const uint64_t gap_us = static_cast<uint64_t>(now_us - last_us);
        if (gap_us > 8000) g_win.gaps8.fetch_add(1, std::memory_order_relaxed);
        if (gap_us > 12000) g_win.gaps12.fetch_add(1, std::memory_order_relaxed);
        if (gap_us > 20000) g_win.gaps20.fetch_add(1, std::memory_order_relaxed);
        RaiseMax(g_win.max_gap_us, gap_us);
    }
    last_us = now_us;

    if (bundle) {
        const int32_t diff = static_cast<int32_t>(now_ms - tick);
        const uint64_t stale = diff > 0 ? static_cast<uint64_t>(diff) : 0;
        g_win.ticks.fetch_add(1, std::memory_order_relaxed);
        g_win.stale_sum.fetch_add(stale, std::memory_order_relaxed);
        if (stale >= 2) g_win.stale2.fetch_add(1, std::memory_order_relaxed);
        if (stale >= 3) g_win.stale3.fetch_add(1, std::memory_order_relaxed);
        if (stale >= 5) g_win.stale5.fetch_add(1, std::memory_order_relaxed);
        if (stale >= 10) g_win.stale10.fetch_add(1, std::memory_order_relaxed);
        RaiseMax(g_win.max_stale, stale);
    }
}

}  // namespace

// The body of the audio callback that sub_823F56D0 jumps to; runs on the SDK's audio worker.
extern "C" REX_FUNC(rex_sub_823ECA40) {
    if (REXCVAR_GET(lowcpu_probe)) {
        OnAudioCallback(base);
    }
    __imp__rex_sub_823ECA40(ctx, base);
}
#endif  // _WIN32
#endif  // !REXGLUE_HAS_XEO3_TARGET

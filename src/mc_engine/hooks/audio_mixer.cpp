// The game's own audio threads: a host priority for each.
//
// How the game's audio reaches the speakers (read from the recompiled code): the SDK's audio
// worker (a host thread named "Audio Worker") runs the game's callback once per 5.33 ms frame.
// The callback is sub_823F56D0 (sub_823F3718 passes it to XAudioRegisterRenderDriverClient); it
// loads the audio object and jumps to sub_823ECA40, which hands the frame to a guest thread that
// mixes it (about 0.7 ms of CPU on a fast PC) and waits for the answer. That thread is created
// by sub_823ECC60 with ExCreateThread, start address 0x823EC8C8, and its body is
// rex_sub_823EC8C8. The game asks for priority +15 for it (KeSetBasePriorityThread), which the
// SDK ignores (ignore_thread_priorities, default true), so the thread runs at the host's normal
// priority like the render, GPU and job threads, and so does the audio worker. When those take
// every core the mixer waits to run, or the worker does, and the output runs dry. The worker
// does almost nothing itself (it sets the mixer's event and waits) but it has to be on a CPU
// soon after the device frees a slot.
//
// audio_mixer_priority gives both threads a host priority when they start. The mixer is found
// by its start address: the override of its body below runs on it. The worker is found by the
// callback it runs: the SDK calls the game's callback on that thread only, so the first call of
// the override of sub_823F56D0 is on it. Neither is found by handle (the handle depends on how
// many kernel objects the game made before it).
//   0 normal (the game's own behaviour), 1 above normal, 2 highest,
//   3 highest and registered with the Windows multimedia scheduler ("Pro Audio"),
//  -1 automatic: 3 on a PC with 5 or fewer logical processors, else 0.
// Takes effect at the next start. Whatever the setting, one line in larecomp.log per thread
// says that it started and what was done to it. Windows only: elsewhere the threads are left
// as they are.
//
// The same override counts audio dropouts. The callback runs for each 5.33 ms frame, but the
// output device takes audio in 10 ms steps, so about half of the gaps between two calls are
// ~10 ms on a healthy PC. A gap well over that means the worker or the mixer was not run in
// time; the device has 8 frames (42.7 ms) queued, and the sound breaks once they run out. Each
// call is timed; a gap over 20 ms is counted and the longest is kept. Nothing is logged per
// call. About once a minute, and only if a gap was counted in that minute, one info line says
// how many ("[Audio] N gaps over 20 ms in the last 60 s, longest X ms"); one more line
// ("[Audio] session: ...") is logged when the game closes (LogAudioDropoutSummary). Left out:
// the first 10 s after the first call (the game is still starting), and a gap with the game
// paused or the window not in front at either end of it. A load is not left out (nothing here
// tells one), so a gap that comes with a load is counted. The counters are written by the audio
// worker alone, with no lock; the closing line reads them with atomics.

#if defined(_WIN32) && !defined(REXGLUE_HAS_XEO3_TARGET)
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#include <windows.h>
#include <avrt.h>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "mc_engine/adaptive_triggers.h"

REXCVAR_DEFINE_INT32(audio_mixer_priority, -1, "MCLA/Audio",
    "Host priority of the game's two audio threads: the guest thread that mixes each 5.33 ms "
    "frame (the game's request for a higher priority is ignored) and the SDK's audio worker "
    "that calls the game for each frame: 0 = normal, 1 = above normal, 2 = highest, 3 = highest "
    "and registered with the Windows multimedia scheduler (\"Pro Audio\"), -1 = automatic: 3 on "
    "a PC with 5 or fewer logical processors, 0 otherwise. Takes effect at the next start.")
    .range(-1, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__rex_sub_823EC8C8);  // the mixer thread's body
REX_EXTERN(__imp__sub_823F56D0);      // the game's audio callback, run by the SDK's audio worker

namespace {

constexpr DWORD kSmallPcLogicalProcessors = 5;

// The level to apply, from the setting and the PC.
int AudioLevel(DWORD logical) {
    const int setting = REXCVAR_GET(audio_mixer_priority);
    if (setting >= 0) {
        return setting;
    }
    return logical <= kSmallPcLogicalProcessors ? 3 : 0;
}

// Applies the setting to the calling thread and logs one line about it; `who` starts the line.
void ApplyToThisThread(const char* who) {
    const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const int level = AudioLevel(logical);
    HANDLE self = GetCurrentThread();
    // The name the SDK gave the thread, if any: it shows that the right thread was found.
    char name[64] = "?";
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(self, &description)) && description) {
        if (WideCharToMultiByte(CP_UTF8, 0, description, -1, name, int(sizeof(name)), nullptr,
                                nullptr) <= 0) {
            name[0] = '?';
            name[1] = '\0';
        }
        LocalFree(description);
    }
    if (level == 0) {
        LARECOMP_APP_INFO("{}, priority left alone (audio_mixer_priority {}, {} logical "
                          "processors); thread \"{}\"",
                          who, REXCVAR_GET(audio_mixer_priority), logical, name);
        return;
    }
    const BOOL set = SetThreadPriority(
        self, level >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
    const DWORD set_error = set ? 0 : GetLastError();
    // The multimedia scheduler registration lasts until the thread ends; both threads run for
    // the whole session, so it is never reverted.
    DWORD task = 0;
    HANDLE mmcss = level >= 3 ? AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) : nullptr;
    const DWORD mmcss_error = (level >= 3 && !mmcss) ? GetLastError() : 0;
    LARECOMP_APP_INFO("{}, level {} (audio_mixer_priority {}, {} logical processors): "
                      "SetThreadPriority {} (error {}), now {}; multimedia scheduler {} (error "
                      "{}); thread \"{}\"",
                      who, level, REXCVAR_GET(audio_mixer_priority), logical,
                      set ? "ok" : "failed", set_error, GetThreadPriority(self),
                      level >= 3 ? (mmcss ? "ok" : "failed") : "not asked", mmcss_error, name);
}

// --- The dropout counter ---------------------------------------------------------------------

constexpr int64_t kStartupIgnoreUs = 10'000'000;  // after the first call of the callback
constexpr int64_t kDropoutGapUs = 20'000;
constexpr int64_t kReportEveryUs = 60'000'000;

// Since the start, for the closing line. The audio worker is the only writer.
struct SessionCounts {
    std::atomic<bool> counting{false};      // past the first 10 s
    std::atomic<uint64_t> gaps{0};          // gaps over 20 ms
    std::atomic<uint64_t> longest_us{0};    // the longest of those
    std::atomic<uint64_t> measured_us{0};   // time between calls that were counted
};

SessionCounts g_session;
std::atomic<bool> g_summary_logged{false};

// Called first thing in each call of the callback, on the audio worker.
void CountCallbackGap() {
    static thread_local int64_t first_us = 0;         // time of the first call
    static thread_local int64_t previous_us = 0;      // time of the previous call
    static thread_local bool previous_quiet = true;   // paused or in the background then
    static thread_local int64_t window_start_us = 0;  // start of the minute being counted
    static thread_local uint64_t window_gaps = 0;
    static thread_local uint64_t window_longest_us = 0;

    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t gap_us = now_us - previous_us;
    previous_us = now_us;
    if (!first_us) {
        first_us = now_us;
        return;
    }
    if (now_us - first_us < kStartupIgnoreUs) {
        return;
    }

    const bool quiet =
        larecomp::triggers::IsGamePaused() || !larecomp::triggers::IsWindowInFront();
    const bool gap_is_quiet = quiet || previous_quiet;
    previous_quiet = quiet;
    if (!window_start_us) {
        // The first call past the startup: the gap before it is the startup's.
        window_start_us = now_us;
        g_session.counting.store(true, std::memory_order_relaxed);
        return;
    }

    if (!gap_is_quiet) {
        g_session.measured_us.store(
            g_session.measured_us.load(std::memory_order_relaxed) + uint64_t(gap_us),
            std::memory_order_relaxed);
        if (gap_us > kDropoutGapUs) {
            ++window_gaps;
            if (uint64_t(gap_us) > window_longest_us) window_longest_us = uint64_t(gap_us);
            g_session.gaps.fetch_add(1, std::memory_order_relaxed);
            if (uint64_t(gap_us) > g_session.longest_us.load(std::memory_order_relaxed)) {
                g_session.longest_us.store(uint64_t(gap_us), std::memory_order_relaxed);
            }
        }
    }

    if (now_us - window_start_us >= kReportEveryUs) {
        if (window_gaps) {
            LARECOMP_APP_INFO("[Audio] {} gaps over 20 ms in the last {} s, longest {:.1f} ms",
                              window_gaps, (now_us - window_start_us + 500'000) / 1'000'000,
                              double(window_longest_us) / 1000.0);
        }
        window_start_us = now_us;
        window_gaps = 0;
        window_longest_us = 0;
    }
}

}  // namespace

// The closing line of the dropout counter; once, from any thread.
void LogAudioDropoutSummary() {
    if (g_summary_logged.exchange(true) || !g_session.counting.load(std::memory_order_relaxed)) {
        return;
    }
    const uint64_t gaps = g_session.gaps.load(std::memory_order_relaxed);
    const double minutes = double(g_session.measured_us.load(std::memory_order_relaxed)) / 6e7;
    if (gaps) {
        LARECOMP_APP_INFO("[Audio] session: {} gaps over 20 ms in {:.1f} min counted, longest "
                          "{:.1f} ms",
                          gaps, minutes,
                          double(g_session.longest_us.load(std::memory_order_relaxed)) / 1000.0);
    } else {
        LARECOMP_APP_INFO("[Audio] session: no gap over 20 ms in {:.1f} min counted", minutes);
    }
    rex::FlushLogging();  // the window's close ends the process without flushing
}

// Runs on the mixer thread, once, as it starts.
extern "C" REX_FUNC(rex_sub_823EC8C8) {
    ApplyToThisThread("[AudioMixer] mixer thread started (entry 823EC8C8)");
    __imp__rex_sub_823EC8C8(ctx, base);
}

// Runs on the SDK's audio worker once per frame; acts on the first call on each thread (the
// worker is one thread for the whole session, so that is once), and times every call.
extern "C" REX_FUNC(sub_823F56D0) {
    static thread_local bool applied = false;
    if (!applied) {
        applied = true;
        ApplyToThisThread(
            "[AudioWorker] audio worker thread: first call of the callback (823F56D0)");
    }
    CountCallbackGap();
    __imp__sub_823F56D0(ctx, base);
}
#else
void LogAudioDropoutSummary() {}
#endif  // _WIN32 && !REXGLUE_HAS_XEO3_TARGET

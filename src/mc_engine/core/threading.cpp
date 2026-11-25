#ifndef REXGLUE_HAS_XEO3_TARGET
#include "threading.h"

#include "logging.h"

#include "settings/larecomp_settings.h"
#include "os/cpu_affinity_rules.h"
#include "os/thread_yield.h"
#include <rex/ppc/hook.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/title_runtime.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <timeapi.h>
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <filesystem>
#include <set>
#endif

namespace mc {

// Windows only. Linux has nothing to ask for: its timers are high resolution (a sleeper wakes within the 50 us timer slack
// of its deadline) and no process-wide setting makes them coarser, so the guest's Sleep there needs neither this nor the
// spin after the sleep (Sleep_hook below, os/thread_yield.h).
void EnableHighResTimer() {
#if defined(_WIN32)
    static std::once_flag s_init;
    std::call_once(s_init, [] {
        // Since Windows 11 a process none of whose windows is visible
        // (minimized, fully covered, display off) loses its timeBeginPeriod
        // request, and every 1 ms sleep in the frame limiter turns into up to
        // 15.6 ms. Ask to keep the requested resolution regardless.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0;  // controlled bit cleared: do not ignore our request
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state,
                                   sizeof(state))) {
            // Windows 10 does not know the flag and does not need it.
            MC_INFO("[threading] timer-resolution throttling opt-out unavailable ({})",
                    GetLastError());
        }
        timeBeginPeriod(1);
        MC_INFO("[threading] high-res timer enabled");
    });
#endif
}

void DisableHighResTimer() {
#if defined(_WIN32)
    timeEndPeriod(1);
    MC_INFO("[threading] high-res timer disabled");
#endif
}

void LimitLogicalProcessors(unsigned requested) {
    if (requested == 0) return;
#if defined(_WIN32)
    const HANDLE process = GetCurrentProcess();
    DWORD_PTR allowed = 0, system = 0, mask = 0;
    if (!GetProcessAffinityMask(process, &allowed, &system)) {
        MC_WARN("[threading] cpu_limit_lp={}: GetProcessAffinityMask failed ({})",
                requested, GetLastError());
        return;
    }
    if (allowed == 0) {
        MC_WARN("[threading] cpu_limit_lp={}: the process has no single-group affinity mask", requested);
        return;
    }
    const unsigned selected = larecomp::os::SelectFirstLogicalProcessors(
        requested, sizeof(allowed) * 8,
        [&](unsigned cpu) { return (allowed & (DWORD_PTR{1} << cpu)) != 0; },
        [&](unsigned cpu) { mask |= DWORD_PTR{1} << cpu; });
    if (!SetProcessAffinityMask(process, mask)) {
        MC_WARN("[threading] cpu_limit_lp={}: SetProcessAffinityMask failed ({})",
                requested, GetLastError());
        return;
    }
#elif defined(__linux__)
    cpu_set_t allowed, mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        MC_WARN("[threading] cpu_limit_lp={}: sched_getaffinity failed ({})", requested, errno);
        return;
    }
    const unsigned selected = larecomp::os::SelectFirstLogicalProcessors(
        requested, CPU_SETSIZE, [&](unsigned cpu) { return CPU_ISSET(cpu, &allowed); },
        [&](unsigned cpu) { CPU_SET(cpu, &mask); });
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        MC_WARN("[threading] cpu_limit_lp={}: sched_setaffinity failed ({})", requested, errno);
        return;
    }
    // Linux applies affinity per thread. The startup thread's future children inherit its mask.
    // Set it on existing SDK threads too, including any children they start during this pass.
    // CHECK: verify every task's Cpus_allowed_list on Linux after startup with the cap enabled.
    std::set<pid_t> seen;
    std::error_code scan_error;
    int affinity_error = 0;
    bool found_new;
    do {
        found_new = false;
        for (std::filesystem::directory_iterator it("/proc/self/task", scan_error), end;
             !scan_error && it != end; it.increment(scan_error)) {
            const auto name = it->path().filename().native();
            const char* end_name = name.data() + name.size();
            pid_t tid;
            const auto parsed = std::from_chars(name.data(), end_name, tid);
            if (parsed.ec != std::errc{} || parsed.ptr != end_name || !seen.insert(tid).second) continue;
            found_new = true;
            if (sched_setaffinity(tid, sizeof(mask), &mask) != 0 && errno != ESRCH) {
                affinity_error = errno;
            }
        }
    } while (found_new && !scan_error);
    if (scan_error || affinity_error != 0) {
        MC_WARN("[threading] cpu_limit_lp={}: startup thread limited to {} LP, some existing threads "
                "could not be restricted (task scan {}, sched_setaffinity {})",
                requested, selected, scan_error.value(), affinity_error);
        return;
    }
#else
    MC_WARN("[threading] cpu_limit_lp={}: process affinity is unavailable on this platform", requested);
    return;
#endif
#if defined(_WIN32) || defined(__linux__)
    MC_INFO("[threading] cpu_limit_lp={}: game limited to {} logical processors", requested, selected);
#endif
}

unsigned UsableLogicalProcessors() {
#if defined(_WIN32)
    const unsigned active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    DWORD_PTR process = 0, system = 0;
    if (active <= 64 && GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        unsigned allowed = 0;
        for (DWORD_PTR m = process; m; m &= m - 1) ++allowed;
        return allowed < active ? allowed : active;
    }
    return active;
#elif defined(__linux__)
    // The calling thread's affinity mask (a game started with taskset, or inside a cgroup with a
    // cpuset, has only those); every online one when the mask can not be read.
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int allowed = CPU_COUNT(&set);
        if (allowed > 0) return unsigned(allowed);
    }
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? unsigned(online) : 0;
#else
    return 0;
#endif
}

bool FewLogicalProcessors() {
    static const bool few = [] {
        const unsigned logical = UsableLogicalProcessors();  // 0 where it is not counted: the waits stay as they are
        const bool small = logical != 0 && logical <= 5;
        MC_INFO("[threading] {} logical processors: {}", logical,
                small ? "the spin waits sleep instead" : "the spin waits are unchanged");
        return small;
    }();
    return few;
}

bool FourOrFewerLogicalProcessors() {
    static const bool few = [] {
        const unsigned logical = UsableLogicalProcessors();  // 0 where it is not counted: nothing is lowered
        return logical != 0 && logical <= 4;
    }();
    return few;
}

} // namespace mc

// ---------------------------------------------------------------------------
// PPC kernel bypass hooks
// ---------------------------------------------------------------------------

#if defined(_WIN32)

// Sleep (0x8244FEC0)
//
// This used to spin the entire remainder of every sleep on YieldProcessor,
// re-reading steady_clock::now() each iteration. With ms == 1 it never slept
// at all: SwitchToThread, then a full millisecond of spinning. Since
// steady_clock::now() is QueryPerformanceCounter, a single guest thread
// looping on Sleep(1) pinned a whole core. Measured on the thread that sat at
// 99.7% of a core for an entire session: 68% of its samples in
// RtlQueryPerformanceCounter and 25% in this hook, doing no game work.
//
// timeBeginPeriod(1) is already set process-wide, so sleep_for is accurate to
// about a millisecond on its own and the spin was buying very little. The tail
// spin is kept but bounded, so a late wake-up is still tightened up without
// turning a sleep into a busy-wait. MCLA_SLEEP_SPIN=1 restores the old
// unbounded behaviour for comparison.
constexpr auto kMaxSleepSpin = std::chrono::microseconds(300);

u32 Sleep_hook(u32 ms) {
    mc::EnableHighResTimer();

    if (uint32_t(ms) == 0) {
        SwitchToThread();
        return 0;
    }

    static const bool legacy_spin = [] {
        const char* e = std::getenv("MCLA_SLEEP_SPIN");
        return e && *e == '1';
    }();

    const auto target = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(uint32_t(ms));

    if (legacy_spin) {
        if (uint32_t(ms) >= 2) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(uint32_t(ms)) - std::chrono::microseconds(1500));
        } else {
            SwitchToThread();
        }
        while (std::chrono::steady_clock::now() < target)
            YieldProcessor();
        return 0;
    }

    // Sleep for all but a short tail, then spin at most kMaxSleepSpin to land
    // on the deadline. A 1 ms request sleeps rather than spinning.
    const auto lead = std::chrono::milliseconds(uint32_t(ms)) - kMaxSleepSpin;
    if (lead > std::chrono::microseconds(0)) std::this_thread::sleep_for(lead);

    const auto spin_deadline = std::chrono::steady_clock::now() + kMaxSleepSpin;
    while (std::chrono::steady_clock::now() < target) {
        if (std::chrono::steady_clock::now() >= spin_deadline) break;
        YieldProcessor();
    }

    return 0;
}
REX_HOOK(mc_Sleep, Sleep_hook);

#elif defined(__linux__)

// Sleep (0x8244FEC0), Linux. The guest's original is `li r4,0; b SleepEx`, which ends in the SDK's
// KernelThread::Delay (KeDelayExecutionThread). What Linux needs of it:
//   * Sleep(0) is a yield, as SwitchToThread is on Windows. Delay turns a zero interval into a 100 us
//     sleep for every thread whose priority is at or below the host's "below normal" value, which is
//     -1 on Windows but 8 on POSIX, and a guest thread's own priority is 0 unless the game changes
//     it: so each Sleep(0) cost 100 us plus the timer slack instead of a sched_yield
//     (LINUX-AUDIT-V011 R2; the SDK side is LINUX-V011-SDK's). The guest polls with Sleep(0) (the
//     main thread's ring wait at 0x82148AB0 and others).
//   * Sleep(n) is a nanosleep to the deadline. Delay does the same after a sched_yield; what the
//     Windows hook adds is not needed here: no timeBeginPeriod (hrtimers), and the bounded spin after
//     the sleep that the 1 ms Windows tick asked for would burn a core of the Deck's four for nothing.
//   * What Delay does after the wait is kept: a thread asked to stop, or to suspend, does so here
//     (KernelThread::CheckStop). A suspend the SDK could not carry out at once on a POSIX thread
//     is taken at the thread's next check, and a thread that only polls with Sleep would never
//     reach another one.
// CHECK: not run on Linux. Measure on the Deck: Sleep(0) and Sleep(n) calls per second in a race
// (the load-wait census) and the frame time, with and without the hook.
u32 Sleep_hook(u32 ms) {
    if (ms == 0) {
        larecomp::os::YieldThread();
    } else {
        larecomp::os::SleepFor(std::chrono::milliseconds(ms));
    }
    rex::kernel::KernelThread::CheckStop();
    return 0;
}
REX_HOOK(mc_Sleep, Sleep_hook);

#endif  // _WIN32 / __linux__

// The two hooks below do nothing the OS has a say in, so they are the same on every OS.

// BadassBaboon's Recomp Adjustments: hardware cache flush bypass with memory barrier.
//
// FlushDataCache (0x821D5510), signature (addr, size, flush): walks the range
// one 128-byte line at a time issuing `dcbf 0, r11` (flush=1) or `dcbst 0, r11`
// (flush=0), then `blr` with r3 untouched.
//
// On x86_64 host caches are coherent, but this call serves as a critical publication
// point across audio/worker threads (XMA Decoder & Audio Worker). A single seq_cst
// memory fence maintains inter-thread visibility while skipping ~540,000 emulated
// loop operations per second.
u32 FlushDataCache_hook(u32 addr, u32 size, u32 flush) {
    (void)size;
    (void)flush;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return addr;  // r3 is the guest's own return value here
}
REX_HOOK(mc_FlushDataCache, FlushDataCache_hook);

// ResumeThread (0x8244FE58)
u32 ResumeThread_hook(u32 handle) {
    auto* runtime = REX_RUNTIME();
    if (!runtime) return 0;
    auto thread = runtime->GetThreadRegistry().FindByHandle(handle);  // a pin: the thread cannot go away under Resume
    if (thread)
        thread->Resume();
    return 0;
}
REX_HOOK(mc_ResumeThread, ResumeThread_hook);

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

#endif // REXGLUE_HAS_XEO3_TARGET

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "threading.h"

#include "logging.h"

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/system/xthread.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <timeapi.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#endif

namespace mc {

void EnableHighResTimer() {
#if defined(_WIN32)
    static std::once_flag s_init;
    std::call_once(s_init, [] {
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

} // namespace mc

// ---------------------------------------------------------------------------
// PPC kernel bypass hooks (Windows only)
// ---------------------------------------------------------------------------

#if defined(_WIN32) && !defined(REXGLUE_HAS_XEO3_TARGET)

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
    auto thread = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XThread>(handle);
    if (thread)
        thread->Resume();
    return 0;
}
REX_HOOK(mc_ResumeThread, ResumeThread_hook);

#endif // _WIN32

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

#endif // REXGLUE_HAS_XEO3_TARGET

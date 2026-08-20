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
u32 Sleep_hook(u32 ms) {
    mc::EnableHighResTimer();

    if (uint32_t(ms) == 0) {
        SwitchToThread();
        return 0;
    }

    auto target = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(uint32_t(ms));

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

// The game's physics job threads: a host priority below the other threads' on a small PC.
//
// What the threads are (read from the recompiled code; the slow-frame sampler's stacks agree).
// The engine has a pool class for tasks: sub_821C4A70 makes a pool, sub_821C4740 starts one
// thread per hardware-thread bit of the pool's mask, and the body of every pool thread is
// sub_821C4528 (called by the engine's thread entry sub_821C91C8, which the trampoline
// sub_82460F08 starts). The body waits on the pool's semaphore, takes the next task off the
// pool's ring and calls it. sub_82131790 makes four pools at startup, each thread named
// "<pool>:<hardware thread>" (sub_821C46A8):
//   "[RAGE] Physics Task"   3 threads (mask 0xE)
//   "[RAGE] rmOcclude Task" 2 threads (mask 0xA)
//   "[MC4] Ambient Task"    1 thread  (mask 0x2)
//   "[RAGE] Race Line Task" 2 threads (mask 0xA)
// The game asks for priority -1 for the physics pool (-2 for the race line pool) with
// KeSetBasePriorityThread (sub_821C9308), which the SDK ignores (ignore_thread_priorities,
// default true): all of them run at the host's normal priority.
//
// The physics pool's task is sub_82589D98 (the game's name for it is "ProcessPairListTask").
// sub_8258B210 queues 6 of them from the guest thread that runs sub_8258D9F0; that thread then
// builds the list of pairs and, at the end, waits for the 6 tasks in a blocking kernel wait
// (sub_8258D5F0, sub_821C4338). The tasks do not block: each loops, taking the guest spin lock
// sub_821C4278 (a compare-and-swap under the SDK's global lock, sub_821CE0A0) to look at the
// list, until the list is marked complete. So while the thread that fills the list is slow, the
// three workers spin. In 178 slow frames of a 4-core run they went from 11% to 60-70% of a
// core each while the main thread fell from 90% to 35% and the render thread from 85% to 32%.
//
// On a PC with 4 or fewer logical processors each physics pool thread gets
// the below-normal intent of os/thread_priority.h when it starts (THREAD_PRIORITY_BELOW_NORMAL
// on Windows, a nice value 5 above the process's own on Linux, which an unprivileged process may
// always do), so the main, render and audio threads take a core first and the workers fill what
// is left. The thread that fills the list blocks, not spins, on the workers, so it is not held
// up by their lower priority. Elsewhere nothing changes. The other pools are left as they are.
// One line in larecomp.log per pool thread says what was done to it.

#if !defined(REXGLUE_HAS_XEO3_TARGET)
#include <rex/ppc.h>

#include <cstdint>
#include <cstring>

#include "larecomp_log.h"
#include "mc_engine/core/threading.h"
#include "os/thread_priority.h"

REX_EXTERN(__imp__rex_sub_821C4528);  // the body of every task pool thread

namespace {

// The start of the physics pool's thread names; the rest is ":<hardware thread>".
constexpr char kPhysicsPoolName[] = "[RAGE] Physics Task";
constexpr uint32_t kNameBytes = 32;  // the name field at the start of the thread's slot

// Whether a physics pool thread gets a lower priority on this PC.
bool LowerPhysicsThreads() {
    return mc::FourOrFewerLogicalProcessors();
}

// Applies the priority to the calling thread, if it is one of the physics pool's, and logs one
// line about the thread. `slot` is the guest address of the thread's slot (the argument of the
// body), which starts with the thread's name.
void ApplyToThisThread(const uint8_t* base, uint32_t slot) {
    char name[kNameBytes + 1];
    std::memcpy(name, base + slot, kNameBytes);
    name[kNameBytes] = '\0';
    const bool physics = std::strncmp(name, kPhysicsPoolName, sizeof(kPhysicsPoolName) - 1) == 0;
    const unsigned logical = mc::UsableLogicalProcessors();
    if (!physics || !LowerPhysicsThreads()) {
        LARECOMP_APP_INFO("[JobWorker] pool thread \"{}\" started, priority left alone ({}, {} "
                          "logical processors)",
                          name, physics ? "physics pool" : "not the physics pool", logical);
        return;
    }
    const larecomp::os::ThreadResult result =
        larecomp::os::SetCurrentThreadIntent(larecomp::os::ThreadIntent::kBelowNormal);
    LARECOMP_APP_INFO("[JobWorker] physics pool thread \"{}\" started ({} logical processors): {}",
                      name, logical, result.Text());
}

}  // namespace

// Runs on each pool thread, once, as it starts, and does not return until the pool ends.
extern "C" REX_FUNC(rex_sub_821C4528) {
    static thread_local bool applied = false;
    if (!applied) {
        applied = true;
        ApplyToThisThread(base, ctx.r3.u32);
    }
    __imp__rex_sub_821C4528(ctx, base);
}
#endif  // !REXGLUE_HAS_XEO3_TARGET

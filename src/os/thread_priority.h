#pragma once
// ===========================================================================
// Thread priorities for every OS: what a thread is for, not which API sets it
// ===========================================================================
// The game's own threads, the renderer's workers and the helper threads ask for one of a few
// intents; this file turns the intent into what the OS can do.
//
//   kBackground    work nobody ever waits for (a feature's pipelines made behind the frame, the shader
//                  pack's compiler workers). The game's threads come first on every core.
//   kBelowNormal   work that may be waited for now and then (pipeline workers, texture decodes, the
//                  physics job pool on a 4-thread PC, the profiler's sampler): behind the game's
//                  threads, but never starved to a standstill.
//   kNormal        the thread's own baseline (the process's, on Linux).
//   kAboveNormal   a little ahead of the game's threads.
//   kHigh          well ahead of them (the frame generation pacer).
//   kAudio         the game's audio threads: as high as the OS lets an ordinary program go.
//
// Windows: exactly the calls the game made before this file (SetThreadPriority with BELOW_NORMAL,
// NORMAL, ABOVE_NORMAL, HIGHEST; kAudio adds the multimedia scheduler's "Pro Audio" task, as the
// audio mixer did). kBackground and kBelowNormal are the same value there: every site that asks
// for either ran at BELOW_NORMAL already, and nothing about Windows changes.
//
// Linux, as an ordinary user, with no rtkit (the Steam Deck runs games under gamescope that way):
//   * Lowering is always allowed and is what does the work: a nice value above the process's own
//     (setpriority on the thread's own tid; Linux nice is per thread) and, for kBackground,
//     SCHED_BATCH, which also stops the thread from pre-empting the game's threads when it wakes.
//     SCHED_IDLE is not used: a frame can end up waiting for a worker (a draw that may not be
//     skipped waits for its pipeline), and an idle-class thread on a CPU that every other thread
//     keeps busy can wait for as long as they do, which is a hitch, not a saving.
//   * A lowering is ONE WAY for an unprivileged process: the kernel refuses to give back a nice
//     value or SCHED_IDLE without CAP_SYS_NICE or an RLIMIT_NICE that allows it (SCHED_BATCH to
//     SCHED_OTHER is allowed). A thread that is raised again later (the host precache's builder)
//     must be lowered with Lowering::kRaisable, which on Linux changes the policy only.
//   * Raising (kAboveNormal, kHigh, kAudio: a negative nice value) is tried once per call. It works
//     with RLIMIT_NICE (limits.conf, a systemd unit's LimitNICE) or CAP_SYS_NICE; otherwise the
//     result says so, ONE line is logged for the process, and the thread stays as it was. Nothing
//     in the game depends on it. rtkit (D-Bus) is not used: the Deck has none, and a D-Bus client
//     is a dependency this one call does not earn. SCHED_RR / SCHED_FIFO are not used either: a
//     real-time thread that spins takes a core from everything else.
//   * Other OSes: nothing is done.
//
// Threading: every function is safe from any thread. SetCurrentThreadIntent acts on the caller;
// SetThreadIntent on a std::thread another thread started (Windows: the whole priority; Linux:
// the scheduling policy only, because a nice value needs the thread's own id).
// ===========================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace larecomp::os {

enum class ThreadIntent : uint8_t { kBackground, kBelowNormal, kNormal, kAboveNormal, kHigh, kAudio };

// How a lowered thread may be treated later (matters on Linux only; see the header comment).
enum class Lowering : uint8_t {
  kOneWay,     // the thread stays at its level, or goes lower
  kRaisable,   // a later SetThreadIntent(kNormal) must work without privileges
};

// What an intent means on each OS, as plain numbers so that the mapping can be read and tested
// without either OS's headers. The .cpp checks the Windows values against the macros.
struct ThreadPlan {
  int windows_priority;    // the THREAD_PRIORITY_* value: -1 below normal, 0 normal, 1 above, 2 highest
  bool windows_scheduler;  // also join the Windows multimedia scheduler ("Pro Audio")
  bool linux_batch;        // SCHED_BATCH instead of the thread's own policy
  int linux_nice;          // added to the process's own nice value (+ is lower priority)
};

constexpr ThreadPlan PlanFor(ThreadIntent intent, Lowering lowering = Lowering::kOneWay) {
  const bool one_way = lowering == Lowering::kOneWay;
  switch (intent) {
    case ThreadIntent::kBackground:
      return {-1, false, true, one_way ? 10 : 0};
    case ThreadIntent::kBelowNormal:
      return {-1, false, false, one_way ? 5 : 0};
    case ThreadIntent::kNormal:
      return {0, false, false, 0};
    case ThreadIntent::kAboveNormal:
      return {1, false, false, -5};
    case ThreadIntent::kHigh:
      return {2, false, false, -10};
    case ThreadIntent::kAudio:
      return {2, true, false, -11};
  }
  return {0, false, false, 0};
}

// What happened. `detail` is one log-ready phrase for the priority part, `scheduler_detail` for
// the multimedia scheduler (empty off Windows); Text() joins them as the audio mixer's log line
// always did.
struct ThreadResult {
  bool set = false;              // everything asked for was done
  unsigned error = 0;            // the OS error of the first failure (GetLastError, errno); 0 when set
  int now = 0;                   // Windows: GetThreadPriority; Linux: the thread's nice value; kUnknown
  bool scheduler_asked = false;  // Windows: the multimedia scheduler was asked for
  bool scheduler_ok = false;
  unsigned scheduler_error = 0;
  std::string detail;
  std::string scheduler_detail;

  static constexpr int kUnknown = -1000;

  std::string Text(bool with_scheduler = false) const {
    return with_scheduler && !scheduler_detail.empty() ? detail + "; " + scheduler_detail : detail;
  }
};

// The calling thread.
ThreadResult SetCurrentThreadIntent(ThreadIntent intent, Lowering lowering = Lowering::kOneWay);

// A thread another thread started and has not joined (nothing is done to a thread that is not
// joinable). Windows: the same priority as for the calling thread, without the multimedia
// scheduler (that is a call a thread makes about itself). Linux: the scheduling policy only.
ThreadResult SetThreadIntent(std::thread& thread, ThreadIntent intent,
                             Lowering lowering = Lowering::kOneWay);

// The calling thread's name for a log line: Windows the description the SDK gave it, Linux the
// kernel's name (15 characters); "?" when there is none.
std::string CurrentThreadName();

// The one-per-process lines (a refused raise on Linux) go through this callback; the default
// writes to stderr. The app installs its own once, early. Returns the previous callback.
using LogCallback = std::function<void(const std::string& line)>;
LogCallback SetLogCallback(LogCallback callback);

}  // namespace larecomp::os

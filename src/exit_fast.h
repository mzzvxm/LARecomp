#pragma once
//
// How the process ends once the orderly shutdown is done (EXIT-HARD, 2026-11-22): at once, without the C runtime's exit, so that none of the
// static destructors of the exe (and, on Windows, none of the DLL detach routines of the SDK's libraries) runs.
//
// WHY. The exe ends two times in the same way for years: LarecompMain returns the SDK's exit code, the C runtime runs every atexit handler and
// static destructor, then ExitProcess detaches every DLL. The SDK never force-kills a guest thread that does not leave within 200 ms
// (TerminateTitle leaves "stragglers" running), so a thread of the game can still call into the exe, or into an SDK library, while those
// destructors run. Three exit crashes came from it, each fixed for one object: the change-callback table (1b9761f2, Mtx_lock), the settings
// table (d64f264d, larecomp::settings::Find, 2 ms after "Process exiting normally") and, in the 2026-11-18 watchdog path, the graphics
// module's static in rexgpu.dll (process_exit.h). Every global and function-local static with a destructor is the same exposure, hundreds
// of them. The class is closed here instead: everything that has to be written has been written by the orderly shutdown (saves finished,
// settings and logs flushed, the renderer and audio stopped), so the process ends before any of those destructors can run.
//
// WHAT RUNS FIRST (and so cannot be lost):
//   1. the flush hooks (AddFlush): writers of the exe that keep data in memory or in a stream that its own destructor would have written
//      (the state log's trailer, the free camera's recording). They run once, in the order they were added, on the calling thread.
//   2. the note hook (SetNote): "Process exiting normally ..." and the log's flush (crash_handler.cpp NoteProcessExiting).
//   3. fflush(nullptr): every open C stream (the profiler's, the timing logs', the capture files').
//   4. EndProcessNow (process_exit.h): TerminateProcess on Windows, _Exit on Linux; neither runs an atexit handler or a static destructor,
//      and the exit code is the one given.
// The close watchdog (larecomp_app.cpp; close_watchdog.h: 5 s of a close with no progress, 8 s inside a driver call, 12 s at the most) is the other way out: when the orderly shutdown hung it ends the process through
// EndAfterHang (below), which runs the hooks that have not run on a helper thread within a bound and ends with kWatchdogExitCode, not 0.
// What does NOT need a hook: the settings file (config_save.cpp writes it to a temporary file and renames it before the menu returns), the
// career save (MclaWaitForSaves before the title ends, then the SDK closes the packages in rex::Stop), the log (the SDK's logger is
// synchronous: rex::log::Flush() has written every sink when it returns), every file a function opens, writes and closes in one go.
//
// The switch is mcla_fast_exit (debug only, default on). Arm(false) makes Finish return the code, and the process ends the old way (the flush
// hooks still run first: the statics that used to finish those files in their destructors are never destroyed any more, in either mode).
//
// No SDK in this header (larecomp_exit_fast_test builds it alone, and runs a child process through a real exit).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <system_error>
#include <thread>

#include "process_exit.h"
#include "settings/immortal.h"

namespace larecomp::exitfast {

using Hook = void (*)();

// What RunFlushes did, for the log line of the caller.
struct Report {
  int ran = 0;        // hooks that ran
  int threw = 0;      // hooks that threw (the exception is dropped: the exit goes on)
  long long ms = 0;   // time they took together
};

namespace detail {

constexpr int kMaxHooks = 32;

struct Slot {
  const char* name = nullptr;
  Hook hook = nullptr;
  bool done = false;
};

// Made on first use and never destroyed: a straggler thread may call AddFlush or Finish while the process ends (immortal.h).
struct State {
  std::mutex mutex;
  Slot slots[kMaxHooks];
  int count = 0;
  Hook note = nullptr;
  std::atomic<bool> armed{true};
  std::atomic<bool> ending{false};
};

inline State& S() {
  static larecomp::Immortal<State> s;
  return *s;
}

}  // namespace detail

// Adds a function that must have run before the process ends without static destruction. False when the table is full (32) or the hook is
// null; the same function twice is kept once. Call it from start-up code; any thread may.
inline bool AddFlush(const char* name, Hook hook) {
  if (!hook) return false;
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  for (int i = 0; i < s.count; ++i) {
    if (s.slots[i].hook == hook) return true;
  }
  if (s.count >= detail::kMaxHooks) return false;
  s.slots[s.count].name = name;
  s.slots[s.count].hook = hook;
  s.slots[s.count].done = false;
  ++s.count;
  return true;
}

// The hook that logs "Process exiting normally" and flushes the log: it runs after the flush hooks, whichever way the process ends here.
inline void SetNote(Hook hook) {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.note = hook;
}

// The switch. On at the start (the default of mcla_fast_exit); the app sets it from the setting before the SDK shuts down.
inline void Arm(bool on) { detail::S().armed.store(on, std::memory_order_release); }
inline bool Armed() { return detail::S().armed.load(std::memory_order_acquire); }

// Runs the hooks that have not run yet, in the order they were added. A hook that throws is counted and the rest still run.
inline Report RunFlushes() {
  detail::State& s = detail::S();
  Report report;
  const auto started = std::chrono::steady_clock::now();
  for (int i = 0;; ++i) {
    Hook hook = nullptr;
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (i >= s.count) break;
      if (s.slots[i].done) continue;
      s.slots[i].done = true;
      hook = s.slots[i].hook;
    }
    try {
      hook();
      ++report.ran;
    } catch (...) {
      ++report.threw;
    }
  }
  report.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  return report;
}

// Ends the process now with `code`, after the flush hooks, the note hook and fflush(nullptr). Never returns. A second thread that gets here
// while the first is flushing waits (the first one ends the process); after 10 s it ends the process itself without the flushes.
[[noreturn]] inline void EndNow(int code) {
  detail::State& s = detail::S();
  if (s.ending.exchange(true, std::memory_order_acq_rel)) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    larecomp::EndProcessNow(code);
  }
  (void)RunFlushes();
  Hook note = nullptr;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    note = s.note;
  }
  if (note) {
    try {
      note();
    } catch (...) {
    }
  }
  std::fflush(nullptr);
  larecomp::EndProcessNow(code);
}

// The status of a process that the close watchdog ended (EndAfterHang): the orderly shutdown did not finish in time. Not 0 (a normal close; the watchdog
// used to end with 0, which hid the hang), not 3 (the crash paths: std::terminate, abort, ...), not crash::kNoDialogExitCode: a log or a chain tells them
// apart by it. 124 is also the status the `timeout` command gives a command it had to stop.
inline constexpr int kWatchdogExitCode = 124;

namespace detail {

// What the watcher thread and its helper share. Made on the heap and never freed: the helper may still run when the watcher ends the process.
struct HangShared {
  std::mutex mutex;  // private to the two threads, held for an instant: the watcher never waits on a lock any other thread may hold
  std::condition_variable cv;
  bool done = false;
};

}  // namespace detail

// What the close watchdog does when the shutdown has hung (close_watchdog.h, larecomp_app.cpp): the flush hooks that have not run, within a bound, then the
// process ends with `code`. Never returns, and never waits for longer than `max_ms`.
//
// WHY A HELPER THREAD. The watchdog fires because some thread is stuck, and it may be stuck holding a lock that a hook, a log line or a stream flush
// needs (the state log's mutex, the logger's sink, a C stream's lock). So nothing that can take such a lock runs on the watcher: `before` (the log line),
// the hooks, `after` (the log's flush) and fflush(nullptr) run one after the other on a detached helper thread, and the watcher only waits for it on a
// condition variable of their own, at most `max_ms`. If the helper is stuck, the watcher ends the process anyway, with what was done up to then.
// A hook the hung thread is running right now is not run again and not waited for (RunFlushes marks a hook as taken before it calls it, and holds no lock
// during the call); the hooks behind it do run. The note hook does NOT run: "Process exiting normally" would say the opposite of what happened.
// If the helper cannot be started the process ends at once.
[[noreturn]] inline void EndAfterHang(int code, int max_ms, Hook before = nullptr, void (*after)(const Report&) = nullptr) {
  auto* shared = new detail::HangShared();
  try {
    std::thread([shared, before, after]() {
      try {
        if (before) before();
        const Report report = RunFlushes();
        if (after) after(report);
      } catch (...) {
      }
      std::fflush(nullptr);  // the last: the one that can wait on a stream's lock a stuck thread holds
      {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->done = true;
      }
      shared->cv.notify_all();
    }).detach();
    std::unique_lock<std::mutex> lock(shared->mutex);
    shared->cv.wait_for(lock, std::chrono::milliseconds(max_ms > 0 ? max_ms : 0), [shared] { return shared->done; });
  } catch (...) {
  }
  larecomp::EndProcessNow(code);
}

// What main does with the SDK's exit code: the process ends here when the switch is on, and the code comes back when it is off.
// The flush hooks run in both cases (EXIT-REVIEW): the exe's global objects are made immortal (immortal.h) whatever the switch says, so their
// destructors never run, not even through the C runtime's exit of the "off" path, and a writer that finished its file there (the state log's
// trailer, the free camera's recording) depends on its hook in both.
inline int Finish(int code) {
  if (Armed()) EndNow(code);
  (void)RunFlushes();
  return code;
}

}  // namespace larecomp::exitfast

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
// What does NOT need a hook: the settings file (config_save.cpp writes it to a temporary file and renames it before the menu returns), the
// career save (MclaWaitForSaves before the title ends, then the SDK closes the packages in rex::Stop), the log (the SDK's logger is
// synchronous: rex::log::Flush() has written every sink when it returns), every file a function opens, writes and closes in one go.
//
// The switch is mcla_fast_exit (debug only, default on). Arm(false) makes Finish return the code, and the process ends the old way.
//
// No SDK in this header (larecomp_exit_fast_test builds it alone, and runs a child process through a real exit).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
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

// What main does with the SDK's exit code: the process ends here when the switch is on, and the code comes back when it is off.
inline int Finish(int code) {
  if (Armed()) EndNow(code);
  return code;
}

}  // namespace larecomp::exitfast

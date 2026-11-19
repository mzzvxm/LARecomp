#pragma once
// ===========================================================================
// Giving up the CPU and sleeping, for every OS
// ===========================================================================
// What the guest's Sleep (mc_engine/core/threading.cpp) needs from the OS, as two calls that mean the same everywhere.
//
//   YieldThread   the rest of the calling thread's time slice goes to another thread that wants the core; returns at once when none
//                 does. Windows SwitchToThread, Linux sched_yield. This is the guest's Sleep(0).
//   SleepFor      the calling thread sleeps for at least `duration`. Windows: std::this_thread::sleep_for (the process asks for a 1 ms
//                 timer, mc::EnableHighResTimer). Linux: clock_nanosleep on CLOCK_MONOTONIC to an absolute deadline, taken up again
//                 after a signal: the SDK suspends and alerts guest threads with real-time signals, which end a plain sleep early.
//
// The timer on Linux: hrtimers wake a sleeper within the timer slack (50 us by default) of the deadline, there is no process-wide
// resolution to ask for as on Windows (timeBeginPeriod), and so no spin tail after a sleep is needed there.
// Every function is safe from any thread.
// ===========================================================================

#include <chrono>

namespace larecomp::os {

void YieldThread();

void SleepFor(std::chrono::nanoseconds duration);

}  // namespace larecomp::os

#pragma once
// ===========================================================================
// Thread QoS: tell the OS that the game's threads are the foreground work
// ===========================================================================
// On a hybrid CPU (Intel 12th gen and later: performance + efficiency cores; some AMD laptop chips
// with two core types) Windows 11 chooses a core class for every thread from its Quality of Service
// level, its priority and whether its process is in front. A process that Windows judges "not in
// front" gets EcoQoS-like treatment (efficient cores, lower clocks). perf/HYBRID-QOS.md has the
// whole survey and the Microsoft sources; this file is the small part that was built from it.
//
// Levels (debug cvar `thread_qos`, read at start; -1 is automatic):
//   0  off      nothing is asked of the OS here (the 1 ms timer opt-out of mc::EnableHighResTimer
//               stays: it is a different call and always on).
//   1  process  while the game's window has focus the process is tagged HighQoS
//               (SetProcessInformation ProcessPowerThrottling, EXECUTION_SPEED taken over and
//               left off). When the window loses focus the control is handed back to Windows
//               (its own rules: audible or visible still counts for something).
//   2  threads  level 1, and the game's critical threads (host render thread, the audio threads,
//               the frame generation pacer) are tagged HighQoS for the whole session
//               (SetThreadInformation ThreadPowerThrottling), so they stay HighQoS while the
//               window is out of focus (recording, a second monitor). On a hybrid CPU the audio
//               threads also join the multimedia scheduler ("Pro Audio") whatever the number of
//               logical processors: audio_mixer_priority -1 reads WantsAudioScheduler().
//   -1 auto     level 1 where the OS has the call (Windows 10 1709 and later), 0 elsewhere.
//
// What is NOT done, on purpose: no hard affinity, no ideal processor, no CPU sets (on a hybrid CPU
// they take the choice away from a scheduler that knows more than the game does), no EcoQoS for
// any thread (a thread that "nobody waits for" has been waited for before: HYBRID-QOS.md 3.4), no
// priority class. Linux: nothing is done here; the kernel has no EcoQoS and an unprivileged
// process cannot raise a thread, so there is nothing to ask for (os/thread_priority.h has the
// nice values).
//
// The decision (DecideQos) and the call sequences (PlanProcessQos) are plain numbers and need no
// OS header: thread_qos_test.cpp checks them. The calls themselves are in thread_qos.cpp.
// ===========================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace larecomp::os {

// The values of the Windows constants (the .cpp checks them against the SDK's macros).
inline constexpr uint32_t kQosExecutionSpeed = 0x1;         // PROCESS/THREAD_POWER_THROTTLING_EXECUTION_SPEED
inline constexpr uint32_t kQosIgnoreTimerResolution = 0x4;  // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
inline constexpr unsigned kFirstBuildWithExecutionSpeed = 16299;  // Windows 10 1709

// What the decision looks at.
struct QosHost {
  bool windows = false;
  unsigned windows_build = 0;  // 0 when it could not be read: the call is tried and may fail
  bool hybrid = false;         // more than one efficiency class among the logical processors
};

struct QosDecision {
  int level = 0;                  // 0, 1 or 2 (the setting resolved)
  bool process = false;           // tag the process HighQoS while its window has focus
  bool threads = false;           // tag the critical threads HighQoS
  bool audio_scheduler = false;   // the audio threads' automatic level is "Pro Audio" on this PC
  const char* why = "";           // one phrase for the log
};

// setting: the cvar (-1 automatic, 0 off, 1 process, 2 process and threads; anything else is read
// as -1).
constexpr QosDecision DecideQos(int setting, const QosHost& host) {
  if (!host.windows) {
    return {0, false, false, false, "not Windows: the OS has no QoS call and the scheduler is left alone"};
  }
  if (host.windows_build != 0 && host.windows_build < kFirstBuildWithExecutionSpeed) {
    return {0, false, false, false, "this Windows is older than 10 1709: no execution speed control"};
  }
  if (setting == 0) {
    return {0, false, false, false, "setting 0: off, Windows decides as it always did"};
  }
  if (setting == 2) {
    return {2, true, true, host.hybrid,
            host.hybrid ? "setting 2: process and critical threads, audio on the multimedia scheduler"
                        : "setting 2: process and critical threads (no hybrid CPU: audio rule unchanged)"};
  }
  if (setting == 1) {
    return {1, true, false, false, "setting 1: process while the window has focus"};
  }
  return {1, true, false, false, "automatic: process while the window has focus"};
}

// One SetProcessInformation(ProcessPowerThrottling) call: ControlMask and StateMask.
struct QosCall {
  uint32_t control = 0;
  uint32_t state = 0;
};

// What to ask for when the window gains or loses focus: ONE call.
// A SetProcessInformation(ProcessPowerThrottling) call REPLACES the whole control mask (it does not add to it): a second call with control 0x4 after one with control 0x1
// leaves control 0x4, the execution speed control is gone again and the process is not HighQoS. The first version of this plan made that mistake (two calls, 0x1 then 0x4;
// thread_qos_test read back "control 0x4" on a Windows 11 build 26300 laptop, 2026-11-24), so level 1 did nothing. One call carries every mechanism the game controls.
struct ProcessQosPlan {
  QosCall calls[2];
  int count = 0;
};

// focused: take over execution speed and leave it off (HighQoS), and keep the timer resolution opt-out (always honour the requests): control 0x5, state 0.
// not focused: hand execution speed back to Windows (not in the control mask: "system managed") and keep the timer resolution opt-out, so that the 1 ms timer still
// survives a minimised window (HYBRID-QOS.md 3.2): control 0x4, state 0 = exactly what mc::EnableHighResTimer set before this file did anything.
constexpr ProcessQosPlan PlanProcessQos(bool focused) {
  ProcessQosPlan plan{};
  plan.calls[0] = {focused ? (kQosExecutionSpeed | kQosIgnoreTimerResolution) : kQosIgnoreTimerResolution, 0};
  plan.count = 1;
  return plan;
}

// ---------------------------------------------------------------------------------------------
// The calls
// ---------------------------------------------------------------------------------------------

// Reads the host (OS build, efficiency classes) and returns what it found as one log phrase in
// `detail` ("Windows build 26100; 20 logical processors, hybrid: efficiency class 1 x12, class 0 x8";
// Linux: the cpu_core / cpu_atom lists when the kernel shows them).
QosHost DetectQosHost(std::string* detail = nullptr);

// Decides from the setting and the host, remembers the decision for the rest of the session, and
// applies the process part for a window that has focus (the game was just started by the player).
// Returns the log lines (decision, what the OS said). Call once, early, from the main thread,
// after mc::EnableHighResTimer.
std::vector<std::string> StartQos(int setting);

// The focus handler (larecomp::uievents::AddFocusHandler takes this signature, but logging is the
// caller's: this returns the log line, empty when nothing was asked of the OS).
std::string ApplyQosFocus(bool focused);

// Tags the calling thread HighQoS when the decision says threads. `role` names it in the line
// that is returned (empty when the decision does not ask for it, or off Windows). Any thread.
std::string TagCurrentThreadHighQos(const char* role);

// The decision StartQos made (all zero before it, or when it decided against).
QosDecision CurrentQosDecision();

// audio_mixer_priority -1: whether this PC takes the multimedia scheduler even above 6 logical
// processors (a hybrid CPU at setting 2).
bool WantsAudioScheduler();

}  // namespace larecomp::os

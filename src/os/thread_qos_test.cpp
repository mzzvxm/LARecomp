// Standalone check of the thread QoS decision (os/thread_qos.h): which setting turns on what for which
// host, the call sequences for gaining and losing focus, and, on Windows, what the OS says about the
// process afterwards. No game, no SDK.
//
//   cmake --build <dir> --target larecomp_thread_qos_test, then run it (exit 0 = passed).
//   Or by hand:  clang++ -std=c++23 -I src src/os/thread_qos_test.cpp src/os/thread_qos.cpp
//
// What it can not say: whether HighQoS changes where a hybrid CPU puts a thread (that is a laptop
// measurement, perf/HYBRID-QOS.md section 5). The live part changes this test process's own
// throttling state, nothing else.

#include "os/thread_qos.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace os = larecomp::os;

namespace {

int g_failed = 0;
int g_checked = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checked;                                                          \
    if (!(cond)) {                                                        \
      ++g_failed;                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                     \
  } while (0)

os::QosHost WindowsHost(unsigned build, bool hybrid) {
  os::QosHost host;
  host.windows = true;
  host.windows_build = build;
  host.hybrid = hybrid;
  return host;
}

// --- the decision ----------------------------------------------------------------------------

void TestDecision() {
  // Not Windows: nothing, whatever the setting or the CPU.
  for (int setting : {-1, 0, 1, 2}) {
    os::QosHost host;
    host.hybrid = true;
    const os::QosDecision d = os::DecideQos(setting, host);
    CHECK(d.level == 0 && !d.process && !d.threads && !d.audio_scheduler);
  }

  // Too old a Windows (before 10 1709, build 16299) has no execution speed control.
  CHECK(!os::DecideQos(-1, WindowsHost(16298, true)).process);
  CHECK(os::DecideQos(-1, WindowsHost(16299, true)).process);
  CHECK(!os::DecideQos(2, WindowsHost(10240, true)).threads);
  // A build that could not be read: the call is tried (and may fail with a log line).
  CHECK(os::DecideQos(-1, WindowsHost(0, false)).process);

  // 0 is off on every Windows.
  const os::QosDecision off = os::DecideQos(0, WindowsHost(26100, true));
  CHECK(off.level == 0 && !off.process && !off.threads && !off.audio_scheduler);

  // Automatic is level 1: the process only, on a hybrid CPU and on any other.
  for (bool hybrid : {false, true}) {
    const os::QosDecision automatic = os::DecideQos(-1, WindowsHost(26100, hybrid));
    CHECK(automatic.level == 1 && automatic.process && !automatic.threads && !automatic.audio_scheduler);
    const os::QosDecision one = os::DecideQos(1, WindowsHost(26100, hybrid));
    CHECK(one.level == 1 && one.process && !one.threads && !one.audio_scheduler);
  }

  // 2: the threads too; the audio scheduler rule only on a hybrid CPU.
  const os::QosDecision two_hybrid = os::DecideQos(2, WindowsHost(22631, true));
  CHECK(two_hybrid.level == 2 && two_hybrid.process && two_hybrid.threads && two_hybrid.audio_scheduler);
  const os::QosDecision two_plain = os::DecideQos(2, WindowsHost(22631, false));
  CHECK(two_plain.level == 2 && two_plain.process && two_plain.threads && !two_plain.audio_scheduler);

  // A value outside the range (the setting clamps it, but the decision must not misread it).
  CHECK(os::DecideQos(7, WindowsHost(26100, true)).level == 1);
  CHECK(os::DecideQos(-5, WindowsHost(26100, true)).level == 1);

  // Every decision carries a phrase for the log.
  for (int setting : {-1, 0, 1, 2}) {
    CHECK(std::string(os::DecideQos(setting, WindowsHost(26100, true)).why).size() > 10);
  }
}

// --- the call sequences ----------------------------------------------------------------------

void TestPlan() {
  // The documented values the code stands on (Microsoft Learn, SetProcessInformation).
  CHECK(os::kQosExecutionSpeed == 0x1);
  CHECK(os::kQosIgnoreTimerResolution == 0x4);

  const os::ProcessQosPlan focused = os::PlanProcessQos(true);
  const os::ProcessQosPlan unfocused = os::PlanProcessQos(false);
  CHECK(focused.count == 2 && unfocused.count == 2);

  // Focus: take execution speed over and leave it off (HighQoS).
  CHECK(focused.calls[0].control == os::kQosExecutionSpeed && focused.calls[0].state == 0);
  // No focus: everything back to Windows.
  CHECK(unfocused.calls[0].control == 0 && unfocused.calls[0].state == 0);

  for (const os::ProcessQosPlan& plan : {focused, unfocused}) {
    // The 1 ms timer opt-out is the last call of both, so a reset can not leave a minimised game on
    // the 15.6 ms timer.
    CHECK(plan.calls[1].control == os::kQosIgnoreTimerResolution && plan.calls[1].state == 0);
    for (int i = 0; i < plan.count; ++i) {
      // Never EcoQoS and never "ignore the timer": no call turns either mechanism ON.
      CHECK((plan.calls[i].state & os::kQosExecutionSpeed) == 0);
      CHECK((plan.calls[i].state & os::kQosIgnoreTimerResolution) == 0);
      // A call only sets what it controls.
      CHECK((plan.calls[i].state & ~plan.calls[i].control) == 0);
    }
  }
}

// --- what the host calls do ------------------------------------------------------------------

#if defined(_WIN32)

bool ReadProcess(ULONG* control, ULONG* state) {
  PROCESS_POWER_THROTTLING_STATE s{};
  s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s))) {
    return false;
  }
  *control = s.ControlMask;
  *state = s.StateMask;
  return true;
}

bool Contains(const std::string& text, const char* part) {
  return text.find(part) != std::string::npos;
}

void TestLiveWindows() {
  // Level 2: process and threads. The host is real, so a Windows without the call is allowed to
  // decide 0; the checks that need the call are skipped then.
  std::vector<std::string> lines = os::StartQos(2);
  std::printf("  info: %zu start line(s)\n", lines.size());
  for (const std::string& line : lines) {
    std::printf("  info: %s\n", line.c_str());
  }
  const os::QosDecision decision = os::CurrentQosDecision();
  CHECK(!lines.empty());
  CHECK(Contains(lines[0], "[qos] thread_qos 2 -> level"));
  if (!decision.process) {
    std::printf("  info: this Windows decided against the process call: live checks skipped\n");
    return;
  }
  CHECK(lines.size() == 2);
  CHECK(lines.size() < 2 || Contains(lines[1], "window has focus"));

  ULONG control = 0;
  ULONG state = 0;
  if (ReadProcess(&control, &state)) {
    // HighQoS: execution speed controlled by us, and off.
    CHECK((control & os::kQosExecutionSpeed) != 0);
    CHECK((state & os::kQosExecutionSpeed) == 0);
    CHECK((control & os::kQosIgnoreTimerResolution) != 0);
    CHECK((state & os::kQosIgnoreTimerResolution) == 0);
  } else {
    std::printf("  info: GetProcessInformation(ProcessPowerThrottling) is not readable here\n");
  }

  // The same focus again: nothing is asked of the OS a second time.
  CHECK(os::ApplyQosFocus(true).empty());

  // Focus lost: control of execution speed handed back; the timer opt-out must survive (the plan
  // re-asserts it after the reset).
  const std::string lost = os::ApplyQosFocus(false);
  CHECK(Contains(lost, "lost focus"));
  CHECK(os::ApplyQosFocus(false).empty());
  if (ReadProcess(&control, &state)) {
    CHECK((control & os::kQosExecutionSpeed) == 0);
    CHECK((control & os::kQosIgnoreTimerResolution) != 0);
    CHECK((state & os::kQosIgnoreTimerResolution) == 0);
  }

  // Focus again.
  const std::string gained = os::ApplyQosFocus(true);
  CHECK(Contains(gained, "has focus"));
  if (ReadProcess(&control, &state)) {
    CHECK((control & os::kQosExecutionSpeed) != 0);
    CHECK((state & os::kQosExecutionSpeed) == 0);
  }

  // A thread tag, from a thread of its own.
  std::string tag_line;
  std::thread([&] { tag_line = os::TagCurrentThreadHighQos("test thread"); }).join();
  std::printf("  info: %s\n", tag_line.c_str());
  CHECK(Contains(tag_line, "\"test thread\" HighQoS: SetThreadInformation ok"));

  // Level 0 afterwards: nothing is applied or tagged, and the audio rule is off.
  lines = os::StartQos(0);
  CHECK(lines.size() == 1);
  CHECK(os::ApplyQosFocus(false).empty());
  std::thread([&] { tag_line = os::TagCurrentThreadHighQos("test thread"); }).join();
  CHECK(tag_line.empty());
  CHECK(!os::WantsAudioScheduler());
}

#else

void TestLiveOther() {
  // Linux and the others: one log line, nothing asked of the OS.
  const std::vector<std::string> lines = os::StartQos(2);
  CHECK(lines.size() == 1);
  CHECK(!lines.empty() && lines[0].find("not Windows") != std::string::npos);
  CHECK(os::ApplyQosFocus(false).empty());
  CHECK(os::TagCurrentThreadHighQos("x").empty());
  CHECK(!os::WantsAudioScheduler());
  CHECK(os::CurrentQosDecision().level == 0);
  std::printf("  info: %s\n", lines.empty() ? "" : lines[0].c_str());
}

#endif

}  // namespace

int main() {
  TestDecision();
  TestPlan();
#if defined(_WIN32)
  TestLiveWindows();
#else
  TestLiveOther();
#endif
  std::printf("%s: %d checks, %d failed\n", g_failed == 0 ? "PASS" : "FAIL", g_checked, g_failed);
  return g_failed == 0 ? 0 : 1;
}

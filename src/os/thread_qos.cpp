// Thread QoS: the calls. See thread_qos.h and perf/HYBRID-QOS.md.

#include "os/thread_qos.h"

#include <atomic>
#include <cstdio>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
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

namespace larecomp::os {

namespace {

std::mutex g_mutex;           // the decision, the focus state and the calls on the process
QosDecision g_decision;       // written once by StartQos
[[maybe_unused]] bool g_focus_known = false;   // the process part has been applied at least once
[[maybe_unused]] bool g_last_focus = false;
[[maybe_unused]] std::atomic<bool> g_threads{false};  // read by every thread that asks to be tagged
std::atomic<bool> g_audio{false};

}  // namespace

QosDecision CurrentQosDecision() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_decision;
}

bool WantsAudioScheduler() {
  return g_audio.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------------------------
#if defined(_WIN32)

static_assert(PROCESS_POWER_THROTTLING_EXECUTION_SPEED == kQosExecutionSpeed,
              "kQosExecutionSpeed holds PROCESS_POWER_THROTTLING_EXECUTION_SPEED");
static_assert(THREAD_POWER_THROTTLING_EXECUTION_SPEED == kQosExecutionSpeed,
              "kQosExecutionSpeed holds THREAD_POWER_THROTTLING_EXECUTION_SPEED");
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
static_assert(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION == kQosIgnoreTimerResolution,
              "kQosIgnoreTimerResolution holds PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION");
#endif

namespace {

// The OS build from ntdll (GetVersionEx lies to a program without the right manifest); 0 when it
// can not be read.
unsigned ReadWindowsBuild() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    return 0;
  }
  auto* rtl_get_version =
      reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
  if (!rtl_get_version) {
    return 0;
  }
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  return rtl_get_version(&info) == 0 ? static_cast<unsigned>(info.dwBuildNumber) : 0;
}

// How many logical processors have each efficiency class (a higher class is a faster core type: on
// Intel hybrid parts the performance cores are 1 and the efficiency cores 0). False when the CPU
// sets can not be read.
bool ReadEfficiencyClasses(unsigned (&counts)[256]) {
  ULONG length = 0;
  GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
  if (length == 0) {
    return false;
  }
  std::vector<uint8_t> buffer(length);
  if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                  length, &length, GetCurrentProcess(), 0)) {
    return false;
  }
  for (ULONG offset = 0; offset + sizeof(ULONG) * 2 <= length;) {
    const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
    if (entry->Size == 0) {
      break;
    }
    if (entry->Type == CpuSetInformation) {
      ++counts[entry->CpuSet.EfficiencyClass];
    }
    offset += entry->Size;
  }
  return true;
}

// One SetProcessInformation call as one log phrase.
std::string SetProcessThrottling(const QosCall& call) {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = call.control;
  state.StateMask = call.state;
  const BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
  const DWORD error = ok ? 0 : GetLastError();
  return std::format("SetProcessInformation(control 0x{:x}, state 0x{:x}) {} (error {})", call.control,
                     call.state, ok ? "ok" : "failed", error);
}

// What the OS says the process has now, as one log phrase.
std::string ReadProcessThrottling() {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
    return std::format("read-back unavailable (error {})", GetLastError());
  }
  return std::format("now control 0x{:x}, state 0x{:x}", state.ControlMask, state.StateMask);
}

// The process part for a window that has / has not focus. g_mutex held.
std::string ApplyProcessLocked(bool focused) {
  const ProcessQosPlan plan = PlanProcessQos(focused);
  std::string line = std::format("[qos] window {}: ", focused ? "has focus, process HighQoS" : "lost focus, process back to Windows");
  for (int i = 0; i < plan.count; ++i) {
    line += SetProcessThrottling(plan.calls[i]);
    line += "; ";
  }
  line += ReadProcessThrottling();
  g_focus_known = true;
  g_last_focus = focused;
  return line;
}

}  // namespace

QosHost DetectQosHost(std::string* detail) {
  QosHost host;
  host.windows = true;
  host.windows_build = ReadWindowsBuild();
  unsigned counts[256] = {};
  const bool have_classes = ReadEfficiencyClasses(counts);
  unsigned classes = 0;
  unsigned logical = 0;
  std::string by_class;
  for (int c = 255; c >= 0; --c) {
    if (counts[c] != 0) {
      ++classes;
      logical += counts[c];
      by_class += std::format("{}class {} x{}", by_class.empty() ? "" : ", ", c, counts[c]);
    }
  }
  host.hybrid = classes > 1;
  if (detail) {
    *detail = std::format("Windows build {}; {} logical processors{}", host.windows_build, logical,
                          !have_classes ? " (CPU sets unreadable)"
                                        : (host.hybrid ? ", hybrid: efficiency " + by_class
                                                       : ", one efficiency class (" + by_class + ")"));
  }
  return host;
}

std::vector<std::string> StartQos(int setting) {
  std::string detail;
  const QosHost host = DetectQosHost(&detail);
  const QosDecision decision = DecideQos(setting, host);
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_decision = decision;
    g_threads.store(decision.threads, std::memory_order_release);
    g_audio.store(decision.audio_scheduler, std::memory_order_release);
    lines.push_back(std::format("[qos] thread_qos {} -> level {}: {}; {}", setting, decision.level,
                                decision.why, detail));
    if (decision.process) {
      // The player has just started the game: the window is taken to have focus until the UI says
      // otherwise (the first focus event, ApplyQosFocus).
      lines.push_back(ApplyProcessLocked(true));
    }
  }
  return lines;
}

std::string ApplyQosFocus(bool focused) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_decision.process || (g_focus_known && g_last_focus == focused)) {
    return {};
  }
  return ApplyProcessLocked(focused);
}

std::string TagCurrentThreadHighQos(const char* role) {
  if (!g_threads.load(std::memory_order_acquire)) {
    return {};
  }
  THREAD_POWER_THROTTLING_STATE state{};
  state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask = 0;
  const BOOL ok = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
  const DWORD error = ok ? 0 : GetLastError();
  THREAD_POWER_THROTTLING_STATE now{};
  now.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  const bool read = GetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &now, sizeof(now)) != FALSE;
  return std::format("[qos] thread \"{}\" HighQoS: SetThreadInformation {} (error {}), {}", role,
                     ok ? "ok" : "failed", error,
                     read ? std::format("now control 0x{:x}, state 0x{:x}", now.ControlMask, now.StateMask)
                          : std::string("read-back unavailable"));
}

// ---------------------------------------------------------------------------------------------
// Anything else (Linux included): nothing is asked of the OS; the host is read for the log only
// ---------------------------------------------------------------------------------------------
#else

namespace {

// "0-5,12-19" style list the kernel writes, or empty when the file is not there.
[[maybe_unused]] std::string ReadSysfsList(const char* path) {
  std::ifstream file(path);
  std::string text;
  if (file && std::getline(file, text)) {
    return text;
  }
  return {};
}

}  // namespace

QosHost DetectQosHost(std::string* detail) {
  QosHost host;  // windows = false: DecideQos turns everything off
#if defined(__linux__)
  // Intel hybrid parts show their two core types here (cpu_core = performance, cpu_atom = efficiency).
  const std::string performance = ReadSysfsList("/sys/devices/cpu_core/cpus");
  const std::string efficiency = ReadSysfsList("/sys/devices/cpu_atom/cpus");
  host.hybrid = !performance.empty() && !efficiency.empty();
  if (detail) {
    *detail = host.hybrid ? "hybrid CPU: performance cpus " + performance + ", efficiency cpus " + efficiency
                          : "no hybrid CPU reported by the kernel";
  }
#else
  if (detail) {
    *detail = "this OS reports nothing";
  }
#endif
  return host;
}

std::vector<std::string> StartQos(int setting) {
  std::string detail;
  const QosHost host = DetectQosHost(&detail);
  const QosDecision decision = DecideQos(setting, host);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_decision = decision;
  }
  return {std::format("[qos] thread_qos {} -> level {}: {}; {}", setting, decision.level, decision.why, detail)};
}

std::string ApplyQosFocus(bool) {
  return {};
}

std::string TagCurrentThreadHighQos(const char*) {
  return {};
}

#endif

}  // namespace larecomp::os

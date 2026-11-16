// Thread priorities for every OS. See thread_priority.h.

#include "os/thread_priority.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace larecomp::os {

namespace {

std::mutex& CallbackMutex() {
  static std::mutex mutex;
  return mutex;
}

LogCallback& Callback() {
  static LogCallback callback;
  return callback;
}

[[maybe_unused]] void LogLine(const std::string& line) {
  LogCallback callback;
  {
    std::lock_guard<std::mutex> lock(CallbackMutex());
    callback = Callback();
  }
  if (callback) {
    callback(line);
    return;
  }
  std::fprintf(stderr, "%s\n", line.c_str());
}

}  // namespace

LogCallback SetLogCallback(LogCallback callback) {
  std::lock_guard<std::mutex> lock(CallbackMutex());
  std::swap(callback, Callback());
  return callback;
}

// ---------------------------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------------------------
#if defined(_WIN32)

static_assert(THREAD_PRIORITY_BELOW_NORMAL == -1 && THREAD_PRIORITY_NORMAL == 0 &&
                  THREAD_PRIORITY_ABOVE_NORMAL == 1 && THREAD_PRIORITY_HIGHEST == 2,
              "ThreadPlan::windows_priority holds THREAD_PRIORITY_* values");

namespace {

// AvSetMmThreadCharacteristicsW, looked up when first needed so that the program that uses this
// file need not link avrt (the shader pack tool does not). The same call, the same DLL.
using AvSetMmThreadCharacteristicsFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);

AvSetMmThreadCharacteristicsFn FindSchedulerCall(DWORD* error) {
  static DWORD load_error = 0;
  static const AvSetMmThreadCharacteristicsFn function = [] {
    HMODULE module = LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      load_error = GetLastError();
      return AvSetMmThreadCharacteristicsFn{};
    }
    auto* found = reinterpret_cast<AvSetMmThreadCharacteristicsFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "AvSetMmThreadCharacteristicsW")));
    if (!found) {
      load_error = GetLastError();
    }
    return found;
  }();
  *error = load_error;
  return function;
}

}  // namespace

ThreadResult SetCurrentThreadIntent(ThreadIntent intent, Lowering lowering) {
  const ThreadPlan plan = PlanFor(intent, lowering);
  HANDLE self = GetCurrentThread();
  ThreadResult result;
  result.set = SetThreadPriority(self, plan.windows_priority) != FALSE;
  result.error = result.set ? 0 : GetLastError();
  // The multimedia scheduler registration lasts until the thread ends.
  if (plan.windows_scheduler) {
    result.scheduler_asked = true;
    DWORD load_error = 0;
    const AvSetMmThreadCharacteristicsFn join = FindSchedulerCall(&load_error);
    DWORD task = 0;
    HANDLE registered = join ? join(L"Pro Audio", &task) : nullptr;
    result.scheduler_ok = registered != nullptr;
    result.scheduler_error = result.scheduler_ok ? 0 : (join ? GetLastError() : load_error);
  }
  // Read after the registration, which raises the thread above what SetThreadPriority gave it.
  result.now = GetThreadPriority(self);
  result.detail = std::format("SetThreadPriority {} (error {}), now {}",
                              result.set ? "ok" : "failed", result.error, result.now);
  result.scheduler_detail = std::format("multimedia scheduler {} (error {})",
                                        result.scheduler_asked
                                            ? (result.scheduler_ok ? "ok" : "failed")
                                            : "not asked",
                                        result.scheduler_error);
  return result;
}

ThreadResult SetThreadIntent(std::thread& thread, ThreadIntent intent, Lowering lowering) {
  const ThreadPlan plan = PlanFor(intent, lowering);
  ThreadResult result;
  if (!thread.joinable()) {
    result.error = ERROR_INVALID_HANDLE;
    result.now = ThreadResult::kUnknown;
    result.detail = "thread not joinable";
    return result;
  }
  HANDLE handle = thread.native_handle();
  result.set = SetThreadPriority(handle, plan.windows_priority) != FALSE;
  result.error = result.set ? 0 : GetLastError();
  result.now = GetThreadPriority(handle);
  result.detail = std::format("SetThreadPriority {} (error {}), now {}",
                              result.set ? "ok" : "failed", result.error, result.now);
  return result;
}

std::string CurrentThreadName() {
  char name[64] = "?";
  PWSTR description = nullptr;
  if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
    if (WideCharToMultiByte(CP_UTF8, 0, description, -1, name, int(sizeof(name)), nullptr,
                            nullptr) <= 0) {
      name[0] = '?';
      name[1] = '\0';
    }
    LocalFree(description);
  }
  return name;
}

// ---------------------------------------------------------------------------------------------
// Linux
// ---------------------------------------------------------------------------------------------
#elif defined(__linux__)

namespace {

constexpr int kPolicyMask = 0xFF;  // SCHED_RESET_ON_FORK is a flag above the policy number

int ThreadId() {
  return static_cast<int>(syscall(SYS_gettid));
}

// The nice value of a thread (0 = the calling one), or kUnknown.
int ReadNice(int tid) {
  errno = 0;
  const int nice = getpriority(PRIO_PROCESS, static_cast<id_t>(tid));
  return nice == -1 && errno != 0 ? ThreadResult::kUnknown : nice;
}

int ReadPolicy(int tid) {
  const int policy = sched_getscheduler(static_cast<pid_t>(tid));
  return policy < 0 ? SCHED_OTHER : (policy & kPolicyMask);
}

const char* PolicyName(int policy) {
  switch (policy) {
    case SCHED_OTHER: return "SCHED_OTHER";
    case SCHED_BATCH: return "SCHED_BATCH";
    case SCHED_IDLE: return "SCHED_IDLE";
    case SCHED_FIFO: return "SCHED_FIFO";
    case SCHED_RR: return "SCHED_RR";
    default: return "SCHED_?";
  }
}

// Where the process stands when it starts: the main thread's nice value and policy, which a
// `nice -n 5 ./game` or `chrt -b 0 ./game` gave every thread. The intents are relative to it.
struct Baseline {
  int nice;
  int policy;
};

const Baseline& GetBaseline() {
  static const Baseline baseline = [] {
    const int nice = ReadNice(0);
    return Baseline{nice == ThreadResult::kUnknown ? 0 : nice, ReadPolicy(0)};
  }();
  return baseline;
}

// Taken at start-up, in the main thread, before anything can have lowered it (the first call of a
// function-local static from a lowered thread would otherwise take that thread's value).
[[maybe_unused]] const Baseline& g_baseline_at_start = GetBaseline();

// What this thread has now, read at its first call and kept up to date by it.
struct ThreadState {
  bool read = false;
  int nice = 0;
  int policy = SCHED_OTHER;
};
thread_local ThreadState t_state;

bool IsFairPolicy(int policy) {
  return policy == SCHED_OTHER || policy == SCHED_BATCH || policy == SCHED_IDLE;
}

// The lowest nice value RLIMIT_NICE allows this process (the kernel's rule is nice >= 20 - limit);
// 20 when it allows none.
int NiceFloorByLimit() {
  rlimit limit{};
  if (getrlimit(RLIMIT_NICE, &limit) != 0) {
    return 20;
  }
  const long soft = limit.rlim_cur == RLIM_INFINITY ? 40 : static_cast<long>(limit.rlim_cur);
  return static_cast<int>(std::clamp(20L - soft, -20L, 20L));
}

// One line per process: a thread's priority could not be raised, and what would allow it.
void LogRefusedRaise(unsigned error) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true)) {
    return;
  }
  LogLine(std::format(
      "[threads] this process may not raise a thread's priority (error {}: {}); the threads that "
      "asked (audio, frame generation pacer) stay at normal priority. Lowering the background "
      "threads works without it. To allow raising: RLIMIT_NICE (limits.conf \"nice\" line, "
      "systemd LimitNICE=) or CAP_SYS_NICE. Nothing needs it.",
      error, std::strerror(static_cast<int>(error))));
}

bool IsPermissionError(unsigned error) {
  return error == EACCES || error == EPERM;
}

}  // namespace

ThreadResult SetCurrentThreadIntent(ThreadIntent intent, Lowering lowering) {
  const ThreadPlan plan = PlanFor(intent, lowering);
  const Baseline& base = GetBaseline();
  const int tid = ThreadId();
  ThreadState& state = t_state;
  if (!state.read) {
    const int nice = ReadNice(tid);
    state.nice = nice == ThreadResult::kUnknown ? base.nice : nice;
    state.policy = ReadPolicy(tid);
    state.read = true;
  }
  ThreadResult result;
  result.set = true;
  const auto fail = [&](unsigned error) {
    if (result.set) {
      result.set = false;
      result.error = error;
    }
  };

  // The policy: SCHED_BATCH for a background thread, the process's own otherwise. A thread that
  // is idle-class is left there (giving it batch would be a raise, which the kernel refuses).
  const int want_policy = plan.linux_batch ? SCHED_BATCH : base.policy;
  if (IsFairPolicy(want_policy) && state.policy != want_policy &&
      !(state.policy == SCHED_IDLE && want_policy == SCHED_BATCH)) {
    sched_param param{};
    if (sched_setscheduler(static_cast<pid_t>(tid), want_policy, &param) == 0) {
      state.policy = want_policy;
    } else {
      fail(static_cast<unsigned>(errno));
    }
  }

  // The nice value: the process's own plus the intent's, within the limits of nice.
  const int target = std::clamp(base.nice + plan.linux_nice, -20, 19);
  if (target != state.nice) {
    const bool raising = target < state.nice;
    errno = 0;
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), target) == 0) {
      state.nice = target;
    } else {
      unsigned error = static_cast<unsigned>(errno);
      // RLIMIT_NICE may allow a smaller raise than the one asked for.
      const int floor = NiceFloorByLimit();
      if (raising && IsPermissionError(error) && floor < state.nice && floor > target &&
          setpriority(PRIO_PROCESS, static_cast<id_t>(tid), floor) == 0) {
        state.nice = floor;
        error = 0;
      }
      if (error != 0) {
        fail(error);
        if (raising && IsPermissionError(error)) {
          LogRefusedRaise(error);
        }
      }
    }
  }

  const int nice_now = ReadNice(tid);
  result.now = nice_now;
  state.nice = nice_now == ThreadResult::kUnknown ? state.nice : nice_now;
  result.detail = std::format("nice/policy {} (error {}), now nice {}, {}",
                              result.set ? "ok" : "refused", result.error, nice_now,
                              PolicyName(ReadPolicy(tid)));
  return result;
}

ThreadResult SetThreadIntent(std::thread& thread, ThreadIntent intent, Lowering lowering) {
  const ThreadPlan plan = PlanFor(intent, lowering);
  const Baseline& base = GetBaseline();
  ThreadResult result;
  result.now = ThreadResult::kUnknown;
  if (!thread.joinable()) {
    result.error = ESRCH;
    result.detail = "thread not joinable";
    return result;
  }
  // Policy only: pthread_setschedparam takes a pthread_t, and a nice value needs the kernel's id.
  const int want_policy = plan.linux_batch ? SCHED_BATCH : base.policy;
  if (!IsFairPolicy(want_policy)) {
    result.set = true;
    result.detail = "policy left alone";
    return result;
  }
  sched_param param{};
  const int rc = pthread_setschedparam(thread.native_handle(), want_policy, &param);
  result.set = rc == 0;
  result.error = static_cast<unsigned>(rc);
  result.detail = std::format("policy {} {} (error {}); the nice value is the thread's own",
                              PolicyName(want_policy), result.set ? "ok" : "refused", result.error);
  return result;
}

std::string CurrentThreadName() {
  char name[32] = {};
  if (prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0) != 0 || name[0] == '\0') {
    return "?";
  }
  return name;
}

// ---------------------------------------------------------------------------------------------
// Anything else: nothing is done
// ---------------------------------------------------------------------------------------------
#else

ThreadResult SetCurrentThreadIntent(ThreadIntent, Lowering) {
  ThreadResult result;
  result.set = true;
  result.now = ThreadResult::kUnknown;
  result.detail = "thread priorities are not set on this OS";
  return result;
}

ThreadResult SetThreadIntent(std::thread&, ThreadIntent, Lowering) {
  return SetCurrentThreadIntent(ThreadIntent::kNormal);
}

std::string CurrentThreadName() {
  return "?";
}

#endif

}  // namespace larecomp::os

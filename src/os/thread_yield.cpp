// Giving up the CPU and sleeping, for every OS. See thread_yield.h.

#include "os/thread_yield.h"

#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#include <time.h>

#include <cerrno>
#endif

namespace larecomp::os {

void YieldThread() {
#if defined(_WIN32)
  SwitchToThread();
#elif defined(__linux__)
  sched_yield();
#else
  std::this_thread::yield();
#endif
}

void SleepFor(std::chrono::nanoseconds duration) {
  if (duration <= std::chrono::nanoseconds::zero()) {
    return;
  }
#if defined(__linux__)
  timespec deadline{};
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
    std::this_thread::sleep_for(duration);
    return;
  }
  const long long total = deadline.tv_nsec + duration.count() % 1'000'000'000LL;
  deadline.tv_sec += time_t(duration.count() / 1'000'000'000LL + total / 1'000'000'000LL);
  deadline.tv_nsec = long(total % 1'000'000'000LL);
  // clock_nanosleep returns the error number (it does not set errno). A signal ends the sleep early with EINTR: sleep on, to the same
  // deadline.
  int error;
  do {
    error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
  } while (error == EINTR);
#else
  std::this_thread::sleep_for(duration);
#endif
}

}  // namespace larecomp::os

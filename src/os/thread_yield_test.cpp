// Standalone check of the yield and the sleep (os/thread_yield.h): a sleep lasts as long as it was asked to, also when signals end it
// early on Linux (the SDK suspends and alerts guest threads with signals), and a yield returns. No game, no SDK.
//
//   cmake --build <dir> --target larecomp_thread_yield_test, then run it (exit 0 = passed).
//   Or by hand:  clang++ -std=c++23 -I src src/os/thread_yield_test.cpp src/os/thread_yield.cpp -pthread
//
// What it can not say: how late a sleep wakes on the Steam Deck (a measurement, not a check: the bound here is loose on purpose).

#include "os/thread_yield.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <signal.h>
#endif

namespace os = larecomp::os;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

int g_failed = 0;
int g_checked = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    ++g_checked;                                                   \
    if (!(cond)) {                                                 \
      ++g_failed;                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
    }                                                              \
  } while (0)

std::chrono::nanoseconds Timed(auto&& body) {
  const auto start = Clock::now();
  body();
  return Clock::now() - start;
}

void TestSleep() {
  // Nothing to wait for: returns at once.
  CHECK(Timed([] { os::SleepFor(0ns); }) < 50ms);
  CHECK(Timed([] { os::SleepFor(-5ms); }) < 50ms);

  // At least as long as asked (the clock the sleep and this check read is the same monotonic one), and not wildly more.
  for (const auto asked : {1ms, 5ms, 20ms}) {
    const auto took = Timed([&] { os::SleepFor(asked); });
    CHECK(took >= asked);
    CHECK(took < asked + 500ms);
  }
  // A sub-millisecond request is a sleep too.
  CHECK(Timed([] { os::SleepFor(300us); }) >= 300us);
}

void TestYield() {
  // A yield with nothing else to run returns at once; many of them do not hang.
  const auto took = Timed([] {
    for (int i = 0; i < 10000; ++i) {
      os::YieldThread();
    }
  });
  CHECK(took < 5s);
}

#if defined(__linux__)

std::atomic<int> g_signals{0};

void OnSignal(int) { g_signals.fetch_add(1, std::memory_order_relaxed); }

void TestSignals() {
  // The handler has no SA_RESTART: a sleep that is not taken up again would end at the first signal.
  struct sigaction action{};
  action.sa_handler = OnSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  CHECK(sigaction(SIGUSR1, &action, nullptr) == 0);

  std::atomic<bool> sleeping{false};
  std::atomic<bool> done{false};
  std::chrono::nanoseconds took{};
  std::thread sleeper([&] {
    sleeping = true;
    took = Timed([] { os::SleepFor(200ms); });
    done = true;
  });
  while (!sleeping) {
    std::this_thread::yield();
  }
  // Ten signals over the first 100 ms of the 200 ms sleep.
  for (int i = 0; i < 10 && !done; ++i) {
    pthread_kill(sleeper.native_handle(), SIGUSR1);
    std::this_thread::sleep_for(10ms);
  }
  sleeper.join();
  CHECK(took >= 200ms);
  CHECK(took < 200ms + 500ms);
  std::printf("  info: %d signal(s) reached the sleeper, it slept %lld ms\n", g_signals.load(),
              static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
}

#endif

}  // namespace

int main() {
  TestSleep();
  TestYield();
#if defined(__linux__)
  TestSignals();
#endif
  std::printf("%s: %d checks, %d failed\n", g_failed == 0 ? "PASS" : "FAIL", g_checked, g_failed);
  return g_failed == 0 ? 0 : 1;
}

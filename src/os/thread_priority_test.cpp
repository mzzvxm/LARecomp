// Standalone check of the thread priority intents (os/thread_priority.h): the mapping per OS, and,
// on the OS this is built for, what the calls really do to a thread. No game, no SDK.
//
//   cmake --build <dir> --target larecomp_thread_priority_test, then run it (exit 0 = passed).
//   Or by hand:  clang++ -std=c++23 -I src src/os/thread_priority_test.cpp src/os/thread_priority.cpp -pthread
//
// What it can not say: that the numbers are good for a frame (that is a Steam Deck measurement,
// perf/LX-PRIO.md), and what a privileged process gets (it only checks that a raise that is
// allowed is applied, and that one that is refused changes nothing and is logged once).

#include "os/thread_priority.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iterator>
#include <mutex>
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
#elif defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace os = larecomp::os;
using os::Lowering;
using os::PlanFor;
using os::ThreadIntent;

namespace {

std::atomic<int> g_failed{0};
std::atomic<int> g_checked{0};

#define CHECK(cond)                                                                  \
  do {                                                                               \
    ++g_checked;                                                                     \
    if (!(cond)) {                                                                   \
      ++g_failed;                                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
    }                                                                                \
  } while (0)

#define CHECK_EQ(a, b)                                                                       \
  do {                                                                                       \
    ++g_checked;                                                                             \
    const auto va = (a);                                                                     \
    const auto vb = (b);                                                                     \
    if (!(va == vb)) {                                                                       \
      ++g_failed;                                                                            \
      std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,       \
                  static_cast<long long>(va), static_cast<long long>(vb));                   \
    }                                                                                        \
  } while (0)

// Runs `body` on a thread of its own (the intents change a thread for good).
void OnNewThread(const std::function<void()>& body) {
  std::thread thread(body);
  thread.join();
}

// --- the mapping -----------------------------------------------------------------------------

void TestWindowsMapping() {
  // The values of every call the game made before the helper existed, by site.
  CHECK_EQ(PlanFor(ThreadIntent::kBelowNormal).windows_priority, -1);  // texture decode, pipeline
                                                                       // workers, physics pool,
                                                                       // profiler sampler
  CHECK_EQ(PlanFor(ThreadIntent::kBackground).windows_priority, -1);   // host precache builder,
                                                                       // shader pack workers
  CHECK_EQ(PlanFor(ThreadIntent::kBackground, Lowering::kRaisable).windows_priority, -1);
  CHECK_EQ(PlanFor(ThreadIntent::kNormal).windows_priority, 0);        // pipeline boost, builder raise
  CHECK_EQ(PlanFor(ThreadIntent::kAboveNormal).windows_priority, 1);   // audio level 1
  CHECK_EQ(PlanFor(ThreadIntent::kHigh).windows_priority, 2);          // audio level 2, the pacer
  CHECK_EQ(PlanFor(ThreadIntent::kAudio).windows_priority, 2);         // audio level 3 ...
  CHECK(PlanFor(ThreadIntent::kAudio).windows_scheduler);              // ... with the scheduler
  const ThreadIntent others[] = {ThreadIntent::kBackground, ThreadIntent::kBelowNormal,
                                 ThreadIntent::kNormal, ThreadIntent::kAboveNormal,
                                 ThreadIntent::kHigh};
  for (const ThreadIntent intent : others) {
    CHECK(!PlanFor(intent).windows_scheduler);
    // What the Windows side does must not depend on the Linux-only option.
    CHECK_EQ(PlanFor(intent, Lowering::kRaisable).windows_priority, PlanFor(intent).windows_priority);
    CHECK_EQ(PlanFor(intent, Lowering::kRaisable).windows_scheduler,
             PlanFor(intent).windows_scheduler);
  }
#if defined(_WIN32)
  CHECK_EQ(PlanFor(ThreadIntent::kBelowNormal).windows_priority, THREAD_PRIORITY_BELOW_NORMAL);
  CHECK_EQ(PlanFor(ThreadIntent::kNormal).windows_priority, THREAD_PRIORITY_NORMAL);
  CHECK_EQ(PlanFor(ThreadIntent::kAboveNormal).windows_priority, THREAD_PRIORITY_ABOVE_NORMAL);
  CHECK_EQ(PlanFor(ThreadIntent::kHigh).windows_priority, THREAD_PRIORITY_HIGHEST);
#endif
}

void TestLinuxMapping() {
  const ThreadIntent ladder[] = {ThreadIntent::kBackground, ThreadIntent::kBelowNormal,
                                 ThreadIntent::kNormal,     ThreadIntent::kAboveNormal,
                                 ThreadIntent::kHigh,       ThreadIntent::kAudio};
  // Along the ladder the nice value only goes down (a higher priority) and the Windows value only up.
  for (size_t i = 1; i < std::size(ladder); ++i) {
    CHECK(PlanFor(ladder[i]).linux_nice <= PlanFor(ladder[i - 1]).linux_nice);
    CHECK(PlanFor(ladder[i]).windows_priority >= PlanFor(ladder[i - 1]).windows_priority);
  }
  // Lowering needs no privilege: the lowering intents are positive, the raising ones negative.
  CHECK(PlanFor(ThreadIntent::kBackground).linux_nice > PlanFor(ThreadIntent::kBelowNormal).linux_nice);
  CHECK(PlanFor(ThreadIntent::kBelowNormal).linux_nice > 0);
  CHECK_EQ(PlanFor(ThreadIntent::kNormal).linux_nice, 0);
  CHECK(PlanFor(ThreadIntent::kAboveNormal).linux_nice < 0);
  CHECK(PlanFor(ThreadIntent::kHigh).linux_nice < PlanFor(ThreadIntent::kAboveNormal).linux_nice);
  CHECK(PlanFor(ThreadIntent::kAudio).linux_nice < PlanFor(ThreadIntent::kHigh).linux_nice);
  CHECK(PlanFor(ThreadIntent::kAudio).linux_nice >= -20);
  CHECK(PlanFor(ThreadIntent::kBackground).linux_nice <= 19);
  // Batch only for the background, and never for an intent at or above normal.
  CHECK(PlanFor(ThreadIntent::kBackground).linux_batch);
  CHECK(!PlanFor(ThreadIntent::kBelowNormal).linux_batch);
  CHECK(!PlanFor(ThreadIntent::kNormal).linux_batch);
  CHECK(!PlanFor(ThreadIntent::kAudio).linux_batch);
  // A lowering that is raised later changes no nice value (the kernel would not undo it) but the
  // background keeps its batch policy (which it can undo).
  for (const ThreadIntent intent : ladder) {
    const os::ThreadPlan raisable = PlanFor(intent, Lowering::kRaisable);
    const os::ThreadPlan one_way = PlanFor(intent);
    CHECK(raisable.linux_nice == 0 || raisable.linux_nice == one_way.linux_nice);
    CHECK_EQ(raisable.linux_batch, one_way.linux_batch);
  }
  CHECK_EQ(PlanFor(ThreadIntent::kBackground, Lowering::kRaisable).linux_nice, 0);
  CHECK(PlanFor(ThreadIntent::kBackground, Lowering::kRaisable).linux_batch);
}

// --- what the calls do -----------------------------------------------------------------------

#if defined(_WIN32)

void TestLiveWindows() {
  OnNewThread([] {
    os::ThreadResult r = os::SetCurrentThreadIntent(ThreadIntent::kBelowNormal);
    CHECK(r.set);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_BELOW_NORMAL);
    CHECK_EQ(r.now, THREAD_PRIORITY_BELOW_NORMAL);
    CHECK(r.Text().find("SetThreadPriority ok (error 0), now -1") == 0);
    r = os::SetCurrentThreadIntent(ThreadIntent::kNormal);
    CHECK(r.set);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_NORMAL);
    r = os::SetCurrentThreadIntent(ThreadIntent::kAboveNormal);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_ABOVE_NORMAL);
    r = os::SetCurrentThreadIntent(ThreadIntent::kHigh);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_HIGHEST);
    // The audio intent: HIGHEST, and the multimedia scheduler asked for (it may be refused on a
    // PC without the service, which is reported, not an error here).
    r = os::SetCurrentThreadIntent(ThreadIntent::kAudio);
    CHECK(r.set);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), r.now);
    CHECK(r.scheduler_ok ? r.now == THREAD_PRIORITY_TIME_CRITICAL  // the scheduler's own boost
                         : r.now == THREAD_PRIORITY_HIGHEST);
    CHECK(r.scheduler_asked);
    CHECK(r.scheduler_ok == (r.scheduler_error == 0));
    CHECK(r.Text(true).find("multimedia scheduler ") != std::string::npos);
    r = os::SetCurrentThreadIntent(ThreadIntent::kHigh);
    CHECK(!r.scheduler_asked);
    CHECK(r.Text(true).find("multimedia scheduler not asked (error 0)") != std::string::npos);
  });

  // A thread another thread lowers and raises: the builder of the host precache.
  std::mutex mutex;
  std::condition_variable cv;
  int step = 0;  // 1: the thread has lowered itself, 2: the main thread raised it, 3: it checked
  std::thread builder([&] {
    const os::ThreadResult r =
        os::SetCurrentThreadIntent(ThreadIntent::kBackground, Lowering::kRaisable);
    CHECK(r.set);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_BELOW_NORMAL);
    std::unique_lock<std::mutex> lock(mutex);
    step = 1;
    cv.notify_all();
    cv.wait(lock, [&] { return step == 2; });
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_NORMAL);
    step = 3;
    cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return step == 1; });
    const os::ThreadResult r = os::SetThreadIntent(builder, ThreadIntent::kNormal);
    CHECK(r.set);
    CHECK_EQ(r.now, THREAD_PRIORITY_NORMAL);
    step = 2;
    cv.notify_all();
    cv.wait(lock, [&] { return step == 3; });
  }
  builder.join();
  std::thread gone;
  CHECK(!os::SetThreadIntent(gone, ThreadIntent::kNormal).set);  // not joinable: nothing done
}

#elif defined(__linux__)

int Tid() {
  return static_cast<int>(syscall(SYS_gettid));
}
int NiceNow() {
  errno = 0;
  return getpriority(PRIO_PROCESS, static_cast<id_t>(Tid()));
}
int PolicyNow() {
  return sched_getscheduler(static_cast<pid_t>(Tid())) & 0xFF;
}

std::atomic<bool> g_refused_seen{false};  // a raise was refused somewhere in the live checks

void TestLiveLinux() {
  // Every log line of the process comes here: refused raises must be logged once, in all.
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  const os::LogCallback previous = os::SetLogCallback([&](const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    lines.push_back(line);
  });

  const int base_nice = [] {
    errno = 0;
    return getpriority(PRIO_PROCESS, 0);
  }();

  OnNewThread([&] {
    CHECK_EQ(NiceNow(), base_nice);  // a new thread starts where the process is
    // Lowering: always allowed.
    os::ThreadResult r = os::SetCurrentThreadIntent(ThreadIntent::kBelowNormal);
    CHECK(r.set);
    CHECK_EQ(NiceNow(), std::min(base_nice + 5, 19));
    CHECK_EQ(r.now, NiceNow());
    CHECK_EQ(PolicyNow(), SCHED_OTHER);
    r = os::SetCurrentThreadIntent(ThreadIntent::kBackground);
    CHECK(r.set);
    CHECK_EQ(NiceNow(), std::min(base_nice + 10, 19));
    CHECK_EQ(PolicyNow(), SCHED_BATCH);
    // The same intent again: nothing to do, still set.
    r = os::SetCurrentThreadIntent(ThreadIntent::kBackground);
    CHECK(r.set);
  });

  // kNormal after a one-way lowering: the batch policy goes back; the nice value goes back only
  // if the process may raise (then everything is set), else it is reported refused and unchanged.
  OnNewThread([&] {
    os::SetCurrentThreadIntent(ThreadIntent::kBackground);
    const int lowered = NiceNow();
    const os::ThreadResult r = os::SetCurrentThreadIntent(ThreadIntent::kNormal);
    CHECK_EQ(PolicyNow(), SCHED_OTHER);
    if (r.set) {
      CHECK_EQ(NiceNow(), base_nice);
    } else {
      g_refused_seen = true;
      CHECK_EQ(NiceNow(), lowered);
      CHECK(r.error == EACCES || r.error == EPERM);
      CHECK(r.detail.find("refused") != std::string::npos);
    }
    std::printf("  info: raising a lowered thread's nice value %s here\n",
                r.set ? "is allowed" : "is refused (the normal case)");
  });

  // The raisable lowering is undone by another thread without privileges.
  {
    std::mutex mutex;
    std::condition_variable cv;
    int step = 0;
    std::thread builder([&] {
      const os::ThreadResult r =
          os::SetCurrentThreadIntent(ThreadIntent::kBackground, Lowering::kRaisable);
      CHECK(r.set);
      CHECK_EQ(PolicyNow(), SCHED_BATCH);
      CHECK_EQ(NiceNow(), base_nice);
      std::unique_lock<std::mutex> lock(mutex);
      step = 1;
      cv.notify_all();
      cv.wait(lock, [&] { return step == 2; });
      CHECK_EQ(PolicyNow(), SCHED_OTHER);
      CHECK_EQ(NiceNow(), base_nice);
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [&] { return step == 1; });
      const os::ThreadResult r = os::SetThreadIntent(builder, ThreadIntent::kNormal);
      CHECK(r.set);
      CHECK_EQ(r.error, 0u);
      step = 2;
      cv.notify_all();
    }
    builder.join();
    std::thread gone;
    CHECK(!os::SetThreadIntent(gone, ThreadIntent::kNormal).set);
  }

  // A refused raise: nothing changes and the result says so. (When the process may raise: root,
  // CAP_SYS_NICE, RLIMIT_NICE, it is applied instead.)
  bool audio_refused = false;
  for (int i = 0; i < 3; ++i) {
    OnNewThread([&] {
      const int before = NiceNow();
      const os::ThreadResult r = os::SetCurrentThreadIntent(ThreadIntent::kAudio);
      if (!r.set) {
        audio_refused = true;
        g_refused_seen = true;
        CHECK_EQ(NiceNow(), before);  // refused wholly (a limit that allows some raise is applied)
        CHECK(r.error == EACCES || r.error == EPERM);
      } else {
        CHECK(NiceNow() < before);
      }
      // A limit that let the audio level some of the way may still refuse the high level.
      if (!os::SetCurrentThreadIntent(ThreadIntent::kHigh).set) {
        g_refused_seen = true;
      }
    });
  }
  std::printf("  info: raising to the audio intent was %s here\n",
              audio_refused ? "refused (the normal case)" : "allowed");

  // However many raises were refused, the process logged ONE line; none when nothing was refused.
  os::SetLogCallback(previous);
  std::printf("  info: %zu log line(s)\n", lines.size());
  if (g_refused_seen) {
    CHECK_EQ(lines.size(), size_t(1));
    CHECK(lines.empty() || lines[0].find("may not raise") != std::string::npos);
  } else {
    CHECK(lines.empty());
  }

  CHECK(os::CurrentThreadName() != "");
}

#endif

}  // namespace

int main() {
  TestWindowsMapping();
  TestLinuxMapping();
#if defined(_WIN32)
  TestLiveWindows();
#elif defined(__linux__)
  TestLiveLinux();
#else
  std::printf("  info: no live checks on this OS\n");
#endif
  std::printf("%s: %d checks, %d failed\n", g_failed == 0 ? "PASS" : "FAIL", g_checked.load(),
              g_failed.load());
  return g_failed == 0 ? 0 : 1;
}

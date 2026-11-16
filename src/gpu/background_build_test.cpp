// Check of the background build's hand-over rules (background_build.h). A program of its own: it
// needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_background_build_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_background_build_test

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "background_build.h"

using mcla::gpu::BackgroundBuild;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

// Waits (bounded) until Collect says the job ended.
bool CollectWithin(BackgroundBuild& build, int milliseconds) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < end) {
    if (build.Collect()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

}  // namespace

int main() {
  // Nothing started: nothing to collect, nothing running.
  {
    BackgroundBuild build;
    Check(!build.Running(), "idle: not running");
    Check(!build.Busy(), "idle: not busy");
    Check(!build.Collect(), "idle: nothing to collect");
    build.Join();  // no thread: harmless
    Check(!build.Running(), "idle after Join: not running");
    Check(!build.Busy(), "idle after Join: not busy");
  }

  // A job runs on another thread, holds the next Start off until collected, and its writes are
  // visible after Collect.
  {
    BackgroundBuild build;
    std::atomic<bool> release{false};
    int result = 0;
    std::thread::id worker;
    Check(build.Start([&] {
            worker = std::this_thread::get_id();
            while (!release.load()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            result = 42;
          }),
          "first Start accepted");
    Check(build.Running(), "running after Start");
    Check(build.Busy(), "busy while it runs");
    Check(!build.Collect(), "no Collect while it runs");
    Check(!build.Start([] {}), "no second Start while it runs");
    release.store(true);
    Check(CollectWithin(build, 5000), "Collect says it ended");
    Check(result == 42, "the worker's write is visible after Collect");
    Check(worker != std::this_thread::get_id(), "the work ran on another thread");
    Check(!build.Running(), "not running after Collect");
    Check(!build.Busy(), "not busy after Collect");
    Check(!build.Collect(), "a result is collected once");
    // Collected: the next job starts.
    int second = 0;
    Check(build.Start([&] { second = 7; }), "Start after Collect accepted");
    Check(CollectWithin(build, 5000), "second job collected");
    Check(second == 7, "second job's write visible");
  }

  // An ended job that nobody collected holds the next Start off until it is.
  {
    BackgroundBuild build;
    Check(build.Start([] {}), "Start before the uncollected check");
    // Wait for it to end without collecting: Running goes false first.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (build.Running() && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!build.Running(), "ended job is not running");
    // The window the owners' gate (post.cpp, present.cpp) had wrong: an ended job is not Running
    // but is still Busy, so an owner that gates on Busy does not stage over its results.
    Check(build.Busy(), "ended job is still busy until collected");
    Check(!build.Start([] {}), "no Start before the ended job is collected");
    Check(build.Busy(), "a refused Start leaves the ended job in place");
    Check(build.Collect(), "the ended job is collected");
    Check(!build.Busy(), "collected: not busy");
    Check(build.Start([] {}), "Start after that");
    build.Join();
    Check(!build.Busy(), "not busy after Join");
  }

  // The owner's pattern, as post.cpp and present.cpp now run it: Collect, then stage and Start only
  // when not Busy. The job here ends between the Collect (which found it running) and the Busy
  // test: its staged result must survive, and the next round collects it before staging again.
  {
    BackgroundBuild build;
    std::atomic<bool> release{false};
    int staged = 0;
    const auto round = [&](int value, const std::function<void()>& between) {
      const bool collected = build.Collect();
      between();
      if (build.Busy()) {
        return collected;
      }
      staged = 0;  // the owner's reset of its staging
      release.store(false);
      build.Start([&staged, &release, value] {
        while (!release.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        staged = value;
      });
      return collected;
    };
    round(11, [] {});  // idle: stages and starts
    Check(build.Running(), "the owner's job is running");
    round(22, [&] {
      release.store(true);
      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (build.Running() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    Check(!build.Running() && build.Busy(), "the job ended, uncollected");
    Check(staged == 11, "the ended job's staged result was not reset or replaced");
    Check(round(33, [] {}), "the next round collects the ended job");
    release.store(true);
    Check(CollectWithin(build, 5000), "the job started after it is collected");
    Check(staged == 33, "the new job's result");
  }

  // Join waits for a running job (the owner's Shutdown), and the destructor joins too.
  {
    std::atomic<bool> done{false};
    {
      BackgroundBuild build;
      build.Start([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        done.store(true);
      });
      build.Join();
      Check(done.load(), "Join waited for the job");
      Check(!build.Running(), "not running after Join");
    }
    std::atomic<bool> done_in_destructor{false};
    {
      BackgroundBuild build;
      build.Start([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        done_in_destructor.store(true);
      });
    }
    Check(done_in_destructor.load(), "the destructor waited for the job");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

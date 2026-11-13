// MCLA GPU renderer -- a feature's pipelines made in the background. See background_build.h.

#include "background_build.h"

#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mcla::gpu {

BackgroundBuild::~BackgroundBuild() {
  Join();
}

bool BackgroundBuild::Start(std::function<void()> work) {
  if (state_.load(std::memory_order_acquire) != kIdle) {
    return false;
  }
  state_.store(kRunning, std::memory_order_relaxed);
  thread_ = std::thread([this, work = std::move(work)] {
#if defined(_WIN32)
    // As the host precache's builder (host_precache.cpp): the game runs meanwhile.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    work();
    state_.store(kEnded, std::memory_order_release);
  });
  return true;
}

bool BackgroundBuild::Collect() {
  if (state_.load(std::memory_order_acquire) != kEnded) {
    return false;
  }
  thread_.join();
  state_.store(kIdle, std::memory_order_relaxed);
  return true;
}

void BackgroundBuild::Join() {
  if (thread_.joinable()) {
    thread_.join();
  }
  state_.store(kIdle, std::memory_order_relaxed);
}

}  // namespace mcla::gpu

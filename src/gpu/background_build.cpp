// MCLA GPU renderer -- a feature's pipelines made in the background. See background_build.h.

#include "background_build.h"

#include <utility>

#include "os/thread_priority.h"

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
    // As the host precache's builder (host_precache.cpp): the game runs meanwhile, and nothing
    // waits for this job (the frames until it ends are drawn without the effect). Windows: below
    // normal, as always; Linux: SCHED_BATCH and a higher nice value (os/thread_priority.h). The
    // thread ends with the job, so the lowering is never undone.
    larecomp::os::SetCurrentThreadIntent(larecomp::os::ThreadIntent::kBackground);
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

// MCLA GPU renderer -- texture decodes on worker threads. See texture_decode_pool.h.

#include "texture_decode_pool.h"

#include <algorithm>
#include <chrono>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/thread.h>

#include "mc_engine/threading.h"

namespace mcla::gpu {

namespace {

inline void CpuRelax() {
#if defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

// How long Finish polls for a job a worker is running before it sleeps on the condition: a job is
// tens to a few hundred microseconds, a wake-up costs about as much.
constexpr uint32_t kSpinsBeforeSleep = 2000;

}  // namespace

void DecodeJob::HoldBytes(DecodePool& pool, uint64_t bytes) {
  ReleaseBytes();
  budget_ = pool.held_;
  bytes_ = bytes;
  budget_->fetch_add(bytes, std::memory_order_relaxed);
}

uint32_t DecodePool::AutoThreads() {
  unsigned usable = ::mc::UsableLogicalProcessors();
  if (usable == 0) {
    usable = std::thread::hardware_concurrency();
  }
  return std::min(3u, usable / 4);
}

void DecodePool::Start(uint32_t threads) {
  if (!workers_.empty() || threads == 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = false;
  }
  for (uint32_t i = 0; i < threads; ++i) {
    workers_.emplace_back([this, i] { WorkerMain(i); });
  }
}

void DecodePool::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
  submitted_.clear();
}

void DecodePool::Submit(const std::shared_ptr<DecodeJob>& job) {
  if (workers_.empty()) {
    return;
  }
  // The finished and cancelled ones go from the list when it grows (it is cleared by Quiesce
  // at every fence too).
  if (submitted_.size() >= 512) {
    submitted_.erase(std::remove_if(submitted_.begin(), submitted_.end(),
                                    [](const std::shared_ptr<DecodeJob>& j) {
                                      return j->state() >= DecodeJob::kDone;
                                    }),
                     submitted_.end());
  }
  submitted_.push_back(job);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(job);
  }
  work_cv_.notify_one();
}

void DecodePool::NotifyDone() {
  {
    // The waiter checks the state under this lock before it sleeps: taking it here, after the
    // state was stored, means a wake-up is never lost.
    std::lock_guard<std::mutex> lock(done_mutex_);
  }
  done_cv_.notify_all();
}

void DecodePool::WaitUntilSettled(DecodeJob& job) {
  for (uint32_t spin = 0; job.state() < DecodeJob::kDone; ++spin) {
    if (spin < kSpinsBeforeSleep) {
      CpuRelax();
      continue;
    }
    std::unique_lock<std::mutex> lock(done_mutex_);
    done_cv_.wait_for(lock, std::chrono::milliseconds(1),
                      [&job] { return job.state() >= DecodeJob::kDone; });
  }
}

bool DecodePool::Finish(DecodeJob& job) {
  uint32_t expected = DecodeJob::kQueued;
  if (job.state_.compare_exchange_strong(expected, DecodeJob::kRunning,
                                         std::memory_order_acq_rel)) {
    job.Run();
    job.state_.store(DecodeJob::kDone, std::memory_order_release);
    return true;
  }
  WaitUntilSettled(job);
  return job.state() == DecodeJob::kDone;
}

void DecodePool::Quiesce(uint64_t before) {
  size_t settled = 0;
  for (; settled < submitted_.size(); ++settled) {
    DecodeJob& job = *submitted_[settled];
    if (job.sequence >= before) {
      break;
    }
    uint32_t expected = DecodeJob::kQueued;
    if (job.state_.compare_exchange_strong(expected, DecodeJob::kCancelled,
                                           std::memory_order_acq_rel)) {
      continue;
    }
    WaitUntilSettled(job);
  }
  submitted_.erase(submitted_.begin(), submitted_.begin() + std::ptrdiff_t(settled));
}

void DecodePool::WorkerMain(uint32_t index) {
  rex::thread::set_current_thread_name("larecomp texture decode");
  (void)index;
#if defined(_WIN32)
  // Below the game's threads: what they leave idle is what this uses.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  for (;;) {
    std::shared_ptr<DecodeJob> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) {
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    uint32_t expected = DecodeJob::kQueued;
    if (!job->state_.compare_exchange_strong(expected, DecodeJob::kRunning,
                                             std::memory_order_acq_rel)) {
      continue;  // the render thread took it over, or it was cancelled
    }
    job->Run();
    job->state_.store(DecodeJob::kDone, std::memory_order_release);
    ran_on_workers_.fetch_add(1, std::memory_order_relaxed);
    NotifyDone();
  }
}

}  // namespace mcla::gpu

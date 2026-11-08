#pragma once
// ===========================================================================
// MCLA GPU renderer -- texture decodes on worker threads
// ===========================================================================
// A small pool of threads that run DecodeJobs: the CPU half of a texture upload (untile, byte swap,
// load; texture_decode.h) for textures the render thread will need a few draws from now
// (TextureCache::Prefetch). The render thread only waits for a job when the draw that needs the
// texture comes up (Finish), and takes the job over itself when no worker has started it, so a pool
// that is busy or starved costs no more than decoding in place.
//
// A job is claimed once: kQueued -> kRunning by whoever gets there first (a worker, or the render
// thread in Finish), then kDone; kQueued -> kCancelled by Quiesce. Whatever a job writes is
// published by the release store of kDone and read after Finish saw it.
//
// The workers run below the game's threads (THREAD_PRIORITY_BELOW_NORMAL): they take the cores the
// game leaves idle, and what they have not started by the time the render thread needs it, the
// render thread decodes.
//
// What a job may read: guest memory of a draw that has not executed yet. The guest may reuse that
// memory once the render thread publishes a fence (Renderer::ExecuteFence), so Quiesce(fence
// sequence) cancels the jobs of earlier records that have not started and waits for those running
// before the fence is published.
//
// Threading: Start, Stop, Submit, Finish and Quiesce from the render thread only; Run on workers.
// ===========================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mcla::gpu {

class DecodePool;

class DecodeJob {
 public:
  enum State : uint32_t { kQueued = 0, kRunning = 1, kDone = 2, kCancelled = 3 };

  DecodeJob() = default;
  DecodeJob(const DecodeJob&) = delete;
  DecodeJob& operator=(const DecodeJob&) = delete;
  virtual ~DecodeJob() { ReleaseBytes(); }

  State state() const { return State(state_.load(std::memory_order_acquire)); }
  // Done: Run finished and its results may be read.
  bool done() const { return state() == kDone; }

  // The stream sequence of the record that will use the result (Quiesce compares it with a
  // fence's).
  uint64_t sequence = 0;

 protected:
  // Runs once, on a worker or on the render thread.
  virtual void Run() = 0;
  // Counts `bytes` of the result's memory against the pool's budget until the job is destroyed.
  void HoldBytes(DecodePool& pool, uint64_t bytes);

 private:
  friend class DecodePool;
  void ReleaseBytes() {
    if (budget_) {
      budget_->fetch_sub(bytes_, std::memory_order_relaxed);
      budget_.reset();
    }
  }

  std::atomic<uint32_t> state_{kQueued};
  std::shared_ptr<std::atomic<uint64_t>> budget_;
  uint64_t bytes_ = 0;
};

class DecodePool {
 public:
  // Result memory the jobs alive may hold (queued, running, or done and not yet taken).
  static constexpr uint64_t kMaxHeldBytes = 96ull << 20;

  DecodePool() : held_(std::make_shared<std::atomic<uint64_t>>(0)) {}
  ~DecodePool() { Stop(); }
  DecodePool(const DecodePool&) = delete;
  DecodePool& operator=(const DecodePool&) = delete;

  // Workers to start for an unset (-1) setting: 1 per 4 usable logical processors, 3 at most, none
  // below 4.
  static uint32_t AutoThreads();

  // `threads` workers (0: none, Submit does nothing).
  void Start(uint32_t threads);
  // Joins the workers (a running job finishes, queued ones are dropped).
  void Stop();
  bool running() const { return !workers_.empty(); }
  uint32_t threads() const { return uint32_t(workers_.size()); }

  // True when a job with `bytes` of results fits in the budget.
  bool Fits(uint64_t bytes) const {
    return held_->load(std::memory_order_relaxed) + bytes <= kMaxHeldBytes;
  }
  uint64_t held_bytes() const { return held_->load(std::memory_order_relaxed); }

  // Queues `job` for the workers. Without workers nothing is queued (Finish still runs it).
  void Submit(const std::shared_ptr<DecodeJob>& job);

  // The job is done, or false when it was cancelled. Runs it here when no worker has started it;
  // waits for a worker that has.
  bool Finish(DecodeJob& job);

  // Before a fence with stream sequence `before` is published: the queued jobs of records earlier
  // than it are cancelled, the running ones waited for. Jobs of later records stay queued.
  void Quiesce(uint64_t before);

  // Jobs run on workers since the last call / jobs the render thread ran itself in Finish.
  uint64_t TakeRanOnWorkers() { return ran_on_workers_.exchange(0, std::memory_order_relaxed); }

 private:
  friend class DecodeJob;
  void WorkerMain(uint32_t index);
  void WaitUntilSettled(DecodeJob& job);
  void NotifyDone();

  std::shared_ptr<std::atomic<uint64_t>> held_;

  std::mutex mutex_;  // guards queue_ and stop_
  std::condition_variable work_cv_;
  std::deque<std::shared_ptr<DecodeJob>> queue_;
  bool stop_ = false;
  std::vector<std::thread> workers_;

  std::mutex done_mutex_;
  std::condition_variable done_cv_;

  // Render thread only: the jobs submitted and not yet settled by a Quiesce, oldest first.
  std::vector<std::shared_ptr<DecodeJob>> submitted_;
  std::atomic<uint64_t> ran_on_workers_{0};
};

}  // namespace mcla::gpu

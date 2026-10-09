// MCLA GPU renderer -- the render thread. See render_thread.h.
//
// Design followed: reeot gpu/render_thread.cpp (BSD-3-Clause) -- bounded ring,
// single consumer, producers blocking on a full ring, Present waited by
// sequence. The lock-free publication, spin-waiting and draw sorting there are
// throughput work that waits for measurements here.

#include "render_thread.h"

#include <rex/logging.h>
#include <rex/thread.h>

namespace mcla::gpu {

RenderThread::~RenderThread() {
  Stop(std::chrono::milliseconds(2000));
}

bool RenderThread::Start(Executor& executor, std::chrono::milliseconds idle_period) {
  if (running_) {
    return true;
  }
  executor_ = &executor;
  idle_period_ = idle_period;
  ring_ = std::make_unique<RenderCommand[]>(kSlots);
  head_ = tail_ = 0;
  last_sequence_ = executed_sequence_ = 0;
  stop_ = false;
  exited_ = false;
  running_ = true;
  thread_ = std::thread([this] { Main(); });
  return true;
}

bool RenderThread::Stop(std::chrono::milliseconds deadline) {
  if (!running_) {
    return true;
  }
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  space_cv_.notify_all();
  done_cv_.notify_all();
  bool exited;
  {
    std::unique_lock lock(mutex_);
    exited = exit_cv_.wait_for(lock, deadline, [&] { return exited_; });
  }
  running_ = false;
  if (!exited) {
    REXLOG_ERROR("[gpu] the render thread did not stop within {} ms; it is left running and "
                 "nothing it uses is destroyed",
                 deadline.count());
    thread_.detach();
    return false;
  }
  thread_.join();
  executor_ = nullptr;
  return true;
}

void RenderThread::WaitExecuted(uint64_t sequence) {
  if (sequence == 0) {
    return;
  }
  std::unique_lock lock(mutex_);
  done_cv_.wait(lock, [&] { return stop_ || executed_sequence_ >= sequence; });
}

void RenderThread::Main() {
  rex::thread::set_current_thread_name("larecomp gpu render");
  for (;;) {
    RenderCommand* command = nullptr;
    {
      std::unique_lock lock(mutex_);
      const bool woke = work_cv_.wait_for(lock, idle_period_,
                                          [&] { return stop_ || head_ != tail_; });
      if (stop_) {
        break;
      }
      if (woke) {
        // The slot stays untouched by producers until head_ moves past it.
        command = &ring_[head_ % kSlots];
      }
    }
    if (!command) {
      executor_->OnIdle();
      continue;
    }
    executor_->Execute(*command);
    {
      std::lock_guard lock(mutex_);
      executed_sequence_ = command->sequence;
      ++head_;
    }
    space_cv_.notify_one();
    done_cv_.notify_all();
  }
  {
    std::lock_guard lock(mutex_);
    exited_ = true;
  }
  exit_cv_.notify_all();
}

}  // namespace mcla::gpu

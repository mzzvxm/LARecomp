// MCLA GPU renderer -- the render thread. See render_thread.h.
//
// Design followed: reeot gpu/render_thread.cpp (BSD-3-Clause) -- single
// consumer, producers blocking only on a full queue, Present waited by
// sequence. The queue itself is stream.cpp.

#include "render_thread.h"

#include <rex/logging.h>
#include <rex/thread.h>

namespace mcla::gpu {

RenderThread::~RenderThread() {
  Stop(std::chrono::milliseconds(2000));
}

bool RenderThread::Start(Executor& executor, std::chrono::milliseconds idle_period,
                         uint32_t stream_capacity) {
  if (running_) {
    return true;
  }
  if (!stream_.Initialize(stream_capacity)) {
    return false;
  }
  executor_ = &executor;
  idle_period_ = idle_period;
  exited_ = false;
  running_ = true;
  thread_ = std::thread([this] { Main(); });
  return true;
}

bool RenderThread::Stop(std::chrono::milliseconds deadline) {
  if (!running_) {
    return true;
  }
  stream_.Stop();
  bool exited;
  {
    std::unique_lock lock(exit_mutex_);
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

void RenderThread::Main() {
  rex::thread::set_current_thread_name("larecomp gpu render");
  using Clock = std::chrono::steady_clock;
  auto next_idle = Clock::now() + idle_period_;
  while (!stream_.stopped()) {
    const RecordHeader* record = stream_.Next();
    if (record) {
      executor_->Execute(*record, reinterpret_cast<const uint8_t*>(record + 1));
      const bool sync = (record->flags & kRecordSync) != 0;
      const uint64_t sequence = record->sequence;
      stream_.Release(record);
      if (sync) {
        stream_.MarkExecuted(sequence);
      }
    } else {
      const auto now = Clock::now();
      if (now < next_idle) {
        stream_.WaitForWork(std::chrono::duration_cast<std::chrono::milliseconds>(next_idle - now) +
                            std::chrono::milliseconds(1));
        continue;
      }
    }
    const auto now = Clock::now();
    if (now >= next_idle) {
      executor_->OnIdle();
      next_idle = Clock::now() + idle_period_;
    }
  }
  {
    std::lock_guard lock(exit_mutex_);
    exited_ = true;
  }
  exit_cv_.notify_all();
}

}  // namespace mcla::gpu

// MCLA GPU renderer -- the render thread. See render_thread.h.
//
// Design followed: reeot gpu/render_thread.cpp (BSD-3-Clause) -- single
// consumer, producers blocking only on a full queue, Present waited by
// sequence. The queue itself is stream.cpp.

#include "render_thread.h"

#include <string>

#include <rex/logging/log_category.h>
#include <rex/platform/thread.h>

#include <atomic>

#include "cycles.h"
#include "os/thread_name.h"
#include "os/thread_qos.h"
#include "present_schedule.h"
#include "render_wait_rules.h"
#include "settings/larecomp_app_settings.h"
#include "thread_load.h"

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
  // Linux keeps 15 bytes of a thread name (os/thread_name.h): the long name stays on Windows, where the profilers look for it.
  rex::platform::SetCurrentThreadName(larecomp::os::ThreadNameFor("larecomp gpu render"));
  // thread_qos 2: the render thread stays HighQoS when the window is out of focus (os/thread_qos.h; nothing when the setting is lower).
  if (const std::string qos_line = larecomp::os::TagCurrentThreadHighQos("larecomp gpu render"); !qos_line.empty()) {
    REXLOG_INFO("{}", qos_line);
  }
  std::atomic<bool> spin_wait{false};
  auto wait_mode = larecomp::settings::render_wait_mode.Bind([&](const std::string& mode) {
    spin_wait.store(render_wait::ShouldSpin(mode), std::memory_order_relaxed);
  });
  using Clock = std::chrono::steady_clock;
  auto next_idle = Clock::now() + idle_period_;
  // While records keep coming the idle deadline is checked every
  // kRecordsPerClockRead of them: the clock (QueryPerformanceFrequency +
  // QueryPerformanceCounter in MSVC's steady_clock) read after every record
  // cost a measurable share of the thread at ~8000 records a frame. A record takes microseconds: the check stays far finer than
  // the 16 ms period.
  constexpr uint32_t kRecordsPerClockRead = 64;
  uint32_t records_since_clock = 0;
  ThreadLoad& load = CurrentThreadLoad();
  while (!stream_.stopped()) {
    // A held present goes out within a record's time of when it is due.
    const int64_t due = executor_->DeferredDue();
    if (due != 0 && QpcNow() >= due) {
      executor_->RunDeferred();
      continue;
    }
    const RecordHeader* record = stream_.Next();
    if (record) {
      executor_->Execute(*record, reinterpret_cast<const uint8_t*>(record + 1));
      const bool sync = (record->flags & kRecordSync) != 0;
      const uint64_t sequence = record->sequence;
      stream_.Release(record);
      if (sync) {
        stream_.MarkExecuted(sequence);
      }
      if (due == 0 && ++records_since_clock < kRecordsPerClockRead) {
        continue;
      }
      records_since_clock = 0;
    } else if (due != 0) {
      if (spin_wait.load(std::memory_order_relaxed)) {
        // Legacy A/B path: sleep to about a millisecond before the present, then yield.
        const int64_t per_ms = QpcFrequency() / 1000;
        const int64_t remaining = due - QpcNow();
        if (remaining <= 2 * per_ms) {
          std::this_thread::yield();
          continue;
        }
        const uint64_t idle_start = cycles::Now();
        stream_.WaitForWork(std::chrono::milliseconds((remaining - per_ms) / per_ms), true);
        load.AddIdle(cycles::Now() - idle_start);
      } else {
        const uint64_t idle_start = cycles::Now();
        stream_.WaitForPresentWork(due);
        load.AddIdle(cycles::Now() - idle_start);
      }
      continue;
    } else {
      const auto now = Clock::now();
      if (now < next_idle) {
        const uint64_t idle_start = cycles::Now();
        const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(next_idle - now) +
                             std::chrono::milliseconds(1);
        stream_.WaitForWork(timeout, spin_wait.load(std::memory_order_relaxed));
        load.AddIdle(cycles::Now() - idle_start);
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

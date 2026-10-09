#pragma once
// ===========================================================================
// MCLA GPU renderer -- the render thread
// ===========================================================================
// The only thread that records command lists, submits, presents and retires
// GPU objects. Guest threads describe work as RenderCommands in a bounded ring
// and go on; the render thread executes them in order.
//
// Why a thread of its own when reblue records on the guest thread: the guest's
// own CPU work already takes ~5-8 ms of the frame in MCLA, against a target of
// 100-120 fps (8.3-10 ms), so recording cannot share its thread -- the legacy
// D3D12 runtime reached the same conclusion with its submission thread. reeot
// is the existence proof of this shape over Plume (gpu/render_thread.cpp:
// guest captures packets, a single consumer replays them, Present is enqueued
// and the guest waits for it).
//
// Commands are filled in place in their ring slot (no allocation per command).
// Today the ring carries only Present; draw, clear, resolve, unlock and
// readback commands are added with the subsystems that execute them.
//
// Idle: when no command arrives for an idle period the executor's OnIdle runs
// (and again every period while it stays idle). The renderer uses it to keep
// presenting the overlays while the guest does not swap -- before the guest
// exists (first-launch profile dialog, SDK wizards) and during long stalls.
//
// Stop has a deadline: a render thread stuck in a driver or DXGI call (a
// present waiting on the very UI thread that is stopping it is the known case)
// is let go rather than joined forever; the caller is told, and must then keep
// everything the thread can reach alive.
//
// Threading: Enqueue/Wait from any thread (producers serialize on a mutex);
// the executor runs on the render thread only.
// ===========================================================================

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace mcla::gpu {

enum class RenderCommandType : uint8_t {
  kPresent,
};

// Carries nothing yet. The presentation pass (M4) adds the front buffer the
// guest swaps -- D3DDevice_Swap's r4 is the D3DTexture* grcDevice::EndFrame
// resolved RT0 into -- together with
// the code that samples it.
struct PresentCommand {};

struct RenderCommand {
  RenderCommandType type = RenderCommandType::kPresent;
  uint64_t sequence = 0;
  PresentCommand present;
};

class RenderThread {
 public:
  class Executor {
   public:
    virtual ~Executor() = default;
    virtual void Execute(RenderCommand& command) = 0;
    virtual void OnIdle() = 0;
  };

  RenderThread() = default;
  ~RenderThread();
  RenderThread(const RenderThread&) = delete;
  RenderThread& operator=(const RenderThread&) = delete;

  bool Start(Executor& executor, std::chrono::milliseconds idle_period);
  // Stops after the command being executed, if any; commands still queued are
  // dropped. Wakes every producer and waiter, then waits up to `deadline` for
  // the thread to exit. True when it exited (and was joined); false when it was
  // detached still running -- then nothing it can reach may be destroyed.
  bool Stop(std::chrono::milliseconds deadline);
  bool running() const { return running_; }

  // Fills the next ring slot in place and publishes it. Blocks while the ring
  // is full. Returns the command's sequence, or 0 if the thread is stopped (the
  // command was not queued).
  template <typename Fill>
  uint64_t Enqueue(Fill&& fill) {
    std::unique_lock lock(mutex_);
    space_cv_.wait(lock, [&] { return stop_ || tail_ - head_ < kSlots; });
    if (stop_) {
      return 0;
    }
    RenderCommand& command = ring_[tail_ % kSlots];
    command = RenderCommand{};
    std::forward<Fill>(fill)(command);
    command.sequence = ++last_sequence_;
    ++tail_;
    work_cv_.notify_one();
    return command.sequence;
  }

  // Blocks until the command with `sequence` has executed, or the thread
  // stopped.
  void WaitExecuted(uint64_t sequence);

 private:
  static constexpr uint64_t kSlots = 256;

  void Main();

  Executor* executor_ = nullptr;
  std::chrono::milliseconds idle_period_{16};
  std::unique_ptr<RenderCommand[]> ring_;
  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable space_cv_;
  std::condition_variable done_cv_;
  std::condition_variable exit_cv_;
  uint64_t head_ = 0;  // next slot to execute
  uint64_t tail_ = 0;  // next slot to fill
  uint64_t last_sequence_ = 0;
  uint64_t executed_sequence_ = 0;
  bool stop_ = false;
  bool exited_ = false;
  bool running_ = false;
  std::thread thread_;
};

}  // namespace mcla::gpu

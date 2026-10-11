#pragma once
// ===========================================================================
// MCLA GPU renderer -- the render thread
// ===========================================================================
// The only thread that records command lists, submits, presents and retires
// GPU objects. Guest threads describe work as records in the CommandStream
// (stream.h) and go on; the render thread executes them in order.
//
// Why a thread of its own when reblue records on the guest thread: the guest's
// own CPU work already takes ~5-8 ms of the frame in MCLA, against a target of
// 100-120 fps (8.3-10 ms), so recording cannot share its thread -- the legacy
// D3D12 runtime reached the same conclusion with its submission thread. reeot
// is the existence proof of this shape over Plume (gpu/render_thread.cpp:
// guest captures packets, a single consumer replays them, Present is enqueued
// and the guest waits for it).
//
// Idle: the executor's OnIdle runs at least once per idle period, whether
// records are flowing or not. The renderer uses it to keep presenting the
// overlays while the guest does not swap -- before the guest exists
// (first-launch profile dialog, SDK wizards) and during long stalls -- and
// returns at once while the guest swaps by itself.
//
// Stop has a deadline: a render thread stuck in a driver or DXGI call (a
// present waiting on the very UI thread that is stopping it is the known case)
// is let go rather than joined forever; the caller is told, and must then keep
// everything the thread can reach alive. The stream is stopped first, so no
// producer or waiter stays blocked on it.
//
// Threading: Start/Stop from the lifecycle thread; stream() producers from any
// thread; the executor runs on the render thread only.
// ===========================================================================

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "records.h"
#include "stream.h"

namespace mcla::gpu {

class RenderThread {
 public:
  class Executor {
   public:
    virtual ~Executor() = default;
    // One record; the payload follows the header (records.h). Never kPad.
    virtual void Execute(const RecordHeader& header, const uint8_t* payload) = 0;
    virtual void OnIdle() = 0;
  };

  RenderThread() = default;
  ~RenderThread();
  RenderThread(const RenderThread&) = delete;
  RenderThread& operator=(const RenderThread&) = delete;

  bool Start(Executor& executor, std::chrono::milliseconds idle_period,
             uint32_t stream_capacity = CommandStream::kDefaultCapacity);
  // Stops after the record being executed, if any; records still queued are
  // dropped. Releases every producer and waiter, then waits up to `deadline`
  // for the thread to exit. True when it exited (and was joined); false when it
  // was detached still running -- then nothing it can reach may be destroyed.
  bool Stop(std::chrono::milliseconds deadline);
  bool running() const { return running_; }

  // Producers write here (guest_capture.cpp). Valid from Start on, for the
  // life of this object.
  CommandStream& stream() { return stream_; }

 private:
  void Main();

  CommandStream stream_;
  Executor* executor_ = nullptr;
  std::chrono::milliseconds idle_period_{16};
  std::mutex exit_mutex_;
  std::condition_variable exit_cv_;
  bool exited_ = false;
  bool running_ = false;
  std::thread thread_;
};

}  // namespace mcla::gpu

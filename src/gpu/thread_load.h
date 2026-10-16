#pragma once
// ===========================================================================
// MCLA GPU renderer -- a thread's time: CPU, GPU waits, idle (M6a)
// ===========================================================================
// The render thread idles in
// CommandStream::WaitForWork and waits for the GPU at the frame slots' fences
// and in the swap chain (acquire, present, present wait); the rest of its time
// is CPU work -- what the M6 targets budget. The waits add themselves to the
// calling thread's ThreadLoad (thread_local, so a wait on another thread never
// touches the render thread's), and the Scene takes the render thread's at
// each statistics window. Times are TSC cycles (cycles.h).
// ===========================================================================

#include <cstdint>

#include "cycles.h"

namespace mcla::gpu {

class ThreadLoad {
 public:
  struct Sample {
    uint64_t total_cycles = 0;  // since the previous Take (0 at the first)
    uint64_t idle_cycles = 0;
    uint64_t gpu_wait_cycles = 0;
  };

  void AddIdle(uint64_t cycle_count) { idle_cycles_ += cycle_count; }
  void AddGpuWait(uint64_t cycle_count) { gpu_wait_cycles_ += cycle_count; }

  Sample Take() {
    const uint64_t now = cycles::Now();
    const Sample s{last_take_ ? now - last_take_ : 0, idle_cycles_, gpu_wait_cycles_};
    last_take_ = now;
    idle_cycles_ = 0;
    gpu_wait_cycles_ = 0;
    return s;
  }

 private:
  uint64_t last_take_ = 0;
  uint64_t idle_cycles_ = 0;
  uint64_t gpu_wait_cycles_ = 0;
};

// The calling thread's.
ThreadLoad& CurrentThreadLoad();

// Adds its lifetime to the calling thread's GPU waits.
class ScopedGpuWait {
 public:
  ScopedGpuWait() : start_(cycles::Now()) {}
  ~ScopedGpuWait() { CurrentThreadLoad().AddGpuWait(cycles::Now() - start_); }
  ScopedGpuWait(const ScopedGpuWait&) = delete;
  ScopedGpuWait& operator=(const ScopedGpuWait&) = delete;

 private:
  uint64_t start_;
};

}  // namespace mcla::gpu

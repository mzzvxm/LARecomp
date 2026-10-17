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
// each statistics window. With mcla_gpu_scene_profile the Renderer also adds
// each record's CPU time (its time less the GPU waits inside it) by type.
// Times are TSC cycles (cycles.h); the totals only grow, and Take returns
// what they grew by since the previous Take.
// ===========================================================================

#include <array>
#include <cstdint>

#include "cycles.h"

namespace mcla::gpu {

class ThreadLoad {
 public:
  static constexpr uint32_t kRecordTypes = 16;  // >= RecordType::kCount (records.h)

  struct Sample {
    uint64_t total_cycles = 0;  // since the previous Take (0 at the first)
    uint64_t idle_cycles = 0;
    uint64_t gpu_wait_cycles = 0;
    std::array<uint64_t, kRecordTypes> record_cycles{};  // CPU, by record type
  };

  void AddIdle(uint64_t cycle_count) { totals_.idle_cycles += cycle_count; }
  void AddGpuWait(uint64_t cycle_count) { totals_.gpu_wait_cycles += cycle_count; }
  void AddRecord(uint32_t type, uint64_t cycle_count) {
    if (type < kRecordTypes) {
      totals_.record_cycles[type] += cycle_count;
    }
  }
  uint64_t gpu_wait_total() const { return totals_.gpu_wait_cycles; }

  Sample Take() {
    const uint64_t now = cycles::Now();
    Sample s;
    s.total_cycles = last_take_ ? now - last_take_ : 0;
    s.idle_cycles = totals_.idle_cycles - taken_.idle_cycles;
    s.gpu_wait_cycles = totals_.gpu_wait_cycles - taken_.gpu_wait_cycles;
    for (uint32_t i = 0; i < kRecordTypes; ++i) {
      s.record_cycles[i] = totals_.record_cycles[i] - taken_.record_cycles[i];
    }
    last_take_ = now;
    taken_ = totals_;
    return s;
  }

 private:
  uint64_t last_take_ = 0;
  Sample totals_;  // total_cycles unused
  Sample taken_;   // the totals at the previous Take
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

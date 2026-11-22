#pragma once
// ===========================================================================
// Render-thread CPU: the switches and the pure parts of the small fixes that
// RENDER-CPU-HOT (perf\RENDER-CPU-HOT.md) found in the host profile (std only;
// render_cpu_test.cpp)
// ===========================================================================
// The debug-only setting render_cpu_mask (read at start) holds one bit per fix:
// 0 runs every path as it was, 7 (the default) runs them all. The bits are the
// A/B switch of perf\scripts\chain-rendercpu.ps1; the render thread's own parts
// also follow ab::B() (ab.h, mcla_gpu_ab_interleave), so one run can alternate
// the two in its statistics windows.
//
// Threading: pure.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu::render_cpu {

enum Bits : int32_t {
  kSampledDrawTimers = 1,  // the per-draw TSC timers of the statistics read the clock on one draw in kPeriod
  kAliasHint = 2,          // the physical range check asks the alias that answered last first (native_gfx/guest/physical_alias.h)
  kDeviceCache = 4,        // the per-draw lookups by device address keep their last answer (constants.cpp, scene.cpp)
  kAll = 7,
};

constexpr bool Has(int32_t mask, Bits bit) {
  return (mask & bit) != 0;
}

// "Time this draw?" for a statistic that sums the time of every draw. Reading the TSC costs
// about 7 ns on the test PC (4 reads a draw, ~3800 draws a frame: 0.1 ms a frame, about 3% of
// what the render thread does in a race); one draw in kPeriod is timed and its time counted
// kPeriod times, so the window's sum is an estimate of the exact one. kPeriod is odd: no pattern
// of the draws' order with a period of 2, 4 or 8 falls on the timed ones every time.
// Not sampled (Configure(false)): every draw is timed and Scale is the identity, as before.
class DrawSampler {
 public:
  static constexpr uint32_t kPeriod = 7;

  void Configure(bool sampled) {
    period_ = sampled ? kPeriod : 1u;
    countdown_ = 1;
  }
  bool sampled() const { return period_ != 1u; }
  // True on the draws whose time is to be read.
  bool Take() {
    if (--countdown_ != 0) {
      return false;
    }
    countdown_ = period_;
    return true;
  }
  // The time measured on a taken draw, as the estimate for the draws it stands for.
  uint64_t Scale(uint64_t measured) const { return measured * period_; }

 private:
  uint32_t period_ = 1;
  uint32_t countdown_ = 1;
};

}  // namespace mcla::gpu::render_cpu

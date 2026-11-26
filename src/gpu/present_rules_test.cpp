// Standalone check for the headless present test and its wait totals.

#include <cstdio>

#include "present_rules.h"
#include "thread_load.h"

namespace {

int checks = 0;
int failed = 0;

void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failed;
    std::printf("FAILED: %s\n", what);
  }
}

}  // namespace

int main() {
  using namespace mcla::gpu;
  using namespace std::chrono_literals;
  Check(SwapChainFrameLatency(2, false, false) == 2, "normal frame latency follows the frame ring");
  Check(SwapChainFrameLatency(2, false, true) == 1, "the D3D12 test requests one frame");
  Check(SwapChainFrameLatency(2, true, false) == 3, "the proxy has room for generated frames");
  Check(SwapChainFrameLatency(2, true, true) == 3, "the test leaves proxy latency alone");
  constexpr int32_t kOccluded = 0x087A0001;
  constexpr int32_t kDeviceRemoved = static_cast<int32_t>(0x887A0005u);
  Check(ClassifyDxgiPresentNowait(0) == PresentResult::kPresented, "S_OK presents");
  Check(ClassifyDxgiPresentNowait(kOccluded) == PresentResult::kPresented,
        "positive DXGI status preserves Plume's success result");
  const auto dropped = ClassifyDxgiPresentNowait(static_cast<int32_t>(0x887A000Au));
  Check(dropped == PresentResult::kDropped, "WAS_STILL_DRAWING drops");
  Check(ClassifyDxgiPresentNowait(kDeviceRemoved) == PresentResult::kFailed,
        "device removal keeps the failure path");

  PresentDrops drops;
  const PresentDrops::Clock::time_point start{};
  Check(drops.OnPresent(PresentResult::kPresented, start) == 0, "no line for success");
  Check(drops.OnPresent(dropped, start + 1ms) == 0, "first drop waits for the interval");
  Check(drops.OnPresent(PresentResult::kFailed, start + 2ms) == 0, "failure does not flush");
  Check(drops.OnPresent(dropped, start + 999ms) == 0, "stays quiet before the boundary");
  Check(drops.OnPresent(dropped, start + 1s) == 3, "boundary includes the current drop");
  Check(drops.total() == 3, "success and failure do not count");
  Check(drops.OnPresent(dropped, start + 1s) == 0, "at most one line at the boundary");
  Check(drops.OnPresent(PresentResult::kPresented, start + 2s) == 1,
        "success flushes pending drops without recounting the previous interval");
  Check(drops.OnPresent(PresentResult::kPresented, start + 3s) == 0, "empty interval is quiet");
  Check(drops.OnPresent(dropped, start + 3100ms) == 0, "new partial interval stays pending");
  Check(drops.TakePending() == 1, "shutdown flushes the partial interval");
  Check(drops.TakePending() == 0, "flush drains the pending counter");
  Check(drops.total() == 5, "session count survives every flush");

  ThreadLoad load;
  load.AddGpuWait(50);
  load.AddGpuWaitDetail(GpuWaitKind::kPresentCall, 9);
  load.AddGpuWaitDetail(GpuWaitKind::kFrameFence, 15);
  load.AddGpuWaitDetail(GpuWaitKind::kAcquire, 4);
  load.AddGpuWaitDetail(GpuWaitKind::kPresentSlot, 2);
  const auto first = load.Take();
  Check(first.gpu_wait_cycles == 50 && load.gpu_wait_total() == 50, "old total includes each wait once");
  Check(first.GpuWaitCycles(GpuWaitKind::kPresentCall) == 9, "present cycles");
  Check(first.GpuWaitCycles(GpuWaitKind::kFrameFence) == 15, "frame fence cycles");
  Check(first.GpuWaitCycles(GpuWaitKind::kAcquire) == 4, "acquire cycles");
  Check(first.GpuWaitCycles(GpuWaitKind::kPresentSlot) == 2, "present slot cycles");
  load.AddGpuWait(7);
  load.AddGpuWaitDetail(GpuWaitKind::kPresentCall, 5);
  const auto next = load.Take();
  Check(next.gpu_wait_cycles == 7 && load.gpu_wait_total() == 57, "interval and cumulative totals");
  Check(next.GpuWaitCycles(GpuWaitKind::kPresentCall) == 5, "present cycles are per interval");
  Check(next.GpuWaitCycles(GpuWaitKind::kFrameFence) == 0, "fence cycles are per interval");
  Check(next.GpuWaitCycles(GpuWaitKind::kAcquire) == 0, "acquire cycles are per interval");
  Check(next.GpuWaitCycles(GpuWaitKind::kPresentSlot) == 0, "present slot cycles are per interval");

  std::printf("present rules: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}

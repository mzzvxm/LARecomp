#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

namespace mcla::gpu {

enum class PresentResult : uint8_t { kPresented, kDropped, kFailed };

constexpr uint32_t SwapChainFrameLatency(uint32_t in_flight, bool wrapped, bool test_latency_one) {
  if (wrapped) {
    return in_flight + 1;
  }
  return test_latency_one ? 1 : in_flight;
}

// Keep the rules test independent of platform headers. The D3D12 backend checks the value.
inline constexpr int32_t kDxgiWasStillDrawing = static_cast<int32_t>(0x887A000Au);

constexpr PresentResult ClassifyDxgiPresentNowait(int32_t result) {
  if (result == kDxgiWasStillDrawing) {
    return PresentResult::kDropped;
  }
  return result >= 0 ? PresentResult::kPresented : PresentResult::kFailed;
}

class PresentDrops {
 public:
  using Clock = std::chrono::steady_clock;

  uint64_t OnPresent(PresentResult result, Clock::time_point now) {
    if (result == PresentResult::kDropped) {
      ++pending_;
      ++total_;
    }
    if (now < next_report_) {
      return 0;
    }
    next_report_ = now + std::chrono::seconds(1);
    return TakePending();
  }

  uint64_t TakePending() { return std::exchange(pending_, 0); }
  uint64_t total() const { return total_; }

 private:
  Clock::time_point next_report_ = Clock::time_point::min();
  uint64_t pending_ = 0;
  uint64_t total_ = 0;
};

}  // namespace mcla::gpu

#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

namespace mcla::gpu {

enum class PresentResult : uint8_t { kPresented, kDropped, kFailed };
enum class AcquireResult : uint8_t { kAcquired, kUnavailable, kFailed };
enum class PresentTopology : uint8_t { kUnknown, kSameAdapter, kCrossAdapter };
enum class PresentPath : uint8_t { kDefault, kDxgiNowait, kVulkanTryAcquire };

constexpr PresentTopology ClassifyPresentTopology(uint64_t render_luid, uint64_t output_luid) {
  if (!render_luid || !output_luid) {
    return PresentTopology::kUnknown;
  }
  return render_luid == output_luid ? PresentTopology::kSameAdapter : PresentTopology::kCrossAdapter;
}

struct PresentPolicy {
  bool d3d12;
  bool enabled;
  bool vsync;
  bool present_wait;
  bool frame_generation;
  bool capture_frame;
  bool dxgi_nowait_test;
};

constexpr PresentPath ChoosePresentPath(PresentTopology topology, const PresentPolicy& policy) {
  if (policy.frame_generation) {
    return PresentPath::kDefault;
  }
  if (policy.d3d12 && policy.dxgi_nowait_test) {
    return PresentPath::kDxgiNowait;
  }
  // Keep the requested pacing when VSync or an explicit present wait paces the game.
  if (!policy.enabled || topology != PresentTopology::kCrossAdapter || policy.vsync ||
      policy.present_wait) {
    return PresentPath::kDefault;
  }
  if (policy.d3d12) {
    return PresentPath::kDxgiNowait;
  }
  // A tagged Vulkan capture needs a back buffer to run the capture pass.
  return policy.capture_frame ? PresentPath::kDefault : PresentPath::kVulkanTryAcquire;
}

// VkResult values are checked against the Vulkan headers in the backend.
inline constexpr int32_t kVkNotReady = 1;
inline constexpr int32_t kVkTimeout = 2;
inline constexpr int32_t kVkSuboptimal = 1000001003;

constexpr AcquireResult ClassifyVulkanAcquire(int32_t result) {
  switch (result) {
    case 0:
    case kVkSuboptimal:
      return AcquireResult::kAcquired;
    case kVkNotReady:
    case kVkTimeout:
      return AcquireResult::kUnavailable;
    default:
      return AcquireResult::kFailed;
  }
}

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

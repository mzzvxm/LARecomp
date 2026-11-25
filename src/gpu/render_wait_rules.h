#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace mcla::gpu::render_wait {

// Auto blocks on every CPU. Only an explicit A/B selection restores polling.
constexpr bool ShouldSpin(std::string_view mode) {
  return mode == "spin";
}

constexpr bool MayUseComposedCopy(bool windows, bool vulkan, uint32_t vendor_id) {
  return windows && vulkan && vendor_id == 0x10DE;
}

// Windowed and borderless NVIDIA Vulkan are assumed to use the DWM copy path.
// A refresh-only ceiling would still allow the observed 200+ presents/s on a 240 Hz display.
constexpr uint32_t ComposedPresentCeiling(bool exclusive, bool enabled, uint32_t refresh_hz) {
  if (exclusive || !enabled) {
    return 0;
  }
  return refresh_hz > 1 ? std::min(refresh_hz, 120u) : 60u;
}

constexpr int32_t LimitRenderRateForPresents(int32_t limit, uint32_t present_ceiling,
                                             uint32_t presents_per_frame) {
  if (present_ceiling == 0) {
    return limit;
  }
  const auto render_ceiling = static_cast<int32_t>(
      std::max(1u, present_ceiling / std::max(1u, presents_per_frame)));
  return limit > 0 ? std::min(limit, render_ceiling) : render_ceiling;
}

constexpr int64_t ComposedPresentDue(int64_t last_present, int64_t frequency, uint32_t ceiling) {
  if (last_present == 0 || ceiling == 0) {
    return 0;
  }
  return last_present + (frequency + ceiling - 1) / ceiling;
}

}  // namespace mcla::gpu::render_wait

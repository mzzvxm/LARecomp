#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mcla::gpu::vram_warning {

enum class Change { kNone, kShow, kClear };

class Warning {
 public:
  static constexpr uint32_t kEnterPercent = 82;
  static constexpr uint32_t kLeavePercent = 68;
  static constexpr uint32_t kRecoverySamples = 8;

  // An unavailable budget cannot clear a warning or trigger one on its own.
  Change Sample(uint64_t used, uint64_t budget, bool allocation_failed) {
    if (allocation_failed || (budget && used >= Percent(budget, kEnterPercent, 99))) {
      recovering_ = 0;
      if (!active_) {
        active_ = true;
        return Change::kShow;
      }
    } else if (active_) {
      if (!budget || used > Percent(budget, kLeavePercent, 0)) {
        recovering_ = 0;
      } else if (++recovering_ == kRecoverySamples) {
        recovering_ = 0;
        active_ = false;
        return Change::kClear;
      }
    }
    return Change::kNone;
  }

 private:
  // Round entry up and recovery down to implement the percentage comparisons
  // exactly, even for budgets that are not multiples of 100 bytes.
  static constexpr uint64_t Percent(uint64_t budget, uint32_t percent, uint32_t round) {
    return (budget / 100) * percent + ((budget % 100) * percent + round) / 100;
  }

  bool active_ = false;
  uint32_t recovering_ = 0;
};

enum class Setting : uint8_t {
  kNone,
  kRenderSize,
  kUpscaling,
  kFrameGeneration,
  kRtCity,
  kRtBounce,
  kLightShadows,
  kCount,
};

struct Cost {
  Setting setting;
  uint64_t bytes;
};

struct Labels {
  std::string_view hint;
  std::string_view full;
};

constexpr size_t Index(Setting setting) { return static_cast<size_t>(setting); }

inline constexpr std::array<Labels, Index(Setting::kCount)> kLabels{{
    {"", ""},
    {"SCL/MSAA", "Render Scale / MSAA"},
    {"UPSCALE", "Upscaling"},
    {"FRAMEGEN", "Frame Generation"},
    {"CITY RT", "RT City Reflections / RT Bounce Light (shared)"},
    {"RT GI", "RT Bounce Light"},
    {"SHADOWS", "Light Shadows"},
}};

using SettingBytes = std::array<uint64_t, Index(Setting::kCount)>;

// Rank only the allocations we can associate with a group of settings.
// Keep ties in table order so identical samples give the same advice.
inline auto Rank(const SettingBytes& bytes) {
  std::array<Cost, Index(Setting::kCount) - 1> costs{};
  for (size_t i = 1; i < bytes.size(); ++i) {
    costs[i - 1] = {static_cast<Setting>(i), bytes[i]};
  }
  std::ranges::sort(costs, [](const Cost& left, const Cost& right) {
    return left.bytes == right.bytes ? left.setting < right.setting : left.bytes > right.bytes;
  });
  return costs;
}

// Read by the guest menu thread and the UI thread's machine.json publisher.
// An empty hint means there is no active notice.
std::string_view CurrentHint();

}  // namespace mcla::gpu::vram_warning

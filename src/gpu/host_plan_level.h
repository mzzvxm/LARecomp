#pragma once

#include <cstdint>
#include <string_view>

namespace mcla::gpu::host_plan {

// Screen-space quality, in the same order as the passes' Mode enums.
enum class Level : uint8_t { kOff, kHigh, kUltra };

constexpr std::string_view LevelName(Level level) {
  switch (level) {
    case Level::kOff: return "off";
    case Level::kHigh: return "high";
    case Level::kUltra: return "ultra";
  }
  return "?";
}

}  // namespace mcla::gpu::host_plan

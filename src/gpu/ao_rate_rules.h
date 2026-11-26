#pragma once

#include <cstdint>

namespace mcla::gpu::ao_rate {
using uint = uint32_t;
#include "hlsl/ao_rate_rules.hlsli"

constexpr uint32_t kFilter = 1u << 23;
constexpr uint32_t kTraceGuide = 1u << 24;
constexpr uint32_t kThinApply = 1u << 31;
constexpr uint32_t kGrid = 1u << 26;
constexpr uint32_t kPhaseShift = 27;
constexpr uint32_t kPreviousPhaseShift = 29;
constexpr uint32_t kMaxAge = kAoAgeRadix - 1;

constexpr uint32_t GridFlags(uint32_t phase) {
  return kGrid | (phase << kPhaseShift);
}
}  // namespace mcla::gpu::ao_rate

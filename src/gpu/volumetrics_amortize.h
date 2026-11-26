#pragma once

#include "volumetrics_rules.h"

namespace mcla::gpu::volumetrics {

inline constexpr uint32_t kLampTileSide = 8;
inline constexpr uint32_t kFusedSlices = 64;
inline constexpr uint32_t kLampTileBytes = 16;
static_assert(kMaxLamps <= 64);
static_assert(GridFor(Level::kUltra).nz <= kFusedSlices);

constexpr uint32_t LampTiles(uint32_t n) { return (n + kLampTileSide - 1) / kLampTileSide; }

constexpr uint32_t CheckerSlice(uint32_t lane, uint32_t x, uint32_t y, uint32_t frame) {
  // Put the fresh slices in the first 32 lanes and the history candidates in
  // the second 32 so a 32-lane wave can skip shading as a whole.
  return 2u * (lane & 31u) + ((lane >> 5u) ^ ((x + y + frame) & 1u));
}

inline float AmortizedJitter(uint32_t frame, uint32_t base, float phase) {
  // Subsampling the original base-2 Halton sequence would pin each parity to
  // one half of its range. Each parity advances its own full sequence.
  const float v = Halton(((frame >> 1u) & 1023u) + 1u, base) + float(frame & 1u) * phase;
  return v - std::floor(v);
}

inline bool SameLampForHistory(const VolLamp& a, const VolLamp& b) {
  if (a.range != b.range || a.cos_outer != b.cos_outer || a.cos_inner != b.cos_inner || a.pad[0] != b.pad[0]) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (a.pos[i] != b.pos[i] || a.dir[i] != b.dir[i]) return false;
    // Keep the temporal filter during the lamp picker's gradual fades.
    // A step over one quarter of the larger value is a lighting change.
    if (std::abs(a.colour[i] - b.colour[i]) > 0.25f * std::max(a.colour[i], b.colour[i])) return false;
  }
  return true;
}

inline void MarkLampChanges(const std::array<VolLamp, kMaxLamps>& previous, uint32_t previous_count, VolGpuConstants& constants) {
  std::array<bool, kMaxLamps> matched{};
  for (uint32_t i = 0; i < constants.counts[0]; ++i) {
    uint32_t j = 0;
    for (; j < previous_count; ++j) {
      if (!matched[j] && SameLampForHistory(constants.lamps[i], previous[j])) {
        matched[j] = true;
        break;
      }
    }
    if (j == previous_count) constants.lamp_changes[i / 32u] |= 1u << (i % 32u);
  }
  for (uint32_t j = 0; j < previous_count; ++j) {
    if (!matched[j]) constants.retired_lamps[constants.lamp_changes[2]++] = previous[j];
  }
}

}  // namespace mcla::gpu::volumetrics

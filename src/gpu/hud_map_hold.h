#pragma once

#include "records.h"
#include "view_hold_rules.h"

namespace mcla::gpu::hud_map_hold {

inline constexpr uint32_t kBegin = 0x7F000017u;
inline constexpr uint32_t kSample = 0x7F000018u;
inline constexpr uint32_t kEnd = 0x7F000019u;
inline constexpr uint32_t kPage = 0x06ACDu;

// A composite side-channel message in stream order with the producer's draws.
struct SampleRecord {
  CompositeRecord marker{kSample};
  view_hold::MapSample sample;
};
static_assert(std::is_trivially_copyable_v<SampleRecord>);

void SubmitSample(const view_hold::MapSample& sample);

}  // namespace mcla::gpu::hud_map_hold

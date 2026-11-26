#pragma once

#include <atomic>
#include <chrono>

#include "cascade_fit_rules.h"

namespace mcla::gpu::cascade_schedule {
struct View;
struct TileAddresses;
}

namespace mcla::gpu::cascade_fit {

using Clock = std::chrono::steady_clock;
struct DepthSample {
  Pose pose{};
  Clock::time_point recorded{};
  double max_depth = 0.0;
  uint32_t epoch = 0;
  uint32_t cut = 0;
};

// The guest selects daylight frames. The host sends back the newest completed
// sample and its anchor camera. A change of guest context invalidates old work.
inline std::atomic<bool> depth_wanted{false};
inline std::atomic<uint32_t> depth_epoch{0};
DepthSample LatestDepth();

bool BeginFrame(uint32_t manager, uint32_t renderer, uint32_t texels, bool census);
void PlanCascade(uint8_t* base, uint32_t cascade, uint32_t camera, uint32_t basis,
                 const cascade_schedule::TileAddresses& addresses, cascade_schedule::View& view);

}  // namespace mcla::gpu::cascade_fit

namespace mcla::gpu {
void BeginCascadeDensity(bool census, uint32_t texels);
void NoteCascadeDensity(unsigned index, float fitted_far, double half_width, double half_height);
}

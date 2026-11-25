#pragma once

#include <cstdint>

namespace mcla::gpu {

// Values are in the active backend's representation. Both states describe the same surface.
struct SwapChainState {
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t color_space;
  uint32_t buffer_count;
  uint32_t present_mode;
  uint32_t flags;

  constexpr bool operator==(const SwapChainState&) const = default;
};

constexpr bool ShouldRebuildSwapChain(const SwapChainState& current, const SwapChainState& requested,
                                      bool needs_recovery) {
  return needs_recovery || current != requested;
}

}  // namespace mcla::gpu

#pragma once

#include <string_view>

namespace mcla::gpu::render_wait {

// Auto blocks on every CPU. Only an explicit A/B selection restores polling.
constexpr bool ShouldSpin(std::string_view mode) {
  return mode == "spin";
}

}  // namespace mcla::gpu::render_wait

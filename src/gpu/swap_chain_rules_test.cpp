#include "swap_chain_rules.h"

#include <cstdio>

int main() {
  using mcla::gpu::ShouldRebuildSwapChain;
  using mcla::gpu::SwapChainState;

  int checks = 0;
  int failed = 0;
  const auto check = [&](bool ok, const char* what) {
    ++checks;
    if (!ok) {
      ++failed;
      std::printf("FAILED: %s\n", what);
    }
  };

  const SwapChainState current{1280, 720, 1, 1, 3, 1, 1};
  check(!ShouldRebuildSwapChain(current, current, false), "same-size window notification skips");
  const struct {
    uint32_t SwapChainState::*field;
    const char* name;
  } changes[] = {
      {&SwapChainState::width, "width"},
      {&SwapChainState::height, "height"},
      {&SwapChainState::format, "format"},
      {&SwapChainState::color_space, "color space"},
      {&SwapChainState::buffer_count, "buffer count"},
      {&SwapChainState::present_mode, "present mode"},
      {&SwapChainState::flags, "creation flags"},
  };
  for (const auto& change : changes) {
    SwapChainState requested = current;
    ++(requested.*change.field);
    check(ShouldRebuildSwapChain(current, requested, false), change.name);
  }

  SwapChainState resized = current;
  resized.width = 1920;
  resized.height = 1080;
  check(ShouldRebuildSwapChain(current, resized, false), "resize changes both dimensions");
  check(!ShouldRebuildSwapChain(resized, resized, false), "duplicate request after resize skips");
  check(ShouldRebuildSwapChain(resized, resized, true), "backend failure forces recovery at the same size");
  check(ShouldRebuildSwapChain(current, resized, true), "backend failure and resize still rebuild");

  std::printf("swap chain rules: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}

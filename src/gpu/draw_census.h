#pragma once
// ===========================================================================
// MCLA GPU renderer -- per-frame draw census by pixel shader (diagnostic)
// ===========================================================================
// mcla_gpu_census_ps names up to
// kMaxShaders pixel shader identities (hex, comma separated). Every guest
// frame, the draws and the vertices (indices, for an indexed draw) recorded
// with each; every report window, the distribution of the per-frame vertices
// and how many frames differ from the one before. Built for MCLA's glow seed
// pass (xstar_glow PSGlowSeedRT draws a quad per glow, 96 a frame at most):
// a census pinned at the cap means the game is dropping glows.
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mcla::gpu {

class DrawCensus {
 public:
  static constexpr uint32_t kMaxShaders = 4;

  // Reads mcla_gpu_census_ps (bad entries are logged and left out).
  void Configure();
  bool active() const { return shader_count_ != 0; }

  void OnDraw(uint64_t ps_identity, uint32_t count) {
    for (uint32_t i = 0; i < shader_count_; ++i) {
      if (ids_[i] == ps_identity) {
        frame_vertices_[i] += count;
        ++frame_draws_[i];
      }
    }
  }
  // A guest frame ended.
  void OnPresent();
  // One line per shader for the window since the last call; empty when off.
  std::string TakeReport();

 private:
  uint32_t shader_count_ = 0;
  std::array<uint64_t, kMaxShaders> ids_{};
  std::array<uint32_t, kMaxShaders> frame_vertices_{};
  std::array<uint32_t, kMaxShaders> frame_draws_{};
  std::array<std::vector<uint32_t>, kMaxShaders> window_vertices_;
  std::array<uint64_t, kMaxShaders> window_draws_{};
};

}  // namespace mcla::gpu

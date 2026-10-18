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
#include <unordered_map>
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

// mcla_gpu_census_targets (for the M6b.2b classes): every report
// window, each pass the frames drew into -- its EDRAM targets, labelled as
// PassTiming labels them -- with its draws per frame and the pixel shaders
// that drew there (identity and draws), to tell the shadow, the reflection,
// the post-processing chain and the rest apart by what draws into them.
class TargetCensus {
 public:
  static constexpr size_t kShadersListed = 8;

  // Reads mcla_gpu_census_targets.
  void Configure();
  bool active() const { return active_; }

  void OnDraw(const std::string& label, uint64_t ps_identity) {
    Pass& p = passes_[label];
    ++p.draws;
    ++p.shaders[ps_identity];
  }
  // A guest frame ended.
  void OnPresent() { ++frames_; }
  // One line per pass, by draws, for the window since the last call; empty
  // when off.
  std::string TakeReport();

 private:
  struct Pass {
    uint64_t draws = 0;
    std::unordered_map<uint64_t, uint64_t> shaders;  // PS identity -> draws
  };
  bool active_ = false;
  uint64_t frames_ = 0;
  std::unordered_map<std::string, Pass> passes_;
};

}  // namespace mcla::gpu

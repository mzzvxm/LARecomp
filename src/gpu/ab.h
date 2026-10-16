#pragma once
// ===========================================================================
// MCLA GPU renderer -- an A/B inside one process for the render thread (M6a)
// ===========================================================================
// Two runs of the bench differ by
// +-0.4 ms/frame (the laptop's clocks drift as it warms up), more than most
// render-thread changes are worth. With mcla_gpu_ab_interleave on, each
// statistics window runs one variant and the next window the other, so the
// scene, the content and the clocks are shared by both; Scene::Report prints
// every window's prepare time under its variant and the means.
//
// Code under test asks B() for the candidate path while its decision is
// pending, and the losing path is deleted once it is made. Off -- the
// default -- B() is always true: the tree runs the candidate.
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu::ab {

namespace detail {
extern bool g_b;
}

// The cvar, read once (Initialize).
void Initialize();
bool Enabled();

// The candidate path runs in this window.
inline bool B() {
  return detail::g_b;
}

// The next window's variant (the other one, when enabled).
void NextWindow();

}  // namespace mcla::gpu::ab

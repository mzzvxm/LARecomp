#pragma once
// ===========================================================================
// MCLA GPU renderer -- how the render thread waits for a guest pipeline a worker is making
// ===========================================================================
// A draw that may not be left out (the rule of Scene::MaySkipForPipeline: it writes the stencil, goes into
// a target that is not drawn every frame, or into a single-sample one) waits for the build a precache
// worker has begun (Pipelines::TakePrecached, TakeRequested). The worker runs below normal priority
// (os/thread_priority.h: the game's threads come first), which is the right level for work nobody waits for
// and the wrong one for the build the render thread now sits on.
//
// VK-FIRST-FRAME (2026-11-22). The Vulkan race smoke of the RT gate (vkrt1-race-vulkan) logged
// "[Watchdog] frame stalled 5s" at its first frames. frame.csv names it: one frame, pipe_wait_n 1,
// pipe_wait_ms 5363.8 of 5452.9 ms: the render thread waited 5.4 s for ONE guest pipeline that a worker had
// begun. Not the renderer's own (host) pipelines: those are all behind Ready() and never block a frame. The
// run was the gate's side-by-side pair of games (a D3D12 and a Vulkan race at once, each with 6 workers and a
// host builder at below normal on a 16 thread CPU, plus both games' own busy threads): a below normal thread
// that the normal ones keep from a core waits for the scheduler's starvation boost, about 4 s, then runs.
// Boot mode (pipelines.h) is the fix for a CPU with fewer than 12 processors; load from anything else on a
// bigger one (a second game, a browser, a capture tool) needs the same answer, so it is made per wait, not
// per CPU:
//   1. HAND OVER the render thread's priority: after kRaiseAfterMs the worker that is making the pipeline
//      runs at normal priority until it has finished that build (it goes back to its own level at its
//      next entry). The host precache does the same for its builder (HostPrecache::Acquire).
//   2. If it is still not done after kMakeHereAfterMs (a driver that is slow on that shader, or a worker
//      that could not be raised: Linux lowers one way), the render thread makes the pipeline itself as
//      well, like one no worker had begun, and the worker's result is dropped. Two compiles of one
//      pipeline for one build that has already taken a quarter of a second.
// The wait never gives up the draw: a draw that may not be left out is still not left out.
//
// This file is the decision, pure, so that a test (pipeline_wait_test.cpp) needs no renderer, no driver and
// no threads: Pipelines::WaitForBuild asks Next at every slice and does what it says.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu::pipeline_wait {

// The wait wakes this often to ask Next (a finished build wakes it sooner).
inline constexpr uint32_t kSliceMs = 10;
// A pipeline under way this long on the render thread's clock: the worker gets normal priority. Short enough
// that an ordinary compile (20 to 150 ms cold) is never raised for no reason, early enough that a starved
// worker loses almost none of the time it is owed.
inline constexpr uint32_t kRaiseAfterMs = 30;
// Still under way this long: the render thread makes it too. Above the longest ordinary compile of the
// 2026-11-22 runs (157 ms on the render thread, 580 ms the longest of a cold night's 254), so a slow build
// that is progressing is not doubled for no reason.
inline constexpr uint32_t kMakeHereAfterMs = 400;

enum class Step : uint8_t {
  kKeepWaiting,  // wait another slice
  kRaise,        // hand the worker normal priority (once), then wait
  kMakeHere,     // stop waiting: the caller makes the pipeline itself
};

struct Facts {
  uint32_t waited_ms = 0;        // since the wait began
  bool raised = false;           // kRaise was already done in this wait
  bool builder_known = false;    // the worker that began the build is known (it says so once it has begun)
  bool may_make_here = true;     // the caller has what it needs to make the pipeline (always, today)
};

constexpr Step Next(const Facts& facts) {
  if (facts.may_make_here && facts.waited_ms >= kMakeHereAfterMs) {
    return Step::kMakeHere;
  }
  if (!facts.raised && facts.builder_known && facts.waited_ms >= kRaiseAfterMs) {
    return Step::kRaise;
  }
  return Step::kKeepWaiting;
}

}  // namespace mcla::gpu::pipeline_wait

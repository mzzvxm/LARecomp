#pragma once
// ===========================================================================
// MCLA GPU renderer -- the pipelines of a feature that is off, made when it is switched on
// ===========================================================================
// The scene's anti-aliasing (post.h) and FSR1 (present.h) are live settings that are off by
// default. Their pipelines used to be made at start whatever the settings were (nine of them: a
// driver compile each, on the lifecycle thread, in front of the first frame). Now an owner makes
// the pipelines at start only when its setting is on then, which keeps the picture of the first
// frame as it was; when the setting is switched on later, the first frame that needs a pipeline
// starts a job here that makes the missing ones on a worker thread, and the frames until it is done
// are drawn as with the setting off (no smoothing, the bilinear present). The player sees the
// effect appear a few frames after switching it on instead of the game stalling on a compile, as
// the guest's own pipelines are made in the background (pipelines.h).
//
// One job at a time per BackgroundBuild. The worker writes only what the owner staged for it and
// the owner reads that after Collect says the job ended (an acquire of the flag the worker released),
// so no lock is needed. The work reads only what the owner made before the job and keeps until
// Join: HostPasses::CreatePipeline and CreateShader are the calls the host precache's builder
// makes from its own thread too (host_precache.h).
//
// Threading: Start, Running, Busy, Collect on the render thread; Join on the lifecycle thread, in the
// owner's Shutdown (the job names the owner's shaders: Join first, before anything is destroyed).
//
// CHECK: written without a build of the game (only the files' syntax and the hand-over rules'
// standalone test, background_build_test.cpp, were run): that the worker's graphics pipeline
// creation runs beside the render thread's own creations (textures, framebuffers) on D3D12 and on
// Vulkan as it does beside the host precache's builder at start; a live HDR/SDR switch while a build
// runs (PresentPass::BuildOutput makes the other set on the render thread; the worker reads only the
// set in use); the Vulkan, Linux and PS5 builds.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace mcla::gpu {

class BackgroundBuild {
 public:
  BackgroundBuild() = default;
  ~BackgroundBuild();
  BackgroundBuild(const BackgroundBuild&) = delete;
  BackgroundBuild& operator=(const BackgroundBuild&) = delete;

  // Runs `work` on a new thread, below normal priority on Windows (the guest runs meanwhile).
  // False, and nothing started, while a job is running or ended and not yet collected.
  bool Start(std::function<void()> work);
  // A job was started and has not ended.
  bool Running() const { return state_.load(std::memory_order_acquire) == kRunning; }
  // A job is running, or ended and not collected yet: Start would refuse. The owner's gate before
  // it stages anything for a new job: a job that ends between Collect and the test is not Running
  // any more but is not collected either, and staging for a new one would overwrite its results.
  bool Busy() const { return state_.load(std::memory_order_acquire) != kIdle; }
  // True once, when a started job has ended: the thread is joined and everything the work wrote is
  // visible. False while it runs, and when none was started.
  bool Collect();
  // Waits for a running job and forgets an ended one (its staged results stay the owner's to drop).
  void Join();

 private:
  enum : uint8_t { kIdle, kRunning, kEnded };
  std::atomic<uint8_t> state_{kIdle};
  std::thread thread_;
};

}  // namespace mcla::gpu

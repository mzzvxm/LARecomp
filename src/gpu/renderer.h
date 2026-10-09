#pragma once
// ===========================================================================
// MCLA GPU renderer -- the renderer: root of ownership and lifecycle
// ===========================================================================
// Owns every GPU object of the process through its components, and fixes the
// order they are created and destroyed in:
//
//   Initialize:  Device -> FrameRing -> BindlessHeap -> UploadRing -> SwapChain
//                -> OverlayRenderer -> GpuTiming -> FrameCapture -> RenderThread
//   Shutdown:    RenderThread stopped (deadline) -> FrameRing::WaitIdle (drains
//                every retirement, running release actions while everything
//                they name is alive) -> FrameCapture -> GpuTiming ->
//                OverlayRenderer -> SwapChain -> UploadRing -> BindlessHeap ->
//                FrameRing -> Device
//
// Members are declared in creation order, so the destructor (which calls
// Shutdown) cannot release a component while one built on it is alive. After
// the drain the GPU is idle, so components destroy their objects directly.
// If the render thread does not stop in time, nothing is destroyed: the thread
// may still be using any of it (the module never deletes the Renderer).
//
// After Initialize, the components belong to the render thread: it is the
// only thread that records, submits, presents or retires. Other threads talk
// to the renderer only through the RenderThread ring and a few atomics
// (resize request, shutdown), so the renderer needs no global lock -- unlike
// reblue's VideoState mutex, which exists because it records on several guest
// threads.
//
// Presents come from two places: the guest's D3DDevice_Swap (a kPresent
// command) and the render thread's idle tick while the guest does not swap
// (OnIdle; 21.2 "Present sem swap do guest"). Both run ExecutePresent.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "capture.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "overlay.h"
#include "render_thread.h"
#include "swap_chain.h"
#include "timing.h"
#include "upload.h"

namespace rex::ui {
class Window;
}

namespace mcla::gpu {

class Renderer final : public RenderThread::Executor {
 public:
  struct Options {
    Device::Options device;
    SwapChain::Options swap_chain;
  };

  // Render thread wakes this often with no command; see OnIdle.
  static constexpr std::chrono::milliseconds kIdlePeriod{16};
  // A guest that swapped this recently drives presentation by itself.
  static constexpr std::chrono::milliseconds kGuestActiveWindow{100};
  static constexpr std::chrono::milliseconds kStopDeadline{2000};

  Renderer() = default;
  ~Renderer() override;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool Initialize(rex::ui::Window* window, const Options& options,
                  std::shared_ptr<OverlayChannel> overlay_channel,
                  std::shared_ptr<OverlayHost> overlay_host);

  // Any thread. After this, SubmitPresent is a no-op.
  void BeginShutdown();
  // Lifecycle thread. Implies BeginShutdown.
  void Shutdown();

  // Guest thread: queue a present and wait until the render thread ran it.
  void SubmitPresent();
  // Any thread.
  void RequestResize() { resize_requested_.store(true, std::memory_order_release); }
  void RequestCapture() { capture_.Request(); }

 private:
  enum class PresentSource : uint8_t { kGuest, kIdle };

  void Execute(RenderCommand& command) override;
  void OnIdle() override;
  void ExecutePresent(PresentSource source);
  bool RebuildSwapChain();
  void RecordPresentPass(plume::RenderCommandList* list, uint32_t slot, uint32_t image);
  void ReportFrameStats();

  Device device_;
  FrameRing frames_;
  BindlessHeap heap_;
  UploadRing upload_;
  SwapChain swap_chain_;
  OverlayRenderer overlay_;
  GpuTiming timing_;
  FrameCapture capture_;
  RenderThread thread_;

  std::shared_ptr<OverlayHost> overlay_host_;
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> resize_requested_{false};
  bool initialized_ = false;

  // Render thread only.
  uint64_t presented_ = 0;
  uint64_t guest_presents_ = 0;
  uint64_t idle_presents_ = 0;
  uint64_t skipped_ = 0;
  uint64_t overlay_draws_ = 0;
  bool guest_has_presented_ = false;
  std::chrono::steady_clock::time_point last_guest_present_{};
  bool reported_no_scene_ = false;
};

}  // namespace mcla::gpu

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the renderer: root of ownership and lifecycle
// ===========================================================================
// Owns every GPU object of the process through its components, and fixes the
// order they are created and destroyed in:
//
//   Initialize:  Device -> FrameRing -> BindlessHeap -> GuestLayout ->
//                ShaderStore -> Samplers -> Pipelines -> UploadRing ->
//                GeometryCache -> EdramTargets -> ResolveCache -> TextureCache
//                -> Scene -> SwapChain ->
//                OverlayRenderer -> GpuTiming -> FrameCapture -> RenderThread
//   Shutdown:    RenderThread stopped (deadline) -> FrameValidator::Finish ->
//                Scene::Finish -> MemoryCensus::Finish -> GuestMemory
//                (callback unregistered) -> FrameRing::WaitIdle (drains every
//                retirement, running release actions while everything they
//                name is alive) -> FrameCapture -> GpuTiming -> OverlayRenderer
//                -> SwapChain -> TextureCache -> ResolveCache -> EdramTargets ->
//                GeometryCache -> UploadRing ->
//                Pipelines -> Samplers -> ShaderStore -> GuestLayout ->
//                BindlessHeap -> FrameRing -> Device
//
// Members are declared in creation order, so the destructor (which calls
// Shutdown) cannot release a component while one built on it is alive. After
// the drain the GPU is idle, so components destroy their objects directly.
// If the render thread does not stop in time, nothing is destroyed: the thread
// may still be using any of it (the module never deletes the Renderer).
//
// After Initialize, the components belong to the render thread: it is the
// only thread that records, submits, presents or retires. Other threads talk
// to the renderer only through the command stream (records written by the
// guest capture) and a few atomics (resize request, shutdown), so the renderer
// needs no global lock -- unlike reblue's VideoState mutex, which exists
// because it records on several guest threads.
//
// A guest frame is recorded into one frame slot from its first scene record
// (draw, clear, resolve, tiling) to its kPresent, whose present pass goes into
// the same list; OpenFrameList opens it. Presents come from two
// places: the guest's D3DDevice_Swap (a kPresent record) and the render
// thread's idle tick while the guest does not swap (OnIdle; 21.2 "Present sem
// swap do guest"), which never presents while a guest frame is open -- the
// overlay then waits for the guest's present. Both run ExecutePresent.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "capture.h"
#include "descriptors.h"
#include "device.h"
#include "edram.h"
#include "frame.h"
#include "frame_trace.h"
#include "geometry.h"
#include "guest_memory.h"
#include "layout.h"
#include "memory_census.h"
#include "overlay.h"
#include "pipelines.h"
#include "render_thread.h"
#include "resolve.h"
#include "samplers.h"
#include "scene.h"
#include "shaders.h"
#include "swap_chain.h"
#include "textures.h"
#include "timing.h"
#include "upload.h"
#include "validation.h"

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

  Renderer() : validator_(shaders_), scene_(shaders_), census_(guest_memory_) {}
  ~Renderer() override;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool Initialize(rex::ui::Window* window, const Options& options,
                  std::shared_ptr<OverlayChannel> overlay_channel,
                  std::shared_ptr<OverlayHost> overlay_host);

  // Any thread.
  void BeginShutdown();
  // Lifecycle thread. Implies BeginShutdown.
  void Shutdown();

  // Where the guest capture writes. Valid after Initialize, for the life of
  // the renderer.
  CommandStream& stream() { return thread_.stream(); }
  // Any thread.
  void RequestResize() { resize_requested_.store(true, std::memory_order_release); }
  void RequestCapture() { capture_.Request(); }
  // Any thread: trace the guest frame after the next present (frame_trace.h).
  void RequestTrace() { trace_.RequestNext(); }

  // Any thread: how far the render thread got, for the "GPU is hung" report
  // (gpu.h OnGuestGpuHangCheck).
  struct FenceProgress {
    uint64_t records_executed = 0;
    uint64_t fences_published = 0;
    uint32_t last_fence = 0;
  };
  FenceProgress fence_progress() const {
    return FenceProgress{records_executed_.load(std::memory_order_relaxed),
                         fences_published_.load(std::memory_order_relaxed),
                         last_fence_.load(std::memory_order_relaxed)};
  }

 private:
  enum class PresentSource : uint8_t { kGuest, kIdle };

  void Execute(const RecordHeader& header, const uint8_t* payload) override;
  void OnIdle() override;
  void ExecuteFence(const FenceRecord& record);
  // The guest frame's open list, opening it (and starting the frame's
  // timestamps and the scene's frame) on the first scene record. Null when the
  // device is lost.
  plume::RenderCommandList* OpenFrameList();
  // With mcla_gpu_batch_draws: submits the frame's list so far, unfenced, and
  // continues in a new one (frame.h).
  void MaybeFlushBatch();
  // The open guest frame goes to the GPU without a present pass (nothing to
  // present into), then the ring moves on.
  void SubmitFrameWithoutPresent();
  void ExecutePresent(PresentSource source);
  bool RebuildSwapChain();
  void RecordPresentPass(plume::RenderCommandList* list, uint32_t slot, uint32_t image);
  void ReportFrameStats();

  GuestMemory guest_memory_;  // attaches on first use; detached after the render thread stops
  Device device_;
  FrameRing frames_;
  BindlessHeap heap_;
  GuestLayout guest_layout_;
  ShaderStore shaders_;
  FrameValidator validator_;  // render thread; reads shaders_; on with mcla_gpu_validate
  Scene scene_;               // render thread; the replay (holds no GPU object of its own)
  FrameTrace trace_;          // render thread; diagnostic dumps of chosen frames
  MemoryCensus census_;       // render thread; diagnostic, reads guest_memory_
  Samplers samplers_;
  Pipelines pipelines_;
  UploadRing upload_;
  GeometryCache geometry_;  // reads guest memory through guest_memory_
  EdramTargets edram_;
  ResolveCache resolve_;    // resolve products (owns their textures and views)
  TextureCache textures_;   // textures decoded from guest memory
  SwapChain swap_chain_;
  OverlayRenderer overlay_;
  GpuTiming timing_;
  FrameCapture capture_;
  RenderThread thread_;

  std::shared_ptr<OverlayHost> overlay_host_;
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> resize_requested_{false};
  bool initialized_ = false;
  bool validate_ = false;       // mcla_gpu_validate, read at Initialize
  uint32_t batch_draws_ = 0;    // mcla_gpu_batch_draws, read at Initialize

  // Render thread only.
  uint64_t presented_ = 0;
  uint64_t guest_presents_ = 0;
  uint64_t guest_frames_ = 0;  // kPresent records executed, presented or skipped
  uint64_t idle_presents_ = 0;
  uint64_t skipped_ = 0;
  uint64_t overlay_draws_ = 0;
  // Records executed since the last stats line, by RecordType.
  uint64_t records_[size_t(RecordType::kCount)] = {};
  // Fence records that arrived while the retired fence was not held (logged
  // once): nothing was published for them.
  uint64_t fences_not_held_ = 0;
  bool guest_has_presented_ = false;
  // Written by the render thread, read by any thread (fence_progress).
  std::atomic<uint64_t> records_executed_{0};
  std::atomic<uint64_t> fences_published_{0};
  std::atomic<uint32_t> last_fence_{0};
  std::chrono::steady_clock::time_point last_guest_present_{};
  bool reported_no_scene_ = false;
};

}  // namespace mcla::gpu

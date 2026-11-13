#pragma once
// ===========================================================================
// MCLA GPU renderer -- the renderer: root of ownership and lifecycle
// ===========================================================================
// Owns every GPU object of the process through its components, and fixes the
// order they are created and destroyed in:
//
//   Initialize:  Device -> FrameRing -> BindlessHeap -> GuestLayout ->
//                ShaderStore -> Samplers -> Pipelines -> UploadRing ->
//                GeometryCache -> EdramTargets -> HostPasses -> ResolveMemory ->
//                ResolveCache ->
//                TextureCache -> VideoMemoryBudget -> Scene -> SwapChain -> PresentPass ->
//                OverlayRenderer -> GpuTiming -> FrameCapture -> RenderThread
//   Shutdown:    RenderThread stopped (deadline) -> FrameValidator::Finish ->
//                Scene::Finish -> MemoryCensus::Finish -> GuestMemory
//                (callback unregistered) -> FrameRing::WaitIdle (drains every
//                retirement, running release actions while everything they
//                name is alive) -> FrameCapture -> GpuTiming -> OverlayRenderer
//                -> PresentPass -> SwapChain -> VideoMemoryBudget -> TextureCache ->
//                ResolveCache ->
//                ResolveMemory -> HostPasses ->
//                EdramTargets -> GeometryCache -> UploadRing ->
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
#include <string>

#include "ao.h"
#include "capture.h"
#include "descriptors.h"
#include "device.h"
#include "dlss_frame_generation.h"
#include "edram.h"
#include "flicker_probe.h"
#include "frame.h"
#include "frame_generation.h"
#include "frame_layers.h"
#include "frame_trace.h"
#include "geometry.h"
#include "guest_memory.h"
#include "hdr_scene.h"
#include "hdr_tonemap.h"
#include "host_passes.h"
#include "layout.h"
#include "memory_census.h"
#include "occlusion.h"
#include "overlay.h"
#include "pass_timing.h"
#include "perf_markers.h"
#include "pipelines.h"
#include "post.h"
#include "present.h"
#include "present_schedule.h"
#include "render_scale.h"
#include "render_thread.h"
#include "resolve.h"
#include "resolve_memory.h"
#include "samplers.h"
#include "scene.h"
#include "shaders.h"
#include "swap_chain.h"
#include "temporal.h"
#include "textures.h"
#include "timing.h"
#include "upload.h"
#include "validation.h"
#include "vram_budget.h"

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
  // The longest a guest present may take after ReleaseGuest for the next one to release early.
  static constexpr std::chrono::microseconds kEarlyReleaseMaxTail{1000};

  Renderer() : validator_(shaders_), scene_(shaders_), census_(guest_memory_) {}
  const Device& device() const { return device_; }
  ~Renderer() override;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool Initialize(rex::ui::Window* window, const Options& options,
                  std::shared_ptr<OverlayChannel> overlay_channel,
                  std::shared_ptr<OverlayHost> overlay_host);

  // Main thread, after Initialize, with the guest not drawing yet (before it is launched): attaches
  // the shader packs in `folder` (the shader_pack_dir cvar replaces it) and starts the pipeline
  // persistence and precache against them (shaders.h: the renderer starts without a pack when none
  // is in place). True when the packs are attached, also when they were already.
  bool AttachShaderPacks(const std::filesystem::path& folder);
  // Any thread.
  bool shader_packs_attached() const { return shaders_.attached(); }

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
  void RequestTextureDump() { textures_.RequestDump(); }
  void RequestTextureDumpAll() { textures_.RequestDumpAll(); }
  void RequestTextureReload() { textures_.RequestReload(); }
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

  // Once the main shader pack is attached, in Initialize or in AttachShaderPacks.
  void OnShaderPackAttached();

  void Execute(const RecordHeader& header, const uint8_t* payload) override;
  void ExecuteRecord(const RecordHeader& header, const uint8_t* payload);
  void OnIdle() override;
  int64_t DeferredDue() const override { return deferred_.pending ? deferred_.due : 0; }
  void RunDeferred() override { FlushDeferredPresent(/*wait=*/false); }
  // The swap chain's present of a submitted frame, and what follows it for a guest frame:
  // the presentation's statistics for the scheduled presents and the present observer.
  // `target` the frame's plan (0 = none), `due` the time it was held to (0 = none).
  // `latency_frame` the frame the reducer's present markers name (latency::DetachRenderFrame).
  void PresentNow(uint32_t image, PresentSource source, int64_t target, int64_t due,
                  int64_t ready, uint64_t guest_present, uint64_t latency_frame);
  // A held present (deferred_) goes out: at once, or with `wait` not before its time.
  void FlushDeferredPresent(bool wait);
  void ExecuteFence(const FenceRecord& record);
  // After a draw ran, while a burst of new textures is on (TextureCache::prefetching): looks at
  // the draw records already published behind it, up to the frame's present and
  // kPrefetchAheadDraws of them, and offers their textures to the decode pool
  // (TextureCache::Prefetch). Each record is looked at once.
  void PrefetchTextures(const RecordHeader& executing);
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
  // HDR10 only, once per present: this frame's levels to the present pass and the overlay, and the
  // display's metadata when the peak moved.
  void UpdateHdr();
  // Before each present: the aspect ratio the picture is shown at. A guest frame that had a 3D scene
  // is shown at the window's (mc_engine/ultrawide.h), the game's own 16:9 stays for the others; an
  // idle present shows the last guest frame, so it keeps that frame's choice.
  void UpdateDisplayAspect();

  // mcla_gpu_frame_generation is live (frame_generation.h). At a frame boundary, with the HDR
  // setting's check, the setting and what the runtime says are compared with whether it runs, and a
  // difference is switched at once: SwitchFrameGeneration makes the swap chain again on the present
  // queue (on) or on the render queue (off) with the pacer paused. True when the swap chain was
  // changed. Render thread.
  bool ApplyFrameGenerationSetting();
  bool SwitchFrameGeneration(FrameGeneration::Change change);
  // DLSS frame generation (dlss_frame_generation.h), the same live setting: switched on, the swap
  // chain is made again as Streamline's proxy chain (the plugin loaded, the DXGI factory's wrapper
  // on D3D12) and the real frame is presented as ever on the render queue, with the extra inputs
  // recorded after the present pass (RecordPresentPass); switched off, it is made again plain after
  // the plugin is unloaded. `resize`: the switch off is for a change of the window's size, which the
  // proxy chain is not resized through. True when the swap chain was changed.
  bool SwitchDlssFrameGeneration(DlssFrameGeneration::Change change, bool resize);
  // The present of a guest or idle frame while frame generation runs: the real frame (and, when the
  // frame can be generated, the generated one) painted into a pair of present images and handed to the
  // pacer, which presents them (ExecutePresent's second half).
  void ExecutePresentGenerated(PresentSource source);
  // True when the pair holds a generated image as well as the real one.
  bool RecordGeneratedPresent(plume::RenderCommandList* list, uint32_t slot,
                              const FrameGeneration::Pair& pair, PresentSource source);
  // The present pass and the overlay into a present image (a colour target that the pacer reads next).
  void DrawPresentInto(plume::RenderCommandList* list, uint32_t slot, HostImage& target,
                       uint32_t width, uint32_t height);

  // mcla_gpu_hdr is live. At a frame boundary (no list open; not inside a present) the output the
  // setting and the display ask for (hdr.h SwitchWanted) is compared with the swap chain's, and a
  // difference is switched at once: SwitchHdr. A swap chain that was lost is made again here too.
  // True when the swap chain was changed. Render thread.
  bool ApplyHdrSetting();
  bool SwitchHdr(bool to_hdr, const std::string& why);
  // After the swap chain was made again (a switch, or recovering a lost chain): the passes use the
  // set for its format, the HDR scene (layers, float scene, tone map) comes up or goes down with it,
  // the sizes are published, the metadata is sent again.
  void AdoptSwapChain();
  // The scene half of the HDR output on the frame layers: makes the HdrScene and the HdrToneMap the
  // first time and wires them in. `layers_ok`: the frame layers run. False (logged) when they
  // cannot, with the layers off: the HDR output then shows the front buffer (stage 1).
  bool SetUpHdrScene(bool layers_ok);
  // Loads the HDR variant pack beside the main pack when it is not loaded yet (a no-op without a
  // main pack or once loaded; logs when there is none).
  void LoadHdrVariants();
  void RecordPresentPass(plume::RenderCommandList* list, uint32_t slot, uint32_t image,
                         PresentSource source);
  void ReportFrameStats();
  // In the present pass, once the last guest memory it reads has been read: lets the
  // guest go on while this thread submits and presents (mcla_gpu_present_release).
  void ReleaseGuest();

  GuestMemory guest_memory_;  // attaches on first use; detached after the render thread stops
  Device device_;
  FrameRing frames_;
  BindlessHeap heap_;
  GuestLayout guest_layout_;
  ShaderStore shaders_;
  FrameValidator validator_;  // render thread; reads shaders_; on with mcla_gpu_validate
  TemporalSystem temporal_;   // render thread; the temporal upscalers' groundwork (temporal.h)
  AmbientOcclusion ao_;       // render thread; screen-space ambient occlusion (ao.h)
  Scene scene_;               // render thread; the replay (holds no GPU object of its own)
  FrameTrace trace_;          // render thread; diagnostic dumps of chosen frames
  MemoryCensus census_;       // render thread; diagnostic, reads guest_memory_
  Samplers samplers_;
  Pipelines pipelines_;
  UploadRing upload_;
  GeometryCache geometry_;  // reads guest memory through guest_memory_
  EdramTargets edram_;
  GuestOcclusion occlusion_;  // render thread; the guest's occlusion queries (occlusion.h)
  HostPasses passes_;       // layout and vertex shader of the full-screen passes
  ResolveMemory resolve_memory_;  // what resolves write, as guest bytes
  ResolveCache resolve_;    // resolve products (owns their textures and views)
  TextureCache textures_;   // textures decoded from guest memory
  VideoMemoryBudget vram_;  // releases what is unused when video memory runs short
  SwapChain swap_chain_;
  PresentPass present_;     // the guest's front buffer on the window
  PostAa post_;             // the scene's anti-aliasing before the UI
  FrameLayers layers_;      // the frame's scene and UI as two layers (HDR output, frame generation)
  HdrScene hdr_scene_;      // HDR10: the two layers composed at the present
  HdrToneMap hdr_tone_;     // HDR10: the tone map drawn again, the scene above paper white
  FrameGeneration fg_;      // FSR 3.1 frame generation and its pacer (frame_generation.h)
  DlssFrameGeneration dlssg_;  // DLSS frame generation through Streamline (dlss_frame_generation.h)
  FlickerProbe flicker_probe_;  // diagnostic, mcla_gpu_flicker_probe
  OverlayRenderer overlay_;
  GpuTiming timing_;
  PassTiming pass_timing_;  // per-pass GPU time, mcla_gpu_pass_timing
  PerfMarkers markers_;     // profiler labels, mcla_gpu_perf_markers
  FrameCapture capture_;
  RenderThread thread_;

  std::shared_ptr<OverlayHost> overlay_host_;
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> resize_requested_{false};
  bool initialized_ = false;
  bool validate_ = false;       // mcla_gpu_validate, read at Initialize
  uint32_t batch_draws_ = 0;    // mcla_gpu_batch_draws, read at Initialize
  bool profile_records_ = false;  // mcla_gpu_scene_profile: CPU per record type (thread_load.h)
  bool release_early_ = false;    // mcla_gpu_present_release, read at Initialize
  // The last HDR10 metadata given to the swap chain (UpdateHdr).
  bool hdr_metadata_sent_ = false;
  plume::RenderHdrMetadata hdr_metadata_;
  // Inside ExecutePresent: OpenFrameList then must not switch the output (the back buffer is
  // acquired).
  bool presenting_ = false;
  // When a lost swap chain was last tried again (ApplyHdrSetting), once a second at most.
  std::chrono::steady_clock::time_point swap_chain_retry_{};
  bool swap_chain_lost_logged_ = false;
  // The internal resolution scale chosen at Initialize from the swap chain's size. It is the start's:
  // the temporal system follows the settings and the window from there (temporal.cpp
  // ReadRenderScale; edram.h SetScales).
  RenderScaleChoice render_scale_;

  // Render thread only.
  // The sequence of the guest's present while it is executed and the guest is still waiting
  // for it; 0 once released, and for an idle present.
  uint64_t release_sequence_ = 0;
  // The previous guest present's work after the release point took no longer than
  // kEarlyReleaseMaxTail (so the guest runs ahead of this thread by that much at most).
  bool tail_short_ = false;
  bool tail_open_ = false;
  std::chrono::steady_clock::time_point tail_start_{};
  // Scheduled presents (gpu.h SetNextPresentTarget): the due time of the guest
  // present being executed (0 = none), and a guest frame that is submitted and
  // waits for its time. The render thread loop presents it then (RunDeferred),
  // executing the next frame's records meanwhile; the next present, a rebuild or
  // the shutdown present it first.
  int64_t present_target_ = 0;
  struct DeferredPresent {
    bool pending = false;
    uint32_t image = 0;
    int64_t target = 0;  // the frame's plan
    int64_t due = 0;     // when it goes out (present_schedule.h Due)
    int64_t ready = 0;
    uint64_t guest_present = 0;
    uint64_t latency_frame = 0;
  };
  DeferredPresent deferred_;
  PresentSchedule schedule_;
  uint64_t early_releases_ = 0;
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
  // PrefetchTextures: the ring position it has looked up to, and how many draws between the
  // executing one and that position it has offered.
  // CHECK: both are guesses (a burst is 66-280 textures over a few hundred draws), and the cost
  // of the scan (a hash and a table probe per fetch of each draw, once per draw) is not measured.
  static constexpr uint32_t kPrefetchAheadDraws = 384;
  static constexpr uint32_t kPrefetchScanRecords = 64;
  uint64_t prefetch_position_ = 0;
  uint32_t prefetch_ahead_ = 0;
  // PrefetchTextures ran for the last draw: when the burst ends the position and the count are
  // dropped, so the next burst starts from the record it is on (a count kept across the gap would
  // claim draws the look never reached).
  bool prefetch_live_ = false;
  bool guest_has_presented_ = false;
  // Written by the render thread, read by any thread (fence_progress).
  std::atomic<uint64_t> records_executed_{0};
  std::atomic<uint64_t> fences_published_{0};
  std::atomic<uint32_t> last_fence_{0};
  std::chrono::steady_clock::time_point last_guest_present_{};
};

}  // namespace mcla::gpu

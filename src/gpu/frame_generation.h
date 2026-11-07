#pragma once
// ===========================================================================
// MCLA GPU renderer -- FSR 3.1 frame generation (F4-DESIGN.md sections 8.1, 8.2 and 14)
// ===========================================================================
// An interpolated frame between every two the game renders, shown half a frame before the real one,
// made by AMD's FSR 3.1 frame generation (frame_generation_ffx.h) in its manual mode: no swap chain
// wrapper of AMD's, the renderer paints the images and presents them itself. On any vendor's GPU,
// with or without an upscaler, on D3D12 and on Vulkan.
//
// The setting. mcla_gpu_frame_generation is off or fsr31; it is live, taken at a frame boundary
// (Renderer::ApplyFrameGenerationSetting). It is offered (the pause menu row, gpu.h
// GetFrameGenerationAvailability) only where the runtime says it can run here.
//
// The frame, on the render thread (Renderer::ExecutePresentGenerated), for a guest frame:
//   1. the real frame is painted as it always is (the front buffer through the display table, FSR1,
//      the overlay) but into a "present image" of the swap chain's format and size, not into a back
//      buffer;
//   2. when the frame can be generated (below) Generate() makes the generated one: the scene of the
//      frame (the 8-bit hudless colour of FrameLayers, or HDR stage 3's float scene) is scaled to the
//      generation size into an RGBA16F image, FFX interpolates from the previous such image to this
//      one with this frame's depth, motion vectors, jitter and camera (all the temporal system keeps,
//      temporal.h), and the UI layer of FrameLayers is composed over the result in the game's code
//      values, as the real frame composes it; that image is painted through the same present path
//      (display table, FSR1, overlay) into the pair's second present image;
//   3. the frame's list is submitted (on the render queue), then an empty list with a fence of the
//      slot's own (the fence signals once everything submitted before it has completed), and the pair
//      is handed to the pacer.
//
// Frames that are NOT generated (the real frame alone is shown, and the next generation starts afresh):
// no 3D scene or no anchor this frame (menus, the pause menu and the map, loading, movies), the
// frame layers not valid or not the front buffer's picture, HDR debug views and the HDR test
// pattern, the depth or vectors of the frame missing, a history that starts again (a camera cut, a
// size change, a gap; the first frame after the setting went on or the context was made), an idle
// present (the guest did not swap), a context that is not ready yet.
//
// The pacer thread (one, started when the setting goes on): for a pair it waits (on the CPU) for the
// frame's fence, then for each image it acquires a swap chain image, copies the present image into it
// with a pass of its own on the present queue, waits until the time the image is due, and presents. Pacing is AMD's: the presents are half the
// rendered interval apart, less a tenth of the mean deviation of that interval and 0.1 ms (the
// formula of AMD's FidelityFX frame interpolation swap chain, MIT), by a high-resolution waitable
// timer and a short spin; the generated image goes out first and not before the last present plus
// that, the real one after it. Presents are FIFO (no tearing; with variable refresh the display
// follows); mcla_gpu_frame_generation_tearing asks for immediate presents instead. Never MAILBOX:
// it could replace a generated image with the real one before it is shown.
//
// Queues and threads (F4-DESIGN.md P4). The swap chain is made on a second DIRECT queue of its own
// (the present queue) while frame generation runs, and on the render queue otherwise: a present on
// the render queue would sit behind the next frame's work. Plume's Vulkan queues are virtual ones
// that share a locked VkQueue only when the family has none to spare (they do on NVIDIA; CHECK on
// AMD and Intel); its D3D12 queues are separate ID3D12CommandQueues. The render thread never touches
// the swap chain while the pacer runs, except for the HDR metadata (a lock in SwapChain) and the
// calls that recreate or resize it, which pause the pacer first (PausePacer) and wait the present
// queue idle. The hand-over of a present image between the queues is by the CPU, not by a
// semaphore: Plume waits a semaphore at the colour attachment stage only, which would not guard a
// shader's read of the image, so the pacer waits for the frame's fence and only then submits the copy
// (its own acquire semaphore is the only one it waits on, and the copy writes the back buffer as a
// colour attachment, which that wait does guard). The pair's slot comes back to the render thread
// after the pacer has seen the copy's fence.
//
// Back-pressure: two slots. ExecutePresentGenerated takes a free slot before it records the present
// images and waits for one up to a second; after that (the pacer is stalled) it does not wait again
// until the pacer has taken a pair, and the frames that found none are dropped (counted, logged).
//
// The frame cap counts rendered frames: mcla_gpu_frame_generation_fps_limit (-1 HALF DISPLAY, the
// default: the floor of 97% of half the display's refresh rate, only where the display is 100 Hz or
// more, else the player's fps_limit applies; 0 uncapped; or N rendered frames, 2N shown) replaces
// fps_limit in the frame limiter while frame generation runs (gpu.h FrameGenerationFpsLimit).
//
// Memory: the present images (4 at the window's size, 4 bytes a pixel), the scene image, the generated
// image and the composed image (RGBA16F at the generation size) and FFX's own context (about 450 MB at
// 4K). After kReleaseAfterFrames guest frames in a row with nothing generated the context and the
// images are released until there is something to generate again.
//
// The vendor's latency reducer is forced on while frame generation runs (latency.cpp). The presents'
// reducer markers are the pacer's (latency.h).
//
// Threading: everything on the render thread except the pacer's own functions; Initialize and
// Shutdown on the thread that owns the renderer's lifecycle (the latter with the GPU idle, after the
// render thread stopped); StopPacer from that thread too, before the swap chain goes.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <plume_render_interface.h>

#include "frame_generation_ffx.h"
#include "host_passes.h"
#include "textures.h"

namespace mcla::gpu {

class Device;
class FrameLayers;
class FrameRing;
class PassTiming;
class SwapChain;
class TemporalSystem;

class FrameGeneration {
 public:
  // Pairs of present images in flight: one being shown by the pacer, one being painted.
  static constexpr uint32_t kSlots = 2;
  // Guest frames in a row with nothing generated before the context and the images are released
  // (20 s at 60 fps, as FrameLayers).
  static constexpr uint32_t kReleaseAfterFrames = 1200;
  // How long the render thread waits for a free slot before it calls the pacer stalled.
  static constexpr std::chrono::milliseconds kStallTimeout{1000};

  // A pair of present images, a slot's.
  struct Pair {
    uint32_t index = 0;
    HostImage* generated = nullptr;
    HostImage* real = nullptr;
  };

  // What Generate needs of the present about to be recorded.
  struct GenerateArgs {
    bool guest_frame = false;          // a guest present (not an idle one)
    TextureKey front;                  // the front buffer's key (PresentPass::front_key)
    uint32_t front_width = 0;          // ... and the size the present reads of it, host pixels
    uint32_t front_height = 0;
    uint32_t window_width = 0;         // the swap chain's size
    uint32_t window_height = 0;
  };
  // What a generated frame is, for the present pass: the composed image (RGBA16F, the game's code
  // values, UI included) and whether the scene in it was HDR stage 3's float scene.
  struct Generated {
    const HostImage* image = nullptr;
    bool float_scene = false;
  };

  FrameGeneration();
  ~FrameGeneration();
  FrameGeneration(const FrameGeneration&) = delete;
  FrameGeneration& operator=(const FrameGeneration&) = delete;

  // After the swap chain, the passes and the temporal system exist (the lifecycle thread, before the
  // render thread starts): makes the backend and probes it on a worker thread. Never fails the
  // renderer; the setting simply is not offered when nothing can run.
  void Initialize(Device& device, FrameRing& frames, HostPasses& passes, PassTiming& pass_timing,
                  FrameLayers& layers, TemporalSystem& temporal, SwapChain& swap_chain,
                  std::atomic<bool>* resize_flag);
  // GPU idle, after the pacer was stopped.
  void Shutdown();
  // After the render thread stopped, before anything waits the GPU idle or goes: the pacer drains
  // what it holds and its thread ends.
  void StopPacer();

  // ---- the setting, at a frame boundary (no list open) --------------------------------------
  enum class Change : uint8_t { kNone, kOn, kOff };
  // Reads the setting and what the runtime says, publishes both (the menu reads them), asks the
  // temporal system for the vectors and the frame layers for their layers while the setting is on,
  // and says which way the renderer should switch the swap chain.
  Change Poll();
  // kOn, before the swap chain is made again: the present queue, its lists and fences, the copy
  // pipeline for `format`. False (logged, frame generation off for the session) when one cannot be
  // made.
  bool PrepareSwitchOn(plume::RenderFormat format);
  // The queue the swap chain is made on: the present queue while frame generation runs (after
  // PrepareSwitchOn), else null (the render queue).
  plume::RenderCommandQueue* present_queue() const { return active_ || switching_on_ ? present_queue_.get() : nullptr; }
  // The present queue goes (frame generation is off and the swap chain is not on it any more, or the
  // swap chain is gone): after SwapChain::Shutdown at the renderer's shutdown, and after a switch off.
  void ReleaseQueue();
  // After the swap chain was made again, rebuilt or resized, with the pacer paused: the present
  // images and the copy pass follow its format and size (the images are made again at the next slot).
  void OnSwapChainChanged();
  // Whether the swap chain asks for vsync: frame generation presents FIFO (or immediate with
  // mcla_gpu_frame_generation_tearing).
  bool WantsVsync() const;
  // The pacer waits until its present queue is idle (every present image copied and presented to the
  // driver) and stops presenting until ResumePacer; the render thread then may recreate or resize
  // the swap chain. No-ops while it does not run. Pause is also the wait that makes Recreate safe.
  void PausePacer();
  void ResumePacer();
  // The pacer is paused for the life of the scope.
  class PauseScope {
   public:
    explicit PauseScope(FrameGeneration& fg) : fg_(fg) { fg_.PausePacer(); }
    ~PauseScope() { fg_.ResumePacer(); }
    PauseScope(const PauseScope&) = delete;
    PauseScope& operator=(const PauseScope&) = delete;

   private:
    FrameGeneration& fg_;
  };
  // After the swap chain was made again (`ok`: it exists on the queue asked for): kOn starts the
  // pacer, kOff stops it and releases what frame generation held; a failure leaves it off for the
  // session. The pacer is paused (or not running) when this is called.
  void CompleteSwitch(Change change, bool ok);

  // True while frame generation runs: the swap chain is on the present queue and the pacer takes
  // every present. The frame's presents go through ExecutePresentGenerated.
  bool active() const { return active_; }

  // ---- per frame, on the render thread -----------------------------------------------------
  // The pair a present is painted into, after the swap chain has width x height of `format`. False
  // when no slot is free (the pacer is stalled, the frame is dropped) or the images could not be
  // made.
  bool AcquireSlot(uint32_t width, uint32_t height, plume::RenderFormat format, Pair* out);
  // The slot goes back unused (the frame was dropped before its submission).
  void ReturnSlot(const Pair& pair);
  // Outside a render pass, after the real frame's present pass was recorded and before the
  // generated one's: when this frame can be generated, makes its generated image (the scene scaled,
  // the interpolation, the UI composed over it) and returns it. The `image` is null when it cannot or
  // is not to be shown (then the real frame alone goes out).
  Generated Generate(plume::RenderCommandList* list, const GenerateArgs& args);
  // After the frame's list was submitted on the render queue: the slot's fence is signalled behind it
  // and the pair goes to the pacer. `latency_frame` is the frame's id for the latency reducer's
  // present markers (0: none). Returns false (the slot is free again, nothing will be shown) when the
  // submission could not be made.
  bool Submit(const Pair& pair, bool has_generated, uint64_t latency_frame, bool guest_frame);

  // Video memory frame generation holds now (images, the context as the runtime reports it).
  uint64_t HeldBytes() const;
  std::string TakeSummary();

 private:
  struct Slot {
    HostImage generated;
    HostImage real;
    // An empty list the render thread executes on the render queue behind the frame, and the fence it
    // signals: complete when the frame's work (the present images included) is.
    std::unique_ptr<plume::RenderCommandList> marker;
    std::unique_ptr<plume::RenderCommandFence> frame_done;
  };
  struct Item {
    uint32_t slot = 0;
    bool has_generated = false;
    uint64_t latency_frame = 0;
    bool guest_frame = false;
    int64_t handed_us = 0;
  };
  // One acquire semaphore, copy list and fence for each present in flight on the pacer's side.
  struct Lane {
    std::unique_ptr<plume::RenderCommandSemaphore> acquire;
    std::unique_ptr<plume::RenderCommandList> list;
    std::unique_ptr<plume::RenderCommandFence> fence;
  };

  // ---- setting and state
  bool WantedBySetting() const;
  void UpdateDisplayRefresh();
  void Publish(bool wanted);
  void ReleaseResources(const char* why);
  // Nothing was generated for this guest frame; after kReleaseAfterFrames in a row the context and
  // the generation images go.
  void NoteNotGenerated();
  bool MakeGenerationPasses();
  bool EnsureImages(uint32_t generation_w, uint32_t generation_h);

  // ---- the pacer
  bool MakeQueue(plume::RenderFormat format);
  void PacerMain();
  void Process(const Item& item);
  // One present image: acquire, copy, wait for the copy, wait for the time it is due, present.
  // `due`: the earliest time (steady microseconds) it may be presented at; returns when it was.
  int64_t PresentImage(HostImage& image, uint32_t lane, int64_t due_us, uint64_t latency_frame,
                       bool* ok);
  void DrainPresentQueue();
  void WaitUntil(int64_t target_us);
  static int64_t NowUs();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  FrameLayers* layers_ = nullptr;
  TemporalSystem* temporal_ = nullptr;
  SwapChain* swap_chain_ = nullptr;
  std::atomic<bool>* resize_flag_ = nullptr;
  std::unique_ptr<FrameGenerationBackend> backend_;

  // The setting's state (render thread).
  bool active_ = false;
  bool failed_ = false;        // switching on could not be done: off for the session
  bool wanted_ = false;        // last read of the setting with the runtime able
  bool logged_unavailable_ = false;
  uint32_t refresh_hz_ = 0;
  std::chrono::steady_clock::time_point refresh_read_{};
  bool initialized_ = false;
  bool switching_on_ = false;  // PrepareSwitchOn done, the swap chain not yet on the present queue

  // The passes (made on the first switch on).
  std::unique_ptr<plume::RenderShader> prepare_shader_;
  std::unique_ptr<plume::RenderShader> compose_shader_;
  std::unique_ptr<plume::RenderShader> copy_shader_;
  std::unique_ptr<plume::RenderPipeline> prepare_pipeline_;
  std::unique_ptr<plume::RenderPipeline> compose_pipeline_;
  std::unique_ptr<plume::RenderPipeline> copy_pipeline_;  // for the swap chain's format
  plume::RenderFormat copy_format_ = plume::RenderFormat::UNKNOWN;

  // The images of the generation (render thread): the scene scaled to the generation size, FFX's
  // output, and the generated frame with the UI over it.
  HostImage color_;
  HostImage generated_;
  HostImage composed_;
  uint64_t frame_id_ = 0;        // FFX's frame counter: one more for each frame it was given
  uint64_t last_generated_frame_ = ~uint64_t(0);  // the guest frame of the last generation
  uint32_t idle_guest_frames_ = 0;
  bool released_ = false;

  // The slots and the hand-over.
  Slot slots_[kSlots];
  bool slot_free_[kSlots] = {true, true};
  bool slot_images_ready_[kSlots] = {false, false};
  uint32_t slot_cursor_ = 0;
  bool stalled_ = false;         // a wait for a slot ran out: no more waiting until the pacer takes one

  // The pacer.
  std::unique_ptr<plume::RenderCommandQueue> present_queue_;
  Lane lanes_[2];
  std::unique_ptr<plume::RenderCommandList> idle_list_;
  std::unique_ptr<plume::RenderCommandFence> idle_fence_;
  std::thread thread_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  bool stop_ = false;
  bool pause_ = false;
  bool idle_ = false;           // the pacer is parked for a pause
  bool busy_ = false;           // the pacer is processing an item
  bool running_ = false;        // the thread exists
  // The pacer's own state (its thread only).
  int64_t last_present_us_ = 0;
  int64_t last_handed_us_ = 0;
  static constexpr uint32_t kWindow = 10;
  int64_t intervals_[kWindow] = {};
  uint32_t interval_count_ = 0;
  uint32_t interval_next_ = 0;
  void* timer_ = nullptr;        // the high-resolution waitable timer (HANDLE)

  struct Stats {
    uint64_t guest_frames = 0;
    uint64_t generated = 0;           // generated frames handed over
    uint64_t not_generated_context = 0;   // the context was not ready (or being made)
    uint64_t not_generated_frame = 0;     // the frame had nothing to generate from
    uint64_t not_generated_reset = 0;     // the history starts again
    uint64_t dropped = 0;             // frames dropped: no slot in time
    uint64_t slot_waits = 0;
    double slot_wait_ms = 0.0;
  };
  Stats stats_;
  // The pacer's counters (its thread writes, the summary reads under mu_).
  struct PacerStats {
    uint64_t presents = 0;
    uint64_t generated_presents = 0;
    uint64_t late = 0;                // presents more than 1 ms after their time
    uint64_t failed = 0;
    double gap_sum_ms = 0.0;          // time between consecutive presents
    double gap_max_ms = 0.0;
    uint64_t gaps = 0;
  };
  PacerStats pacer_stats_;
};

}  // namespace mcla::gpu

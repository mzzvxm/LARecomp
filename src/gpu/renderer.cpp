// MCLA GPU renderer -- the renderer and the module entry points. See
// renderer.h and gpu.h.

#include "renderer.h"

#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>

#include "gpu.h"
#include "guest_capture.h"
#include "native_gfx/guest/guest_fence.h"
#include "native_gfx/guest/occlusion.h"
#include "native_gfx/guest/vblank_probe.h"
#include "native_gfx/nocp/guest_gpu.h"
#include "native_window.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu, false, "MCLA/GPU",
    "Render with the Plume renderer (src/gpu) instead of the legacy D3D12 runtime. Needs "
    "mcla_native_gfx_nocp = true and mcla_native_gfx = false: it owns the window's swap chain, so "
    "neither the SDK presenter nor the legacy runtime can run beside it. Milestone M4 in "
    "progress: every guest draw is prepared on the render thread (state, constants, pipeline, "
    "samplers) but not drawn yet, so each frame is still cleared to black under the SDK "
    "overlays.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_gpu_vsync, false, "MCLA/GPU",
                    "Present on vertical blank. Read every frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_present_wait, false, "MCLA/GPU",
    "Pace presents with the swap chain's latency wait (DXGI waitable object / "
    "VK_KHR_present_wait) when the device has it. On D3D12 Plume then presents with sync "
    "interval 0 and the wait alone paces the frame, so measure before turning it on.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(mcla_gpu_adapter, "", "MCLA/GPU",
                      "Adapter name to create the device on. Empty lets Plume pick.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_verify_constants, false, "MCLA/GPU",
    "Compare every 64-byte block of both constant banks on every draw, not only the blocks the "
    "guest's dirty masks name, and send (and count) any that changed without its dirty bit. "
    "Costs guest CPU per draw. Off by default since M3 measured no block outside the masks; the "
    "guest's XXH3 of both banks every 32 draws still checks the rebuilt banks "
    ".")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_validate, false, "MCLA/GPU",
    "Run the milestone M3 validator on every record besides the replay: per-record counters and "
    "the mcla_gpu_validation_*.csv files next to the executable. Decodes every draw a second "
    "time on the render thread (3.5-7 ms/frame measured in M3).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_gpu_batch_draws, 0, "MCLA/GPU",
    "Submit the guest frame's command list to the GPU every this many prepared draws, unfenced, "
    "and continue in a new list (0: one submission per guest frame, the default). For measuring "
    "submission batching.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_gpu_debug_layer, false, "MCLA/GPU",
                    "Enable the D3D12 debug layer before the device exists and log its warnings "
                    "and errors. Needs D3D12SDKLayers.dll from the same Agility SDK release as "
                    "D3D12\\D3D12Core.dll, placed next to it (the system's copy belongs to the OS "
                    "runtime). Vulkan validation is a property of a Debug build of Plume and "
                    "cannot be switched on here.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DECLARE(bool, mcla_native_gfx);
REXCVAR_DECLARE(bool, mcla_native_gfx_nocp);
REXCVAR_DECLARE(bool, mcla_gpu_memory_census);

namespace {

// D3DDevice_BlockOnFence calls that found their fence pending, and the time
// they waited (gpu.h NoteGuestFenceWait); taken by the stats line.
std::atomic<uint64_t> g_fence_waits{0};
std::atomic<uint64_t> g_fence_wait_ns{0};

}  // namespace

namespace mcla::gpu {

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

Renderer::~Renderer() {
  Shutdown();
}

bool Renderer::Initialize(rex::ui::Window* window, const Options& options,
                          std::shared_ptr<OverlayChannel> overlay_channel,
                          std::shared_ptr<OverlayHost> overlay_host) {
  if (initialized_) {
    return true;
  }
  if (!overlay_channel || !overlay_host) {
    REXLOG_ERROR("[gpu] renderer: no overlay channel/host; the SDK overlays could not be drawn");
    return false;
  }
  plume::RenderWindow render_window{};
  if (!GetRenderWindow(window, render_window)) {
    return false;
  }
  overlay_host_ = std::move(overlay_host);
  // Each step needs the ones before it; on a failure everything already made
  // is torn down in reverse (each Shutdown is a no-op on a component that was
  // never initialized).
  bool ok = device_.Initialize(options.device) && frames_.Initialize(device_) &&
            heap_.Initialize(device_, frames_) && guest_layout_.Initialize(device_, heap_) &&
            shaders_.Initialize(device_);
  if (ok) {
    samplers_.Initialize(device_, heap_);
    pipelines_.Initialize(device_, guest_layout_, shaders_);
    ok = upload_.Initialize(device_) &&
         geometry_.Initialize(device_, frames_, upload_, guest_memory_);
  }
  if (ok) {
    edram_.Initialize(device_, frames_, heap_);
    ok = passes_.Initialize(device_, heap_) &&
         resolve_.Initialize(device_, frames_, heap_, guest_memory_, textures_, edram_, passes_);
  }
  if (ok) {
    textures_.Initialize(device_, frames_, heap_, upload_, guest_memory_, resolve_);
    scene_.Initialize(upload_, pipelines_, samplers_, geometry_, edram_, textures_, resolve_,
                      guest_layout_, heap_);
    ok = swap_chain_.Initialize(device_, render_window, options.swap_chain) &&
         overlay_.Initialize(device_, frames_, heap_, upload_, std::move(overlay_channel),
                             swap_chain_.format());
  }
  if (ok) {
    timing_.Initialize(device_);
    capture_.Initialize();
    census_.SetEnabled(REXCVAR_GET(mcla_gpu_memory_census));
    validate_ = REXCVAR_GET(mcla_gpu_validate);
    batch_draws_ = REXCVAR_GET(mcla_gpu_batch_draws);
    ok = thread_.Start(*this, kIdlePeriod);
  }
  if (!ok) {
    capture_.Shutdown();
    timing_.Shutdown();
    overlay_.Shutdown();
    swap_chain_.Shutdown();
    textures_.Shutdown();
    resolve_.Shutdown();
    passes_.Shutdown();
    edram_.Shutdown();
    geometry_.Shutdown();
    upload_.Shutdown();
    pipelines_.Shutdown();
    samplers_.Shutdown();
    shaders_.Shutdown();
    guest_layout_.Shutdown();
    heap_.Shutdown();
    frames_.Shutdown();
    device_.Shutdown();
    overlay_host_.reset();
    return false;
  }
  initialized_ = true;
  REXLOG_INFO("[gpu] renderer up: {} frames in flight, render thread running (idle tick {} ms), "
              "validator {}, {}",
              kFramesInFlight, kIdlePeriod.count(), validate_ ? "on" : "off",
              batch_draws_ ? "a batch every " + std::to_string(batch_draws_) + " draws"
                           : std::string("one submission per guest frame"));
  return true;
}

void Renderer::BeginShutdown() {
  shutting_down_.store(true, std::memory_order_release);
}

void Renderer::Shutdown() {
  BeginShutdown();
  if (!initialized_) {
    return;
  }
  initialized_ = false;
  // 1. No more recording: the render thread finishes the command it is on and
  //    exits; producers and waiters are released. A thread that does not exit
  //    in time may still be inside any component, so nothing is destroyed.
  if (!thread_.Stop(kStopDeadline)) {
    REXLOG_ERROR("[gpu] renderer left alive after {} presents: its render thread is stuck",
                 presented_);
    return;
  }
  // The last reports (and the validator's CSVs), now that no record arrives.
  if (validate_) {
    validator_.Finish();
  }
  scene_.Finish();
  census_.Finish();
  trace_.Finish();
  // Nothing reads or watches guest memory any more.
  guest_memory_.Shutdown();
  // A guest frame the render thread was recording when it stopped: its list is
  // closed without executing, now -- closing it after the components below
  // destroyed what it names is a use after free (a D3D12 Close reads every
  // object the list recorded; seen as debug layer id 921 and a driver crash).
  if (frames_.is_open()) {
    scene_.EndFrame(nullptr);
    frames_.Abandon();
  }
  // 2. Drain. Every retirement is released here, release actions included,
  //    while all the components they name are still alive. Skipped by
  //    FrameRing when the device is lost.
  frames_.WaitIdle();
  // 3. Reverse creation order; the GPU is idle, so components destroy their
  //    objects directly. Ordered teardown rather than reblue's deliberate leak:
  //    every Plume object here has a single owner destroyed before the device.
  capture_.Shutdown();
  timing_.Shutdown();
  overlay_.Shutdown();
  swap_chain_.Shutdown();
  textures_.Shutdown();
  resolve_.Shutdown();
  passes_.Shutdown();
  edram_.Shutdown();
  geometry_.Shutdown();
  upload_.Shutdown();
  pipelines_.Shutdown();
  samplers_.Shutdown();
  shaders_.Shutdown();
  guest_layout_.Shutdown();
  heap_.Shutdown();
  frames_.Shutdown();
  device_.Shutdown();
  overlay_host_.reset();
  REXLOG_INFO("[gpu] renderer shut down after {} presents ({} guest, {} idle, {} skipped)",
              presented_, guest_presents_, idle_presents_, skipped_);
}

void Renderer::Execute(const RecordHeader& header, const uint8_t* payload) {
  const auto type = static_cast<size_t>(header.type);
  if (type < std::size(records_)) {
    ++records_[type];
  }
  records_executed_.fetch_add(1, std::memory_order_relaxed);
  trace_.OnRecord(guest_frames_, header, payload);
  // Scene records open the guest frame's list; the present closes it.
  switch (header.type) {
    case RecordType::kPresent:
      ++guest_frames_;
      if (validate_) {
        validator_.OnPresent(*reinterpret_cast<const PresentRecord*>(payload));
      }
      scene_.OnPresent();
      census_.OnPresent();
      ExecutePresent(PresentSource::kGuest);
      return;
    case RecordType::kDraw:
      if (validate_) {
        validator_.OnDraw(payload);
      }
      census_.OnDraw(payload);
      scene_.OnDraw(payload, OpenFrameList());
      MaybeFlushBatch();
      return;
    case RecordType::kClear: {
      const auto& record = *reinterpret_cast<const ClearRecord*>(payload);
      if (validate_) {
        validator_.OnClear(record);
      }
      scene_.OnClear(record, OpenFrameList());
      return;
    }
    case RecordType::kResolve: {
      const auto& record = *reinterpret_cast<const ResolveRecord*>(payload);
      if (validate_) {
        validator_.OnResolve(record);
      }
      scene_.OnResolve(record, OpenFrameList());
      return;
    }
    case RecordType::kTilingBegin: {
      const auto& record = *reinterpret_cast<const TilingBeginRecord*>(payload);
      if (validate_) {
        validator_.OnTilingBegin(record);
      }
      scene_.OnTilingBegin(record, OpenFrameList());
      return;
    }
    case RecordType::kTilingEnd: {
      const auto& record = *reinterpret_cast<const TilingEndRecord*>(payload);
      if (validate_) {
        validator_.OnTilingEnd(record);
      }
      scene_.OnTilingEnd(record, OpenFrameList());
      return;
    }
    case RecordType::kFence:
      ExecuteFence(*reinterpret_cast<const FenceRecord*>(payload));
      return;
    case RecordType::kPad:
    case RecordType::kCount:
      break;
  }
  REXLOG_ERROR("[gpu] render thread: unexpected record type {}", static_cast<uint32_t>(type));
}

// The render thread is the guest's GPU: everything captured before
// the fence was inserted has been executed by now -- the M4 render thread reads
// what a record names from guest memory while executing it -- so the guest may
// reuse that memory. Publishing the value is what lets it.
void Renderer::ExecuteFence(const FenceRecord& record) {
  if (mcla::native_gfx::nocp::PublishOwnedRetiredFence(record.device, record.value)) {
    fences_published_.fetch_add(1, std::memory_order_relaxed);
    last_fence_.store(record.value, std::memory_order_relaxed);
    return;
  }
  // Only after Shutdown handed the write back, while this thread stops: the
  // vblank thread already retires everything.
  if (fences_not_held_++ == 0) {
    REXLOG_WARN("[gpu] render thread: fence {} of device {:#010x} arrived while the retired "
                "fence is not held by this renderer; not published",
                record.value, record.device);
  }
}

plume::RenderCommandList* Renderer::OpenFrameList() {
  if (frames_.is_open()) {
    return frames_.list();
  }
  plume::RenderCommandList* list = frames_.Begin();
  if (!list) {
    return nullptr;
  }
  const uint32_t slot = frames_.slot();
  // Outside any render pass: timestamp pool reset, one-time heap transitions.
  timing_.BeginFrame(list, slot);
  heap_.RecordPendingBarriers(list);
  scene_.BeginFrame(list, slot);
  return list;
}

void Renderer::MaybeFlushBatch() {
  if (batch_draws_ == 0 || !frames_.is_open() || scene_.draws_since_flush() < batch_draws_) {
    return;
  }
  scene_.BeforeFlush(frames_.list());
  scene_.OnListChanged(frames_.FlushBatch());
}

void Renderer::SubmitFrameWithoutPresent() {
  if (!frames_.is_open()) {
    return;
  }
  plume::RenderCommandList* list = frames_.list();
  const uint32_t slot = frames_.slot();
  scene_.EndFrame(list);
  timing_.CompleteFrame(list, slot);
  if (frames_.Submit(nullptr, 0, nullptr, 0)) {
    timing_.OnSubmitted(slot);
  }
  frames_.Advance();
  // The slot about to be recorded had its fence waited by Advance.
  upload_.ResetSlot(frames_.slot());
}

void Renderer::OnIdle() {
  if (shutting_down_.load(std::memory_order_acquire) || device_.IsLost()) {
    return;
  }
  if (guest_has_presented_ &&
      std::chrono::steady_clock::now() - last_guest_present_ < kGuestActiveWindow) {
    // The guest is swapping; its presents carry the overlay.
    return;
  }
  if (frames_.is_open()) {
    // A guest frame is being recorded (the guest stopped between its first
    // scene record and its present, e.g. while loading): presenting now would
    // cut it in half. The overlay waits for the guest's present.
    return;
  }
  // The guest is quiet (not started yet, or stalled): keep the UI passes coming
  // and present when there is something new to show.
  overlay_host_->RequestPass();
  const bool first_frame = presented_ == 0;
  const bool resize = resize_requested_.load(std::memory_order_acquire);
  if (first_frame || resize || overlay_.HasNewContent()) {
    ExecutePresent(PresentSource::kIdle);
  }
}

bool Renderer::RebuildSwapChain() {
  // The back buffers are about to be released (and on D3D12 Release()d by
  // Plume's resize): nothing in flight may still write them.
  frames_.WaitIdle();
  return swap_chain_.Rebuild();
}

void Renderer::ExecutePresent(PresentSource source) {
  if (device_.IsLost()) {
    // Nothing recorded will run: drop the guest frame being recorded.
    if (frames_.is_open()) {
      scene_.EndFrame(nullptr);
      frames_.Abandon();
    }
    return;
  }
  if (source == PresentSource::kGuest) {
    guest_has_presented_ = true;
    last_guest_present_ = std::chrono::steady_clock::now();
  }
  swap_chain_.SetVsync(REXCVAR_GET(mcla_gpu_vsync));

  if (resize_requested_.exchange(false, std::memory_order_acq_rel) ||
      swap_chain_.NeedsRebuild()) {
    RebuildSwapChain();
  }
  if (swap_chain_.empty()) {
    // Minimised, or a rebuild the backend refused: nothing to present into.
    // The guest frame still runs (its targets and resolves feed later frames);
    // overlay uploads and deaths wait in the channel for the next frame.
    SubmitFrameWithoutPresent();
    ++skipped_;
    return;
  }

  swap_chain_.WaitForPresentSlot();
  uint32_t image = 0;
  if (!swap_chain_.Acquire(frames_.acquire_semaphore(), &image)) {
    // Out of date (Vulkan) or a lost chain: rebuild once and retry. A failed
    // acquire did not signal the semaphore, so nothing may wait on it.
    if (!RebuildSwapChain() || !swap_chain_.Acquire(frames_.acquire_semaphore(), &image)) {
      device_.CheckLost("swap chain acquire");
      SubmitFrameWithoutPresent();
      ++skipped_;
      return;
    }
  }

  // Already open when the guest frame had scene records.
  plume::RenderCommandList* list = OpenFrameList();
  if (!list) {
    ++skipped_;
    return;
  }
  const uint32_t slot = frames_.slot();
  // Outside any render pass: the scene's last work (geometry promotions), then
  // the overlay's texture uploads.
  scene_.EndFrame(list);
  overlay_.Prepare(list, slot);
  RecordPresentPass(list, slot, image);

  plume::RenderCommandSemaphore* waits[] = {frames_.acquire_semaphore()};
  plume::RenderCommandSemaphore* signals[] = {swap_chain_.render_semaphore(image)};
  if (!frames_.Submit(waits, 1, signals, 1)) {
    // Device lost: the render semaphore will never be signalled, so presenting
    // would wait forever on Vulkan.
    frames_.Advance();
    ++skipped_;
    return;
  }
  timing_.OnSubmitted(slot);

  if (!swap_chain_.Present(image)) {
    // DXGI and Vulkan both report a chain that no longer matches the window
    // here; anything else that fails here is the device.
    if (!device_.CheckLost("swap chain present")) {
      resize_requested_.store(true, std::memory_order_release);
    }
  }
  capture_.OnPresented();
  frames_.Advance();
  // The slot about to be recorded had its fence waited by Advance.
  upload_.ResetSlot(frames_.slot());
  ++presented_;
  if (source == PresentSource::kGuest) {
    ++guest_presents_;
  } else {
    ++idle_presents_;
  }
  ReportFrameStats();
  overlay_host_->RequestPass();
}

void Renderer::RecordPresentPass(plume::RenderCommandList* list, uint32_t slot, uint32_t image) {
  if (!reported_no_scene_) {
    reported_no_scene_ = true;
    REXLOG_WARN(
        "[gpu] the guest's draws are prepared but not drawn yet (milestone M4 in progress: "
        "buffers M4.4, EDRAM targets M4.5, textures and present M4.6): each frame is cleared to "
        "black under the SDK overlays.");
  }
  plume::RenderTexture* back = swap_chain_.texture(image);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(back, plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(swap_chain_.framebuffer(image));
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  timing_.Mark(list, slot, GpuTiming::kOverlayBegin);
  overlay_draws_ += overlay_.Draw(list, slot, swap_chain_.width(), swap_chain_.height());
  timing_.Mark(list, slot, GpuTiming::kOverlayEnd);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
  timing_.Mark(list, slot, GpuTiming::kFrameEnd);
}

void Renderer::ReportFrameStats() {
  // Once a minute at 60 fps: enough to see VRAM creep or a frame ring that
  // stops reclaiming, without a line per frame.
  if (presented_ % 3600 != 0) {
    return;
  }
  const auto vram = device_.QueryVideoMemory();
  const std::string vram_text =
      vram.budget ? std::to_string(vram.used >> 20) + " of " + std::to_string(vram.budget >> 20) +
                        " MiB"
                  : std::string("not reported");
  CommandStream& stream = thread_.stream();
  const uint64_t fence_waits = g_fence_waits.exchange(0, std::memory_order_relaxed);
  const uint64_t fence_wait_ns = g_fence_wait_ns.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO(
      "[gpu] {} presents ({} guest, {} idle), {} skipped | {} | overlay {} draws, {} textures | "
      "bindless {} of {}, samplers {} | upload {} chunks | {} batch flushes | {} retired pending "
      "| VRAM {} | {}x{} | records: {} draws, {} clears, {} resolves, {}/{} tiling, {} fences "
      "(last {}) | guest fence waits {} ({:.1f} ms) | stream {} MiB total, {} consumer wakes, {} "
      "producer waits",
      presented_, guest_presents_, idle_presents_, skipped_, timing_.TakeSummary(),
      overlay_draws_, overlay_.texture_count(), heap_.textures_in_use(),
      kBindlessTextureCount - kFirstAllocatableTextureSlot, heap_.samplers_in_use(),
      upload_.chunk_count(), frames_.TakeBatchFlushes(), frames_.retired_count(), vram_text,
      swap_chain_.width(), swap_chain_.height(),
      records_[size_t(RecordType::kDraw)], records_[size_t(RecordType::kClear)],
      records_[size_t(RecordType::kResolve)], records_[size_t(RecordType::kTilingBegin)],
      records_[size_t(RecordType::kTilingEnd)], records_[size_t(RecordType::kFence)],
      last_fence_.load(std::memory_order_relaxed), fence_waits, double(fence_wait_ns) / 1e6,
      stream.bytes_written() >> 20, stream.consumer_wakes(), stream.producer_waits());
  overlay_draws_ = 0;
  for (uint64_t& count : records_) {
    count = 0;
  }
}

// ---------------------------------------------------------------------------
// Module entry points (gpu.h)
// ---------------------------------------------------------------------------

namespace {

// Never destroyed: a guest thread that loaded the pointer just before Shutdown
// can still call into it, a render thread that missed the stop deadline still
// runs inside it, and every entry point is a no-op once shut down. The process
// ends in _Exit right after (LarecompApp::OnShutdown).
Renderer* g_renderer = nullptr;
std::atomic<Renderer*> g_active{nullptr};
// Same lifetime as g_renderer: a guest thread inside a capture when Shutdown
// runs finishes on the stopped stream.
GuestCapture* g_capture = nullptr;
std::atomic<GuestCapture*> g_capture_active{nullptr};
// The render thread writes the guest's retired fence: from the
// startup hand-over until Shutdown gives the write back to the vblank thread.
std::atomic<bool> g_holds_retired_fence{false};

// UI thread. The channel and drawer exist before the renderer: the SDK asks for
// the drawer inside its SetupPresentation, before Startup runs.
std::shared_ptr<OverlayChannel> g_overlay_channel;
OverlayDrawer* g_overlay_drawer = nullptr;  // owned by the SDK (immediate_drawer_)
std::shared_ptr<OverlayHost> g_overlay_host;

bool ResolveEnabled() {
  if (!REXCVAR_GET(mcla_gpu)) {
    return false;
  }
  const char* missing = nullptr;
  if (!REXCVAR_GET(mcla_native_gfx_nocp)) {
    missing = "mcla_native_gfx_nocp = true (no emulated GPU in the process)";
  } else if (REXCVAR_GET(mcla_native_gfx)) {
    missing = "mcla_native_gfx = false (the legacy runtime would present into the same window)";
  }
  // No user_name is fine: the SDK's first-launch profile dialog is drawn by the
  // overlay drawer and presented by the idle tick before the guest exists
  // (tested on a scratch config).
  if (missing) {
    REXLOG_ERROR("[gpu] mcla_gpu is set but needs {}; staying on the existing renderer", missing);
    return false;
  }
  REXLOG_INFO("[gpu] mcla_gpu: the Plume renderer ({}) owns presentation", BackendName());
  return true;
}

}  // namespace

bool Enabled() {
  static const bool enabled = ResolveEnabled();
  return enabled;
}

std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer() {
  if (!Enabled()) {
    return nullptr;
  }
  if (!g_overlay_channel) {
    g_overlay_channel = std::make_shared<OverlayChannel>();
  }
  auto drawer = std::make_unique<OverlayDrawer>(g_overlay_channel);
  g_overlay_drawer = drawer.get();
  return drawer;
}

bool Startup(rex::ui::Window* window, rex::ui::WindowedAppContext& app_context,
             rex::ui::ImGuiDrawer* imgui_drawer) {
  if (!Enabled()) {
    return false;
  }
  if (g_renderer) {
    return g_active.load(std::memory_order_acquire) != nullptr;
  }
  if (!window || !imgui_drawer || !g_overlay_drawer || !g_overlay_channel) {
    REXLOG_ERROR(
        "[gpu] startup needs the window, the SDK's ImGui drawer and the overlay drawer from "
        "CreateOverlayDrawer (window={}, imgui={}, overlay={}); refusing to run without overlays",
        window != nullptr, imgui_drawer != nullptr, g_overlay_drawer != nullptr);
    return false;
  }
  g_overlay_host =
      std::make_shared<OverlayHost>(app_context, *window, *imgui_drawer, *g_overlay_drawer);

  Renderer::Options options;
  options.device.preferred_adapter = REXCVAR_GET(mcla_gpu_adapter);
  options.device.debug_layer = REXCVAR_GET(mcla_gpu_debug_layer);
  options.swap_chain.vsync = REXCVAR_GET(mcla_gpu_vsync);
  options.swap_chain.present_wait = REXCVAR_GET(mcla_gpu_present_wait);
  g_renderer = new Renderer();
  if (!g_renderer->Initialize(window, options, g_overlay_channel, g_overlay_host)) {
    REXLOG_ERROR("[gpu] renderer startup failed; nothing will be presented");
    g_overlay_host->Disconnect();
    return false;
  }
  g_active.store(g_renderer, std::memory_order_release);

  // The fence hand-over, in this order: (a) take the retired
  // fence from the vblank thread, so nothing retires ahead of the render
  // thread; (b) turn the capture on, after which every fence the guest inserts
  // becomes a record; (c) one record that covers the fences inserted before
  // (b). In the normal order the guest has no device yet and (c) is a no-op.
  mcla::native_gfx::nocp::AcquireFenceRetirement();
  g_holds_retired_fence.store(true, std::memory_order_release);
  g_capture = new GuestCapture(g_renderer->stream(), REXCVAR_GET(mcla_gpu_verify_constants));
  g_capture_active.store(g_capture, std::memory_order_release);
  REXLOG_INFO("[gpu] guest capture on (constant verification {})",
              REXCVAR_GET(mcla_gpu_verify_constants) ? "on" : "off");
  if (const uint32_t device = mcla::native_gfx::GraphicsDeviceVa()) {
    auto* runtime = rex::Runtime::instance();
    const uint8_t* base = runtime ? runtime->virtual_membase() : nullptr;
    if (base && g_capture->FenceCatchUp(base, device)) {
      REXLOG_WARN("[gpu] the guest's device {:#010x} existed before the capture; one fence "
                  "record covers the fences it inserted until now",
                  device);
    }
  }

  // F11. The SDK's bind requests the capture from an IGraphicsSystem, which
  // this mode does not have. A second bind on the same key would never fire
  // (ProcessKeyEvent stops at the first match), so the SDK's is unregistered
  // first, as the legacy runtime does.
  rex::ui::UnregisterBind("bind_renderdoc_capture");
  rex::ui::RegisterBind("bind_gpu_renderdoc_capture", "F11",
                        "Capture the next frame with RenderDoc (Plume renderer)", [] {
                          if (Renderer* r = g_active.load(std::memory_order_acquire)) {
                            r->RequestCapture();
                          }
                        });
  // F10: raw trace of the next guest frame (frame_trace.h), for any scene the
  // frame counter cannot be aimed at (menus, pause, photo mode).
  rex::ui::RegisterBind("bind_gpu_trace_next", "F10",
                        "Write a raw trace of the next guest frame (Plume renderer)", [] {
                          if (Renderer* r = g_active.load(std::memory_order_acquire)) {
                            r->RequestTrace();
                            REXLOG_INFO("[gpu] trace: the frame after the next present will be "
                                        "written");
                          }
                        });
  return true;
}

GuestCallToken BeforeGuestDraw(const uint8_t* base, uint32_t device) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    return c->BeforeDraw(base, device);
  }
  return GuestCallToken{};
}

void AfterGuestDrawIndexed(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                           uint32_t primitive, int32_t base_vertex, uint32_t start_index,
                           uint32_t count) {
  if (!token.active) {
    return;
  }
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->AfterDraw(token, base, device, DrawKind::kIndexed, primitive, count, start_index,
                 base_vertex);
  }
}

void AfterGuestDrawVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                            uint32_t primitive, uint32_t start_vertex, uint32_t count) {
  if (!token.active) {
    return;
  }
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->AfterDraw(token, base, device, DrawKind::kAuto, primitive, count, start_vertex, 0);
  }
}

void AfterGuestBeginVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                             uint32_t primitive, uint32_t count, uint32_t stride,
                             uint32_t buffer) {
  if (!token.active) {
    return;
  }
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->AfterBeginVertices(token, base, device, primitive, count, stride, buffer);
  }
}

void AfterGuestEndVertices(const uint8_t* base, uint32_t device) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->AfterEndVertices(base, device);
  }
}

GuestCallToken BeforeGuestEvent(const uint8_t* base, uint32_t device, GuestEvent event,
                                uint32_t stack_pointer) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    return c->BeforeEvent(base, device, event, stack_pointer);
  }
  return GuestCallToken{};
}

// The After* of an event reaches the capture whenever its token is active, even
// once Shutdown began: it pops the pre-state Before* pushed on this thread, and
// a disabled capture writes nothing.
void AfterGuestClear(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                     uint32_t caller, uint32_t flags, uint32_t rect, uint32_t color, float z,
                     uint32_t stencil) {
  if (token.active && g_capture) {
    g_capture->AfterClear(token, base, device, caller, flags, rect, color, z, stencil);
  }
}

void AfterGuestResolve(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                       uint32_t caller, uint32_t flags, uint32_t source_rect,
                       uint32_t dest_texture, uint32_t dest_point, uint32_t dest_level,
                       uint32_t dest_slice, uint32_t clear_color, float clear_z) {
  if (token.active && g_capture) {
    g_capture->AfterResolve(token, base, device, caller, flags, source_rect, dest_texture,
                            dest_point, dest_level, dest_slice, clear_color, clear_z);
  }
}

void AfterGuestBeginTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                           uint32_t caller, uint32_t flags, uint32_t tile_count, uint32_t rects,
                           uint32_t clear_color, float clear_z, uint32_t clear_stencil) {
  if (token.active && g_capture) {
    g_capture->AfterBeginTiling(token, base, device, caller, flags, tile_count, rects,
                                clear_color, clear_z, clear_stencil);
  }
}

void AfterGuestEndTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                         uint32_t caller, uint32_t flags, uint32_t rects, uint32_t dest_texture,
                         uint32_t clear_color, float clear_z, uint32_t clear_stencil,
                         uint32_t params) {
  if (token.active && g_capture) {
    g_capture->AfterEndTiling(token, base, device, caller, flags, rects, dest_texture,
                              clear_color, clear_z, clear_stencil, params);
  }
}

void OnGuestFrameEnd(uint8_t* base) {
  if (!Enabled() || base == nullptr) {
    return;
  }
  // The game's own no-cache path: its command-buffer replay writes
  // PM4 that no hook sees. Held every frame because the guest turns the cache
  // back on. (The legacy occlusion.h comment predates the correction: it is a
  // command-buffer cache, not an occlusion query.)
  mcla::native_gfx::DisableGuestOcclusionQueries(base);
}

void OnGuestSwap(const uint8_t* base, uint32_t device, uint32_t front_buffer) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->Present(base, device, front_buffer);
  }
}

void OnGuestFenceInserted(uint32_t device, uint32_t value) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->Fence(device, value);
  }
}

bool RetiresGuestFences() {
  return g_holds_retired_fence.load(std::memory_order_acquire) &&
         g_capture_active.load(std::memory_order_acquire) != nullptr;
}

void OnGuestGpuHangCheck(const uint8_t* base, uint32_t device) {
  // The guest calls this from its fence spin after ~5000 ticks without the
  // retired value moving -- in this mode, a render thread busy for that long
  // (creating many pipelines the first time it sees a scene) or one that
  // stopped. Reported, never fatal: the first call of a stall, then every 10 s.
  using Clock = std::chrono::steady_clock;
  static std::atomic<int64_t> last_report_ms{0};
  static std::atomic<uint64_t> calls{0};
  const uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Clock::now().time_since_epoch())
                             .count();
  int64_t last = last_report_ms.load(std::memory_order_relaxed);
  if (last != 0 && now_ms - last < 10000) {
    return;
  }
  if (!last_report_ms.compare_exchange_strong(last, now_ms, std::memory_order_relaxed)) {
    return;
  }
  const mcla::native_gfx::GuestFenceState fence = mcla::native_gfx::ReadFenceState(base, device);
  Renderer::FenceProgress progress;
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    progress = r->fence_progress();
  }
  REXLOG_WARN("[gpu] the guest reports its GPU as hung (call {}): device {:#010x} issued {} "
              "retired {} | render thread: {} records executed, {} fences published, last {} | "
              "retired fence held by the renderer: {} | the guest keeps waiting",
              call, device, fence.issued, fence.retired, progress.records_executed,
              progress.fences_published, progress.last_fence,
              g_holds_retired_fence.load(std::memory_order_relaxed) ? "yes" : "no");
}

void NoteGuestFenceWait(uint64_t wait_ns) {
  g_fence_waits.fetch_add(1, std::memory_order_relaxed);
  g_fence_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
}

void RequestResize() {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestResize();
  }
}

void Shutdown() {
  // No new records; a guest thread inside a capture finishes against the
  // stream, which the render thread's Stop releases.
  if (GuestCapture* c = g_capture_active.exchange(nullptr, std::memory_order_acq_rel)) {
    c->Disable();
  }
  // UI thread: no UI pass touches the SDK's ImGui objects after this, even one
  // already posted (the SDK destroys them after OnShutdown).
  if (g_overlay_host) {
    g_overlay_host->Disconnect();
  }
  Renderer* r = g_active.exchange(nullptr, std::memory_order_acq_rel);
  if (r) {
    r->Shutdown();
  }
  // After the render thread stopped (or was let go): the fence records still
  // queued were dropped with it, and a guest thread waiting on one of them is
  // released by the vblank thread's next tick.
  if (g_holds_retired_fence.exchange(false, std::memory_order_acq_rel)) {
    mcla::native_gfx::nocp::ReleaseFenceRetirement();
  }
}

}  // namespace mcla::gpu

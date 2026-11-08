// MCLA GPU renderer -- the renderer and the module entry points. See
// renderer.h and gpu.h.

#include "renderer.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>

#include "backend.h"
#include "gpu.h"
#include "guest_capture.h"
#include "hdr.h"
#include "native_gfx/guest/guest_fence.h"
#include "native_gfx/guest/occlusion.h"
#include "native_gfx/guest/vblank_probe.h"
#include "native_gfx/nocp/guest_gpu.h"
#include "native_window.h"
#include "temporal_dlss.h"
#include "thread_load.h"
#include "latency.h"  // after the others: keeps this hunk apart from the neighbouring includes
#include "watch.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu, true, "MCLA/GPU",
    "Render with the Plume renderer (src/gpu) instead of the legacy D3D12 runtime. Needs "
    "mcla_native_gfx_nocp = true and mcla_native_gfx = false: it owns the window's swap chain, so "
    "neither the SDK presenter nor the legacy runtime can run beside it. Since milestone M4.6 it "
    "draws the guest's frames: draws into the EDRAM targets, textures from guest memory and "
    "from resolves, the front buffer through the display's gamma table. Not yet (logged when "
    "met): resolves written back into guest memory for the CPU to read (photos, vinyls) and "
    "memory read in another shape than it was resolved in -- milestone M5. "
    "")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(mcla_gpu_vsync, false, "MCLA/GPU",
                    "Present on vertical blank. Read every frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_pass_timing, false, "MCLA/GPU",
    "Measure GPU time per pass: a timestamp at every pass boundary (target change, resolve, "
    "clear, present, overlay), summed per pass and logged by cost with the frame statistics. "
    "A measurement for the performance work, off by "
    "default.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_present_wait, false, "MCLA/GPU",
    "Pace presents with the swap chain's latency wait (DXGI waitable object / "
    "VK_KHR_present_wait) when the device has it. On D3D12 Plume then presents with sync "
    "interval 0 and the wait alone paces the frame, so measure before turning it on.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(mcla_gpu_adapter, "", "MCLA/GPU",
                      "Adapter name to create the device on. Empty: on D3D12 the adapter Windows' "
                      "graphics setting for this app names, else the high-performance GPU; on "
                      "Vulkan Plume picks, or the GPU graphics_backend auto chose Vulkan for. The "
                      "emulated GPU (RENDERER: XENIA) runs on the same one unless d3d12_adapter or "
                      "vulkan_device is set.")
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

REXCVAR_DEFINE_BOOL(
    mcla_gpu_present_release, true, "MCLA/GPU",
    "Release the guest's Swap as soon as the render thread has read the last guest memory a "
    "present needs (after the scene's end-of-frame work and the front buffer's texture), and "
    "before it submits the frame and presents. Without it the guest waits for the whole present, "
    "about 0.2 ms a frame at 240 FPS. Not used with mcla_gpu_vsync, nor when the previous present "
    "took longer than 1 ms after that point (the GPU is behind): the guest then waits as before, "
    "so it never runs more than that ahead.")
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
REXCVAR_DECLARE(bool, mcla_gpu_scene_profile);

namespace {

// D3DDevice_BlockOnFence calls that found their fence pending, and the time
// they waited (gpu.h NoteGuestFenceWait); taken by the stats line.
std::atomic<uint64_t> g_fence_waits{0};
std::atomic<uint64_t> g_fence_wait_ns{0};
// D3DResource_Lock calls, and those on a resolve destination (gpu.h
// OnGuestLock); taken by the stats line.
std::atomic<uint64_t> g_guest_locks{0};
std::atomic<uint64_t> g_guest_locks_on_destinations{0};
// The swap chain's size (width << 32 | height), set at Initialize and after
// each rebuild, for gpu.h PreviewRenderScale; 0 before.
std::atomic<uint64_t> g_window_size{0};

void PublishWindowSize(uint32_t width, uint32_t height) {
  g_window_size.store(uint64_t(width) << 32 | height, std::memory_order_relaxed);
}

// The folder the app wants the shader packs in (gpu.h SetShaderPackFolder); empty: the default.
std::filesystem::path g_shader_pack_folder;
// The app's say on attaching the packs that are in place at Startup (gpu.h SetShaderPackGate).
std::function<bool()> g_shader_pack_gate;

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
  // mcla_gpu_hdr: reads the setting and the display the window is on; the swap chain asks whether
  // to try HDR10 (hdr.h).
  hdr::Start(render_window);
  SwapChain::Options swap_options = options.swap_chain;
  swap_options.hdr10 = hdr::Wanted();
  // Each step needs the ones before it; on a failure everything already made
  // is torn down in reverse (each Shutdown is a no-op on a component that was
  // never initialized).
  bool ok = device_.Initialize(options.device) && frames_.Initialize(device_) &&
            heap_.Initialize(device_, frames_) && guest_layout_.Initialize(device_, heap_) &&
            shaders_.Initialize(device_);
  if (ok) {
    // A pack that is in place is attached now, as before the store could start without one;
    // with none the store stays pending until AttachShaderPacks (the packs are built from the
    // player's game files after the renderer, which draws the ISO wizard, is up). Until then a
    // guest draw is skipped (Scene: kVsNotInPack, counted by the store too).
    // Scene::CountSkip logs the first four draws skipped for want of a pack as errors, then counts
    // them; the app attaches the packs before the guest starts, so none are skipped in its flow.
    // The app may hold the attach back: packs whose stamp is stale are rebuilt first, and a pack
    // that is mapped here could not be replaced.
    if (!g_shader_pack_gate || g_shader_pack_gate()) {
      shaders_.AttachFirst(ShaderStore::SearchFolders(g_shader_pack_folder));
    } else {
      REXLOG_INFO("[gpu] shader pack: not attached at startup; the app checks and, where needed, "
                  "rebuilds it first");
    }
    samplers_.Initialize(device_, heap_);
    pipelines_.Initialize(device_, guest_layout_, shaders_);
    if (shaders_.attached()) {
      OnShaderPackAttached();
    }
    ok = upload_.Initialize(device_) &&
         geometry_.Initialize(device_, frames_, upload_, guest_memory_) &&
         // Before the EDRAM: the window's size can pick the render scale.
         swap_chain_.Initialize(device_, render_window, swap_options);
  }
  if (ok) {
    hdr::SetActive(swap_chain_.hdr());
    // The window's size: the swap chain's; a window with no area yet has none,
    // then what the window reports; with neither it measures as the game's
    // screen (1x).
    uint32_t width = swap_chain_.width();
    uint32_t height = swap_chain_.height();
    if (width == 0 || height == 0) {
      width = window->GetActualPhysicalWidth();
      height = window->GetActualPhysicalHeight();
      if (width == 0 || height == 0) {
        REXLOG_WARN("[gpu] the window has no size yet: a render scale taken from it measures it "
                    "as the game's {}x{}",
                    kAssumedScreenWidth, kAssumedScreenHeight);
      }
    }
    render_scale_ = ChooseStartRenderScale(width, height);
    PublishWindowSize(width, height);
    edram_.Initialize(device_, frames_, heap_, render_scale_);
    // mcla_gpu_edram_capture: the detector asks for a RenderDoc capture.
    edram_.SetCaptureRequest([this] { capture_.Request(); });
    // mcla_gpu_watch_base: the diagnostic watch, and its capture.
    watch::Configure();
    watch::SetCaptureRequest([this] { capture_.Request(); });
    ok = passes_.Initialize(device_, frames_, heap_) &&
         resolve_memory_.Initialize(device_, frames_, heap_, upload_, guest_memory_, passes_,
                                    textures_, pass_timing_) &&
         resolve_.Initialize(device_, frames_, heap_, guest_memory_, textures_, edram_, passes_,
                             pass_timing_, resolve_memory_);
  }
  if (ok) {
    // The ambient occlusion lists its pipelines too (ao.h); it never fails the renderer.
    ao_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);
    // The resolve conversions and the decode listed the pipelines a session
    // uses (host_precache.h): one builder thread makes them while the rest of
    // the renderer starts and the guest boots.
    passes_.precache().Start();
    geometry_.SetResolveMemory(&resolve_memory_);  //
    textures_.Initialize(device_, frames_, heap_, upload_, guest_memory_, resolve_,
                         resolve_memory_);
    vram_.Initialize(device_, textures_, resolve_, resolve_memory_, edram_);
    scene_.Initialize(upload_, pipelines_, samplers_, geometry_, edram_, textures_, resolve_,
                      guest_layout_, heap_, pass_timing_, post_);
    // The guest's occlusion queries (the sun's glare): never fails the renderer.
    occlusion_.Initialize(device_);
    scene_.SetOcclusion(&occlusion_);
    scene_.SetTemporal(&temporal_);
    scene_.SetAmbientOcclusion(&ao_);
    ok = post_.Initialize(device_, heap_, upload_, passes_, edram_, pass_timing_) &&
         present_.Initialize(device_, frames_, heap_, upload_, passes_, textures_, pass_timing_,
                             swap_chain_.format(), swap_chain_.hdr()) &&
         overlay_.Initialize(device_, frames_, heap_, upload_, std::move(overlay_channel),
                             swap_chain_.format(), swap_chain_.hdr());
  }
  if (ok) {
    // The frame's scene and UI as two layers: HDR10 output keeps the UI at paper white with them
    // (frame_layers.h). Not wanted, they make nothing and touch no draw; a failure to make them
    // leaves the HDR output on the front buffer (stage 1), which is not a reason to stop.
    layers_.Request(FrameLayers::kRequesterHdr, swap_chain_.hdr());
    const bool layers_ok = layers_.Initialize(passes_, edram_, pass_timing_);
    scene_.SetFrameLayers(&layers_);  // a draw is touched only while the layers are active
    if (swap_chain_.hdr()) {
      SetUpHdrScene(layers_ok);
    }
  }
  if (ok) {
    flicker_probe_.Initialize(device_, frames_, swap_chain_.format());
    // The temporal groundwork's passes: made on first use, never fail the renderer.
    // The upscaler backend (FidelityFX, when the build has it) is probed on a worker thread.
    temporal_.Initialize(device_, frames_, heap_, passes_, pass_timing_, textures_, resolve_,
                         edram_);
    // Frame generation: probed on a worker thread; the setting is taken at a frame boundary.
    fg_.Initialize(device_, frames_, passes_, pass_timing_, layers_, temporal_, swap_chain_,
                   &resize_requested_);
    // Its images and AMD's context are not something the budget's response can release; its lines
    // name them (vram_budget.h), and frame generation asks the budget's rule before it takes any.
    vram_.AddHolder("frame generation", [this] { return fg_.HeldBytes(); });
    // DLSS frame generation: asked of Streamline when it has started; the setting is taken at a
    // frame boundary.
    dlssg_.Initialize(device_, frames_, passes_, pass_timing_, layers_, temporal_, present_,
                      swap_chain_);
    timing_.Initialize(device_);
    pass_timing_.Initialize(device_, REXCVAR_GET(mcla_gpu_pass_timing));
    markers_.Initialize(device_);
    capture_.Initialize();
    census_.SetEnabled(REXCVAR_GET(mcla_gpu_memory_census));
    validate_ = REXCVAR_GET(mcla_gpu_validate);
    batch_draws_ = REXCVAR_GET(mcla_gpu_batch_draws);
    profile_records_ = REXCVAR_GET(mcla_gpu_scene_profile);
    release_early_ = REXCVAR_GET(mcla_gpu_present_release);
    // The swap chain exists and the render thread, which marks the frames, has not started.
    latency::Initialize(device_, swap_chain_);
    ok = thread_.Start(*this, kIdlePeriod);
  }
  if (!ok) {
    latency::Shutdown();
    capture_.Shutdown();
    occlusion_.Shutdown();
    pass_timing_.Shutdown();
    timing_.Shutdown();
    fg_.Shutdown();
    dlssg_.Shutdown(swap_chain_);
    temporal_.Shutdown();
    ao_.Shutdown();
    flicker_probe_.Shutdown();
    hdr_tone_.Shutdown();
    hdr_scene_.Shutdown();
    layers_.Shutdown();
    overlay_.Shutdown();
    present_.Shutdown();
    post_.Shutdown();
    vram_.Shutdown();
    textures_.Shutdown();
    resolve_.Shutdown();
    resolve_memory_.Shutdown();
    passes_.Shutdown();
    edram_.Shutdown();
    swap_chain_.Shutdown();
    fg_.ReleaseQueue();
    hdr::Stop();
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
              "validator {}, {}, present release {}",
              kFramesInFlight, kIdlePeriod.count(), validate_ ? "on" : "off",
              batch_draws_ ? "a batch every " + std::to_string(batch_draws_) + " draws"
                           : std::string("one submission per guest frame"),
              release_early_ ? "early" : "after the present");
  return true;
}

bool Renderer::SetUpHdrScene(bool layers_ok) {
  if (layers_ok &&
      (hdr_scene_.ready() || hdr_scene_.Initialize(passes_, layers_, pass_timing_))) {
    present_.SetHdrScene(&hdr_scene_);
    // Stage 3, the scene above paper white. Without the variants the scene stays at paper white.
    if (hdr_tone_.ready() ||
        hdr_tone_.Initialize(passes_, shaders_, layers_, post_, pass_timing_)) {
      layers_.SetSceneSource(&hdr_tone_);
      hdr_scene_.SetSceneSource(&hdr_tone_);
      scene_.SetHdrToneMap(&hdr_tone_);
    }
    return true;
  }
  REXLOG_ERROR("[gpu] hdr: the UI layer split could not be set up; the HDR output shows "
               "the front buffer as it is (the scene and the UI together)");
  layers_.Request(FrameLayers::kRequesterHdr, false);
  return false;
}

void Renderer::LoadHdrVariants() {
  if (!shaders_.attached() || shaders_.has_hdr_variants()) {
    return;
  }
  std::string why;
  if (!shaders_.LoadHdrVariants(&why)) {
    REXLOG_INFO("[gpu] hdr scene: the HDR variants of the game's shaders are not available ({}); "
                "the HDR output keeps the scene at paper white (stage 2)",
                why);
  }
}

void Renderer::OnShaderPackAttached() {
  // The HDR scene variants of the game's shaders (stage 3, hdr_tonemap.h), from the folder the
  // main pack came from, when HDR output is on. Before the pipeline records are read: a record
  // that names a variant is dropped while the variants are not loaded. They change the entries
  // (hdr_variant), so this is the one place they are loaded, for a pack attached at startup and for
  // one attached later; HdrToneMap::active() waits for them.
  // With HDR output off they are loaded when it is turned on (SwitchHdr).
  if (hdr::Wanted()) {
    LoadHdrVariants();
  }
  pipelines_.StartPersistence(shaders_);
}

bool Renderer::AttachShaderPacks(const std::filesystem::path& folder) {
  if (shaders_.attached()) {
    return true;
  }
  if (!initialized_) {
    REXLOG_ERROR("[gpu] shader pack: cannot attach, the renderer is not running");
    return false;
  }
  if (!shaders_.Attach(ShaderStore::BuildFolder(folder))) {
    return false;
  }
  OnShaderPackAttached();
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
  // The pacer drains what it holds and ends before anything waits the GPU idle or goes.
  fg_.StopPacer();
  if (release_early_) {
    REXLOG_INFO("[gpu] present release: {} of {} guest presents released the guest early",
                early_releases_, guest_presents_);
  }
  // The last reports (and the validator's CSVs), now that no record arrives.
  if (validate_) {
    validator_.Finish();
  }
  scene_.Finish();
  temporal_.Finish();
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
    markers_.Drop();
  }
  // 2. Drain. Every retirement is released here, release actions included,
  //    while all the components they name are still alive. Skipped by
  //    FrameRing when the device is lost.
  frames_.WaitIdle();
  // 3. Reverse creation order; the GPU is idle, so components destroy their
  //    objects directly. Ordered teardown rather than reblue's deliberate leak:
  //    every Plume object here has a single owner destroyed before the device.
  latency::Shutdown();  // the vendor's calls name the swap chain
  capture_.Shutdown();
  occlusion_.Shutdown();
  pass_timing_.Shutdown();
  timing_.Shutdown();
  fg_.Shutdown();
  // DLSS frame generation: the proxy swap chain is destroyed and the plugin unloaded before
  // Streamline itself is shut down (temporal_.Shutdown below).
  dlssg_.Shutdown(swap_chain_);
  temporal_.Shutdown();
  ao_.Shutdown();
  flicker_probe_.Shutdown();
  hdr_tone_.Shutdown();
  hdr_scene_.Shutdown();
  layers_.Shutdown();
  overlay_.Shutdown();
  present_.Shutdown();
  post_.Shutdown();
  vram_.Shutdown();
  textures_.Shutdown();
  resolve_.Shutdown();
  resolve_memory_.Shutdown();
  passes_.Shutdown();
  edram_.Shutdown();
  swap_chain_.Shutdown();
  fg_.ReleaseQueue();  // the swap chain made on it is gone
  hdr::Stop();
  geometry_.Shutdown();
  upload_.Shutdown();
  pipelines_.Shutdown();
  samplers_.Shutdown();
  shaders_.Shutdown();
  guest_layout_.Shutdown();
  // 4. A component shut down above can still retire a release, such as a bindless slot its
  //    images held (DLSS frame generation's, while it was generating). The release writes the
  //    heap, so it runs here, before the heap goes; the GPU is idle, so nothing is waited for.
  frames_.WaitIdle();
  heap_.Shutdown();
  frames_.Shutdown();
  device_.Shutdown();
  overlay_host_.reset();
  REXLOG_INFO("[gpu] renderer shut down after {} presents ({} guest, {} idle, {} skipped)",
              presented_, guest_presents_, idle_presents_, skipped_);
}

void Renderer::Execute(const RecordHeader& header, const uint8_t* payload) {
  if (!profile_records_) {
    ExecuteRecord(header, payload);
    return;
  }
  // The record's CPU time: its time less the GPU waits inside it (the totals only grow, so a Take inside the record is harmless).
  ThreadLoad& load = CurrentThreadLoad();
  const uint64_t start = cycles::Now();
  const uint64_t gpu_wait_start = load.gpu_wait_total();
  ExecuteRecord(header, payload);
  const uint64_t elapsed = cycles::Now() - start;
  const uint64_t waited = load.gpu_wait_total() - gpu_wait_start;
  load.AddRecord(uint32_t(header.type), elapsed - std::min(elapsed, waited));
}

void Renderer::ExecuteRecord(const RecordHeader& header, const uint8_t* payload) {
  const auto type = static_cast<size_t>(header.type);
  if (type < std::size(records_)) {
    ++records_[type];
  }
  // The render thread is the only writer: a plain store, not a locked add
  // (the hang check reads it from another thread).
  records_executed_.store(records_executed_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_relaxed);
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
      present_.OnGuestPresent(*reinterpret_cast<const PresentRecord*>(payload));
      // The guest waits for this record (kRecordSync); ReleaseGuest may let it go sooner.
      release_sequence_ = (header.flags & kRecordSync) != 0 ? header.sequence : 0;
      ExecutePresent(PresentSource::kGuest);
      release_sequence_ = 0;
      return;
    case RecordType::kDraw:
      if (validate_) {
        validator_.OnDraw(payload);
      }
      census_.OnDraw(payload);
      scene_.OnDraw(payload, OpenFrameList());
      MaybeFlushBatch();
      textures_.DrawExecuted();
      if (textures_.prefetching()) {
        PrefetchTextures(header);
      } else if (prefetch_live_) {
        // The burst is over: the next one starts from the record it is on then.
        prefetch_live_ = false;
        prefetch_position_ = 0;
        prefetch_ahead_ = 0;
      }
      return;
    case RecordType::kClear: {
      const auto& record = *reinterpret_cast<const ClearRecord*>(payload);
      if (validate_) {
        validator_.OnClear(record);
      }
      plume::RenderCommandList* list = OpenFrameList();
      PerfScope label(markers_, list, "Clear", PerfMarkers::kEvent);
      scene_.OnClear(record, list);
      return;
    }
    case RecordType::kResolve: {
      const auto& record = *reinterpret_cast<const ResolveRecord*>(payload);
      if (validate_) {
        validator_.OnResolve(record);
      }
      plume::RenderCommandList* list = OpenFrameList();
      PerfScope label(markers_, list, "Resolve", PerfMarkers::kEvent);
      scene_.OnResolve(record, list);
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
      // The guest may reuse what the records before this fence named once it is published: no
      // decode of theirs may still be reading it.
      textures_.QuiesceDecodes(header.sequence);
      ExecuteFence(*reinterpret_cast<const FenceRecord*>(payload));
      return;
    case RecordType::kLock: {
      // The write-back reads the products on the GPU: what this frame recorded
      // so far (the resolves among it) goes to the queue first, as a batch.
      const auto& lock = *reinterpret_cast<const LockRecord*>(payload);
      resolve_.OnLock(lock, [this, &lock] {
        if (frames_.is_open()) {
          markers_.Push(frames_.list(), "LockWriteBack", PerfMarkers::kEvent);
          scene_.BeforeFlush(frames_.list());
          // The encodes still waiting write bytes the write-back copies.
          resolve_memory_.FlushForLock(frames_.list(), lock.address, lock.size);
          pass_timing_.Mark(frames_.list(), "lock write-back");
          markers_.Pop(frames_.list(), PerfMarkers::kEvent);
          // The labels open here (Frame, Scene) stay open across the flush and end in the
          // frame's next list, on the same queue: the frame keeps one Frame.
          plume::RenderCommandList* next = frames_.FlushBatch();
          if (!next) {
            markers_.Drop();
          }
          scene_.OnListChanged(next);
        }
      });
      return;
    }
    case RecordType::kComposite: {
      plume::RenderCommandList* list = OpenFrameList();
      PerfScope label(markers_, list, "Composite", PerfMarkers::kEvent);
      scene_.OnComposite(*reinterpret_cast<const CompositeRecord*>(payload), list);
      return;
    }
    case RecordType::kOcclusion:
      occlusion_.OnRecord(*reinterpret_cast<const OcclusionRecord*>(payload));
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

void Renderer::PrefetchTextures(const RecordHeader& executing) {
  if (!frames_.is_open()) {
    return;
  }
  prefetch_live_ = true;
  CommandStream& stream = thread_.stream();
  // Never behind the record that just ran (it is still the stream's current one: Release comes
  // after Execute). A draw that ran was one that had been offered, if the position was ahead of it.
  const uint64_t here = stream.consumer_position() + executing.size;
  if (prefetch_position_ < here) {
    prefetch_position_ = here;
    prefetch_ahead_ = 0;
  } else if (prefetch_ahead_ > 0) {
    --prefetch_ahead_;
  }
  for (uint32_t budget = kPrefetchScanRecords;
       budget != 0 && prefetch_ahead_ < kPrefetchAheadDraws; --budget) {
    const uint64_t before = prefetch_position_;
    const RecordHeader* next = stream.Peek(&prefetch_position_);
    if (!next) {
      break;
    }
    if (next->type == RecordType::kPresent) {
      // The frame ends there: the next one's draws are looked at once it has started.
      prefetch_position_ = before;
      break;
    }
    if (next->type == RecordType::kDraw) {
      scene_.PrefetchTextures(next->sequence, reinterpret_cast<const uint8_t*>(next + 1));
      ++prefetch_ahead_;
    }
  }
}

plume::RenderCommandList* Renderer::OpenFrameList() {
  if (frames_.is_open()) {
    return frames_.list();
  }
  if (!presenting_) {
    // A guest frame starts: nothing of it is recorded, so the HDR setting can change the output now
    // (from a present the back buffer is already acquired: ExecutePresent does it before that).
    // The order of the live changes at a frame boundary: this one first (GPU idle, the swap chain
    // made again, the window's size published again), then Scene::BeginFrame: the render scale and
    // the upscaler path (TemporalSystem::BeginFrame, which reads the size published here), then the
    // MSAA level (MsaaLevel::BeginFrame, after the path's own change). Frame generation's swap chain
    // switch comes right after the HDR one (both make the swap chain again).
    ApplyHdrSetting();
    ApplyFrameGenerationSetting();
  }
  plume::RenderCommandList* list = frames_.Begin();
  if (!list) {
    return nullptr;
  }
  const uint32_t slot = frames_.slot();
  device_.BeginFrame();
  // A guest frame starts here for the latency reducer (not an idle present's list).
  latency::OnRenderFrameOpen();
  // Outside any render pass: timestamp pool reset, one-time heap transitions.
  timing_.BeginFrame(list, slot);
  pass_timing_.BeginFrame(list, slot);
  heap_.RecordPendingBarriers(list);
  occlusion_.BeginFrame(list, slot);
  scene_.BeginFrame(list, slot);
  // After the caches' own frame start, before any record of the frame: what
  // the budget releases is retired behind this frame's submission.
  vram_.BeginFrame();
  markers_.BeginFrame();
  markers_.Push(list, "Frame", PerfMarkers::kFrame);
  markers_.Push(list, "Scene", PerfMarkers::kPhase);
  return list;
}

void Renderer::MaybeFlushBatch() {
  if (batch_draws_ == 0 || !frames_.is_open() || scene_.draws_since_flush() < batch_draws_) {
    return;
  }
  scene_.BeforeFlush(frames_.list());
  plume::RenderCommandList* next = frames_.FlushBatch();
  if (!next) {
    markers_.Drop();
  }
  scene_.OnListChanged(next);
}

void Renderer::SubmitFrameWithoutPresent() {
  if (!frames_.is_open()) {
    return;
  }
  plume::RenderCommandList* list = frames_.list();
  const uint32_t slot = frames_.slot();
  markers_.Pop(list, PerfMarkers::kPhase);  // Scene
  markers_.Push(list, "EndOfFrame", PerfMarkers::kPhase);
  scene_.EndFrame(list);
  markers_.PopAll(list);
  timing_.CompleteFrame(list, slot);
  pass_timing_.CompleteFrame(list, slot);
  if (frames_.Submit(nullptr, 0, nullptr, 0)) {
    timing_.OnSubmitted(slot);
    pass_timing_.OnSubmitted(slot);
    occlusion_.OnSubmitted(slot);
    latency::OnRenderSubmitted();
  }
  frames_.Advance();
  // The slot about to be recorded had its fence waited by Advance.
  upload_.ResetSlot(frames_.slot());
  occlusion_.OnSlotReady(frames_.slot());
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
  // and present when there is something new to show. No frame is open, so this is a boundary where
  // the HDR setting is applied (and what it changed is shown).
  const bool hdr_changed = ApplyHdrSetting() | ApplyFrameGenerationSetting();
  overlay_host_->RequestPass();
  const bool first_frame = presented_ == 0;
  const bool resize = resize_requested_.load(std::memory_order_acquire);
  if (first_frame || resize || hdr_changed || overlay_.HasNewContent()) {
    ExecutePresent(PresentSource::kIdle);
  }
}

bool Renderer::RebuildSwapChain() {
  if (dlssg_.active()) {
    // DLSS frame generation must be off before any change of the window, and the proxy chain is not
    // resized: it is switched off, which makes the chain again, plain, at the window's size; frame
    // generation comes back by itself once the window has been still (DlssFrameGeneration::kSettle).
    if (SwitchDlssFrameGeneration(DlssFrameGeneration::Change::kOff, /*resize=*/true)) {
      return !swap_chain_.empty();
    }
  }
  // With frame generation the pacer presents: it stops first, and the present queue's work (every
  // image it copied or presented) is waited for.
  const FrameGeneration::PauseScope pause(fg_);
  // The back buffers are about to be released (and on D3D12 Release()d by
  // Plume's resize): nothing in flight may still write them.
  frames_.WaitIdle();
  // Vulkan's swap chain handle changes inside: the latency reducer's calls wait for it.
  latency::SwapChainChange latency_change;
  if (!swap_chain_.Rebuild()) {
    return false;
  }
  // The new size is what the render scale is measured against from the next frame boundary on
  // (temporal.cpp ReadRenderScale: with the screen's scale or an FSR1 preset the images follow).
  PublishWindowSize(swap_chain_.width(), swap_chain_.height());
  return true;
}

bool Renderer::ApplyHdrSetting() {
  if (swap_chain_.lost()) {
    // A switch left the window without a chain (nothing could be made, not even the old format):
    // try again each second, in the format the passes use.
    const auto now = std::chrono::steady_clock::now();
    if (now - swap_chain_retry_ < std::chrono::seconds(1)) {
      return false;
    }
    swap_chain_retry_ = now;
    const FrameGeneration::PauseScope pause(fg_);
    frames_.WaitIdle();
    SwapChain::Recreated result;
    {
      latency::SwapChainChange latency_change;
      result = swap_chain_.Recreate(present_.hdr_output());
      latency_change.Replaced(swap_chain_);
    }
    dlss::SwapChainRecreated();
    if (result != SwapChain::Recreated::kLost) {
      fg_.OnSwapChainChanged();
      dlssg_.OnSwapChainChanged();
    }
    if (result == SwapChain::Recreated::kLost) {
      if (!swap_chain_lost_logged_) {
        swap_chain_lost_logged_ = true;
        REXLOG_ERROR("[gpu] the window has no swap chain and a new one could not be made; trying "
                     "again each second (nothing is presented meanwhile)");
      }
      return false;
    }
    swap_chain_lost_logged_ = false;
    AdoptSwapChain();
    hdr::SwitchDone(swap_chain_.hdr(), swap_chain_.hdr());
    REXLOG_INFO("[gpu] the swap chain was made again after it was lost: {}x{}, {}", swap_chain_.width(),
                swap_chain_.height(), swap_chain_.hdr() ? "HDR10" : "SDR");
    return true;
  }
  bool to_hdr = false;
  std::string why;
  if (!hdr::SwitchWanted(swap_chain_.hdr(), &to_hdr, &why)) {
    return false;
  }
  return SwitchHdr(to_hdr, why);
}

void Renderer::AdoptSwapChain() {
  // What draws into the back buffer takes the set made for its format. The set of the format the
  // chain has was built (a switch builds the one it asks for first and the chain keeps the other
  // one's only after it was in use); a chain that came back in a format neither was built for is
  // not expected, and is logged.
  const bool hdr = swap_chain_.hdr();
  if (!present_.has_output(hdr)) {
    present_.BuildOutput(SwapChain::FormatFor(hdr), hdr);
  }
  if (!overlay_.has_target(hdr)) {
    overlay_.BuildTarget(SwapChain::FormatFor(hdr), hdr);
  }
  if (!present_.has_output(hdr) || !overlay_.has_target(hdr)) {
    REXLOG_ERROR("[gpu] hdr: the passes for the {} back buffer could not be made; the present and "
                 "the overlay keep the {} ones, which do not match the swap chain",
                 hdr ? "HDR10" : "SDR", present_.hdr_output() ? "HDR10" : "SDR");
  } else {
    present_.UseOutput(hdr);
    overlay_.UseTarget(hdr);
  }
  // The scene half of HDR output follows the chain: layers, float scene and tone map up, or down
  // (their images released at once; the idle wait frees them). Their failures leave an HDR10 output
  // at stage 1 (the front buffer at paper white), as at a start.
  if (hdr) {
    // Also when frame generation already holds the layers up (frame_generation.h): the HDR scene
    // and the tone map are set up (and are no-ops when they are) whenever the output goes HDR10.
    layers_.Request(FrameLayers::kRequesterHdr, true);
    SetUpHdrScene(layers_.active());
  } else if (!hdr && (layers_.active() || hdr_scene_.ready())) {
    layers_.Request(FrameLayers::kRequesterHdr, false);
    hdr_scene_.Release();
    hdr_tone_.Release();
  }
  // A new chain has no metadata; UpdateHdr sends it before the next present. The sizes are the
  // window's, published again as after any rebuild (RebuildSwapChain): the render scale is measured
  // against them at the next frame boundary (temporal.cpp ReadRenderScale), as after a resize.
  hdr_metadata_sent_ = false;
  PublishWindowSize(swap_chain_.width(), swap_chain_.height());
}

// CHECK: not run (written without a build). Look for, in the log of a switch: "hdr: switching A -> B
// (why)" then "hdr: switched A -> B in N ms (passes, swap chain, layers)"; a refusal says "could not
// switch" with the reason above it; the first frames after a switch to HDR may show the scene at
// paper white (stage 2) until the HDR variants' pipelines are made in the background (they are not
// in the pipeline cache when the session started without HDR: the records naming them are dropped
// at load, pipelines.cpp); the picture should flicker at most once.
bool Renderer::SwitchHdr(bool to_hdr, const std::string& why) {
  using Clock = std::chrono::steady_clock;
  const auto millis = [](Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };
  const char* const from_name = swap_chain_.hdr() ? "HDR10" : "SDR";
  const char* const to_name = to_hdr ? "HDR10" : "SDR";
  const bool from_hdr = swap_chain_.hdr();
  const auto begin = Clock::now();
  REXLOG_INFO("[gpu] hdr: switching {} -> {} ({})", from_name, to_name, why);

  // 1. What can fail without leaving a trace comes first, while the old chain still runs: the
  //    passes that draw into the new back buffer's format. A set that could not be made leaves
  //    everything as it was.
  if (!present_.BuildOutput(SwapChain::FormatFor(to_hdr), to_hdr) ||
      !overlay_.BuildTarget(SwapChain::FormatFor(to_hdr), to_hdr)) {
    REXLOG_ERROR("[gpu] hdr: could not switch {} -> {}: the present or overlay passes for the {} "
                 "back buffer could not be made (reason above); the output stays {}",
                 from_name, to_name, to_name, from_name);
    hdr::SwitchDone(to_hdr, from_hdr);
    return false;
  }
  if (to_hdr) {
    // The scene variants of the game's tone map; without them the HDR output keeps the scene at
    // paper white (logged).
    LoadHdrVariants();
  }
  const auto built = Clock::now();

  // 2. The swap chain, in the other format: nothing in flight may name the back buffers, and the
  //    latency reducer's calls that name the chain wait until the new one is in. With frame
  //    generation the pacer is paused for it (the new chain stays on the present queue).
  const FrameGeneration::PauseScope fg_pause(fg_);
  frames_.WaitIdle();
  SwapChain::Recreated result;
  {
    latency::SwapChainChange latency_change;
    result = swap_chain_.Recreate(to_hdr);
    latency_change.Replaced(swap_chain_);
  }
  dlss::SwapChainRecreated();  // D3D12 with Streamline: the plain new chain is offered again
  if (result != SwapChain::Recreated::kLost) {
    fg_.OnSwapChainChanged();  // the pacer's copy pass is for the chain's format
    dlssg_.OnSwapChainChanged();  // and DLSS frame generation's images and UI pass
  }
  const auto remade = Clock::now();

  if (result == SwapChain::Recreated::kLost) {
    // Not even the old format could be made. The passes stay as they were; ApplyHdrSetting makes
    // the chain again each second.
    REXLOG_ERROR("[gpu] hdr: switching {} -> {} left the window without a swap chain: neither the "
                 "new format nor the old one could be made; trying again each second",
                 from_name, to_name);
    swap_chain_lost_logged_ = true;
    swap_chain_retry_ = Clock::now();
    hdr::SwitchDone(to_hdr, from_hdr);
    return false;
  }
  // 3. What draws into the back buffer takes the new format's set, and the scene half of HDR output
  //    (layers, float scene, tone map) comes up or goes down with it.
  AdoptSwapChain();
  const bool now_hdr = swap_chain_.hdr();
  hdr::SwitchDone(to_hdr, now_hdr);
  const auto done = Clock::now();
  if (now_hdr == to_hdr) {
    REXLOG_INFO("[gpu] hdr: switched {} -> {} in {:.0f} ms (passes {:.0f} ms, swap chain {:.0f} ms, "
                "layers {:.0f} ms): swap chain {}x{}, {}",
                from_name, to_name, millis(begin, done), millis(begin, built),
                millis(built, remade), millis(remade, done), swap_chain_.width(),
                swap_chain_.height(), to_name);
    return true;
  }
  REXLOG_WARN("[gpu] hdr: could not switch {} -> {} (the output does not present the HDR10 colour "
              "space; the reason is above): the {} swap chain was made again and stays, {:.0f} ms; "
              "not tried again until mcla_gpu_hdr or Windows' HDR state changes",
              from_name, to_name, from_name, millis(begin, done));
  return true;
}

void Renderer::UpdateHdr() {
  if (!swap_chain_.hdr()) {
    return;
  }
  const HdrLevels levels = hdr::Levels();
  present_.SetHdrLevels(levels);
  overlay_.SetHdrLevels(levels);
  // The scene's curve is bent towards the display's peak (stage 3): the levels, and the game's display
  // gamma table the peak is inverted through.
  float gamma_table[256];
  hdr_tone_.SetDisplay(levels, present_.GreenGammaTable(gamma_table) ? gamma_table : nullptr);
  // The display tone maps towards what it is told the content reaches: the peak setting.
  const plume::RenderHdrMetadata metadata = hdr::Metadata();
  if (!hdr_metadata_sent_ || metadata.maxMasteringLuminance != hdr_metadata_.maxMasteringLuminance ||
      metadata.minMasteringLuminance != hdr_metadata_.minMasteringLuminance ||
      metadata.maxContentLightLevel != hdr_metadata_.maxContentLightLevel ||
      metadata.maxFrameAverageLightLevel != hdr_metadata_.maxFrameAverageLightLevel) {
    hdr_metadata_ = metadata;
    hdr_metadata_sent_ = true;
    if (!swap_chain_.SetHdrMetadata(metadata)) {
      // Once per change: Vulkan without VK_EXT_hdr_metadata lands here, the display then uses its
      // own defaults.
      REXLOG_WARN("[gpu] hdr: the display could not be told the content's levels (peak {:.0f} "
                  "nits); it uses its own",
                  metadata.maxContentLightLevel);
    }
  }
}

void Renderer::ExecutePresent(PresentSource source) {
  tail_open_ = false;
  const latency::IdlePresentScope idle_scope(source == PresentSource::kIdle);
  if (device_.IsLost()) {
    // Nothing recorded will run: drop the guest frame being recorded.
    if (frames_.is_open()) {
      scene_.EndFrame(nullptr);
      frames_.Abandon();
      markers_.Drop();
    }
    return;
  }
  if (source == PresentSource::kGuest) {
    guest_has_presented_ = true;
    last_guest_present_ = std::chrono::steady_clock::now();
  }
  if (!frames_.is_open()) {
    // A present with no frame recorded before it: a boundary too (a frame that has scene records
    // applied the setting when it opened).
    ApplyHdrSetting();
    ApplyFrameGenerationSetting();
  }
  struct PresentingScope {
    explicit PresentingScope(bool& flag) : flag_(flag) { flag_ = true; }
    ~PresentingScope() { flag_ = false; }
    bool& flag_;
  } presenting(presenting_);
  // Frame generation presents FIFO (or immediate by its own setting), whatever mcla_gpu_vsync says.
  swap_chain_.SetVsync(fg_.active() ? fg_.WantsVsync()
                                    : (dlssg_.active() ? dlssg_.WantsVsync()
                                                       : REXCVAR_GET(mcla_gpu_vsync)));
  UpdateHdr();

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
  if (fg_.active()) {
    ExecutePresentGenerated(source);
    return;
  }

  if (!dlssg_.active()) {
    // Streamline's pacer owns the chain's frame latency while DLSS frame generation runs (the
    // game's Reflex sleep paces the frames instead); the waitable object is not waited on.
    swap_chain_.WaitForPresentSlot();
  }
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
  if (dlssg_.active()) {
    // Streamline's fake swap chain image, which its frame generation may have left in GENERAL: the
    // present pass overwrites all of it, so the first barrier on it starts from UNDEFINED (Vulkan).
    backend::ForgetImageContents(swap_chain_.texture(image));
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
  markers_.Pop(list, PerfMarkers::kPhase);  // Scene
  markers_.Push(list, "EndOfFrame", PerfMarkers::kPhase);
  scene_.EndFrame(list);
  markers_.Pop(list, PerfMarkers::kPhase);
  overlay_.Prepare(list, slot);
  RecordPresentPass(list, slot, image, source);

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
  pass_timing_.OnSubmitted(slot);
  occlusion_.OnSubmitted(slot);
  latency::OnRenderSubmitted();

  latency::OnPresentBegin();
  const bool present_ok = swap_chain_.Present(image);
  latency::OnPresentEnd();
  if (dlssg_.active()) {
    dlssg_.AfterPresent();  // Streamline's status now and then
  }
  if (!present_ok) {
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
  // What its previous round counted for the guest's occlusion queries is readable now.
  occlusion_.OnSlotReady(frames_.slot());
  ++presented_;
  if (source == PresentSource::kGuest) {
    ++guest_presents_;
  } else {
    ++idle_presents_;
  }
  ReportFrameStats();
  overlay_host_->RequestPass();
  if (tail_open_ && source == PresentSource::kGuest) {
    tail_short_ = std::chrono::steady_clock::now() - tail_start_ <= kEarlyReleaseMaxTail;
  }
  tail_open_ = false;
}

bool Renderer::ApplyFrameGenerationSetting() {
  const FrameGeneration::Change change = fg_.Poll();
  if (change != FrameGeneration::Change::kNone) {
    return SwitchFrameGeneration(change);
  }
  // The two never run together: the setting names one of them (FSR's Poll ignores "dlss").
  const DlssFrameGeneration::Change dlss_change = dlssg_.Poll();
  if (dlss_change != DlssFrameGeneration::Change::kNone) {
    return SwitchDlssFrameGeneration(dlss_change, /*resize=*/false);
  }
  return false;
}

// CHECK: not run (written without a build). The log of a switch on: "[gpu] dlss frame generation:
// switching on", then Streamline's own sl.log lines for the plugin loading and the proxy factory,
// "on (Nx: ...)", and once Reflex has made a few sleeps "generating: Nx". The swap chain is made
// again twice around a resize (off, then on after the window has been still): the picture may flicker.
bool Renderer::SwitchDlssFrameGeneration(DlssFrameGeneration::Change change, bool resize) {
  using Clock = std::chrono::steady_clock;
  const bool on = change == DlssFrameGeneration::Change::kOn;
  const auto begin = Clock::now();
  if (on) {
    // The plugin is loaded before the chain is made through its proxy of the DXGI factory.
    switch (dlssg_.PrepareSwitchOn()) {
      case DlssFrameGeneration::Prepared::kWait:
        return false;  // Streamline is making a feature: asked again at the next frame boundary
      case DlssFrameGeneration::Prepared::kFailed:
        return false;  // logged; off for the session
      case DlssFrameGeneration::Prepared::kReady:
        break;
    }
  }
  REXLOG_INFO("[gpu] dlss frame generation: switching {}{} (the swap chain is made again{})",
              on ? "on" : "off", resize ? " for a change of the window" : "",
              on ? " as Streamline's own" : ", plain");
  const bool hdr = swap_chain_.hdr();
  bool ok = true;
  frames_.WaitIdle();
  SwapChain::Recreated result;
  {
    // The latency reducer's calls that name the chain wait until the new one is in.
    latency::SwapChainChange latency_change;
    swap_chain_.SetFactoryWrapper(on ? &dlss::UpgradeFactory : nullptr);
    result = swap_chain_.RecreateOn(nullptr, hdr,
                                    on ? dlssg_.WantsVsync() : REXCVAR_GET(mcla_gpu_vsync),
                                    on ? std::function<void()>{}
                                       : std::function<void()>([this] { dlssg_.BeforePlainChain(); }));
    latency_change.Replaced(swap_chain_);
  }
  if (result == SwapChain::Recreated::kLost && on) {
    // Streamline's proxy chain could not be made: the plain chain is made again, and frame
    // generation is off for the session.
    ok = false;
    latency::SwapChainChange latency_change;
    swap_chain_.SetFactoryWrapper(nullptr);
    result = swap_chain_.RecreateOn(nullptr, hdr, REXCVAR_GET(mcla_gpu_vsync),
                                    [this] { dlssg_.BeforePlainChain(); });
    latency_change.Replaced(swap_chain_);
  }
  if (!on || !ok) {
    dlss::SwapChainRecreated();  // the plain new chain is offered to Streamline's wrapper again
  }
  if (result == SwapChain::Recreated::kLost) {
    REXLOG_ERROR("[gpu] dlss frame generation: switching {} left the window without a swap chain; "
                 "trying again each second (nothing is presented meanwhile)",
                 on ? "on" : "off");
    swap_chain_lost_logged_ = true;
    swap_chain_retry_ = Clock::now();
    ok = false;
  }
  // A new chain has no metadata (UpdateHdr sends it before the next present), and its size is what the
  // render scale is measured against.
  hdr_metadata_sent_ = false;
  PublishWindowSize(swap_chain_.width(), swap_chain_.height());
  dlssg_.CompleteSwitch(change, ok, resize);
  REXLOG_INFO("[gpu] dlss frame generation: switched {} in {:.0f} ms", on ? "on" : "off",
              std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
  return true;
}

// CHECK: not run (written without a build). Look for, in the log of a switch: "frame generation:
// switching on/off", then "frame generation: on (FSR 3.1; ...)" or "off"; the first frames after it
// are real frames only until the context is made ("[gpu] frame generation (ffx): context for ... made
// in N ms"), then "first generation recorded". The swap chain is made again on the other queue, as an
// HDR switch does: the picture may flicker once. D3D12: the debug layer on the present queue's lists
// and the cross-queue use of the present images (the render queue wrote them, the present queue read
// them after a CPU wait on a fence); Vulkan: validation on the second queue (the swap chain's images
// are used on a queue other than the one that made the frame, the family is the same) and, on a GPU
// whose DIRECT family has one queue (AMD, Intel), that the shared VkQueue's lock does not hold the
// render thread's submissions behind a blocked present.
bool Renderer::SwitchFrameGeneration(FrameGeneration::Change change) {
  using Clock = std::chrono::steady_clock;
  const bool on = change == FrameGeneration::Change::kOn;
  const auto begin = Clock::now();
  REXLOG_INFO("[gpu] frame generation: switching {} (the swap chain is made again on the {} queue)",
              on ? "on" : "off", on ? "present" : "render");
  if (on && !fg_.PrepareSwitchOn(swap_chain_.format())) {
    return false;  // logged; frame generation stays off for the session
  }
  // The swap chain on the other queue: nothing in flight may name the back buffers (on the render
  // queue by WaitIdle, on the present queue by the pacer's pause), and the latency reducer's calls
  // that name the chain wait until the new one is in.
  const bool hdr = swap_chain_.hdr();
  bool ok = true;
  {
    const FrameGeneration::PauseScope pause(fg_);
    frames_.WaitIdle();
    SwapChain::Recreated result;
    {
      latency::SwapChainChange latency_change;
      result = swap_chain_.RecreateOn(on ? fg_.present_queue() : nullptr, hdr,
                                      on ? fg_.WantsVsync() : REXCVAR_GET(mcla_gpu_vsync));
      latency_change.Replaced(swap_chain_);
    }
    dlss::SwapChainRecreated();  // D3D12 with Streamline: the plain new chain is offered again
    if (result == SwapChain::Recreated::kLost && on) {
      // The present queue could not have it: the render queue's chain is made again, and frame
      // generation is off for the session.
      ok = false;
      latency::SwapChainChange latency_change;
      result = swap_chain_.RecreateOn(nullptr, hdr, REXCVAR_GET(mcla_gpu_vsync));
      latency_change.Replaced(swap_chain_);
      dlss::SwapChainRecreated();
    }
    if (result == SwapChain::Recreated::kLost) {
      REXLOG_ERROR("[gpu] frame generation: switching {} left the window without a swap chain; "
                   "trying again each second (nothing is presented meanwhile)",
                   on ? "on" : "off");
      swap_chain_lost_logged_ = true;
      swap_chain_retry_ = Clock::now();
      ok = false;
    }
  }
  // A new chain has no metadata (UpdateHdr sends it before the next present), and its size is what the
  // render scale is measured against.
  hdr_metadata_sent_ = false;
  PublishWindowSize(swap_chain_.width(), swap_chain_.height());
  fg_.CompleteSwitch(change, ok);
  REXLOG_INFO("[gpu] frame generation: switched {} in {:.0f} ms", on ? "on" : "off",
              std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
  return true;
}

void Renderer::DrawPresentInto(plume::RenderCommandList* list, uint32_t slot, HostImage& target,
                               uint32_t width, uint32_t height) {
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target.texture.get(),
                                             plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(target.framebuffer.get());
  // Black around the letterboxed picture (and a first use of the image is initialised).
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  present_.Draw(list, width, height);
  overlay_draws_ += overlay_.Draw(list, slot, width, height);
  list->setFramebuffer(nullptr);
  // The pacer's copy pass reads it next (on the present queue, after a CPU wait for the frame).
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  target.layout = plume::RenderTextureLayout::SHADER_READ;
}

bool Renderer::RecordGeneratedPresent(plume::RenderCommandList* list, uint32_t slot,
                                      const FrameGeneration::Pair& pair, PresentSource source) {
  const uint32_t width = swap_chain_.width();
  const uint32_t height = swap_chain_.height();
  pass_timing_.Mark(list, "present");
  markers_.Push(list, "Present", PerfMarkers::kPhase);
  // The real frame, as ever, into the pair's real image: the front buffer through the display's
  // table, FSR1, the overlay.
  present_.Prepare(list, slot, width, height);
  if (source == PresentSource::kGuest && !swap_chain_.hdr()) {
    flicker_probe_.Record(list, present_, guest_presents_ + 1);
  }
  ReleaseGuest();
  FrameGeneration::GenerateArgs args;
  const bool have_front = present_.front_key(&args.front) && present_.source_ready();
  args.guest_frame = source == PresentSource::kGuest && have_front;
  args.front_width = present_.source_width();
  args.front_height = present_.source_height();
  args.window_width = width;
  args.window_height = height;
  timing_.Mark(list, slot, GpuTiming::kOverlayBegin);
  DrawPresentInto(list, slot, *pair.real, width, height);
  timing_.Mark(list, slot, GpuTiming::kOverlayEnd);
  // The generated frame, when this frame can be generated: the same present path over the generated
  // scene with this frame's UI on it, into the pair's other image.
  bool has_generated = false;
  const FrameGeneration::Generated generated = fg_.Generate(list, args);
  if (generated.image) {
    present_.PrepareImage(list, width, height, *generated.image, generated.float_scene);
    if (present_.source_ready()) {
      DrawPresentInto(list, slot, *pair.generated, width, height);
      has_generated = true;
    }
  }
  markers_.Pop(list, PerfMarkers::kPhase);  // Present
  markers_.PopAll(list);                    // Frame
  timing_.Mark(list, slot, GpuTiming::kFrameEnd);
  pass_timing_.CompleteFrame(list, slot);
  return has_generated;
}

void Renderer::ExecutePresentGenerated(PresentSource source) {
  // ExecutePresent's second half with frame generation running: the swap chain is the pacer's, so
  // nothing is acquired or presented here; a slot of present images is taken (waiting for the pacer
  // when it is a frame behind), the frame is painted into it and the pair is handed over.
  FrameGeneration::Pair pair;
  if (!fg_.AcquireSlot(swap_chain_.width(), swap_chain_.height(), swap_chain_.format(), &pair)) {
    // No free slot in time (the pacer is stalled), or no image: this frame is not shown.
    SubmitFrameWithoutPresent();
    ++skipped_;
    return;
  }
  // Already open when the guest frame had scene records.
  plume::RenderCommandList* list = OpenFrameList();
  if (!list) {
    fg_.ReturnSlot(pair);
    ++skipped_;
    return;
  }
  const uint32_t slot = frames_.slot();
  markers_.Pop(list, PerfMarkers::kPhase);  // Scene
  markers_.Push(list, "EndOfFrame", PerfMarkers::kPhase);
  scene_.EndFrame(list);
  markers_.Pop(list, PerfMarkers::kPhase);
  overlay_.Prepare(list, slot);
  const bool has_generated = RecordGeneratedPresent(list, slot, pair, source);

  if (!frames_.Submit(nullptr, 0, nullptr, 0)) {
    fg_.ReturnSlot(pair);
    frames_.Advance();
    ++skipped_;
    return;
  }
  timing_.OnSubmitted(slot);
  pass_timing_.OnSubmitted(slot);
  occlusion_.OnSubmitted(slot);
  latency::OnRenderSubmitted();
  // The present markers are the pacer's: it names the frame around the real image's present.
  const uint64_t latency_frame = latency::DetachRenderFrame();
  fg_.Submit(pair, has_generated, latency_frame, source == PresentSource::kGuest);
  capture_.OnPresented();
  frames_.Advance();
  // The slot about to be recorded had its fence waited by Advance.
  upload_.ResetSlot(frames_.slot());
  occlusion_.OnSlotReady(frames_.slot());
  ++presented_;
  if (source == PresentSource::kGuest) {
    ++guest_presents_;
  } else {
    ++idle_presents_;
  }
  ReportFrameStats();
  overlay_host_->RequestPass();
  if (tail_open_ && source == PresentSource::kGuest) {
    tail_short_ = std::chrono::steady_clock::now() - tail_start_ <= kEarlyReleaseMaxTail;
  }
  tail_open_ = false;
}

void Renderer::ReleaseGuest() {
  // From here the present reads no guest memory: the scene's end-of-frame work, the
  // front buffer's texture (present_.Prepare) and the gamma table are done; what follows
  // draws from the GPU's copies, submits and presents.
  tail_start_ = std::chrono::steady_clock::now();
  tail_open_ = true;
  if (release_sequence_ == 0 || !release_early_ || !tail_short_ ||
      REXCVAR_GET(mcla_gpu_vsync)) {
    return;
  }
  thread_.stream().MarkExecuted(release_sequence_);
  release_sequence_ = 0;
  ++early_releases_;
}

void Renderer::RecordPresentPass(plume::RenderCommandList* list, uint32_t slot, uint32_t image,
                                 PresentSource source) {
  // Outside the back buffer's pass: the front buffer's texture (an upload when
  // it comes from memory), the gamma table and FSR1's passes
  // before the back buffer's.
  pass_timing_.Mark(list, "present");
  markers_.Push(list, "Present", PerfMarkers::kPhase);
  present_.Prepare(list, slot, swap_chain_.width(), swap_chain_.height());
  if (source == PresentSource::kGuest && !swap_chain_.hdr()) {
    // An idle present shows the same front buffer again: not a new frame. (Not with an HDR10 back
    // buffer: the probe records 8-bit SDR, and a live HDR switch can turn the chain HDR10 under it.)
    flicker_probe_.Record(list, present_, guest_presents_ + 1);
  }
  ReleaseGuest();
  plume::RenderTexture* back = swap_chain_.texture(image);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(back, plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(swap_chain_.framebuffer(image));
  // Black around the letterboxed front buffer.
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  present_.Draw(list, swap_chain_.width(), swap_chain_.height());
  markers_.Pop(list, PerfMarkers::kPhase);  // Present
  timing_.Mark(list, slot, GpuTiming::kOverlayBegin);
  pass_timing_.Mark(list, "overlay");
  markers_.Push(list, "Overlay", PerfMarkers::kPhase);
  overlay_draws_ += overlay_.Draw(list, slot, swap_chain_.width(), swap_chain_.height());
  markers_.Pop(list, PerfMarkers::kPhase);
  timing_.Mark(list, slot, GpuTiming::kOverlayEnd);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
  if (dlssg_.active()) {
    // DLSS frame generation: what the plugin is given with the back buffer (the picture without the
    // UI, the UI, the depth and the vectors) and its options for this frame; the present pass's own
    // state (the front buffer it bound) is read first, the passes below overwrite it.
    DlssFrameGeneration::FrameArgs args;
    const bool have_front = present_.front_key(&args.front) && present_.source_ready();
    args.guest_frame = source == PresentSource::kGuest && have_front;
    args.front_width = present_.source_width();
    args.front_height = present_.source_height();
    args.window_width = swap_chain_.width();
    args.window_height = swap_chain_.height();
    args.back_buffers = swap_chain_.image_count();
    args.hdr = swap_chain_.hdr();
    dlssg_.Record(list, args);
  }
  markers_.PopAll(list);  // Frame
  timing_.Mark(list, slot, GpuTiming::kFrameEnd);
  pass_timing_.CompleteFrame(list, slot);
}

void Renderer::ReportFrameStats() {
  // Once a minute at 60 fps: enough to see VRAM creep or a frame ring that
  // stops reclaiming, without a line per frame.
  if (presented_ % 3600 != 0) {
    return;
  }
  const auto vram = device_.QueryVideoMemory();
  // What the renderer itself holds, by kind: where the VRAM goes (the budget trim releases the first
  // four when memory runs short, vram_budget.cpp; the frame layers give theirs back after
  // FrameLayers::kReleaseAfterFrames guest frames without layers).
  const auto mib = [](uint64_t bytes) { return std::to_string((bytes + (1u << 19)) >> 20); };
  const std::string held = " (held: textures " + mib(textures_.HeldBytes()) + ", resolve products " +
                           mib(resolve_.HeldBytes()) + ", EDRAM targets " + mib(edram_.HeldBytes()) +
                           ", resolve memory " + mib(resolve_memory_.HeldBytes()) +
                           (layers_.active() ? ", frame layers " + mib(layers_.HeldBytes() +
                                                                      hdr_scene_.HeldBytes() +
                                                                      hdr_tone_.HeldBytes())
                                             : std::string()) +
                           ", ambient occlusion " + mib(ao_.HeldBytes()) +
                           (fg_.active() ? ", frame generation " + mib(fg_.HeldBytes())
                                         : std::string()) +
                           (dlssg_.active() ? ", DLSS frame generation " + mib(dlssg_.HeldBytes())
                                            : std::string()) +
                           ", upscaler (output, depth, vectors, its own context) " +
                           mib(temporal_.HeldBytes()) + " MiB)";
  const std::string vram_text =
      (vram.budget ? std::to_string(vram.used >> 20) + " of " + std::to_string(vram.budget >> 20) +
                         " MiB"
                   : std::string("not reported")) +
      held;
  CommandStream& stream = thread_.stream();
  const uint64_t fence_waits = g_fence_waits.exchange(0, std::memory_order_relaxed);
  const uint64_t fence_wait_ns = g_fence_wait_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t locks = g_guest_locks.exchange(0, std::memory_order_relaxed);
  const uint64_t locks_on_destinations =
      g_guest_locks_on_destinations.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO(
      "[gpu] {} presents ({} guest, {} idle), {} skipped | {} | overlay {} draws, {} textures | "
      "bindless {} of {}, samplers {} | upload {} chunks | {} batch flushes | {} retired pending "
      "| VRAM {} | {}x{} | records: {} draws, {} clears, {} resolves, {}/{} tiling, {} fences "
      "(last {}), {} locks | guest locks {} ({} on resolve destinations) | guest fence waits {} "
      "({:.1f} ms) | stream {} MiB total, {} consumer wakes, {} producer waits",
      presented_, guest_presents_, idle_presents_, skipped_, timing_.TakeSummary(),
      overlay_draws_, overlay_.texture_count(), heap_.textures_in_use(),
      kBindlessTextureCount - kFirstAllocatableTextureSlot, heap_.samplers_in_use(),
      upload_.chunk_count(), frames_.TakeBatchFlushes(), frames_.retired_count(), vram_text,
      swap_chain_.width(), swap_chain_.height(),
      records_[size_t(RecordType::kDraw)], records_[size_t(RecordType::kClear)],
      records_[size_t(RecordType::kResolve)], records_[size_t(RecordType::kTilingBegin)],
      records_[size_t(RecordType::kTilingEnd)], records_[size_t(RecordType::kFence)],
      last_fence_.load(std::memory_order_relaxed), records_[size_t(RecordType::kLock)], locks,
      locks_on_destinations, fence_waits, double(fence_wait_ns) / 1e6,
      stream.bytes_written() >> 20, stream.consumer_wakes(), stream.producer_waits());
  REXLOG_INFO("[gpu] {}", present_.TakeSummary());
  if (fg_.active()) {
    REXLOG_INFO("[gpu] {}", fg_.TakeSummary());
  }
  if (dlssg_.active()) {
    REXLOG_INFO("[gpu] {}", dlssg_.TakeSummary());
  }
  REXLOG_INFO("[gpu] {}", post_.TakeSummary());
  if (layers_.active()) {
    REXLOG_INFO("[gpu] {}", layers_.TakeSummary());
    REXLOG_INFO("[gpu] {}", hdr_scene_.TakeSummary());
    if (hdr_tone_.active()) {
      REXLOG_INFO("[gpu] {}", hdr_tone_.TakeSummary());
    }
  }
  if (flicker_probe_.enabled()) {
    REXLOG_INFO("[gpu] {}", flicker_probe_.TakeSummary());
  }
  if (pass_timing_.enabled()) {
    REXLOG_INFO("[gpu] {}", pass_timing_.TakeSummary());
  }
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
// What graphics_backend asks this renderer for (RequestedBackend), decided with
// Enabled().
Backend g_requested_backend = kDefaultBackend;
// Set only when "auto" chose Vulkan: the GPU that choice was made on, which the Vulkan device is
// created on. Empty otherwise.
std::string g_auto_adapter;

// The SDK's graphics_backend -- the cvar that also picks the
// emulated GPU's backend -- as a backend this build has.
Backend RequestedBackend() {
  if (!rex::cvar::GetFlagInfo("graphics_backend")) {
    REXLOG_INFO("[gpu] this SDK has no graphics_backend cvar; {} runs",
                BackendName(kDefaultBackend));
    return kDefaultBackend;
  }
  const std::string text = rex::cvar::GetFlagByName("graphics_backend");
  Backend backend = kDefaultBackend;
  if (!ParseBackend(text, backend)) {
    REXLOG_ERROR("[gpu] graphics_backend = \"{}\" is not auto, d3d12 or vulkan; {} runs", text,
                 BackendName(backend));
  } else if (!BackendCompiled(backend)) {
    REXLOG_ERROR("[gpu] graphics_backend = \"{}\", but this build has no {}; {} runs", text,
                 BackendName(backend), BackendName(kDefaultBackend));
    backend = kDefaultBackend;
  } else if (text == "auto") {
    backend = ChooseAutoBackend(REXCVAR_GET(mcla_gpu_adapter), g_auto_adapter);
  }
  return backend;
}

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
  g_requested_backend = RequestedBackend();
  REXLOG_INFO("[gpu] mcla_gpu: the Plume renderer ({}) owns presentation",
              BackendName(g_requested_backend));
  return true;
}

}  // namespace

bool Enabled() {
  static const bool enabled = ResolveEnabled();
  return enabled;
}

Device::Options PlannedDeviceOptions(const Backend* forced) {
  Device::Options options;
  options.preferred_adapter = REXCVAR_GET(mcla_gpu_adapter);
  if (options.preferred_adapter.empty() && (!forced || *forced == g_requested_backend)) {
    // "auto" chose Vulkan for one GPU: render on it, not on the one Plume's Vulkan rule takes.
    options.preferred_adapter = g_auto_adapter;
  }
  options.debug_layer = REXCVAR_GET(mcla_gpu_debug_layer);
  options.backend = forced ? *forced : g_requested_backend;
  return options;
}

std::vector<std::filesystem::path> ShaderPackSearchFolders() {
  return ShaderStore::SearchFolders(g_shader_pack_folder);
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
  options.device = PlannedDeviceOptions(nullptr);  // shared with the pipeline prebuild
  options.swap_chain.vsync = REXCVAR_GET(mcla_gpu_vsync);
  options.swap_chain.present_wait = REXCVAR_GET(mcla_gpu_present_wait);
  g_renderer = new Renderer();
  if (!g_renderer->Initialize(window, options, g_overlay_channel, g_overlay_host)) {
    REXLOG_ERROR("[gpu] renderer startup failed; nothing will be presented");
    g_overlay_host->Disconnect();
    return false;
  }
  if (g_renderer->device().fell_back() && !g_auto_adapter.empty()) {
    // "auto" picked Vulkan, not the player, and D3D12 is what "auto" ran before: no box.
    REXLOG_WARN("[gpu] Vulkan, which graphics_backend auto picked for '{}', could not be started "
                "(the reason is above); D3D12 runs",
                g_auto_adapter);
  } else if (g_renderer->device().fell_back()) {
    // The log has the reason; the player chose the API in the menu and would
    // otherwise never learn that another one runs.
    rex::ShowSimpleMessageBox(
        rex::SimpleMessageBoxType::Warning,
        std::string(BackendName(g_renderer->device().requested_backend())) +
            " could not be started on this machine, so the game runs on " + BackendName() +
            ". The reason is in the log. The API is chosen in the pause menu: RENDERER, "
            "GRAPHICS API (graphics_backend in larecomp.toml).");
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
  // F2: every texture of the next frame to the dump folder, named after the
  // game's texture and its dictionary, for texture mods (textures.h).
  rex::ui::RegisterBind("bind_gpu_texture_dump", "F2",
                        "Dump the textures of the next frame (Plume renderer)", [] {
                          if (Renderer* r = g_active.load(std::memory_order_acquire)) {
                            r->RequestTextureDump();
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

void OnGuestLock(uint32_t resource, uint32_t level, uint32_t address, uint32_t size) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    g_guest_locks.fetch_add(1, std::memory_order_relaxed);
    if (c->Lock(resource, level, address, size)) {
      g_guest_locks_on_destinations.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void OnGuestSceneComposited(uint32_t bucket) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->Composite(bucket);
  }
}

void OnGuestOcclusionEvent(const uint8_t* base, uint32_t query, bool is_begin) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->OcclusionEvent(base, query, is_begin);
  }
}

void OnGuestQueryGetData(const uint8_t* base, uint32_t query) {
  if (g_capture_active.load(std::memory_order_acquire) != nullptr) {
    occlusion::OnGuestGetData(const_cast<uint8_t*>(base), query);
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

void RequestTextureDumpAll() {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestTextureDumpAll();
  }
}

void RequestTextureReload() {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestTextureReload();
  }
}

RenderScaleChoice PreviewRenderScale(double asked, bool fsr, FsrPreset preset) {
  const uint64_t size = g_window_size.load(std::memory_order_relaxed);
  return ChooseRenderScale(asked, fsr, preset, uint32_t(size >> 32), uint32_t(size));
}

RenderScaleChoice WantedRenderScale() {
  const uint64_t size = g_window_size.load(std::memory_order_relaxed);
  return ChooseWantedRenderScale(uint32_t(size >> 32), uint32_t(size));
}

void SetShaderPackFolder(const std::filesystem::path& folder) {
  g_shader_pack_folder = folder;
}

void SetShaderPackGate(std::function<bool()> gate) {
  g_shader_pack_gate = std::move(gate);
}

std::filesystem::path ShaderPackFolder() {
  return ShaderStore::BuildFolder(g_shader_pack_folder);
}

bool ShaderPacksAttached() {
  Renderer* r = g_active.load(std::memory_order_acquire);
  return r && r->shader_packs_attached();
}

bool AttachShaderPacks(const std::filesystem::path& folder) {
  Renderer* r = g_active.load(std::memory_order_acquire);
  if (!r) {
    REXLOG_ERROR("[gpu] shader pack: cannot attach, the Plume renderer is not running");
    return false;
  }
  return r->AttachShaderPacks(folder.empty() ? g_shader_pack_folder : folder);
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

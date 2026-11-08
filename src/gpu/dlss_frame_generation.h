#pragma once
// ===========================================================================
// MCLA GPU renderer -- NVIDIA DLSS frame generation (F4-DESIGN.md sections 8.3 and 15)
// ===========================================================================
// An interpolated frame between every two the game renders (2x), or two or three (3x, 4x on an
// RTX 50 series), made by NVIDIA's DLSS frame generation through Streamline's sl.dlss_g plugin. It
// is the NVIDIA counterpart of FSR frame generation (frame_generation.h) and is chosen in the same
// setting: mcla_gpu_frame_generation = off | fsr31 | dlss (live), with mcla_gpu_frame_generation_
// multiplier (2, 3 or 4) beside it where the card takes more than 2x. The two never run together.
// An RTX 40 or 50 series card (the plugin's own support query decides, never a list: on D3D12 and
// on Vulkan with Streamline hooked into the device, which is a start-time choice there like DLSS
// Super Resolution's), the files next to the game (the launcher's setup fetches them on a PC with an
// NVIDIA RTX card), hardware-accelerated GPU scheduling on in Windows.
//
// What is different from FSR's: Streamline owns the swap chain and the pacing.
//  - The swap chain is Streamline's proxy chain while frame generation runs: the frame generation
//    plugin is loaded (temporal_dlss.h dlss::LoadFrameGeneration), the chain made again through the
//    plugin's proxy of the DXGI factory (D3D12: SwapChain::SetFactoryWrapper; Vulkan: the chain is
//    made through the interposer's functions, whose plugin hooks are then in), and Present is
//    asynchronous on the plugin's pacer. Switched off, the chain is made again, plain, after the plugin
//    is unloaded, so nothing pays for the proxy (the guide's section 18). FSR's pacer thread and its
//    second queue are not used: the real frame is presented as ever on the render queue.
//  - Reflex through Streamline is required and is made the latency reducer while it runs
//    (latency.h UseStreamline): the PC latency markers carry the frame index the constants and tags
//    have, the id of the frame the host render thread is presenting (latency::RenderFrameId); a frame
//    that has none is not generated.
//  - Inputs, per generated frame (dlss::TagFrameGeneration, valid until the present): the constants
//    from the temporal system's camera (the same as Super Resolution's), the depth and motion vectors it
//    keeps (the render size), the HUD-less colour and the UI buffer at the back buffer's size. The
//    HUD-less colour is the present pass run a second time over the frame layers' hudless image
//    (or HDR stage 3's float scene), so it has the back buffer's own colour space and post-processing
//    (the game's gamma table, FSR1, the HDR10 output step); the UI buffer is the frame layers' UI
//    fitted the same way (dlssg_ui_ps.hlsl: premultiplied colour and coverage, nothing where there is
//    no UI). The ImGui overlay is drawn into the back buffer only, so it is interpolated with the
//    picture: occasional, accepted.
//  - It is off (slDLSSGSetOptions mode off, nothing tagged, the plugin keeps its memory) in every frame
//    that is not a rendered game scene: the pause menu, the map, menus, loading and movies (no anchor,
//    no vectors, frame layers not valid), an idle present, a frame with no latency id or before Reflex
//    runs, the HDR debug views, a back buffer under the plugin's smallest size. Around a resize, a
//    fullscreen change or any other remake of the swap chain it is switched off altogether (the chain
//    made plain, frame generation on again 0.6 s after the last change): the guide forbids window
//    operations while it is on.
//  - It never runs without the temporal system's vectors and the frame layers: both are asked for
//    while the setting is on (RequestVectorsForDlssFrameGeneration, FrameLayers::
//    kRequesterDlssFrameGeneration), with or without an upscaler (with DLSS Super Resolution the two
//    share the frame token and the camera; Super Resolution's frame index is the latency id while the
//    proxy chain is in use).
//
// The frame (Renderer::RecordPresentPass, render thread): after the real frame's present pass, Record
// makes the HUD-less image and the UI image, leaves the four tagged images (depth, vectors, HUD-less,
// UI) in the compute read state (D3D12 NON_PIXEL_SHADER_RESOURCE, Vulkan SHADER_READ_ONLY_OPTIMAL: what
// the tags say), sets the options when they changed and the constants and tags; the renderer then
// submits and presents, with the latency reducer's present markers around Present as always.
//
// The frame cap counts rendered frames, as FSR's does (mcla_gpu_frame_generation_fps_limit; HALF
// DISPLAY is the display's rate over the multiplier). Vertical sync is the player's own where the
// plugin says it supports it (D3D12), else off.
//
// Threading: the render thread for everything; Initialize and Shutdown on the lifecycle thread.
//
// CHECK (not run: nothing here was built; each point is settled by a run on an RTX 5080 at 240 Hz
// with the D3D12 debug layer and with Vulkan validation on, DLSS-G 2x, 3x and 4x, the pause menu,
// HDR on, the log and Streamline's sl.log):
//  - it compiles on both backends and with the files of an older Streamline folder (without the
//    frame generation headers the module is a stub: LARECOMP_HAS_DLSS_G);
//  - D3D12: the swap chain made through Streamline's proxy factory with a native ID3D12CommandQueue
//    (the interposer says "Detected base interface 'ID3D12CommandQueue' while expecting SL proxy": a
//    warning only, it takes the queue as it is; if frame generation then does not generate, the queue
//    and the device would have to be wrapped as well: Plume's upgradeDevice hook, which cannot run
//    before Streamline has started), sl.log naming DLSS-G's present hooks, "Upgrading IDXGISwapChain"
//    never logged for the proxy chain, no second wrapper over it;
//  - the swap chain's first frames after the switch: nothing is tagged until Reflex has made a few
//    sleeps (latency::StreamlineRunning), DLSSGState.status stays eOk, numFramesActuallyPresented is
//    2 (3, 4) once it generates;
//  - D3D12 SetMaximumFrameLatency: Plume sets the swap chain's latency after it is made, which
//    overwrites the one Streamline raised ("last writer wins"); the proxy chain is made with the
//    buffer count as its latency (SwapChain::MakeChain) to leave room for the generated presents;
//  - the four tagged images' states at the present (NativeImageOf kComputeRead) against the debug
//    layer, and that eValidUntilPresent holds with the next frame's work on the same queue (the
//    default queue parallelism mode blocks the presenting queue until the plugin is done with them);
//    if a flicker or a validation error shows the images were overwritten early, tag with
//    eOnlyValidNow (Streamline copies them, costs memory) in temporal_dlss.cpp TagFrameGeneration;
//  - the HUD-less colour equals the back buffer where there is no UI (a screenshot difference of the
//    two images), the UI buffer is zero there and the picture's UI where there is, in SDR and HDR10
//    (the table applied to premultiplied colour is only near enough on partly covered pixels);
//    DLSSGOptions::enableUserInterfaceRecomposition stays off;
//  - HDR: the proxy chain is R10G10B10A2 in HDR10 (the plugin's guide: UINT10 HDR10 only; not scRGB);
//    an HDR switch while it runs makes the proxy chain again (SwapChain::Recreate through the same
//    wrapper), the metadata is sent again;
//  - Vulkan: the swap chain made while the plugin is loaded is Streamline's (the interposer's
//    vkCreateSwapchainKHR), the acquire and present semaphores as the guide's section 16 says, the
//    queue the plugin asks Plume for (extra graphics, compute and optical flow queues added by its
//    vkCreateDevice), vertical sync off (FIFO is not supported with frame generation on Vulkan), the
//    device made through Streamline only when frame generation was saved at the start (otherwise the
//    row says RESTART);
//  - the multiplier: 3x and 4x on an RTX 50 (numFramesToGenerateMax), the frame limiter's number of
//    SHOWN frames, HALF DISPLAY at the multiplier, and that Reflex's pacing keeps up at 4x;
//  - the switch's cost: slSetFeatureLoaded loads the plugin and its network at the switch on (a
//    pause of the picture, logged), the idle wait and the chain made twice around a resize;
//  - frame time cost (the two window-size passes and their images: 8 bytes a pixel more video memory;
//    held, reported with the other held memory), against the plugin's own 1.5 to 5 ms;
//  - the shutdown order: the proxy chain is destroyed before Streamline is shut down
//    (DlssFrameGeneration::Shutdown), no object left alive in the debug layer's report;
//  - (fix/dlssg-markers, from the first runs, 2026-11-07) D3D12: Reflex through Streamline now waits for
//    Streamline's lock instead of giving up (the DLSS feature's 8 s creation held it, and three refused
//    calls had turned Reflex off for the session: every present was "not marked"). The log says
//    "low latency: NVIDIA Reflex (Streamline) is busy with something that takes seconds ..." at the
//    switch, "... (Streamline) on" some seconds later (when the creation ends), "generating: Nx"
//    after it, and the summary counts frames generated (D3D12 at 2x and 4x: numFramesActuallyPresented
//    follows the multiplier, no "Streamline's status says it does not generate");
//  - Vulkan (same fix): the validation layer's THREADING ERRORs between vkLatencySleepNV /
//    vkSetLatencyMarkerNV and the pacer's vkQueuePresentKHR are Streamline's (it makes both calls, with a
//    swap chain it picked, from sl.reflex and its pacer thread): they stay, a few a run, and no game
//    call is named in them. The one about the game's own command buffer ("expects PRESENT_SRC_KHR,
//    current GENERAL" on a "fake-swapchain-buffer") is gone (backend::ForgetImageContents); the ones
//    about "nv.sl.dlss_g.*" command buffers and images are Streamline's own, and so is the repeated
//    present-semaphore one;
//  - Vulkan: the 5 s watchdog stall at the first scene (the render thread waited for the end of the
//    DLSS feature's creation) was dlss::VulkanInstanceProcAddr(), which Publish and Poll call at every
//    frame boundary on Vulkan only and which took Streamline's lock (Runtime::mu) that the worker holds
//    for the 8 s of the creation: it is a plain atomic read now. Run next7-vk-dlssg: no idle, no GPU
//    wait, 8.36 s of unmeasured thread time in the frame, the guest's present waiting 8.35 s, the
//    render thread's next record 41 ms after "the feature was made"; D3D12 never calls it, and the
//    Vulkan runs before the frame generation code had no stall. CHECK: a Vulkan start with DLSS
//    (frame generation or not) has no "[Watchdog] frame stalled" at the first scene and frames at the
//    scene's pace while "the feature ... was made in the background" is still to come.
//  - Switching off (the setting) tells the plugin off and gives it kWindDownFrames presents before the
//    swap chain is made plain and the plugin unloaded (Poll, WoundDown), then frees its resources
//    (temporal_dlss.cpp LoadFrameGeneration): in the owner's play session (2026-11-07, 19:35:47) the
//    plugin was unloaded still generating, and the game then held exactly 119.8 presents a second
//    for minutes with every cap of ours cleared. The log says 5 s after the switch what the rate is.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "host_passes.h"
#include "textures.h"

namespace mcla::gpu {

class Device;
class FrameLayers;
class FrameRing;
class PassTiming;
class PresentPass;
class SwapChain;
class TemporalSystem;

class DlssFrameGeneration {
 public:
  // The generated frames a rendered one gets at most (the multiplier row's top): 2x, 3x, 4x.
  static constexpr uint32_t kMaxMultiplier = 4;
  // Guest frames in a row with nothing generated before the window-size images are released.
  static constexpr uint32_t kReleaseAfterFrames = 1200;
  // After a resize or any other remake of the swap chain, frame generation waits this long for the
  // window to settle before it is switched on again.
  static constexpr std::chrono::milliseconds kSettle{600};
  // Switching off from the setting: the plugin is first told to stop generating and given this many
  // presents to act on it (it tidies its pacing and flip state in its present, not in the options
  // call), at most kWindDownMax, before the swap chain is made plain and the plugin unloaded.
  // A switch off for a window change (Renderer::RebuildSwapChain) is not wound down: the chain has to
  // change at once.
  // CHECK: whether a pace that outlasts the plugin shows after such a switch too (the "5 s after
  // switching off" line is not logged for it); if so the wind-down needs to run before the rebuild.
  static constexpr uint32_t kWindDownFrames = 3;
  static constexpr std::chrono::milliseconds kWindDownMax{1500};

  DlssFrameGeneration();
  ~DlssFrameGeneration();
  DlssFrameGeneration(const DlssFrameGeneration&) = delete;
  DlssFrameGeneration& operator=(const DlssFrameGeneration&) = delete;

  // After the swap chain, the passes, the present pass, the frame layers and the temporal system
  // exist (the lifecycle thread, before the render thread starts). Never fails the renderer; the
  // setting is simply not offered where nothing can run.
  void Initialize(Device& device, FrameRing& frames, HostPasses& passes, PassTiming& pass_timing,
                  FrameLayers& layers, TemporalSystem& temporal, PresentPass& present,
                  SwapChain& swap_chain);
  // GPU idle, before the temporal system and Streamline shut down: frame generation off, the proxy
  // swap chain destroyed (a Streamline plugin must not outlive the chain it owns), the plugin unloaded.
  void Shutdown(SwapChain& swap_chain);

  // ---- the setting, at a frame boundary (no list open) --------------------------------------
  enum class Change : uint8_t { kNone, kOn, kOff };
  // Reads the setting and what Streamline says, publishes both (the menu reads them), asks the
  // temporal system for the vectors and the frame layers for their layers while the setting is on
  // and the plugin can run, and says which way the renderer should switch the swap chain.
  Change Poll();
  // kOn, before the swap chain is made again: the plugin is loaded (a pause: it starts its network)
  // and the swap chain is told to be Streamline's. kWait: Streamline is busy with a feature, ask again
  // at the next frame; kFailed: frame generation is off for the session (logged).
  enum class Prepared : uint8_t { kReady, kWait, kFailed };
  Prepared PrepareSwitchOn();
  // kOff, between the old swap chain's destruction and the plain one's creation: the plugin is
  // unloaded (the chain made after is not its proxy).
  void BeforePlainChain();
  // After the swap chain was made again (`ok`: a chain exists on the queue asked for): kOn starts
  // generating (the first frames are real ones until the latency reducer is Streamline's and has
  // made its sleeps), kOff releases what frame generation held; a failure leaves it off for the
  // session. `resize`: the switch off was made for a window change, and frame generation comes back
  // by itself once the window has been still for kSettle.
  void CompleteSwitch(Change change, bool ok, bool resize);
  // The swap chain was made again for another reason (an HDR switch): the window-size images follow
  // the new format and size (made again at the next frame).
  void OnSwapChainChanged();
  // True while the plugin is being taken off generation before the switch off (Poll).
  bool winding_down() const { return winding_down_; }
  // Vertical sync: the player's own where the plugin supports it, else off.
  bool WantsVsync() const;

  // True while frame generation runs: the swap chain is Streamline's proxy chain and the frame is
  // recorded through Record.
  bool active() const { return active_; }

  // ---- per frame, on the render thread -----------------------------------------------------
  // What Record needs of the present about to be recorded (as FrameGeneration::GenerateArgs).
  struct FrameArgs {
    bool guest_frame = false;      // a guest present (not an idle one)
    TextureKey front;              // the front buffer's key (PresentPass::front_key)
    uint32_t front_width = 0;      // ... and the size the present reads of it, host pixels
    uint32_t front_height = 0;
    uint32_t window_width = 0;     // the swap chain's size
    uint32_t window_height = 0;
    uint32_t back_buffers = 0;     // its image count
    bool hdr = false;              // an HDR10 chain
  };
  // Outside a render pass, after the real frame's present pass was recorded (the present pass holds
  // nothing for the frame after it): when this frame can be generated, makes the HUD-less image and the
  // UI image, leaves the four tagged images in the compute read state and tags them with the frame's
  // constants, and tells Streamline to generate; otherwise tells it not to. Returns whether this frame
  // is generated.
  bool Record(plume::RenderCommandList* list, const FrameArgs& args);
  // After the frame was presented: the plugin's state is read now and then; a failure it names
  // switches frame generation off (Poll).
  void AfterPresent();

  // Video memory frame generation holds now (the two window-size images; the plugin's own memory
  // is Streamline's and is reported by the plugin's state when asked).
  uint64_t HeldBytes() const;
  std::string TakeSummary();

 private:
  struct Options {
    bool on = false;
    uint32_t frames = 0;
    uint32_t back[2] = {};
    uint32_t render[2] = {};
    uint32_t buffers = 0;
    bool operator==(const Options& o) const {
      return on == o.on && frames == o.frames && back[0] == o.back[0] && back[1] == o.back[1] &&
             render[0] == o.render[0] && render[1] == o.render[1] && buffers == o.buffers;
    }
  };

  bool SettingIsDlss() const;
  bool Candidate() const;       // this PC may run it: an NVIDIA RTX 40 or 50 by name, the files there
  void Publish();
  void ReadDisplayRefresh();
  uint32_t Multiplier() const;  // the setting clamped to what the plugin says
  bool MakePasses(bool hdr);
  bool EnsureImages(uint32_t width, uint32_t height, plume::RenderFormat format);
  void ReleaseImages(const char* why);
  // Tells the plugin to generate (`on`) or not, when that changed; false when it could not be told
  // (Streamline busy making a feature: the frame goes without it).
  bool ApplyOptions(bool on, const FrameArgs& args, uint32_t render_w, uint32_t render_h);
  bool NotGenerated(uint64_t& counter, const FrameArgs& args);
  // Poll, with frame generation running and the setting off (or the plugin unable to run): true when
  // the plugin has been told off and has presented since, so the swap chain can be made plain.
  bool WoundDown();
  // Record, while winding down: the plugin is told off and the presents after that are counted.
  bool WindDownFrame(const FrameArgs& args);
  // Poll, after a switch off: says once what the game's frame rate is five seconds later and which
  // frame caps are in effect, so a pace that outlasts frame generation is told from a cap of ours.
  void ProbeAfterOff();
  void Fail(const std::string& why);
  void NoteState();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  FrameLayers* layers_ = nullptr;
  TemporalSystem* temporal_ = nullptr;
  PresentPass* present_ = nullptr;
  SwapChain* swap_chain_ = nullptr;
  bool initialized_ = false;
  bool vulkan_ = false;
  std::string gpu_name_;
  uint32_t vendor_ = 0;
  bool files_ok_ = false;         // the frame generation files are next to the exe
  std::string files_missing_;
  uint32_t min_size_ = 0;         // the plugin's smallest back buffer width or height (0: not known)
  uint64_t last_frame_ = ~uint64_t(0);  // the guest frame of the last one generated

  // The setting's state (render thread).
  bool active_ = false;
  bool failed_ = false;           // it could not be switched on, or failed: off for the session
  bool wanted_ = false;           // the last read: chosen, supported, nothing in the way
  bool plugin_loaded_ = false;    // PrepareSwitchOn loaded it and BeforePlainChain has not unloaded it
  bool unload_pending_ = false;   // the plugin could not be unloaded at the switch off: tried again
  bool winding_down_ = false;     // the setting went off: the plugin is told off before the switch
  std::chrono::steady_clock::time_point wind_began_{};
  uint32_t wind_frames_ = 0;      // presents since the plugin was told off, while winding down
  bool wind_timeout_logged_ = false;
  std::chrono::steady_clock::time_point off_at_{};  // when the last switch off completed
  uint64_t off_polls_ = 0;        // frame boundaries since then (about one a guest frame)
  bool probe_pending_ = false;
  bool logged_unavailable_ = false;
  std::chrono::steady_clock::time_point settle_until_{};
  uint32_t refresh_hz_ = 0;
  std::chrono::steady_clock::time_point refresh_read_{};

  // What Streamline has (render thread): the last options told, and the count of frames since it
  // was told on (the first is a reset).
  Options applied_;
  bool options_known_ = false;
  uint32_t frames_on_ = 0;
  uint32_t status_failures_ = 0;
  uint32_t frames_since_state_ = 0;
  uint32_t idle_guest_frames_ = 0;

  // The passes and the two window-size images (made on the first switch on).
  std::unique_ptr<plume::RenderShader> ui_shader_;
  std::unique_ptr<plume::RenderPipeline> ui_pipeline_;
  bool ui_hdr_ = false;
  HostImage hudless_;  // the swap chain's format: the picture without the UI, as the back buffer has it
  HostImage ui_;       // R8G8B8A8_UNORM: the UI, premultiplied colour and coverage, back buffer terms

  struct Stats {
    uint64_t presents = 0;
    uint64_t generated = 0;
    uint64_t not_idle = 0;          // an idle present
    uint64_t not_unmarked = 0;      // Reflex runs, and the frame had no latency id (not a game frame)
    uint64_t not_reflex = 0;        // Reflex through Streamline was not running yet (no frame has
                                    // markers before: the first seconds, or while it waits for Streamline)
    uint64_t not_scene = 0;         // no anchor or no vectors: not a rendered game scene
    uint64_t not_layers = 0;        // the frame layers were not the picture's, or not valid
    uint64_t not_busy = 0;          // Streamline was making a feature
    uint64_t not_small = 0;         // the back buffer is under the plugin's smallest size
    uint64_t not_failed = 0;        // a call failed
    uint64_t not_winding = 0;       // the setting went off: being taken off generation
    uint64_t option_changes = 0;
  };
  Stats stats_;
};

}  // namespace mcla::gpu

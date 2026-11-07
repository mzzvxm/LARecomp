#pragma once
// ===========================================================================
// MCLA GPU renderer -- the DLSS Super Resolution backend (F4 step 6, F4-DESIGN.md sections 7, 14, 15)
// ===========================================================================
// NVIDIA DLSS Super Resolution, and DLAA at ratio 1, through NVIDIA Streamline (the SDK's
// sl.interposer.dll, sl.common.dll, sl.dlss.dll and nvngx_dlss.dll). Compiled only when CMake found
// the Streamline headers (LARECOMP_HAS_DLSS, cmake/FetchStreamline.cmake: fetched into the build
// folder and checked against their SHA-256, never committed); otherwise MakeDlssBackend() returns
// null and nothing of it exists. Windows only.
//
// Nothing is linked. sl.interposer.dll is opened at run time by its FULL PATH in the folder of the
// exe (LoadLibraryExW with the DLL's own folder and the system folder as the only places its
// dependencies are looked for) after Streamline's own signature check
// (sl::security::verifyEmbeddedSignature: WinVerifyTrust, and the signer's key against NVIDIA's) of
// the three sl.*.dll files, and its functions are taken with GetProcAddress, so both toolchains
// (Clang for MSVC and llvm-mingw) build it without an import library. Production DLLs only.
//
// How Streamline is hooked in (the choice that keeps FSR and the game without DLSS untouched):
//
//   D3D12   Super Resolution needs none of Streamline's proxies for the device, the factory or
//           the queues: the manual hooking guide (docs/ProgrammingGuideManualHooking.md,
//           "slSetD3DDevice": "When using D3D simply create your device and call slSetD3DDevice")
//           gives the interposer the native device. slInit with eUseManualHooking runs after the
//           device exists, on the probe's worker thread: at once when DLSS is the saved choice,
//           else in the background a few seconds after the game first draws (the pre-warm below)
//           or at its first use, whichever comes first, so DLSS can be switched on while the game
//           runs. The swap chain does get
//           Streamline's wrapper (slUpgradeInterface), which is how its common plugin sees the
//           presents (presentCommon: its garbage collection and the clearing of its caches at a
//           resize); it goes in at the first present after Streamline has finished starting, not at
//           the swap chain's creation (UpgradeSwapChain below).
//   Vulkan  The instance and the device have to be made with the extensions, features and queues
//           Streamline asks for, so on Vulkan it starts BEFORE the device (Device::Open ->
//           dlss::BeforeDevice), and only when DLSS is the saved choice at the start (an NVIDIA
//           GPU, the files in place and signed). Plume loads every Vulkan function through the
//           interposer's vkGetInstanceProcAddr (Plume's VulkanInterfaceHooks): its vkCreateInstance
//           and vkCreateDevice proxies add what DLSS needs, so slSetVulkanInfo is not used. That
//           costs several seconds at every start whether DLSS is used or not (measured on an RTX
//           5080: the device up 9.7 s after the process started, not 0.5 s: slInit 1.6 s and NGX
//           7.7 s inside the hooked device creation, on the game's thread), so a Vulkan game
//           started with FSR or no upscaler does not pay it, and choosing DLSS from its pause menu
//           applies from the next start: the row lists DLSS as "after restart" (UPSCALING says
//           RESTART and the "Restart now?" prompt offers it), the renderer goes on without it, and
//           there is no pre-warm. If the hooked instance or device cannot be made, the game starts
//           again without it (Device): DLSS is off for the session.
//
// Per frame (Upscale, on the render thread, inside the frame's list and outside a render pass):
// slGetNewFrameToken, slSetConstants (matrices, jitter, vector scale, reset), then slEvaluateFeature
// with the viewport and four local resource tags (depth, vectors, colour in, colour out) with
// Plume's native image states; the caller then forgets the list's cached state
// (backend.h InvalidateAfterExternalWork). The DLSS options (mode, output size, NVIDIA's default
// presets, HDR colour, automatic exposure) are set when a context is made.
//
// Threads: see temporal_backend.h. Every Streamline call is serialised by one mutex (its API is
// not thread safe). The worker makes everything that can take seconds, and the render thread never
// waits for it:
//   - the one-time start-up (load, signature check, slInit, device, support query: 2 s to over 10 s
//     on a cold start). The start decision does not wait for the end of it: once the files are in
//     place and signed, an RTX card counts as able to run DLSS (PublishEarlyVerdict); if the rest
//     fails, the log says so and what the player then sees (FailStart).
//   - every context (JobLoop, Build). DLSS makes its feature (NGX builds the network, 8.2 s on an
//     RTX 5080) inside the first slEvaluateFeature of a viewport, so Build makes that first
//     evaluation itself, once, on a command list, queue and fence of its own (SideKit: a second
//     direct queue made through Plume, so on Vulkan a submission shares a VkQueue only under
//     Plume's own queue mutex) over four images of the request's sizes (WarmImages, made by
//     Request() on the render thread), and waits for the fence. Request() only records what is
//     wanted; Ready() is false until the worker has published the context, and the frames in
//     between are plain, as for FSR's context. A live change of mode or upscaler is made before the
//     old one is broken: Prepare() asks for the new context in a second slot (the same worker, one
//     build at a time, a viewport of its own), the old context goes on dispatching, and Commit()
//     swaps them between two frames once PreparedReady() says the feature exists. Streamline's
//     API is serialised by that one mutex (slEvaluateFeature is not thread safe), and the worker
//     holds it for the whole of the feature's first evaluation, so the old DLSS context cannot be
//     dispatched meanwhile: Busy() says so, the frames are plain and not jittered at the old scale
//     until the feature exists (a switch from FSR, from no upscaler, or to FSR or off never meets
//     this: only DLSS to DLSS does), and the render thread never waits for the mutex while a
//     feature is being made (Upscale tries it first; a short hold, a context's options or the
//     freeing of an old one, is waited for). A request that changes while the worker is inside
//     Streamline is not cancelled (nothing can interrupt it): the context it makes is freed when it
//     is done, and the new one is made next. Freeing a context (slFreeResources) is also the
//     worker's, after the frames that may use it (FrameRing::RetireAction). Shutdown() waits for
//     the worker, so a game closed in the first seconds of a creation waits for the rest of it.
//   - which modes DLSS takes at the output size (AskModes, Modes): DLSS gives each mode one render
//     size for an output size, and the scene is drawn on the scale grid, 0.5x of the guest's 1280x720
//     at the least, so a mode can be unable to run at an output (Ultra Performance at 1280x720 and at
//     2560x1440). Once Streamline runs the worker asks slDLSSGetOptimalSettings for the five modes at
//     the sizes the caller works at (a size query: no feature, no video memory) and keeps the answer
//     per sizes; a context that is refused for its sizes adds the same record. That is a refusal and
//     not a failure: no context is made for the mode (Request and Prepare return at once for it),
//     DLSS stays available, and the caller goes on with another mode (temporal.cpp ResolveDlssMode).
//     The modes refused at the caller's current sizes go to the pause menu (gpu.h
//     UpscalerAvailability::dlss_modes_refused). Only a real failure of a context (slDLSSSetOptions,
//     the query itself, the feature) turns DLSS off for the session.
//   - without a side kit or warm-up images (or if the warm-up fails) the context is published
//     anyway and the first dispatch makes the feature on the render thread, with the pause.
//   - the pre-warm (temporal_backend.h Prewarm): on D3D12, on a PC that can run DLSS whose game was
//     not started with it (a Vulkan game has no live switch to DLSS, so no pre-warm), the worker
//     makes one context per mode (the saved mode first) at the real sizes a few
//     seconds after the game first draws, each with the same first evaluation, and frees it at once:
//     nothing is kept in video memory. On D3D12 it begins with Streamline's own start (slInit and
//     NGX, about 10 s, on the worker). What the runs measured (RTX 5080): the first feature of a
//     process 1.2 s (Performance) to 8.4 s (DLAA, and Quality on Vulkan), every later one 25 to
//     30 ms even after all earlier ones were freed, so the cost is NGX's and the driver's first-time
//     work, once per kind of feature, and paying it with a transient feature leaves later switches
//     at about 30 ms. A DLSS request (Request, Prepare) ends the pre-warm after the step in hand,
//     and the context asked for is the worker's next job; a step is skipped when video memory is
//     nearly full. When DLSS is the start's choice there is no pre-warm (the context made at start
//     paid for its kind, and further steps would hold Streamline's mutex while it dispatches).
//
// Streamline's log: every line goes to sl.log in the logs folder (InitLocked); only its warnings
// and errors reach the game's log.
//
// DLSS Frame Generation and Reflex through Streamline (dlss_frame_generation.h, latency_streamline.cpp)
// are the second half of this file's Streamline services, behind the neutral functions at the end of
// this header (no Streamline type in them). Streamline's start asks for the plugins of frame
// generation, Reflex and the PC latency markers where they may be used (InitLocked: DLSS frame
// generation chosen, or on D3D12 an RTX 40 or 50 series card, since plugins cannot be added later),
// asks whether the feature is supported and what it can do (QueryFrameGenerationLocked: the most
// generated frames, the smallest size), and unloads the frame generation plugin again: while it is
// loaded Streamline owns the swap chain Plume makes, so it is loaded only while frame generation
// runs, with the swap chain made again each way (LoadFrameGeneration, the factory's proxy).
//
// Beside Reflex (latency.h) and HDR output (hdr.h), which use Plume's own paths unless frame
// generation runs: while it does the reducer is Streamline's (sl.reflex, sl.pcl: slReflexSleep and
// the PC latency markers with the same frame token as the constants and the tags, which DLSS-G
// requires) and the NvAPI / VK_NV_low_latency2 one is put aside; otherwise Streamline makes no
// latency calls of its own, and the reducer's calls (NvAPI on the native D3D12 device, which is never
// wrapped; VK_NV_low_latency2 on Vulkan) are the only ones. The scene colour DLSS reads and writes is the
// same linear scene-referred picture FSR gets, so the HDR float scene (hdr_tonemap.h) takes the
// output image as the tone map's input here exactly as it does under FSR; mcla_gpu_aa is held off
// by TemporalSystem::HoldsAaOff for either.
// CHECK: Reflex or Anti-Lag 2 with DLSS on one device, both APIs. Vulkan: Plume enables the
// reducers' extensions and feature structs itself (VK_NV_low_latency2 with VkPhysicalDevice
// PresentIdFeaturesKHR and TimelineSemaphoreFeatures, VK_AMD_anti_lag), the interposer's
// vkCreateDevice adds its own: look for a duplicate extension name or a feature struct twice
// (validation, sl.log), the swap chain made through the interposer with the latency mode in its
// pNext and the HDR10 colour space (R10G10B10A2), the presents with VkPresentIdKHR and
// VkLatencySubmissionPresentIdNV through the interposer's vkQueuePresentKHR, and the lock the
// swap chain's Present/Acquire hold (swap_chain.cpp) with Streamline's own calls. D3D12: the HDR10
// colour space is applied on the native swap chain at creation; after the wrapper goes in,
// resizes and SetHdrMetadata reach it through the wrapper (QueryInterface for IDXGISwapChain4,
// SetColorSpace1, SetHDRMetaData, ResizeBuffers).
// ===========================================================================

#include <cctype>
#include <cstdint>
#include <memory>
#include <string>

#include "backend.h"
#include "temporal_backend.h"

namespace mcla::gpu {

class DlssBackend final : public TemporalBackend {
 public:
  DlssBackend();
  ~DlssBackend() override;

  const char* Name() const override { return "DLSS (Streamline)"; }
  void Probe(Device& device, FrameRing& frames) override;
  BackendStatus Status() const override;
  void Request(const BackendRequest& request) override;
  bool Ready(const BackendRequest& request) const override;
  uint32_t JitterPhaseCount() const override { return 0; }  // 8 x ratio^2, as NVIDIA recommends
  bool Busy() const override;
  void AskModes(const ModeSizes& sizes) override;
  ModeVerdicts Modes(const ModeSizes& sizes) const override;
  void Prepare(const BackendRequest& next) override;
  bool PreparedReady(const BackendRequest& next) const override;
  bool Commit(const BackendRequest& next) override;
  void CancelPrepared() override;
  void Release() override;
  void Fail(const std::string& reason) override;
  bool PrewarmPending() const override;
  bool PrewarmNext(UpscaleMode* mode) override;
  void Prewarm(const BackendRequest& request) override;
  bool Upscale(plume::RenderCommandList* list, const TemporalInputs& inputs) override;
  void Shutdown() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Before the graphics interface exists (Device::Open). Vulkan only: when DLSS is the saved choice,
// this build can run it and the GPU is an NVIDIA one (the files in place and signed), starts
// Streamline now so that the interface is made through its vkGetInstanceProcAddr. With another
// upscaler or none chosen it does nothing (the start would take seconds longer for DLSS that is not
// used; DLSS chosen later applies from the next start). A no-op for D3D12 (Streamline starts after
// the device there, and DLSS switches on live) and for a build without DLSS.
#if defined(LARECOMP_HAS_DLSS)
namespace dlss {
void BeforeDevice(Backend backend, const std::string& preferred_adapter);
// A PFN_vkGetInstanceProcAddr in a void*: sl.interposer.dll's, when BeforeDevice started
// Streamline for this device; null otherwise (Plume then uses the Vulkan loader's).
void* VulkanInstanceProcAddr();
// The Vulkan interface or device could not be made with the hook: DLSS is off for the rest of the
// process and the hook is not offered again (the caller tries again without it).
void AbandonVulkanHook(const std::string& why);

// D3D12, from the thread that presents (backend_d3d12.cpp, Plume's upgradeSwapChainAtPresent hook,
// before every present). SwapChainUpgradePending is a plain atomic read: true from the end of
// Streamline's start until the swap chain was handed to it. UpgradeSwapChain then replaces
// `*swap_chain` (the IDXGISwapChain Plume presents on) with Streamline's wrapper of it and returns
// true; the wrapper holds a reference of its own to the original, so the caller releases the
// original's. It returns false, with `*swap_chain` untouched, when it is not time yet (Streamline
// is busy making a feature: asked again at the next present), when Streamline would not take it
// (logged), and always after the first time it took it or refused.
bool SwapChainUpgradePending();
bool UpgradeSwapChain(void** swap_chain);
// Render thread, after the swap chain was made again (the HDR switch: SwapChain::Recreate destroys
// the chain, wrapper included, and makes a plain one). Once Streamline has been offered a chain
// the new one is offered at the next present as the first was; before that nothing is needed.
void SwapChainRecreated();
}  // namespace dlss
#else
namespace dlss {
inline void BeforeDevice(Backend, const std::string&) {}
inline void* VulkanInstanceProcAddr() { return nullptr; }
inline void AbandonVulkanHook(const std::string&) {}
inline bool SwapChainUpgradePending() { return false; }
inline bool UpgradeSwapChain(void**) { return false; }
inline void SwapChainRecreated() {}
}  // namespace dlss
#endif

// ---------------------------------------------------------------------------
// DLSS Frame Generation and Reflex through Streamline: the neutral functions
// ---------------------------------------------------------------------------
// What dlss_frame_generation.cpp and latency_streamline.cpp call (no Streamline type in any of them;
// the bodies are in temporal_dlss.cpp, or inline stubs where the build has no Streamline or no
// frame generation plugins). Streamline's API is not thread safe: every call takes its one lock
// (Runtime::mu), except Reflex's sleep and the PC latency markers (thread safe in Streamline, and
// they must not wait behind the seconds a DLSS feature takes to make). The render thread's calls
// never wait for a feature's creation: they say `busy` and are asked again at the next frame.
namespace dlss {

// By name only (the runtime's support query is the final word): a GeForce RTX 40 or 50 series card
// (and the workstation cards of those generations, "RTX 5000 Ada", "RTX PRO 6000 Blackwell"), the
// cards DLSS frame generation runs on. A card that looks like one and is not (Quadro RTX 4000, a
// Turing) is only asked.
inline bool NameLooksLikeFrameGeneration(const std::string& name) {
  std::string upper;
  for (char c : name) {
    upper += char(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper.find("BLACKWELL") != std::string::npos || upper.find(" ADA") != std::string::npos) {
    return true;
  }
  const size_t rtx = upper.find("RTX");
  if (rtx == std::string::npos) {
    return false;
  }
  size_t at = rtx + 3;
  while (at < upper.size() && upper[at] == ' ') {
    ++at;
  }
  return at < upper.size() && (upper[at] == '4' || upper[at] == '5');
}
// An RTX 50 series (Blackwell): the cards that take more than one generated frame (3x and 4x).
inline bool NameLooksLikeMultiFrameGeneration(const std::string& name) {
  std::string upper;
  for (char c : name) {
    upper += char(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper.find("BLACKWELL") != std::string::npos) {
    return true;
  }
  const size_t rtx = upper.find("RTX");
  if (rtx == std::string::npos) {
    return false;
  }
  size_t at = rtx + 3;
  while (at < upper.size() && upper[at] == ' ') {
    ++at;
  }
  return at < upper.size() && upper[at] == '5';
}

// What Streamline said about frame generation (any thread, never waits for a feature's creation).
struct FrameGenerationFacts {
  bool asked = false;      // Streamline was started with the frame generation, Reflex and PC latency plugins
  bool queried = false;    // the support question was asked: the start is over, the answer is final
  bool supported = false;  // this card, this driver, Windows and its graphics settings can run it
  uint32_t max_frames_generated = 0;  // slDLSSGGetState: 1 is 2x, 2 is 3x, 3 is 4x
  uint32_t min_size = 0;              // the smallest width or height it takes
  bool vsync_supported = false;
  bool dynamic_supported = false;     // dynamic multi frame generation (not used)
  std::string why;                    // not supported, or left out of the start: one plain sentence
};

// FrameGenerationFilesPresent: the files frame generation needs are next to the exe (nothing is
// loaded); `missing` lists the ones that are not there.

// The frame generation plugin loaded or unloaded (slSetFeatureLoaded), at a frame boundary on the
// render thread, with no swap chain of the game's alive when unloading and the chain made after
// loading (Streamline's own proxy chain exists only for a chain made while the plugin is loaded).
// kBusy: Streamline is making a feature, ask again.
enum class LoadResult : uint8_t { kDone, kBusy, kFailed };

// The size and the content of what is tagged for a frame, and the camera it was made with (Streamline's
// constants).
struct FrameGenerationInputs {
  uint32_t frame_index = 0;           // the latency frame's id: the constants, the tags and the markers share it
  uint32_t render_size[2] = {};       // the depth and the vectors (the scene's size)
  uint32_t back_size[2] = {};         // the back buffer, the HUD-less colour and the UI
  TemporalImage depth;                // R32_FLOAT, 1 near / 0 far
  TemporalImage motion;               // RG16_FLOAT, render pixels, previous minus current, +y down
  TemporalImage hudless;              // the scene as the back buffer has it, without the UI
  TemporalImage ui;                   // the UI, premultiplied colour and coverage in alpha
  float jitter[2] = {};               // render pixels, +y down: what the frame's draws were moved by
  bool reset = false;                 // the history starts again with this frame
  float camera_near = 0.1f;
  float camera_far = 1000.0f;
  float camera_fov_y = 1.0f;          // radians
  float camera_aspect = 1.0f;
  float view_to_clip[16] = {};        // row-major matrices of row vectors, no jitter (temporal.h StreamlineCamera)
  float clip_to_view[16] = {};
  float clip_to_prev_clip[16] = {};
  float prev_clip_to_clip[16] = {};
  float camera_position[3] = {};
  float camera_up[3] = {};
  float camera_right[3] = {};
  float camera_forward[3] = {};
};

struct FrameGenerationOptions {
  bool on = false;                    // generate: else the plugin passes the frames through (and keeps its memory)
  uint32_t frames_to_generate = 1;    // 1 is 2x, 2 is 3x, 3 is 4x
  uint32_t color_size[2] = {};        // the back buffer
  uint32_t mvec_depth_size[2] = {};   // the depth and the vectors
  uint32_t back_buffers = 0;
  bool request_vram_estimate = false; // slDLSSGGetState then fills vram_bytes (not every frame: it is costly)
};

// slDLSSGGetState, for the checks and the log.
struct FrameGenerationState {
  bool ok = false;                    // the call worked
  uint32_t frames_presented = 0;      // per application frame since the last call (2 at 2x)
  uint32_t max_frames_generated = 0;
  uint32_t min_size = 0;
  uint64_t vram_bytes = 0;
  // sl::DLSSGStatus, split: why Streamline would not generate.
  bool resolution_too_low = false;
  bool reflex_missing = false;        // Reflex did not run with the frame's markers
  bool hdr_format_unsupported = false;
  bool constants_invalid = false;
  bool back_buffer_index_not_asked = false;
};

enum class ReflexMarker : uint8_t {
  kSimulationStart,
  kSimulationEnd,
  kRenderSubmitStart,
  kRenderSubmitEnd,
  kPresentStart,
  kPresentEnd,
  kInputSample,
};

#if defined(LARECOMP_HAS_DLSS)
FrameGenerationFacts GetFrameGenerationFacts();
bool FrameGenerationFilesPresent(std::string* missing);
LoadResult LoadFrameGeneration(bool loaded, std::string* why);
// D3D12: replaces `*factory` (Plume's native IDXGIFactory4, not given a reference) with Streamline's
// proxy of it when the frame generation plugin is loaded; the caller owns the proxy's one reference
// and releases it after the swap chain is made. False with `*factory` untouched otherwise.
bool UpgradeFactory(void** factory);
// The swap chain Plume presents on is, or is no longer, Streamline's own proxy chain: no wrapper of
// ours goes over it (SwapChainUpgradePending says false) and SwapChainRecreated waits.
void NoteSwapChainProxied(bool proxied);
bool SetFrameGenerationOptions(const FrameGenerationOptions& options, std::string* why, bool* busy);
// Records nothing itself: sets this frame's constants and the tags (slSetConstants,
// slSetTagForFrame with `native_list` as the command buffer the tags' states are given on). The
// four images must already be in the compute read state (backend::ImageUse::kComputeRead) and stay
// in it until the present.
bool TagFrameGeneration(void* native_list, const FrameGenerationInputs& inputs, std::string* why,
                        bool* busy);
bool ReadFrameGenerationState(FrameGenerationState* state, bool request_vram_estimate,
                              std::string* why, bool* busy);
// Frame generation is going: the plugin's resources go, then it is unloaded (the GPU idle, the
// swap chain's proxy gone or about to).
void ShutdownFrameGeneration();

namespace reflex {
// Streamline's Reflex and PC latency plugins can be called now (the start is over, frame generation
// is supported: they were asked for with it).
bool Available();
// slReflexSetOptions: on or off, with boost. False when Streamline refused it.
bool SetMode(bool on, bool boost);
// slReflexSleep for the frame: the wait before the frame starts. False when it could not be made.
bool Sleep(uint32_t frame);
// slPCLSetMarker with the frame's token.
void SetMarker(ReflexMarker marker, uint32_t frame);
}  // namespace reflex
#else
inline FrameGenerationFacts GetFrameGenerationFacts() {
  FrameGenerationFacts facts;
  facts.queried = true;
  facts.why = "this build has no DLSS";
  return facts;
}
inline bool FrameGenerationFilesPresent(std::string* missing) {
  if (missing) {
    *missing = "this build has no DLSS";
  }
  return false;
}
inline LoadResult LoadFrameGeneration(bool, std::string*) { return LoadResult::kFailed; }
inline bool UpgradeFactory(void**) { return false; }
inline void NoteSwapChainProxied(bool) {}
inline bool SetFrameGenerationOptions(const FrameGenerationOptions&, std::string*, bool*) { return false; }
inline bool TagFrameGeneration(void*, const FrameGenerationInputs&, std::string*, bool*) { return false; }
inline bool ReadFrameGenerationState(FrameGenerationState*, bool, std::string*, bool*) { return false; }
inline void ShutdownFrameGeneration() {}
namespace reflex {
inline bool Available() { return false; }
inline bool SetMode(bool, bool) { return false; }
inline bool Sleep(uint32_t) { return false; }
inline void SetMarker(ReflexMarker, uint32_t) {}
}  // namespace reflex
#endif

}  // namespace dlss

}  // namespace mcla::gpu

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
//           device exists, on the probe's worker thread, and only when DLSS is the saved choice
//           (or at its first use, when it is chosen from the pause menu). The swap chain does get
//           Streamline's wrapper (slUpgradeInterface), which is how its common plugin sees the
//           presents (presentCommon: its garbage collection and the clearing of its caches at a
//           resize); it goes in at the first present after Streamline has finished starting, not at
//           the swap chain's creation (UpgradeSwapChain below).
//   Vulkan  The instance and the device have to be made with the extensions, features and queues
//           Streamline asks for, so on Vulkan it starts BEFORE the device (Device::Open ->
//           dlss::BeforeDevice), only when DLSS is the saved choice at the start and the GPU is an
//           NVIDIA one, and Plume loads every Vulkan function through the interposer's
//           vkGetInstanceProcAddr (Plume's VulkanInterfaceHooks): its vkCreateInstance and
//           vkCreateDevice proxies add what DLSS needs, so slSetVulkanInfo is not used. A Vulkan
//           game that was not started with DLSS chosen lists it as "after restart". If the
//           hooked instance or device cannot be made, the game starts again without it (Device).
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
//   - without a side kit or warm-up images (or if the warm-up fails) the context is published
//     anyway and the first dispatch makes the feature on the render thread, with the pause.
//
// Streamline's log: every line goes to sl.log in the logs folder (InitLocked); only its warnings
// and errors reach the game's log.
//
// Beside Reflex (latency.h) and HDR output (hdr.h), which use Plume's own paths and not
// Streamline's: the Reflex plugin (sl.reflex) is not loaded, so Streamline makes no latency calls of
// its own; the reducer's calls (NvAPI on the native D3D12 device, which is never wrapped;
// VK_NV_low_latency2 on Vulkan) are the only ones. The scene colour DLSS reads and writes is the
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
  void Prepare(const BackendRequest& next) override;
  bool PreparedReady(const BackendRequest& next) const override;
  bool Commit(const BackendRequest& next) override;
  void CancelPrepared() override;
  void Release() override;
  void Fail(const std::string& reason) override;
  bool Upscale(plume::RenderCommandList* list, const TemporalInputs& inputs) override;
  void Shutdown() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Before the graphics interface exists (Device::Open). Vulkan only: when DLSS is the saved choice
// and this build and GPU can run it, starts Streamline now so that the interface is made through
// its vkGetInstanceProcAddr. A no-op for D3D12 (Streamline starts after the device there) and for a
// build without DLSS.
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
}  // namespace dlss
#else
namespace dlss {
inline void BeforeDevice(Backend, const std::string&) {}
inline void* VulkanInstanceProcAddr() { return nullptr; }
inline void AbandonVulkanHook(const std::string&) {}
inline bool SwapChainUpgradePending() { return false; }
inline bool UpgradeSwapChain(void**) { return false; }
}  // namespace dlss
#endif

}  // namespace mcla::gpu

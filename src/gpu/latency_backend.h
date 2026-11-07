#pragma once
// MCLA GPU renderer -- one vendor's latency reducer on the rendering device, as
// latency.cpp (the policy, the frame bookkeeping) asks for it. The halves that
// talk to a driver sit in latency_d3d12.cpp (NVAPI, the AMD driver extension)
// and latency_vulkan.cpp (VK_NV_low_latency2, VK_AMD_anti_lag), beside
// backend_d3d12.cpp / backend_vulkan.cpp, which is where code that reaches into
// Plume's backend objects lives. See latency.h.

#include <cstdint>
#include <memory>
#include <string>

#include "backend.h"

namespace plume {
struct RenderDevice;
struct RenderSwapChain;
}  // namespace plume

namespace mcla::gpu::latency {

// The points of a frame the driver is told about.
enum class Marker : uint8_t {
  kSimulationStart,
  kSimulationEnd,
  kRenderSubmitStart,
  kRenderSubmitEnd,
  kPresentStart,
  kPresentEnd,
  kInputSample,
};

// This is the one place the vendors' calls (sleep mode, sleep, markers) sit behind: latency.cpp and
// the call sites in renderer.cpp / hooks/frame_timing.cpp only use Reducer and latency.h's free
// functions. DLSS frame generation needs Reflex to run through NVIDIA Streamline (sl.reflex, sl.pcl)
// instead of NvAPI directly: that is a third Reducer (latency_streamline.cpp), taken over from the
// device's own while frame generation runs (latency.h UseStreamline).
//
// Calls may come from several threads at once (the update thread sleeps and marks, the guest
// render thread and the host render thread mark); an implementation takes no lock across a
// driver wait, and survives Shutdown while a call is in progress.
class Reducer {
 public:
  virtual ~Reducer() = default;
  // The feature's plain-text name, as the log says it: "NVIDIA Reflex", "AMD Anti-Lag 2".
  virtual const char* Name() const = 0;
  virtual bool IsReflex() const = 0;
  // Reflex's On + Boost (the GPU clocks stay high).
  virtual bool SupportsBoost() const { return false; }
  // What mcla_gpu_low_latency = auto comes to.
  virtual bool OnByDefault() const { return false; }

  // The mode: on or off, and boost. False when the driver refused it. The vendor's own frame
  // limit is left at none (latency.h: the frame rate limit). Called by the update thread only.
  virtual bool SetMode(bool on, bool boost) = 0;
  // The driver's wait at the start of a frame. True when it was made; false where it could not
  // be: no swap chain yet, a call that failed.
  virtual bool Sleep(uint64_t frame_id) = 0;
  virtual void SetMarker(Marker marker, uint64_t frame_id) = 0;

  // Vulkan: the swap chain's handle is about to change / has changed (render thread). Calls that
  // need the handle wait in between.
  virtual void BeginSwapChainChange() {}
  virtual void EndSwapChainChange() {}
  // Vulkan: between the two, when the chain is another Plume object, not the same one with a new
  // handle (the HDR switch makes it again: SwapChain::Recreate). `chain` may be null when no chain
  // could be made; the calls that name it then do nothing until a later change brings one.
  virtual void SwapChainReplaced(plume::RenderSwapChain* /*chain*/) {}
  // Vulkan: the render thread's present and acquire use the swap chain "externally synchronized"
  // (the spec), so the calls that take the chain (the sleep mode, the sleep, the markers) must not
  // overlap them; they wait in between. Nothing on D3D12.
  virtual void BeginSwapChainUse() {}
  virtual void EndSwapChainUse() {}
  // Releases what the vendor's API holds; after it every call does nothing.
  virtual void Shutdown() {}
};

// Reflex through Streamline's plugins (temporal_dlss.h dlss::reflex); null, with why, where they are
// not running. Built in every build: without DLSS it never finds Streamline.
std::shared_ptr<Reducer> CreateStreamlineReducer(std::string& why);

#if defined(LARECOMP_GPU_HAS_D3D12)
// Null where the device has no reducer, with why in plain words.
std::shared_ptr<Reducer> CreateD3D12Reducer(plume::RenderDevice* device, std::string& why);
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
// `swap_chain` is the renderer's own (it may still be empty: a window with no area yet).
std::shared_ptr<Reducer> CreateVulkanReducer(plume::RenderDevice* device,
                                             plume::RenderSwapChain* swap_chain,
                                             std::string& why);
#endif

}  // namespace mcla::gpu::latency

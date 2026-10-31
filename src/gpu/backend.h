#pragma once
// ===========================================================================
// MCLA GPU renderer -- the graphics backend this process runs
// ===========================================================================
// Every backend the platform has is built in (CMake LARECOMP_GPU_BACKENDS:
// D3D12 and Vulkan on Windows, Vulkan elsewhere) and one of them runs, picked
// at startup from the SDK's graphics_backend cvar -- the same cvar that picks
// the backend of the SDK's emulated GPU. Device::Initialize selects it before the interface exists, and it is
// fixed for the process from then on. The two things that differ per backend
// -- the host shader blob format and how constant buffers reach a guest
// shader (D3D12 root CBVs, Vulkan buffer device addresses in a push constant)
// -- are asked of ActiveBackend() where they are used.
//
// Everything here is backend-neutral to include. The code that has to reach
// past the Plume interface into a backend's own objects lives in exactly two
// translation units, backend_d3d12.cpp and backend_vulkan.cpp, behind the
// functions declared at the bottom (backend.cpp picks the active one). No other
// file under gpu/ includes plume_d3d12.h or plume_vulkan.h.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <plume_render_interface.h>

#if !defined(LARECOMP_GPU_HAS_D3D12) && !defined(LARECOMP_GPU_HAS_VULKAN)
#error "no backend: CMake LARECOMP_GPU_BACKENDS defines LARECOMP_GPU_HAS_D3D12 and/or _VULKAN"
#endif

namespace mcla::gpu {

enum class Backend : uint8_t { kD3D12, kVulkan };

// Whether this build carries `backend`.
constexpr bool BackendCompiled(Backend backend) {
#if defined(LARECOMP_GPU_HAS_D3D12)
  if (backend == Backend::kD3D12) {
    return true;
  }
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
  if (backend == Backend::kVulkan) {
    return true;
  }
#endif
  return false;
}

// What graphics_backend = "auto" runs: D3D12 where the build has it.
inline constexpr Backend kDefaultBackend =
    BackendCompiled(Backend::kD3D12) ? Backend::kD3D12 : Backend::kVulkan;

namespace detail {
// Written once, by SelectBackend on the UI thread before the render thread
// exists; only read after that.
inline Backend g_active_backend = kDefaultBackend;
}  // namespace detail

// The backend this process runs (kDefaultBackend before Device::Initialize).
inline Backend ActiveBackend() {
  return detail::g_active_backend;
}

// Device::Initialize only, before the interface exists, with a compiled backend.
void SelectBackend(Backend backend);

// "D3D12" / "Vulkan".
const char* BackendName(Backend backend);
inline const char* BackendName() {
  return BackendName(ActiveBackend());
}

// graphics_backend's text as a backend: "auto" is kDefaultBackend, "d3d12" and
// "vulkan" themselves. False for anything else, with kDefaultBackend in `out`.
bool ParseBackend(std::string_view text, Backend& out);

// The format of the active backend's shader blobs: DXIL on D3D12, SPIR-V on
// Vulkan.
inline plume::RenderShaderFormat HostShaderFormat() {
  return ActiveBackend() == Backend::kD3D12 ? plume::RenderShaderFormat::DXIL
                                            : plume::RenderShaderFormat::SPIRV;
}

// Plume's Vulkan swap chain creates its VkSwapchainKHR in resize(), so a fresh
// one is empty until resized; the D3D12 one creates it in the constructor.
// (plume_vulkan.cpp VulkanSwapChain, reblue gpu/device.cpp CreateHostDevice.)
inline bool SwapChainNeedsInitialResize() {
  return ActiveBackend() == Backend::kVulkan;
}

// Everything below acts on the active backend.
namespace backend {

// Enables the D3D12 debug layer before the interface exists. Plume only does
// it itself when its own translation unit is built without NDEBUG, which no
// RelWithDebInfo build is. Returns false, and says why, when the request cannot
// be honoured (Vulkan, or no SDK layers installed). With the Agility SDK the
// layers DLL must be the one from the same release, next to D3D12Core.dll in
// .\D3D12\; the system's copy belongs to the OS runtime.
bool EnableDebugLayer();

// After the device exists, with the debug layer on: routes the layer's
// warnings and errors into the log (ID3D12InfoQueue1 callback). Without it they
// only reach OutputDebugString, which nothing reads outside a debugger.
void InstallDebugMessageSink(plume::RenderDevice* device);

// With the debug layer on: keeps the device's debug interface across the
// release of every wrapper, so what is still alive afterwards -- a leak, by
// definition -- can be listed by name through the message sink. A no-op
// without the layer and on Vulkan.
class LiveObjectReport {
 public:
  LiveObjectReport() = default;
  ~LiveObjectReport();
  LiveObjectReport(const LiveObjectReport&) = delete;
  LiveObjectReport& operator=(const LiveObjectReport&) = delete;

  void Capture(plume::RenderDevice* device);
  // Logs the device's live objects (detail, internal ones ignored), then
  // releases the interface, which may be the device's last reference.
  void ReportAndRelease();

 private:
  void* debug_device_ = nullptr;
};

// plume::CreateD3D12Interface / CreateVulkanInterface. Plume declares neither
// in a header; consumers declare them.
std::unique_ptr<plume::RenderInterface> CreateInterface();

// Plume's create* calls hand back a live wrapper around a null native object
// when the backend call failed (plume_d3d12.cpp D3D12Texture ctor prints
// "CreateResource failed" and returns). These say whether that happened.
// Objects that have no native object on a backend (a D3D12 view or sampler is a
// CPU-side description) are null-backed there only when the pointer is null.
bool IsNullBacked(const plume::RenderTexture* texture);
bool IsNullBacked(const plume::RenderTextureView* view);
bool IsNullBacked(const plume::RenderBuffer* buffer);
bool IsNullBacked(const plume::RenderPipeline* graphics_pipeline);
bool IsNullBackedCompute(const plume::RenderPipeline* compute_pipeline);
bool IsNullBacked(const plume::RenderPipelineLayout* layout);
bool IsNullBacked(const plume::RenderShader* shader);
bool IsNullBacked(const plume::RenderSampler* sampler);
bool IsNullBacked(const plume::RenderQueryPool* pool);
bool IsNullBacked(const plume::RenderCommandQueue* queue);
bool IsNullBacked(const plume::RenderFramebuffer* framebuffer);

// Whether a descriptor set really got its descriptors. Plume's D3D12 set reports
// a full shader-visible heap only on stderr and keeps a zero-sized allocation
// (plume_d3d12.cpp D3D12DescriptorSet ctor), so the counts the set was built
// with are checked against what it holds.
bool DescriptorSetAllocated(const plume::RenderDescriptorSet* set, uint32_t view_count,
                            uint32_t sampler_count);

// Whether GPU timestamps can be written on the direct queue. Plume sets
// queryPools unconditionally (plume_vulkan.cpp, plume_d3d12.cpp); Vulkan only
// guarantees timestamps on every graphics queue with
// timestampComputeAndGraphics, and D3D12 needs the queue's timestamp
// frequency, which Plume leaves at 1 when GetTimestampFrequency fails.
bool TimestampsSupported(plume::RenderDevice* device);

// "D3D12 12_2" / "Vulkan 1.3.290": the version a driver bug report needs.
std::string DescribeDevice(plume::RenderDevice* device);

// The adapter to ask Plume for when the user named none (mcla_gpu_adapter empty). D3D12: the
// one Windows' graphics setting for this exe names (Settings > System > Display > Graphics:
// power saving or high performance), else the first of Windows' high-performance order; software
// and remote adapters and adapters without Direct3D 12 at feature level 11_0 are skipped, and the
// choice is logged with its reason. Plume alone takes the adapter with the most dedicated video
// memory, so an integrated GPU can win and Windows' setting is ignored. Empty means no opinion
// and Plume picks as before: Vulkan, no IDXGIFactory6, no adapter found, or two adapters with
// the first one's name (Plume matches by name and cannot tell them apart).
std::string DefaultAdapterName();

// The adapter's local video memory in use and the OS budget for it. Both 0 when
// the backend cannot report them.
struct VideoMemory {
  uint64_t used = 0;
  uint64_t budget = 0;
};
VideoMemory QueryVideoMemory(plume::RenderDevice* device);

// The device-removed reason, or 0 while the device is alive. Vulkan reports
// loss per call (VK_ERROR_DEVICE_LOST), which Plume does not surface, so it
// answers 0 there; the device also latches loss it sees on its own paths.
int64_t DeviceRemovedReason(plume::RenderDevice* device);

}  // namespace backend

}  // namespace mcla::gpu

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the graphics backend this build targets
// ===========================================================================
// One backend per build, chosen by CMake (LARECOMP_GPU_BACKEND), the way both
// reference recomps do it (reblue gpu/backend.h, reeot gpu/backend.h). Plume
// can host several backends in one process, but the two things that differ
// between them -- the host shader blob format and how constant buffers reach
// a shader (D3D12 root CBVs, Vulkan buffer device addresses in a push constant)
// -- are decided at build time anyway, so a runtime switch would only carry the
// unused half along.
//
// Everything here is backend-neutral to include. The code that has to reach
// past the Plume interface into a backend's own objects lives in exactly one
// translation unit, backend.cpp, behind the functions declared at the bottom.
// No other file under gpu/ includes plume_d3d12.h or plume_vulkan.h.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#if defined(LARECOMP_GPU_D3D12) + defined(LARECOMP_GPU_VULKAN) != 1
#error "exactly one of LARECOMP_GPU_D3D12 / LARECOMP_GPU_VULKAN must be defined (CMake LARECOMP_GPU_BACKEND)"
#endif

namespace mcla::gpu {

enum class Backend : uint8_t { kD3D12, kVulkan };

#if defined(LARECOMP_GPU_D3D12)
inline constexpr Backend kBackend = Backend::kD3D12;
inline constexpr plume::RenderShaderFormat kHostShaderFormat = plume::RenderShaderFormat::DXIL;
#else
inline constexpr Backend kBackend = Backend::kVulkan;
inline constexpr plume::RenderShaderFormat kHostShaderFormat = plume::RenderShaderFormat::SPIRV;
#endif

// Plume's Vulkan swap chain creates its VkSwapchainKHR in resize(), so a fresh
// one is empty until resized; the D3D12 one creates it in the constructor.
// (plume_vulkan.cpp VulkanSwapChain, reblue gpu/device.cpp CreateHostDevice.)
inline constexpr bool kSwapChainNeedsInitialResize = kBackend == Backend::kVulkan;

const char* BackendName();

namespace backend {

// Enables the D3D12 debug layer before the interface exists. Plume only does
// it itself when its own translation unit is built without NDEBUG, which no
// RelWithDebInfo build is. Returns false, and says why, when the request cannot
// be honoured (another backend, or no SDK layers installed). With the Agility
// SDK the layers DLL must be the one from the same release, next to
// D3D12Core.dll in .\D3D12\; the system's copy belongs to the OS runtime.
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

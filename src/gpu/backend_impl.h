#pragma once
// MCLA GPU renderer -- the per-backend halves of backend.h. Each compiled
// backend implements this set in its own translation unit (backend_d3d12.cpp,
// backend_vulkan.cpp, the only two that include a backend's Plume header);
// backend.cpp forwards every backend.h call to the active one.

#include "backend.h"

namespace mcla::gpu::backend {

// The GPU the renderer will run on, as DXGI lists it (d3d12::DescribeRenderingAdapter). Asked
// before any backend exists, to decide graphics_backend = "auto" (ChooseAutoBackend, backend.h).
struct RenderingAdapter {
  bool found = false;       // DXGI lists an adapter the renderer can use
  std::string name;         // DXGI's description: the name Plume and mcla_gpu_adapter match on
  uint32_t vendor_id = 0;   // PCI vendor id: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
  bool has_outputs = true;  // a display is connected to it; true when DXGI could not say
};

#if defined(LARECOMP_GPU_HAS_D3D12)
namespace d3d12 {
bool EnableDebugLayer();
void InstallDebugMessageSink(plume::RenderDevice* device);
// LiveObjectReport: the device's debug interface (an ID3D12DebugDevice*), or null.
void* CaptureDebugDevice(plume::RenderDevice* device);
void ReportLiveObjects(void* debug_device);  // and releases it
void ReleaseDebugDevice(void* debug_device);
std::unique_ptr<plume::RenderInterface> CreateInterface();
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
bool DescriptorSetAllocated(const plume::RenderDescriptorSet* set, uint32_t view_count,
                            uint32_t sampler_count);
bool TimestampsSupported(plume::RenderDevice* device);
std::string DescribeDevice(plume::RenderDevice* device);
VideoMemory QueryVideoMemory(plume::RenderDevice* device);
int64_t DeviceRemovedReason(plume::RenderDevice* device);
std::string DefaultAdapterName();
RenderingAdapter DescribeRenderingAdapter(const std::string& preferred_adapter);
NativeDevice NativeDeviceOf(plume::RenderDevice* device);
void* NativeListHandle(plume::RenderCommandList* list);
void* NativeTextureHandle(const plume::RenderTexture* texture);
NativeImage NativeImageOf(const plume::RenderTexture* texture, ImageUse use);
void InvalidateAfterExternalWork(plume::RenderCommandList* list);
void ForgetImageContents(plume::RenderTexture* texture);
UpscalerSupport QueryUpscalerSupport(plume::RenderDevice* device);
// SwapChainFactoryScope: the factory replaced by its wrapper (`original`, `wrapper` filled), and put
// back and the wrapper released.
bool BeginSwapChainFactory(plume::RenderDevice* device, bool (*wrap)(void** factory), void** original,
                           void** wrapper);
void EndSwapChainFactory(plume::RenderDevice* device, void* original, void* wrapper);
}  // namespace d3d12
#endif

#if defined(LARECOMP_GPU_HAS_VULKAN)
namespace vulkan {
bool EnableDebugLayer();
void InstallDebugMessageSink(plume::RenderDevice* device);
void* CaptureDebugDevice(plume::RenderDevice* device);
void ReportLiveObjects(void* debug_device);
void ReleaseDebugDevice(void* debug_device);
std::unique_ptr<plume::RenderInterface> CreateInterface();
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
bool DescriptorSetAllocated(const plume::RenderDescriptorSet* set, uint32_t view_count,
                            uint32_t sampler_count);
bool TimestampsSupported(plume::RenderDevice* device);
std::string DescribeDevice(plume::RenderDevice* device);
VideoMemory QueryVideoMemory(plume::RenderDevice* device);
int64_t DeviceRemovedReason(plume::RenderDevice* device);
std::string DefaultAdapterName();
NativeDevice NativeDeviceOf(plume::RenderDevice* device);
void* NativeListHandle(plume::RenderCommandList* list);
void* NativeTextureHandle(const plume::RenderTexture* texture);
NativeImage NativeImageOf(const plume::RenderTexture* texture, ImageUse use);
void InvalidateAfterExternalWork(plume::RenderCommandList* list);
void ForgetImageContents(plume::RenderTexture* texture);
UpscalerSupport QueryUpscalerSupport(plume::RenderDevice* device);
bool BeginSwapChainFactory(plume::RenderDevice* device, bool (*wrap)(void** factory), void** original,
                           void** wrapper);
void EndSwapChainFactory(plume::RenderDevice* device, void* original, void* wrapper);
}  // namespace vulkan
#endif

}  // namespace mcla::gpu::backend

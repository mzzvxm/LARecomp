// MCLA GPU renderer -- the active backend, and backend.h's calls forwarded to
// its half (backend_d3d12.cpp, backend_vulkan.cpp). See backend.h.

#include "backend.h"

#include <rex/logging.h>

#include "backend_impl.h"

// The active backend's version of `call`. With one backend in the build there
// is nothing to choose.
#if defined(LARECOMP_GPU_HAS_D3D12) && defined(LARECOMP_GPU_HAS_VULKAN)
#define MCLA_GPU_ACTIVE(call) \
  (::mcla::gpu::ActiveBackend() == ::mcla::gpu::Backend::kD3D12 ? d3d12::call : vulkan::call)
#elif defined(LARECOMP_GPU_HAS_D3D12)
#define MCLA_GPU_ACTIVE(call) d3d12::call
#else
#define MCLA_GPU_ACTIVE(call) vulkan::call
#endif

namespace mcla::gpu {

void SelectBackend(Backend backend) {
  if (!BackendCompiled(backend)) {
    REXLOG_ERROR("[gpu] {} is not in this build; {} stays selected", BackendName(backend),
                 BackendName());
    return;
  }
  detail::g_active_backend = backend;
}

const char* BackendName(Backend backend) {
  return backend == Backend::kD3D12 ? "D3D12" : "Vulkan";
}

bool ParseBackend(std::string_view text, Backend& out) {
  out = kDefaultBackend;
  if (text == "auto") {
    return true;
  }
  if (text == "d3d12") {
    out = Backend::kD3D12;
    return true;
  }
  if (text == "vulkan") {
    out = Backend::kVulkan;
    return true;
  }
  return false;
}

namespace backend {

bool EnableDebugLayer() {
  return MCLA_GPU_ACTIVE(EnableDebugLayer());
}

void InstallDebugMessageSink(plume::RenderDevice* device) {
  MCLA_GPU_ACTIVE(InstallDebugMessageSink(device));
}

LiveObjectReport::~LiveObjectReport() {
  if (debug_device_) {
    MCLA_GPU_ACTIVE(ReleaseDebugDevice(debug_device_));
  }
}

void LiveObjectReport::Capture(plume::RenderDevice* device) {
  if (!debug_device_) {
    debug_device_ = MCLA_GPU_ACTIVE(CaptureDebugDevice(device));
  }
}

void LiveObjectReport::ReportAndRelease() {
  if (!debug_device_) {
    return;
  }
  void* debug_device = debug_device_;
  debug_device_ = nullptr;
  MCLA_GPU_ACTIVE(ReportLiveObjects(debug_device));
}

std::unique_ptr<plume::RenderInterface> CreateInterface() {
  return MCLA_GPU_ACTIVE(CreateInterface());
}

bool IsNullBacked(const plume::RenderTexture* texture) {
  return MCLA_GPU_ACTIVE(IsNullBacked(texture));
}

bool IsNullBacked(const plume::RenderTextureView* view) {
  return MCLA_GPU_ACTIVE(IsNullBacked(view));
}

bool IsNullBacked(const plume::RenderBuffer* buffer) {
  return MCLA_GPU_ACTIVE(IsNullBacked(buffer));
}

bool IsNullBacked(const plume::RenderPipeline* graphics_pipeline) {
  return MCLA_GPU_ACTIVE(IsNullBacked(graphics_pipeline));
}

bool IsNullBackedCompute(const plume::RenderPipeline* compute_pipeline) {
  return MCLA_GPU_ACTIVE(IsNullBackedCompute(compute_pipeline));
}

bool IsNullBacked(const plume::RenderPipelineLayout* layout) {
  return MCLA_GPU_ACTIVE(IsNullBacked(layout));
}

bool IsNullBacked(const plume::RenderShader* shader) {
  return MCLA_GPU_ACTIVE(IsNullBacked(shader));
}

bool IsNullBacked(const plume::RenderSampler* sampler) {
  return MCLA_GPU_ACTIVE(IsNullBacked(sampler));
}

bool IsNullBacked(const plume::RenderQueryPool* pool) {
  return MCLA_GPU_ACTIVE(IsNullBacked(pool));
}

bool IsNullBacked(const plume::RenderCommandQueue* queue) {
  return MCLA_GPU_ACTIVE(IsNullBacked(queue));
}

bool IsNullBacked(const plume::RenderFramebuffer* framebuffer) {
  return MCLA_GPU_ACTIVE(IsNullBacked(framebuffer));
}

bool DescriptorSetAllocated(const plume::RenderDescriptorSet* set, uint32_t view_count,
                            uint32_t sampler_count) {
  return MCLA_GPU_ACTIVE(DescriptorSetAllocated(set, view_count, sampler_count));
}

bool TimestampsSupported(plume::RenderDevice* device) {
  return MCLA_GPU_ACTIVE(TimestampsSupported(device));
}

std::string DescribeDevice(plume::RenderDevice* device) {
  return MCLA_GPU_ACTIVE(DescribeDevice(device));
}

VideoMemory QueryVideoMemory(plume::RenderDevice* device) {
  return MCLA_GPU_ACTIVE(QueryVideoMemory(device));
}

int64_t DeviceRemovedReason(plume::RenderDevice* device) {
  return MCLA_GPU_ACTIVE(DeviceRemovedReason(device));
}

}  // namespace backend

}  // namespace mcla::gpu

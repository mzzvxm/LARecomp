// MCLA GPU renderer -- the active backend, and backend.h's calls forwarded to
// its half (backend_d3d12.cpp, backend_vulkan.cpp). See backend.h.

#include "backend.h"
#include "gpu.h"

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

namespace {

#if defined(_WIN32) && defined(LARECOMP_GPU_HAS_D3D12) && defined(LARECOMP_GPU_HAS_VULKAN)
constexpr uint32_t kVendorNvidia = 0x10DE;

const char* VendorName(uint32_t vendor_id) {
  switch (vendor_id) {
    case kVendorNvidia:
      return "NVIDIA";
    case 0x1002:
      return "AMD";
    case 0x8086:
      return "Intel";
    default:
      return "other";
  }
}
#endif

}  // namespace

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

Backend ChooseAutoBackend(const std::string& preferred_adapter, std::string& adapter) {
  adapter.clear();
#if defined(_WIN32) && defined(LARECOMP_GPU_HAS_D3D12) && defined(LARECOMP_GPU_HAS_VULKAN)
  // Why Vulkan here and D3D12 everywhere else: on a desktop where the GPU that renders is not
  // the one that drives the display (measured with an NVIDIA GPU rendering and an AMD one
  // presenting), D3D12 presentation pauses for about 20-25 ms every 5 s whenever frames are fast
  // (uncapped, or vsync on a fast display); Vulkan does not. Players on hybrid laptops reported a
  // shaking camera on D3D12 that Vulkan fixed. A GPU with no output is how DXGI shows that it is
  // not driving the display. Only NVIDIA: on AMD, Vulkan's GPU time was about 20% longer than
  // D3D12's here, so AMD and Intel keep D3D12.
  const backend::RenderingAdapter gpu = backend::d3d12::DescribeRenderingAdapter(preferred_adapter);
  if (!gpu.found) {
    REXLOG_INFO("[gpu] graphics_backend auto: D3D12 (DXGI lists no adapter to look at)");
    return Backend::kD3D12;
  }
  const bool hybrid_nvidia = gpu.vendor_id == kVendorNvidia && !gpu.has_outputs;
  const char* why = "";
  if (hybrid_nvidia) {
    why = ": it renders for a display driven by another GPU, where D3D12 presents with pauses";
  } else if (!gpu.has_outputs) {
    why = ": Vulkan is only chosen for NVIDIA";
  }
  REXLOG_INFO("[gpu] graphics_backend auto: {} (GPU '{}', vendor {:#06x} {}, {}{})",
              hybrid_nvidia ? "Vulkan" : "D3D12", gpu.name, gpu.vendor_id,
              VendorName(gpu.vendor_id),
              gpu.has_outputs ? "has a display output" : "no display output", why);
  if (!hybrid_nvidia) {
    return Backend::kD3D12;
  }
  adapter = gpu.name;
  return Backend::kVulkan;
#else
  (void)preferred_adapter;
  return kDefaultBackend;
#endif
}

bool AutoBackendIsVulkan(const std::string& preferred_adapter) {
  std::string adapter;
  return ChooseAutoBackend(preferred_adapter, adapter) == Backend::kVulkan;
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

std::string DefaultAdapterName() {
  return MCLA_GPU_ACTIVE(DefaultAdapterName());
}

int64_t DeviceRemovedReason(plume::RenderDevice* device) {
  return MCLA_GPU_ACTIVE(DeviceRemovedReason(device));
}

NativeDevice NativeDeviceOf(plume::RenderDevice* device) {
  return MCLA_GPU_ACTIVE(NativeDeviceOf(device));
}

void* NativeListHandle(plume::RenderCommandList* list) {
  return MCLA_GPU_ACTIVE(NativeListHandle(list));
}

void* NativeTextureHandle(const plume::RenderTexture* texture) {
  return MCLA_GPU_ACTIVE(NativeTextureHandle(texture));
}

NativeImage NativeImageOf(const plume::RenderTexture* texture, ImageUse use) {
  return MCLA_GPU_ACTIVE(NativeImageOf(texture, use));
}

uint32_t RenderingAdapterVendor(const std::string& preferred_adapter) {
#if defined(_WIN32) && defined(LARECOMP_GPU_HAS_D3D12)
  return d3d12::DescribeRenderingAdapter(preferred_adapter).vendor_id;
#else
  (void)preferred_adapter;
  return 0;
#endif
}

void InvalidateAfterExternalWork(plume::RenderCommandList* list) {
  MCLA_GPU_ACTIVE(InvalidateAfterExternalWork(list));
}

void ForgetImageContents(plume::RenderTexture* texture) {
  MCLA_GPU_ACTIVE(ForgetImageContents(texture));
}

UpscalerSupport QueryUpscalerSupport(plume::RenderDevice* device) {
  return MCLA_GPU_ACTIVE(QueryUpscalerSupport(device));
}

SwapChainFactoryScope::SwapChainFactoryScope(plume::RenderDevice* device,
                                             bool (*wrap)(void** factory))
    : device_(device) {
  if (device_ && wrap) {
    void* original = nullptr;
    void* wrapper = nullptr;
    if (MCLA_GPU_ACTIVE(BeginSwapChainFactory(device_, wrap, &original, &wrapper))) {
      original_ = original;
      wrapper_ = wrapper;
    }
  }
}

SwapChainFactoryScope::~SwapChainFactoryScope() {
  if (wrapper_) {
    MCLA_GPU_ACTIVE(EndSwapChainFactory(device_, original_, wrapper_));
  }
}

}  // namespace backend

}  // namespace mcla::gpu

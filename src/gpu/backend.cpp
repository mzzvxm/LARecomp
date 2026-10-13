// MCLA GPU renderer -- the one translation unit that reaches past the Plume
// interface into a backend's own objects. See backend.h for why each exists.
//
// Design followed (not copied): reblue gpu/device.cpp CreateHostBuffer/
// CreateHostTexture/DescribeBackend/MemoryUsage and reeot gpu/device.cpp
// ReportCreationFailure, both BSD-3-Clause.

#include "backend.h"

#include <format>

#include <rex/logging.h>

#if defined(LARECOMP_GPU_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif

namespace plume {
#if defined(LARECOMP_GPU_D3D12)
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#else
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
}  // namespace plume

namespace mcla::gpu {

const char* BackendName() {
  return kBackend == Backend::kD3D12 ? "D3D12" : "Vulkan";
}

namespace backend {

#if defined(LARECOMP_GPU_D3D12)

bool EnableDebugLayer() {
  ID3D12Debug* debug = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || !debug) {
    REXLOG_ERROR("[gpu] D3D12 debug layer requested but D3D12GetDebugInterface failed "
                 "(Graphics Tools optional feature not installed?)");
    return false;
  }
  debug->EnableDebugLayer();
  debug->Release();
  return true;
}

namespace {

void __stdcall OnD3D12DebugMessage(D3D12_MESSAGE_CATEGORY category,
                                   D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                                   LPCSTR description, void*) {
  switch (severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
    case D3D12_MESSAGE_SEVERITY_ERROR:
      REXLOG_ERROR("[gpu] D3D12 debug layer (category {}, id {}): {}",
                   static_cast<int>(category), static_cast<int>(id),
                   description ? description : "");
      break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
      REXLOG_WARN("[gpu] D3D12 debug layer (category {}, id {}): {}", static_cast<int>(category),
                  static_cast<int>(id), description ? description : "");
      break;
    default:
      break;
  }
}

}  // namespace

void InstallDebugMessageSink(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->d3d) {
    return;
  }
  ID3D12InfoQueue1* queue = nullptr;
  if (FAILED(dev->d3d->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
    REXLOG_ERROR("[gpu] the D3D12 debug layer is not active on this device (no ID3D12InfoQueue1): "
                 "its messages cannot be logged");
    return;
  }
  // Notes this renderer produces by design, kept out so that what is left is
  // worth reading (the callback honours the storage filter):
  // - CLEAR{RENDERTARGET,DEPTHSTENCIL}VIEW_MISMATCHINGCLEARVALUE: the EDRAM
  //   targets have no optimized clear value -- the guest clears them to
  //   whatever each frame needs -- so every clear carries this performance note;
  // - CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET: a pixel shader that
  //   writes a colour slot its pass has no target in; the layer itself says
  //   writes to an unbound target are discarded, and the shader is still needed
  //   for what it discards and the depth it leaves.
  D3D12_MESSAGE_ID denied[] = {
      D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
      D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
      D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
  };
  D3D12_INFO_QUEUE_FILTER filter = {};
  filter.DenyList.NumIDs = UINT(std::size(denied));
  filter.DenyList.pIDList = denied;
  if (FAILED(queue->PushStorageFilter(&filter))) {
    REXLOG_WARN("[gpu] could not filter the D3D12 debug layer's by-design notes");
  }
  DWORD cookie = 0;
  const HRESULT hr = queue->RegisterMessageCallback(&OnD3D12DebugMessage,
                                                    D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                                    &cookie);
  // The callback belongs to the device's queue and lives as long as the device.
  queue->Release();
  if (FAILED(hr)) {
    REXLOG_ERROR("[gpu] could not register the D3D12 debug message callback ({:#010x})",
                 static_cast<uint32_t>(hr));
    return;
  }
  REXLOG_INFO("[gpu] D3D12 debug layer messages (warnings and errors) go to this log");
}

LiveObjectReport::~LiveObjectReport() {
  if (debug_device_) {
    static_cast<ID3D12DebugDevice*>(debug_device_)->Release();
  }
}

void LiveObjectReport::Capture(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (debug_device_ || !dev || !dev->d3d) {
    return;
  }
  ID3D12DebugDevice* debug_device = nullptr;
  if (SUCCEEDED(dev->d3d->QueryInterface(IID_PPV_ARGS(&debug_device)))) {
    debug_device_ = debug_device;
  }
}

void LiveObjectReport::ReportAndRelease() {
  if (!debug_device_) {
    return;
  }
  auto* debug_device = static_cast<ID3D12DebugDevice*>(debug_device_);
  debug_device_ = nullptr;
  REXLOG_INFO("[gpu] D3D12 objects still alive after the renderer released everything (the "
              "device itself is held by this report):");
  debug_device->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
  debug_device->Release();
}

std::unique_ptr<plume::RenderInterface> CreateInterface() {
  return plume::CreateD3D12Interface();
}

bool IsNullBacked(const plume::RenderTexture* texture) {
  return !texture || static_cast<const plume::D3D12Texture*>(texture)->d3d == nullptr;
}

bool IsNullBacked(const plume::RenderTextureView* view) {
  // A CPU-side view description; it only names its texture.
  return !view || static_cast<const plume::D3D12TextureView*>(view)->texture == nullptr;
}

bool IsNullBacked(const plume::RenderBuffer* buffer) {
  return !buffer || static_cast<const plume::D3D12Buffer*>(buffer)->d3d == nullptr;
}

bool IsNullBacked(const plume::RenderPipeline* graphics_pipeline) {
  return !graphics_pipeline ||
         static_cast<const plume::D3D12GraphicsPipeline*>(graphics_pipeline)->d3d == nullptr;
}

bool IsNullBacked(const plume::RenderPipelineLayout* layout) {
  return !layout ||
         static_cast<const plume::D3D12PipelineLayout*>(layout)->rootSignature == nullptr;
}

bool IsNullBacked(const plume::RenderShader* shader) {
  // D3D12 keeps the bytecode; nothing is compiled until a pipeline uses it.
  return !shader || static_cast<const plume::D3D12Shader*>(shader)->d3d.empty();
}

bool IsNullBacked(const plume::RenderSampler* sampler) {
  return !sampler;
}

bool IsNullBacked(const plume::RenderQueryPool* pool) {
  if (!pool) {
    return true;
  }
  const auto* p = static_cast<const plume::D3D12QueryPool*>(pool);
  return p->d3d == nullptr || IsNullBacked(p->readbackBuffer.get());
}

bool IsNullBacked(const plume::RenderCommandQueue* queue) {
  return !queue || static_cast<const plume::D3D12CommandQueue*>(queue)->d3d == nullptr;
}

bool IsNullBacked(const plume::RenderFramebuffer* framebuffer) {
  // A D3D12 framebuffer is its RTV/DSV descriptors: failed if any of them
  // could not be allocated.
  if (!framebuffer) {
    return true;
  }
  const auto* f = static_cast<const plume::D3D12Framebuffer*>(framebuffer);
  for (uint32_t offset : f->colorTargetAllocatorOffsets) {
    if (offset == plume::D3D12DescriptorHeapAllocator::INVALID_OFFSET) {
      return true;
    }
  }
  return f->depthTarget != nullptr &&
         f->depthTargetAllocatorOffset == plume::D3D12DescriptorHeapAllocator::INVALID_OFFSET;
}

bool DescriptorSetAllocated(const plume::RenderDescriptorSet* set, uint32_t view_count,
                            uint32_t sampler_count) {
  if (!set) {
    return false;
  }
  const auto* s = static_cast<const plume::D3D12DescriptorSet*>(set);
  return s->viewAllocation.count == view_count && s->samplerAllocation.count == sampler_count;
}

bool TimestampsSupported(plume::RenderDevice* device) {
  const auto* dev = static_cast<const plume::D3D12Device*>(device);
  return dev && dev->timestampFrequency > 1;
}

std::string DescribeDevice(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->d3d) {
    return "D3D12";
  }
  static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                              D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                              D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
  levels.NumFeatureLevels = static_cast<UINT>(std::size(kLevels));
  levels.pFeatureLevelsRequested = kLevels;
  if (FAILED(dev->d3d->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels,
                                           sizeof(levels)))) {
    return "D3D12";
  }
  const uint32_t fl = static_cast<uint32_t>(levels.MaxSupportedFeatureLevel);
  return std::format("D3D12 {}_{}", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
}

VideoMemory QueryVideoMemory(plume::RenderDevice* device) {
  VideoMemory out;
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->adapter) {
    return out;
  }
  IDXGIAdapter3* adapter = nullptr;
  if (FAILED(dev->adapter->QueryInterface(IID_PPV_ARGS(&adapter))) || !adapter) {
    return out;
  }
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
    out.used = info.CurrentUsage;
    out.budget = info.Budget;
  }
  adapter->Release();
  return out;
}

int64_t DeviceRemovedReason(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::D3D12Device*>(device);
  if (!dev || !dev->d3d) {
    return 0;
  }
  const HRESULT reason = dev->d3d->GetDeviceRemovedReason();
  return FAILED(reason) ? static_cast<int64_t>(static_cast<uint32_t>(reason)) : 0;
}

#else  // LARECOMP_GPU_VULKAN

bool EnableDebugLayer() {
  // Plume turns VK_LAYER_KHRONOS_validation on from inside its own translation
  // unit, and only when that unit is compiled without NDEBUG. There is no
  // switch to flip from out here, so say so instead of pretending.
  REXLOG_ERROR("[gpu] Vulkan validation cannot be enabled at runtime: Plume enables it only "
               "when plume_vulkan.cpp is compiled without NDEBUG (a Debug build of plume)");
  return false;
}

void InstallDebugMessageSink(plume::RenderDevice*) {
  // EnableDebugLayer never succeeds on Vulkan (see above), so there is no layer
  // whose messages could be routed.
}

LiveObjectReport::~LiveObjectReport() = default;

void LiveObjectReport::Capture(plume::RenderDevice*) {}

void LiveObjectReport::ReportAndRelease() {}

std::unique_ptr<plume::RenderInterface> CreateInterface() {
  return plume::CreateVulkanInterface();
}

bool IsNullBacked(const plume::RenderTexture* texture) {
  return !texture || static_cast<const plume::VulkanTexture*>(texture)->vk == VK_NULL_HANDLE;
}

bool IsNullBacked(const plume::RenderTextureView* view) {
  return !view || static_cast<const plume::VulkanTextureView*>(view)->vk == VK_NULL_HANDLE;
}

bool IsNullBacked(const plume::RenderBuffer* buffer) {
  return !buffer || static_cast<const plume::VulkanBuffer*>(buffer)->vk == VK_NULL_HANDLE;
}

bool IsNullBacked(const plume::RenderPipeline* graphics_pipeline) {
  return !graphics_pipeline ||
         static_cast<const plume::VulkanGraphicsPipeline*>(graphics_pipeline)->vk ==
             VK_NULL_HANDLE;
}

bool IsNullBacked(const plume::RenderPipelineLayout* layout) {
  return !layout ||
         static_cast<const plume::VulkanPipelineLayout*>(layout)->vk == VK_NULL_HANDLE;
}

bool IsNullBacked(const plume::RenderShader* shader) {
  return !shader || static_cast<const plume::VulkanShader*>(shader)->vk == VK_NULL_HANDLE;
}

bool IsNullBacked(const plume::RenderSampler* sampler) {
  return !sampler || static_cast<const plume::VulkanSampler*>(sampler)->vk == VK_NULL_HANDLE;
}

bool IsNullBacked(const plume::RenderQueryPool* pool) {
  return !pool || static_cast<const plume::VulkanQueryPool*>(pool)->vk == VK_NULL_HANDLE;
}

bool IsNullBacked(const plume::RenderCommandQueue* queue) {
  return !queue || static_cast<const plume::VulkanCommandQueue*>(queue)->queue == nullptr;
}

bool IsNullBacked(const plume::RenderFramebuffer* framebuffer) {
  return !framebuffer ||
         static_cast<const plume::VulkanFramebuffer*>(framebuffer)->vk == VK_NULL_HANDLE;
}

bool DescriptorSetAllocated(const plume::RenderDescriptorSet* set, uint32_t, uint32_t) {
  // vkAllocateDescriptorSets either returns the whole set or nothing.
  return set && static_cast<const plume::VulkanDescriptorSet*>(set)->vk != VK_NULL_HANDLE;
}

bool TimestampsSupported(plume::RenderDevice* device) {
  const auto* dev = static_cast<const plume::VulkanDevice*>(device);
  return dev && dev->physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE;
}

std::string DescribeDevice(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev) {
    return "Vulkan";
  }
  // The physical device's own version, not the one Plume asks the instance for.
  const uint32_t v = dev->physicalDeviceProperties.apiVersion;
  return std::format("Vulkan {}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
                     VK_API_VERSION_PATCH(v));
}

VideoMemory QueryVideoMemory(plume::RenderDevice*) {
  // Not reported on Vulkan, deliberately:
  //  - Plume does not enable VK_EXT_memory_budget, so VMA's vmaGetHeapBudgets
  //    (what reblue calls) would only return its own estimate;
  //  - this SDK's rexruntime DLL exports its own copy of VMA, and a call to
  //    vma* from this translation unit binds to that import before
  //    plume_vulkan.obj (which defines the same names) is loaded, which the
  //    linker refuses ("vmaGetHeapBudgets was replaced", measured 05/10).
  // Enabling the extension in Plume and reading it through volk is the fix
  // when the Vulkan path needs a VRAM report.
  return {};
}

int64_t DeviceRemovedReason(plume::RenderDevice*) {
  return 0;
}

#endif

}  // namespace backend

}  // namespace mcla::gpu

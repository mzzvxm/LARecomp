// MCLA GPU renderer -- the Vulkan half of backend.h: one of the two translation
// units that reach past the Plume interface into a backend's own objects (the
// other is backend_d3d12.cpp). See backend.h for why each call exists.
//
// Design followed (not copied): reblue gpu/device.cpp CreateHostBuffer/
// CreateHostTexture/DescribeBackend/MemoryUsage and reeot gpu/device.cpp
// ReportCreationFailure, both BSD-3-Clause.

#include "backend_impl.h"

#include <cstring>
#include <format>
#include <vector>

#include <rex/logging.h>

#include <plume_vulkan.h>

namespace plume {
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}  // namespace plume

namespace mcla::gpu::backend::vulkan {

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

void* CaptureDebugDevice(plume::RenderDevice*) {
  return nullptr;
}

void ReportLiveObjects(void*) {}

void ReleaseDebugDevice(void*) {}

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

bool IsNullBackedCompute(const plume::RenderPipeline* compute_pipeline) {
  return !compute_pipeline ||
         static_cast<const plume::VulkanComputePipeline*>(compute_pipeline)->vk == VK_NULL_HANDLE;
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

namespace {

// Whether the physical device lists VK_EXT_memory_budget.
bool ListsMemoryBudget(VkPhysicalDevice physical_device) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr) !=
          VK_SUCCESS ||
      count == 0) {
    return false;
  }
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data()) <
      0) {
    return false;
  }
  for (const VkExtensionProperties& extension : extensions) {
    if (std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

VideoMemory QueryVideoMemory(plume::RenderDevice* device) {
  // VK_EXT_memory_budget through vkGetPhysicalDeviceMemoryProperties2, read
  // through volk (Plume's loader). Not through VMA's vmaGetHeapBudgets: this
  // SDK's rexruntime DLL exports its own copy of VMA, and a call to vma* from
  // this translation unit binds to that import before plume_vulkan.obj (which
  // defines the same names) is loaded, which the linker refuses
  // ("vmaGetHeapBudgets was replaced", measured 05/10).
  // Plume does not enable the extension on the device (OptionalDeviceExtensions
  // in plume_vulkan.cpp), and the specification asks for it to be enabled; the
  // physical device's answer does not depend on the device that asks on the
  // drivers this was written against, but where one answers 0 -- or lists no
  // such extension -- nothing is reported, as before, and the budget response
  // (vram_budget.h) runs only on a creation that failed.
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || dev->physicalDevice == VK_NULL_HANDLE || !vkGetPhysicalDeviceMemoryProperties2) {
    return {};
  }
  static const bool lists_budget = ListsMemoryBudget(dev->physicalDevice);
  if (!lists_budget) {
    return {};
  }
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {};
  budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
  VkPhysicalDeviceMemoryProperties2 properties = {};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
  properties.pNext = &budget;
  vkGetPhysicalDeviceMemoryProperties2(dev->physicalDevice, &properties);
  // The device-local heap Plume sized dedicatedVideoMemory from: the biggest
  // (the others are the small host-visible window of a discrete card).
  const VkPhysicalDeviceMemoryProperties& memory = properties.memoryProperties;
  uint32_t heap = UINT32_MAX;
  VkDeviceSize biggest = 0;
  for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
    if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
        memory.memoryHeaps[i].size > biggest) {
      heap = i;
      biggest = memory.memoryHeaps[i].size;
    }
  }
  if (heap == UINT32_MAX || budget.heapBudget[heap] == 0) {
    return {};
  }
  VideoMemory out;
  out.used = budget.heapUsage[heap];
  out.budget = budget.heapBudget[heap];
  return out;
}

int64_t DeviceRemovedReason(plume::RenderDevice*) {
  return 0;
}

// Plume's Vulkan rule stands; the D3D12 choice is made from DXGI's adapters.
std::string DefaultAdapterName() {
  return {};
}

}  // namespace mcla::gpu::backend::vulkan

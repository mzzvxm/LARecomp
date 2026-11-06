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

#include "temporal_dlss.h"

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
#if defined(LARECOMP_PLUME_STREAMLINE)
  // NVIDIA Streamline (temporal_dlss.h), started before the instance when DLSS is the choice:
  // Plume loads every Vulkan function through the interposer's vkGetInstanceProcAddr, whose
  // vkCreateInstance and vkCreateDevice add the extensions, features and queues DLSS needs.
  if (void* hook = dlss::VulkanInstanceProcAddr()) {
    plume::VulkanInterfaceHooks hooks;
    hooks.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(hook);
    return plume::CreateVulkanInterface(hooks);
  }
#endif
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

NativeDevice NativeDeviceOf(plume::RenderDevice* device) {
  NativeDevice out;
  const auto* dev = static_cast<const plume::VulkanDevice*>(device);
  if (dev) {
    out.device = reinterpret_cast<void*>(dev->vk);
    out.physical_device = reinterpret_cast<void*>(dev->physicalDevice);
    out.get_device_proc_addr = reinterpret_cast<void*>(vkGetDeviceProcAddr);
  }
  return out;
}

void* NativeListHandle(plume::RenderCommandList* list) {
  const auto* l = static_cast<const plume::VulkanCommandList*>(list);
  return l ? reinterpret_cast<void*>(l->vk) : nullptr;
}

void* NativeTextureHandle(const plume::RenderTexture* texture) {
  const auto* t = static_cast<const plume::VulkanTexture*>(texture);
  return t ? reinterpret_cast<void*>(t->vk) : nullptr;
}

NativeImage NativeImageOf(const plume::RenderTexture* texture, ImageUse use) {
  NativeImage out;
  const auto* t = static_cast<const plume::VulkanTexture*>(texture);
  if (!t || t->vk == VK_NULL_HANDLE || t->imageView == VK_NULL_HANDLE) {
    return out;
  }
  out.resource = reinterpret_cast<void*>(t->vk);
  out.view = reinterpret_cast<void*>(t->imageView);
  out.memory = reinterpret_cast<void*>(t->allocationInfo.deviceMemory);
  out.native_format = uint32_t(t->imageFormat);
  out.state = use == ImageUse::kStorage ? uint32_t(VK_IMAGE_LAYOUT_GENERAL)
                                        : uint32_t(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  out.mip_levels = t->desc.mipLevels ? t->desc.mipLevels : 1;
  out.array_layers = t->desc.arraySize ? t->desc.arraySize : 1;
#if defined(LARECOMP_PLUME_STREAMLINE)
  // Plume's feat/streamline-hooks keeps the usage and creation flags the image was made with.
  out.usage = uint32_t(t->imageUsage);
  out.create_flags = uint32_t(t->imageFlags);
  out.complete = out.usage != 0;
#else
  out.complete = false;
#endif
  return out;
}

void InvalidateAfterExternalWork(plume::RenderCommandList* list) {
  auto* l = static_cast<plume::VulkanCommandList*>(list);
  if (!l) {
    return;
  }
  // A compute runtime binds its own compute pipeline, layout and descriptor sets; the caller has
  // ended the render pass and bound no framebuffer. Plume's cached layouts go, so the next
  // setXxxPipelineLayout / setXxxDescriptorSet names them again (the bind points are separate in
  // Vulkan, so graphics state itself is untouched).
  l->activeComputePipelineLayout = nullptr;
  l->activeGraphicsPipelineLayout = nullptr;
  l->activeRaytracingPipelineLayout = nullptr;
}

namespace {

// What Plume's Vulkan device must report for FidelityFX (the P1 list in the report of the change
// that added this): the compute capabilities of plume_render_interface_types.h, added by Plume's
// feat/ffx-prereqs. A Plume without them has no such members: a requires-expression inside a
// template sees that (a discarded branch of an if constexpr in a template is never instantiated).
// Returns why it cannot run, empty when it can.
template <typename Caps>
std::string ComputeCapsProblem(const Caps& caps, bool has_float16, bool has_subgroup_control) {
  if constexpr (requires {
                  caps.native16BitShaderOps;
                  caps.subgroupSizeControl;
                  caps.typedUnorderedAccessLoad;
                }) {
    // The runtime picks its 16-bit shaders and its wave sizes from what the physical device
    // supports, not from what the device was created with: a feature the device has and Plume did
    // not enable is a shader that uses it anyway.
    if (has_float16 && !caps.native16BitShaderOps) {
      return "Plume's Vulkan device was created without the 16-bit shader features this graphics "
             "card has (Plume change P1)";
    }
    if (has_subgroup_control && !caps.subgroupSizeControl) {
      return "Plume's Vulkan device was created without the subgroup size control this graphics "
             "card has (Plume change P1)";
    }
    // Typed reads of the four-channel 16-bit unorm storage image FidelityFX's passes declare.
    if (!caps.typedUnorderedAccessLoad) {
      return "the Vulkan driver cannot read storage images without a format, which FidelityFX's "
             "shaders do";
    }
    return {};
  } else {
    (void)caps;
    (void)has_float16;
    (void)has_subgroup_control;
    return "this build's Plume does not enable the Vulkan device features FidelityFX asks for "
           "(Plume change P1)";
  }
}

}  // namespace

UpscalerSupport QueryUpscalerSupport(plume::RenderDevice* device) {
  UpscalerSupport out;
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || dev->physicalDevice == VK_NULL_HANDLE || !vkGetPhysicalDeviceFeatures2) {
    out.reason = "there is no Vulkan device";
    return out;
  }
  const uint32_t v = dev->physicalDeviceProperties.apiVersion;
  out.detail = std::format("Vulkan {}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
                           VK_API_VERSION_PATCH(v));
  // CHECK: FidelityFX's Vulkan upscaler needs Vulkan 1.2 or later (the version Plume asks the
  // instance for); the runtime's creation is the final word.
  if (VK_API_VERSION_MAJOR(v) < 1 || (VK_API_VERSION_MAJOR(v) == 1 && VK_API_VERSION_MINOR(v) < 2)) {
    out.reason = std::format("the Vulkan driver is version {}.{} and FidelityFX needs 1.2",
                             VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v));
    return out;
  }

  // What the physical device supports. Plume creates the device with every core feature it supports
  // (pEnabledFeatures = what vkGetPhysicalDeviceFeatures2 returned), so the core ones read here are
  // the ones enabled; the extension features are enabled only by Plume's P1 and are compared with
  // what it reports below.
  VkPhysicalDeviceShaderFloat16Int8Features float16 = {};
  float16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
  VkPhysicalDevice16BitStorageFeatures storage16 = {};
  storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
  storage16.pNext = &float16;
  VkPhysicalDeviceSubgroupSizeControlFeatures subgroup = {};
  subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
  subgroup.pNext = &storage16;
  VkPhysicalDeviceFeatures2 features = {};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = &subgroup;
  vkGetPhysicalDeviceFeatures2(dev->physicalDevice, &features);
  const bool has_float16 = float16.shaderFloat16 && storage16.storageBuffer16BitAccess &&
                           features.features.shaderInt16;
  const bool has_subgroup_control = subgroup.subgroupSizeControl;
  out.detail += std::format(", 16-bit math {}, subgroup size control {}, extended storage formats {}",
                            has_float16 ? "yes" : "no", has_subgroup_control ? "yes" : "no",
                            features.features.shaderStorageImageExtendedFormats ? "yes" : "no");
  if (!features.features.shaderStorageImageExtendedFormats) {
    out.reason = "the Vulkan driver cannot write the two-channel 16-bit images FidelityFX uses "
                 "(shaderStorageImageExtendedFormats)";
    return out;
  }

  out.reason = ComputeCapsProblem(dev->capabilities, has_float16, has_subgroup_control);
  out.ok = out.reason.empty();
  return out;
}

}  // namespace mcla::gpu::backend::vulkan

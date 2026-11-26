// MCLA GPU renderer -- the Vulkan half of backend.h: one of the two translation
// units that reach past the Plume interface into a backend's own objects (the
// other is backend_d3d12.cpp). See backend.h for why each call exists.
//
// Design followed (not copied): reblue gpu/device.cpp CreateHostBuffer/
// CreateHostTexture/DescribeBackend/MemoryUsage and reeot gpu/device.cpp
// ReportCreationFailure, both BSD-3-Clause.

#include "backend_impl.h"

#include <algorithm>
#include <concepts>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <vector>

#include <rex/logging/log_category.h>

#include <plume_vulkan.h>

#include "device_fault_format.h"
#include "swap_chain_rules.h"
#include "temporal_dlss.h"
#include "uma_budget.h"

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

// Vulkan has no factory: with the interposer's functions loaded (backend_vulkan.cpp CreateInterface)
// a swap chain made while the frame generation plugin is loaded is Streamline's own.
bool BeginSwapChainFactory(plume::RenderDevice*, bool (*)(void**), void**, void**) {
  return false;
}

void EndSwapChainFactory(plume::RenderDevice*, void*, void*) {}

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

std::string RequirementProblem(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || dev->physicalDevice == VK_NULL_HANDLE || !vkGetPhysicalDeviceFeatures2) {
    return {};
  }
  // What the physical device supports. Plume creates the device with every core feature it supports
  // (pEnabledFeatures = what vkGetPhysicalDeviceFeatures2 returned), so a core feature the device
  // has is one that is enabled.
  VkPhysicalDeviceDescriptorIndexingFeatures indexing = {};
  indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
  VkPhysicalDeviceFeatures2 features = {};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = &indexing;
  vkGetPhysicalDeviceFeatures2(dev->physicalDevice, &features);
  const VkPhysicalDeviceFeatures& core = features.features;
  const VkPhysicalDeviceLimits& limits = dev->physicalDeviceProperties.limits;

  std::string missing;
  const auto lacks = [&missing](bool present, const char* what) {
    if (!present) {
      missing += std::format("{}{}", missing.empty() ? "" : ", ", what);
    }
  };
  // The guest shaders read their constant buffers through 64-bit addresses (shader_common.h:
  // vk::RawBufferLoad off a push constant), which is a 64-bit integer in the SPIR-V.
  lacks(core.shaderInt64, "64-bit integers in shaders (shaderInt64)");
  // The guest's textures are mostly DXT (BC1 to BC3, format.cpp), made as they are.
  lacks(core.textureCompressionBC, "BC textures (textureCompressionBC)");
  // The bindless texture and sampler sets are written while they are bound (descriptors.h: the
  // slots are rewritten as textures come and go), which their UPDATE_AFTER_BIND flag asks for.
  lacks(indexing.descriptorBindingSampledImageUpdateAfterBind,
        "updating sampled image and sampler descriptors after a set is bound "
        "(descriptorBindingSampledImageUpdateAfterBind)");
  // The host passes, the resolve conversions and the ambient occlusion bind four texture sets and
  // one more (set 0 to 4); the Vulkan minimum is 4.
  lacks(limits.maxBoundDescriptorSets >= 5, "five bound descriptor sets (maxBoundDescriptorSets)");
  return missing.empty() ? std::string() : "it lacks " + missing;
}

std::string DescribeCapabilities(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || dev->physicalDevice == VK_NULL_HANDLE || !vkGetPhysicalDeviceFeatures2) {
    return {};
  }
  VkPhysicalDeviceDescriptorIndexingFeatures indexing = {};
  indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
  VkPhysicalDeviceBufferDeviceAddressFeatures address = {};
  address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
  indexing.pNext = &address;
  VkPhysicalDeviceFeatures2 features = {};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = &indexing;
  vkGetPhysicalDeviceFeatures2(dev->physicalDevice, &features);
  const VkPhysicalDeviceFeatures& core = features.features;
  const VkPhysicalDeviceLimits& limits = dev->physicalDeviceProperties.limits;
  const auto yes = [](VkBool32 value) { return value ? "yes" : "no"; };
  // What Plume made of the ray query extensions (plume_vulkan.cpp: VK_KHR_ray_query with VK_KHR_acceleration_structure, buffer device addresses and a Vulkan 1.2 device,
  // all or none) and, where it is on, the alignment a build's scratch buffer must keep: the car ray tracing's "[gpu] car ray tracing:" line says only that a driver
  // has no ray queries, this says what Plume decided and the number the structures' builds depend on.
  const std::string ray_query =
      dev->capabilities.rayQuery
          ? std::format("yes (scratch alignment {} bytes)",
                        dev->accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment)
          : std::string("no");
  // The subgroup operations the ray traced car effects' compute shaders use (backend.h WaveOps): without all three the ray tracing stays off.
  const WaveOps waves = QueryWaveOps(device);
  const auto wave = [&waves](bool value) { return waves.known ? (waves.in_compute && value ? "yes" : "no") : "?"; };
  return std::format(
      "Vulkan 64-bit integers in shaders {}, buffer device address {}, descriptor indexing: partially "
      "bound {}, variable count {}, runtime arrays {}, sampled image update after bind {}; BC textures "
      "{}, bound descriptor sets {}, push constants {} bytes; ray queries {}, ray tracing pipelines {}; "
      "compute subgroup operations: basic {}, ballot {}, arithmetic {}",
      yes(core.shaderInt64), yes(address.bufferDeviceAddress),
      yes(indexing.descriptorBindingPartiallyBound),
      yes(indexing.descriptorBindingVariableDescriptorCount), yes(indexing.runtimeDescriptorArray),
      yes(indexing.descriptorBindingSampledImageUpdateAfterBind), yes(core.textureCompressionBC),
      limits.maxBoundDescriptorSets, limits.maxPushConstantsSize, ray_query,
      yes(dev->capabilities.raytracing), wave(waves.basic), wave(waves.ballot),
      wave(waves.arithmetic));
}

WaveOps QueryWaveOps(plume::RenderDevice* device) {
  WaveOps out;
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || dev->physicalDevice == VK_NULL_HANDLE || !vkGetPhysicalDeviceProperties2) {
    return out;
  }
  // The core 1.1 structure (the instance asks for 1.2): what the physical device supports in each stage, which the device creation does not change.
  // CHECK: not run (no build here): the three answers on this RTX and the Radeon iGPU are expected yes; Intel Arc and RADV were never run.
  VkPhysicalDeviceSubgroupProperties subgroup = {};
  subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
  VkPhysicalDeviceProperties2 properties = {};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &subgroup;
  vkGetPhysicalDeviceProperties2(dev->physicalDevice, &properties);
  out.known = true;
  out.in_compute = (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
  out.basic = (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
  out.ballot = (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0;
  out.arithmetic = (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0;
  return out;
}

AdapterIds QueryAdapterIds(plume::RenderDevice* device) {
  AdapterIds out;
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev) {
    return out;
  }
  out.vendor_id = dev->physicalDeviceProperties.vendorID;
  out.device_id = dev->physicalDeviceProperties.deviceID;
  return out;
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

static_assert(uma_budget::kMaxHeaps == VK_MAX_MEMORY_HEAPS);

// The heaps vkGetPhysicalDeviceMemoryProperties2 lists with their budgets, as uma_budget.h's list,
// and whether the adapter shares memory with the CPU. VK_EXT_memory_budget through volk (Plume's
// loader). Not through VMA's vmaGetHeapBudgets: this SDK's rexruntime DLL exports its own copy of
// VMA, and a call to vma* from this translation unit binds to that import before plume_vulkan.obj
// (which defines the same names) is loaded, which the linker refuses ("vmaGetHeapBudgets was
// replaced", measured 05/10). Plume enables the extension where the driver has it
// (memoryBudgetSupported); without it nothing is reported, and the budget response
// (vram_budget.h) runs only on a creation that failed.
struct HeapList {
  uma_budget::Heap heap[uma_budget::kMaxHeaps];
  uint32_t count = 0;
  bool shared = false;
  bool ok = false;
};

bool ReadHeaps(plume::RenderDevice* device, HeapList& out) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || !dev->memoryBudgetSupported || dev->physicalDevice == VK_NULL_HANDLE ||
      !vkGetPhysicalDeviceMemoryProperties2) {
    return false;
  }
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {};
  budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
  VkPhysicalDeviceMemoryProperties2 properties = {};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
  properties.pNext = &budget;
  vkGetPhysicalDeviceMemoryProperties2(dev->physicalDevice, &properties);
  const VkPhysicalDeviceMemoryProperties& memory = properties.memoryProperties;
  out.count = memory.memoryHeapCount < uma_budget::kMaxHeaps ? memory.memoryHeapCount
                                                              : uma_budget::kMaxHeaps;
  for (uint32_t i = 0; i < out.count; ++i) {
    uma_budget::Heap& heap = out.heap[i];
    heap.size = memory.memoryHeaps[i].size;
    heap.budget = budget.heapBudget[i];
    heap.usage = budget.heapUsage[i];
    heap.device_local = (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
  }
  // A heap the CPU can map is memory the GPU can be given too (every type of a host-only heap on an
  // APU is HOST_VISIBLE: RADV's GTT third, Windows' host heap).
  for (uint32_t t = 0; t < memory.memoryTypeCount; ++t) {
    if ((memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        memory.memoryTypes[t].heapIndex < out.count) {
      out.heap[memory.memoryTypes[t].heapIndex].host_visible = true;
    }
  }
  const plume::RenderDeviceType type = dev->getDescription().type;
  out.shared = uma_budget::IsSharedMemory(dev->getCapabilities().uma,
                                          type == plume::RenderDeviceType::INTEGRATED,
                                          type == plume::RenderDeviceType::CPU);
  out.ok = true;
  return true;
}

uma_budget::Result BudgetOf(const HeapList& list) {
  return uma_budget::Compute(list.heap, list.count, list.shared,
                             uma_budget::SystemMemoryBytes());
}

}  // namespace

VideoMemory QueryVideoMemory(plume::RenderDevice* device) {
  // A card with its own memory: the biggest device-local heap, as always. An adapter that shares
  // memory with the CPU: every heap the GPU can reach, held to a share of the RAM and floored
  // (uma_budget.h), because the heap labelled device-local is only part of it (RADV's 2/3 of the
  // Deck's carve-out plus GTT; a driver that lists only the carve-out).
  HeapList list;
  if (!ReadHeaps(device, list)) {
    return {};
  }
  const uma_budget::Result result = BudgetOf(list);
  VideoMemory out;
  out.used = result.used;
  out.budget = result.budget;
  return out;
}

std::string DescribeVideoMemory(plume::RenderDevice* device) {
  HeapList list;
  if (!ReadHeaps(device, list)) {
    return uma_budget::Describe(uma_budget::Result{}, nullptr, 0, 0) +
           "; the driver has no VK_EXT_memory_budget";
  }
  return uma_budget::Describe(BudgetOf(list), list.heap, list.count,
                              uma_budget::SystemMemoryBytes());
}

namespace {

// VulkanDevice::deviceFaultSupported exists only in a Plume that enables VK_EXT_device_fault
// (Plume branch fix/vulkan-device-fault). This file builds against the pinned Plume with or
// without it: without, the fault report is left out and the log line says so.
template <class T>
concept HasDeviceFaultFlag = requires(const T& device) {
  { device.deviceFaultSupported } -> std::convertible_to<bool>;
};
constexpr bool kPlumeEnablesDeviceFault = HasDeviceFaultFlag<plume::VulkanDevice>;

// A template, so that the branch the Plume at hand cannot compile is not looked at.
template <class T>
bool DeviceFaultEnabled(const T& device) {
  if constexpr (HasDeviceFaultFlag<T>) {
    return device.deviceFaultSupported;
  } else {
    return false;
  }
}

// Most entries read from the driver's fault report (it has a handful at most).
constexpr uint32_t kMaxFaultEntries = 32;

}  // namespace

// How a Vulkan device loss reaches the renderer. Plume's queue submit, fence wait, acquire and
// present each come back with VK_ERROR_DEVICE_LOST on a lost device, but Plume prints it to stderr
// (submit, wait) or turns it into a plain false (acquire, present), and keeps no state. Changing
// that in Plume is not needed: a loss is sticky, so a status call on any object of the device
// answers VK_ERROR_DEVICE_LOST from the moment one call has, and asking right after each call
// (Device::CheckLost, from FrameRing and the swap chain's failure paths) names the call that was
// the first to see it. The object is a fence nothing ever signals: vkGetFenceStatus is the status
// call every driver implements with the device-lost check (the spec lists VK_ERROR_DEVICE_LOST
// among its results, as for vkGetEventStatus, which was the other candidate).
void* CreateLossProbe(plume::RenderDevice* device) {
  static_assert(sizeof(VkFence) == sizeof(void*), "the probe is a VkFence held as a pointer");
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || dev->vk == VK_NULL_HANDLE || !vkCreateFence || !vkGetFenceStatus ||
      !vkDestroyFence) {
    return nullptr;
  }
  VkFenceCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  const VkResult result = vkCreateFence(dev->vk, &info, nullptr, &fence);
  if (result != VK_SUCCESS) {
    REXLOG_WARN("[gpu] the device-loss probe fence could not be made ({}): a device loss will not "
                "be noticed on Vulkan",
                device_fault::VkResultName(static_cast<int32_t>(result)));
    return nullptr;
  }
  return reinterpret_cast<void*>(fence);
}

void DestroyLossProbe(plume::RenderDevice* device, void* probe) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (dev && probe && dev->vk != VK_NULL_HANDLE) {
    vkDestroyFence(dev->vk, reinterpret_cast<VkFence>(probe), nullptr);
  }
}

int64_t DeviceRemovedReason(plume::RenderDevice* device, void* probe) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || !probe || dev->vk == VK_NULL_HANDLE) {
    return 0;
  }
  // VK_NOT_READY while the device is alive (nothing signals the fence). Out of memory and the
  // like are not a loss.
  const VkResult result = vkGetFenceStatus(dev->vk, reinterpret_cast<VkFence>(probe));
  return result == VK_ERROR_DEVICE_LOST ? static_cast<int64_t>(result) : 0;
}

// VK_EXT_device_fault: a description, the faulting addresses and the vendor's own fault codes,
// from the driver, once a command has returned VK_ERROR_DEVICE_LOST (the probe's status call is
// one). Asked for once, by Device::CheckLost, on whichever thread saw the loss first.
//
// Why not VK_NV_device_diagnostic_checkpoints or VK_AMD_buffer_marker (the other two ways to learn
// which pass a hang was in): each needs a marker recorded into every pass of every frame, through
// a new method on Plume's backend-neutral command list (and a persistent host-visible buffer for
// AMD's), which costs on the 2-CU iGPU and the Deck where frames are tight; each works for one
// vendor family only (NVIDIA; AMD and Mesa); and each answers with the last marker, which only
// helps when the renderer also names its passes -- it keeps labels only under
// mcla_gpu_pass_timing. device_fault costs nothing until a loss; the pass labels of the frames in
// flight (Renderer::ReportDeviceLost) cover the rest. Worth revisiting only if the fault report
// proves empty on the drivers that matter.
std::vector<std::string> DescribeDeviceFault(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || dev->vk == VK_NULL_HANDLE) {
    return {};
  }
  if (!DeviceFaultEnabled(*dev)) {
    return {kPlumeEnablesDeviceFault
                ? "device fault: VK_EXT_device_fault is not offered by this driver: the result "
                  "code is all it says"
                : "device fault: this Plume does not enable VK_EXT_device_fault (its branch "
                  "fix/vulkan-device-fault does): the result code is all it says"};
  }
  const auto get = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
      vkGetDeviceProcAddr ? vkGetDeviceProcAddr(dev->vk, "vkGetDeviceFaultInfoEXT") : nullptr);
  if (!get) {
    return {"device fault: vkGetDeviceFaultInfoEXT could not be loaded"};
  }
  VkDeviceFaultCountsEXT counts = {};
  counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
  VkResult result = get(dev->vk, &counts, nullptr);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    return {std::format("device fault: vkGetDeviceFaultInfoEXT (counts) answered {}",
                        device_fault::VkResultName(static_cast<int32_t>(result)))};
  }
  device_fault::FaultReport report;
  report.address_count = counts.addressInfoCount;
  report.vendor_count = counts.vendorInfoCount;
  report.vendor_binary_bytes = static_cast<uint64_t>(counts.vendorBinarySize);

  std::vector<VkDeviceFaultAddressInfoEXT> addresses(
      std::min(counts.addressInfoCount, kMaxFaultEntries));
  std::vector<VkDeviceFaultVendorInfoEXT> vendors(
      std::min(counts.vendorInfoCount, kMaxFaultEntries));
  VkDeviceFaultCountsEXT capacity = {};
  capacity.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
  capacity.addressInfoCount = static_cast<uint32_t>(addresses.size());
  capacity.vendorInfoCount = static_cast<uint32_t>(vendors.size());
  capacity.vendorBinarySize = 0;  // the crash dump is not read (and its feature is not enabled)
  VkDeviceFaultInfoEXT info = {};
  info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
  info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
  info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
  info.pVendorBinaryData = nullptr;
  result = get(dev->vk, &capacity, &info);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    return {std::format("device fault: vkGetDeviceFaultInfoEXT answered {}",
                        device_fault::VkResultName(static_cast<int32_t>(result)))};
  }
  report.description.assign(info.description, strnlen(info.description, sizeof(info.description)));
  const size_t address_count = std::min<size_t>(capacity.addressInfoCount, addresses.size());
  for (size_t i = 0; i < address_count; ++i) {
    report.addresses.push_back({static_cast<uint32_t>(addresses[i].addressType),
                                static_cast<uint64_t>(addresses[i].reportedAddress),
                                static_cast<uint64_t>(addresses[i].addressPrecision)});
  }
  const size_t vendor_count = std::min<size_t>(capacity.vendorInfoCount, vendors.size());
  for (size_t i = 0; i < vendor_count; ++i) {
    report.vendors.push_back(
        {std::string(vendors[i].description,
                     strnlen(vendors[i].description, sizeof(vendors[i].description))),
         vendors[i].vendorFaultCode, vendors[i].vendorFaultData});
  }
  return device_fault::FormatFaultReport(report);
}

std::string DescribeLossReporting(plume::RenderDevice* device, void* probe) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  const char* fault =
      !dev ? "no device"
      : DeviceFaultEnabled(*dev)
          ? "VK_EXT_device_fault is on: a loss logs the driver's fault report"
      : kPlumeEnablesDeviceFault ? "VK_EXT_device_fault is not offered by this driver"
                                 : "VK_EXT_device_fault is not enabled (this Plume does not ask for it)";
  return std::format(
      "a loss is read from {}; {}",
      probe ? "a fence status call after every submit, wait and present"
            : "nothing (the probe fence could not be made: a loss will go unnoticed)",
      fault);
}

bool NeedsSwapChainResize(const plume::RenderSwapChain& swap_chain, bool needs_recovery) {
  const auto& chain = static_cast<const plume::VulkanSwapChain&>(swap_chain);
  const SwapChainState current{chain.getWidth(), chain.getHeight(),
                               static_cast<uint32_t>(chain.pickedSurfaceFormat.format),
                               static_cast<uint32_t>(chain.pickedSurfaceFormat.colorSpace),
                               chain.getTextureCount(), static_cast<uint32_t>(chain.createdPresentMode),
                               chain.createInfo.flags};
  // Plume reuses the selected format, color space, flags and actual image count on resize.
  SwapChainState requested = current;
  requested.present_mode = static_cast<uint32_t>(chain.requiredPresentMode);
  chain.getWindowSize(requested.width, requested.height);
  // VulkanSwapChain::resize clamps the window size before creating its images. Compare that
  // extent when the two sizes differ, so a compositor clamp cannot trigger a resize every frame.
  if (!needs_recovery && requested.width && requested.height &&
      (requested.width != current.width || requested.height != current.height) &&
      chain.surface && chain.commandQueue && chain.commandQueue->device) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(chain.commandQueue->device->physicalDevice,
                                                  chain.surface, &caps) == VK_SUCCESS) {
      requested.width =
          std::clamp(requested.width, caps.minImageExtent.width, caps.maxImageExtent.width);
      requested.height =
          std::clamp(requested.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
  }
  return ShouldRebuildSwapChain(current, requested, needs_recovery);
}

bool QueryPresentStatistics(plume::RenderSwapChain*, PresentStatistics&) {
  return false;
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

namespace {

class VulkanIndirectCompute final : public IndirectCompute {
 public:
  plume::RenderBuffer* arguments() const override { return buffer.get(); }

  void Dispatch(plume::RenderCommandList& list) const override {
    const VkCommandBuffer native = static_cast<plume::VulkanCommandList&>(list).vk;
    if (native == VK_NULL_HANDLE || !buffer || buffer->vk == VK_NULL_HANDLE) return;
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer->vk;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(native, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    vkCmdDispatchIndirect(native, buffer->vk, 0);
    std::swap(barrier.srcAccessMask, barrier.dstAccessMask);
    vkCmdPipelineBarrier(native, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
  }

  std::unique_ptr<plume::VulkanBuffer> buffer;
};

}  // namespace

std::unique_ptr<IndirectCompute> CreateIndirectCompute(plume::RenderDevice* device) {
  auto* dev = static_cast<plume::VulkanDevice*>(device);
  if (!dev || dev->allocator == VK_NULL_HANDLE) return nullptr;
  auto buffer = std::make_unique<plume::VulkanBuffer>();
  buffer->device = dev;
  buffer->desc = plume::RenderBufferDesc::DefaultBuffer(sizeof(VkDispatchIndirectCommand),
      plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS);
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = sizeof(VkDispatchIndirectCommand);
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  VmaAllocationCreateInfo allocation{};
  allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateBuffer(dev->allocator, &info, &allocation, &buffer->vk, &buffer->allocation,
                      &buffer->allocationInfo) != VK_SUCCESS) return nullptr;
  buffer->setName("ssr tile dispatch arguments");
  auto result = std::make_unique<VulkanIndirectCompute>();
  result->buffer = std::move(buffer);
  return result;
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
  // Plume (larecomp branch, PR #8) keeps the usage and creation flags the image was made with.
  out.usage = uint32_t(t->imageUsage);
  out.create_flags = uint32_t(t->imageFlags);
  out.complete = out.usage != 0;
#else
  out.complete = false;
#endif
  return out;
}

void ForgetImageContents(plume::RenderTexture* texture) {
  auto* t = static_cast<plume::VulkanTexture*>(texture);
  if (t) {
    // Plume's next barrier on it takes this as the old layout: UNDEFINED, whatever it was left in.
    t->textureLayout = plume::RenderTextureLayout::UNKNOWN;
  }
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

int DeviceIndexFor(const std::string& name) {
  // Plume is not started on this path (the SDK's emulated GPU runs), so this makes the loader and a
  // throwaway instance of its own, through volk like Plume, and keeps none of it: the instance goes
  // again, and every function is taken from it by name instead of through volk's global table.
  // CHECK: not run (a PC with two GPUs on the emulated GPU's Vulkan; one GPU; no Vulkan loader).
  if (volkInitialize() != VK_SUCCESS || !vkGetInstanceProcAddr) {
    return -1;
  }
  const auto create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
  if (!create_instance) {
    return -1;
  }
  VkApplicationInfo application = {};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &application;
  VkInstance instance = VK_NULL_HANDLE;
  if (create_instance(&info, nullptr, &instance) != VK_SUCCESS || !instance) {
    return -1;
  }
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
  const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
  const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
      vkGetInstanceProcAddr(instance, "vkDestroyInstance"));

  // Plume's own order (plume_vulkan.cpp, VulkanDevice): the first device of the best type; the one
  // the user named wins at once. Indexed by VkPhysicalDeviceType: other, integrated, discrete,
  // virtual, CPU.
  constexpr int kTypeScore[] = {0, 3, 4, 2, 1};
  int chosen = -1;
  int chosen_score = 0;
  uint32_t count = 0;
  if (enumerate && get_properties && enumerate(instance, &count, nullptr) == VK_SUCCESS && count) {
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(instance, &count, devices.data()) >= VK_SUCCESS) {
      for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties = {};
        get_properties(devices[i], &properties);
        if (!name.empty() && name == properties.deviceName) {
          chosen = static_cast<int>(i);
          break;
        }
        const uint32_t type = properties.deviceType;
        if (type < std::size(kTypeScore) && kTypeScore[type] > chosen_score) {
          chosen = static_cast<int>(i);
          chosen_score = kTypeScore[type];
        }
      }
    }
  }
  if (destroy_instance) {
    destroy_instance(instance, nullptr);
  }
  return chosen;
}

}  // namespace mcla::gpu::backend::vulkan

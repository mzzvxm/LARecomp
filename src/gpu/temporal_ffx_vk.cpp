// MCLA GPU renderer -- the Vulkan half of the FidelityFX backend. See temporal_ffx_glue.h.

#include <new>

#include <vk/ffx_api_vk.h>

#include "temporal_ffx_glue.h"

namespace mcla::gpu::ffx_glue::vulkan {

bool FillBackendDesc(const backend::NativeDevice& device, BackendDesc* out) {
  static_assert(sizeof(ffxCreateBackendVKDesc) <= sizeof(BackendDesc::storage));
  if (!device.device || !device.physical_device || !device.get_device_proc_addr) {
    return false;
  }
  auto* desc = new (out->storage) ffxCreateBackendVKDesc{};
  desc->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  desc->deviceContext.vkDevice = reinterpret_cast<VkDevice>(device.device);
  desc->deviceContext.vkPhysicalDevice = reinterpret_cast<VkPhysicalDevice>(device.physical_device);
  desc->deviceContext.vkDeviceProcAddr =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(device.get_device_proc_addr);
  out->header = &desc->header;
  out->query_device = &desc->deviceContext;
  return true;
}

}  // namespace mcla::gpu::ffx_glue::vulkan

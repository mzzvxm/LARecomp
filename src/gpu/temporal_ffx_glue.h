#pragma once
// ===========================================================================
// MCLA GPU renderer -- the API-specific half of the FidelityFX backend
// ===========================================================================
// AMD's D3D12 and Vulkan headers cannot be included in one translation unit (both typedef
// PfnFfxResourceAllocatorFunc, differently), so each API's part of temporal_ffx.cpp is its own file:
// temporal_ffx_dx12.cpp and temporal_ffx_vk.cpp. They make the backend description a context is
// created with and name the device a version query is asked about; everything else is common.
// ===========================================================================

#include <cstdint>

#include <ffx_api.h>

#include "backend.h"

namespace mcla::gpu::ffx_glue {

// The backend create description of one device: storage the runtime reads until the context is
// destroyed (so this lives as long as the context and is never copied).
struct BackendDesc {
  BackendDesc() = default;
  BackendDesc(const BackendDesc&) = delete;
  BackendDesc& operator=(const BackendDesc&) = delete;

  alignas(16) unsigned char storage[96] = {};  // ffxCreateBackendDX12Desc 24 bytes, ffxCreateBackendVKDesc 40
  ffxApiHeader* header = nullptr;              // the description inside `storage`, to chain
  void* query_device = nullptr;                // ffxQueryDescGetVersions::device (DX12: the ID3D12Device, VK: a VkDeviceContext)
};

namespace dx12 {
bool FillBackendDesc(const backend::NativeDevice& device, BackendDesc* out);
}  // namespace dx12

namespace vulkan {
bool FillBackendDesc(const backend::NativeDevice& device, BackendDesc* out);
}  // namespace vulkan

}  // namespace mcla::gpu::ffx_glue

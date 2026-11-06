// MCLA GPU renderer -- the D3D12 half of the FidelityFX backend. See temporal_ffx_glue.h.

#include <new>

#include <dx12/ffx_api_dx12.h>

#include "temporal_ffx_glue.h"

namespace mcla::gpu::ffx_glue::dx12 {

bool FillBackendDesc(const backend::NativeDevice& device, BackendDesc* out) {
  static_assert(sizeof(ffxCreateBackendDX12Desc) <= sizeof(BackendDesc::storage));
  if (!device.device) {
    return false;
  }
  auto* desc = new (out->storage) ffxCreateBackendDX12Desc{};
  desc->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  desc->device = static_cast<ID3D12Device*>(device.device);
  out->header = &desc->header;
  out->query_device = desc->device;
  return true;
}

}  // namespace mcla::gpu::ffx_glue::dx12

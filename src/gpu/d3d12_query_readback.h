#pragma once

#include <d3d12.h>

#include <cstdint>
#include <cstring>
#include <span>

namespace mcla::gpu::backend::d3d12 {

template <class Resource>
HRESULT ReadQueryBytes(Resource& resource, std::span<uint64_t> results) {
  const D3D12_RANGE read_range{0, results.size_bytes()};
  void* mapped = nullptr;
  const HRESULT hr = resource.Map(0, &read_range, &mapped);
  if (FAILED(hr)) {
    return hr;
  }
  if (mapped) {
    std::memcpy(results.data(), mapped, read_range.End);
  }
  const D3D12_RANGE no_writes{0, 0};
  resource.Unmap(0, &no_writes);
  return mapped ? S_OK : E_POINTER;
}

}  // namespace mcla::gpu::backend::d3d12

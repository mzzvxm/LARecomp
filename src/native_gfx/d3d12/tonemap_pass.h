#pragma once
// MCLA Native Graphics Runtime — HDR->LDR tonemap compute pass.
// Reads an HDR scene target (e.g. the R16G16B16A16 anchor), applies
// exposure + ACES + gamma, writes an R8G8B8A8 LDR target. Used by the
// continuous present path so the raw HDR anchor is viewable on screen while
// the composite/tonemap pass selection is still being fixed.

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

namespace mcla::native_gfx {

class D3D12Context;

class TonemapPass {
 public:
  bool Initialize(D3D12Context& context);
  // Records src(HDR SRV) -> dst(LDR UAV) into cl. dst must be
  // UNORDERED_ACCESS state; src must be a non-pixel/all shader resource state.
  void Record(D3D12Context& context, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
              uint32_t src_dxgi_format, ID3D12Resource* dst, uint32_t width, uint32_t height,
              float exposure);
  bool initialized() const { return pso_ != nullptr; }

 private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;  // [0]=SRV(src) [1]=UAV(dst)
  uint32_t inc_ = 0;
};

}  // namespace mcla::native_gfx

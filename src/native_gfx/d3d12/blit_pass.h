#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — fullscreen blit
// ===========================================================================
// Presents an arbitrary render target by sampling it into the presenter output
// (R10G10B10A2), which CopyResource cannot reach from an R8G8B8A8 or HDR source
// because the formats are not copy-compatible. A fullscreen triangle draw does
// the format conversion the copy cannot.
//
// It carries its own tiny SRV heap: the source is a pooled render target whose
// descriptor lives in a different heap than the one bound for the draw, and a
// shader-visible SRV has to sit in a heap this pass owns.
// ===========================================================================

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

struct ID3D12GraphicsCommandList;

namespace mcla::native_gfx {

class D3D12Context;

class BlitPass {
 public:
  bool Initialize(D3D12Context& context, uint32_t dest_format);
  void Shutdown();
  bool initialized() const { return pso_ != nullptr; }

  // Records a fullscreen blit of `source` into whatever RTV is currently bound.
  // `source_srv_format` is the format the source is sampled through (a typeless
  // resource needs its concrete view format). The caller has already set the
  // viewport, scissor and render target.
  void Record(D3D12Context& context, ID3D12GraphicsCommandList* cl, ID3D12Resource* source,
              uint32_t source_srv_format);

 private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
  uint32_t srv_increment_ = 0;
  uint32_t dest_format_ = 0;
};

}  // namespace mcla::native_gfx

#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — FXAA
// ===========================================================================
// Same shape as GammaPass and TonemapPass: a compute pass at present time that
// reads one texture as an SRV and writes another as a UAV. Compute rather than
// a fullscreen triangle because the display buffers on this path are never
// bound as render targets -- rebinding one as an RTV while the guest-output
// blit still samples it is the continuous-mode device removal (see the
// g_owned_display note in frame_capture.cpp).
//
// Placed on the LDR composite, before the display gamma ramp. FXAA is a
// luma-edge filter and wants tonemapped values; run on the HDR scene target it
// would chase edges tonemapping is about to move. Before the ramp because the
// console's own ramp is the DC_LUT, applied at scanout after everything the GPU
// drew.
// ===========================================================================

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

namespace mcla::native_gfx {

class D3D12Context;

class FxaaPass {
 public:
  bool Initialize(D3D12Context& context);
  void Shutdown();
  bool initialized() const { return pso_ != nullptr; }

  // src (SRV, `src_dxgi_format`) -> dst (UAV, `dst_dxgi_format`), both
  // `width` x `height`. FXAA steps in SOURCE texels, so the two must match --
  // this is not a scaling pass. `edge_threshold` and `subpixel` come straight
  // from the cvars.
  void Record(D3D12Context& context, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
              uint32_t src_dxgi_format, ID3D12Resource* dst, uint32_t dst_dxgi_format,
              uint32_t width, uint32_t height, float edge_threshold, float subpixel);

 private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;  // [0]=SRV(src) [1]=UAV(dst)
  uint32_t inc_ = 0;
};

}  // namespace mcla::native_gfx

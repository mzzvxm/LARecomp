#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — display gamma ramp
// ===========================================================================
// The console runs every presented pixel through a ramp the title builds
// itself: it asks the kernel for the display gamma type (VdGetCurrentDisplayGamma,
// 2 = BT.709 in this SDK) and fills a 1024-entry 10-bit table at guest
// 0x828CDA48 (sub_82426B68). The emulated path applies it in the command
// processor's present as the DC_LUT; the no-CP native path never saw those
// register writes and presented the ramp-less image, which left every dark
// tone lifted.
//
// The table is plain guest memory, so no command processor and no hook is
// needed -- it is read straight out of the guest address space each frame.
// Measured against the emulated DC_LUT: guest[257]=194 against DC_LUT[64]=193,
// guest[512]=461 against 462, guest[770]=741 against 741. Same curve.
// ===========================================================================

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

namespace mcla::native_gfx {

class D3D12Context;

class GammaPass {
 public:
  bool Initialize(D3D12Context& context);
  void Shutdown();
  bool initialized() const { return pso_ != nullptr; }

  // Uploads the guest's ramp for this frame. `base` is the guest membase.
  // Returns false when the table is not built yet (all zero), in which case
  // the caller should keep the plain copy rather than present a black frame.
  bool UpdateRamp(D3D12Context& context, ID3D12GraphicsCommandList* cl, const uint8_t* base);

  // src (SRV) -> dst (UAV), applying the ramp uploaded by UpdateRamp.
  void Record(D3D12Context& context, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
              uint32_t src_dxgi_format, ID3D12Resource* dst, uint32_t width, uint32_t height);

 private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;  // [0]=SRV(src) [1]=SRV(ramp) [2]=UAV(dst)
  Microsoft::WRL::ComPtr<ID3D12Resource> ramp_;        // default heap, 1024 x R16_UINT
  Microsoft::WRL::ComPtr<ID3D12Resource> ramp_upload_;
  uint32_t inc_ = 0;
  bool ramp_ready_ = false;
  uint16_t last_[8] = {};  // sample points, to skip the copy when unchanged
};

}  // namespace mcla::native_gfx

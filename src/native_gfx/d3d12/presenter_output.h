#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — presenter output
// ===========================================================================
// The native runtime's "backbuffer": an R10G10B10A2 render target the frame
// renders into, plus the copy path into the Presenter's guest output texture
// (which has no RTV capability — ALLOW_UNORDERED_ACCESS only — and must be
// returned in PIXEL_SHADER_RESOURCE state).
//
//   native frame -> frame_rt (RTV) -> CopyResource -> guest output -> paint
//
// The Presenter paints on the same direct queue, so queue order makes the
// copy visible without a CPU wait. This is the permanent presentation path
// of the runtime, extracted from the Phase 2 smoke test.
// ===========================================================================

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui {
class Presenter;
}

namespace mcla::native_gfx {

class D3D12Context;

class PresenterOutput {
 public:
  bool Initialize(D3D12Context& context, uint32_t width, uint32_t height);
  void Shutdown(D3D12Context& context);
  bool initialized() const { return rt_ != nullptr; }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

  // The frame render target, in RENDER_TARGET state between BeginFrame and
  // the CopyToGuestOutput call inside Present.
  ID3D12Resource* frame_rt() const { return rt_.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE rtv() const { return rtv_; }

  // Binds the frame RT and applies full-frame viewport/scissor.
  void BindAndClear(ID3D12GraphicsCommandList* cl, const float clear_color[4]);

  // Records the copy of the frame RT into `guest_output` on the command
  // list, honoring the Presenter's state contract, and leaves the frame RT
  // back in RENDER_TARGET state for the next frame.
  void RecordCopyToGuestOutput(ID3D12GraphicsCommandList* cl, ID3D12Resource* guest_output);

  // Full present: opens a native frame if the caller hasn't, records
  // `record` (may be null for a plain clear), then refreshes the Presenter's
  // guest output and submits. Convenience used by the smoke test and by the
  // early bring-up of the draw pipeline.
  using RecordFn = void (*)(void* user, D3D12Context& context, ID3D12GraphicsCommandList* cl,
                            PresenterOutput& output);
  bool PresentFrame(D3D12Context& context, rex::ui::Presenter* presenter, RecordFn record,
                    void* user);

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> rt_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_ = {};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace mcla::native_gfx

#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — presenter output.
// Frame render target + guest-output copy path, extracted from the Phase 2
// smoke test. See presenter_output.h.

#include "presenter_output.h"

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "context.h"

namespace mcla::native_gfx {

namespace {
constexpr DXGI_FORMAT kFormat = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
}

bool PresenterOutput::Initialize(D3D12Context& context, uint32_t width, uint32_t height) {
  if (rt_) {
    return true;
  }
  width_ = width;
  height_ = height;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = kFormat;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = kFormat;
  if (FAILED(context.device()->CreateCommittedResource(
          &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&rt_)))) {
    REXLOG_ERROR("[native_gfx] frame render target creation failed ({}x{})", width, height);
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  if (FAILED(context.device()->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_)))) {
    REXLOG_ERROR("[native_gfx] frame RTV heap creation failed");
    rt_.Reset();
    return false;
  }
  rtv_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  context.device()->CreateRenderTargetView(rt_.Get(), nullptr, rtv_);
  return true;
}

void PresenterOutput::BindAndClear(ID3D12GraphicsCommandList* cl, const float clear_color[4]) {
  cl->OMSetRenderTargets(1, &rtv_, FALSE, nullptr);
  cl->ClearRenderTargetView(rtv_, clear_color, 0, nullptr);
  D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(width_), float(height_), 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, LONG(width_), LONG(height_)};
  cl->RSSetViewports(1, &viewport);
  cl->RSSetScissorRects(1, &scissor);
}

void PresenterOutput::RecordCopyToGuestOutput(ID3D12GraphicsCommandList* cl,
                                              ID3D12Resource* guest_output) {
  D3D12_RESOURCE_BARRIER barriers[2] = {};
  barriers[0].Transition.pResource = rt_.Get();
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[1].Transition.pResource = guest_output;
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(2, barriers);
  cl->CopyResource(guest_output, rt_.Get());
  std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
  std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
  cl->ResourceBarrier(2, barriers);
}

void PresenterOutput::Shutdown(D3D12Context& context) {
  if (!rt_) {
    return;
  }
  // The GPU may still be sampling/copying; hand ownership to the context's
  // deferred-release queue (or wait if the context is going down too).
  context.WaitForIdle();
  rt_.Reset();
  rtv_heap_.Reset();
  rtv_ = {};
  width_ = height_ = 0;
}

bool PresenterOutput::PresentFrame(D3D12Context& context, rex::ui::Presenter* presenter,
                                   RecordFn record, void* user) {
  if (!rt_ || !presenter) {
    return false;
  }
  return presenter->RefreshGuestOutput(
      width_, height_, width_, height_,
      [this, &context, record, user](
          rex::ui::Presenter::GuestOutputRefreshContext& refresh) -> bool {
        auto& ctx =
            static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(refresh);
        ID3D12GraphicsCommandList* cl = context.BeginFrame();
        if (!cl) {
          return false;
        }
        if (record) {
          record(user, context, cl, *this);
        }
        RecordCopyToGuestOutput(cl, ctx.resource_uav_capable());
        return context.EndFrame();
      });
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

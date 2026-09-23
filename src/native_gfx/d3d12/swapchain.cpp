#include "swapchain.h"
#include "device_manager.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mcla_native_gfx_own_swapchain, true, "MCLA/NativeGfx",
                    "Use NativeSwapChain (FLIP_DISCARD, tearing, frame latency waitable object) "
                    "presenting directly to the HWND backbuffer, eliminating intermediate blits "
                    "and enabling full ReShade / Reno DX DevKit visibility.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::native_gfx {

NativeSwapChain& NativeSwapChain::Instance() {
  static NativeSwapChain s_instance;
  return s_instance;
}

NativeSwapChain::~NativeSwapChain() {
  Shutdown();
}

void NativeSwapChain::ReleaseBuffers() {
  for (uint32_t i = 0; i < kBufferCount; ++i) {
    back_buffers_[i].Reset();
  }
}

void NativeSwapChain::Shutdown() {
  ReleaseBuffers();
  if (waitable_object_) {
    CloseHandle(waitable_object_);
    waitable_object_ = nullptr;
  }
  rtv_heap_.Reset();
  swap_chain_.Reset();
  hwnd_ = nullptr;
  width_ = 0;
  height_ = 0;
  current_back_buffer_index_ = 0;
}

bool NativeSwapChain::CreateRtvHeapAndViews(ID3D12Device* device) {
  if (!device || !swap_chain_) {
    return false;
  }

  if (!rtv_heap_) {
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = kBufferCount;
    rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_desc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_));
    if (FAILED(hr)) {
      REXLOG_ERROR("[NativeSwapChain] Failed to create RTV descriptor heap: 0x{:08X}",
                   static_cast<uint32_t>(hr));
      return false;
    }
    rtv_descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = 0; i < kBufferCount; ++i) {
    HRESULT hr = swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
    if (FAILED(hr)) {
      REXLOG_ERROR("[NativeSwapChain] Failed to get backbuffer {}: 0x{:08X}", i,
                   static_cast<uint32_t>(hr));
      return false;
    }

    device->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, rtv_handle);
    rtv_handle.ptr += rtv_descriptor_size_;
  }

  return true;
}

bool NativeSwapChain::Initialize(DeviceManager& dm, HWND hwnd, uint32_t width, uint32_t height) {
  if (IsInitialized()) {
    return true;
  }
  if (!dm.IsInitialized() || !hwnd) {
    REXLOG_ERROR("[NativeSwapChain] Invalid device manager or null HWND");
    return false;
  }

  hwnd_ = hwnd;
  width_ = (width > 0) ? width : 1280;
  height_ = (height > 0) ? height : 720;
  tearing_supported_ = dm.capabilities().tearing_supported;

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = width_;
  desc.Height = height_;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = kBufferCount;
  desc.Scaling = DXGI_SCALING_NONE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (tearing_supported_) {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_1;
  HRESULT hr = dm.dxgi_factory()->CreateSwapChainForHwnd(
      dm.direct_queue(), hwnd_, &desc, nullptr, nullptr, &swap_chain_1);
  if (FAILED(hr)) {
    // Retry with STRETCH scaling if NONE is rejected on the surface
    desc.Scaling = DXGI_SCALING_STRETCH;
    hr = dm.dxgi_factory()->CreateSwapChainForHwnd(
        dm.direct_queue(), hwnd_, &desc, nullptr, nullptr, &swap_chain_1);
    if (FAILED(hr)) {
      REXLOG_ERROR("[NativeSwapChain] CreateSwapChainForHwnd failed: 0x{:08X}",
                   static_cast<uint32_t>(hr));
      return false;
    }
  }

  dm.dxgi_factory()->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

  hr = swap_chain_1.As(&swap_chain_);
  if (FAILED(hr)) {
    REXLOG_ERROR("[NativeSwapChain] Failed to query IDXGISwapChain3: 0x{:08X}",
                 static_cast<uint32_t>(hr));
    Shutdown();
    return false;
  }

  swap_chain_->SetMaximumFrameLatency(1);
  waitable_object_ = swap_chain_->GetFrameLatencyWaitableObject();

  if (!CreateRtvHeapAndViews(dm.device())) {
    Shutdown();
    return false;
  }

  current_back_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
  REXLOG_INFO("[NativeSwapChain] Initialized: {}x{}, {} buffers, FlipDiscard, Tearing: {}, WaitableObject: {}",
              width_, height_, kBufferCount, tearing_supported_, (waitable_object_ != nullptr));
  return true;
}

bool NativeSwapChain::ResizeBuffers(DeviceManager& dm, uint32_t width, uint32_t height) {
  if (!IsInitialized() || width == 0 || height == 0) {
    return false;
  }
  if (width == width_ && height == height_) {
    return true;
  }

  ReleaseBuffers();

  UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (tearing_supported_) {
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  HRESULT hr = swap_chain_->ResizeBuffers(kBufferCount, width, height, DXGI_FORMAT_UNKNOWN, flags);
  if (FAILED(hr)) {
    REXLOG_ERROR("[NativeSwapChain] ResizeBuffers to {}x{} failed: 0x{:08X}", width, height,
                 static_cast<uint32_t>(hr));
    return false;
  }

  width_ = width;
  height_ = height;
  if (!CreateRtvHeapAndViews(dm.device())) {
    return false;
  }

  current_back_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
  REXLOG_INFO("[NativeSwapChain] Resized to {}x{}", width_, height_);
  return true;
}

void NativeSwapChain::WaitForNextFrameBuffer() {
  if (waitable_object_) {
    WaitForSingleObjectEx(waitable_object_, 1000, TRUE);
  }
}

D3D12_CPU_DESCRIPTOR_HANDLE NativeSwapChain::GetCurrentBackBufferRTV() const {
  D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += current_back_buffer_index_ * rtv_descriptor_size_;
  return handle;
}

bool NativeSwapChain::Present(bool vsync) {
  if (!IsInitialized()) {
    return false;
  }

  UINT sync_interval = vsync ? 1 : 0;
  UINT flags = 0;
  if (!vsync && tearing_supported_) {
    flags |= DXGI_PRESENT_ALLOW_TEARING;
  }

  HRESULT hr = swap_chain_->Present(sync_interval, flags);
  if (FAILED(hr)) {
    REXLOG_ERROR("[NativeSwapChain] Present failed: 0x{:08X}", static_cast<uint32_t>(hr));
    return false;
  }

  current_back_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
  return true;
}

}  // namespace mcla::native_gfx

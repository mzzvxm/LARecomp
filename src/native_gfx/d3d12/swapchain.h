#pragma once

#include <cstdint>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace mcla::native_gfx {

class DeviceManager;

class NativeSwapChain {
 public:
  static constexpr uint32_t kBufferCount = 3;

  static NativeSwapChain& Instance();

  bool Initialize(DeviceManager& dm, HWND hwnd, uint32_t width, uint32_t height);
  void Shutdown();

  bool IsInitialized() const { return swap_chain_ != nullptr; }

  bool ResizeBuffers(DeviceManager& dm, uint32_t width, uint32_t height);
  bool Present(bool vsync);

  // Synchronizes with GPU frame latency to prevent queue buildup. Call before recording next frame.
  void WaitForNextFrameBuffer();

  ID3D12Resource* GetCurrentBackBuffer() const {
    return back_buffers_[current_back_buffer_index_].Get();
  }
  D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackBufferRTV() const;
  uint32_t GetCurrentBackBufferIndex() const { return current_back_buffer_index_; }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  HWND hwnd() const { return hwnd_; }

 private:
  NativeSwapChain() = default;
  ~NativeSwapChain();

  NativeSwapChain(const NativeSwapChain&) = delete;
  NativeSwapChain& operator=(const NativeSwapChain&) = delete;

  bool CreateRtvHeapAndViews(ID3D12Device* device);
  void ReleaseBuffers();

  HWND hwnd_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t current_back_buffer_index_ = 0;
  HANDLE waitable_object_ = nullptr;
  bool tearing_supported_ = false;

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  UINT rtv_descriptor_size_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> back_buffers_[kBufferCount];
};

}  // namespace mcla::native_gfx

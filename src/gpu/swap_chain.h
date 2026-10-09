#pragma once
// ===========================================================================
// MCLA GPU renderer -- the swap chain
// ===========================================================================
// The window's presentation surface: Plume swap chain, one framebuffer and one
// "render finished" semaphore per back buffer. A semaphore per image, not per
// frame slot, because a binary semaphore may not be re-signalled while a wait
// on it is outstanding, and re-acquiring the image is what proves the wait is
// done (reblue gpu/device.h render_semaphores, reeot BuildSwapFramebuffers).
//
// kFramesInFlight + 1 back buffers with maxFrameLatency = kFramesInFlight: a
// flip-model chain needs one image beyond the frames in flight so acquiring
// the next never waits on scan-out, and without a valid latency the D3D12
// present blocked inside vsync (reblue bd74ef0).
//
// Exactly one swap chain may exist on a window (DXGI flip model), which is why
// mcla_gpu mode never creates the SDK's D3D12Presenter.
//
// Threading: Initialize on the lifecycle thread before the render thread
// starts; everything else on the render thread.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

namespace mcla::gpu {

class Device;
class FrameRing;

class SwapChain {
 public:
  struct Options {
    bool vsync = false;
    // DXGI frame-latency waitable object / VK_KHR_present_wait. Only honoured
    // when the device reports it. Note Plume's D3D12 present uses sync
    // interval 0 whenever present wait is on (plume_d3d12.cpp
    // D3D12SwapChain::present), so vsync then comes from the wait alone.
    bool present_wait = false;
  };

  SwapChain() = default;
  ~SwapChain();
  SwapChain(const SwapChain&) = delete;
  SwapChain& operator=(const SwapChain&) = delete;

  bool Initialize(Device& device, plume::RenderWindow window, const Options& options);
  // The GPU must be idle with respect to the back buffers (FrameRing::WaitIdle).
  void Shutdown();

  // True when the window size no longer matches the buffers.
  bool NeedsRebuild() const;
  // Resizes to the window. The caller has waited the GPU idle. Returns false
  // when the window has no area (minimised) or the backend refused; the chain
  // is then empty() until a later rebuild succeeds.
  bool Rebuild();
  bool empty() const;

  // Acquires the next back buffer, signalling `signal` when it is writable.
  // False when the chain is out of date or empty; the caller rebuilds and
  // retries once, and must not wait on `signal` after a failure (Vulkan does
  // not signal it then).
  bool Acquire(plume::RenderCommandSemaphore* signal, uint32_t* image);
  // Presents `image` once its render-finished semaphore is signalled.
  bool Present(uint32_t image);

  void SetVsync(bool vsync);
  void WaitForPresentSlot();  // no-op unless present wait is active

  plume::RenderTexture* texture(uint32_t image) const;
  plume::RenderFramebuffer* framebuffer(uint32_t image) const {
    return framebuffers_[image].get();
  }
  plume::RenderCommandSemaphore* render_semaphore(uint32_t image) const {
    return render_semaphores_[image].get();
  }
  uint32_t image_count() const { return static_cast<uint32_t>(framebuffers_.size()); }
  uint32_t width() const;
  uint32_t height() const;
  plume::RenderFormat format() const { return kFormat; }

 private:
  static constexpr plume::RenderFormat kFormat = plume::RenderFormat::B8G8R8A8_UNORM;

  bool BuildPerImageObjects();

  Device* device_ = nullptr;
  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores_;
  bool present_wait_ = false;
};

}  // namespace mcla::gpu

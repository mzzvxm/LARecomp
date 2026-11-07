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
// The format is B8G8R8A8_UNORM, or R10G10B10A2_UNORM in the HDR10 colour space when
// Options::hdr10 asks and the output takes it (hdr.h). It is chosen in Initialize and changes only
// in Recreate (mcla_gpu_hdr is live, Renderer::SwitchHdr): what draws into the back buffer (the
// present pass, the overlay) keeps its pipelines for FormatFor(false) and FormatFor(true) and is
// told which set to use.
//
// Threading: Initialize on the lifecycle thread before the render thread
// starts; everything else on the render thread (Recreate at a frame boundary, with the GPU idle).
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
    // Try an HDR10 swap chain first (hdr.h Wanted). When Plume cannot make one for this window
    // the SDR one is made, with a log line.
    bool hdr10 = false;
  };

  SwapChain() = default;
  ~SwapChain();
  SwapChain(const SwapChain&) = delete;
  SwapChain& operator=(const SwapChain&) = delete;

  bool Initialize(Device& device, plume::RenderWindow window, const Options& options);
  // The GPU must be idle with respect to the back buffers (FrameRing::WaitIdle).
  void Shutdown();

  // The back buffer's format for an SDR (false) or an HDR10 (true) chain.
  static constexpr plume::RenderFormat FormatFor(bool hdr10) {
    return hdr10 ? plume::RenderFormat::R10G10B10A2_UNORM : plume::RenderFormat::B8G8R8A8_UNORM;
  }

  enum class Recreated : uint8_t {
    kChanged,  // the chain is made in the format asked for
    kKept,     // the chain is made again in the format it had: the other one was refused
    kLost,     // no chain could be made; Recreate is asked again later (lost())
  };
  // Makes the chain again, in the HDR10 colour space (`hdr10`) or the SDR one, at the window's size,
  // with the vsync it had: the old chain is destroyed first (a window has one flip-model chain, and
  // on Vulkan one chain per surface), so the caller has waited the GPU idle (FrameRing::WaitIdle) and
  // holds latency::SwapChainChange. native() is another object afterwards. When the new format is
  // refused (the output does not present HDR10) the old one is made again: kKept, with the reason
  // logged; the other values are in the enum. Anything that drew into the old back buffers (the
  // framebuffers, the semaphores) is gone with it.
  Recreated Recreate(bool hdr10);
  // Recreate found no chain to make: the renderer calls it again until one exists. Nothing is
  // presented meanwhile (empty() is true).
  bool lost() const { return device_ != nullptr && !swap_chain_; }

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
  // HDR10 chains: what the display is told about the content (the peak). False otherwise, or when
  // the backend cannot pass it on.
  bool SetHdrMetadata(const plume::RenderHdrMetadata& metadata);

  plume::RenderTexture* texture(uint32_t image) const;
  // Plume's chain, for the vendor's latency reducer to name (latency_vulkan.cpp).
  plume::RenderSwapChain* native() const { return swap_chain_.get(); }
  plume::RenderFramebuffer* framebuffer(uint32_t image) const {
    return framebuffers_[image].get();
  }
  plume::RenderCommandSemaphore* render_semaphore(uint32_t image) const {
    return render_semaphores_[image].get();
  }
  uint32_t image_count() const { return static_cast<uint32_t>(framebuffers_.size()); }
  uint32_t width() const;
  uint32_t height() const;
  plume::RenderFormat format() const { return format_; }
  // The chain is HDR10 (R10G10B10A2 in ST 2084): what is drawn into it must be PQ code values.
  bool hdr() const { return hdr_; }

 private:
  static constexpr plume::RenderFormat kSdrFormat = FormatFor(false);

  // Makes swap_chain_ (HDR10 first when `hdr10`, else, or when that is refused, SDR), its per-image
  // objects and logs it; false (logged) when no chain could be made, with swap_chain_ empty.
  bool MakeChain(bool hdr10);
  bool BuildPerImageObjects();

  Device* device_ = nullptr;
  plume::RenderWindow window_{};
  bool vsync_ = false;  // what the next chain starts with
  plume::RenderFormat format_ = kSdrFormat;
  bool hdr_ = false;
  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores_;
  bool present_wait_ = false;
};

}  // namespace mcla::gpu

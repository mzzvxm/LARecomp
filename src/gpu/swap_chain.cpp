// MCLA GPU renderer -- the swap chain. See swap_chain.h.

#include "swap_chain.h"

#include <format>

#include <rex/logging.h>

#include "backend.h"
#include "device.h"
#include "frame.h"
#include "latency.h"
#include "thread_load.h"

namespace mcla::gpu {

SwapChain::~SwapChain() {
  Shutdown();
}

bool SwapChain::Initialize(Device& device, plume::RenderWindow window, const Options& options) {
  if (swap_chain_) {
    return true;
  }
  device_ = &device;
  present_wait_ = options.present_wait && device.capabilities().presentWait;
  if (options.present_wait && !present_wait_) {
    REXLOG_WARN("[gpu] present wait requested but {} does not support it; vsync paces presents",
                device.backend_info());
  }
  format_ = kSdrFormat;
  hdr_ = false;
  if (options.hdr10) {
    // The output has to take the HDR10 colour space (D3D12 CheckColorSpaceSupport, Vulkan's
    // surface formats); Plume says no by returning null, with the reason on stderr. The SDR chain
    // is the answer then, made after this one is gone (a window has one flip-model chain).
    // CHECK: with the renderer on one GPU and the display on another, whether D3D12 accepts the HDR10
    // colour space is not verified; a refusal lands in the SDR chain below with the log line.
    const plume::RenderSwapChainDesc hdr_desc(
        window, plume::RenderFormat::R10G10B10A2_UNORM, kFramesInFlight + 1, present_wait_,
        kFramesInFlight, plume::RenderSwapChainColorSpace::HDR10_ST2084);
    swap_chain_ = device.queue()->createSwapChain(hdr_desc);
    if (swap_chain_) {
      format_ = plume::RenderFormat::R10G10B10A2_UNORM;
      hdr_ = true;
    } else {
      REXLOG_WARN("[gpu] hdr: {} would not make an HDR10 swap chain for this window (the output "
                  "does not present the HDR10 colour space, or the surface does not list it; "
                  "Plume's message is on stderr); the SDR swap chain is used",
                  device.backend_info());
    }
  }
  if (!swap_chain_) {
    const plume::RenderSwapChainDesc desc(window, kSdrFormat, kFramesInFlight + 1, present_wait_,
                                          kFramesInFlight);
    swap_chain_ = device.queue()->createSwapChain(desc);
  }
  if (!swap_chain_) {
    REXLOG_ERROR("[gpu] Plume could not create the swap chain");
    device_ = nullptr;
    return false;
  }
  swap_chain_->setVsyncEnabled(options.vsync);
  if (SwapChainNeedsInitialResize()) {
    swap_chain_->resize();
  }
  if (swap_chain_->isEmpty()) {
    // A window with no client area yet still gets a chain; the first present
    // rebuilds it. Anything else empty here is a failure.
    if (swap_chain_->getWidth() != 0 && swap_chain_->getHeight() != 0) {
      REXLOG_ERROR("[gpu] the swap chain came back empty");
      swap_chain_.reset();
      device_ = nullptr;
      return false;
    }
    REXLOG_WARN("[gpu] window has no area yet; the swap chain is built at the first present");
    return true;
  }
  if (!BuildPerImageObjects()) {
    swap_chain_.reset();
    device_ = nullptr;
    return false;
  }
  REXLOG_INFO("[gpu] swap chain {}x{}, {} buffers, latency {}, vsync {}, present wait {}, {}",
              width(), height(), image_count(), kFramesInFlight, options.vsync ? "on" : "off",
              present_wait_ ? "on" : "off", hdr_ ? "HDR10" : "SDR");
  return true;
}

void SwapChain::Shutdown() {
  // Framebuffers name the back buffers, so they go before the chain.
  framebuffers_.clear();
  render_semaphores_.clear();
  swap_chain_.reset();
  device_ = nullptr;
}

bool SwapChain::NeedsRebuild() const {
  return swap_chain_ && (swap_chain_->needsResize() || framebuffers_.empty());
}

bool SwapChain::Rebuild() {
  if (!swap_chain_) {
    return false;
  }
  // Plume's D3D12 resize() releases the back buffers, so nothing may still
  // reference them.
  framebuffers_.clear();
  render_semaphores_.clear();
  if (!swap_chain_->resize()) {
    // Zero-sized (minimised) windows land here every frame; only a real size
    // that the backend refused is worth a line.
    if (swap_chain_->getWidth() != 0 && swap_chain_->getHeight() != 0) {
      REXLOG_ERROR("[gpu] swap chain resize to {}x{} failed", swap_chain_->getWidth(),
                   swap_chain_->getHeight());
    }
    return false;
  }
  if (!BuildPerImageObjects()) {
    return false;
  }
  REXLOG_INFO("[gpu] swap chain rebuilt at {}x{}", width(), height());
  return true;
}

bool SwapChain::empty() const {
  return !swap_chain_ || swap_chain_->isEmpty() || framebuffers_.empty();
}

bool SwapChain::Acquire(plume::RenderCommandSemaphore* signal, uint32_t* image) {
  if (empty()) {
    return false;
  }
  const ScopedGpuWait wait;  // may block for a free image
  // Vulkan: the acquire needs the chain to itself, so the latency reducer's calls on the update
  // thread that name it wait (latency.h).
  const latency::SwapChainUse use;
  return swap_chain_->acquireTexture(signal, image);
}

bool SwapChain::Present(uint32_t image) {
  plume::RenderCommandSemaphore* waits[] = {render_semaphores_[image].get()};
  const ScopedGpuWait wait;  // may block on the queue of presents
  // Vulkan: the present needs the chain to itself, so the latency reducer's calls on the update
  // thread that name it wait (latency.h).
  const latency::SwapChainUse use;
  return swap_chain_->present(image, waits, 1);
}

void SwapChain::SetVsync(bool vsync) {
  if (swap_chain_ && swap_chain_->isVsyncEnabled() != vsync) {
    swap_chain_->setVsyncEnabled(vsync);
  }
}

bool SwapChain::SetHdrMetadata(const plume::RenderHdrMetadata& metadata) {
  return hdr_ && swap_chain_ && swap_chain_->setHdrMetadata(metadata);
}

void SwapChain::WaitForPresentSlot() {
  if (present_wait_ && swap_chain_ && !swap_chain_->isEmpty()) {
    const ScopedGpuWait wait;
    swap_chain_->wait();
  }
}

plume::RenderTexture* SwapChain::texture(uint32_t image) const {
  return swap_chain_->getTexture(image);
}

uint32_t SwapChain::width() const {
  return swap_chain_ ? swap_chain_->getWidth() : 0;
}

uint32_t SwapChain::height() const {
  return swap_chain_ ? swap_chain_->getHeight() : 0;
}

bool SwapChain::BuildPerImageObjects() {
  framebuffers_.clear();
  render_semaphores_.clear();
  const uint32_t count = swap_chain_->getTextureCount();
  framebuffers_.reserve(count);
  render_semaphores_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    plume::RenderTexture* back = swap_chain_->getTexture(i);
    back->setName(std::format("swap chain image {}", i));
    const plume::RenderTexture* colors[] = {back};
    auto fb = device_->device()->createFramebuffer(plume::RenderFramebufferDesc(colors, 1));
    auto sem = device_->device()->createCommandSemaphore();
    if (!fb || !sem) {
      REXLOG_ERROR("[gpu] could not create the framebuffer/semaphore of back buffer {}", i);
      // All or nothing: a partial set would make empty() lie.
      framebuffers_.clear();
      render_semaphores_.clear();
      return false;
    }
    framebuffers_.push_back(std::move(fb));
    render_semaphores_.push_back(std::move(sem));
  }
  return true;
}

}  // namespace mcla::gpu

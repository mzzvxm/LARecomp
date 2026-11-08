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
  window_ = window;
  vsync_ = options.vsync;
  present_wait_ = options.present_wait && device.capabilities().presentWait;
  if (options.present_wait && !present_wait_) {
    REXLOG_WARN("[gpu] present wait requested but {} does not support it; vsync paces presents",
                device.backend_info());
  }
  if (!MakeChain(options.hdr10)) {
    device_ = nullptr;
    return false;
  }
  return true;
}

plume::RenderCommandQueue* SwapChain::Queue() const {
  return queue_ ? queue_ : device_->queue();
}

bool SwapChain::MakeChain(bool hdr10) {
  format_ = kSdrFormat;
  hdr_ = false;
  // NVIDIA Streamline's proxy factory, while DLSS frame generation is on (D3D12): the chains made in
  // this call are its own. The frame latency is then the number of buffers: its pacer presents the
  // generated frames behind the game's, and the guide asks for room for them ("raising the limit to
  // the internal back-buffer count"); Plume sets the value on the chain after it is made.
  const backend::SwapChainFactoryScope factory(device_->device(), factory_wrapper_);
  through_wrapper_ = factory.wrapped();
  const uint32_t latency = factory.wrapped() ? kFramesInFlight + 1 : kFramesInFlight;
  if (hdr10) {
    // The output has to take the HDR10 colour space (D3D12 CheckColorSpaceSupport, Vulkan's
    // surface formats); Plume says no by returning null, with the reason on stderr. The SDR chain
    // is the answer then, made after this one is gone (a window has one flip-model chain).
    // CHECK: with the renderer on one GPU and the display on another, whether D3D12 accepts the HDR10
    // colour space is not verified; a refusal lands in the SDR chain below with the log line.
    const plume::RenderSwapChainDesc hdr_desc(
        window_, FormatFor(true), kFramesInFlight + 1, present_wait_, latency,
        plume::RenderSwapChainColorSpace::HDR10_ST2084);
    swap_chain_ = Queue()->createSwapChain(hdr_desc);
    if (swap_chain_) {
      format_ = FormatFor(true);
      hdr_ = true;
    } else {
      REXLOG_WARN("[gpu] hdr: {} would not make an HDR10 swap chain for this window (the output "
                  "does not present the HDR10 colour space, or the surface does not list it; "
                  "Plume's message is on stderr); the SDR swap chain is used",
                  device_->backend_info());
    }
  }
  if (!swap_chain_) {
    const plume::RenderSwapChainDesc desc(window_, kSdrFormat, kFramesInFlight + 1, present_wait_,
                                          latency);
    swap_chain_ = Queue()->createSwapChain(desc);
  }
  if (!swap_chain_) {
    REXLOG_ERROR("[gpu] Plume could not create the swap chain");
    return false;
  }
  swap_chain_->setVsyncEnabled(vsync_);
  if (SwapChainNeedsInitialResize()) {
    swap_chain_->resize();
  }
  if (swap_chain_->isEmpty()) {
    // A window with no client area yet still gets a chain; the first present
    // rebuilds it. Anything else empty here is a failure.
    if (swap_chain_->getWidth() != 0 && swap_chain_->getHeight() != 0) {
      REXLOG_ERROR("[gpu] the swap chain came back empty");
      swap_chain_.reset();
      return false;
    }
    REXLOG_WARN("[gpu] window has no area yet; the swap chain is built at the first present");
    return true;
  }
  if (!BuildPerImageObjects()) {
    swap_chain_.reset();
    return false;
  }
  REXLOG_INFO("[gpu] swap chain {}x{}, {} buffers, latency {}, vsync {}, present wait {}, {}",
              width(), height(), image_count(), kFramesInFlight, vsync_ ? "on" : "off",
              present_wait_ ? "on" : "off", hdr_ ? "HDR10" : "SDR");
  return true;
}

// CHECK: not run (written without a build). D3D12: DXGI makes the chain for the window only once the
// old one is fully released (the framebuffers, the back buffer references, Streamline's wrapper);
// CreateSwapChainForHwnd failing here shows as "CreateSwapChainForHwnd failed" on stderr and the
// old format coming back (kKept) or "the swap chain" errors (kLost). Vulkan: the old chain and its
// surface are destroyed straight after FrameRing::WaitIdle, as Rebuild does before it resizes; the
// validation layer is the judge (VUID-vkDestroySwapchainKHR, binary semaphores of a present).
SwapChain::Recreated SwapChain::Recreate(bool hdr10) {
  if (device_ && swap_chain_) {
    vsync_ = swap_chain_->isVsyncEnabled();
  }
  return RecreateOn(queue_, hdr10, vsync_);
}

SwapChain::Recreated SwapChain::RecreateOn(plume::RenderCommandQueue* queue, bool hdr10,
                                           bool vsync, const std::function<void()>& between) {
  if (!device_) {
    return Recreated::kLost;
  }
  const bool was_hdr = hdr_;
  queue_ = queue;
  vsync_ = vsync;
  // The framebuffers name the back buffers, so they go before the chain.
  framebuffers_.clear();
  render_semaphores_.clear();
  swap_chain_.reset();
  if (between) {
    between();  // the old chain is gone, the new one not made yet
  }
  // The format asked for, else the one it had: a refused HDR10 chain falls back to SDR inside
  // MakeChain (which says why), and an SDR chain that could not be made is tried in HDR10 again.
  if (!MakeChain(hdr10) && !MakeChain(was_hdr)) {
    hdr_ = false;
    return Recreated::kLost;
  }
  return hdr_ == hdr10 ? Recreated::kChanged : Recreated::kKept;
}

void SwapChain::Shutdown() {
  // Framebuffers name the back buffers, so they go before the chain.
  framebuffers_.clear();
  render_semaphores_.clear();
  swap_chain_.reset();
  device_ = nullptr;
  queue_ = nullptr;
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
  const std::lock_guard lock(use_mutex_);
  return swap_chain_->acquireTexture(signal, image);
}

bool SwapChain::Present(uint32_t image) {
  plume::RenderCommandSemaphore* waits[] = {render_semaphores_[image].get()};
  const ScopedGpuWait wait;  // may block on the queue of presents
  // Vulkan: the present needs the chain to itself, so the latency reducer's calls on the update
  // thread that name it wait (latency.h).
  const latency::SwapChainUse use;
  const std::lock_guard lock(use_mutex_);
  return swap_chain_->present(image, waits, 1);
}

void SwapChain::SetVsync(bool vsync) {
  if (swap_chain_ && swap_chain_->isVsyncEnabled() != vsync) {
    swap_chain_->setVsyncEnabled(vsync);
  }
}

bool SwapChain::SetHdrMetadata(const plume::RenderHdrMetadata& metadata) {
  if (!hdr_ || !swap_chain_) {
    return false;
  }
  // Vulkan: vkSetHdrMetadataEXT needs the chain to itself too (as Present and Acquire).
  const latency::SwapChainUse use;
  const std::lock_guard lock(use_mutex_);
  return swap_chain_->setHdrMetadata(metadata);
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

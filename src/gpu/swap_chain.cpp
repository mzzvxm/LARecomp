// MCLA GPU renderer -- the swap chain. See swap_chain.h.

#include "swap_chain.h"

#include <algorithm>
#include <format>

#include <rex/logging/log_category.h>

#include "backend.h"
#include "device.h"
#include "frame.h"
#include "latency.h"
#include "thread_load.h"

#if defined(_WIN32)
#include <chrono>
#include <cstring>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

#if defined(LARECOMP_GPU_HAS_D3D12)
#include <plume_d3d12.h>
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include <plume_vulkan.h>
#endif

namespace mcla::gpu {

namespace {

bool CanResizeInPlace(const plume::RenderSwapChain& chain) {
#if defined(LARECOMP_GPU_HAS_D3D12)
  if (ActiveBackend() == Backend::kD3D12) {
    // The pinned Plume resize unconditionally releases every native buffer.
    const auto& dxgi = static_cast<const plume::D3D12SwapChain&>(chain);
    return dxgi.d3d && dxgi.textures.size() == dxgi.getTextureCount() &&
           std::all_of(dxgi.textures.begin(), dxgi.textures.end(),
                       [](const auto& texture) { return texture.d3d != nullptr; });
  }
#else
  (void)chain;
#endif
  return true;
}

bool HasBackBuffer(const plume::RenderTexture* texture) {
  if (backend::IsNullBacked(texture)) {
    return false;
  }
#if defined(LARECOMP_GPU_HAS_VULKAN)
  if (ActiveBackend() == Backend::kVulkan) {
    // The framebuffer constructor consumes the view even if its creation failed.
    return static_cast<const plume::VulkanTexture*>(texture)->imageView != VK_NULL_HANDLE;
  }
#endif
  return true;
}

bool HasRenderSemaphore(const plume::RenderCommandSemaphore* semaphore) {
  if (!semaphore) {
    return false;
  }
#if defined(LARECOMP_GPU_HAS_D3D12)
  if (ActiveBackend() == Backend::kD3D12) {
    return static_cast<const plume::D3D12CommandSemaphore*>(semaphore)->d3d != nullptr;
  }
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
  return static_cast<const plume::VulkanCommandSemaphore*>(semaphore)->vk != VK_NULL_HANDLE;
#else
  return false;
#endif
}

}  // namespace

#if defined(_WIN32)
namespace {

using Microsoft::WRL::ComPtr;

uint64_t FindOutputLuid(IDXGIFactory1& factory, HMONITOR monitor) {
  uint64_t owner = 0;
  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enumerated = factory.EnumAdapters1(a, &adapter);
    if (enumerated == DXGI_ERROR_NOT_FOUND) {
      return owner;
    }
    if (FAILED(enumerated) || !adapter) {
      return 0;
    }
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> output;
      const HRESULT found = adapter->EnumOutputs(o, &output);
      if (found == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(found) || !output) {
        return 0;
      }
      DXGI_OUTPUT_DESC desc{};
      if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop || desc.Monitor != monitor) {
        continue;
      }
      DXGI_ADAPTER_DESC1 adapter_desc{};
      if (FAILED(adapter->GetDesc1(&adapter_desc))) {
        return 0;
      }
      uint64_t luid = 0;
      static_assert(sizeof(luid) == sizeof(adapter_desc.AdapterLuid));
      std::memcpy(&luid, &adapter_desc.AdapterLuid, sizeof(luid));
      if (owner && owner != luid) {
        return 0;  // Ambiguous output ownership.
      }
      owner = luid;
      break;
    }
  }
}

}  // namespace

struct SwapChain::DisplayProbe {
  using Clock = std::chrono::steady_clock;
  Clock::time_point next_poll = Clock::time_point::min();
  ComPtr<IDXGIFactory1> factory;
  HMONITOR monitor = nullptr;
  uint64_t render_luid = 0;
  uint64_t output_luid = 0;
};
#endif

SwapChain::SwapChain() = default;

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
  test_d3d12_latency_one_ = options.test_d3d12_latency_one && ActiveBackend() == Backend::kD3D12;
  present_wait_ = options.present_wait && device.capabilities().presentWait;
  if (options.present_wait && !present_wait_) {
    REXLOG_WARN("[gpu] present wait requested but {} does not support it; vsync paces presents",
                device.backend_info());
  }
  if (!MakeChain(options.hdr10)) {
    device_ = nullptr;
    return false;
  }
#if defined(_WIN32)
  display_probe_ = std::make_unique<DisplayProbe>();
  display_probe_->render_luid = backend::NativeDeviceOf(device.device()).adapter_luid;
  RefreshPresentTopology();
#endif
  return true;
}

void SwapChain::RefreshPresentTopology() {
#if defined(_WIN32)
  if (!display_probe_) {
    return;
  }
  auto& probe = *display_probe_;
  const auto now = DisplayProbe::Clock::now();
  if (now < probe.next_poll) {
    return;
  }
  const bool first = probe.next_poll == DisplayProbe::Clock::time_point::min();
  probe.next_poll = now + std::chrono::seconds(1);
  // CHECK: verify the owner changes when the game moves to the ROG's direct dGPU output.
  const HMONITOR monitor = window_ ? MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST) : nullptr;
  if (probe.render_luid && monitor) {
    const bool refresh_factory = !probe.factory || !probe.factory->IsCurrent();
    if (refresh_factory &&
        FAILED(CreateDXGIFactory1(IID_PPV_ARGS(probe.factory.ReleaseAndGetAddressOf())))) {
      probe.factory.Reset();
    }
    if (refresh_factory || monitor != probe.monitor || !probe.output_luid) {
      probe.output_luid = probe.factory ? FindOutputLuid(*probe.factory.Get(), monitor) : 0;
    }
  } else {
    probe.output_luid = 0;
  }
  probe.monitor = monitor;
  const auto topology = ClassifyPresentTopology(probe.render_luid, probe.output_luid);
  if (first || topology != present_topology_) {
    const char* name = topology == PresentTopology::kCrossAdapter ? "cross-adapter"
                       : topology == PresentTopology::kSameAdapter ? "same-adapter" : "unknown";
    REXLOG_INFO("[gpu] present topology: {} (render LUID {:#x}, output LUID {:#x})",
                name, probe.render_luid, probe.output_luid);
  }
  present_topology_ = topology;
#endif
}

plume::RenderCommandQueue* SwapChain::Queue() const {
  return queue_ ? queue_ : device_->queue();
}

bool SwapChain::MakeChain(bool hdr10) {
  recovery_requested_.store(false, std::memory_order_release);
  format_ = kSdrFormat;
  hdr_ = false;
  // NVIDIA Streamline's proxy factory, while DLSS frame generation is on (D3D12): the chains made in
  // this call are its own. The frame latency is then the number of buffers: its pacer presents the
  // generated frames behind the game's, and the guide asks for room for them ("raising the limit to
  // the internal back-buffer count"); Plume sets the value on the chain after it is made.
  const backend::SwapChainFactoryScope factory(device_->device(), factory_wrapper_);
  through_wrapper_ = factory.wrapped();
  // CHECK: compare native D3D12 latency 1 and 2 and the actual displayed cadence on the ROG.
  const uint32_t latency = SwapChainFrameLatency(kFramesInFlight, factory.wrapped(),
                                                 test_d3d12_latency_one_ && !queue_);
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
              width(), height(), image_count(), latency, vsync_ ? "on" : "off",
              present_wait_ ? "on" : "off", hdr_ ? "HDR10" : "SDR");
  return true;
}

// CHECK: not run (written without a build). D3D12: DXGI makes the chain for the window only once the
// old one is fully released (the framebuffers, the back buffer references, Streamline's wrapper);
// CreateSwapChainForHwnd failing here shows as "CreateSwapChainForHwnd failed" on stderr and the
// old format coming back (kKept) or "the swap chain" errors (kLost). Vulkan: the old chain and its
// surface are destroyed straight after FrameRing::WaitIdle, as Rebuild does before it resizes; the
// validation layer is the judge (VUID-vkDestroySwapchainKHR, binary semaphores of a present).
SwapChain::Recreated SwapChain::Recreate(bool hdr10, bool vsync) {
  return RecreateOn(queue_, hdr10, vsync);
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
#if defined(_WIN32)
  display_probe_.reset();
#endif
  present_topology_ = PresentTopology::kUnknown;
  device_ = nullptr;
  queue_ = nullptr;
}

bool SwapChain::NeedsRebuild() const {
  return swap_chain_ && (empty() || backend::NeedsSwapChainResize(*swap_chain_, recovery_requested()));
}

bool SwapChain::Rebuild() {
  if (!swap_chain_) {
    return false;
  }
  // The pacer is paused: any pending failure referred to the buffers being replaced.
  recovery_requested_.store(false, std::memory_order_release);
  // Plume's D3D12 resize() releases the back buffers, so nothing may still
  // reference them.
  framebuffers_.clear();
  render_semaphores_.clear();
  if (CanResizeInPlace(*swap_chain_)) {
    const bool resized = swap_chain_->resize();
    if (resized && BuildPerImageObjects()) {
      REXLOG_INFO("[gpu] swap chain rebuilt at {}x{}", width(), height());
      return true;
    }
    if (!resized && width() && height()) {
      // A zero-area window keeps its native chain for restore.
      REXLOG_ERROR("[gpu] swap chain resize to {}x{} failed", swap_chain_->getWidth(),
                   swap_chain_->getHeight());
    }
  }
  if (!CanResizeInPlace(*swap_chain_)) {
    // A failed ResizeBuffers or GetBuffer can leave DXGI's wrappers with no resources.
    // Use the renderer's lost-chain retry. The Vulkan reducer keeps its chain wrapper.
    // CHECK: verify live recreation after a native DXGI buffer failure.
    REXLOG_ERROR("[gpu] swap chain cannot resize in place; recreating");
    swap_chain_.reset();
  }
  return false;
}

bool SwapChain::empty() const {
  return !swap_chain_ || swap_chain_->isEmpty() || framebuffers_.empty();
}

AcquireResult SwapChain::Acquire(plume::RenderCommandSemaphore* signal, uint32_t* image,
                                PresentPath path) {
  if (!image || empty()) {
    return AcquireResult::kFailed;
  }
  const ScopedGpuWait wait;  // may block for a free image
  // Vulkan: the acquire needs the chain to itself, so the latency reducer's calls on the update
  // thread that name it wait (latency.h).
  const latency::SwapChainUse use;
  const std::lock_guard lock(use_mutex_);
  const ScopedGpuWaitDetail detail(GpuWaitKind::kAcquire);
  const AcquireResult result = path == PresentPath::kVulkanTryAcquire
      ? backend::TryAcquireVulkanImage(swap_chain_.get(), signal, image)
      : (swap_chain_->acquireTexture(signal, image) ? AcquireResult::kAcquired
                                                   : AcquireResult::kFailed);
  if (result == AcquireResult::kFailed) {
    recovery_requested_.store(true, std::memory_order_release);
  }
  return result;
}

PresentResult SwapChain::Present(uint32_t image, PresentPath path) {
  if (!swap_chain_ || image >= render_semaphores_.size()) {
    return PresentResult::kFailed;
  }
  const ScopedGpuWait wait;  // may block on the queue of presents
  // Vulkan: the present needs the chain to itself, so the latency reducer's calls on the update
  // thread that name it wait (latency.h).
  const latency::SwapChainUse use;
  const std::lock_guard lock(use_mutex_);
  PresentResult result;
  if (path == PresentPath::kDxgiNowait) {
    result = backend::PresentD3D12Nowait(swap_chain_.get());
  } else {
    plume::RenderCommandSemaphore* waits[] = {render_semaphores_[image].get()};
    // Plume's call includes its own Vulkan queue lock. The other waits are measured separately.
    const ScopedGpuWaitDetail detail(GpuWaitKind::kPresentCall);
    result = swap_chain_->present(image, waits, 1) ? PresentResult::kPresented : PresentResult::kFailed;
  }
  if (result == PresentResult::kFailed) {
    recovery_requested_.store(true, std::memory_order_release);
  }
  return result;
}

void SwapChain::SetVsync(bool vsync) {
  if (swap_chain_ && vsync_ != vsync) {
    swap_chain_->setVsyncEnabled(vsync);
    vsync_ = vsync;
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
    const ScopedGpuWaitDetail detail(GpuWaitKind::kPresentSlot);
    swap_chain_->wait();
  }
}

bool SwapChain::QueryPresentStatistics(backend::PresentStatistics& out) const {
  return swap_chain_ && backend::QueryPresentStatistics(swap_chain_.get(), out);
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
  auto* device = device_ ? device_->device() : nullptr;
  if (!device || !swap_chain_ || swap_chain_->isEmpty()) {
    return false;
  }
  const uint32_t count = swap_chain_->getTextureCount();
  if (!count) {
    REXLOG_ERROR("[gpu] the swap chain reported no back buffers");
    return false;
  }
  // The caller has already released the previous set. Publish the next set only
  // after every back buffer and both of its dependent objects are usable.
  // CHECK: confirm live Vulkan recovery after an image-view or allocation failure.
  decltype(render_semaphores_) render_semaphores;
  decltype(framebuffers_) framebuffers;
  framebuffers.reserve(count);
  render_semaphores.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    plume::RenderTexture* back = swap_chain_->getTexture(i);
    if (!HasBackBuffer(back)) {
      REXLOG_ERROR("[gpu] swap chain back buffer {} is unavailable", i);
      return false;
    }
    back->setName(std::format("swap chain image {}", i));
    const plume::RenderTexture* colors[] = {back};
    auto fb = device->createFramebuffer(plume::RenderFramebufferDesc(colors, 1));
    if (backend::IsNullBacked(fb.get())) {
      REXLOG_ERROR("[gpu] could not create the framebuffer of back buffer {}", i);
      return false;
    }
    auto sem = device->createCommandSemaphore();
    if (!HasRenderSemaphore(sem.get())) {
      REXLOG_ERROR("[gpu] could not create the semaphore of back buffer {}", i);
      return false;
    }
    framebuffers.push_back(std::move(fb));
    render_semaphores.push_back(std::move(sem));
  }
  framebuffers_ = std::move(framebuffers);
  render_semaphores_ = std::move(render_semaphores);
  return true;
}

}  // namespace mcla::gpu

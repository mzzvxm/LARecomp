// MCLA GPU renderer -- the Vulkan half of the latency reducer: NVIDIA Reflex
// through VK_NV_low_latency2 and AMD Anti-Lag 2 through VK_AMD_anti_lag. See
// latency.h and latency_backend.h.
//
// The device asks for the extensions (and the features they need) itself, in
// Plume: VulkanDevice::nvLowLatency / amdAntiLag say what it got, and the swap
// chain is made in the latency mode (VulkanSwapChain::latencyMode). Here:
// vkSetLatencySleepModeNV sets the mode, vkLatencySleepNV signals a timeline
// semaphore that is waited for here (bounded), vkSetLatencyMarkerNV carries the
// markers, and a frame's PRESENT_START hands its id to the swap chain, which
// sends it with the present (VkPresentIdKHR). Anti-Lag 2 is vkAntiLagUpdateAMD
// with the input stage before the frame and the present stage before its present.
//
// Threads: the calls here that name the swap chain run on the update thread and the render
// thread, while the render thread's present and acquire need the chain "externally synchronized"
// (the spec; the validation layer's threading check flags an overlap). One shared_mutex: these
// calls hold it shared, for the driver call only, and the present, the acquire and the rebuild hold
// it exclusively (BeginSwapChainUse / BeginSwapChainChange). The wait on the sleep semaphore after
// vkLatencySleepNV is outside it.
//
// Design followed (not copied): the MCLA fork's vulkan_low_latency.cpp, written by
// the same author. Not ported: its explicit attribution of each submission to
// its frame (VkLatencySubmissionPresentIdNV), which the fork measured as a small
// gain; here the markers bracket the one thread that submits, so the driver's own
// attribution has what it needs.

#include "latency_backend.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include <rex/logging.h>

#include <plume_vulkan.h>

namespace mcla::gpu::latency {

namespace {

class VulkanReducer final : public Reducer {
 public:
  static std::shared_ptr<VulkanReducer> Create(plume::VulkanDevice* device,
                                               plume::VulkanSwapChain* swap_chain,
                                               std::string& why) {
    if (!device->nvLowLatency && !device->amdAntiLag) {
      switch (device->getDescription().vendor) {
        case plume::RenderDeviceVendor::NVIDIA:
          why = "the device has no VK_NV_low_latency2 with its features (a newer NVIDIA driver has it)";
          break;
        case plume::RenderDeviceVendor::AMD:
          why = "the device has no VK_AMD_anti_lag (a newer Radeon driver has it)";
          break;
        default:
          why = "only NVIDIA GPUs (Reflex) and AMD GPUs (Anti-Lag 2) have a reducer the game can use";
          break;
      }
      return nullptr;
    }
    std::shared_ptr<VulkanReducer> reducer(new VulkanReducer(device, swap_chain));
    if (device->nvLowLatency) {
      reducer->nvidia_ = true;
      if (swap_chain->desc.enablePresentWait) {
        // Plume numbers the presents itself then (mcla_gpu_present_wait), and the markers' ids
        // are the presents'.
        why = "mcla_gpu_present_wait numbers the presents with ids of its own; turn it off to use Reflex";
        return nullptr;
      }
      reducer->set_sleep_mode_ = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
          vkGetDeviceProcAddr(device->vk, "vkSetLatencySleepModeNV"));
      reducer->sleep_fn_ = reinterpret_cast<PFN_vkLatencySleepNV>(
          vkGetDeviceProcAddr(device->vk, "vkLatencySleepNV"));
      reducer->set_marker_ = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(
          vkGetDeviceProcAddr(device->vk, "vkSetLatencyMarkerNV"));
      if (!reducer->set_sleep_mode_ || !reducer->sleep_fn_ || !reducer->set_marker_) {
        why = "the driver lists VK_NV_low_latency2 but not its functions";
        return nullptr;
      }
      VkSemaphoreTypeCreateInfo type_info = {};
      type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
      type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
      type_info.initialValue = 0;
      VkSemaphoreCreateInfo create_info = {};
      create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      create_info.pNext = &type_info;
      if (vkCreateSemaphore(device->vk, &create_info, nullptr, &reducer->sleep_semaphore_) !=
          VK_SUCCESS) {
        why = "could not create the timeline semaphore the driver's wait signals";
        return nullptr;
      }
      // With a swap chain to ask, the neutral mode (Reflex off, no limit) tells whether the driver
      // takes the latency mode on it, without turning anything on. A window with no area yet has
      // no chain; the first mode set then finds out (a refusal three times switches it off).
      // CHECK: not run here.
      if (swap_chain->vk != VK_NULL_HANDLE) {
        if (!swap_chain->latencyMode) {
          why = "the driver made the swap chain without the latency mode";
          return nullptr;
        }
        const VkResult result = reducer->SetSleepModeLocked(false, false);
        if (result != VK_SUCCESS) {
          why = "vkSetLatencySleepModeNV refused the neutral mode (VkResult " +
                std::to_string(int32_t(result)) + ")";
          return nullptr;
        }
      }
      return reducer;
    }
    reducer->anti_lag_fn_ = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(
        vkGetDeviceProcAddr(device->vk, "vkAntiLagUpdateAMD"));
    if (!reducer->anti_lag_fn_) {
      why = "the driver lists VK_AMD_anti_lag but not vkAntiLagUpdateAMD";
      return nullptr;
    }
    return reducer;
  }

  ~VulkanReducer() override { Shutdown(); }

  const char* Name() const override { return nvidia_ ? "NVIDIA Reflex" : "AMD Anti-Lag 2"; }
  bool IsReflex() const override { return nvidia_; }
  bool SupportsBoost() const override { return nvidia_; }
  bool OnByDefault() const override { return nvidia_; }

  bool SetMode(bool on, bool boost) override {
    std::shared_lock lock(mutex_);
    if (shut_down_) {
      return true;
    }
    mode_set_ = true;
    mode_on_ = on;
    mode_boost_ = boost;
    if (nvidia_) {
      if (swap_chain_->vk == VK_NULL_HANDLE) {
        // Told to the chain when there is one (EndSwapChainChange).
        return true;
      }
      if (!swap_chain_->latencyMode) {
        return false;
      }
      const VkResult result = SetSleepModeLocked(on, boost);
      if (result != VK_SUCCESS) {
        REXLOG_WARN("[gpu] low latency: vkSetLatencySleepModeNV failed ({})", int32_t(result));
        return false;
      }
      return true;
    }
    // Not on: left to the driver's own setting (the player's Adrenalin choice), not forced off.
    anti_lag_on_.store(on, std::memory_order_relaxed);
    VkAntiLagDataAMD data = {};
    data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
    data.mode = on ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_DRIVER_CONTROL_AMD;
    data.maxFPS = 0;  // the game's own limiter holds the cap
    anti_lag_fn_(device_->vk, &data);
    return true;
  }

  bool Sleep(uint64_t frame_id) override {
    if (nvidia_) {
      uint64_t value;
      // Held for the whole call, the wait included, so that Shutdown does not destroy the
      // semaphore under it; the swap chain's lock is only held for the driver call (a wait never
      // holds a swap chain change off).
      std::shared_lock semaphore_lock(semaphore_mutex_);
      {
        std::shared_lock lock(mutex_);
        if (shut_down_ || swap_chain_->vk == VK_NULL_HANDLE || !swap_chain_->latencyMode ||
            sleep_semaphore_ == VK_NULL_HANDLE) {
          return false;
        }
        value = ++sleep_value_;
        VkLatencySleepInfoNV info = {};
        info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
        info.signalSemaphore = sleep_semaphore_;
        info.value = value;
        // CHECK: vkLatencySleepNV returns at once and the driver signals the semaphore later; if it
        // blocked inside the call, the render thread's present would wait behind it here.
        if (sleep_fn_(device_->vk, swap_chain_->vk, &info) != VK_SUCCESS) {
          return false;
        }
      }
      // Bounded: a driver that never signals must not hold the game. A little over what
      // latency.cpp counts as a long wait (a quarter of a second), so a wait that times out counts
      // as one.
      const uint64_t timeout_ns = (250'000ull + 20'000ull) * 1000ull;
      VkSemaphoreWaitInfo wait_info = {};
      wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &sleep_semaphore_;
      wait_info.pValues = &value;
      return vkWaitSemaphores(device_->vk, &wait_info, timeout_ns) == VK_SUCCESS;
    }
    std::shared_lock lock(mutex_);
    if (shut_down_) {
      return false;
    }
    SendAntiLag(VK_ANTI_LAG_STAGE_INPUT_AMD, frame_id);
    return true;
  }

  void SetMarker(Marker marker, uint64_t frame_id) override {
    std::shared_lock lock(mutex_);
    if (shut_down_) {
      return;
    }
    if (!nvidia_) {
      // The present stage goes just before the vkQueuePresentKHR of the frame's image.
      if (marker == Marker::kPresentStart) {
        SendAntiLag(VK_ANTI_LAG_STAGE_PRESENT_AMD, frame_id);
      }
      return;
    }
    if (swap_chain_->vk == VK_NULL_HANDLE || !swap_chain_->latencyMode) {
      return;
    }
    VkLatencyMarkerNV type;
    switch (marker) {
      case Marker::kSimulationStart:
        type = VK_LATENCY_MARKER_SIMULATION_START_NV;
        break;
      case Marker::kSimulationEnd:
        type = VK_LATENCY_MARKER_SIMULATION_END_NV;
        break;
      case Marker::kRenderSubmitStart:
        type = VK_LATENCY_MARKER_RENDERSUBMIT_START_NV;
        break;
      case Marker::kRenderSubmitEnd:
        type = VK_LATENCY_MARKER_RENDERSUBMIT_END_NV;
        break;
      case Marker::kPresentStart:
        type = VK_LATENCY_MARKER_PRESENT_START_NV;
        // The present that follows on this thread carries the frame's id (Plume's present()).
        // Only the render thread comes here (OnPresentBegin) and it is the one that presents, so
        // nothing else reads the field.
        swap_chain_->latencyPresentId = frame_id;
        break;
      case Marker::kPresentEnd:
        type = VK_LATENCY_MARKER_PRESENT_END_NV;
        break;
      case Marker::kInputSample:
        type = VK_LATENCY_MARKER_INPUT_SAMPLE_NV;
        break;
      default:
        return;
    }
    VkSetLatencyMarkerInfoNV info = {};
    info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
    info.presentID = frame_id;
    info.marker = type;
    set_marker_(device_->vk, swap_chain_->vk, &info);
  }

  // The swap chain's handle changes in Rebuild: calls that need it wait, and the sleep mode,
  // which belongs to the chain, is told again to the new one.
  void BeginSwapChainChange() override { mutex_.lock(); }

  void EndSwapChainChange() override {
    if (nvidia_ && mode_set_ && swap_chain_->vk != VK_NULL_HANDLE && swap_chain_->latencyMode) {
      const VkResult result = SetSleepModeLocked(mode_on_, mode_boost_);
      if (result != VK_SUCCESS) {
        REXLOG_WARN("[gpu] low latency: vkSetLatencySleepModeNV failed on the rebuilt swap chain "
                    "({})",
                    int32_t(result));
      }
    }
    mutex_.unlock();
  }

  // The render thread's present and acquire (SwapChainUse): the chain to themselves. Only NVIDIA's
  // calls name the chain; Anti-Lag 2's do not, so nothing is held for it.
  void BeginSwapChainUse() override {
    if (nvidia_) {
      mutex_.lock();
    }
  }

  void EndSwapChainUse() override {
    if (nvidia_) {
      mutex_.unlock();
    }
  }

  // After the calls in progress (the wait for the driver's signal is bounded) and for good.
  void Shutdown() override {
    std::unique_lock semaphore_lock(semaphore_mutex_);
    std::unique_lock lock(mutex_);
    shut_down_ = true;
    if (sleep_semaphore_ != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_->vk, sleep_semaphore_, nullptr);
      sleep_semaphore_ = VK_NULL_HANDLE;
    }
  }

 private:
  VulkanReducer(plume::VulkanDevice* device, plume::VulkanSwapChain* swap_chain)
      : device_(device), swap_chain_(swap_chain) {}

  // With mutex_ held (shared or exclusive), on a chain in the latency mode.
  VkResult SetSleepModeLocked(bool on, bool boost) {
    VkLatencySleepModeInfoNV mode = {};
    mode.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
    mode.lowLatencyMode = on ? VK_TRUE : VK_FALSE;
    mode.lowLatencyBoost = (on && boost) ? VK_TRUE : VK_FALSE;
    return set_sleep_mode_(device_->vk, swap_chain_->vk, &mode);
  }

  // With mutex_ held.
  void SendAntiLag(VkAntiLagStageAMD stage, uint64_t frame_id) {
    VkAntiLagPresentationInfoAMD presentation = {};
    presentation.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
    presentation.stage = stage;
    presentation.frameIndex = frame_id;
    VkAntiLagDataAMD data = {};
    data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
    data.mode = anti_lag_on_.load(std::memory_order_relaxed) ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_DRIVER_CONTROL_AMD;
    data.maxFPS = 0;
    data.pPresentationInfo = &presentation;
    anti_lag_fn_(device_->vk, &data);
  }

  plume::VulkanDevice* device_;
  plume::VulkanSwapChain* swap_chain_;
  bool nvidia_ = false;
  PFN_vkSetLatencySleepModeNV set_sleep_mode_ = nullptr;
  PFN_vkLatencySleepNV sleep_fn_ = nullptr;
  PFN_vkSetLatencyMarkerNV set_marker_ = nullptr;
  PFN_vkAntiLagUpdateAMD anti_lag_fn_ = nullptr;
  VkSemaphore sleep_semaphore_ = VK_NULL_HANDLE;
  uint64_t sleep_value_ = 0;  // the update thread's
  // The swap chain's handle (VulkanSwapChain::vk) is read under this, shared, by every call that
  // names the chain; Rebuild, the present and the acquire hold it exclusively (see the top of the
  // file). The mode below is written under it too, by the update thread, and read by
  // EndSwapChainChange under the exclusive hold.
  std::shared_mutex mutex_;
  std::shared_mutex semaphore_mutex_;
  bool shut_down_ = false;
  bool mode_set_ = false;
  bool mode_on_ = false;
  bool mode_boost_ = false;
  std::atomic<bool> anti_lag_on_{false};
};

}  // namespace

std::shared_ptr<Reducer> CreateVulkanReducer(plume::RenderDevice* device,
                                             plume::RenderSwapChain* swap_chain,
                                             std::string& why) {
  auto* vulkan_device = static_cast<plume::VulkanDevice*>(device);
  auto* vulkan_swap_chain = static_cast<plume::VulkanSwapChain*>(swap_chain);
  if (!vulkan_device || !vulkan_swap_chain) {
    why = "no Vulkan device or swap chain";
    return nullptr;
  }
  return VulkanReducer::Create(vulkan_device, vulkan_swap_chain, why);
}

}  // namespace mcla::gpu::latency

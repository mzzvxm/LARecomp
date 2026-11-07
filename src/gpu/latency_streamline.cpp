// MCLA GPU renderer -- NVIDIA Reflex through Streamline: the latency reducer while DLSS frame
// generation runs. See latency.h and latency_backend.h.
//
// DLSS frame generation requires Reflex through Streamline ("an existing Reflex integration that does
// not use Streamline cannot be used with DLSS-G", the Streamline guide's section 8): its PC latency
// markers, with the frame index the constants and the tags of the frame carry, are how Streamline
// knows which frame a present belongs to, and its sleep paces the game for the generated frames.
// This reducer makes the same calls as the NvAPI one (latency_d3d12.cpp) and the VK_NV_low_latency2
// one (latency_vulkan.cpp), through Streamline's Reflex and PC latency plugins (temporal_dlss.h
// dlss::reflex). It is made when frame generation goes on and given up when it goes off; the
// device's own reducer is put aside in between (latency.cpp UseStreamline), never both at once.
//
// Threads: as the Reducer interface says: the update thread sleeps and marks, the guest and host
// render threads mark. Streamline's sleep and markers are thread safe and take no lock of ours.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include <rex/logging.h>

#include "latency_backend.h"
#include "temporal_dlss.h"

namespace mcla::gpu::latency {

namespace {

// A call into Streamline that took long is said (at most one line a second). In run next6-vk-dlssg the
// game and its present stopped for the 8 s of a DLSS feature's creation on Vulkan, and the logs do not
// say what the render thread was waiting in; this says whether one of the latency calls was it.
// CHECK: that a Vulkan start with DLSS frame generation saved, played through the feature's creation
// (about 8 s into the first scene), has no stall; if it has, whether this line names a call.
class SlowCall {
 public:
  explicit SlowCall(const char* name) : name_(name), start_(std::chrono::steady_clock::now()) {}
  ~SlowCall() {
    const auto taken = std::chrono::steady_clock::now() - start_;
    if (taken < std::chrono::milliseconds(300)) {
      return;
    }
    static std::atomic<int64_t> last_said_ms{-1'000'000};
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    int64_t before = last_said_ms.load(std::memory_order_relaxed);
    if (now_ms - before >= 1000 &&
        last_said_ms.compare_exchange_strong(before, now_ms, std::memory_order_relaxed)) {
      REXLOG_WARN("[gpu] low latency: Streamline's {} took {} ms on this thread", name_,
                  std::chrono::duration_cast<std::chrono::milliseconds>(taken).count());
    }
  }
  SlowCall(const SlowCall&) = delete;
  SlowCall& operator=(const SlowCall&) = delete;

 private:
  const char* name_;
  std::chrono::steady_clock::time_point start_;
};

class StreamlineReducer final : public Reducer {
 public:
  const char* Name() const override { return "NVIDIA Reflex (Streamline)"; }
  bool IsReflex() const override { return true; }
  bool SupportsBoost() const override { return true; }
  bool OnByDefault() const override { return true; }

  bool SetMode(bool on, bool boost) override {
    busy_.store(false, std::memory_order_relaxed);
    if (!on && !ever_on_) {
      return true;  // never taken: nothing to leave
    }
    std::string why;
    const dlss::ReflexModeResult result = dlss::reflex::SetMode(on, boost, &why);
    if (result == dlss::ReflexModeResult::kBusy) {
      busy_.store(true, std::memory_order_relaxed);
      return false;
    }
    if (result == dlss::ReflexModeResult::kRefused) {
      REXLOG_WARN("[gpu] low latency: Streamline refused the Reflex mode: {}", why);
      return false;
    }
    if (on) {
      ever_on_ = true;
    }
    return true;
  }

  bool Busy() const override { return busy_.load(std::memory_order_relaxed); }

  // (The sleep is meant to block for up to a frame; only a wait far beyond that is said.)
  bool Sleep(uint64_t frame_id) override {
    const SlowCall timed("Reflex sleep");
    return dlss::reflex::Sleep(uint32_t(frame_id));
  }

  void SetMarker(Marker marker, uint64_t frame_id) override {
    dlss::ReflexMarker mapped = dlss::ReflexMarker::kSimulationStart;
    switch (marker) {
      case Marker::kSimulationStart: mapped = dlss::ReflexMarker::kSimulationStart; break;
      case Marker::kSimulationEnd: mapped = dlss::ReflexMarker::kSimulationEnd; break;
      case Marker::kRenderSubmitStart: mapped = dlss::ReflexMarker::kRenderSubmitStart; break;
      case Marker::kRenderSubmitEnd: mapped = dlss::ReflexMarker::kRenderSubmitEnd; break;
      case Marker::kPresentStart: mapped = dlss::ReflexMarker::kPresentStart; break;
      case Marker::kPresentEnd: mapped = dlss::ReflexMarker::kPresentEnd; break;
      case Marker::kInputSample: mapped = dlss::ReflexMarker::kInputSample; break;
    }
    const SlowCall timed("latency marker");
    dlss::reflex::SetMarker(mapped, uint32_t(frame_id));
  }

 private:
  bool ever_on_ = false;
  std::atomic<bool> busy_{false};  // the update thread's SetMode; the render thread's may follow it
};

}  // namespace

std::shared_ptr<Reducer> CreateStreamlineReducer(std::string& why) {
  if (!dlss::reflex::Available()) {
    why = "Streamline's Reflex and PC latency plugins are not running";
    return nullptr;
  }
  return std::make_shared<StreamlineReducer>();
}

}  // namespace mcla::gpu::latency

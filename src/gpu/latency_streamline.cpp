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

#include <memory>
#include <string>

#include "latency_backend.h"
#include "temporal_dlss.h"

namespace mcla::gpu::latency {

namespace {

class StreamlineReducer final : public Reducer {
 public:
  const char* Name() const override { return "NVIDIA Reflex (Streamline)"; }
  bool IsReflex() const override { return true; }
  bool SupportsBoost() const override { return true; }
  bool OnByDefault() const override { return true; }

  bool SetMode(bool on, bool boost) override {
    if (!on && !ever_on_) {
      return true;  // never taken: nothing to leave
    }
    const bool ok = dlss::reflex::SetMode(on, boost);
    if (ok && on) {
      ever_on_ = true;
    }
    return ok;
  }

  bool Sleep(uint64_t frame_id) override { return dlss::reflex::Sleep(uint32_t(frame_id)); }

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
    dlss::reflex::SetMarker(mapped, uint32_t(frame_id));
  }

 private:
  bool ever_on_ = false;
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

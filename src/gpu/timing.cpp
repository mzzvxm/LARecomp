// MCLA GPU renderer -- GPU timestamps per frame. See timing.h.
//
// Design followed: reeot gpu/gpu_timing.cpp (BSD-3-Clause) -- a timestamp pool
// per frame slot, read back once the slot's fence is waited.

#include "timing.h"

#include <format>

#include <rex/logging.h>

#include "backend.h"
#include "device.h"

namespace mcla::gpu {

GpuTiming::~GpuTiming() {
  Shutdown();
}

void GpuTiming::Initialize(Device& device) {
  if (available_) {
    return;
  }
  if (!backend::TimestampsSupported(device.device())) {
    REXLOG_WARN("[gpu] {} cannot write timestamps on its graphics queue; GPU time will not be "
                "reported",
                device.backend_info());
    return;
  }
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    pools_[i] = device.CreateQueryPool(kTimestamps, "frame timestamps");
    if (!pools_[i]) {
      REXLOG_ERROR("[gpu] GPU time will not be reported: the timestamp pools could not be made");
      Shutdown();
      return;
    }
    pending_[i] = false;
  }
  available_ = true;
}

void GpuTiming::Shutdown() {
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    pools_[i].reset();
    pending_[i] = false;
  }
  available_ = false;
}

void GpuTiming::Harvest(uint32_t slot) {
  if (!pending_[slot]) {
    return;
  }
  pending_[slot] = false;
  pools_[slot]->queryResults();
  const uint64_t* t = pools_[slot]->getResults();
  if (t[kFrameEnd] < t[kFrameBegin] || t[kOverlayEnd] < t[kOverlayBegin]) {
    // A counter that ran backwards (a GPU clock reset, a TDR recovery) gives
    // no duration; counting it would poison the average.
    ++discarded_;
    return;
  }
  frame_ns_ += t[kFrameEnd] - t[kFrameBegin];
  overlay_ns_ += t[kOverlayEnd] - t[kOverlayBegin];
  ++frames_;
}

void GpuTiming::BeginFrame(plume::RenderCommandList* list, uint32_t slot) {
  if (!available_ || !list) {
    return;
  }
  Harvest(slot);
  list->resetQueryPool(pools_[slot].get(), 0, kTimestamps);
  list->writeTimestamp(pools_[slot].get(), kFrameBegin);
}

void GpuTiming::Mark(plume::RenderCommandList* list, uint32_t slot, Timestamp timestamp) {
  if (!available_ || !list) {
    return;
  }
  list->writeTimestamp(pools_[slot].get(), timestamp);
}

void GpuTiming::OnSubmitted(uint32_t slot) {
  if (available_) {
    pending_[slot] = true;
  }
}

std::string GpuTiming::TakeSummary() {
  if (!available_) {
    return "GPU time unavailable";
  }
  if (frames_ == 0) {
    return "GPU time: no frame measured yet";
  }
  std::string text = std::format("GPU {:.3f} ms/frame (overlay {:.3f} ms) over {} frames",
                                 double(frame_ns_) / double(frames_) / 1e6,
                                 double(overlay_ns_) / double(frames_) / 1e6, frames_);
  if (discarded_) {
    text += std::format(", {} discarded", discarded_);
  }
  frame_ns_ = overlay_ns_ = frames_ = discarded_ = 0;
  return text;
}

}  // namespace mcla::gpu

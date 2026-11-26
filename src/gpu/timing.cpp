// MCLA GPU renderer -- GPU timestamps per frame. See timing.h.
//
// Design followed: reeot gpu/gpu_timing.cpp (BSD-3-Clause) -- a timestamp pool
// per frame slot, read back once the slot's fence is waited.

#include "timing.h"

#include <format>

#include <rex/logging/log_category.h>

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
  device_ = &device;
}

void GpuTiming::Shutdown() {
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    pools_[i].reset();
    pending_[i] = false;
    city_pools_[i].reset();
    city_pool_tried_[i] = false;
    city_pending_[i] = false;
    city_triangles_[i] = 0;
  }
  available_ = false;
  device_ = nullptr;
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
  city_triangles_[slot] = 0;
  list->resetQueryPool(pools_[slot].get(), 0, kTimestamps);
  list->writeTimestamp(pools_[slot].get(), kFrameBegin);
  written_[slot] = 1u << kFrameBegin;
}

void GpuTiming::Mark(plume::RenderCommandList* list, uint32_t slot, Timestamp timestamp) {
  if (!available_ || !list) {
    return;
  }
  list->writeTimestamp(pools_[slot].get(), timestamp);
  written_[slot] |= 1u << timestamp;
}

void GpuTiming::CompleteFrame(plume::RenderCommandList* list, uint32_t slot) {
  if (!available_ || !list) {
    return;
  }
  for (uint32_t t = 0; t < kTimestamps; ++t) {
    if ((written_[slot] & (1u << t)) == 0) {
      Mark(list, slot, Timestamp(t));
    }
  }
}

void GpuTiming::OnSubmitted(uint32_t slot) {
  if (available_) {
    pending_[slot] = true;
    city_pending_[slot] = city_triangles_[slot] != 0;
  }
}

void GpuTiming::BeginCityBuild(plume::RenderCommandList* list, uint32_t slot) {
  if (!device_ || !list || city_triangles_[slot] != 0) return;
  if (!city_pool_tried_[slot]) {
    city_pool_tried_[slot] = true;
    city_pools_[slot] = device_->CreateQueryPool(2, "city BLAS timestamps");
  }
  if (auto* pool = city_pools_[slot].get()) {
    // CHECK: query reset and both timestamps are outside render passes on D3D12 and Vulkan.
    list->resetQueryPool(pool, 0, 2);
    list->writeTimestamp(pool, 0);
  }
}

void GpuTiming::EndCityBuild(plume::RenderCommandList* list, uint32_t slot, uint64_t triangles) {
  if (!list || city_triangles_[slot] != 0 || !city_pools_[slot]) return;
  list->writeTimestamp(city_pools_[slot].get(), 1);
  city_triangles_[slot] = triangles;
}

GpuTiming::CityBuild GpuTiming::ReadCityBuild(uint32_t slot, bool device_lost) {
  if (!city_pending_[slot]) return {};
  city_pending_[slot] = false;
  auto* pool = city_pools_[slot].get();
  if (device_lost || !pool) return {};
  pool->queryResults();
  const uint64_t* timestamps = pool->getResults();
  if (!timestamps || timestamps[1] <= timestamps[0]) return {};
  return {timestamps[1] - timestamps[0], city_triangles_[slot]};
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

#pragma once
// ===========================================================================
// MCLA GPU renderer -- GPU time per pass
// ===========================================================================
// A measurement, off unless
// mcla_gpu_pass_timing is set: a timestamp at every pass boundary of a frame --
// the draws changing target (Scene), a resolve's copy (ResolveCache), a clear,
// the end-of-frame copies, the present pass and the overlay -- each with a
// label: the EDRAM target ("1280 2x k_2_10_10_10_FLOAT@1024 / kD24FS8@0"), or
// "resolve <source> -> <destination>", "clear", "present", "overlay". A mark's
// duration runs to the next mark and is summed under its label; the report
// lists the labels by cost, per frame, against the whole frame.
//
// One query pool per frame slot, read once the slot's fence has been waited
// (the GpuTiming rule, timing.h). Every query of the pool is written every
// frame: Plume's Vulkan queryResults reads the whole pool without waiting, and
// one query never written fails it (VK_NOT_READY) -- so CompleteFrame writes
// the unused rest back to back. A frame with more marks than the pool loses
// the extra ones (counted, logged once) and the pools grow at the next frame
// of each slot.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "frame.h"

namespace mcla::gpu {

class Device;

class PassTiming {
 public:
  PassTiming() = default;
  ~PassTiming();
  PassTiming(const PassTiming&) = delete;
  PassTiming& operator=(const PassTiming&) = delete;

  // Off unless `enabled`; without timestamps it says so and stays off.
  void Initialize(Device& device, bool enabled);
  void Shutdown();

  bool enabled() const { return enabled_; }

  // Outside any render pass, on a freshly opened list for `slot`: harvests the
  // slot's previous frame, resets the pool, marks the frame start.
  void BeginFrame(plume::RenderCommandList* list, uint32_t slot);
  // A pass boundary on the frame being recorded: what follows is `label`.
  void Mark(plume::RenderCommandList* list, std::string_view label);
  // Outside any render pass, before the frame's last list is submitted: the
  // frame end, then the rest of the pool.
  void CompleteFrame(plume::RenderCommandList* list, uint32_t slot);
  // After FrameRing::Submit executed the list.
  void OnSubmitted(uint32_t slot);

  // The labels by GPU time per frame since the last call, then a new window.
  std::string TakeSummary();

 private:
  bool Grow(Device& device, uint32_t marks);
  void Harvest(uint32_t slot);
  uint16_t Intern(std::string_view label);
  void Write(plume::RenderCommandList* list, uint16_t label);

  Device* device_ = nullptr;
  bool enabled_ = false;
  uint32_t wanted_ = 0;  // queries the next pools need (more after an overflow)
  std::unique_ptr<plume::RenderQueryPool> pools_[kFramesInFlight];
  std::vector<uint16_t> labels_[kFramesInFlight];  // label of each query written
  bool pending_[kFramesInFlight] = {};
  uint32_t slot_ = 0;
  bool open_ = false;

  std::unordered_map<std::string, uint16_t> ids_;
  std::vector<std::string> names_;
  std::vector<uint64_t> label_ns_;
  uint64_t frame_ns_ = 0;
  uint64_t frames_ = 0;
  uint64_t discarded_ = 0;
  uint64_t lost_marks_ = 0;
  uint32_t max_marks_ = 0;
  bool reported_overflow_ = false;
};

}  // namespace mcla::gpu

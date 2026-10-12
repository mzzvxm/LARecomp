#pragma once
// ===========================================================================
// MCLA GPU renderer -- GPU timestamps per frame
// ===========================================================================
// One query pool of kTimestamps per frame slot:
//
//   0 kFrameBegin     right after the frame's first list opens (the first
//                     scene record of the guest frame, or the present)
//   1 kOverlayBegin   before the overlay draws (after uploads and the clear)
//   2 kOverlayEnd     after the overlay draws
//   3 kFrameEnd       after the back buffer went back to PRESENT
//
// A frame submitted without a present pass (minimised window, failed
// acquire) gets its missing timestamps written back to back by CompleteFrame,
// so its readback never holds an unwritten query.
//
// BeginFrame first reads what the slot's previous submission wrote -- the
// FrameRing has waited that submission's fence by the time the slot is
// recorded again -- then resets the pool (vkCmdResetQueryPool must sit outside
// a render pass, and Plume does not close one for it; on D3D12 it is a no-op)
// and writes timestamp 0. A slot is only read back if its frame was submitted:
// an abandoned list never wrote its timestamps. Plume's queryResults()
// converts to nanoseconds on both backends.
//
// When timestamps are unsupported (backend::TimestampsSupported) or the pools
// cannot be created, the renderer runs without them and the statistics line
// says "GPU time unavailable": measurement degraded and said, never invented.
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "frame.h"

namespace mcla::gpu {

class Device;

class GpuTiming {
 public:
  enum Timestamp : uint32_t {
    kFrameBegin = 0,
    kOverlayBegin = 1,
    kOverlayEnd = 2,
    kFrameEnd = 3,
    kTimestamps = 4,
  };

  GpuTiming() = default;
  ~GpuTiming();
  GpuTiming(const GpuTiming&) = delete;
  GpuTiming& operator=(const GpuTiming&) = delete;

  // Never fails the renderer: without timestamps the stats say so.
  void Initialize(Device& device);
  void Shutdown();

  bool available() const { return available_; }

  // Outside any render pass, on a freshly opened list for `slot`.
  void BeginFrame(plume::RenderCommandList* list, uint32_t slot);
  void Mark(plume::RenderCommandList* list, uint32_t slot, Timestamp timestamp);
  // Writes, in order, every timestamp of the frame not written yet. Outside
  // any render pass, before the frame's last list is submitted.
  void CompleteFrame(plume::RenderCommandList* list, uint32_t slot);
  // After FrameRing::Submit executed the list (not after an abandoned one).
  void OnSubmitted(uint32_t slot);

  // "GPU 1.23 ms/frame (overlay 0.05 ms) over 3600 frames", then starts a new
  // averaging window.
  std::string TakeSummary();

 private:
  void Harvest(uint32_t slot);

  bool available_ = false;
  std::unique_ptr<plume::RenderQueryPool> pools_[kFramesInFlight];
  bool pending_[kFramesInFlight] = {};
  uint32_t written_[kFramesInFlight] = {};  // bit per Timestamp of the frame being recorded
  uint64_t frame_ns_ = 0;
  uint64_t overlay_ns_ = 0;
  uint64_t frames_ = 0;
  uint64_t discarded_ = 0;  // samples whose timestamps went backwards
};

}  // namespace mcla::gpu

#pragma once
// ===========================================================================
// MCLA GPU renderer -- census of the guest memory the draws read (diagnostic)
// ===========================================================================
// With mcla_gpu_memory_census on,
// every vertex and index range an executed draw names becomes a watched region
// of a RegionIndex, held the way the M4.4 buffer cache will hold it: armed
// when first seen, re-armed when used again after a write (arm before copy).
// Each 600-frame window reports what that cache would upload -- regions and
// bytes first seen, regions found written when used again, the SDK's
// notifications -- and the regions found written in nearly every frame they
// are used (better copied at every use than watched, 24.7). Arming changes the
// protection of guest pages, so it is off by default.
//
// Ranges: a vertex stream is the fetch constant of slot 95 - stream, address
// as stored (physical) and size = dword1 & 0x03FFFFFC bytes (SetStreamSource,
// sub_82415DC8, stores the vertex buffer's +28 dword minus the byte offset,
// so the field is already a byte count). Indices are the range the draw uses:
// start and count in indices from the object's windowed address made
// physical. Inline draws read no memory.
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <vector>

#include "guest_memory.h"
#include "region_index.h"

namespace mcla::gpu {

class MemoryCensus {
 public:
  static constexpr uint64_t kWindowFrames = 600;
  // A region written in at least this share of the frames it is used in,
  // over at least kAlwaysWrittenMinFrames frames, counts as written every use.
  static constexpr uint64_t kAlwaysWrittenMinFrames = 10;
  static constexpr uint64_t kAlwaysWrittenPercent = 90;

  explicit MemoryCensus(GuestMemory& memory) : memory_(memory) {}

  void SetEnabled(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }

  void OnDraw(const uint8_t* payload);
  void OnPresent();
  void Finish();

 private:
  struct RegionState {
    uint64_t last_frame = UINT64_MAX;
    uint64_t frames_used = 0;
    uint64_t frames_written = 0;  // used in a frame while found written
    bool written = false;         // a notification since it was last armed
    bool index = false;
  };

  void Touch(uint32_t start, uint32_t length, bool index);
  void ApplyInvalidations();
  void Report(const char* when);

  GuestMemory& memory_;
  GuestMemory::Subscriber subscriber_ = GuestMemory::kNoSubscriber;
  bool enabled_ = false;
  RegionIndex regions_;
  std::vector<RegionState> state_;  // by region id
  std::vector<GuestMemory::Range> drained_;
  uint64_t frame_ = 0;

  // Since the last report.
  uint64_t frames_ = 0;
  uint64_t vertex_uses_ = 0;
  uint64_t index_uses_ = 0;
  uint64_t new_regions_ = 0;
  uint64_t new_bytes_ = 0;
  uint64_t reuploads_ = 0;
  uint64_t reupload_bytes_ = 0;
  uint64_t written_ = 0;  // regions a notification touched
  uint64_t written_bytes_ = 0;
  uint64_t unreadable_ = 0;
};

}  // namespace mcla::gpu

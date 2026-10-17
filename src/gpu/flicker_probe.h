#pragma once
// ===========================================================================
// MCLA GPU renderer -- flicker probe (diagnostic, mcla_gpu_flicker_probe)
// ===========================================================================
// Lights, halos and shadows go dark
// for 1-5 frames at random moments, at every resolution scale, with no draw
// skipped. This finds those frames.
//
// Every guest present, the front buffer goes through the present pass a second
// time, 1:1 into a probe target of its own size (the window's image without
// the letterbox and the overlay), and the target is copied into a readback
// buffer. When the GPU has run the copy (FrameRing::RetireAction), the image is
// cut into kBlock x kBlock blocks and each block is hashed (XXH3). A block that
// held the same bytes for two frames, then other bytes for 1..kMaxBlip frames,
// then exactly the first bytes again, is a flicker. A static scene (the pause
// menu) draws the same bytes every frame, so anything flagged there is a change
// that came and went.
//
// Each frame with flicker: one log line (the frames, the blip's length, the
// block count, their bounding box in pixels of the image). The first
// mcla_gpu_flicker_probe_dumps of them also write the frame before, the first
// frame of the blip and the frame after as TGA next to the executable:
// mcla_flicker_<event>_<frame>_{before,blip,after}.tga.
//
// Cost while on: a 1:1 draw and a copy of the image on the GPU, about a
// millisecond of render-thread CPU a frame (map, copy, hash). Off: nothing is
// made.
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "frame.h"

namespace mcla::gpu {

class Device;
class PresentPass;

class FlickerProbe {
 public:
  static constexpr uint32_t kBlock = 16;
  static constexpr uint32_t kMaxBlip = 5;
  // Frames a block is compared over: two equal ones, up to kMaxBlip others,
  // and the one that came back.
  static constexpr uint32_t kHistory = kMaxBlip + 3;
  static constexpr uint32_t kReadbacks = kFramesInFlight + 2;

  FlickerProbe() = default;
  ~FlickerProbe() = default;
  FlickerProbe(const FlickerProbe&) = delete;
  FlickerProbe& operator=(const FlickerProbe&) = delete;

  // Reads mcla_gpu_flicker_probe; off, nothing is made and Record returns.
  // `format`: the present pass's target format (the swap chain's).
  void Initialize(Device& device, FrameRing& frames, plume::RenderFormat format);
  // GPU idle (every retire action run), before the device shuts down.
  void Shutdown();

  bool enabled() const { return enabled_; }

  // Outside any render pass, after PresentPass::Prepare of a guest present:
  // records the 1:1 draw and the copy of guest frame `frame`.
  void Record(plume::RenderCommandList* list, PresentPass& present, uint64_t frame);

  std::string TakeSummary();

 private:
  struct Readback {
    std::unique_ptr<plume::RenderBuffer> buffer;
    bool busy = false;
  };
  struct Frame {
    uint64_t number = 0;
    bool valid = false;
    std::vector<uint8_t> image;    // width_ * height_ * 4, rows packed
    std::vector<uint64_t> hashes;  // one per block, row-major
  };

  bool EnsureTarget(uint32_t width, uint32_t height);
  // A retire action: the copy into `readback` has run.
  void Process(uint32_t readback, uint64_t frame, uint64_t generation);
  void Detect(uint64_t frame);
  void Dump(uint64_t event, uint64_t frame, const char* what, const Frame& f) const;

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  plume::RenderFormat format_ = plume::RenderFormat::UNKNOWN;
  bool enabled_ = false;
  uint32_t max_dumps_ = 0;

  // The probe target and its readbacks, at the front buffer's size. A new
  // size makes them again; `generation_` tells a retire action of the old
  // ones to drop its copy.
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t row_pitch_ = 0;  // bytes; a multiple of 256 (D3D12 placed footprints)
  uint32_t blocks_x_ = 0;
  uint32_t blocks_y_ = 0;
  uint64_t generation_ = 0;
  std::unique_ptr<plume::RenderTexture> target_;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer_;
  std::array<Readback, kReadbacks> readbacks_;
  uint32_t next_readback_ = 0;

  std::array<Frame, kHistory> history_;  // frame n at history_[n % kHistory]

  uint32_t dumps_ = 0;
  uint64_t events_ = 0;  // every event since Initialize
  struct Stats {
    uint64_t frames = 0;        // images compared
    uint64_t busy = 0;          // guest presents not probed: every readback in flight
    uint64_t gaps = 0;          // frames missing between two probed ones
    uint64_t changed = 0;       // blocks that differ from the frame before, summed
    uint32_t changed_max = 0;   // ... in one frame
    uint64_t events = 0;        // frames with flicker
    uint64_t blocks = 0;        // blocks that flickered
    std::array<uint64_t, kMaxBlip + 1> by_length{};  // blocks by blip length
  };
  Stats stats_;
};

}  // namespace mcla::gpu

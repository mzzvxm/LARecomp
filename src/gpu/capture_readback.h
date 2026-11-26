#pragma once
// ===========================================================================
// MCLA GPU renderer -- capture mode (trailer toolkit M2a): the take's readback
// ===========================================================================
// A take (mc_engine/cinematic/capture_mode.cpp) reads every guest frame back, as the present pass writes it: the
// front buffer (or the frame layers' composition) through the display's gamma table, 1:1 at the
// guest picture's size (1280x720 times the render scale), before the SDK's overlay pass, so no
// ImGui and no F4 menu is in the picture. It is the probe of FlickerProbe (flicker_probe.h) with
// the output kept: PresentPass::DrawUnscaled into a target of its own, a copy into a GPU readback
// buffer, and, once the GPU has run the copy, a copy of the rows into a buffer of the take's host
// ring (mc::cinematic::CaptureAcquire) from which the writer thread takes them. No change to Plume or to
// the present pass was needed.
//
// Which frames: those whose present record carries mc::cinematic::capture::kTagCapture (the hand-over of the frame stamps it, the
// swap hands the stamp on: CaptureOnHandOver, CaptureOnSwap). A tagged frame that cannot be read back (the sink is not open, the output is HDR10,
// frame generation is on, the chain is minimised, ...) is reported lost, so that the game's gate
// does not wait for it.
//
// Lock-step: with mcla_capture_lag 0 the frame ring is told to wait for the frame it just
// submitted (AfterAdvance), so the readback is done when the present returns and the game's next
// update can start. With a lag the copy is processed by the ring's own retirement, a frame later.
//
// Memory (M2b): how many readback buffers are made (2 to kReadbacks) is the take's memory plan
// (mc::cinematic::CaptureMemoryPlan: a hard cap and the free memory), asked before anything is made; a
// plan that does not fit ends the take and the frame is reported lost. The target and the buffers go
// when the take ends (BeginPresent finds no take running): nothing stays allocated between takes.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#include "frame.h"
#include "mc_engine/cinematic/capture_visual.h"

namespace mcla::gpu {

class Device;
class FrameLayers;
class GBuffer;
class PassTiming;
class PresentPass;
struct HostImage;

namespace vis = mc::cinematic::capture::visual;

struct VisualSource {
  plume::RenderTexture* texture = nullptr;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t width = 0, height = 0, channel_offset = 0;
  bool swap_rb = false;
  bool alternate = false;

  static VisualSource Image(const HostImage* image, uint32_t channel_offset = 0);
};
struct VisualSources {
  std::array<VisualSource, vis::kCount> images;
  uint32_t empty = 0;
  const HostImage* depth = nullptr;
  bool native_reflection_fallback = false;
  bool road_view_drawn = false;
};

class CaptureReadback {
 public:
  static constexpr uint32_t kReadbacks = kFramesInFlight + 2;

  CaptureReadback() = default;
  ~CaptureReadback() = default;
  CaptureReadback(const CaptureReadback&) = delete;
  CaptureReadback& operator=(const CaptureReadback&) = delete;

  // `format`: the present pass's target format (the swap chain's). Nothing is made until a take's frame arrives.
  void Initialize(Device& device, FrameRing& frames, PassTiming& pass_timing,
                  plume::RenderFormat format);
  // GPU idle (every retire action run), before the device shuts down.
  void Shutdown();

  // ExecutePresent opens and closes around a guest present: `tag` is the present record's tag (0: no take
  // frame). EndPresent reports a tagged frame that Record did not deal with as lost.
  void BeginPresent(uint64_t tag);
  void EndPresent();

  // Outside any render pass, after PresentPass::Prepare of the guest present BeginPresent named.
  void Record(plume::RenderCommandList* list, PresentPass& present, GBuffer& gbuffer,
              const FrameLayers& layers);
  VisualSources& visual_sources() { return visual_sources_; }

  // Right after FrameRing::Advance: in lock-step, waits for the frame just submitted so that its readback is done.
  void AfterAdvance();

 private:
  using Clock = std::chrono::steady_clock;
  struct Readback {
    std::unique_ptr<plume::RenderBuffer> buffer;
    bool busy = false;
  };

  // `buffers`: the readback buffers to have (2 to kReadbacks), the take's plan.
  bool EnsureTarget(uint32_t width, uint32_t height, uint32_t buffers, bool legacy,
                    uint64_t visual_capacity, bool final);
  // Everything made goes after the frames that use it; a copy still in flight is dropped when it lands.
  void ReleaseTarget();
  void Lost(const char* why);
  // A retire action: the copy into `readback` has run.
  void Process(uint32_t readback, uint64_t update, uint64_t generation, double record_ms,
               const vis::Frame& visual_frame, uint64_t visual_used);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  plume::RenderFormat format_ = plume::RenderFormat::UNKNOWN;
  bool shut_down_ = false;

  // The target and its readbacks, at the front buffer's size. A new size makes them again; `generation_` tells a retire
  // action of the old ones to drop its copy.
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t row_pitch_ = 0;  // bytes; a multiple of 256 (D3D12 placed footprints)
  bool legacy_ = true;
  uint64_t visual_base_ = 0, visual_capacity_ = 0;
  uint64_t visual_take_ = 0;
  uint32_t missing_streams_ = 0;
  uint32_t logged_streams_ = 0;
  VisualSources visual_sources_{};
  uint64_t generation_ = 0;
  std::unique_ptr<plume::RenderTexture> target_;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer_;
  std::array<Readback, kReadbacks> readbacks_;
  uint32_t active_readbacks_ = 0;  // how many of them are made
  uint32_t next_readback_ = 0;

  // The present in progress.
  uint64_t tag_ = 0;
  bool handled_ = false;
  // Lock-step: the wait after the submit is in progress since...
  bool sync_pending_ = false;
  bool wait_valid_ = false;
  Clock::time_point wait_start_;
  double last_vram_mb_ = 0.0;
  uint64_t processed_ = 0;
};

}  // namespace mcla::gpu

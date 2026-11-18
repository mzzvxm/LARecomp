// MCLA GPU renderer -- capture mode: the take's readback. See capture_readback.h.
//
// The recording is FlickerProbe::Record's (flicker_probe.cpp): the present pass drawn 1:1 into a target of its own, the
// target copied into a readback buffer. The Plume calls are copied from the probe, which is built and runs; the code ran in the E7 takes
// (a 4K take of 240 frames).

#include "capture_readback.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include <rex/logging/log_category.h>

#include "device.h"
#include "mc_engine/cinematic/capture_core.h"
#include "mc_engine/cinematic/capture_mode.h"
#include "pass_timing.h"
#include "present.h"

namespace mcla::gpu {

namespace {
double MillisSince(std::chrono::steady_clock::time_point from) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}
}  // namespace

void CaptureReadback::Initialize(Device& device, FrameRing& frames, PassTiming& pass_timing,
                             plume::RenderFormat format) {
  device_ = &device;
  frames_ = &frames;
  pass_timing_ = &pass_timing;
  format_ = format;
  shut_down_ = false;
}

void CaptureReadback::Shutdown() {
  shut_down_ = true;
  framebuffer_.reset();
  target_.reset();
  for (Readback& r : readbacks_) {
    r.buffer.reset();
    r.busy = false;
  }
  device_ = nullptr;
  frames_ = nullptr;
  pass_timing_ = nullptr;
}

void CaptureReadback::ReleaseTarget() {
  // The old objects die after the frames that use them, and the copies still in flight are dropped when they land (generation).
  ++generation_;
  if (framebuffer_) {
    frames_->Retire(std::move(framebuffer_));
  }
  if (target_) {
    frames_->Retire(std::move(target_));
  }
  for (Readback& r : readbacks_) {
    if (r.buffer) {
      frames_->Retire(std::move(r.buffer));
    }
    r.busy = false;
  }
  active_readbacks_ = 0;
  next_readback_ = 0;
  width_ = 0;
  height_ = 0;
}

bool CaptureReadback::EnsureTarget(uint32_t width, uint32_t height, uint32_t buffers) {
  buffers = std::clamp<uint32_t>(buffers, 2, kReadbacks);
  if (target_ && width == width_ && height == height_ && buffers == active_readbacks_) {
    return true;
  }
  if (format_ != plume::RenderFormat::B8G8R8A8_UNORM &&
      format_ != plume::RenderFormat::R8G8B8A8_UNORM) {
    REXLOG_ERROR("[cap] the present target format {} is not 8 bits per channel; no readback "
                 "(TODO: R10G10B10A2 / float16 for the HDR output)",
                 static_cast<uint32_t>(format_));
    return false;
  }
  // A new size, or another number of buffers: the old objects go.
  ReleaseTarget();
  width_ = width;
  height_ = height;
  row_pitch_ = (width * 4 + 255) / 256 * 256;

  const plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      width, height, 1, format_, plume::RenderTextureFlag::RENDER_TARGET);
  target_ = device_->CreateTexture(desc, "capture target");
  if (target_) {
    const plume::RenderTexture* attachments[1] = {target_.get()};
    framebuffer_ = device_->CreateFramebuffer(plume::RenderFramebufferDesc(attachments, 1),
                                              "capture framebuffer");
  }
  bool ok = target_ && framebuffer_;
  for (uint32_t i = 0; i < buffers && ok; ++i) {
    readbacks_[i].buffer = device_->CreateBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(uint64_t(row_pitch_) * height), "capture readback");
    ok = readbacks_[i].buffer != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[cap] the {}x{} target or its readbacks could not be made", width, height);
    framebuffer_.reset();
    target_.reset();
    for (Readback& r : readbacks_) {
      r.buffer.reset();
    }
    width_ = height_ = 0;
    active_readbacks_ = 0;
    return false;
  }
  active_readbacks_ = buffers;
  REXLOG_INFO("[cap] readback: {}x{}, {} GPU readback buffers of {:.1f} MiB", width, height,
              buffers, double(uint64_t(row_pitch_) * height) / 1048576.0);
  return true;
}

void CaptureReadback::BeginPresent(uint64_t tag) {
  tag_ = (tag & mc::cinematic::capture::kTagCapture) != 0 ? tag : 0;
  handled_ = false;
  // The take is over (its last frame was read back before the gate let it end): nothing stays allocated between takes.
  if (target_ && tag_ == 0 && !mc::cinematic::CaptureTakeActive() && frames_) {
    ReleaseTarget();
  }
}

void CaptureReadback::EndPresent() {
  if (tag_ != 0 && !handled_) {
    Lost("the present did not record the readback (device lost, minimised, frame generation on, or an early return)");
  }
  tag_ = 0;
  handled_ = false;
}

void CaptureReadback::Lost(const char* why) {
  if (tag_ == 0 || handled_) {
    return;
  }
  handled_ = true;
  mc::cinematic::CaptureFrameLost(tag_ & mc::cinematic::capture::kTagUpdateMask, why);
}

void CaptureReadback::Record(plume::RenderCommandList* list, PresentPass& present) {
  if (tag_ == 0 || handled_ || !device_ || shut_down_) {
    return;
  }
  const uint64_t update = tag_ & mc::cinematic::capture::kTagUpdateMask;
  if (present.hdr_output()) {
    Lost("the output is HDR10; the readback is of the 8-bit SDR picture (turn mcla_gpu_hdr off)");
    return;
  }
  if (!present.source_ready()) {
    Lost("no front buffer to read back");
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  // The take's memory plan first: how many readback buffers fit the cap and the free memory (2 is enough in lock-step; a lag needs them all).
  const uint32_t gpu_min = mc::cinematic::CaptureLag() == 0 ? 2 : kReadbacks;
  const mc::cinematic::MemoryGrant grant =
      mc::cinematic::CaptureMemoryPlan(present.source_width(), present.source_height(), kReadbacks, gpu_min);
  if (!grant.ok) {
    Lost("the take's memory plan does not fit, or the picture size changed (the [cap] TAKE FAILED line says which)");
    return;
  }
  if (!EnsureTarget(present.source_width(), present.source_height(), grant.gpu_buffers)) {
    Lost("the readback target could not be made");
    return;
  }
  mc::cinematic::FrameFormat format;
  format.width = width_;
  format.height = height_;
  format.bgra = format_ == plume::RenderFormat::B8G8R8A8_UNORM;
  if (!mc::cinematic::CaptureSinkOpen(format)) {
    Lost("the take's sink is not open (the take ended, or the size changed)");
    return;
  }
  Readback& r = readbacks_[next_readback_];
  if (r.busy || !r.buffer) {
    Lost("every GPU readback buffer is still in flight");
    return;
  }
  const uint32_t index = next_readback_;
  next_readback_ = (next_readback_ + 1) % active_readbacks_;

  pass_timing_->Mark(list, "capture");
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target_.get(),
                                             plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(framebuffer_.get());
  present.DrawUnscaled(list);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(target_.get(),
                                             plume::RenderTextureLayout::COPY_SOURCE));
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(r.buffer.get(), format_, width_, height_,
                                                        1, row_pitch_ / 4, 0),
      plume::RenderTextureCopyLocation::Subresource(target_.get()));
  // What follows is the present pass again.
  pass_timing_->Mark(list, "present");
  r.busy = true;
  handled_ = true;
  const double record_ms = MillisSince(t0);
  const uint64_t generation = generation_;
  frames_->RetireAction([this, index, update, generation, record_ms] {
    Process(index, update, generation, record_ms);
  });
  sync_pending_ = mc::cinematic::CaptureLag() == 0;
}

void CaptureReadback::AfterAdvance() {
  if (!sync_pending_ || !frames_) {
    return;
  }
  sync_pending_ = false;
  // The frame just submitted is "the previous frame" of the slot Advance moved to: waiting for it runs its retire actions,
  // so Process has run when this returns (the readback is in the ring).
  // CHECK: kFramesInFlight is 2, so "the other slot" is the one just submitted; WaitPreviousFrame was written for the FidelityFX
  // backend (frame_generation_ffx.cpp) and is called here with no list open, which its comment allows but nothing else does.
  wait_start_ = Clock::now();
  wait_valid_ = true;
  frames_->WaitPreviousFrame();
  wait_valid_ = false;
}

void CaptureReadback::Process(uint32_t readback, uint64_t update, uint64_t generation,
                          double record_ms) {
  if (shut_down_ || !device_) {
    return;  // the renderer is going away (the ring's last clean-up runs the action)
  }
  const auto t_in = Clock::now();
  if (generation != generation_) {
    mc::cinematic::CaptureFrameLost(update, "the readback target was made again before the copy ran");
    return;
  }
  Readback& r = readbacks_[readback];
  r.busy = false;
  const void* mapped = r.buffer ? r.buffer->map() : nullptr;
  if (!mapped) {
    mc::cinematic::CaptureFrameLost(update, "the GPU readback buffer could not be mapped");
    return;
  }
  double stall_ms = 0.0;
  const mc::cinematic::HostBuffer host = mc::cinematic::CaptureAcquire(&stall_ms);
  if (!host.data) {
    r.buffer->unmap();
    mc::cinematic::CaptureFrameLost(update, "no host buffer (the sink is not open)");
    return;
  }
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  const size_t row_bytes = size_t(width_) * 4;
  if (row_pitch_ == row_bytes) {
    std::memcpy(host.data, src, row_bytes * height_);
  } else {
    for (uint32_t y = 0; y < height_; ++y) {
      std::memcpy(host.data + size_t(y) * row_bytes, src + size_t(y) * row_pitch_, row_bytes);
    }
  }
  r.buffer->unmap();
  // An emulated slow capture: the frame takes this much longer to read back.
  if (const double delay = mc::cinematic::CaptureDelayMs(); delay > 0.0) {
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(delay));
  }
  mc::cinematic::FrameCosts costs;
  costs.update = update;
  costs.record_ms = record_ms;
  costs.gpu_ms = wait_valid_ ? std::chrono::duration<double, std::milli>(t_in - wait_start_).count() : 0.0;
  costs.copy_ms = MillisSince(t_in) - stall_ms;
  costs.stall_ms = stall_ms;
  if (processed_++ % 15 == 0) {
    const auto vram = device_->QueryVideoMemory();
    last_vram_mb_ = double(vram.used) / 1048576.0;
  }
  costs.vram_mb = last_vram_mb_;
  mc::cinematic::CaptureFrameDone(host, costs);
}

}  // namespace mcla::gpu

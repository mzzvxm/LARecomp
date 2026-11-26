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
#include "frame_layers.h"
#include "gbuffer.h"
#include "mc_engine/cinematic/capture_core.h"
#include "mc_engine/cinematic/capture_mode.h"
#include "pass_timing.h"
#include "present.h"

namespace mcla::gpu {

namespace {
double MillisSince(std::chrono::steady_clock::time_point from) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}

void CopyPlane(plume::RenderCommandList& list, const VisualSource& image,
               plume::RenderBuffer& readback, const vis::Plane& plane, uint64_t base) {
  const auto& crop = plane.crop;
  const plume::RenderBox box(crop.x, crop.y, crop.x + crop.width, crop.y + crop.height);
  if (image.layout != plume::RenderTextureLayout::COPY_SOURCE) {
    list.barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::COPY_SOURCE));
  }
  list.copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(&readback, image.format, crop.width,
                                                        crop.height, 1, plane.row_pitch / plane.pixel_bytes,
                                                        base + plane.offset),
      plume::RenderTextureCopyLocation::Subresource(image.texture), 0, 0, 0, &box);
  if (image.layout != plume::RenderTextureLayout::COPY_SOURCE) {
    list.barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                  plume::RenderTextureBarrier(image.texture, image.layout));
  }
}
}  // namespace

VisualSource VisualSource::Image(const HostImage* image, uint32_t channel_offset) {
  if (!image || !image->texture || image->samples != 1) return {};
  return {image->texture.get(), image->format, image->layout, image->width, image->height, channel_offset};
}

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

bool CaptureReadback::EnsureTarget(uint32_t width, uint32_t height, uint32_t buffers, bool legacy,
                                   uint64_t visual_capacity, bool final) {
  buffers = std::clamp<uint32_t>(buffers, 2, kReadbacks);
  if (width == width_ && height == height_ && buffers == active_readbacks_ &&
      legacy == legacy_ && visual_capacity == visual_capacity_ && final == bool(target_)) {
    return true;
  }
  if (final && format_ != plume::RenderFormat::B8G8R8A8_UNORM &&
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
  legacy_ = legacy;
  visual_capacity_ = visual_capacity;
  const uint64_t legacy_bytes = legacy ? uint64_t(row_pitch_) * height : 0;
  visual_base_ = vis::Align(legacy_bytes, 512);

  if (final) {
    const auto desc = plume::RenderTextureDesc::Texture2D(
        width, height, 1, format_, plume::RenderTextureFlag::RENDER_TARGET);
    target_ = device_->CreateTexture(desc, "capture target");
    if (target_) {
      const plume::RenderTexture* attachments[1] = {target_.get()};
      framebuffer_ = device_->CreateFramebuffer(plume::RenderFramebufferDesc(attachments, 1),
                                                "capture framebuffer");
    }
  }
  const uint64_t bytes = visual_capacity ? visual_base_ + visual_capacity : legacy_bytes;
  bool ok = !final || (target_ && framebuffer_);
  for (uint32_t i = 0; i < buffers && ok; ++i) {
    readbacks_[i].buffer = device_->CreateBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(bytes), "capture readback");
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
              buffers, double(bytes) / 1048576.0);
  return true;
}

void CaptureReadback::BeginPresent(uint64_t tag) {
  tag_ = (tag & mc::cinematic::capture::kTagCapture) != 0 ? tag : 0;
  handled_ = false;
  // The take is over (its last frame was read back before the gate let it end): nothing stays allocated between takes.
  if (active_readbacks_ && tag_ == 0 && !mc::cinematic::CaptureTakeActive() && frames_) {
    ReleaseTarget();
  }
}

void CaptureReadback::EndPresent() {
  if (tag_ != 0 && !handled_) {
    Lost("the present did not record the readback (device lost, minimised, frame generation on, or an early return)");
  }
  if (tag_ != 0) visual_sources_ = {};
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

void CaptureReadback::Record(plume::RenderCommandList* list, PresentPass& present, GBuffer& gbuffer,
                             const FrameLayers& layers) {
  if (tag_ == 0 || handled_ || !list || !device_ || !frames_ || !pass_timing_ || shut_down_) {
    return;
  }
  const uint64_t update = tag_ & mc::cinematic::capture::kTagUpdateMask;
  const auto config = mc::cinematic::CaptureVisualConfig();
  if (present.hdr_output() && config.legacy) {
    Lost("the output is HDR10; the readback is of the 8-bit SDR picture (turn mcla_gpu_hdr off)");
    return;
  }
  if (!present.source_ready()) {
    Lost("no front buffer to read back");
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t width = present.source_width(), height = present.source_height();
  const bool sample = config.Samples(update);
  const bool final = config.legacy || (!present.hdr_output() && (config.mask & vis::Bit(vis::kFinal)));
  if (config.crop.width && !config.crop.Fits(width, height)) {
    Lost("the visual capture rectangle is outside the display-sized readback");
    mc::cinematic::CaptureVisualFail("visual capture rectangle outside the display-sized readback");
    return;
  }
  if (config.mask && config.first != visual_take_) {
    visual_take_ = config.first;
    missing_streams_ = 0;
    logged_streams_ = 0;
    ReleaseTarget();
  }
  uint64_t capacity = visual_capacity_;
  uint64_t scratch = 0;
  if (config.mask && !active_readbacks_) {
    uint32_t max_width = width, max_height = height;
    for (const auto& source : visual_sources_.images) {
      max_width = std::max(max_width, source.width);
      max_height = std::max(max_height, source.height);
    }
    if (config.mask & (vis::Bit(vis::kClass) | vis::Bit(vis::kReflHit))) {
      max_width = std::max(max_width, gbuffer.frame().width);
      max_height = std::max(max_height, gbuffer.frame().height);
    }
    if (config.mask & vis::Bit(vis::kClass)) {
      scratch += uint64_t(max_width) * max_height;
    }
    if ((config.mask & vis::Bit(vis::kReflHit)) && visual_sources_.native_reflection_fallback) {
      scratch += uint64_t(max_width) * max_height * vis::kStreams[vis::kReflHit].max_source_bytes;
    }
    if (final && !config.legacy) scratch += uint64_t(width) * height * 4;
    capacity = 0;
    for (uint32_t i = 0; i < vis::kCount; ++i) {
      if (config.mask & (1u << i)) {
        vis::Place(vis::ScaleCrop(config.crop, width, height, max_width, max_height),
                   max_width, max_height, vis::kStreams[i].max_source_bytes, 0, false, capacity);
      }
    }
  } else if (!config.mask) {
    capacity = 0;
  }
  const uint64_t legacy_bytes = config.legacy ? vis::Align(uint64_t(width) * 4, 256) * height : 0;
  const uint64_t extra = capacity ? capacity + vis::Align(legacy_bytes, 512) - legacy_bytes : 0;
  // The take's memory plan first: how many readback buffers fit the cap and the free memory (2 is enough in lock-step; a lag needs them all).
  const uint32_t gpu_min = mc::cinematic::CaptureLag() == 0 ? 2 : kReadbacks;
  const mc::cinematic::MemoryGrant grant =
      mc::cinematic::CaptureMemoryPlan(width, height, kReadbacks, gpu_min, config.legacy, extra, scratch);
  if (!grant.ok) {
    Lost("the take's memory plan does not fit, or the picture size changed (the [cap] TAKE FAILED line says which)");
    return;
  }
  if (!EnsureTarget(width, height, grant.gpu_buffers, config.legacy, capacity, final)) {
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
  if (target_ && framebuffer_ && (config.legacy || (sample && (config.mask & vis::Bit(vis::kFinal))))) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(target_.get(), plume::RenderTextureLayout::COLOR_WRITE));
    list->setFramebuffer(framebuffer_.get());
    present.DrawUnscaled(list);
    list->setFramebuffer(nullptr);
    list->barriers(plume::RenderBarrierStage::COPY,
                   plume::RenderTextureBarrier(target_.get(), plume::RenderTextureLayout::COPY_SOURCE));
    if (config.legacy) {
      list->copyTextureRegion(
          plume::RenderTextureCopyLocation::PlacedFootprint(r.buffer.get(), format_, width_, height_,
                                                            1, row_pitch_ / 4, 0),
          plume::RenderTextureCopyLocation::Subresource(target_.get()));
    }
    visual_sources_.images[vis::kFinal] = {target_.get(), format_, plume::RenderTextureLayout::COPY_SOURCE,
                                         width, height, 0, format.bgra};
  }
  vis::Frame visual_frame;
  uint64_t visual_used = 0;
  if (sample) {
    visual_frame.index = update - config.first;
    if (config.mask & vis::Bit(vis::kClass)) {
      const auto layer = layers.presentable();
      TextureKey front;
      const bool ui = present.front_key(&front) && layer.IsPictureOf(front) && layers.UiAlphaFor(layer) > 0.0f;
      visual_sources_.images[vis::kClass] = VisualSource::Image(
          gbuffer.DrawCaptureClass(list, visual_sources_.depth, ui ? layer.ui : nullptr, layer.gap));
    }
    if ((config.mask & vis::Bit(vis::kReflHit)) && visual_sources_.native_reflection_fallback) {
      auto& hit = visual_sources_.images[vis::kReflHit];
      hit = VisualSource::Image(gbuffer.DrawCaptureReflectionFallback(list, visual_sources_.depth, visual_sources_.road_view_drawn));
      hit.alternate = true;
    }
    for (uint32_t i = 0; i < vis::kCount; ++i) {
      const uint32_t bit = 1u << i;
      if (!(config.mask & bit)) continue;
      const auto& src = visual_sources_.images[i];
      const uint32_t pixel_bytes = plume::RenderFormatSize(src.format);
      if (!src.texture || !src.width || !src.height || !pixel_bytes ||
          src.layout == plume::RenderTextureLayout::UNKNOWN) {
        if (!(missing_streams_ & bit)) {
          REXLOG_WARN("[cap] {} has no readable target for update {} (omitted)", vis::kStreams[i].name, update);
          missing_streams_ |= bit;
        }
        continue;
      }
      uint64_t next = visual_used;
      auto plane = vis::Place(vis::ScaleCrop(config.crop, width, height, src.width, src.height),
                              src.width, src.height, pixel_bytes, src.channel_offset, src.swap_rb, next);
      plane.alternate = src.alternate;
      if (next > visual_capacity_) {
        mc::cinematic::CaptureVisualFail("a visual target outgrew the take's readback reservation");
        break;
      }
      CopyPlane(*list, src, *r.buffer, plane, visual_base_);
      if (!(logged_streams_ & bit)) {
        logged_streams_ |= bit;
        REXLOG_INFO("[cap] {} source {}x{}, crop {},{} {}x{}{}",
                    vis::kStreams[i].name, plane.width, plane.height, plane.crop.x,
                    plane.crop.y, plane.crop.width, plane.crop.height,
                    plane.alternate ? " (alternate target)" : "");
      }
      visual_frame.planes[i] = plane;
      visual_used = next;
    }
  }
  // What follows is the present pass again.
  pass_timing_->Mark(list, "present");
  r.busy = true;
  handled_ = true;
  const double record_ms = MillisSince(t0);
  const uint64_t generation = generation_;
  frames_->RetireAction([this, index, update, generation, record_ms, visual_frame, visual_used] {
    Process(index, update, generation, record_ms, visual_frame, visual_used);
  });
  sync_pending_ = mc::cinematic::CaptureSyncReadback(update);
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
                              double record_ms, const vis::Frame& visual_frame, uint64_t visual_used) {
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
  if (device_->IsLost()) {
    mc::cinematic::CaptureFrameLost(update, "device lost before the readback completed");
    return;
  }
  const bool needs_map = legacy_ || visual_used != 0;
  const auto* mapped = needs_map && r.buffer ? static_cast<const uint8_t*>(r.buffer->map()) : nullptr;
  if (needs_map && !mapped) {
    mc::cinematic::CaptureFrameLost(update, "the GPU readback buffer could not be mapped");
    return;
  }
  double stall_ms = 0.0;
  const mc::cinematic::HostBuffer host = mc::cinematic::CaptureAcquire(&stall_ms);
  if (!host.data) {
    if (mapped && r.buffer) r.buffer->unmap();
    mc::cinematic::CaptureFrameLost(update, "no host buffer (the sink is not open)");
    return;
  }
  const size_t row_bytes = size_t(width_) * 4;
  if (legacy_ && mapped) {
    if (row_pitch_ == row_bytes) {
      std::memcpy(host.data, mapped, row_bytes * height_);
    } else {
      for (uint32_t y = 0; y < height_; ++y) {
        std::memcpy(host.data + size_t(y) * row_bytes, mapped + size_t(y) * row_pitch_, row_bytes);
      }
    }
  }
  if (visual_capacity_) {
    uint8_t* dst = host.data + (legacy_ ? row_bytes * height_ : 0);
    std::memcpy(dst, &visual_frame, sizeof(visual_frame));
    if (visual_used && mapped) {
      std::memcpy(dst + sizeof(visual_frame), mapped + visual_base_, visual_used);
    }
  }
  if (mapped && r.buffer) r.buffer->unmap();
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

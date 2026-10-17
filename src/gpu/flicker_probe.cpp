// MCLA GPU renderer -- flicker probe. See flicker_probe.h.

#include "flicker_probe.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include "device.h"
#include "present.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_flicker_probe, false, "MCLA/GPU",
    "Diagnostic: read every guest frame back, as the window shows it, and log "
    "the blocks of 16x16 pixels that change for 1-5 frames and come back to exactly their "
    "old bytes (a flicker). Meant for a static scene such as the pause menu. Costs a copy of "
    "the image per frame on the GPU and about a millisecond of render-thread CPU. Off: "
    "nothing is made.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_gpu_flicker_probe_dumps, 12, "MCLA/GPU",
    "With mcla_gpu_flicker_probe: how many flicker events also write the frame before, the "
    "first frame of the blip and the frame after as mcla_flicker_<event>_<frame>_<what>.tga "
    "next to the executable (3.7 MB each at 1280x720).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu {

void FlickerProbe::Initialize(Device& device, FrameRing& frames, plume::RenderFormat format) {
  device_ = &device;
  frames_ = &frames;
  format_ = format;
  enabled_ = REXCVAR_GET(mcla_gpu_flicker_probe);
  max_dumps_ = REXCVAR_GET(mcla_gpu_flicker_probe_dumps);
  if (!enabled_) {
    return;
  }
  if (format_ != plume::RenderFormat::B8G8R8A8_UNORM &&
      format_ != plume::RenderFormat::R8G8B8A8_UNORM) {
    REXLOG_ERROR("[gpu] flicker probe: the present target format {} is not 8 bits per channel; "
                 "the probe stays off (TODO other formats)",
                 static_cast<uint32_t>(format_));
    enabled_ = false;
    return;
  }
  REXLOG_WARN("[gpu] flicker probe ON (diagnostic): every guest frame is read back and "
              "compared; up to {} events dump TGA next to the executable",
              max_dumps_);
}

void FlickerProbe::Shutdown() {
  framebuffer_.reset();
  target_.reset();
  for (Readback& r : readbacks_) {
    r.buffer.reset();
    r.busy = false;
  }
  for (Frame& f : history_) {
    f = Frame{};
  }
  device_ = nullptr;
  frames_ = nullptr;
  enabled_ = false;
}

bool FlickerProbe::EnsureTarget(uint32_t width, uint32_t height) {
  if (target_ && width == width_ && height == height_) {
    return true;
  }
  // A new size: the old objects die after the frames that use them, and the
  // copies still in flight are dropped when they land (generation).
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
  for (Frame& f : history_) {
    f.valid = false;
  }
  width_ = width;
  height_ = height;
  row_pitch_ = (width * 4 + 255) / 256 * 256;
  blocks_x_ = (width + kBlock - 1) / kBlock;
  blocks_y_ = (height + kBlock - 1) / kBlock;

  const plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      width, height, 1, format_, plume::RenderTextureFlag::RENDER_TARGET);
  target_ = device_->CreateTexture(desc, "flicker probe target");
  if (target_) {
    const plume::RenderTexture* attachments[1] = {target_.get()};
    framebuffer_ = device_->CreateFramebuffer(plume::RenderFramebufferDesc(attachments, 1),
                                              "flicker probe framebuffer");
  }
  bool ok = target_ && framebuffer_;
  for (Readback& r : readbacks_) {
    if (!ok) {
      break;
    }
    r.buffer = device_->CreateBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(uint64_t(row_pitch_) * height),
        "flicker probe readback");
    ok = r.buffer != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[gpu] flicker probe: the {}x{} target or its readbacks could not be made; "
                 "the probe stays off",
                 width, height);
    framebuffer_.reset();
    target_.reset();
    for (Readback& r : readbacks_) {
      r.buffer.reset();
    }
    enabled_ = false;
    return false;
  }
  for (Frame& f : history_) {
    f.image.assign(size_t(width) * height * 4, 0);
    f.hashes.assign(size_t(blocks_x_) * blocks_y_, 0);
  }
  REXLOG_INFO("[gpu] flicker probe: {}x{} ({}x{} blocks of {} px)", width, height, blocks_x_,
              blocks_y_, kBlock);
  return true;
}

void FlickerProbe::Record(plume::RenderCommandList* list, PresentPass& present, uint64_t frame) {
  if (!enabled_ || !present.source_ready()) {
    return;
  }
  if (!EnsureTarget(present.source_width(), present.source_height())) {
    return;
  }
  Readback& r = readbacks_[next_readback_];
  if (r.busy) {
    ++stats_.busy;
    return;
  }
  const uint32_t index = next_readback_;
  next_readback_ = (next_readback_ + 1) % kReadbacks;

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
  r.busy = true;
  const uint64_t generation = generation_;
  frames_->RetireAction(
      [this, index, frame, generation] { Process(index, frame, generation); });
}

void FlickerProbe::Process(uint32_t readback, uint64_t frame, uint64_t generation) {
  if (!enabled_ || generation != generation_) {
    return;
  }
  Readback& r = readbacks_[readback];
  r.busy = false;
  const void* mapped = r.buffer ? r.buffer->map() : nullptr;
  if (!mapped) {
    REXLOG_ERROR("[gpu] flicker probe: the readback of frame {} could not be mapped", frame);
    return;
  }
  Frame& f = history_[frame % kHistory];
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  const size_t row_bytes = size_t(width_) * 4;
  for (uint32_t y = 0; y < height_; ++y) {
    std::memcpy(f.image.data() + y * row_bytes, src + size_t(y) * row_pitch_, row_bytes);
  }
  r.buffer->unmap();

  for (uint32_t by = 0; by < blocks_y_; ++by) {
    const uint32_t y0 = by * kBlock;
    const uint32_t y1 = std::min(y0 + kBlock, height_);
    for (uint32_t bx = 0; bx < blocks_x_; ++bx) {
      const uint32_t x0 = bx * kBlock;
      const size_t bytes = size_t(std::min(x0 + kBlock, width_) - x0) * 4;
      uint64_t h = 0;
      for (uint32_t y = y0; y < y1; ++y) {
        h = XXH3_64bits_withSeed(f.image.data() + y * row_bytes + size_t(x0) * 4, bytes, h);
      }
      f.hashes[size_t(by) * blocks_x_ + bx] = h;
    }
  }
  const Frame& before = history_[(frame + kHistory - 1) % kHistory];
  if (before.valid && before.number + 1 < frame) {
    stats_.gaps += frame - before.number - 1;
  }
  f.number = frame;
  f.valid = true;
  ++stats_.frames;
  Detect(frame);
}

void FlickerProbe::Detect(uint64_t frame) {
  // h[i]: frame - i, when probed (else null).
  std::array<const Frame*, kHistory> h{};
  for (uint32_t i = 0; i < kHistory && i <= frame; ++i) {
    const Frame& f = history_[(frame - i) % kHistory];
    h[i] = f.valid && f.number == frame - i ? &f : nullptr;
  }
  if (!h[1]) {
    return;
  }
  const size_t blocks = size_t(blocks_x_) * blocks_y_;
  uint32_t changed = 0;
  uint32_t flickered = 0;
  uint32_t longest = 0;
  std::array<uint32_t, kMaxBlip + 1> by_length{};
  uint32_t bx0 = blocks_x_, by0 = blocks_y_, bx1 = 0, by1 = 0;
  for (size_t b = 0; b < blocks; ++b) {
    const uint64_t now = h[0]->hashes[b];
    if (h[1]->hashes[b] == now) {
      continue;
    }
    ++changed;
    // The blip: frames 1..length differ from now, frame length+1 equals it,
    // and so does frame length+2 (the block was stable before the blip).
    for (uint32_t length = 1; length <= kMaxBlip; ++length) {
      if (!h[length + 2] || h[length]->hashes[b] == now) {
        break;
      }
      if (h[length + 1]->hashes[b] == now && h[length + 2]->hashes[b] == now) {
        ++flickered;
        ++by_length[length];
        longest = std::max(longest, length);
        const uint32_t x = uint32_t(b % blocks_x_), y = uint32_t(b / blocks_x_);
        bx0 = std::min(bx0, x);
        by0 = std::min(by0, y);
        bx1 = std::max(bx1, x);
        by1 = std::max(by1, y);
        break;
      }
    }
  }
  stats_.changed += changed;
  stats_.changed_max = std::max(stats_.changed_max, changed);
  if (!flickered) {
    return;
  }
  ++stats_.events;
  stats_.blocks += flickered;
  for (uint32_t i = 1; i <= kMaxBlip; ++i) {
    stats_.by_length[i] += by_length[i];
  }
  const uint64_t event = ++events_;
  std::string lengths;
  for (uint32_t i = 1; i <= kMaxBlip; ++i) {
    if (by_length[i]) {
      lengths += std::format("{}{} of {} frame(s)", lengths.empty() ? "" : ", ", by_length[i], i);
    }
  }
  REXLOG_WARN("[gpu] flicker probe: event {} at frame {}: {} blocks came back to their bytes "
              "of frame {} ({}); box ({},{})-({},{}) px; {} blocks changed this frame",
              event, frame, flickered, frame - longest - 1, lengths, bx0 * kBlock, by0 * kBlock,
              std::min((bx1 + 1) * kBlock, width_), std::min((by1 + 1) * kBlock, height_),
              changed);
  if (dumps_ < max_dumps_ && h[longest + 1] && h[longest]) {
    ++dumps_;
    Dump(event, frame - longest - 1, "before", *h[longest + 1]);
    Dump(event, frame - longest, "blip", *h[longest]);
    Dump(event, frame, "after", *h[0]);
  }
}

void FlickerProbe::Dump(uint64_t event, uint64_t frame, const char* what, const Frame& f) const {
  const std::filesystem::path path =
      rex::filesystem::GetExecutableFolder() /
      std::format("mcla_flicker_{:03}_{:06}_{}.tga", event, frame, what);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    REXLOG_ERROR("[gpu] flicker probe: {} could not be written", path.string());
    return;
  }
  uint8_t header[18] = {};
  header[2] = 2;  // uncompressed true colour
  header[12] = uint8_t(width_);
  header[13] = uint8_t(width_ >> 8);
  header[14] = uint8_t(height_);
  header[15] = uint8_t(height_ >> 8);
  header[16] = 32;
  header[17] = 0x28;  // 8 alpha bits, top-left origin
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  if (format_ == plume::RenderFormat::B8G8R8A8_UNORM) {
    out.write(reinterpret_cast<const char*>(f.image.data()), std::streamsize(f.image.size()));
    return;
  }
  std::vector<uint8_t> bgra(f.image.size());
  for (size_t i = 0; i < f.image.size(); i += 4) {
    bgra[i + 0] = f.image[i + 2];
    bgra[i + 1] = f.image[i + 1];
    bgra[i + 2] = f.image[i + 0];
    bgra[i + 3] = f.image[i + 3];
  }
  out.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
}

std::string FlickerProbe::TakeSummary() {
  if (!enabled_ && stats_.frames == 0) {
    return {};
  }
  const Stats s = stats_;
  stats_ = Stats{};
  std::string lengths;
  for (uint32_t i = 1; i <= kMaxBlip; ++i) {
    lengths += std::format("{}{}", i > 1 ? " " : "", s.by_length[i]);
  }
  return std::format(
      "flicker probe: {} frames compared ({} not probed, {} missing), blocks changed per frame "
      "{:.1f} (max {}), {} frames with flicker, {} blocks (by blip length 1-{}: {})",
      s.frames, s.busy, s.gaps, s.frames ? double(s.changed) / double(s.frames) : 0.0,
      s.changed_max, s.events, s.blocks, kMaxBlip, lengths);
}

}  // namespace mcla::gpu

#include "title_puddle_capture.h"

#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <utility>

#include <rex/logging/log_category.h>
#include <rex/platform/file.h>

#include "constants.h"
#include "descriptors.h"
#include "device.h"
#include "draw_decode.h"
#include "format.h"
#include "frame.h"
#include "mc_engine/hooks/run_ready.h"
#include "resolve.h"
#include "settings/generated/settings_debug.h"
#include "temporal_identities.h"
#include "textures.h"
#include "upload.h"

namespace mcla::gpu {
namespace {
namespace probe = mc::title_puddles;
using probe::Arm;
using probe::Input;
using Format = plume::RenderFormat;
using Layout = plume::RenderTextureLayout;

uint32_t Dxgi(Format format) {
  switch (format) {
    case Format::R16G16B16A16_FLOAT: return 10;
    case Format::R16G16B16A16_UNORM: return 11;
    case Format::R16G16B16A16_SNORM: return 13;
    case Format::R32G32_FLOAT: return 16;
    case Format::R8G8B8A8_UNORM: return 28;
    case Format::R16G16_FLOAT: return 34;
    case Format::R32_FLOAT: return 41;
    case Format::R32_UINT: return 42;
    case Format::R8_UNORM: return 61;
    case Format::B5G5R5A1_UNORM: return 86;
    case Format::B8G8R8A8_UNORM: return 87;
    default: return 0;
  }
}

}  // namespace

void TitlePuddleCapture::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  arm_ = probe::ConfiguredArm();
  const auto targets = larecomp::settings::mcla_dbg_capture_pass.Get();
  if (!targets.empty()) {
    auto plan = title_capture::Parse(targets, larecomp::settings::mcla_dbg_capture_at.Get());
    if (plan) {
      plan_ = std::move(*plan);
    } else {
      REXLOG_WARN("[title capture] invalid targets or seconds list");
    }
  }
  if (enabled()) mc::run_ready::RequestTitleClock();
}

void TitlePuddleCapture::Shutdown() {
  EndFrame();
  for (; next_sample_ < plan_.seconds.size(); ++next_sample_) {
    REXLOG_WARN("[title capture] t{} unavailable: title session ended before the capture frame",
                plan_.seconds[next_sample_]);
  }
  if (heap_ && exposure_slot_ != kInvalidBindlessSlot) {
    heap_->ReleaseTexture(exposure_slot_, TextureDimension::k2D);
    // The release rewrites the slot after its last use. Retire in that order.
    if (frames_) {
      frames_->Retire(std::move(exposure_view_));
      frames_->Retire(std::move(exposure_));
    }
  }
  exposure_slot_ = kInvalidBindlessSlot;
  exposure_view_.reset();
  exposure_.reset();
}

void TitlePuddleCapture::BeginFrame(uint64_t frame) {
  frame_ = frame;
  const auto start = mc::run_ready::TitleStart();
  title_ = start != title_capture::Clock::time_point{};
  if (next_sample_ == plan_.seconds.size()) return;
  const auto now = title_capture::Clock::now();
  if (title_capture::Due(plan_.seconds[next_sample_], start, now)) {
    sample_ = plan_.seconds[next_sample_++];
    elapsed_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    resolve_number_ = 0;
    scene_seen_ = false;
    for (auto& selector : plan_.targets) selector.seen = false;
  }
}

void TitlePuddleCapture::EndFrame() {
  if (!sample_) return;
  for (const auto& selector : plan_.targets) {
    if (!selector.seen) {
      REXLOG_WARN("[title capture] t{} frame {} '{}': no matching resolve write",
                  *sample_, frame_, selector.text);
    }
  }
  if (!scene_seen_) {
    REXLOG_WARN("[title capture] t{} frame {} scene: no pre-tone-map scene resolve", *sample_, frame_);
  }
  sample_.reset();
}

void TitlePuddleCapture::RecordResolve(plume::RenderCommandList& list, const ResolveRecord& record,
                                       const ResolveCache::CaptureWrite& write) {
  if (!sample_ || !write.product) return;
  const bool scene = write.pre_tone_scene;
  const auto& key = write.source;
  const char* format_name = key.depth ? format::DepthTargetFormat(key.format).guest_name
                                      : format::ColorTargetFormat(key.format).guest_name;
  const auto label = std::format("{} {}x {}@{}", key.pitch, key.samples, format_name, key.base_tiles);
  const char* side = write.product->source_view == SideView::kNone
                         ? "" : side_views::Name(write.product->source_view);
  const uint32_t order = ++resolve_number_;
  bool selected = scene;
  for (auto& selector : plan_.targets) {
    if (selector.Matches(key.base_tiles, label, side)) {
      selector.seen = true;
      selected = true;
    }
  }
  if (!selected) return;
  scene_seen_ |= scene;
  const auto name = scene ? std::format("trp3-t{}-scene-{}", *sample_, order)
                         : std::format("trp3-t{}-rt{}-{}", *sample_, key.base_tiles, order);
  const auto detail = std::format("t{} elapsed {}ms frame {} resolve {} '{}' caller {:08X} level {} rect {},{}-{},{} {}",
                                  *sample_, elapsed_ms_, frame_, order, label, record.caller, write.level,
                                  write.x0, write.y0, write.x1, write.y1, write.product->key.Describe());
  if (const char* reason = Save(list, write, name, detail)) {
    REXLOG_WARN("[title capture] {}: {}", detail, reason);
  }
}

bool TitlePuddleCapture::Wants(uint64_t ps) const {
  return title_ && (arm_ == Arm::NoPlanar ||
                    (ps == temporal::kToneMapPs && (arm_ == Arm::FixedExposure || arm_ == Arm::NoBloom)));
}

void TitlePuddleCapture::Record(plume::RenderCommandList& list, ResolveCache& resolve, UploadRing& upload,
                                uint32_t frame_slot, const DrawView& draw, uint32_t vs_mask,
                                uint32_t ps_mask, SharedConstants& shared) {
  for (uint32_t mask = vs_mask | ps_mask; mask; mask &= mask - 1) {
    const uint32_t slot = uint32_t(std::countr_zero(mask));
    uint32_t words[6];
    draw.FetchGroup(slot, words);
    const TextureFetch fetch = ReadTextureFetch(words);
    if (!fetch.is_texture) continue;
    if (arm_ == Arm::NoPlanar) {
      // CHECK: The smoke must log a live consumer of the captured planar resolve.
      const auto* product = resolve.Find(fetch.key, fetch.image_base_page);
      if (product && !product->depth_source && product->source_view == SideView::kReflection &&
          shared.texture_2d[slot] != kNullTexture2DSlot) {
        shared.texture_2d[slot] = kNullTexture2DSlot;
        if (!switch_logged_) {
          REXLOG_INFO("[title puddles] overriding planar input f{} VS {:016X} PS {:016X}",
                      slot, draw.record().vs_identity, draw.record().ps_identity);
          switch_logged_ = true;
        }
      }
      continue;
    }
    const Input role = probe::ClassifyToneInput((vs_mask & (1u << slot)) != 0,
                                               (ps_mask & (1u << slot)) != 0,
                                               fetch.key.width, fetch.key.height);
    // CHECK: match the 1x1 VS exposure and quarter-size PS bloom against
    // the trace. Both overrides replace only this draw's descriptor.
    uint32_t replacement = kInvalidBindlessSlot;
    if (role == Input::Exposure && arm_ == Arm::FixedExposure) replacement = ExposureTexture(list, upload, frame_slot);
    if (role == Input::Bloom && arm_ == Arm::NoBloom) replacement = kNullTexture2DSlot;
    if (replacement != kInvalidBindlessSlot) {
      shared.texture_2d[slot] = replacement;
      if (!switch_logged_) {
        REXLOG_INFO("[title puddles] overriding tone-map {} f{}", role == Input::Exposure ? "exposure" : "bloom", slot);
        switch_logged_ = true;
      }
    }
  }
}

uint32_t TitlePuddleCapture::ExposureTexture(plume::RenderCommandList& list, UploadRing& upload, uint32_t slot) {
  if (exposure_slot_ != kInvalidBindlessSlot || !device_ || !heap_) return exposure_slot_;
  if (!exposure_) {
    exposure_ = device_->CreateTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, Format::R32_FLOAT),
                                       "title puddles fixed exposure");
    if (!exposure_) return kInvalidBindlessSlot;
  }
  if (!exposure_view_) {
    auto desc = plume::RenderTextureViewDesc::Texture2D(Format::R32_FLOAT);
    using Swizzle = plume::RenderSwizzle;
    desc.mipLevels = 1;
    desc.componentMapping = {Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::R};
    exposure_view_ = device_->CreateTextureView(*exposure_, desc, "title puddles fixed exposure");
    if (!exposure_view_) return kInvalidBindlessSlot;
  }
  const UploadSpan input = upload.Allocate(slot, kTextureCopyPitchAlignment, kTextureCopyOffsetAlignment);
  if (!input || !input.cpu) return kInvalidBindlessSlot;
  constexpr float kExposure = 0.18f;
  std::memcpy(input.cpu, &kExposure, sizeof(kExposure));
  exposure_slot_ = heap_->AllocateTexture(*exposure_, *exposure_view_);
  if (exposure_slot_ == kInvalidBindlessSlot) return exposure_slot_;
  list.barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(exposure_.get(), Layout::COPY_DEST));
  list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(exposure_.get()),
                         plume::RenderTextureCopyLocation::PlacedFootprint(
                             input.buffer, Format::R32_FLOAT, 1, 1, 1, uint32_t(kTextureCopyPitchAlignment / 4), input.offset));
  list.barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(exposure_.get(), Layout::SHADER_READ));
  return exposure_slot_;
}

const char* TitlePuddleCapture::Save(plume::RenderCommandList& list, const ResolveCache::CaptureWrite& write,
                                    const std::string& name, const std::string& detail) {
  const auto* product = write.product;
  if (!product || !product->texture || !device_ || !frames_ || !write.width || !write.height ||
      product->layout == Layout::UNKNOWN) return "no readable texture";
  const auto format = product->format;
  const auto layout = product->layout;
  const uint32_t width = write.width, height = write.height;
  const uint32_t dxgi = Dxgi(format);
  if (!dxgi) return "format has no DDS mapping";
  const uint32_t pixel_bytes = plume::RenderFormatSize(format);
  const uint32_t row_bytes = width * pixel_bytes;
  const uint32_t alignment = uint32_t(kTextureCopyPitchAlignment);
  const uint32_t pitch = (row_bytes + alignment - 1) / alignment * alignment;
  auto buffer = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(pitch) * height),
                                       "title puddles target readback");
  if (!buffer) return "could not allocate readback buffer";
  const auto work = frames_->TrackWork();
  // CHECK: confirm the copy and restore barriers on D3D12 and Vulkan.
  if (layout != Layout::COPY_SOURCE) {
    list.barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(product->texture.get(), Layout::COPY_SOURCE));
  }
  list.copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(
                             buffer.get(), format, width, height, 1, pitch / pixel_bytes, 0),
                         plume::RenderTextureCopyLocation::Subresource(product->texture.get(), write.level));
  if (layout != Layout::COPY_SOURCE) {
    list.barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, plume::RenderTextureBarrier(product->texture.get(), layout));
  }
  frames_->RetireAction([work, data = buffer.get(), device = device_, name, detail, dxgi, width, height, row_bytes, pitch] {
    if (!FrameWorkCompleted(work)) {
      REXLOG_WARN("[title capture] {}: capture frame did not complete", detail);
      return;
    }
    const auto* p = data && device && !device->IsLost() ? static_cast<const char*>(data->map()) : nullptr;
    if (!p) {
      REXLOG_WARN("[title capture] {}: could not map readback buffer", detail);
      return;
    }
    // DDS with a DX10 extension. Preserve the float bits and leave row padding out.
    std::array<uint32_t, 37> header{};
    header[0] = 0x20534444;  // DDS magic
    header[1] = 124;
    header[2] = 0x100F;     // caps, width, height, pitch, pixel format
    header[3] = height;
    header[4] = width;
    header[5] = row_bytes;
    header[19] = 32;
    header[20] = 4;        // FourCC
    header[21] = 0x30315844;  // DX10
    header[27] = 0x1000;   // texture
    header[32] = dxgi;
    header[33] = 3;        // texture2D
    header[35] = 1;
    const auto path = rex::platform::GetExecutableFolder() / (name + ".dds");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
    for (uint32_t y = 0; y < height; ++y) {
      file.write(p + size_t(y) * pitch, row_bytes);
    }
    data->unmap();
    file.close();
    if (file) {
      REXLOG_INFO("[title capture] {}: saved {}.dds", detail, name);
    } else {
      REXLOG_WARN("[title capture] {}: could not write {}.dds", detail, name);
    }
  });
  frames_->Retire(std::move(buffer));
  return nullptr;
}

}  // namespace mcla::gpu

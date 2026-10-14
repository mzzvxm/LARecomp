// MCLA GPU renderer -- the guest's front buffer on the window. See present.h.
//
// The fetch the Swap builds and the gamma table it programs were read in the
// IDB (sub_82419E98, sub_82426028, sub_82416000, sub_82426CF0, sub_82426B68;
//). The table's use per 8-bit value follows Xenia's
// apply_gamma_table.xesli (Copyright Ben Vanik and the Xenia authors,
// BSD-3-Clause).

#include "present.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "constants.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_passes.h"
#include "host_shaders.h"
#include "textures.h"
#include "upload.h"

namespace mcla::gpu {

PresentPass::~PresentPass() {
  Shutdown();
}

bool PresentPass::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                             UploadRing& upload, HostPasses& passes, TextureCache& textures,
                             plume::RenderFormat target_format) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  upload_ = &upload;
  passes_ = &passes;
  textures_ = &textures;
  const HostShaderBlob blob = PresentPixelShader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  if (!shader_) {
    return false;
  }
  pipeline_ = passes.CreatePipeline(*shader_, target_format, "present front buffer");
  ramp_texture_ = device.CreateTexture(
      plume::RenderTextureDesc::Texture2D(256, 1, 1, plume::RenderFormat::R32G32B32A32_FLOAT),
      "display gamma table");
  if (!pipeline_ || !ramp_texture_) {
    return false;
  }
  plume::RenderTextureViewDesc view =
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R32G32B32A32_FLOAT);
  view.mipLevels = 1;
  ramp_view_ = device.CreateTextureView(*ramp_texture_, view, "display gamma table");
  if (!ramp_view_) {
    return false;
  }
  ramp_slot_ = heap.AllocateTexture(*ramp_texture_, *ramp_view_);
  if (ramp_slot_ == kInvalidBindlessSlot) {
    return false;
  }
  // The SDK presenter's output effects (FSR and the rest) are the host's
  // optional post, milestone M6; this mode replaces that presenter.
  const std::string effect = rex::cvar::GetFlagByName("present_effect");
  if (!effect.empty() && effect != "bilinear") {
    REXLOG_WARN("[gpu] present: present_effect = \"{}\" belongs to the SDK presenter, which this "
                "mode replaces; the front buffer is scaled bilinear (the host's optional post, "
                "FSR included, is milestone M6)",
                effect);
  }
  return true;
}

void PresentPass::Shutdown() {
  // The GPU is idle and the heap goes later: the slot is not handed back.
  pipeline_.reset();
  shader_.reset();
  ramp_view_.reset();
  ramp_texture_.reset();
  ramp_slot_ = kInvalidBindlessSlot;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  upload_ = nullptr;
  passes_ = nullptr;
  textures_ = nullptr;
}

void PresentPass::OnGuestPresent(const PresentRecord& r) {
  ++stats_.guest_presents;
  if (r.flags & (kPresentFrontBufferUnreadable | kPresentSwapControlUnreadable)) {
    if (!reported_unreadable_) {
      reported_unreadable_ = true;
      REXLOG_ERROR("[gpu] present: the front buffer {:#010x} or the device's swap control could "
                   "not be read (flags {:#x}); the last front buffer stays on screen (reported "
                   "once)",
                   r.front_buffer, r.flags);
    }
  } else if (r.front_buffer != 0) {
    // The fetch D3DDevice_Swap builds from the texture's header.
    uint32_t d[6];
    for (uint32_t i = 0; i < 6; ++i) {
      d[i] = LoadBe32(r.front_buffer_header + guest::kTextureFetchOffset + 4 * i);
    }
    uint32_t format = d[1] & 0x3F;
    if (format == 50) {
      format = 6;  // k_8_8_8_8_AS_16_16_16_16 -> k_8_8_8_8
    } else if (format == 7) {
      format = 54;  // k_2_10_10_10 -> k_2_10_10_10_AS_16_16_16_16
    }
    d[1] = (d[1] & ~0x3Fu) | format;
    const uint32_t control = r.swap_control;
    d[0] = (d[0] & 0xFFFFFC03u) | ((control >> 7) & 0x3FCu);  // signs
    d[3] = (d[3] & 0xFFFFE000u) | ((control >> 17) & 0x1FFEu) |  // swizzle
           ((control & 0x20000u) != 0 ? 1u : 0u);              // num_format
    std::memcpy(front_dwords_, d, sizeof(d));
    have_front_ = true;
  }
  if (!(r.flags & kPresentGammaRampUnreadable) &&
      std::memcmp(ramp_bytes_.data(), r.gamma_ramp, ramp_bytes_.size()) != 0) {
    std::memcpy(ramp_bytes_.data(), r.gamma_ramp, ramp_bytes_.size());
    ramp_dirty_ = true;
  }
}

void PresentPass::Prepare(plume::RenderCommandList* list, uint32_t slot) {
  source_ready_ = false;
  if (ramp_dirty_ || !ramp_written_) {
    bool zero = true;
    for (uint8_t b : ramp_bytes_) {
      zero &= b == 0;
    }
    {
      if (zero && !reported_zero_ramp_ && stats_.guest_presents > 0) {
        reported_zero_ramp_ = true;
        REXLOG_WARN("[gpu] present: the device's gamma table is all zero (not built yet); the "
                    "identity is used until it is (reported once)");
      }
      float texels[256][4];
      for (uint32_t i = 0; i < 256; ++i) {
        for (uint32_t c = 0; c < 3; ++c) {
          if (zero) {
            texels[i][c] = float(i) / 255.0f;
          } else {
            const uint8_t* entry = ramp_bytes_.data() + c * 512 + i * 2;
            const uint32_t value = (uint32_t(entry[0]) << 8 | entry[1]) >> 6;
            texels[i][c] = float(value) / 1023.0f;
          }
        }
        texels[i][3] = 1.0f;
      }
      const UploadSpan span = upload_->Allocate(slot, sizeof(texels), kTextureCopyOffsetAlignment);
      if (span) {
        std::memcpy(span.cpu, texels, sizeof(texels));
        if (ramp_layout_ != plume::RenderTextureLayout::COPY_DEST) {
          list->barriers(plume::RenderBarrierStage::COPY,
                         plume::RenderTextureBarrier(ramp_texture_.get(),
                                                     plume::RenderTextureLayout::COPY_DEST));
        }
        list->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(ramp_texture_.get()),
            plume::RenderTextureCopyLocation::PlacedFootprint(
                span.buffer, plume::RenderFormat::R32G32B32A32_FLOAT, 256, 1, 1, 256,
                span.offset));
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(ramp_texture_.get(),
                                                   plume::RenderTextureLayout::SHADER_READ));
        ramp_layout_ = plume::RenderTextureLayout::SHADER_READ;
        ramp_written_ = true;
        ramp_dirty_ = false;
        ++stats_.ramp_writes;
      } else {
        REXLOG_ERROR("[gpu] present: no upload memory for the gamma table");
      }
    }
  }
  if (!have_front_ || !ramp_written_) {
    return;
  }
  const TextureFetch fetch = ReadTextureFetch(front_dwords_);
  if (!fetch.is_texture) {
    if (!reported_unreadable_) {
      reported_unreadable_ = true;
      REXLOG_ERROR("[gpu] present: the front buffer's fetch is not a texture (reported once)");
    }
    return;
  }
  if ((fetch.signs[0] | fetch.signs[1] | fetch.signs[2] | fetch.signs[3]) != 0 &&
      !reported_signs_) {
    reported_signs_ = true;
    REXLOG_ERROR("[gpu] present: the Swap gives the front buffer signs {}{}{}{}; they are not "
                 "applied (MCLA's swap control has none; reported once)",
                 fetch.signs[0], fetch.signs[1], fetch.signs[2], fetch.signs[3]);
  }
  textures_->Sync();
  const TextureCache::Binding bound = textures_->Bind2D(fetch, list);
  if (!bound.from_product) {
    ++stats_.from_memory;
  }
  source_slot_ = bound.slot & ~kTextureGammaBit;
  source_width_ = fetch.key.width;
  source_height_ = fetch.key.height;
  source_ready_ = true;
}

void PresentPass::Draw(plume::RenderCommandList* list, uint32_t width, uint32_t height) {
  if (!source_ready_ || width == 0 || height == 0 || source_width_ == 0 || source_height_ == 0) {
    return;
  }
  // Letterbox: the largest rectangle of the source's aspect, centred, on
  // whole pixels.
  const double scale = std::min(double(width) / source_width_, double(height) / source_height_);
  const uint32_t w = std::max(uint32_t(std::lround(source_width_ * scale)), 1u);
  const uint32_t h = std::max(uint32_t(std::lround(source_height_ * scale)), 1u);
  const uint32_t x = (width - std::min(w, width)) / 2;
  const uint32_t y = (height - std::min(h, height)) / 2;
  const plume::RenderViewport viewport{float(x), float(y), float(w), float(h)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline_.get());
  Push push{};
  push.source = source_slot_;
  push.ramp = ramp_slot_;
  push.source_width = source_width_;
  push.source_height = source_height_;
  push.origin_x = float(x);
  push.origin_y = float(y);
  push.scale_x = float(source_width_) / float(w);
  push.scale_y = float(source_height_) / float(h);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  ++stats_.drawn;
}

std::string PresentPass::TakeSummary() {
  std::string text = std::format(
      "present: {} guest presents, {} drawn, front buffer from memory {} (not a resolve "
      "product), gamma table written {}",
      stats_.guest_presents, stats_.drawn, stats_.from_memory, stats_.ramp_writes);
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

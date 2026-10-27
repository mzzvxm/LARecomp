// MCLA GPU renderer -- the guest's front buffer on the window. See present.h.
//
// The fetch the Swap builds and the gamma table it programs were read in the
// IDB (sub_82419E98, sub_82426028, sub_82416000, sub_82426CF0, sub_82426B68;
//). The table's use per 8-bit value follows Xenia's
// apply_gamma_table.xesli (Copyright Ben Vanik and the Xenia authors,
// BSD-3-Clause). The FSR1 chain (M6c.1) follows the ReXGlue SDK's
// presenter (derived from Xenia, BSD-3-Clause; src/ui/presenter.cpp, the
// GuestOutputPaintFlow of present_effect = "fsr"): EASU in steps of at most 2x
// per axis, RCAS 1:1, a bilinear stretch for what is left.

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
#include "pass_timing.h"
#include "textures.h"
#include "upload.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_fsr, false, "MCLA/GPU",
    "FSR1 on the present (M6c.1): when the window's letterbox is larger than the "
    "front buffer on both axes, EASU passes (at most 2x per axis each) up to it, then RCAS. "
    "Otherwise the bilinear present. Live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_fsr_sharpness, 0.2, "MCLA/GPU",
    "FSR1 RCAS sharpness reduction in stops: 0 sharpest, 2 softest (the SDK presenter's "
    "present_fsr_sharpness_reduction and its default). Live.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    mcla_gpu_fsr_max_passes, 4, "MCLA/GPU",
    "Most FSR1 EASU passes (each at most 2x per axis); a bilinear stretch finishes what they "
    "leave. The SDK presenter's present_fsr_max_upsampling_passes. Live.")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

// The FSR1 intermediates: float16 keeps the table's 10 bits (Plume has no
// 10:10:10:2) and is a colour target every Vulkan device must support.
constexpr plume::RenderFormat kIntermediateFormat = plume::RenderFormat::R16G16B16A16_FLOAT;

}  // namespace

PresentPass::~PresentPass() {
  Shutdown();
}

bool PresentPass::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                             UploadRing& upload, HostPasses& passes, TextureCache& textures,
                             PassTiming& pass_timing, plume::RenderFormat target_format) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  upload_ = &upload;
  passes_ = &passes;
  textures_ = &textures;
  pass_timing_ = &pass_timing;
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
  // FSR1: the table applied 1:1 by the present's own shader, EASU
  // and RCAS, and the stretch without the table.
  const HostShaderBlob easu = FsrEasuPixelShader();
  const HostShaderBlob rcas = FsrRcasPixelShader();
  const HostShaderBlob stretch = PresentStretchPixelShader();
  easu_shader_ = passes.CreateShader(easu.data, easu.size, easu.name);
  rcas_shader_ = passes.CreateShader(rcas.data, rcas.size, rcas.name);
  stretch_shader_ = passes.CreateShader(stretch.data, stretch.size, stretch.name);
  if (!easu_shader_ || !rcas_shader_ || !stretch_shader_) {
    return false;
  }
  ramp_pipeline_ = passes.CreatePipeline(*shader_, kIntermediateFormat, "fsr1 gamma table");
  easu_pipeline_ = passes.CreatePipeline(*easu_shader_, kIntermediateFormat, "fsr1 easu");
  rcas_pipeline_ = passes.CreatePipeline(*rcas_shader_, target_format, "fsr1 rcas");
  rcas_offscreen_pipeline_ =
      passes.CreatePipeline(*rcas_shader_, kIntermediateFormat, "fsr1 rcas (intermediate)");
  stretch_pipeline_ = passes.CreatePipeline(*stretch_shader_, target_format, "fsr1 stretch");
  if (!ramp_pipeline_ || !easu_pipeline_ || !rcas_pipeline_ || !rcas_offscreen_pipeline_ ||
      !stretch_pipeline_) {
    return false;
  }
  // The SDK presenter's output effects belong to that presenter, which this
  // mode replaces; the renderer has its own FSR1 (mcla_gpu_fsr).
  const std::string effect = rex::cvar::GetFlagByName("present_effect");
  if (!effect.empty() && effect != "bilinear") {
    REXLOG_WARN("[gpu] present: present_effect = \"{}\" belongs to the SDK presenter, which this "
                "mode replaces; it does nothing here. This renderer's FSR1 is mcla_gpu_fsr "
                "(now {})",
                effect, REXCVAR_GET(mcla_gpu_fsr) ? "on" : "off");
  }
  return true;
}

void PresentPass::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back.
  for (HostImage& image : chain_) {
    image = HostImage{};
  }
  stretch_pipeline_.reset();
  rcas_offscreen_pipeline_.reset();
  rcas_pipeline_.reset();
  easu_pipeline_.reset();
  ramp_pipeline_.reset();
  stretch_shader_.reset();
  rcas_shader_.reset();
  easu_shader_.reset();
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
  pass_timing_ = nullptr;
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

void PresentPass::Prepare(plume::RenderCommandList* list, uint32_t slot, uint32_t width,
                          uint32_t height) {
  source_ready_ = false;
  route_ = Route::kBilinear;
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
  const TextureCache::Binding bound = textures_->Bind2D(fetch, 0, list);
  if (!bound.from_product) {
    ++stats_.from_memory;
  }
  source_slot_ = bound.slot & ~kTextureGammaBit;
  // The pass reads texels: a resolve product made at the internal resolution
  // scale holds the guest's size times it.
  source_width_ = fetch.key.width * bound.scale;
  source_height_ = fetch.key.height * bound.scale;
  source_ready_ = true;
  fit_ = FitOf(source_width_, source_height_, width, height);
  if (REXCVAR_GET(mcla_gpu_fsr)) {
    PrepareFsr(list, fit_);
  } else if (!reported_route_.empty()) {
    reported_route_.clear();
    REXLOG_INFO("[gpu] present: FSR1 off (mcla_gpu_fsr); bilinear present");
  }
}

void PresentPass::Draw(plume::RenderCommandList* list, uint32_t width, uint32_t height) {
  if (route_ == Route::kBilinear) {
    if (RecordDraw(list, width, height)) {
      ++stats_.drawn;
    }
    return;
  }
  const HostImage& image = chain_[final_];
  if (fit_.w == 0 || fit_.h == 0) {
    return;
  }
  if (route_ == Route::kFsrStretch) {
    RecordFitted(list, stretch_pipeline_.get(), image.slot, image.width, image.height, fit_);
  } else {
    // RCAS 1:1 from the last EASU pass into the letterbox.
    pass_timing_->Mark(list, "fsr1 rcas");
    const plume::RenderViewport viewport{float(fit_.x), float(fit_.y), float(fit_.w),
                                         float(fit_.h)};
    list->setViewports(&viewport, 1);
    const plume::RenderRect scissor(int32_t(fit_.x), int32_t(fit_.y), int32_t(fit_.x + fit_.w),
                                    int32_t(fit_.y + fit_.h));
    list->setScissors(&scissor, 1);
    passes_->Bind(list);
    list->setPipeline(rcas_pipeline_.get());
    RcasPush push{};
    push.source = image.slot;
    push.sharpness = sharpness_;
    push.output_x = int32_t(fit_.x);
    push.output_y = int32_t(fit_.y);
    push.input_max_x = int32_t(image.width) - 1;
    push.input_max_y = int32_t(image.height) - 1;
    list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    list->drawInstanced(3, 1, 0, 0);
  }
  ++stats_.drawn;
}

void PresentPass::DrawUnscaled(plume::RenderCommandList* list) {
  RecordDraw(list, source_width_, source_height_);
}

PresentPass::Fit PresentPass::FitOf(uint32_t source_width, uint32_t source_height,
                                    uint32_t width, uint32_t height) {
  Fit fit;
  if (source_width == 0 || source_height == 0 || width == 0 || height == 0) {
    return fit;
  }
  const double scale = std::min(double(width) / source_width, double(height) / source_height);
  fit.w = std::max(uint32_t(std::lround(source_width * scale)), 1u);
  fit.h = std::max(uint32_t(std::lround(source_height * scale)), 1u);
  fit.x = (width - std::min(fit.w, width)) / 2;
  fit.y = (height - std::min(fit.h, height)) / 2;
  return fit;
}

bool PresentPass::RecordDraw(plume::RenderCommandList* list, uint32_t width, uint32_t height) {
  if (!source_ready_ || width == 0 || height == 0 || source_width_ == 0 || source_height_ == 0) {
    return false;
  }
  RecordFitted(list, pipeline_.get(), source_slot_, source_width_, source_height_,
               FitOf(source_width_, source_height_, width, height));
  return true;
}

void PresentPass::RecordFitted(plume::RenderCommandList* list, plume::RenderPipeline* pipeline,
                               uint32_t source, uint32_t source_width, uint32_t source_height,
                               const Fit& fit) {
  const plume::RenderViewport viewport{float(fit.x), float(fit.y), float(fit.w), float(fit.h)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(int32_t(fit.x), int32_t(fit.y), int32_t(fit.x + fit.w),
                                  int32_t(fit.y + fit.h));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  Push push{};
  push.source = source;
  push.ramp = ramp_slot_;
  push.source_width = source_width;
  push.source_height = source_height;
  push.origin_x = float(fit.x);
  push.origin_y = float(fit.y);
  push.scale_x = float(source_width) / float(fit.w);
  push.scale_y = float(source_height) / float(fit.h);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// FSR1 (M6c.1)
// ---------------------------------------------------------------------------

void PresentPass::PrepareFsr(plume::RenderCommandList* list, const Fit& fit) {
  // Only an upscale on both axes: EASU is an upscaler, and the letterbox keeps
  // the aspect, so one axis up and the other not is rounding.
  if (fit.w <= source_width_ || fit.h <= source_height_) {
    const std::string route = std::format("bilinear {}x{} -> {}x{}", source_width_,
                                          source_height_, fit.w, fit.h);
    if (route != reported_route_) {
      reported_route_ = route;
      REXLOG_INFO("[gpu] present: FSR1 on, not used: the letterbox {}x{} is not larger than the "
                  "front buffer {}x{} on both axes; bilinear present",
                  fit.w, fit.h, source_width_, source_height_);
    }
    return;
  }
  // The sizes: the source, then each EASU pass at most 2x per axis.
  const uint32_t max_passes =
      std::clamp<uint32_t>(uint32_t(REXCVAR_GET(mcla_gpu_fsr_max_passes)), 1u, kMaxFsrPasses);
  uint32_t passes = 0;
  uint32_t w = source_width_;
  uint32_t h = source_height_;
  if (!EnsureIntermediate(0, w, h)) {
    ++stats_.fsr_failed;
    return;
  }
  while (passes < max_passes && (w < fit.w || h < fit.h)) {
    w = std::min(w * 2, fit.w);
    h = std::min(h * 2, fit.h);
    ++passes;
    if (!EnsureIntermediate(passes, w, h)) {
      ++stats_.fsr_failed;
      return;
    }
  }
  const bool stretch = w != fit.w || h != fit.h;
  if (stretch && !EnsureIntermediate(passes + 1, w, h)) {
    ++stats_.fsr_failed;
    return;
  }
  sharpness_ = float(std::exp2(-REXCVAR_GET(mcla_gpu_fsr_sharpness)));
  const std::string route =
      std::format("fsr1 {}x{} -> {}x{}, {} EASU pass{}{}", source_width_, source_height_, fit.w,
                  fit.h, passes, passes == 1 ? "" : "es",
                  stretch ? std::format(" to {}x{} then bilinear", w, h) : std::string());
  if (route != reported_route_) {
    reported_route_ = route;
    REXLOG_INFO("[gpu] present: {}, RCAS sharpness {:.3f}", route, sharpness_);
  }
  // The table, 1:1 into chain_[0]: the present's own pass.
  pass_timing_->Mark(list, "fsr1 gamma table");
  {
    Push push{};
    push.source = source_slot_;
    push.ramp = ramp_slot_;
    push.source_width = source_width_;
    push.source_height = source_height_;
    push.scale_x = 1.0f;
    push.scale_y = 1.0f;
    passes_->DrawInto(list, chain_[0], ramp_pipeline_.get(), &push, sizeof(push));
  }
  for (uint32_t i = 1; i <= passes; ++i) {
    pass_timing_->Mark(list, "fsr1 easu");
    const HostImage& in = chain_[i - 1];
    HostImage& out = chain_[i];
    EasuPush push{};
    push.source = in.slot;
    push.ratio_x = float(in.width) / float(out.width);
    push.ratio_y = float(in.height) / float(out.height);
    push.input_size_inv_x = 1.0f / float(in.width);
    push.input_size_inv_y = 1.0f / float(in.height);
    passes_->DrawInto(list, out, easu_pipeline_.get(), &push, sizeof(push));
  }
  if (stretch) {
    pass_timing_->Mark(list, "fsr1 rcas");
    const HostImage& in = chain_[passes];
    RcasPush push{};
    push.source = in.slot;
    push.sharpness = sharpness_;
    push.input_max_x = int32_t(in.width) - 1;
    push.input_max_y = int32_t(in.height) - 1;
    passes_->DrawInto(list, chain_[passes + 1], rcas_offscreen_pipeline_.get(), &push,
                      sizeof(push));
    final_ = passes + 1;
    route_ = Route::kFsrStretch;
    ++stats_.fsr_stretched;
  } else {
    final_ = passes;
    route_ = Route::kFsr;
  }
  ++stats_.fsr;
}

bool PresentPass::EnsureIntermediate(uint32_t index, uint32_t width, uint32_t height) {
  return passes_->EnsureImage(chain_[index], width, height, kIntermediateFormat,
                              "fsr1 intermediate");
}

std::string PresentPass::TakeSummary() {
  std::string text = std::format(
      "present: {} guest presents, {} drawn, front buffer from memory {} (not a resolve "
      "product), gamma table written {}, FSR1 {} ({} stretched, {} failed)",
      stats_.guest_presents, stats_.drawn, stats_.from_memory, stats_.ramp_writes, stats_.fsr,
      stats_.fsr_stretched, stats_.fsr_failed);
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

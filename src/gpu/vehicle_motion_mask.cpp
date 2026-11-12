// MCLA GPU renderer -- the mask of the cars' shadow decals. See vehicle_motion_mask.h.

#include "vehicle_motion_mask.h"

#include <format>

#include <rex/logging.h>

#include "host_shaders.h"
#include "pass_timing.h"
#include "pipelines.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] shadow mask:";
constexpr plume::RenderFormat kCoverageFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr plume::RenderFormat kMaskFormat = plume::RenderFormat::R8_UNORM;

// The decals' shader pairs (vehicle_motion_mask.h).
constexpr uint64_t kContactVs = 0xA829AE1437B66C8Bull;
constexpr uint64_t kContactPs = 0x0252424701F07592ull;
constexpr uint64_t kWheelVs = 0x72F466E81A37357Eull;
constexpr uint64_t kWheelPs = 0xB4C34BD40E64FB0Full;

}  // namespace

ShadowMask::~ShadowMask() {
  Shutdown();
}

bool ShadowMask::IsDecal(uint64_t vs_identity, uint64_t ps_identity) {
  return (ps_identity == kContactPs && vs_identity == kContactVs) ||
         (ps_identity == kWheelPs && vs_identity == kWheelVs);
}

void ShadowMask::VariantKey(const PipelineKey& key, PipelineKey* out) {
  *out = key;
  out->color_count = 1;
  out->color_format[0] = uint32_t(kCoverageFormat);
  for (uint32_t i = 1; i < kColorTargetCount; ++i) {
    out->color_format[i] = 0;
    out->blend[i] = 0;
  }
  // Only alpha is written; the layers' union: a + (1 - a) * what is there. The colour part is not
  // written (the write mask), so its factors only have to be valid.
  plume::RenderBlendDesc blend;
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::ONE;
  blend.dstBlend = plume::RenderBlend::ZERO;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ONE;
  blend.dstBlendAlpha = plume::RenderBlend::INV_SRC_ALPHA;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALPHA);
  out->blend[0] = PackBlend(blend);
  out->depth_format = 0;
  out->depth = 0;
  out->stencil = 0;
  out->stencil_masks = 0;
  out->depth_bias = 0;
  out->slope_scaled_depth_bias = 0.0f;
  out->sample_count = 1;
}

void ShadowMask::Initialize(HostPasses& passes, PassTiming& pass_timing) {
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  failed_ = false;
  initialized_ = true;
}

void ShadowMask::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back (TemporalMotion::Shutdown).
  coverage_ = HostImage{};
  mask_ = HostImage{};
  pipeline_.reset();
  shader_.reset();
  mask_valid_ = false;
  drawn_ = false;
  clear_pending_ = true;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  initialized_ = false;
}

void ShadowMask::Release() {
  if (!initialized_ || !passes_) {
    return;
  }
  passes_->ReleaseImage(coverage_);
  passes_->ReleaseImage(mask_);
  mask_valid_ = false;
  drawn_ = false;
  clear_pending_ = true;
}

uint64_t ShadowMask::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage* image : {&coverage_, &mask_}) {
    if (image->texture) {
      bytes += TextureBytes(image->width, image->height, 1, image->format);
    }
  }
  return bytes;
}

void ShadowMask::BeginFrame() {
  clear_pending_ = true;
  drawn_ = false;
  mask_valid_ = false;
}

HostImage* ShadowMask::Target(uint32_t width, uint32_t height, bool* clear) {
  *clear = false;
  if (!initialized_ || failed_ || width == 0 || height == 0) {
    return nullptr;
  }
  if (!passes_->EnsureImage(coverage_, width, height, kCoverageFormat, "shadow decal coverage")) {
    ++stats_.image_failures;
    return nullptr;
  }
  // The frame's first second draw clears the image; so does a new image (its first use).
  if (clear_pending_ || coverage_.layout == plume::RenderTextureLayout::UNKNOWN) {
    *clear = true;
    clear_pending_ = false;
  }
  drawn_ = true;
  return &coverage_;
}

bool ShadowMask::Prepare() {
  if (pipeline_) {
    return true;
  }
  if (failed_ || !initialized_) {
    return false;
  }
  const HostShaderBlob blob = VehicleMotionMaskPixelShader();
  shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
  if (shader_) {
    pipeline_ = passes_->CreatePipeline(*shader_, kMaskFormat, "shadow decal mask");
  }
  if (!pipeline_) {
    failed_ = true;
    shader_.reset();
    REXLOG_ERROR("{} the pass could not be made (shader or pipeline): no mask this session", kPrefix);
    return false;
  }
  return true;
}

bool ShadowMask::Resolve(plume::RenderCommandList* list, uint32_t render_width,
                         uint32_t render_height, uint32_t stencil_ref) {
  mask_valid_ = false;
  const bool had = drawn_;
  drawn_ = false;  // taken: a draw of a later frame starts a new image
  if (!had || !list || !initialized_ || failed_) {
    return false;
  }
  ++stats_.frames;
  if (coverage_.width != render_width || coverage_.height != render_height) {
    // The draws' viewport was not the render size (a decal in a view of another size): no mask, and
    // never a mask that is a different picture from the colour.
    ++stats_.size_mismatch;
    if (!mismatch_logged_) {
      mismatch_logged_ = true;
      REXLOG_WARN("{} the decals' image is {}x{} and the render size is {}x{}: no mask in such "
                  "frames (the first of its kind)",
                  kPrefix, coverage_.width, coverage_.height, render_width, render_height);
    }
    return false;
  }
  if (!Prepare() ||
      !passes_->EnsureImage(mask_, render_width, render_height, kMaskFormat, "shadow decal mask")) {
    ++stats_.image_failures;
    return false;
  }
  // The second draws left the image a render target; the pass reads it.
  if (coverage_.layout != plume::RenderTextureLayout::SHADER_READ) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(coverage_.texture.get(),
                                               plume::RenderTextureLayout::SHADER_READ));
    coverage_.layout = plume::RenderTextureLayout::SHADER_READ;
  }
  MaskPush push{};
  push.coverage = coverage_.slot;
  push.stencil = stencil_ref;
  push.gain = kGain;
  pass_timing_->Mark(list, "shadow decal mask");
  passes_->DrawInto(list, mask_, pipeline_.get(), &push, sizeof(push));
  mask_valid_ = true;
  ++stats_.resolved;
  return true;
}

std::string ShadowMask::Summary() const {
  if (stats_.drawn == 0 && stats_.image_failures == 0) {
    return {};
  }
  return std::format("shadow decals: {} draws recorded again in {} frames, {} masks made ({} frames "
                     "whose image was not the render size, {} image failures)",
                     stats_.drawn, stats_.frames, stats_.resolved, stats_.size_mismatch,
                     stats_.image_failures);
}

}  // namespace mcla::gpu

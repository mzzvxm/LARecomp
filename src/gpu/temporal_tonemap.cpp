// MCLA GPU renderer -- the reversible tone map around the temporal upscalers. See temporal_tonemap.h.

#include "temporal_tonemap.h"

#include <cstring>

#include <rex/logging.h>

#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] temporal tonemap:";
constexpr plume::RenderFormat kFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
// What a backend writes: a compute pass's image.
constexpr plume::RenderTextureFlags kUpscaledFlags =
    plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

}  // namespace

TemporalToneMap::~TemporalToneMap() {
  Shutdown();
}

void TemporalToneMap::Initialize(HostPasses& passes, PassTiming& pass_timing) {
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  failed_ = false;
  initialized_ = true;
}

void TemporalToneMap::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back (TemporalMotion::Shutdown).
  compressed_ = HostImage{};
  upscaled_ = HostImage{};
  compress_pipeline_.reset();
  expand_pipeline_.reset();
  compress_shader_.reset();
  expand_shader_.reset();
  std::memset(logged_sizes_, 0, sizeof(logged_sizes_));
  passes_ = nullptr;
  pass_timing_ = nullptr;
  initialized_ = false;
}

void TemporalToneMap::Release() {
  if (!initialized_ || !passes_) {
    return;
  }
  passes_->ReleaseImage(compressed_);
  passes_->ReleaseImage(upscaled_);
  std::memset(logged_sizes_, 0, sizeof(logged_sizes_));
}

uint64_t TemporalToneMap::HeldBytes() const {
  uint64_t bytes = 0;
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  add(compressed_);
  add(upscaled_);
  return bytes;
}

bool TemporalToneMap::MakePipelines() {
  if (compress_pipeline_ && expand_pipeline_) {
    return true;
  }
  if (failed_ || !initialized_) {
    return false;
  }
  const HostShaderBlob compress = TemporalCompressPixelShader();
  const HostShaderBlob expand = TemporalExpandPixelShader();
  compress_shader_ = passes_->CreateShader(compress.data, compress.size, compress.name);
  expand_shader_ = passes_->CreateShader(expand.data, expand.size, expand.name);
  if (compress_shader_ && expand_shader_) {
    compress_pipeline_ =
        passes_->CreatePipeline(*compress_shader_, kFormat, "temporal tone map compress");
    expand_pipeline_ =
        passes_->CreatePipeline(*expand_shader_, kFormat, "temporal tone map expand");
  }
  if (!compress_pipeline_ || !expand_pipeline_) {
    failed_ = true;
    compress_pipeline_.reset();
    expand_pipeline_.reset();
    REXLOG_ERROR("{} the passes could not be made (shaders or pipelines): no upscaling this session",
                 kPrefix);
    return false;
  }
  return true;
}

bool TemporalToneMap::Prepare(uint32_t render_width, uint32_t render_height,
                              uint32_t output_width, uint32_t output_height) {
  if (!available() || !MakePipelines()) {
    return false;
  }
  if (!passes_->EnsureImage(compressed_, render_width, render_height, kFormat,
                            "temporal upscaler input") ||
      !passes_->EnsureImage(upscaled_, output_width, output_height, kFormat,
                            "temporal upscaler raw output", kUpscaledFlags)) {
    return false;
  }
  if (logged_sizes_[0] != render_width || logged_sizes_[1] != render_height ||
      logged_sizes_[2] != output_width || logged_sizes_[3] != output_height) {
    logged_sizes_[0] = render_width;
    logged_sizes_[1] = render_height;
    logged_sizes_[2] = output_width;
    logged_sizes_[3] = output_height;
    REXLOG_INFO("{} the upscaler works on the scene compressed with c / (1 + max(r, g, b)) and its "
                "output is expanded again: input {}x{} and raw output {}x{}, both R16G16B16A16_FLOAT "
                "({:.1f} MiB held, on top of the upscaler's output image)",
                kPrefix, render_width, render_height, output_width, output_height,
                double(HeldBytes()) / (1024.0 * 1024.0));
  }
  return true;
}

void TemporalToneMap::RecordCompress(plume::RenderCommandList* list, uint32_t color_slot) {
  pass_timing_->Mark(list, "temporal tone map compress");
  const Push push{color_slot};
  passes_->DrawInto(list, compressed_, compress_pipeline_.get(), &push, sizeof(push));
}

void TemporalToneMap::RecordExpand(plume::RenderCommandList* list, HostImage& target) {
  pass_timing_->Mark(list, "temporal tone map expand");
  const Push push{upscaled_.slot};
  passes_->DrawInto(list, target, expand_pipeline_.get(), &push, sizeof(push));
}

}  // namespace mcla::gpu

// MCLA GPU renderer -- the HDR frame at the present: the scene and the UI composed. See
// hdr_scene.h.

#include "hdr_scene.h"

#include <algorithm>
#include <format>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

REXCVAR_DEFINE_INT32(
    mcla_gpu_hdr_layers_view, 0, "MCLA/GPU",
    "Diagnostic (HDR output): what a frame whose UI was kept as a layer shows. 0 the frame, 1 the "
    "UI layer alone, 2 the scene alone, 3 the UI's coverage as grey. Frames without layers show "
    "the front buffer whatever this says. Live.")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

// Float16 holds every 8-bit code value (and the sums of two) to a small fraction of a level, and
// the stage 3 scene above 1.
constexpr plume::RenderFormat kSourceFormat = plume::RenderFormat::R16G16B16A16_FLOAT;

}  // namespace

HdrScene::~HdrScene() {
  Shutdown();
}

bool HdrScene::Initialize(HostPasses& passes, FrameLayers& layers, PassTiming& pass_timing) {
  passes_ = &passes;
  layers_ = &layers;
  pass_timing_ = &pass_timing;
  const HostShaderBlob blob = HdrComposePixelShader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  if (!shader_) {
    return false;
  }
  pipeline_ = passes.CreatePipeline(*shader_, kSourceFormat, "hdr compose");
  return pipeline_ != nullptr;
}

void HdrScene::Shutdown() {
  // The GPU is idle and the heap goes later: the slot is not handed back.
  image_ = HostImage{};
  pipeline_.reset();
  shader_.reset();
  passes_ = nullptr;
  layers_ = nullptr;
  pass_timing_ = nullptr;
}

bool HdrScene::Compose(plume::RenderCommandList* list, const TextureKey& front, uint32_t width,
                       uint32_t height) {
  if (!pipeline_ || !layers_) {
    return false;
  }
  const FrameLayers::Frame frame = layers_->presentable();
  if (!frame.valid) {
    if (++idle_presents_ == FrameLayers::kReleaseAfterFrames) {
      passes_->ReleaseImage(image_);
      composed_generation_ = 0;
    }
    return false;
  }
  idle_presents_ = 0;
  if (!frame.IsPictureOf(front)) {
    layers_->NoteUnusable(true);
    return false;
  }
  if (frame.width < width || frame.height < height) {
    // The front buffer reaches past what the layers cover.
    layers_->NoteUnusable(false);
    return false;
  }
  const uint32_t view = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_hdr_layers_view), 0, 3));
  const bool kept = image_.texture && image_.width == width && image_.height == height;
  if (kept && composed_generation_ == frame.generation && composed_view_ == view) {
    ++stats_.reused;
    return true;
  }
  if (!kept) {
    composed_generation_ = 0;
    if (!passes_->EnsureImage(image_, width, height, kSourceFormat, "hdr source")) {
      ++stats_.failed;
      return false;
    }
  }
  pass_timing_->Mark(list, "hdr compose");
  const Push push{frame.hudless->slot, frame.ui->slot, view, 0};
  passes_->DrawInto(list, image_, pipeline_.get(), &push, sizeof(push));
  composed_generation_ = frame.generation;
  composed_view_ = view;
  ++stats_.composed;
  return true;
}

uint64_t HdrScene::HeldBytes() const {
  return image_.texture ? TextureBytes(image_.width, image_.height, 1, image_.format) : 0;
}

std::string HdrScene::TakeSummary() {
  std::string text = std::format("hdr scene: {} frames composed, {} idle presents reused, {} failed | {:.1f} MiB",
                                 stats_.composed, stats_.reused, stats_.failed,
                                 double(HeldBytes()) / double(1u << 20));
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

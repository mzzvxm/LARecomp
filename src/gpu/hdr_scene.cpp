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
    "UI layer alone, 2 the scene alone, 3 the UI's coverage as grey, 4 magenta where the scene "
    "is above the game's white, 5 the float scene's difference from the 8-bit scene times 16. "
    "Frames without layers show the front buffer whatever this says. Live.")
    .debug_only()
    .range(0, 5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_hdr_compose_in_present, true, "MCLA/GPU",
    "Diagnostic (HDR output): the present's passes that read the frame 1:1 add the UI layer to the "
    "scene themselves, instead of reading a float image the compose pass made of the two. On by "
    "default: one full-screen float16 pass and its image less (about 0.15 ms and 63 MiB at 4K). Off: "
    "the compose pass and its image as before, for comparing the two. A scaled present and the "
    "debug views of mcla_gpu_hdr_layers_view use the image whatever this says. Live.")
    .debug_only()
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

void HdrScene::Release() {
  if (passes_) {
    passes_->ReleaseImage(image_);
  }
  composed_generation_ = 0;
  composed_view_ = 0;
  composed_alpha_ = 1.0f;
  float_scene_ = false;
  idle_presents_ = 0;
  inline_presents_ = 0;
}

bool HdrScene::Select(const TextureKey& front, uint32_t width, uint32_t height, Layers* out) {
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
  // The scene under the UI: the frame's float scene (HDR stage 3) while it is still what the source
  // holds, else the 8-bit one the game's post chain finished.
  out->frame = frame;
  out->scene = frame.hudless;
  out->float_scene = false;
  out->stale = false;
  // The HUD fade: a pause view's frame shows its UI whole (FrameLayers::UiAlphaFor). Both ways of
  // summing the layers take this one value, so a fading HUD looks the same with the compose in the
  // present on and off.
  out->ui_alpha = layers_->UiAlphaFor(frame);
  if (frame.scene && scene_source_) {
    if (frame.scene_epoch == scene_source_->SceneEpoch() && frame.scene->texture &&
        frame.scene->width >= width && frame.scene->height >= height) {
      out->scene = frame.scene;
      out->float_scene = true;
    } else {
      out->stale = true;
    }
  }
  return true;
}

bool HdrScene::ComposeInPresent() const {
  return REXCVAR_GET(mcla_gpu_hdr_compose_in_present) && REXCVAR_GET(mcla_gpu_hdr_layers_view) == 0;
}

void HdrScene::NoteInline(const Layers& layers) {
  ++stats_.inlined;
  stats_.inlined_float += layers.float_scene ? 1 : 0;
  stats_.stale_scene += layers.stale ? 1 : 0;
  // An image left from before (the setting was just turned on, or a scaled present made it): the
  // sum is taken in the present now, so it goes after as many presents as an idle frame waits.
  if (image_.texture && ++inline_presents_ >= FrameLayers::kReleaseAfterFrames) {
    passes_->ReleaseImage(image_);
    composed_generation_ = 0;
    inline_presents_ = 0;
  }
}

bool HdrScene::Compose(plume::RenderCommandList* list, const Layers& layers, uint32_t width,
                       uint32_t height) {
  if (!pipeline_ || !layers_) {
    return false;
  }
  inline_presents_ = 0;
  const FrameLayers::Frame& frame = layers.frame;
  const uint32_t view = uint32_t(std::clamp(REXCVAR_GET(mcla_gpu_hdr_layers_view), 0, 5));
  const bool kept = image_.texture && image_.width == width && image_.height == height;
  const float ui_alpha = layers.ui_alpha;  // the HUD fade Select chose for this frame
  if (kept && composed_generation_ == frame.generation && composed_view_ == view &&
      composed_alpha_ == ui_alpha) {
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
  stats_.stale_scene += layers.stale ? 1 : 0;
  pass_timing_->Mark(list, "hdr compose");
  const Push push{layers.scene_slot(), layers.ui_slot(), view, frame.hudless->slot, frame.gap, ui_alpha};
  passes_->DrawInto(list, image_, pipeline_.get(), &push, sizeof(push));
  composed_generation_ = frame.generation;
  composed_view_ = view;
  composed_alpha_ = ui_alpha;
  float_scene_ = layers.float_scene;
  ++stats_.composed;
  stats_.composed_float += layers.float_scene ? 1 : 0;
  return true;
}

uint64_t HdrScene::HeldBytes() const {
  return image_.texture ? TextureBytes(image_.width, image_.height, 1, image_.format) : 0;
}

std::string HdrScene::TakeSummary() {
  std::string text = std::format(
      "hdr scene: {} frames composed in the present ({} with the float scene), {} composed into the "
      "float image ({} with the float scene), {} lost the float scene before the present, {} idle "
      "presents reused, {} failed | {:.1f} MiB",
      stats_.inlined, stats_.inlined_float, stats_.composed, stats_.composed_float,
      stats_.stale_scene, stats_.reused, stats_.failed, double(HeldBytes()) / double(1u << 20));
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

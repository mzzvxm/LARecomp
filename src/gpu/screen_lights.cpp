// MCLA GPU renderer -- the shared screen products: the lights' share of the bounce light's radiance (GI-BOUNCE-SS). See screen_shared.h, ssgi_lights.h and perf/GI-BOUNCE.md.
//
// (GI-BOUNCE-JOIN: the two sides are those of the lights the screen-space bounce carries: the light pass draws the lights the probe field and the per-pixel pass carry first, outside the pair: gi_bounce_carrier.h.)
// Two plain captures of the main colour at the two sides of the deferred light pass (neon, headlights, street lamps), at the AO's traced size, the MSAA samples averaged (the colour_post capture's shader and
// pipelines: hlsl/screen_colour_ps.hlsl): the colour BEFORE the pass (after the occlusion's apply and the probe field's apply) and the colour AFTER it. The next frame's colour chain (hlsl/ao_color_ps.hlsl)
// reprojects them with the AO's own camera motion and takes their difference as the last frame's light. Nothing here reads a depth and nothing writes the main colour.

#include "screen_shared.h"

#include "cpu_timing.h"

#include <algorithm>
#include <string>

#include <rex/logging/log_category.h>

#include "backend.h"
#include "pass_timing.h"
#include "records.h"
#include "temporal_identities.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kLightsPrefix = "[screen]";
constexpr plume::RenderFormat kLightsFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
// The frames the two images are kept after the last frame that wanted them (a pause spell, a menu: not made again at once).
constexpr uint32_t kLightsReleaseAfterFrames = 120;
constexpr uint32_t kLightsMinRenderSize = 64;

}  // namespace

ScreenShared::LightHistory ScreenShared::LightHistoryFor(bool pause, uint32_t width, uint32_t height, bool ao_history) const {
  LightHistory out;
  if (!lights_pre_.texture || !lights_post_.texture) {
    return out;
  }
  out.usable = ssgi_lights::Usable(lights_key_, frame_count_, pause, width, height, ao_history);
  if (out.usable) {
    out.post_slot = lights_post_.slot;
    out.pre_slot = lights_pre_.slot;
  }
  return out;
}

void ScreenShared::ReleaseLights() {
  if (!passes_) {
    return;
  }
  for (HostImage* image : {&lights_pre_, &lights_post_}) {
    if (image->texture) {
      passes_->ReleaseImage(*image);
    }
  }
  lights_key_ = ssgi_lights::Key{};
  lights_idle_frames_ = 0;
}

void ScreenShared::LightsBeginFrame() {
  // Called from BeginFrame: the wants are the last frame's until the AO says this frame's (it plans after this). The images go back after a couple of seconds without a want.
  lights_before_done_ = false;
  lights_after_done_ = false;
  if (lights_wanted_) {
    lights_idle_frames_ = 0;
  } else if ((lights_pre_.texture || lights_post_.texture) && ++lights_idle_frames_ >= kLightsReleaseAfterFrames) {
    ReleaseLights();
  }
  lights_wanted_ = false;
}

bool ScreenShared::CaptureLightsInto(HostImage& image, const char* label, const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool* pause_out) {
  (void)record;
  if (!list || !passes_ || !edram_ || !summary.source || !LightsFrameMade()) {
    return false;  // no AO frame with the bounce light ran at this trigger (or, for a residual pair, no screen frame)
  }
  const uint32_t width = frame_.traced[0];
  const uint32_t height = frame_.traced[1];
  const uint32_t render_w = frame_.render[0];
  const uint32_t render_h = frame_.render[1];
  if (width == 0 || height == 0 || render_w < kLightsMinRenderSize || render_h < kLightsMinRenderSize) {
    return false;
  }
  EdramTargets::Target& depth = *summary.source;
  // The main colour's identity beside the depth's, at the same samples (CapturePost, ao.cpp PrepareGi).
  EdramKey colour_key;
  colour_key.base_tiles = temporal::kMainColorBase;
  colour_key.pitch = temporal::kMainPitch;
  colour_key.depth = 0;
  colour_key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  colour_key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(colour_key);
  bool unset = false;
  EdramTargets::Target* colour = found && found->texture ? edram_->Get(colour_key, found->height, found->scale, 0, list, &unset) : nullptr;
  if (!colour || colour->host_samples != depth.host_samples || colour->scale != depth.scale) {
    return false;
  }
  if (!ColourPipelinesReady(false)) {
    return false;  // listed with the host precache and made in the background: a frame never waits for a compile (the capture is the colour_post capture's pipelines)
  }
  const bool ms = depth.host_samples > 1;
  plume::RenderPipeline* pipeline = ColourPipeline(ms ? kColourMs : kColourPlain);
  if (!pipeline) {
    return false;
  }
  if (!passes_->EnsureImage(image, width, height, kLightsFormat, label)) {
    if (!lights_failed_logged_) {
      lights_failed_logged_ = true;
      REXLOG_ERROR("{} the bounce light's lights images could not be made: the bounce light takes no light from the lights while that lasts (reported once)", kLightsPrefix);
    }
    return false;
  }
  uint32_t colour_slot = 0;
  uint32_t colour_stencil = 0;
  if (!edram_->SampleSlots(*colour, &colour_slot, &colour_stencil)) {
    return false;
  }
  // The colour plane is read as a texture; the light pass (before) and the game's next draws (after) move it back themselves.
  edram_->Transition(list, *colour, plume::RenderTextureLayout::SHADER_READ);
  ColourPush push;
  push.slots[0] = colour_slot;
  push.slots[1] = depth.host_samples;
  push.slots[2] = (frame_.flags & screen::kFrameHalfTraced) != 0 ? 2u : 1u;
  push.sizes[0] = width;
  push.sizes[1] = height;
  push.sizes[2] = render_w;
  push.sizes[3] = render_h;
  pass_timing_->Mark(list, label);
  passes_->DrawInto(list, image, pipeline, &push, sizeof(push));
  *pause_out = temporal::MainDepthSamples(summary.source_key) == temporal::kPauseSamples;
  return true;
}

bool ScreenShared::CaptureLightsBefore(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  const cpu_timing::Scope cpu_scope(cpu_timing::Section::kScreenCapture);
  if (!ssgi_lights::CaptureBefore(lights_wanted_, LightsFrameMade(), true) || lights_before_done_) {
    return false;
  }
  bool pause = false;
  if (!CaptureLightsInto(lights_pre_, "ssgi lights before", record, summary, list, &pause)) {
    return false;
  }
  lights_before_done_ = true;
  lights_pause_ = pause;
  return true;
}

bool ScreenShared::CaptureLightsAfter(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  const cpu_timing::Scope cpu_scope(cpu_timing::Section::kScreenCapture);
  if (!ssgi_lights::CaptureAfter(lights_wanted_, LightsFrameMade(), lights_before_done_, true) || lights_after_done_) {
    return false;
  }
  bool pause = false;
  if (!CaptureLightsInto(lights_post_, "ssgi lights after", record, summary, list, &pause) || pause != lights_pause_) {
    return false;
  }
  lights_after_done_ = true;
  // Both are made in this frame, at the traced size, of this kind of view: the next frame's chain may use them (ssgi_lights.h Usable).
  lights_key_.valid = true;
  lights_key_.frame = frame_count_;
  lights_key_.pause = pause;
  lights_key_.size[0] = lights_pre_.width;
  lights_key_.size[1] = lights_pre_.height;
  if (!lights_first_logged_) {
    lights_first_logged_ = true;
    REXLOG_INFO("{} the first lights capture for the bounce light: {}x{} RGBA16F, the colour before and after the deferred light pass ({:.1f} MiB for the two; the next frame's colour chain adds the difference "
                "where it reprojects into it, perf/GI-BOUNCE.md)",
                kLightsPrefix, lights_pre_.width, lights_pre_.height, double(2 * TextureBytes(lights_pre_.width, lights_pre_.height, 1, lights_pre_.format)) / (1024.0 * 1024.0));
  }
  return true;
}

}  // namespace mcla::gpu

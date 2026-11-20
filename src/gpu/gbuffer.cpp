// MCLA GPU renderer -- the G-buffer's sidecar. See gbuffer.h.

#include "gbuffer.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "host_shaders.h"
#include "pass_timing.h"
#include "records.h"
#include "temporal_identities.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gbuffer]";
constexpr plume::RenderFormat kNormalFormat = plume::RenderFormat::R16G16_UNORM;
constexpr uint32_t kIntervalFrames = 600;  // the scene's own logging interval
constexpr uint32_t kWindowLogs = 6;        // the first frames' window lines
constexpr uint32_t kTopShaders = 6;

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

// "ABCD...=n, ..." of the most used identities of a map, most first.
std::string TopOf(const std::unordered_map<uint64_t, uint32_t>& counts) {
  std::vector<std::pair<uint64_t, uint32_t>> items(counts.begin(), counts.end());
  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  std::string text;
  for (size_t i = 0; i < items.size() && i < kTopShaders; ++i) {
    if (!text.empty()) text += ", ";
    text += std::format("{:016X} x{}", items[i].first, items[i].second);
  }
  return text.empty() ? std::string("none") : text;
}

}  // namespace

GBuffer::~GBuffer() = default;

void GBuffer::Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
}

void GBuffer::Shutdown() {
  view_pipelines_.clear();
  view_shaders_ = {};
  if (passes_ && image_.texture) {
    ReleaseImage();
  }
}

uint64_t GBuffer::HeldBytes() const {
  return image_.texture ? held_bytes_ : 0;
}

void GBuffer::BeginFrame(uint64_t frame_serial) {
  frame_serial_ = frame_serial;
  frame_ = gbuffer::GBufferFrame{};
  window_.BeginFrame();
  const gbuffer::Setting setting = gbuffer::SettingOfWord(larecomp::settings::mcla_gpu_gbuffer.Get());
  // No plane has a consumer yet (auto asks for none); the spike writes the normal plane only, whatever `full` asks for.
  const uint32_t planes = gbuffer::PlanesFor(setting, 0) & gbuffer::PlaneBit(gbuffer::kPlaneNormal);
  view_ = gbuffer::ViewOf(larecomp::settings::mcla_gpu_gbuffer_view.Get());
  if (planes != planes_) {
    REXLOG_INFO("{} the G-buffer is {} (mcla_gpu_gbuffer; live)", kPrefix, planes != 0 ? "on: the normal plane" : "off");
    planes_ = planes;
    if (planes_ == 0 && image_.texture) {
      ReleaseImage();
    }
  }
  if (planes_ == 0) {
    return;
  }
  ++active_frames_;
  if (++interval_frames_ >= kIntervalFrames) {
    LogInterval();
  }
}

void GBuffer::NoteDraw(gbuffer::Why why, uint64_t ps_identity) {
  ++counts_[size_t(why)];
  if (why == gbuffer::Why::kWrites) {
    ++written_by_[ps_identity];
  } else if (why == gbuffer::Why::kMaskedNoVariant) {
    ++masked_by_[ps_identity];
  }
}

void GBuffer::Transition(plume::RenderCommandList* list, plume::RenderTextureLayout layout) {
  if (image_.layout == layout) {
    return;
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image_.texture.get(), layout));
  image_.layout = layout;
}

bool GBuffer::EnsureImage(uint32_t width, uint32_t height, uint32_t samples) {
  samples = std::max<uint32_t>(samples, 1);
  if (image_.texture && image_.width == width && image_.height == height && image_.samples == samples) {
    return true;
  }
  // The framebuffers that hold the old texture go first (after the lists that still use them); the image's own release is HostPasses'.
  if (image_.texture) {
    edram_->RetireFramebuffersWithExtra(image_.texture.get());
  }
  if (!passes_->EnsureImage(image_, width, height, kNormalFormat, "gbuffer normal", plume::RenderTextureFlag::RENDER_TARGET, samples)) {
    return false;
  }
  ++generation_;
  needs_clear_ = true;
  held_bytes_ = TextureBytes(width, height, 1, kNormalFormat, samples);
  REXLOG_INFO("{} sidecar: the normal plane {}x{} R16G16_UNORM, {} sample{} ({:.1f} MiB), generation {}", kPrefix, width, height, samples,
              samples == 1 ? "" : "s", Mib(held_bytes_), generation_);
  return true;
}

void GBuffer::ReleaseImage() {
  if (image_.texture) {
    edram_->RetireFramebuffersWithExtra(image_.texture.get());
  }
  passes_->ReleaseImage(image_);
  held_bytes_ = 0;
  frame_ = gbuffer::GBufferFrame{};
}

void GBuffer::ClearRect(plume::RenderCommandList* list, const plume::RenderRect& host) {
  Transition(list, plume::RenderTextureLayout::COLOR_WRITE);
  list->setFramebuffer(image_.framebuffer.get());
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f), &host, 1);
  framebuffer_dirty_ = true;
}

plume::RenderTexture* GBuffer::Attach(plume::RenderCommandList* list, EdramTargets::Target& colour0) {
  if (!EnsureImage(colour0.scale.Edge(colour0.width), colour0.scale.Edge(colour0.height), colour0.host_samples)) {
    return nullptr;
  }
  const bool no_clear_yet = window_.GateForDraw() == gbuffer::Window::Gate::kNeedsClear;
  if (needs_clear_ || no_clear_yet) {
    // A draw of the frame came before the guest's clear of the main colour (or the image is new): the whole image is made "no data" first.
    if (list) {
      ClearRect(list, plume::RenderRect(0, 0, int32_t(image_.width), int32_t(image_.height)));
    }
    needs_clear_ = false;
    if (no_clear_yet) {
      window_.OnInitialClear();
      ++initial_clears_;
      if (window_logs_ < kWindowLogs) {
        ++window_logs_;
        REXLOG_INFO("{} frame {}: a main view draw came before any clear of the main colour: the sidecar was cleared whole first", kPrefix, frame_serial_);
      }
    }
  }
  Transition(list, plume::RenderTextureLayout::COLOR_WRITE);
  return image_.texture.get();
}

void GBuffer::OnColorClear(plume::RenderCommandList* list, EdramTargets::Target& colour0, const plume::RenderRect& guest_rect) {
  if (!active() || !list) {
    return;
  }
  if (window_.triggered()) {
    ++late_clears_;  // after the frame was published: the image is the consumers', the clear belongs to something else
    return;
  }
  if (!EnsureImage(colour0.scale.Edge(colour0.width), colour0.scale.Edge(colour0.height), colour0.host_samples)) {
    return;
  }
  plume::RenderRect rect = guest_rect;
  rect.right = std::min(rect.right, int32_t(colour0.width));
  rect.bottom = std::min(rect.bottom, int32_t(colour0.height));
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }
  const bool whole = rect.left <= 0 && rect.top <= 0 && rect.right >= int32_t(colour0.width) &&
                     rect.bottom + int32_t(EdramTargets::kHeightAlignment) >= int32_t(colour0.height);
  // A new image has undefined contents: whatever the rectangle, the whole of it is cleared now.
  const plume::RenderRect host = needs_clear_ ? plume::RenderRect(0, 0, int32_t(image_.width), int32_t(image_.height))
                                              : EdramTargets::ScaledRect(rect, colour0.scale);
  ClearRect(list, host);
  needs_clear_ = false;
  window_.OnClear(whole);
  ++clears_;
  if (!whole) ++partial_clears_;
  if (window_logs_ < kWindowLogs) {
    ++window_logs_;
    REXLOG_INFO("{} frame {}: the guest cleared the main colour ({},{})-({},{}) of {}x{}: {} clear of the sidecar, clear {} of the frame", kPrefix, frame_serial_,
                rect.left, rect.top, rect.right, rect.bottom, colour0.width, colour0.height, whole ? "whole" : "partial", window_.clears());
  }
}

bool GBuffer::OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  if (!active() || !list) {
    return false;
  }
  gbuffer::TriggerFacts facts;
  facts.depth_copy = summary.depth_copy;
  facts.main_depth_samples = summary.depth_copy ? temporal::MainDepthSamples(summary.source_key) : 0u;
  facts.dest_address = summary.dest_address;
  facts.clears_depth = summary.clears_depth;
  facts.inside_end_tiling = (record.flags & kEventInsideEndTiling) != 0;
  facts.first_this_frame = !window_.triggered();
  if (!gbuffer::IsTrigger(facts)) {
    return false;
  }
  window_.OnTrigger();
  if (!image_.texture || !window_.had_clear()) {
    return false;  // the frame carried no sidecar (a view with nothing of the family, or the image could not be made)
  }
  Transition(list, plume::RenderTextureLayout::SHADER_READ);
  frame_ = gbuffer::GBufferFrame{};
  frame_.generation = generation_;
  frame_.frame_serial = uint32_t(frame_serial_);
  frame_.width = image_.width;
  frame_.height = image_.height;
  frame_.host_samples = image_.samples;
  frame_.sample0 = summary.host_sample;
  frame_.valid = gbuffer::PlaneBit(gbuffer::kPlaneNormal);
  frame_.slot[gbuffer::kPlaneNormal] = image_.slot;
  ++published_frames_;
  return true;
}

plume::RenderPipeline* GBuffer::ViewPipelineFor(plume::RenderFormat format, uint32_t samples) {
  for (ViewPipeline& v : view_pipelines_) {
    if (v.format == format && v.samples == samples) {
      return v.pipeline.get();
    }
  }
  if (view_shaders_failed_) {
    return nullptr;
  }
  const size_t slot = samples > 1 ? 1 : 0;
  if (!view_shaders_[slot]) {
    const HostShaderBlob blob = GBufferViewPixelShader(samples > 1);
    view_shaders_[slot] = passes_->CreateShader(blob.data, blob.size, blob.name);
    if (!view_shaders_[slot]) {
      view_shaders_failed_ = true;
      REXLOG_ERROR("{} the debug view's shader could not be made: no debug view (reported once)", kPrefix);
      return nullptr;
    }
  }
  ViewPipeline v;
  v.format = format;
  v.samples = samples;
  v.pipeline = passes_->CreatePipeline(passes_->layout(), *view_shaders_[slot], format, plume::RenderBlendDesc::Copy(), samples, "gbuffer view");
  if (!v.pipeline) {
    view_shaders_failed_ = true;
    REXLOG_ERROR("{} the debug view's pipeline could not be made: no debug view (reported once)", kPrefix);
    return nullptr;
  }
  plume::RenderPipeline* raw = v.pipeline.get();
  view_pipelines_.push_back(std::move(v));
  return raw;
}

bool GBuffer::DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  if (view_ == gbuffer::View::kOff || !list || !summary.source || !frame_.Has(gbuffer::kPlaneNormal)) {
    return false;
  }
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != frame_.host_samples || found->scale.Edge(found->width) != frame_.width ||
      found->scale.Edge(found->height) != frame_.height) {
    return false;
  }
  plume::RenderPipeline* pipeline = ViewPipelineFor(found->format, found->host_samples);
  if (!pipeline) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  ViewPush push;
  push.slots[0] = frame_.slot[gbuffer::kPlaneNormal];
  push.slots[1] = frame_.slot[gbuffer::kPlaneAlbedo];
  push.slots[2] = frame_.slot[gbuffer::kPlaneMaterial];
  push.slots[3] = uint32_t(view_);
  push.info[0] = frame_.sample0;
  push.info[1] = frame_.valid;
  pass_timing_->Mark(list, "gbuffer view");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(frame_.width), float(frame_.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(frame_.width), int32_t(frame_.height));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

void GBuffer::LogInterval() {
  const double frames = double(std::max<uint64_t>(active_frames_, 1));
  const auto n = [&](gbuffer::Why why) { return double(counts_[size_t(why)]) / frames; };
  REXLOG_INFO("{} {} frames with the G-buffer on, {} published: draws a frame: writes {:.1f}, masked {:.1f} (blended {:.1f}, no variant {:.1f}, other variant {:.1f}, no colour {:.1f}), "
              "not carried: not the main view {:.1f}, pause view {:.1f}, window closed {:.1f}; clears {} ({} partial, {} before any clear, {} after the trigger); "
              "sidecar {:.1f} MiB",
              kPrefix, active_frames_, published_frames_, n(gbuffer::Why::kWrites),
              n(gbuffer::Why::kMaskedBlended) + n(gbuffer::Why::kMaskedNoVariant) + n(gbuffer::Why::kMaskedOtherVariant) + n(gbuffer::Why::kMaskedNoColor),
              n(gbuffer::Why::kMaskedBlended), n(gbuffer::Why::kMaskedNoVariant), n(gbuffer::Why::kMaskedOtherVariant), n(gbuffer::Why::kMaskedNoColor),
              n(gbuffer::Why::kNotMainView), n(gbuffer::Why::kPauseView), n(gbuffer::Why::kWindowClosed), clears_, partial_clears_, initial_clears_, late_clears_,
              Mib(HeldBytes()));
  REXLOG_INFO("{} pixel shaders that wrote it: {}; carried it with no variant (the next families): {}", kPrefix, TopOf(written_by_), TopOf(masked_by_));
  interval_frames_ = 0;
  active_frames_ = 0;
  published_frames_ = 0;
  counts_.fill(0);
  clears_ = 0;
  partial_clears_ = 0;
  initial_clears_ = 0;
  late_clears_ = 0;
  written_by_.clear();
  masked_by_.clear();
}

}  // namespace mcla::gpu

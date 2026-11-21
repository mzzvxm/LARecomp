// MCLA GPU renderer -- the shared screen-space products: the owner. See screen_shared.h.

#include "screen_shared.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "records.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[screen]";
// The frames the Hi-Z's images are kept after the last frame that wanted it (a pause spell, a menu: not made again at once).
constexpr uint32_t kHizReleaseAfterFrames = 120;
constexpr plume::RenderFormat kHizFormat = plume::RenderFormat::R32_FLOAT;

constexpr const char* kHizNames[screen::kHizMaxLevels] = {"screen hiz 0", "screen hiz 1", "screen hiz 2", "screen hiz 3",
                                                          "screen hiz 4", "screen hiz 5", "screen hiz 6", "screen hiz 7"};

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

uint32_t SlotOf(const HostImage* image) {
  return image && image->texture ? image->slot : screen::kNoSlot;
}

// "lin_depth, hiz_min" of a mask.
std::string ProductNames(uint32_t products) {
  std::string text;
  for (uint32_t p = 0; p < screen::kProductCount; ++p) {
    if ((products & (1u << p)) == 0) continue;
    if (!text.empty()) text += ", ";
    text += screen::ProductName(screen::Product(p));
  }
  return text.empty() ? std::string("none") : text;
}

}  // namespace

ScreenShared::~ScreenShared() = default;

void ScreenShared::Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  const HostShaderBlob hiz = ScreenHizPixelShader();
  hiz_shader_ = passes.CreateShader(hiz.data, hiz.size, hiz.name);
  if (!hiz_shader_) {
    hiz_failed_ = true;
    REXLOG_ERROR("{} the Hi-Z's shader could not be made: no hiz_min this session", kPrefix);
  }
  const HostShaderBlob view = ScreenViewPixelShader();
  view_shader_ = passes.CreateShader(view.data, view.size, view.name);
  if (!view_shader_) {
    view_failed_ = true;
    REXLOG_ERROR("{} the debug view's shader could not be made: no debug view this session", kPrefix);
  }
}

void ScreenShared::Shutdown() {
  // The host builder's function names the Hi-Z's shader: stop it first (AmbientOcclusion::Shutdown does the same; stopping twice is safe).
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (AmbientOcclusion::Shutdown).
  view_pipelines_.clear();
  hiz_pipeline_.reset();
  hiz_shader_.reset();
  view_shader_.reset();
  for (HostImage& image : hiz_) {
    image = HostImage{};
  }
  hiz_images_ = 0;
  frame_ = screen::ScreenFrame{};
  adopted_ = Adopted{};
  published_ = false;
}

void ScreenShared::BeginFrame(uint64_t frame_serial) {
  frame_serial_ = frame_serial;
  // The frame is empty until the AO's depth chain publishes it.
  frame_ = screen::ScreenFrame{};
  frame_.frame_serial = uint32_t(frame_serial);
  adopted_ = Adopted{};
  published_ = false;
  plan_ = screen::PlanFrame(wants_, screen::ViewFromSetting(larecomp::settings::mcla_gpu_screen_view.Get()),
                            screen::LevelFromSetting(larecomp::settings::mcla_gpu_screen_view_level.Get()));
  if (plan_.wanted != logged_wanted_) {
    LogWants();
  }
  if ((plan_.make & screen::ProductBit(screen::kHizMin)) != 0) {
    hiz_idle_frames_ = 0;
  } else if (hiz_images_ != 0 && ++hiz_idle_frames_ >= kHizReleaseAfterFrames) {
    ReleaseHiz();
  }
}

void ScreenShared::LogWants() {
  logged_wanted_ = plan_.wanted;
  std::string who;
  for (uint32_t c = 0; c < uint32_t(screen::Consumer::kCount); ++c) {
    const screen::Consumer consumer = screen::Consumer(c);
    const uint32_t products = consumer == screen::Consumer::kDebugView ? screen::ViewNeeds(plan_.view) : wants_.Of(consumer);
    if (products == 0) continue;
    who += std::format("{}{} ({})", who.empty() ? "" : "; ", screen::ConsumerName(consumer), ProductNames(products));
  }
  REXLOG_INFO("{} products wanted: {}{}{}", kPrefix, ProductNames(plan_.wanted), who.empty() ? "" : " by ", who);
}

// ---------------------------------------------------------------------------
// the Hi-Z
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> ScreenShared::MakeHizPipeline() {
  if (!hiz_shader_) {
    return nullptr;
  }
  // The host pass layout (a push block of 32 bytes), one R32_FLOAT target, written whole: the full-screen triangle's pipeline.
  return passes_->CreatePipeline(*hiz_shader_, kHizFormat, "screen hiz");
}

plume::RenderPipeline* ScreenShared::HizPipeline() {
  if (hiz_pipeline_) {
    return hiz_pipeline_.get();
  }
  if (hiz_failed_ || !hiz_shader_) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  const uint64_t key = HostPrecache::Key(HostPrecache::kScreenShared, 0);
  if (!hiz_listed_) {
    hiz_listed_ = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({key, "screen hiz", [this] { return MakeHizPipeline(); }});
    const uint32_t count = precache.AddLive("screen products", std::move(items));
    REXLOG_INFO("{} hiz_min is wanted: its pipeline ({}) is made in the background first, and the chain is made from the frame it is done", kPrefix, count);
  }
  if (!precache.Ready(key)) {
    return nullptr;  // a frame never waits for a compile: no hiz_min this frame
  }
  hiz_pipeline_ = precache.Acquire(key, "screen hiz", [this] { return MakeHizPipeline(); });
  if (!hiz_pipeline_) {
    hiz_failed_ = true;
    REXLOG_ERROR("{} the Hi-Z's pipeline could not be made: no hiz_min this session", kPrefix);
  }
  return hiz_pipeline_.get();
}

uint32_t ScreenShared::RecordHiz(plume::RenderCommandList* list, const HostImage& depth0, uint32_t slots[screen::kHizMaxLevels]) {
  plume::RenderPipeline* pipeline = HizPipeline();
  if (!pipeline || !list || !depth0.texture || depth0.width == 0 || depth0.height == 0) {
    return 0;
  }
  const uint32_t levels = screen::HizLevelCount(depth0.width, depth0.height);
  // The images: one a level, kept while the size is; a different size remakes them (and the generation says so).
  bool ok = true;
  for (uint32_t level = 0; level < levels && ok; ++level) {
    const plume::RenderTexture* before = hiz_[level].texture.get();
    ok = passes_->EnsureImage(hiz_[level], screen::HizLevelSize(depth0.width, level), screen::HizLevelSize(depth0.height, level), kHizFormat, kHizNames[level]);
    if (ok && hiz_[level].texture.get() != before) {
      ++hiz_generation_;
    }
  }
  for (uint32_t level = levels; level < screen::kHizMaxLevels; ++level) {
    if (hiz_[level].texture) {
      passes_->ReleaseImage(hiz_[level]);
      ++hiz_generation_;
    }
  }
  uint32_t held = 0;
  for (uint32_t level = 0; level < screen::kHizMaxLevels; ++level) {
    held += hiz_[level].texture ? 1u : 0u;
  }
  hiz_images_ = held;
  if (!ok) {
    if (!hiz_images_failed_logged_) {
      hiz_images_failed_logged_ = true;
      REXLOG_ERROR("{} the Hi-Z's images could not be made: no hiz_min while that lasts (asked again every frame)", kPrefix);
    }
    return 0;
  }
  // CHECK (not run): the pass reads the AO's level 0 depth in the shader-read layout it was left in (DrawInto leaves its own images there too; the ray traced occlusion's compute read keeps
  // that layout, car_rt_ao.cpp), on both APIs, with the Vulkan synchronisation validation on; and the seven passes' cost (expected below 0.05 ms at 1080p traced: 7 tiny R32_FLOAT passes).
  pass_timing_->Mark(list, "screen hiz");
  for (uint32_t level = 0; level < levels; ++level) {
    const HostImage& source = level == 0 ? depth0 : hiz_[level - 1];
    HizPush push;
    push.slots[0] = source.slot;
    push.slots[1] = level == 0 ? 0u : 1u;
    push.sizes[0] = source.width;
    push.sizes[1] = source.height;
    push.sizes[2] = hiz_[level].width;
    push.sizes[3] = hiz_[level].height;
    passes_->DrawInto(list, hiz_[level], pipeline, &push, sizeof(push));
    slots[level] = hiz_[level].slot;
  }
  if (hiz_logged_size_[0] != depth0.width || hiz_logged_size_[1] != depth0.height) {
    hiz_logged_size_[0] = depth0.width;
    hiz_logged_size_[1] = depth0.height;
    REXLOG_INFO("{} hiz_min: {} levels from {}x{} down to {}x{} (R32_FLOAT, the min of 2x2 each, level 0 the linear depth with its marks off), {:.1f} MiB", kPrefix, levels,
                depth0.width, depth0.height, screen::HizLevelSize(depth0.width, levels - 1), screen::HizLevelSize(depth0.height, levels - 1),
                Mib(screen::HizBytes(depth0.width, depth0.height)));
  }
  return levels;
}

void ScreenShared::ReleaseHiz() {
  for (HostImage& image : hiz_) {
    if (image.texture) {
      passes_->ReleaseImage(image);
    }
  }
  hiz_images_ = 0;
  hiz_idle_frames_ = 0;
  ++hiz_generation_;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void ScreenShared::Publish(plume::RenderCommandList* list, const Adopted& adopted) {
  adopted_ = adopted;
  published_ = false;
  frame_ = screen::ScreenFrame{};
  frame_.frame_serial = uint32_t(frame_serial_);
  const HostImage* depth0 = adopted.depth[0];
  if (!depth0 || !depth0->texture) {
    return;  // no depth chain this frame: nothing to publish
  }
  screen::FrameInputs in;
  in.serial = uint32_t(frame_serial_);
  in.traced[0] = depth0->width;
  in.traced[1] = depth0->height;
  in.render[0] = adopted.render[0];
  in.render[1] = adopted.render[1];
  in.host_samples = adopted.host_samples;
  in.sample0 = adopted.sample0;
  in.flags = (adopted.pause_view ? screen::kFramePause : 0u);
  if (in.traced[0] < in.render[0] || in.traced[1] < in.render[1]) {
    in.flags |= screen::kFrameHalfTraced;
  }
  for (uint32_t i = 0; i < screen::kDepthLevels; ++i) {
    in.depth[i] = SlotOf(adopted.depth[i]);
  }
  in.depth_full = SlotOf(adopted.depth_full);
  in.normal = SlotOf(adopted.normal);
  for (uint32_t i = 0; i < screen::kColourPreLevels; ++i) {
    in.colour_pre[i] = SlotOf(adopted.colour_pre[i]);
  }
  in.owner_generation = adopted.owner_generation;
  in.camera = adopted.camera;
  if ((plan_.make & screen::ProductBit(screen::kHizMin)) != 0) {
    in.hiz_levels = RecordHiz(list, *depth0, in.hiz);
  }
  in.hiz_generation = hiz_generation_;
  screen::ScreenFrame frame = screen::MakeFrame(in, 0);
  frame.generation = generation_.Of(screen::SignatureOf(in, frame));
  frame_ = frame;
  published_ = true;
  ++published_frames_;
  if (!first_publish_logged_) {
    first_publish_logged_ = true;
    REXLOG_INFO("{} the first published frame: {} at {}x{} traced, {}x{} picture, {} host samples", kPrefix, ProductNames(frame_.valid), frame_.traced[0], frame_.traced[1],
                frame_.render[0], frame_.render[1], frame_.host_samples);
  }
}

const HostImage* ScreenShared::Image(screen::Product product, uint32_t level) const {
  if (!published_ || !frame_.Has(product)) {
    return nullptr;
  }
  switch (product) {
    case screen::kLinDepth: return level < screen::kDepthLevels ? adopted_.depth[level] : nullptr;
    case screen::kLinDepthFull: return adopted_.depth_full;
    case screen::kHizMin: return level < frame_.hiz_levels && hiz_[level].texture ? &hiz_[level] : nullptr;
    case screen::kNormal: return adopted_.normal;
    case screen::kColourPre: return level < screen::kColourPreLevels ? adopted_.colour_pre[level] : nullptr;
    case screen::kColourPost:
    case screen::kHistory:
    case screen::kProductCount: break;
  }
  return nullptr;
}

uint64_t ScreenShared::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage& image : hiz_) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// the debug view
// ---------------------------------------------------------------------------

plume::RenderPipeline* ScreenShared::ViewPipelineFor(plume::RenderFormat format, uint32_t samples) {
  for (ViewPipeline& v : view_pipelines_) {
    if (v.format == format && v.samples == samples) {
      return v.pipeline.get();
    }
  }
  if (view_failed_ || !view_shader_) {
    return nullptr;
  }
  ViewPipeline v;
  v.format = format;
  v.samples = samples;
  // Into the main view's colour with the target's sample count, the colour replaced (the pass writes one colour for every sample of a pixel).
  v.pipeline = passes_->CreatePipeline(passes_->layout(), *view_shader_, format, plume::RenderBlendDesc::Copy(), samples, "screen view");
  if (!v.pipeline) {
    view_failed_ = true;
    REXLOG_ERROR("{} the debug view's pipeline could not be made: no debug view this session", kPrefix);
    return nullptr;
  }
  plume::RenderPipeline* raw = v.pipeline.get();
  view_pipelines_.push_back(std::move(v));
  return raw;
}

bool ScreenShared::DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  if (plan_.view == screen::View::kOff || !list || !summary.source || !published_) {
    return false;
  }
  const uint32_t needs = screen::ViewNeeds(plan_.view);
  if ((frame_.valid & needs) != needs) {
    return false;  // the product is not in this frame (no normal image, the Hi-Z's pipeline still being made): the picture stays the game's
  }
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply, gbuffer.cpp DrawView).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != summary.source->host_samples || found->scale != summary.source->scale) {
    return false;
  }
  // CHECK (not run): the view's pipeline for each colour format and sample count the main view has (made on the render thread at the first use: a debug view), D3D12 and Vulkan.
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
  push.slots[0] = frame_.lin_depth_slot[0];
  push.slots[1] = screen::kNoSlot;
  push.slots[2] = frame_.normal_slot;
  push.slots[3] = uint32_t(plan_.view);
  push.sizes[0] = frame_.render[0];
  push.sizes[1] = frame_.render[1];
  push.sizes[2] = frame_.traced[0];
  push.sizes[3] = frame_.traced[1];
  if (frame_.Has(screen::kHizMin)) {
    const uint32_t level = std::min(plan_.view_level, frame_.hiz_levels - 1);
    push.slots[1] = frame_.hiz_min_slot[level];
    push.level[0] = level;
    push.level[1] = screen::HizLevelSize(frame_.traced[0], level);
    push.level[2] = screen::HizLevelSize(frame_.traced[1], level);
  }
  pass_timing_->Mark(list, "screen view");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(frame_.render[0]), float(frame_.render[1])};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(frame_.render[0]), int32_t(frame_.render[1]));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

}  // namespace mcla::gpu

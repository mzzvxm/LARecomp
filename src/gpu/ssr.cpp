// MCLA GPU renderer -- screen-space reflections. See ssr.h.

#include "ssr.h"

#include "cpu_timing.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "ao.h"
#include "ao_gbuffer.h"
#include "denoiser_setting.h"
#include "descriptors.h"
#include "device.h"
#include "dlss_rr_rules.h"
#include "gbuffer.h"
#include "gpu_class.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "ray_regen.h"
#include "ray_regen_plan.h"
#include "records.h"
#include "screen_shared.h"
#include "ssr_rt.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[ssr]";
constexpr plume::RenderFormat kFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
// CITY-RT R4: the confidence target (r the class group, g the picture's share: ssr_rt_rules.h) and the trace image's flags in a frame the fallback follows (its compute passes read and write it).
constexpr plume::RenderFormat kConfidenceFormat = plume::RenderFormat::R8G8_UNORM;
constexpr plume::RenderTextureFlags kTraceRtFlags = plume::RenderTextureFlag::RENDER_TARGET | plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
// RR-4a: the hit distance image of a raw frame (the guide pass's input: one float, the distance, in x).
constexpr plume::RenderFormat kHitFormat = plume::RenderFormat::R16_FLOAT;
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) | uint8_t(plume::RenderColorWriteEnable::BLUE);
// The frames the images are kept after the last frame that ran the effect (a pause spell, a menu: not made again at once).
constexpr uint32_t kReleaseAfterFrames = 120;
// VS c43: gReflectionParams, whose x is the road's wetness (0.095 dry, 1.17 in rain; ao.cpp).
constexpr uint32_t kVsReflectionParams = 43;

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}
double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

float ClampSetting(double value, double lo, double hi, float fallback) {
  if (!std::isfinite(value)) return fallback;
  return float(std::clamp(value, lo, hi));
}

// mcla_gpu_ssr (the row REFLECTIONS ON SURFACES: auto, off, high, ultra), read through LiveText (the pause menu's thread writes the cvar's string while this one reads it).
LiveText& Setting() {
  static LiveText text("mcla_gpu_ssr");
  return text;
}

}  // namespace

ScreenReflections::~ScreenReflections() = default;

void ScreenReflections::Initialize(Device& device, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing, FrameRing* frames) {
  device_ = &device;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  listed_.fill(false);
  failed_.fill(false);
  ready_ = false;
  if (frames) {
    regen_pass_.Initialize(device, *frames, heap, passes, pass_timing);   // RG-3: Direct3D 12 only (nothing is made on another backend)
  }
  // The host pass layout (host_passes.h) with a push block of 128 bytes instead of its 64, as the occlusion's: the four texture sets, the sampler set, the push range at (b0, space4), for both
  // stages. CHECK: the same signature as ao.cpp's, which the same devices take (Vulkan guarantees 128 bytes of push constants; D3D12 gets 32 root constants).
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap.texture_set_builder());
  }
  builder.addDescriptorSet(heap.sampler_set_builder());
  builder.addPushConstant(0, 4, sizeof(ssr::Push), plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "ssr layout");
  bool ok = layout_ != nullptr;
  for (uint32_t i = 0; i < kShaderCount && ok; ++i) {
    const HostShaderBlob blob = ScreenReflectionsPixelShader(i);
    shaders_[i] = passes.CreateShader(blob.data, blob.size, blob.name);
    ok = shaders_[i] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("{} the layout or a shader could not be made: no screen-space reflections this session", kPrefix);
    for (auto& shader : shaders_) {
      shader.reset();
    }
    layout_.reset();
    return;
  }
  ready_ = true;
}

void ScreenReflections::InitializeRt(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, CarRtScene* car_scene, RtSceneService* service, RtWorldFrame* world) {
  rt_.Initialize(device, frames, heap, passes, pass_timing, car_scene, service, world);
  // CITY-RT R6: the shared denoiser's host (rt_denoise.h) with this effect's spatial compile, only where the fallback can run (a card that does not trace makes nothing of it).
  if (rt_.available()) {
    const HostShaderBlob spatial = SsrDenoiseSpatialShader();
    denoise_.Initialize(device, frames, heap, passes, /*instance=*/0, "ssr", RtDenoise::Blob{spatial.data, spatial.size, spatial.name}, RtDenoise::Blob{});
  }
}

void ScreenReflections::Shutdown() {
  // The host builder's functions name this object's layout and shaders: stop it first (the other owners do the same; stopping twice is safe).
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (AmbientOcclusion::Shutdown).
  for (auto& pipeline : pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : shaders_) {
    shader.reset();
  }
  layout_.reset();
  trace_ = HostImage{};
  confidence_ = HostImage{};
  rt_framebuffer_.reset();
  rt_framebuffer_for_[0] = rt_framebuffer_for_[1] = nullptr;
  rt_.Shutdown();   // its pipelines are the host builder's too (stopped above)
  denoise_.Shutdown();
  for (HostImage& image : accum_) {
    image = HostImage{};
  }
  hit_ = HostImage{};
  hit_ready_ = false;
  regen_pass_.Shutdown();
  regen_key_ = ssr::HistoryKey{};
  last_key_ = ssr::HistoryKey{};
  ready_ = false;
  level_ = ssr::Level::kOff;
  screen_ = nullptr;
  gbuffer_ = nullptr;
  ao_ = nullptr;
  passes_ = nullptr;
}

const char* ScreenReflections::VariantName(uint32_t variant) const {
  switch (variant) {
    case kVTrace: return "ssr trace";
    case kVTraceMs: return "ssr trace (multisampled planes)";
    case kVTemporal: return "ssr temporal";
    case kVTraceRt: return "ssr trace (confidence target)";
    case kVTraceRtMs: return "ssr trace (confidence target, multisampled planes)";
    case kVApplyRaw: return "ssr apply raw (Ray Reconstruction)";
    case kVHit: return "ssr hit distance (Ray Reconstruction)";
    default: break;
  }
  static constexpr const char* kApply[8] = {"ssr apply 1x",      "ssr view 1x",      "ssr apply 2x",      "ssr view 2x",
                                           "ssr apply 4x",      "ssr view 4x",      "ssr apply 8x",      "ssr view 8x"};
  const uint32_t index = variant - kVApplyBase;
  return index < 8 ? kApply[index] : "ssr ?";
}

std::unique_ptr<plume::RenderPipeline> ScreenReflections::MakePipeline(uint32_t variant) {
  if (!layout_) {
    return nullptr;
  }
  const plume::RenderBlendDesc copy = plume::RenderBlendDesc::Copy();
  const auto make = [&](Shader shader, const plume::RenderBlendDesc& blend, uint32_t samples, plume::RenderFormat format = kFormat) -> std::unique_ptr<plume::RenderPipeline> {
    if (!shaders_[shader]) {
      return nullptr;
    }
    return passes_->CreatePipeline(*layout_, *shaders_[shader], format, blend, samples, VariantName(variant));
  };
  // The apply pass's blend: into the main colour (RGBA16F with the target's sample count), alpha untouched. The term is ADDED (the shader writes it); a debug view replaces the rgb.
  const auto apply_blend = [&](bool view) {
    plume::RenderBlendDesc blend = copy;
    if (!view) {
      blend.blendEnabled = true;
      blend.srcBlend = plume::RenderBlend::ONE;
      blend.dstBlend = plume::RenderBlend::ONE;
      blend.blendOp = plume::RenderBlendOperation::ADD;
      blend.srcBlendAlpha = plume::RenderBlend::ZERO;
      blend.dstBlendAlpha = plume::RenderBlend::ONE;
      blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
    }
    blend.renderTargetWriteMask = kWriteRgb;
    return blend;
  };
  switch (variant) {
    case kVTrace: return make(kShaderTrace, copy, 1);
    case kVTraceMs: return make(kShaderTraceMs, copy, 1);
    case kVTemporal: return make(kShaderTemporal, copy, 1);
    case kVTraceRt:
    case kVTraceRtMs: {
      const Shader shader = variant == kVTraceRt ? kShaderTraceRt : kShaderTraceRtMs;
      if (!shaders_[shader]) {
        return nullptr;
      }
      const plume::RenderFormat formats[2] = {kFormat, kConfidenceFormat};
      return passes_->CreatePipeline(*layout_, *shaders_[shader], formats, 2, VariantName(variant));
    }
    case kVApplyRaw: return make(kShaderApplyRaw, apply_blend(false), 1);   // RR-4a: the same resolve, adding, over the single sample colour
    case kVHit: return make(kShaderHit, copy, 1, kHitFormat);               // RR-4a: the hit distance image (R16_FLOAT, all channels written)
    default: break;
  }
  const uint32_t index = variant - kVApplyBase;
  const uint32_t samples = 1u << (index >> 1);
  const bool view = (index & 1u) != 0;
  return make(samples > 1 ? kShaderApplyMs : kShaderApply, apply_blend(view), samples);
}

plume::RenderPipeline* ScreenReflections::Pipeline(uint32_t variant) {
  if (pipelines_[variant]) {
    return pipelines_[variant].get();
  }
  if (failed_[variant] || !ready_ || !passes_) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  const uint64_t key = HostPrecache::Key(HostPrecache::kScreenReflections, variant);
  if (!listed_[variant]) {
    listed_[variant] = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({key, VariantName(variant), [this, variant] { return MakePipeline(variant); }});
    precache.AddLive("screen reflections", std::move(items));
    REXLOG_INFO("{} the pipeline '{}' is wanted: it is made in the background, and the effect runs from the frame it is done", kPrefix, VariantName(variant));
  }
  if (!precache.Ready(key)) {
    return nullptr;  // a frame never waits for a compile
  }
  pipelines_[variant] = precache.Acquire(key, VariantName(variant), [this, variant] { return MakePipeline(variant); });
  if (!pipelines_[variant]) {
    failed_[variant] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no screen-space reflections in that configuration this session", kPrefix, VariantName(variant));
  }
  return pipelines_[variant].get();
}

void ScreenReflections::Bind(plume::RenderCommandList* list) const {
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
}

void ScreenReflections::DrawPass(plume::RenderCommandList* list, HostImage& image, const plume::RenderPipeline* pipeline, const ssr::Push& push) {
  // The same sequence as HostPasses::DrawInto with this layout: the image to a colour target, cleared on its first use (a new render target is initialised before it is drawn into, D3D12 debug
  // layer id 1422), one triangle over all of it, back to a shader read.
  const bool first_use = image.layout == plume::RenderTextureLayout::UNKNOWN;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  if (first_use) {
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(image.width), float(image.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(image.width), int32_t(image.height));
  list->setScissors(&scissor, 1);
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

void ScreenReflections::DrawPassRt(plume::RenderCommandList* list, const plume::RenderPipeline* pipeline, const ssr::Push& push) {
  // The same sequence as DrawPass over two targets: both to colour targets (cleared on their first use: D3D12 debug layer id 1422), one triangle over all of them, both back to a shader read for graphics AND
  // compute (the fallback's compute passes read the confidence image and read and write the trace image).
  const bool first_use = trace_.layout == plume::RenderTextureLayout::UNKNOWN || confidence_.layout == plume::RenderTextureLayout::UNKNOWN;
  const plume::RenderTextureBarrier to_write[2] = {{trace_.texture.get(), plume::RenderTextureLayout::COLOR_WRITE}, {confidence_.texture.get(), plume::RenderTextureLayout::COLOR_WRITE}};
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_write, 2);
  trace_.layout = confidence_.layout = plume::RenderTextureLayout::COLOR_WRITE;
  list->setFramebuffer(rt_framebuffer_.get());
  if (first_use) {
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    list->clearColor(1, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(trace_.width), float(trace_.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(trace_.width), int32_t(trace_.height));
  list->setScissors(&scissor, 1);
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  const plume::RenderTextureBarrier to_read[2] = {{trace_.texture.get(), plume::RenderTextureLayout::SHADER_READ}, {confidence_.texture.get(), plume::RenderTextureLayout::SHADER_READ}};
  list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, to_read, 2);
  trace_.layout = confidence_.layout = plume::RenderTextureLayout::SHADER_READ;
}

bool ScreenReflections::EnsureImages(uint32_t width, uint32_t height, bool rt, bool raw, uint32_t render_width, uint32_t render_height) {
  const plume::RenderTexture* before[4] = {trace_.texture.get(), accum_[0].texture.get(), accum_[1].texture.get(), confidence_.texture.get()};
  // A frame the ray traced fallback follows: the trace image is also a storage image (its compute passes read and write it), and the confidence image exists. Otherwise the images are what they were.
  // A raw frame (RR-4a) has no accumulation: the trace and the hit distance image at the picture's size are all it needs (the accumulation's two go back after a spell of raw frames: Record's tail).
  bool ok = passes_->EnsureImage(trace_, width, height, kFormat, "ssr trace", rt ? kTraceRtFlags : plume::RenderTextureFlag::RENDER_TARGET);
  if (raw) {
    ok = ok && passes_->EnsureImage(hit_, render_width, render_height, kHitFormat, "ssr hit distance");
  } else {
    ok = ok && passes_->EnsureImage(accum_[0], width, height, kFormat, "ssr accumulated 0") && passes_->EnsureImage(accum_[1], width, height, kFormat, "ssr accumulated 1");
  }
  if (rt) {
    ok = ok && passes_->EnsureImage(confidence_, width, height, kConfidenceFormat, "ssr confidence");
    if (ok && (!rt_framebuffer_ || rt_framebuffer_for_[0] != trace_.texture.get() || rt_framebuffer_for_[1] != confidence_.texture.get())) {
      passes_->RetireFramebuffer(std::move(rt_framebuffer_));
      const plume::RenderTexture* attachments[2] = {trace_.texture.get(), confidence_.texture.get()};
      rt_framebuffer_ = device_->CreateFramebuffer(plume::RenderFramebufferDesc(attachments, 2), "ssr trace framebuffer (two targets)");
      rt_framebuffer_for_[0] = trace_.texture.get();
      rt_framebuffer_for_[1] = confidence_.texture.get();
      ok = rt_framebuffer_ != nullptr;
    }
  } else if (confidence_.texture || rt_framebuffer_) {
    passes_->RetireFramebuffer(std::move(rt_framebuffer_));
    rt_framebuffer_for_[0] = rt_framebuffer_for_[1] = nullptr;
    passes_->ReleaseImage(confidence_);
  }
  if (trace_.texture.get() != before[0] || accum_[0].texture.get() != before[1] || accum_[1].texture.get() != before[2] || confidence_.texture.get() != before[3]) {
    ++images_generation_;
    last_key_.valid = false;  // a new image has no history
  }
  if (trace_.texture.get() != before[0]) {
    rt_.NoteTraceChanged();  // the fallback's output set names the trace image: a new image is a new set, whatever address it was made at
  }
  size_[0] = width;
  size_[1] = height;
  return ok;
}

void ScreenReflections::ReleaseImages() {
  if (!passes_) {
    return;
  }
  hit_ready_ = false;
  for (HostImage* image : {&trace_, &accum_[0], &accum_[1], &confidence_, &hit_}) {
    if (image->texture) {
      passes_->ReleaseImage(*image);
    }
  }
  passes_->RetireFramebuffer(std::move(rt_framebuffer_));
  rt_framebuffer_for_[0] = rt_framebuffer_for_[1] = nullptr;
  ++images_generation_;
  last_key_.valid = false;
  rt_.NoteTraceChanged();  // the trace image is gone: the fallback's set names it
}

uint64_t ScreenReflections::HeldBytes() const {
  uint64_t bytes = regen_pass_.HeldBytes();   // RG-3: the signal, the guides and the denoised image while a regen frame ran lately
  for (const HostImage* image : {&trace_, &accum_[0], &accum_[1], &confidence_, &hit_}) {
    if (image->texture) {
      bytes += TextureBytes(image->width, image->height, 1, image->format);
    }
  }
  return bytes + rt_.HeldBytes() + denoise_.HeldBytes();
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void ScreenReflections::BeginFrame() {
  // Once for every list the Scene opens: a second list of a guest frame whose trigger already ran must not forget that it did (`ran_` is what OnPresent keeps the history for), nor count the frame twice
  // (guest_frame.h, review F2). A list that opens before the trigger (the empty list of an idle present) starts the frame afresh as it always did.
  if (guest_.Begin()) {
    trigger_done_ = false;
    ran_ = false;
    hit_ready_ = false;  // RR-4a: the raw frame's hit distance is this guest frame's (a second list of the same frame keeps it for the guide pass)
    regen_frame_ = false;  // RG-3: this frame's Ray Regeneration dispatch was not recorded yet
    wet_votes_.Reset();
    ++frames_;
  } else if (guest_frame::LogDue(guest_.repeats())) {
    REXLOG_INFO("{} BeginFrame ran twice in one guest frame ({} times so far: a second list was opened after the trigger had run; the frame's state is kept)", kPrefix, guest_.repeats());
  }
  // `auto` is the GPU class's cell (gpu_class.h kSsr: off on every class until the owner has seen the effect); a word that is no level (a hand-edited file) is off.
  ssr::Level level = ssr::LevelFromText(gpu_class::Resolve(gpu_class::Effect::kSsr, Setting().Get()));
  if (!ready_ || !screen_ || !gbuffer_) {
    level = ssr::Level::kOff;  // the layout or a shader could not be made, or the owners are not there
  }
  level_ = level;
  // RR-INT: the DENOISER row's board (denoiser_setting.h): this effect is on this frame. It is a consumer of Ray Reconstruction (rr::kConsumers: its raw mode exists, RR-4a), and the plan reads the
  // board at the temporal system's frame boundary, which comes right after this call (frame_order.h kBeginSsr, kBeginTemporal: lag 0). Only a flag is set; with the plan not RR nothing else changes.
  denoiser::NoteConsumer(rr::Consumer::kSsr, level_ != ssr::Level::kOff);
  rt_.BeginFrame(level_);   // CITY-RT R4: the ray traced fallback's level (off with the effect, on a card that does not trace, or by the debug word) and its want of the city's structures
  // CITY-RT R6: the fallback is a consumer of the DENOISER row too (rr::kConsumers kCityRtReflections: its one ray a pixel is what Ray Reconstruction is for). `wanted` is the fallback's own answer: the effect is on, the
  // card traces and the setting does not say off; the plan reads the board at the temporal system's frame boundary, as for the effect itself.
  denoiser::NoteConsumer(rr::Consumer::kCityRtReflections, rt_.wanted());
  if (!logged_level_once_ || level != logged_level_) {
    logged_level_once_ = true;
    logged_level_ = level;
    REXLOG_INFO("{} screen-space reflections {} (mcla_gpu_ssr {}, live)", kPrefix, ssr::LevelName(level), Setting().Get());
  }
  view_ = ssr::ViewFromSetting(larecomp::settings::mcla_gpu_ssr_view.Get());
  strength_ = ClampSetting(larecomp::settings::mcla_gpu_ssr_strength.Get(), 0.0, 4.0, ssr::kDefaultStrength);
  const double min_smooth = larecomp::settings::mcla_gpu_ssr_min_smooth.Get();
  min_smooth_ = min_smooth >= 0.0 ? ClampSetting(min_smooth, 0.0, 1.0, ssr::kDefaultMinSmooth) : ssr::kDefaultMinSmooth;
  force_stub_ = larecomp::settings::mcla_gpu_ssr_stub.Get() != 0;
  const double wet = larecomp::settings::mcla_gpu_ssr_wet.Get();
  wet_override_ = wet >= 0.0 ? ClampSetting(wet, 0.0, 1.0, 0.0f) : -1.0f;
  const double history = larecomp::settings::mcla_gpu_ssr_history.Get();
  history_override_ = history >= 0.0 ? ClampSetting(history, 0.0, 0.98, 0.0f) : -1.0f;
  fallback_setting_ = ClampSetting(larecomp::settings::mcla_gpu_ssr_fallback.Get(), 0.0, 1.0, 1.0f);
  vehicle_rows_ = larecomp::settings::mcla_gpu_ssr_vehicle_rows.Get() != 0;
  // What the effect reads of the shared products and of the G-buffer, asked ONLY WHILE THE ROW IS ON (ssr_rules.h WantedProducts / WantedPlanes / WantsCamera: nothing at off, the same at high and
  // ultra): the Hi-Z, colour_post (its Want also makes the mip chain), the camera, and all three planes (RT3 is written now, GB-W2: the class table decides; the stub only stands in for a frame
  // without RT3, or when mcla_gpu_ssr_stub forces it). The wants are sticky and the owners read them at their frame start, which comes AFTER this call (Scene::BeginFrame declares the effect before
  // the G-buffer and the screen owner open their frames: frame_order.h kBeginSsr): the planes, the Hi-Z and the capture are there in the very frame the setting changes, and gone in the very frame
  // it is switched off. CHECK (run, not tried): REFLECTIONS ON SURFACES switched on and off live in the play view: the first frame with it on (no history, the images made) and the first
  // frame with it off (the owners stop at once) show no stall, no flash and no log line other than the level's.
  if (screen_) {
    screen_->Want(screen::Consumer::kReflections, ssr::WantedProducts(level_));
    screen_->WantCamera(screen::Consumer::kReflections, ssr::WantsCamera(level_));
  }
  if (gbuffer_) {
    gbuffer_->Want(gbuffer::Consumer::kReflections, ssr::WantedPlanes(level_));
  }
}

void ScreenReflections::OnMainDraw(const temporal::MainMatch& match, const ConstantBanks::Banks& banks) {
  if (level_ == ssr::Level::kOff || trigger_done_ || !match || !match.color) {
    return;
  }
  wet_votes_.Add(BankFloat(banks.vs.data(), kVsReflectionParams, 0), BankFloat(banks.vs.data(), kVsReflectionParams, 1));
}

bool ScreenReflections::OnTrigger(const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool raw_requested, bool upscaler_running, const RegenAsk& ask) {
  const cpu_timing::Scope cpu_scope(cpu_timing::Section::kReflections);
  if (level_ == ssr::Level::kOff || trigger_done_ || !list || !summary.source || !screen_ || !gbuffer_ || !passes_) {
    return false;
  }
  trigger_done_ = true;  // once a frame, whatever comes of it
  guest_.Worked();       // a later list of this guest frame keeps this (BeginFrame)
  const screen::ScreenFrame& sf = screen_->Frame();
  const gbuffer::GBufferFrame& gf = gbuffer_->frame();
  EdramTargets::Target& depth = *summary.source;
  const ssr::LevelNumbers numbers = ssr::NumbersOf(level_);
  const bool ms = depth.host_samples > 1;
  const bool debug_view = view_ != ssr::View::kOff;

  ssr::FrameFacts facts;
  facts.level = level_;
  facts.pause_view = (sf.flags & screen::kFramePause) != 0;
  facts.has_lens = (sf.camera.flags & screen::kCameraValid) != 0 && sf.camera.tan_half_fov[0] > 0.0f && sf.camera.tan_half_fov[1] > 0.0f && sf.camera.lin[0] != 0.0f;
  facts.has_hiz = sf.Has(screen::kHizMin);
  facts.hiz_levels = sf.hiz_levels;
  facts.has_colour = sf.Has(screen::kColourPost);
  facts.colour_levels = sf.colour_post_levels;
  const bool gbuffer_current = gf.frame_serial == sf.frame_serial;
  facts.has_rt1 = gbuffer_current && gf.Has(gbuffer::kPlaneNormal);
  facts.has_rt2 = gbuffer_current && gf.Has(gbuffer::kPlaneAlbedo);
  facts.has_rt3 = gbuffer_current && gf.Has(gbuffer::kPlaneMaterial) && !force_stub_;
  facts.render[0] = sf.render[0];
  facts.render[1] = sf.render[1];
  facts.traced[0] = sf.traced[0];
  facts.traced[1] = sf.traced[1];
  facts.plane_size[0] = gf.width;
  facts.plane_size[1] = gf.height;
  facts.depth_samples = depth.host_samples;
  facts.plane_samples = gf.host_samples;
  facts.depth_sample0 = summary.host_sample;
  facts.plane_sample0 = gf.sample0;
  // The pipelines (listed with the host precache the first time they are asked for; a frame never waits for a compile).
  plume::RenderPipeline* trace_pipeline = Pipeline(ms ? kVTraceMs : kVTrace);
  plume::RenderPipeline* temporal_pipeline = Pipeline(kVTemporal);
  plume::RenderPipeline* apply_pipeline = Pipeline(ApplyVariant(depth.host_samples, debug_view));
  facts.shaders_ready = trace_pipeline && temporal_pipeline && apply_pipeline;
  uint32_t depth_slot = 0;
  uint32_t stencil_slot = 0;
  facts.depth_slot_ok = facts.shaders_ready && edram_->SampleSlots(depth, &depth_slot, &stencil_slot);
  const ssr::Decision decision = ssr::Decide(facts);
  if (uint32_t(decision.why) != why_logged_) {
    why_logged_ = uint32_t(decision.why);
    if (decision.why == ssr::Why::kUsed) {
      REXLOG_INFO("{} runs: {}", kPrefix, decision.stub ? "RT3 (the material plane) is not in the frame: the STUB class stands in (every pixel whose world normal points up and is no vehicle is a road)" : "the G-buffer's material plane gives the class and the smoothness");
    } else {
      REXLOG_INFO("{} does not run: {} (logged when it changes)", kPrefix, ssr::WhyName(decision.why));
    }
  }
  if (decision.why != ssr::Why::kUsed) {
    return false;
  }
  const uint32_t width = decision.size.size[0];
  const uint32_t height = decision.size.size[1];
  // RR-4a, the raw mode (ssr.h): asked for by the caller while DLSS Ray Reconstruction takes the frame; the frame decides whether it can (a debug view, the stub, a multisampled colour and the pipelines
  // still being made keep it plain). The raw pipelines are listed with the host precache only for a frame that is asked and could take them: with the plan not RR none is ever made.
  ssr::RawFacts raw_facts;
  raw_facts.asked = raw_requested;
  raw_facts.debug_view = debug_view;
  raw_facts.stub = decision.stub;
  raw_facts.host_samples = depth.host_samples;
  plume::RenderPipeline* raw_apply_pipeline = nullptr;
  plume::RenderPipeline* hit_pipeline = nullptr;
  if (ssr::RawPossible(raw_facts)) {
    raw_apply_pipeline = Pipeline(kVApplyRaw);
    hit_pipeline = Pipeline(kVHit);
    raw_facts.pipelines_ready = raw_apply_pipeline && hit_pipeline;
  }
  ssr::RawWhy raw_why = ssr::DecideRaw(raw_facts);
  bool raw = raw_why == ssr::RawWhy::kRaw;
  // CITY-RT R4: the frame follows the ray traced fallback where it is wanted, the frame has the material plane (the stub class has no use for rays) and its pipelines are made (the compute passes and the
  // trace with two targets; asked for, so listed with the precache, only while the fallback is wanted). Otherwise nothing of it exists in the frame. A RAW frame follows it too (RR-6: the fallback's ray is
  // the one Ray Reconstruction takes, its radiance in the trace image and its hit distance in the alpha, which the hit pass turns into the guide): RtDenoise does not run in a raw frame, RR is the denoiser.
  plume::RenderPipeline* trace_rt_pipeline = nullptr;
  bool rt = false;
  if (rt_.wanted() && !decision.stub) {
    trace_rt_pipeline = Pipeline(ms ? kVTraceRtMs : kVTraceRt);
    rt = trace_rt_pipeline != nullptr && rt_.PipelinesReady(ms);
  }
  // The images, in the order of what a failure costs least: the fallback goes first (the effect runs as it was without it, raw or plain), then the raw mode (the plain effect).
  bool images_ok = EnsureImages(width, height, rt, raw, sf.render[0], sf.render[1]);
  if (!images_ok && rt) {
    rt = false;     // the confidence image or its framebuffer could not be made
    images_ok = EnsureImages(width, height, false, raw, sf.render[0], sf.render[1]);
  }
  if (!images_ok && raw) {
    raw = false;
    raw_why = ssr::RawWhy::kImages;   // the hit distance image could not be made (logged by the host passes): this frame is plain
    images_ok = EnsureImages(width, height, false, false);
  }
  if (raw_requested) {
    raw_ever_asked_ = true;
  }
  if (raw_ever_asked_ && uint32_t(raw_why) != raw_why_logged_) {
    raw_why_logged_ = uint32_t(raw_why);
    REXLOG_INFO("{} raw mode (DLSS Ray Reconstruction takes the frame): {} (logged when it changes)", kPrefix, ssr::RawWhyName(raw_why));
  }
  // RG-3, AMD FSR Ray Regeneration (ssr.h): asked for by the caller while the DENOISER row's AMD value takes the frame; one vendor, so never together with the raw mode. The context's size is the trace's.
  rayregen::NoteTraced(rayregen::Site::kReflections, width, height);
  rgd::RegenFacts regen_facts;
  regen_facts.asked = rayregen::ReflectionSignalWanted(ask.asked, ask.city, rt) && !raw;
  regen_facts.debug_view = debug_view;
  regen_facts.stub = decision.stub;
  regen_facts.host_samples = depth.host_samples;
  plume::RenderPipeline* regen_apply_pipeline = nullptr;   // the raw mode's resolve: it reads the image it is given as it stands (hlsl/ssr_apply_ps.hlsl SSR_RAW)
  if (rgd::RegenPossible(regen_facts)) {
    regen_apply_pipeline = Pipeline(kVApplyRaw);
    regen_facts.pipelines_ready = regen_pass_.Available() && regen_pass_.PipelineReady() && regen_apply_pipeline != nullptr;
  }
  const rgd::RegenWhy regen_why = rgd::DecideRegen(regen_facts);
  const bool regen = regen_why == rgd::RegenWhy::kRegen;
  if (regen_facts.asked) {
    regen_ever_asked_ = true;
  }
  if (regen_ever_asked_ && uint32_t(regen_why) != regen_why_logged_) {
    regen_why_logged_ = uint32_t(regen_why);
    REXLOG_INFO("{} AMD FSR Ray Regeneration: {} (logged when it changes)", kPrefix, rgd::RegenWhyName(regen_why));
  }
  if (!images_ok) {
    return false;  // the images could not be made (logged by the host passes): asked again every frame
  }
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != depth.host_samples || found->scale != depth.scale) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }

  // The camera: the occlusion's (the frame's own axes, the motion into the last frame), else the last frame's anchor camera for the axes and no history.
  AmbientOcclusion::FrameCameraOut cam;
  const bool have_camera = ao_ && ao_->FrameCameraNow(&cam);
  ssr::CameraAxes axes;
  bool rows_from_ao = false;
  if (have_camera && cam.have_rows) {
    axes = ssr::AxesFromRows(cam.right, cam.up, cam.forward);
    rows_from_ao = axes.ok;
  }
  if (!axes.ok && (sf.camera.flags & screen::kCameraValid) != 0) {
    double m[4][4];
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        m[r][c] = double(sf.camera.view_inverse[r * 4 + c]);
      }
    }
    ao_gbuffer::Rows rows;
    if (ao_gbuffer::RowsFromViewInverse(m, &rows)) {
      axes = ssr::AxesFromRows(rows.x, rows.y, rows.z);
    }
  }
  // The motion is the occlusion's (a vertex constants' camera, or the last step repeated). Its identity fallback (nothing known) is right for a camera that does not move and wrong for one that
  // does, and the frames' anchor cameras can tell which: a still motion is taken only when the last two anchors are the same camera.
  const bool camera_still = (sf.camera.flags & screen::kCameraHasPrevious) != 0 && ssr::SameCamera(sf.camera.view_projection, sf.camera.prev_view_projection);
  const bool motion_known = have_camera && cam.have_motion && (!cam.motion_still || camera_still);
  ssr::HistoryKey now;
  now.valid = true;
  now.frame = screen_->frame_count();
  now.size[0] = width;
  now.size[1] = height;
  now.generation = sf.generation;
  const float history_weight = history_override_ >= 0.0f ? history_override_ : numbers.history_weight;
  const bool use_history = ssr::HistoryUsable(last_key_, now, motion_known) && last_tan_[0] > 0.0f && last_tan_[1] > 0.0f && history_weight > 0.0f;
  if (!camera_logged_) {
    camera_logged_ = true;
    REXLOG_INFO("{} the camera's axes come from {}; the history {}", kPrefix,
                rows_from_ao ? "the occlusion's own (the frame's view inverse where the vertex constants are trusted)" : (axes.ok ? "the last frame's anchor camera (the occlusion's gave none)" : "nowhere: the depth's normals are used"),
                motion_known ? "uses the occlusion's camera motion" : "is off until the occlusion gives a camera motion (it is off, has no previous frame, or the camera cut)");
  }

  // The wetness: the commonest value the frame's draws carried (VS c43.x, the game's own scalar: 0.095 dry, 1.17 in rain), smoothed over about a second of TIME (GB-I2: not of frames), a forced value
  // replacing it; and, with it, the strength the game shows its road view with (c43.y), which says how much of that view the fallback may take out.
  const auto clock_now = std::chrono::steady_clock::now();
  const float dt = wet_clock_valid_ ? float(std::chrono::duration<double>(clock_now - wet_clock_).count()) : 0.0f;
  wet_clock_ = clock_now;
  wet_clock_valid_ = true;
  if (wet_override_ >= 0.0f) {
    wet_ = wet_override_;
    wet_strength_ = 1.0f;   // a forced wetness shows the road as the rain does
    wet_known_ = true;
  } else {
    float c43 = 0.0f;
    float c43y = ssr::WetVotes::kDefaultGameStrength;
    if (wet_votes_.Pick(&c43, &c43y)) {
      const float target = ssr::Wet01(c43);
      wet_ = wet_known_ ? ssr::SmoothWetDt(wet_, target, dt) : target;
      wet_strength_ = wet_known_ ? ssr::SmoothWetDt(wet_strength_, c43y, dt) : c43y;
      wet_known_ = true;
      wet_raw_ = c43;
    }
  }

  // GB-I2, the off-screen fallback: the game's own road reflection view (the whole screen drawn with the roads' mirrored geometry into a 256 x 256 target, side_views.h), which the trace reads at the
  // pixel's screen position to take out of the road where it finds a reflection of its own (hlsl/ssr_trace_ps.hlsl). It is the host target of the view's identity, read as it stands at the trigger: the
  // game drew and resolved it before the roads that read it. Found by the identity (ssr_rules.h roadview::), accepted only when the side view classification made it and this frame drew into it.
  ssr::RoadViewFacts road;
  road.share = ssr::FallbackShare(fallback_setting_, wet_strength_);
  EdramKey road_key;
  road_key.base_tiles = uint16_t(ssr::roadview::kColorBase);
  road_key.pitch = uint16_t(ssr::roadview::kPitch);
  road_key.depth = 0;
  road_key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  road_key.samples = uint8_t(ssr::roadview::kGuestSamples);
  road_key.reserved = 0;
  EdramTargets::Target* road_target = nullptr;
  uint32_t road_slot = screen::kNoSlot;
  if (road.share > 0.0f) {
    const EdramTargets::Target* road_found = edram_->Find(road_key);
    if (road_found && road_found->texture) {
      road.has_target = true;
      road.is_reflection_class = road_found->side_view == SideView::kReflection;
      road.drawn_this_frame = uint32_t(road_found->drawn_frame) == sf.frame_serial;
      road.side = road_found->scale.Edge(ssr::roadview::kDeclared);
      road.host_samples = road_found->host_samples;
      if (road.is_reflection_class && road.drawn_this_frame) {
        bool road_unset = false;
        road_target = edram_->Get(road_key, road_found->height, road_found->scale, 0, list, &road_unset);
        uint32_t road_stencil = screen::kNoSlot;
        road.slot_ok = road_target && edram_->SampleSlots(*road_target, &road_slot, &road_stencil);
      }
    }
  }
  const ssr::RoadViewWhy road_why = ssr::DecideRoadView(road);
  if (uint32_t(road_why) != road_logged_) {
    road_logged_ = uint32_t(road_why);
    REXLOG_INFO("{} the game's road reflection view (the fallback under the effect on roads): {}{} (logged when it changes)", kPrefix, ssr::RoadViewWhyName(road_why),
                road_why == ssr::RoadViewWhy::kUsed ? std::format(": {}x{} host pixels, {} sample{}, {:.2f} of it taken out where a ray finds a reflection (the setting {:.2f} x the game's own strength {:.2f})", road.side, road.side,
                                                                  road.host_samples, road.host_samples == 1 ? "" : "s", road.share, fallback_setting_, wet_strength_)
                                                        : std::string());
  }
  uint32_t road_word = ssr::kNoRoadView;
  if (road_why == ssr::RoadViewWhy::kUsed) {
    edram_->Transition(list, *road_target, plume::RenderTextureLayout::SHADER_READ);   // the view was a colour target when the game drew it; the next frame's draws move it back themselves
    road_word = ssr::PackRoadView(road_slot, road.host_samples, road.side);
  }

  // GB-I2, the vehicles: the AO's motion rows of this frame (its history pass drew them for its own use), for the temporal pass's reflected vehicles. None: they are taken as the world's.
  uint32_t vehicle_table_slot = screen::kNoSlot;
  if (vehicle_rows_ && ao_) {
    AmbientOcclusion::VehicleTableOut table;
    if (ao_->VehicleTableNow(&table)) {
      vehicle_table_slot = table.slot;
    }
  }
  if (vehicle_table_slot != screen::kNoSlot) {
    ++vehicle_table_frames_;
    if (!vehicle_table_logged_) {
      vehicle_table_logged_ = true;
      REXLOG_INFO("{} the AO's vehicle motion table is in the frame: a reflected vehicle's image moves with its own row", kPrefix);
    }
  }

  // ---- the push block of the passes ---------------------------------------------------------------------------------------------------------------------------------------------------
  ssr::Push push;
  namespace pk = ssr::push;
  push.Set(pk::kDepthRt1, ssr::PackSlots(depth_slot, gf.slot[gbuffer::kPlaneNormal]));
  push.Set(pk::kRt2Rt3, ssr::PackSlots(gf.Has(gbuffer::kPlaneAlbedo) ? gf.slot[gbuffer::kPlaneAlbedo] : screen::kNoSlot, facts.has_rt3 ? gf.slot[gbuffer::kPlaneMaterial] : screen::kNoSlot));
  const auto hiz_slot = [&](uint32_t level) { return level < sf.hiz_levels ? sf.hiz_min_slot[level] : screen::kNoSlot; };
  push.Set(pk::kHiz01, ssr::PackSlots(hiz_slot(0), hiz_slot(1)));
  push.Set(pk::kHiz23, ssr::PackSlots(hiz_slot(2), hiz_slot(3)));
  push.Set(pk::kHiz45, ssr::PackSlots(hiz_slot(4), hiz_slot(5)));
  const auto col_slot = [&](uint32_t level) { return level < sf.colour_post_levels ? sf.colour_post_slot[level] : screen::kNoSlot; };
  push.Set(pk::kCol01, ssr::PackSlots(col_slot(0), col_slot(1)));
  push.Set(pk::kCol23, ssr::PackSlots(col_slot(2), col_slot(3)));
  push.Set(pk::kRender, ssr::PackSize(sf.render[0], sf.render[1]));
  ssr::FlagsIn flags;
  flags.sample0 = summary.host_sample;
  flags.hiz_levels = sf.hiz_levels;
  flags.colour_levels = sf.colour_post_levels;
  flags.ms = ms;
  flags.hz1 = decision.size.hz == 1;
  flags.colour_half = ssr::ColourIsHalf(sf.render[1]);
  flags.stub = decision.stub;
  flags.camera = axes.ok;
  flags.rays = numbers.rays;
  flags.max_iterations = numbers.max_iterations;
  flags.view = uint32_t(view_);
  push.Set(pk::kFlags, ssr::PackFlags(flags));
  // The noise's frame number, and bit 31 for the first version's noise (the A/B: mcla_gpu_ssr_blue_noise false; hlsl/ssr_common.hlsli THE NOISE).
  push.Set(pk::kFrame, (uint32_t(frames_) & ~pk::kFrameLegacyNoise) | (larecomp::settings::mcla_gpu_ssr_blue_noise.Get() ? 0u : pk::kFrameLegacyNoise));
  push.Set(pk::kLin0, ssr::PackSlots(sf.lin_depth_slot[0], stencil_slot));   // the low half the depth chain's level 0, the high half (pk::kStencil) the depth target's stencil plane, for the vehicle a hit is on (GB-I2)
  push.SetFloat(pk::kTanX, sf.camera.tan_half_fov[0]);
  push.SetFloat(pk::kTanY, sf.camera.tan_half_fov[1]);
  push.SetFloat(pk::kLinX, sf.camera.lin[0]);
  push.SetFloat(pk::kLinY, sf.camera.lin[1]);
  push.SetFloat(pk::kLinZ, sf.camera.lin[2]);
  push.SetFloat(pk::kLinW, sf.camera.lin[3]);
  push.SetFloat(pk::kNear, sf.camera.near_plane);
  push.SetFloat(pk::kMaxDist, ssr::kDefaultMaxDistance);
  for (uint32_t i = 0; i < 3; ++i) {
    push.SetFloat(pk::kRight + i, axes.right[i]);
    push.SetFloat(pk::kUp + i, axes.up[i]);
  }
  push.SetFloat(pk::kStrength, strength_);
  push.SetFloat(pk::kMinSmooth, min_smooth_);
  push.SetFloat(pk::kStubSmooth, ssr::kDefaultStubSmooth);
  push.SetFloat(pk::kWet, wet_);
  push.SetFloat(pk::kThick, ssr::kDefaultThickness);
  push.SetFloat(pk::kCap, ssr::CapFor(sf.camera.exposure_bias));

  // The depth plane is read as a texture (after the trigger's copy it already is: the call records a barrier only when it is elsewhere); the game's next draws move it back themselves.
  edram_->Transition(list, depth, plume::RenderTextureLayout::SHADER_READ);
  // CHECK (not run): the three passes on both APIs with the Vulkan synchronisation validation on (the Hi-Z and the colour in the shader-read layout the owner left them in, the G-buffer's planes
  // as published, the depth plane, the new images' first use), MSAA 1, 2, 4 and 8, and the cost: the trace is the pass that matters (the walk's iterations, the class gate's early out).
  ssr::Push trace_push = push;
  trace_push.Set(pk::kRoadView, road_word);
  trace_push.SetFloat(pk::kFallback, road_why == ssr::RoadViewWhy::kUsed ? road.share : 0.0f);
  pass_timing_->Mark(list, "ssr trace");
  bool rt_recorded = false;   // the fallback's compute passes were recorded in this frame
  if (rt) {
    DrawPassRt(list, trace_rt_pipeline, trace_push);
    // the ray traced fallback: the pixels the picture could not speak for, traced into the city and added to the trace image (its two compute passes' time is the label 'ssr rt': it runs to the next mark)
    pass_timing_->Mark(list, "ssr rt");
    ScreenReflectionsRt::Frame rt_frame;
    rt_frame.list = list;
    rt_frame.trace = &trace_;
    rt_frame.confidence = &confidence_;
    rt_frame.push = trace_push;
    rt_frame.width = width;
    rt_frame.height = height;
    rt_frame.multisampled = ms;
    rt_frame.ssr_rays = numbers.rays;
    rt_frame.screen = sf.lin_depth_slot[0] != screen::kNoSlot;
    rt_frame.frame = frames_run_;
    rt_recorded = rt_.Record(rt_frame);   // false where the frame has no top level: the trace and the confidence image are the picture's alone
  } else {
    DrawPass(list, trace_, trace_pipeline, trace_push);
  }

  // RG-3: the signal and its guides, the runtime's dispatch. Refused (no camera rows, an image, the plan of the dispatch, the runtime): the effect's own accumulation runs below, this frame.
  bool regen_ok = false;
  // The city fallback has already put its blended radiance and hit distance
  // in trace_. When only the city requested AMD, require that pass to run.
  if (regen && rayregen::ReflectionSignalWanted(ask.asked, ask.city, rt_recorded)) {
    if (regen_city_ != rt_recorded) regen_key_.valid = false;
    RegenCameraRows rows;
    const bool have_rows = have_camera && cam.have_rows && axes.ok;
    if (have_rows) {
      for (int i = 0; i < 3; ++i) {
        rows.right[i] = cam.right[i];
        rows.up[i] = cam.up[i];
        rows.forward[i] = cam.forward[i];
      }
      for (int i = 0; i < 12; ++i) {
        rows.motion[i] = cam.motion[i];
      }
    }
    regen_ok = RecordRegen(list, push, sf, have_rows ? &rows : nullptr, motion_known, ask, width, height, now);
    if (!regen_ok) {
      ++regen_fallbacks_;
    }
  }

  if (!raw && !regen_ok) {
    // The accumulation. A raw frame (RR-4a) has none: Ray Reconstruction is the denoiser, the resolve reads the trace itself. A regen frame (RG-3) has none either, unless its dispatch was refused.
    ssr::Push temporal = push;
    temporal.Set(pk::kSrc, ssr::PackSlots(trace_.slot, accum_[write_ ^ 1u].slot));
    flags.history = use_history;
    temporal.Set(pk::kFlags, ssr::PackFlags(flags));
    temporal.SetFloat(pk::kHistoryWeight, use_history ? history_weight : 0.0f);
    temporal.Set(pk::kVehTable, ssr::PackTemporalWord(vehicle_table_slot, larecomp::settings::mcla_gpu_ssr_resolve.Get(), upscaler_running,
                                                     larecomp::settings::mcla_gpu_ssr_sparkle_resolve.Get()));
    temporal.SetFloat(pk::kPrevTanX, last_tan_[0]);
    temporal.SetFloat(pk::kPrevTanY, last_tan_[1]);
    for (uint32_t i = 0; i < 12; ++i) {
      temporal.SetFloat(pk::kMotion + i, motion_known ? cam.motion[i] : 0.0f);
    }
    pass_timing_->Mark(list, "ssr temporal");
    DrawPass(list, accum_[write_], temporal_pipeline, temporal);
  }

  // CITY-RT R6, RtDenoise's spatial stage (ssr_denoise_rules.h has when): between the accumulation and the resolve, in a frame the fallback followed, a bilateral a-trous over the accumulated image on the
  // pixels the fallback made (their share is 1 - the picture's confidence); the resolve reads its result in place of the accumulation. The accumulation's own image stays the next frame's history (a blur
  // fed back would stack). A raw frame (Ray Reconstruction takes it) skips it: RR is the denoiser of the raw ray.
  const HostImage* resolve_source = &accum_[write_];
  ssr_denoise::Facts denoise_facts;
  denoise_facts.level = level_;
  denoise_facts.setting = int(larecomp::settings::mcla_gpu_rt_denoise.Get());
  denoise_facts.raw = raw;
  if (regen_ok) {
    // RG-3: AMD FSR Ray Regeneration denoised this frame and no accumulation was written: this stage would filter an image no pass made this frame, and the resolve reads the runtime's output. Counted with the raw
    // frames (another denoiser took the frame; ssr_denoise_rules.h Why::kRawFrame).
    denoise_facts.raw = true;
  }
  denoise_facts.fallback_followed = rt_recorded;
  denoise_facts.multisampled = ms;
  denoise_facts.pipelines_ready = true;
  denoise_facts.images_ok = true;
  ssr_denoise::Plan denoise = ssr_denoise::Decide(denoise_facts);
  if (denoise.runs()) {
    denoise_facts.pipelines_ready = denoise_.SpatialReady();   // everything else says go: ask for the pipeline (listed with the host precache the first time)
    denoise = ssr_denoise::Decide(denoise_facts);
  }
  if (denoise.runs()) {
    pass_timing_->Mark(list, "ssr denoise");
    RtDenoise::Frame stage;
    stage.list = list;
    stage.signal = &accum_[write_];
    stage.reads = {&trace_, &confidence_};
    stage.width = width;
    stage.height = height;
    stage.iterations = denoise.iterations;
    ssr::Push pass = push;   // the planes, the Hi-Z, the lens, the camera's axes and the wetness as the apply has them
    pass.Set(ssr_denoise::kWordSrc, ssr::PackSlots(accum_[write_].slot, trace_.slot));
    pass.Set(ssr_denoise::kWordConf, confidence_.slot);
    pass.Set(ssr_denoise::kWordIteration, 0u);
    pass.SetFloat(ssr_denoise::kWordLength, ssr_denoise::HistoryLength(use_history));
    stage.push = pass.w;
    stage.patch = ssr_denoise::PatchFor();
    const HostImage* filtered = denoise_.RecordSpatial(stage);
    if (filtered) {
      resolve_source = filtered;
    } else {
      denoise.why = ssr_denoise::Why::kImages;   // the images or the sets could not be made: the resolve reads the accumulation
    }
  }
  if (rt_.wanted() && denoise_facts.setting != 0) {
    denoise_totals_.wanted_once = true;
    denoise_totals_.last_why = denoise.why;
    denoise_totals_.last_iterations = ssr_denoise::IterationsFor(denoise_facts.level, denoise_facts.setting);
    if (denoise.runs()) {
      ++denoise_totals_.frames_run;
      denoise_totals_.iterations += denoise.iterations;
    } else if (denoise.why == ssr_denoise::Why::kRawFrame) {
      ++denoise_totals_.frames_raw;
    } else if (denoise.why != ssr_denoise::Why::kOff && denoise.why != ssr_denoise::Why::kNoFallback) {
      ++denoise_totals_.frames_other;
    }
    if (uint32_t(denoise.why) != denoise_why_logged_) {
      denoise_why_logged_ = uint32_t(denoise.why);
      REXLOG_INFO("{} denoise (RtDenoise, the fallback's pixels): {}{} (logged when it changes)", kPrefix, ssr_denoise::WhyName(denoise.why),
                  denoise.runs() ? std::format(": {} iteration{} of the bilateral a-trous (steps 1, 2, 4), mcla_gpu_rt_denoise {}", denoise.iterations, denoise.iterations == 1 ? "" : "s", denoise_facts.setting) : std::string());
    }
  }

  // The apply: the main colour as a colour target in the middle of the frame (the occlusion's way: no depth, the next draw of the game moves things back itself).
  ssr::Push apply = push;
  if (raw) {
    // RR-4a: the resolve reads the trace itself (both halves of the source word); the Hi-Z words stay as pushed (the debug view that borrows the first one is never on in a raw frame).
    apply.Set(pk::kSrc, ssr::PackSlots(trace_.slot, trace_.slot));
  } else if (regen_ok) {
    // RG-3: the same resolve over the DENOISED image (rgb the runtime's radiance, alpha the hit distance it preserved: not 0 where a ray was traced, which is all the resolve asks of the alpha).
    const uint32_t denoised_slot = regen_pass_.image(rgd::Image::kDenoised).slot;
    apply.Set(pk::kSrc, ssr::PackSlots(denoised_slot, denoised_slot));
  } else {
    apply.Set(pk::kSrc, ssr::PackSlots(resolve_source->slot, trace_.slot));   // the accumulation, or its filtered image (RtDenoise)
    apply.Set(pk::kRoadViewApply, road_word);
  }
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (framebuffer) {
    if (raw) {
      pass_timing_->Mark(list, "ssr apply raw");
    } else if (regen_ok) {
      pass_timing_->Mark(list, "ssr apply denoised");
    } else {
      pass_timing_->Mark(list, "ssr apply");
    }
    list->setFramebuffer(framebuffer);
    const plume::RenderViewport viewport{0.0f, 0.0f, float(sf.render[0]), float(sf.render[1])};
    list->setViewports(&viewport, 1);
    const plume::RenderRect scissor(0, 0, int32_t(sf.render[0]), int32_t(sf.render[1]));
    list->setScissors(&scissor, 1);
    Bind(list);
    list->setPipeline(raw ? raw_apply_pipeline : (regen_ok ? regen_apply_pipeline : apply_pipeline));
    list->setGraphicsPushConstants(0, &apply, 0, sizeof(apply));
    list->drawInstanced(3, 1, 0, 0);
    list->setFramebuffer(nullptr);
    capture_signal_ = raw ? &trace_ : regen_ok ? &regen_pass_.image(rgd::Image::kDenoised) : resolve_source;
  }  // else the trace and the accumulation were recorded and there is no apply this frame: the next one starts from this history all the same
  if (raw) {
    // RR-4a: how far each of the reflections the resolve just added went, into the image the guide pass reads as the hit distance (hlsl/ssr_hit_ps.hlsl). It reads the trace like the resolve.
    ssr::Push hit_push = push;
    hit_push.Set(pk::kSrc, ssr::PackSlots(trace_.slot, trace_.slot));
    pass_timing_->Mark(list, "ssr hit");
    DrawPass(list, hit_, hit_pipeline, hit_push);
    hit_ready_ = true;
  }
  // The closing mark (PassTiming: a mark's time runs to the next mark): what is recorded next (the car history, the debug views, the game's draws) is not the apply's. No label of the chains' own
  // reflection sum (`ssr ...`): the span after this mark is whatever follows.
  pass_timing_->Mark(list, "after ssr");

  // The frame is the history of the next (a raw frame made none: the frame after it starts again, whatever mode it is in; a regen frame's history is the runtime's, and ours is the lens and the key).
  if (!raw && !regen_ok) {
    last_key_ = now;
    last_tan_[0] = sf.camera.tan_half_fov[0];
    last_tan_[1] = sf.camera.tan_half_fov[1];
    write_ ^= 1u;
  } else {
    last_key_.valid = false;
  }
  if (regen_ok) {
    regen_key_ = now;
    regen_city_ = rt_recorded;
    last_tan_[0] = sf.camera.tan_half_fov[0];
    last_tan_[1] = sf.camera.tan_half_fov[1];
  } else {
    regen_key_.valid = false;   // the next regen frame starts the runtime's history again
  }
  regen_frame_ = regen_ok;
  ran_ = true;
  ++frames_run_;
  if (raw) {
    ++raw_frames_;
  } else if (regen_ok) {
    ++regen_frames_;
  } else if (use_history) {
    ++history_frames_;
  } else {
    ++no_motion_frames_;
  }
  // The images the mode in use does not need go back after a spell of it: the accumulation's two after kRawReleaseAfter raw frames in a row, the hit image after as many plain ones.
  if (raw) {
    plain_streak_ = 0;
    if (++raw_streak_ >= ssr::kRawReleaseAfter && (accum_[0].texture || accum_[1].texture)) {
      for (HostImage* image : {&accum_[0], &accum_[1]}) {
        if (image->texture) {
          passes_->ReleaseImage(*image);
        }
      }
      ++images_generation_;
      last_key_.valid = false;
    }
    if (!raw_first_logged_) {
      raw_first_logged_ = true;
      REXLOG_INFO("{} the first raw frame (DLSS Ray Reconstruction takes it): {} ray{} a pixel straight into the colour at {}x{} (no accumulation: its history and images are not used), "
                  "the hit distance of each in an R16_FLOAT image of {}x{} ({:.1f} MiB) for the guide pass",
                  kPrefix, numbers.rays, numbers.rays == 1 ? "" : "s", width, height, sf.render[0], sf.render[1], Mib(ssr::HitImageBytes(sf.render[0], sf.render[1])));
    }
  } else {
    raw_streak_ = 0;
    if (plain_streak_ < ssr::kRawReleaseAfter) {
      ++plain_streak_;
    } else if (hit_.texture) {
      passes_->ReleaseImage(hit_);
    }
  }
  if (regen_ok && !regen_first_logged_) {
    regen_first_logged_ = true;
    REXLOG_INFO("{} the first AMD FSR Ray Regeneration frame: the trace ({}x{}, {} ray{} a pixel) is the indirect specular signal with its hit distance in the alpha, five guides and the denoised image ({:.1f} MiB) are made at "
                "that size, the runtime's output goes through the resolve (the Fresnel and the class gain are multiplied in after the denoise); no accumulation of its own runs while it does",
                kPrefix, width, height, numbers.rays, numbers.rays == 1 ? "" : "s", Mib(regen_pass_.HeldBytes()));
  }
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first frame: {}x{} RGBA16F images (trace + two accumulated: {:.1f} MiB) for a {}x{} picture traced at {}x{} (Hi-Z level {} has the size), {} ray{} a pixel, up to {} Hi-Z "
                "steps, {} host sample{}, colour_post {}x half size {}, the material from {}, the wetness {:.2f} (VS c43.x {:.2f}, the game's reflection strength c43.y {:.2f}), the fallback share {:.2f}, vehicle rows {}",
                kPrefix, width, height, Mib(ssr::HeldBytes(width, height)), sf.render[0], sf.render[1], sf.traced[0], sf.traced[1], decision.size.hz, numbers.rays, numbers.rays == 1 ? "" : "s",
                numbers.max_iterations, depth.host_samples, depth.host_samples == 1 ? "" : "s", sf.colour_post_levels, ssr::ColourIsHalf(sf.render[1]) ? "yes" : "no",
                decision.stub ? "the stub" : "RT3", wet_, wet_raw_, wet_strength_, road.share, vehicle_rows_ ? "on" : "off");
  }
  if (frames_run_ % 1800 == 0) {
    REXLOG_INFO("{} {} frames ran: {} with a usable history, {} with the AO's vehicle table, wetness {:.2f} (c43.x {:.2f}, strength {:.2f}), road view: {}{}", kPrefix, frames_run_, history_frames_, vehicle_table_frames_, wet_,
                wet_raw_, wet_strength_, ssr::RoadViewWhyName(road_why),
                (raw_ever_asked_ ? std::format(", {} raw (Ray Reconstruction)", raw_frames_) : std::string()) +
                    (regen_ever_asked_ ? std::format(", {} Ray Regeneration ({} refused: the effect's own filter)", regen_frames_, regen_fallbacks_) : std::string()));
  }
  if (size_logged_[0] != width || size_logged_[1] != height) {
    if (size_logged_[0] != 0) {
      REXLOG_INFO("{} the images are {}x{} now", kPrefix, width, height);
    }
    size_logged_[0] = width;
    size_logged_[1] = height;
  }
  return true;
}

// RG-3: AMD FSR Ray Regeneration's part of a regen frame, after the trace: the guide and signal pass, then the runtime's dispatch at the trace's size. The camera is the occlusion's own of THIS frame (its axes and its
// motion into the last frame: the game's real camera exists only at the anchor, after the trigger) with the lens of the screen frame (ray_regen_guides_rules.h BuildCamera); the history of the runtime is kept
// only while the last frame was a regen frame of the same size and the camera motion is known (else the dispatch resets it). False: no image the resolve can read exists, and the frame is the effect's own.
// CHECK (not run on a Radeon): C8 the states of what the pass and the runtime read and write (the shared products in the layout their owners left them in, the trace moved to a state both stages read, the five
// outputs in SHADER_READ at the compute stage, the denoised image in unordered access, with the D3D12 debug layer on); C25 the runtime's output alpha is the hit distance preserved (the resolve reads alpha != 0 as
// "traced here": a runtime that wrote 0 would make the reflections vanish, never black: the game's own stays); C22 the reflections of moving cars carry the camera's motion only.
bool ScreenReflections::RecordRegen(plume::RenderCommandList* list, const ssr::Push& base, const screen::ScreenFrame& sf, const RegenCameraRows* cam, bool motion_usable, const RegenAsk& ask, uint32_t width,
                                    uint32_t height, const ssr::HistoryKey& now) {
  namespace pk = ssr::push;
  if (!cam || !trace_.texture) {
    return false;   // no camera of this frame (the occlusion's rows): nothing to hand the runtime's matrices from
  }
  const bool use_motion = ssr::HistoryUsable(regen_key_, now, motion_usable) && last_tan_[0] > 0.0f && last_tan_[1] > 0.0f;
  rgd::Push gp;
  gp.base = base;
  gp.base.Set(pk::kSrc, ssr::PackSlots(trace_.slot, trace_.slot));   // the trace, which the pass reads
  // The last frame's lens in the words the accumulation uses for it (the near and distance words: the pass reads neither).
  gp.base.SetFloat(pk::kPrevTanX, last_tan_[0] > 0.0f ? last_tan_[0] : sf.camera.tan_half_fov[0]);
  gp.base.SetFloat(pk::kPrevTanY, last_tan_[1] > 0.0f ? last_tan_[1] : sf.camera.tan_half_fov[1]);
  for (uint32_t i = 0; i < 12; ++i) {
    gp.SetFloat(rgd::push::kMotion + i, use_motion ? cam->motion[i] : 0.0f);
  }
  gp.SetFloat(rgd::push::kJitterX, ask.jitter[0]);
  gp.SetFloat(rgd::push::kJitterY, ask.jitter[1]);
  gp.SetFloat(rgd::push::kDepthSign, rgd::DepthSign());
  gp.x[rgd::push::kFlags] = use_motion ? rgd::push::kFlagHaveMotion : 0u;
  if (!regen_pass_.Record(list, gp, trace_, width, height)) {
    return false;
  }

  rgd::CameraInputs camera;
  for (int i = 0; i < 3; ++i) {
    camera.right[i] = cam->right[i];
    camera.up[i] = cam->up[i];
    camera.forward[i] = cam->forward[i];
  }
  for (int i = 0; i < 12; ++i) {
    camera.motion[i] = cam->motion[i];
  }
  camera.have_motion = use_motion;
  for (int i = 0; i < 4; ++i) {
    camera.lin[i] = sf.camera.lin[i];
  }
  camera.tan_half_fov[0] = sf.camera.tan_half_fov[0];
  camera.tan_half_fov[1] = sf.camera.tan_half_fov[1];
  camera.near_plane = sf.camera.near_plane;
  camera.far_plane = sf.camera.far_plane;
  const rgd::RegenCamera matrices = rgd::BuildCamera(camera);

  rayregen::DispatchFrame frame;
  frame.game_frame = now.frame;
  frame.force_reset = !use_motion;
  frame.render_size[0] = width;
  frame.render_size[1] = height;
  frame.linear_depth = regen_pass_.Tagged(rgd::Image::kDepth);
  frame.motion = regen_pass_.Tagged(rgd::Image::kMotion);
  frame.normals = regen_pass_.Tagged(rgd::Image::kNormals);
  frame.specular_albedo = regen_pass_.Tagged(rgd::Image::kAlbedo);
  // The indirect specular signal alone: its albedo is the specular one (the diffuse albedo may stay empty: AMD's rule, ray_regen_rules.h NeedsDiffuseAlbedo).
  frame.signals[size_t(rayregen::Signal::kIndirectSpecular)] = {regen_pass_.Tagged(rgd::Image::kSignal), regen_pass_.Tagged(rgd::Image::kDenoised)};
  frame.dispatch_signals = rayregen::FlagOf(rayregen::Signal::kIndirectSpecular);
  frame.jitter[0] = rgd::JitterAtSize(ask.jitter[0], sf.render[0], width);
  frame.jitter[1] = rgd::JitterAtSize(ask.jitter[1], sf.render[1], height);
  for (int i = 0; i < 3; ++i) {
    frame.camera_position_delta[i] = matrices.position_delta[i];
  }
  std::memcpy(frame.view, matrices.view, sizeof(frame.view));
  std::memcpy(frame.projection, matrices.projection, sizeof(frame.projection));
  frame.depth_bounds = matrices.bounds;
  frame.motion_in_pixels = false;
  frame.shared_linear_albedo = true;
  pass_timing_->Mark(list, "ray regen denoise");
  regen_pass_.PrepareDenoised(list);
  const bool dispatched = rayregen::Instance().Dispatch(list, frame);
  pass_timing_->Mark(list, "after ray regen denoise");
  if (!dispatched) {
    return false;   // refused (logged once by the runtime): the effect's own filter runs this frame
  }
  regen_pass_.ReadDenoised(list);   // the runtime left the output in unordered access: readable by the resolve
  return true;
}

void ScreenReflections::OnPresent() {
  capture_signal_ = nullptr;
  rt_.OnPresent();
  denoise_.OnPresent();
  regen_pass_.OnFrame(regen_frame_);
  regen_frame_ = false;
  if (ran_) {
    idle_frames_ = 0;
  } else {
    last_key_.valid = false;  // a frame without the effect breaks the history
    if ((trace_.texture || accum_[0].texture || accum_[1].texture) && ++idle_frames_ >= kReleaseAfterFrames) {
      ReleaseImages();
      idle_frames_ = 0;
    }
  }
  ran_ = false;
  guest_.Present();  // the guest frame is over: the next list starts a frame afresh (BeginFrame)
}

}  // namespace mcla::gpu

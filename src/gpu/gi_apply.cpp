// MCLA GPU renderer -- the probe field's apply. See gi_apply.h; the rules and the twin of the maths are in gi_apply_rules.h, the passes in hlsl/gi_apply_ps.hlsl.

#include "gi_apply.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "ao.h"
#include "denoiser_setting.h"
#include "descriptors.h"
#include "device.h"
#include "gbuffer.h"
#include "gi_field.h"
#include "gi_pixel.h"
#include "host_shaders.h"
#include "live_text.h"
#include "pass_timing.h"
#include "records.h"
#include "screen_shared.h"
#include "ssgi_level.h"
#include "temporal_identities.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace ga = gi_apply;

namespace {

constexpr const char* kPrefix = "[gi apply]";
constexpr uint32_t kIntervalFrames = 600;
// The lookup image goes back after this many frames without a use (the pause menu, the setting turned off).
constexpr uint32_t kReleaseAfterIdleFrames = 600;
constexpr plume::RenderFormat kLookupFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) | uint8_t(plume::RenderColorWriteEnable::BLUE);

// The bounce light's strength level (mcla_gpu_ssgi_intensity) and its debug overrides, read as the occlusion reads them (ao.cpp PlanGi): the apply's strength and cap are the level's, so the owner's
// tuning of BOUNCE LIGHT STRENGTH stays valid on a ray tracing card.
LiveText& SsgiLevelText() {
  static LiveText text("mcla_gpu_ssgi_intensity");
  return text;
}

ssgi_level::Effective LevelNumbers() {
  ssgi_level::Overrides overrides;
  overrides.strength = larecomp::settings::mcla_gpu_ssgi_strength.Get();
  overrides.limit = larecomp::settings::mcla_gpu_ssgi_clamp.Get();
  overrides.reach = larecomp::settings::mcla_gpu_ssgi_reach.Get();
  overrides.cap = larecomp::settings::mcla_gpu_ssgi_test_clamp.Get();
  overrides.far_steps = larecomp::settings::mcla_gpu_ssgi_far_steps.Get();
  return ssgi_level::Choose(ssgi_level::Resolve(SsgiLevelText().Get()), overrides);
}

float Luma3(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

}  // namespace

GiApply::~GiApply() = default;

void GiApply::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  layout_failed_ = false;
  pipelines_failed_ = false;
}

void GiApply::Shutdown() {
  // The GPU is idle and the FrameRing drained: the framebuffers the draws used belong to the EDRAM's cache and the lookup image's own, the rest goes at once.
  if (passes_ != nullptr) {
    ReleaseImage();
  }
  pipelines_.clear();
  shaders_ = {};
  for (Slot& slot : slots_) {
    slot.set.reset();
    slot.buffer.reset();
  }
  layout_.reset();
  gbuffer_ = nullptr;
  screen_ = nullptr;
  ao_ = nullptr;
  field_ = nullptr;
  pixel_ = nullptr;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
  enabled_ = false;
}

uint64_t GiApply::HeldBytes() const {
  uint64_t bytes = lookup_.texture ? TextureBytes(lookup_.width, lookup_.height, 1, lookup_.format) : 0;
  for (const Slot& slot : slots_) {
    bytes += slot.buffer ? ga::kBufferAlloc : 0u;
  }
  return bytes;
}

void GiApply::ReleaseImage() {
  if (lookup_.texture && passes_) {
    passes_->ReleaseImage(lookup_);
  }
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------

bool GiApply::EnsureLayout() {
  if (layout_) {
    return true;
  }
  if (layout_failed_ || !device_ || !passes_ || !heap_) {
    return false;
  }
  // set 0 (space0) the bindless Texture2D<float4>[]; set 1 (space1) the same set read as Texture2DMS<float4>[]; set 2 (space2) the samplers; set 3 (space3) the buffer of rows; the push block
  // (b0, space4) for the pixel stage.
  set_builder_.begin();
  set_builder_.addByteAddressBuffer(0);  // t0, space3
  set_builder_.end();
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  builder.addDescriptorSet(heap_->texture_set_builder());
  builder.addDescriptorSet(heap_->texture_set_builder());
  builder.addDescriptorSet(heap_->sampler_set_builder());
  builder.addDescriptorSet(set_builder_);
  builder.addPushConstant(0, ga::kPushSpace, sizeof(ga::Push), plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  layout_ = device_->CreatePipelineLayout(builder, "gi apply layout");
  if (!layout_) {
    layout_failed_ = true;
    REXLOG_ERROR("{} the layout could not be made: no ray traced bounce light this session", kPrefix);
    return false;
  }
  return true;
}

bool GiApply::EnsureSlot(uint32_t frame_slot) {
  if (frame_slot >= kFramesInFlight || !device_) {
    return false;
  }
  Slot& slot = slots_[frame_slot];
  if (slot.set) {
    return true;
  }
  slot.buffer = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(ga::kBufferAlloc, plume::RenderBufferFlag::STORAGE), "gi apply rows");
  if (!slot.buffer) {
    return false;
  }
  slot.set = device_->CreateDescriptorSet(set_builder_, 1, 0, "gi apply set");
  if (!slot.set) {
    slot.buffer.reset();
    return false;
  }
  slot.set->setBuffer(0, slot.buffer.get(), ga::kBufferAlloc);
  return true;
}

plume::RenderShader* GiApply::ShaderFor(uint32_t which) {
  if (which >= shaders_.size()) {
    return nullptr;
  }
  if (!shaders_[which]) {
    const HostShaderBlob blob = GiApplyShader(which);
    shaders_[which] = passes_->CreateShader(blob.data, blob.size, blob.name);
  }
  return shaders_[which].get();
}

plume::RenderPipeline* GiApply::PipelineFor(Kind kind, plume::RenderFormat format, uint32_t samples, uint32_t shader) {
  for (PipelineEntry& entry : pipelines_) {
    if (entry.kind == kind && entry.format == format && entry.samples == samples && entry.shader == shader) {
      return entry.pipeline.get();
    }
  }
  plume::RenderShader* ps = pipelines_failed_ ? nullptr : ShaderFor(shader);
  if (!layout_ || !ps) {
    pipelines_failed_ = true;
    return nullptr;
  }
  PipelineEntry entry;
  entry.kind = kind;
  entry.format = format;
  entry.samples = samples;
  entry.shader = shader;
  if (kind == Kind::kLookup) {
    // The lookup image: written whole (rgb and the linear depth in alpha), no blend.
    entry.pipeline = passes_->CreatePipeline(*layout_, *ps, format, "gi apply lookup");
  } else {
    plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
    blend.blendEnabled = true;
    if (kind == Kind::kAdd) {
      // ADD (ONE, ONE) on the colour, the alpha never written (the light pass's blend): the bounce is added to every sample of the pixel.
      blend.srcBlend = plume::RenderBlend::ONE;
      blend.dstBlend = plume::RenderBlend::ONE;
    } else {
      // The debug view: its colour over the picture where it has something (alpha 1), the picture left alone where it has not (alpha 0).
      blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
      blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
    }
    blend.blendOp = plume::RenderBlendOperation::ADD;
    blend.srcBlendAlpha = plume::RenderBlend::ZERO;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
    blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
    blend.renderTargetWriteMask = kWriteRgb;
    entry.pipeline = passes_->CreatePipeline(*layout_, *ps, format, blend, samples, kind == Kind::kAdd ? "gi apply" : "gi apply view");
  }
  if (!entry.pipeline) {
    pipelines_failed_ = true;
    REXLOG_ERROR("{} a pipeline could not be made ({} host samples, shader {}): no ray traced bounce light this session (reported once)", kPrefix, samples, shader);
    return nullptr;
  }
  plume::RenderPipeline* raw = entry.pipeline.get();
  pipelines_.push_back(std::move(entry));
  return raw;
}

void GiApply::Bind(plume::RenderCommandList* list, const Slot& slot) const {
  list->setGraphicsPipelineLayout(layout_.get());
  list->setGraphicsDescriptorSet(heap_->texture_set(), 0);
  list->setGraphicsDescriptorSet(heap_->texture_set(), 1);
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 2);
  list->setGraphicsDescriptorSet(slot.set.get(), 3);
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void GiApply::BeginFrame() {
  ++frame_;
  view_ = uint32_t(std::clamp<int>(int(larecomp::settings::mcla_gpu_rt_gi_view.Get()), 0, int(ga::kViewMax)));
  if (!ga::IsApplyView(view_)) {
    view_ = 0;  // the field owner's own views (4, 7, 8) and the gaps are not this pass's
  }
  // Only while the field runs this frame (GiField::BeginFrame has been called): with it off nothing asks for any plane or product, so no sidecar is made and the occlusion's depth chain runs for
  // nobody; the wants are sticky and read at the owners' next frame boundary.
  const bool was = enabled_;
  enabled_ = field_ != nullptr && field_->running();
  if (enabled_ != was) {
    REXLOG_INFO("{} {} (the probe field {}running)", kPrefix, enabled_ ? "on" : "off", enabled_ ? "" : "is not ");
  }
  if (gbuffer_ != nullptr) {
    gbuffer_->Want(gbuffer::Consumer::kRtBounce, enabled_ ? gbuffer::ConsumerPlanes(gbuffer::Consumer::kRtBounce) : 0u);
  }
  if (screen_ != nullptr) {
    screen_->Want(screen::Consumer::kRtBounce, enabled_ ? screen::ProductBit(screen::kLinDepth) : 0u);
  }
  // G6: the per-pixel pass is a consumer of the DENOISER row (dlss_rr_rules.h rr::kConsumers kRtGi: its raw mode exists, AUTO does not trust Ray Reconstruction with it before the RR-6 A/B). It says it is
  // wanted every frame, whatever the plan is: the field runs, the frame's layout has the pass (the strong card at ULTRA, gi_frame.h) and the pass is made. This call comes before the temporal system's frame
  // boundary (frame_order.h kBeginGiApply before kBeginTemporal: no lag).
  pixel_wanted_ = enabled_ && pixel_ != nullptr && pixel_->available() && gi_pixel::ConsumerWanted(field_->fit().wanted);
  denoiser::NoteConsumer(rr::Consumer::kRtGi, pixel_wanted_);
  ssgi_ran_share_ = 0.0f;
  trigger_seen_ = false;
  ran_this_frame_ = false;
}

float GiApply::TakeSsgiShare() {
  if (field_ == nullptr) {
    return 1.0f;
  }
  const float share = ga::SsgiShareFor(field_->frame());
  ssgi_ran_share_ = share;
  return share;
}

float GiApply::LevelStrength() { return LevelNumbers().strength; }

bool GiApply::OnTrigger(const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool gbuffer_published, uint32_t frame_slot, bool raw_requested) {
  expect_add_ = false;
  const bool recorded = OnTriggerImpl(summary, list, gbuffer_published, frame_slot, raw_requested);
  if (expect_add_) {
    starved_frames_ = recorded ? 0u : starved_frames_ + 1u;  // the watchdog (Starved): a frame the field stood for and the apply drew nothing
  }
  return recorded;
}

const HostImage* GiApply::RawHitImage() const {
  if (pixel_ == nullptr || !pixel_wanted_) {
    return nullptr;
  }
  const GiPixel::RawOutput out = pixel_->raw_output();
  return out.valid && out.frame == frame_ ? out.hit : nullptr;   // the pass's own frame is the apply's counter: a frame the trigger did not reach has none
}

bool GiApply::OnTriggerImpl(const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool gbuffer_published, uint32_t frame_slot, bool raw_requested) {
  if (!enabled_ || !list || trigger_seen_ || field_ == nullptr || gbuffer_ == nullptr || screen_ == nullptr || edram_ == nullptr || !summary.source) {
    return false;
  }
  // The trigger is the copy of the MAIN view's depth (the field's own rule, gi_integration.cpp OnResolve: temporal::MainDepthSamples of the source's key). The scene calls this at every resolve, and any depth
  // copy has a source: with the sun up the game copies its shadow cascades' depth first, and the first such copy took the frame's one run ("trigger_seen_") while the field was not published yet
  // (its own trigger skips it): the apply saw no field in every frame of the day (the chain's noon runs: 'the apply pass never ran'), while at night, with no cascades, the main copy came first.
  if (!summary.depth_copy || temporal::MainDepthSamples(summary.source_key) == 0) {
    return false;
  }
  trigger_seen_ = true;  // once a frame
  const gi::GiFrame& gf = field_->frame();
  if (!gf.FieldPresent()) {
    Count(ga::Skip::kNoField);  // the field is not in this frame: nothing is recorded, the picture is the game's
    return false;
  }
  ++frames_with_field_;
  const float field_share = ga::FieldShare(gf, ssgi_ran_share_);
  const ssgi_level::Effective numbers = LevelNumbers();
  const bool nothing_to_add = !(field_share > 0.0f) || !(numbers.strength > 0.0f);
  expect_add_ = !nothing_to_add;
  if (nothing_to_add && view_ == 0) {  // a debug view is still drawn: it is how the first frames of a fade are looked at
    Count(ga::Skip::kNoStrength);
    return false;
  }
  if (!gbuffer_published) {
    Count(ga::Skip::kNoPlanes);
    return false;
  }
  const gbuffer::GBufferFrame& gb = gbuffer_->frame();
  if (!gb.Has(gbuffer::kPlaneNormal) || !gb.Has(gbuffer::kPlaneAlbedo)) {
    Count(ga::Skip::kNoPlanes);
    return false;
  }

  // The camera: the lens and the projection's depth rows from the shared products' camera, the pose from the occlusion's trigger pose where it trusts the vertex constants, else the last frame's.
  const screen::ScreenFrame& sf = screen_->Frame();
  if ((sf.camera.flags & screen::kCameraValid) == 0 || sf.frame_serial != gb.frame_serial || (sf.flags & screen::kFramePause) != 0 || !(sf.camera.tan_half_fov[0] > 0.0f) ||
      !(sf.camera.tan_half_fov[1] > 0.0f) || !sf.Has(screen::kLinDepth) || sf.lin_depth_slot[0] == screen::kNoSlot || sf.traced[0] == 0 || sf.traced[1] == 0 || sf.render[0] == 0 || sf.render[1] == 0) {
    Count(ga::Skip::kNoCamera);
    return false;
  }
  float pose[16];
  bool own_pose = false;
  if (ao_ == nullptr || !ao_->TriggerViewInverse(pose, &own_pose)) {
    std::memcpy(pose, sf.camera.view_inverse, sizeof(pose));
  }
  ga::Frame fr;
  if (!ga::CameraFromViewInverse(pose, &fr)) {
    Count(ga::Skip::kNoCamera);
    return false;
  }
  // The light: the frame's constants (PS c26, c39) and the sun's world direction.
  float ambient[3] = {}, sun[3] = {};
  double to_sun[3] = {};
  if (ao_ == nullptr || !ao_->FrameLight(ambient, sun, to_sun)) {
    Count(ga::Skip::kNoSun);
    return false;
  }

  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply, local_lights.cpp).
  EdramTargets::Target& depth = *summary.source;
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != gb.host_samples || depth.host_samples != gb.host_samples || found->scale.Edge(found->width) != gb.width ||
      found->scale.Edge(found->height) != gb.height) {
    Count(ga::Skip::kNoTarget);
    return false;
  }

  // The sizes: the picture's, the occlusion's traced one (half above 1080 lines: the lookup is made there and upsampled), and the occlusion's visibility image where there is one.
  // The traced images are half the picture's size (the lookup is made there and upsampled) or the picture's own: the shared frame says which. The G-buffer's planes are NOT the picture: they are the
  // main colour target's, which the guest pads (1280x736 for a 1280x720 picture, 1920x1088 for 1080 lines, 832x478 for 832x468: 612e6dee), so comparing the traced size with them took a full size trace
  // (the picture's own at 1080 lines and below, with the occlusion or the contact shadows at ultra: the High class's auto) for a half size one, and the upsample then read the occlusion's visibility
  // and the vehicle mark at half the coordinates.
  const bool up = (sf.flags & screen::kFrameHalfTraced) != 0;
  AmbientOcclusion::OcclusionView ov;
  const bool has_visibility = ao_->FrameOcclusionNow(&ov) && ov.has_visibility && ov.visibility_slot != ga::kNoSlot && ov.traced[0] == sf.traced[0] && ov.traced[1] == sf.traced[1];
  const bool view_run = view_ != 0;
  const uint32_t apply_shader = 2u + (gb.host_samples > 1 ? 1u : 0u) + (up ? 2u : 0u);
  const uint32_t lookup_shader = gb.host_samples > 1 ? 1u : 0u;
  plume::RenderPipeline* apply_pipeline = nullptr;
  plume::RenderPipeline* lookup_pipeline = nullptr;
  if (EnsureLayout()) {
    apply_pipeline = PipelineFor(view_run ? Kind::kView : Kind::kAdd, found->format, gb.host_samples, apply_shader);
    lookup_pipeline = up ? PipelineFor(Kind::kLookup, kLookupFormat, 1u, lookup_shader) : nullptr;
  }
  if (!apply_pipeline || (up && !lookup_pipeline)) {
    Count(ga::Skip::kNoPipeline);
    if (!no_pipeline_logged_) {
      no_pipeline_logged_ = true;
      REXLOG_WARN("{} frame {}: no layout, shader or pipeline: the pass does not run (reported once)", kPrefix, frame_);
    }
    return false;
  }
  if (!EnsureSlot(frame_slot) || (up && !passes_->EnsureImage(lookup_, sf.traced[0], sf.traced[1], kLookupFormat, "gi apply lookup"))) {
    Count(ga::Skip::kNoBuffer);
    return false;
  }

  // The frame's rows: the camera, the lens, the sizes, the light, the bounce light level's cap and strength, the occlusion's look, then the field's own 20 rows in front.
  fr.tan_x = sf.camera.tan_half_fov[0];
  fr.tan_y = sf.camera.tan_half_fov[1];
  for (int i = 0; i < 4; ++i) {
    fr.lin[i] = sf.camera.lin[i];
  }
  // The picture's size, which the lens spans (the occlusion's own), not the planes': a pixel's place in the picture is its row over the picture's height (the pad is below the picture).
  fr.render[0] = float(sf.render[0]);
  fr.render[1] = float(sf.render[1]);
  fr.traced[0] = float(sf.traced[0]);
  fr.traced[1] = float(sf.traced[1]);
  for (int i = 0; i < 3; ++i) {
    fr.sun_dir[i] = float(to_sun[i]);
  }
  fr.ambient = Luma3(ambient);
  fr.sun = Luma3(sun);
  fr.cap = numbers.cap;
  fr.field_share = field_share;
  fr.strength = numbers.strength;
  if (has_visibility) {
    fr.ao_strength = ov.strength;
    fr.ao_fade_end = ov.fade_end;
    fr.paint_power = ov.paint_power;
  }
  fr.view_gain = 1.0f;
  const ga::Buffer base_rows = ga::MakeBuffer(field_->gpu_frame(), fr);
  Slot& slot = slots_[frame_slot];

  // The colour is drawn into; the depth plane, the G-buffer's planes (published in the shader-read layout), the occlusion's depth and visibility images and the field's three textures are read.
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    Count(ga::Skip::kNoTarget);
    return false;
  }
  edram_->Transition(list, depth, plume::RenderTextureLayout::SHADER_READ);

  // G5, ULTRA on a strong card only (gi_pixel_rules.h): the per-pixel detail pass, BEFORE the lookup (which mixes its denoised term into the field's irradiance). The layout decides first: at HIGH, OFF and on every
  // card without the extended layout nothing is asked, recorded or marked. The pass is the same trigger's compute work (outside any render pass: the depth plane is in the shader-read layout above), and it
  // publishes its image in the apply's own copy of the field's frame, so that the rows below carry its slot, its guides and the flag.
  gi::GiFrame shown = gf;
  bool pixel_ran = false;
  if (pixel_ != nullptr && pixel_->available() && gi::BudgetOf(gf.layout).pixel_rays_per_pixel != 0) {
    GiPixel::Inputs pixel_in;
    pixel_in.frame = frame_;
    pixel_in.frame_slot = frame_slot;
    pixel_in.field = &gf;
    pixel_in.rows = &base_rows;
    pixel_in.camera = &fr;
    pixel_in.traced[0] = sf.traced[0];
    pixel_in.traced[1] = sf.traced[1];
    pixel_in.render[0] = sf.render[0];
    pixel_in.render[1] = sf.render[1];
    pixel_in.depth_slot = summary.sample_slot;
    pixel_in.normal_slot = gb.slot[gbuffer::kPlaneNormal];
    pixel_in.albedo_slot = gb.slot[gbuffer::kPlaneAlbedo];
    pixel_in.lin_slot = sf.lin_depth_slot[0];
    pixel_in.sample0 = gb.sample0;
    pixel_in.multisampled = gb.host_samples > 1;
    pixel_in.raw = raw_requested;
    if (pixel_->Begin(pixel_in)) {
      pass_timing_->Mark(list, "rt gi pixel");
      pixel_->RecordTrace(list);
      pass_timing_->Mark(list, "rt gi denoise");
      pixel_->RecordDenoise(list);
      pixel_ran = pixel_->Publish(&shown);
    }
  }
  // The frame's rows: the field's own as published, or (with the pixel pass's image in it) as the apply's copy says.
  const ga::Buffer buffer = pixel_ran ? ga::MakeBuffer(gi::MakeGpuFrame(shown), fr) : base_rows;
  void* mapped = slot.buffer->map();
  if (mapped == nullptr) {
    Count(ga::Skip::kNoBuffer);
    return false;
  }
  std::memcpy(mapped, buffer.data(), sizeof(buffer));
  slot.buffer->unmap();

  ga::Push push = {};
  push.slots[0] = summary.sample_slot;
  push.slots[1] = gb.slot[gbuffer::kPlaneNormal];
  push.slots[2] = gb.slot[gbuffer::kPlaneAlbedo];
  push.slots[3] = sf.lin_depth_slot[0];
  push.info[0] = has_visibility ? ov.visibility_slot : ga::kNoSlot;
  push.info[1] = up ? lookup_.slot : ga::kNoSlot;
  push.info[2] = gb.sample0;
  push.info[3] = ga::PackInfoW((up ? ga::kFlagLowTraced : 0u) | (has_visibility ? ga::kFlagVisibility : 0u), view_);

  if (up) {
    // The lookup, at the traced size, into the image (cleared on its first use: a new render target is initialised before it is drawn into, D3D12 debug layer id 1422).
    pass_timing_->Mark(list, "gi lookup");
    const bool first_use = lookup_.layout == plume::RenderTextureLayout::UNKNOWN;
    if (lookup_.layout != plume::RenderTextureLayout::COLOR_WRITE) {
      list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(lookup_.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
      lookup_.layout = plume::RenderTextureLayout::COLOR_WRITE;
    }
    list->setFramebuffer(lookup_.framebuffer.get());
    if (first_use) {
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    }
    const plume::RenderViewport lookup_viewport{0.0f, 0.0f, float(lookup_.width), float(lookup_.height)};
    list->setViewports(&lookup_viewport, 1);
    const plume::RenderRect lookup_scissor(0, 0, int32_t(lookup_.width), int32_t(lookup_.height));
    list->setScissors(&lookup_scissor, 1);
    Bind(list, slot);
    list->setPipeline(lookup_pipeline);
    list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    list->drawInstanced(3, 1, 0, 0);
    list->setFramebuffer(nullptr);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(lookup_.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    lookup_.layout = plume::RenderTextureLayout::SHADER_READ;
  }

  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    Count(ga::Skip::kNoTarget);
    return false;
  }
  pass_timing_->Mark(list, view_run ? "gi apply view" : "gi apply");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(gb.width), float(gb.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(gb.width), int32_t(gb.height));
  list->setScissors(&scissor, 1);
  Bind(list, slot);
  list->setPipeline(apply_pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  pass_timing_->Mark(list, "after gi apply");

  Count(ga::Skip::kRecorded);
  ++frames_run_;
  ran_this_frame_ = true;
  frames_upsampled_ += up ? 1u : 0u;
  frames_with_visibility_ += has_visibility ? 1u : 0u;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} frame {}: the first run: the field's {} ({} cascades, fade {:.2f}, safe range {:.0f} m), the screen-space bounce light ran at share {:.2f}, so the field's share is {:.2f}; the picture {}x{} at {} host samples, "
                "the lookup {} ({}x{}), G-buffer planes {}, the occlusion's visibility {}, the camera {}; strength {:.2f}, cap {:.2f} x the frame's light (ambient {:.3f}, sun {:.3f}), view {}",
                kPrefix, frame_, gi::LayoutName(gf.layout), gf.cascade_count, gf.fade, gf.safe_range_m, ssgi_ran_share_, field_share, gb.width, gb.height, gb.host_samples, up ? "at the traced size, upsampled" : "inline",
                sf.traced[0], sf.traced[1], gb.valid, has_visibility ? "multiplies it" : "not in this frame", own_pose ? "the frame's own pose" : "the last frame's", fr.strength, fr.cap, fr.ambient, fr.sun, view_);
  }
  return true;
}

void GiApply::OnPresent() {
  if (pixel_ != nullptr) {
    pixel_->OnPresent();  // the per-pixel pass's idle count: its images go back after a spell without a run
  }
  idle_frames_ = ran_this_frame_ ? 0u : idle_frames_ + 1u;
  if (lookup_.texture && idle_frames_ >= kReleaseAfterIdleFrames) {
    ReleaseImage();  // the pause menu, the setting turned off: the lookup image goes back (made again on the next run)
    idle_frames_ = 0;
  }
  if (enabled_ && frame_ % kIntervalFrames == 0) {
    LogInterval();
  }
}

void GiApply::LogInterval() {
  std::string why;
  for (uint32_t i = 0; i < uint32_t(ga::Skip::kCount); ++i) {
    if (counts_[i] != 0) {
      why += std::format("{}{} {}", why.empty() ? "" : ", ", ga::SkipName(ga::Skip(i)), counts_[i]);
    }
  }
  REXLOG_INFO("{} {} frames with the effect on: the trigger frames: {}; the field was in {} frames, the lookup was upsampled in {}, the occlusion's visibility multiplied the bounce in {}; held {} B", kPrefix,
              kIntervalFrames, why.empty() ? std::string("none") : why, frames_with_field_, frames_upsampled_, frames_with_visibility_, HeldBytes());
  counts_.fill(0);
  frames_with_field_ = 0;
  frames_upsampled_ = 0;
  frames_with_visibility_ = 0;
}

}  // namespace mcla::gpu

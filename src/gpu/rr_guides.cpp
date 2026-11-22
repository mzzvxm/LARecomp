// MCLA GPU renderer -- DLSS Ray Reconstruction's guide pass. See rr_guides.h.

#include "rr_guides.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "gbuffer.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[rr guides]";
constexpr const char* kPipelineNames[3] = {"rr guides", "rr guides ms", "rr guides view"};
constexpr plume::RenderTextureFlags kImageFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

uint64_t PipelineKey(uint32_t index) { return HostPrecache::Key(HostPrecache::kRrGuides, index); }

// The renderer's format of a guide (the rules name them by what Streamline takes).
plume::RenderFormat FormatOf(rr_guides::Guide guide) {
  switch (rr_guides::SpecOf(guide).format) {
    case rr::Format::kRgba16Float: return plume::RenderFormat::R16G16B16A16_FLOAT;
    case rr::Format::kR16Float: return plume::RenderFormat::R16_FLOAT;
    default: break;
  }
  return plume::RenderFormat::UNKNOWN;
}

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

}  // namespace

RrGuides::~RrGuides() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void RrGuides::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  listed_.fill(false);
  failed_.fill(false);
  ready_to_list_ = MakeLayoutsAndShaders(device, heap);
  if (!ready_to_list_) {
    REXLOG_ERROR("{} the layouts or the shaders could not be made: no Ray Reconstruction guide images this session", kPrefix);
  }
  // Nothing is listed at the start: the pass is wanted only with the debug switch or a view on, and then its pipelines are made in the background the first time (Pipeline).
  passes.precache().Note("Ray Reconstruction guide pass: made when first wanted", 0);
}

bool RrGuides::MakeLayoutsAndShaders(Device& device, BindlessHeap& heap) {
  guides_builder_.begin();
  for (uint32_t i = 0; i < rr_guides::kGuideCount; ++i) {
    guides_builder_.addReadWriteTexture(i);  // u0..u4: the five guide images
  }
  guides_builder_.end();
  view_builder_.begin();
  view_builder_.addReadWriteTexture(0);  // u0: the upscaler's output
  view_builder_.end();

  // The host pass layout's four texture sets (spaces 0 to 3: host_pass_common.hlsli), then the output set at space 4 and the push block at (b0, space 5).
  const auto make_layout = [&](const plume::RenderDescriptorSetBuilder& outputs, uint32_t push_bytes, const char* name) {
    plume::RenderPipelineLayoutBuilder builder;
    builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
    for (uint32_t set = 0; set < 4; ++set) {
      builder.addDescriptorSet(heap.texture_set_builder());
    }
    const uint32_t output_set = builder.addDescriptorSet(outputs);
    builder.addPushConstant(0, kPushSpace, push_bytes, plume::RenderShaderStageFlag::COMPUTE);
    builder.end();
    if (output_set != kOutputSet) {
      REXLOG_ERROR("{} the layout's output set is not at the index the shader declares", kPrefix);
      return std::unique_ptr<plume::RenderPipelineLayout>();
    }
    return device.CreatePipelineLayout(builder, name);
  };
  guides_layout_ = make_layout(guides_builder_, sizeof(rr_guides::GuidesPush), "rr guides layout");
  view_layout_ = make_layout(view_builder_, sizeof(rr_guides::ViewPush), "rr guides view layout");
  if (!guides_layout_ || !view_layout_) {
    return false;
  }
  const HostShaderBlob blobs[3] = {RrGuidesShader(false), RrGuidesShader(true), RrGuidesViewShader()};
  for (size_t i = 0; i < 3; ++i) {
    shaders_[i] = passes_->CreateShader(blobs[i].data, blobs[i].size, blobs[i].name);
    if (!shaders_[i]) {
      return false;
    }
  }
  return true;
}

void RrGuides::Shutdown() {
  // The host builder's functions name this object's layouts and shaders: stop it first (the other owners do the same; stopping twice is safe).
  if (passes_) {
    passes_->precache().Stop();
  }
  if (passes_run_ != 0 || views_run_ != 0 || not_ready_ != 0 || refused_ != 0) {
    REXLOG_INFO("{} {} guide pass(es) ({} reflection hit distance image(s) and {} bounce light one(s) read), {} debug view(s); {} frame(s) waited for the pipeline, {} frame(s) had no guides (the gate: no G-buffer, a plane missing, the pause or map view)",
                kPrefix, passes_run_, hits_used_, bounce_hits_used_, views_run_, not_ready_, refused_);
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (CarRtRefl::Shutdown).
  for (HostImage& image : images_) {
    image = HostImage{};
  }
  set_ = OutputSet{};
  view_set_ = OutputSet{};
  view_output_ = nullptr;
  for (auto& pipeline : pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : shaders_) {
    shader.reset();
  }
  guides_layout_.reset();
  view_layout_.reset();
  ready_to_list_ = false;
  ready_ = false;
  wanted_ = false;
  guides_ = TemporalGuides{};
  gbuffer_ = nullptr;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> RrGuides::MakePipeline(uint32_t index) {
  const plume::RenderPipelineLayout* layout = index == 2 ? view_layout_.get() : guides_layout_.get();
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout, shaders_[index].get(), rr_guides::kGroup, rr_guides::kGroup, 1), kPipelineNames[index]);
}

plume::RenderPipeline* RrGuides::Pipeline(uint32_t index) {
  if (pipelines_[index]) {
    return pipelines_[index].get();
  }
  if (failed_[index] || !ready_to_list_) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  if (!listed_[index]) {
    listed_[index] = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({PipelineKey(index), kPipelineNames[index], [this, index] { return MakePipeline(index); }});
    precache.AddLive("Ray Reconstruction guide pass", std::move(items));
    REXLOG_INFO("{} the pass wanted its pipeline '{}', which was not listed at the start: it is made in the background, and the pass runs from the frame it is done", kPrefix, kPipelineNames[index]);
  }
  if (!precache.Ready(PipelineKey(index))) {
    return nullptr;
  }
  pipelines_[index] = precache.Acquire(PipelineKey(index), kPipelineNames[index], [this, index] { return MakePipeline(index); });
  if (!pipelines_[index]) {
    failed_[index] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no Ray Reconstruction guide images this session", kPrefix, kPipelineNames[index]);
  }
  return pipelines_[index].get();
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

rr_guides::View RrGuides::ViewSetting() { return rr_guides::ViewOf(int(larecomp::settings::mcla_gpu_dlss_rr_view.Get())); }

void RrGuides::BeginFrame(bool wanted) {
  ready_ = false;
  guides_ = TemporalGuides{};
  gate_ = rr::Gate::kOpen;
  view_ = ViewSetting();
  wanted_ = wanted && ready_to_list_;
  if (gbuffer_) {
    // The planes exist only while somebody wants them: from the frame after this one (the G-buffer read its wants before the temporal system's frame boundary, frame_order.h kBeginTemporal).
    gbuffer_->Want(gbuffer::Consumer::kRayReconstruction, wanted_ ? rr_guides::kWantedPlanes : 0u);
  }
  if (wanted_) {
    unwanted_frames_ = 0;
  } else if (unwanted_frames_ < kReleaseAfter) {
    if (++unwanted_frames_ == kReleaseAfter) {
      ReleaseImages();  // not wanted for a while: the video memory goes back
    }
  }
}

void RrGuides::Refuse(rr::Gate gate, uint64_t frame, const char* why) {
  ++refused_;
  const size_t kind = std::min<size_t>(size_t(gate), refused_logged_.size() - 1);
  if (!refused_logged_[kind]) {
    refused_logged_[kind] = true;
    REXLOG_INFO("{} frame {} has no guide images: {} (the first of its kind; the frame is Super Resolution's, the summary counts them)", kPrefix, frame, why);
  }
}

bool RrGuides::Record(plume::RenderCommandList* list, const Inputs& in) {
  ready_ = false;
  guides_ = TemporalGuides{};
  if (!wanted_ || !list || !gbuffer_ || !device_) {
    return false;
  }
  const uint32_t width = in.render_size[0];
  const uint32_t height = in.render_size[1];
  // The frame gate (dlss_rr_rules.h): the planes of this frame's G-buffer must be there, whole, and cover the render size. They are a few rows larger than it (the guest pads its main colour target:
  // 832x478 planes for an 832x468 render size, 1280x736 for 1280x720), from the same top-left corner, and the shader reads the render size's part of them (it loads by pixel, never by a 0..1 coordinate).
  const gbuffer::GBufferFrame& frame = gbuffer_->frame();
  rr::FrameFacts facts;
  facts.gbuffer_published = frame.valid != 0;
  facts.planes_valid = frame.valid;
  facts.gbuffer_size[0] = frame.width;
  facts.gbuffer_size[1] = frame.height;
  facts.render_size[0] = width;
  facts.render_size[1] = height;
  facts.cutscene = false;  // a cutscene's frames carry no sidecar: the frame is then not published (gbuffer.h)
  facts.pause_view = in.pause_view;
  gate_ = rr::FrameGate(facts);
  if (gate_ != rr::Gate::kOpen) {
    Refuse(gate_, in.frame, rr::GateText(gate_));
    return false;
  }
  for (uint32_t plane = 0; plane < gbuffer::kPlaneCount; ++plane) {
    if (!frame.Has(gbuffer::Plane(plane)) || !gbuffer_->PlaneTexture(gbuffer::Plane(plane))) {
      gate_ = rr::Gate::kPlanesMissing;
      Refuse(gate_, in.frame, "a plane has no texture or bindless slot");
      return false;
    }
  }
  if (!in.depth || !in.depth->texture || in.depth->slot == kInvalidBindlessSlot || in.depth->width != width || in.depth->height != height) {
    Refuse(rr::Gate::kNoGBuffer, in.frame, "the kept depth is not there at the render size");
    return false;
  }
  const bool multisampled = frame.host_samples > 1;
  plume::RenderPipeline* pipeline = Pipeline(multisampled ? 1u : 0u);
  if (!pipeline) {
    ++not_ready_;  // being made, or failed (logged)
    return false;
  }
  if (!EnsureImages(width, height) || !EnsureSet()) {
    return false;
  }

  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  const bool have_mask = in.mask && in.mask->texture && in.mask->slot != kInvalidBindlessSlot && in.mask->width == width && in.mask->height == height;
  const bool have_transparency = in.transparency && in.transparency->texture && in.transparency->slot != kInvalidBindlessSlot && in.transparency->width == width &&
                                 in.transparency->height == height;

  // RR-4: the reflection effects' hit distance images of this frame, where they are the right ones (the render size, readable): up to two sources.
  const HostImage* hit_used[2] = {nullptr, nullptr};
  for (int i = 0; i < 2; ++i) {
    const HostImage* hit = in.hit[i];
    if (hit && hit->texture && hit->slot != kInvalidBindlessSlot && hit->width == width && hit->height == height && hit->format == plume::RenderFormat::R16_FLOAT) {
      hit_used[i] = hit;
    }
  }

  // CITY-RT G6: the bounce light's hit distance image, at the pass's own size (any size above 0; the shader maps the pixel into it), RG16F, readable.
  const HostImage* bounce_used = nullptr;
  if (in.bounce_hit && in.bounce_hit->texture && in.bounce_hit->slot != kInvalidBindlessSlot && in.bounce_hit->width != 0 && in.bounce_hit->height != 0 &&
      in.bounce_hit->format == plume::RenderFormat::R16G16_FLOAT) {
    bounce_used = in.bounce_hit;
  }

  rr_guides::GuidesPush push = {};
  push.slots[0] = frame.slot[gbuffer::kPlaneNormal];
  push.slots[1] = frame.slot[gbuffer::kPlaneAlbedo];
  push.slots[2] = frame.slot[gbuffer::kPlaneMaterial];
  push.slots[3] = in.depth->slot;
  push.slots2[0] = have_mask ? in.mask->slot : rr_guides::kNoSlot;
  push.slots2[1] = have_transparency ? in.transparency->slot : rr_guides::kNoSlot;
  push.slots2[2] = hit_used[0] ? hit_used[0]->slot : rr_guides::kNoSlot;
  push.slots2[3] = hit_used[1] ? hit_used[1]->slot : rr_guides::kNoSlot;
  push.info[0] = width;
  push.info[1] = height;
  push.info[2] = frame.sample0;
  push.info[3] = bounce_used ? bounce_used->slot : rr_guides::kNoSlot;
  for (int i = 0; i < 3; ++i) {
    push.right[i] = in.right[i];
    push.up[i] = in.up[i];
    push.forward[i] = in.forward[i];
  }
  push.right[3] = in.tan_x;
  push.up[3] = in.tan_y;
  for (int i = 0; i < 4; ++i) {
    push.lin[i] = in.lin[i];
  }

  // The planes, the kept depth and the masks are read by a compute shader here: the compute stage's read state (as RecordUpscale does for the depth and the vectors).
  plume::RenderTextureBarrier reads[9];
  uint32_t reads_count = 0;
  for (uint32_t plane = 0; plane < gbuffer::kPlaneCount; ++plane) {
    reads[reads_count++] = plume::RenderTextureBarrier(gbuffer_->PlaneTexture(gbuffer::Plane(plane)), Layout::SHADER_READ);
  }
  reads[reads_count++] = plume::RenderTextureBarrier(in.depth->texture.get(), Layout::SHADER_READ);
  if (have_mask) {
    reads[reads_count++] = plume::RenderTextureBarrier(in.mask->texture.get(), Layout::SHADER_READ);
  }
  if (have_transparency) {
    reads[reads_count++] = plume::RenderTextureBarrier(in.transparency->texture.get(), Layout::SHADER_READ);
  }
  for (const HostImage* hit : hit_used) {
    if (hit) {
      reads[reads_count++] = plume::RenderTextureBarrier(hit->texture.get(), Layout::SHADER_READ);
    }
  }
  if (bounce_used) {
    reads[reads_count++] = plume::RenderTextureBarrier(bounce_used->texture.get(), Layout::SHADER_READ);
  }
  plume::RenderTextureBarrier writes[rr_guides::kGuideCount];
  for (size_t i = 0; i < rr_guides::kGuideCount; ++i) {
    writes[i] = plume::RenderTextureBarrier(images_[i].texture.get(), Layout::GENERAL);
    images_[i].layout = Layout::GENERAL;
  }

  pass_timing_->Mark(list, "rr guides");
  list->barriers(Stage::COMPUTE, reads, reads_count);
  list->barriers(Stage::COMPUTE, writes, uint32_t(rr_guides::kGuideCount));
  list->setComputePipelineLayout(guides_layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setComputeDescriptorSet(heap_->texture_set(), set);
  }
  list->setComputeDescriptorSet(set_.set.get(), kOutputSet);
  list->setPipeline(pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(rr_guides::Groups(width), rr_guides::Groups(height), 1);

  // The guides go to the read state the upscaler takes its inputs in (SHADER_READ at the compute stage: it tags them and takes them back itself); the planes go back where the frame's
  // graphics read them (nothing reads them again this frame, and the next frame's sidecar draws take them from SHADER_READ).
  plume::RenderTextureBarrier to_read[rr_guides::kGuideCount];
  for (size_t i = 0; i < rr_guides::kGuideCount; ++i) {
    to_read[i] = plume::RenderTextureBarrier(images_[i].texture.get(), Layout::SHADER_READ);
    images_[i].layout = Layout::SHADER_READ;
  }
  list->barriers(Stage::COMPUTE, to_read, uint32_t(rr_guides::kGuideCount));
  plume::RenderTextureBarrier planes_back[gbuffer::kPlaneCount];
  for (uint32_t plane = 0; plane < gbuffer::kPlaneCount; ++plane) {
    planes_back[plane] = plume::RenderTextureBarrier(gbuffer_->PlaneTexture(gbuffer::Plane(plane)), Layout::SHADER_READ);
  }
  list->barriers(Stage::GRAPHICS_AND_COMPUTE, planes_back, gbuffer::kPlaneCount);
  pass_timing_->Mark(list, "after rr guides");

  const auto fill = [](TemporalImage& image, const HostImage& held) {
    image = {held.texture.get(), held.width, held.height, held.format, held.flags};
  };
  fill(guides_.diffuse_albedo, images_[size_t(rr_guides::Guide::kDiffuseAlbedo)]);
  fill(guides_.specular_albedo, images_[size_t(rr_guides::Guide::kSpecularAlbedo)]);
  fill(guides_.normal_roughness, images_[size_t(rr_guides::Guide::kNormalRoughness)]);
  fill(guides_.specular_hit_distance, images_[size_t(rr_guides::Guide::kHitDistance)]);
  fill(guides_.responsivity, images_[size_t(rr_guides::Guide::kResponsivity)]);
  ready_ = true;
  ++passes_run_;
  for (const HostImage* hit : hit_used) {
    hits_used_ += hit ? 1u : 0u;
  }
  bounce_hits_used_ += bounce_used ? 1u : 0u;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first guide pass: frame {}, {}x{} render size, planes read at host sample {} of {}, the kept depth, {} and {} (reactive and transparency masks); five images {:.1f} MiB "
                "(diffuse albedo, specular albedo and normals + roughness R16G16B16A16_FLOAT; hit distance and responsivity R16_FLOAT); the temporal system hands them to the upscaler's Ray Reconstruction dispatch "
                "and the debug view mcla_gpu_dlss_rr_view draws them",
                kPrefix, in.frame, width, height, frame.sample0, frame.host_samples, have_mask ? "with" : "without", have_transparency ? "with" : "without", Mib(HeldBytes()));
  }
  return true;
}

bool RrGuides::RecordView(plume::RenderCommandList* list, HostImage& output) {
  if (!ready_ || view_ == rr_guides::View::kOff || !list || !output.texture || output.slot == kInvalidBindlessSlot) {
    return false;
  }
  plume::RenderPipeline* pipeline = Pipeline(2);
  if (!pipeline || !EnsureViewSet(output)) {
    return false;
  }
  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  const HostImage& diffuse = images_[size_t(rr_guides::Guide::kDiffuseAlbedo)];
  rr_guides::ViewPush push = {};
  push.slots[0] = diffuse.slot;
  push.slots[1] = images_[size_t(rr_guides::Guide::kNormalRoughness)].slot;
  push.slots[2] = images_[size_t(rr_guides::Guide::kSpecularAlbedo)].slot;
  push.slots[3] = images_[size_t(rr_guides::Guide::kHitDistance)].slot;
  push.slots2[0] = images_[size_t(rr_guides::Guide::kResponsivity)].slot;
  push.slots2[1] = uint32_t(view_);
  push.sizes[0] = diffuse.width;
  push.sizes[1] = diffuse.height;
  push.sizes[2] = output.width;
  push.sizes[3] = output.height;

  pass_timing_->Mark(list, "rr guides view");
  // The upscaler's output was left readable (its writes made visible); the view overwrites it as a storage image, then it is readable again for the rest of the frame.
  list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(output.texture.get(), Layout::GENERAL));
  list->setComputePipelineLayout(view_layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setComputeDescriptorSet(heap_->texture_set(), set);
  }
  list->setComputeDescriptorSet(view_set_.set.get(), kOutputSet);
  list->setPipeline(pipeline);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(rr_guides::Groups(output.width), rr_guides::Groups(output.height), 1);
  list->barriers(Stage::GRAPHICS_AND_COMPUTE, plume::RenderTextureBarrier(output.texture.get(), Layout::SHADER_READ));
  output.layout = Layout::SHADER_READ;
  pass_timing_->Mark(list, "after rr guides view");
  ++views_run_;
  return true;
}

// ---------------------------------------------------------------------------
// the images and their sets
// ---------------------------------------------------------------------------

bool RrGuides::EnsureImages(uint32_t width, uint32_t height) {
  std::array<const plume::RenderTexture*, rr_guides::kGuideCount> before{};
  for (size_t i = 0; i < rr_guides::kGuideCount; ++i) {
    before[i] = images_[i].texture.get();
  }
  for (size_t i = 0; i < rr_guides::kGuideCount; ++i) {
    const rr_guides::Guide guide = rr_guides::Guide(i);
    if (!passes_->EnsureImage(images_[i], width, height, FormatOf(guide), rr_guides::SpecOf(guide).name, kImageFlags)) {
      // EnsureImage released the one that failed and logged it; the others go too (the pass needs all five).
      ReleaseImages();
      return false;
    }
  }
  bool changed = false;
  for (size_t i = 0; i < rr_guides::kGuideCount; ++i) {
    changed = changed || images_[i].texture.get() != before[i];
  }
  if (changed) {
    RetireSets();  // a new texture is a new set for it (the old one goes after the lists that name it)
  }
  if (image_for_[0] != width || image_for_[1] != height) {
    image_for_[0] = width;
    image_for_[1] = height;
    REXLOG_INFO("{} guide images {}x{}: {:.1f} MiB ({} bytes a pixel)", kPrefix, width, height, Mib(HeldBytes()), rr_guides::BytesPerPixel());
  }
  return true;
}

bool RrGuides::EnsureSet() {
  std::array<const plume::RenderTexture*, rr_guides::kGuideCount> textures{};
  for (size_t i = 0; i < rr_guides::kGuideCount; ++i) {
    textures[i] = images_[i].texture.get();
  }
  if (set_.set && set_.textures == textures) {
    return true;
  }
  if (set_.set && frames_) {
    frames_->Retire(std::move(set_.set));
  }
  set_ = OutputSet{};
  set_.set = device_->CreateDescriptorSet(guides_builder_, uint32_t(rr_guides::kGuideCount), 0, "rr guides output set");
  if (!set_.set) {
    return false;
  }
  for (size_t i = 0; i < rr_guides::kGuideCount; ++i) {
    set_.set->setTexture(uint32_t(i), textures[i], plume::RenderTextureLayout::GENERAL, images_[i].view.get());
  }
  set_.textures = textures;
  return true;
}

bool RrGuides::EnsureViewSet(HostImage& output) {
  if (view_set_.set && view_output_ == output.texture.get()) {
    return true;
  }
  if (view_set_.set && frames_) {
    frames_->Retire(std::move(view_set_.set));
  }
  view_set_ = OutputSet{};
  view_output_ = nullptr;
  view_set_.set = device_->CreateDescriptorSet(view_builder_, 1, 0, "rr guides view set");
  if (!view_set_.set) {
    return false;
  }
  view_set_.set->setTexture(0, output.texture.get(), plume::RenderTextureLayout::GENERAL, output.view.get());
  view_output_ = output.texture.get();
  return true;
}

void RrGuides::RetireSets() {
  if (set_.set && frames_) {
    frames_->Retire(std::move(set_.set));
  }
  set_ = OutputSet{};
}

void RrGuides::ReleaseImages() {
  ready_ = false;
  if (!passes_) {
    return;
  }
  for (HostImage& image : images_) {
    passes_->ReleaseImage(image);
  }
  RetireSets();
  image_for_[0] = image_for_[1] = 0;
}

uint64_t RrGuides::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage& held : images_) {
    bytes += held.texture ? TextureBytes(held.width, held.height, 1, held.format) : 0;
  }
  return bytes;
}

}  // namespace mcla::gpu

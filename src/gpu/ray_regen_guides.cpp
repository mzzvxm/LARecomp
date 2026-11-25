// MCLA GPU renderer -- AMD FSR Ray Regeneration: the screen-space reflections' guide and signal pass. See ray_regen_guides.h.

#include "ray_regen_guides.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <rex/logging/log_category.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[ray regen guides]";
constexpr plume::RenderTextureFlags kImageFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

plume::RenderFormat FormatOf(rgd::Image image) {
  switch (rgd::SpecOf(image).format) {
    case rr::Format::kRgba16Float: return plume::RenderFormat::R16G16B16A16_FLOAT;
    case rr::Format::kR32Float: return plume::RenderFormat::R32_FLOAT;
    default: break;
  }
  return plume::RenderFormat::UNKNOWN;
}

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

}  // namespace

RayRegenRadiancePass::~RayRegenRadiancePass() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void RayRegenRadiancePass::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, PassTiming& pass_timing, rayregen::Site site) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  site_ = site;
  listed_ = false;
  failed_ = false;
  ready_to_list_ = false;
  if (ActiveBackend() != Backend::kD3D12) {
    return;  // AMD ships no Vulkan provider: nothing is made
  }
  ready_to_list_ = MakeLayoutAndShader(device, heap);
  if (!ready_to_list_) {
    REXLOG_ERROR("{} the layout or the shader could not be made for {}", kPrefix, PipelineName());
  }
}

bool RayRegenRadiancePass::MakeLayoutAndShader(Device& device, BindlessHeap& heap) {
  outputs_builder_.begin();
  for (size_t i = 0; i < rgd::kPassImageCount; ++i) {
    outputs_builder_.addReadWriteTexture(uint32_t(i));  // u0 .. u4 (space 5)
  }
  outputs_builder_.end();
  // The reflections' host pass layout (four texture sets at spaces 0 to 3, the sampler set at 4, the push block at (b0, space 4): ssr.cpp) with the five output images as one more set (space 5), for the
  // compute stage. 192 bytes of push constants: 48 of the 64 root dwords of Direct3D 12 (this pass is never made on Vulkan, whose guaranteed range is 128).
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap.texture_set_builder());
  }
  builder.addDescriptorSet(heap.sampler_set_builder());
  const uint32_t output_set = builder.addDescriptorSet(outputs_builder_);
  const uint32_t push_size = site_ == rayregen::Site::kBounce ? sizeof(rgd::BouncePush) : sizeof(rgd::Push);
  builder.addPushConstant(0, kPushSpace, push_size, plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (output_set != kOutputSet) {
    REXLOG_ERROR("{} the layout's output set is not at the index the shader declares", kPrefix);
    return false;
  }
  layout_ = device.CreatePipelineLayout(builder, PipelineName());
  if (!layout_ || !passes_) {
    return false;
  }
  const HostShaderBlob blob = site_ == rayregen::Site::kBounce ? RayRegenBounceShader() : RayRegenSsrShader();
  shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
  return shader_ != nullptr;
}

void RayRegenRadiancePass::Shutdown() {
  // The host builder's functions name this object's layout and shader: stop it first (the other owners do the same; stopping twice is safe).
  if (passes_) {
    passes_->precache().Stop();
  }
  if (passes_run_ != 0) {
    REXLOG_INFO("{} {} guide and signal pass(es) recorded", kPrefix, passes_run_);
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (RrGuides::Shutdown).
  for (HostImage& image : images_) {
    image = HostImage{};
  }
  set_.reset();
  set_textures_ = {};
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  ready_to_list_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipeline
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> RayRegenRadiancePass::MakePipeline() {
  if (!layout_ || !shader_ || !device_) {
    return nullptr;
  }
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), 8, 8, 1), PipelineName());
}

const char* RayRegenRadiancePass::PipelineName() const {
  return site_ == rayregen::Site::kBounce ? "ray regen bounce guides" : "ray regen ssr guides";
}

bool RayRegenRadiancePass::PipelineReady() {
  if (pipeline_) {
    return true;
  }
  if (failed_ || !ready_to_list_ || !passes_) {
    return false;
  }
  HostPrecache& precache = passes_->precache();
  const uint64_t key = HostPrecache::Key(site_ == rayregen::Site::kBounce ? HostPrecache::kGiPixel : HostPrecache::kScreenReflections, kVariant);
  if (!listed_) {
    listed_ = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({key, PipelineName(), [this] { return MakePipeline(); }});
    precache.AddLive("AMD FSR Ray Regeneration guides", std::move(items));
    REXLOG_INFO("{} making the {} pipeline in the background", kPrefix, PipelineName());
  }
  if (!precache.Ready(key)) {
    return false;
  }
  pipeline_ = precache.Acquire(key, PipelineName(), [this] { return MakePipeline(); });
  if (!pipeline_) {
    failed_ = true;
    REXLOG_ERROR("{} the {} pipeline could not be made", kPrefix, PipelineName());
  }
  return pipeline_ != nullptr;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

bool RayRegenRadiancePass::Record(plume::RenderCommandList* list, const rgd::Push& push, HostImage& trace, uint32_t width, uint32_t height) {
  return RecordGuides(list, &push, sizeof(push), {&trace}, width, height);
}

bool RayRegenRadiancePass::RecordBounce(plume::RenderCommandList* list, const rgd::BouncePush& push, HostImage& signal, HostImage& depth, HostImage& hit) {
  return RecordGuides(list, &push, sizeof(push), {&signal, &depth, &hit}, push.width, push.height);
}

bool RayRegenRadiancePass::RecordGuides(plume::RenderCommandList* list, const void* push, uint32_t push_size,
                                      std::initializer_list<HostImage*> sources, uint32_t width, uint32_t height) {
  if (!ready_to_list_ || !list || !heap_ || !passes_ || !pass_timing_ || width == 0 || height == 0 || !PipelineReady()) {
    return false;
  }
  if (!EnsureImages(width, height) || !EnsureSet()) {
    return false;
  }
  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;

  for (HostImage* source : sources) {
    if (!source || !source->texture) return false;
    list->barriers(Stage::GRAPHICS_AND_COMPUTE, plume::RenderTextureBarrier(source->texture.get(), Layout::SHADER_READ));
    source->layout = Layout::SHADER_READ;
  }
  plume::RenderTextureBarrier writes[rgd::kPassImageCount];
  for (size_t i = 0; i < rgd::kPassImageCount; ++i) {
    writes[i] = plume::RenderTextureBarrier(images_[i].texture.get(), Layout::GENERAL);
    images_[i].layout = Layout::GENERAL;
  }
  const bool bounce = site_ == rayregen::Site::kBounce;
  pass_timing_->Mark(list, bounce ? "ray regen bounce guides" : "ray regen guides");
  list->barriers(Stage::COMPUTE, writes, uint32_t(rgd::kPassImageCount));
  list->setComputePipelineLayout(layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setComputeDescriptorSet(heap_->texture_set(), set);
  }
  list->setComputeDescriptorSet(heap_->sampler_set(), kSamplerSet);
  list->setComputeDescriptorSet(set_.get(), kOutputSet);
  list->setPipeline(pipeline_.get());
  list->setComputePushConstants(0, push, 0, push_size);
  list->dispatch(rgd::Groups(width), rgd::Groups(height), 1);
  // The five go to the read state the dispatch declares its inputs in (SHADER_READ at the compute stage: the runtime takes them back to it).
  plume::RenderTextureBarrier to_read[rgd::kPassImageCount];
  for (size_t i = 0; i < rgd::kPassImageCount; ++i) {
    to_read[i] = plume::RenderTextureBarrier(images_[i].texture.get(), Layout::SHADER_READ);
    images_[i].layout = Layout::SHADER_READ;
  }
  list->barriers(Stage::COMPUTE, to_read, uint32_t(rgd::kPassImageCount));
  pass_timing_->Mark(list, bounce ? "after ray regen bounce guides" : "after ray regen guides");
  ++passes_run_;
  if (!first_logged_) {
    first_logged_ = true;
    REXLOG_INFO("{} the first guide and signal pass: {}x{} ({}), six images {:.1f} MiB", kPrefix, width, height, PipelineName(), Mib(HeldBytes()));
  }
  return true;
}

void RayRegenRadiancePass::PrepareDenoised(plume::RenderCommandList* list) {
  HostImage& out = images_[size_t(rgd::Image::kDenoised)];
  if (!list || !out.texture) {
    return;
  }
  list->barriers(plume::RenderBarrierStage::COMPUTE, plume::RenderTextureBarrier(out.texture.get(), plume::RenderTextureLayout::GENERAL));
  out.layout = plume::RenderTextureLayout::GENERAL;
}

void RayRegenRadiancePass::ReadDenoised(plume::RenderCommandList* list) {
  HostImage& out = images_[size_t(rgd::Image::kDenoised)];
  if (!list || !out.texture) {
    return;
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, plume::RenderTextureBarrier(out.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  out.layout = plume::RenderTextureLayout::SHADER_READ;
}

TemporalImage RayRegenRadiancePass::Tagged(rgd::Image which) const {
  const HostImage& held = images_[size_t(which)];
  return {held.texture.get(), held.width, held.height, held.format, held.flags};
}

void RayRegenRadiancePass::OnFrame(bool recorded) {
  if (recorded) {
    idle_frames_ = 0;
  } else if (idle_frames_ < rgd::kReleaseAfterFrames) {
    if (++idle_frames_ == rgd::kReleaseAfterFrames) {
      ReleaseImages();  // not Ray Regeneration's for a while: the video memory goes back
    }
  }
}

// ---------------------------------------------------------------------------
// the images and their set
// ---------------------------------------------------------------------------

bool RayRegenRadiancePass::EnsureImages(uint32_t width, uint32_t height) {
  std::array<const plume::RenderTexture*, rgd::kPassImageCount> before{};
  for (size_t i = 0; i < rgd::kPassImageCount; ++i) {
    before[i] = images_[i].texture.get();
  }
  for (size_t i = 0; i < rgd::kImageCount; ++i) {
    const rgd::Image which = rgd::Image(i);
    if (!passes_->EnsureImage(images_[i], width, height, FormatOf(which), rgd::SpecOf(which).name, kImageFlags)) {
      ReleaseImages();   // EnsureImage released and logged the one that failed; the others go too (the pass needs all six)
      return false;
    }
  }
  bool changed = false;
  for (size_t i = 0; i < rgd::kPassImageCount; ++i) {
    changed = changed || images_[i].texture.get() != before[i];
  }
  if (changed) {
    RetireSet();  // a new texture is a new set for it (the old one goes after the lists that name it)
  }
  if (image_for_[0] != width || image_for_[1] != height) {
    image_for_[0] = width;
    image_for_[1] = height;
    REXLOG_INFO("{} images {}x{}: {:.1f} MiB ({} bytes a texel)", kPrefix, width, height, Mib(HeldBytes()), rgd::BytesPerTexel());
  }
  return true;
}

bool RayRegenRadiancePass::EnsureSet() {
  if (!device_) return false;
  std::array<const plume::RenderTexture*, rgd::kPassImageCount> textures{};
  for (size_t i = 0; i < rgd::kPassImageCount; ++i) {
    textures[i] = images_[i].texture.get();
  }
  if (set_ && set_textures_ == textures) {
    return true;
  }
  RetireSet();
  set_ = device_->CreateDescriptorSet(outputs_builder_, uint32_t(rgd::kPassImageCount), 0, "ray regen ssr output set");
  if (!set_) {
    return false;
  }
  for (size_t i = 0; i < rgd::kPassImageCount; ++i) {
    set_->setTexture(uint32_t(i), textures[i], plume::RenderTextureLayout::GENERAL, images_[i].view.get());
  }
  set_textures_ = textures;
  return true;
}

void RayRegenRadiancePass::RetireSet() {
  if (set_ && frames_) {
    frames_->Retire(std::move(set_));
  }
  set_.reset();
  set_textures_ = {};
}

void RayRegenRadiancePass::ReleaseImages() {
  if (!passes_) {
    return;
  }
  for (HostImage& image : images_) {
    passes_->ReleaseImage(image);
  }
  RetireSet();
  image_for_[0] = image_for_[1] = 0;
}

uint64_t RayRegenRadiancePass::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage& held : images_) {
    bytes += held.texture ? TextureBytes(held.width, held.height, 1, held.format) : 0;
  }
  return bytes;
}

}  // namespace mcla::gpu

#include "rtao_compose.h"

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_shaders.h"
#include "rtao_denoise_rules.h"

namespace mcla::gpu {
namespace {
using Layout = plume::RenderTextureLayout;
namespace Stage = plume::RenderBarrierStage;
constexpr uint32_t kNoSlot = rtao_denoise::kNoSlot;
constexpr uint32_t kOutputSet = 4;
constexpr uint32_t kGroup = 8;
constexpr const char* kLabel = "rtao compose";
constexpr plume::RenderTextureFlags kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

struct Push {
  uint32_t signal, depth_guide, depth_full, screen;
  uint32_t bounce, raw, id_share, stencil;
  uint32_t traced[2], size[2];
  uint32_t render[2], stencil_params, car_term;
  float power, floor;
  uint32_t flags, view;
};
static_assert(sizeof(Push) == 80);

bool Valid(const HostImage* image, const uint32_t (&size)[2]) {
  return image && image->texture && image->slot < kBindlessTextureCount && image->width == size[0] && image->height == size[1];
}

uint64_t PipelineKey(bool bounce) {
  // CHECK: R1 adds the named kRtAo owner at the frozen value 36.
  return HostPrecache::Key(static_cast<HostPrecache::Owner>(36), bounce ? 3 : 2);
}
}  // namespace

void RtAoCompose::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);
  output_builder_.end();
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, false);
  for (uint32_t set = 0; set < kOutputSet; ++set) builder.addDescriptorSet(heap.texture_set_builder());
  builder.addDescriptorSet(output_builder_);
  builder.addPushConstant(0, 4, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "rtao compose layout");
  if (!layout_) return;
  for (uint32_t variant = 0; variant < shaders_.size(); ++variant) {
    const HostShaderBlob blob = RtAoComposeShader(variant != 0);
    shaders_[variant] = passes.CreateShader(blob.data, blob.size, blob.name);
  }
}

void RtAoCompose::Shutdown() {
  if (passes_) passes_->precache().Stop();
  output_.reset();
  image_ = {};
  pipelines_ = {};
  shaders_ = {};
  layout_.reset();
  listed_ = {};
  failed_ = {};
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
}

std::unique_ptr<plume::RenderPipeline> RtAoCompose::MakePipeline(bool bounce) const {
  return device_->CreateComputePipeline({layout_.get(), shaders_[bounce].get(), kGroup, kGroup, 1}, kLabel);
}

plume::RenderPipeline* RtAoCompose::Pipeline(bool bounce) const {
  if (pipelines_[bounce]) return pipelines_[bounce].get();
  if (!layout_ || !shaders_[bounce] || failed_[bounce]) return nullptr;
  auto& precache = passes_->precache();
  const uint64_t key = PipelineKey(bounce);
  const auto make = [this, bounce] { return MakePipeline(bounce); };
  if (!listed_[bounce]) {
    listed_[bounce] = true;
    precache.AddLive(kLabel, {{key, kLabel, make}});
  }
  if (!precache.Ready(key)) return nullptr;
  pipelines_[bounce] = precache.Acquire(key, kLabel, make);
  failed_[bounce] = !pipelines_[bounce];
  return pipelines_[bounce].get();
}

bool RtAoCompose::available() const {
  const bool plain = Pipeline(false) != nullptr;
  const bool bounce = Pipeline(true) != nullptr;
  return plain && bounce;
}

bool RtAoCompose::EnsureImage(uint32_t width, uint32_t height, bool bounce) {
  if (!passes_ || width == 0 || height == 0) return false;
  const auto format = bounce ? plume::RenderFormat::R16G16B16A16_FLOAT : plume::RenderFormat::R16_FLOAT;
  if (image_.width != width || image_.height != height || image_.format != format) ReleaseImage();
  if (!passes_->EnsureImage(image_, width, height, format, kLabel, kFlags)) return false;
  if (!output_) {
    output_ = device_->CreateDescriptorSet(output_builder_, 1, 0, "rtao compose output");
    if (!output_) {
      ReleaseImage();
      return false;
    }
    output_->setTexture(0, image_.texture.get(), Layout::GENERAL, image_.view.get());
  }
  return true;
}

uint32_t RtAoCompose::Record(const Frame& in) { return Record(in, {}); }

uint32_t RtAoCompose::Record(const Frame& in, const Guides& guides) {
  const bool bounce = in.bounce_slot != kNoSlot;
  if (!in.list || !Valid(in.filtered, in.traced) || !Valid(in.depth_guide, in.traced) || !Valid(in.depth_full, in.plan_ao)) return kNoSlot;
  auto* pipeline = Pipeline(bounce);
  if (!pipeline || !EnsureImage(in.plan_ao[0], in.plan_ao[1], bounce)) return kNoSlot;

  const bool scalar_signal = in.filtered->format == plume::RenderFormat::R16_FLOAT;
  const bool needs_raw = scalar_signal || in.view == 1 || in.view == 5;
  const uint32_t raw = needs_raw && guides.raw != in.filtered && Valid(guides.raw, in.traced) ? guides.raw->slot : kNoSlot;
  const bool history = !scalar_signal && guides.raw != in.filtered;
  const uint32_t id_share = Valid(guides.id_share, in.traced) ? guides.id_share->slot : kNoSlot;
  const uint32_t stencil = guides.render[0] && guides.render[1] && guides.stencil_slot < kBindlessTextureCount ? guides.stencil_slot : kNoSlot;
  const Push push{
      in.filtered->slot, in.depth_guide->slot, in.depth_full->slot, in.screen_term_slot,
      in.bounce_slot, raw, id_share, stencil,
      {in.traced[0], in.traced[1]}, {in.plan_ao[0], in.plan_ao[1]},
      {guides.render[0], guides.render[1]}, (guides.stencil_sample & 0xFFu) | (guides.stencil_ms ? 0x100u : 0u), guides.car_term_slot,
      in.power, in.floor_, uint32_t(in.blend_screen) | (history ? 2u : 0u), uint32_t(in.view),
  };
  // CHECK: validate the term's typed stores and the screen/bounce texture
  // handoff at the AO trigger on D3D12 and Vulkan, plus the thin pole and
  // cockpit coverage pictures on both APIs.
  plume::RenderTextureBarrier reads[] = {{in.filtered->texture.get(), Layout::SHADER_READ},
                                         {in.depth_guide->texture.get(), Layout::SHADER_READ},
                                         {in.depth_full->texture.get(), Layout::SHADER_READ}};
  in.list->barriers(Stage::GRAPHICS_AND_COMPUTE, reads, 3);
  in.list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(image_.texture.get(), Layout::GENERAL));
  in.list->setComputePipelineLayout(layout_.get());
  for (uint32_t set = 0; set < kOutputSet; ++set) in.list->setComputeDescriptorSet(heap_->texture_set(), set);
  in.list->setComputeDescriptorSet(output_.get(), kOutputSet);
  in.list->setPipeline(pipeline);
  in.list->setComputePushConstants(0, &push, 0, sizeof(push));
  in.list->dispatch((in.plan_ao[0] + kGroup - 1) / kGroup, (in.plan_ao[1] + kGroup - 1) / kGroup, 1);
  in.list->barriers(Stage::GRAPHICS_AND_COMPUTE, plume::RenderTextureBarrier(image_.texture.get(), Layout::SHADER_READ));
  image_.layout = Layout::SHADER_READ;
  return image_.slot;
}

void RtAoCompose::ReleaseImage() {
  if (output_) frames_->Retire(std::move(output_));
  if (passes_) passes_->ReleaseImage(image_);
}

uint64_t RtAoCompose::HeldBytes() const {
  const uint64_t stride = image_.format == plume::RenderFormat::R16_FLOAT ? 2 : 8;
  return image_.texture ? uint64_t(image_.width) * image_.height * stride : 0;
}
}  // namespace mcla::gpu

#include "ray_regen_ao.h"

#include "pbr_screen_switch.h"
#include "gfx_preset_apply.h"

#include <algorithm>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_shaders.h"

namespace mcla::gpu {
namespace {
namespace rgd = rayregen::guides;
using Layout = plume::RenderTextureLayout;
namespace Stage = plume::RenderBarrierStage;
constexpr uint32_t kOutputSet = 1;
constexpr uint32_t kGroup = 8;
constexpr const char* kName = "ray regen rtao guides";
constexpr auto kImageFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

struct Push {
  uint32_t inputs[4];
  uint32_t gbuffer[3], table;
  uint32_t traced[2], render[2];
  float lens[4];
  float motion[12];
  float jitter[2], sign;
  uint32_t ids;
};
static_assert(sizeof(Push) == 128);

uint64_t PipelineKey() {
  return HostPrecache::Key(HostPrecache::Owner::kRayRegenAo, 0);
}

plume::RenderFormat FormatOf(rgd::Image image) {
  switch (rgd::AoSpecOf(image).format) {
    case rr::Format::kR16Float: return plume::RenderFormat::R16_FLOAT;
    case rr::Format::kR32Float: return plume::RenderFormat::R32_FLOAT;
    default: return plume::RenderFormat::R16G16B16A16_FLOAT;
  }
}
}  // namespace

void RayRegenAoPass::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes) {
  if (ActiveBackend() != Backend::kD3D12) return;
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  output_builder_.begin();
  for (uint32_t i = 0; i < rgd::kPassImageCount; ++i) output_builder_.addReadWriteTexture(i);
  output_builder_.end();
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, false);
  builder.addDescriptorSet(heap.texture_set_builder());
  builder.addDescriptorSet(output_builder_);
  builder.addPushConstant(0, 4, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, kName);
  if (!layout_) return;
  const HostShaderBlob blob = RayRegenAoShader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
}

void RayRegenAoPass::Shutdown() {
  if (passes_) passes_->precache().Stop();
  output_.reset();
  images_ = {};
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  listed_ = failed_ = false;
  idle_frames_ = 0;
  recorded_on_ = nullptr;
}

plume::RenderPipeline* RayRegenAoPass::Pipeline() {
  if (pipeline_) return pipeline_.get();
  if (!shader_ || failed_) return nullptr;
  auto& precache = passes_->precache();
  const auto make = [this] {
    return device_->CreateComputePipeline({layout_.get(), shader_.get(), kGroup, kGroup, 1}, kName);
  };
  const uint64_t key = PipelineKey();
  if (!listed_) {
    listed_ = true;
    precache.AddLive(kName, {{key, kName, make}});
  }
  if (!precache.Ready(key)) return nullptr;
  pipeline_ = precache.Acquire(key, kName, make);
  failed_ = !pipeline_;
  return pipeline_.get();
}

bool RayRegenAoPass::EnsureImages(uint32_t width, uint32_t height) {
  if (images_[0].width != width || images_[0].height != height) ReleaseImages();
  for (size_t i = 0; i < images_.size(); ++i) {
    if (!passes_->EnsureImage(images_[i], width, height, FormatOf(Image(i)), rgd::AoSpecOf(Image(i)).name, kImageFlags)) {
      ReleaseImages();
      return false;
    }
  }
  if (output_) return true;
  output_ = device_->CreateDescriptorSet(output_builder_, uint32_t(rgd::kPassImageCount), 0, "ray regen AO output");
  if (!output_) {
    ReleaseImages();
    return false;
  }
  for (uint32_t i = 0; i < rgd::kPassImageCount; ++i) {
    output_->setTexture(i, images_[i].texture.get(), Layout::GENERAL, images_[i].view.get());
  }
  return true;
}

bool RayRegenAoPass::Record(const Frame& in) {
  recorded_on_ = nullptr;
  if (!in.list || !in.signal || !in.render[0] || !in.render[1]) return false;
  const uint32_t width = in.signal->width;
  const uint32_t height = in.signal->height;
  const HostImage* inputs[] = {in.signal, in.depth, in.normal, in.id_share};
  for (const HostImage* image : inputs) {
    if (!image || !image->texture || image->slot >= kBindlessTextureCount || image->width != width || image->height != height) return false;
  }
  auto* pipeline = Pipeline();
  if (!pipeline || !EnsureImages(width, height)) return false;
  const uint32_t table = in.vehicle_table_slot < kBindlessTextureCount ? in.vehicle_table_slot : 0u;
  Push push{{in.signal->slot, in.depth->slot, in.normal->slot, in.id_share->slot},
            {in.gbuffer[0], in.gbuffer[1], in.gbuffer[2]}, table,
            {width, height}, {in.render[0], in.render[1]}, {in.tan_x, in.tan_y, in.prev_tan_x, in.prev_tan_y}, {},
            {in.jitter[0], in.jitter[1]}, rayregen::kLinearDepthPositiveAhead ? 1.0f : -1.0f, (table ? in.vehicle_ids : 0u) | (pbr_screen::Enabled() ? 1u << 31 : 0u)};
  std::copy_n(in.motion, 12, push.motion);
  // CHECK: the AO/SSR dispatch sites and the G-buffer's compute-read
  // handoff need runtime validation on a Radeon RX 9000.
  for (uint32_t i = 0; i < rgd::kPassImageCount; ++i) {
    in.list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(images_[i].texture.get(), Layout::GENERAL));
  }
  in.list->setComputePipelineLayout(layout_.get());
  in.list->setComputeDescriptorSet(heap_->texture_set(), 0);
  in.list->setComputeDescriptorSet(output_.get(), kOutputSet);
  in.list->setPipeline(pipeline);
  in.list->setComputePushConstants(0, &push, 0, sizeof(push));
  in.list->dispatch((width + kGroup - 1) / kGroup, (height + kGroup - 1) / kGroup, 1);
  for (uint32_t i = 0; i < rgd::kPassImageCount; ++i) {
    in.list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(images_[i].texture.get(), Layout::SHADER_READ));
    images_[i].layout = Layout::SHADER_READ;
  }
  recorded_on_ = in.list;
  idle_frames_ = 0;
  return true;
}

const HostImage* RayRegenAoPass::Dispatch(plume::RenderCommandList* list, rayregen::RayRegen& runtime, rayregen::DispatchFrame frame) {
  constexpr auto signal = rayregen::Signal::kAmbientOcclusion;
  constexpr uint32_t flags = rayregen::FlagOf(signal);
  if (!list || list != recorded_on_ || !rayregen::kConsumers[size_t(rr::Consumer::kRtAo)].signal_built) return nullptr;
  const auto& input = images_[size_t(Image::kSignal)];
  if (!runtime.Ready(flags, input.width, input.height)) return nullptr;
  frame.render_size[0] = input.width;
  frame.render_size[1] = input.height;
  frame.linear_depth = Tagged(Image::kDepth);
  frame.motion = Tagged(Image::kMotion);
  frame.normals = Tagged(Image::kNormals);
  frame.specular_albedo = Tagged(Image::kAlbedo);
  frame.dispatch_signals = flags;
  frame.signals = {};
  frame.signals[size_t(signal)] = {Tagged(Image::kSignal), Tagged(Image::kDenoised)};
  HostImage& result = images_[size_t(Image::kDenoised)];
  list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(result.texture.get(), Layout::GENERAL));
  result.layout = Layout::GENERAL;
  if (!runtime.Dispatch(list, frame)) return nullptr;
  list->barriers(Stage::COMPUTE, plume::RenderTextureBarrier(result.texture.get(), Layout::SHADER_READ));
  result.layout = Layout::SHADER_READ;
  return &result;
}

TemporalImage RayRegenAoPass::Tagged(Image image) const {
  const auto& in = images_[size_t(image)];
  return {in.texture.get(), in.width, in.height, in.format, in.flags};
}

void RayRegenAoPass::OnPresent() {
  if (!recorded_on_ && images_[0].texture && !gfx_preset::retain_scene_resources.load(std::memory_order_relaxed) && ++idle_frames_ >= rgd::kReleaseAfterFrames) ReleaseImages();
  recorded_on_ = nullptr;
}

void RayRegenAoPass::ReleaseImages() {
  if (output_) frames_->Retire(std::move(output_));
  for (HostImage& image : images_) passes_->ReleaseImage(image);
}

uint64_t RayRegenAoPass::HeldBytes() const {
  uint64_t bytes = 0;
  for (size_t i = 0; i < images_.size(); ++i) {
    if (images_[i].texture) bytes += uint64_t(images_[i].width) * images_[i].height * rgd::AoSpecOf(Image(i)).bytes_per_texel;
  }
  return bytes;
}
}  // namespace mcla::gpu

#include "ssr_classify.h"

#include "descriptors.h"
#include "device.h"
#include "host_shaders.h"

namespace mcla::gpu {

namespace {
constexpr uint32_t kArgumentBytes = 3u * sizeof(uint32_t);
constexpr auto kStorage = plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS;
constexpr uint64_t PipelineKey(uint32_t variant) {
  return HostPrecache::Key(HostPrecache::kScreenReflections, 0x120u + variant);
}
constexpr const char* PipelineName(uint32_t variant) { return variant == 0 ? "ssr classify" : "ssr classify reprojection"; }
constexpr uint32_t kFeedback = 0;
constexpr uint32_t kReproject = 1;
}

void ReflectionClassifier::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes,
                                      const plume::RenderDescriptorSetBuilder& handoff) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);
  output_builder_.addReadWriteByteAddressBuffer(1);
  output_builder_.addReadWriteByteAddressBuffer(2);
  output_builder_.addReadWriteTexture(3);
  output_builder_.end();
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, false);
  for (uint32_t i = 0; i < 4; ++i) builder.addDescriptorSet(heap.texture_set_builder());
  builder.addDescriptorSet(heap.sampler_set_builder());
  builder.addDescriptorSet(handoff);
  builder.addDescriptorSet(output_builder_);
  builder.addPushConstant(0, 4, sizeof(ssr::Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "ssr classifier layout");
  for (uint32_t i = 0; i < shaders_.size(); ++i) {
    const auto blob = SsrClassifyShader(i == kReproject);
    shaders_[i] = passes.CreateShader(blob.data, blob.size, blob.name);
  }
}

std::unique_ptr<plume::RenderPipeline> ReflectionClassifier::MakePipeline(uint32_t variant) {
  if (!device_ || !layout_ || !shaders_[variant]) return nullptr;
  return device_->CreateComputePipeline(
      plume::RenderComputePipelineDesc(layout_.get(), shaders_[variant].get(), 4, 4, 1), PipelineName(variant));
}

void ReflectionClassifier::Shutdown() {
  for (auto& set : sets_) set.reset();
  for (auto& tile : tiles_) tile = {};
  plan_ = {};
  indirect_.reset();
  unused_args_.reset();
  tile_list_.reset();
  for (auto& pipeline : pipelines_) pipeline.reset();
  for (auto& shader : shaders_) shader.reset();
  listed_ = failed_ = false;
  layout_.reset();
  list_bytes_ = 0;
  width_ = height_ = write_ = 0;
  valid_ = false;
}

void ReflectionClassifier::Release() {
  if (!frames_ || !passes_ || width_ == 0 || height_ == 0) return;
  for (auto& set : sets_) frames_->Retire(std::move(set));
  for (auto& tile : tiles_) passes_->ReleaseImage(tile);
  passes_->ReleaseImage(plan_);
  frames_->Retire(std::move(tile_list_));
  list_bytes_ = 0;
  width_ = height_ = write_ = 0;
  valid_ = false;
}

bool ReflectionClassifier::Prepare(uint32_t width, uint32_t height, bool rt) {
  if (!device_ || !frames_ || !passes_ || !layout_ || !shaders_[kFeedback] || !shaders_[kReproject] || failed_) return false;
  auto& precache = passes_->precache();
  if (!listed_) {
    listed_ = true;
    precache.AddLive("surface reflection classification", {
        {PipelineKey(kFeedback), PipelineName(kFeedback), [this] { return MakePipeline(kFeedback); }},
        {PipelineKey(kReproject), PipelineName(kReproject), [this] { return MakePipeline(kReproject); }}});
  }
  for (uint32_t i = 0; i < pipelines_.size(); ++i) {
    if (pipelines_[i]) continue;
    if (!precache.Ready(PipelineKey(i))) return false;
    pipelines_[i] = precache.Acquire(PipelineKey(i), PipelineName(i), [this, i] { return MakePipeline(i); });
    if (!pipelines_[i]) {
      failed_ = true;
      return false;
    }
  }
  const uint32_t cols = (width + 7u) / 8u;
  const uint32_t rows = (height + 7u) / 8u;
  if (width_ != cols || height_ != rows) {
    Release();
    width_ = cols;
    height_ = rows;
  }
  using Flag = plume::RenderTextureFlag::Bits;
  for (auto& tile : tiles_) {
    if (!passes_->EnsureImage(tile, cols, rows, plume::RenderFormat::R16G16B16A16_FLOAT, "ssr tile feedback",
                              Flag::STORAGE | Flag::UNORDERED_ACCESS)) return false;
  }
  if (!passes_->EnsureImage(plan_, cols, rows, plume::RenderFormat::R16G16B16A16_FLOAT, "ssr tile plan",
                            Flag::STORAGE | Flag::UNORDERED_ACCESS)) return false;
  if (rt && !indirect_) {
    indirect_ = backend::CreateIndirectCompute(device_->device());
    if (!indirect_) return false;
  }
  const uint64_t bytes = (rt ? uint64_t(cols) * rows + 1u : 1u) * sizeof(uint32_t);
  if (list_bytes_ < bytes) {
    auto buffer = device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(bytes, kStorage), "ssr active tiles");
    if (!buffer) return false;
    frames_->Retire(std::move(tile_list_));
    tile_list_ = std::move(buffer);
    list_bytes_ = bytes;
  }
  if (!rt && !unused_args_) {
    unused_args_ = device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(kArgumentBytes, kStorage), "ssr unused dispatch");
    if (!unused_args_) return false;
  }
  auto& set = sets_[frames_->slot()];
  if (!set) set = device_->CreateDescriptorSet(output_builder_, 4, 0, "ssr classifier output");
  HostImage& image = tiles_[write_];
  auto* args = rt && indirect_ ? indirect_->arguments() : unused_args_.get();
  if (!set || !image.texture || !plan_.texture || !tile_list_ || !args) return false;
  set->setTexture(0, image.texture.get(), plume::RenderTextureLayout::GENERAL, image.view.get());
  set->setBuffer(1, tile_list_.get(), list_bytes_);
  set->setBuffer(2, args, kArgumentBytes);
  set->setTexture(3, plan_.texture.get(), plume::RenderTextureLayout::GENERAL, plan_.view.get());
  return true;
}

void ReflectionClassifier::Bind(plume::RenderCommandList& list, plume::RenderDescriptorSet& handoff, uint32_t variant) {
  if (!heap_ || !frames_) return;
  list.setComputePipelineLayout(layout_.get());
  for (uint32_t i = 0; i < 4; ++i) list.setComputeDescriptorSet(heap_->texture_set(), i);
  list.setComputeDescriptorSet(heap_->sampler_set(), 4);
  list.setComputeDescriptorSet(&handoff, 5);
  list.setComputeDescriptorSet(sets_[frames_->slot()].get(), 6);
  list.setPipeline(pipelines_[variant].get());
}

void ReflectionClassifier::Reproject(plume::RenderCommandList& list, plume::RenderDescriptorSet& handoff, const ssr::Push& push) {
  if (!frames_ || !heap_ || !layout_ || !pipelines_[kReproject] || !plan_.texture || !sets_[frames_->slot()]) return;
  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  list.barriers(Stage::COMPUTE, plume::RenderTextureBarrier(plan_.texture.get(), Layout::GENERAL));
  Bind(list, handoff, kReproject);
  list.setComputePushConstants(0, &push, 0, sizeof(push));
  list.dispatch((width_ + 3u) / 4u, (height_ + 3u) / 4u, 1);
  list.barriers(Stage::GRAPHICS_AND_COMPUTE, plume::RenderTextureBarrier(plan_.texture.get(), Layout::SHADER_READ));
  plan_.layout = Layout::SHADER_READ;
}

void ReflectionClassifier::Record(plume::RenderCommandList& list, plume::RenderDescriptorSet& handoff, const ssr::Push& push, bool rt) {
  if (!frames_ || !heap_ || !layout_ || !pipelines_[kFeedback]) return;
  const auto& set = sets_[frames_->slot()];
  HostImage& image = tiles_[write_];
  auto* args = rt && indirect_ ? indirect_->arguments() : unused_args_.get();
  if (!set || !image.texture || !tile_list_ || !args) return;
  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  constexpr auto rw = plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE;
  const plume::RenderBufferBarrier buffers[] = {{tile_list_.get(), rw}, {args, rw}};
  const plume::RenderTextureBarrier output(image.texture.get(), Layout::GENERAL);
  list.barriers(Stage::COMPUTE, buffers, 2, &output, 1);
  Bind(list, handoff, kFeedback);
  if (rt) {
    ssr::Push reset{};
    list.setComputePushConstants(0, &reset, 0, sizeof(reset));
    list.dispatch(1, 1, 1);
    list.barriers(Stage::COMPUTE, buffers, 2);
  }
  list.setComputePushConstants(0, &push, 0, sizeof(push));
  list.dispatch(width_, height_, 1);
  // CHECK: validate ping-pong and the UAV-to-indirect handoff on both APIs.
  const plume::RenderTextureBarrier read(image.texture.get(), Layout::SHADER_READ);
  const plume::RenderBufferBarrier active(tile_list_.get(), plume::RenderBufferAccess::READ);
  list.barriers(Stage::GRAPHICS_AND_COMPUTE, rt ? &active : nullptr, rt ? 1u : 0u, &read, 1);
  image.layout = Layout::SHADER_READ;
  write_ ^= 1u;
  valid_ = true;
}

}  // namespace mcla::gpu

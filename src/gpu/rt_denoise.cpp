// MCLA GPU renderer -- RtDenoise, the shared host. See rt_denoise.h and rt_denoise_rules.h.

#include "rt_denoise.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_precache.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[rt denoise]";
// A spell of frames without a stage gives the images back (2 s at 60).
constexpr uint32_t kReleaseAfterFrames = 120;
constexpr plume::RenderTextureFlags kImageFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;

using Layout = plume::RenderTextureLayout;
namespace Stage = plume::RenderBarrierStage;

}  // namespace

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void RtDenoise::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, uint32_t instance, const char* name, const Blob& spatial, const Blob& temporal,
                           rt_denoise::Format format) {
  format_ = format;
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  instance_ = instance & 15u;
  name_ = name ? name : "";
  ready_ = false;
  listed_.fill(false);
  failed_.fill(false);

  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);   // u0: the result image
  output_builder_.end();
  // The compute layout every consumer shares (the effects' own: the bindless texture set four times, the samplers, this class's output set, the push block at (b0, space 4)).
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < rt_denoise::kTextureSets; ++set) {
    builder.addDescriptorSet(heap_->texture_set_builder());
  }
  const uint32_t sampler_set = builder.addDescriptorSet(heap_->sampler_set_builder());
  const uint32_t output_set = builder.addDescriptorSet(output_builder_);
  builder.addPushConstant(0, rt_denoise::kPushSpace, sizeof(uint32_t) * rt_denoise::kPushWords, plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  if (sampler_set != rt_denoise::kSamplerSet || output_set != rt_denoise::kOutputSet) {
    REXLOG_ERROR("{} {}: the layout's sets are not in the order the shaders declare them", kPrefix, name_);
    return;
  }
  layout_ = device.CreatePipelineLayout(builder, "rt denoise layout");
  if (!layout_) {
    REXLOG_ERROR("{} {}: the layout could not be made: no denoiser for this effect this session", kPrefix, name_);
    return;
  }
  const Blob* blobs[kStageCount] = {&spatial, &temporal};
  bool ok = true;
  for (uint32_t stage = 0; stage < kStageCount; ++stage) {
    if (blobs[stage]->data) {
      shaders_[stage] = passes.CreateShader(blobs[stage]->data, blobs[stage]->size, blobs[stage]->name);
      ok = ok && shaders_[stage] != nullptr;
    }
  }
  if (!ok) {
    REXLOG_ERROR("{} {}: a shader could not be made: no denoiser for this effect this session", kPrefix, name_);
    for (auto& shader : shaders_) shader.reset();
    layout_.reset();
    return;
  }
  ready_ = true;
}

void RtDenoise::Shutdown() {
  if (passes_) {
    passes_->precache().Stop();   // the host builder's functions name this object's layout and shaders: stop it first
  }
  if (spatial_passes_ != 0 || temporal_passes_ != 0) {
    REXLOG_INFO("{} {}: {} spatial and {} temporal pass(es) recorded", kPrefix, name_, spatial_passes_, temporal_passes_);
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (the other host classes do the same).
  for (auto& set : sets_) set.reset();
  set_textures_.fill(nullptr);
  for (HostImage& image : images_) image = HostImage{};
  for (auto& pipeline : pipelines_) pipeline.reset();
  for (auto& shader : shaders_) shader.reset();
  layout_.reset();
  ready_ = false;
  history_valid_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> RtDenoise::MakePipeline(uint32_t stage) {
  if (!layout_ || !shaders_[stage]) {
    return nullptr;
  }
  const std::string name = "rt denoise " + name_ + (stage == kSpatial ? " spatial" : " temporal");
  return device_->CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shaders_[stage].get(), rt_denoise::kThreads, rt_denoise::kThreads, 1), name.c_str());
}

plume::RenderPipeline* RtDenoise::Pipeline(uint32_t stage) {
  if (pipelines_[stage]) {
    return pipelines_[stage].get();
  }
  if (failed_[stage] || !ready_ || !passes_ || !shaders_[stage]) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  const uint64_t key = HostPrecache::Key(HostPrecache::kRtDenoise, (instance_ << 4) | stage);
  const std::string name = "rt denoise " + name_ + (stage == kSpatial ? " spatial" : " temporal");
  if (!listed_[stage]) {
    listed_[stage] = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({key, name, [this, stage] { return MakePipeline(stage); }});
    precache.AddLive("rt denoise " + name_, std::move(items));
    REXLOG_INFO("{} the pipeline '{}' is wanted: it is made in the background, and the stage runs from the frame it is done", kPrefix, name);
  }
  if (!precache.Ready(key)) {
    return nullptr;   // a frame never waits for a compile
  }
  pipelines_[stage] = precache.Acquire(key, name, [this, stage] { return MakePipeline(stage); });
  if (!pipelines_[stage]) {
    failed_[stage] = true;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no such denoiser stage this session", kPrefix, name);
  }
  return pipelines_[stage].get();
}

bool RtDenoise::SpatialReady() { return Pipeline(kSpatial) != nullptr; }
bool RtDenoise::TemporalReady() { return Pipeline(kTemporal) != nullptr; }

// ---------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------

void RtDenoise::RetireSet(uint32_t slot) {
  if (sets_[slot] && frames_) {
    frames_->Retire(std::move(sets_[slot]));
  }
  sets_[slot].reset();
  set_textures_[slot] = nullptr;
}

bool RtDenoise::EnsureImage(uint32_t slot, uint32_t width, uint32_t height) {
  const plume::RenderTexture* before = images_[slot].texture.get();
  const plume::RenderFormat format = format_ == rt_denoise::Format::kRgba8 ? plume::RenderFormat::R8G8B8A8_UNORM : plume::RenderFormat::R16G16B16A16_FLOAT;
  if (!passes_->EnsureImage(images_[slot], width, height, format, "rt denoise image", kImageFlags)) {
    return false;
  }
  if (images_[slot].texture.get() != before) {
    RetireSet(slot);
    if (slot == kHistoryA || slot == kHistoryB) {
      history_valid_ = false;   // a new image has no history
    }
  }
  return true;
}

bool RtDenoise::EnsureSet(uint32_t slot) {
  if (sets_[slot] && set_textures_[slot] == images_[slot].texture.get()) {
    return true;
  }
  RetireSet(slot);
  // One view: u0 the result image (output_builder_). Device::CreateDescriptorSet compares this count with what the backend allocated, and D3D12 allocates one view for each descriptor; Vulkan does not count.
  // rt_denoise_test reads every CreateDescriptorSet call's literal count against its builder.
  sets_[slot] = device_->CreateDescriptorSet(output_builder_, 1, 0, "rt denoise output set");
  if (!sets_[slot]) {
    return false;
  }
  sets_[slot]->setTexture(0, images_[slot].texture.get(), Layout::GENERAL, images_[slot].view.get());
  set_textures_[slot] = images_[slot].texture.get();
  return true;
}

void RtDenoise::ReleaseAll() {
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    RetireSet(slot);
    if (images_[slot].texture) {
      passes_->ReleaseImage(images_[slot]);
    }
  }
  history_valid_ = false;
}

uint64_t RtDenoise::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage& image : images_) {
    if (image.texture) {
      bytes += rt_denoise::ImageBytes(image.width, image.height, format_);
    }
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// the passes
// ---------------------------------------------------------------------------

void RtDenoise::Dispatch(plume::RenderCommandList* list, uint32_t stage, uint32_t output_slot, const std::array<uint32_t, rt_denoise::kPushWords>& push, uint32_t width, uint32_t height, bool final_pass) {
  HostImage& out = images_[output_slot];
  {
    const plume::RenderTextureBarrier write(out.texture.get(), Layout::GENERAL);
    list->barriers(Stage::COMPUTE, nullptr, 0, &write, 1);
    out.layout = Layout::GENERAL;
  }
  list->setComputePipelineLayout(layout_.get());
  for (uint32_t set = 0; set < rt_denoise::kTextureSets; ++set) {
    list->setComputeDescriptorSet(heap_->texture_set(), set);
  }
  list->setComputeDescriptorSet(heap_->sampler_set(), rt_denoise::kSamplerSet);
  list->setComputeDescriptorSet(sets_[output_slot].get(), rt_denoise::kOutputSet);
  list->setPipeline(pipelines_[stage].get());
  list->setComputePushConstants(0, push.data(), 0, sizeof(uint32_t) * rt_denoise::kPushWords);
  list->dispatch(rt_denoise::Groups(width), rt_denoise::Groups(height), 1);
  // The result is read next: by the next iteration (the compute stage), or by the effect's graphics pass (both stages).
  const plume::RenderTextureBarrier read(out.texture.get(), Layout::SHADER_READ);
  list->barriers(final_pass ? Stage::GRAPHICS_AND_COMPUTE : Stage::COMPUTE, nullptr, 0, &read, 1);
  out.layout = Layout::SHADER_READ;
}

namespace {

// The consumer's words the class sets: `low` replaces the low 16 bits of a word, `iteration` the low 4.
void SetLow16(std::array<uint32_t, rt_denoise::kPushWords>& push, uint32_t word, uint32_t value) {
  if (word < rt_denoise::kPushWords) {
    push[word] = (push[word] & 0xFFFF0000u) | (value & 0xFFFFu);
  }
}
void SetIteration(std::array<uint32_t, rt_denoise::kPushWords>& push, uint32_t word, uint32_t iteration) {
  if (word < rt_denoise::kPushWords) {
    push[word] = (push[word] & ~15u) | (iteration & 15u);
  }
}

// The images a stage reads are in the read state of BOTH stages before its first dispatch (D3D12: ALL_SHADER_RESOURCE). They are the effect's own images (the reflections' trace, confidence and
// accumulation images), which its pixel passes read again after this stage (the resolve, the next frame's temporal pass): Plume's D3D12 barriers name the state by the stage, and a compute-only
// state (NON_PIXEL_SHADER_RESOURCE) is not one a pixel shader may read.
void ReadBarriers(plume::RenderCommandList* list, const RtDenoise::Frame& in) {
  plume::RenderTextureBarrier reads[10];
  uint32_t count = 0;
  const auto add = [&](const HostImage* image) {
    if (image && image->texture && count < 10) {
      reads[count++] = plume::RenderTextureBarrier(image->texture.get(), Layout::SHADER_READ);
    }
  };
  add(in.signal);
  for (const HostImage* image : in.reads) add(image);
  if (count > 0) {
    list->barriers(Stage::GRAPHICS_AND_COMPUTE, reads, count);
  }
}

}  // namespace

const HostImage* RtDenoise::RecordSpatial(const Frame& in) {
  if (!ready_ || !in.list || !in.signal || !in.signal->texture || in.width == 0 || in.height == 0 || in.signal->slot > 0xFFFEu) {
    return nullptr;
  }
  if (!Pipeline(kSpatial)) {
    return nullptr;   // not made yet (the first ask lists it)
  }
  const uint32_t iterations = std::clamp<uint32_t>(in.iterations, 1, rt_denoise::kMaxIterations);
  if (!EnsureImage(kSpatialA, in.width, in.height) || !EnsureSet(kSpatialA)) {
    return nullptr;
  }
  if (iterations > 1 && (!EnsureImage(kSpatialB, in.width, in.height) || !EnsureSet(kSpatialB))) {
    return nullptr;
  }
  ReadBarriers(in.list, in);
  const HostImage* source = in.signal;
  for (uint32_t i = 0; i < iterations; ++i) {
    std::array<uint32_t, rt_denoise::kPushWords> push = in.push;
    SetLow16(push, in.patch.src_word, source->slot);
    SetIteration(push, in.patch.iteration_word, i);
    const uint32_t out = (i & 1u) == 0 ? kSpatialA : kSpatialB;
    Dispatch(in.list, kSpatial, out, push, in.width, in.height, i + 1 == iterations);
    source = &images_[out];
  }
  ran_ = true;
  idle_frames_ = 0;
  ++spatial_passes_;
  return source;
}

const HostImage* RtDenoise::RecordTemporal(const Frame& in) {
  last_history_read_ = false;
  if (!ready_ || !in.list || !in.signal || !in.signal->texture || in.width == 0 || in.height == 0 || in.signal->slot > 0xFFFEu) {
    return nullptr;
  }
  if (!Pipeline(kTemporal)) {
    return nullptr;
  }
  if (!EnsureImage(kHistoryA, in.width, in.height) || !EnsureImage(kHistoryB, in.width, in.height) || !EnsureSet(kHistoryA) || !EnsureSet(kHistoryB)) {
    return nullptr;
  }
  const uint32_t read_slot = kHistoryA + history_current_;
  const uint32_t write_slot = kHistoryA + (history_current_ ^ 1u);
  const bool read_history = history_valid_ && in.history_valid && images_[read_slot].width == in.width && images_[read_slot].height == in.height;
  ReadBarriers(in.list, in);
  if (images_[read_slot].layout != Layout::SHADER_READ) {
    // never written yet (or left in another state): the descriptor is bound whether the pass reads it or not
    const plume::RenderTextureBarrier read(images_[read_slot].texture.get(), Layout::SHADER_READ);
    in.list->barriers(Stage::COMPUTE, nullptr, 0, &read, 1);
    images_[read_slot].layout = Layout::SHADER_READ;
  }
  std::array<uint32_t, rt_denoise::kPushWords> push = in.push;
  SetLow16(push, in.patch.src_word, in.signal->slot);
  SetLow16(push, in.patch.history_word, images_[read_slot].slot);
  if (in.patch.flags_word < rt_denoise::kPushWords) {
    const uint32_t bit = 1u << in.patch.has_history_bit;
    push[in.patch.flags_word] = read_history ? (push[in.patch.flags_word] | bit) : (push[in.patch.flags_word] & ~bit);
  }
  Dispatch(in.list, kTemporal, write_slot, push, in.width, in.height, /*final_pass=*/true);
  history_current_ ^= 1u;
  history_valid_ = true;
  last_history_read_ = read_history;
  ran_ = true;
  idle_frames_ = 0;
  ++temporal_passes_;
  return &images_[write_slot];
}

void RtDenoise::OnPresent() {
  if (ran_) {
    idle_frames_ = 0;
  } else {
    history_valid_ = false;   // a frame without a stage breaks the history: the next temporal frame reads none
    if (ready_ && passes_ && (images_[kSpatialA].texture || images_[kSpatialB].texture || images_[kHistoryA].texture || images_[kHistoryB].texture) && ++idle_frames_ >= kReleaseAfterFrames) {
      ReleaseAll();
      idle_frames_ = 0;
    }
  }
  ran_ = false;
}

}  // namespace mcla::gpu

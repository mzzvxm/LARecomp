// MCLA GPU renderer -- the renderer's own full-screen passes. See
// host_passes.h.

#include "host_passes.h"

#include <algorithm>
#include <format>
#include <string>

#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_shaders.h"

namespace mcla::gpu {

HostPasses::~HostPasses() {
  Shutdown();
}

bool HostPasses::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  layout_ = CreateLayout(kPushConstantBytes, "host pass layout");
  const HostShaderBlob vs = FullscreenVertexShader();
  vertex_shader_ = device.CreateShader(vs.data, vs.size, "main", vs.name);
  if (!layout_ || !vertex_shader_) {
    Shutdown();
    return false;
  }
  return true;
}

std::unique_ptr<plume::RenderPipelineLayout> HostPasses::CreateLayout(uint32_t push_bytes, const char* name) const {
  if (!device_ || !heap_) return nullptr;
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  builder.addDescriptorSet(heap_->texture_set_builder());  // set 0: Texture2D<float4>[]   space0
  builder.addDescriptorSet(heap_->texture_set_builder());  // set 1: Texture2DMS<float4>[] space1
  builder.addDescriptorSet(heap_->texture_set_builder());  // set 2: Texture2D<uint4>[]    space2
  builder.addDescriptorSet(heap_->texture_set_builder());  // set 3: Texture2DMS<uint4>[]  space3
  builder.addDescriptorSet(heap_->sampler_set_builder());  // set 4: SamplerState[]        space4
  builder.addPushConstant(0, 4, push_bytes,
                          plume::RenderShaderStageFlag::VERTEX |
                              plume::RenderShaderStageFlag::PIXEL);  // b0, space4
  builder.end();
  return device_->CreatePipelineLayout(builder, name);
}

void HostPasses::Shutdown() {
  // The builder uses the layout and the vertex shader below.
  precache_.Stop();
  vertex_shader_.reset();
  layout_.reset();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
}

std::unique_ptr<plume::RenderShader> HostPasses::CreateShader(const void* data, uint64_t size,
                                                              const char* name) {
  return device_->CreateShader(data, size, "main", name);
}

std::unique_ptr<plume::RenderPipeline> HostPasses::CreatePipeline(
    const plume::RenderShader& pixel_shader, plume::RenderFormat format, const char* name) {
  return CreatePipeline(*layout_, pixel_shader, format, name);
}

std::unique_ptr<plume::RenderPipeline> HostPasses::CreatePipeline(
    const plume::RenderShader& pixel_shader, plume::RenderFormat format, uint8_t write_mask,
    const char* name) {
  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
  blend.renderTargetWriteMask = write_mask;
  return CreatePipeline(pixel_shader, format, blend, name);
}

std::unique_ptr<plume::RenderPipeline> HostPasses::CreatePipeline(
    const plume::RenderShader& pixel_shader, plume::RenderFormat format,
    const plume::RenderBlendDesc& blend, const char* name) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = &pixel_shader;
  desc.depthClipEnabled = false;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = blend;
  desc.renderTargetCount = 1;
  return device_->CreateGraphicsPipeline(desc, name);
}

std::unique_ptr<plume::RenderPipeline> HostPasses::CreatePipeline(
    const plume::RenderPipelineLayout& layout, const plume::RenderShader& pixel_shader,
    plume::RenderFormat format, const char* name) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = &layout;
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = &pixel_shader;
  desc.depthClipEnabled = false;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.renderTargetFormat[0] = format;
  // Blending off is a copy: ONE/ZERO/ADD (UNKNOWN factors are invalid in Plume
  // even when the blend is disabled).
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.renderTargetCount = 1;
  return device_->CreateGraphicsPipeline(desc, name);
}

std::unique_ptr<plume::RenderPipeline> HostPasses::CreatePipeline(
    const plume::RenderPipelineLayout& layout, const plume::RenderShader& pixel_shader,
    plume::RenderFormat format, const plume::RenderBlendDesc& blend, uint32_t sample_count,
    const char* name) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = &layout;
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = &pixel_shader;
  desc.depthClipEnabled = false;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(sample_count));
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = blend;
  desc.renderTargetCount = 1;
  return device_->CreateGraphicsPipeline(desc, name);
}

std::unique_ptr<plume::RenderPipeline> HostPasses::CreatePipeline(
    const plume::RenderPipelineLayout& layout, const plume::RenderShader& pixel_shader,
    const plume::RenderFormat* formats, uint32_t count, const char* name) {
  // CITY-RT R4's overload (2 to 8 targets): the one implementation is CreateMrtPipeline below (SS-3), which also takes one.
  if (!formats || count < 2) {
    return nullptr;
  }
  return CreateMrtPipeline(layout, pixel_shader, formats, count, name);
}

std::unique_ptr<plume::RenderPipeline> HostPasses::CreateMrtPipeline(
    const plume::RenderPipelineLayout& layout, const plume::RenderShader& pixel_shader,
    const plume::RenderFormat* formats, uint32_t count, const char* name) {
  if (!formats || count == 0 || count > plume::RenderGraphicsPipelineDesc::MaxRenderTargets) {
    return nullptr;
  }
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = &layout;
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = &pixel_shader;
  desc.depthClipEnabled = false;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  for (uint32_t i = 0; i < count; ++i) {
    desc.renderTargetFormat[i] = formats[i];
    desc.renderTargetBlend[i] = plume::RenderBlendDesc::Copy();
  }
  desc.renderTargetCount = count;
  return device_->CreateGraphicsPipeline(desc, name);
}

std::unique_ptr<plume::RenderFramebuffer> HostPasses::CreateMrtFramebuffer(
    const plume::RenderTexture* const* colors, uint32_t count, const char* name) {
  const plume::RenderTexture* attachments[plume::RenderGraphicsPipelineDesc::MaxRenderTargets] = {};
  if (!colors || count == 0 || count > plume::RenderGraphicsPipelineDesc::MaxRenderTargets) {
    return nullptr;
  }
  for (uint32_t i = 0; i < count; ++i) {
    attachments[i] = colors[i];
  }
  return device_->CreateFramebuffer(plume::RenderFramebufferDesc(attachments, count), name);
}

void HostPasses::Bind(plume::RenderCommandList* list, const plume::RenderPipelineLayout* layout) const {
  if (!list || !heap_) return;
  list->setGraphicsPipelineLayout(layout ? layout : layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
}

bool HostPasses::EnsureImage(HostImage& image, uint32_t width, uint32_t height,
                             plume::RenderFormat format, const char* name,
                             plume::RenderTextureFlags flags, uint32_t samples) {
  samples = std::max<uint32_t>(samples, 1);
  if (image.texture && !image.alias_bytes && image.width == width && image.height == height &&
      image.format == format && image.flags == flags && image.samples == samples) {
    return true;
  }
  ReleaseImage(image);
  plume::RenderTextureDesc desc =
      plume::RenderTextureDesc::Texture2D(width, height, 1, format, flags);
  desc.multisampling.sampleCount = plume::RenderSampleCounts(samples);
  if (!device_ || !heap_ || !frames_) {
    return false;
  }
  image.texture = device_->CreateTexture(desc, name);
  return FinishImage(image, desc, name);
}

bool HostPasses::FinishImage(HostImage& image, const plume::RenderTextureDesc& desc, const char* name) {
  const bool render_target = (desc.flags & plume::RenderTextureFlag::RENDER_TARGET) != 0;
  const uint32_t samples = desc.multisampling.sampleCount;
  if (image.texture && device_ && heap_) {
    plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(desc.format);
    view.mipLevels = 1;
    image.view = device_->CreateTextureView(*image.texture, view, name);
    if (render_target) {
      const plume::RenderTexture* attachments[1] = {image.texture.get()};
      image.framebuffer =
          device_->CreateFramebuffer(plume::RenderFramebufferDesc(attachments, 1), name);
    }
  }
  if (image.view) {
    image.slot = heap_->AllocateTexture(*image.texture, *image.view);
  }
  if (!image.texture || !image.view || (render_target && !image.framebuffer) ||
      image.slot == kInvalidBindlessSlot) {
    // Asked again by the next frame while the cause lasts: the first 8 are logged.
    if (image_failures_logged_ < 8) {
      ++image_failures_logged_;
      REXLOG_ERROR("[gpu] {}: the {}x{}{} image could not be made", name, desc.width, desc.height,
                   samples > 1 ? std::format(" {} sample", samples) : std::string());
    }
    ReleaseImage(image);
    return false;
  }
  image.width = desc.width;
  image.height = desc.height;
  image.format = desc.format;
  image.flags = desc.flags;
  image.samples = samples;
  image.serial = ++image_serial_;
  image.layout = plume::RenderTextureLayout::UNKNOWN;
  return true;
}

bool HostPasses::EnsureAliasedTargets(HostImage& first, const plume::RenderTextureDesc& first_desc,
                                      const char* first_name, HostImage& second,
                                      const plume::RenderTextureDesc& second_desc, const char* second_name) {
  const auto matches = [](const HostImage& image, const plume::RenderTextureDesc& desc) {
    return image.texture && image.alias_bytes && image.width == desc.width &&
           image.height == desc.height && image.format == desc.format;
  };
  if (matches(first, first_desc) && matches(second, second_desc)) {
    return true;
  }
  if (!device_ || !frames_ || !heap_ || device_->frame() < next_alias_attempt_) {
    return false;
  }
  auto textures = backend::CreateAliasedTargets(device_->device(), first_desc, second_desc);
  if (textures.bytes == 0) {
    next_alias_attempt_ = device_->frame() + Device::kBackoffFrames;
    return false;
  }

  HostImage a, b;
  a.texture = std::move(textures.first);
  b.texture = std::move(textures.second);
  a.alias_bytes = b.alias_bytes = textures.bytes;
  if (!FinishImage(a, first_desc, first_name) || !FinishImage(b, second_desc, second_name)) {
    ReleaseImage(a);
    ReleaseImage(b);
    next_alias_attempt_ = device_->frame() + Device::kBackoffFrames;
    return false;
  }
  ReleaseImage(first);
  ReleaseImage(second);
  first = std::move(a);
  second = std::move(b);
  REXLOG_INFO("[gpu] video memory: {} and {} share {:.2f} MiB",
              first_name, second_name, double(textures.bytes) / double(1u << 20));
  return true;
}

void HostPasses::ActivateAliasedTarget(plume::RenderCommandList* list, HostImage& image) const {
  if (list && image.texture && image.alias_bytes) {
    backend::ActivateAliasedTarget(list, image.texture.get());
    image.layout = plume::RenderTextureLayout::UNKNOWN;  // DrawPass clears after every activation.
  }
}

void HostPasses::ReleaseImage(HostImage& image) {
  // The slot first, so the descriptor is null before the texture dies; the
  // objects after the lists that still name them (FrameRing serials).
  if (heap_ && image.slot != kInvalidBindlessSlot) {
    heap_->ReleaseTexture(image.slot, TextureDimension::k2D);
  }
  if (frames_ && image.framebuffer) {
    frames_->Retire(std::move(image.framebuffer));
  }
  if (frames_ && image.view) {
    frames_->Retire(std::move(image.view));
  }
  if (image.texture) {
    // Aliases share the allocation until both placements retire.
    if (device_ && !image.alias_bytes) {
      device_->NoteReleased(TextureBytes(image.width, image.height, 1, image.format, image.samples));
    }
    if (frames_) {
      frames_->Retire(std::move(image.texture));
    }
  }
  image = HostImage{};
}

std::unique_ptr<plume::RenderFramebuffer> HostPasses::CreateFramebuffer(
    const plume::RenderTexture* color, const plume::RenderTexture* depth, const char* name) {
  const plume::RenderTexture* attachments[1] = {color};
  return device_->CreateFramebuffer(plume::RenderFramebufferDesc(attachments, 1, depth), name);
}

void HostPasses::RetireFramebuffer(std::unique_ptr<plume::RenderFramebuffer> framebuffer) {
  if (framebuffer) {
    frames_->Retire(std::move(framebuffer));
  }
}

bool HostMrtPair::Ensure(HostPasses& passes, const HostImage& first, const HostImage& second,
                         const char* name) {
  if (!first.texture || !second.texture || first.width != second.width ||
      first.height != second.height || first.samples != 1 || second.samples != 1) {
    return false;
  }
  if (framebuffer && first_serial == first.serial && second_serial == second.serial) {
    return true;
  }
  Release(passes);
  const plume::RenderTexture* colors[] = {first.texture.get(), second.texture.get()};
  framebuffer = passes.CreateMrtFramebuffer(colors, 2, name);
  if (!framebuffer) {
    return false;
  }
  first_serial = first.serial;
  second_serial = second.serial;
  return true;
}

void HostMrtPair::Release(HostPasses& passes) {
  passes.RetireFramebuffer(std::move(framebuffer));
  first_serial = second_serial = 0;
}

void HostPasses::DrawMrtPair(plume::RenderCommandList& list, const plume::RenderFramebuffer& framebuffer,
                            HostImage& first, HostImage& second,
                            const plume::RenderPipeline& pipeline, const void* push,
                            uint32_t push_size, const plume::RenderPipelineLayout* layout) const {
  using Layout = plume::RenderTextureLayout;
  const bool clear_first = first.layout == Layout::UNKNOWN;
  const bool clear_second = second.layout == Layout::UNKNOWN;
  const plume::RenderTextureBarrier writes[] = {{first.texture.get(), Layout::COLOR_WRITE},
                                                {second.texture.get(), Layout::COLOR_WRITE}};
  list.barriers(plume::RenderBarrierStage::GRAPHICS, writes, 2);
  list.setFramebuffer(&framebuffer);
  if (clear_first) {
    list.clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  if (clear_second) {
    list.clearColor(1, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(first.width), float(first.height)};
  const plume::RenderRect scissor(0, 0, int32_t(first.width), int32_t(first.height));
  list.setViewports(&viewport, 1);
  list.setScissors(&scissor, 1);
  Bind(&list, layout);
  list.setPipeline(&pipeline);
  list.setGraphicsPushConstants(0, push, 0, push_size);
  list.drawInstanced(3, 1, 0, 0);
  list.setFramebuffer(nullptr);
  const plume::RenderTextureBarrier reads[] = {{first.texture.get(), Layout::SHADER_READ},
                                               {second.texture.get(), Layout::SHADER_READ}};
  list.barriers(plume::RenderBarrierStage::GRAPHICS, reads, 2);
  first.layout = second.layout = Layout::SHADER_READ;
}

void HostPasses::DrawInto(plume::RenderCommandList* list, HostImage& image,
                          const plume::RenderPipeline* pipeline, const void* push,
                          uint32_t push_size, bool clear,
                          const plume::RenderPipelineLayout* layout) const {
  // A new colour target from a heap made without zeroing is cleared before
  // anything else uses it, even when the draw covers it whole (D3D12 debug
  // layer id 1422, as the EDRAM targets).
  const bool first_use = image.layout == plume::RenderTextureLayout::UNKNOWN;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  if (clear || first_use) {
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(image.width), float(image.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(image.width), int32_t(image.height));
  list->setScissors(&scissor, 1);
  Bind(list, layout);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, push, 0, push_size);
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

void HostPasses::DrawRectsInto(plume::RenderCommandList* list, HostImage& image,
                               const plume::RenderPipeline* pipeline, const plume::RenderRect* rects,
                               const void* pushes, uint32_t push_size, uint32_t count) const {
  if (count == 0 || !rects || !pushes) {
    return;
  }
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  const plume::RenderViewport viewport{0.0f, 0.0f, float(image.width), float(image.height)};
  list->setViewports(&viewport, 1);
  Bind(list);
  list->setPipeline(pipeline);
  const uint8_t* block = static_cast<const uint8_t*>(pushes);
  for (uint32_t i = 0; i < count; ++i) {
    list->setScissors(&rects[i], 1);
    list->setGraphicsPushConstants(0, block + size_t(i) * push_size, 0, push_size);
    list->drawInstanced(3, 1, 0, 0);
  }
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

}  // namespace mcla::gpu

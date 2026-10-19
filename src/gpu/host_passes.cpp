// MCLA GPU renderer -- the renderer's own full-screen passes. See
// host_passes.h.

#include "host_passes.h"

#include <rex/logging.h>

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
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  builder.addDescriptorSet(heap.texture_set_builder());  // set 0: Texture2D<float4>[]   space0
  builder.addDescriptorSet(heap.texture_set_builder());  // set 1: Texture2DMS<float4>[] space1
  builder.addDescriptorSet(heap.texture_set_builder());  // set 2: Texture2D<uint4>[]    space2
  builder.addDescriptorSet(heap.texture_set_builder());  // set 3: Texture2DMS<uint4>[]  space3
  builder.addDescriptorSet(heap.sampler_set_builder());  // set 4: SamplerState[]        space4
  builder.addPushConstant(0, 4, kPushConstantBytes,
                          plume::RenderShaderStageFlag::VERTEX |
                              plume::RenderShaderStageFlag::PIXEL);  // b0, space4
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "host pass layout");
  const HostShaderBlob vs = FullscreenVertexShader();
  vertex_shader_ = device.CreateShader(vs.data, vs.size, "main", vs.name);
  if (!layout_ || !vertex_shader_) {
    Shutdown();
    return false;
  }
  return true;
}

void HostPasses::Shutdown() {
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
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.renderTargetBlend[0].renderTargetWriteMask = write_mask;
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

void HostPasses::Bind(plume::RenderCommandList* list) const {
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
}

bool HostPasses::EnsureImage(HostImage& image, uint32_t width, uint32_t height,
                             plume::RenderFormat format, const char* name) {
  if (image.texture && image.width == width && image.height == height && image.format == format) {
    return true;
  }
  ReleaseImage(image);
  image.texture = device_->CreateTexture(
      plume::RenderTextureDesc::Texture2D(width, height, 1, format,
                                          plume::RenderTextureFlag::RENDER_TARGET),
      name);
  if (image.texture) {
    plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(format);
    view.mipLevels = 1;
    image.view = device_->CreateTextureView(*image.texture, view, name);
    const plume::RenderTexture* attachments[1] = {image.texture.get()};
    image.framebuffer =
        device_->CreateFramebuffer(plume::RenderFramebufferDesc(attachments, 1), name);
  }
  if (image.view) {
    image.slot = heap_->AllocateTexture(*image.texture, *image.view);
  }
  if (!image.texture || !image.view || !image.framebuffer || image.slot == kInvalidBindlessSlot) {
    REXLOG_ERROR("[gpu] {}: the {}x{} image could not be made", name, width, height);
    ReleaseImage(image);
    return false;
  }
  image.width = width;
  image.height = height;
  image.format = format;
  image.layout = plume::RenderTextureLayout::UNKNOWN;
  return true;
}

void HostPasses::ReleaseImage(HostImage& image) {
  // The slot first, so the descriptor is null before the texture dies; the
  // objects after the lists that still name them (FrameRing serials).
  if (image.slot != kInvalidBindlessSlot) {
    heap_->ReleaseTexture(image.slot, TextureDimension::k2D);
  }
  if (image.framebuffer) {
    frames_->Retire(std::move(image.framebuffer));
  }
  if (image.view) {
    frames_->Retire(std::move(image.view));
  }
  if (image.texture) {
    frames_->Retire(std::move(image.texture));
  }
  image = HostImage{};
}

void HostPasses::DrawInto(plume::RenderCommandList* list, HostImage& image,
                          const plume::RenderPipeline* pipeline, const void* push,
                          uint32_t push_size, bool clear) const {
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
  Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, push, 0, push_size);
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

}  // namespace mcla::gpu

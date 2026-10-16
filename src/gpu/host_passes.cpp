// MCLA GPU renderer -- the renderer's own full-screen passes. See
// host_passes.h.

#include "host_passes.h"

#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "host_shaders.h"

namespace mcla::gpu {

HostPasses::~HostPasses() {
  Shutdown();
}

bool HostPasses::Initialize(Device& device, const BindlessHeap& heap) {
  device_ = &device;
  heap_ = &heap;
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  builder.addDescriptorSet(heap.texture_set_builder());  // set 0: Texture2D<float4>[]   space0
  builder.addDescriptorSet(heap.texture_set_builder());  // set 1: Texture2DMS<float4>[] space1
  builder.addDescriptorSet(heap.texture_set_builder());  // set 2: Texture2D<uint4>[]    space2
  builder.addDescriptorSet(heap.texture_set_builder());  // set 3: Texture2DMS<uint4>[]  space3
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
}

}  // namespace mcla::gpu

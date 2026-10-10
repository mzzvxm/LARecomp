// MCLA GPU renderer -- the pipeline layout of the guest shaders. See layout.h.
//
// Design followed: reblue gpu/device_pipelines.cpp BuildPipelineLayout
// (BSD-3-Clause): the texture set at three spaces, the sampler set, root
// constant buffers on D3D12 and a single push range of addresses on Vulkan,
// the occlusion counter reserved from the start. Reimplemented.

#include "layout.h"

#include <rex/logging.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"

namespace mcla::gpu {

GuestLayout::~GuestLayout() {
  Shutdown();
}

bool GuestLayout::Initialize(Device& device, const BindlessHeap& heap) {
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/true);
  builder.addDescriptorSet(heap.texture_set_builder());  // set 0: Texture2D[]   space0
  builder.addDescriptorSet(heap.texture_set_builder());  // set 1: Texture3D[]   space1
  builder.addDescriptorSet(heap.texture_set_builder());  // set 2: TextureCube[] space2
  builder.addDescriptorSet(heap.sampler_set_builder());  // set 3: SamplerState[] space3
#if defined(LARECOMP_GPU_D3D12)
  builder.addRootDescriptor(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  builder.addRootDescriptor(1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  builder.addRootDescriptor(2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  builder.addRootDescriptor(0, 5, plume::RenderRootDescriptorType::UNORDERED_ACCESS);
#else
  builder.addPushConstant(0, 4, kPushConstantBytes,
                          plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  occlusion_builder_.begin();
  occlusion_builder_.addReadWriteByteAddressBuffer(0);
  occlusion_builder_.end();
  builder.addDescriptorSet(occlusion_builder_);  // set 4
#endif
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "guest shader layout");
  if (!layout_) {
    return false;
  }
  REXLOG_INFO("[gpu] guest shader layout created ({})", BackendName());
  return true;
}

void GuestLayout::Shutdown() {
  layout_.reset();
}

}  // namespace mcla::gpu

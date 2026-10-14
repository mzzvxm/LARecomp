// MCLA GPU renderer -- the pipeline layout of the guest shaders. See layout.h.
//
// Design followed: reblue gpu/device_pipelines.cpp BuildPipelineLayout
// (BSD-3-Clause): the texture set at three spaces, the sampler set, root
// constant buffers on D3D12 and a single push range of addresses on Vulkan.
// Reimplemented, without reblue's occlusion counter (layout.h).

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
  if (ActiveBackend() == Backend::kD3D12) {
    // The generator's registers: VertexShaderConstants b0, PixelShaderConstants
    // b1, SharedConstants b2, all space4. A root descriptor's index is the order
    // it was added in (what setGraphicsRootDescriptor takes).
    const uint32_t vs_index =
        builder.addRootDescriptor(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    const uint32_t ps_index =
        builder.addRootDescriptor(1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    const uint32_t shared_index =
        builder.addRootDescriptor(2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    if (vs_index != kVertexConstants || ps_index != kPixelConstants ||
        shared_index != kSharedConstants) {
      REXLOG_ERROR("[gpu] guest shader layout: root descriptor indices {}/{}/{} are not the "
                   "ones the renderer binds",
                   vs_index, ps_index, shared_index);
      return false;
    }
  } else {
    // Root descriptors are D3D12-only in Plume (Vulkan asserts): the three
    // buffers' device addresses go in one push range instead (shader_common.h
    // vk::RawBufferLoad).
    builder.addPushConstant(
        0, 4, kPushConstantBytes,
        plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  }
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

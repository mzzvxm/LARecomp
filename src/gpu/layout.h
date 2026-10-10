#pragma once
// ===========================================================================
// MCLA GPU renderer -- the pipeline layout of the guest shaders
// ===========================================================================
// One layout for every translated guest shader, fixed by the LAR-XenosRecomp
// contract:
//
//   sets 0, 1, 2   the BindlessHeap texture set, bound three times: the shaders
//                  declare Texture2D[] at space0, Texture3D[] at space1 and
//                  TextureCube[] at space2 over the same descriptors (reblue
//                  device_pipelines.cpp does the same)
//   set 3          the BindlessHeap sampler set (space3)
//   D3D12          root CBVs b0/b1/b2 space4: VS constants, PS constants,
//                  SharedConstants; root UAV u0 space5 reserved for the
//                  occlusion counter
//   Vulkan         one VERTEX|PIXEL push range of 24 bytes: the three buffer
//                  device addresses the shaders read with vk::RawBufferLoad;
//                  set 4, a read-write byte address buffer, reserved as above
//
// Input-assembler input is allowed: the guest's vertex streams arrive through
// vertex buffers. M3 creates the layout and proves it exists on both backends;
// pipelines are M4.
//
// Threading: Initialize/Shutdown on the lifecycle thread, used by the render
// thread.
// ===========================================================================

#include <memory>

#include <plume_render_interface.h>

namespace mcla::gpu {

class BindlessHeap;
class Device;

class GuestLayout {
 public:
  // The three addresses the SPIR-V shaders read (Vulkan push constants).
  static constexpr uint32_t kPushConstantBytes = 3 * sizeof(uint64_t);

  GuestLayout() = default;
  ~GuestLayout();
  GuestLayout(const GuestLayout&) = delete;
  GuestLayout& operator=(const GuestLayout&) = delete;

  bool Initialize(Device& device, const BindlessHeap& heap);
  // No pipeline built on the layout may still exist.
  void Shutdown();

  plume::RenderPipelineLayout* layout() const { return layout_.get(); }

 private:
  // Vulkan's reserved set 4 needs its builder alive as long as the layout
  // (RenderDescriptorSetBuilder::end points the desc into the builder).
  plume::RenderDescriptorSetBuilder occlusion_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
};

}  // namespace mcla::gpu

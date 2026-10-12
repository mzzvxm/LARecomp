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
//                  SharedConstants (root parameters 0, 1, 2 after the sets)
//   Vulkan         one VERTEX|PIXEL push range of 24 bytes: the three buffer
//                  device addresses the shaders read with vk::RawBufferLoad
//
// No occlusion counter: the reserve M3 kept (a root UAV / a fifth set) left
// before the first pipeline, the game never arming a query (viz query 0 and
// RB_SAMPLE_COUNT_CTL constant in every measured scene
// item 4).
//
// Input-assembler input is allowed: the guest's vertex streams arrive through
// vertex buffers.
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
  // The three addresses the SPIR-V shaders read (Vulkan push constants), in
  // this order; on D3D12 the same three are root CBVs with these indices.
  static constexpr uint32_t kPushConstantBytes = 3 * sizeof(uint64_t);
  static constexpr uint32_t kVertexConstants = 0;
  static constexpr uint32_t kPixelConstants = 1;
  static constexpr uint32_t kSharedConstants = 2;

  GuestLayout() = default;
  ~GuestLayout();
  GuestLayout(const GuestLayout&) = delete;
  GuestLayout& operator=(const GuestLayout&) = delete;

  bool Initialize(Device& device, const BindlessHeap& heap);
  // No pipeline built on the layout may still exist.
  void Shutdown();

  plume::RenderPipelineLayout* layout() const { return layout_.get(); }

 private:
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
};

}  // namespace mcla::gpu

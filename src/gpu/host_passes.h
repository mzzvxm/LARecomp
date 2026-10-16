#pragma once
// ===========================================================================
// MCLA GPU renderer -- the renderer's own full-screen passes
// ===========================================================================
// The resolve conversions (resolve.h)
// and the present pass (present.h) draw one triangle over a viewport with a
// pixel shader that reads bindless textures by slot and takes its parameters
// from 32 bytes of push constants. One pipeline layout serves them all
// (hlsl/host_pass_common.hlsli): the BindlessHeap's texture set bound at
// spaces 0-3 -- Texture2D<float4>[], Texture2DMS<float4>[], Texture2D<uint4>[],
// Texture2DMS<uint4>[]: a descriptor carries its own dimension and format, so
// one set serves every view type, each declared in a space of its own as the
// guest shaders' layout does (layout.h) -- and the push range at (b0, space4).
// No samplers: the passes Load texels.
//
// Threading: Initialize/Shutdown on the lifecycle thread; the rest on the
// render thread.
// ===========================================================================

#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

namespace mcla::gpu {

class BindlessHeap;
class Device;

class HostPasses {
 public:
  static constexpr uint32_t kPushConstantBytes = 32;

  HostPasses() = default;
  ~HostPasses();
  HostPasses(const HostPasses&) = delete;
  HostPasses& operator=(const HostPasses&) = delete;

  bool Initialize(Device& device, const BindlessHeap& heap);
  // No pipeline made by CreatePipeline may still exist.
  void Shutdown();

  // A pipeline drawing the full-screen triangle with `pixel_shader` into one
  // single-sampled target of `format`, written whole, no blending, no depth.
  // Null on failure (logged by Device).
  std::unique_ptr<plume::RenderPipeline> CreatePipeline(const plume::RenderShader& pixel_shader,
                                                        plume::RenderFormat format,
                                                        const char* name);
  // The same with another pipeline layout, for a pass with resources of its
  // own (resolve_memory.h's decode). The full-screen vertex shader reads none.
  std::unique_ptr<plume::RenderPipeline> CreatePipeline(const plume::RenderPipelineLayout& layout,
                                                        const plume::RenderShader& pixel_shader,
                                                        plume::RenderFormat format,
                                                        const char* name);

  // Binds the layout and the four texture sets on `list`.
  void Bind(plume::RenderCommandList* list) const;

  // A shader from a host blob (host_shaders.h). Null on failure (logged).
  std::unique_ptr<plume::RenderShader> CreateShader(const void* data, uint64_t size,
                                                    const char* name);

 private:
  Device* device_ = nullptr;
  const BindlessHeap* heap_ = nullptr;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> vertex_shader_;
};

}  // namespace mcla::gpu

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
// guest shaders' layout does (layout.h) -- the BindlessHeap's sampler set at
// space 4 (M6c: FSR1's gathers, FXAA and SMAA filter; the other
// passes Load texels and leave it unused; hlsl/host_pass_samplers.hlsli) and
// the push range at (b0, space4).
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
class FrameRing;

// An image one host pass draws into and the next one samples (M6c): the texture, its view in a bindless slot, its framebuffer, and the
// layout it was left in (HostPasses::EnsureImage/DrawInto).
struct HostImage {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t slot = 0xFFFFFFFFu;
  uint32_t width = 0;
  uint32_t height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
};

class HostPasses {
 public:
  static constexpr uint32_t kPushConstantBytes = 32;

  HostPasses() = default;
  ~HostPasses();
  HostPasses(const HostPasses&) = delete;
  HostPasses& operator=(const HostPasses&) = delete;

  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap);
  // No pipeline made by CreatePipeline may still exist.
  void Shutdown();

  // A pipeline drawing the full-screen triangle with `pixel_shader` into one
  // single-sampled target of `format`, written whole, no blending, no depth.
  // Null on failure (logged by Device).
  std::unique_ptr<plume::RenderPipeline> CreatePipeline(const plume::RenderShader& pixel_shader,
                                                        plume::RenderFormat format,
                                                        const char* name);
  // The same writing only the channels of `write_mask`
  // (plume::RenderColorWriteEnable bits): the scene's anti-aliasing keeps the
  // guest's alpha.
  std::unique_ptr<plume::RenderPipeline> CreatePipeline(const plume::RenderShader& pixel_shader,
                                                        plume::RenderFormat format,
                                                        uint8_t write_mask, const char* name);
  // The same with another pipeline layout, for a pass with resources of its
  // own (resolve_memory.h's decode). The full-screen vertex shader reads none.
  std::unique_ptr<plume::RenderPipeline> CreatePipeline(const plume::RenderPipelineLayout& layout,
                                                        const plume::RenderShader& pixel_shader,
                                                        plume::RenderFormat format,
                                                        const char* name);

  // Binds the layout, the four texture sets and the sampler set on `list`.
  void Bind(plume::RenderCommandList* list) const;

  // A shader from a host blob (host_shaders.h). Null on failure (logged).
  std::unique_ptr<plume::RenderShader> CreateShader(const void* data, uint64_t size,
                                                    const char* name);

  // `image` at `width` x `height` in `format`, a colour target with a 2D view
  // in a bindless slot: kept when it already is that, otherwise made anew (the
  // old one retired after the lists that name it). False when it cannot be
  // made (logged with `name`; the image is left empty).
  bool EnsureImage(HostImage& image, uint32_t width, uint32_t height, plume::RenderFormat format,
                   const char* name);
  // Its slot back to the heap and its objects to the FrameRing.
  void ReleaseImage(HostImage& image);
  // Outside a render pass: the full-screen triangle into all of `image` with
  // `pipeline` and `push` (cleared to zero first when `clear`, and on the
  // image's first use: a new render target is initialised before it is drawn
  // into, D3D12 debug layer id 1422), leaving no framebuffer bound and `image`
  // readable by the next pass.
  void DrawInto(plume::RenderCommandList* list, HostImage& image,
                const plume::RenderPipeline* pipeline, const void* push, uint32_t push_size,
                bool clear = false) const;

 private:
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> vertex_shader_;
};

}  // namespace mcla::gpu

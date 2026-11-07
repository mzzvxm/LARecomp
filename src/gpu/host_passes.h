#pragma once
// ===========================================================================
// MCLA GPU renderer -- the renderer's own full-screen passes
// ===========================================================================
// The resolve conversions (resolve.h)
// and the present pass (present.h) draw one triangle over a viewport with a
// pixel shader that reads bindless textures by slot and takes its parameters
// from up to 48 bytes of push constants. One pipeline layout serves them all
// (hlsl/host_pass_common.hlsli): the BindlessHeap's texture set bound at
// spaces 0-3 -- Texture2D<float4>[], Texture2DMS<float4>[], Texture2D<uint4>[],
// Texture2DMS<uint4>[]: a descriptor carries its own dimension and format, so
// one set serves every view type, each declared in a space of its own as the
// guest shaders' layout does (layout.h) -- the BindlessHeap's sampler set at
// space 4 (M6c: FSR1's gathers, FXAA and SMAA filter; the other
// passes Load texels and leave it unused; hlsl/host_pass_samplers.hlsli) and
// the push range at (b0, space4).
//
// The pipelines the passes' owners can name at start are made ahead of their
// first use, off the render thread (host_precache.h); precache() is where they
// are listed and asked for.
//
// Threading: Initialize/Shutdown on the lifecycle thread; the rest on the
// render thread. CreatePipeline and CreateShader read only what Initialize
// made and call the Device's validated creation, so the host builder thread
// (host_precache.h) calls them too.
// ===========================================================================

#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#include "host_precache.h"

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
  plume::RenderTextureFlags flags = plume::RenderTextureFlag::RENDER_TARGET;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
};

class HostPasses {
 public:
  // 32 bytes held every pass's constants until the HDR10 output (hdr.h) added the four values
  // its last passes take; the passes that push less push less.
  // CHECK: Plume turns the range into 12 root constants on D3D12 (the signature stays well under 64
  // dwords) and a 48-byte push range on Vulkan; the passes' shaders declare less and that is fine on both.
  static constexpr uint32_t kPushConstantBytes = 48;

  HostPasses() = default;
  ~HostPasses();
  HostPasses(const HostPasses&) = delete;
  HostPasses& operator=(const HostPasses&) = delete;

  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap);
  // No pipeline made by CreatePipeline may still exist (the host builder
  // thread is stopped first, and the pipelines it made that nobody took die).
  void Shutdown();

  // The pipelines of the passes' owners made ahead of first use. Each owner
  // lists its own after its shaders exist (Initialize), the Renderer starts
  // the builder when all have, and each owner stops it first in its Shutdown:
  // the functions it was given name that owner's shaders.
  HostPrecache& precache() { return precache_; }

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
  // The same with the target's blend as given (the UI layer split's put-back draws the scene
  // under the UI with INV_DEST_ALPHA / ONE, frame_layers.h).
  std::unique_ptr<plume::RenderPipeline> CreatePipeline(const plume::RenderShader& pixel_shader,
                                                        plume::RenderFormat format,
                                                        const plume::RenderBlendDesc& blend,
                                                        const char* name);
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
  // made (logged with `name`; the image is left empty). `flags` other than the
  // default make an image a compute pass writes (a temporal upscaler's output:
  // plume::RenderTextureFlag::STORAGE | UNORDERED_ACCESS): with no RENDER_TARGET
  // in them it has no framebuffer and DrawInto must not be used on it.
  bool EnsureImage(HostImage& image, uint32_t width, uint32_t height, plume::RenderFormat format,
                   const char* name,
                   plume::RenderTextureFlags flags = plume::RenderTextureFlag::RENDER_TARGET);
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
  HostPrecache precache_;
  uint32_t image_failures_logged_ = 0;
};

}  // namespace mcla::gpu

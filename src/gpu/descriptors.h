#pragma once
// ===========================================================================
// MCLA GPU renderer -- the bindless descriptor heap
// ===========================================================================
// One texture descriptor set and one sampler descriptor set for the whole
// process, created once and bound by every pipeline that samples. A shader
// picks its texture and sampler by index (push constants today, the guest
// shader contract in M3), so binding a texture is writing one descriptor into a
// free slot, and nothing is ever rebound per draw.
//
//   textures: kBindlessTextureCount slots, one boundless range at binding 0.
//             Slots 0..2 hold null 2D/3D/Cube views (R8, swizzle 0000); every
//             released slot is rewritten to the null of its dimension, so a
//             stale index reads zeros instead of a destroyed resource.
//   samplers: kBindlessSamplerCount slots. Slots 0..3 are the four fixed
//             samplers (linear/point x clamp/wrap) the overlay asks for, as
//             the SDK's D3D12 immediate drawer does. Samplers built from guest
//             state get slots of their own with the sampler cache (M4).
//
// The texture set is the same set at every space a shader declares a texture
// array of a different dimension (reblue device_pipelines.cpp binds one set at
// spaces 0-2): a descriptor carries its own view dimension. Pipeline layouts
// add the sets through texture_set_builder()/sampler_set_builder(), the very
// builders the sets were created from -- Vulkan only accepts a set on a layout
// whose set layout is defined identically.
//
// Sizes: Plume's D3D12 device has one shader-visible view heap of 65536 and one
// sampler heap of 1024 for every set (plume_d3d12.cpp ShaderDescriptorHeapSize,
// SamplerDescriptorHeapSize). Half of each is taken here, as reeot does; the
// rest is left for the compute sets of untiling and resolve (M5). reblue takes
// the whole view heap.
//
// Release. A command list in flight may still index a slot, so a released slot
// is rewritten and returned to the free stack only once the FrameRing proves
// every use recorded so far complete (FrameRing::RetireAction). The caller
// retires the view and the texture after calling ReleaseTexture, so the slot
// is null before the resource dies.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

namespace mcla::gpu {

class Device;
class FrameRing;

inline constexpr uint32_t kBindlessTextureCount = 32768;
inline constexpr uint32_t kBindlessSamplerCount = 512;
inline constexpr uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;

enum class TextureDimension : uint8_t { k2D, k3D, kCube };

// Reserved texture slots.
inline constexpr uint32_t kNullTexture2DSlot = 0;
inline constexpr uint32_t kNullTexture3DSlot = 1;
inline constexpr uint32_t kNullTextureCubeSlot = 2;
inline constexpr uint32_t kFirstAllocatableTextureSlot = 3;

// Reserved sampler slots.
inline constexpr uint32_t kSamplerLinearClamp = 0;
inline constexpr uint32_t kSamplerLinearWrap = 1;
inline constexpr uint32_t kSamplerPointClamp = 2;
inline constexpr uint32_t kSamplerPointWrap = 3;
inline constexpr uint32_t kFixedSamplerCount = 4;

class BindlessHeap {
 public:
  BindlessHeap() = default;
  ~BindlessHeap();
  BindlessHeap(const BindlessHeap&) = delete;
  BindlessHeap& operator=(const BindlessHeap&) = delete;

  bool Initialize(Device& device, FrameRing& frames);
  // The GPU must be idle and the FrameRing drained (pending release actions
  // name this heap).
  void Shutdown();

  const plume::RenderDescriptorSetBuilder& texture_set_builder() const { return texture_builder_; }
  const plume::RenderDescriptorSetBuilder& sampler_set_builder() const { return sampler_builder_; }
  plume::RenderDescriptorSet* texture_set() const { return texture_set_.get(); }
  plume::RenderDescriptorSet* sampler_set() const { return sampler_set_.get(); }

  // Records the layout transitions the heap's own textures still need (the
  // nulls, once). Call outside a render pass, before the first draw that binds
  // the sets.
  void RecordPendingBarriers(plume::RenderCommandList* list);

  // Writes `view` of `texture` (already in SHADER_READ when it is sampled) into
  // a free slot. kInvalidBindlessSlot when every slot is taken.
  uint32_t AllocateTexture(const plume::RenderTexture& texture, const plume::RenderTextureView& view);
  // Defers the release of `slot` until the GPU is done with it (see top).
  void ReleaseTexture(uint32_t slot, TextureDimension dimension);

  // Writes `sampler` into the next free sampler slot. Guest samplers are few
  // (27 distinct in the measured frames) and are kept for the life
  // of the process, so a sampler slot is never released; the caller keeps the
  // sampler alive until the heap is shut down. kInvalidBindlessSlot when every
  // slot is taken (logged once).
  uint32_t AddSampler(const plume::RenderSampler& sampler);
  uint32_t samplers_in_use() const { return next_sampler_slot_; }

  uint32_t textures_in_use() const {
    return kBindlessTextureCount - kFirstAllocatableTextureSlot -
           static_cast<uint32_t>(free_texture_slots_.size());
  }

 private:
  bool CreateNullTextures();
  bool CreateFixedSamplers();
  void WriteNull(uint32_t slot, TextureDimension dimension);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  plume::RenderDescriptorSetBuilder texture_builder_;
  plume::RenderDescriptorSetBuilder sampler_builder_;
  std::unique_ptr<plume::RenderDescriptorSet> texture_set_;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_set_;
  // Indexed by TextureDimension.
  std::unique_ptr<plume::RenderTexture> null_textures_[3];
  std::unique_ptr<plume::RenderTextureView> null_views_[3];
  std::unique_ptr<plume::RenderSampler> fixed_samplers_[kFixedSamplerCount];
  std::vector<uint32_t> free_texture_slots_;  // stack; back() is the next slot handed out
  std::vector<uint8_t> slot_in_use_;          // per slot; catches a double release
  bool nulls_need_barrier_ = false;
  bool reported_full_ = false;
  uint32_t next_sampler_slot_ = kFixedSamplerCount;
  bool reported_samplers_full_ = false;
};

}  // namespace mcla::gpu

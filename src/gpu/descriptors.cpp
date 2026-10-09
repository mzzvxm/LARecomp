// MCLA GPU renderer -- the bindless descriptor heap. See descriptors.h.
//
// Design followed: reblue gpu/device_pipelines.cpp BuildPipelineLayout /
// BuildNullTextureDescriptors and gpu/bindless.cpp (BSD-3-Clause) -- one set
// created once, dimension-matched null descriptors, fence-deferred slot
// release. Differences: the release goes through FrameRing's serial instead of
// per-slot graveyards, every allocatable slot starts as a null, and a double
// release is refused instead of corrupting the free list.

#include "descriptors.h"

#include <rex/logging.h>

#include "device.h"
#include "frame.h"

namespace mcla::gpu {

namespace {

constexpr size_t DimensionIndex(TextureDimension dimension) {
  return static_cast<size_t>(dimension);
}

}  // namespace

BindlessHeap::~BindlessHeap() {
  Shutdown();
}

bool BindlessHeap::Initialize(Device& device, FrameRing& frames) {
  if (device_) {
    return true;
  }
  device_ = &device;
  frames_ = &frames;

  texture_builder_.begin();
  texture_builder_.addTexture(0, kBindlessTextureCount);
  texture_builder_.end(true, kBindlessTextureCount);
  texture_set_ = device.CreateDescriptorSet(texture_builder_, kBindlessTextureCount, 0,
                                            "bindless textures");

  sampler_builder_.begin();
  sampler_builder_.addSampler(0, kBindlessSamplerCount);
  sampler_builder_.end(true, kBindlessSamplerCount);
  sampler_set_ = device.CreateDescriptorSet(sampler_builder_, 0, kBindlessSamplerCount,
                                            "bindless samplers");

  if (!texture_set_ || !sampler_set_ || !CreateNullTextures() || !CreateFixedSamplers()) {
    Shutdown();
    return false;
  }

  // Every allocatable slot starts as the 2D null, so an index that was never
  // written reads zeros like a released one does, instead of whatever the
  // heap memory held.
  free_texture_slots_.clear();
  free_texture_slots_.reserve(kBindlessTextureCount - kFirstAllocatableTextureSlot);
  for (uint32_t slot = kBindlessTextureCount; slot-- > kFirstAllocatableTextureSlot;) {
    WriteNull(slot, TextureDimension::k2D);
    free_texture_slots_.push_back(slot);
  }
  slot_in_use_.assign(kBindlessTextureCount, 0);
  for (uint32_t slot = 0; slot < kFirstAllocatableTextureSlot; ++slot) {
    slot_in_use_[slot] = 1;
  }
  nulls_need_barrier_ = true;
  reported_full_ = false;
  REXLOG_INFO("[gpu] bindless heap: {} texture slots ({} reserved), {} sampler slots ({} fixed)",
              kBindlessTextureCount, kFirstAllocatableTextureSlot, kBindlessSamplerCount,
              kFixedSamplerCount);
  return true;
}

void BindlessHeap::Shutdown() {
  // Sets first: they name the views and samplers below.
  texture_set_.reset();
  sampler_set_.reset();
  for (auto& sampler : fixed_samplers_) {
    sampler.reset();
  }
  for (size_t i = 0; i < 3; ++i) {
    null_views_[i].reset();
    null_textures_[i].reset();
  }
  free_texture_slots_.clear();
  slot_in_use_.clear();
  nulls_need_barrier_ = false;
  device_ = nullptr;
  frames_ = nullptr;
}

bool BindlessHeap::CreateNullTextures() {
  struct NullSpec {
    TextureDimension dimension;
    uint32_t slot;
    const char* name;
  };
  static constexpr NullSpec kSpecs[] = {
      {TextureDimension::k2D, kNullTexture2DSlot, "bindless null 2D"},
      {TextureDimension::k3D, kNullTexture3DSlot, "bindless null 3D"},
      {TextureDimension::kCube, kNullTextureCubeSlot, "bindless null cube"},
  };
  for (const NullSpec& spec : kSpecs) {
    plume::RenderTextureDesc desc;
    plume::RenderTextureViewDesc view_desc;
    switch (spec.dimension) {
      case TextureDimension::k2D:
        desc = plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8_UNORM);
        view_desc = plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8_UNORM);
        break;
      case TextureDimension::k3D:
        desc = plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, plume::RenderFormat::R8_UNORM);
        view_desc = plume::RenderTextureViewDesc::Texture3D(plume::RenderFormat::R8_UNORM);
        break;
      case TextureDimension::kCube:
        desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, 1, 1,
                                                 1, 1, 6, plume::RenderFormat::R8_UNORM,
                                                 plume::RenderTextureFlag::CUBE);
        view_desc = plume::RenderTextureViewDesc::TextureCube(plume::RenderFormat::R8_UNORM);
        break;
    }
    // Committed: a placed resource that is never written trips the D3D12
    // debug layer's uninitialized-placed-resource fill (reblue: same reason).
    desc.committed = true;
    view_desc.mipLevels = 1;
    // The content is never written and never matters: every component reads 0.
    view_desc.componentMapping =
        plume::RenderComponentMapping(plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ZERO,
                                      plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ZERO);
    const size_t i = DimensionIndex(spec.dimension);
    null_textures_[i] = device_->CreateTexture(desc, spec.name);
    if (!null_textures_[i]) {
      return false;
    }
    null_views_[i] = device_->CreateTextureView(*null_textures_[i], view_desc, spec.name);
    if (!null_views_[i]) {
      return false;
    }
    WriteNull(spec.slot, spec.dimension);
  }
  return true;
}

bool BindlessHeap::CreateFixedSamplers() {
  struct SamplerSpec {
    uint32_t slot;
    plume::RenderFilter filter;
    plume::RenderMipmapMode mip;
    plume::RenderTextureAddressMode address;
    const char* name;
  };
  static constexpr SamplerSpec kSpecs[kFixedSamplerCount] = {
      {kSamplerLinearClamp, plume::RenderFilter::LINEAR, plume::RenderMipmapMode::LINEAR,
       plume::RenderTextureAddressMode::CLAMP, "sampler linear clamp"},
      {kSamplerLinearWrap, plume::RenderFilter::LINEAR, plume::RenderMipmapMode::LINEAR,
       plume::RenderTextureAddressMode::WRAP, "sampler linear wrap"},
      {kSamplerPointClamp, plume::RenderFilter::NEAREST, plume::RenderMipmapMode::NEAREST,
       plume::RenderTextureAddressMode::CLAMP, "sampler point clamp"},
      {kSamplerPointWrap, plume::RenderFilter::NEAREST, plume::RenderMipmapMode::NEAREST,
       plume::RenderTextureAddressMode::WRAP, "sampler point wrap"},
  };
  for (const SamplerSpec& spec : kSpecs) {
    plume::RenderSamplerDesc desc;
    desc.minFilter = spec.filter;
    desc.magFilter = spec.filter;
    desc.mipmapMode = spec.mip;
    desc.addressU = spec.address;
    desc.addressV = spec.address;
    desc.addressW = spec.address;
    desc.anisotropyEnabled = false;
    fixed_samplers_[spec.slot] = device_->CreateSampler(desc, spec.name);
    if (!fixed_samplers_[spec.slot]) {
      return false;
    }
    sampler_set_->setSampler(spec.slot, fixed_samplers_[spec.slot].get());
  }
  return true;
}

void BindlessHeap::WriteNull(uint32_t slot, TextureDimension dimension) {
  const size_t i = DimensionIndex(dimension);
  texture_set_->setTexture(slot, null_textures_[i].get(), plume::RenderTextureLayout::SHADER_READ,
                           null_views_[i].get());
}

void BindlessHeap::RecordPendingBarriers(plume::RenderCommandList* list) {
  if (!nulls_need_barrier_ || !list) {
    return;
  }
  plume::RenderTextureBarrier barriers[3];
  for (size_t i = 0; i < 3; ++i) {
    barriers[i] = plume::RenderTextureBarrier(null_textures_[i].get(),
                                              plume::RenderTextureLayout::SHADER_READ);
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, barriers, 3);
  nulls_need_barrier_ = false;
}

uint32_t BindlessHeap::AllocateTexture(const plume::RenderTexture& texture,
                                       const plume::RenderTextureView& view) {
  if (!texture_set_) {
    return kInvalidBindlessSlot;
  }
  if (free_texture_slots_.empty()) {
    if (!reported_full_) {
      reported_full_ = true;
      REXLOG_ERROR("[gpu] bindless heap full: all {} texture slots are in use; the texture is "
                   "not bound",
                   kBindlessTextureCount - kFirstAllocatableTextureSlot);
    }
    return kInvalidBindlessSlot;
  }
  const uint32_t slot = free_texture_slots_.back();
  free_texture_slots_.pop_back();
  slot_in_use_[slot] = 1;
  texture_set_->setTexture(slot, &texture, plume::RenderTextureLayout::SHADER_READ, &view);
  return slot;
}

void BindlessHeap::ReleaseTexture(uint32_t slot, TextureDimension dimension) {
  if (!frames_) {
    return;
  }
  if (slot < kFirstAllocatableTextureSlot || slot >= kBindlessTextureCount ||
      !slot_in_use_[slot]) {
    REXLOG_ERROR("[gpu] bindless heap: release of slot {} that is not allocated (ignored)", slot);
    return;
  }
  // Not handed out again before the action below returns it, so clearing the
  // flag now only serves to catch a second release of the same slot.
  slot_in_use_[slot] = 0;
  frames_->RetireAction([this, slot, dimension] {
    WriteNull(slot, dimension);
    free_texture_slots_.push_back(slot);
    reported_full_ = false;
  });
}

}  // namespace mcla::gpu

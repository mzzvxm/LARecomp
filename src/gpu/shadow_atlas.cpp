// MCLA GPU renderer -- the shadow atlas. See shadow_atlas.h and shadow_atlas_rules.h.

#include "shadow_atlas.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "local_shadow_rules.h"
#include "texture_bytes.h"
#include "vram_budget.h"

namespace mcla::gpu {

namespace {

namespace sa = shadow_atlas;

constexpr const char* kPrefix = "[shadow-atlas]";
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
// The format the replay's atlas always had: the renderer's own depth targets' (D32 float + S8), whose depth plane Plume views as one float on both backends. CHECK: D32 float alone would
// halve the memory (shadow_atlas_rules.h kTexelBytes is the other place that says 8 bytes).
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;

plume::RenderBarrierStages StagesOf(uint8_t stages) {
  plume::RenderBarrierStages out = plume::RenderBarrierStage::NONE;
  if ((stages & sa::kStageGraphics) != 0) {
    out |= plume::RenderBarrierStage::GRAPHICS;
  }
  if ((stages & sa::kStageCompute) != 0) {
    out |= plume::RenderBarrierStage::COMPUTE;
  }
  return out;
}

}  // namespace

ShadowAtlas::ShadowAtlas() = default;
ShadowAtlas::~ShadowAtlas() = default;

void ShadowAtlas::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
}

void ShadowAtlas::Shutdown() {
  // The GPU is idle and the heap goes later: the slot is not handed back (LocalShadow::Shutdown, CarShadowMap::Shutdown).
  framebuffer_.reset();
  view_.reset();
  texture_.reset();
  slot_ = kNoSlot;
  width_ = height_ = 0;
  layout_ = sa::Layout{};
  planned_ = sa::Layout{};
  published_ = sa::Published{};
  use_.Reset();
  providers_.fill(nullptr);
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
}

void ShadowAtlas::AddProvider(ShadowProvider* provider) {
  if (provider != nullptr) {
    providers_[uint32_t(provider->provider_id())] = provider;
  }
}

void ShadowAtlas::RemoveProvider(ShadowProvider* provider) {
  if (provider != nullptr && providers_[uint32_t(provider->provider_id())] == provider) {
    providers_[uint32_t(provider->provider_id())] = nullptr;
  }
}

uint64_t ShadowAtlas::HeldBytes() const { return texture_ ? TextureBytes(width_, height_, 1, kDepthFormat) : 0; }

// ---------------------------------------------------------------------------
// the plan and the texture
// ---------------------------------------------------------------------------

const sa::Layout& ShadowAtlas::Plan() {
  spec_ = sa::AtlasSpec{};
  for (ShadowProvider* p : providers_) {
    if (p != nullptr) {
      p->RequestPools(&spec_);
    }
  }
  planned_ = sa::MakeLayout(spec_);
  for (ShadowProvider* p : providers_) {
    if (p != nullptr) {
      p->OnLayout(planned_);
    }
  }
  return planned_;
}

bool ShadowAtlas::Ensure() {
  if (device_ == nullptr || !planned_.valid()) {
    return false;
  }
  if (texture_ && width_ == planned_.width && height_ == planned_.height) {
    if (!(layout_ == planned_)) {
      static_invalid_ = ~0u;  // the pools moved inside the same texture: whatever a static pool cached is not where it was
      layout_ = planned_;
      ++generation_;
    }
    return true;
  }
  // Another size (a level change, a lamp provider joining): the old texture goes after the lists that name it.
  const auto memory = device_->QueryVideoMemory();
  if (!VideoMemoryBudget::HasRoomFor(memory, sa::Bytes(planned_)) && !memory_warned_) {
    memory_warned_ = true;
    REXLOG_WARN("{} video memory is nearly full ({} of {} MiB in use); allocating the selected {}x{} shadow atlas",
                kPrefix, memory.used >> 20, memory.budget >> 20, planned_.width, planned_.height);
  }
  Release();
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(planned_.width, planned_.height, 1, kDepthFormat, plume::RenderTextureFlag::DEPTH_TARGET);
  desc.multisampling.sampleCount = plume::RenderSampleCounts(1);
  std::unique_ptr<plume::RenderTexture> texture = device_->CreateTexture(desc, "shadow atlas");
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t slot = kNoSlot;
  if (texture) {
    // The texture's own format: Plume views a depth format through its depth plane (R32_FLOAT_X8X24 on D3D12, the DEPTH aspect on Vulkan).
    plume::RenderTextureViewDesc view_desc = plume::RenderTextureViewDesc::Texture2D(kDepthFormat);
    view_desc.mipLevels = 1;
    view = device_->CreateTextureView(*texture, view_desc, "shadow atlas view");
    const plume::RenderTexture* none[1] = {nullptr};
    framebuffer = device_->CreateFramebuffer(plume::RenderFramebufferDesc(none, 0, texture.get()), "shadow atlas");
  }
  if (view) {
    slot = heap_->AllocateTexture(*texture, *view);
  }
  if (!texture || !view || !framebuffer || slot == kInvalidBindlessSlot) {
    if (!create_failed_logged_) {
      create_failed_logged_ = true;
      REXLOG_ERROR("{} the {}x{} atlas could not be made (reported once): no shadows from it while that lasts", kPrefix, planned_.width, planned_.height);
    }
    if (slot != kNoSlot && slot != kInvalidBindlessSlot) {
      heap_->ReleaseTexture(slot, TextureDimension::k2D);
    }
    return false;
  }
  texture_ = std::move(texture);
  view_ = std::move(view);
  framebuffer_ = std::move(framebuffer);
  slot_ = slot;
  width_ = planned_.width;
  height_ = planned_.height;
  layout_ = planned_;
  static_invalid_ = ~0u;
  use_.Reset();
  ++generation_;
  return true;
}

void ShadowAtlas::Release() {
  ++generation_;
  if (!device_ || !texture_) {
    framebuffer_.reset();
    view_.reset();
    texture_.reset();
    slot_ = kNoSlot;
    width_ = height_ = 0;
    layout_ = sa::Layout{};
    published_ = sa::Published{};
    use_.Reset();
    return;
  }
  if (slot_ != kNoSlot) {
    heap_->ReleaseTexture(slot_, TextureDimension::k2D);
  }
  frames_->Retire(std::move(framebuffer_));
  frames_->Retire(std::move(view_));
  device_->NoteReleased(TextureBytes(width_, height_, 1, kDepthFormat));
  frames_->Retire(std::move(texture_));
  slot_ = kNoSlot;
  width_ = height_ = 0;
  layout_ = sa::Layout{};
  published_ = sa::Published{};
  use_.Reset();
}

// ---------------------------------------------------------------------------
// the record pass: the one barrier
// ---------------------------------------------------------------------------

bool ShadowAtlas::BeginRecord(plume::RenderCommandList* list) {
  if (list == nullptr || !texture_ || !framebuffer_) {
    return false;
  }
  const sa::Barrier before = use_.BeginWrite();
  if (before.needed) {
    const plume::RenderTextureBarrier barrier(texture_.get(), plume::RenderTextureLayout::DEPTH_WRITE);
    list->barriers(StagesOf(before.stages), barrier);
  }
  list->setFramebuffer(framebuffer_.get());
  const sa::ClearPlan clear = sa::MakeClearPlan(layout_, static_invalid_);
  if (clear.whole_target) {
    list->clearDepthStencil(true, true, sa::kClearDepth, sa::kClearStencil);  // every pool is drawn every record: one clear of the texture (the replay's, as it always was)
  } else if (clear.rect_count != 0) {
    std::array<plume::RenderRect, sa::kMaxPools> rects{};
    for (uint32_t i = 0; i < clear.rect_count; ++i) {
      const sa::Area& a = clear.rects[i];
      rects[i] = plume::RenderRect(int32_t(a.x), int32_t(a.y), int32_t(a.x + a.w), int32_t(a.y + a.h));
    }
    list->clearDepthStencil(true, true, sa::kClearDepth, sa::kClearStencil, rects.data(), clear.rect_count);
  }
  // a static pool that was cleared is clean now: what is drawn into it from here is its map
  for (uint32_t i = 0; i < clear.rect_count; ++i) {
    static_invalid_ &= ~(1u << clear.pools[i]);
  }
  if (clear.whole_target) {
    static_invalid_ = 0;
  }
  return true;
}

void ShadowAtlas::SetTile(plume::RenderCommandList* list, const sa::Rect& tile) {
  if (list == nullptr || !tile.valid()) {
    return;
  }
  const plume::RenderViewport viewport(float(tile.x), float(tile.y), float(tile.size), float(tile.size), 0.0f, 1.0f);
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(int32_t(tile.x), int32_t(tile.y), int32_t(tile.x + tile.size), int32_t(tile.y + tile.size));
  list->setScissors(&scissor, 1);
}

void ShadowAtlas::ClearTile(plume::RenderCommandList* list, const sa::Rect& tile) {
  if (list == nullptr || !texture_ || !tile.valid()) {
    return;
  }
  const plume::RenderRect rect(int32_t(tile.x), int32_t(tile.y), int32_t(tile.x + tile.size), int32_t(tile.y + tile.size));
  list->clearDepthStencil(true, true, sa::kClearDepth, sa::kClearStencil, &rect, 1);
}

void ShadowAtlas::EndRecord(plume::RenderCommandList* list) {
  if (list == nullptr || !texture_) {
    return;
  }
  list->setFramebuffer(nullptr);
  // THE ONE BARRIER. The lit draws of the next frame (and the translucent ones after this) read the atlas as a texture, and the volumetric light's inject (a compute pass, later in this
  // frame) reads it too: both stages (D3D12: ALL_SHADER_RESOURCE, a compute shader may not read PIXEL_SHADER_RESOURCE; Vulkan: fragment and compute). A partial pass publishes too.
  const sa::Barrier after = use_.Publish();
  const plume::RenderTextureBarrier read(texture_.get(), plume::RenderTextureLayout::SHADER_READ);
  list->barriers(StagesOf(after.stages), read);
}

// ---------------------------------------------------------------------------
// the published form
// ---------------------------------------------------------------------------

void ShadowAtlas::Collect(uint32_t sampler) {
  sa::ShadowFrameBuilder builder;
  for (const ShadowProvider* p : providers_) {
    if (p != nullptr) {
      p->CollectViews(&builder);
    }
  }
  const bool any = builder.view_count() != 0;
  published_ = builder.Build(sa::InfoOf(layout_, slot_, sampler, texture_ != nullptr && any));
  if (!published_.frame.atlas.valid || published_.frame.count == 0) {
    // Nothing the readers can use: the empty frame, the atlas not named (the readers' own rule: no valid atlas or no view is no map at all).
    const sa::Published masks_only = published_;
    published_ = sa::Published{};
    published_.masks = masks_only.masks;
    published_.mask_count = masks_only.mask_count;
  }
}

void ShadowAtlas::ClearPublished() { published_ = sa::Published{}; }

std::string ShadowAtlas::Describe() const {
  if (!texture_) {
    return "no atlas";
  }
  std::string pools;
  for (uint32_t i = 0; i < layout_.pool_count; ++i) {
    const sa::PoolLayout& p = layout_.pools[i];
    pools += std::format("{}{} x {}x{} ({}{})", i ? ", " : "", p.request.slots, p.request.tile, p.request.tile, sa::ProviderName(p.request.provider), p.request.sticky ? ", sticky" : "");
  }
  return std::format("atlas {}x{} D32 float + S8 ({:.1f} MiB), slot {}, pools: {}", width_, height_, sa::Mib(HeldBytes()), slot_, pools);
}

}  // namespace mcla::gpu

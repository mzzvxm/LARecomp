// MCLA GPU renderer -- the guest's EDRAM render targets on the host. See
// edram.h.

#include "edram.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <rex/hash.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "format.h"
#include "frame.h"

namespace mcla::gpu {

namespace {

constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

uint32_t BytesPerPixel(plume::RenderFormat format) {
  switch (format) {
    case plume::RenderFormat::R16G16B16A16_FLOAT:
    case plume::RenderFormat::R16G16B16A16_SNORM:
    case plume::RenderFormat::R32G32_FLOAT:
    case plume::RenderFormat::D32_FLOAT_S8_UINT:
      return 8;
    default:
      return 4;
  }
}

}  // namespace

uint32_t FoldColorFormat(uint32_t color_format) {
  switch (color_format) {
    case 10: return 2;  // k_2_10_10_10_AS_10_10_10_10
    case 12: return 3;  // k_2_10_10_10_FLOAT_AS_16_16_16_16
    default: return color_format;
  }
}

void EncodeColorClear(uint32_t color_format, int32_t exp_bias, const float color[4],
                      float out[4]) {
  float scale = std::ldexp(1.0f, exp_bias);
  if (color_format == 4 || color_format == 5) {
    scale *= 1.0f / 32.0f;  // k_16_16, k_16_16_16_16: SNORM +-1 on the host = +-32
  }
  for (uint32_t c = 0; c < 4; ++c) {
    out[c] = color[c] * scale;
    if (color_format == 1) {
      out[c] = std::clamp(out[c], 0.0f, 1.0f);  // k_8_8_8_8_GAMMA, kept linear
    }
  }
}

size_t EdramTargets::KeyHash::operator()(const EdramKey& k) const {
  const uint64_t v = uint64_t(k.base_tiles) | (uint64_t(k.pitch) << 16) |
                     (uint64_t(k.depth) << 32) | (uint64_t(k.format) << 40) |
                     (uint64_t(k.samples) << 48);
  return size_t(v * 0x9E3779B97F4A7C15ull);
}

bool EdramTargets::FramebufferKey::operator==(const FramebufferKey& o) const {
  if (color_count != o.color_count) {
    return false;
  }
  for (uint32_t i = 0; i < 5; ++i) {
    if (targets[i] != o.targets[i] || generations[i] != o.generations[i]) {
      return false;
    }
  }
  return true;
}

size_t EdramTargets::FramebufferKeyHash::operator()(const FramebufferKey& k) const {
  return size_t(XXH3_64bits(&k, sizeof(k)));
}

EdramTargets::~EdramTargets() {
  Shutdown();
}

void EdramTargets::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
}

void EdramTargets::Shutdown() {
  // The GPU is idle and the heap goes next: the views die without handing
  // their slots back.
  framebuffers_.clear();
  targets_.clear();
  failed_.clear();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
}

void EdramTargets::ReleaseViews(Target& t) {
  if (t.sample_slot != 0xFFFFFFFFu) {
    heap_->ReleaseTexture(t.sample_slot, TextureDimension::k2D);
    t.sample_slot = 0xFFFFFFFFu;
  }
  if (t.stencil_slot != 0xFFFFFFFFu) {
    heap_->ReleaseTexture(t.stencil_slot, TextureDimension::k2D);
    t.stencil_slot = 0xFFFFFFFFu;
  }
  frames_->Retire(std::move(t.sample_view));
  frames_->Retire(std::move(t.stencil_view));
}

bool EdramTargets::SampleSlots(Target& t, uint32_t* sample_slot, uint32_t* stencil_slot) {
  if (!t.texture) {
    return false;
  }
  if (t.views_generation != t.generation) {
    ReleaseViews(t);
    t.views_generation = t.generation;
  }
  if (t.sample_slot == 0xFFFFFFFFu) {
    // The texture's own format: Plume views a depth format through its depth
    // plane (R32_FLOAT_X8X24 / R24_UNORM_X8 on D3D12, the DEPTH aspect on Vulkan).
    plume::RenderTextureViewDesc desc = plume::RenderTextureViewDesc::Texture2D(t.format);
    desc.mipLevels = 1;
    t.sample_view = device_->CreateTextureView(*t.texture, desc, "edram shader view");
    if (!t.sample_view) {
      return false;
    }
    t.sample_slot = heap_->AllocateTexture(*t.texture, *t.sample_view);
    if (t.sample_slot == 0xFFFFFFFFu) {
      return false;
    }
  }
  if (t.key.depth && t.stencil_slot == 0xFFFFFFFFu) {
    plume::RenderTextureViewDesc desc = plume::RenderTextureViewDesc::Texture2D(t.format);
    desc.mipLevels = 1;
    desc.planeSlice = 1;
    t.stencil_view = device_->CreateTextureView(*t.texture, desc, "edram stencil view");
    if (!t.stencil_view) {
      return false;
    }
    t.stencil_slot = heap_->AllocateTexture(*t.texture, *t.stencil_view);
    if (t.stencil_slot == 0xFFFFFFFFu) {
      return false;
    }
  }
  *sample_slot = t.sample_slot;
  *stencil_slot = t.key.depth ? t.stencil_slot : 0xFFFFFFFFu;
  return true;
}

plume::RenderFramebuffer* EdramTargets::ClearColor(plume::RenderCommandList* list, Target& t,
                                                   plume::RenderRect rect, const float value[4]) {
  rect.right = std::min(rect.right, int32_t(t.width));
  rect.bottom = std::min(rect.bottom, int32_t(t.height));
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return nullptr;
  }
  Transition(list, t, plume::RenderTextureLayout::COLOR_WRITE);
  Target* only[1] = {&t};
  plume::RenderFramebuffer* framebuffer = Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return nullptr;
  }
  list->setFramebuffer(framebuffer);
  list->clearColor(0, plume::RenderColor(value[0], value[1], value[2], value[3]), &rect, 1);
  return framebuffer;
}

plume::RenderFramebuffer* EdramTargets::ClearDepthStencil(plume::RenderCommandList* list,
                                                          Target& t, plume::RenderRect rect,
                                                          bool depth, bool stencil, float z,
                                                          uint32_t stencil_value) {
  rect.right = std::min(rect.right, int32_t(t.width));
  rect.bottom = std::min(rect.bottom, int32_t(t.height));
  if (rect.right <= rect.left || rect.bottom <= rect.top || (!depth && !stencil)) {
    return nullptr;
  }
  Transition(list, t, plume::RenderTextureLayout::DEPTH_WRITE);
  plume::RenderFramebuffer* framebuffer = Framebuffer(nullptr, 0, &t);
  if (!framebuffer) {
    return nullptr;
  }
  list->setFramebuffer(framebuffer);
  list->clearDepthStencil(depth, stencil, z, stencil_value & 0xFFu, &rect, 1);
  return framebuffer;
}

bool EdramTargets::Create(Target& target, uint32_t height) {
  const EdramKey& k = target.key;
  const format::Mapping m = k.depth ? format::DepthTargetFormat(k.format)
                                    : format::ColorTargetFormat(k.format);
  if (m.status != format::Status::kOk) {
    return false;
  }
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      k.pitch, height, 1, m.format,
      k.depth ? plume::RenderTextureFlag::DEPTH_TARGET : plume::RenderTextureFlag::RENDER_TARGET);
  desc.multisampling.sampleCount = plume::RenderSampleCounts(k.samples);
  const std::string name =
      std::format("edram {} {}@{} pitch {} {}x", k.depth ? "depth" : "colour", m.guest_name,
                  k.base_tiles, k.pitch, k.samples);
  std::unique_ptr<plume::RenderTexture> texture = device_->CreateTexture(desc, name.c_str());
  if (!texture) {
    return false;
  }
  target.texture = std::move(texture);
  target.format = m.format;
  target.width = k.pitch;
  target.height = height;
  target.layout = plume::RenderTextureLayout::UNKNOWN;
  ++target.generation;
  return true;
}

void EdramTargets::Clear(Target& target, plume::RenderCommandList* list) {
  if (target.key.depth) {
    Transition(list, target, plume::RenderTextureLayout::DEPTH_WRITE);
    plume::RenderFramebuffer* framebuffer = Framebuffer(nullptr, 0, &target);
    if (framebuffer) {
      list->setFramebuffer(framebuffer);
      list->clearDepthStencil(true, true, 0.0f, 0);
    }
  } else {
    Transition(list, target, plume::RenderTextureLayout::COLOR_WRITE);
    Target* only[1] = {&target};
    plume::RenderFramebuffer* framebuffer = Framebuffer(only, 1, nullptr);
    if (framebuffer) {
      list->setFramebuffer(framebuffer);
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    }
  }
  list->setFramebuffer(nullptr);
  ++stats_.cleared;
}

EdramTargets::Target* EdramTargets::Get(const EdramKey& key, uint32_t height,
                                        plume::RenderCommandList* list, bool* framebuffer_unset) {
  if (key.pitch == 0 || key.pitch > kMaxExtent) {
    return nullptr;
  }
  const uint32_t wanted =
      std::min(AlignUp(std::max<uint32_t>(height, 1), kHeightAlignment), kMaxExtent);
  auto it = targets_.find(key);
  if (it == targets_.end()) {
    if (std::find(failed_.begin(), failed_.end(), key) != failed_.end()) {
      return nullptr;
    }
    auto target = std::make_unique<Target>();
    target->key = key;
    if (!Create(*target, wanted)) {
      failed_.push_back(key);
      REXLOG_ERROR("[gpu] edram: no host target for {} format {} at tile {} pitch {} {}x "
                   "(reported once)",
                   key.depth ? "depth" : "colour", key.format, key.base_tiles, key.pitch,
                   key.samples);
      return nullptr;
    }
    ++stats_.created;
    it = targets_.emplace(key, std::move(target)).first;
    Clear(*it->second, list);
    *framebuffer_unset = true;
  }
  Target& t = *it->second;
  t.last_frame = frame_;
  if (t.height >= wanted) {
    return &t;
  }
  // Grow: a new texture, the old one copied into its top rows.
  std::unique_ptr<plume::RenderTexture> old = std::move(t.texture);
  const plume::RenderTextureLayout old_layout = t.layout;
  const uint32_t old_height = t.height;
  if (!Create(t, wanted)) {
    t.texture = std::move(old);
    t.height = old_height;
    t.layout = old_layout;
    REXLOG_ERROR("[gpu] edram: could not grow target at tile {} pitch {} to {} rows",
                 key.base_tiles, key.pitch, wanted);
    return &t;  // the old size still draws everything but the new rows
  }
  // The whole new texture is initialized, then the old rows copied over it.
  Clear(t, list);
  *framebuffer_unset = true;
  {
    const plume::RenderTextureBarrier before[] = {
        plume::RenderTextureBarrier(old.get(), plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST)};
    list->barriers(plume::RenderBarrierStage::COPY, before, 2);
    if (key.depth) {
      // Depth and stencil are separate planes on D3D12; copied one by one.
      for (uint32_t plane = 0; plane < 2; ++plane) {
        list->copyTextureRegion(
            plume::RenderTextureCopyLocation::SubresourcePlane(t.texture.get(), 0, 0, plane),
            plume::RenderTextureCopyLocation::SubresourcePlane(old.get(), 0, 0, plane), 0, 0, 0,
            nullptr);
      }
    } else {
      list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get()),
                              plume::RenderTextureCopyLocation::Subresource(old.get()), 0, 0, 0,
                              nullptr);
    }
    t.layout = plume::RenderTextureLayout::COPY_DEST;
    stats_.barriers += 2;
  }
  ++stats_.grown;
  stats_.grown_bytes += uint64_t(t.width) * (wanted - old_height) * BytesPerPixel(t.format) *
                        t.key.samples;
  RetireFramebuffersOf(&t);
  // Views of the old texture: their slots go null before it dies.
  ReleaseViews(t);
  t.views_generation = t.generation;
  frames_->Retire(std::move(old));
  return &t;
}

void EdramTargets::Transition(plume::RenderCommandList* list, Target& target,
                              plume::RenderTextureLayout layout) {
  if (target.layout == layout || !target.texture) {
    return;
  }
  const plume::RenderTextureBarrier barrier(target.texture.get(), layout);
  // A target read as a texture is read by the resolve passes (graphics) and by
  // the resolve memory's depth encode (compute).
  const plume::RenderBarrierStages stages =
      layout == plume::RenderTextureLayout::SHADER_READ
          ? plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS |
                                       plume::RenderBarrierStage::COMPUTE)
          : plume::RenderBarrierStages(plume::RenderBarrierStage::GRAPHICS);
  list->barriers(stages, &barrier, 1);
  target.layout = layout;
  ++stats_.barriers;
}

plume::RenderFramebuffer* EdramTargets::Framebuffer(Target* const* colors, uint32_t color_count,
                                                    Target* depth) {
  FramebufferKey key;
  key.color_count = color_count;
  for (uint32_t i = 0; i < color_count && i < 4; ++i) {
    key.targets[i] = colors[i];
    key.generations[i] = colors[i] ? colors[i]->generation : 0;
  }
  key.targets[4] = depth;
  key.generations[4] = depth ? depth->generation : 0;
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    return it->second.get();
  }
  const plume::RenderTexture* attachments[4] = {};
  for (uint32_t i = 0; i < color_count && i < 4; ++i) {
    attachments[i] = colors[i]->texture.get();
  }
  plume::RenderFramebufferDesc desc(attachments, color_count,
                                    depth ? depth->texture.get() : nullptr);
  std::unique_ptr<plume::RenderFramebuffer> framebuffer =
      device_->CreateFramebuffer(desc, "edram framebuffer");
  if (!framebuffer) {
    return nullptr;
  }
  ++stats_.framebuffers_created;
  plume::RenderFramebuffer* raw = framebuffer.get();
  framebuffers_.emplace(key, std::move(framebuffer));
  return raw;
}

void EdramTargets::RetireFramebuffersOf(const Target* target) {
  for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
    bool names = false;
    for (const Target* t : it->first.targets) {
      names |= t == target;
    }
    if (names) {
      frames_->Retire(std::move(it->second));
      it = framebuffers_.erase(it);
    } else {
      ++it;
    }
  }
}

std::string EdramTargets::TakeSummary() {
  uint64_t bytes = 0;
  for (const auto& [key, target] : targets_) {
    bytes += uint64_t(target->width) * target->height * BytesPerPixel(target->format) *
             target->key.samples;
  }
  std::string text = std::format(
      "edram: {} targets ({:.1f} MiB), {} framebuffers | created {}, grown {} ({:.1f} MiB "
      "copied), initialized {}, framebuffers made {}, {} barriers",
      targets_.size(), double(bytes) / 1048576.0, framebuffers_.size(), stats_.created,
      stats_.grown, double(stats_.grown_bytes) / 1048576.0, stats_.cleared,
      stats_.framebuffers_created, stats_.barriers);
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

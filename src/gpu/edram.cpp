// MCLA GPU renderer -- the guest's EDRAM render targets on the host. See
// edram.h.

#include "edram.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "format.h"
#include "frame.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_edram_aliasing, false, "MCLA/GPU",
    "EDRAM inheritance detector (M5.2d): per EDRAM "
    "tile and aspect, the identity that wrote it last, reporting a draw or a resolve that would "
    "see another identity's bytes on the console -- which the host does not carry over. Off: "
    "none of its work is done.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_coverage_queries, false, "MCLA/GPU",
    "With mcla_gpu_edram_aliasing: an occlusion query around each opaque draw the detector "
    "takes to cover its scissor, checking that it did. Costs GPU time, for "
    "checking a report. Off: no query pool is made, read or reset, and such takes go "
    "unchecked (listed once per pair).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_gpu_edram_capture, 0, "MCLA/GPU",
    "With mcla_gpu_edram_aliasing: a RenderDoc capture of the frame after the first report of "
    "each kind picked by these bits, 4 at most: 1 a pair met by a write that keeps "
    "or blends another identity's bytes, 2 a pair met by a read, 4 the first reader of "
    "inherited bytes of each kind. 0: none. Needs the game started under RenderDoc.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_gpu_ghost_log_interval, 0, "MCLA/GPU",
    "Diagnostic: log one live frame in every N with the 1280x720 scene clears "
    "and three-stripe resolves, plus all EDRAM target creations and growths. 0: off; 1: every "
    "frame. Hot reload for reproducing the frame ghost.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(
    mcla_gpu_resolution_scale, 1, "MCLA/GPU",
    "Internal resolution scale (M6b): the host render targets of the scene are this "
    "many times the guest's in each direction (1, 2 or 3; 2 = 2560x1440, 3 = 3840x2160 from "
    "720p). The guest keeps seeing its own sizes. M6b in progress: keep 1.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_scale_shadow, false, "MCLA/GPU",
    "With mcla_gpu_resolution_scale above 1: the sun's shadow map (pitch 640 4x) at "
    "the internal scale too -- sharper shadows, more GPU time and memory. Off: the guest's size.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_scale_reflection, false, "MCLA/GPU",
    "With mcla_gpu_resolution_scale above 1: the reflection pass and the night shadow "
    "that shares its depth (pitch 520 4x) at the internal scale too. Off: the guest's size.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_scale_post, false, "MCLA/GPU",
    "With mcla_gpu_resolution_scale above 1: the bloom chain (colour 1x, pitch 640 "
    "and 320) at the internal scale too. Off: the guest's size.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DECLARE(bool, mcla_gpu_work_17oct);  // scene.cpp: the 17/10 work on or off

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

EdramTargets::ScaleClass EdramTargets::ClassOf(const EdramKey& key) {
  // The census of passes: what draws into each identity.
  constexpr uint8_t k2101010Float = 3;  // xenos ColorRenderTargetFormat
  constexpr uint8_t k16161616Float = 7;
  if (key.pitch == kScenePitch) {
    return ScaleClass::kScene;
  }
  if (key.samples == 4 && key.pitch == 640) {
    return ScaleClass::kShadow;  // kD24S8@0, PS_ShadowDepthAlpha
  }
  if (key.samples == 4 && key.pitch == 520) {
    return ScaleClass::kReflection;  // the *Env pass and PS_ShadowNight, one depth
  }
  if (key.samples == 1 && !key.depth &&
      ((key.pitch == 640 && (key.format == k2101010Float || key.format == k16161616Float)) ||
       (key.pitch == 320 && key.format == k2101010Float))) {
    return ScaleClass::kPost;  // PSInitialScaleBuffer, PSStreakMotionBlur, PSBrightPass, PSGauss*
  }
  return ScaleClass::kBase;
}

const char* EdramTargets::ScaleClassName(ScaleClass c) {
  switch (c) {
    case ScaleClass::kBase: return "base";
    case ScaleClass::kScene: return "scene";
    case ScaleClass::kShadow: return "shadow";
    case ScaleClass::kReflection: return "reflection";
    case ScaleClass::kPost: return "post";
  }
  return "?";
}

std::string DescribeEdramKey(const EdramKey& k) {
  const format::Mapping m = k.depth ? format::DepthTargetFormat(k.format)
                                    : format::ColorTargetFormat(k.format);
  return std::format("{} {}@{} pitch {} {}x", k.depth ? "depth" : "colour", m.guest_name,
                     k.base_tiles, k.pitch, k.samples);
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
  // The internal resolution scale, chosen at start.
  resolution_scale_ = REXCVAR_GET(mcla_gpu_resolution_scale);
  if (resolution_scale_ < 1 || resolution_scale_ > kMaxResolutionScale) {
    REXLOG_ERROR("[gpu] edram: mcla_gpu_resolution_scale = {} is not 1 to {}; the targets keep "
                 "the guest's size",
                 resolution_scale_, kMaxResolutionScale);
    resolution_scale_ = 1;
  }
  if (resolution_scale_ > 1) {
    REXLOG_INFO("[gpu] edram: the scene's targets at {}x the guest's size",
                resolution_scale_);
  }
  // The classes: the scene's always, the others by their cvars --
  // and only with mcla_gpu_work_17oct on at start (off: as before 17/10).
  class_scale_.fill(1);
  class_scale_[size_t(ScaleClass::kScene)] = resolution_scale_;
  const bool work_17_10 = REXCVAR_GET(mcla_gpu_work_17oct);
  const std::pair<ScaleClass, bool> classes[] = {
      {ScaleClass::kShadow, REXCVAR_GET(mcla_gpu_scale_shadow)},
      {ScaleClass::kReflection, REXCVAR_GET(mcla_gpu_scale_reflection)},
      {ScaleClass::kPost, REXCVAR_GET(mcla_gpu_scale_post)},
  };
  for (const auto& [c, on] : classes) {
    if (on && work_17_10 && resolution_scale_ > 1) {
      class_scale_[size_t(c)] = resolution_scale_;
      REXLOG_INFO("[gpu] edram: the {} targets at {}x too", ScaleClassName(c),
                  resolution_scale_);
    }
  }
  // The detector and its checks are chosen at start.
  aliasing_ = REXCVAR_GET(mcla_gpu_edram_aliasing);
  if (!aliasing_) {
    return;
  }
  for (auto& owners : tile_owner_) {
    owners.fill(kNoOwner);
  }
  capture_ = REXCVAR_GET(mcla_gpu_edram_capture);
  if (!REXCVAR_GET(mcla_gpu_coverage_queries)) {
    REXLOG_INFO("[gpu] edram: inheritance detector on; opaque draws it takes to cover their "
                "scissor go unchecked (mcla_gpu_coverage_queries off)");
    return;
  }
  if (!device.capabilities().occlusionQueries) {
    REXLOG_ERROR("[gpu] edram: mcla_gpu_coverage_queries needs occlusion queries that count "
                 "samples exactly, which this device has not: opaque draws the detector takes "
                 "to cover their scissor go unchecked");
    return;
  }
  coverage_pools_.resize(kFramesInFlight);
  coverage_checks_.resize(kFramesInFlight);
  coverage_capacity_ = 256;
  coverage_ = true;
  for (uint32_t slot = 0; slot < kFramesInFlight && coverage_; ++slot) {
    coverage_pools_[slot] = device.CreateQueryPool(coverage_capacity_, "edram coverage checks",
                                                   plume::RenderQueryType::OCCLUSION);
    coverage_ = coverage_pools_[slot] != nullptr;
  }
  if (!coverage_) {
    coverage_pools_.clear();
    coverage_checks_.clear();
    REXLOG_ERROR("[gpu] edram: the coverage checks' query pools could not be made: opaque draws "
                 "the detector takes to cover their scissor go unchecked");
  }
}

void EdramTargets::Shutdown() {
  // The GPU is idle and the heap goes next: the views die without handing
  // their slots back.
  framebuffers_.clear();
  targets_.clear();
  failed_.clear();
  ++change_serial_;
  for (auto& owners : tile_owner_) {
    owners.fill(kNoOwner);
  }
  owner_keys_.clear();
  epoch_ = 1;
  reported_pairs_.clear();
  listed_takes_.clear();
  for (auto& taint : tile_taint_) {
    taint.fill(0);
  }
  inherited_resolves_.clear();
  reach_reported_.clear();
  tainted_reported_.clear();
  reach_seen_.fill(false);
  request_capture_ = nullptr;
  capture_ = 0;
  captures_ = 0;
  tiling_ = false;
  pending_.clear();
  coverage_pools_.clear();
  coverage_checks_.clear();
  coverage_capacity_ = 0;
  coverage_wanted_ = 0;
  coverage_reported_.clear();
  aliasing_ = false;
  coverage_ = false;
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
  if (aliasing_) {
    // Every bit of every texel in the rectangle: its tiles are this
    // identity's from now on.
    Accesses access;
    access.aspect[kMain] = Access::kTake;
    access.aspect[kStencil] = Access::kTake;
    if (!Settled(t, rect, access)) {
      EdramEvent event;
      event.kind = EdramEvent::Kind::kClear;
      Note(t, rect, access, event);
    }
  }
  Transition(list, t, plume::RenderTextureLayout::COLOR_WRITE);
  Target* only[1] = {&t};
  plume::RenderFramebuffer* framebuffer = Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return nullptr;
  }
  list->setFramebuffer(framebuffer);
  // The rectangle is the guest's (the tile map above); the host clears it
  // at the target's scale.
  const plume::RenderRect host = ScaledRect(rect, t.scale);
  list->clearColor(0, plume::RenderColor(value[0], value[1], value[2], value[3]), &host, 1);
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
  if (aliasing_) {
    // Each aspect cleared is this identity's from now on; one not cleared
    // keeps its owner.
    Accesses access;
    access.aspect[kMain] = depth ? Access::kTake : Access::kNone;
    access.aspect[kStencil] = stencil ? Access::kTake : Access::kNone;
    if (!Settled(t, rect, access)) {
      EdramEvent event;
      event.kind = EdramEvent::Kind::kClear;
      Note(t, rect, access, event);
    }
  }
  Transition(list, t, plume::RenderTextureLayout::DEPTH_WRITE);
  plume::RenderFramebuffer* framebuffer = Framebuffer(nullptr, 0, &t);
  if (!framebuffer) {
    return nullptr;
  }
  list->setFramebuffer(framebuffer);
  const plume::RenderRect host = ScaledRect(rect, t.scale);
  list->clearDepthStencil(depth, stencil, z, stencil_value & 0xFFu, &host, 1);
  return framebuffer;
}

bool EdramTargets::Create(Target& target, uint32_t height) {
  const EdramKey& k = target.key;
  const format::Mapping m = k.depth ? format::DepthTargetFormat(k.format)
                                    : format::ColorTargetFormat(k.format);
  if (m.status != format::Status::kOk) {
    return false;
  }
  // The host texture: the guest's size times the target's scale.
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      k.pitch * target.scale, height * target.scale, 1, m.format,
      k.depth ? plume::RenderTextureFlag::DEPTH_TARGET : plume::RenderTextureFlag::RENDER_TARGET);
  desc.multisampling.sampleCount = plume::RenderSampleCounts(k.samples);
  const std::string name =
      std::format("edram {} {}@{} pitch {} {}x scale {}", k.depth ? "depth" : "colour",
                  m.guest_name, k.base_tiles, k.pitch, k.samples, target.scale);
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
  ++change_serial_;
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
    target->scale = ScaleFor(key);
    if (!Create(*target, wanted)) {
      failed_.push_back(key);
      REXLOG_ERROR("[gpu] edram: no host target for {} format {} at tile {} pitch {} {}x "
                   "(reported once)",
                   key.depth ? "depth" : "colour", key.format, key.base_tiles, key.pitch,
                   key.samples);
      return nullptr;
    }
    ++stats_.created;
    AssignOwner(*target);
    it = targets_.emplace(key, std::move(target)).first;
    if (REXCVAR_GET(mcla_gpu_ghost_log_interval) != 0) {
      const Target& made = *it->second;
      REXLOG_INFO("[gpu] ghost target create frame {} {} target {} texture {} height {} gen {}",
                  frame_, DescribeEdramKey(key), static_cast<const void*>(&made),
                  static_cast<const void*>(made.texture.get()), made.height, made.generation);
    }
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
  if (REXCVAR_GET(mcla_gpu_ghost_log_interval) != 0) {
    REXLOG_INFO("[gpu] ghost target grow frame {} {} target {} texture {} -> {} height {} -> {} "
                "gen {}",
                frame_, DescribeEdramKey(key), static_cast<const void*>(&t),
                static_cast<const void*>(old.get()), static_cast<const void*>(t.texture.get()),
                old_height, t.height, t.generation);
  }
  stats_.grown_bytes += uint64_t(t.width) * (wanted - old_height) * BytesPerPixel(t.format) *
                        t.key.samples;
  RetireFramebuffersOf(&t);
  // Views of the old texture: their slots go null before it dies.
  ReleaseViews(t);
  t.views_generation = t.generation;
  frames_->Retire(std::move(old));
  return &t;
}

void EdramTargets::AssignOwner(Target& t) {
  if (!aliasing_) {
    return;
  }
  // Tile columns of one row of tiles: the pitch in samples over 80 at 32 bpp,
  // doubled for a 64 bpp format (Xenia's RenderTargetKey::GetPitchTiles). 4x
  // doubles the samples across, 2x and 4x down.
  const uint32_t across = t.key.samples >= 4 ? 2 : 1;
  const bool wide = !t.key.depth && rex::graphics::xenos::IsColorRenderTargetFormat64bpp(
                                        rex::graphics::xenos::ColorRenderTargetFormat(t.key.format));
  t.columns_per_texel = wide ? 2 : 1;
  t.pitch_tiles = ((uint32_t(t.key.pitch) * across + 79) / 80) * t.columns_per_texel;
  if (owner_keys_.size() >= kNoOwner) {
    if (!owners_exhausted_logged_) {
      owners_exhausted_logged_ = true;
      REXLOG_ERROR("[gpu] edram: more than {} identities; the newer ones are not watched for "
                   "EDRAM inheritance (reported once)",
                   uint32_t(kNoOwner));
    }
    return;
  }
  t.owner = uint16_t(owner_keys_.size());
  owner_keys_.push_back(t.key);
}

void EdramTargets::TileSpan(const Target& t, const plume::RenderRect& px, uint32_t* col0,
                            uint32_t* col1, uint32_t* row0, uint32_t* row1) {
  // The rectangle in the identity's tiles (80x16 samples).
  const uint32_t across = (t.key.samples >= 4 ? 2 : 1) * t.columns_per_texel;
  const uint32_t down = t.key.samples >= 2 ? 2 : 1;
  *col0 = uint32_t(std::max(px.left, 0)) * across / 80;
  *col1 = std::min((uint32_t(std::max(px.right, 0)) * across + 79) / 80, t.pitch_tiles);
  *row0 = uint32_t(std::max(px.top, 0)) * down / 16;
  *row1 = (uint32_t(std::max(px.bottom, 0)) * down + 15) / 16;
}

EdramTargets::Inheritance EdramTargets::Apply(Target& t, const plume::RenderRect& px,
                                              const Accesses& access) {
  Inheritance result;
  if (t.owner == kNoOwner || px.right <= px.left || px.bottom <= px.top) {
    return result;
  }
  uint32_t col0 = 0, col1 = 0, row0 = 0, row1 = 0;
  TileSpan(t, px, &col0, &col1, &row0, &row1);
  ++stats_.walks;
  bool changed = false;
  bool own[kAspectCount] = {true, true};
  uint32_t depend_tiles = 0;
  uint32_t read_tiles = 0;
  uint32_t tainted_tiles = 0;
  bool tainted_read = false;
  uint16_t from = kNoOwner;
  uint16_t taken_from = kNoOwner;
  for (uint32_t a = 0; a < kAspectCount; ++a) {
    const Access acc = access.aspect[a];
    if (acc == Access::kNone) {
      continue;
    }
    std::array<uint16_t, kTileCount>& owners = tile_owner_[a];
    std::array<uint8_t, kTileCount>& taint = tile_taint_[a];
    for (uint32_t row = row0; row < row1; ++row) {
      // The EDRAM's addressing wraps at its last tile.
      const uint32_t first = uint32_t(t.key.base_tiles) + row * t.pitch_tiles;
      for (uint32_t col = col0; col < col1; ++col) {
        const uint32_t tile = (first + col) & (kTileCount - 1);
        uint16_t& owner = owners[tile];
        uint8_t& marked = taint[tile];
        if (acc == Access::kTake) {
          // Every texel replaced: whatever the tile held is gone.
          if (marked) {
            marked = 0;
            ++stats_.untainted;
            changed = true;
          }
          if (owner == t.owner) {
            continue;
          }
          if (owner != kNoOwner) {
            ++stats_.tiles_taken;
            ++result.taken;
            if (taken_from == kNoOwner) {
              taken_from = owner;
              result.taken_owner = owner;
            }
          }
          owner = t.owner;
          changed = true;
          continue;
        }
        // kDepend or kRead: what the tile holds may reach the result.
        if (owner == t.owner) {
          if (marked) {
            // Its own tile, holding another identity's bytes since a write
            // kept them: walked again until something takes it.
            ++tainted_tiles;
            tainted_read |= acc == Access::kRead;
            own[a] = false;
          }
          continue;
        }
        if (owner != kNoOwner) {
          ++(acc == Access::kRead ? read_tiles : depend_tiles);
          if (from == kNoOwner) {
            from = owner;
            result.aspect = Aspect(a);
          }
        }
        if (acc == Access::kRead) {
          own[a] = false;
        } else {
          if (owner != kNoOwner) {
            // It keeps or blends another identity's bytes: they stay in the
            // tile (an unowned tile holds the EDRAM's undefined first bytes,
            // no identity's).
            if (!marked) {
              marked = 1;
              ++stats_.tainted;
            }
            own[a] = false;
          }
          owner = t.owner;
          changed = true;
        }
      }
    }
  }
  if (changed) {
    ++epoch_;
  }
  result.tainted = tainted_tiles;
  stats_.tainted_reads += tainted_tiles;
  if (tainted_read && tainted_reported_.size() < kReportedPairs &&
      std::find(tainted_reported_.begin(), tainted_reported_.end(), t.owner) ==
          tainted_reported_.end()) {
    tainted_reported_.push_back(t.owner);
    result.tainted_new = true;
  }
  // What is known now, per aspect: the larger rectangle of the epoch.
  const auto area = [](const plume::RenderRect& r) {
    return int64_t(r.right - r.left) * int64_t(r.bottom - r.top);
  };
  for (uint32_t a = 0; a < kAspectCount; ++a) {
    const Access acc = access.aspect[a];
    if (acc == Access::kNone) {
      continue;
    }
    if (own[a]) {
      if (t.own_epoch[a] != epoch_ || area(px) > area(t.own_rect[a])) {
        t.own_rect[a] = px;
      }
      t.own_epoch[a] = epoch_;
    } else if (acc == Access::kRead) {
      if (t.read_epoch[a] != epoch_ || area(px) > area(t.read_rect[a])) {
        t.read_rect[a] = px;
      }
      t.read_epoch[a] = epoch_;
    }
  }
  if (taken_from != kNoOwner) {
    const uint32_t pair = uint32_t(taken_from) << 16 | t.owner;
    if (listed_takes_.size() < 32 &&
        std::find(listed_takes_.begin(), listed_takes_.end(), pair) == listed_takes_.end()) {
      listed_takes_.push_back(pair);
      result.taken_from = &owner_keys_[taken_from];
    }
  }
  result.tiles = depend_tiles + read_tiles;
  if (result.tiles != 0) {
    if (depend_tiles != 0) {
      ++stats_.depend_events;
    } else {
      ++stats_.read_events;
    }
    stats_.depend_tiles += depend_tiles;
    stats_.read_tiles += read_tiles;
    const uint32_t pair = uint32_t(from) << 16 | t.owner;
    if (reported_pairs_.size() < kReportedPairs &&
        std::find(reported_pairs_.begin(), reported_pairs_.end(), pair) == reported_pairs_.end()) {
      reported_pairs_.push_back(pair);
      result.from = &owner_keys_[from];
    }
  }
  return result;
}

void EdramTargets::Note(Target& t, const plume::RenderRect& px, const Accesses& access,
                        const EdramEvent& event, Target* const* outputs,
                        uint32_t output_count) {
  output_count = outputs ? std::min<uint32_t>(output_count, 4) : 0;
  if (tiling_) {
    // A repeat of the access kept last changes nothing when the tiles replay
    // it: one is enough. A resolve is never a repeat: each copy's destination
    // is marked on its own (reach).
    if (!pending_.empty() && event.kind != EdramEvent::Kind::kResolve) {
      const Pending& last = pending_.back();
      bool same = last.target == &t && last.access == access && last.rect.left == px.left &&
                  last.rect.top == px.top && last.rect.right == px.right &&
                  last.rect.bottom == px.bottom && last.output_count == output_count &&
                  last.event.kind == event.kind;
      for (uint32_t i = 0; i < output_count && same; ++i) {
        same = last.outputs[i] == outputs[i];
      }
      if (same) {
        return;
      }
    }
    Pending p{&t, px, access, event};
    for (uint32_t i = 0; i < output_count; ++i) {
      p.outputs[i] = outputs[i];
    }
    p.output_count = output_count;
    pending_.push_back(p);
    ++stats_.tiled_kept;
    return;
  }
  Account(t, px, access, event, outputs, output_count, -1);
}

void EdramTargets::Account(Target& t, const plume::RenderRect& px, const Accesses& access,
                           const EdramEvent& event, Target* const* outputs,
                           uint32_t output_count, int32_t tile) {
  const Inheritance found = Apply(t, px, access);
  if (output_count != 0 && (found.tiles != 0 || found.tainted != 0)) {
    // A test over inherited bytes decides what the draw writes.
    for (uint32_t i = 0; i < output_count; ++i) {
      Taint(*outputs[i], px);
    }
  }
  Report(t, access, found, event, tile);
}

void EdramTargets::Taint(const Target& t, const plume::RenderRect& px) {
  if (t.owner == kNoOwner || px.right <= px.left || px.bottom <= px.top) {
    return;
  }
  uint32_t col0 = 0, col1 = 0, row0 = 0, row1 = 0;
  TileSpan(t, px, &col0, &col1, &row0, &row1);
  bool changed = false;
  for (uint32_t a = 0; a < kAspectCount; ++a) {
    for (uint32_t row = row0; row < row1; ++row) {
      const uint32_t first = uint32_t(t.key.base_tiles) + row * t.pitch_tiles;
      for (uint32_t col = col0; col < col1; ++col) {
        const uint32_t tile = (first + col) & (kTileCount - 1);
        // Only what the output wrote: a tile it does not own is not its result.
        if (tile_owner_[a][tile] == t.owner && !tile_taint_[a][tile]) {
          tile_taint_[a][tile] = 1;
          ++stats_.tainted;
          changed = true;
        }
      }
    }
  }
  if (changed) {
    ++epoch_;  // what was known clean of these tiles is not any more
  }
}

void EdramTargets::BeginTiling() {
  if (!aliasing_) {
    return;
  }
  if (tiling_ && !tiling_mismatch_logged_) {
    tiling_mismatch_logged_ = true;
    REXLOG_ERROR("[gpu] edram: a tiled section began inside another; the EDRAM ownership of "
                 "the first is noted with the second's tiles (reported once)");
  }
  tiling_ = true;
  ++stats_.tiled_sections;
}

void EdramTargets::EndTiling(const EdramTile* tiles, uint32_t count) {
  if (!aliasing_) {
    return;
  }
  if (!tiling_) {
    if (!tiling_mismatch_logged_) {
      tiling_mismatch_logged_ = true;
      REXLOG_ERROR("[gpu] edram: a tiled section ended that did not begin ("
                   "reported once)");
    }
    return;
  }
  tiling_ = false;
  if (count == 0 && !pending_.empty()) {
    if (!tiling_mismatch_logged_) {
      tiling_mismatch_logged_ = true;
      REXLOG_ERROR("[gpu] edram: a tiled section ended without tiles; its {} accesses are noted "
                   "in surface pixels, as if untiled (TODO M5.2d; reported once)",
                   pending_.size());
    }
    for (const Pending& p : pending_) {
      if (!SettledFor(p, p.rect)) {
        Account(*p.target, p.rect, p.access, p.event, p.outputs, p.output_count, -1);
      }
    }
  }
  // The GPU runs the whole section for one tile before the next.
  for (uint32_t i = 0; i < count; ++i) {
    const EdramTile& tile = tiles[i];
    for (const Pending& p : pending_) {
      plume::RenderRect r(std::max(p.rect.left, tile.rect.left),
                          std::max(p.rect.top, tile.rect.top),
                          std::min(p.rect.right, tile.rect.right),
                          std::min(p.rect.bottom, tile.rect.bottom));
      if (r.right <= r.left || r.bottom <= r.top) {
        continue;
      }
      r.left -= tile.origin_x;
      r.right -= tile.origin_x;
      r.top -= tile.origin_y;
      r.bottom -= tile.origin_y;
      ++stats_.tiled_noted;
      if (!SettledFor(p, r)) {
        Account(*p.target, r, p.access, p.event, p.outputs, p.output_count, int32_t(i));
      }
    }
  }
  pending_.clear();
}

bool EdramTargets::SettledFor(const Pending& p, const plume::RenderRect& r) const {
  // A copy, or a test with outputs, of bytes already counted still carries
  // them into its result (reach).
  return p.event.kind == EdramEvent::Kind::kResolve || p.output_count != 0
             ? SettledClean(*p.target, r, p.access)
             : Settled(*p.target, r, p.access);
}

void EdramTargets::Report(const Target& t, const Accesses& access, const Inheritance& found,
                          const EdramEvent& e, int32_t tile) {
  if (found.taken != 0 && e.check == kNoCheck && e.kind == EdramEvent::Kind::kDraw) {
    ++stats_.takes_unchecked;
  }
  if (found.taken != 0 && e.check != kNoCheck) {
    // Rests on the draw's extent until its check is read.
    std::vector<CoverageCheck>& checks = coverage_checks_[e.check >> 24];
    const uint32_t index = e.check & 0xFFFFFFu;
    if (index < checks.size()) {
      CoverageCheck& check = checks[index];
      if (check.taken_owner == kNoOwner) {
        check.taken_owner = found.taken_owner;
        check.target = t.owner;
      }
      check.taken += found.taken;
    }
  }
  if (e.kind == EdramEvent::Kind::kResolve && e.resolve != 0 &&
      (found.tiles != 0 || found.tainted != 0)) {
    // Its destination holds what the console would copy from another
    // identity: the ResolveCache marks it and watches its readers.
    inherited_resolves_.push_back(e.resolve);
    ++stats_.inherited_resolves;
  }
  if (!found.from && !found.taken_from && !found.tainted_new) {
    return;
  }
  std::string what;
  switch (e.kind) {
    case EdramEvent::Kind::kDraw:
      what = std::format(
          "a draw: {} {} n={}, blend {:08X} mask {:X}, RB_DEPTHCONTROL {:08X}, RB_COLORCONTROL "
          "{:08X}, stencil {:08X}/{:08X}, AA mask {:X}, scissor ({}, {})-({}, {}), VS {:016X} PS "
          "{:016X}",
          e.draw_kind, e.primitive, e.count, e.blend, e.write_mask, e.depth_control,
          e.color_control, e.stencil, e.stencil_bf, e.aa_mask, e.scissor.left, e.scissor.top,
          e.scissor.right, e.scissor.bottom, e.vs, e.ps);
      break;
    case EdramEvent::Kind::kClear:
      what = "a clear";
      break;
    case EdramEvent::Kind::kResolve:
      what = std::format("a resolve into {:#010x}, texture format {} {}x{}", e.dest_page << 12,
                         e.dest_format, e.dest_width, e.dest_height);
      break;
  }
  const std::string where = tile < 0 ? std::string() : std::format(", tile {}", tile);
  if (found.from) {
    const char* aspect = t.key.depth ? (found.aspect == kMain ? "depth" : "stencil")
                                     : "colour words";
    const char* how = access.aspect[found.aspect] == Access::kRead ? "reads" : "keeps or blends";
    REXLOG_ERROR("[gpu] edram: {} {} the {} of {} aspect-tiles {} wrote last{}: on the console "
                 "it sees that identity's bytes, which the host does not carry over between "
                 "EDRAM identities (TODO M5.2d; pair reported once) -- {}",
                 DescribeEdramKey(t.key), how, aspect, found.tiles,
                 DescribeEdramKey(*found.from), where, what);
    RequestCapture(access.aspect[found.aspect] == Access::kRead ? kCaptureReadPair
                                                                 : kCaptureWritePair);
  }
  if (found.tainted_new) {
    REXLOG_ERROR("[gpu] edram: {} reads {} aspect-tiles of its own that hold another identity's "
                 "bytes (a write kept or blended them, or a test of them decided it){}: on the "
                 "console the result carries those bytes (TODO M5.2d; reported once per "
                 "identity) -- {}",
                 DescribeEdramKey(t.key), found.tainted, where, what);
  }
  if (found.taken_from && e.kind == EdramEvent::Kind::kDraw) {
    REXLOG_INFO("[gpu] edram: {} takes {} aspect-tiles of {}{} with an opaque draw taken to "
                "cover its scissor, {} (pair listed once) -- {}",
                DescribeEdramKey(t.key), found.taken, DescribeEdramKey(*found.taken_from), where,
                e.check != kNoCheck ? "checked by an occlusion query"
                : coverage_         ? "UNCHECKED (no query left this frame)"
                                    : "UNCHECKED (mcla_gpu_coverage_queries off)",
                what);
  }
}

void EdramTargets::NoteReach(Reach kind, uint64_t id, const std::function<std::string()>& describe) {
  ++stats_.reach[size_t(kind)];
  const uint64_t key = uint64_t(kind) << 62 | (id & ((uint64_t(1) << 62) - 1));
  if (reach_reported_.size() < kReportedPairs &&
      std::find(reach_reported_.begin(), reach_reported_.end(), key) == reach_reported_.end()) {
    reach_reported_.push_back(key);
    static const char* const kWhat[] = {"a fetch", "a texture decoded from the resolve memory",
                                        "a CPU lock"};
    REXLOG_ERROR("[gpu] edram: inherited bytes reach {}: {} -- on the console they are another "
                 "EDRAM identity's, which the host does not carry over (TODO M5.2d; reported "
                 "once)",
                 kWhat[size_t(kind)], describe());
  }
  if (!reach_seen_[size_t(kind)]) {
    reach_seen_[size_t(kind)] = true;
    RequestCapture(kCaptureReach);
  }
}

void EdramTargets::RequestCapture(uint32_t why) {
  if (!(capture_ & why) || !request_capture_ || captures_ >= 4) {
    return;
  }
  ++captures_;
  request_capture_();
  REXLOG_INFO("[gpu] edram: RenderDoc capture of the next frame requested ({} of 4, "
              "mcla_gpu_edram_capture bit {})",
              captures_, why);
}

uint32_t EdramTargets::ReserveCoverageCheck() {
  if (!coverage_) {
    return kNoCheck;
  }
  std::vector<CoverageCheck>& checks = coverage_checks_[slot_];
  if (checks.size() >= coverage_capacity_) {
    // The pools grow at their next frame; until then such draws depend.
    coverage_wanted_ = std::max(coverage_wanted_, coverage_capacity_ * 2);
    ++stats_.checks_wanting;
    return kNoCheck;
  }
  checks.emplace_back();
  return slot_ << 24 | uint32_t(checks.size() - 1);
}

void EdramTargets::DescribeCoverageCheck(uint32_t check, const EdramEvent& event) {
  if (check == kNoCheck || (check >> 24) != slot_) {
    return;
  }
  coverage_checks_[slot_][check & 0xFFFFFFu].event = event;
}

void EdramTargets::BeginCoverageCheck(plume::RenderCommandList* list, uint32_t check,
                                      uint64_t expected_samples) {
  if (check == kNoCheck || (check >> 24) != slot_) {
    return;
  }
  CoverageCheck& c = coverage_checks_[slot_][check & 0xFFFFFFu];
  c.expected = expected_samples;
  c.recorded = true;
  list->beginOcclusionQuery(coverage_pools_[slot_].get(), check & 0xFFFFFFu);
}

void EdramTargets::EndCoverageCheck(plume::RenderCommandList* list, uint32_t check) {
  if (check == kNoCheck || (check >> 24) != slot_) {
    return;
  }
  list->endOcclusionQuery(coverage_pools_[slot_].get(), check & 0xFFFFFFu);
}

void EdramTargets::HarvestCoverage(uint32_t slot) {
  std::vector<CoverageCheck>& checks = coverage_checks_[slot];
  if (checks.empty() || !coverage_pools_[slot]) {
    checks.clear();
    return;
  }
  coverage_pools_[slot]->queryResults();
  const uint64_t* passed = coverage_pools_[slot]->getResults();
  for (size_t i = 0; i < checks.size(); ++i) {
    const CoverageCheck& c = checks[i];
    if (!c.recorded) {
      ++stats_.checks_unrecorded;
      continue;
    }
    ++stats_.checks;
    if (passed[i] == c.expected) {
      ++stats_.checks_full;
      continue;
    }
    if (passed[i] > c.expected) {
      // Two triangles that overlap: the count proves nothing either way.
      ++stats_.checks_over;
      if (!coverage_over_logged_) {
        coverage_over_logged_ = true;
        REXLOG_ERROR("[gpu] edram: a draw taken to cover its scissor counted {} samples of {}: "
                     "its triangles overlap, and the count cannot tell whether it covered it "
                     "(reported once) -- {} {} n={}, VS {:016X} PS {:016X}",
                     passed[i], c.expected, c.event.draw_kind, c.event.primitive,
                     c.event.count, c.event.vs, c.event.ps);
      }
      if (c.taken == 0) {
        continue;
      }
    } else {
      ++stats_.checks_partial;
      if (c.taken == 0) {
        continue;  // nothing of another identity rested on it
      }
    }
    ++stats_.checks_partial_taken;
    const uint32_t pair = uint32_t(c.taken_owner) << 16 | c.target;
    if (c.taken_owner < owner_keys_.size() && c.target < owner_keys_.size() &&
        coverage_reported_.size() < kReportedPairs &&
        std::find(coverage_reported_.begin(), coverage_reported_.end(), pair) ==
            coverage_reported_.end()) {
      coverage_reported_.push_back(pair);
      const EdramEvent& e = c.event;
      REXLOG_ERROR(
          "[gpu] edram: an opaque draw on {} took {} aspect-tiles of {} as if it covered its "
          "scissor, and wrote {} of its {} samples: on the console what it left keeps that "
          "identity's bytes, which the host does not carry over between EDRAM identities (TODO "
          "M5.2d; pair reported once) -- {} {} n={}, scissor ({}, {})-({}, {}), VS {:016X} PS "
          "{:016X}",
          DescribeEdramKey(owner_keys_[c.target]), c.taken,
          DescribeEdramKey(owner_keys_[c.taken_owner]), passed[i], c.expected, e.draw_kind,
          e.primitive, e.count, e.scissor.left, e.scissor.top, e.scissor.right, e.scissor.bottom,
          e.vs, e.ps);
    }
  }
  checks.clear();
}

void EdramTargets::BeginFrame(plume::RenderCommandList* list, uint32_t slot) {
  ++frame_;
  if (!coverage_ || slot >= coverage_checks_.size()) {
    return;  // no pool to read or reset
  }
  slot_ = slot;
  HarvestCoverage(slot);
  if (!list) {
    return;
  }
  if (coverage_wanted_ > coverage_capacity_ && device_) {
    // The other slots' pools grow when their frames come back.
    std::unique_ptr<plume::RenderQueryPool> pool = device_->CreateQueryPool(
        coverage_wanted_, "edram coverage checks", plume::RenderQueryType::OCCLUSION);
    if (pool) {
      // The slot's frame is complete: its old pool is no longer in use.
      coverage_pools_[slot] = std::move(pool);
      bool all = true;
      for (const auto& p : coverage_pools_) {
        all &= p && p->getCount() >= coverage_wanted_;
      }
      if (all) {
        coverage_capacity_ = coverage_wanted_;
      }
    }
  }
  list->resetQueryPool(coverage_pools_[slot].get(), 0, coverage_pools_[slot]->getCount());
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
  ++change_serial_;
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
             target->key.samples * target->scale * target->scale;
  }
  std::string text = std::format(
      "edram: {} targets ({:.1f} MiB), {} framebuffers | created {}, grown {} ({:.1f} MiB "
      "copied), initialized {}, framebuffers made {}, {} barriers",
      targets_.size(), double(bytes) / 1048576.0, framebuffers_.size(), stats_.created,
      stats_.grown, double(stats_.grown_bytes) / 1048576.0, stats_.cleared,
      stats_.framebuffers_created, stats_.barriers);
  if (aliasing_) {
    text += std::format(
        " | ownership: {} aspect-tiles taken over ({} draws unchecked), inherited {} "
        "aspect-tiles in {} writes and {} in {} reads ({} pairs so far), {} walks; tiled "
        "sections {}, {} accesses kept, {} noted per tile",
        stats_.tiles_taken, stats_.takes_unchecked, stats_.depend_tiles, stats_.depend_events,
        stats_.read_tiles, stats_.read_events, reported_pairs_.size(), stats_.walks,
        stats_.tiled_sections, stats_.tiled_kept, stats_.tiled_noted);
    text += std::format(
        "; reach: {} aspect-tiles marked, {} taken back, {} marked read, {} inherited resolves, "
        "readers {} fetches {} decodes {} locks",
        stats_.tainted, stats_.untainted, stats_.tainted_reads, stats_.inherited_resolves,
        stats_.reach[size_t(Reach::kFetch)], stats_.reach[size_t(Reach::kDecode)],
        stats_.reach[size_t(Reach::kLock)]);
  }
  if (coverage_) {
    text += std::format(
        "; coverage checks {} ({} full, {} partial, {} of them over another identity's tiles, "
        "{} overlapping, {} not recorded, {} candidates without a check)",
        stats_.checks, stats_.checks_full, stats_.checks_partial, stats_.checks_partial_taken,
        stats_.checks_over, stats_.checks_unrecorded, stats_.checks_wanting);
  }
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

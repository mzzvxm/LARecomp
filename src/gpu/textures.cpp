// MCLA GPU renderer -- guest textures on the host. See textures.h.
//
// Mapping followed: Xenia's texture cache in the ReXGlue SDK (src/graphics/
// pipeline/texture/cache.cpp TextureCache::GuestToHostSwizzle and the binding
// rules of RequestTextures; src/graphics/d3d12/texture_cache.cpp for the host
// formats and the decompression rule; Copyright Ben Vanik and the Xenia
// authors, BSD-3-Clause). The texture layout arithmetic is the SDK's own
// texture_util, called, not copied. The watched/hashed residency is
// GeometryCache's (geometry.h).

#include "textures.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include "constants.h"
#include "device.h"
#include "frame.h"
#include "resolve.h"
#include "resolve_memory.h"
#include "texture_decode.h"
#include "upload.h"
#include "watch.h"

namespace mcla::gpu {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;
using rex::graphics::FormatInfo;

namespace {

using Clock = std::chrono::steady_clock;

uint64_t NsSince(Clock::time_point t0) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

plume::RenderSwizzle HostComponent(uint32_t component) {
  switch (component) {
    case 0: return plume::RenderSwizzle::R;
    case 1: return plume::RenderSwizzle::G;
    case 2: return plume::RenderSwizzle::B;
    case 3: return plume::RenderSwizzle::A;
    case 4: return plume::RenderSwizzle::ZERO;
    default: return plume::RenderSwizzle::ONE;
  }
}

const char* FormatName(uint32_t format) {
  const FormatInfo* info = FormatInfo::Get(format);
  return info ? info->name : "?";
}

}  // namespace

// ---------------------------------------------------------------------------
// keys and fetch constants
// ---------------------------------------------------------------------------

bool TextureKey::operator==(const TextureKey& o) const {
  return std::memcmp(this, &o, sizeof(TextureKey)) == 0;
}

size_t TextureKeyHash::operator()(const TextureKey& key) const {
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

std::string TextureKey::Describe() const {
  return std::format("{} {}x{}{} {} endian {} pitch {} base {:08x} mips {:08x} levels 0-{}{}",
                     FormatName(format), width, height,
                     depth > 1 ? std::format("x{}", depth) : std::string(),
                     tiled ? "tiled" : "linear", endian, uint32_t(pitch) * 32,
                     base_page << 12, mip_page << 12, mip_max_level,
                     packed_mips ? " packed" : "");
}

TextureFetch ReadTextureFetch(const uint32_t dwords[6]) {
  TextureFetch out;
  xe::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, dwords, sizeof(f));
  if (f.type != xe::FetchConstantType::kTexture) {
    return out;
  }
  out.is_texture = true;
  // Texture headers carry the 0xE0000000 window, the device's fetch constants
  // physical addresses; the guest's own conversion makes both physical (a
  // no-op on the latter), and the SDK expects physical pages.
  if (f.base_address) {
    f.base_address = GuestMemory::ToPhysical(uint32_t(f.base_address) << 12) >> 12;
  }
  if (f.mip_address) {
    f.mip_address = GuestMemory::ToPhysical(uint32_t(f.mip_address) << 12) >> 12;
  }
  out.image_base_page = uint32_t(f.base_address) & 0x1FFFF;
  uint32_t width_minus_1 = 0, height_minus_1 = 0, depth_minus_1 = 0;
  uint32_t base_page = 0, mip_page = 0, mip_min = 0, mip_max = 0;
  tu::GetSubresourcesFromFetchConstant(f, &width_minus_1, &height_minus_1, &depth_minus_1,
                                       &base_page, &mip_page, &mip_min, &mip_max);
  TextureKey& k = out.key;
  k.base_page = base_page;
  k.mip_page = mip_page;
  k.width = uint16_t(width_minus_1 + 1);
  k.height = uint16_t(height_minus_1 + 1);
  k.depth = uint16_t(depth_minus_1 + 1);
  k.pitch = uint16_t(f.pitch);
  k.format = uint8_t(f.format);
  k.endian = uint8_t(f.endianness);
  k.mip_max_level = uint8_t(mip_max);
  k.dimension = uint8_t(f.dimension);
  k.tiled = uint8_t(f.tiled);
  k.packed_mips = uint8_t(f.packed_mips);
  k.stacked = uint8_t(f.stacked);
  out.swizzle = uint16_t(f.swizzle);
  out.signs[0] = uint8_t(f.sign_x);
  out.signs[1] = uint8_t(f.sign_y);
  out.signs[2] = uint8_t(f.sign_z);
  out.signs[3] = uint8_t(f.sign_w);
  out.mip_min = mip_min;
  out.mip_max = mip_max;
  out.exp_adjust = f.exp_adjust;
  out.gamma = f.sign_x == xe::TextureSign::kGamma && f.sign_y == xe::TextureSign::kGamma &&
              f.sign_z == xe::TextureSign::kGamma;
  return out;
}

void TextureGuestRanges(const TextureKey& key, uint32_t ranges[2][2]) {
  ranges[0][0] = ranges[0][1] = ranges[1][0] = ranges[1][1] = 0;
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      xe::DataDimension::k2DOrStacked, key.pitch, key.width, key.height, 1, key.tiled != 0,
      xe::TextureFormat(key.format), key.packed_mips != 0, key.base_page != 0, key.mip_max_level);
  if (key.base_page) {
    ranges[0][0] = key.base_page << 12;
    ranges[0][1] = layout.base.level_data_extent_bytes;
  }
  if (key.mip_page && key.mip_max_level > 0) {
    ranges[1][0] = key.mip_page << 12;
    ranges[1][1] = layout.mips_total_extent_bytes;
  }
}

bool GetTextureLevelStorage(const TextureKey& key, uint32_t level, TextureLevelStorage* out) {
  const FormatInfo* info = FormatInfo::Get(key.format);
  if (!info || info->bytes_per_block() == 0) {
    return false;
  }
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      xe::DataDimension::k2DOrStacked, key.pitch, key.width, key.height, 1, key.tiled != 0,
      xe::TextureFormat(key.format), key.packed_mips != 0, key.base_page != 0, key.mip_max_level);
  const tu::TextureGuestLayout::Level* stored;
  if (level == 0) {
    if (!key.base_page) {
      return false;
    }
    stored = &layout.base;
    out->address = key.base_page << 12;
  } else {
    if (!key.mip_page) {
      return false;
    }
    const uint32_t stored_level = std::min(level, layout.packed_level);
    stored = &layout.mips[stored_level];
    out->address = (key.mip_page << 12) + layout.mip_offsets_bytes[stored_level];
  }
  out->extent = stored->level_data_extent_bytes;
  texture_decode::Request& r = out->request;
  r = texture_decode::Request{};
  r.source.row_pitch_bytes = stored->row_pitch_bytes;
  r.source.bytes_per_block = info->bytes_per_block();
  r.source.tiled = key.tiled != 0;
  r.source.endian = key.endian;
  if (key.packed_mips && level >= layout.packed_level) {
    uint32_t ox = 0, oy = 0, oz = 0;
    tu::GetPackedMipOffset(key.width, key.height, 1, xe::TextureFormat(key.format), level, ox, oy,
                           oz);
    r.source.offset_x = ox;
    r.source.offset_y = oy;
  }
  r.width = std::max(uint32_t(key.width) >> level, 1u);
  r.height = std::max(uint32_t(key.height) >> level, 1u);
  r.width_blocks = (r.width + info->block_width - 1) / info->block_width;
  r.height_blocks = (r.height + info->block_height - 1) / info->block_height;
  r.load = format::TextureHost(key.format).load;
  r.decompress = format::TextureDecompress::kNone;
  return true;
}

uint16_t GuestToHostSwizzle(uint16_t fetch_swizzle, uint16_t host_format_swizzle) {
  uint16_t host = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t guest = (fetch_swizzle >> (3 * i)) & 7;
    uint32_t component;
    if (guest >= xe::XE_GPU_TEXTURE_SWIZZLE_0) {
      // 6 and 7 folded to 0 and 1, as Xenia does against broken games.
      component = guest & 0b101;
    } else {
      component = (host_format_swizzle >> (3 * guest)) & 7;
    }
    host |= uint16_t(component << (3 * i));
  }
  return host;
}

// ---------------------------------------------------------------------------
// views
// ---------------------------------------------------------------------------

uint32_t TextureViews::Slot(Device& device, BindlessHeap& heap,
                            const plume::RenderTexture& texture, plume::RenderFormat format,
                            uint32_t levels, uint16_t host_swizzle, const char* name) {
  for (const View& v : views_) {
    if (v.host_swizzle == host_swizzle && v.levels == levels) {
      return v.slot;
    }
  }
  plume::RenderTextureViewDesc desc = plume::RenderTextureViewDesc::Texture2D(format);
  desc.mipLevels = levels;
  desc.componentMapping = plume::RenderComponentMapping(
      HostComponent(host_swizzle & 7), HostComponent((host_swizzle >> 3) & 7),
      HostComponent((host_swizzle >> 6) & 7), HostComponent((host_swizzle >> 9) & 7));
  std::unique_ptr<plume::RenderTextureView> view = device.CreateTextureView(texture, desc, name);
  if (!view) {
    return kInvalidBindlessSlot;
  }
  const uint32_t slot = heap.AllocateTexture(texture, *view);
  if (slot == kInvalidBindlessSlot) {
    return kInvalidBindlessSlot;
  }
  View v;
  v.host_swizzle = host_swizzle;
  v.levels = levels;
  v.view = std::move(view);
  v.slot = slot;
  views_.push_back(std::move(v));
  return slot;
}

void TextureViews::Release(FrameRing& frames, BindlessHeap& heap) {
  for (View& v : views_) {
    heap.ReleaseTexture(v.slot, TextureDimension::k2D);
    frames.Retire(std::move(v.view));
  }
  views_.clear();
}

// ---------------------------------------------------------------------------
// the cache
// ---------------------------------------------------------------------------

TextureCache::~TextureCache() {
  Shutdown();
}

void TextureCache::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                              UploadRing& upload, GuestMemory& memory, ResolveCache& resolve,
                              ResolveMemory& resolve_memory) {
  resolve_memory_ = &resolve_memory;
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  upload_ = &upload;
  memory_ = &memory;
  resolve_ = &resolve;
  subscriber_ = memory.Subscribe("textures");
}

void TextureCache::Shutdown() {
  // The GPU is idle and the heap goes next: nothing is handed back to it.
  for (auto& [key, entry] : entries_) {
    entry->views.Drop();
  }
  entries_.clear();
  regions_.Clear();
  region_owners_.clear();
  failed_.clear();
  reported_.clear();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  upload_ = nullptr;
  memory_ = nullptr;
  resolve_ = nullptr;
  resolve_memory_ = nullptr;
}

void TextureCache::BeginFrame(uint32_t slot) {
  slot_ = slot;
  ++frame_;
  if (frame_ % kRefreshFrames == 0) {
    // Every memoized binding is resolved again once: the entries' last use
    // stays current for the eviction below.
    ++epoch_;
    EvictSome();
  }
}

void TextureCache::Sync() {
  DrainInvalidations();
  resolve_->Sync();
  resolve_memory_->Sync();
}

void TextureCache::DrainInvalidations() {
  if (!memory_->HasInvalidations(subscriber_)) {
    return;
  }
  drained_.clear();
  memory_->Drain(subscriber_, drained_);
  bool hit = false;
  for (const GuestMemory::Range& range : drained_) {
    regions_.ForEachOverlap(range.start, range.length, [&](RegionIndex::Id id) {
      if (id >= region_owners_.size()) {
        return;
      }
      for (Entry* e : region_owners_[id]) {
        ++e->version;
        // One notification per arm: the touched pages are unwatched now.
        e->armed = false;
        ++stats_.invalidations;
        hit = true;
      }
    });
  }
  if (hit) {
    ++epoch_;
  }
}

TextureCache::Binding TextureCache::Fail(const TextureKey& key, const char* what,
                                         const std::string& detail) {
  ++stats_.failures;
  if (failed_.insert(key).second) {
    REXLOG_ERROR("[gpu] textures: {} -- {}{}{}; the null texture is bound (reported once per "
                 "texture)",
                 key.Describe(), what, detail.empty() ? "" : ": ", detail);
  }
  // Tried again next frame: what failed may be there by then (memory committed).
  return Binding{kNullTexture2DSlot, true};
}

TextureCache::Binding TextureCache::Bind2D(const TextureFetch& fetch, uint32_t reader_scale,
                                           plume::RenderCommandList* list) {
  ++stats_.lookups;
  const uint32_t gamma_bit = fetch.gamma ? kTextureGammaBit : 0u;
  const TextureKey& key = fetch.key;
  const auto remember = [&](Binding binding) {
    binding.slot |= gamma_bit;
    return binding;
  };
  const auto bound = [&](uint32_t slot, bool per_frame) {
    return remember(Binding{slot, per_frame});
  };

  if (key.dimension != uint8_t(xe::DataDimension::k2DOrStacked) || key.stacked) {
    return Fail(key, "only 2D textures are bound (TODO M5)",
                std::format("dimension {}, stacked {}", key.dimension, key.stacked));
  }
  const format::TextureHostFormat host = format::TextureHost(key.format);
  if (host.status != format::Status::kOk) {
    return Fail(key, "no host format for this texture format (TODO M5)", "");
  }
  // Reported, not handled (none in the measured frames).
  for (uint32_t c = 0; c < 4; ++c) {
    const auto sign = xe::TextureSign(fetch.signs[c]);
    // A float host format reads a signed component as it is.
    const bool unorm_host = host.format != plume::RenderFormat::R16G16B16A16_FLOAT &&
                            host.format != plume::RenderFormat::R32_FLOAT &&
                            host.format != plume::RenderFormat::R32G32_FLOAT;
    if ((sign == xe::TextureSign::kSigned && unorm_host) ||
        sign == xe::TextureSign::kUnsignedBiased) {
      if (reported_.insert(key).second) {
        REXLOG_ERROR("[gpu] textures: {} -- sign {} on component {} is read unsigned (no signed "
                     "host view, TODO M5; reported once)",
                     key.Describe(), uint32_t(sign), c);
      }
      break;
    }
  }
  if (fetch.exp_adjust != 0 && reported_.insert(key).second) {
    REXLOG_ERROR("[gpu] textures: {} -- exponent adjust {} is not applied (TODO M5; reported "
                 "once)",
                 key.Describe(), fetch.exp_adjust);
  }
  const uint16_t host_swizzle = GuestToHostSwizzle(fetch.swizzle, host.host_swizzle);

  // A resolve product of this image: what the guest rendered there (all its
  // levels, or the first ones when the fetch names fewer; a fetch whose mip
  // range starts above level 0 by the base the SDK dropped, resolve.h).
  const bool watched = watch::Active() && watch::TouchesKey(key);
  if (ResolveCache::Product* product = resolve_->Find(key, fetch.image_base_page)) {
    ++stats_.product_hits;
    if (watched) {
      watch::Log(std::format("  bind {}: product {} at {}x, read at {}x", key.Describe(),
                             product->key.Describe(), product->scale, reader_scale));
    }
    if (reader_scale != 0 && product->scale > reader_scale) {
      // A reader at a smaller scale samples the product at its own density:
      // through a copy reduced to it (the guest's texels at 1x), never a
      // sparse pick of the larger image.
      bool recorded = false;
      std::string why;
      const uint32_t slot = resolve_->ReducedSlot(*product, reader_scale, key.levels(),
                                                  host_swizzle, list, &recorded, &why);
      if (slot == kInvalidBindlessSlot) {
        return Fail(key, "no reduced copy of the resolve product", why);
      }
      ++stats_.product_reduced;
      Binding binding = bound(slot, false);
      binding.from_product = true;
      binding.scale = reader_scale;
      binding.list_state_changed = recorded;
      return binding;
    }
    const uint32_t slot =
        product->views.Slot(*device_, *heap_, *product->texture, product->format,
                            key.levels(), host_swizzle, "resolve product view");
    if (slot == kInvalidBindlessSlot) {
      return Fail(key, "no view or bindless slot for the resolve product", "");
    }
    Binding binding = bound(slot, false);
    binding.from_product = true;
    binding.scale = product->scale;
    return binding;
  }
  // Memory a resolve wrote that guest memory does not hold, read with another
  // shape (M5.2's reinterpretation): decoded from the resolve
  // memory, which holds the console's bytes.
  if (resolve_memory_->Holds(key)) {
    uint32_t slot = kInvalidBindlessSlot;
    bool state_changed = false;
    std::string why;
    switch (resolve_memory_->Bind(key, host_swizzle, list, &slot, &state_changed, &why)) {
      case ResolveMemory::BindResult::kOk: {
        ++stats_.from_resolve_memory;
        if (watched) {
          watch::Log(std::format("  bind {}: no active product, from the resolve memory{}",
                                 key.Describe(), state_changed ? " (decoded now)" : ""));
        }
        // A page of its range may change before the frame ends: compared once
        // per frame of use (and the resolve memory drops the memo on writes).
        Binding binding = bound(slot, true);
        binding.from_product = true;
        binding.list_state_changed = state_changed;
        return binding;
      }
      case ResolveMemory::BindResult::kUnsupported:
        ++stats_.resolve_memory_unsupported;
        if (reported_.insert(key).second) {
          REXLOG_ERROR("[gpu] textures: {} reads memory a resolve wrote, but {}; decoded from "
                       "guest memory, which does not hold those bytes (reported once)",
                       key.Describe(), why);
        }
        break;
      case ResolveMemory::BindResult::kFailed: {
        Binding failed = Fail(key, "decode from the resolve memory failed", why);
        failed.list_state_changed = state_changed;
        return failed;
      }
    }
  }
  // Another active product over the memory the fetch reads (its base or its
  // mips), which the resolve memory does not hold: its encode failed (logged
  // there). Reported; memory is decoded, which does not hold the image.
  {
    uint32_t ranges[2][2];
    TextureGuestRanges(key, ranges);
    const ResolveCache::Product* other = nullptr;
    for (const auto& r : ranges) {
      if (!other && r[1]) {
        other = resolve_->FindOverlap(r[0], r[1]);
      }
    }
    if (other) {
      ++stats_.reinterpretations;
      if (reported_.insert(key).second) {
        REXLOG_ERROR("[gpu] textures: {} reads memory a resolve rendered as {}, which the "
                     "resolve memory does not hold: decoded from guest memory, which does not "
                     "hold the image (reported once)",
                     key.Describe(), other->key.Describe());
      }
    }
  }

  ++stats_.memory_hits;
  if (watched) {
    watch::Log(std::format("  bind {}: no product, not in the resolve memory: guest memory",
                           key.Describe()));
  }
  auto it = entries_.find(key);
  Entry* e = it != entries_.end() ? it->second.get() : nullptr;
  if (!e) {
    std::string why;
    e = Create(key, fetch, &why);
    if (!e) {
      return Fail(key, "host texture not made", why);
    }
  }
  if (!list) {
    return Fail(key, "no command list to upload with", "");
  }

  const bool first_use_in_frame = e->last_frame != frame_;
  e->last_frame = frame_;
  std::string why;
  if (e->mode == Mode::kWatched) {
    if (e->uploaded_version != e->version) {
      const bool changed = e->uploaded_version != 0;
      if (first_use_in_frame || changed) {
        e->changed_streak = changed ? e->changed_streak + 1 : 0;
      }
      if (e->changed_streak >= kDynamicStreak) {
        e->mode = Mode::kHashed;
        e->stable_streak = 0;
        e->checked_frame = 0;
        ++stats_.to_hashed;
      } else {
        // Arm BEFORE reading: a write after it is then always seen.
        if (e->watchable && !e->armed) {
          for (const auto& r : e->ranges) {
            if (r[1]) memory_->Arm(r[0], r[1]);
          }
          e->armed = true;
        }
        const uint64_t version = e->version;
        if (!Upload(*e, list, &why)) {
          return Fail(key, "decode failed", why);
        }
        e->uploaded_version = version;
      }
    } else if (first_use_in_frame) {
      e->changed_streak = 0;
    }
  }
  if (e->mode == Mode::kHashed && e->checked_frame != frame_) {
    e->checked_frame = frame_;
    const uint64_t hash = HashGuest(*e);
    const bool unchanged = e->uploaded_version != 0 && hash == e->uploaded_hash;
    e->stable_streak = unchanged ? e->stable_streak + 1 : 0;
    if (!unchanged) {
      if (!Upload(*e, list, &why)) {
        return Fail(key, "decode failed", why);
      }
      e->uploaded_hash = hash;
      e->uploaded_version = e->version;
    }
    if (e->watchable && e->stable_streak >= e->stable_required) {
      // Back to the watch: arm first, then compare once more, so a write
      // between the hash and the arm cannot be lost.
      for (const auto& r : e->ranges) {
        if (r[1]) memory_->Arm(r[0], r[1]);
      }
      e->armed = true;
      const uint64_t again = HashGuest(*e);
      if (again != e->uploaded_hash) {
        if (!Upload(*e, list, &why)) {
          return Fail(key, "decode failed", why);
        }
        e->uploaded_hash = again;
      }
      e->mode = Mode::kWatched;
      e->uploaded_version = e->version;
      e->changed_streak = 0;
      e->stable_required = std::min(e->stable_required * 2, kMaxStableStreak);
      ++stats_.to_watched;
    }
  }

  const uint32_t slot = e->views.Slot(*device_, *heap_, *e->texture, e->host_format,
                                      key.levels(), host_swizzle, "guest texture view");
  if (slot == kInvalidBindlessSlot) {
    return Fail(key, "no view or bindless slot", "");
  }
  // A hashed entry is compared once per frame of use.
  return bound(slot, e->mode == Mode::kHashed);
}

TextureCache::Entry* TextureCache::Create(const TextureKey& key, const TextureFetch& fetch,
                                          std::string* why) {
  (void)fetch;
  const FormatInfo* info = FormatInfo::Get(key.format);
  if (!info || info->bytes_per_block() == 0) {
    *why = "the SDK has no block size for the format";
    return nullptr;
  }
  auto entry = std::make_unique<Entry>();
  Entry& e = *entry;
  e.key = key;
  e.host = format::TextureHost(key.format);
  // Block-compressed with a base that is not 4x4-aligned: RGBA8 (Xenia
  // IsDecompressionNeeded; Plume exposes no unaligned-BC capability).
  e.decompressed = e.host.decompress != format::TextureDecompress::kNone &&
                   ((key.width & (info->block_width - 1)) != 0 ||
                    (key.height & (info->block_height - 1)) != 0);
  e.host_format = e.decompressed ? plume::RenderFormat::R8G8B8A8_UNORM : e.host.format;

  TextureGuestRanges(key, e.ranges);
  // The watch covers what the layout names; the exact reach only shortens a
  // level the layout's padding runs past committed memory (Upload).
  e.watchable = true;
  for (const auto& r : e.ranges) {
    if (r[1] && !memory_->IsWatchable(r[0], r[1])) {
      e.watchable = false;
    }
  }
  e.mode = e.watchable ? Mode::kWatched : Mode::kHashed;

  plume::RenderTextureDesc desc =
      plume::RenderTextureDesc::Texture2D(key.width, key.height, key.levels(), e.host_format);
  const std::string name = std::format("texture {}", key.Describe());
  e.texture = device_->CreateTexture(desc, name.c_str());
  if (!e.texture) {
    *why = std::format("{} {}x{} with {} levels", format::PlumeFormatName(e.host_format),
                       key.width, key.height, key.levels());
    return nullptr;
  }
  e.bytes = 0;
  for (uint32_t level = 0; level < key.levels(); ++level) {
    const uint32_t w = std::max(uint32_t(key.width) >> level, 1u);
    const uint32_t h = std::max(uint32_t(key.height) >> level, 1u);
    const uint32_t bw = plume::RenderFormatBlockWidth(e.host_format);
    e.bytes += uint64_t((w + bw - 1) / bw) * ((h + bw - 1) / bw) *
               plume::RenderFormatSize(e.host_format);
  }
  for (uint32_t i = 0; i < 2; ++i) {
    if (!e.ranges[i][1]) {
      continue;
    }
    RegionIndex::Id id = regions_.Find(e.ranges[i][0], e.ranges[i][1]);
    if (id == RegionIndex::kNone) {
      id = regions_.Insert(e.ranges[i][0], e.ranges[i][1]);
    }
    if (region_owners_.size() < regions_.id_bound()) {
      region_owners_.resize(regions_.id_bound());
    }
    region_owners_[id].push_back(&e);
    e.region_ids[i] = id;
  }
  ++stats_.created;
  if (e.decompressed) {
    ++stats_.decompressed;
  }
  Entry* raw = entry.get();
  entries_.emplace(key, std::move(entry));
  return raw;
}

bool TextureCache::Upload(Entry& e, plume::RenderCommandList* list, std::string* why) {
  const auto t0 = Clock::now();
  const TextureKey& key = e.key;
  const FormatInfo* info = FormatInfo::Get(key.format);
  const uint32_t bpb = info->bytes_per_block();
  const uint32_t bw = info->block_width, bh = info->block_height;
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      xe::DataDimension::k2DOrStacked, key.pitch, key.width, key.height, 1, key.tiled != 0,
      xe::TextureFormat(key.format), key.packed_mips != 0, key.base_page != 0, key.mip_max_level);
  const uint32_t host_block_bytes = plume::RenderFormatSize(e.host_format);
  const uint32_t host_block_width = plume::RenderFormatBlockWidth(e.host_format);

  if (e.layout != plume::RenderTextureLayout::COPY_DEST) {
    list->barriers(plume::RenderBarrierStage::COPY,
                   plume::RenderTextureBarrier(e.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    e.layout = plume::RenderTextureLayout::COPY_DEST;
  }
  bool ok = true;
  for (uint32_t level = 0; level <= key.mip_max_level; ++level) {
    // Where the level is stored (util.h): the base under base_address with the
    // fetch's pitch, the mips under mip_address at mip_offsets_bytes, levels
    // from the packed one on inside a tail at GetPackedMipOffset.
    const tu::TextureGuestLayout::Level* stored;
    uint32_t address;
    if (level == 0) {
      if (!key.base_page) {
        continue;  // no base: the sampler's range starts at a mip
      }
      stored = &layout.base;
      address = key.base_page << 12;
    } else {
      if (!key.mip_page) {
        continue;
      }
      const uint32_t stored_level = std::min(level, layout.packed_level);
      stored = &layout.mips[stored_level];
      address = (key.mip_page << 12) + layout.mip_offsets_bytes[stored_level];
    }
    texture_decode::Request r;
    r.source.row_pitch_bytes = stored->row_pitch_bytes;
    r.source.bytes_per_block = bpb;
    r.source.tiled = key.tiled != 0;
    r.source.endian = key.endian;
    if (key.packed_mips && level >= layout.packed_level) {
      uint32_t ox = 0, oy = 0, oz = 0;
      tu::GetPackedMipOffset(key.width, key.height, 1, xe::TextureFormat(key.format), level, ox,
                             oy, oz);
      r.source.offset_x = ox;
      r.source.offset_y = oy;
    }
    r.width = std::max(uint32_t(key.width) >> level, 1u);
    r.height = std::max(uint32_t(key.height) >> level, 1u);
    r.width_blocks = (r.width + bw - 1) / bw;
    r.height_blocks = (r.height + bh - 1) / bh;
    r.load = e.host.load;
    r.decompress = e.decompressed ? e.host.decompress : format::TextureDecompress::kNone;

    uint64_t readable = stored->level_data_extent_bytes;
    const uint8_t* src = memory_->Read(address, uint32_t(readable));
    if (!src) {
      // The layout's tile padding may run into memory the guest never
      // committed: the swizzle's exact reach is what is read then.
      readable = texture_decode::Reach(r.source, r.width_blocks, r.height_blocks);
      src = memory_->Read(address, uint32_t(readable));
      if (!src) {
        *why = std::format("level {} at {:08x}: {} bytes (exact reach) not readable", level,
                           address, readable);
        ok = false;
        break;
      }
    }
    r.source.data = src;
    r.source.readable = readable;

    const uint32_t row_bytes = texture_decode::HostRowBytes(r);
    const uint32_t rows = texture_decode::HostRows(r);
    const uint64_t pitch = AlignUp(row_bytes, kTextureCopyPitchAlignment);
    const uint64_t size = pitch * rows;
    scratch_.resize(size_t(size));
    if (!texture_decode::Decode(r, scratch_.data(), uint32_t(pitch))) {
      *why = std::format("level {} at {:08x}: a block beyond the {} readable bytes", level, address,
                         readable);
      ok = false;
      break;
    }
    const UploadSpan span = upload_->Allocate(slot_, size, kTextureCopyOffsetAlignment);
    if (!span) {
      *why = std::format("no upload memory for {} bytes", size);
      ok = false;
      break;
    }
    std::memcpy(span.cpu, scratch_.data(), size_t(size));
    const uint32_t row_width = uint32_t(pitch / host_block_bytes) * host_block_width;
    list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(e.texture.get(), level, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(span.buffer, e.host_format, r.width,
                                                          r.height, 1, row_width, span.offset));
    stats_.upload_bytes += size;
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(e.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  e.layout = plume::RenderTextureLayout::SHADER_READ;
  ++stats_.uploads;
  stats_.decode_ns += NsSince(t0);
  return ok;
}

uint64_t TextureCache::HashGuest(const Entry& e) {
  uint64_t hash = 0;
  for (const auto& r : e.ranges) {
    if (!r[1]) {
      continue;
    }
    const uint8_t* src = memory_->Read(r[0], r[1]);
    // An unreadable range hashes as its absence: a later commit changes it.
    const uint64_t h = src ? XXH3_64bits(src, r[1]) : 0;
    hash = hash * 0x9E3779B97F4A7C15ull ^ h;
  }
  ++stats_.hashes;
  stats_.hashed_bytes += uint64_t(e.ranges[0][1]) + e.ranges[1][1];
  return hash;
}

void TextureCache::Destroy(Entry& e, bool for_shutdown) {
  for (RegionIndex::Id& id : e.region_ids) {
    if (id == RegionIndex::kNone) {
      continue;
    }
    if (id < region_owners_.size()) {
      std::vector<Entry*>& owners = region_owners_[id];
      owners.erase(std::remove(owners.begin(), owners.end(), &e), owners.end());
      if (owners.empty()) {
        regions_.Remove(id);
      }
    }
    id = RegionIndex::kNone;
  }
  if (for_shutdown) {
    e.views.Drop();
    return;
  }
  e.views.Release(*frames_, *heap_);
  frames_->Retire(std::move(e.texture));
}

void TextureCache::EvictSome() {
  bool any = false;
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& e = *it->second;
    if (e.last_frame + kEvictFrames < frame_) {
      Destroy(e, false);
      ++stats_.evicted;
      any = true;
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  if (any) {
    ++epoch_;  // no memoized binding may name a released slot
  }
}

std::string TextureCache::TakeSummary(uint64_t frames) {
  const double f = frames ? double(frames) : 1.0;
  uint64_t bytes = 0;
  uint64_t hashed = 0;
  for (const auto& [key, e] : entries_) {
    bytes += e->bytes;
    hashed += e->mode == Mode::kHashed;
  }
  std::string text = std::format(
      "textures: {} live ({:.1f} MiB, {} hashed) | resolutions {:.0f}/frame: products {} ({} "
      "through a reduced copy), memory {} | created {}, uploads {} ({:.2f} MiB/frame, {:.2f} "
      "ms/frame), decompressed {} | writes seen {}, hashes {} ({:.2f} MiB/frame), to hashed {}, to "
      "watched {} | evicted {}, failures {} | from the resolve memory {} ({} unsupported), memory "
      "read under a product {}",
      entries_.size(), double(bytes) / 1048576.0, hashed, double(stats_.lookups) / f,
      stats_.product_hits, stats_.product_reduced, stats_.memory_hits, stats_.created, stats_.uploads,
      double(stats_.upload_bytes) / f / 1048576.0, double(stats_.decode_ns) / f / 1e6,
      stats_.decompressed, stats_.invalidations, stats_.hashes,
      double(stats_.hashed_bytes) / f / 1048576.0, stats_.to_hashed, stats_.to_watched,
      stats_.evicted, stats_.failures, stats_.from_resolve_memory,
      stats_.resolve_memory_unsupported, stats_.reinterpretations);
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

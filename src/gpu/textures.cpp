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
#include <cmath>
#include <cstring>
#include <format>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include "constants.h"
#include "device.h"
#include "frame.h"
#include "mc_engine/texture_dump.h"
#include "resolve.h"
#include "resolve_memory.h"
#include "texture_decode.h"
#include "texture_names.h"
#include "upload.h"
#include "watch.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_texture_dump, false, "MCLA/GPU",
    "Write every texture the NATIVE renderer uploads to <mods>/textures_dump/<dictionary>/"
    "<name>__<hash>.dds, once per texture per session (F2 does it for one frame). Texture mods "
    "replace a texture by the hash at the end of the file name, from "
    "<mods>/<Mod>/textures_replace/; <mods> is modloader/, mods/ or models/.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    texture_mods, true, "MCLA/Textures",
    "Use the texture mods (<mods>/<Mod>/textures_replace/). False shows the game's own textures; "
    "changing it re-indexes the mods at the start of the next frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    texture_reload, false, "MCLA/Textures",
    "Set to true to reload the texture mods (<mods>/<Mod>/textures_replace/) at the start of the "
    "next frame, the same as RELOAD TEXTURE MODS in the pause menu. It clears itself afterwards.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_texture_decode_threads, -1, "MCLA/GPU",
    "Threads that decode the textures of the draws queued behind the one the renderer is on "
    "(untile, byte swap), so a burst of new textures as the game streams in the next stretch of "
    "road does not stall the render thread. -1: one thread per 4 logical processors, up to 3, none "
    "below 4 processors. 0: the render thread decodes every texture itself, when its draw needs "
    "it. The threads run below the game's.")
    .range(-1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_texture_decode_selftest, false, "MCLA/GPU",
    "Test only: at start-up, decodes a few thousand random texture levels (every block size, "
    "endian, tiled and linear, packed-tail offsets, DXT decompression) with the fast decoder and "
    "with the plain per-block one, and logs whether every byte matched.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_texture_decode_verify, false, "MCLA/GPU",
    "Test only: every texture level the game uploads is decoded a second time with the plain "
    "per-block decoder and compared byte for byte with the fast one's; a difference is logged "
    "once per texture. Slow: it brings the old cost back.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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

enum class PlanResult { kOk, kSkip, kFail };

// Where level `level` of `key` is read from and how its decoded rows lie in the upload memory
// (TextureLevelPlan). kSkip: the level has no storage (no base: the sampler's range starts at a
// mip; no mips). kFail: its memory cannot be read (`why` says); the draw's own Upload reports it.
// What Upload and Prefetch both start from.
PlanResult PlanLevel(const GuestMemory& memory, const TextureKey& key, const FormatInfo& info,
                     const format::TextureHostFormat& host, bool decompressed,
                     const tu::TextureGuestLayout& layout, uint32_t level, TextureLevelPlan* plan,
                     std::string* why) {
  // Where the level is stored (util.h): the base under base_address with the
  // fetch's pitch, the mips under mip_address at mip_offsets_bytes, levels
  // from the packed one on inside a tail at GetPackedMipOffset.
  const tu::TextureGuestLayout::Level* stored;
  uint32_t address;
  if (level == 0) {
    if (!key.base_page) {
      return PlanResult::kSkip;
    }
    stored = &layout.base;
    address = key.base_page << 12;
  } else {
    if (!key.mip_page) {
      return PlanResult::kSkip;
    }
    const uint32_t stored_level = std::min(level, layout.packed_level);
    stored = &layout.mips[stored_level];
    address = (key.mip_page << 12) + layout.mip_offsets_bytes[stored_level];
  }
  const uint32_t bw = info.block_width, bh = info.block_height;
  texture_decode::Request& r = plan->request;
  r = texture_decode::Request{};
  r.source.row_pitch_bytes = stored->row_pitch_bytes;
  r.source.bytes_per_block = info.bytes_per_block();
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
  r.width_blocks = (r.width + bw - 1) / bw;
  r.height_blocks = (r.height + bh - 1) / bh;
  r.load = host.load;
  r.decompress = decompressed ? host.decompress : format::TextureDecompress::kNone;

  uint64_t readable = stored->level_data_extent_bytes;
  const uint8_t* src = memory.Read(address, uint32_t(readable));
  if (!src) {
    // The layout's tile padding may run into memory the guest never
    // committed: the swizzle's exact reach is what is read then.
    readable = texture_decode::Reach(r.source, r.width_blocks, r.height_blocks);
    src = memory.Read(address, uint32_t(readable));
    if (!src) {
      *why = std::format("level {} at {:08x}: {} bytes (exact reach) not readable", level, address,
                         readable);
      return PlanResult::kFail;
    }
  }
  r.source.data = src;
  r.source.readable = readable;

  const uint32_t row_bytes = texture_decode::HostRowBytes(r);
  plan->level = level;
  plan->address = address;
  plan->rows = texture_decode::HostRows(r);
  plan->pitch = uint32_t(AlignUp(row_bytes, kTextureCopyPitchAlignment));
  plan->size = uint64_t(plan->pitch) * plan->rows;
  plan->offset = 0;
  return PlanResult::kOk;
}

// The guest layout of a 2D texture's key (what every range, plan and extent below comes from).
tu::TextureGuestLayout LayoutOf(const TextureKey& key) {
  return tu::GetGuestTextureLayout(xe::DataDimension::k2DOrStacked, key.pitch, key.width,
                                   key.height, 1, key.tiled != 0, xe::TextureFormat(key.format),
                                   key.packed_mips != 0, key.base_page != 0, key.mip_max_level);
}

// TextureGuestRanges from a layout that is already at hand.
void RangesOfLayout(const TextureKey& key, const tu::TextureGuestLayout& layout,
                    uint32_t ranges[2][2]) {
  ranges[0][0] = ranges[0][1] = ranges[1][0] = ranges[1][1] = 0;
  if (key.base_page) {
    ranges[0][0] = key.base_page << 12;
    ranges[0][1] = layout.base.level_data_extent_bytes;
  }
  if (key.mip_page && key.mip_max_level > 0) {
    ranges[1][0] = key.mip_page << 12;
    ranges[1][1] = layout.mips_total_extent_bytes;
  }
}

// Arms the pages of a texture's two ranges (base, mips). The mips usually follow the base in the
// allocation: when the two ranges touch or share a page the pages are armed with one call, which
// names the same pages the two calls would (the SDK takes its global lock and protects the pages
// once per call and per physical alias).
void ArmTextureRanges(GuestMemory& memory, const uint32_t ranges[2][2]) {
  if (ranges[0][1] && ranges[1][1]) {
    const uint64_t first0 = ranges[0][0] >> 12;
    const uint64_t last0 = (uint64_t(ranges[0][0]) + ranges[0][1] - 1) >> 12;
    const uint64_t first1 = ranges[1][0] >> 12;
    const uint64_t last1 = (uint64_t(ranges[1][0]) + ranges[1][1] - 1) >> 12;
    if (first1 <= last0 + 1 && first0 <= last1 + 1) {
      const uint64_t first = std::min(first0, first1);
      const uint64_t last = std::max(last0, last1);
      memory.Arm(uint32_t(first << 12), uint32_t((last - first + 1) << 12));
      return;
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (ranges[i][1]) {
      memory.Arm(ranges[i][0], ranges[i][1]);
    }
  }
}

}  // namespace

// A texture's levels decoded by the pool ahead of the draw that needs them (see textures.h). One
// buffer holds every level's rows at the pitch the upload ring takes; the plan names each
// level's place in it.
class PrefetchedTexture final : public DecodeJob {
 public:
  std::vector<TextureLevelPlan> levels;
  std::unique_ptr<uint8_t[]> data;
  uint64_t version = 0;  // the entry's version when the pages were armed and the plan read
  uint64_t frame = 0;    // the render thread's frame it was made in
  bool verify = false;   // decode with DecodeVerified (mcla_gpu_texture_decode_verify)
  // Written by Run, read after the job is done.
  bool complete = false;
  bool mismatch = false;

  void Hold(DecodePool& pool, uint64_t bytes) { HoldBytes(pool, bytes); }
  const TextureLevelPlan* Find(uint32_t level) const {
    for (const TextureLevelPlan& l : levels) {
      if (l.level == level) {
        return &l;
      }
    }
    return nullptr;
  }

 protected:
  void Run() override {
    bool ok = true;
    for (const TextureLevelPlan& l : levels) {
      uint8_t* out = data.get() + l.offset;
      if (verify) {
        bool bad = false;
        ok &= texture_decode::DecodeVerified(l.request, out, l.pitch, &bad);
        mismatch |= bad;
      } else {
        ok &= texture_decode::Decode(l.request, out, l.pitch);
      }
    }
    complete = ok;
  }
};

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
  RangesOfLayout(key, LayoutOf(key), ranges);
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
  mods_.Initialize();
  if (REXCVAR_GET(mcla_gpu_texture_decode_selftest)) {
    const auto t0 = Clock::now();
    const texture_decode::SelfTestResult test = texture_decode::SelfTest(1500);
    const double ms = double(NsSince(t0)) / 1e6;
    if (test.mismatches == 0 && test.tables_ok && test.fast > test.runs / 2) {
      REXLOG_INFO("[gpu] textures: decode self-test passed: {} random levels, {} decodes ({} on the "
                  "fast path, {} on the per-block decoder: unreadable tails), all bytes equal to "
                  "the per-block decoder's ({:.0f} ms)",
                  test.cases, test.runs, test.fast, test.fallback, ms);
    } else if (test.mismatches == 0) {
      REXLOG_ERROR("[gpu] textures: decode self-test INCONCLUSIVE: no byte differs but only {} of "
                   "{} decodes ran the fast path (tile tables {})",
                   test.fast, test.runs, test.tables_ok ? "ok" : "do NOT match the SDK");
    } else {
      REXLOG_ERROR("[gpu] textures: decode self-test FAILED: {} of {} decodes differ from the "
                   "per-block decoder; first: {}",
                   test.mismatches, test.runs, test.first);
    }
  }
  const int32_t threads_setting = REXCVAR_GET(mcla_gpu_texture_decode_threads);
  const uint32_t threads =
      threads_setting < 0 ? DecodePool::AutoThreads() : uint32_t(threads_setting);
  pool_.Start(threads);
  REXLOG_INFO("[gpu] textures: {} decode thread(s) for the draws queued behind the current one{}",
              pool_.threads(), pool_.running() ? "" : " (the render thread decodes every texture)");
}

void TextureCache::SetOverride(const Override& override) {
  if (override_.texture != override.texture) {
    ReleaseOverrideViews();
  }
  // What was memoized under the override that goes (a product it replaced that the new one does
  // not) and under the one that comes is bound again.
  if (override_active_) {
    BumpOverridePages(override_);
  }
  override_ = override;
  override_active_ = override.texture != nullptr;
  BumpOverridePages(override);
}

void TextureCache::ClearOverride() {
  if (!override_active_) {
    return;
  }
  override_active_ = false;
  if (override_.other_product) {
    BumpProductGeneration(override_.other_page);
  }
  BumpProductGeneration(override_.page);
}

void TextureCache::ReleaseOverrideViews() {
  if (frames_ && heap_) {
    override_views_.Release(*frames_, *heap_);
  } else {
    override_views_.Drop();
  }
}

void TextureCache::Shutdown() {
  // No worker reads guest memory or an entry's plan from here on.
  pool_.Stop();
  // The GPU is idle and the heap goes next: nothing is handed back to it.
  override_views_.Drop();
  override_active_ = false;
  override_ = Override{};
  for (auto& [key, entry] : entries_) {
    entry->views.Drop();
  }
  entries_.clear();
  for (auto& [hash, replacement] : replacements_) {
    if (replacement) {
      replacement->views.Drop();
    }
  }
  replacements_.clear();
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
  static bool mods_were_on = REXCVAR_GET(texture_mods);
  if (mods_were_on != REXCVAR_GET(texture_mods)) {
    mods_were_on = REXCVAR_GET(texture_mods);
    reload_requested_.store(true, std::memory_order_relaxed);
  }
  if (REXCVAR_GET(texture_reload)) {
    // A trigger, not a setting: put it back so setting it again reloads again.
    rex::cvar::SetFlagByName("texture_reload", "false");
    reload_requested_.store(true, std::memory_order_relaxed);
  }
  if (reload_requested_.exchange(false, std::memory_order_relaxed)) {
    ReloadMods();
  }
  if (dump_all_requested_.exchange(false, std::memory_order_relaxed)) {
    const uint64_t before = stats_.dumped;
    for (const auto& [key, entry] : entries_) {
      DumpEntry(*entry);
    }
    REXLOG_INFO("[gpu] textures: {} of {} held texture(s) dumped to {} (the rest were dumped "
                "before or have no dumpable format)",
                stats_.dumped - before, entries_.size(), mcla::TextureDumpDir().string());
  }
  if (dump_requested_.exchange(false, std::memory_order_relaxed)) {
    // Every texture bound in this frame; the memoized bindings resolve again
    // so each one passes through Bind2D.
    dump_frame_ = frame_;
    ++epoch_;
    REXLOG_INFO("[gpu] textures: dumping the textures of this frame to {}",
                mcla::TextureDumpDir().string());
  }
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

TextureCache::Binding TextureCache::Bind2D(const TextureFetch& fetch, Scale reader_scale,
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

  const bool watched = watch::Active() && watch::TouchesKey(key);
  // A resolve product of this image: what the guest rendered there (all its
  // levels, or the first ones when the fetch names fewer; a fetch whose mip
  // range starts above level 0 by the base the SDK dropped, resolve.h).
  ResolveCache::Product* product = resolve_->Find(key, fetch.image_base_page);
  // The temporal upscaler's output stands for the scene colour product (the anchor's, and the post
  // chain's reads before it): bound through this fetch's swizzle, good for this frame only.
  if (override_active_ && override_.texture && product && override_.Replaces(product)) {
    const uint32_t slot = override_views_.Slot(*device_, *heap_, *override_.texture,
                                               override_.format, 1, host_swizzle,
                                               "temporal upscaler output view");
    if (slot != kInvalidBindlessSlot) {
      ++stats_.product_hits;
      if (watched) {
        watch::Log(std::format("  bind {}: the temporal upscaler's output replaces the product",
                               key.Describe()));
      }
      Binding binding = bound(slot, true);
      ++override_binds_;
      binding.from_product = true;
      binding.scale = override_.scale;
      binding.product_scale = override_.scale;
      return binding;
    }
    // No view or slot: the product itself is read, as without the override.
  }
  if (product) {
    ++stats_.product_hits;
    if (watched) {
      watch::Log(std::format("  bind {}: product {} at {}x, read at {}x", key.Describe(),
                             product->key.Describe(), product->scale, reader_scale));
    }
    // A reader at another scale than the product's:
    // - a product made above 1x (an image of the screen) read by a draw at a
    //   smaller scale is sampled at the reader's density, through a copy
    //   reduced to it, never a sparse pick of the larger image;
    // - a product a depth source made below 1x read by a draw at a larger
    //   scale goes through a copy enlarged by the nearest texel: the game's
    //   linear fetch of the smaller image would blend neighbouring depths;
    // - anything else is read as it is: a product at the guest's size already
    //   has the console's density, and colour is enlarged by the game's filter.
    const bool reduce = product->scale > reader_scale && product->scale > Scale{};
    const bool enlarge =
        product->scale < reader_scale && product->scale < Scale{} && product->depth_source;
    if (!reader_scale.none() && (reduce || enlarge)) {
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
      binding.product_scale = product->scale;
      binding.depth_source = product->depth_source;
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
    binding.product_scale = product->scale;
    binding.depth_source = product->depth_source;
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
  if (dump_frame_ == frame_) {
    DumpEntry(*e);
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
          ArmTextureRanges(*memory_, e->ranges);
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
      ArmTextureRanges(*memory_, e->ranges);
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

  if (Replacement* r = e->replacement) {
    // A mod's replacement, read through the original format's swizzle: the
    // dump the file was made from wrote guest component i into channel i,
    // which is what that swizzle reads from the host format.
    r->last_frame = frame_;
    const uint32_t slot = r->views.Slot(*device_, *heap_, *r->texture, r->format, r->levels,
                                        host_swizzle, "texture replacement view");
    if (slot == kInvalidBindlessSlot) {
      return Fail(key, "no view or bindless slot for the replacement", "");
    }
    ++stats_.replaced_binds;
    Binding binding = bound(slot, e->mode == Mode::kHashed);
    binding.replaced = true;
    binding.levels = r->levels;
    binding.lod_shift =
        int32_t(std::lround(std::log2(double(r->width) / double(std::max<uint32_t>(key.width, 1)))));
    return binding;
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
                                          std::string* why, const uint32_t (*known_ranges)[2]) {
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

  if (known_ranges) {
    std::memcpy(e.ranges, known_ranges, sizeof(e.ranges));
  } else {
    TextureGuestRanges(key, e.ranges);
  }
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
  NoteCreated();
  Entry* raw = entry.get();
  entries_.emplace(key, std::move(entry));
  return raw;
}

void TextureCache::Prefetch(const uint32_t dwords[6], uint64_t sequence) {
  // CHECK: not built or run. How much of a burst it takes off the render thread depends on how far
  // behind the guest the render thread is when the burst starts (the lookahead sees only the
  // records already published) and is read from the "decode threads" part of the textures line:
  // used / prefetched, and "render thread waited".
  // CHECK: the entry is made (CreateTexture, the regions) at the lookahead, a few draws before
  // Bind2D would; a draw that never binds the texture (skipped, or served by a product that
  // appeared in between) leaves an entry that is evicted after kEvictFrames like any other.
  if (!pool_.running()) {
    return;
  }
  ++stats_.prefetch_looked;
  // A fetch repeats in many draws: one that was decided on in this epoch (a write notification, an
  // eviction or a trim starts a new one) is not decided on again.
  uint64_t h = 0;
  for (uint32_t i = 0; i < 6; ++i) {
    h = (h ^ dwords[i]) * 0x9E3779B97F4A7C15ull;
  }
  h ^= h >> 32;
  SeenFetch& seen = prefetch_seen_[size_t(h) & (kSeenFetches - 1)];
  if (seen.hash == h && seen.epoch == epoch_) {
    return;
  }
  seen.hash = h;
  seen.epoch = epoch_;

  const TextureFetch fetch = ReadTextureFetch(dwords);
  if (!fetch.is_texture) {
    return;
  }
  const TextureKey& key = fetch.key;
  // What Bind2D would refuse, serve from a product, or serve from the resolve memory is not decoded
  // from guest memory.
  if (key.dimension != uint8_t(xe::DataDimension::k2DOrStacked) || key.stacked ||
      format::TextureHost(key.format).status != format::Status::kOk) {
    return;
  }
  // Mods look at the guest's bytes first (a replacement needs no decode) and a texture dump
  // wants them as they are at the upload: left to the draw.
  if (!mods_.empty() || REXCVAR_GET(mcla_gpu_texture_dump)) {
    return;
  }
  if (entries_.find(key) != entries_.end() || failed_.find(key) != failed_.end()) {
    return;
  }
  // The key's layout is worked out once: the ranges, the entry's own and the plan below all come
  // from it.
  const tu::TextureGuestLayout layout = LayoutOf(key);
  uint32_t ranges[2][2];
  RangesOfLayout(key, layout, ranges);
  if (uint64_t(ranges[0][1]) + ranges[1][1] < kPrefetchMinBytes) {
    ++stats_.prefetch_small;
    return;  // the draw decodes it: a job would cost more than its decode saves
  }
  // (ResolveCache::Find would say the same for the key's own product, but it marks the product
  // used: the overlap test reads only.)
  if (resolve_memory_->Holds(key)) {
    return;
  }
  for (const auto& r : ranges) {
    if (r[1] && resolve_->FindOverlap(r[0], r[1])) {
      return;
    }
  }
  if (!pool_.Fits(1ull << 20)) {
    return;  // the decoded textures already waiting hold enough memory
  }

  // The notifications queued so far are taken first (as Sync does before every draw): the entry
  // is then made, and its pages armed, after the writes already seen, and a job is not made stale
  // by one that was only waiting in the queue.
  DrainInvalidations();
  std::string why;
  Entry* e = Create(key, fetch, &why, ranges);
  if (!e) {
    return;  // the draw's own Bind2D tries again and reports it
  }
  // As good as bound in this frame: neither the eviction nor the budget's trim may take it before
  // its first draw.
  e->last_frame = frame_;
  if (!e->watchable) {
    return;  // hashed: decoded when it is bound, as before
  }
  // Arm BEFORE the workers read, as Bind2D does before its own read: a write after it makes a
  // notification, which moves the entry's version and makes the draw decode again.
  if (!e->armed) {
    ArmTextureRanges(*memory_, e->ranges);
    e->armed = true;
  }

  const FormatInfo* info = FormatInfo::Get(key.format);
  auto job = std::make_shared<PrefetchedTexture>();
  job->levels.reserve(size_t(key.mip_max_level) + 1);
  uint64_t total = 0;
  for (uint32_t level = 0; level <= key.mip_max_level; ++level) {
    TextureLevelPlan plan;
    const PlanResult planned =
        PlanLevel(*memory_, key, *info, e->host, e->decompressed, layout, level, &plan, &why);
    if (planned == PlanResult::kSkip) {
      continue;
    }
    if (planned == PlanResult::kFail) {
      return;  // the draw's Upload reports it
    }
    plan.offset = total;
    total += plan.size;
    job->levels.push_back(plan);
  }
  if (job->levels.empty() || !pool_.Fits(total)) {
    return;
  }
  // Uninitialized: the worker writes every row it is asked for.
  job->data.reset(new uint8_t[size_t(total)]);
  job->version = e->version;
  job->frame = frame_;
  job->sequence = sequence;
  job->verify = REXCVAR_GET(mcla_gpu_texture_decode_verify);
  job->Hold(pool_, total);
  e->prefetch = job;
  pool_.Submit(job);
  ++stats_.prefetched;
  stats_.prefetch_bytes += total;
}

bool TextureCache::Upload(Entry& e, plume::RenderCommandList* list, std::string* why) {
  // What the pool decoded ahead of this draw, if anything: taken out of the entry whichever way
  // the upload goes.
  std::shared_ptr<PrefetchedTexture> ahead = std::move(e.prefetch);
  e.prefetch.reset();
  // New guest contents: the replacement they hash to, if a mod has one,
  // holds the texture instead of a decode.
  if (SelectReplacement(e, list)) {
    return true;
  }
  const auto t0 = Clock::now();
  const TextureKey& key = e.key;
  const uint32_t host_block_bytes = plume::RenderFormatSize(e.host_format);
  const uint32_t host_block_width = plume::RenderFormatBlockWidth(e.host_format);

  // The pool's levels are the guest's bytes while the watch has not been notified since the pages
  // were armed (the entry's version is the one it was made under) and every level decoded. A
  // worker that is on them is waited for; one that has not started is taken over here.
  if (ahead) {
    if (ahead->version == e.version) {
      const auto wait_start = Clock::now();
      const bool finished = pool_.Finish(*ahead);
      stats_.prefetch_wait_ns += NsSince(wait_start);
      if (finished && ahead->complete) {
        ++stats_.prefetch_used;
        if (ahead->verify) {
          stats_.verified += ahead->levels.size();
          if (ahead->mismatch) {
            ++stats_.verify_mismatches;
            if (reported_.insert(key).second) {
              REXLOG_ERROR("[gpu] textures: {} decoded ahead: the fast decoder's bytes differ from "
                           "the per-block decoder's (reported once per texture)",
                           key.Describe());
            }
          }
        }
      } else {
        ++stats_.prefetch_dropped;
        ahead.reset();
      }
    } else {
      ++stats_.prefetch_dropped;
      ahead.reset();
    }
  }

  if (e.layout != plume::RenderTextureLayout::COPY_DEST) {
    list->barriers(plume::RenderBarrierStage::COPY,
                   plume::RenderTextureBarrier(e.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    e.layout = plume::RenderTextureLayout::COPY_DEST;
  }
  bool ok = true;
  // Records the copy of one level: its decoded rows (`pitch` apart, `size` bytes) go to the upload
  // ring and from there into the texture. False: no upload memory (`why` says).
  const auto copy_level = [&](uint32_t level, uint32_t width, uint32_t height, uint64_t pitch,
                              uint64_t size, const uint8_t* decoded) {
    const UploadSpan span = upload_->Allocate(slot_, size, kTextureCopyOffsetAlignment);
    if (!span) {
      *why = std::format("no upload memory for {} bytes", size);
      return false;
    }
    std::memcpy(span.cpu, decoded, size_t(size));
    const uint32_t row_width = uint32_t(pitch / host_block_bytes) * host_block_width;
    list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(e.texture.get(), level, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(span.buffer, e.host_format, width,
                                                          height, 1, row_width, span.offset));
    stats_.upload_bytes += size;
    return true;
  };
  if (ahead) {
    // Every level the pool decoded brought the plan Prefetch made for it (the same key, format and
    // guest memory, whose pages were readable then and have not been written since: the version
    // is the one the plan was made under): it is not planned again, which would work out the
    // layout and ask the guest's heap about every level once more.
    // CHECK: a level whose memory stopped being readable between the lookahead and this draw
    // uploads the bytes the worker read; planning again would have failed the upload.
    for (const TextureLevelPlan& done : ahead->levels) {
      if (!copy_level(done.level, done.request.width, done.request.height, done.pitch, done.size,
                      ahead->data.get() + done.offset)) {
        ok = false;
        break;
      }
    }
  } else {
    const FormatInfo* info = FormatInfo::Get(key.format);
    const tu::TextureGuestLayout layout = LayoutOf(key);
    for (uint32_t level = 0; level <= key.mip_max_level; ++level) {
      TextureLevelPlan plan;
      const PlanResult planned =
          PlanLevel(*memory_, key, *info, e.host, e.decompressed, layout, level, &plan, why);
      if (planned == PlanResult::kSkip) {
        continue;
      }
      if (planned == PlanResult::kFail) {
        ok = false;
        break;
      }
      const texture_decode::Request& r = plan.request;
      const uint64_t pitch = plan.pitch;
      const uint64_t size = plan.size;
      scratch_.resize(size_t(size));
      bool decode_ok;
      if (REXCVAR_GET(mcla_gpu_texture_decode_verify)) {
        bool mismatch = false;
        decode_ok = texture_decode::DecodeVerified(r, scratch_.data(), uint32_t(pitch), &mismatch);
        ++stats_.verified;
        if (mismatch) {
          ++stats_.verify_mismatches;
          if (reported_.insert(key).second) {
            REXLOG_ERROR("[gpu] textures: {} level {} at {:08x}: the fast decoder's bytes differ "
                         "from the per-block decoder's (reported once per texture)",
                         key.Describe(), level, plan.address);
          }
        }
      } else {
        decode_ok = texture_decode::Decode(r, scratch_.data(), uint32_t(pitch));
      }
      if (!decode_ok) {
        *why = std::format("level {} at {:08x}: a block beyond the {} readable bytes", level,
                           plan.address, r.source.readable);
        ok = false;
        break;
      }
      if (!copy_level(level, r.width, r.height, pitch, size, scratch_.data())) {
        ok = false;
        break;
      }
    }
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(e.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  e.layout = plume::RenderTextureLayout::SHADER_READ;
  ++stats_.uploads;
  stats_.decode_ns += NsSince(t0);
  return ok;
}

bool TextureCache::SelectReplacement(Entry& e, plume::RenderCommandList* list) {
  const bool dumping = REXCVAR_GET(mcla_gpu_texture_dump);
  e.base_hash = 0;
  if ((!mods_.empty() || dumping) && e.ranges[0][1]) {
    // The key every dump and replacement agrees on (SDK HashGuestData):
    // XXH3-64 of the base level's whole extent at its address.
    const uint8_t* src = memory_->Read(e.ranges[0][0], e.ranges[0][1]);
    if (src) {
      e.base_hash = XXH3_64bits(src, e.ranges[0][1]);
    }
  }
  if (dumping && e.base_hash) {
    DumpEntry(e, e.base_hash);
  }
  Replacement* r =
      e.base_hash && !mods_.empty() ? ReplacementFor(e.base_hash, list) : nullptr;
  if (r != e.replacement) {
    e.replacement = r;
    ++epoch_;  // what this texture's fetches bind changed
  }
  return r != nullptr;
}

TextureCache::Replacement* TextureCache::ReplacementFor(uint64_t hash,
                                                        plume::RenderCommandList* list) {
  const auto known = replacements_.find(hash);
  if (known != replacements_.end()) {
    return known->second.get();
  }
  const std::filesystem::path* path = mods_.Find(hash);
  if (!path) {
    return nullptr;
  }
  const auto refuse = [&](const std::string& why) -> Replacement* {
    REXLOG_ERROR("[gpu] texture mods: {} is not used: {}; the game's texture {:016x} stays "
                 "(reported once)",
                 path->string(), why, hash);
    replacements_.emplace(hash, nullptr);
    return nullptr;
  };
  ReplacementImage image;
  std::string why;
  if (!TextureMods::LoadDds(*path, &image, &why)) {
    return refuse(why);
  }
  auto r = std::make_unique<Replacement>();
  r->format = image.format;
  r->width = image.width;
  r->height = image.height;
  r->levels = uint32_t(image.levels.size());
  const plume::RenderTextureDesc desc =
      plume::RenderTextureDesc::Texture2D(r->width, r->height, r->levels, r->format);
  const std::string name = std::format("texture replacement {:016x}", hash);
  r->texture = device_->CreateTexture(desc, name.c_str());
  if (!r->texture) {
    return refuse(std::format("the host cannot make a {} {}x{} texture with {} levels",
                              format::PlumeFormatName(r->format), r->width, r->height, r->levels));
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(r->texture.get(), plume::RenderTextureLayout::COPY_DEST));
  const uint32_t block_bytes = plume::RenderFormatSize(r->format);
  const uint32_t block_width = plume::RenderFormatBlockWidth(r->format);
  for (uint32_t level = 0; level < r->levels; ++level) {
    const ReplacementImage::Level& l = image.levels[level];
    const uint64_t pitch = AlignUp(l.row_bytes, kTextureCopyPitchAlignment);
    const UploadSpan span =
        upload_->Allocate(slot_, pitch * l.rows, kTextureCopyOffsetAlignment);
    if (!span) {
      // Recorded copies of the earlier levels still read their spans: the
      // texture is retired with the frame, not freed now.
      frames_->Retire(std::move(r->texture));
      return refuse(std::format("no upload memory for level {} ({} bytes)", level,
                                pitch * l.rows));
    }
    for (uint32_t row = 0; row < l.rows; ++row) {
      std::memcpy(span.cpu + row * pitch, l.data.data() + size_t(row) * l.row_bytes,
                  l.row_bytes);
    }
    r->bytes += l.data.size();
    list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(r->texture.get(), level, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            span.buffer, r->format, l.width, l.height, 1,
            uint32_t(pitch / block_bytes) * block_width, span.offset));
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(r->texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  ++stats_.replacements_loaded;
  REXLOG_INFO("[gpu] texture mods: {:016x} replaced by {} ({} {}x{}, {} levels)", hash,
              path->string(), format::PlumeFormatName(r->format), r->width, r->height, r->levels);
  Replacement* raw = r.get();
  replacements_.emplace(hash, std::move(r));
  return raw;
}

void TextureCache::ReloadMods() {
  mods_.Initialize();
  // Every replacement goes, refusals too: a refused file may have been fixed.
  for (auto& [hash, r] : replacements_) {
    if (r) {
      r->views.Release(*frames_, *heap_);
      frames_->Retire(std::move(r->texture));
    }
  }
  replacements_.clear();
  // An entry is uploaded again when it held a replacement or now has one; the
  // upload selects it. Entries uploaded while no mod was indexed have no hash
  // yet: it is taken here, once.
  size_t again = 0;
  for (auto& [key, entry] : entries_) {
    Entry& e = *entry;
    if (!e.base_hash && !mods_.empty() && e.ranges[0][1]) {
      if (const uint8_t* src = memory_->Read(e.ranges[0][0], e.ranges[0][1])) {
        e.base_hash = XXH3_64bits(src, e.ranges[0][1]);
      }
    }
    if (e.replacement || (e.base_hash && mods_.Find(e.base_hash))) {
      e.replacement = nullptr;
      e.uploaded_version = 0;
      ++again;
    }
  }
  ++epoch_;  // no memoized binding may name a released slot
  REXLOG_INFO("[gpu] texture mods: reloaded, {} file(s), {} texture(s) uploaded again",
              mods_.size(), again);
}

void TextureCache::DumpEntry(const Entry& e, uint64_t hash) {
  const TextureKey& key = e.key;
  if (!e.ranges[0][1] || !mcla::IsDumpableTextureFormat(xe::TextureFormat(key.format))) {
    return;
  }
  const uint8_t* src = memory_->Read(e.ranges[0][0], e.ranges[0][1]);
  if (!src) {
    return;
  }
  // Not e.base_hash: the bytes may have changed since the last upload, and
  // the name has to be the hash of what is written.
  if (!hash) {
    hash = XXH3_64bits(src, e.ranges[0][1]);
  }
  if (!dumped_.insert(hash).second) {
    return;
  }
  texture_names::Name name;
  if (!texture_names::Lookup(key.base_page, &name)) {
    name.texture = "unnamed";
    name.dictionary = "unknown";
  }
  const std::filesystem::path folder = mcla::TextureDumpDir() / name.dictionary;
  const std::filesystem::path file =
      folder / std::format("{}__{:016x}.dds", name.texture, hash);
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) {
    return;  // an earlier session wrote it
  }
  std::filesystem::create_directories(folder, ec);
  if (mcla::DumpGuestTextureTo(file, key.width, key.height, key.pitch, key.tiled != 0,
                               xe::TextureFormat(key.format), xe::Endian(key.endian), src,
                               e.ranges[0][1])) {
    ++stats_.dumped;
  }
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
  e.prefetch.reset();  // a worker may still hold it: the job keeps its own copy of the plan
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
  device_->NoteReleased(e.bytes);
}

void TextureCache::EvictSome() {
  bool any = false;
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& e = *it->second;
    // Decoded ahead and never taken (the draw was skipped, or read a product): the bytes go.
    if (e.prefetch && e.prefetch->frame + kPrefetchHoldFrames < frame_ &&
        e.prefetch->state() >= DecodeJob::kDone) {
      e.prefetch.reset();
    }
    if (e.last_frame + kEvictFrames < frame_) {
      Destroy(e, false);
      ++stats_.evicted;
      any = true;
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  // Replacements no remaining entry has bound for as long: their textures go
  // (a file refused stays remembered, null, so it is not read again).
  for (auto it = replacements_.begin(); it != replacements_.end();) {
    Replacement* r = it->second.get();
    if (r && r->last_frame + kEvictFrames < frame_) {
      bool held = false;
      for (const auto& [key, entry] : entries_) {
        held |= entry->replacement == r;
      }
      if (!held) {
        r->views.Release(*frames_, *heap_);
        frames_->Retire(std::move(r->texture));
        device_->NoteReleased(r->bytes);
        any = true;
        it = replacements_.erase(it);
        continue;
      }
    }
    ++it;
  }
  if (any) {
    ++epoch_;  // no memoized binding may name a released slot
  }
}

TrimResult TextureCache::Trim(uint64_t idle_frames, uint64_t bytes, uint32_t max_items) {
  TrimResult result;
  std::vector<Entry*> idle;
  for (const auto& [key, entry] : entries_) {
    // last_frame is current to within kRefreshFrames: the memoized bindings are
    // resolved again that often (BeginFrame), and each updates it.
    if (entry->last_frame + idle_frames < frame_) {
      idle.push_back(entry.get());
    }
  }
  std::sort(idle.begin(), idle.end(),
            [](const Entry* a, const Entry* b) { return a->last_frame < b->last_frame; });
  for (Entry* e : idle) {
    if (result.bytes >= bytes || result.items >= max_items) {
      break;
    }
    result.bytes += e->bytes;
    ++result.items;
    const TextureKey key = e->key;
    Destroy(*e, false);
    ++stats_.evicted;
    entries_.erase(key);
  }
  // Replacements no remaining entry holds and nothing bound for as long.
  for (auto it = replacements_.begin();
       it != replacements_.end() && result.bytes < bytes && result.items < max_items;) {
    Replacement* r = it->second.get();
    if (r && r->last_frame + idle_frames < frame_) {
      bool held = false;
      for (const auto& [key, entry] : entries_) {
        held |= entry->replacement == r;
      }
      if (!held) {
        result.bytes += r->bytes;
        ++result.items;
        r->views.Release(*frames_, *heap_);
        frames_->Retire(std::move(r->texture));
        device_->NoteReleased(r->bytes);
        it = replacements_.erase(it);
        continue;
      }
    }
    ++it;
  }
  if (result.items) {
    ++epoch_;  // no memoized binding may name a released slot
  }
  return result;
}

uint64_t TextureCache::HeldBytes() const {
  uint64_t bytes = 0;
  for (const auto& [key, entry] : entries_) {
    bytes += entry->bytes;
  }
  for (const auto& [hash, replacement] : replacements_) {
    if (replacement) {
      bytes += replacement->bytes;
    }
  }
  return bytes;
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
  if (!mods_.empty() || stats_.dumped) {
    size_t live = 0;
    for (const auto& [hash, r] : replacements_) {
      live += r != nullptr;
    }
    text += std::format(" | mods: {} files, {} replacements live ({} loaded now), {} binds | "
                        "dumped {}",
                        mods_.size(), live, stats_.replacements_loaded, stats_.replaced_binds,
                        stats_.dumped);
  }
  if (pool_.running()) {
    // prefetched: textures handed to the workers; used: uploads that took their levels; dropped:
    // stale (written since) or incomplete, decoded again; too small: new textures under
    // kPrefetchMinBytes, left to their draws; waited: the render thread in Upload for a worker or
    // on a job none had started; ran on workers: jobs the workers finished.
    text += std::format(
        " | decode threads {}: prefetched {} ({:.2f} MiB/frame), used {}, dropped {}, too small {}, "
        "render thread waited {:.3f} ms/frame, workers ran {}, {:.1f} MiB held, lookahead on for "
        "{:.1f} draws/frame and handed {:.1f} fetches/frame",
        pool_.threads(), stats_.prefetched, double(stats_.prefetch_bytes) / f / 1048576.0,
        stats_.prefetch_used, stats_.prefetch_dropped, stats_.prefetch_small,
        double(stats_.prefetch_wait_ns) / f / 1e6,
        pool_.TakeRanOnWorkers(), double(pool_.held_bytes()) / 1048576.0,
        double(stats_.prefetch_draws) / f, double(stats_.prefetch_looked) / f);
  }
  if (stats_.verified) {
    text += std::format(" | decode check: {} levels compared, {} differ", stats_.verified,
                        stats_.verify_mismatches);
  }
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

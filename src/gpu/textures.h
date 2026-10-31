#pragma once
// ===========================================================================
// MCLA GPU renderer -- guest textures on the host
// ===========================================================================
// Every texture fetch
// constant a draw's shaders read becomes a bindless slot (bit 31 = PWL gamma
// on read, shader_common.h): from a resolve product when the guest rendered
// that texture (resolve.h), or from a host texture decoded from guest memory.
//
// Identity (TextureKey): the fetch constant after the SDK's own cleanup
// (texture_util::GetSubresourcesFromFetchConstant: the base or the mips
// dropped when absent, the mip range clamped to the size), with the base and
// mip addresses physical -- texture headers carry the 0xE0000000 window,
// the device's fetch constants physical addresses (memory
// project_mcla_frame_census) -- plus format, endian, tiling, packed mips,
// dimension, size and pitch. The swizzle and the signs are not part of it:
// one host texture serves every swizzle through views (TextureViews), and the
// gamma sign is the index's bit 31.
//
// A host texture has every level the key names (level 0 even when the base
// is absent: the sampler's mip range keeps it unread) in the host format of
// format.h TextureHost, decoded on the CPU (texture_decode.h) into scratch
// memory, copied in one sequential pass into the frame's upload ring (never
// read back: write-combined, the M4.4 lesson) and copied to the texture.
//
// Staying exact (as GeometryCache): the guest ranges of an entry (its base
// and its mips) are watched -- armed before the copy -- and a write
// notification makes the next use decode again, into the same texture: the
// list's order keeps earlier draws on the earlier contents. An entry whose
// contents change in kDynamicStreak frames of use in a row, or whose pages
// the watch cannot cover (a read-only page), is hashed instead: every frame
// it is used its guest bytes are hashed and compared; it goes back to the
// watch after kStableStreak unchanged frames, the wait doubling at each
// return. Entries unused for kEvictFrames frames give their memory back.
//
// Lookups: a draw reads ~2 fetch constants, a bench frame ~12000. The Scene
// memoizes what Bind2D returns by the fetch's raw dwords and reuses it while
// two counters are unchanged: epoch() -- every write notification that touches
// an entry, every eviction, and BeginFrame every kRefreshFrames frames (which
// keeps the entries' last use current for the eviction) -- and the product
// generation of the fetch's base page (ProductGeneration), which the resolve
// bridge bumps when a product of that page is activated or deactivated: the
// ~13 product changes a frame then invalidate only their own fetches, not the
// whole memo (measured: a global epoch there dropped the memo from 99% to
// 83%). A binding of a hashed entry, or a failure, is only good for the frame
// it was made in (per_frame): those are compared, or retried, once per frame
// of use. Sync() takes the write notifications; the Scene calls it once per
// draw before the memo.
//
// What is not done (logged once per key, the null texture bound; none of it
// in the measured frames): formats outside format.h's table,
// 1D/3D/cube/stacked textures, a signed sign on a UNORM host format, the
// biased sign, a non-zero exponent adjust (TODO M5 for each).
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "descriptors.h"
#include "format.h"
#include "guest_memory.h"
#include "region_index.h"
#include "texture_decode.h"
#include "texture_mods.h"

namespace mcla::gpu {

class Device;
class FrameRing;
class ResolveCache;
class ResolveMemory;
class UploadRing;

// The identity of a guest texture's contents.
struct TextureKey {
  uint32_t base_page = 0;  // physical address >> 12; 0 = no base level
  uint32_t mip_page = 0;   // physical address >> 12; 0 = no mips
  uint16_t width = 0;      // texels
  uint16_t height = 0;
  uint16_t depth = 0;      // 1 for 2D; layers or slices otherwise
  uint16_t pitch = 0;      // base row pitch in texels / 32 (fetch dword 0)
  uint8_t format = 0;      // xenos::TextureFormat
  uint8_t endian = 0;
  uint8_t mip_max_level = 0;
  uint8_t dimension = 0;   // xenos::DataDimension
  uint8_t tiled = 0;
  uint8_t packed_mips = 0;
  uint8_t stacked = 0;
  uint8_t reserved = 0;

  bool operator==(const TextureKey& o) const;
  bool operator!=(const TextureKey& o) const { return !(*this == o); }
  uint32_t levels() const { return uint32_t(mip_max_level) + 1; }
  // "k_DXT1 512x256 tiled @06e65000 mips @06f65000 0-9", for the logs.
  std::string Describe() const;
};
static_assert(sizeof(TextureKey) == 24);

struct TextureKeyHash {
  size_t operator()(const TextureKey& key) const;
};

// The guest memory a 2D texture's contents occupy, from the SDK's layout:
// [0] the base level, [1] the mips (address, bytes; bytes 0 when absent).
void TextureGuestRanges(const TextureKey& key, uint32_t ranges[2][2]);

// Where level `level` of a 2D `key` lives in guest memory and how its blocks
// are laid out there -- what TextureCache::Upload reads, and what a resolve or
// a write-back writes: the physical origin of the level's
// storage (base, mip or packed tail), the bytes the storage spans from it, and
// the decode request (pitch, block size, tiling, endian, tail offset, size in
// blocks; `decompress` kNone, `data`/`readable` unset). False when the level
// has no storage (no base for level 0, no mips above it) or the SDK has no
// block size for the format.
struct TextureLevelStorage {
  uint32_t address = 0;
  uint64_t extent = 0;
  texture_decode::Request request;
};
bool GetTextureLevelStorage(const TextureKey& key, uint32_t level, TextureLevelStorage* out);

// A texture fetch constant read for binding.
struct TextureFetch {
  bool is_texture = false;  // fetch constant type kTexture
  TextureKey key;
  uint16_t swizzle = 0;     // the fetch's 12-bit swizzle
  uint8_t signs[4] = {};    // xenos::TextureSign per component
  uint32_t mip_min = 0;     // as GetSubresourcesFromFetchConstant leaves them
  uint32_t mip_max = 0;
  int32_t exp_adjust = 0;
  bool gamma = false;       // signs x, y, z all kGamma (shader_common.h bit 31)
  // The fetch's own base page (physical), before the SDK's cleanup drops it
  // from the key when the mip range starts above level 0: the image the
  // guest made, which a resolve product of it is keyed by.
  uint32_t image_base_page = 0;
};
// `dwords`: the six host-order dwords of the fetch constant, from the device
// (physical addresses) or from a texture header (windowed): both normalize.
TextureFetch ReadTextureFetch(const uint32_t dwords[6]);

// The host swizzle a view needs: the fetch constant's swizzle over the host
// format's (Xenia TextureCache::GuestToHostSwizzle, SDK pipeline/texture/
// cache.cpp, BSD-3-Clause): a guest component >= 4 is a constant (6 and 7
// folded to 0 and 1), otherwise it reads the host component the host
// format's swizzle names.
uint16_t GuestToHostSwizzle(uint16_t fetch_swizzle, uint16_t host_format_swizzle);

// The views of one host texture, one per (host swizzle, mip count), each in
// its own bindless slot -- shared by memory textures and resolve products (a
// fetch may read a product's first levels only, resolve.h).
class TextureViews {
 public:
  TextureViews() = default;
  TextureViews(const TextureViews&) = delete;
  TextureViews& operator=(const TextureViews&) = delete;

  // The slot of `texture` (2D, its first `levels` mips, `format`) seen with
  // `host_swizzle`, created on first use. kInvalidBindlessSlot on failure
  // (logged by the caller).
  uint32_t Slot(Device& device, BindlessHeap& heap, const plume::RenderTexture& texture,
                plume::RenderFormat format, uint32_t levels, uint16_t host_swizzle,
                const char* name);
  // Hands every slot back to the heap (deferred by the FrameRing) and retires
  // the views. The texture they name must be retired after this.
  void Release(FrameRing& frames, BindlessHeap& heap);
  // GPU idle, heap about to go: drops the views without touching the heap.
  void Drop() { views_.clear(); }
  size_t size() const { return views_.size(); }

 private:
  struct View {
    uint16_t host_swizzle = 0;
    uint32_t levels = 0;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = kInvalidBindlessSlot;
  };
  std::vector<View> views_;
};

class TextureCache {
 public:
  static constexpr uint32_t kDynamicStreak = 3;
  static constexpr uint32_t kStableStreak = 60;
  static constexpr uint32_t kMaxStableStreak = 7680;
  static constexpr uint64_t kEvictFrames = 600;
  static constexpr uint64_t kRefreshFrames = 60;

  struct Binding {
    uint32_t slot = kNullTexture2DSlot;  // with kTextureGammaBit when asked
    bool per_frame = false;              // good for this frame only
    bool from_product = false;           // a resolve product, not guest memory
    // Host texels per guest texel of what is bound: a resolve product's scale
    //, 1 for everything made from guest memory. A reader in
    // texel units (the present) multiplies the guest size by it.
    uint32_t scale = 1;
    // The binding recorded a pass of its own (a decode from the resolve
    // memory): the list has no framebuffer and another pipeline state, and the
    // caller binds its own again.
    bool list_state_changed = false;
    // A texture mod's replacement is bound (texture_mods.h): its own size and
    // mip chain. The sampler's mip range moves by `lod_shift` (log2 of the
    // replacement's width over the guest's) and stays below `levels`.
    bool replaced = false;
    int32_t lod_shift = 0;
    uint32_t levels = 0;
  };

  TextureCache() = default;
  ~TextureCache();
  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload,
                  GuestMemory& memory, ResolveCache& resolve, ResolveMemory& resolve_memory);
  // GPU idle, FrameRing drained, before the BindlessHeap shuts down.
  void Shutdown();

  void BeginFrame(uint32_t slot);

  // The bindless slot (kTextureGammaBit when the fetch asks for the PWL gamma)
  // of the 2D texture `fetch` names, its decode and upload recorded into
  // `list` when needed. The null 2D slot when it cannot be had (counted, the
  // reason logged once per key). `reader_scale`: the resolution scale of the
  // reading draw's targets -- a resolve product made at a larger one is read
  // through its copy reduced to the reader's density (ResolveCache::
  // ReducedSlot), as the console reads it; 0 binds the product as it is (the
  // present, which samples it by its own size).
  Binding Bind2D(const TextureFetch& fetch, uint32_t reader_scale,
                 plume::RenderCommandList* list);

  // Takes the write notifications of textures and resolve products.
  void Sync();
  uint64_t epoch() const { return epoch_; }
  // Every memoized binding goes.
  void BumpEpoch() { ++epoch_; }

  // The generation of the products at physical page `page` (a bucket of
  // pages): bumped by the resolve bridge whenever a product whose base is
  // there is activated or deactivated, which changes what fetches of that
  // page bind.
  static constexpr uint32_t kGenerationBuckets = 4096;
  static uint32_t GenerationBucket(uint32_t page) {
    return (page * 2654435761u) >> (32 - 12);
  }
  uint32_t ProductGeneration(uint32_t page) const {
    return product_generations_[GenerationBucket(page)];
  }
  void BumpProductGeneration(uint32_t page) { ++product_generations_[GenerationBucket(page)]; }

  // Texture dumps (texture_mods.h): every texture bound in the next frame is
  // written to <mods>/textures_dump/<dictionary>/<name>__<hash16>.dds, once
  // per hash per session. mcla_gpu_texture_dump does it for every texture
  // uploaded, all the time. Any thread (a key bind).
  void RequestDump() { dump_requested_.store(true, std::memory_order_relaxed); }

  // One line of statistics since the last call.
  std::string TakeSummary(uint64_t frames);

 private:
  enum class Mode : uint8_t { kWatched, kHashed };
  // A mod's replacement, one per content hash, shared by every entry whose
  // base level hashes to it; uploaded once when first needed.
  struct Replacement {
    std::unique_ptr<plume::RenderTexture> texture;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    TextureViews views;
    uint64_t bytes = 0;
    uint64_t last_frame = 0;
  };
  struct Entry {
    TextureKey key;
    format::TextureHostFormat host;
    plume::RenderFormat host_format = plume::RenderFormat::UNKNOWN;
    bool decompressed = false;
    std::unique_ptr<plume::RenderTexture> texture;
    TextureViews views;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
    // Guest storage: [address, address + bytes) for the base and the mips.
    uint32_t ranges[2][2] = {};
    RegionIndex::Id region_ids[2] = {RegionIndex::kNone, RegionIndex::kNone};
    bool watchable = true;
    Mode mode = Mode::kWatched;
    bool armed = false;
    uint64_t version = 1;           // bumped by write notifications
    uint64_t uploaded_version = 0;  // 0 = never uploaded
    uint64_t uploaded_hash = 0;     // hashed mode: the guest bytes uploaded
    uint64_t last_frame = 0;
    uint64_t checked_frame = 0;     // hashed mode: frame of the last comparison
    bool changed_in_frame = false;
    uint32_t changed_streak = 0;
    uint32_t stable_streak = 0;
    uint32_t stable_required = kStableStreak;
    uint64_t bytes = 0;             // host bytes, for the statistics
    // The content hash of the base level as last uploaded (0: not computed --
    // no mods and no dumping, or no base); the replacement it selects, null
    // when none: the guest's decode is what the entry holds then.
    uint64_t base_hash = 0;
    Replacement* replacement = nullptr;
  };
  Entry* Create(const TextureKey& key, const TextureFetch& fetch, std::string* why);
  // Decodes every level into the frame's ring and records the copies. False:
  // a level could not be read (logged by the caller).
  bool Upload(Entry& entry, plume::RenderCommandList* list, std::string* why);
  uint64_t HashGuest(const Entry& entry);
  // Before a decode: hashes the base level when mods or dumps need it, dumps
  // it when asked, and points the entry at its replacement. True when a
  // replacement holds the texture (no decode then).
  bool SelectReplacement(Entry& entry, plume::RenderCommandList* list);
  // The replacement for `hash`, loaded and uploaded on first use; null when
  // no mod has one or its file was refused (logged once).
  Replacement* ReplacementFor(uint64_t hash, plume::RenderCommandList* list);
  // Writes the entry's base level (hash computed) unless done this session.
  void DumpEntry(const Entry& entry);
  void DrainInvalidations();
  void Destroy(Entry& entry, bool for_shutdown);
  void EvictSome();
  Binding Fail(const TextureKey& key, const char* what, const std::string& detail);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  UploadRing* upload_ = nullptr;
  GuestMemory* memory_ = nullptr;
  GuestMemory::Subscriber subscriber_ = GuestMemory::kNoSubscriber;
  ResolveCache* resolve_ = nullptr;
  ResolveMemory* resolve_memory_ = nullptr;

  std::unordered_map<TextureKey, std::unique_ptr<Entry>, TextureKeyHash> entries_;
  RegionIndex regions_;
  // By region id: every entry over that exact range (two keys may read the
  // same bytes -- another format or size); a write reaches all of them.
  std::vector<std::vector<Entry*>> region_owners_;
  std::unordered_set<TextureKey, TextureKeyHash> failed_;    // failures, reported once
  std::unordered_set<TextureKey, TextureKeyHash> reported_;  // warnings, reported once
  std::vector<GuestMemory::Range> drained_;
  std::vector<uint8_t> scratch_;
  uint64_t epoch_ = 1;
  std::array<uint32_t, kGenerationBuckets> product_generations_{};
  uint32_t slot_ = 0;
  uint64_t frame_ = 1;
  std::vector<TextureKey> evict_scan_;
  size_t evict_cursor_ = 0;

  TextureMods mods_;
  // By content hash; null = no file for it, or its file was refused.
  std::unordered_map<uint64_t, std::unique_ptr<Replacement>> replacements_;
  std::unordered_set<uint64_t> dumped_;  // hashes written this session
  std::atomic<bool> dump_requested_{false};
  uint64_t dump_frame_ = 0;  // the frame a requested dump covers (0: none)

  struct Stats {
    uint64_t lookups = 0;  // Bind2D calls (the Scene's memo misses)
    uint64_t product_hits = 0;
    uint64_t product_reduced = 0;  // ... bound through a copy reduced to the reader's scale
    uint64_t memory_hits = 0;
    uint64_t created = 0;
    uint64_t uploads = 0;
    uint64_t upload_bytes = 0;
    uint64_t decompressed = 0;   // textures decompressed to RGBA8
    uint64_t invalidations = 0;
    uint64_t hashes = 0;
    uint64_t hashed_bytes = 0;
    uint64_t to_hashed = 0;
    uint64_t to_watched = 0;
    uint64_t evicted = 0;
    uint64_t failures = 0;
    uint64_t reinterpretations = 0;  // memory read under an active product (not held by
                                     // the resolve memory)
    uint64_t from_resolve_memory = 0;      // decoded from the resolve memory
    uint64_t resolve_memory_unsupported = 0;
    uint64_t decode_ns = 0;
    uint64_t replaced_binds = 0;       // bindings of a mod's replacement
    uint64_t replacements_loaded = 0;  // replacement files uploaded
    uint64_t dumped = 0;               // textures written to the dump folder
  };
  Stats stats_;
};

}  // namespace mcla::gpu

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the resolve bridge: what the guest rendered, as textures
// ===========================================================================
// On the console a resolve
// (D3DDevice_Resolve, sub_82420BA8) copies an EDRAM rectangle into a texture
// in memory; every later fetch of that texture reads it. Here the copy never
// reaches guest memory (write-back is M5): it goes into a PRODUCT -- a host
// texture keyed exactly as the fetches of the destination will be keyed
// (TextureKey; 10280 of 10280 fetches in the bench census read a resolve
// destination with its own fetch constant), with every mip of the destination,
// in the host format the texture cache uses for that format (format.h).
//
// The copy is a full-screen pass over the product level's rectangle
// (host_passes.h, hlsl/resolve_ps.hlsl): the EDRAM target is read through its
// shader views (edram.h SampleSlots), samples selected and averaged as the
// guest asked, the exponent bias applied, red and blue swapped when the
// destination's swizzle does not start with X, and the value written as the
// destination's fetch will read it -- quantized as Xenia packs it, so the host
// target stores it exactly; depth copied raw (depth24 << 8 | stencil) and read
// as k_24_8, k_24_8_FLOAT, k_8_8_8_8 or k_32_FLOAT. Then the clears the call
// asked for (0x100 colour on the source target, 0x200 depth and stencil),
// with the clear records' rule (edram.h EncodeColorClear); 0x2000 = only the
// clears. The rectangle is the guest's, x0/y0 down and x1/y1 up to 8 (the
// hardware's granularity; the IDB rounds x1/y1, Xenia both); destination =
// source - (x0, y0) + point.
//
// Versioned in time: the same memory is resolved into with different shapes
// within a frame (the shadow atlas and the post-processing targets share one
// pool; the front buffer takes a float16 copy of the scene and then the 8888
// UI composite). A product is ACTIVE -- found by fetches -- until a resolve
// writes an overlapping range with another key, which deactivates it; an
// inactive product waits in a reservoir for its key to be resolved again
// (re-creating textures twice a frame would be the wrong cost) and dies after
// kEvictFrames frames unused. A CPU write into an active product's range (the
// write watch, armed at every activation) deactivates it too: the fetch goes
// back to memory, which is what the guest wrote there (logged four times,
// counted). Seen while driving: the vinyl work area (1024x1024 and 512x512 at
// one address), which the CPU locks and distills in place -- it reads memory
// that does not hold the image until M5's write-back.
//
// Not done (logged once, counted; none in the measured frames): 3D or array
// destinations, a slice other than 0, destination formats outside the
// conversions above, a destination offset that is not a whole number of 32x32
// tiles (the console's addressing would scatter it; copied as a plain offset).
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "guest_memory.h"
#include "records.h"
#include "region_index.h"
#include "textures.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class EdramTargets;
class FrameRing;
class HostPasses;
class PassTiming;

class ResolveCache {
 public:
  static constexpr uint64_t kEvictFrames = 600;

  struct Product {
    TextureKey key;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    std::unique_ptr<plume::RenderTexture> texture;
    TextureViews views;  // sampling views, per host swizzle
    // Render target views and framebuffers, per level.
    std::vector<std::unique_ptr<plume::RenderTextureView>> level_views;
    std::vector<std::unique_ptr<plume::RenderFramebuffer>> level_framebuffers;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
    bool active = false;
    // Guest ranges the destination occupies: base and mips.
    uint32_t ranges[2][2] = {};
    RegionIndex::Id region_ids[2] = {RegionIndex::kNone, RegionIndex::kNone};
    uint64_t last_frame = 0;
  };

  ResolveCache() = default;
  ~ResolveCache();
  ResolveCache(const ResolveCache&) = delete;
  ResolveCache& operator=(const ResolveCache&) = delete;

  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, GuestMemory& memory,
                  TextureCache& textures, EdramTargets& edram, HostPasses& passes,
                  PassTiming& pass_timing);
  // GPU idle, FrameRing drained, before the BindlessHeap shuts down.
  void Shutdown();

  void BeginFrame();

  // Executes a resolve record into `list` (null: the device is lost; counted).
  // Records outside any render pass and leaves none open; it binds its own
  // pipeline state, which the caller binds again.
  void Execute(const ResolveRecord& record, plume::RenderCommandList* list);

  // Takes the write notifications (TextureCache::Sync calls it once per draw).
  void Sync() { DrainInvalidations(); }
  // The active product a fetch of `key` reads, or null: the product of that
  // very key, or the active product of the same image (the key without its
  // mip fields) whose levels include the fetch's -- the reflection's resolve
  // writes a 4-level texture and a pass reads its base through a fetch with no
  // mips. Its first key.levels() levels are what the fetch sees.
  Product* Find(const TextureKey& key);
  // The key without its mip fields (base, size, format, layout).
  static TextureKey ImageKey(const TextureKey& key);
  // An active product whose guest range shares a byte with [start, start +
  // length), or null (the reinterpretation report).
  const Product* FindOverlap(uint32_t start, uint32_t length);

  std::string TakeSummary(uint64_t frames);

 private:
  // Conversions, as resolve_ps.hlsl numbers them.
  enum Mode : uint32_t {
    kModeUnorm8888 = 0,
    kModeUnorm1555 = 1,
    kModeUnorm8 = 2,
    kModeFloat = 3,
    kModeRed32 = 4,
    kModeDepthFloat = 5,
    kModeDepthRaw8888 = 6,
    kModeDepthFloatBits = 7,
    kModeCount,
  };
  enum ModeFlags : uint32_t {
    kFlagSwapRedBlue = 1u << 8,
    kFlagGammaSource = 1u << 9,
    kFlagSourceFloat24 = 1u << 10,
    kFlagDestFloat24 = 1u << 11,
  };
  struct Push {
    uint32_t source;
    uint32_t stencil;
    int32_t offset_x;
    int32_t offset_y;
    uint32_t samples;
    float scale;
    uint32_t mode;
    uint32_t reserved;
  };
  static_assert(sizeof(Push) == 32);

  enum class Failure : uint8_t {
    kDeviceLost,
    kUnreadable,       // the record's source header or destination could not be read
    kNoSource,         // the source surface is not bound
    kSourceFormat,     // a source target the host does not make
    kDestination,      // a destination this bridge does not handle (see top)
    kConversion,       // a source/destination pair with no conversion
    kProduct,          // the product texture, a view or a framebuffer not made
    kSourceView,       // the source target's shader views not made
    kPipeline,
    kCount,
  };

  Product* Activate(const TextureKey& key, plume::RenderFormat format, plume::RenderCommandList* list,
                    std::string* why);
  plume::RenderFramebuffer* LevelFramebuffer(Product& product, uint32_t level);
  plume::RenderPipeline* Pipeline(bool depth, bool msaa, plume::RenderFormat format);
  void TransitionProduct(plume::RenderCommandList* list, Product& product,
                         plume::RenderTextureLayout layout);
  void Fail(Failure failure, const std::string& detail);
  void DrainInvalidations();
  void Deactivate(Product& product);
  void IndexRanges(Product& product);
  void UnindexRanges(Product& product);
  void EvictSome();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  GuestMemory* memory_ = nullptr;
  GuestMemory::Subscriber subscriber_ = GuestMemory::kNoSubscriber;
  TextureCache* textures_ = nullptr;
  EdramTargets* edram_ = nullptr;
  HostPasses* passes_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  // Pixel shaders: [depth][msaa].
  std::unique_ptr<plume::RenderShader> shaders_[2][2];
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;

  std::unordered_map<TextureKey, std::unique_ptr<Product>, TextureKeyHash> products_;
  // Active products by ImageKey (at most one: two would overlap).
  std::unordered_map<TextureKey, Product*, TextureKeyHash> active_by_image_;
  RegionIndex regions_;  // ranges of ACTIVE products only
  std::vector<std::vector<Product*>> region_owners_;  // by region id
  std::vector<GuestMemory::Range> drained_;
  std::vector<Product*> scratch_hits_;
  uint64_t frame_ = 1;
  std::array<uint32_t, size_t(Failure::kCount)> failures_logged_{};
  uint32_t cpu_writes_logged_ = 0;

  struct Stats {
    uint64_t resolves = 0;
    uint64_t copies = 0;
    uint64_t clear_only = 0;
    uint64_t color_clears = 0;
    uint64_t depth_clears = 0;
    std::array<uint64_t, kModeCount> modes{};
    std::array<uint64_t, size_t(Failure::kCount)> failures{};
    uint64_t created = 0;
    uint64_t activated = 0;           // products found by fetches again (new or back)
    uint64_t deactivated_by_overlap = 0;
    uint64_t deactivated_by_cpu = 0;
    uint64_t partial = 0;             // copies that do not cover their whole level
    uint64_t misaligned = 0;          // destination offset not in whole 32x32 tiles
    uint64_t from_registers = 0;      // source surface not bound: taken from the registers
    uint64_t found_by_image = 0;      // fetches served the first levels of a product
    uint64_t pixels = 0;
    uint64_t finds = 0;
    uint64_t found = 0;
    uint64_t evicted = 0;
  };
  Stats stats_;
};

}  // namespace mcla::gpu

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the resolve bridge: what the guest rendered, as textures
// ===========================================================================
// On the console a resolve
// copies an EDRAM rectangle into a texture in memory; every later fetch of
// that texture reads it. Here the copy never reaches guest memory (write-back
// is M5): it goes into a PRODUCT -- a host texture keyed exactly as the
// fetches of the destination will be keyed (TextureKey; 10280 of 10280
// fetches in the bench census read a resolve destination with its own fetch
// constant), with every mip of the destination.
//
// Versioned in time: the same memory is resolved into with different shapes
// within a frame (the shadow atlas and the post-processing targets share one
// pool; the front buffer takes a float16 copy of the scene and then the
// 8888 UI composite). A product is ACTIVE -- found by fetches -- until a
// resolve writes an overlapping range with another key, which deactivates it;
// an inactive product waits in a reservoir for its key to be resolved again
// (re-creating textures twice a frame would be the wrong cost) and dies after
// kEvictFrames frames unused. A CPU write into an active product's range (the
// write watch) deactivates it too: the fetch goes back to memory, which is
// what the guest wrote there.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "guest_memory.h"
#include "region_index.h"
#include "textures.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;

class ResolveCache {
 public:
  static constexpr uint64_t kEvictFrames = 600;

  struct Product {
    TextureKey key;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    std::unique_ptr<plume::RenderTexture> texture;
    TextureViews views;
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

  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, GuestMemory& memory,
                  TextureCache& textures);
  // GPU idle, FrameRing drained, before the BindlessHeap shuts down.
  void Shutdown();

  void BeginFrame();

  // Takes the write notifications (TextureCache::Sync calls it once per draw).
  void Sync() { DrainInvalidations(); }
  // The active product of `key`, or null.
  Product* Find(const TextureKey& key);
  // An active product whose guest range shares a byte with [start, start +
  // length), or null (the reinterpretation report).
  const Product* FindOverlap(uint32_t start, uint32_t length);

  std::string TakeSummary(uint64_t frames);

 private:
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

  std::unordered_map<TextureKey, std::unique_ptr<Product>, TextureKeyHash> products_;
  RegionIndex regions_;  // ranges of ACTIVE products only
  std::vector<std::vector<Product*>> region_owners_;  // by region id
  std::vector<GuestMemory::Range> drained_;
  uint64_t frame_ = 1;

  struct Stats {
    uint64_t finds = 0;
    uint64_t found = 0;
    uint64_t deactivated_by_cpu = 0;
    uint64_t evicted = 0;
  };
  Stats stats_;
};

}  // namespace mcla::gpu

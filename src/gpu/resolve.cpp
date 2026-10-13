// MCLA GPU renderer -- the resolve bridge. See resolve.h.

#include "resolve.h"

#include <algorithm>
#include <format>

#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"

namespace mcla::gpu {

ResolveCache::~ResolveCache() {
  Shutdown();
}

void ResolveCache::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                              GuestMemory& memory, TextureCache& textures) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  memory_ = &memory;
  textures_ = &textures;
  subscriber_ = memory.Subscribe("resolve products");
}

void ResolveCache::Shutdown() {
  // The GPU is idle and the heap goes next: nothing is handed back to it.
  for (auto& [key, product] : products_) {
    product->views.Drop();
  }
  products_.clear();
  regions_.Clear();
  region_owners_.clear();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  memory_ = nullptr;
  textures_ = nullptr;
}

void ResolveCache::BeginFrame() {
  ++frame_;
  if (frame_ % 60 == 0) {
    EvictSome();
  }
}

void ResolveCache::IndexRanges(Product& p) {
  for (uint32_t i = 0; i < 2; ++i) {
    if (!p.ranges[i][1]) {
      continue;
    }
    RegionIndex::Id id = regions_.Find(p.ranges[i][0], p.ranges[i][1]);
    if (id == RegionIndex::kNone) {
      id = regions_.Insert(p.ranges[i][0], p.ranges[i][1]);
    }
    if (region_owners_.size() < regions_.id_bound()) {
      region_owners_.resize(regions_.id_bound());
    }
    region_owners_[id].push_back(&p);
    p.region_ids[i] = id;
  }
}

void ResolveCache::UnindexRanges(Product& p) {
  for (RegionIndex::Id& id : p.region_ids) {
    if (id == RegionIndex::kNone) {
      continue;
    }
    if (id < region_owners_.size()) {
      std::vector<Product*>& owners = region_owners_[id];
      owners.erase(std::remove(owners.begin(), owners.end(), &p), owners.end());
      if (owners.empty()) {
        regions_.Remove(id);
      }
    }
    id = RegionIndex::kNone;
  }
}

void ResolveCache::Deactivate(Product& p) {
  if (!p.active) {
    return;
  }
  p.active = false;
  UnindexRanges(p);
  textures_->BumpEpoch();
}

void ResolveCache::DrainInvalidations() {
  if (!memory_->HasInvalidations(subscriber_)) {
    return;
  }
  drained_.clear();
  memory_->Drain(subscriber_, drained_);
  std::vector<Product*> hit;
  for (const GuestMemory::Range& range : drained_) {
    regions_.ForEachOverlap(range.start, range.length, [&](RegionIndex::Id id) {
      if (id < region_owners_.size()) {
        for (Product* p : region_owners_[id]) {
          hit.push_back(p);
        }
      }
    });
  }
  for (Product* p : hit) {
    if (p->active) {
      // The guest wrote into what it rendered: its own bytes win (M5 writes the
      // image back so a partial write merges instead).
      ++stats_.deactivated_by_cpu;
      REXLOG_WARN("[gpu] resolve: the CPU wrote into the product {}; it is deactivated and the "
                  "texture is decoded from memory again",
                  p->key.Describe());
      Deactivate(*p);
    }
  }
}

ResolveCache::Product* ResolveCache::Find(const TextureKey& key) {
  ++stats_.finds;
  const auto it = products_.find(key);
  if (it == products_.end() || !it->second->active) {
    return nullptr;
  }
  ++stats_.found;
  it->second->last_frame = frame_;
  return it->second.get();
}

const ResolveCache::Product* ResolveCache::FindOverlap(uint32_t start, uint32_t length) {
  const Product* found = nullptr;
  regions_.ForEachOverlap(start, length, [&](RegionIndex::Id id) {
    if (!found && id < region_owners_.size() && !region_owners_[id].empty()) {
      found = region_owners_[id].front();
    }
  });
  return found;
}

void ResolveCache::EvictSome() {
  for (auto it = products_.begin(); it != products_.end();) {
    Product& p = *it->second;
    if (!p.active && p.last_frame + kEvictFrames < frame_) {
      p.views.Release(*frames_, *heap_);
      frames_->Retire(std::move(p.texture));
      ++stats_.evicted;
      it = products_.erase(it);
    } else {
      ++it;
    }
  }
}

std::string ResolveCache::TakeSummary(uint64_t frames) {
  (void)frames;
  size_t active = 0;
  for (const auto& [key, p] : products_) {
    active += p->active;
  }
  std::string text = std::format(
      "resolve products: {} ({} active) | fetch lookups {}, found {} | deactivated by CPU "
      "writes {}, evicted {}",
      products_.size(), active, stats_.finds, stats_.found, stats_.deactivated_by_cpu,
      stats_.evicted);
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu

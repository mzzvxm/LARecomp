#pragma once
// ===========================================================================
// MCLA GPU renderer -- index of guest memory regions
// ===========================================================================
// The regions of guest physical memory a cache holds copies of, found by exact
// range and by overlap with a written range. Regions may overlap each other -- one vertex buffer seen through two
// fetch extents, several index ranges of one buffer -- so the overlap search
// starts at (start - longest region): an index keyed by start alone that
// assumed disjoint regions skipped the big region containing a written page,
// which was the legacy runtime's third watch bug (22/09).
//
// Ids are small integers, reused after Remove, so a cache can keep its own
// per-region state in a plain vector indexed by id.
//
// Threading: one thread (the render thread).
// ===========================================================================

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace mcla::gpu {

class RegionIndex {
 public:
  using Id = uint32_t;
  static constexpr Id kNone = UINT32_MAX;

  struct Region {
    uint32_t start = 0;
    uint32_t length = 0;
  };

  Id Find(uint32_t start, uint32_t length) const;
  // A region with this exact range must not exist yet.
  Id Insert(uint32_t start, uint32_t length);
  void Remove(Id id);
  void Clear();

  const Region& region(Id id) const { return regions_[id]; }
  size_t size() const { return exact_.size(); }
  // One past the largest id ever handed out: the size a per-id vector needs.
  size_t id_bound() const { return regions_.size(); }

  // Calls fn(id) for every region that shares at least one byte with
  // [start, start + length).
  template <typename Fn>
  void ForEachOverlap(uint32_t start, uint32_t length, Fn&& fn) const {
    if (length == 0 || by_start_.empty()) {
      return;
    }
    const uint64_t end = uint64_t(start) + length;
    const uint32_t scan_from = start > max_length_ ? start - max_length_ : 0;
    for (auto it = by_start_.lower_bound(scan_from); it != by_start_.end() && it->first < end;
         ++it) {
      const Region& r = regions_[it->second];
      if (uint64_t(r.start) + r.length > start) {
        fn(it->second);
      }
    }
  }

 private:
  static uint64_t Key(uint32_t start, uint32_t length) {
    return (uint64_t(start) << 32) | length;
  }

  std::vector<Region> regions_;
  std::vector<Id> free_;
  std::unordered_map<uint64_t, Id> exact_;
  std::multimap<uint32_t, Id> by_start_;
  // Longest region ever inserted. Never shrinks: a stale larger value only
  // makes the overlap scan start earlier, never miss a region.
  uint32_t max_length_ = 0;
};

}  // namespace mcla::gpu

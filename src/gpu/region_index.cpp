// MCLA GPU renderer -- index of guest memory regions. See region_index.h.

#include "region_index.h"

#include <cassert>

namespace mcla::gpu {

RegionIndex::Id RegionIndex::Find(uint32_t start, uint32_t length) const {
  const auto it = exact_.find(Key(start, length));
  return it == exact_.end() ? kNone : it->second;
}

RegionIndex::Id RegionIndex::Insert(uint32_t start, uint32_t length) {
  assert(Find(start, length) == kNone);
  Id id;
  if (!free_.empty()) {
    id = free_.back();
    free_.pop_back();
    regions_[id] = Region{start, length};
  } else {
    id = Id(regions_.size());
    regions_.push_back(Region{start, length});
  }
  exact_.emplace(Key(start, length), id);
  by_start_.emplace(start, id);
  if (length > max_length_) {
    max_length_ = length;
  }
  return id;
}

void RegionIndex::Remove(Id id) {
  if (id >= regions_.size()) {
    return;
  }
  const Region r = regions_[id];
  const auto exact = exact_.find(Key(r.start, r.length));
  if (exact == exact_.end() || exact->second != id) {
    return;
  }
  exact_.erase(exact);
  for (auto [it, end] = by_start_.equal_range(r.start); it != end; ++it) {
    if (it->second == id) {
      by_start_.erase(it);
      break;
    }
  }
  free_.push_back(id);
}

void RegionIndex::Clear() {
  regions_.clear();
  free_.clear();
  exact_.clear();
  by_start_.clear();
  max_length_ = 0;
}

}  // namespace mcla::gpu

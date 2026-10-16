// MCLA GPU renderer -- index of guest memory regions. See region_index.h.

#include "region_index.h"

#include <bit>
#include <cassert>

namespace mcla::gpu {

size_t RegionIndex::FindSlot(uint64_t key) const {
  if (slots_.empty()) {
    return SIZE_MAX;
  }
  const size_t mask = slots_.size() - 1;
  // At most half full, tombstones included: the probe always meets an empty slot.
  for (size_t i = Home(key);; i = (i + 1) & mask) {
    const Slot& s = slots_[i];
    if (s.key == key) {
      return i;
    }
    if (s.key == kEmptyKey) {
      return SIZE_MAX;
    }
  }
}

void RegionIndex::Rehash(size_t capacity) {
  std::vector<Slot> old = std::move(slots_);
  slots_.assign(capacity, Slot{kEmptyKey, kNone});
  shift_ = uint32_t(64 - std::countr_zero(capacity));
  tombstones_ = 0;
  const size_t mask = capacity - 1;
  for (const Slot& s : old) {
    if (s.key == kEmptyKey || s.key == kTombstoneKey) {
      continue;
    }
    size_t i = Home(s.key);
    while (slots_[i].key != kEmptyKey) {
      i = (i + 1) & mask;
    }
    slots_[i] = s;
  }
}

RegionIndex::Id RegionIndex::Find(uint32_t start, uint32_t length) const {
  const size_t slot = FindSlot(Key(start, length));
  return slot == SIZE_MAX ? kNone : slots_[slot].id;
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
  // Grow (or sweep the tombstones) before the table passes half full.
  if ((exact_count_ + tombstones_ + 1) * 2 > slots_.size()) {
    size_t capacity = slots_.empty() ? 64 : slots_.size();
    while ((exact_count_ + 1) * 4 > capacity) {  // a quarter full after the rehash, at most
      capacity *= 2;
    }
    Rehash(capacity);
  }
  const uint64_t key = Key(start, length);
  assert(key != kEmptyKey && key != kTombstoneKey);
  const size_t mask = slots_.size() - 1;
  size_t i = Home(key);
  while (slots_[i].key != kEmptyKey && slots_[i].key != kTombstoneKey) {
    i = (i + 1) & mask;
  }
  if (slots_[i].key == kTombstoneKey) {
    --tombstones_;
  }
  slots_[i] = Slot{key, id};
  ++exact_count_;
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
  const size_t slot = FindSlot(Key(r.start, r.length));
  if (slot == SIZE_MAX || slots_[slot].id != id) {
    return;
  }
  slots_[slot] = Slot{kTombstoneKey, kNone};
  --exact_count_;
  ++tombstones_;
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
  slots_.clear();
  shift_ = 64;
  exact_count_ = 0;
  tombstones_ = 0;
  by_start_.clear();
  max_length_ = 0;
}

}  // namespace mcla::gpu

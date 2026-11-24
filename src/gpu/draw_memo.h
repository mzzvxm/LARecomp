#pragma once

#include <array>
#include <cstdint>

namespace mcla::gpu {

struct DrawMemoStats {
  uint64_t tries = 0;
  uint64_t hits = 0;
};

// Render-thread memo with most-recently-used replacement. A caller can use
// just the first way, or look for the key in both ways before doing a lookup.
template <typename Key, typename Value>
class TwoWayDrawMemo {
 public:
  template <typename LookupKey>
  const Value* Find(const LookupKey& key) const {
    const Entry& entry = entries_[recent_];
    return entry.valid && entry.key == key ? &entry.value : nullptr;
  }

  template <typename LookupKey>
  const Value* FindOther(const LookupKey& key) {
    const Entry& entry = entries_[recent_ ^ 1];
    if (!entry.valid || !(entry.key == key)) {
      return nullptr;
    }
    recent_ ^= 1;
    return &entry.value;
  }

  template <typename LookupKey>
  void Put(const LookupKey& key, const Value& value, bool two_way = true) {
    if (two_way && entries_[recent_].valid) {
      recent_ ^= 1;
    }
    Entry& entry = entries_[recent_];
    entry.valid = false;
    entry.key = key;
    entry.value = value;
    entry.valid = true;
  }

  void Clear() {
    for (Entry& entry : entries_) {
      entry.valid = false;
    }
  }

 private:
  struct Entry {
    Key key{};
    Value value{};
    bool valid = false;
  };
  std::array<Entry, 2> entries_{};
  unsigned recent_ = 0;
};

}  // namespace mcla::gpu

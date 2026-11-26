#pragma once
// ===========================================================================
// Render-thread CPU: the switches and the pure parts of the small fixes that
// RENDER-CPU-HOT (perf\RENDER-CPU-HOT.md) found in the host profile (std only;
// render_cpu_test.cpp)
// ===========================================================================
// The debug-only setting render_cpu_mask (read at start) holds one bit per fix:
// 0 runs every path as it was, 15 (kAll, the default) runs them all. The bits are the
// A/B switch of perf\scripts\chain-rendercpu.ps1; the render thread's own parts
// also follow ab::B() (ab.h, mcla_gpu_ab_interleave), so one run can alternate
// the two in its statistics windows.
//
// Threading: pure.
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mcla::gpu::render_cpu {

enum Bits : int32_t {
  kSampledDrawTimers = 1,  // the per-draw TSC timers of the statistics read the clock on one draw in kPeriod
  kAliasHint = 2,          // the physical range check asks the alias that answered last first (native_gfx/guest/physical_alias.h)
  kDeviceCache = 4,        // the per-draw lookups by device address keep their last answer (constants.cpp, scene.cpp)
  kForeignUseMemo = 8,     // side_views::IsForeignUse keeps its last answer (a pure function of its two arguments)
  kAll = 15,
};

constexpr bool Has(int32_t mask, Bits bit) {
  return (mask & bit) != 0;
}

constexpr bool DrawTimingWanted(bool requested, bool profile, bool ab, bool mirror_census) {
  return requested || profile || ab || mirror_census;
}

// "Time this draw?" for a statistic that sums the time of every draw. Reading the TSC costs
// about 7 ns on the test PC (4 reads a draw, ~3800 draws a frame: 0.1 ms a frame, about 3% of
// what the render thread does in a race); one draw in kPeriod is timed and its time counted
// kPeriod times, so the window's sum is an estimate of the exact one. kPeriod is odd: no pattern
// of the draws' order with a period of 2, 4 or 8 falls on the timed ones every time. A pattern of exactly
// kPeriod draws that repeats (an object of 7 sub-draws) would still fall on the same one every time, so
// the owner calls NextFrame() at each frame's start: the timed draws of a frame are those whose index in
// the frame has the frame's phase, and the phase moves on by one each frame (draws 0, 7, 14.. in one
// frame, 1, 8, 15.. in the next), so every sub-draw of such a pattern is timed once in kPeriod frames.
// Over kPeriod frames of the same draw count each draw is timed exactly once, so the estimate stays
// unbiased. Without NextFrame() the sampler keeps one count running across the frames (the unit test
// does that).
// Not sampled (Configure(false)): every draw is timed and Scale is the identity, as before.
class DrawSampler {
 public:
  static constexpr uint32_t kPeriod = 7;

  void Configure(bool sampled, bool enabled = true) {
    enabled_ = enabled;
    period_ = sampled ? kPeriod : 1u;
    countdown_ = 1;
    phase_ = 0;
  }
  // At a frame's start: the next frame times the draws whose index has the next phase (the first draw of the frame
  // counts down from 1 + phase). Not sampled: nothing to do.
  void NextFrame() {
    if (!enabled_ || period_ == 1u) {
      return;
    }
    phase_ = (phase_ + 1u) % period_;
    countdown_ = 1u + phase_;
  }
  bool sampled() const { return period_ != 1u; }
  bool enabled() const { return enabled_; }
  // True on the draws whose time is to be read.
  bool Take() {
    if (!enabled_ || --countdown_ != 0) {
      return false;
    }
    countdown_ = period_;
    return true;
  }
  // The time measured on a taken draw, as the estimate for the draws it stands for.
  uint64_t Scale(uint64_t measured) const { return measured * period_; }

 private:
  bool enabled_ = true;
  uint32_t period_ = 1;
  uint32_t countdown_ = 1;
  uint32_t phase_ = 0;
};

// A one-entry cache in front of `map[key]` for a map with stable element addresses (std::unordered_map: its nodes stay where they are
// through a rehash and an insert; only erasing the element or clearing the map ends them). Consecutive draws ask for the same device's
// constant bank and upload, so the hash lookup (a modulo and a pointer chase, ~10 ns, 3800 draws a frame) is skipped when the key is the
// last one's. The owner calls Forget() wherever it clears the map or erases an element. A copy of the cache starts empty, so it never
// points into another map. `use` false is the plain `map[key]`.
template <class Map>
class LastEntry {
 public:
  using Key = typename Map::key_type;
  using Value = typename Map::mapped_type;

  LastEntry() = default;
  LastEntry(const LastEntry&) noexcept {}
  LastEntry& operator=(const LastEntry&) noexcept {
    value_ = nullptr;
    return *this;
  }

  Value& Find(Map& map, const Key& key, bool use) {
    if (use && value_ != nullptr && key == key_) {
      return *value_;
    }
    Value& found = map[key];
    key_ = key;
    value_ = &found;
    return found;
  }
  void Forget() { value_ = nullptr; }

 private:
  Key key_{};
  Value* value_ = nullptr;
};

// The last answer of a pure function of one key (side_views::IsForeignUse of an EDRAM identity and a declared size: four rule
// functions compared against constants, asked about the same key by draw after draw). Only for functions of nothing but the key.
template <class Key, class Result>
class LastAnswer {
 public:
  bool Find(const Key& key, Result* result) const {
    if (valid_ && key == key_) {
      *result = result_;
      return true;
    }
    return false;
  }
  template <class Compute>
  const Result& GetOrCompute(const Key& key, Compute&& compute) {
    if (!valid_ || !(key == key_)) {
      Put(key, compute());
    }
    return result_;
  }

  void Put(const Key& key, const Result& result) {
    key_ = key;
    result_ = result;
    valid_ = true;
  }

 private:
  Key key_{};
  Result result_{};
  bool valid_ = false;
};

// A failed reservation leaves the cursor where the next ordinary allocation expects it.
inline bool ReserveUpload(uint64_t& used, uint64_t capacity, uint64_t size,
                          uint64_t alignment, uint64_t& offset) {
  const uint64_t padding = (0 - used) & (alignment - 1);
  const uint64_t remaining = capacity - used;
  if (padding > remaining || size > remaining - padding) {
    return false;
  }
  offset = used + padding;
  used = offset + size;
  return true;
}

// A memo stores either wire bytes or decoded host words, selected at frame setup.
template <size_t Bytes>
struct FetchMemoKey {
  std::array<uint8_t, Bytes> bytes{};
  bool Matches(const void* key) const {
    return key && std::memcmp(bytes.data(), key, Bytes) == 0;
  }
  void Set(const void* key) {
    if (key) {
      std::memcpy(bytes.data(), key, Bytes);
    }
  }
};

// One stable descriptor binding in a frame slot. The caller resets it when the backing
// resource or set is replaced, including if a replacement could reuse the same address.
struct DescriptorBinding {
  bool Update(bool enabled, const void* object, uint64_t bytes = 0) {
    const bool changed = !enabled || !valid || resource != object || range != bytes;
    resource = object;
    range = bytes;
    valid = true;
    return changed;
  }

  const void* resource = nullptr;
  uint64_t range = 0;
  bool valid = false;
};

}  // namespace mcla::gpu::render_cpu

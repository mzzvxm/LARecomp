#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace mcla::gpu::rt_as {

inline constexpr uint32_t kQueriesPerBatch = 16;
inline constexpr uint32_t kCopiesPerFrame = 8;
inline constexpr uint64_t kCompactionHeadroom = 16ull << 20;
inline constexpr uint64_t kASAlignment = 256;
inline constexpr uint64_t kAllocationAlignment = 64ull << 10;

inline constexpr uint64_t CompactBytes(uint64_t original, uint64_t reported) {
  if (reported == 0 || reported > std::numeric_limits<uint64_t>::max() - (kASAlignment - 1)) return 0;
  const uint64_t rounded = (reported + kASAlignment - 1) & ~(kASAlignment - 1);
  return rounded < original ? rounded : 0;
}

// Charge at least a D3D12 buffer placement per candidate, including small BLASes.
inline constexpr uint64_t ReservationBytes(uint64_t original) {
  if (original == 0 || original > kCompactionHeadroom) return 0;
  return (original + kAllocationAlignment - 1) & ~(kAllocationAlignment - 1);
}

inline constexpr bool CanReserve(uint64_t held, uint64_t original) {
  const uint64_t charge = ReservationBytes(original);
  return charge != 0 && held <= kCompactionHeadroom && charge <= kCompactionHeadroom - held;
}

struct CompactionMemory {
  uint64_t reserved = 0;  // charged until the original allocation can be released
  uint64_t extra = 0;     // live compact destinations and retired originals

  void BeginCopy(uint64_t charge, uint64_t compact) {
    reserved += charge;
    extra += compact;
  }
  void Publish(uint64_t original, uint64_t compact) {
    extra += original - compact;
  }
  void CancelCopy(uint64_t charge, uint64_t compact) {
    reserved -= charge;
    extra -= compact;
  }
  void RetireOriginal(uint64_t charge, uint64_t original) {
    reserved -= charge;
    extra -= original;
  }
};

inline constexpr bool MatchesGeneration(uint64_t expected, uint64_t actual) {
  return expected != 0 && expected == actual;
}

enum class CopyResult { kWait, kDrop, kRetry, kPublish };

inline constexpr CopyResult FinishCopy(bool settled, bool completed, bool same_generation, bool enabled) {
  if (!settled) return CopyResult::kWait;
  if (!same_generation) return CopyResult::kDrop;
  return completed && enabled ? CopyResult::kPublish : CopyResult::kRetry;
}

// Store the descriptor fields explicitly. The generation identifies the BLAS even
// when an allocator reuses both the native address and the handle.
struct InstanceKey {
  uint64_t generation = 0;
  uint32_t id = 0;
  uint32_t mask = 0;
  uint32_t hit_group = 0;
  bool cull_disabled = false;
  std::array<uint32_t, 12> transform{};
  bool operator==(const InstanceKey&) const = default;
};

template <typename Instance>
InstanceKey KeyOf(const Instance& instance, uint64_t generation) {
  InstanceKey key{generation, instance.instanceID, instance.instanceMask, instance.instanceContributionToHitGroupIndex, instance.cullDisable};
  static_assert(sizeof(key.transform) == sizeof(instance.transform.m));
  std::memcpy(key.transform.data(), instance.transform.m, sizeof(key.transform));
  return key;
}

inline bool CanReuse(bool enabled, bool completed, bool fast_build, bool previous_fast_build,
                     std::span<const InstanceKey> previous, std::span<const InstanceKey> current) {
  if (!enabled || !completed || fast_build != previous_fast_build || current.empty() || previous.size() != current.size()) return false;
  for (std::size_t i = 0; i < current.size(); ++i) {
    if (current[i].generation == 0 || current[i] != previous[i]) return false;
  }
  return true;
}

}  // namespace mcla::gpu::rt_as

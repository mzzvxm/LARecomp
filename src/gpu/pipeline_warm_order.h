#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mcla::gpu::pipeline_warm_order {

template <class MaySkip>
constexpr bool Learn(bool enabled, uint64_t nanoseconds, MaySkip may_skip) {
  return enabled && nanoseconds >= 30'000'000 && !may_skip();
}

// Keep the title's first entries and the relative order of unmeasured ones.
// The file layout is unchanged: older builds load the same keys in the new order.
template <class Record, class Cost>
std::vector<const Record*> Order(const std::vector<Record>& records, Cost cost) {
  constexpr size_t kBootPrefix = 128;
  constexpr size_t kMaxPromoted = 64;
  std::vector<const Record*> order;
  order.reserve(records.size());
  const size_t prefix = std::min(records.size(), kBootPrefix);
  for (const Record& r : records) order.push_back(&r);
  if (prefix == order.size()) return order;
  const auto first = order.begin() + prefix;
  const auto warm_end = std::stable_partition(first, order.end(), [&](const Record* r) {
    return cost(*r) != 0;
  });
  std::stable_sort(first, warm_end, [&](const Record* a, const Record* b) { return cost(*a) > cost(*b); });
  // Leave any excess measured keys in their original positions among the rest.
  const size_t promoted = std::min(kMaxPromoted, size_t(warm_end - first));
  std::sort(first + promoted, order.end());
  return order;
}

}  // namespace mcla::gpu::pipeline_warm_order

#pragma once

#include <cstdint>
#include <optional>

#include "device_fault_format.h"

namespace mcla::gpu::device_fault {

inline constexpr std::string_view kDredPassPrefix = "LARecomp pass: ";

inline std::string_view DredPassLabel(std::string_view context) {
  return context.starts_with(kDredPassPrefix) ? context.substr(kDredPassPrefix.size())
                                              : std::string_view{};
}

struct DredOperationIndices {
  std::optional<uint32_t> last_completed;
  std::optional<uint32_t> first_unfinished;
};

// The count is absolute. The runtime only retains the final 64 Ki operations.
constexpr DredOperationIndices DredHistoryIndices(uint32_t count, uint32_t completed) {
  constexpr uint32_t kHistorySize = 65536;
  DredOperationIndices indices;
  if (completed > count) {
    return indices;
  }
  const uint32_t first_retained = count > kHistorySize ? count - kHistorySize : 0;
  if (completed > first_retained) {
    indices.last_completed = (completed - 1) % kHistorySize;
  }
  if (completed >= first_retained && completed < count) {
    indices.first_unfinished = completed % kHistorySize;
  }
  return indices;
}

struct DredPassMarker {
  uint32_t operation;
  std::string label;
};

struct DredBreadcrumbs {
  std::string queue;
  std::string list;
  uint32_t operation_count = 0;
  std::optional<uint32_t> completed;
  std::string last_completed_operation;
  std::string first_unfinished_operation;
  // Sorted by operation index, as provided by DRED.
  std::vector<DredPassMarker> markers;
};

inline std::string DescribeBreadcrumbs(const DredBreadcrumbs& list) {
  const std::string queue = detail::Clean(list.queue);
  const std::string name = detail::Clean(list.list);
  std::string text = std::format("DRED breadcrumbs: queue '{}', list '{}'",
      queue.empty() ? "<unnamed>" : queue, name.empty() ? "<unnamed>" : name);
  if (!list.completed || *list.completed > list.operation_count) {
    return text + ": progress unavailable";
  }

  const uint32_t completed = *list.completed;
  const bool finished = completed == list.operation_count;
  const DredPassMarker* last_pass = nullptr;
  const DredPassMarker* active_pass = nullptr;
  for (const DredPassMarker& marker : list.markers) {
    if (marker.operation > completed || marker.operation >= list.operation_count) {
      break;
    }
    last_pass = active_pass;
    active_pass = &marker;
  }
  if (finished) {
    last_pass = active_pass;
  }
  const auto label = [](const DredPassMarker* marker) {
    const std::string name = marker ? detail::Clean(marker->label) : std::string();
    return name.empty() ? std::string("<unknown>") : name;
  };
  text += std::format(": {}/{} operations completed; last completed op {}; "
                      "last completed pass '{}'",
      completed, list.operation_count,
      list.last_completed_operation.empty() ? "<unknown>" : list.last_completed_operation,
      label(last_pass));
  if (!finished) {
    text += std::format("; first unfinished op {}; possible faulting pass '{}'",
        list.first_unfinished_operation.empty() ? "<unknown>" : list.first_unfinished_operation,
        label(active_pass));
  }
  return text;
}

struct DredAllocation {
  std::string name;
  std::string type;
  bool freed;
};

inline std::vector<std::string> DescribePageFault(uint64_t address,
                                                 const std::vector<DredAllocation>& allocations) {
  if (address == 0) {
    return {"DRED page fault: no GPU virtual address reported"};
  }
  std::vector<std::string> lines{std::format("DRED page fault VA: {:#018x}", address)};
  for (const DredAllocation& allocation : allocations) {
    const std::string name = detail::Clean(allocation.name);
    lines.push_back(std::format("  {} allocation: {} '{}'",
        allocation.freed ? "recently freed" : "existing", allocation.type,
        name.empty() ? "<unnamed>" : name));
  }
  if (allocations.empty()) {
    lines.push_back("  DRED found no matching existing or recently freed allocation");
  }
  return lines;
}

}  // namespace mcla::gpu::device_fault

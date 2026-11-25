#pragma once

namespace larecomp::os {

// Take the lowest-numbered CPUs in the existing mask, including sparse masks from taskset or jobs.
template <typename IsAllowed, typename Select>
unsigned SelectFirstLogicalProcessors(unsigned requested, unsigned slots, IsAllowed allowed,
                                      Select select) {
  unsigned selected = 0;
  for (unsigned cpu = 0; cpu < slots && selected < requested; ++cpu) {
    if (allowed(cpu)) {
      select(cpu);
      ++selected;
    }
  }
  return selected;
}

}  // namespace larecomp::os

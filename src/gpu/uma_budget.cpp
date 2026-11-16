// MCLA GPU renderer -- the machine's RAM for the shared-memory video memory budget. See uma_budget.h.

#include "uma_budget.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mcla::gpu::uma_budget {

namespace {

uint64_t ReadSystemMemoryBytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#else
  // Physical pages times the page size; -1 where the C library cannot say.
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages <= 0 || page_size <= 0) {
    return 0;
  }
  return uint64_t(pages) * uint64_t(page_size);
#endif
}

}  // namespace

uint64_t SystemMemoryBytes() {
  // Read once: the installed RAM does not change while the game runs, and the budget is asked for
  // by several callers every half second.
  static const uint64_t bytes = ReadSystemMemoryBytes();
  return bytes;
}

}  // namespace mcla::gpu::uma_budget

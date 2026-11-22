#pragma once
// ===========================================================================
// The three virtual aliases of the guest's physical memory, and the order a
// readability check asks them in (std only; physical_alias_test.cpp)
// ===========================================================================
// Physical memory is reachable through three virtual aliases (0xA0000000,
// 0xC0000000, 0xE0000000), and each heap keeps its own page table, so "is this
// physical range readable" is a check of the range in each alias, true when any
// alias says yes (guest_resources.cpp IsPhysicalRangeReadable has the history).
// Every check takes the heap's lock and walks its page table, and the first
// alias asked answers no for the allocations made with 64 KiB pages (vertex and
// index data: a no costs as much as a yes). The checks are independent of each
// other and only read, so the answer is the same in whatever order they are
// asked: this asks first the alias that answered yes last time (the caller keeps
// that in a thread's own variable) and the others after it, in table order. A
// repeat of the same kind of range then costs one check, not two.
//
// Threading: pure; the hint is the caller's.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

struct PhysicalAlias {
  uint32_t base;         // the alias's first virtual address
  uint32_t page_offset;  // virtual = base + (physical - page_offset); the 0xE0 alias is one page off
};

// The table order is the order the checks were always asked in.
inline constexpr PhysicalAlias kPhysicalAliases[3] = {
    {0xE0000000u, 0x1000u},
    {0xC0000000u, 0u},
    {0xA0000000u, 0u},
};

// The physical space is 512 MiB.
inline constexpr uint64_t kPhysicalSpace = 0x20000000ull;

// `check(virtual_address, size)` is IsGuestRangeReadable. `hint` (in, out) is the table index
// asked first; null asks in table order, as before the hint existed.
template <class Check>
bool PhysicalRangeReadable(uint32_t physical_address, uint64_t size, uint32_t* hint, Check&& check) {
  const uint32_t phys = physical_address & 0x1FFFFFFFu;
  if (size == 0 || uint64_t(phys) + size > kPhysicalSpace) {
    return false;
  }
  const uint32_t first = (hint && *hint < 3u) ? *hint : 0u;
  for (uint32_t n = 0; n < 3; ++n) {
    // first, then the others in table order.
    const uint32_t index = n == 0 ? first : (n - 1 < first ? n - 1 : n);
    const PhysicalAlias& alias = kPhysicalAliases[index];
    if (phys < alias.page_offset) {
      continue;  // the 0xE0 alias has nothing below its one page
    }
    if (check(alias.base + (phys - alias.page_offset), size)) {
      if (hint) {
        *hint = index;
      }
      return true;
    }
  }
  return false;
}

}  // namespace mcla::native_gfx

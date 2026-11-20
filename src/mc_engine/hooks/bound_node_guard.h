#pragma once
// ===========================================================================
// The guard of the game's bound-node test sub_82574FF8 (hooks/vehicle.cpp, larecomp_config.toml): which nodes it cannot be given.
// Pure: no guest, no SDK; bound_node_guard_test.cpp checks it.
// ===========================================================================
// sub_82574FF8(r3 = the query, r4 = a node of the game's bound pool, r5 = a mask) tests the query's box against the node: it reads the node's matrix
// at +16 and its bound through two pointers, `lwz r9, 4(r4)` then `lwz r4, 12(r9)`, on both of its paths (the sphere test and the box test). Its
// callers (sub_8256CF78, and sub_8256CEC8 which walks the node's chain until one test answers yes) take the node from the pool by index. A node whose
// pointer at +4 is 0 is a node that has been dropped or is not filled yet (inferred: the thread that drops it was not seen): the first read then lands
// on guest 0xC and the title dies (run vol3-race-vulkan, 2026-11-20, 3 s into a race load, 1 race load in 318 logged, three game instances running
// at once). A node like that can contain nothing, so the test answers no (r3 = 0) and the callers go on to the next node of the chain.
// ===========================================================================

#include <cstdint>

namespace mcla::hooks {

inline constexpr uint32_t kBoundNodeBoundOffset = 4;       // the node's pointer to its bound (0 once the node is dropped)
inline constexpr uint32_t kGuestFirstMappedAddress = 0x10000u;  // the guest's first 64 KB are never mapped: a pointer below it is none

// Whether `node` (a guest address) is a node the test must not read: no node at all, one the two reads would leave the guest address space for,
// or one whose bound pointer is 0. `base` is the host base of the guest memory; it is read only when the node looks like a node.
inline bool BoundNodeIsDropped(const uint8_t* base, uint32_t node) {
  if (base == nullptr || node < kGuestFirstMappedAddress || uint64_t(node) + kBoundNodeBoundOffset + 4u > 0x100000000ull) {
    return true;
  }
  const uint8_t* p = base + node + kBoundNodeBoundOffset;  // big-endian u32
  return (p[0] | p[1] | p[2] | p[3]) == 0;
}

}  // namespace mcla::hooks

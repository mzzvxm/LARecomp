// Unit test of the bound-node guard (hooks/bound_node_guard.h).
//   clang++ -std=c++20 -I src/mc_engine/hooks src/mc_engine/hooks/bound_node_guard_test.cpp -o bound_node_guard_test && ./bound_node_guard_test
// Exit code 0 = passed.
#include "bound_node_guard.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what);
  }
}

// A fake guest: `size` bytes, address 0 at index 0, big-endian like the guest.
struct FakeGuest {
  std::vector<uint8_t> bytes;
  explicit FakeGuest(size_t size) : bytes(size, 0) {}
  void SetU32(uint32_t addr, uint32_t v) {
    bytes[addr + 0] = uint8_t(v >> 24);
    bytes[addr + 1] = uint8_t(v >> 16);
    bytes[addr + 2] = uint8_t(v >> 8);
    bytes[addr + 3] = uint8_t(v);
  }
};

}  // namespace

int main() {
  using mcla::hooks::BoundNodeIsDropped;
  FakeGuest g(0x30000);
  const uint8_t* base = g.bytes.data();

  // A node with a bound is a node.
  g.SetU32(0x20000 + 4, 0x82A00000);
  Check(!BoundNodeIsDropped(base, 0x20000), "a node with a bound pointer is not dropped");

  // The crash of vol3-race-vulkan: the node's bound pointer is 0 (the title read guest 0xC through it).
  g.SetU32(0x21000 + 4, 0);
  Check(BoundNodeIsDropped(base, 0x21000), "a node whose bound pointer is 0 is dropped");

  // Each byte of the pointer counts: a pointer with only its low byte, or only its high byte, set is a pointer.
  g.SetU32(0x22000 + 4, 0x00000001);
  Check(!BoundNodeIsDropped(base, 0x22000), "a pointer with only the low byte set is not dropped");
  g.SetU32(0x22100 + 4, 0x01000000);
  Check(!BoundNodeIsDropped(base, 0x22100), "a pointer with only the high byte set is not dropped");
  g.SetU32(0x22200 + 4, 0x00010000);
  Check(!BoundNodeIsDropped(base, 0x22200), "a pointer with only the third byte set is not dropped");
  g.SetU32(0x22300 + 4, 0x00000100);
  Check(!BoundNodeIsDropped(base, 0x22300), "a pointer with only the second byte set is not dropped");

  // Neighbouring words do not matter: only +4 is the bound pointer.
  g.SetU32(0x23000 + 0, 0xFFFFFFFF);
  g.SetU32(0x23000 + 8, 0xFFFFFFFF);
  g.SetU32(0x23000 + 4, 0);
  Check(BoundNodeIsDropped(base, 0x23000), "the words around +4 do not make a node of it");

  // No node, and the null page: nothing is read (the fake guest has no byte there to read, so a read would be out of range).
  Check(BoundNodeIsDropped(base, 0), "node 0 is dropped");
  Check(BoundNodeIsDropped(base, 0xC), "a node in the null page is dropped");
  Check(BoundNodeIsDropped(base, 0xFFFC), "the last word below the first mapped address is dropped");
  g.SetU32(0x10000 + 4, 0x1234);
  Check(!BoundNodeIsDropped(base, 0x10000), "a node at the first mapped address is read, not refused");

  // No guest memory (a title shutting down).
  Check(BoundNodeIsDropped(nullptr, 0x20000), "no guest memory: dropped");

  // A node at the top of the address space: the read of +4 would leave it. (Only the range is checked: the fake guest is not 4 GB.)
  Check(BoundNodeIsDropped(base, 0xFFFFFFFCu), "a node whose bound pointer would be past 4 GB is dropped");
  Check(BoundNodeIsDropped(base, 0xFFFFFFFFu), "node 0xFFFFFFFF is dropped");

  if (g_failures == 0) {
    std::printf("bound_node_guard_test: all passed\n");
    return 0;
  }
  std::printf("bound_node_guard_test: %d failed\n", g_failures);
  return 1;
}

#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — shader identity.
// C++ port of the offline-validated normalization (ucode_normalize.py from
// the Milestone 0 research). The two implementations must stay byte-exact:
// the offline shader database is generated with the Python side, the runtime
// looks up with this one.

#include "shader_identity.h"

#include <cstring>
#include <vector>

namespace mcla::native_gfx {

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  // Hex on purpose: the decimal form of the offset basis once shipped with a
  // dropped digit and silently produced a self-consistent but wrong hash.
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

namespace {

inline uint32_t LoadBe32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

inline void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Control-flow opcodes that execute an instruction block (kExec.. families).
// Everything else (jmp/call/alloc/nop) carries no fetch instructions.
inline bool IsExecOpcode(uint32_t op) {
  return (op >= 1 && op <= 7) || op == 13 || op == 14;
}

struct ExecRef {
  uint32_t address;   // in instruction units (3 dwords each)
  uint32_t count;     // instructions in the block
  uint32_t sequence;  // 2 bits per instruction, bit0 = fetch
};

}  // namespace

size_t NormalizeVertexFetches(uint8_t* ucode, size_t size) {
  const size_t n = size / 4;
  if (n < 3) {
    return 0;
  }

  // Pass 1: walk the CF section (two 48-bit instructions per 3 dwords),
  // collecting exec blocks and shrinking the CF limit to the first
  // instruction block, exactly like the offline generator.
  size_t limit = n;
  std::vector<ExecRef> execs;
  for (size_t i = 0; i + 2 < limit; i += 3) {
    const uint32_t d0 = LoadBe32(ucode + 4 * i);
    const uint32_t d1 = LoadBe32(ucode + 4 * (i + 1));
    const uint32_t d2 = LoadBe32(ucode + 4 * (i + 2));
    const uint32_t w0[2] = {d0, (d1 >> 16) | (d2 << 16)};
    const uint32_t w1[2] = {d1 & 0xFFFFu, d2 >> 16};
    for (int k = 0; k < 2; ++k) {
      const uint32_t op = (w1[k] >> 12) & 0xF;
      if (IsExecOpcode(op)) {
        const uint32_t addr = w0[k] & 0xFFFu;
        const uint32_t cnt = (w0[k] >> 12) & 0x7u;
        const uint32_t seq = (w0[k] >> 16) & 0xFFFu;
        if (addr != 0 && size_t(addr) * 3 < limit) {
          limit = size_t(addr) * 3;
        }
        if (addr != 0) {
          execs.push_back(ExecRef{addr, cnt, seq});
        }
      }
    }
  }

  // Pass 2: zero the payload of every vertex fetch. The sequence field only
  // covers 6 instructions (12 bits); larger blocks continue into the next CF
  // instruction, which the offline side also caps at 6 — keep parity.
  size_t normalized = 0;
  for (const ExecRef& e : execs) {
    const uint32_t cap = e.count < 6 ? e.count : 6;
    for (uint32_t k = 0; k < cap; ++k) {
      if (((e.sequence >> (2 * k)) & 1u) == 0) {
        continue;  // ALU instruction
      }
      const size_t base = (size_t(e.address) + k) * 3;
      if (base + 2 >= n) {
        continue;
      }
      const uint32_t f0 = LoadBe32(ucode + 4 * base);
      if ((f0 & 0x1Fu) != 0) {
        continue;  // texture fetch (kVertexFetch == 0), leave intact
      }
      StoreBe32(ucode + 4 * base, f0 & 0x1Fu);
      StoreBe32(ucode + 4 * (base + 1), 0);
      StoreBe32(ucode + 4 * (base + 2), 0);
      ++normalized;
    }
  }
  return normalized;
}

namespace {

// Memo for ShaderIdentity. The identity is a pure function of the microcode,
// the microcode of a bound shader does not change between draws, and a frame
// binds ~165 distinct shaders across ~4300 draws -- so without this every draw
// paid for a heap copy of the ucode, a std::vector of exec blocks, a control
// flow walk and a byte-at-a-time FNV over ~2 KB, twice (vertex and pixel).
//
// Direct-mapped so a lookup is one comparison and nothing allocates. The tag
// carries the address and size AND the first and last dword of the microcode:
// the guest can reuse an address when it reloads a shader, and a stale identity
// there would silently fetch the wrong bytecode out of the pack.
struct IdentityEntry {
  const uint8_t* ucode = nullptr;
  size_t size = 0;
  uint32_t first = 0;
  uint32_t last = 0;
  uint64_t identity = 0;
  bool valid = false;
};

constexpr size_t kIdentityCacheSize = 1024;  // power of two
IdentityEntry g_identity_cache[kIdentityCacheSize];

}  // namespace

uint64_t ShaderIdentity(const uint8_t* ucode, size_t size) {
  if (ucode == nullptr || size == 0 || size > kMaxUcodeBytes) {
    return 0;
  }
  const uint32_t first = size >= 4 ? LoadBe32(ucode) : 0;
  const uint32_t last = size >= 8 ? LoadBe32(ucode + size - 4) : 0;
  const uint64_t mix = (uint64_t(reinterpret_cast<uintptr_t>(ucode)) >> 4) ^ (uint64_t(size) << 20)
                       ^ (uint64_t(first) << 7) ^ uint64_t(last);
  IdentityEntry& slot = g_identity_cache[size_t(mix) & (kIdentityCacheSize - 1)];
  if (slot.valid && slot.ucode == ucode && slot.size == size && slot.first == first &&
      slot.last == last) {
    return slot.identity;
  }
  std::vector<uint8_t> copy(ucode, ucode + size);
  NormalizeVertexFetches(copy.data(), copy.size());
  const uint64_t identity = Fnv1a64(copy.data(), copy.size());
  slot = IdentityEntry{ucode, size, first, last, identity, true};
  return identity;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

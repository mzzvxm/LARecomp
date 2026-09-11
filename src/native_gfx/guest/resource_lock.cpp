#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest resource lock/unlock.
// See resource_lock.h for the decompilation this is built on.

#include "resource_lock.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/system/xmemory.h>

#include "d3d_structs.h"
#include "guest_resources.h"

namespace mcla::native_gfx {

namespace {

// D3DResource. Both flush words are inside the 24-byte common header, so they
// are in the same place for every resource type, not just textures.
constexpr uint32_t kResCommon = 0x00;
constexpr uint32_t kResBaseFlush = 0x14;
constexpr uint32_t kTexMipFlush = 0x18;
// D3DTexture fetch constant: dword 5 carries the mip page. The base page in
// dword 1 is not read here -- the unlock thunk already masks and passes both
// addresses, and each resource type stores its address differently (a vertex
// buffer keeps it at +0x18 masked with ~3).
constexpr uint32_t kTexFetchMip = 48;

// Common bits 8..11 hold the outstanding lock count, 0x100 per lock. The guest
// flushes only on the transition to zero.
constexpr uint32_t kCommonLockCountMask = 0xF00;
constexpr uint32_t kCommonOneLock = 0x100;

// The empty range: start (high half) above every possible end (low half).
constexpr uint32_t kFlushEmpty = 0xFFFF0000;
// Both halves count 128-byte units.
constexpr uint32_t kFlushUnitShift = 7;

// Base resource types, as D3DResource::Common & 0xF reports them. Counted raw
// rather than mapped to names: the point of this measurement is to find out
// WHICH types the game actually locks at runtime, and assuming the answer
// would defeat it.
constexpr uint32_t kTypeBuckets = 16;

struct Counters {
  std::atomic<uint64_t> locks{0};
  std::atomic<uint64_t> unlocks{0};
  std::atomic<uint64_t> nested{0};       // unlock that did not reach zero
  std::atomic<uint64_t> clean{0};        // last unlock, nothing dirty
  std::atomic<uint64_t> base_ranges{0};
  std::atomic<uint64_t> mip_ranges{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> creates{0};
  std::atomic<uint64_t> create_no_mip{0};
  std::atomic<uint64_t> locks_by_type[kTypeBuckets] = {};
  std::atomic<uint64_t> unlocks_by_type[kTypeBuckets] = {};
};

Counters& counters() {
  static Counters c;
  return c;
}

inline uint32_t LoadBe32At(const uint8_t* base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

// Turns one packed flush word into a range over `region_address`.
GuestFlushRange DecodeFlush(uint32_t packed, uint32_t region_address) {
  GuestFlushRange out;
  if (packed == kFlushEmpty || !region_address) {
    return out;
  }
  const uint32_t start = (packed >> 16) << kFlushUnitShift;
  const uint32_t end = (packed & 0xFFFFu) << kFlushUnitShift;
  if (end <= start) {
    // Any other start-above-end encoding means the same thing as the sentinel.
    return out;
  }
  out.address = region_address + start;
  out.size = end - start;
  out.valid = true;
  return out;
}

}  // namespace

ResourceUnlock ReadResourceUnlock(const uint8_t* base, uint32_t resource_va,
                                  uint32_t base_address, uint32_t mip_address) {
  ResourceUnlock out;
  // The common header is 24 bytes and both flush words live inside it, so this
  // is valid for every resource type, not just textures.
  if (!base || resource_va < 0x1000u || !IsGuestRangeReadable(resource_va, sizeof(D3DResource))) {
    return out;
  }
  const uint32_t common = LoadBe32At(base, resource_va + kResCommon);
  out.type = common & 0xFu;
  // The count still includes THIS lock: the original decrements after reading.
  if ((common & kCommonLockCountMask) != kCommonOneLock) {
    return out;  // nested unlock, the guest flushes nothing
  }
  out.last = true;
  out.base_range = DecodeFlush(LoadBe32At(base, resource_va + kResBaseFlush), base_address);
  // MipFlush is at +0x18, which is the first dword PAST the common header, so
  // it is only meaningful for a resource that has one. The thunks say so
  // themselves: the vertex buffer one passes 0 as the mip address.
  if (mip_address && IsGuestRangeReadable(resource_va + kTexMipFlush, 4)) {
    out.mip_range = DecodeFlush(LoadBe32At(base, resource_va + kTexMipFlush), mip_address);
  }
  return out;
}

void NoteResourceLock(const uint8_t* base, uint32_t resource_va) {
  Counters& c = counters();
  c.locks.fetch_add(1, std::memory_order_relaxed);
  if (!base || resource_va < 0x1000u || !IsGuestRangeReadable(resource_va, sizeof(D3DResource))) {
    return;
  }
  const uint32_t type = LoadBe32At(base, resource_va + kResCommon) & 0xFu;
  c.locks_by_type[type & (kTypeBuckets - 1)].fetch_add(1, std::memory_order_relaxed);
}

void NoteResourceUnlock(const ResourceUnlock& u) {
  Counters& c = counters();
  c.unlocks.fetch_add(1, std::memory_order_relaxed);
  c.unlocks_by_type[u.type & (kTypeBuckets - 1)].fetch_add(1, std::memory_order_relaxed);
  if (!u.last) {
    c.nested.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!u.base_range.valid && !u.mip_range.valid) {
    c.clean.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (u.base_range.valid) {
    c.base_ranges.fetch_add(1, std::memory_order_relaxed);
    c.bytes.fetch_add(u.base_range.size, std::memory_order_relaxed);
  }
  if (u.mip_range.valid) {
    c.mip_ranges.fetch_add(1, std::memory_order_relaxed);
    c.bytes.fetch_add(u.mip_range.size, std::memory_order_relaxed);
  }
}

bool IsTextureUnlock(const ResourceUnlock& unlock) { return unlock.type == kBaseTypeTexture; }

void NoteTextureCreate(const uint8_t* base, uint32_t texture_va) {
  Counters& c = counters();
  if (!texture_va) {
    return;  // the allocation failed; the guest turns this into E_OUTOFMEMORY
  }
  c.creates.fetch_add(1, std::memory_order_relaxed);
  if (!base || texture_va < 0x1000u || !IsGuestRangeReadable(texture_va, sizeof(D3DTexture))) {
    return;
  }
  // CreateTexture makes three allocations -- header, base pixels, mip pixels --
  // and writes the last two into the fetch constant as pages. A texture with no
  // mip chain simply skips the third.
  const uint32_t mip_address = LoadBe32At(base, texture_va + kTexFetchMip) & ~0xFFFu;
  if (!mip_address) {
    c.create_no_mip.fetch_add(1, std::memory_order_relaxed);
  }
}

std::string ResourceLockSummary() {
  const Counters& c = counters();
  // Only the buckets that fired: the whole point is to find out which resource
  // types the game locks at runtime, and an empty bucket says it locks none.
  std::string by_type;
  for (uint32_t t = 0; t < kTypeBuckets; ++t) {
    const uint64_t l = c.locks_by_type[t].load(std::memory_order_relaxed);
    const uint64_t u = c.unlocks_by_type[t].load(std::memory_order_relaxed);
    if (!l && !u) {
      continue;
    }
    char one[56];
    std::snprintf(one, sizeof(one), "%st%u=%llu/%llu", by_type.empty() ? "" : " ", t,
                  (unsigned long long)l, (unsigned long long)u);
    by_type += one;
  }
  if (by_type.empty()) {
    by_type = "none";
  }

  char buf[288];
  std::snprintf(buf, sizeof(buf),
                "locks=%llu unlocks=%llu (nested=%llu clean=%llu) | dirty base=%llu mip=%llu "
                "bytes=%llu | creates=%llu (no_mip=%llu)",
                (unsigned long long)c.locks.load(std::memory_order_relaxed),
                (unsigned long long)c.unlocks.load(std::memory_order_relaxed),
                (unsigned long long)c.nested.load(std::memory_order_relaxed),
                (unsigned long long)c.clean.load(std::memory_order_relaxed),
                (unsigned long long)c.base_ranges.load(std::memory_order_relaxed),
                (unsigned long long)c.mip_ranges.load(std::memory_order_relaxed),
                (unsigned long long)c.bytes.load(std::memory_order_relaxed),
                (unsigned long long)c.creates.load(std::memory_order_relaxed),
                (unsigned long long)c.create_no_mip.load(std::memory_order_relaxed));
  return std::string(buf) + " | by type (lock/unlock): " + by_type;
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET

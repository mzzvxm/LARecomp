#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — the guest's GPU fence pair.
// See guest_fence.h for the reverse-engineering provenance.

#include "guest_fence.h"

#include <cstring>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "guest_resources.h"

namespace mcla::native_gfx {

namespace {

inline uint32_t LoadBe32At(const uint8_t* base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

inline void StoreBe32At(uint8_t* base, uint32_t ea, uint32_t value) {
  const uint32_t v = __builtin_bswap32(value);
  std::memcpy(rex::memory::GuestPtr(base, ea), &v, 4);
}

}  // namespace

GuestFenceState ReadFenceState(const uint8_t* base, uint32_t dev) {
  GuestFenceState s;
  if (!base || dev < 0x1000u) {
    return s;
  }
  if (!IsGuestRangeReadable(dev + kDevFenceIssuedOffset, 4) ||
      !IsGuestRangeReadable(dev + kDevFenceRetiredPtrOffset, 4)) {
    return s;
  }
  s.issued = LoadBe32At(base, dev + kDevFenceIssuedOffset);

  // The retired half is behind one indirection: the field holds the guest
  // address of the dword the GPU writes back to, not the value.
  const uint32_t retired_ea = LoadBe32At(base, dev + kDevFenceRetiredPtrOffset);
  if (retired_ea < 0x1000u || !IsGuestRangeReadable(retired_ea, 4)) {
    return s;
  }
  s.retired = LoadBe32At(base, retired_ea);
  s.valid = true;
  return s;
}

bool IsRecordingCommandBuffer(const uint8_t* base, uint32_t dev) {
  if (!base || dev < 0x1000u ||
      !IsGuestRangeReadable(dev + kDevCommandBufferRecordingOffset, 4)) {
    return false;
  }
  return LoadBe32At(base, dev + kDevCommandBufferRecordingOffset) != 0;
}

void PublishRetiredFence(uint8_t* base, uint32_t dev, uint32_t retired) {
  if (!base || dev < 0x1000u) {
    return;
  }
  const GuestFenceState s = ReadFenceState(base, dev);
  if (!s.valid) {
    return;
  }
  // Monotonic under the same wraparound rule the guest compares with: only
  // publish a value that is newer than what is already there. Going backwards
  // would leave a waiter spinning on a fence that now reads as unretired.
  if (uint32_t(s.issued - retired) >= uint32_t(s.issued - s.retired)) {
    return;
  }
  const uint32_t retired_ea = LoadBe32At(base, dev + kDevFenceRetiredPtrOffset);
  if (retired_ea < 0x1000u || !IsGuestRangeReadable(retired_ea, 4)) {
    return;
  }
  StoreBe32At(base, retired_ea, retired);
}

void ProbeFenceState(const uint8_t* base, uint32_t dev) {
  // One line roughly every few seconds of gameplay, then silence: this exists
  // to prove the offsets, not to trace the frame.
  constexpr uint32_t kFenceProbeInterval = 4096;
  constexpr uint32_t kFenceProbeLimit = 12;
  static uint32_t calls = 0;
  static uint32_t emitted = 0;
  if (emitted >= kFenceProbeLimit || (calls++ % kFenceProbeInterval) != 0) {
    return;
  }
  ++emitted;

  const GuestFenceState s = ReadFenceState(base, dev);
  if (!s.valid) {
    REXLOG_ERROR(
        "[native_gfx] fence probe: UNREADABLE at dev={:#010x} (+{} issued, +{} retired-ptr). "
        "The offsets in guest_fence.h are wrong.",
        dev, kDevFenceIssuedOffset, kDevFenceRetiredPtrOffset);
    return;
  }
  const uint32_t retired_ea = LoadBe32At(base, dev + kDevFenceRetiredPtrOffset);
  REXLOG_INFO(
      "[native_gfx] fence probe: issued={} retired={} lag={} retired_ea={:#010x} recording={}",
      s.issued, s.retired, uint32_t(s.issued - s.retired), retired_ea,
      IsRecordingCommandBuffer(base, dev) ? 1 : 0);
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET

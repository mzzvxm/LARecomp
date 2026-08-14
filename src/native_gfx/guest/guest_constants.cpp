#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — ALU constant banks.
// See guest_constants.h for the reverse-engineering provenance.

#include "guest_constants.h"

#include <cstring>

#include <rex/system/xmemory.h>

#include "guest_resources.h"

namespace mcla::native_gfx {

namespace {

inline uint32_t LoadBe32At(const uint8_t* base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

}  // namespace

ConstantDirtyMasks ReadConstantDirtyMasks(const uint8_t* base, uint32_t dev) {
  ConstantDirtyMasks m;
  if (!base || dev < 0x1000u) {
    return m;
  }
  // Guest qwords are big-endian: high dword first.
  const uint32_t vs_hi = LoadBe32At(base, dev + kDevVsConstantDirtyOffset);
  const uint32_t vs_lo = LoadBe32At(base, dev + kDevVsConstantDirtyOffset + 4);
  const uint32_t ps_hi = LoadBe32At(base, dev + kDevPsConstantDirtyOffset);
  const uint32_t ps_lo = LoadBe32At(base, dev + kDevPsConstantDirtyOffset + 4);
  m.vs = (uint64_t(vs_hi) << 32) | vs_lo;
  m.ps = (uint64_t(ps_hi) << 32) | ps_lo;
  return m;
}

bool ReadConstantBank(const uint8_t* base, uint32_t bank_ea, void* dst) {
  if (!base || !dst || !IsGuestRangeReadable(bank_ea, kAluBankBytes)) {
    return false;
  }
  const auto* src = reinterpret_cast<const uint32_t*>(
      rex::memory::GuestPtr(const_cast<uint8_t*>(base), bank_ea));
  auto* out = static_cast<uint32_t*>(dst);
  for (uint32_t i = 0; i < kAluBankBytes / 4; ++i) {
    uint32_t v;
    std::memcpy(&v, src + i, 4);
    out[i] = __builtin_bswap32(v);
  }
  return true;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

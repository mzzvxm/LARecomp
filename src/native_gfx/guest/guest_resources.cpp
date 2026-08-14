#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest resource decoding.
// See guest_resources.h for the reverse-engineering provenance.

#include "guest_resources.h"

#include <cstring>

#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace mcla::native_gfx {

bool IsGuestRangeReadable(uint32_t guest_address, uint64_t size) {
  if (guest_address < 0x1000u || size == 0 ||
      uint64_t(guest_address) + size > 0x100000000ull) {
    return false;
  }
  auto* runtime = rex::Runtime::instance();
  if (!runtime) {
    return false;
  }
  auto* memory = runtime->memory();
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(guest_address);
  if (!heap) {
    return false;
  }
  // QueryRangeAccess walks the page table across the whole range and reports
  // the strictest protection found, so a range spanning into an uncommitted
  // page comes back kNoAccess.
  //
  // NOTE: do NOT use QueryBaseAndSize here — it returns the region base as a
  // heap-RELATIVE page number shifted into an address (it never adds
  // heap_base_ back), so comparing it against an absolute guest address
  // rejects every allocation on the physical heaps, which is where textures
  // and shader microcode live.
  const uint32_t last = uint32_t(uint64_t(guest_address) + size - 1);
  return heap->QueryRangeAccess(guest_address, last) != rex::memory::PageAccess::kNoAccess;
}

const uint8_t* TranslatePhysicalGuest(uint32_t physical_address) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime) {
    return nullptr;
  }
  auto* memory = runtime->memory();
  return memory ? memory->TranslatePhysical<const uint8_t*>(physical_address) : nullptr;
}

bool IsPhysicalRangeReadable(uint32_t physical_address, uint64_t size) {
  // The physical space is 512 MiB; the range must fit inside it.
  const uint32_t phys = physical_address & 0x1FFFFFFFu;
  if (size == 0 || uint64_t(phys) + size > 0x20000000ull) {
    return false;
  }
  // Physical memory is reachable through THREE virtual aliases (0xA0, 0xC0,
  // 0xE0), and each heap keeps its own page table: an allocation made with
  // 64 KiB pages is committed in the 0xC0 heap and is NOT marked committed
  // in the 0xE0 one. Checking a single alias produces false negatives —
  // observed with a vertex buffer at 0x0C3D4000 that the game was actively
  // rendering from while the 0xE0 alias reported it unmapped.
  static constexpr uint32_t kPhysicalAliases[] = {0xE0000000u, 0xC0000000u, 0xA0000000u};
  for (uint32_t aliasBase : kPhysicalAliases) {
    if (IsGuestRangeReadable(aliasBase | phys, size)) {
      return true;
    }
  }
  return false;
}

namespace {

// Big-endian dword read through the guest membase. The 0xE0 physical heap
// carries a host offset on Win32, so every read goes through GuestPtr.
inline uint32_t R32(const uint8_t* base, uint32_t ea) {
  if (ea < 0x1000u) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

}  // namespace

VertexFetch DecodeVertexFetch(uint32_t dword0, uint32_t dword1) {
  VertexFetch f;
  f.type = dword0 & 0x3u;
  f.guest_address = dword0 & 0xFFFFFFFCu;  // address:30 in dwords == bytes & ~3
  f.endian = dword1 & 0x3u;
  f.size_bytes = dword1 & 0x03FFFFFCu;  // size:24 in dwords == bytes & mask
  return f;
}

GuestIndexBuffer DecodeIndexBuffer(uint32_t dword0, uint32_t addr) {
  GuestIndexBuffer ib;
  ib.indices_32bit = (int32_t(dword0) < 0);
  ib.endian = (dword0 << 1) & 0xC0000000u;  // matches (2 * dword0) & 0xC0000000
  ib.guest_address = addr;
  return ib;
}

ShaderUcodeRef ReadPixelShaderUcode(const uint8_t* base, uint32_t obj_ea) {
  ShaderUcodeRef r;
  if (!obj_ea) {
    return r;
  }
  const uint32_t sub = obj_ea + R32(base, obj_ea + 64);
  r.guest_address = R32(base, sub + 40) + R32(base, obj_ea + 24);
  r.size_bytes = R32(base, sub + 44);
  return r;
}

uint32_t SelectVertexShaderVariant(const uint8_t* base, uint32_t vs_obj, uint32_t ps_obj) {
  if (!vs_obj || ps_obj != 0) {
    return 0;
  }
  return (R32(base, vs_obj + 872) & 0x20u) ? 1u : 0u;
}

ShaderUcodeRef ReadVertexShaderUcode(const uint8_t* base, uint32_t obj_ea, uint32_t variant) {
  ShaderUcodeRef r;
  if (!obj_ea) {
    return r;
  }
  const uint32_t off = R32(base, obj_ea + 896 + 8 * variant);
  if (off == 0) {
    return r;
  }
  r.guest_address = R32(base, obj_ea + off + 872) + R32(base, obj_ea + 32);
  r.size_bytes = R32(base, obj_ea + off + 876);
  return r;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

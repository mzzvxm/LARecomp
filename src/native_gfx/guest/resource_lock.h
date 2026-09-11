#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — guest resource lock/unlock
// ===========================================================================
// The guest tells us exactly which bytes of a texture it changed, and until
// now nobody was listening.
//
// D3DTexture_UnlockRect (sub_8240F220) is five instructions: it pulls the base
// and mip page out of the fetch constant (+32 and +48, masked with ~0xFFF) and
// tail-calls D3DResource_Unlock (sub_82421F38), which is where the real work
// is:
//
//   atomic { old = res->Common; res->Common = old - 0x100; }
//   if ((old & 0xF00) == 0x100) {            // this was the last lock
//     if (res->BaseFlush != 0xFFFF0000)      // +0x14
//       FlushRange(base + (HIWORD << 7), base + (LOWORD << 7));
//     if (res->MipFlush  != 0xFFFF0000)      // +0x18
//       FlushRange(mip  + (HIWORD << 7), mip  + (LOWORD << 7));
//   }
//
// So:
//   * Common bits 8..11 are a LOCK COUNT, 0x100 per outstanding lock. The
//     flush only happens on the transition to zero, and a nested unlock must
//     not be read as a write.
//   * BaseFlush and MipFlush are a DIRTY RANGE each, packed 16.16 in units of
//     128 bytes, with the start in the high half and the end in the low half.
//     0xFFFF0000 is the empty encoding (start above every end).
//   * Both are reset by the original, so they can only be read BEFORE it runs.
//
// What that buys: TextureCache invalidation today comes from a page write
// watch, which is coarse (whole pages), costs a page-protection fault per
// write, and can only fire after the fact. The unlock range is exact, free,
// and arrives at the moment the guest itself considers the data final.
// ===========================================================================

#include <cstdint>
#include <string>

namespace mcla::native_gfx {

// A dirty range the guest recorded, already resolved to a guest address.
struct GuestFlushRange {
  uint32_t address = 0;  // guest byte address of the first dirty byte
  uint32_t size = 0;     // bytes
  bool valid = false;
};

// One unlock, as seen at D3DResource_Unlock -- the single funnel every
// resource type reaches. Confirmed by xref: exactly three thunks call it, the
// texture one (sub_8240F220), the vertex buffer one (sub_82422370, which
// passes vb+0x18 & ~3 as the base and 0 as the mip) and the index buffer chunk
// at 0x82422478. Nothing else in the binary unlocks anything.
struct ResourceUnlock {
  uint32_t type = 0;  // D3DResource::Common & 0xF, the base resource type
  bool last = false;  // this unlock dropped the lock count to zero
  GuestFlushRange base_range;
  GuestFlushRange mip_range;
};

// Reads the state BEFORE the original runs -- it is what resets the flush
// words and decrements the lock count. `base_address` and `mip_address` are
// the thunk's own r4/r5, already masked for the resource type, so this does
// not have to know how each type stores its address.
ResourceUnlock ReadResourceUnlock(const uint8_t* base, uint32_t resource_va,
                                  uint32_t base_address, uint32_t mip_address);

// Counters, for the periodic report.
// D3DResource_Lock (sub_82421CA0), the matching funnel on the way in: the same
// three entry points reach it and nothing else locks anything.
void NoteResourceLock(const uint8_t* base, uint32_t resource_va);
void NoteResourceUnlock(const ResourceUnlock& unlock);

// Whether this unlock was a texture, by the base type in Common. Kept here so
// the guest object layout stays in the guest module.
bool IsTextureUnlock(const ResourceUnlock& unlock);
// `texture_va` is 0 when the allocation failed (the guest turns that into
// E_OUTOFMEMORY). Reads the two pixel pages off the fresh header itself.
void NoteTextureCreate(const uint8_t* base, uint32_t texture_va);

std::string ResourceLockSummary();

}  // namespace mcla::native_gfx

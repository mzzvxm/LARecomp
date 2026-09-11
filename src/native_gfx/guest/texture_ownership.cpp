#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — texture read ownership.
// See texture_ownership.h for the reverse-engineering provenance and the
// reason streamed textures are out of scope.

#include "texture_ownership.h"

#include <cstdio>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "d3d_structs.h"
#include "guest_resources.h"
#include "host_heap.h"

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_own_textures, 0, "MCLA/NativeGfx",
    "Resource ownership, step two: who allocates the 52-byte D3DTexture header. "
    "0 = the guest, as always. "
    "1 = mirror and validate: the runtime allocates the header out of its own guest-visible "
    "heap, copies the guest's bytes in, checks the guest VA translates back to the same bytes, "
    "then frees it. Nothing the game can observe changes -- this is the proof that objects the "
    "runtime allocates are addressable from guest code. "
    "2 = hand over: grcTextureXenon+16/+28 are repointed at the runtime's copy, so every guest "
    "read of that texture header reads runtime memory. The guest's original block is left "
    "untouched and restored at destruction, so its allocator only frees what it allocated. "
    "Covers only textures born in grcTextureXenon::Init; streamed ones are deserialized with "
    "the header embedded in the resource and stay with the guest.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    mcla_native_gfx_own_heap_kb, 0, "MCLA/NativeGfx",
    "Size of the guest-addressable block the runtime allocates its D3D objects out of, in KiB. "
    "0 uses the built-in default (4096). Rounded down to a power of two -- o1heap only uses the "
    "largest power-of-two span of its arena. "
    "Taken from the guest VIRTUAL window, never from a physical one: the physical windows are "
    "views of the console's 512 MiB of RAM, and taking 32 MiB of it killed the title during "
    "loading with 'PhysicalHeap::Alloc unable to alloc physical memory in parent heap'. "
    "Raise this if the exhaustion report fires.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::native_gfx {

namespace {

// grcTextureXenon: Init stores the D3DTexture pointer at BOTH of these.
constexpr uint32_t kGrcTexD3DTextureAlias = 16;
constexpr uint32_t kGrcTexD3DTexture = 28;
constexpr uint32_t kD3DTextureBytes = uint32_t(sizeof(D3DTexture));  // 52

struct Owned {
  uint32_t grc_va = 0;
  uint32_t ours_va = 0;      // header in the runtime's guest-visible heap
  uint32_t original_va = 0;  // the block the guest allocator produced
};

struct State {
  std::mutex mutex;
  std::unordered_map<uint32_t, Owned> by_grc;   // grcTextureXenon VA -> record
  std::unordered_map<uint32_t, uint32_t> ours;  // our D3DTexture VA -> grc VA
  bool heap_ready = false;
  bool heap_failed = false;
  // Counters, all cumulative.
  uint64_t validated = 0;
  uint64_t validate_failed = 0;
  uint64_t handed_over = 0;
  uint64_t handed_back = 0;
  uint64_t alloc_failed = 0;
  uint64_t skipped_unreadable = 0;
  uint64_t orphaned = 0;    // torn down while owned but with nothing safe to restore
  uint64_t readopted = 0;   // Init rebuilt a texture that was already owned
};

State& state() {
  static State s;
  return s;
}

// Read on every texture teardown in the game, including modes 0 and 1 where
// nothing is ever owned. Keeping the count outside the mutex means the common
// case costs one relaxed load instead of a lock plus two map lookups.
std::atomic<uint32_t> g_live_owned{0};

inline uint32_t LoadBe32At(const uint8_t* base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

inline void StoreBe32At(uint8_t* base, uint32_t ea, uint32_t value) {
  const uint32_t v = __builtin_bswap32(value);
  std::memcpy(rex::memory::GuestPtr(base, ea), &v, 4);
}

// Reads the D3DTexture pointer Init left behind, or 0 when this object is not
// one this path can own.
uint32_t ReadD3DTextureVa(const uint8_t* base, uint32_t grc_va) {
  if (!base || grc_va < 0x1000u || !IsGuestRangeReadable(grc_va + kGrcTexD3DTexture, 4)) {
    return 0;
  }
  const uint32_t d3d_va = LoadBe32At(base, grc_va + kGrcTexD3DTexture);
  if (d3d_va < 0x1000u || !IsGuestRangeReadable(d3d_va, kD3DTextureBytes)) {
    return 0;
  }
  return d3d_va;
}

bool EnsureHeap() {
  State& s = state();
  if (s.heap_ready) {
    return true;
  }
  if (s.heap_failed) {
    return false;
  }
  if (HostHeap::Get().Init()) {
    s.heap_ready = true;
    return true;
  }
  s.heap_failed = true;
  REXLOG_ERROR(
      "[native_gfx] texture ownership disabled: the guest-visible heap could not be reserved");
  return false;
}

}  // namespace

bool InitTextureOwnership() {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  return EnsureHeap();
}

void AdoptInitTexture(const uint8_t* base, uint32_t grc_texture_va) {
  const uint32_t mode = REXCVAR_GET(mcla_native_gfx_own_textures);
  if (mode == 0) {
    return;
  }
  const uint32_t original_va = ReadD3DTextureVa(base, grc_texture_va);
  if (!original_va) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.skipped_unreadable;
    return;
  }

  State& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!EnsureHeap()) {
    return;
  }
  const auto existing = s.by_grc.find(grc_texture_va);
  if (existing != s.by_grc.end()) {
    if (existing->second.ours_va == original_va) {
      // The pointer at +28 is still the one handed over: Init re-entered
      // without rebuilding. Leave the standing adoption alone.
      return;
    }
    // Init rebuilt the texture in place, so the guest has already replaced the
    // pointer with a fresh block of its own and nothing references the copy
    // from the first adoption. Drop it -- restoring it would write over an
    // object that is now live, and keeping it would leak the heap block and
    // leave a record that a later teardown could act on.
    const uint32_t stale_ours = existing->second.ours_va;
    s.ours.erase(stale_ours);
    s.by_grc.erase(existing);
    g_live_owned.store(uint32_t(s.by_grc.size()), std::memory_order_relaxed);
    HostHeap::Get().FreeGuest(stale_ours);
    ++s.readopted;
  }

  const uint32_t ours_va = HostHeap::Get().AllocGuest(kD3DTextureBytes, 16);
  if (!ours_va) {
    ++s.alloc_failed;
    return;
  }

  auto* mutable_base = const_cast<uint8_t*>(base);
  // Two independent ways to reach the same guest VA, and they have to agree.
  // The heap handed out `ours_va` through Memory::HostToGuestVirtual over a
  // block it got from Memory::TranslateVirtual; recompiled guest code will
  // reach it as base + ea + PhysicalHostOffset(ea), which is what GuestPtr
  // does. If the two disagree the game reads a different address than the one
  // the header was written to, and every texture comes out as garbage far from
  // here. Checking it at adoption turns that into a counter.
  auto* memory = rex::Runtime::instance() ? rex::Runtime::instance()->memory() : nullptr;
  auto* via_guest = rex::memory::GuestPtr<uint8_t*>(mutable_base, ours_va);
  auto* via_memory = memory ? memory->TranslateVirtual<uint8_t*>(ours_va) : nullptr;
  if (via_memory != via_guest) {
    ++s.validate_failed;
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR(
          "[native_gfx] texture ownership: guest VA {:#010x} translates to {} through the "
          "memory object but {} through base arithmetic; the heap is not guest-addressable",
          ours_va, (void*)via_memory, (void*)via_guest);
    }
    HostHeap::Get().FreeGuest(ours_va);
    return;
  }
  std::memcpy(via_guest, rex::memory::GuestPtr(mutable_base, original_va), kD3DTextureBytes);
  ++s.validated;

  if (mode < 2) {
    // Validation mode: prove the round-trip, change nothing the game sees.
    HostHeap::Get().FreeGuest(ours_va);
    return;
  }

  // Hand over. Both fields Init wrote, so no reader can find the old block.
  StoreBe32At(mutable_base, grc_texture_va + kGrcTexD3DTexture, ours_va);
  StoreBe32At(mutable_base, grc_texture_va + kGrcTexD3DTextureAlias, ours_va);
  s.by_grc[grc_texture_va] = Owned{grc_texture_va, ours_va, original_va};
  s.ours[ours_va] = grc_texture_va;
  g_live_owned.store(uint32_t(s.by_grc.size()), std::memory_order_relaxed);
  ++s.handed_over;
}

namespace {

// Puts the guest back in charge of a header the runtime took over: the live
// bytes go back into the block the guest allocator produced, every field that
// pointed at the runtime's copy is repointed at it, and the copy is freed.
// Callers hold the state lock.
//
// The copy-back is not cosmetic. ReferenceCount and the two page fields inside
// the fetch constant have been maintained in OUR block for the whole lifetime,
// and grcTextureFactoryXenon::DestroyTexture reads exactly those to decide
// between D3DResource_Release and the raw free, and to hand the pixel pages
// back (Common & 0x100000, then the tag at +16, then +32 and +48).
void RestoreLocked(State& s, uint8_t* base, const Owned& owned, uint32_t extra_slot_va) {
  s.by_grc.erase(owned.grc_va);
  s.ours.erase(owned.ours_va);
  g_live_owned.store(uint32_t(s.by_grc.size()), std::memory_order_relaxed);

  const bool restorable = base && owned.original_va >= 0x1000u &&
                          IsGuestRangeReadable(owned.original_va, kD3DTextureBytes) &&
                          IsGuestRangeReadable(owned.ours_va, kD3DTextureBytes);
  if (restorable) {
    std::memcpy(rex::memory::GuestPtr(base, owned.original_va),
                rex::memory::GuestPtr(base, owned.ours_va), kD3DTextureBytes);
    if (IsGuestRangeReadable(owned.grc_va + kGrcTexD3DTexture, 4)) {
      StoreBe32At(base, owned.grc_va + kGrcTexD3DTexture, owned.original_va);
      StoreBe32At(base, owned.grc_va + kGrcTexD3DTextureAlias, owned.original_va);
    }
    if (extra_slot_va >= 0x1000u && extra_slot_va != owned.grc_va + kGrcTexD3DTexture &&
        extra_slot_va != owned.grc_va + kGrcTexD3DTextureAlias &&
        IsGuestRangeReadable(extra_slot_va, 4)) {
      StoreBe32At(base, extra_slot_va, owned.original_va);
    }
    ++s.handed_back;
  } else {
    // Nothing safe to restore: the guest block went away under us, so the free
    // that follows would run on runtime memory. Say so loudly -- this is the
    // failure mode that corrupts the guest heap.
    ++s.orphaned;
    REXLOG_ERROR(
        "[native_gfx] texture ownership: grc {:#010x} torn down with its original header "
        "{:#010x} unreadable; the guest free would run on runtime memory",
        owned.grc_va, owned.original_va);
    // Leak the copy rather than free it: the guest still holds the address and
    // is about to pass it to its own allocator. A leak of 52 bytes beats
    // handing the heap a block it can hand out again while the guest frees it.
    return;
  }
  HostHeap::Get().FreeGuest(owned.ours_va);
}

}  // namespace

void ReleaseOwnedTexture(uint8_t* base, uint32_t grc_texture_va) {
  if (g_live_owned.load(std::memory_order_relaxed) == 0) {
    return;
  }
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  const auto it = s.by_grc.find(grc_texture_va);
  if (it == s.by_grc.end()) {
    return;
  }
  const Owned owned = it->second;  // RestoreLocked erases the map entry.
  RestoreLocked(s, base, owned, /*extra_slot_va=*/0);
}

void ReleaseOwnedTextureSlot(uint8_t* base, uint32_t slot_va) {
  if (g_live_owned.load(std::memory_order_relaxed) == 0) {
    return;
  }
  if (!base || slot_va < 0x1000u || !IsGuestRangeReadable(slot_va, 4)) {
    return;
  }
  const uint32_t tex_va = LoadBe32At(base, slot_va);
  if (!tex_va) {
    return;
  }
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  const auto it = s.ours.find(tex_va);
  if (it == s.ours.end()) {
    return;  // Not ours: the ordinary case, and the only case in modes 0 and 1.
  }
  const auto rec = s.by_grc.find(it->second);
  if (rec == s.by_grc.end()) {
    // The index says it is ours but the record is gone. Dropping the stale
    // index is all that is left; the guest is about to free runtime memory.
    s.ours.erase(it);
    ++s.orphaned;
    return;
  }
  const Owned owned = rec->second;  // RestoreLocked erases the map entry.
  RestoreLocked(s, base, owned, slot_va);
}

bool IsOwnedTexture(uint32_t d3d_texture_va) {
  if (!d3d_texture_va || g_live_owned.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.ours.find(d3d_texture_va) != s.ours.end();
}

std::string TextureOwnershipSummary() {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  const HostHeap::Snapshot heap = HostHeap::Get().GetSnapshot();
  char buf[352];
  std::snprintf(buf, sizeof(buf),
                "ownership mode=%u live=%zu | validated=%llu validate_failed=%llu "
                "handed_over=%llu handed_back=%llu alloc_failed=%llu unreadable=%llu "
                "orphaned=%llu readopted=%llu | heap ready=%d allocated=%zu peak=%zu "
                "oom=%llu",
                uint32_t(REXCVAR_GET(mcla_native_gfx_own_textures)), s.by_grc.size(),
                (unsigned long long)s.validated, (unsigned long long)s.validate_failed,
                (unsigned long long)s.handed_over, (unsigned long long)s.handed_back,
                (unsigned long long)s.alloc_failed, (unsigned long long)s.skipped_unreadable,
                (unsigned long long)s.orphaned, (unsigned long long)s.readopted,
                heap.ready ? 1 : 0, heap.allocated, heap.peak_allocated,
                (unsigned long long)heap.oom_count);
  return std::string(buf);
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET

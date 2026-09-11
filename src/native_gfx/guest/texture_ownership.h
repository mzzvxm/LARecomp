#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — texture read ownership
// ===========================================================================
// Step two of resource ownership: the runtime owns the STORAGE of the
// D3DTexture object, while the guest still owns everything the object
// describes -- the pixels, the fences, the command buffer that binds it.
// Hence "read" ownership: the game keeps driving, we just hold the header.
//
// Why the header and not the pixels first: the header is the only part whose
// address the engine hands around. It reads Common and ReferenceCount off
// offset 0 without going through any D3D entry point, so the object must live
// in GUEST memory at a guest VA -- which is exactly what HostHeap provides
// (a block in the physical vA0000000 window, so a host pointer round-trips).
// Everything the runtime later takes over (Lock/Unlock, pixel allocation,
// resource fences) needs that guest-addressable object to exist first.
//
// Which textures this can cover, and why it is not all of them:
//
//   grcTextureXenon::Init (sub_821841B0) allocates the 52-byte D3DTexture
//   itself (sub_82130528(52)) and stores it at grc+16 and grc+28. That object
//   is relocatable: nothing else holds its address at the moment Init
//   returns, and the fetch constant inside it carries the PIXEL base, never
//   its own address. So the runtime can move it into its own heap.
//
//   Streamed textures never reach Init. They are deserialized:
//   grcTextureXenon's datResource constructor (sub_82184458) receives an
//   object whose D3DTexture is embedded IN the resource block, and only fixes
//   up +24 (name), +28 (the D3DTexture pointer) and the two page fields
//   inside the fetch constant (D3DTexture+32 base, +48 mip). Relocating that
//   one would mean taking the resource's memory apart, so those stay observed
//   only -- see texture_registry.h.
//
// Modes (cvar mcla_native_gfx_own_textures):
//   0  off.
//   1  mirror and validate. Allocates the object out of the guest-visible
//      heap, copies the guest's bytes in, checks that the guest VA translates
//      back to the same bytes, then frees it. Nothing the game can observe
//      changes. This is what proves the heap is addressable from guest code
//      before anything depends on it.
//   2  hand over. Same allocation, but grc+16/+28 are repointed at it, so
//      from that moment every guest read of the texture header reads OUR
//      memory. The original block is left allocated and untouched, and the
//      destructor hook copies our bytes back into it and restores the
//      pointers before the guest's own free runs -- so the guest allocator
//      only ever frees blocks it allocated.
// ===========================================================================

#include <cstdint>
#include <string>

namespace mcla::native_gfx {

// Reserves the guest-visible heap. Idempotent, logs once on failure. Called
// from the native runtime bring-up; ownership is a no-op until it succeeds.
bool InitTextureOwnership();

// Called from the grcTextureXenon_Init hook AFTER the original: that is the
// first moment the D3DTexture exists and the last moment nobody else holds
// its address.
void AdoptInitTexture(const uint8_t* base, uint32_t grc_texture_va);

// Called from the grcTextureXenon_dtor hook BEFORE the original: hands the
// header back so grcTextureFactoryXenon::DestroyTexture frees the block its
// own allocator produced.
void ReleaseOwnedTexture(uint8_t* base, uint32_t grc_texture_va);

// Called from the grcTextureFactoryXenon_DestroyTexture hook (sub_82177CB0)
// BEFORE the original. That function takes the ADDRESS of the field holding
// the D3DTexture pointer -- it reads *slot, frees it through the guest
// allocator and then writes 0 back -- so it is the one funnel every teardown
// route passes through, whatever object owns the field.
//
// The destructor hook normally restores first and this finds nothing; this is
// the net under any route that destroys a texture without running the
// grcTextureXenon destructor.
void ReleaseOwnedTextureSlot(uint8_t* base, uint32_t slot_va);

// Whether this D3DTexture VA is storage the runtime owns.
bool IsOwnedTexture(uint32_t d3d_texture_va);

// Counters for the periodic report: adoption, validation and heap use.
std::string TextureOwnershipSummary();

}  // namespace mcla::native_gfx

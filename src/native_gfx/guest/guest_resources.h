#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — guest resource decoding
// ===========================================================================
// Pure decoders for the guest-side objects the runtime consumes. No D3D12
// here; d3d12/resource_cache.* resolves these into native resources.
//
// Reverse-engineering provenance (all confirmed by decompilation):
//
// Vertex buffers
//   The D3D vertex buffer object embeds a ready-made Xenos vertex fetch
//   constant pair at +24/+28 (D3DVertexBuffer_Lock, sub_82422320, masks the
//   address with ~3 and the size with 0x3FFFFFC — exactly the
//   xe_gpu_vertex_fetch_t field layout). Static geometry arrives PRE-BAKED
//   inside RPF resources: grcVertexBufferD3D's deserializing ctor
//   (sub_821B7F30) only fixes up +24 from file offset to guest address
//   ("Invalid fixup, address is neither virtual nor physical"). So the fetch
//   constant shadow (D3DDevice+1148) is the authoritative binding source at
//   draw time; no create-time hook is needed for static geometry.
//
//   grcVertexBufferD3D (the rage wrapper): +6 locked flag, +8 locked CPU
//   pointer, +20 lock fence (reset to -1 by Unlock sub_821B8188),
//   +28 pointer to the D3D vertex buffer object.
//
// Index buffers
//   Current IB object at D3DDevice+12436. From the draw builder
//   (sub_8241D620): dword0 sign bit set -> 32-bit indices (draw initiator
//   gains 0x800 and the index address advances 4*startIndex instead of 2*);
//   dword0 bits 30..29 (via (2*dword0) & 0xC0000000) -> VGT DMA endian
//   field; +24 = guest byte address of index data (v15[6]).
//
// Fetch constants
//   Vertex fetch shadow: D3DDevice+1148, 32 groups x 6 dwords (3 fetch
//   pairs per group; slot k = group k/3, pair k%3).
//   Layout per xe_gpu_vertex_fetch_t (rex/graphics/xenos.h):
//     dword0: type:2 | address:30   (address in dwords)
//     dword1: endian:2 | size:24   (size in dwords)  [| pad:6]
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

// --- vertex fetch constant -------------------------------------------------

struct VertexFetch {
  uint32_t guest_address = 0;  // byte address in guest memory
  uint32_t size_bytes = 0;
  uint32_t endian = 0;  // xenos::Endian: 1 = 8in16, 2 = 8in32 (typical)
  uint32_t type = 0;    // 3 = valid vertex fetch
  bool valid() const { return type == 3 && guest_address != 0 && size_bytes != 0; }
};

// Decodes one vertex fetch constant from its two big-endian dwords as read
// from guest memory (fetch shadow or a D3D VB object's +24/+28 pair).
VertexFetch DecodeVertexFetch(uint32_t dword0_be_decoded, uint32_t dword1_be_decoded);

// Guest D3DDevice fetch shadow geometry. The block at +1148 starts with a
// 4-byte header; the 32 fetch groups live at +1152 and end exactly at +1920
// (the VS ALU constants). Confirmed empirically via the raw shadow dump:
// decoding at +1152 yields well-formed texture fetch groups (type bits == 2)
// while +1148 yields none — and the flush routine (sub_82423788) reads the
// groups with the same +4 skew.
inline constexpr uint32_t kDevFetchShadowOffset = 1152;
inline constexpr uint32_t kFetchShadowGroups = 32;
inline constexpr uint32_t kFetchGroupDwords = 6;  // 1 texture fetch or 3 vertex-fetch pairs

// --- index buffer object ---------------------------------------------------

struct GuestIndexBuffer {
  uint32_t guest_address = 0;  // byte address of index 0
  bool indices_32bit = false;
  uint32_t endian = 0;  // VGT DMA endian bits (30..29 of the size dword)
  bool valid() const { return guest_address != 0; }
};

// Decodes the D3D index buffer object (pointer found at D3DDevice+12436).
// `dword0` and `addr` are the object's +0 and +24 dwords, already
// byte-swapped to host order.
GuestIndexBuffer DecodeIndexBuffer(uint32_t dword0, uint32_t addr);

inline constexpr uint32_t kDevIndexBufferOffset = 12436;
inline constexpr uint32_t kIndexBufferObjAddrOffset = 24;

// --- D3D vertex buffer object ---------------------------------------------

// Offsets inside the D3D vertex buffer object (baked fetch constant pair).
inline constexpr uint32_t kVertexBufferObjFetch0 = 24;
inline constexpr uint32_t kVertexBufferObjFetch1 = 28;

// --- rage wrapper (grcVertexBufferD3D) --------------------------------------

inline constexpr uint32_t kGrcVbLockedFlagOffset = 6;
inline constexpr uint32_t kGrcVbLockedPtrOffset = 8;
inline constexpr uint32_t kGrcVbLockFenceOffset = 20;
inline constexpr uint32_t kGrcVbD3DObjectOffset = 28;

// --- shader objects ---------------------------------------------------------
// Current shader objects at D3DDevice +12692 (PS) / +12696 (VS). Microcode
// location inside the objects, from sub_82424670 (Milestone 0):
//   PS: sub = ps + u32(ps+64); addr = u32(sub+40) + u32(ps+24); size = u32(sub+44)
//   VS: off = u32(vs + 896 + 8*variant);
//       addr = u32(vs+off+872) + u32(vs+32); size = u32(vs+off+876)
// The microcode these point at is the RUNTIME (vfetch-patched) copy; feed it
// through ShaderIdentity() to get the database key.

inline constexpr uint32_t kDevPixelShaderOffset = 12692;
inline constexpr uint32_t kDevVertexShaderOffset = 12696;

// --- guest memory validity --------------------------------------------------
// The membase is a sparse 4 GiB reservation: an address being numerically in
// range says nothing about the page being committed. Reading an uncommitted
// page faults the process, so anything derived from guest-supplied data
// (fetch constants, resource descriptors) must be validated against the
// memory system before it is dereferenced.

// True when [guest_address, guest_address + size) lies entirely inside one
// committed guest allocation. `guest_address` is a VIRTUAL address.
bool IsGuestRangeReadable(uint32_t guest_address, uint64_t size);

// GPU resource addresses (texture and vertex fetch constants) are PHYSICAL
// addresses, not virtual: the Xenos reads memory directly. They must be
// translated through the physical membase, which the SDK masks to the 512 MiB
// physical space (Memory::TranslatePhysical, `addr & 0x1FFFFFFF`) — the same
// path the SDK's own texture cache uses. Reading them through the virtual
// membase lands on unmapped pages.
const uint8_t* TranslatePhysicalGuest(uint32_t physical_address);

// Physical-address counterpart of IsGuestRangeReadable.
bool IsPhysicalRangeReadable(uint32_t physical_address, uint64_t size);

struct ShaderUcodeRef {
  uint32_t guest_address = 0;
  uint32_t size_bytes = 0;
  bool valid() const { return guest_address != 0 && size_bytes != 0; }
};

// `base` is the guest membase; `obj_ea` the shader object address.
ShaderUcodeRef ReadPixelShaderUcode(const uint8_t* base, uint32_t obj_ea);
ShaderUcodeRef ReadVertexShaderUcode(const uint8_t* base, uint32_t obj_ea, uint32_t variant = 0);

// A vertex shader object holds more than one microcode variant, and the D3D
// runtime patches vfetch instructions per variant — reading variant 0 when
// the draw uses variant 1 yields an entirely UNPATCHED shader (every vfetch
// with format/stride/offset zero), which is exactly how this was found.
//
// Selection, verbatim from sub_82424670:
//   variant = (no pixel shader bound && (u32(vs_obj + 872) & 0x20)) ? 1 : 0
// i.e. depth-only passes use variant 1 when the shader provides one.
uint32_t SelectVertexShaderVariant(const uint8_t* base, uint32_t vs_obj, uint32_t ps_obj);

}  // namespace mcla::native_gfx

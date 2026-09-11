#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — Xbox 360 D3D9 object layouts
// ===========================================================================
// The structs the guest's D3D9 library hands around: the game stores pointers
// to these and reads fields out of them directly, so anything the native
// runtime creates on the game's behalf must open with the same bytes.
//
// These come from the Xbox 360 XDK, not from MCLA, so they are the same in
// every 360 title. Two independent confirmations that the layout below is the
// one MCLA uses:
//
//   * D3DVertexBuffer.FetchLo/FetchHi land at +24/+28, which is exactly where
//     guest_resources.h already documents the ready-made Xenos vertex fetch
//     constant pair (found by decompiling D3DVertexBuffer_Lock, sub_82422320).
//   * D3DTexture.Format lands at +0x1C = +28, which is exactly the offset the
//     resolve path already reads the destination fetch constant from
//     (D3DDevice_Resolve's pDestTexture, see NotifyResolve).
//
// All fields are big-endian: this is guest memory, written by PowerPC code.
// ===========================================================================

#include <cstddef>
#include <cstdint>

#include <rex/types.h>

namespace mcla::native_gfx {

// Common header of every D3D resource. D3DResource_GetType reads Common;
// the engine increments and decrements ReferenceCount itself without calling
// AddRef/Release, so a native object must keep that field where it belongs.
struct D3DResource {
  rex::be_u32 Common;          // +0x00  type/flag bits
  rex::be_u32 ReferenceCount;  // +0x04  engine touches this directly
  rex::be_u32 Fence;           // +0x08
  rex::be_u32 ReadFence;       // +0x0C
  rex::be_u32 Identifier;      // +0x10
  rex::be_u32 BaseFlush;       // +0x14
};
static_assert(sizeof(D3DResource) == 24);

// The six-dword Xenos texture fetch constant embedded in a D3DTexture. Kept
// as raw big-endian dwords rather than xe::xe_gpu_texture_fetch_t so the
// layout stays byte-exact regardless of how the compiler lays out that
// union's bitfields; DecodeTextureFetch (texture_format.h) already turns
// these six dwords into fields.
struct GpuTextureFetchConstant {
  rex::be_u32 dword[6];
};
static_assert(sizeof(GpuTextureFetchConstant) == 24);

struct D3DTexture {
  D3DResource resource;           // +0x00
  rex::be_u32 MipFlush;           // +0x18
  GpuTextureFetchConstant Format; // +0x1C  <- the fetch constant at +28
};
static_assert(sizeof(D3DTexture) == 52);
static_assert(offsetof(D3DTexture, Format) == 0x1C);

// D3DSurface. Width/Height are packed into SizeBits:
//   Width  = (SizeBits >> 18) + 1             (width-1  in bits [31:18])
//   Height = ((SizeBits >> 3) & 0x7FFF) + 1   (height-1 in bits [17:3])
struct D3DSurface {
  D3DResource resource;    // +0x00
  rex::be_u32 SurfaceInfo; // +0x18  GPU_SURFACEINFO
  rex::be_u32 DepthInfo;   // +0x1C  GPU_DEPTHINFO
  rex::be_u32 HiControl;   // +0x20
  rex::be_u32 SizeBits;    // +0x24  packed Height/Width
  rex::be_u32 Format;      // +0x28  D3DFORMAT
  rex::be_u32 Size;        // +0x2C
};
static_assert(sizeof(D3DSurface) == 48);

inline uint32_t SurfaceWidth(uint32_t size_bits) {
  return (size_bits >> 18) + 1;
}
inline uint32_t SurfaceHeight(uint32_t size_bits) {
  return ((size_bits >> 3) & 0x7FFFu) + 1;
}

// The vertex fetch constant pair the guest bakes in at create time, which is
// why static geometry needs no fixup at draw time.
struct D3DVertexBuffer {
  D3DResource resource; // +0x00
  rex::be_u32 FetchLo;  // +0x18  <- +24
  rex::be_u32 FetchHi;  // +0x1C  <- +28
};
static_assert(sizeof(D3DVertexBuffer) == 32);
static_assert(offsetof(D3DVertexBuffer, FetchLo) == 24);

struct D3DIndexBuffer {
  D3DResource resource; // +0x00
  rex::be_u32 Address;  // +0x18
  rex::be_u32 Size;     // +0x1C  size and index format bits
};
static_assert(sizeof(D3DIndexBuffer) == 32);

// The pViewport argument to D3DDevice_SetViewport, and the device's cached
// copy of it.
struct D3DViewport9 {
  rex::be_u32 X;
  rex::be_u32 Y;
  rex::be_u32 Width;
  rex::be_u32 Height;
  rex::be_f32 MinZ;
  rex::be_f32 MaxZ;
};
static_assert(sizeof(D3DViewport9) == 24);

// Guest RECT, left/top/right/bottom.
struct D3DRect {
  rex::be_i32 left;
  rex::be_i32 top;
  rex::be_i32 right;
  rex::be_i32 bottom;
};
static_assert(sizeof(D3DRect) == 16);

// ---------------------------------------------------------------------------
// D3DResource::Common
// ---------------------------------------------------------------------------
// Bits 0-3 hold the base resource type; bit 30 is a flag the type query also
// consults. Anything the native runtime hands the game has to carry the right
// bits here, because the engine reads Common directly rather than calling a
// D3D entry point for it.
inline constexpr uint32_t kCommonTypeMask = 0xFu;
inline constexpr uint32_t kCommonFlag40000000 = 0x40000000u;

// The two flags grcTextureFactoryXenon::DestroyTexture (sub_82177CB0)
// branches on, and which therefore say who owns the memory behind a texture:
//   0x100000 -> refcounted by D3D. Teardown goes through D3DResource_Release
//               (sub_824221D8), which decrements ReferenceCount at +4.
//   0x200000 -> the header was placed. Set on BOTH roads in
//               grcTextureXenon::Init, so it is not on its own a discriminator;
//               only the absence of 0x100000 is.
inline constexpr uint32_t kCommonRefcounted = 0x100000u;
inline constexpr uint32_t kCommonPlaced = 0x200000u;

// Base types seen in Common & 0xF.
inline constexpr uint32_t kBaseTypeTexture = 3;
inline constexpr uint32_t kBaseTypeOther = 4;

// Extended types D3DResource_GetType (sub_82421A18) reports. It does not just
// return Common & 0xF: for a texture it disambiguates by the DIMENSION field
// of the embedded fetch constant, which is the same (dword[5] >> 9) & 3 that
// DecodeTextureFetch reads.
//
//   Common & 0xF == 3  ->  dim 0 -> 20   (1D)
//                          dim 1 -> 19   (2D, only when resource[8] & 0x400)
//                          dim 2 -> 17   (3D / volume)
//                          dim 3 -> 18   (cube)
//   Common & 0xF == 4  ->  16 when bit 30 is set and the owner qualifies
//
// MCLA binds 2D only (100% of 11.7k sampled fetches), so 19 is the case that
// matters; the rest are listed so a create path writes something coherent
// rather than a value the query cannot classify.
inline constexpr uint32_t kExtTypeArray2D = 16;
inline constexpr uint32_t kExtTypeVolume = 17;
inline constexpr uint32_t kExtTypeCube = 18;
inline constexpr uint32_t kExtType2D = 19;
inline constexpr uint32_t kExtType1D = 20;

// ---------------------------------------------------------------------------
// The guest D3D entry points this layout was derived from
// ---------------------------------------------------------------------------
// Each one reads the fields above, so together they are five independent
// confirmations that the struct offsets here are the ones the game uses.
//
//   sub_82421A18  D3DResource_GetType   Common & 0xF, then (dword[5] >> 9) & 3
//   sub_82421CA0  D3DResource_Lock      Fence at +8 / ReadFence at +12
//   sub_82422320  D3DVertexBuffer_Lock  +24 & ~3 base, +28 & 0x3FFFFFC size
//   sub_82422430  D3DIndexBuffer_Lock   +24 / +28 raw (Address / Size)
//   sub_82410440  D3DTexture_LockRect   +32 & 0x3F format, +32 & ~0xFFF base,
//                                       +48 & ~0xFFF mip  (fetch dwords 1 and 5)
//
// Supporting cast, for when the create path is written:
//   sub_8240F2A8  block dimensions for a Xenos format
//   sub_824328F8  resource descriptor query (format/width/height/pitch/size)
//   byte_82009270 bits-per-block table, indexed by format & 0x3F
//
// NOT a vtable: 0x821136B0 and 0x82113740 are .pdata entries (8 bytes, address
// plus flags), which merely enumerate the D3D library's functions in address
// order.
//
// ---------------------------------------------------------------------------
// How a texture actually comes into existence in MCLA
// ---------------------------------------------------------------------------
// There is no single "CreateTexture" hook that catches everything, because the
// game reaches a D3DTexture by two different roads. Owning textures means
// covering both.
//
//   grcTextureXenon ctor            sub_82184F58   (falls back to a 32x32
//                                                   checkerboard when the
//                                                   image is invalid)
//     -> grcTextureXenon::Init      sub_821841B0
//
//   Init, road A -- PLACED, the common one for streamed assets:
//       sub_82130528(52)            allocate the D3DTexture itself
//       sub_82430938(...)           fill the header (XGSetTextureHeader)
//       Common |= 0x200000
//       sub_82430C38(tex, base)     point the header at memory the resource
//                                   already owns
//     Nothing is allocated for the pixels: the bytes arrived baked in the
//     resource. The matching failure string names the road --
//     "Bad resource type %d in grcTextureFactoryXenon::PlaceTexture".
//
//   Init, road B -- ALLOCATED:
//       sub_82177EB0                grcTextureFactoryXenon::CreateTexture
//         -> sub_82410C50(w, h, depth, levels, usage, format, pool, type)
//            = D3DDevice_CreateTexture. `type` is the extended type above:
//            3 = 2D, 17 = volume, 18 = cube. Returns the D3DTexture, 0 on
//            failure (the caller turns that into E_OUTOFMEMORY).
//         Then reads (tex+32) >> 12 and (tex+48) >> 12 -- the base and mip
//         pages out of fetch dwords 1 and 5 -- and zero-fills them.
//
// Header builders, one per dimension (all in the XG block):
//   sub_82430938 / sub_824309A8   2D
//   sub_82430A20 / sub_82430A90   cube
//   sub_82430B08 / sub_82430B68   volume
//   sub_82430C38                  bind a header to a base address
//   sub_82130528                  the 52-byte allocation for D3DTexture
//
// Lock / unlock, from the upload path (sub_82182FA0):
//   sub_82410C00 / C20 / C48      LockRect, dispatched by texture type
//   sub_82410440                  the shared texture lock body
//   sub_8240F220                  Unlock
//   sub_82410BE8                  level count
//   sub_82432D28                  per-level descriptor
//
// grcTexture instance layout used above: +14 levels-1, +16 and +28 both hold
// the D3DTexture pointer, +20 total size in bytes, +32/+34 width/height,
// +36 level count.

}  // namespace mcla::native_gfx

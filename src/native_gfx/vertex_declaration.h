#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — vertex declaration decoding
// ===========================================================================
// rage::grcDevice::CreateVertexDeclaration (sub_8217AAC0) converts grc vertex
// elements into an array of Xenon D3DVERTEXELEMENT9 records:
//
//   struct D3DVERTEXELEMENT9 {          // 12 bytes, big-endian in guest
//     uint16 Stream;
//     uint16 Offset;
//     uint32 Type;      // packed Xenon decltype (VertexFormat in low 6 bits)
//     uint8  Method;    // always 0 in MCLA
//     uint8  Usage;     // D3DDECLUSAGE
//     uint8  UsageIndex;
//   };                                  // terminated by Stream == 0xFF
//
// and stores them in the D3D declaration object at +52 (count at +24,
// max stream at +28) — sub_82417EA0. The D3D runtime later bakes these into
// the vertex shader's vfetch instructions; the native runtime instead feeds
// them to the D3D12 input assembler.
//
// The packed Type values MCLA can produce (table dword_827D42B0, indexed by
// grc format) and their D3D12 mapping:
//
//   0x002C235F  k_16_16_FLOAT        -> DXGI_FORMAT_R16G16_FLOAT
//   0x001A2360  k_16_16_16_16_FLOAT  -> DXGI_FORMAT_R16G16B16A16_FLOAT
//   0x002C23A5  k_32_FLOAT           -> DXGI_FORMAT_R32_FLOAT
//   0x001A23A6  k_32_32_FLOAT        -> DXGI_FORMAT_R32G32_FLOAT
//   0x002A23B9  k_32_32_32_FLOAT     -> DXGI_FORMAT_R32G32B32_FLOAT
//   0x001A2286  k_8_8_8_8 (raw)      -> DXGI_FORMAT_R8G8B8A8_UINT
//   0x00182886  k_8_8_8_8 (norm/BGRA)-> DXGI_FORMAT_B8G8R8A8_UNORM
//   0x001A2187  k_2_10_10_10         -> DXGI_FORMAT_R10G10B10A2_UINT
//
// The UINT choices match the XenosRecomp HLSL entry points, which declare
// packed attributes as uint4 and decode them in the shader
// (iBlendIndices0 : BLENDINDICES0 uint4, iNormal0 : NORMAL0 uint4, ...).
//
// Usage byte -> semantic name follows the XenosRecomp convention:
//   0 POSITION, 1 BLENDWEIGHT, 2 BLENDINDICES, 3 NORMAL, 5 TEXCOORD,
//   6 TANGENT, 7 BINORMAL, 10 COLOR.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mcla::native_gfx {

// Deliberately mirrors D3D12_INPUT_ELEMENT_DESC without pulling d3d12.h into
// every consumer. `semantic_name` points at a string literal with static
// lifetime. `dxgi_format` holds a DXGI_FORMAT value.
struct InputElement {
  const char* semantic_name = nullptr;
  uint32_t semantic_index = 0;
  uint32_t dxgi_format = 0;  // DXGI_FORMAT
  uint32_t input_slot = 0;   // = guest Stream
  uint32_t aligned_byte_offset = 0;
};

struct VertexDeclarationDesc {
  std::vector<InputElement> elements;
  uint32_t stream_count = 0;  // max stream index + 1
  // Set when the guest data contained a Type / Usage this module does not
  // know. The declaration must not be used for rendering in that case —
  // fail loud instead of guessing.
  bool unknown_field = false;
  uint32_t first_unknown_type = 0;
};

// Decodes `count` 12-byte big-endian D3DVERTEXELEMENT9 records (the guest
// declaration object's array at +52; count from +24). Stops early at the
// 0xFF stream terminator if one appears before `count` records.
VertexDeclarationDesc DecodeVertexDeclaration(const uint8_t* records_be, size_t count);

// Guest object layout constants (D3D vertex declaration, sub_82417EA0).
inline constexpr uint32_t kDeclOffsetElementCount = 24;
inline constexpr uint32_t kDeclOffsetMaxStream = 28;
inline constexpr uint32_t kDeclOffsetElements = 52;

// D3DDevice fields the vfetch patcher (sub_82423A38) reads.
//   +11820  current vertex declaration object
//   +12528  per-stream vertex stride table, one BYTE per stream, in dwords
//           (the patcher writes `a4[stream]` straight into the vfetch stride
//           field, and a4 == device + 12528)
inline constexpr uint32_t kDevVertexDeclarationOffset = 11820;
inline constexpr uint32_t kDevStreamStrideTableOffset = 12528;
// Sixteen-byte mirror of the stride table, copied by the vfetch patcher at the
// moment it patches (sub_82424320: `*(_QWORD*)(dev+11832) = *(_QWORD*)(dev+12528)`
// and the same for +11840/+12536). SetStreamSource compares against it and
// raises dirty bit 0x80000 when they differ, which is what schedules a repatch.
// Not an input to the layout -- it exists here so a diagnostic can show whether
// the stride a draw uses is the one the shader was actually patched with.
inline constexpr uint32_t kDevDeclStrideMirrorOffset = 11832;

// The Xenos vertex fetch constant slot a stream binds to. Taken verbatim
// from the patcher, which computes `95 - stream` and writes it into word0
// masked with 0x7F00000:
//     stream 0 -> slot 95,  stream 1 -> slot 94,  stream 2 -> slot 93, ...
inline constexpr uint32_t kFetchSlotForStream0 = 95;
inline uint32_t FetchSlotForStream(uint32_t stream) {
  return kFetchSlotForStream0 - stream;
}

// One decoded D3DVERTEXELEMENT9 as stored in the guest declaration.
struct DeclarationElement {
  uint32_t stream = 0;
  uint32_t offset = 0;  // bytes into the vertex
  uint32_t type = 0;    // packed Xenon decltype
  uint32_t usage = 0;
  uint32_t usage_index = 0;
};

// Reads the element at `index` from a declaration object's array. `records`
// points at the object's +52 array in host memory (big-endian records).
DeclarationElement ReadDeclarationElement(const uint8_t* records, uint32_t index);

// Packed Xenon decltype -> DXGI. Returns DXGI_FORMAT_UNKNOWN (0) for a type
// the game has not been observed to use; callers must fail loudly instead of
// guessing a format.
uint32_t DeclTypeToDxgi(uint32_t packed_type);

}  // namespace mcla::native_gfx

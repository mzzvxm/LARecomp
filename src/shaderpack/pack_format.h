#pragma once
// ===========================================================================
// Shader pack file format -- the constants shared by whoever writes a pack, the
// renderer's ShaderStore that reads it, and the validator
// ===========================================================================
// One source of truth for the numbers below: the builder (src/shaderpack), the
// reader (src/gpu/shaders.cpp) and the offline checks include this header
// instead of keeping a copy each. Changing the layout means changing it here,
// bumping kPackVersion, and nowhere else.
//
// Main pack, mcla_shaders.v8.pack (little-endian, as the host):
//   header (32 bytes) : magic "MSPK" | version u32 | entry count u32 | formats u32 | 16 reserved
//   entries (88 bytes): sorted by key, strictly increasing
//       key u64 | stage u8 ('v' / 'p') | spec_mask u8 | velem_count u16 | velem_off u32 |
//       flags u32 | texture_fetch_mask u32 | 4 blob records: DXIL variant 0, DXIL variant 1,
//       SPIR-V variant 0, SPIR-V variant 1 (variant 1 exists only when spec_mask != 0)
//   blob record (16)  : offset u32 | size u32 | fnv u64 (FNV-1a 64 of the blob)
//   then the vertex element words (u32: address:12 | usage:4 | usage_index:4), then the blobs.
// Offsets are u32, so a pack stays under 4 GiB (kMaxPackBytes).
//
// HDR variant pack, mcla_shaders_hdr.v1.pack (the optional second pack of the HDR stage 3 work,
// branch feat/hdr-scene; main does not read it yet -- the constants live here so the writer and
// that reader agree from the start):
//   header (32 bytes) : magic "MSHV" | version u32 | entry count u32 | formats u32 | contract u32 |
//                       main pack version u32 | 8 reserved
//   entries (80 bytes): pixel shader key u64 | 4 vertex shader keys u64 (0 = none) | flags u32 |
//                       reserved u32 | DXIL blob record | SPIR-V blob record
// ===========================================================================

#include <cstdint>

namespace larecomp::shaderpack {

// --- file names (a pack folder holds these) --------------------------------

inline constexpr const char* kPackFileName = "mcla_shaders.v8.pack";
inline constexpr const char* kHdrPackFileName = "mcla_shaders_hdr.v1.pack";
// Written last by the builder, after the packs are in place (section 3.1 of the pipeline plan).
inline constexpr const char* kStampFileName = "mcla_shaders.stamp";

// --- main pack ---------------------------------------------------------------

inline constexpr char kPackMagic[4] = {'M', 'S', 'P', 'K'};
inline constexpr uint32_t kPackVersion = 8;
inline constexpr uint32_t kPackHeaderBytes = 32;
inline constexpr uint32_t kPackEntryBytes = 88;
inline constexpr uint64_t kMaxPackBytes = 0xFFFFFFFFull;  // blob and element offsets are u32

// Header field offsets.
inline constexpr uint32_t kHeaderVersion = 4;
inline constexpr uint32_t kHeaderCount = 8;
inline constexpr uint32_t kHeaderFormats = 12;

// Which blob formats a pack carries (the header's `formats`).
enum Format : uint32_t {
  kDxil = 1u << 0,   // D3D12
  kSpirv = 1u << 1,  // Vulkan
};

// Entry field offsets.
inline constexpr uint32_t kEntryKey = 0;
inline constexpr uint32_t kEntryStage = 8;
inline constexpr uint32_t kEntrySpecMask = 9;
inline constexpr uint32_t kEntryVertexElementCount = 10;
inline constexpr uint32_t kEntryVertexElementOffset = 12;
inline constexpr uint32_t kEntryFlags = 16;
inline constexpr uint32_t kEntryTextureFetchMask = 20;
inline constexpr uint32_t kEntryDxil0 = 24;
inline constexpr uint32_t kEntryDxil1 = 40;
inline constexpr uint32_t kEntrySpirv0 = 56;
inline constexpr uint32_t kEntrySpirv1 = 72;

inline constexpr uint32_t kBlobRecordBytes = 16;  // offset u32 | size u32 | fnv u64

inline constexpr uint8_t kStageVertex = 'v';
inline constexpr uint8_t kStagePixel = 'p';

// Entry flags. Bit 0: a vertex fetch takes its index from something other than r0.x. Bits 8..12:
// the pixel shader's outputs (kPixelOutput* bits, PixelShader::outputs of the container).
inline constexpr uint32_t kFlagComputedFetchIndex = 1u << 0;
inline constexpr uint32_t kFlagPixelOutputsShift = 8;
inline constexpr uint32_t kFlagPixelOutputsMask = 0x1Fu;

enum PixelOutput : uint8_t {
  kPixelOutputColor0 = 1u << 0,
  kPixelOutputColor1 = 1u << 1,
  kPixelOutputColor2 = 1u << 2,
  kPixelOutputColor3 = 1u << 3,
  kPixelOutputDepth = 1u << 4,
};

// Blob checks done when a blob is first used (and by the validator): DXIL starts "DXBC", SPIR-V
// is at least a header's 20 bytes and starts with its magic word.
inline constexpr char kDxbcMagic[4] = {'D', 'X', 'B', 'C'};
inline constexpr uint32_t kSpirvMagic = 0x07230203u;
inline constexpr uint32_t kSpirvMinBytes = 20;

// --- HDR variant pack ------------------------------------------------------------

inline constexpr char kHdrPackMagic[4] = {'M', 'S', 'H', 'V'};
inline constexpr uint32_t kHdrPackVersion = 1;
// The shared-constants layout the HDR variants were compiled against (hdr_tonemap.h).
inline constexpr uint32_t kHdrContract = 1;
inline constexpr uint32_t kHdrPackHeaderBytes = 32;
inline constexpr uint32_t kHdrPackEntryBytes = 80;
inline constexpr uint32_t kHdrVertexShaders = 4;

}  // namespace larecomp::shaderpack

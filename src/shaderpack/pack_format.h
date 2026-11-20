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
// Variant pack, mcla_shaders_variants.v3.pack (the optional second pack: the shaders the renderer can draw with
// another version of the game's own text -- the HDR scene's tone map, the bloom's Gaussians at a render scale, the
// car reflections' environment map at its scale, the sun shadow collector's soft lookup, ...; src/shaderpack/
// variant_pack.h and variant_chain.h; read by src/gpu/shaders.cpp):
//   header (32 bytes) : magic "MSHV" | version u32 | entry count u32 | formats u32 | contract u32 |
//                       main pack version u32 | 8 reserved
//   entries (88 bytes): key u64 | 4 vertex shader keys u64 (0 = none) | variant u32 | flags u32 |
//                       vs_features u32 | reserved u32 | DXIL blob record | SPIR-V blob record
//                       sorted by (flags & vertex, variant, key), strictly increasing
//       key             the pixel shader's identity, or the vertex shader's when flags bit 0 is set
//       vertex shaders  of a pixel shader entry: the vertex shaders it is drawn with, when it reads their outputs
//                       (the HDR tone map; the runtime requires one of them); 0 = none
//       variant         the variant word, as PipelineKey::ps_variant / vs_variant carries it: bit 0 the main pack's spec
//                       variant of the shader (a pixel shader's ALPHA_TEST, a vertex shader's R11G11B10_NORMAL: the
//                       entry is the same text compiled with that spec mask), bits 1..31 the set of features (kFeature*)
//                       whose edits it carries, composed in chain order (variant_chain.h): never 0 and never 1, those
//                       are the main pack's. Version 2 had one id 2..15 for one change; a shader can now have the
//                       edits of several features in one text.
//       flags           bit 0: a vertex shader entry (kVariantEntryVertex)
//       vs_features     a pixel shader entry: the vertex shader features its draw's vertex shader must have (a vertex
//                       entry with exactly those features, for the draw's own spec variant); 0 = none
//   blob record (16)  : offset u32 | size u32 | fnv u64 (FNV-1a 64 of the blob), as in the main pack
//                       Entries whose blobs are byte for byte the same (the shadow collector's shaders
//                       share one text per body shape) point at one copy: offsets need not be increasing.
// Version 1 (mcla_shaders_hdr.v1.pack) held the HDR scene's tone map only; version 2
// (mcla_shaders_variants.v2.pack) had 80 byte entries with `variant` as a single id 2..15 (one variant per shader,
// 14 ids in all). Neither is read: the file names differ, the builder removes both old files when it writes the
// new one, and the reader refuses a file whose version is not this one (ShaderStore::LoadVariants), so a pack
// built for another version is never half read.
// ===========================================================================

#include <cstdint>

namespace larecomp::shaderpack {

// --- file names (a pack folder holds these) --------------------------------

inline constexpr const char* kPackFileName = "mcla_shaders.v8.pack";
inline constexpr const char* kVariantPackFileName = "mcla_shaders_variants.v3.pack";
// What version 2 of the variant pack was called: the builder removes it when it writes the new one.
inline constexpr const char* kLegacyVariantPackFileName = "mcla_shaders_variants.v2.pack";
// What version 1 of the variant pack was called: the builder removes it too.
inline constexpr const char* kLegacyHdrPackFileName = "mcla_shaders_hdr.v1.pack";
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

// --- variant pack ----------------------------------------------------------------

inline constexpr char kVariantPackMagic[4] = {'M', 'S', 'H', 'V'};
inline constexpr uint32_t kVariantPackVersion = 3;
// The shared-constants layout the variants were compiled against: the main pack's 608 bytes (v7 and
// v8), and for the variants that have extra constants the bytes after them, laid out by the chain
// (variant_chain.h LayoutConstants: 1 had one block at byte 608 per variant, 2 allocates the blocks of the
// features of a composed shader one after the other in chain order; a variant alone is at 608 in both).
inline constexpr uint32_t kVariantContract = 2;
inline constexpr uint32_t kVariantPackHeaderBytes = 32;
inline constexpr uint32_t kVariantPackEntryBytes = 88;
inline constexpr uint32_t kVariantVertexShaders = 4;

// The variant word: PipelineKey::ps_variant (and vs_variant) and the variant pack's `variant` field. Bit 0 is the main pack's
// spec variant (0: none; 1: the pixel shader's ALPHA_TEST, the alpha test and alpha to coverage one; for a vertex shader its
// R11G11B10_NORMAL one), the other 31 bits are the set of FEATURES whose edits the shader has (kFeature* below, one bit each).
// 0 and 1 are the main pack's own shaders; any other word is the variant pack's, and a word with more than one feature bit is
// a composed shader: its text has the edits of those features applied one after the other in chain order (ascending feature
// index; src/shaderpack/variant_chain.h, which also has the word helpers and the constants layout). Version 2 of the pack
// had one id 2..15 per change and a shader had one of them at most; the ids 2..11 of that are the feature indices
// below plus 2, so the HDR tone map's word is still 2. Indices are never reused for another change: a pipeline cache holds
// words, and a word whose shader does not have it is dropped at load.
inline constexpr uint32_t kFirstPackedVariant = 2;  // words 0 and 1 are the main pack's
inline constexpr uint32_t kMaxFeatures = 31;        // indices 0..30: the word's bits 1..31
inline constexpr uint32_t kFeatureHdrScene = 0;     // the tone map continued above white, into a float image
inline constexpr uint32_t kFeatureBloomGauss = 1;   // the bloom's two Gaussians: taps at the render scale
inline constexpr uint32_t kFeatureEnvMap = 2;       // the car reflections' environment map: its mip level pass and its 64x32 reduction at the map's scale
inline constexpr uint32_t kFeatureShadowFilter = 3;  // the sun shadow collector's lookup: soft Vogel taps (16 bytes of constants)
inline constexpr uint32_t kFeatureGlowSoft = 4;     // the glow sprites' visibility: a share of 24 depth taps, not one compare
inline constexpr uint32_t kFeatureCarSelfRefl = 5;  // the player's car paint: its environment reads occluded and blended with the car's own previous frame (112 bytes of constants; G8b)
inline constexpr uint32_t kFeatureLuminanceScale = 6;  // the auto-exposure's sample pass at a render scale
// The car's chrome (xChrome::PS_Chrome: the tailpipes, the rims, the trim) with its reflection and its sky ambient occluded by the car's
// own parts: the same 112 bytes of constants as the paint's (feature 5), centimetre scale rays (shaderpack/variants/car_chrome.h).
inline constexpr uint32_t kFeatureCarChrome = 7;
// The UI's textured pixel shaders (xrage_im's, the Flash player's bitmaps, text and panels) with each 2D fetch reconstructed sharper where the
// console's 720p bitmaps are magnified: Catmull-Rom colour and a steeper alpha edge (16 bytes of constants; shaderpack/variants/ui_sharp.h). It has
// the alpha test twin: compiled with the shader's ALPHA_TEST spec constants (the main pack's spec variant 1) the word's bit 0 is set (it declares SV_Coverage).
inline constexpr uint32_t kFeatureUiSharp = 8;
// Reserved for the cars' headlights as spot lights in the city's lit pixel shaders (feat/headlights, 672 bytes of constants, with the alpha test
// twin); not built by this tree. Its old id was 12 and 13.
inline constexpr uint32_t kFeatureHeadlight = 9;
// The next free index is 10 (the G-buffer's stages, the lamp shadows ...): a new feature takes the next one, with its row in variant_chain.h's table.

// Entry flags of the variant pack (the entry's `flags`).
inline constexpr uint32_t kVariantEntryVertex = 1u << 0;  // the key is a vertex shader's

}  // namespace larecomp::shaderpack

#pragma once
// ===========================================================================
// LARecomp city hit data -- THE FROZEN INTERFACE (package H0 of perf/CITY-RT-M2.md, section 3; perf/CITY-RT-M3.md uses it too)
// ===========================================================================
// A ray that hits the city needs more than the triangle: the texture coordinates and the vertex colour of the corners, the material of the surface (class, smoothness,
// the texture, the tint, the average colour, the emissive scale), the texture itself. The city pack (pack_format.h, M1) holds positions, indices and runs of classes only,
// and its stream ids from kStreamFirstAttribute up are reserved for this. M2 (decision D1) keeps the hit data in a SEPARATE file, built on the player's PC from the player's own
// game files and only when ray traced reflections or global illumination are wanted; M1's pack is untouched. Never shipped, never committed.
//
//   mcla_city_hit.v1.pack   (kHitFileName)  the hit data of the sectors, the global material table and the texture table
//   mcla_city_tex.v1.pack   (kTexFileName)  the textures' blocks (BC1/BC2/BC3, RGBA8 for the few raw ones), the texture table says where
//   mcla_city_hit.stamp     (kHitStampFileName) what they were built from (the base pack's stamp + the rule versions + the texture caps; H1 writes it, stamp.h has the shape)
//
// THIS FILE, hlsl/city_hit_common.hlsli (the decode, in HLSL) and city_hit_format_test.cpp (which compiles the .hlsli as C++ and checks it against this file) are THE INTERFACE of
// the packages H1 (the builder: writes the hit file), H2 (the texture set: writes and loads the texture file, fills the texture rows), H3 (the pool and the scene: fills the pool and the
// tables), H4 (the shading library: reads through the .hlsli) and G0 (M3's frame interface). It is frozen: a package that needs another field or value says so in its report, it does not
// edit this file. WHAT MAY CHANGE without a new version: values that are APPENDED (a texture format, a stream id, a flag bit, a corner encoding, a field of HitBudget) where the
// reader skips or refuses what it does not know; everything else is a new kHitFormatVersion (the reader refuses another number, as pack_format.h) and, where the GPU layout changes, a new
// CITY_HIT_FORMAT_VERSION in the .hlsli. kHitBuilderVersion is the builder's rules (what a run is, how a material is made): it is in the stamp, a pack of another builder version is
// rebuilt but still readable. Bump the right one by hand.
//
// WHAT EACH PACKAGE BUILDS ON IT
//   H1  the builder: fills HitMaterial / HitTexture / HitRun / HitBlob, writes the sections with PlanHitFile and the Write functions, picks the corner encoding, stamps; the golden counts.
//   H2  the texture set: writes the texture file (TexHeader + the blocks HitTexture::data_offset names, mips by MipOffset), loads it, fills a GpuTextureRow (MakeTextureRow) when a texture
//       is uploaded, counts it in vram_budget.
//   H3  the pool and the scene: sub-allocates blocks (PlanPoolBlock), fills them (FillPoolBlock), writes the sector rows (MakeSectorRow), binds t6..t9, keeps positions and indices.
//   H4  the library: hlsl/city_hit_shade.hlsli on CityHitDecode / CityHitLoadMaterial / CityHitLoadTexture; its C++ reference reads through DecodeHit; the rt_hit_common split below.
//   G0  M3's frame interface: the material table's class, average colour and emissive scale (the probes' albedo and light), the pool, and HitBudget's gi_* fields.
//
// THE ORDER: the hit file has the SAME sectors in the SAME order as the base pack (a sector's record says its name hash, the reader checks it), and inside a sector the SAME
// triangles in the SAME order: first the opaque ones [0, opaque_triangles), then the alpha tested ones, exactly as the structure was built (city_rt_scene.cpp: one geometry for each
// non empty group, so the geometry index of a hit is 0 for opaque triangles, and 1 for the alpha tested ones when there are opaque triangles too, else 0).
//
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// THE FILE (all integers little endian; the file is written and read field by field through bytes.h, never by copying a struct)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//   HitHeader            128 bytes at 0
//   sector table         sector_count x kHitSectorRecordBytes (48) at table_offset: the blobs' places (HitSectorRecord)
//   material table       material_count x 32 bytes (HitMaterial::Pack: EIGHT words, and the same eight words are one row pair of the GPU's material buffer: uploaded as they are)
//   texture table        texture_count x 32 bytes (HitTexture::Pack)
//   hit blobs            at blobs_offset: for each sector its hi blob, then its lod blob (either may be absent: size 0)
// Each section starts on 16 bytes (PlanHitFile). header.table_hash is the FNV-1a over everything from table_offset to blobs_offset (the three tables and their padding): the texture file
// holds a copy (TexHeader::hit_table_hash) so that a texture file of another build is never read with this file.
//
// A HIT BLOB is the attributes of one mesh of the base pack (a sector's hi detail or its lod), triangle for triangle:
//   HitBlobHeader        64 bytes: counts, the flags, a hash of what follows
//   stream table         stream_count x 24 bytes (StreamEntry of pack_format.h: {id, encoding, offset, size, count, element_bytes}; offsets from the blob's start)
//   streams              kHitStreamRuns (HitRun, 32 bytes each, raw) and, unless the blob says it has none, kHitStreamCorners (the corner attributes, encoded)
// The runs PARTITION the triangles in order (no gaps, no overlap: first_triangle of run i+1 is first_triangle + triangle_count of run i, the first starts at 0) and no run holds both
// opaque and alpha tested triangles. The base pack's own runs (classes) are another partition of the same list and are not repeated here. A run is "triangles of one MATERIAL
// with one UV range": the builder cuts a run where the material changes, the source geometry changes, or the run's UV range would be wider than kMaxRunUvRange (the quantisation below).
// The per-triangle run id the GPU wants (u16) is NOT stored: the runs give it (ExpandRunIds), so the file is 2 bytes a triangle smaller than the pool.
//
// A CORNER is 8 bytes, per triangle corner (3 a triangle, in the index stream's order, so corner k of triangle t is element 3 t + k):
//   word 0   u in bits 0..15, v in bits 16..31: UNORM16 over the RUN's range (uv = bias + scale * q / 65535; the run holds bias and scale as float16 pairs, chosen so that the
//            range is covered: ChooseUvTransform). Texture coordinates of 8 to 16 (tiled roads and walls) in a run of range 8 keep 0.016 texel at 256 pixels; a plain half float
//            has a step of 2^-7 there, two texels at 256 pixels.
//   word 1   the vertex colour RGBA8: r in bits 0..7 ... a in bits 24..31. The alpha is the game's baked ambient occlusion (CHECK C2 of the plan: what the rgb means).
// Only UV set 0 in version 1 (CHECK C3: which groups read a second set). A corner stream has two encodings (CornerEncoding): raw (the GPU's own bytes) and delta (below; the plan's
// estimate is about 2.5 bytes a corner on the city's meshes [E], H1 measures it: the colour is constant over a run, the uv changes by a small step from one corner to the next).
// The builder keeps whichever of the two is smaller for a blob (the decoder takes either).
// Measured on the retail game (H1) neither reaches the owner's budget: the builder writes the corner stream as the INDEXED stream of hit_corner_codec.h (stream id 3, appended as
// this header allows) and, since CITY-HIT-SIZE, as a ZSTD FRAME of it (the stream entry's encoding 2, city_zstd.h): 174 MB for the retail corners, 110 MB with the frames. A reader goes
// through DecodeHitBlobAny and sees the same HitBlob whatever the stream was.
//
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// THE GPU LAYOUT (what H3 builds from the files, what hlsl/city_hit_common.hlsli reads; the C++ twin of the decode is DecodeHit)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// ONE pool, ONE ByteAddressBuffer (gCityPool, t6): every sector's per-triangle data, sub-allocated per sector detail (H3's range allocator, car_rt_scene_rules.h) and given back on
// eviction, addressed by a 32 bit BYTE offset from the buffer's start (so the pool is at most 4 GiB less 16 bytes: kMaxPoolBytes; the class budgets are 64 to 256 MiB). One buffer, not
// a few slabs: the shader indexes by one number, no unbounded buffer array (D3D12 and Vulkan differ there). A slab design would be a new CITY_HIT_FORMAT_VERSION.
// A sector detail's BLOCK (PlanPoolBlock; every array starts on 16 bytes; the sector row holds the five offsets, they are absolute pool offsets):
//   indices      uint32 x 3 a triangle, the triangle's vertex numbers (the base pack's index order, vertex numbers local to the sector's mesh)
//   positions    float x 3 a vertex, WORLD space (the base pack's welded positions; H3 may build the structure over these very bytes)
//   run ids      uint16 a triangle, two to a word (triangle t is bits 16 (t & 1) .. of word t / 2), the index of the triangle's run in the run table   [only with kSectorRowRuns]
//   run table    HitRun x run_count, 32 bytes each                                                                                                       [only with kSectorRowRuns]
//   corners      8 bytes x 3 a triangle, raw (the corner words above)                                                                                      [only with kSectorRowCorners]
// The SECTOR TABLE is a StructuredBuffer<uint4> (gCitySectors, t7), TWO rows a slot (GpuSectorRow, 8 words), indexed by InstanceID decoded as city_rt_rules.h InstanceId made it:
// `row = InstanceID & 0x7FFFFF` = (sector << 1) | detail (0 hi, 1 lod), so the table has 2 x sector_count slots (878 for the retail game, 28 KiB) and a slot of a structure that is not
// built has flags 0. The MATERIAL TABLE (gCityMaterials, t8) is a StructuredBuffer<uint4>, TWO rows a material (the 32 byte record), indexed by the run's global material id;
// material 0 is ALWAYS the default material (a world surface of a mid grey), so an id that was not resolved is 0, never a hole. The TEXTURE TABLE (gCityTextures, t9) is a
// StructuredBuffer<uint4>, ONE row a texture (GpuTextureRow, 4 words), indexed by the material's texture index; H2 writes a row's slot when the texture is uploaded (the records
// above are immutable, only this table changes while the game runs): slot 0xFFFFFFFF = not uploaded (the hit takes the material's average colour). The bindings sit in the RT
// descriptor set after the car scene's t0..t3 and rt-hit-shading's t4 (uvs) and t5 (frame rows and car materials); the car code path is untouched, the instance mask picks the path.
//
// THE MATERIAL RECORD (HitMaterial, 32 bytes, eight words; shared with the cars where the cars have the field):
//   word 0   the G-BUFFER'S MATERIAL WORD (gbuffer_types.h): smoothness unorm8 in bits 0..7, SurfaceClass in 8..12, flags (SSR, alpha covered, vehicle) in 13..15, lit weight unorm8 in
//            16..23, and the METAL byte in 24..31 (the bits gbuffer_types.h keeps reserved "a metalness byte may take"). So a reflected wall has the class and gloss the wall has
//            on screen, and GB-F1's word (gbuffer_material.h WordFor) drops in with the metal byte added. A class is the G-buffer's, not the cars' (car_rt_material_rules.h Class:
//            paint, glass, tyre, chrome, light, interior, other): a city hit is never a car part.
//   word 1   texture index in bits 0..15 (the diffuse; 0xFFFF none), second layer's texture index in 16..31 (road grime, decal multiply: version 1 never writes it)
//   word 2   the tint, R10G10B10 over 0..2 (the cars' PackRgb10, bit for bit; the group's parameters: where the shader multiplies the texture), bits 30..31 the GLOW KIND (R7: what the
//            material's glow follows: kGlowNight the fill lights' night curve, kGlowLens the street lamps' schedule, kGlowAlways none; 0 for a material that does not glow, and for a pack
//            written before R7 whose bits were zero: it reads as kGlowNight, the schedule the glow had)
//   word 3   the AVERAGE colour, R10G10B10 over 0..2: tint x the mean of the diffuse texture (linear), or the tint alone / the class grey where there is no texture; what a hit shows
//            where the texture is not uploaded, and all that M3's probes read; bits 30..31 zero
//   word 4   the emissive scale (float bits; 0 for what does not glow)
//   words 5, 6   the diffuse texture's XXH3-64 key (low, high): the number GB-F2's per-texture rows ("hash:<hex>") and the texture dumps name a texture by; the GPU never reads it
//   word 7   the shader hash (rage hash of "<group>/<preset>", as the pack's runs): the census and the per-shader rules; the GPU never reads it
// THE TEXTURE RECORD (HitTexture, 32 bytes): key (XXH3-64 of the base level, the same number), width and height (the file's full size), format, mip count, flags (kTextureGamma: the
// game's piecewise-linear gamma where the fetch constant asks, kTextureAlpha), the data's offset and size in the texture file (size = the chain's; the last word, stored_size, is what
// the texture takes IN the file when that is a zstd frame of the chain: city_zstd.h, TexHeader::flags kTexFlagZstd; 0 = the chain as it is). The GPU's row (GpuTextureRow): slot (bit 31 the gamma bit as
// descriptors.h kTextureGammaBit, as the cars' diffuse slot), the full size, the number of resident mips and the first resident one (the class's cap drops the top levels), flags.
//
// WHAT THE HLSL KNOWS: the decode of all the above for a ray's hit: CityHitDecode(InstanceID, GeometryIndex, PrimitiveIndex, barycentrics) -> the triangle's three object space positions
// (the structure's own space: world; the instance transform is the frame's world to view, CommittedObjectToWorld3x4), the geometric normal and the two areas (world, texture) a texture
// level is chosen from, the interpolated uv and colour, the material id, the run's flags; CityHitLoadMaterial, CityHitLoadTexture. The shading is H4's (city_hit_shade.hlsli).
//
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// THE rt_hit_common SPLIT (the plan H4 carries out together with the owner of feat/rt-hit-shading: ONE agent, ONE branch, no double rebase; do it after rt-hit-shading is merged)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The car hit shading (hlsl/car_rt_hit_shade.hlsli, car_rt_material_rules.h, car_rt_hit_rules.h on feat/rt-hit-shading) holds, beside what is about a CAR, the pieces every hit shading
// needs. They move, by name (renamed Car -> RtHit, bodies unchanged), into hlsl/rt_hit_common.hlsli and a pure twin src/gpu/rt_hit_frame.h; the car files keep only the car's own.
//   MOVES (hlsl)   CarMatFrame + CarMatLoadFrame (the frame's rows 0..7: sun direction and colour, ambient, env map slots, night factor, sky, gain, the world's axes), the row and flag
//                  constants (kMatFrameRows, kMatCascade*Row, CAR_MAT_FRAME_*), CarMatCascadeLit + CarMatLoadDepth (the game's sun cascades at a world point), CarMatPwl, CarMatTexel,
//                  CarMatBilinear, CarMatSample (a texel / bilinear / two-level read of a bindless texture by slot, gamma bit, wrap or clamp), CarMatEnvUv (the paraboloid) and the
//                  environment read at a world direction, CarMatRgb10, CarMatSunVisible (gets the instance mask as an argument: the city's cars + city = every bit), the constants
//                  kConeLodScale, kEmissiveGain, kFillShare, kCascadeNormalOffset, kMatNoSlot, kMatTintMax. The frame rows are read through ONE macro (RT_HIT_FRAME_ROW(i)) the includer
//                  defines as its own buffer (gCarRtMaterials for the car passes, where the rows come first; another buffer for the SSR fallback and the GI passes), so a pass without
//                  the car's instances still has a frame.
//   MOVES (C++)    car_rt_material_rules.h: FrameLight, SunCascades, FrameInput, MakeFrameRows, the frame row constants and kFrame* flags, kMaxCascades, PackRgb10 / UnpackRgb10,
//                  FloatToHalf, kTintMax, kNoSlot, kGammaBit, Bits / FromBits (rt_hit_frame.h; the car header includes it). car_rt_hit_rules.h: the calibration (the census's stats rows,
//                  the per-class EWMA gain, the clamp 0.5 to 3): it becomes keyed by class with room for the G-buffer's 16 classes beside the cars' 8 (rt_hit_calibration), and the
//                  frame's night factor stays a frame row.
//   STAYS (car)    the car classes (CAR_MAT_CLASS_*), CarMat and its loader, CarRtHitUv (the car's own uv buffer), CarHitShade, the clear coat's helpers, the per-class tables, the
//                  level (car_rt_hit::Level: others_reach, coat, cone_rays) and the shader rules by identity.
//   THIS FILE'S SHARE   city_hit_common.hlsli includes nothing of it and needs none of it (it decodes records); city_hit_shade.hlsli (H4) includes both and holds ResolveHit and
//                  CityHitShade. The frame rows' layout is not touched by the split: the 32 rows of rt-hit-shading stay as they are, so the city's passes read what the car's do.
//
// THE LEVEL TABLE (HitBudget below): the fields M2 and M3 fill per (graphics class, level); this header fixes WHAT is budgeted, the numbers are M2 section 8 and M3 section 6 (estimates
// until the first cost run) and live where the class table lives (R8, G4). HitBudget is host only (never in a file, never on the GPU), so a field may be appended later without a version.
//
// CHECK (nothing here was run against the game; each is a line of the first builder run or a picture):
//   1. the byte parity of the material tint with the cars' PackRgb10 (the test holds a copy of the formula as it stands on feat/rt-hit-shading 0a1cc4e4: compare again after the merge);
//   2. the vertex colour's meaning and the alpha as ambient occlusion (plan C2), the second UV set (C3): corners have room for neither beyond what is here;
//   3. the texture-name resolution rate (C1): a material whose texture does not resolve has texture 0xFFFF and its average is the tint alone, and the log counts it;
//   4. one pool buffer of 64 to 256 MiB on both backends (the largest storage buffer range is 4 GiB on desktop Vulkan and D3D12; a device with less takes a smaller class budget);
//   5. kMaxRunUvRange (the builder splits a run beyond it): 256 gives 0.004 of a texture repeat, one texel at 256 pixels, a tiled road keeps its detail;
//   6. whether the base pack's welded vertices are in the order a pool block assumes (the base pack's index stream is vertex numbers local to the mesh: yes in version 1).
//
// Includes bytes.h and pack_format.h (the stream entry) and nothing of the renderer: the builder, the reader and the tests link it alone.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "citypack/bytes.h"
#include "citypack/pack_format.h"

namespace larecomp::citypack {

// ---------------------------------------------------------------------------
// versions, names, sizes
// ---------------------------------------------------------------------------

inline constexpr char kHitMagic[8] = {'L', 'A', 'R', 'C', 'H', 'I', 'T', '1'};
inline constexpr char kTexMagic[8] = {'L', 'A', 'R', 'C', 'T', 'E', 'X', '1'};
inline constexpr uint32_t kHitFormatVersion = 1;
inline constexpr uint32_t kHitBuilderVersion = 2;  // 2: R7's glow kind in the material's tint word and the glow a GB-F2 row gives (the lit windows)
inline constexpr const char* kHitFileName = "mcla_city_hit.v1.pack";
inline constexpr const char* kTexFileName = "mcla_city_tex.v1.pack";
inline constexpr const char* kHitStampFileName = "mcla_city_hit.stamp";

inline constexpr uint32_t kHitHeaderBytes = 128;
inline constexpr uint32_t kHitSectorRecordBytes = 48;
inline constexpr uint32_t kHitBlobHeaderBytes = 64;
inline constexpr uint32_t kHitMaterialBytes = 32;
inline constexpr uint32_t kHitTextureBytes = 32;
inline constexpr uint32_t kHitRunBytes = 32;
inline constexpr uint32_t kTexHeaderBytes = 64;
inline constexpr uint32_t kHitBlobMagic = 0x31424843;  // "CHB1"
inline constexpr uint32_t kHitSectionAlignment = 16;

inline constexpr uint32_t kHitStreamRuns = 1;
inline constexpr uint32_t kHitStreamCorners = 2;
inline constexpr uint32_t kTexFlagZstd = 1;       // TexHeader::flags: textures may be stored as zstd frames (HitTexture::stored_size); a reader refuses a flag it does not know
inline constexpr uint32_t kTexFlagsKnown = kTexFlagZstd;
inline constexpr uint32_t kHitBlobHasCorners = 1;  // HitBlobHeader::flags: the corner stream is there (a blob may hold runs alone: materials without texture coordinates)

inline constexpr uint32_t kDefaultMaterial = 0;      // material 0 is always the default: a world surface, a mid grey
inline constexpr uint32_t kMaxMaterials = 1u << 20;  // a run's material id is 32 bits; the table is never this big (an estimate of 5 to 10 thousand)
inline constexpr uint32_t kMaxTextures = 0xFFFEu;    // texture indices are 16 bits, 0xFFFF is "none"
inline constexpr uint32_t kMaxRunsPerBlob = 65534;   // a triangle's run id is 16 bits
inline constexpr uint16_t kNoTexture = 0xFFFF;
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;     // a bindless slot that is not there (the same value as gbuffer::kNoSlot)
inline constexpr uint32_t kGammaBit = 0x80000000u;   // descriptors.h kTextureGammaBit: bit 31 of a slot says the texture is read through the game's piecewise-linear gamma
inline constexpr float kMaxRunUvRange = 256.0f;      // the widest texture coordinate range of one run (the builder splits beyond it)

// ---------------------------------------------------------------------------
// the material word (gbuffer_types.h: THE SAME LAYOUT; the test compares every number with that header)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kWordSmoothnessShift = 0;
inline constexpr uint32_t kWordClassShift = 8;
inline constexpr uint32_t kWordFlagsShift = 13;
inline constexpr uint32_t kWordLitShift = 16;
inline constexpr uint32_t kWordMetalShift = 24;  // the byte gbuffer_types.h keeps reserved
inline constexpr uint32_t kWordClassMask = 31;
inline constexpr uint32_t kWordFlagSsr = 1;            // gbuffer_types.h kMaterialFlagSsr
inline constexpr uint32_t kWordFlagAlphaCovered = 2;   // kMaterialFlagAlphaCovered
inline constexpr uint32_t kWordFlagVehicle = 4;        // kMaterialFlagVehicle

// What a glowing material's glow follows (HitMaterial::glow_kind, bits 30..31 of the tint word; hlsl CITY_HIT_GLOW_*; src/gpu/city_hit_shade_rules.h uses them).
inline constexpr uint32_t kGlowNight = 0;   // neon, signs, lit windows, interiors: the fill lights' night curve
inline constexpr uint32_t kGlowLens = 1;    // a street lamp's lens: the street lamps' schedule (the prop pass's on hours)
inline constexpr uint32_t kGlowAlways = 2;  // a traffic signal: lit by day as well
inline constexpr uint32_t kGlowKinds = 3;   // the value 3 is not used: it reads as kGlowNight
inline constexpr uint32_t kGlowShift = 30;

constexpr uint32_t PackMaterialWord(uint32_t smoothness, uint32_t surface_class, uint32_t flags, uint32_t lit, uint32_t metal) {
  return (smoothness & 0xFFu) << kWordSmoothnessShift | (surface_class & kWordClassMask) << kWordClassShift | (flags & 7u) << kWordFlagsShift | (lit & 0xFFu) << kWordLitShift |
         (metal & 0xFFu) << kWordMetalShift;
}
constexpr uint32_t WordSmoothness(uint32_t word) { return (word >> kWordSmoothnessShift) & 0xFFu; }
constexpr uint32_t WordClass(uint32_t word) { return (word >> kWordClassShift) & kWordClassMask; }
constexpr uint32_t WordFlags(uint32_t word) { return (word >> kWordFlagsShift) & 7u; }
constexpr uint32_t WordLit(uint32_t word) { return (word >> kWordLitShift) & 0xFFu; }
constexpr uint32_t WordMetal(uint32_t word) { return (word >> kWordMetalShift) & 0xFFu; }

// R10G10B10 over 0..kHitRgb10Max: the cars' PackRgb10 (car_rt_material_rules.h), bit for bit, so a tint means the same in both records.
inline constexpr float kHitRgb10Max = 2.0f;
uint32_t PackRgb10(const float rgb[3]);
void UnpackRgb10(uint32_t packed, float rgb[3]);

// ---------------------------------------------------------------------------
// float16 (the run's UV transform)
// ---------------------------------------------------------------------------

float HalfToFloat(uint16_t half);
// To the nearest half (ties to even); beyond +-65504 it saturates (never infinity), a NaN is 0.
uint16_t HalfFromFloat(float value);
// The largest half that is <= value, and the smallest that is >= value (the same saturation).
uint16_t HalfFromFloatDown(float value);
uint16_t HalfFromFloatUp(float value);

// ---------------------------------------------------------------------------
// the material record
// ---------------------------------------------------------------------------

struct HitMaterial {
  uint32_t word = PackMaterialWord(38, 1, 0, 255, 0);  // the G-buffer's word + metal: smoothness 0.15, class world, lit
  uint16_t texture = kNoTexture;                       // index in the texture table (the diffuse)
  uint16_t layer = kNoTexture;                         // the second layer's texture (version 1: never written)
  float tint[3] = {1.0f, 1.0f, 1.0f};                  // linear, 0..kHitRgb10Max
  float average[3] = {0.5f, 0.5f, 0.5f};               // tint x the diffuse texture's mean, linear, 0..kHitRgb10Max
  float emissive = 0.0f;
  uint8_t glow_kind = 0;                               // kGlow*: what the glow follows (word 2's bits 30..31); only meaningful where emissive > 0
  uint64_t key = 0;                                    // XXH3-64 of the diffuse texture's base level (0: none)
  uint32_t shader_hash = 0;

  // The eight words (the GPU's two rows). Unpack is Pack's inverse up to the 10 bit colours.
  void Pack(uint32_t words[8]) const;
  static HitMaterial Unpack(const uint32_t words[8]);
  void Write(ByteWriter& w) const;
  static bool Read(ByteReader& r, HitMaterial* out);
};

// ---------------------------------------------------------------------------
// the texture record
// ---------------------------------------------------------------------------

enum class HitTextureFormat : uint8_t {
  kBc1 = 0,    // 8 bytes a 4x4 block
  kBc2 = 1,    // 16
  kBc3 = 2,    // 16
  kRgba8 = 3,  // 4 bytes a pixel (the few raw textures)
  kCount = 4,
};
inline constexpr uint32_t kTextureGamma = 1;  // HitTexture::flags: read through the game's piecewise-linear gamma
inline constexpr uint32_t kTextureAlpha = 2;  // the alpha carries coverage or opacity (a cut-out or a blend), not just a channel

struct HitTexture {
  uint64_t key = 0;  // XXH3-64 of the base level
  uint16_t width = 0;
  uint16_t height = 0;
  HitTextureFormat format = HitTextureFormat::kBc1;
  uint8_t mip_count = 0;  // levels in the file, from the full size down to 1x1 (or the last whole block)
  uint8_t flags = 0;
  uint64_t data_offset = 0;  // in the texture file: where the stored bytes start (see stored_size)
  uint32_t data_size = 0;    // the chain's size: the mips, the full size first, each tightly packed after the one before (ChainBytes(0))
  // CITY-HIT-SIZE (an APPENDED field, the record's last word; the format version stays): the bytes the texture takes IN THE FILE. 0 and data_size mean the chain as it is; any other value
  // (< data_size) is a zstd frame of the chain's levels split into BC planes (city_zstd.h CompressTextureChain), which the reader unpacks into the chain. The texture file's header says
  // kTexFlagZstd when a texture may be like that.
  uint32_t stored_size = 0;

  void Pack(uint32_t words[8]) const;
  static HitTexture Unpack(const uint32_t words[8]);
  void Write(ByteWriter& w) const;
  static bool Read(ByteReader& r, HitTexture* out);
  // Bytes of the levels [first_mip, mip_count), and where level `mip` starts inside the data (0 for the first).
  uint64_t ChainBytes(uint32_t first_mip = 0) const;
  uint64_t MipOffset(uint32_t mip) const;
  // What the texture takes in the texture file, and whether that is a zstd frame (not the chain itself).
  uint64_t StoredBytes() const { return stored_size != 0 ? stored_size : data_size; }
  bool IsCompressed() const { return stored_size != 0 && stored_size != data_size; }
};
// Bytes of one level of `format` at width x height (a block format rounds each side up to whole blocks).
uint64_t HitMipBytes(HitTextureFormat format, uint32_t width, uint32_t height);

// The GPU's row of the texture table (gCityTextures): four words. H2 fills it when the texture is uploaded.
struct GpuTextureRow {
  uint32_t slot = kNoSlot;  // the bindless slot, bit 31 the gamma bit; kNoSlot: not uploaded
  uint32_t size = 0;        // the FULL size: width in bits 0..15, height in 16..31
  uint32_t mips = 0;        // resident mip count in bits 0..7, the first resident level (0: the full size is there) in 8..15, HitTexture::flags in 16..23
  uint32_t reserved = 0;
};
GpuTextureRow MakeTextureRow(const HitTexture& texture, uint32_t slot, uint32_t first_resident_mip);

// ---------------------------------------------------------------------------
// runs and the UV transform
// ---------------------------------------------------------------------------

inline constexpr uint32_t kRunAlphaTested = 1;  // HitRun::flags: the triangles are alpha tested (not opaque: the base pack's GeometryRun flag)
inline constexpr uint32_t kRunHasUv = 2;        // the corners' texture coordinates are meaningful (not set: uv 0)
inline constexpr uint32_t kRunHasColour = 4;    // the corners' vertex colour is meaningful (not set: white)
inline constexpr uint32_t kRunTwoSided = 8;     // reserved for foliage and fences: a hit's normal is not turned to a side

// One run: triangles [first_triangle, first_triangle + triangle_count) of one material with one UV range. The record is 32 bytes (eight words; the GPU reads the first four):
//   word 0 material, 1 flags, 2 uv bias (two float16: u low, v high), 3 uv scale (the same), 4 first_triangle, 5 triangle_count, 6 and 7 zero.
struct HitRun {
  uint32_t first_triangle = 0;
  uint32_t triangle_count = 0;
  uint32_t material = kDefaultMaterial;
  uint32_t flags = 0;
  float uv_bias[2] = {0.0f, 0.0f};   // float16 exact (ChooseUvTransform makes them so)
  float uv_scale[2] = {1.0f, 1.0f};  // float16 exact, > 0

  void Pack(uint32_t words[8]) const;
  static HitRun Unpack(const uint32_t words[8]);
};
// A transform that covers [min_u, max_u] x [min_v, max_v]: the bias rounded down and the scale up to float16, so that every texture coordinate of the range is inside 0..1 after it.
// False (the run is left alone) when a bound is not finite or the range does not fit a float16 (|value| above 65504 or a range wider than 65504): the builder splits the run first
// (kMaxRunUvRange). A flat range gets scale 1.
bool ChooseUvTransform(float min_u, float max_u, float min_v, float max_v, HitRun* run);
// The corner word of a texture coordinate under the run's transform (clamped into the range), and the other way.
uint32_t EncodeCornerUv(const HitRun& run, float u, float v);
void DecodeCornerUv(const HitRun& run, uint32_t word, float uv[2]);
constexpr uint32_t PackCornerColour(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (r & 0xFFu) | (g & 0xFFu) << 8 | (b & 0xFFu) << 16 | (a & 0xFFu) << 24; }

// ---------------------------------------------------------------------------
// the corner stream's encodings
// ---------------------------------------------------------------------------

enum class CornerEncoding : uint32_t {
  kRaw = 0,    // two uint32 a corner, the GPU's own bytes (element_bytes 8)
  kDelta = 1,  // per corner: varint (zigzag(du) << 1 | colour_changed), varint zigzag(dv), then the four colour bytes when it changed; du and dv against the corner before (the
               // first corner against 0, colour 0); element_bytes 0
};
// `words` holds two words a corner (the layout above), `corners` corners.
void EncodeCorners(const uint32_t* words, size_t corners, CornerEncoding encoding, ByteWriter& w);
bool DecodeCorners(ByteReader& r, size_t corners, CornerEncoding encoding, std::vector<uint32_t>* out);

// ---------------------------------------------------------------------------
// the hit blob
// ---------------------------------------------------------------------------

struct HitBlob {
  uint32_t flags = 0;  // kHitBlobHasCorners
  uint32_t triangle_count = 0;
  uint32_t opaque_triangles = 0;
  std::vector<HitRun> runs;
  std::vector<uint32_t> corners;  // 2 words a corner, 3 corners a triangle; empty without kHitBlobHasCorners

  bool has_corners() const { return (flags & kHitBlobHasCorners) != 0; }
};
// Why the runs of a blob are not a partition of its triangles ("" when they are): first run at 0, each next where the last ended, the last at triangle_count, no run across
// opaque_triangles, no run empty, flags kRunAlphaTested only on the alpha tested side, at most kMaxRunsPerBlob runs.
std::string CheckRuns(const std::vector<HitRun>& runs, uint32_t triangle_count, uint32_t opaque_triangles);
std::vector<uint8_t> EncodeHitBlob(const HitBlob& blob, CornerEncoding corners = CornerEncoding::kDelta);
// False (with `error`) for a cut, damaged (hash), inconsistent (runs, counts) or unknown blob. A stream id this build does not know is skipped.
bool DecodeHitBlob(const uint8_t* data, size_t size, HitBlob* out, std::string* error);
// The u16 run id of every triangle, from the runs (the pool's run id array). Empty when CheckRuns would say no.
std::vector<uint16_t> ExpandRunIds(const std::vector<HitRun>& runs, uint32_t triangle_count);

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

struct HitHeader {
  uint32_t format_version = kHitFormatVersion;
  uint32_t builder_version = kHitBuilderVersion;
  uint32_t flags = 0;
  uint32_t sector_count = 0;  // the base pack's
  uint32_t sector_record_bytes = kHitSectorRecordBytes;
  uint32_t material_count = 0;
  uint32_t material_record_bytes = kHitMaterialBytes;
  uint32_t texture_count = 0;
  uint32_t texture_record_bytes = kHitTextureBytes;
  uint64_t base_table_hash = 0;  // PackHeader::table_hash of the base pack this was built over: a hit file is read only beside that pack
  uint64_t base_file_size = 0;
  uint64_t table_offset = 0;
  uint64_t materials_offset = 0;
  uint64_t textures_offset = 0;
  uint64_t blobs_offset = 0;
  uint64_t file_size = 0;
  uint64_t table_hash = 0;  // FNV-1a over [table_offset, blobs_offset)

  void Write(ByteWriter& w) const;
  // False (with `error`) for a file that is not a hit file, of another format version, or whose record sizes are not this build's.
  static bool Read(ByteReader& r, HitHeader* out, std::string* error);
};

// A sector's blobs (hi detail and lod), in the base pack's order. A blob that is absent has offset 0 and size 0.
struct HitSectorRecord {
  uint32_t name_hash = 0;  // the base pack's record's name_hash: the reader checks it
  uint32_t flags = 0;      // kHitSectorFoliageHeavy
  uint64_t hi_offset = 0;
  uint64_t lod_offset = 0;
  uint32_t hi_size = 0;
  uint32_t lod_size = 0;
  uint32_t hi_triangles = 0;  // the base pack's counts: the reader checks them against its own record
  uint32_t lod_triangles = 0;
  uint32_t hi_runs = 0;
  uint32_t lod_runs = 0;

  void Write(ByteWriter& w) const;
  static bool Read(ByteReader& r, HitSectorRecord* out);
};
inline constexpr uint32_t kHitSectorFoliageHeavy = 1;  // many of the sector's props are not in the structures (M2 risk R1: keep part of the environment's weight in a reflection)

// Where the sections go (each on 16 bytes): table at the header's end, then the materials, the textures, the blobs.
struct HitFileLayout {
  uint64_t table_offset = 0;
  uint64_t materials_offset = 0;
  uint64_t textures_offset = 0;
  uint64_t blobs_offset = 0;
};
HitFileLayout PlanHitFile(uint32_t sector_count, uint32_t material_count, uint32_t texture_count);

// The texture file's first 64 bytes; the textures' blocks follow from data_offset.
struct TexHeader {
  uint32_t format_version = kHitFormatVersion;
  uint32_t builder_version = kHitBuilderVersion;
  uint32_t texture_count = 0;
  uint32_t flags = 0;
  uint64_t hit_table_hash = 0;  // HitHeader::table_hash of the hit file this belongs to
  uint64_t data_offset = kTexHeaderBytes;
  uint64_t file_size = 0;

  void Write(ByteWriter& w) const;
  static bool Read(ByteReader& r, TexHeader* out, std::string* error);
};

// ---------------------------------------------------------------------------
// the GPU's pool and tables (the sector row, the block's plan, the reference writer)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kPoolAlignment = 16;
inline constexpr uint64_t kMaxPoolBytes = 0xFFFFFFF0ull;  // 32 bit byte offsets
inline constexpr uint32_t kInstanceIdFlag = 0x800000u;    // city_rt_rules.h kInstanceIdFlag
inline constexpr uint32_t kInstanceRowMask = 0x7FFFFFu;   // InstanceID & this = (sector << 1) | detail = the sector table's slot
constexpr uint32_t SectorRowOf(uint32_t sector, bool lod) { return (sector << 1) | (lod ? 1u : 0u); }
constexpr uint32_t SectorRowOfInstance(uint32_t instance_id) { return instance_id & kInstanceRowMask; }

inline constexpr uint32_t kSectorRowValid = 1;     // GpuSectorRow::flags: the structure is built and its indices and positions are in the pool
inline constexpr uint32_t kSectorRowRuns = 2;      // run ids and the run table are in the pool (the materials are known)
inline constexpr uint32_t kSectorRowCorners = 4;   // the corner attributes are in the pool (needs kSectorRowRuns: the uv transform is the run's)
inline constexpr uint32_t kSectorRowFoliage = 8;   // HitSectorRecord's kHitSectorFoliageHeavy, copied

// The sector table's slot: eight words (two rows), absolute pool byte offsets.
struct GpuSectorRow {
  uint32_t index_offset = 0;
  uint32_t position_offset = 0;
  uint32_t attribute_offset = 0;  // the corners
  uint32_t run_id_offset = 0;
  uint32_t run_table_offset = 0;
  uint32_t triangle_count = 0;
  uint32_t opaque_triangles = 0;
  uint32_t flags = 0;
};

// One block of the pool: the plan of a sector detail's arrays, from their counts. Offsets are from the block's start; `total_bytes` is what the allocator is asked for.
struct PoolBlockPlan {
  uint64_t index_offset = 0;
  uint64_t position_offset = 0;
  uint64_t run_id_offset = 0;
  uint64_t run_table_offset = 0;
  uint64_t attribute_offset = 0;
  uint64_t total_bytes = 0;
  bool has_runs = false;
  bool has_corners = false;
};
PoolBlockPlan PlanPoolBlock(uint32_t vertex_count, uint32_t triangle_count, uint32_t run_count, bool has_runs, bool has_corners);
// The row of a block placed at pool offset `base` (a multiple of kPoolAlignment). False when the block would end beyond kMaxPoolBytes.
bool MakeSectorRow(const PoolBlockPlan& plan, uint64_t base, uint32_t triangle_count, uint32_t opaque_triangles, uint32_t extra_flags, GpuSectorRow* out);
// The reference writer of a block: `mesh` (the base pack's decoded mesh) and `blob` (this mesh's hit blob; null: no runs and no corners) written to `dest` (plan.total_bytes bytes) in
// the layout above, little endian. H3 uses it to fill the staging memory. False (with `error`) when the counts of the mesh and the blob do not agree.
bool FillPoolBlock(const PoolBlockPlan& plan, const MeshData& mesh, const HitBlob* blob, uint8_t* dest, size_t dest_size, std::string* error);

// ---------------------------------------------------------------------------
// the decode, in C++ (the twin of hlsl/city_hit_common.hlsli CityHitDecode; the reference of the shading's tests)
// ---------------------------------------------------------------------------

// What the decode reads: the pool's bytes, the sector table (8 words a slot), the material table (8 words a material).
struct HitView {
  const uint8_t* pool = nullptr;
  size_t pool_size = 0;
  const uint32_t* sectors = nullptr;
  size_t sector_slots = 0;
  const uint32_t* materials = nullptr;
  size_t material_count = 0;
};

struct DecodedHit {
  bool valid = false;  // the slot is built, the triangle is in it and the data is inside the pool
  uint32_t triangle = 0;
  uint32_t sector_flags = 0;
  uint32_t material = kDefaultMaterial;
  uint32_t run_flags = 0;
  float p[3][3] = {};         // the triangle's corners, object space (the world)
  float normal[3] = {0, 1, 0};  // unit, from (p1 - p0) x (p2 - p0), not turned toward a ray
  float world_area = 0.0f;
  float uv[2] = {0, 0};       // interpolated, texture coordinates
  float uv_area = 0.0f;       // the triangle's area in texture coordinates (a texture level is log2 of the texels a metre has: sqrt(uv_area / world_area) x the texture's size)
  float colour[4] = {1, 1, 1, 1};  // interpolated vertex colour, 0..1
};
// geometry_index and primitive_index as a ray query commits them; (b1, b2) the committed barycentrics (the point is (1 - b1 - b2) p0 + b1 p1 + b2 p2).
DecodedHit DecodeHit(const HitView& view, uint32_t instance_id, uint32_t geometry_index, uint32_t primitive_index, float b1, float b2);
// The triangle's index in the sector detail's order from a committed hit: primitive_index + (geometry_index == 1 ? opaque_triangles : 0).
constexpr uint32_t TriangleOfHit(uint32_t geometry_index, uint32_t primitive_index, uint32_t opaque_triangles) {
  return primitive_index + (geometry_index == 1 ? opaque_triangles : 0u);
}

// ---------------------------------------------------------------------------
// the level table's fields (host only; the numbers are M2 section 8 and M3 section 6, set where the class table lives)
// ---------------------------------------------------------------------------

struct HitBudget {
  // the pool and the texture set (H2, H3)
  uint64_t pool_bytes = 0;             // the pool buffer's size (0: the hit data is off)
  uint32_t staged_bytes = 0;           // hit data staged for upload in one frame, beside the structures' own staging (city_rt_rules.h Budget::upload_bytes)
  uint32_t texture_cap_px = 0;         // the longest side a resident texture keeps: 128 Mid, 256 High, 0 = the file's full size (Ultra)
  uint64_t texture_bytes = 0;          // the resident set's memory cap
  uint32_t texture_upload_bytes = 8u << 20;  // texture bytes uploaded in one frame
  // M2's consumers (the reflections)
  float city_reach_m = 0.0f;           // how far a car's reflection ray looks for the city: 60 Mid, 150 High, 300 Ultra
  float far_reach_m = 0.0f;            // ... and through the far level of detail: 0 Mid and High, 1000 Ultra
  uint32_t cone_rays = 0;              // rays a texel of the car's paint: 2 Mid, 4 High and Ultra
  bool coat_ray = false;               // the clear-coat ray on top
  uint32_t fallback_ray_cap = 0;       // SSR's ray traced fallback: rays a frame (0 Mid, 0.25 M High, 1 M Ultra)
  uint32_t fallback_class_mask = 0;    // the G-buffer classes the fallback serves, one bit a SurfaceClass (glass, metal, water at High; road and sidewalk added at Ultra)
  // M3's consumers (the global illumination; G0's gi_frame.h owns the probe field's own table, these are the numbers the pool and the hit shading need)
  uint32_t gi_cascades = 0;            // 2 Mid, 3 High, 4 Ultra
  uint32_t gi_rays_per_probe = 0;      // 32 Mid, 64 High and Ultra
  uint32_t gi_rays_per_frame = 0;      // the field's trace budget: 12 K Mid, 459 K High, 983 K Ultra
  bool gi_pixel_pass = false;          // the per-pixel detail pass (Ultra)

  bool on() const { return pool_bytes != 0; }
};

// ---------------------------------------------------------------------------
// the static checks (the layouts the files and the GPU hold)
// ---------------------------------------------------------------------------

static_assert(sizeof(GpuSectorRow) == 32, "a sector slot is two uint4 rows");
static_assert(sizeof(GpuTextureRow) == 16, "a texture row is one uint4");
static_assert(kHitHeaderBytes == 128 && kHitSectorRecordBytes == 48 && kHitBlobHeaderBytes == 64 && kHitMaterialBytes == 32 && kHitTextureBytes == 32 && kHitRunBytes == 32 &&
                  kTexHeaderBytes == 64,
              "the sizes the files promise");
static_assert(kHitMaterialBytes % 16 == 0 && kHitRunBytes % 16 == 0 && kHitSectionAlignment == kPoolAlignment, "records are whole uint4 rows and sections align like the pool");
static_assert(kWordFlagSsr == 1 && kWordFlagAlphaCovered == 2 && kWordFlagVehicle == 4 && (7u << kWordFlagsShift) == 0xE000u, "the G-buffer's three flags in bits 13..15");
static_assert(kWordClassShift + 5 == kWordFlagsShift && kWordFlagsShift + 3 == 16 && kWordLitShift + 8 == kWordMetalShift && kWordMetalShift + 8 == 32,
              "the word's fields tile 32 bits: smoothness, class, flags, lit, metal");
static_assert(PackMaterialWord(255, 31, 7, 255, 255) == 0xFFFFFFFFu && PackMaterialWord(0, 0, 0, 0, 0) == 0u, "the word's fields do not overlap");
static_assert(WordClass(PackMaterialWord(1, 13, 5, 2, 3)) == 13 && WordFlags(PackMaterialWord(1, 13, 5, 2, 3)) == 5 && WordMetal(PackMaterialWord(1, 13, 5, 2, 3)) == 3, "the word's accessors");
static_assert(static_cast<uint32_t>(HitTextureFormat::kCount) == 4, "texture formats: BC1, BC2, BC3, RGBA8");
static_assert(kInstanceIdFlag == 0x800000u && kInstanceRowMask == 0x7FFFFFu && SectorRowOfInstance(kInstanceIdFlag | (438u << 1) | 1u) == 877u, "the sector slot of an instance id");
static_assert(kMaxTextures < kNoTexture && kMaxRunsPerBlob < 0xFFFFu, "the 16 bit indices keep their 'none' value free");
static_assert(TriangleOfHit(0, 7, 100) == 7 && TriangleOfHit(1, 7, 100) == 107 && TriangleOfHit(0, 7, 0) == 7, "the triangle of a committed hit");

}  // namespace larecomp::citypack

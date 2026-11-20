#pragma once
// ===========================================================================
// MCLA GPU renderer -- a car's own model data: the walk from a vehicle down to its geometries (the rules that need no renderer)
// ===========================================================================
// perf/RT-MODEL-SURVEY.md (agent RT-MODEL-SURVEY, 2026-11-19). The ray traced car scene is built from the DRAWS the game makes, so a car the camera has
// not drawn is not in it. Every car part is a RAGE drawable whose vertex and index buffers sit in guest memory (the mod loader, mc_engine/modloader/rsc5.cpp,
// parses the same bytes on disk), so the scene can be built from the car's own model data. This header is the PARSING of that data, as pure functions
// over a memory reader, so that car_model_walk_rules_test.cpp feeds it made-up bytes. The reading of real guest memory (the adapter, the hook, the log and
// the comparison with the frame's draws) is car_model_walk.h / .cpp.
//
// THE CHAIN (offsets are decimal; every word is big endian; "asm" = read from the recompiled functions sub_82371A38 / sub_82371678 / sub_82371868 /
// sub_8235D500 / sub_823580D0 / sub_82325040 in the survey session and again here, [I] = inferred):
//   vehicle V (the mcCarModel the renderer's slot-7 client draws; Vehicle::Draw = 0x82360518)
//     V+20  -> E, the car's entity: E+80 -> I (model info), E+132 -> C (customisation), E+144 / E+208 the world Matrix44 (update / render copy, row 3 = position)
//     I+32  -> M, the MODEL SET (u32 words): M[3*(10p+v)+lod] = the entry of part p (0..54), variant v (0..9), lod (0..2); M[1664 + that] = its load state
//              (3 = loaded); M[3314 + 10p+v] = the variant's count (> 0 = the variant exists); the body: M[1650 + lod], its state M[1659]; the byte M+6652
//              (0: the car has no LOD 0: the game clamps the lod it asks for to 1..3)
//     entry: +0 -> drawable D, +8 flags (the entry is placed when (flags & 0x30000000) == 0x30000000); the game's accessor returns D only when the entry
//              is non-null, the variant's count > 0, the state is 3 and the entry is placed
//     D+8   -> the LOD object L the game draws (D+12 / D+16 are the other two slots of a body resource; the per-LOD drawables of the parts use +8)
//     L     -> +0 array of model pointers, +4 u16 model count, +8 array of u32 (one per model: the model's BONE index; the matrix is the part's matrix array
//              [bone * 64]), +16 the bucket mask
//     model -> +4 array of geometry pointers, +8 u16 geometry count, +16 the shader index array
//     geometry G -> +12 vertex buffer VB, +28 index buffer IB, +44 index count, +48 primitive count, +52 u16 vertex count, +54 u16 draw mode (3 = triangle
//              list), +56 pointer to a u16 bone palette, +62 u16 palette entry count (a skinned geometry)
//     VB -> +4 u16 vertex count, +8 / +24 the vertex data (two copies of the pointer), +12 stride, +16 declaration, +28 GPU header
//     IB -> +4 index count, +8 the index data, +12 GPU header
//     GPU header (8 dwords, a D3D resource header): word 0 (the index buffer's: bit 31 32-bit indices, bits 30..29 the endian), word 6 the address (the vertex
//              flavour has the endian field 3 in its low bits: the Xenos vertex fetch constant's first dword), word 7 the size (the fetch constant's second
//              dword, size in bytes in bits 2..25; for indices a byte count). THE TWO DWORDS OF A VERTEX BUFFER'S HEADER ARE THE VERTEX FETCH CONSTANT A DRAW
//              READS, and the device's index buffer object of a draw IS the IB's GPU header: that is what the comparison with the captured draws uses.
//     declaration -> +0 the mask of present semantics (bit s), +8 / +12 the high / low word of 16 four-bit component types (semantic s at bits 4s..4s+3);
//              the elements are packed in semantic order from offset 0, so the POSITION (semantic 0) is at offset 0
//   vehicle V: +116 u16 visible in the main view (written at phase 0x200), +120 u16 enabled, +5996 forced lod, +6004 the body's lod, +6008 the parts' lod,
//              +6028 the draw-order slot, +6048 / +6052 floats the lod distances, +6732 / +6736 the body's item (the item's +20 = its matrix array, 64 bytes a
//              matrix), +6960 + 4p the part's item (asm: vehicle[1740 + p]; the item's +20 = the part's matrix array), +1600 + 64w the wheels' matrices
//   customisation C: +1632 + 48r the paint of region r (0..5): +0 colour A (four floats), +16 colour B, +32 paint type id, +36 gradient mode, +40 byte
//              "default paint" (asm sub_823580D0); +2144 + p the variant of part p the car wears; +2203 the byte that picks the part table
//   tables in the game image: the part's paint region, u32 at 0x82050FD0 + 4p; the parts the main pass draws, bytes at 0x82050D9C (normal) / 0x82050DDC
//              (when C+2203 != 0)
//
// WHAT IS IN USE. The game draws the body at M[1650 + lod of V+6004] and the part p at the variant C+2144+p and the lod of V+6008 (asm sub_8235D500), skipping
// a part whose item (V+6960+4p) is null or whose byte in the draw table is 0. A geometry is IN USE when its entry is that one. The comparison counts the
// in-use geometries first (a car the camera sees draws exactly those) and all the walked ones next.
//
// This header reads guest memory only through the template parameter `Mem` (bounds-checked by the adapter) and never writes it:
//   bool Word(uint32_t va, uint32_t* out) const;   // a big-endian u32; false when the bytes are not readable
//   bool Half(uint32_t va, uint32_t* out) const;   // a big-endian u16
//   bool Byte(uint32_t va, uint32_t* out) const;
//   const uint8_t* Window(uint32_t va, uint32_t bytes) const;  // the raw bytes of a readable range, else null
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "car_dump_rules.h"
#include "temporal_math.h"

namespace mcla::gpu::car_model_walk {

// ---------------------------------------------------------------------------
// the layout (see the header comment)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kPartSlots = 55;
inline constexpr uint32_t kVariants = 10;
inline constexpr uint32_t kLods = 3;
inline constexpr uint32_t kPaintRegions = 6;
inline constexpr uint32_t kBodyPart = kPartSlots;  // Geometry::part of the body's geometries
inline constexpr uint32_t kNoBone = 0xFFFFFFFFu;

inline constexpr uint32_t kVehicleEntity = 20;
inline constexpr uint32_t kVehicleVisible = 116;
inline constexpr uint32_t kVehicleEnabled = 120;
inline constexpr uint32_t kVehicleWheelMatrices = 1600;
inline constexpr uint32_t kVehicleForcedLod = 5996;
inline constexpr uint32_t kVehicleBodyLod = 6004;
inline constexpr uint32_t kVehiclePartLod = 6008;
inline constexpr uint32_t kVehicleDrawSlot = 6028;
inline constexpr uint32_t kVehicleLodDistances = 6048;
inline constexpr uint32_t kVehicleBodyItemGate = 6732;
inline constexpr uint32_t kVehicleBodyItem = 6736;
inline constexpr uint32_t kVehiclePartItems = 6960;
// The matrices the part's draw sets as gWorld, one pointer a part (asm sub_8235D500 -> sub_823252B8 -> sub_82325040: vehicle[1435 + p], the body's vehicle[1434]); an
// array of 64-byte matrices like the item's, indexed by the same bone. Which of the two arrays (or what of both) a draw's model-to-world is made of is not
// certain from the asm alone: car_model_rt_rules.h checks the hypotheses against the draws of the frame (Pose).
inline constexpr uint32_t kVehiclePartWorlds = 5740;
inline constexpr uint32_t kVehicleBodyWorlds = 5736;
// The bytes the draw's loop reads (asm sub_8235D500): the ghost flag, the two that cut the loop to the first 44 parts, the two that say a bumper is on or off.
inline constexpr uint32_t kVehicleGhost = 123;
inline constexpr uint32_t kVehicleLoop44A = 6477;
inline constexpr uint32_t kVehicleLoop44B = 6478;
inline constexpr uint32_t kVehicleBumperFront = 6482;
inline constexpr uint32_t kVehicleBumperRear = 6483;
inline constexpr uint32_t kVehicleCarObject = 1020;    // -> an object whose byte +2154 lets the sub-bumpers be drawn
inline constexpr uint32_t kCarObjectSubBumpers = 2154;
inline constexpr uint32_t kCustomIntercooler = 2205;   // C + 2144 + 61: the intercooler is drawn only when set
inline constexpr uint32_t kLastLodPart = 44;           // parts 0..43 draw at the vehicle's lod (+6004), 44 and up at +6008
inline constexpr uint32_t kPartsShort = 44;            // the loop's count when the two bytes say so (else kPartSlots)
inline constexpr uint32_t kItemMatrices = 20;
inline constexpr uint32_t kWheelCount = 4;

inline constexpr uint32_t kEntityModelInfo = 80;
inline constexpr uint32_t kEntityCustom = 132;
inline constexpr uint32_t kEntityState = 128;       // -> an object whose +656 == 3 when the car is loaded and active (Vehicle::Draw's gate)
inline constexpr uint32_t kStateWord = 656;
inline constexpr uint32_t kEntityMatrix = 144;
inline constexpr uint32_t kEntityRenderMatrix = 208;
inline constexpr uint32_t kMatrixTranslation = 48;

inline constexpr uint32_t kInfoModelSet = 32;
inline constexpr uint32_t kInfoHasLod0 = 40;        // byte

// The model set, in u32 words.
inline constexpr uint32_t kSetEntries = 0;          // 3 * (10p + v) + lod
inline constexpr uint32_t kSetEntryCount = 1650;    // the entries 0..1649; then the body's three lods and the two packs
inline constexpr uint32_t kSetBody = 1650;          // + lod
inline constexpr uint32_t kSetBodyState = 1659;
inline constexpr uint32_t kSetStates = 1664;        // + the entry's index
inline constexpr uint32_t kSetCounts = 3314;        // + 10p + v
inline constexpr uint32_t kSetHasLod0Byte = 6652;   // a byte offset
inline constexpr uint32_t kSetBytes = 6656;
inline constexpr uint32_t kStateLoaded = 3;
inline constexpr uint32_t kPlacedMask = 0x30000000u;

inline constexpr uint32_t kCustomPaint = 1632;      // + 48 * region
inline constexpr uint32_t kPaintBytes = 48;
inline constexpr uint32_t kCustomVariants = 2144;   // + part
inline constexpr uint32_t kCustomTableMode = 2203;  // byte
inline constexpr uint32_t kCustomWindowBytes = 2208 - kCustomPaint;

// The tables in the game image (retail build).
inline constexpr uint32_t kRegionTableAddress = 0x82050FD0u;   // u32 per part
inline constexpr uint32_t kDrawTableAddress = 0x82050D9Cu;     // byte per part
inline constexpr uint32_t kDrawTableAltAddress = 0x82050DDCu;  // byte per part, used when C+2203 != 0

// Drawable, LOD, model, geometry, buffers.
inline constexpr uint32_t kEntryDrawable = 0;
inline constexpr uint32_t kEntryFlags = 8;
inline constexpr uint32_t kDrawableLod = 8;
inline constexpr uint32_t kLodModels = 0;
inline constexpr uint32_t kLodModelCount = 4;
inline constexpr uint32_t kLodBones = 8;
inline constexpr uint32_t kLodBucketMask = 16;
inline constexpr uint32_t kModelGeometries = 4;
inline constexpr uint32_t kModelGeometryCount = 8;
inline constexpr uint32_t kGeomVertexBuffer = 12;
inline constexpr uint32_t kGeomIndexBuffer = 28;
inline constexpr uint32_t kGeomIndexCount = 44;
inline constexpr uint32_t kGeomPrimitiveCount = 48;
inline constexpr uint32_t kGeomVertexCount = 52;
inline constexpr uint32_t kGeomDrawMode = 54;
inline constexpr uint32_t kGeomPalette = 56;
inline constexpr uint32_t kGeomPaletteCount = 62;
inline constexpr uint32_t kGeomBytes = 64;
inline constexpr uint32_t kVbCount = 4;
inline constexpr uint32_t kVbData = 24;
inline constexpr uint32_t kVbDataCopy = 8;
inline constexpr uint32_t kVbStride = 12;
inline constexpr uint32_t kVbDeclaration = 16;
inline constexpr uint32_t kVbHeader = 28;
inline constexpr uint32_t kIbCount = 4;
inline constexpr uint32_t kIbData = 8;
inline constexpr uint32_t kIbHeader = 12;
inline constexpr uint32_t kHeaderWord0 = 0;
inline constexpr uint32_t kHeaderAddress = 24;  // word 6
inline constexpr uint32_t kHeaderSize = 28;     // word 7
inline constexpr uint32_t kHeaderBytes = 32;

inline constexpr uint32_t kPrimitiveTriangleList = 3;
inline constexpr uint32_t kFetchSizeMask = 0x03FFFFFCu;  // the size in bytes in a vertex fetch constant's second dword
inline constexpr uint32_t kFetchTypeMask = 3u;           // low bits of its first dword (3 = a vertex fetch); the rest is the address

// Bounds of one walk (a made-up or corrupt object must not run away).
inline constexpr uint32_t kMaxModels = 512;
inline constexpr uint32_t kMaxGeometriesPerModel = 1024;
inline constexpr uint32_t kMaxGeometries = 6000;
inline constexpr uint32_t kMaxPalette = 256;
inline constexpr uint32_t kMaxMatrixBones = 1024;
inline constexpr uint32_t kMaxBufferBytes = 64u * 1024u * 1024u;
inline constexpr uint64_t kValidateBudgetBytes = 24ull * 1024u * 1024u;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

inline uint32_t Be32(const uint8_t* p) { return car_dump::LoadBe32(p); }
inline uint32_t Be16(const uint8_t* p) { return car_dump::LoadBe16(p); }
inline float BeFloat(const uint8_t* p) {
  const uint32_t bits = Be32(p);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// A value that can be a guest object address (the first pages are never objects).
inline bool Plausible(uint32_t address) { return address >= 0x10000u; }

// The guest's conversion of a windowed address to a physical one (GuestMemory::ToPhysical, the draw builder sub_8241D620): the mask plus one page for the
// 0xE0000000 window. Copied here so that this header stays free of the renderer.
inline constexpr uint32_t PhysicalOf(uint32_t address) { return (address & 0x1FFFFFFFu) + (((address >> 20) + 0x200u) & 0x1000u); }

// The index of the entry of part p, variant v, lod in the model set's table.
inline constexpr uint32_t FlatIndex(uint32_t part, uint32_t variant, uint32_t lod) { return 3u * (10u * part + variant) + lod; }

// The lod the game asks the model set for (asm sub_82371A38 / sub_82371868): a car without a LOD 0 (the byte M+6652 is 0) has it clamped to 1..3. A negative
// lod is no lod (-1: returned as is).
inline int32_t GameLod(bool has_lod0, int32_t lod) {
  if (lod < 0) {
    return -1;
  }
  if (has_lod0) {
    return lod;
  }
  return std::clamp<int32_t>(lod, 1, 3);
}

// ---------------------------------------------------------------------------
// the game's lod rule (asm sub_8235F738, read again for phase 2 and checked against the first check run's log)
// ---------------------------------------------------------------------------
// What the game does for EVERY vehicle every frame, drawn or not (phase 0x200), before it tests visibility:
//   lod = the number of the two thresholds (floats at vehicle+6048 and +6052, 15 m and 100 m in the retail game) that are <= the distance from the main
//         viewport's eye to the entity's matrix row 3 (0, 1 or 2);
//   a car whose model info has no LOD 0 (info byte +40 is 0) is raised from 0 to 1; the lod is at most 2;
//   a global object [0x8286D814] whose vtable slot 26 says so, or the byte [[0x8286E7FC] + 157], caps it at 1 (the cap is not read here: the walk takes the
//   game's own answer from vehicle+6004);
//   +5996 (not -1) overrides all of it. +6008, the lod of the second pass, is +6000 when that is not -1, else the same lod.
// The draw (sub_8235D500) asks the model set for parts 0..43 at the vehicle's lod and for parts 44.. (the interior, the seats, the steering wheel) at +6008.
// FIRST CHECK RUN (ts-mw30-rt-offscreen, 999 results): in every walk the geometry the frame's draws were of sat in the table's lod slot the rule picks
// (lodmatch equal to lods in all of them): the player at slot 0, the racers (no LOD 0) at slot 1 from 3 m to 56 m, the cars 525 m away at slot 2. The one car at
// 73 percent was not a lod mistake: for 315 of its walks (+116 = 0) the frame drew 30 fewer of its in-use geometries than in its 72 walks with +116 = 1, and all of
// the missing ones were of parts the draw's loop leaves out in some states (the two bytes below cut it to 44 parts). PartDrawn models those.
struct LodRuleInput {
  double distance = 0.0;           // metres, eye to the vehicle
  float threshold[2] = {15.0f, 100.0f};
  bool info_has_lod0 = true;       // model info byte +40
  int32_t forced = -1;             // vehicle+5996
  bool capped = false;             // the global cap (read by the caller, or false)
};

inline int32_t GameBodyLod(const LodRuleInput& in) {
  if (in.forced != -1) {
    return in.forced;
  }
  int32_t lod = 0;
  for (int i = 0; i < 2; ++i) {
    if (in.distance >= double(in.threshold[i])) {
      lod = i + 1;
    } else {
      break;
    }
  }
  if (!in.info_has_lod0 && lod == 0) {
    lod = 1;
  }
  lod = std::min(lod, 2);
  if (in.capped && lod > 1) {
    lod = 1;
  }
  return lod;
}

// The lod the draw asks for part `part`: the vehicle's for the first 44, the second pass's for the rest.
inline int32_t PartLodOf(uint32_t part, int32_t body_lod, int32_t part_lod) { return part >= kLastLodPart ? part_lod : body_lod; }

// The lods to try for a wanted lod, in order: the wanted one, each coarser one, then each finer one (a part may have no entry at some lod: the shipped hood has
// lods 0 and 1 and no 2).
inline std::array<int32_t, kLods> LodOrder(int32_t wanted) {
  wanted = std::clamp<int32_t>(wanted, 0, int32_t(kLods) - 1);
  std::array<int32_t, kLods> order{};
  uint32_t n = 0;
  for (int32_t l = wanted; l < int32_t(kLods); ++l) {
    order[n++] = l;
  }
  for (int32_t l = wanted - 1; l >= 0; --l) {
    order[n++] = l;
  }
  return order;
}

// The bytes of a vehicle the draw's per-part decisions read (all zero where a read failed: the draw would see the same for a null object).
struct DrawFlags {
  bool ghost = false;           // V+123
  bool loop44_a = false;        // V+6477
  bool loop44_b = false;        // V+6478
  bool bumper_front_off = false;  // V+6482 != 0: the front bumper is not drawn, the front sub-bumper is
  bool bumper_rear_off = false;   // V+6483 != 0
  bool sub_bumpers = false;     // [V+1020]+2154 != 0: both sub-bumpers are drawn whatever the bumpers say
  bool intercooler = false;     // C+2205 != 0
};

// How many parts the main pass's loop covers: 44 when the second byte is set and the car is no ghost, or when the first is set; else all 55.
inline uint32_t LoopParts(const DrawFlags& f) { return ((f.loop44_b && !f.ghost) || f.loop44_a) ? kPartsShort : kPartSlots; }

// Whether the main pass draws part `part` (its table byte `table_on` says yes and its item exists, which the caller checks): the per-part tests of
// sub_8235D500 that depend on the vehicle: the intercooler needs C+2205, a bumper is drawn unless its byte says it is off, a sub-bumper when the car object
// allows both or its bumper is off, and nothing past the loop's count. The convertible's top and its parts (0, 1, 22) have tests of their own that are not
// modelled here: they stay as the table says.
inline bool PartDrawn(uint32_t part, const DrawFlags& f) {
  if (part >= LoopParts(f)) {
    return false;
  }
  switch (part) {
    case 5: return f.intercooler;
    case 6: return !f.bumper_front_off;
    case 3: return !f.bumper_rear_off;
    case 36: return f.sub_bumpers || f.bumper_front_off;
    case 37: return f.sub_bumpers || f.bumper_rear_off;
    default: return true;
  }
}

// A big-endian float vector from `p`.
inline void LoadFloats(const uint8_t* p, uint32_t n, float* out) {
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = BeFloat(p + 4u * i);
  }
}

// ---------------------------------------------------------------------------
// the vertex declaration
// ---------------------------------------------------------------------------

// The bytes of a component type (the loader's table, rsc5.cpp ComponentSize: only the ones the shipped layouts use; anything else is unknown, and the offsets
// after it are not computed).
inline uint32_t ComponentBytes(uint32_t type) {
  switch (type) {
    case 1: return 4;    // Half2 (the UV)
    case 6: return 12;   // Float3 (the position)
    case 9: return 4;    // D3DCOLOR (weights, indices, colour)
    case 10: return 4;   // Dec3N (normal, tangent)
    default: return 0;
  }
}

inline constexpr uint32_t kSemPosition = 0;
inline constexpr uint32_t kSemBlendWeights = 1;
inline constexpr uint32_t kSemBlendIndices = 2;
inline constexpr uint32_t kSemNormal = 3;
inline constexpr uint32_t kSemColour = 4;
inline constexpr uint32_t kSemTexcoord0 = 6;
inline constexpr uint32_t kSemTexcoord1 = 7;
inline constexpr uint32_t kSemTangent = 14;
inline constexpr uint32_t kComponentFloat3 = 6;

struct Declaration {
  bool read = false;        // the three words were readable
  uint32_t fvf = 0;         // the semantics present, one bit each
  uint32_t types_hi = 0;
  uint32_t types_lo = 0;
  int32_t offset[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  // byte offset of each present semantic, -1: absent or unknown
  uint32_t bytes = 0;       // the sum of the sizes counted
  bool complete = false;    // every present semantic has a known size (then `bytes` is the vertex size the elements account for)
  uint32_t position_type = 0;
  bool Has(uint32_t semantic) const { return semantic < 16 && ((fvf >> semantic) & 1u) != 0; }
  bool PositionIsFloat3() const { return Has(kSemPosition) && position_type == kComponentFloat3; }
};

inline Declaration DecodeDeclaration(uint32_t fvf, uint32_t hi, uint32_t lo) {
  Declaration d;
  d.read = true;
  d.fvf = fvf & 0xFFFFu;
  d.types_hi = hi;
  d.types_lo = lo;
  const uint64_t types = (uint64_t(hi) << 32) | lo;
  uint32_t at = 0;
  d.complete = true;
  for (uint32_t s = 0; s < 16; ++s) {
    if (((d.fvf >> s) & 1u) == 0) {
      continue;
    }
    const uint32_t type = uint32_t((types >> (4u * s)) & 0xFu);
    if (s == kSemPosition) {
      d.position_type = type;
    }
    const uint32_t size = ComponentBytes(type);
    if (size == 0) {
      d.complete = false;  // the rest cannot be placed; this one starts at `at` all the same
      d.offset[s] = int32_t(at);
      break;
    }
    d.offset[s] = int32_t(at);
    at += size;
    d.bytes = at;
  }
  return d;
}

// ---------------------------------------------------------------------------
// what a walk finds
// ---------------------------------------------------------------------------

struct Geometry {
  uint32_t address = 0;            // the geometry object
  uint16_t part = 0;               // 0..54, kBodyPart for the body
  uint8_t variant = 0;
  uint8_t lod = 0;                 // the table's lod slot 0..2
  uint16_t model = 0;
  uint16_t slot = 0;               // in its model
  uint32_t entry_index = 0;        // the PartEntry (in CarWalk::parts) whose drawable it belongs to: its matrices place it
  bool in_use = false;
  bool read = false;               // the geometry object was readable
  uint32_t vertex_count = 0;       // G+52
  uint32_t index_count = 0;        // G+44
  uint32_t primitive_count = 0;    // G+48
  uint32_t draw_mode = 0;          // G+54 (3 = triangle list)
  // the vertex buffer
  bool vb_ok = false;              // the object and its header were readable
  uint32_t vb_object = 0;
  uint32_t vb_count = 0;           // VB+4
  uint32_t vb_stride = 0;          // VB+12 bytes
  uint32_t vb_data = 0;            // VB+24
  uint32_t vb_data_copy = 0;       // VB+8
  uint32_t vb_decl = 0;            // VB+16
  uint32_t vb_header = 0;          // VB+28
  uint32_t vb_w0 = 0, vb_w6 = 0, vb_w7 = 0;  // the GPU header's words 0, 6, 7: w6 / w7 = the vertex fetch constant
  Declaration decl;
  // the index buffer
  bool ib_ok = false;
  uint32_t ib_object = 0;
  uint32_t ib_count = 0;           // IB+4
  uint32_t ib_data = 0;            // IB+8
  uint32_t ib_header = 0;          // IB+12: the object a draw's device index buffer points at
  uint32_t ib_w0 = 0, ib_w6 = 0, ib_w7 = 0;  // w0: bit 31 32-bit indices, bits 30..29 endian; w6 the address; w7 the byte count
  // bones
  uint32_t bone = kNoBone;         // the model's bone index (L+8[model])
  uint32_t palette = 0;            // G+56
  uint32_t palette_count = 0;      // G+62
  uint32_t palette_first = 0;      // into CarWalk::palette_entries
  uint32_t palette_read = 0;       // entries read there
  // what the data says (in-use geometries only, within the walk's byte budget)
  bool validated = false;
  bool vb_readable = false;        // the vertex data range is readable
  bool ib_readable = false;
  bool positions_finite = false;   // the first and the last vertex's position are finite floats
  uint32_t max_index = 0;
  uint32_t bad_indices = 0;        // indices >= the vertex count
  float first_position[3] = {};

  bool Index32() const { return (ib_w0 & 0x80000000u) != 0; }
  uint32_t IndexEndian() const { return (ib_w0 >> 29) & 3u; }
  uint32_t IndexBytes() const { return Index32() ? 4u : 2u; }
  // The data pointers the software side holds name the memory the header's address names (the loader's long-standing assumption).
  bool HeaderMatchesData() const { return vb_data != 0 && PhysicalOf(vb_data) == PhysicalOf(vb_w6 & ~kFetchTypeMask); }
};

struct PartEntry {
  uint16_t part = 0;
  uint8_t variant = 0;
  uint8_t lod = 0;
  uint32_t entry = 0;
  uint32_t drawable = 0;
  uint32_t lod_object = 0;
  uint32_t flags = 0;              // entry+8
  uint32_t state = 0;              // M[1664 + index]
  int32_t count = 0;               // M[3314 + 10p+v]
  uint32_t item = 0;               // V+6960+4p (the body: V+6736)
  uint32_t matrices = 0;           // item+20
  uint32_t world_matrices = 0;     // V+5740+4p (the body: V+5736): the array the draw sets gWorld from
  uint32_t region = 0;             // the paint region (kPaintRegions: none known)
  uint32_t models = 0;
  uint32_t bucket_mask = 0;
  bool loaded = false;             // state == 3
  bool placed = false;
  bool valid = false;              // the game's accessor would hand out the drawable
  bool in_use = false;
  bool shared_drawable = false;    // the drawable was already walked for another entry
  bool entry_unreadable = false;
  uint32_t first_geometry = 0;
  uint32_t geometries = 0;
  // The part's matrix source: the matrix array's entry for the lowest bone index among its geometries (item+20, 64 bytes a matrix, row 3 = the translation).
  // What it holds for a car nobody draws is the survey's CHECK 3: the walk reads it and the log says where it is next to the vehicle.
  uint32_t matrix_bone = kNoBone;
  bool matrix_read = false;
  float matrix_position[3] = {};
};

struct Paint {
  bool read = false;
  float a[4] = {};
  float b[4] = {};
  uint32_t type = 0;
  uint32_t mode = 0;
  uint32_t use_default = 0;
};

struct Tally {
  uint32_t geometries = 0;
  uint32_t vertices = 0;
  uint32_t triangles = 0;
};

struct CarWalk {
  uint32_t vehicle = 0;
  bool ok = false;                 // the chain down to the model set was readable
  std::string why;                 // when it was not
  uint32_t entity = 0, info = 0, custom = 0, model_set = 0;
  // the vehicle's own state
  uint32_t visible = 0, enabled = 0;
  int32_t body_lod = -1, part_lod = -1, forced_lod = -1;
  uint32_t draw_slot = 0;
  float lod_distance[2] = {};
  uint32_t entity_state_object = 0;
  uint32_t entity_state = 0;       // [E+128]+656 (3 = loaded and active)
  bool has_lod0 = false;           // M+6652 != 0
  bool info_lod0 = false;          // I+40 != 0
  uint32_t body_state = 0;         // M[1659]
  uint32_t body_item = 0, body_item_gate = 0;
  float position[3] = {};          // E+144 row 3
  float render_position[3] = {};   // E+208 row 3
  bool position_read = false;
  float wheel_position[kWheelCount][3] = {};
  bool wheels_read = false;
  Paint paint[kPaintRegions];
  DrawFlags flags;                 // the bytes the draw's per-part tests read
  uint32_t loop_parts = kPartSlots;
  bool has_entity_matrix = false;  // the entity's world matrices (update copy at E+144, render copy at E+208), whole
  temporal::Mat4 entity_matrix;
  temporal::Mat4 entity_render_matrix;
  uint8_t variant_of_part[kPartSlots] = {};
  bool variants_read = false;
  uint32_t table_mode = 0;         // C+2203
  // the table, and what was walked
  uint32_t entries_present = 0;    // non-null entries of the table
  uint32_t entries_loaded = 0;     // ... whose state is 3
  uint32_t entries_valid = 0;      // ... that the game's accessor would hand out
  uint32_t entries_unreadable = 0;
  uint32_t drawables_shared = 0;
  bool truncated = false;          // a bound of the walk was hit
  std::vector<PartEntry> parts;    // the body's entries too (part kBodyPart)
  std::vector<Geometry> geometries;
  std::vector<uint16_t> palette_entries;  // the bone palettes of the skinned geometries, one after the other
  Tally all;                       // every geometry walked
  Tally in_use;                    // the geometries the game draws now
  Tally lod_tally[kLods];          // every geometry walked, by lod slot
  uint32_t skinned = 0;            // geometries with a bone palette
  uint32_t resident_in_use = 0;    // in-use geometries whose buffers were validated: data readable, indices in range, positions finite
  uint32_t validated_in_use = 0;
  uint64_t validated_bytes = 0;
};

// What the adapter reads once from the game image and hands the walk.
struct Options {
  bool tables_read = false;
  uint32_t region_of_part[kPartSlots] = {};
  uint8_t draws_part[kPartSlots] = {};      // the normal table
  uint8_t draws_part_alt[kPartSlots] = {};  // the table C+2203 != 0 picks
  bool validate = true;                     // read the in-use geometries' data: the indices' range and the first and last position
  uint64_t validate_budget = kValidateBudgetBytes;
  // PRODUCTION (the ray traced scene's cars from model data): only the entry of each part that the scene would draw is walked, not every variant and lod, and the
  // data is not validated. The lod asked for is the game's, raised to `min_lod` (the reflection does not need a car's finest lod), and a part whose entry at that
  // lod is not valid takes the next coarser lod that is, then a finer one.
  bool production = false;
  int32_t min_lod = 1;
  // The phase 1 rule: every part at the second pass's lod (+6008). The retail loop asks the parts under 44 for the vehicle's lod (+6004), so the walker's default
  // is that; this switch is for the fixtures that were made with the other rule.
  bool all_parts_at_part_lod = false;
};

// ---------------------------------------------------------------------------
// positions and indices
// ---------------------------------------------------------------------------

// A Float3 position (component type 6) at `vertex + offset`; false for any other type or a non-finite value.
inline bool ReadFloat3(const uint8_t* vertex, float out[3]) {
  LoadFloats(vertex, 3, out);
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// The largest index among `count` indices at `raw` (16 or 32 bit, swapped by `endian`) and how many of them are >= `vertex_count`; `count` indices only.
inline void ScanIndices(const uint8_t* raw, uint32_t count, bool index32, uint32_t endian, uint32_t vertex_count, uint32_t* max_index, uint32_t* bad) {
  *max_index = 0;
  *bad = 0;
  const uint32_t unit = index32 ? 4u : 2u;
  uint8_t chunk[4096];
  uint32_t done = 0;
  while (done < count) {
    const uint32_t n = std::min<uint32_t>(count - done, uint32_t(sizeof(chunk)) / unit);
    car_dump::SwapEndian(chunk, raw + size_t(done) * unit, n * unit, endian);
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t v;
      if (index32) {
        std::memcpy(&v, chunk + size_t(i) * 4, 4);
      } else {
        uint16_t h;
        std::memcpy(&h, chunk + size_t(i) * 2, 2);
        v = h;
      }
      *max_index = std::max(*max_index, v);
      if (v >= vertex_count) {
        ++*bad;
      }
    }
    done += n;
  }
}

// ---------------------------------------------------------------------------
// the walk
// ---------------------------------------------------------------------------

// A guest Matrix44 (16 big-endian floats, row vectors: rows right, up, back and position) at `address`, as a double matrix with the fourth column made
// (0, 0, 0, 1) (the game leaves NaN there and the palette code never reads it). False when it is not readable or a number it uses is not finite.
template <class Mem>
bool ReadMatrix(const Mem& mem, uint32_t address, temporal::Mat4* out) {
  const uint8_t* p = Plausible(address) ? mem.Window(address, 64) : nullptr;
  if (!p) {
    return false;
  }
  *out = temporal::Identity();
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 3; ++c) {
      const float v = BeFloat(p + 16u * r + 4u * c);
      if (!std::isfinite(v)) {
        return false;
      }
      out->m[r][c] = double(v);
    }
  }
  return true;
}

namespace detail {

template <class Mem>
bool ReadHeader(const Mem& mem, uint32_t header, uint32_t* w0, uint32_t* w6, uint32_t* w7) {
  const uint8_t* h = Plausible(header) ? mem.Window(header, kHeaderBytes) : nullptr;
  if (!h) {
    return false;
  }
  *w0 = Be32(h + kHeaderWord0);
  *w6 = Be32(h + kHeaderAddress);
  *w7 = Be32(h + kHeaderSize);
  return true;
}

}  // namespace detail

// Reads the geometry at `address` and its buffers into `out` (part, variant, lod, model, slot are the caller's). False when the geometry object itself is not
// readable; a buffer that is not has vb_ok / ib_ok false.
template <class Mem>
bool ReadGeometry(const Mem& mem, uint32_t address, Geometry* out) {
  out->address = address;
  const uint8_t* g = Plausible(address) ? mem.Window(address, kGeomBytes) : nullptr;
  if (!g) {
    return false;
  }
  out->read = true;
  out->vb_object = Be32(g + kGeomVertexBuffer);
  out->ib_object = Be32(g + kGeomIndexBuffer);
  out->index_count = Be32(g + kGeomIndexCount);
  out->primitive_count = Be32(g + kGeomPrimitiveCount);
  out->vertex_count = Be16(g + kGeomVertexCount);
  out->draw_mode = Be16(g + kGeomDrawMode);
  out->palette = Be32(g + kGeomPalette);
  out->palette_count = Be16(g + kGeomPaletteCount);

  if (const uint8_t* vb = Plausible(out->vb_object) ? mem.Window(out->vb_object, 32) : nullptr) {
    out->vb_count = Be16(vb + kVbCount);
    out->vb_data_copy = Be32(vb + kVbDataCopy);
    out->vb_stride = Be32(vb + kVbStride);
    out->vb_decl = Be32(vb + kVbDeclaration);
    out->vb_data = Be32(vb + kVbData);
    out->vb_header = Be32(vb + kVbHeader);
    out->vb_ok = detail::ReadHeader(mem, out->vb_header, &out->vb_w0, &out->vb_w6, &out->vb_w7);
    if (const uint8_t* d = Plausible(out->vb_decl) ? mem.Window(out->vb_decl, 16) : nullptr) {
      out->decl = DecodeDeclaration(Be32(d), Be32(d + 8), Be32(d + 12));
    }
  }
  if (const uint8_t* ib = Plausible(out->ib_object) ? mem.Window(out->ib_object, 16) : nullptr) {
    out->ib_count = Be32(ib + kIbCount);
    out->ib_data = Be32(ib + kIbData);
    out->ib_header = Be32(ib + kIbHeader);
    out->ib_ok = detail::ReadHeader(mem, out->ib_header, &out->ib_w0, &out->ib_w6, &out->ib_w7);
  }
  return true;
}

// The bone palette of a skinned geometry: its entries (u16 each) appended to `pool`. Returns how many were read (0: none or not readable).
template <class Mem>
uint32_t ReadPalette(const Mem& mem, Geometry* g, std::vector<uint16_t>* pool) {
  g->palette_first = uint32_t(pool->size());
  g->palette_read = 0;
  if (g->palette_count == 0 || !Plausible(g->palette)) {
    return 0;
  }
  const uint32_t n = std::min(g->palette_count, kMaxPalette);
  const uint8_t* p = mem.Window(g->palette, 2u * n);
  if (!p) {
    return 0;
  }
  for (uint32_t i = 0; i < n; ++i) {
    pool->push_back(uint16_t(Be16(p + 2u * i)));
  }
  g->palette_read = n;
  return n;
}

// Reads what the data of an in-use geometry says: whether its vertex and index ranges are readable, the indices' range against the vertex count and the first
// and last vertex's position. `*budget` is the bytes this walk may still read (a full body is about 2.5 MB of vertices and 0.5 MB of indices).
template <class Mem>
void ValidateGeometry(const Mem& mem, Geometry* g, uint64_t* budget) {
  if (!g->vb_ok || !g->ib_ok || g->vb_stride == 0 || g->vertex_count == 0 || g->index_count == 0) {
    return;
  }
  const uint64_t vertex_bytes = uint64_t(g->vertex_count) * g->vb_stride;
  const uint64_t index_bytes = uint64_t(g->index_count) * g->IndexBytes();
  if (vertex_bytes > kMaxBufferBytes || index_bytes > kMaxBufferBytes) {
    return;
  }
  const uint8_t* vertices = mem.Window(g->vb_data, uint32_t(vertex_bytes));
  const uint8_t* indices = mem.Window(g->ib_data, uint32_t(index_bytes));
  g->vb_readable = vertices != nullptr;
  g->ib_readable = indices != nullptr;
  if (!vertices || !indices || *budget < index_bytes) {
    return;
  }
  *budget -= index_bytes;
  g->validated = true;
  ScanIndices(indices, g->index_count, g->Index32(), g->IndexEndian(), g->vertex_count, &g->max_index, &g->bad_indices);
  const uint32_t stride = g->vb_stride;
  if (g->decl.PositionIsFloat3() && stride >= 12) {
    float last[3];
    const bool a = ReadFloat3(vertices, g->first_position);
    const bool b = ReadFloat3(vertices + size_t(g->vertex_count - 1) * stride, last);
    g->positions_finite = a && b;
  }
}

namespace detail {

// The drawable's LOD object down to the geometries, appended to `out->geometries`. Returns how many geometries were added.
template <class Mem>
uint32_t WalkLod(const Mem& mem, uint32_t lod_object, const PartEntry& entry, uint32_t entry_index, CarWalk* out) {
  const uint8_t* l = Plausible(lod_object) ? mem.Window(lod_object, 20) : nullptr;
  if (!l) {
    return 0;
  }
  const uint32_t models = Be32(l + kLodModels);
  const uint32_t model_count = std::min<uint32_t>(Be16(l + kLodModelCount), kMaxModels);
  const uint32_t bones = Be32(l + kLodBones);
  const uint32_t before = uint32_t(out->geometries.size());
  const uint8_t* model_array = Plausible(models) && model_count != 0 ? mem.Window(models, 4u * model_count) : nullptr;
  const uint8_t* bone_array = Plausible(bones) && model_count != 0 ? mem.Window(bones, 4u * model_count) : nullptr;
  if (!model_array) {
    return 0;
  }
  for (uint32_t m = 0; m < model_count; ++m) {
    const uint32_t model = Be32(model_array + 4u * m);
    const uint8_t* mo = Plausible(model) ? mem.Window(model, 12) : nullptr;
    if (!mo) {
      continue;
    }
    const uint32_t geometries = Be32(mo + kModelGeometries);
    const uint32_t geometry_count = std::min<uint32_t>(Be16(mo + kModelGeometryCount), kMaxGeometriesPerModel);
    const uint8_t* geometry_array = Plausible(geometries) && geometry_count != 0 ? mem.Window(geometries, 4u * geometry_count) : nullptr;
    if (!geometry_array) {
      continue;
    }
    const uint32_t bone = bone_array ? Be32(bone_array + 4u * m) : kNoBone;
    for (uint32_t i = 0; i < geometry_count; ++i) {
      if (out->geometries.size() >= kMaxGeometries) {
        out->truncated = true;
        return uint32_t(out->geometries.size()) - before;
      }
      Geometry geometry;
      geometry.part = entry.part;
      geometry.variant = entry.variant;
      geometry.lod = entry.lod;
      geometry.model = uint16_t(m);
      geometry.slot = uint16_t(i);
      geometry.in_use = entry.in_use;
      geometry.entry_index = entry_index;
      geometry.bone = bone;
      if (!ReadGeometry(mem, Be32(geometry_array + 4u * i), &geometry)) {
        continue;
      }
      ReadPalette(mem, &geometry, &out->palette_entries);
      out->geometries.push_back(std::move(geometry));
    }
  }
  return uint32_t(out->geometries.size()) - before;
}

}  // namespace detail

// Walks the vehicle at `vehicle`: its state, its paint and variants, and every (part, variant, lod) entry of its model set that the game's own accessor would
// hand out, down to each geometry. Writes nothing in the guest. A chain that is not readable stops the walk with `out->ok` false and `out->why` saying where.
template <class Mem>
void WalkVehicle(const Mem& mem, uint32_t vehicle, const Options& options, CarWalk* out) {
  *out = CarWalk{};
  out->vehicle = vehicle;
  if (!Plausible(vehicle)) {
    out->why = "no vehicle";
    return;
  }
  uint32_t w = 0;
  if (!mem.Word(vehicle + kVehicleEntity, &out->entity) || !Plausible(out->entity)) {
    out->why = "vehicle+20 holds no entity";
    return;
  }
  const uint32_t e = out->entity;
  if (!mem.Word(e + kEntityModelInfo, &out->info) || !Plausible(out->info)) {
    out->why = "entity+80 holds no model info";
    return;
  }
  mem.Word(e + kEntityCustom, &out->custom);
  if (!mem.Word(out->info + kInfoModelSet, &out->model_set) || !Plausible(out->model_set)) {
    out->why = "model info+32 holds no model set";
    return;
  }
  const uint8_t* set = mem.Window(out->model_set, kSetBytes);
  if (!set) {
    out->why = "the model set is not readable";
    return;
  }
  out->ok = true;

  // the vehicle's own state
  if (mem.Half(vehicle + kVehicleVisible, &w)) {
    out->visible = w;
  }
  if (mem.Half(vehicle + kVehicleEnabled, &w)) {
    out->enabled = w;
  }
  if (mem.Word(vehicle + kVehicleBodyLod, &w)) {
    out->body_lod = int32_t(w);
  }
  if (mem.Word(vehicle + kVehiclePartLod, &w)) {
    out->part_lod = int32_t(w);
  }
  if (mem.Word(vehicle + kVehicleForcedLod, &w)) {
    out->forced_lod = int32_t(w);
  }
  mem.Word(vehicle + kVehicleDrawSlot, &out->draw_slot);
  if (const uint8_t* d = mem.Window(vehicle + kVehicleLodDistances, 8)) {
    LoadFloats(d, 2, out->lod_distance);
  }
  mem.Word(vehicle + kVehicleBodyItemGate, &out->body_item_gate);
  mem.Word(vehicle + kVehicleBodyItem, &out->body_item);
  if (mem.Word(e + kEntityState, &out->entity_state_object) && Plausible(out->entity_state_object)) {
    mem.Word(out->entity_state_object + kStateWord, &out->entity_state);
  }
  if (mem.Byte(out->info + kInfoHasLod0, &w)) {
    out->info_lod0 = w != 0;
  }
  out->has_lod0 = set[kSetHasLod0Byte] != 0;
  out->body_state = Be32(set + 4u * kSetBodyState);
  if (const uint8_t* m = mem.Window(e + kEntityMatrix + kMatrixTranslation, 12)) {
    LoadFloats(m, 3, out->position);
    out->position_read = true;
  }
  if (const uint8_t* m = mem.Window(e + kEntityRenderMatrix + kMatrixTranslation, 12)) {
    LoadFloats(m, 3, out->render_position);
  }
  out->wheels_read = true;
  for (uint32_t i = 0; i < kWheelCount; ++i) {
    const uint8_t* m = mem.Window(vehicle + kVehicleWheelMatrices + 64u * i + kMatrixTranslation, 12);
    if (!m) {
      out->wheels_read = false;
      break;
    }
    LoadFloats(m, 3, out->wheel_position[i]);
  }

  out->has_entity_matrix = ReadMatrix(mem, e + kEntityMatrix, &out->entity_matrix) && ReadMatrix(mem, e + kEntityRenderMatrix, &out->entity_render_matrix);
  // the bytes the draw's per-part tests read
  {
    uint32_t b = 0;
    out->flags.ghost = mem.Byte(vehicle + kVehicleGhost, &b) && b != 0;
    out->flags.loop44_a = mem.Byte(vehicle + kVehicleLoop44A, &b) && b != 0;
    out->flags.loop44_b = mem.Byte(vehicle + kVehicleLoop44B, &b) && b != 0;
    out->flags.bumper_front_off = mem.Byte(vehicle + kVehicleBumperFront, &b) && b != 0;
    out->flags.bumper_rear_off = mem.Byte(vehicle + kVehicleBumperRear, &b) && b != 0;
    uint32_t car_object = 0;
    out->flags.sub_bumpers = mem.Word(vehicle + kVehicleCarObject, &car_object) && Plausible(car_object) && mem.Byte(car_object + kCarObjectSubBumpers, &b) && b != 0;
    out->flags.intercooler = Plausible(out->custom) && mem.Byte(out->custom + kCustomVariants + 61, &b) && b != 0;
    out->loop_parts = LoopParts(out->flags);
  }

  // the paint and the variants
  if (const uint8_t* c = Plausible(out->custom) ? mem.Window(out->custom + kCustomPaint, kCustomWindowBytes) : nullptr) {
    for (uint32_t r = 0; r < kPaintRegions; ++r) {
      const uint8_t* p = c + kPaintBytes * r;
      Paint& paint = out->paint[r];
      LoadFloats(p, 4, paint.a);
      LoadFloats(p + 16, 4, paint.b);
      paint.type = Be32(p + 32);
      paint.mode = Be32(p + 36);
      paint.use_default = p[40];
      paint.read = true;
    }
    for (uint32_t p = 0; p < kPartSlots; ++p) {
      out->variant_of_part[p] = c[kCustomVariants - kCustomPaint + p];
    }
    out->variants_read = true;
    out->table_mode = c[kCustomTableMode - kCustomPaint];
  }

  // the part items (the matrix sources) and the arrays the draw sets gWorld from
  const uint8_t* items = mem.Window(vehicle + kVehiclePartItems, 4u * kPartSlots);
  const uint8_t* worlds = mem.Window(vehicle + kVehiclePartWorlds, 4u * kPartSlots);
  uint32_t body_worlds = 0;
  mem.Word(vehicle + kVehicleBodyWorlds, &body_worlds);

  // what the game draws now
  const int32_t part_lod = GameLod(out->has_lod0, out->part_lod);
  const int32_t body_lod = GameLod(out->has_lod0, out->body_lod);
  const uint8_t* draws = out->table_mode != 0 ? options.draws_part_alt : options.draws_part;
  const uint32_t body_flat = body_lod >= 0 ? kSetBody + uint32_t(body_lod) : 0xFFFFFFFFu;
  std::unordered_map<uint32_t, uint32_t> walked;  // drawable -> the entry (in out->parts) whose geometries it has

  const auto add_entry = [&](PartEntry entry, uint32_t flat_state, uint32_t count, bool is_body) {
    uint32_t flags = 0;
    if (!mem.Word(entry.entry + kEntryFlags, &flags)) {
      entry.entry_unreadable = true;
      ++out->entries_unreadable;
    }
    entry.flags = flags;
    entry.state = flat_state;
    entry.count = int32_t(count);
    entry.placed = (flags & kPlacedMask) == kPlacedMask;
    entry.loaded = entry.state == kStateLoaded;
    ++out->entries_present;
    out->entries_loaded += entry.loaded;
    // the game's accessor: the variant exists (the body: its state word says so), the entry is loaded and placed
    entry.valid = !entry.entry_unreadable && (is_body || int32_t(count) > 0) && entry.loaded && entry.placed;
    if (entry.valid) {
      ++out->entries_valid;
      uint32_t drawable = 0;
      mem.Word(entry.entry + kEntryDrawable, &drawable);
      entry.drawable = drawable;
      uint32_t lod_object = 0;
      if (Plausible(drawable) && mem.Word(drawable + kDrawableLod, &lod_object)) {
        entry.lod_object = lod_object;
      }
    }
    entry.first_geometry = uint32_t(out->geometries.size());
    if (entry.valid && Plausible(entry.drawable)) {
      const auto seen = options.production ? walked.end() : walked.find(entry.drawable);
      if (seen != walked.end()) {
        // the same drawable under another entry: its geometries are listed once; this entry shares them and passes its "in use" on
        const PartEntry& first = out->parts[seen->second];
        entry.shared_drawable = true;
        entry.first_geometry = first.first_geometry;
        entry.geometries = first.geometries;
        entry.models = first.models;
        ++out->drawables_shared;
        if (entry.in_use) {
          for (uint32_t i = 0; i < entry.geometries; ++i) {
            out->geometries[entry.first_geometry + i].in_use = true;
          }
        }
      } else {
        walked.emplace(entry.drawable, uint32_t(out->parts.size()));
        if (const uint8_t* l = Plausible(entry.lod_object) ? mem.Window(entry.lod_object, 20) : nullptr) {
          entry.models = Be16(l + kLodModelCount);
          entry.bucket_mask = Be32(l + kLodBucketMask);
        }
        entry.geometries = detail::WalkLod(mem, entry.lod_object, entry, uint32_t(out->parts.size()), out);
      }
    }
    if (entry.valid && entry.geometries != 0) {
      for (uint32_t i = 0; i < entry.geometries && entry.first_geometry + i < out->geometries.size(); ++i) {
        entry.matrix_bone = std::min(entry.matrix_bone, out->geometries[entry.first_geometry + i].bone);
      }
      if (Plausible(entry.matrices) && entry.matrix_bone < kMaxMatrixBones) {
        if (const uint8_t* m = mem.Window(entry.matrices + 64u * entry.matrix_bone + kMatrixTranslation, 12)) {
          LoadFloats(m, 3, entry.matrix_position);
          entry.matrix_read = true;
        }
      }
    }
    out->parts.push_back(entry);
  };

  // the body: in production the lod to take is the first with a placed entry among the wanted lod and then the coarser and the finer ones
  int32_t body_pick = -1;
  if (options.production && out->body_state == kStateLoaded) {
    const int32_t wanted = std::clamp<int32_t>(std::max<int32_t>(body_lod, options.min_lod), 0, int32_t(kLods) - 1);
    for (const int32_t candidate : LodOrder(wanted)) {
      uint32_t flags = 0;
      const uint32_t address = Be32(set + 4u * (kSetBody + uint32_t(candidate)));
      if (address != 0 && mem.Word(address + kEntryFlags, &flags) && (flags & kPlacedMask) == kPlacedMask) {
        body_pick = candidate;
        break;
      }
    }
  }
  for (uint32_t lod = 0; lod < kLods && !out->truncated; ++lod) {
    const uint32_t entry_address = Be32(set + 4u * (kSetBody + lod));
    if (entry_address == 0) {
      continue;
    }
    PartEntry entry;
    entry.part = uint16_t(kBodyPart);
    entry.variant = 0;
    entry.lod = uint8_t(lod);
    entry.entry = entry_address;
    entry.item = out->body_item;
    if (Plausible(entry.item)) {
      mem.Word(entry.item + kItemMatrices, &entry.matrices);
    }
    entry.world_matrices = body_worlds;
    entry.region = 0;
    const bool placed_state = out->body_state == kStateLoaded;
    entry.in_use = placed_state && kSetBody + lod == body_flat;
    if (options.production) {
      // Production: the one body entry the scene would draw (Options::production), nothing else of the body.
      entry.in_use = placed_state && int32_t(lod) == body_pick;
      if (!entry.in_use) {
        continue;
      }
    }
    add_entry(entry, out->body_state, 1, true);
  }

  // the parts
  for (uint32_t p = 0; p < kPartSlots && !out->truncated; ++p) {
    const uint32_t item = items ? Be32(items + 4u * p) : 0;
    uint32_t matrices = 0;
    if (Plausible(item)) {
      mem.Word(item + kItemMatrices, &matrices);
    }
    const uint32_t chosen = out->variants_read ? out->variant_of_part[p] : 0u;
    const uint32_t world_matrices = worlds ? Be32(worlds + 4u * p) : 0u;
    const bool draws_part = (!options.tables_read || draws[p] != 0) && PartDrawn(p, out->flags);
    const int32_t lod_of_part = options.all_parts_at_part_lod ? part_lod : PartLodOf(p, body_lod, part_lod);
    const uint32_t in_use_flat = (lod_of_part >= 0 && chosen < kVariants) ? FlatIndex(p, chosen, uint32_t(lod_of_part)) : 0xFFFFFFFFu;
    int32_t pick = -1;  // production: the lod of the chosen variant's entry the scene takes
    if (options.production && Plausible(item) && draws_part && chosen < kVariants && Be32(set + 4u * (kSetCounts + 10u * p + chosen)) != 0) {
      const int32_t wanted = std::clamp<int32_t>(std::max<int32_t>(lod_of_part, options.min_lod), 0, int32_t(kLods) - 1);
      for (const int32_t candidate : LodOrder(wanted)) {
        const uint32_t flat = FlatIndex(p, chosen, uint32_t(candidate));
        const uint32_t address = Be32(set + 4u * (kSetEntries + flat));
        uint32_t flags = 0;
        if (address != 0 && Be32(set + 4u * (kSetStates + flat)) == kStateLoaded && mem.Word(address + kEntryFlags, &flags) && (flags & kPlacedMask) == kPlacedMask) {
          pick = candidate;
          break;
        }
      }
      if (pick < 0) {
        continue;  // no lod of this part is loaded: nothing of it
      }
    } else if (options.production) {
      continue;
    }
    for (uint32_t v = 0; v < kVariants && !out->truncated; ++v) {
      if (options.production && v != chosen) {
        continue;
      }
      const uint32_t count = Be32(set + 4u * (kSetCounts + 10u * p + v));
      for (uint32_t lod = 0; lod < kLods && !out->truncated; ++lod) {
        if (options.production && int32_t(lod) != pick) {
          continue;
        }
        const uint32_t flat = FlatIndex(p, v, lod);
        const uint32_t entry_address = Be32(set + 4u * (kSetEntries + flat));
        if (entry_address == 0) {
          continue;
        }
        PartEntry entry;
        entry.part = uint16_t(p);
        entry.variant = uint8_t(v);
        entry.lod = uint8_t(lod);
        entry.entry = entry_address;
        entry.item = item;
        entry.matrices = matrices;
        entry.world_matrices = world_matrices;
        entry.region = options.tables_read ? options.region_of_part[p] : 0u;
        entry.in_use = Plausible(item) && draws_part && (options.production ? int32_t(lod) == pick : flat == in_use_flat);
        add_entry(entry, Be32(set + 4u * (kSetStates + flat)), count, false);
      }
    }
  }

  // the data of the geometries the game draws, within the budget; the tallies
  uint64_t budget = options.validate_budget;
  for (Geometry& g : out->geometries) {
    ++out->all.geometries;
    out->all.vertices += g.vertex_count;
    out->all.triangles += g.index_count / 3;
    if (g.lod < kLods) {
      ++out->lod_tally[g.lod].geometries;
      out->lod_tally[g.lod].vertices += g.vertex_count;
      out->lod_tally[g.lod].triangles += g.index_count / 3;
    }
    out->skinned += g.palette_count != 0;
    if (!g.in_use) {
      continue;
    }
    ++out->in_use.geometries;
    out->in_use.vertices += g.vertex_count;
    out->in_use.triangles += g.index_count / 3;
    if (options.validate && g.index_count != 0) {
      const uint64_t before = budget;
      ValidateGeometry(mem, &g, &budget);
      out->validated_bytes += before - budget;
      if (g.validated) {
        ++out->validated_in_use;
        out->resident_in_use += g.vb_readable && g.ib_readable && g.bad_indices == 0 && (g.positions_finite || !g.decl.PositionIsFloat3());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// the comparison with a frame's draws
// ---------------------------------------------------------------------------
// A captured draw names its buffers by the fetch constants and the device's index buffer; a walked geometry names them by the buffers' GPU headers, which are
// the same words. MatchFrame says, for the cars walked, which geometry each draw of the frame is and which geometries no draw used.

// What a captured draw says about its geometry (filled by the renderer from the DrawRecord).
struct DrawKey {
  uint32_t id = 0;                // the stencil ID the draw writes (the car's number this frame)
  uint32_t vertex_d0 = 0;         // the position stream's vertex fetch constant: the address with the type 3 in its low bits
  uint32_t vertex_d1 = 0;         // the size in bytes in bits 2..25 and the endian in bits 0..1
  uint32_t stride = 0;            // bytes
  uint32_t position_offset = 0;
  bool indexed = false;
  uint32_t index_object = 0;      // the device's index buffer object (the buffer's GPU header)
  uint32_t index_dword0 = 0;      // its first dword
  uint32_t index_address = 0;     // its address as stored (windowed)
  uint32_t count = 0;             // the draw's indices
  uint32_t start = 0;
  // The draw's own model-to-world (the vertex shader's recipe: palette then gWorld, or gWorld alone), when the caller could make it and the draw is rigid: what the
  // placement of a walked geometry is checked against (car_model_rt_rules.h).
  bool has_world = false;
  temporal::Mat4 world;
};

enum class DrawHit : uint8_t {
  kNone,        // no walked geometry has this vertex window
  kVertexOnly,  // a vertex window matched, no index buffer of it did (or the draw is not indexed)
  kGeometry,    // vertex window and index buffer matched
};

struct DrawMatch {
  DrawHit hit = DrawHit::kNone;
  uint32_t car = 0;               // the first car whose geometry it is (index into the cars given)
  uint32_t geometry = 0;          // ... its geometry (index into that car's geometries)
};

// One car's numbers.
struct CarMatch {
  uint32_t vehicle = 0;
  uint32_t geometries = 0;          // walked
  uint32_t in_use = 0;              // walked and in use
  uint32_t empty_in_use = 0;        // in use with no indices (the game draws nothing for them)
  uint32_t matched = 0;             // walked geometries some draw of the frame is
  uint32_t matched_in_use = 0;      // ... of those in use
  uint32_t matched_not_in_use = 0;  // ... of those not in use (the walk's idea of "in use" is off when this is large)
  uint32_t walked_only_in_use = 0;  // in use, with indices, and no draw
  uint32_t draws = 0;               // draws that are one of this car's geometries
  uint32_t drawn_only = 0;          // draws with this car's ID that are none of any walked car's geometries
  uint32_t vertex_only = 0;         // ... of those, the ones whose vertex window matched but whose index buffer did not
  // How exactly the matched geometries equal their draw (counted per matched geometry, against its first draw).
  uint32_t exact_words = 0;         // the fetch constant's two dwords equal the header's two words
  uint32_t index_object_equal = 0;  // the device's index buffer object is the IB's header
  uint32_t index_address_equal = 0; // ... its address is the header's word 6
  uint32_t index_count_equal = 0;   // the draw's index count is the geometry's
  uint32_t stride_equal = 0;        // the draw's stride is the vertex buffer's
  uint32_t position_offset_equal = 0;
  uint32_t shared_buffers = 0;      // geometries whose vertex window another walked car has too
  uint32_t walked_by_lod[kLods] = {};   // by the table's lod slot (the body's and the parts'): which lod the frame's draws are of
  uint32_t matched_by_lod[kLods] = {};
  bool drawn = false;               // some draw is one of its geometries
  std::vector<uint32_t> ids;        // the stencil IDs of those draws (sorted, unique)
};

struct FrameMatch {
  std::vector<CarMatch> cars;
  std::vector<DrawMatch> draws;
  uint32_t draws_total = 0;
  uint32_t draws_matched = 0;       // a geometry
  uint32_t draws_vertex_only = 0;
  uint32_t draws_unowned = 0;       // no geometry, and an ID no walked car owns
};

namespace detail {

inline uint64_t VertexWindowKey(uint32_t d0, uint32_t d1) {
  return (uint64_t(PhysicalOf(d0 & ~kFetchTypeMask)) << 32) | (d1 & kFetchSizeMask);
}

}  // namespace detail

// `cars` are the walks (not null); `draws` the frame's draws that write a vehicle ID. `owner_of_draw` (by the draws given; null: none) names the car each draw is OF
// (an index into `cars`, or -1 for none: car_model_rt_rules.h AssignDraws, which tells the cars of one model apart by the draw's stencil ID and origin). Without it a draw is credited
// to EVERY car whose buffers it names, so a car of the same model that the frame does not draw (a racer 56 m away, culled) looks drawn when its twin is, and its in-use list
// (another part variant: 19 geometries different) is then scored against its twin's draws: the 88.7 percent car of the first check run. With it a draw hits only its own car's
// geometries; a draw that matched some car's vertex window but has no owner is vertex-only.
inline FrameMatch MatchFrame(const std::vector<const CarWalk*>& cars, const std::vector<DrawKey>& draws, const std::vector<int32_t>* owner_of_draw = nullptr) {
  FrameMatch out;
  out.cars.resize(cars.size());
  out.draws.resize(draws.size());
  out.draws_total = uint32_t(draws.size());

  struct Owner {
    uint32_t car;
    uint32_t geometry;
  };
  std::unordered_map<uint64_t, std::vector<Owner>> windows;
  for (uint32_t c = 0; c < cars.size(); ++c) {
    for (uint32_t g = 0; g < cars[c]->geometries.size(); ++g) {
      const Geometry& geometry = cars[c]->geometries[g];
      if (geometry.vb_ok) {
        windows[detail::VertexWindowKey(geometry.vb_w6, geometry.vb_w7)].push_back({c, g});
      }
    }
  }

  // which geometries a draw hit (a bit per geometry), and the first draw's numbers
  std::vector<std::vector<uint8_t>> hit(cars.size());
  for (uint32_t c = 0; c < cars.size(); ++c) {
    hit[c].assign(cars[c]->geometries.size(), 0);
    out.cars[c].vehicle = cars[c]->vehicle;
    out.cars[c].geometries = uint32_t(cars[c]->geometries.size());
    for (const Geometry& g : cars[c]->geometries) {
      out.cars[c].in_use += g.in_use;
      out.cars[c].empty_in_use += g.in_use && g.index_count == 0;
      if (g.lod < kLods) {
        ++out.cars[c].walked_by_lod[g.lod];
      }
    }
  }
  for (uint32_t d = 0; d < draws.size(); ++d) {
    const DrawKey& draw = draws[d];
    DrawMatch& match = out.draws[d];
    const auto found = windows.find(detail::VertexWindowKey(draw.vertex_d0, draw.vertex_d1));
    if (found == windows.end()) {
      continue;
    }
    bool any_vertex = false;
    for (const Owner& owner : found->second) {
      const Geometry& g = cars[owner.car]->geometries[owner.geometry];
      any_vertex = true;
      if (owner_of_draw && d < owner_of_draw->size() && (*owner_of_draw)[d] != int32_t(owner.car)) {
        continue;  // the draw is another car's (or nobody's): the buffers are the same, the car is not
      }
      const bool index_ok = draw.indexed && g.ib_ok &&
                            ((draw.index_object != 0 && draw.index_object == g.ib_header) ||
                             PhysicalOf(draw.index_address) == PhysicalOf(g.ib_w6));
      if (!index_ok) {
        continue;
      }
      CarMatch& car = out.cars[owner.car];
      ++car.draws;
      car.ids.push_back(draw.id);
      if (match.hit != DrawHit::kGeometry) {
        match.hit = DrawHit::kGeometry;
        match.car = owner.car;
        match.geometry = owner.geometry;
      }
      if (!hit[owner.car][owner.geometry]) {
        hit[owner.car][owner.geometry] = 1;
        ++car.matched;
        car.matched_in_use += g.in_use;
        car.matched_not_in_use += !g.in_use;
        if (g.lod < kLods) {
          ++car.matched_by_lod[g.lod];
        }
        car.exact_words += draw.vertex_d0 == g.vb_w6 && draw.vertex_d1 == g.vb_w7;
        car.index_object_equal += draw.index_object != 0 && draw.index_object == g.ib_header;
        car.index_address_equal += draw.index_address == g.ib_w6;
        car.index_count_equal += draw.count == g.index_count;
        car.stride_equal += draw.stride == g.vb_stride;
        car.position_offset_equal += draw.position_offset == 0;
      }
    }
    if (match.hit == DrawHit::kNone && any_vertex) {
      match.hit = DrawHit::kVertexOnly;
      match.car = found->second.front().car;
      match.geometry = found->second.front().geometry;
    }
  }

  // shared vertex windows, the drawn flag, the IDs
  for (const auto& [key, owners] : windows) {
    if (owners.size() < 2) {
      continue;
    }
    std::unordered_set<uint32_t> distinct;
    for (const Owner& o : owners) {
      distinct.insert(o.car);
    }
    if (distinct.size() < 2) {
      continue;
    }
    for (const Owner& o : owners) {
      ++out.cars[o.car].shared_buffers;
    }
  }
  std::unordered_map<uint32_t, std::vector<uint32_t>> owners_of_id;  // stencil ID -> the cars that have a draw of it
  for (uint32_t c = 0; c < out.cars.size(); ++c) {
    CarMatch& car = out.cars[c];
    std::sort(car.ids.begin(), car.ids.end());
    car.ids.erase(std::unique(car.ids.begin(), car.ids.end()), car.ids.end());
    car.drawn = car.draws != 0;
    for (const uint32_t id : car.ids) {
      owners_of_id[id].push_back(c);
    }
    for (uint32_t g = 0; g < cars[c]->geometries.size(); ++g) {
      const Geometry& geometry = cars[c]->geometries[g];
      car.walked_only_in_use += geometry.in_use && geometry.index_count != 0 && !hit[c][g];
    }
  }
  for (uint32_t d = 0; d < draws.size(); ++d) {
    const DrawMatch& match = out.draws[d];
    if (match.hit == DrawHit::kGeometry) {
      ++out.draws_matched;
      continue;
    }
    out.draws_vertex_only += match.hit == DrawHit::kVertexOnly;
    const auto owners = owners_of_id.find(draws[d].id);
    if (owners == owners_of_id.end()) {
      ++out.draws_unowned;
      continue;
    }
    for (const uint32_t c : owners->second) {
      ++out.cars[c].drawn_only;
      out.cars[c].vertex_only += match.hit == DrawHit::kVertexOnly;
    }
  }
  return out;
}

}  // namespace mcla::gpu::car_model_walk

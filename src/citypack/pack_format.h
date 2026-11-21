#pragma once
// ===========================================================================
// LARecomp city pack -- the file format (version 1)
// ===========================================================================
// The pack is the city's geometry for ray tracing, read from the PLAYER's OWN game files on the player's PC (like the shader packs:
// src/shaderpack) and never shipped or committed: resources/city/sc/<sector>.xcs (439 streamed sectors, the table city.xct) and the
// far level of detail lod.xmd (one model per sector). One file, mcla_city.v1.pack, in the folder `city` next to the shader packs, and
// a small text stamp (stamp.h) that says what it was built from. perf/CITY-RT-SURVEY.md has the sources' layout (section 1.2).
//
//   PackHeader            128 bytes at 0
//   sector table          sector_count x kSectorRecordBytes (160), at table_offset
//   name heap             the sectors' names, no terminators (a record holds offset and length), at names_offset
//   mesh blobs            at blobs_offset: for each sector its hi detail blob, then its far lod blob (either may be absent)
//
// All integers are little endian. A reader opens the file, reads the header and the table (about 70 KB) and then reads ONE blob a
// sector when the streamer wants it (hi_offset/hi_size, lod_offset/lod_size in the record), so nothing here has to be read whole.
//
// A MESH BLOB is the triangles of one sector (or of its lod model) ready to become one bottom level acceleration structure:
//   MeshHeader            64 bytes: counts, the bounds, a hash of what follows
//   stream table          stream_count x 24 bytes: {id, encoding, offset, size, count, element_bytes}; offsets are from the blob's start
//   streams               positions (float x, y, z a vertex; exact, welded), indices (a triangle list), runs (the geometry's classes)
// The triangles are in ONE order: first the OPAQUE triangles [0, opaque_triangles), then the alpha tested ones, so a structure is built as one
// geometry flagged opaque and (when there are alpha tested triangles) one that is not. Every position is in WORLD space (the city has no
// instancing and no per model matrix), so the structure needs no transform but the frame's.
//
// WHAT THE PACK LEAVES OUT (kDefaultDropMask; the builder's rule, stamped): decals (coplanar layers over a road or a wall: as casters they
// would shadow their own surface), store interiors and water. Everything else is a caster: windows too (their class is kept in the runs, so
// a later version can split them). The classes come from the shader names of the sector's shader group (material_class.h).
//
// RESERVED FOR HIT SHADING (reflections, global illumination: CITY-RT M2/M3): stream ids from kStreamFirstAttribute up. They hold PER
// TRIANGLE CORNER attributes (triangle_count x 3 elements, in the index stream's order: uv0 as half2, colour as 4 bytes, ...), not per
// vertex, so the positional weld above stays valid and a hit reads corner `3 * PrimitiveIndex() + k`. Version 1 writes none and a reader
// skips every stream id it does not know.
//
// VERSIONS. kPackFormatVersion is the reader's contract (this file): a reader refuses another number. kBuilderVersion is the builder's
// rules (the classes, what is dropped, the weld, the order): it is in the stamp, and a pack of another builder version is rebuilt but still
// readable. Bump the right one by hand.
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "citypack/bytes.h"

namespace larecomp::citypack {

inline constexpr char kPackMagic[8] = {'L', 'A', 'R', 'C', 'I', 'T', 'Y', '1'};
inline constexpr uint32_t kPackFormatVersion = 1;
inline constexpr uint32_t kBuilderVersion = 1;
inline constexpr const char* kPackFileName = "mcla_city.v1.pack";
inline constexpr const char* kStampFileName = "mcla_city.stamp";

inline constexpr uint32_t kHeaderBytes = 128;
inline constexpr uint32_t kSectorRecordBytes = 160;
inline constexpr uint32_t kMeshHeaderBytes = 64;
inline constexpr uint32_t kStreamEntryBytes = 24;
inline constexpr uint32_t kRunBytes = 16;
inline constexpr uint32_t kMeshMagic = 0x31424D43;  // "CMB1"

// The game's own counts (a golden check of the builder, CITY-RT-SURVEY section 0): sectors, all hi detail geometries, triangles and vertices
// of the sources, and the far level of detail's. A different game build gives different numbers: they are expectations of a census, not limits.
inline constexpr uint32_t kGoldenSectors = 439;
inline constexpr uint64_t kGoldenHiGeometries = 78865;
inline constexpr uint64_t kGoldenHiTriangles = 24941817;
inline constexpr uint64_t kGoldenHiVertices = 35231569;
inline constexpr uint64_t kGoldenLodTriangles = 382734;
inline constexpr uint64_t kGoldenLodVertices = 686050;

// What a geometry of the city is, from its shader's names (material_class.h). The numbers are in the file: never reuse one.
enum class MaterialClass : uint8_t {
  kSolid = 0,        // walls, roofs, props: the default
  kGround = 1,       // roads, terrain, ground grime
  kGlass = 2,        // windows (CityWindowUpper, CityWindowLower)
  kEmissive = 3,     // neon signs
  kBillboard = 4,    // billboards
  kAlphaTested = 5,  // cut-out geometry (a shader that says Alpha): the pack's non opaque triangles
  kDecal = 6,        // a layer over a surface: left out by default
  kInterior = 7,     // store interiors: left out by default
  kWater = 8,        // ponds, pools: left out by default
  kCount = 9,
};
constexpr uint32_t ClassBit(MaterialClass c) { return 1u << static_cast<uint32_t>(c); }
inline constexpr uint32_t kDefaultDropMask =
    ClassBit(MaterialClass::kDecal) | ClassBit(MaterialClass::kInterior) | ClassBit(MaterialClass::kWater);
constexpr bool IsOpaqueClass(MaterialClass c) { return c != MaterialClass::kAlphaTested; }
std::string_view ClassName(MaterialClass c);

enum class PositionEncoding : uint32_t {
  kFloat32 = 0,  // x, y, z as IEEE floats, exact (the default: the weld is by bit pattern, neighbouring sectors agree at their seam)
  kGrid16 = 1,   // 16 bits an axis inside the mesh's box: 6 bytes a vertex, an error of at most extent / 131070 an axis (4.4 mm over 575 m)
};
enum class IndexEncoding : uint32_t {
  kRaw32 = 0,         // uint32 each
  kDeltaVarint = 1,   // the difference to the previous index, zigzag, LEB128: about 1.3 bytes an index on the city's meshes
};
enum class MeshKind : uint32_t { kHi = 0, kLod = 1 };

inline constexpr uint32_t kStreamPositions = 1;
inline constexpr uint32_t kStreamIndices = 2;
inline constexpr uint32_t kStreamRuns = 3;
inline constexpr uint32_t kStreamFirstAttribute = 16;

// One run of triangles of one class, by the source geometry (adjacent runs of one class and shader merge).
struct GeometryRun {
  uint32_t first_triangle = 0;  // within the whole triangle list (opaque ones first)
  uint32_t triangle_count = 0;
  MaterialClass material = MaterialClass::kSolid;
  uint8_t flags = 0;            // bit 0: the triangles are not opaque
  uint16_t shader_index = 0;    // the source shader's index in the sector's group (0xFFFF: unknown)
  uint32_t shader_hash = 0;     // rage hash of "<group>/<preset>"
};

// A mesh in memory: what the builder makes, what a blob decodes to. Positions are x, y, z floats; indices address them.
struct MeshData {
  std::vector<float> positions;
  std::vector<uint32_t> indices;       // triangle_count * 3, opaque triangles first
  uint32_t opaque_triangles = 0;
  std::vector<GeometryRun> runs;
  float bounds_min[3] = {0, 0, 0};
  float bounds_max[3] = {0, 0, 0};

  uint32_t vertex_count() const { return static_cast<uint32_t>(positions.size() / 3); }
  uint32_t triangle_count() const { return static_cast<uint32_t>(indices.size() / 3); }
  uint32_t alpha_triangles() const { return triangle_count() - opaque_triangles; }
  bool empty() const { return indices.empty(); }
};

// The first 128 bytes of the file.
struct PackHeader {
  uint32_t format_version = kPackFormatVersion;
  uint32_t builder_version = kBuilderVersion;
  uint32_t flags = 0;
  uint32_t sector_count = 0;
  uint32_t sector_record_bytes = kSectorRecordBytes;
  uint32_t names_bytes = 0;
  uint32_t drop_mask = kDefaultDropMask;
  uint32_t position_encoding = static_cast<uint32_t>(PositionEncoding::kFloat32);
  uint64_t table_offset = 0;
  uint64_t names_offset = 0;
  uint64_t blobs_offset = 0;
  uint64_t file_size = 0;
  uint64_t table_hash = 0;  // FNV-1a over the table's bytes and the name heap's

  void Write(ByteWriter& w) const;
  // False (with `error`) for a file that is not a city pack or of another format version.
  static bool Read(ByteReader& r, PackHeader* out, std::string* error);
};

// One sector: the game's own box, where its blobs are, and what is in them.
struct SectorRecord {
  uint32_t name_hash = 0;      // rage hash of the sector's name (what the game's table is searched by)
  uint32_t name_offset = 0;    // in the name heap
  uint32_t name_length = 0;
  uint32_t flags = 0;          // kSectorHasHi, kSectorHasLod, kSectorFromMods
  float aabb_min[3] = {0, 0, 0};  // the game's box of the sector (city.xct): what streaming measures distances to
  float aabb_max[3] = {0, 0, 0};
  float hi_min[3] = {0, 0, 0};    // the kept hi geometry's own bounds
  float hi_max[3] = {0, 0, 0};
  uint64_t hi_offset = 0;
  uint32_t hi_size = 0;
  uint32_t hi_triangles = 0;
  uint32_t hi_vertices = 0;
  uint32_t hi_opaque_triangles = 0;
  uint64_t lod_offset = 0;
  uint32_t lod_size = 0;
  uint32_t lod_triangles = 0;
  uint32_t lod_vertices = 0;
  uint32_t dropped_triangles = 0;  // hi triangles the drop mask left out
  float lod_min[3] = {0, 0, 0};
  float lod_max[3] = {0, 0, 0};

  bool has_hi() const { return (flags & 1u) != 0; }
  bool has_lod() const { return (flags & 2u) != 0; }
  void Write(ByteWriter& w) const;
  static bool Read(ByteReader& r, SectorRecord* out);
};
inline constexpr uint32_t kSectorHasHi = 1;
inline constexpr uint32_t kSectorHasLod = 2;
inline constexpr uint32_t kSectorFromMods = 4;

struct MeshHeader {
  uint32_t flags = 0;  // bit 0: has alpha tested triangles
  uint32_t vertex_count = 0;
  uint32_t triangle_count = 0;
  uint32_t opaque_triangles = 0;
  uint32_t run_count = 0;
  uint32_t stream_count = 0;
  float bounds_min[3] = {0, 0, 0};
  float bounds_max[3] = {0, 0, 0};
  uint64_t body_hash = 0;  // FNV-1a over everything after the header

  void Write(ByteWriter& w) const;
  static bool Read(ByteReader& r, MeshHeader* out);
};

struct StreamEntry {
  uint32_t id = 0;
  uint32_t encoding = 0;
  uint32_t offset = 0;  // from the blob's start
  uint32_t size = 0;
  uint32_t count = 0;
  uint32_t element_bytes = 0;
  void Write(ByteWriter& w) const;
  static bool Read(ByteReader& r, StreamEntry* out);
};

// Makes a blob out of a mesh. `positions` and `indices` choose the encodings (the default float and delta are the builder's).
std::vector<uint8_t> EncodeMesh(const MeshData& mesh, PositionEncoding positions = PositionEncoding::kFloat32,
                                IndexEncoding indices = IndexEncoding::kDeltaVarint);
// The other way. False (with `error`) for a cut, corrupt (hash, counts, an index past the vertices) or unknown blob.
bool DecodeMesh(const uint8_t* data, size_t size, MeshData* out, std::string* error);

// The delta + zigzag + LEB128 index codec (a stream of `count` indices), exposed for the tests.
void EncodeIndicesDelta(const uint32_t* indices, size_t count, ByteWriter& w);
bool DecodeIndicesDelta(ByteReader& r, size_t count, uint32_t vertex_count, std::vector<uint32_t>* out);

}  // namespace larecomp::citypack

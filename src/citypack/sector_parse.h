#pragma once
// ===========================================================================
// LARecomp city pack -- reading the game's city resources
// ===========================================================================
// The layouts below were read from the player's own files (CITY-RT-SURVEY section 1.2, checked here on hw_ss_07 and on lod.xmd against city.xct's boxes);
// all fields are big endian and the pointers are the resource's own (rsc_image.h).
//
//   city.xct (the city manager's resource, type 1086)
//     +20 pointer to the sector array, +24 u16 count (439); a sector entry is 304 bytes:
//        +208 float3 box min, +224 float3 box max (world), +240 pointer to the name (cstring), +244 u32 index
//
//   <sector>.xcs  (one drawable; every vertex is a WORLD position)
//     +16 drawable: +8 shader group, +12 model dictionary
//     shader group: +8 pointer to the shader pointer array, +12 u16 count;  shader: +48 pointer to the group name, +60 pointer to the preset name
//     model dictionary: +16 pointer to the u32 name hashes, +20 u16 count, +24 pointer to the grmModel pointer array
//     grmModel: +4 pointer to the geometry pointer array, +8 u16 geometry count, +16 pointer to u16 shader indices (one a geometry)
//     grmGeometry: +12 vertex buffer, +28 index buffer, +44 u32 index count, +48 u32 primitive count, +52 u16 vertex count, +54 u16 primitive type (3 = triangle list),
//                  +56 bone palette (null in every city geometry)
//     vertex buffer: +4 u16 vertex count, +12 u32 stride, +16 declaration (+0 u32 FVF mask: bit 0 = position float3 at offset 0), +24 pointer to the vertices
//     index buffer: +4 u32 index count, +8 pointer to the u16 indices
//     +32 pointer to the city model records (80 bytes each), +36 u16 count: +74 u8 bucket mask, +76 u32 name hash (= the dictionary's)
//
//   lod.xmd (the far level of detail: one model for each sector, by the sector's INDEX; the box of model i is sector i's)
//     +8 shader group (as above), +12 pointer to the model pointer array, +16 u16 count (439; one entry is null)
// ===========================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "citypack/bytes.h"
#include "citypack/rsc_image.h"

namespace larecomp::citypack {

struct SectorInfo {
  std::string name;
  uint32_t name_hash = 0;
  uint32_t index = 0;  // the table's order, which lod.xmd's models follow
  float aabb_min[3] = {0, 0, 0};
  float aabb_max[3] = {0, 0, 0};
};

// city.xct. False (with `error`) for a resource that is not the city table.
bool ParseCityTable(const RscImage& image, std::vector<SectorInfo>* out, std::string* error);

struct ShaderNames {
  std::string group;
  std::string preset;
};

// One geometry as the game stores it. `vertices` and `indices` point into the image (valid while it is); positions are the first three floats of each
// vertex, big endian; indices are big endian u16.
struct SourceGeometry {
  uint32_t model_index = 0;     // the model in the dictionary (a sector), or the lod model's index (= the sector's)
  uint32_t geometry_index = 0;  // within the model
  uint32_t shader_index = 0xFFFF;
  const ShaderNames* shader = nullptr;  // null: no names
  uint32_t vertex_count = 0;
  uint32_t stride = 0;
  uint32_t index_count = 0;
  uint32_t fvf = 0;
  uint32_t bucket_mask = 0;  // the city model record's, 0 where there is none (the lod)
  const uint8_t* vertices = nullptr;
  const uint8_t* indices = nullptr;
};

struct ParseStats {
  uint32_t shaders = 0;
  uint32_t models = 0;
  uint32_t null_models = 0;
  uint32_t geometries = 0;
  uint64_t triangles = 0;  // the sources' (index count / 3)
  uint64_t vertices = 0;   // the sources' vertex buffers' (repeated seams included)
  uint32_t skipped_not_triangle_list = 0;
  uint32_t skipped_no_position = 0;  // the FVF has no position at offset 0
  uint32_t skipped_bad_range = 0;    // a pointer or a length outside the resource
  uint32_t unknown_shader_index = 0;
  uint32_t vertex_count_mismatch = 0;  // the geometry's and the buffer's vertex counts differ (the buffer's is used)
  uint32_t index_count_mismatch = 0;
  uint32_t skinned = 0;                // a geometry with a bone palette (none in the city)
  uint32_t city_models = 0;            // the model records of a sector
  uint32_t models_without_record = 0;  // a dictionary model whose hash no city model record has
};

using GeometryCallback = std::function<void(const SourceGeometry&)>;

// A sector's drawable: every geometry of every model of its dictionary, in order. `shaders` receives the shader table (indexed by shader_index); the
// callback's `shader` pointers point into it. False (with `error`) for a resource that is not a sector.
bool ParseSector(const RscImage& image, std::vector<ShaderNames>* shaders, const GeometryCallback& on_geometry, ParseStats* stats, std::string* error);
// lod.xmd: the same for the far level of detail; `model_index` of a geometry is the sector's index.
bool ParseLod(const RscImage& image, std::vector<ShaderNames>* shaders, const GeometryCallback& on_geometry, ParseStats* stats, std::string* error);

// A geometry's vertex position `v` (host floats).
inline void SourcePosition(const SourceGeometry& g, uint32_t v, float out[3]) {
  const uint8_t* p = g.vertices + size_t(v) * g.stride;
  out[0] = LoadBEFloat(p);
  out[1] = LoadBEFloat(p + 4);
  out[2] = LoadBEFloat(p + 8);
}
}  // namespace larecomp::citypack

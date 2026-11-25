#pragma once
// ===========================================================================
// LARecomp city pack -- the mesh builder: source geometries in, one welded mesh out
// ===========================================================================
// The sources are many geometries (a vertex buffer and a 16 bit triangle list each, vertices repeated at every attribute seam: normals, UVs,
// colours). The builder welds them into ONE vertex list by exact position (the bit pattern of the three floats, -0 taken as 0), so a corner
// shared by two meshes of a sector is one vertex and the acceleration structure has no crack there that the source did not have. A triangle
// left with a repeated corner or no area is dropped (and counted). Opaque and alpha tested triangles are kept in two lists; Finish joins them,
// opaque first, and fills the runs.
// ===========================================================================

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "citypack/pack_format.h"

namespace larecomp::citypack {

struct WeldStats {
  uint64_t source_vertices = 0;
  uint64_t source_triangles = 0;
  uint64_t dropped_degenerate = 0;   // a repeated corner (after the weld) or no area
  uint64_t dropped_bad_index = 0;    // an index past the geometry's vertices
  uint64_t dropped_not_finite = 0;   // a corner that is not a finite number
};

class MeshBuilder {
 public:
  MeshBuilder();
  // One geometry: `positions` are vertex_count x 3 floats (already host order), `indices` a triangle list of index_count entries addressing them.
  // `material` decides the list (alpha tested or opaque) and the run. Returns the triangles kept. `kept_triangles`, when given, receives the number (within this
  // geometry's own triangle list) of each triangle kept, in the order they were appended to the list: the hit data's builder (hit_collect.h) reads the corners of exactly those
  // triangles, so that its corner stream is the mesh's triangle list triangle for triangle.
  uint32_t AddGeometry(const float* positions, uint32_t vertex_count, const uint16_t* indices, uint32_t index_count, MaterialClass material,
                       uint16_t shader_index, uint32_t shader_hash, std::vector<uint32_t>* kept_triangles = nullptr);
  // The same with 32 bit indices (the tests, a later source).
  uint32_t AddGeometry(const float* positions, uint32_t vertex_count, const uint32_t* indices, uint32_t index_count, MaterialClass material,
                       uint16_t shader_index, uint32_t shader_hash, std::vector<uint32_t>* kept_triangles = nullptr);
  // The finished mesh: opaque triangles first, runs merged, bounds of the kept vertices. The builder is empty afterwards.
  MeshData Finish();
  const WeldStats& stats() const { return stats_; }

 private:
  struct Key {
    uint32_t x, y, z;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      uint64_t h = k.x * 0x9E3779B97F4A7C15ull;
      h = (h ^ (h >> 29)) + k.y * 0xBF58476D1CE4E5B9ull;
      h = (h ^ (h >> 32)) + k.z * 0x94D049BB133111EBull;
      return static_cast<size_t>(h ^ (h >> 31));
    }
  };
  template <typename Index>
  uint32_t Add(const float* positions, uint32_t vertex_count, const Index* indices, uint32_t index_count, MaterialClass material, uint16_t shader_index,
               uint32_t shader_hash, std::vector<uint32_t>* kept_triangles);
  uint32_t Weld(float x, float y, float z);

  std::vector<float> positions_;
  std::unordered_map<Key, uint32_t, KeyHash> welded_;
  std::vector<uint32_t> opaque_;
  std::vector<uint32_t> alpha_;
  std::vector<GeometryRun> opaque_runs_;  // first_triangle within the opaque list
  std::vector<GeometryRun> alpha_runs_;   // ... within the alpha list
  WeldStats stats_;
};

}  // namespace larecomp::citypack

#include "citypack/mesh_builder.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace larecomp::citypack {

MeshBuilder::MeshBuilder() = default;

uint32_t MeshBuilder::Weld(float x, float y, float z) {
  // -0 and +0 are one position
  if (x == 0.0f) x = 0.0f;
  if (y == 0.0f) y = 0.0f;
  if (z == 0.0f) z = 0.0f;
  const Key key{std::bit_cast<uint32_t>(x), std::bit_cast<uint32_t>(y), std::bit_cast<uint32_t>(z)};
  const auto [it, inserted] = welded_.try_emplace(key, static_cast<uint32_t>(positions_.size() / 3));
  if (inserted) {
    positions_.push_back(x);
    positions_.push_back(y);
    positions_.push_back(z);
  }
  return it->second;
}

template <typename Index>
uint32_t MeshBuilder::Add(const float* positions, uint32_t vertex_count, const Index* indices, uint32_t index_count, MaterialClass material,
                          uint16_t shader_index, uint32_t shader_hash) {
  stats_.source_vertices += vertex_count;
  const bool alpha = !IsOpaqueClass(material);
  std::vector<uint32_t>& list = alpha ? alpha_ : opaque_;
  std::vector<GeometryRun>& runs = alpha ? alpha_runs_ : opaque_runs_;
  const uint32_t before = static_cast<uint32_t>(list.size() / 3);
  uint32_t kept = 0;
  // Welded index of each source vertex, made when a triangle first uses it (a vertex no triangle uses never enters the mesh).
  std::vector<uint32_t> remap(vertex_count, std::numeric_limits<uint32_t>::max());
  const uint32_t triangles = index_count / 3;
  stats_.source_triangles += triangles;
  for (uint32_t t = 0; t < triangles; ++t) {
    uint32_t c[3] = {0, 0, 0};
    bool good = true;
    for (int k = 0; k < 3; ++k) {
      const uint32_t i = static_cast<uint32_t>(indices[t * 3 + uint32_t(k)]);
      if (i >= vertex_count) {
        good = false;
        break;
      }
      c[k] = i;
    }
    if (!good) {
      ++stats_.dropped_bad_index;
      continue;
    }
    float p[3][3];
    for (int k = 0; k < 3; ++k) {
      for (int a = 0; a < 3; ++a) p[k][a] = positions[size_t(c[k]) * 3 + size_t(a)];
      if (!std::isfinite(p[k][0]) || !std::isfinite(p[k][1]) || !std::isfinite(p[k][2])) good = false;
    }
    if (!good) {
      ++stats_.dropped_not_finite;
      continue;
    }
    // The degenerate test is made on the positions themselves (exact), not on the indices: two source vertices at one place are one corner.
    const bool same01 = p[0][0] == p[1][0] && p[0][1] == p[1][1] && p[0][2] == p[1][2];
    const bool same12 = p[1][0] == p[2][0] && p[1][1] == p[2][1] && p[1][2] == p[2][2];
    const bool same02 = p[0][0] == p[2][0] && p[0][1] == p[2][1] && p[0][2] == p[2][2];
    if (same01 || same12 || same02) {
      ++stats_.dropped_degenerate;
      continue;
    }
    // No area: the cross product of two edges is exactly zero in double (a sliver with a tiny area stays: it casts a hair of shadow, which costs nothing).
    const double e1[3] = {double(p[1][0]) - p[0][0], double(p[1][1]) - p[0][1], double(p[1][2]) - p[0][2]};
    const double e2[3] = {double(p[2][0]) - p[0][0], double(p[2][1]) - p[0][1], double(p[2][2]) - p[0][2]};
    const double cx = e1[1] * e2[2] - e1[2] * e2[1];
    const double cy = e1[2] * e2[0] - e1[0] * e2[2];
    const double cz = e1[0] * e2[1] - e1[1] * e2[0];
    if (cx == 0.0 && cy == 0.0 && cz == 0.0) {
      ++stats_.dropped_degenerate;
      continue;
    }
    for (int k = 0; k < 3; ++k) {
      uint32_t& w = remap[c[k]];
      if (w == std::numeric_limits<uint32_t>::max()) w = Weld(p[k][0], p[k][1], p[k][2]);
      list.push_back(w);
    }
    ++kept;
  }
  if (kept != 0) {
    // adjacent runs of one class and shader are one run
    if (!runs.empty() && runs.back().material == material && runs.back().shader_index == shader_index && runs.back().shader_hash == shader_hash &&
        runs.back().first_triangle + runs.back().triangle_count == before) {
      runs.back().triangle_count += kept;
    } else {
      GeometryRun run;
      run.first_triangle = before;
      run.triangle_count = kept;
      run.material = material;
      run.flags = alpha ? 1 : 0;
      run.shader_index = shader_index;
      run.shader_hash = shader_hash;
      runs.push_back(run);
    }
  }
  return kept;
}

uint32_t MeshBuilder::AddGeometry(const float* positions, uint32_t vertex_count, const uint16_t* indices, uint32_t index_count, MaterialClass material,
                                  uint16_t shader_index, uint32_t shader_hash) {
  return Add(positions, vertex_count, indices, index_count, material, shader_index, shader_hash);
}

uint32_t MeshBuilder::AddGeometry(const float* positions, uint32_t vertex_count, const uint32_t* indices, uint32_t index_count, MaterialClass material,
                                  uint16_t shader_index, uint32_t shader_hash) {
  return Add(positions, vertex_count, indices, index_count, material, shader_index, shader_hash);
}

MeshData MeshBuilder::Finish() {
  MeshData mesh;
  mesh.opaque_triangles = static_cast<uint32_t>(opaque_.size() / 3);
  mesh.indices = std::move(opaque_);
  mesh.indices.insert(mesh.indices.end(), alpha_.begin(), alpha_.end());
  mesh.runs = std::move(opaque_runs_);
  for (GeometryRun run : alpha_runs_) {
    run.first_triangle += mesh.opaque_triangles;
    mesh.runs.push_back(run);
  }
  mesh.positions = std::move(positions_);
  if (!mesh.positions.empty()) {
    for (int a = 0; a < 3; ++a) {
      mesh.bounds_min[a] = std::numeric_limits<float>::max();
      mesh.bounds_max[a] = std::numeric_limits<float>::lowest();
    }
    for (size_t v = 0; v + 2 < mesh.positions.size(); v += 3) {
      for (int a = 0; a < 3; ++a) {
        mesh.bounds_min[a] = std::min(mesh.bounds_min[a], mesh.positions[v + size_t(a)]);
        mesh.bounds_max[a] = std::max(mesh.bounds_max[a], mesh.positions[v + size_t(a)]);
      }
    }
  }
  welded_.clear();
  opaque_.clear();
  alpha_.clear();
  opaque_runs_.clear();
  alpha_runs_.clear();
  positions_.clear();
  return mesh;
}

}  // namespace larecomp::citypack

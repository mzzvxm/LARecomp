#include "objmesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

namespace mc::modloader {

namespace {

struct Key {
    int position = 0;
    int uv = 0;
    int normal = 0;

    bool operator<(const Key& other) const {
        if (position != other.position) return position < other.position;
        if (uv != other.uv) return uv < other.uv;
        return normal < other.normal;
    }
};

// Resolves an .obj index, which is 1-based and may be negative (relative to the
// end of the list). Returns -1 when absent.
int ResolveIndex(const std::string& token, size_t count) {
    if (token.empty()) return -1;
    const long value = std::strtol(token.c_str(), nullptr, 10);
    if (value > 0) return static_cast<int>(value - 1);
    if (value < 0) return static_cast<int>(static_cast<long>(count) + value);
    return -1;
}

Key ParseFaceVertex(const std::string& token, size_t positions, size_t uvs, size_t normals) {
    std::string parts[3];
    int slot = 0;
    for (char c : token) {
        if (c == '/') {
            if (++slot > 2) break;
            continue;
        }
        parts[slot].push_back(c);
    }
    return Key{ResolveIndex(parts[0], positions), ResolveIndex(parts[1], uvs),
               ResolveIndex(parts[2], normals)};
}

}  // namespace

void Mesh::Bounds(float min_out[3], float max_out[3]) const {
    for (int i = 0; i < 3; ++i) {
        min_out[i] = std::numeric_limits<float>::max();
        max_out[i] = -std::numeric_limits<float>::max();
    }
    for (const auto& vertex : vertices) {
        const float p[3] = {vertex.px, vertex.py, vertex.pz};
        for (int i = 0; i < 3; ++i) {
            min_out[i] = std::min(min_out[i], p[i]);
            max_out[i] = std::max(max_out[i], p[i]);
        }
    }
    if (vertices.empty()) {
        for (int i = 0; i < 3; ++i) min_out[i] = max_out[i] = 0.0f;
    }
}

bool LoadObj(const std::filesystem::path& path, Mesh& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }

    std::vector<float> positions;  // xyz triples
    std::vector<float> uvs;        // uv pairs
    std::vector<float> normals;    // xyz triples
    std::map<Key, uint32_t> unique;

    out.vertices.clear();
    out.indices.clear();

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream stream(line);
        std::string tag;
        stream >> tag;

        if (tag == "v") {
            float x = 0, y = 0, z = 0;
            stream >> x >> y >> z;
            positions.insert(positions.end(), {x, y, z});
        } else if (tag == "vt") {
            float u = 0, v = 0;
            stream >> u >> v;
            uvs.insert(uvs.end(), {u, v});
        } else if (tag == "vn") {
            float x = 0, y = 0, z = 0;
            stream >> x >> y >> z;
            normals.insert(normals.end(), {x, y, z});
        } else if (tag == "f") {
            std::vector<uint32_t> face;
            std::string token;
            while (stream >> token) {
                const Key key = ParseFaceVertex(token, positions.size() / 3, uvs.size() / 2,
                                                normals.size() / 3);
                if (key.position < 0 ||
                    static_cast<size_t>(key.position) * 3 + 2 >= positions.size()) {
                    continue;
                }

                auto it = unique.find(key);
                if (it == unique.end()) {
                    MeshVertex vertex;
                    vertex.px = positions[key.position * 3 + 0];
                    vertex.py = positions[key.position * 3 + 1];
                    vertex.pz = positions[key.position * 3 + 2];
                    if (key.uv >= 0 && static_cast<size_t>(key.uv) * 2 + 1 < uvs.size()) {
                        vertex.u = uvs[key.uv * 2 + 0];
                        // .obj UVs run bottom-up, the game samples top-down.
                        vertex.v = 1.0f - uvs[key.uv * 2 + 1];
                    }
                    if (key.normal >= 0 &&
                        static_cast<size_t>(key.normal) * 3 + 2 < normals.size()) {
                        vertex.nx = normals[key.normal * 3 + 0];
                        vertex.ny = normals[key.normal * 3 + 1];
                        vertex.nz = normals[key.normal * 3 + 2];
                    }
                    it = unique.emplace(key, static_cast<uint32_t>(out.vertices.size())).first;
                    out.vertices.push_back(vertex);
                }
                face.push_back(it->second);
            }

            // Fan-triangulate whatever polygon came in.
            for (size_t i = 2; i < face.size(); ++i) {
                out.indices.push_back(face[0]);
                out.indices.push_back(face[i - 1]);
                out.indices.push_back(face[i]);
            }
        }
    }

    if (out.vertices.empty() || out.indices.empty()) {
        error = "no geometry in " + path.filename().string();
        return false;
    }
    return true;
}

namespace {

// Welds every vertex that lands in the same cell of a `resolution`^3 grid,
// averaging what falls together, then rebuilds the triangles and throws away
// the ones that collapsed onto an edge or a point.
//
// Vertices from different primitives never weld to each other, however close
// they sit. Once the textures have been packed into an atlas each primitive owns
// one square of it, and averaging the UVs of a face vertex with a jacket vertex
// that happens to share a grid cell lands the result between the two squares --
// on a model decimated hard enough, a fifth of the triangles end up straddling
// cells and sampling whatever is next door. Keeping the primitives apart also
// preserves the seams the model was built with, which is what those boundaries
// are.
Mesh ClusterMesh(const Mesh& mesh, int resolution) {
    float min[3], max[3];
    mesh.Bounds(min, max);

    float extent = 0.0f;
    for (int i = 0; i < 3; ++i) extent = std::max(extent, max[i] - min[i]);
    if (extent <= 0.0f) return mesh;

    const float cell = extent / static_cast<float>(resolution);

    std::vector<int> island(mesh.vertices.size(), 0);
    for (size_t p = 0; p < mesh.parts.size(); ++p) {
        const MeshPart& part = mesh.parts[p];
        const uint32_t end = std::min<uint32_t>(part.first_vertex + part.vertex_count,
                                                static_cast<uint32_t>(island.size()));
        for (uint32_t v = part.first_vertex; v < end; ++v) island[v] = static_cast<int>(p);
    }

    struct Accumulator {
        uint32_t index = 0;
        int count = 0;
        float px = 0, py = 0, pz = 0;
        float nx = 0, ny = 0, nz = 0;
        float u = 0, v = 0;
    };

    std::map<std::array<int, 4>, Accumulator> cells;
    std::vector<uint32_t> remap(mesh.vertices.size(), 0);

    Mesh out;
    out.joint_bind = mesh.joint_bind;
    out.joint_parent = mesh.joint_parent;
    const bool skinned = mesh.skinned();
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const MeshVertex& vertex = mesh.vertices[i];
        const std::array<int, 4> key = {
            static_cast<int>(std::floor((vertex.px - min[0]) / cell)),
            static_cast<int>(std::floor((vertex.py - min[1]) / cell)),
            static_cast<int>(std::floor((vertex.pz - min[2]) / cell)),
            island[i],
        };

        auto [it, inserted] = cells.emplace(key, Accumulator{});
        if (inserted) {
            it->second.index = static_cast<uint32_t>(out.vertices.size());
            out.vertices.push_back(MeshVertex{});
            // Weights are not averaged: blending influences of vertices that
            // merely landed in the same cell invents attachments that were
            // never authored. The first one in wins.
            if (skinned) out.skin.push_back(mesh.skin[i]);
        }

        Accumulator& cluster = it->second;
        ++cluster.count;
        cluster.px += vertex.px; cluster.py += vertex.py; cluster.pz += vertex.pz;
        cluster.nx += vertex.nx; cluster.ny += vertex.ny; cluster.nz += vertex.nz;
        cluster.u += vertex.u;   cluster.v += vertex.v;
        remap[i] = cluster.index;
    }

    for (const auto& [key, cluster] : cells) {
        const float inverse = 1.0f / static_cast<float>(cluster.count);
        MeshVertex& vertex = out.vertices[cluster.index];
        vertex.px = cluster.px * inverse;
        vertex.py = cluster.py * inverse;
        vertex.pz = cluster.pz * inverse;
        vertex.u = cluster.u * inverse;
        vertex.v = cluster.v * inverse;

        const float length =
            std::sqrt(cluster.nx * cluster.nx + cluster.ny * cluster.ny + cluster.nz * cluster.nz);
        if (length > 1e-6f) {
            vertex.nx = cluster.nx / length;
            vertex.ny = cluster.ny / length;
            vertex.nz = cluster.nz / length;
        } else {
            vertex.ny = 1.0f;
        }
    }

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = remap[mesh.indices[i]];
        const uint32_t b = remap[mesh.indices[i + 1]];
        const uint32_t c = remap[mesh.indices[i + 2]];
        if (a == b || b == c || a == c) continue;
        out.indices.insert(out.indices.end(), {a, b, c});
    }

    return out;
}

}  // namespace

bool SimplifyMeshToFit(Mesh& mesh, size_t max_vertices, size_t max_indices) {
    if (mesh.vertices.size() <= max_vertices && mesh.indices.size() <= max_indices) return true;

    // Finer grids keep more vertices, so search for the finest one that fits.
    Mesh best;
    bool found = false;
    int low = 2, high = 256;
    while (low <= high) {
        const int middle = low + (high - low) / 2;
        Mesh candidate = ClusterMesh(mesh, middle);
        if (candidate.vertices.size() <= max_vertices &&
            candidate.indices.size() <= max_indices && !candidate.indices.empty()) {
            best = std::move(candidate);
            found = true;
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }

    if (!found) return false;
    mesh = std::move(best);
    return true;
}

void FitMeshToBox(Mesh& mesh, const float target_min[3], const float target_max[3]) {
    if (mesh.vertices.empty()) return;

    float min[3], max[3];
    mesh.Bounds(min, max);

    const float source_height = max[1] - min[1];
    const float target_height = target_max[1] - target_min[1];
    if (source_height <= 1e-6f || target_height <= 1e-6f) return;
    const float scale = target_height / source_height;

    const float source_center[3] = {(min[0] + max[0]) * 0.5f, min[1], (min[2] + max[2]) * 0.5f};
    const float target_center[3] = {(target_min[0] + target_max[0]) * 0.5f, target_min[1],
                                    (target_min[2] + target_max[2]) * 0.5f};

    for (auto& vertex : mesh.vertices) {
        vertex.px = (vertex.px - source_center[0]) * scale + target_center[0];
        vertex.py = (vertex.py - source_center[1]) * scale + target_center[1];
        vertex.pz = (vertex.pz - source_center[2]) * scale + target_center[2];
    }
    for (size_t j = 0; j + 2 < mesh.joint_bind.size(); j += 3) {
        for (int i = 0; i < 3; ++i) {
            mesh.joint_bind[j + i] =
                (mesh.joint_bind[j + i] - source_center[i]) * scale + target_center[i];
        }
    }
}

void TransformMesh(Mesh& mesh, float yaw, float pitch, float roll, float scale) {
    const float to_radians = 3.14159265358979323846f / 180.0f;
    const float cy = std::cos(yaw * to_radians), sy = std::sin(yaw * to_radians);
    const float cp = std::cos(pitch * to_radians), sp = std::sin(pitch * to_radians);
    const float cr = std::cos(roll * to_radians), sr = std::sin(roll * to_radians);

    auto apply = [&](float& x, float& y, float& z, bool translate) {
        float ax = x, ay = y, az = z;
        float bx = ax * cy + az * sy, by = ay, bz = -ax * sy + az * cy;   // yaw, about Y
        float cx = bx, cyv = by * cp - bz * sp, cz = by * sp + bz * cp;   // pitch, about X
        float dx = cx * cr - cyv * sr, dy = cx * sr + cyv * cr, dz = cz;  // roll, about Z
        const float factor = translate ? scale : 1.0f;
        x = dx * factor; y = dy * factor; z = dz * factor;
    };

    for (auto& vertex : mesh.vertices) {
        apply(vertex.px, vertex.py, vertex.pz, true);
        apply(vertex.nx, vertex.ny, vertex.nz, false);
    }
    for (size_t j = 0; j + 2 < mesh.joint_bind.size(); j += 3) {
        apply(mesh.joint_bind[j], mesh.joint_bind[j + 1], mesh.joint_bind[j + 2], true);
    }
}

}  // namespace mc::modloader

// Minimal Wavefront .obj reader.
//
// Only what a character swap needs: positions, normals, the first UV channel
// and triangles. Materials, groups and smoothing are ignored -- MCLA's drawable
// keeps its own shader group, so the mod mesh inherits the original materials.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::modloader {

struct MeshVertex {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
};

// Up to four influences per vertex, in the file's own joint numbering.
struct MeshSkin {
    uint16_t joint[4] = {0, 0, 0, 0};
    float weight[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// One primitive of the source file. An exporter splits a character into one
// primitive per material -- body, head, shoes, legs -- and the geometry rewrite
// merges them all into a single submesh drawn by a single shader. Recording
// which vertices came from which primitive, and which image its material used,
// is what lets the atlas send each of them to its own corner of one texture.
struct MeshPart {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    int image = -1;  // index into Mesh::images, -1 when the material has none

    // The named node this primitive hangs under, when the file has one. A
    // character does not need it, but a car is delivered as one file holding
    // every part of it -- hood, doors, each bumper variant -- and the node names
    // are what says which is which. Empty for a .obj and for a glTF whose
    // primitives have no named ancestor.
    std::string group;
};

struct Mesh {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;

    // Encoded (PNG/JPEG) bytes exactly as the source file carried them, and the
    // primitives that reference them. Both empty for a .obj.
    std::vector<std::vector<uint8_t>> images;
    std::vector<MeshPart> parts;

    // Both empty for an unskinned mesh (.obj). `skin` runs parallel to
    // `vertices`; `joint_bind` holds the bind-pose position of each joint, three
    // floats per joint, and is what the bone matching keys off.
    std::vector<MeshSkin> skin;
    std::vector<float> joint_bind;
    // Inverse bind matrix per joint, 16 floats, column-major as glTF stores it.
    std::vector<float> joint_inverse_bind;
    // Parent of each joint, in joint numbering; -1 for a root. The bind
    // positions alone cannot say which rig a joint belongs to -- a T-posed
    // wrist and an A-posed forearm sit in the same place -- so the retarget
    // walks the hierarchy instead of measuring distances.
    std::vector<int> joint_parent;
    // What the source file called each joint. Nothing reads it to decide
    // anything -- the retarget is deliberately name-blind, because rigs do not
    // agree on names -- but a diagnostic dump is unreadable without it.
    std::vector<std::string> joint_name;

    bool skinned() const { return !skin.empty() && !joint_bind.empty(); }
    size_t joint_count() const { return joint_bind.size() / 3; }

    void Bounds(float min_out[3], float max_out[3]) const;
};

bool LoadObj(const std::filesystem::path& path, Mesh& out, std::string& error);

// Scales the mesh so its height matches the target box and stands on its floor,
// centred on X and Z. Height is the one dimension of a humanoid that means the
// same thing in every rig: matching the widest axis instead shrinks a T-posed
// character to a child, because arms out make it wider than it is tall.
void FitMeshToBox(Mesh& mesh, const float target_min[3], const float target_max[3]);

// Rotates the mesh about the origin (degrees, applied yaw then pitch then roll)
// and scales it, before it is fitted. Source models rarely agree with the
// game on which way is forward.
void TransformMesh(Mesh& mesh, float yaw, float pitch, float roll, float scale);

// Reduces the mesh until it fits within `max_vertices` and `max_indices`, by
// welding vertices onto a grid and dropping the triangles that collapse. The
// grid is searched for the finest spacing that still fits, so the result is as
// detailed as the slot allows. A no-op when the mesh already fits. Returns
// false only if even the coarsest grid cannot get there.
//
// This is what lets one .obj serve every LOD of a character: the shipped low
// LOD has room for a few hundred vertices, and asking people to prepare a
// separate decimated model for it is a worse answer than doing it here.
bool SimplifyMeshToFit(Mesh& mesh, size_t max_vertices, size_t max_indices);

}  // namespace mc::modloader

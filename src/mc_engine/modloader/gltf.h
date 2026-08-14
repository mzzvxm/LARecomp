// glTF 2.0 reader, .gltf and .glb, limited to what a character swap needs.
//
// The point of supporting glTF at all is skinning: unlike .obj it carries
// JOINTS_0 / WEIGHTS_0 per vertex plus a skin whose inverse bind matrices say
// where each joint sits. That is the only way a replacement mesh can follow the
// driver's seated pose instead of riding one bone rigidly.
//
// The mod's skeleton is not MCLA's, and MCLA's resource stores bone name hashes
// rather than names, so joints are matched to bones by bind-pose position --
// see RetargetJoints in rsc5.cpp.
//
// Reads the first skinned primitive it finds (or the first primitive at all if
// nothing is skinned). Triangles only; materials, animations, morph targets,
// cameras and scene transforms are ignored.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "objmesh.h"

namespace mc::modloader {

// Fills `out`, including `out.skin` and `out.joint_bind` when the file has a
// skinned primitive. Positions come through in the file's own units and space;
// the caller fits them to the drawable afterwards.
bool LoadGltf(const std::filesystem::path& path, Mesh& out, std::string& error);

}  // namespace mc::modloader

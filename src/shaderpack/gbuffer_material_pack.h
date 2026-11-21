#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the G-buffer's material table (package GB-F1, src/gpu/gbuffer_material.h)
// ===========================================================================
// The renderer knows a shader by its identity (a hash of the microcode); the game's names (group and entry point: xCityRoad__PSMultiLight) are only known while the pack is built
// from the player's own game files. The material table is what the build derives from them for the G-buffer: per shader, its surface class and flags (the table of groups in
// gpu/gbuffer_material.h), its default smoothness and lit weight, and where its draw's constant bank holds the game's SpecularExponent (the per-draw gloss). One file,
// mcla_gbuffer_materials.v1.bin (gpu/gbuffer_material.h kMaterialTableFileName), beside the variant pack, written by the variant step right after the variant pack (variant_pack.cpp
// BuildChecked) from the same containers; the renderer reads it once (gpu/gbuffer_material.h MaterialTable, Scene::FillVariantBlock's G-buffer case).
//
// Beside it goes the shader names file (mcla_gbuffer_shader_names.v1.tsv, gpu/gbuffer_materials.h FormatShaderNamesTsv): every shader's identity, group, entry and class, which
// tools/gbuffer_materials/census_merge.py joins to the identities the renderer's texture census logs. Nothing reads it at run time.
//
// Failure never fails the step: without the file every draw takes the default material word (world class, smoothness 0, lit), which is what the G-buffer did before this existed.
// The step's stamp has the line variants_gbuffer_material_version (gpu/gbuffer_material.h kMaterialRulesVersion, hand bumped when a table changes), so an edit to the tables
// rebuilds the variant pack and this file, and nothing else.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "shaderpack/containers.h"

namespace larecomp::shaderpack {

struct MaterialTableResult {
  bool written = false;
  size_t records = 0;
  std::string message;  // a one line summary when written, why not otherwise
};

// Writes <out_dir>/mcla_gbuffer_materials.v1.bin (a temporary name, then moved into place) for the shaders of `containers`. Pure of the compiler: it only looks at names and identities.
MaterialTableResult WriteGBufferMaterialTable(const ContainerSet& containers, const std::filesystem::path& out_dir);

// The number in the stamp line variants_gbuffer_material_version.
uint32_t GBufferMaterialRulesVersion();

}  // namespace larecomp::shaderpack

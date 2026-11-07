#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the shader containers, and what the pack says about each
// ===========================================================================
// One pass over every shader container the game has (the RPF's Shaders folder, rpf_shaders.h, and
// the executable's image, image_scan.h) that works out, per container, everything the pack records
// that comes from the Xbox 360 microcode and not from the translated HLSL:
//
//   key                 FNV-1a 64 of the microcode with every vertex fetch's payload zeroed: the
//                       runtime's own function (native_gfx NormalizeVertexFetches + Fnv1a64,
//                       shader_identity.h), so the runtime finds the shader by what the game draws
//   stage               from the container's flags (bit 0 set: vertex), as the translator decides
//   vertex elements     the VertexShader struct's element words (address:12 | usage:4 | index:4)
//   flags               bit 0 a vertex fetch takes its index from something other than r0.x
//                       (native_gfx ProbeComputedVertexFetchIndex, on the raw microcode);
//                       bits 8..12 the pixel shader's outputs (PixelShader::outputs)
//   texture fetch mask  the fetch constants a texture instruction reads
//
// Containers that give the same key are one shader: the first found wins (the RPF files in path
// order, then the image). Which of them wins does not matter -- on the retail game's 173 keys with
// more than one container, the HLSL and every field above are identical -- but the order is fixed,
// so the result never depends on timing.
//
// A port of collect_full (build_shader_pack_v8.py, with v2's normalize/collect_shaders/
// read_vertex_elements, v3's computed_fetch_index, v8's pixel_shader_outputs/texture_fetch_mask),
// by mzzvxm; the microcode walks are the Python's, instruction for instruction.
//
// No logging, no threads.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/dxc.h"
#include "shaderpack/image_scan.h"
#include "shaderpack/rpf_shaders.h"

namespace larecomp::shaderpack {

// What one container says by itself.
struct ContainerInfo {
  ShaderStage stage = ShaderStage::kPixel;
  uint64_t key = 0;
  uint32_t shader_offset = 0;      // ShaderContainer::shaderOffset
  uint32_t flags = 0;              // pack flags (see above)
  uint32_t texture_fetch_mask = 0;
  std::vector<uint32_t> vertex_elements;  // vertex shaders only
};

// False with *error set when the container is too short for the structs it points to, or its
// microcode is larger than the runtime accepts (native_gfx kMaxUcodeBytes).
bool AnalyzeContainer(std::span<const uint8_t> container, ContainerInfo* info, std::string* error);

// The two microcode walks, public for the tests. `ucode` is the raw big endian microcode.
uint32_t TextureFetchMask(const uint8_t* ucode, size_t size);

// One unique shader: the first container that gave its key.
struct ShaderSource {
  uint64_t key = 0;
  ShaderStage stage = ShaderStage::kPixel;
  std::string name;    // "group__entry" from the .updb path inside the container, else "unnamed_<key>"
  std::string origin;  // where the container was found, for messages
  std::vector<uint8_t> container;
  uint32_t shader_offset = 0;
  uint32_t flags = 0;
  uint32_t texture_fetch_mask = 0;
  std::vector<uint32_t> vertex_elements;
};

struct ContainerSet {
  std::vector<ShaderSource> shaders;  // strictly increasing key

  size_t rpf_containers = 0;        // containers found in the RPF files
  size_t image_containers = 0;      // containers found in the image
  size_t duplicate_containers = 0;  // containers whose key was already taken
  size_t vertex_shaders = 0;
  size_t pixel_shaders = 0;
  // XXH3-64 over the containers found (size, then bytes, in the order found): the stamp's proof that
  // the game files are the ones the pack was built from. 0 for a source that was not read.
  uint64_t rpf_fingerprint = 0;
  uint64_t image_fingerprint = 0;

  const ShaderSource* Find(uint64_t key) const;
  // The first shader with this name (a name is not unique: two containers of different keys can
  // carry one .updb path). Null when there is none.
  const ShaderSource* FindByName(std::string_view name) const;
};

// Called as containers are read; return false to stop. `what` names the file or the source.
using GatherProgress = std::function<bool(size_t done, size_t total, const std::string& what)>;

// Scans the RPF files (null: skipped) and the image scan (null: skipped) for containers. False
// with *error set on a malformed container or when the progress callback stopped it
// (*cancelled set then). The result is only filled when it returns true.
bool GatherContainers(const RpfShaders* rpf, const ImageScan* image, ContainerSet* out,
                      std::string* error, bool* cancelled = nullptr,
                      const GatherProgress& progress = {});

}  // namespace larecomp::shaderpack

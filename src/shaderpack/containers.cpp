// LARecomp shader pack builder -- the shader containers (see containers.h).
// Ported from native/tools/build_shader_pack*.py (mzzvxm).

#include "shaderpack/containers.h"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <utility>

#include "XenosRecomp/container_scan.h"
#include "native_gfx/shader_identity.h"
#include "shaderpack/hash.h"
#include "shaderpack/log.h"
#include "shaderpack/pack_format.h"

namespace larecomp::shaderpack {

namespace {

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// sizeof(ShaderContainer): nine big endian dwords (container_scan.h).
constexpr size_t kContainerHeaderBytes = 36;

// Control-flow opcodes that execute an instruction block, for the texture mask (v8's
// EXEC_BLOCK_OPCODES): Exec, ExecEnd, CondExec, CondExecEnd, CondExecPred, CondExecPredEnd,
// CondExecPredClean, CondExecPredCleanEnd. (NormalizeVertexFetches also takes opcode 7; the runtime's
// function owns that one.)
bool IsExecBlockOpcode(uint32_t op) {
  return (op >= 1 && op <= 6) || op == 13 || op == 14;
}

// Fetch opcodes that read the texture a fetch constant names (v8's TEXTURE_READ_OPCODES, shader_code.h
// FetchOpcode): TextureFetch, GetTextureBorderColorFrac, GetTextureComputedLod, GetTextureGradients,
// GetTextureWeights. The Set* opcodes only load registers.
bool IsTextureReadOpcode(uint32_t op) {
  return op == 1 || (op >= 16 && op <= 19);
}

// VertexShader (shader.h): after the six dword Shader header come field18, vertexElementCount and
// field20, then the array whose elements start at index field18.
bool ReadVertexElements(const uint8_t* container, uint64_t size, uint32_t shader_offset,
                        std::vector<uint32_t>* out) {
  out->clear();
  const uint64_t base = uint64_t(shader_offset) + 6 * 4;  // past struct Shader
  if (base + 8 > size) return false;
  const uint32_t field18 = Be32(container + base);
  const uint32_t count = Be32(container + base + 4);
  const uint64_t array = base + 3 * 4;  // past field18, vertexElementCount, field20
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t position = array + 4 * (uint64_t(field18) + i);
    if (position + 4 > size) break;  // the Python stops at the end of the container as well
    out->push_back(Be32(container + position));
  }
  return true;
}

std::string ShaderNameOf(const uint8_t* container, size_t size, uint64_t key) {
  const ShaderName name = extractShaderName(container, size);
  if (!name.valid) return "unnamed_" + Hex64(key);
  // The generator names its HLSL files the same way.
  std::string base = sanitizeFileName(name.group);
  if (!base.empty()) base += "__";
  base += sanitizeFileName(name.entryPoint);
  return base;
}

void HashSize(Xxh3Stream& hash, size_t size) {
  const uint8_t bytes[4] = {uint8_t(size), uint8_t(size >> 8), uint8_t(size >> 16), uint8_t(size >> 24)};
  hash.Update(bytes, sizeof(bytes));
}

}  // namespace

uint32_t TextureFetchMask(const uint8_t* ucode, size_t size) {
  const size_t n = size / 4;
  if (n < 3) return 0;

  struct Block {
    uint32_t address;
    uint32_t count;
    uint32_t sequence;
  };
  std::vector<Block> blocks;
  size_t limit = n;
  for (size_t i = 0; i + 2 < limit; i += 3) {
    const uint32_t d0 = Be32(ucode + 4 * i);
    const uint32_t d1 = Be32(ucode + 4 * (i + 1));
    const uint32_t d2 = Be32(ucode + 4 * (i + 2));
    // Two 48 bit control-flow instructions in three dwords.
    const uint32_t w0[2] = {d0, (d1 >> 16) | (d2 << 16)};
    const uint32_t w1[2] = {d1 & 0xFFFFu, d2 >> 16};
    for (int k = 0; k < 2; ++k) {
      if (!IsExecBlockOpcode((w1[k] >> 12) & 0xF)) continue;
      const uint32_t address = w0[k] & 0xFFFu;
      if (address != 0) limit = std::min(limit, size_t(address) * 3);
      // The Python keeps a block at address 0 too (normalize does not).
      blocks.push_back(Block{address, (w0[k] >> 12) & 0x7u, (w0[k] >> 16) & 0xFFFu});
    }
  }

  uint32_t mask = 0;
  for (const Block& block : blocks) {
    for (uint32_t k = 0; k < block.count; ++k) {
      if (((block.sequence >> (2 * k)) & 1u) == 0) continue;  // ALU
      const size_t base = (size_t(block.address) + k) * 3;
      if (base + 2 >= n) continue;
      const uint32_t f0 = Be32(ucode + 4 * base);
      if (IsTextureReadOpcode(f0 & 0x1Fu)) mask |= 1u << ((f0 >> 20) & 0x1Fu);
    }
  }
  return mask;
}

bool AnalyzeContainer(std::span<const uint8_t> container, ContainerInfo* info, std::string* error) {
  *info = ContainerInfo{};
  const uint8_t* c = container.data();
  const uint64_t size = container.size();
  if (size < kContainerHeaderBytes) {
    *error = "shorter than a container header";
    return false;
  }
  const uint32_t container_flags = Be32(c);
  const uint32_t virtual_size = Be32(c + 4);
  const uint32_t shader_offset = Be32(c + 24);
  // The translator's own rule: bit 0 of the container's flags set is a vertex shader.
  info->stage = (container_flags & 1u) == 0 ? ShaderStage::kPixel : ShaderStage::kVertex;
  info->shader_offset = shader_offset;
  if (uint64_t(shader_offset) + 8 > size) {
    *error = "the shader struct lies outside the container";
    return false;
  }

  // The microcode: `usize` bytes at virtualSize + physOff, cut at the end of the container like a
  // Python slice.
  const uint32_t physical_offset = Be32(c + shader_offset);
  const uint32_t microcode_size = Be32(c + shader_offset + 4);
  const uint64_t begin = std::min<uint64_t>(uint64_t(virtual_size) + physical_offset, size);
  const uint64_t end =
      std::min<uint64_t>(uint64_t(virtual_size) + physical_offset + microcode_size, size);
  if (end - begin > mcla::native_gfx::kMaxUcodeBytes) {
    *error = std::format("the microcode is {} bytes, more than the runtime accepts ({})", end - begin,
                         mcla::native_gfx::kMaxUcodeBytes);
    return false;
  }
  std::vector<uint8_t> microcode(c + begin, c + end);

  uint32_t flags = 0;
  // Both on the raw microcode: normalizing clears the source register the probe reads.
  info->texture_fetch_mask = TextureFetchMask(microcode.data(), microcode.size());
  if (info->stage == ShaderStage::kVertex) {
    if (mcla::native_gfx::ProbeComputedVertexFetchIndex(microcode.data(), microcode.size())) {
      flags |= kFlagComputedFetchIndex;
    }
    if (!ReadVertexElements(c, size, shader_offset, &info->vertex_elements)) {
      *error = "the vertex shader struct lies outside the container";
      return false;
    }
  } else {
    // PixelShader::outputs, after the six dword Shader header and field18.
    if (uint64_t(shader_offset) + 32 > size) {
      *error = "the pixel shader struct lies outside the container";
      return false;
    }
    flags |= (Be32(c + shader_offset + 28) & kFlagPixelOutputsMask) << kFlagPixelOutputsShift;
  }
  info->flags = flags;

  mcla::native_gfx::NormalizeVertexFetches(microcode.data(), microcode.size());
  info->key = mcla::native_gfx::Fnv1a64(microcode.data(), microcode.size());
  return true;
}

const ShaderSource* ContainerSet::Find(uint64_t key) const {
  const auto it = std::lower_bound(shaders.begin(), shaders.end(), key,
                                   [](const ShaderSource& shader, uint64_t value) { return shader.key < value; });
  return it != shaders.end() && it->key == key ? &*it : nullptr;
}

const ShaderSource* ContainerSet::FindByName(std::string_view name) const {
  for (const ShaderSource& shader : shaders) {
    if (shader.name == name) return &shader;
  }
  return nullptr;
}

bool GatherContainers(const RpfShaders* rpf, const ImageScan* image, ContainerSet* out,
                      std::string* error, bool* cancelled, const GatherProgress& progress) {
  if (cancelled) *cancelled = false;
  ContainerSet set;
  std::unordered_map<uint64_t, size_t> seen;  // key -> index in set.shaders
  Xxh3Stream rpf_hash;
  Xxh3Stream image_hash;

  const auto add = [&](std::span<const uint8_t> container, Xxh3Stream& hash, const std::string& origin,
                       size_t* counter) {
    ++*counter;
    HashSize(hash, container.size());
    hash.Update(container.data(), container.size());

    ContainerInfo info;
    std::string why;
    if (!AnalyzeContainer(container, &info, &why)) {
      *error = std::format("shader container at {}: {}", origin, why);
      return false;
    }
    if (!seen.try_emplace(info.key, set.shaders.size()).second) {
      ++set.duplicate_containers;
      return true;
    }
    ShaderSource source;
    source.key = info.key;
    source.stage = info.stage;
    source.name = ShaderNameOf(container.data(), container.size(), info.key);
    source.origin = origin;
    source.container.assign(container.begin(), container.end());
    source.shader_offset = info.shader_offset;
    source.flags = info.flags;
    source.texture_fetch_mask = info.texture_fetch_mask;
    source.vertex_elements = std::move(info.vertex_elements);
    set.shaders.push_back(std::move(source));
    return true;
  };

  if (rpf) {
    const size_t total = rpf->files.size();
    for (size_t i = 0; i < total; ++i) {
      const RpfShaderFile& file = rpf->files[i];
      if (progress && !progress(i, total, file.path)) {
        if (cancelled) *cancelled = true;
        *error = "cancelled while reading the shader containers";
        return false;
      }
      bool ok = true;
      forEachShaderContainer(file.bytes.data(), file.bytes.size(), [&](size_t offset, size_t data_size) {
        if (!ok) return;
        ok = add(std::span<const uint8_t>(file.bytes.data() + offset, data_size), rpf_hash,
                 std::format("{} +0x{:X}", file.path, offset), &set.rpf_containers);
      });
      if (!ok) return false;
    }
    if (progress) progress(total, total, std::string());
    set.rpf_fingerprint = rpf_hash.Digest();
  }

  if (image) {
    for (const ImageContainer& container : image->containers) {
      if (!add(std::span<const uint8_t>(container.bytes), image_hash,
               std::format("image 0x{:08X}", container.guest_address), &set.image_containers)) {
        return false;
      }
    }
    set.image_fingerprint = image_hash.Digest();
  }

  std::sort(set.shaders.begin(), set.shaders.end(),
            [](const ShaderSource& a, const ShaderSource& b) { return a.key < b.key; });
  for (const ShaderSource& shader : set.shaders) {
    (shader.stage == ShaderStage::kVertex ? set.vertex_shaders : set.pixel_shaders)++;
  }
  *out = std::move(set);
  return true;
}

}  // namespace larecomp::shaderpack

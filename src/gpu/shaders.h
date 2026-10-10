#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest shader store (shader pack v3)
// ===========================================================================
// The translated guest shaders (LAR-XenosRecomp HLSL compiled offline by
// tools/build_shader_pack_v3.py), keyed by the runtime shader identity
// (FNV-1a 64 of the vfetch-normalized microcode, native_gfx/shader_identity.h).
// Layout of the file: and the builder's
// header. v3 carries v2's DXIL byte for byte and SPIR-V for every shader.
//
// The pack is memory-mapped read-only from <exe folder>/assets (the legacy
// runtime opens v2 relative to the working directory, a known pending issue);
// only the pages of the blobs actually used are read. The header, every entry
// and every range are validated at open; a blob's FNV and magic are verified
// the first time it is used, and a bad blob marks its entry unusable, logged
// with its key.
//
// Shaders: one plume::RenderShader per blob, created on first use on the
// render thread through Device::CreateShader (which rejects a null-backed
// result). D3D12 has one DXIL blob per spec variant present; Vulkan has one
// SPIR-V blob per shader, its spec constants set when a pipeline is created.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

namespace rex::memory {
class MappedMemory;
}

namespace mcla::gpu {

class Device;

class ShaderStore {
 public:
  static constexpr const char* kPackFileName = "mcla_shaders.v3.pack";
  static constexpr uint32_t kPackVersion = 3;

  enum class Stage : uint8_t { kVertex, kPixel };

  struct Blob {
    uint32_t offset = 0;
    uint32_t size = 0;
    uint64_t fnv = 0;
  };

  enum EntryFlags : uint32_t {
    kComputedFetchIndex = 1u << 0,  // a vfetch takes its index from something other than r0.x
  };

  struct Entry {
    uint64_t key = 0;
    Stage stage = Stage::kVertex;
    uint8_t spec_mask = 0;  // spec bits the shader uses (VS 0x1 R11G11B10_NORMAL, PS 0x2 ALPHA_TEST)
    uint16_t velem_count = 0;
    uint32_t velem_offset = 0;
    uint32_t flags = 0;
    Blob dxil[2];  // spec 0 / spec = spec_mask
    Blob spirv;
  };

  struct VertexElement {
    uint32_t address = 0;  // vfetch instruction address in the microcode
    uint32_t usage = 0;    // D3DDECLUSAGE
    uint32_t usage_index = 0;
  };

  enum class ShaderResult : uint8_t {
    kOk,
    kNoVariant,     // the variant asked for was not compiled for this shader
    kBadBlob,       // FNV or magic mismatch (logged once)
    kCreateFailed,  // the backend refused it (logged once)
  };
  static const char* ResultName(ShaderResult result);

  struct Stats {
    uint64_t shaders_created = 0;
    uint64_t blobs_verified = 0;
    uint64_t blob_failures = 0;
    uint64_t create_failures = 0;
  };

  ShaderStore();
  ~ShaderStore();
  ShaderStore(const ShaderStore&) = delete;
  ShaderStore& operator=(const ShaderStore&) = delete;

  // Opens and validates the pack. False (logged) when it is missing or malformed.
  bool Initialize(Device& device);
  // The GPU must be idle: shaders are destroyed directly.
  void Shutdown();

  uint32_t entry_count() const { return uint32_t(entries_.size()); }
  const std::filesystem::path& path() const { return path_; }
  const Stats& stats() const { return stats_; }

  const Entry* Find(uint64_t identity) const;
  // Appends the vertex shader's elements to `out` (cleared first).
  void GetVertexElements(const Entry& entry, std::vector<VertexElement>& out) const;
  // The shader for `entry` with spec `variant` (0, or 1 = spec_mask applied),
  // created on first use. Null with the reason in `result` otherwise.
  plume::RenderShader* GetShader(const Entry& entry, uint32_t variant, ShaderResult* result);

 private:
  struct Slot {
    std::unique_ptr<plume::RenderShader> shader;
    ShaderResult result = ShaderResult::kOk;
    bool done = false;
  };

  bool Validate();
  bool VerifyBlob(const Entry& entry, const Blob& blob, bool spirv);

  Device* device_ = nullptr;
  std::filesystem::path path_;
  std::unique_ptr<rex::memory::MappedMemory> file_;
  std::unordered_map<uint64_t, Entry> entries_;
  std::unordered_map<uint64_t, Slot> slots_;  // (identity, variant) for DXIL, identity for SPIR-V
  Stats stats_;
};

}  // namespace mcla::gpu

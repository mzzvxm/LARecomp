#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — shader database
// ===========================================================================
// Loads the offline-built shader pack (tools/build_shader_pack.py) and
// resolves runtime shader identities to precompiled DXIL:
//
//   guest VS/PS microcode
//        -> ShaderIdentity (vfetch-normalized FNV-1a 64, shader_identity.h)
//        -> pack entry
//        -> variant blob (spec constants)
//        -> D3D12_SHADER_BYTECODE
//
// Variants, as shipped (confirmed over all 2333 translated shaders):
//   VS: spec 0x0 or 0x1 (SPEC_CONSTANT_R11G11B10_NORMAL)
//   PS: spec 0x2 on every pixel shader (SPEC_CONSTANT_ALPHA_TEST)
// Variant 0 is always the spec_mask == 0 compile; variant 1, when present,
// is the single available spec bit enabled. Lookup picks by requested mask;
// an unavailable mask falls back to variant 0 with a one-time warning.
//
// Every blob is integrity-checked at load (FNV recorded by the builder,
// DXBC magic) so a truncated or stale pack fails loudly at startup instead
// of producing undefined GPU behavior later.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

struct ShaderBytecode {
  const void* data = nullptr;
  uint32_t size = 0;
  bool valid() const { return data != nullptr && size != 0; }
};

class ShaderDatabase {
 public:
  // Loads and verifies the pack. Searches "assets/mcla_shaders.pack" and
  // "mcla_shaders.pack" relative to the working directory. Returns false
  // (and logs why) on any failure; the database stays empty and every
  // lookup misses.
  bool Load();

  // Resolves a runtime shader identity to DXIL. `spec_mask` is the desired
  // spec-constant state (0, 1 for VS R11G11B10, 2 for PS alpha test).
  // `is_pixel` cross-checks the entry's stage. Misses are counted and
  // logged once per key.
  ShaderBytecode Lookup(uint64_t identity, uint32_t spec_mask, bool is_pixel);

  // Vertex elements of a vertex shader: the container's declaration table,
  // keyed by the microcode address of the vfetch instruction that reads the
  // attribute. This is what turns a decoded vfetch into an input-layout
  // element with a real semantic.
  struct VertexElementRef {
    uint32_t address = 0;      // vfetch instruction address
    uint32_t usage = 0;        // DeclUsage
    uint32_t usage_index = 0;
  };
  // Returns the table for `identity`, or an empty span for non-VS / unknown
  // keys. The pointer stays valid for the lifetime of the database.
  const VertexElementRef* GetVertexElements(uint64_t identity, uint32_t* out_count);

  bool loaded() const { return loaded_; }
  uint32_t entry_count() const { return entry_count_; }
  uint64_t miss_count() const { return miss_count_; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool loaded_ = false;
  uint32_t entry_count_ = 0;
  uint64_t miss_count_ = 0;
};

}  // namespace mcla::native_gfx

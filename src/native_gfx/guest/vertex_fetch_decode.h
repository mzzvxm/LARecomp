#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — vertex fetch instruction decoding
// ===========================================================================
// Decodes the vfetch instructions of a shader's microcode.
//
// IMPORTANT — this is NOT the source of truth for the vertex input layout.
// The D3D runtime patches vfetch instructions from the bound vertex
// declaration, but it does so in one of TWO places:
//   * in place, in the shader's canonical microcode, when that copy is free;
//   * into a COPY inside the command buffer (sub_82424320, emitted with
//     PM4 IM_LOAD_IMMEDIATE) when the canonical copy is still referenced by
//     in-flight GPU work — the canonical copy then stays unpatched.
// Which path is taken depends on GPU timing, so reading the layout back out
// of the bound microcode is right most of the time and silently wrong the
// rest. geometry.cpp therefore builds the layout from the vertex declaration
// plus the stride table, exactly like the patcher does.
//
// This decode remains useful for: shader identity/normalization, resource
// resolution, cross-checking a patched shader against the declaration, and
// reading fetch constants.
//
// Instruction layout per rex/graphics/format/ucode.h (VertexFetchInstruction).
// ===========================================================================

#include <cstdint>
#include <vector>

namespace mcla::native_gfx {

struct VertexFetchInstr {
  uint32_t address = 0;       // microcode instruction index (join key)
  uint32_t fetch_slot = 0;    // vertex fetch constant slot (0..95)
  uint32_t stride_bytes = 0;  // 0 means "packed", derived from the format
  uint32_t offset_bytes = 0;  // element offset inside the vertex
  uint32_t format = 0;        // xenos::VertexFormat
  uint32_t dst_swizzle = 0;
  bool signed_format = false;
  bool is_float = false;      // num_format: 1 = normalized/float path
};

// Decodes every kVertexFetch in `ucode` (raw big-endian guest microcode, the
// PATCHED runtime copy). Returns them in instruction-address order.
std::vector<VertexFetchInstr> DecodeVertexFetches(const uint8_t* ucode, size_t size);

// Size in bytes one element of `format` occupies.
uint32_t VertexFormatSizeBytes(uint32_t format);

// Native format for a vertex attribute. Returns DXGI_FORMAT_UNKNOWN (0) for
// anything MCLA has not been observed to use; callers must fail loudly.
uint32_t VertexFormatToDxgi(uint32_t format, bool signed_format, bool is_float);

}  // namespace mcla::native_gfx

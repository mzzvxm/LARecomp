#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — topology expansion
// ===========================================================================
// Four Xenos primitive types have no D3D12 topology and have to be turned
// into triangle lists by an index buffer the host generates:
//
//   kTriangleFan (0x05)   N verts -> a fan of N-2 triangles (D3D12 dropped fans)
//   kQuadList (0x0D)      N verts -> N/4 quads -> 6 indices per quad
//   kQuadStrip (0x0E)     N verts -> N/2-1 quads
//   kPolygon (0x0F)       N verts -> a fan of N-2 triangles
//
// Measured in MCLA: 265 non-indexed kQuadList draws per frame, every one of
// them rejected before this existed. They are the single largest block of
// geometry the native runtime was dropping.
//
// kRectangleList (0x08) needs more than indices: its three vertices describe a
// parallelogram whose fourth corner does not exist in the buffer, so the
// missing vertex is synthesised on the CPU (see SynthesiseRectList) and the
// generated indices address that FOUR-vertex-per-rect buffer.
//
// The generated buffers depend only on (primitive type, vertex count), never
// on the vertex data, so they are cached and reused across draws and frames.
// ===========================================================================

#include <cstdint>
#include <map>
#include <vector>

#include "../vertex_declaration.h"

// Not <wrl/client.h> directly: it drags in windows.h without the project's
// macro guards, and the min/max macros that come with it break rex/math.h in
// any translation unit that includes this header first.
#include <rex/ui/d3d12/d3d12_api.h>

namespace mcla::native_gfx {

class D3D12Context;

// True when the type needs an index buffer to become a triangle list.
bool IsExpandableTopology(uint32_t primitive_type);

// How many indices the expansion of `vertex_count` vertices produces. Zero
// when the count is too small to form a single primitive.
uint32_t ExpandedIndexCount(uint32_t primitive_type, uint32_t vertex_count);

class TopologyExpander {
 public:
  struct Buffer {
    uint64_t gpu_address = 0;
    uint32_t size_bytes = 0;
    uint32_t index_count = 0;
  };

  // Returns a 32-bit index buffer expanding `vertex_count` vertices of
  // `primitive_type` into a triangle list. `index_count` of zero means the
  // draw cannot be expanded and must be skipped.
  Buffer Acquire(D3D12Context& context, uint32_t primitive_type, uint32_t vertex_count);

  size_t buffers_created() const { return buffers_.size(); }

 private:
  struct Key {
    uint32_t primitive_type;
    uint32_t vertex_count;
    bool operator<(const Key& o) const {
      return primitive_type != o.primitive_type ? primitive_type < o.primitive_type
                                                : vertex_count < o.vertex_count;
    }
  };
  struct Entry {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Buffer view;
  };
  std::map<Key, Entry> buffers_;
};

// Vertices the host builds for a draw, rather than pointing at guest memory.
// A bump allocator over one upload buffer: a capture is finite and every
// allocation is consumed by the command list it was made for.
class InlineVertexRing {
 public:
  // Returns a CPU pointer to write `bytes` into, and its GPU address. Null on
  // failure or when the ring is exhausted.
  uint8_t* Allocate(D3D12Context& context, uint32_t bytes, uint64_t* out_gpu_address);
  void Reset() { used_ = 0; }
  uint32_t used() const { return used_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
  uint8_t* mapped_ = nullptr;
  uint64_t gpu_base_ = 0;
  uint32_t capacity_ = 0;
  uint32_t used_ = 0;
};

// How many vertices SynthesiseRectList writes for `vertex_count` guest ones.
uint32_t RectListVertexCount(uint32_t vertex_count);

// Expands guest RECTLIST vertices (3 per rect, big-endian, k8in32) into 4 per
// rect in host byte order, deriving the missing corner as v3 = v1 + v2 - v0.
// Float attributes are interpolated; everything else (packed colour, for
// instance) is copied from v1, which is correct for the flat-shaded screen
// quads this primitive is used for.
//
// `elements` describes the vertex layout so the two kinds of field can be told
// apart; `stride` is the vertex size in bytes.
bool SynthesiseRectList(const uint8_t* guest_vertices, uint32_t vertex_count, uint32_t stride,
                        const std::vector<InputElement>& elements, uint8_t* out);

}  // namespace mcla::native_gfx

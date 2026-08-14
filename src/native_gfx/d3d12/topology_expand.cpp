#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — topology expansion. See topology_expand.h.

#include "topology_expand.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "context.h"

namespace mcla::native_gfx {

namespace {
// Numeric xenos::PrimitiveType values. Spelled out rather than including
// rex/graphics/xenos.h, which does not compile standalone here — the rest of
// the runtime (geometry.cpp) uses the same literals for the same reason.
constexpr uint32_t kTriangleFan = 5;
constexpr uint32_t kRectangleList = 8;
constexpr uint32_t kQuadList = 13;
constexpr uint32_t kQuadStrip = 14;
constexpr uint32_t kPolygon = 15;
}  // namespace

bool IsExpandableTopology(uint32_t primitive_type) {
  return primitive_type == kTriangleFan || primitive_type == kRectangleList ||
         primitive_type == kQuadList || primitive_type == kQuadStrip ||
         primitive_type == kPolygon;
}

uint32_t RectListVertexCount(uint32_t vertex_count) { return (vertex_count / 3u) * 4u; }

uint32_t ExpandedIndexCount(uint32_t primitive_type, uint32_t vertex_count) {
  switch (primitive_type) {
    case kRectangleList:
      // Three guest vertices per rect, four in the buffer we synthesise.
      return (vertex_count / 3u) * 6u;
    case kQuadList:
      return (vertex_count / 4u) * 6u;
    case kQuadStrip:
      // Vertices come in pairs; every pair after the first closes a quad.
      return vertex_count >= 4u ? ((vertex_count / 2u) - 1u) * 6u : 0u;
    case kTriangleFan:
    case kPolygon:
      // Both are a fan around vertex 0. D3D12 dropped triangle fans entirely.
      return vertex_count >= 3u ? (vertex_count - 2u) * 3u : 0u;
    default:
      return 0u;
  }
}

namespace {

void BuildIndices(uint32_t primitive_type, uint32_t vertex_count, std::vector<uint32_t>& out) {
  switch (primitive_type) {
    case kRectangleList: {
      // Indices address the SYNTHESISED buffer, which holds four vertices per
      // rect (v3 is derived on the CPU), not the three the guest supplied.
      // v0 v1        triangles (0,1,2) and (1,3,2), so both wind the same way.
      // v2 v3
      const uint32_t rects = vertex_count / 3u;
      for (uint32_t r = 0; r < rects; ++r) {
        const uint32_t b = r * 4u;
        out.push_back(b + 0u);
        out.push_back(b + 1u);
        out.push_back(b + 2u);
        out.push_back(b + 1u);
        out.push_back(b + 3u);
        out.push_back(b + 2u);
      }
      break;
    }
    case kQuadList: {
      const uint32_t quads = vertex_count / 4u;
      for (uint32_t q = 0; q < quads; ++q) {
        const uint32_t b = q * 4u;
        out.push_back(b + 0u);
        out.push_back(b + 1u);
        out.push_back(b + 2u);
        out.push_back(b + 0u);
        out.push_back(b + 2u);
        out.push_back(b + 3u);
      }
      break;
    }
    case kQuadStrip: {
      // Each quad is (2i, 2i+1, 2i+3, 2i+2): the strip advances two vertices
      // at a time and the far edge is reversed, so winding stays consistent
      // along the whole strip.
      const uint32_t quads = vertex_count >= 4u ? (vertex_count / 2u) - 1u : 0u;
      for (uint32_t q = 0; q < quads; ++q) {
        const uint32_t b = q * 2u;
        out.push_back(b + 0u);
        out.push_back(b + 1u);
        out.push_back(b + 3u);
        out.push_back(b + 0u);
        out.push_back(b + 3u);
        out.push_back(b + 2u);
      }
      break;
    }
    case kTriangleFan:
    case kPolygon: {
      for (uint32_t i = 1; i + 1 < vertex_count; ++i) {
        out.push_back(0u);
        out.push_back(i);
        out.push_back(i + 1u);
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace

namespace {

// Component count when `dxgi_format` is a 32-bit float vector, else 0. Only
// these fields may be interpolated to derive the rect's fourth corner; a
// packed colour or a normalised integer would be corrupted by arithmetic on
// its bits.
uint32_t FloatComponents(uint32_t dxgi_format) {
  switch (dxgi_format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      return 4;
    case DXGI_FORMAT_R32G32B32_FLOAT:
      return 3;
    case DXGI_FORMAT_R32G32_FLOAT:
      return 2;
    case DXGI_FORMAT_R32_FLOAT:
      return 1;
    default:
      return 0;
  }
}

inline float LoadBeFloat(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

inline void StoreFloat(uint8_t* p, float f) { std::memcpy(p, &f, 4); }

}  // namespace

bool SynthesiseRectList(const uint8_t* guest_vertices, uint32_t vertex_count, uint32_t stride,
                        const std::vector<InputElement>& elements, uint8_t* out) {
  if (!guest_vertices || !out || stride == 0 || vertex_count < 3) {
    return false;
  }
  const uint32_t rects = vertex_count / 3u;
  for (uint32_t r = 0; r < rects; ++r) {
    const uint8_t* src = guest_vertices + size_t(r) * 3u * stride;
    uint8_t* dst = out + size_t(r) * 4u * stride;

    // The three supplied corners, byte-swapped from k8in32 into host order.
    for (uint32_t i = 0; i < 3; ++i) {
      const uint8_t* s = src + size_t(i) * stride;
      uint8_t* d = dst + size_t(i) * stride;
      for (uint32_t b = 0; b + 4 <= stride; b += 4) {
        uint32_t v;
        std::memcpy(&v, s + b, 4);
        v = __builtin_bswap32(v);
        std::memcpy(d + b, &v, 4);
      }
    }

    // The fourth corner. Start from v1 so every field that cannot be
    // interpolated (packed colour, indices) carries a valid value, then
    // overwrite the float fields with v1 + v2 - v0.
    uint8_t* v3 = dst + size_t(3) * stride;
    std::memcpy(v3, dst + stride, stride);
    for (const InputElement& e : elements) {
      const uint32_t comps = FloatComponents(e.dxgi_format);
      if (comps == 0) {
        continue;
      }
      const uint32_t off = e.aligned_byte_offset;
      if (off + comps * 4u > stride) {
        continue;
      }
      for (uint32_t c = 0; c < comps; ++c) {
        const uint32_t o = off + c * 4u;
        const float f0 = LoadBeFloat(src + o);
        const float f1 = LoadBeFloat(src + stride + o);
        const float f2 = LoadBeFloat(src + 2u * stride + o);
        StoreFloat(v3 + o, f1 + f2 - f0);
      }
    }

    // TEMP DIAG (remove after): the synthesised corners, once per distinct
    // vertex signature. Everything upstream of this point was audited by
    // reading and looked correct, yet the composite's fullscreen quad writes a
    // single colour over the whole target -- which is what a quad whose
    // TEXCOORD does not vary across its corners produces. These are the values
    // the GPU actually receives, read back from `dst` in host byte order.
    if (r == 0) {
      static std::set<uint64_t> seen;
      uint64_t sig = (uint64_t(stride) << 32) ^ (uint64_t(elements.size()) << 16) ^ vertex_count;
      for (const InputElement& e : elements) {
        sig = sig * 1099511628211ull ^
              (uint64_t(e.dxgi_format) << 20) ^ (uint64_t(e.aligned_byte_offset) << 4) ^
              uint64_t(e.semantic_index);
      }
      if (seen.insert(sig).second) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "RECTVTX stride=%u verts=%u elements=%zu\n", stride, vertex_count,
                       elements.size());
          for (const InputElement& e : elements) {
            const uint32_t comps = FloatComponents(e.dxgi_format);
            std::fprintf(f, "  %s%u fmt=%u off=%u slot=%u", e.semantic_name ? e.semantic_name : "?",
                         e.semantic_index, e.dxgi_format, e.aligned_byte_offset, e.input_slot);
            if (comps == 0) {
              std::fprintf(f, " (packed, not interpolated)\n");
              continue;
            }
            for (uint32_t i = 0; i < 4; ++i) {
              std::fprintf(f, " v%u=[", i);
              for (uint32_t c = 0; c < comps; ++c) {
                float fv;
                std::memcpy(&fv, dst + size_t(i) * stride + e.aligned_byte_offset + c * 4u, 4);
                std::fprintf(f, "%s%.4f", c ? "," : "", fv);
              }
              std::fprintf(f, "]");
            }
            std::fprintf(f, "\n");
          }
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
  }
  return true;
}

uint8_t* InlineVertexRing::Allocate(D3D12Context& context, uint32_t bytes,
                                    uint64_t* out_gpu_address) {
  if (bytes == 0) {
    return nullptr;
  }
  // 16-byte alignment keeps every vertex buffer view start well-aligned
  // regardless of stride.
  const uint32_t aligned = (bytes + 15u) & ~15u;
  if (!buffer_) {
    capacity_ = 4u << 20;  // 4 MiB; inline geometry is tens of KiB per frame
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = capacity_;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(context.device()->CreateCommittedResource(
            &rex::ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer_)))) {
      REXLOG_ERROR("[native_gfx] inline vertex ring creation failed");
      capacity_ = 0;
      return nullptr;
    }
    void* mapped = nullptr;
    const D3D12_RANGE empty = {0, 0};
    if (FAILED(buffer_->Map(0, &empty, &mapped)) || !mapped) {
      REXLOG_ERROR("[native_gfx] inline vertex ring map failed");
      buffer_.Reset();
      capacity_ = 0;
      return nullptr;
    }
    mapped_ = static_cast<uint8_t*>(mapped);
    gpu_base_ = buffer_->GetGPUVirtualAddress();
  }
  if (used_ + aligned > capacity_) {
    return nullptr;
  }
  uint8_t* p = mapped_ + used_;
  if (out_gpu_address) {
    *out_gpu_address = gpu_base_ + used_;
  }
  used_ += aligned;
  return p;
}

TopologyExpander::Buffer TopologyExpander::Acquire(D3D12Context& context, uint32_t primitive_type,
                                                   uint32_t vertex_count) {
  const uint32_t index_count = ExpandedIndexCount(primitive_type, vertex_count);
  if (index_count == 0) {
    return Buffer{};
  }
  const Key key{primitive_type, vertex_count};
  auto it = buffers_.find(key);
  if (it != buffers_.end()) {
    return it->second.view;
  }

  std::vector<uint32_t> indices;
  indices.reserve(index_count);
  BuildIndices(primitive_type, vertex_count, indices);
  if (indices.size() != index_count) {
    REXLOG_ERROR("[native_gfx] topology expansion built {} indices, expected {} (prim {}, {} verts)",
                 indices.size(), index_count, primitive_type, vertex_count);
    return Buffer{};
  }
  const uint32_t size_bytes = uint32_t(indices.size() * sizeof(uint32_t));

  // An UPLOAD-heap buffer: written once at creation and only read afterwards.
  // The contents depend on nothing but the key, so there is no update path to
  // race against and no reason to stage a copy into a default heap.
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size_bytes;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  Entry entry;
  if (FAILED(context.device()->CreateCommittedResource(
          &rex::ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE, &d,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&entry.resource)))) {
    REXLOG_ERROR("[native_gfx] topology expansion buffer creation failed ({} bytes)", size_bytes);
    return Buffer{};
  }
  void* mapped = nullptr;
  const D3D12_RANGE empty = {0, 0};
  if (FAILED(entry.resource->Map(0, &empty, &mapped)) || !mapped) {
    REXLOG_ERROR("[native_gfx] topology expansion buffer map failed");
    return Buffer{};
  }
  std::memcpy(mapped, indices.data(), size_bytes);
  entry.resource->Unmap(0, nullptr);

  entry.view.gpu_address = entry.resource->GetGPUVirtualAddress();
  entry.view.size_bytes = size_bytes;
  entry.view.index_count = index_count;
  const Buffer view = entry.view;
  buffers_.emplace(key, std::move(entry));
  return view;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

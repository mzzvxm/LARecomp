// MCLA GPU renderer -- guest primitive types on the host. See topology.h.
//
// The quad and rectangle decompositions and the polygonal rule follow Xenia
// (src/graphics/d3d12/pipeline_cache.cpp geometry shaders kQuadList and
// kRectangleList, include/rex/graphics/util/draw.h IsPrimitivePolygonal, in
// the ReXGlue SDK; Copyright Ben Vanik, the Xenia authors, BSD-3-Clause).

#include "topology.h"

#include <rex/graphics/xenos.h>

namespace mcla::gpu {

namespace {

namespace xe = rex::graphics::xenos;
using plume::RenderPrimitiveTopology;

// The 0-based host pattern of one guest primitive unit: `unit` guest vertices
// step, the host indices relative to the unit's first vertex.
struct Pattern {
  uint32_t step;          // guest vertices per unit
  uint32_t count;         // host indices per unit
  uint32_t indices[6];
};

constexpr Pattern kQuadListPattern{4, 6, {0, 1, 3, 3, 1, 2}};
constexpr Pattern kQuadStripPattern{2, 6, {0, 1, 2, 2, 1, 3}};
// Rect lists address the host buffer, 4 vertices per rectangle.
constexpr Pattern kRectListPattern{4, 6, {0, 1, 2, 2, 1, 3}};

uint32_t Units(PrimitiveConversion conversion, uint32_t count) {
  switch (conversion) {
    case PrimitiveConversion::kQuadList: return count / 4;
    case PrimitiveConversion::kQuadStrip: return count >= 4 ? count / 2 - 1 : 0;
    case PrimitiveConversion::kRectList: return count / 3;
    case PrimitiveConversion::kTriangleFan: return count >= 3 ? count - 2 : 0;
    case PrimitiveConversion::kLineLoop: return count >= 2 ? count : 0;
    default: return 0;
  }
}

}  // namespace

HostPrimitive HostPrimitiveFor(uint32_t primitive) {
  switch (xe::PrimitiveType(primitive)) {
    case xe::PrimitiveType::kLineList:
      return {RenderPrimitiveTopology::LINE_LIST, PrimitiveConversion::kNone};
    case xe::PrimitiveType::kLineStrip:
      return {RenderPrimitiveTopology::LINE_STRIP, PrimitiveConversion::kNone};
    case xe::PrimitiveType::kTriangleList:
      return {RenderPrimitiveTopology::TRIANGLE_LIST, PrimitiveConversion::kNone};
    case xe::PrimitiveType::kTriangleStrip:
      return {RenderPrimitiveTopology::TRIANGLE_STRIP, PrimitiveConversion::kNone};
    case xe::PrimitiveType::kTriangleFan:
    case xe::PrimitiveType::kPolygon:
      return {RenderPrimitiveTopology::TRIANGLE_LIST, PrimitiveConversion::kTriangleFan};
    case xe::PrimitiveType::kQuadList:
      return {RenderPrimitiveTopology::TRIANGLE_LIST, PrimitiveConversion::kQuadList};
    case xe::PrimitiveType::kQuadStrip:
      return {RenderPrimitiveTopology::TRIANGLE_LIST, PrimitiveConversion::kQuadStrip};
    case xe::PrimitiveType::kRectangleList:
      return {RenderPrimitiveTopology::TRIANGLE_LIST, PrimitiveConversion::kRectList};
    case xe::PrimitiveType::kLineLoop:
      return {RenderPrimitiveTopology::LINE_LIST, PrimitiveConversion::kLineLoop};
    default:
      return {};
  }
}

const char* PrimitiveName(uint32_t primitive) {
  switch (xe::PrimitiveType(primitive)) {
    case xe::PrimitiveType::kNone: return "none";
    case xe::PrimitiveType::kPointList: return "point list";
    case xe::PrimitiveType::kLineList: return "line list";
    case xe::PrimitiveType::kLineStrip: return "line strip";
    case xe::PrimitiveType::kTriangleList: return "triangle list";
    case xe::PrimitiveType::kTriangleFan: return "triangle fan";
    case xe::PrimitiveType::kTriangleStrip: return "triangle strip";
    case xe::PrimitiveType::kTriangleWithWFlags: return "triangle with W flags";
    case xe::PrimitiveType::kRectangleList: return "rectangle list";
    case xe::PrimitiveType::kLineLoop: return "line loop";
    case xe::PrimitiveType::kQuadList: return "quad list";
    case xe::PrimitiveType::kQuadStrip: return "quad strip";
    case xe::PrimitiveType::kPolygon: return "polygon";
    default: return "explicit major mode / unknown";
  }
}

bool IsPolygonal(uint32_t primitive) {
  switch (xe::PrimitiveType(primitive)) {
    case xe::PrimitiveType::kTriangleList:
    case xe::PrimitiveType::kTriangleFan:
    case xe::PrimitiveType::kTriangleStrip:
    case xe::PrimitiveType::kTriangleWithWFlags:
    case xe::PrimitiveType::kQuadList:
    case xe::PrimitiveType::kQuadStrip:
    case xe::PrimitiveType::kPolygon:
      return true;
    default:
      return false;
  }
}

uint32_t ConvertedIndexCount(PrimitiveConversion conversion, uint32_t count) {
  const uint32_t units = Units(conversion, count);
  switch (conversion) {
    case PrimitiveConversion::kQuadList:
    case PrimitiveConversion::kQuadStrip:
    case PrimitiveConversion::kRectList:
      return units * 6;
    case PrimitiveConversion::kTriangleFan:
      return units * 3;
    case PrimitiveConversion::kLineLoop:
      return units * 2;
    default:
      return 0;
  }
}

void GenerateIndices(PrimitiveConversion conversion, uint32_t count, uint32_t* out) {
  const uint32_t units = Units(conversion, count);
  const Pattern* pattern = nullptr;
  switch (conversion) {
    case PrimitiveConversion::kQuadList: pattern = &kQuadListPattern; break;
    case PrimitiveConversion::kQuadStrip: pattern = &kQuadStripPattern; break;
    case PrimitiveConversion::kRectList: pattern = &kRectListPattern; break;
    case PrimitiveConversion::kTriangleFan:
      for (uint32_t i = 0; i < units; ++i) {
        *out++ = 0;
        *out++ = i + 1;
        *out++ = i + 2;
      }
      return;
    case PrimitiveConversion::kLineLoop:
      for (uint32_t i = 0; i < units; ++i) {
        *out++ = i;
        *out++ = (i + 1) % count;
      }
      return;
    default:
      return;
  }
  for (uint32_t u = 0; u < units; ++u) {
    const uint32_t base = u * pattern->step;
    for (uint32_t k = 0; k < pattern->count; ++k) {
      *out++ = base + pattern->indices[k];
    }
  }
}

void ConvertIndices(PrimitiveConversion conversion, const uint32_t* in, uint32_t count,
                    uint32_t* out) {
  const uint32_t n = ConvertedIndexCount(conversion, count);
  // The 0-based pattern names guest positions; replace each by the index there.
  // Written in place over `out` first, then mapped (n is small per draw).
  GenerateIndices(conversion, count, out);
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = in[out[i]];
  }
}

uint32_t StripToList(bool triangles, const uint32_t* in, uint32_t count, bool reset,
                     uint32_t reset_index, uint32_t* out) {
  uint32_t written = 0;
  uint32_t segment_start = 0;
  for (uint32_t i = 0; i <= count; ++i) {
    const bool end = i == count || (reset && in[i] == reset_index);
    if (!end) {
      continue;
    }
    const uint32_t length = i - segment_start;
    const uint32_t* s = in + segment_start;
    if (triangles) {
      for (uint32_t k = 0; k + 2 < length; ++k) {
        // Odd triangles of a strip swap their first two vertices to keep the
        // strip's winding.
        if (k & 1) {
          out[written++] = s[k + 1];
          out[written++] = s[k];
        } else {
          out[written++] = s[k];
          out[written++] = s[k + 1];
        }
        out[written++] = s[k + 2];
      }
    } else {
      for (uint32_t k = 0; k + 1 < length; ++k) {
        out[written++] = s[k];
        out[written++] = s[k + 1];
      }
    }
    segment_start = i + 1;
  }
  return written;
}

}  // namespace mcla::gpu

// MCLA GPU renderer -- guest primitive types on the host. See topology.h.

#include "topology.h"

#include <rex/graphics/xenos.h>

namespace mcla::gpu {

namespace {

namespace xe = rex::graphics::xenos;
using plume::RenderPrimitiveTopology;

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

}  // namespace mcla::gpu

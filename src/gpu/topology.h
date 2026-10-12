#pragma once
// ===========================================================================
// MCLA GPU renderer -- guest primitive types on the host
// ===========================================================================
// Lists and strips of points, lines
// and triangles draw as they are; the types the host has no equivalent for
// are drawn as triangle (or line) lists through indices generated on the
// render thread (the conversions are M4.4's, buffers.*): quad list and quad
// strip, triangle fan and polygon, rectangle list (three vertices given, the
// fourth synthesized), line loop (closed by a repeated first vertex).
//
// Not drawn, reported: point lists (the Xenos draws them as point sprites of
// PA_SU_POINT_SIZE; never seen in MCLA, 24.3), triangles with W flags and the
// explicit-major-mode types (2D copy/fill rects, tessellation patches).
// Plume's TRIANGLE_FAN is not used: D3D12 has it only with options15 and
// Plume's Agility SDK build.
//
// Threading: pure functions.
// ===========================================================================

#include <cstdint>

#include <plume_render_interface_types.h>

namespace mcla::gpu {

enum class PrimitiveConversion : uint8_t {
  kNone,
  kQuadList,
  kQuadStrip,
  kTriangleFan,  // also the polygon (convex, one fan)
  kRectList,
  kLineLoop,
  kUnsupported,
};

struct HostPrimitive {
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::UNKNOWN;
  PrimitiveConversion conversion = PrimitiveConversion::kUnsupported;
};

// `primitive`: the xenos::PrimitiveType of the draw (DrawRecord::primitive).
HostPrimitive HostPrimitiveFor(uint32_t primitive);
const char* PrimitiveName(uint32_t primitive);

}  // namespace mcla::gpu

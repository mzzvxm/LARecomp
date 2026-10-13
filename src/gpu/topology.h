#pragma once
// ===========================================================================
// MCLA GPU renderer -- guest primitive types on the host
// ===========================================================================
// Lists and strips of lines and
// triangles draw as they are; the types the host has no equivalent for are
// drawn as triangle (or line) lists through indices made on the render thread:
//   quad list     (0,1,3) (3,1,2) per quad: Xenia's strip 0,1,3,2
//   quad strip    the triangle strip's pairs (2k,2k+1,2k+2) (2k+2,2k+1,2k+3)
//   fan, polygon  (0,i,i+1)
//   rect list     four host vertices per rectangle (the fourth synthesized by
//                 the geometry cache, a,b,c,d with d = b + c - a, Xenia's
//                 rule), (0,1,2) (2,1,3) per rectangle -- strip a,b,c,d
//   line loop     a line list closed by the first vertex
// Auto and inline draws use the 0-based pattern; indexed draws map the
// pattern through the guest's indices.
//
// Not drawn, reported: point lists (the Xenos draws them as point sprites of
// PA_SU_POINT_SIZE; never seen in MCLA, 24.3), triangles with W flags and the
// explicit-major-mode types (2D copy/fill rects, tessellation patches).
// Plume's TRIANGLE_FAN is not used: D3D12 has it only with options15 and
// Plume's Agility SDK build.
//
// Polygonal (Xenia draw_util::IsPrimitivePolygonal): triangles, quads and
// polygons, not rectangle lists -- culling, fill mode and the polygon offset
// of a face only apply to polygonal primitives (Xenia: back-face culling of
// rectangles breaks 4D53082D).
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
inline constexpr uint32_t kPrimitiveConversionCount = 7;

struct HostPrimitive {
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::UNKNOWN;
  PrimitiveConversion conversion = PrimitiveConversion::kUnsupported;
};

// `primitive`: the xenos::PrimitiveType of the draw (DrawRecord::primitive).
HostPrimitive HostPrimitiveFor(uint32_t primitive);
const char* PrimitiveName(uint32_t primitive);
bool IsPolygonal(uint32_t primitive);

// Host indices a converted draw of `count` guest vertices needs (0 when the
// count makes no primitive). For kRectList the guest gives 3 vertices per
// rectangle and the host buffer holds 4.
uint32_t ConvertedIndexCount(PrimitiveConversion conversion, uint32_t count);
// The 0-based host indices (ConvertedIndexCount of them) into `out`.
void GenerateIndices(PrimitiveConversion conversion, uint32_t count, uint32_t* out);
// The host indices for `count` guest indices `in` (indexed draws): the same
// pattern, each entry replaced by the guest index it names. Not for kRectList
// (its fourth vertex does not exist in the guest's buffer).
void ConvertIndices(PrimitiveConversion conversion, const uint32_t* in, uint32_t count,
                    uint32_t* out);

// A strip as a list, honouring the guest's primitive reset when `reset` is on
// (a reset index ends the strip and starts another). Triangle strips keep the
// strip's alternating winding; line strips become line pairs. Writes at most
// (count - 2) * 3 (triangles) or (count - 1) * 2 (lines) indices; returns how
// many.
uint32_t StripToList(bool triangles, const uint32_t* in, uint32_t count, bool reset,
                     uint32_t reset_index, uint32_t* out);

}  // namespace mcla::gpu

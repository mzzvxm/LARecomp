#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — large draw slicing
// ===========================================================================
// The Xbox 360 D3D draw builders (D3DDevice_DrawIndexedVertices sub_8241D620,
// D3DDevice_DrawVertices sub_8241D230) cap a single VGT_DRAW_INITIATOR at
// 65535 elements: the count field is 16 bits. Larger draws are emitted as a
// loop of slices. Verbatim guest algorithm:
//
//   sliceLen = ((0xFFFF / groupSize) & ~1) * groupSize;      // per slice
//   ... emit slice [start, start + sliceLen) ...
//   remaining = rewind + remaining - sliceLen;
//   start    += sliceLen - rewind;
//
// where {groupSize, rewind} come from the per-primitive table at 0x82000BF8
// (big-endian dword pairs, extracted from the XEX and validated against the
// decompiled loop):
//
//   prim                     group  rewind
//   1  kPointList              1      0
//   2  kLineList               2      0
//   3  kLineStrip              1      1     (strip carries 1 vertex over)
//   4  kTriangleList           3      0
//   5  kTriangleFan            1      2     (fan re-emits 2; NOTE: loses the
//                                            fan origin — the guest table
//                                            says 2, faithfully reproduced)
//   6  kTriangleStrip          1      2
//   8  kRectangleList          3      0
//   12 kLineLoop               1      0
//   13 kQuadList               4      0
//
// Never observed in capture (max seen: 16233 elements/draw), but present in
// guest code and therefore reproduced exactly.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

struct DrawSlice {
  uint32_t start;  // relative element offset (add to StartIndex / StartVertex)
  uint32_t count;
};

// Iterates the slices of a draw exactly like the guest D3D loop.
//   DrawSlicer s(prim_type, element_count);
//   while (s.Next(slice)) { issue slice }
// A draw that fits in one packet yields exactly one slice (the whole draw).
// Unknown/degenerate prim types (group size 0 in the guest table) yield the
// whole draw unsliced and set valid() == false — the guest would have
// trapped (twllei) on those, so they cannot occur in shipped content.
class DrawSlicer {
 public:
  DrawSlicer(uint32_t prim_type, uint32_t element_count);

  bool Next(DrawSlice& out);
  bool valid() const { return valid_; }

 private:
  uint32_t remaining_;
  uint32_t start_ = 0;
  uint32_t slice_len_;
  uint32_t rewind_;
  bool valid_;
  bool done_ = false;
};

}  // namespace mcla::native_gfx

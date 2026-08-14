#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — large draw slicing.
// Byte-exact port of the guest slicing loop; see draw_slicing.h.

#include "draw_slicing.h"

namespace mcla::native_gfx {

namespace {

struct PrimGroup {
  uint8_t group;
  uint8_t rewind;
};

// Guest table at 0x82000BF8 (prim types 0..13). Entries with group 0 are
// prim types the guest traps on.
constexpr PrimGroup kPrimGroups[16] = {
    {0, 0},  // 0  kNone
    {1, 0},  // 1  kPointList
    {2, 0},  // 2  kLineList
    {1, 1},  // 3  kLineStrip
    {3, 0},  // 4  kTriangleList
    {1, 2},  // 5  kTriangleFan
    {1, 2},  // 6  kTriangleStrip
    {0, 0},  // 7  kTriangleWithWFlags
    {3, 0},  // 8  kRectangleList
    {0, 0},  // 9
    {0, 0},  // 10
    {0, 0},  // 11
    {1, 0},  // 12 kLineLoop
    {4, 0},  // 13 kQuadList
    {0, 0},  // 14 kQuadStrip
    {0, 0},  // 15
};

}  // namespace

DrawSlicer::DrawSlicer(uint32_t prim_type, uint32_t element_count)
    : remaining_(element_count) {
  const PrimGroup pg = kPrimGroups[prim_type & 0xF];
  valid_ = pg.group != 0 || element_count <= 0xFFFFu;
  if (pg.group == 0) {
    // Guest would trap; emit unsliced so the problem is visible, not hidden.
    slice_len_ = 0xFFFFFFFFu;
    rewind_ = 0;
    return;
  }
  slice_len_ = ((0xFFFFu / pg.group) & ~1u) * pg.group;
  rewind_ = pg.rewind;
}

bool DrawSlicer::Next(DrawSlice& out) {
  if (done_ || remaining_ == 0) {
    return false;
  }
  // Guest: v17 = remaining; if (remaining > 0xFFFF) v17 = sliceLen;
  // A draw of exactly 0xFFFF elements is NOT sliced.
  const uint32_t len = remaining_ > 0xFFFFu ? slice_len_ : remaining_;
  out.start = start_;
  out.count = len;
  if (len == remaining_) {
    done_ = true;
  } else {
    // remaining = rewind + remaining - len;  start += len - rewind;
    remaining_ = rewind_ + remaining_ - len;
    start_ += len - rewind_;
  }
  return true;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET

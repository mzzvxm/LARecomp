#pragma once
// ===========================================================================
// MCLA GPU renderer -- the rear view mirror's draws as a class of their own
// ===========================================================================
// The HUD's rear view mirror (mc_engine/hooks/rear_view.cpp) is one more render phase of the frame: the game draws the city, and with CARS IN REAR VIEW the
// racers, into a small rectangle of the MAIN scene target with another camera (perf/MIRRORS-SURVEY.md section 2). To the renderer those draws look like the
// main view's: same colour and depth identities, same shaders. What tells them apart is their viewport, which is exactly the rectangle the settings put there
// (rearview_position, rearview_shift_*, rearview_window_*): TickRearView publishes it here every frame the mirror is up, in guest pixels (1280x720 units), and
// the Scene compares each prepared draw's host viewport with it.
//
// What the class is for:
//   - COUNTING (always, once a rectangle is published): the interval line of the scene gets "mirror: N draws/frame", the number the cost chain
//     (perf/scripts/chain-mirror-cost.ps1) reads to see what a cheaper mirror saves;
//   - SKIPPING (mcla_gpu_mirror_class, a debug switch, off): a mirror draw does not feed the per-main-draw consumers (ambient occlusion, sun shafts,
//     volumetric light, car reflections, the car dump and frame, the headlights, the local light shadows). They keep "the first or last main draw's camera
//     constants" and a draw of a second camera would reach them as if it were the main view's. The draw itself is still recorded and drawn.
//
// CHECK: that the mirror's draws arrive with exactly this viewport (the game builds the camera's viewport from the same four numbers); a run with the mirror up says it: the
// "mirror: N draws/frame" line of the log is above zero.
//
// The rule is pure integer arithmetic on the numbers the renderer already has, so it has a test that needs no renderer (mirror_draws_test.cpp). Threading:
// Publish and Withdraw on the guest thread (once a frame), Current on the render thread; one relaxed atomic.
// ===========================================================================

#include <atomic>
#include <cstdint>

namespace mcla::gpu::mirror_draws {

// A rectangle in guest pixels of the main target. w and h are zero when none is published.
struct Rect {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  bool valid() const { return w > 0 && h > 0; }
  bool operator==(const Rect&) const = default;
};

// Four 16 bit fields; x and y may be negative (a position preset pushing the rectangle off the left edge), w and h are a size.
inline constexpr uint64_t Pack(const Rect& r) {
  if (!r.valid() || r.w > 0x7FFF || r.h > 0x7FFF || r.x < -0x8000 || r.x > 0x7FFF || r.y < -0x8000 || r.y > 0x7FFF) {
    return 0;
  }
  return (uint64_t(uint16_t(int16_t(r.x)))) | (uint64_t(uint16_t(int16_t(r.y))) << 16) | (uint64_t(uint16_t(r.w)) << 32) |
         (uint64_t(uint16_t(r.h)) << 48);
}

inline constexpr Rect Unpack(uint64_t v) {
  Rect r;
  r.x = int16_t(uint16_t(v & 0xFFFFu));
  r.y = int16_t(uint16_t((v >> 16) & 0xFFFFu));
  r.w = int32_t((v >> 32) & 0xFFFFu);
  r.h = int32_t((v >> 48) & 0xFFFFu);
  if (!r.valid()) {
    return Rect{};
  }
  return r;
}

inline std::atomic<uint64_t> g_rect{0};

// The guest thread: the mirror is up and its rectangle (guest pixels) is this.
inline void Publish(const Rect& r) { g_rect.store(Pack(r), std::memory_order_relaxed); }
// The guest thread: the mirror is off or not on the screen.
inline void Withdraw() { g_rect.store(0, std::memory_order_relaxed); }
// The render thread.
inline Rect Current() { return Unpack(g_rect.load(std::memory_order_relaxed)); }

// The host pixel a guest edge lands on at a scale of `twentieths`/20 host pixels per guest pixel (scale.h Scale::Edge: floor(x * s + 1/2)).
inline constexpr int64_t HostEdge(int64_t guest, uint32_t twentieths) {
  const int64_t n = int64_t(twentieths);
  const int64_t v = guest * n + 10;
  // floor division for a negative edge
  return v >= 0 ? v / 20 : -((-v + 19) / 20);
}

// True when a draw's host viewport (x, y, width, height in host pixels, as ComputeViewport made it) is the mirror's rectangle at that scale. The
// two edges of each axis are compared with a tolerance of one host pixel: the guest viewport's own half pixel offset and a scale that is not a whole
// number land an edge one pixel off. A viewport that is the whole target, a shadow cascade's or any other window never matches: the rectangle is
// small (229x44 of 1280x720 by default) and a second view that happened to share it would be one of the HUD's own.
inline bool MatchesViewport(const Rect& r, float vx, float vy, float vw, float vh, uint32_t twentieths) {
  if (!r.valid() || twentieths == 0) {
    return false;
  }
  // (not called `near`: minwindef.h defines it)
  const auto within_one = [](float got, int64_t want) { return got >= float(want - 1) && got <= float(want + 1); };
  const int64_t x0 = HostEdge(r.x, twentieths);
  const int64_t y0 = HostEdge(r.y, twentieths);
  const int64_t x1 = HostEdge(int64_t(r.x) + r.w, twentieths);
  const int64_t y1 = HostEdge(int64_t(r.y) + r.h, twentieths);
  return within_one(vx, x0) && within_one(vy, y0) && within_one(vw, x1 - x0) && within_one(vh, y1 - y0);
}

}  // namespace mcla::gpu::mirror_draws

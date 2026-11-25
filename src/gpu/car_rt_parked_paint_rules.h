#pragma once
// ===========================================================================
// MCLA GPU renderer -- the PARKED cars' own ray traced paint reflections: the rules that need no renderer (RT-PARKED-PAINT)
// ===========================================================================
// A parked car writes no vehicle stencil ID (the game hands the IDs to cars that MOVE, for its own motion blur: perf/CAR-RT-PARKED.md section 1). RT-PARKED-CARS (car_model_rt_rules.h,
// feat/rt-parked-cars) put such a car into the ray traced scene from its model data, so it is REFLECTED. Its own paint stayed the game's, because every step of the paint's reflection is keyed
// on the ID: the history image holds only the pixels whose stencil byte says a vehicle, the paint variant's constants are the game's for a draw with no ID, the opponents' pass finds a paint by
// a history texel, and the placed car had no paint bit. This is the rest (perf/CAR-RT-PARKED.md section 6, as built):
//
//   1. THE MARKS. At the history trigger, after the pass that fills the history from the stencil, one more small draw per parked car marks the texels INSIDE the car's rectangle (the scissor) that
//      the stencil says are no vehicle's (the ID is 0: the world) and whose view distance is inside the car's box's depth range (MarksTexel): the texel gets the scene colour and the NEGATED
//      distance (MarkedAlpha). Every reader that asks `a > 0` for "a vehicle's pixel" (the paint variant's march and self-test, the player's own pass, the chrome variant) ignores a marked texel;
//      the ray traced passes read the distance through HistoryDistance (the opponents' pass finds the parked car's paint by it, the hit colours take the colour the camera sees of the car, the
//      denoise judges the taps by it). A rectangle also holds the kerb or the wall behind the car: harmless, the primary ray tests the car's own triangles at the pixel's distance and answers
//      "no data" for anything else, and the paint variant's march never sees the texel.
//   2. THE PAINT BIT. A parked car's walked parts stand for draws of the traffic paint (OtherDraw::parked_paint, car_rt_instances_rules.h ParkedPaintPart): they carry kMaskOtherPaint and the
//      traffic paint kind, so the car's box is a traffic box (ViewBox::traffic) and PARKED (ViewBox::parked), and the class rule of the traffic (car_rt_traffic_rules.h: High 4 cars within 20 m, Mid 2 within
//      14 m, 40 % / 25 % of the opponents' texel budget, 45 m / 30 m, 600 texels) decides whether the car is traced. The car's record has a positive radius (RecordRadius): the colour of its hidden parts
//      is the one the camera sees of it, from the marked texels, not a fixed grey: the EXACT paint, which the model data does not give (a traffic car's palette colour is not known).
//   3. THE PAINT DRAW. A draw of the traffic paint with no vehicle stencil ID is a parked car's by construction (a moving one has an ID): the paint variant's constants are those of a still car (the
//      vehicle's own motion is none: the history frame's view-projection alone), with the flag kParkedFlag (car_selfrefl_rules.h) that says the history holds the car's pixels with a negated distance. The
//      variant's self-test (the shaded point must be found in the history at its own distance) and its read of the ray traced image then take the distance as abs(a) for that draw alone.
// The marks cost nothing where there is no parked car in the plan (no draw, no rectangle), and nothing on a card with no ray tracing (no scene, no walk). The marks are made only for the cars the
// opponents' pass will trace (the same admission: PlanMarks takes the traffic plan's), so the class's caps are the marks' too.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "car_rt_instances_rules.h"
#include "car_rt_opp_refl_rules.h"
#include "car_rt_refl_rules.h"
#include "car_rt_traffic_rules.h"

namespace mcla::gpu::car_rt_parked_paint {

namespace ci = car_rt_instances;
namespace op = car_rt_opp_refl;
namespace rt = car_rt_refl;
namespace traffic = car_rt_traffic;

// The most marks a frame makes: the High class's traffic count (the scene's own cap on the cars in the list is the class's: this is the history pass's, whatever a debug override of the
// traffic count says).
inline constexpr uint32_t kMaxMarks = 4;
// Metres added to both ends of the car's box's depth range: the walked car is placed to within 0.16 m of the game's own matrix (the walker's self-check), and the depth the history holds has the
// game's own noise. The primary ray decides what is the car's, so a generous range only costs rays.
inline constexpr float kMarkDepthMargin = 0.3f;
// The nearest distance a mark may start at.
inline constexpr float kMarkMinDepth = 0.05f;

// ---------------------------------------------------------------------------------------------------------------------
// the history's alpha
// ---------------------------------------------------------------------------------------------------------------------
// 0: no vehicle; > 0: the view distance of a vehicle's pixel (stencil IDs 1 to 23, hlsl/car_history_ps.hlsl); < 0 (new): the NEGATED distance of a parked car's texel (a mark).
inline float MarkedAlpha(float distance) { return -distance; }
inline bool IsVehicleAlpha(float alpha) { return alpha > 0.0f; }  // what the paint variant, the player's pass and the chrome variant read as "a vehicle's pixel"
inline bool IsMarkedAlpha(float alpha) { return alpha < 0.0f; }
// What the ray traced passes read: the distance of a texel of either kind (0: none): car_rt_refl_rules.h HistoryDistance, hlsl/car_rt_refl_common.hlsli HistoryDistance.
inline float HistoryDistance(float alpha) { return rt::HistoryDistance(alpha); }

// The rule the history pass applies to a texel inside a mark's rectangle (hlsl/car_history_ps.hlsl, the mark mode): a texel the stencil gives no vehicle ID (0: the world; a vehicle already has its own
// pixel, and the UI's IDs are no car) whose view distance (0: the sky, or a depth the lens cannot place) lies between the car's box's near and far.
inline bool MarksTexel(uint32_t stencil_id, float distance, float near_depth, float far_depth) {
  return stencil_id == 0u && distance > 0.0f && distance >= near_depth && distance <= far_depth;
}

// The depth range of a box in the frame's view space (z forward): the box's own range plus the margin. False: not a box, not finite, or wholly behind the camera.
inline bool DepthRange(const ci::ViewBox& box, float* near_depth, float* far_depth) {
  if (!box.valid || !std::isfinite(box.min[2]) || !std::isfinite(box.max[2]) || !(box.max[2] > kMarkMinDepth)) {
    return false;
  }
  *near_depth = std::max(box.min[2] - kMarkDepthMargin, kMarkMinDepth);
  *far_depth = box.max[2] + kMarkDepthMargin;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// the plan
// ---------------------------------------------------------------------------------------------------------------------
struct Mark {
  op::TexelRect rect;          // history texels: the scissor of the mark's draw
  float near_depth = 0.0f;     // view distance range the texels must be in (DepthRange)
  float far_depth = 0.0f;
  bool operator==(const Mark&) const = default;
};

// The marks of a frame: one for every PARKED car the opponents' pass will trace. `rects` are the cars' rectangles as CarRtOppRefl::Record made them, `rect_box[i]` the index into `boxes` of the
// box rects[i] is of, `plan` the traffic plan made from them (its admitted_cars index `rects`). The nearest cars first, at most `max_marks`. A car whose box has no depth range, or whose rectangle is
// empty, has none.
inline std::vector<Mark> PlanMarks(const ci::ViewBox* boxes, uint32_t box_count, const std::vector<traffic::CarRect>& rects, const std::vector<uint32_t>& rect_box,
                                   const traffic::TrafficPlan& plan, uint32_t max_marks = kMaxMarks) {
  std::vector<Mark> marks;
  if (!boxes) {
    return marks;
  }
  for (const uint32_t i : plan.admitted_cars) {
    if (i >= rects.size() || i >= rect_box.size() || rect_box[i] >= box_count || rects[i].rect.Empty()) {
      continue;
    }
    const ci::ViewBox& box = boxes[rect_box[i]];
    Mark mark;
    if (!box.parked || !box.paint || !DepthRange(box, &mark.near_depth, &mark.far_depth)) {
      continue;
    }
    mark.rect = rects[i].rect;
    marks.push_back(mark);
  }
  std::stable_sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) { return a.near_depth < b.near_depth; });
  if (marks.size() > max_marks) {
    marks.resize(max_marks);
  }
  return marks;
}

// How many texels the marks' scissors cover (what the history pass reads the stencil of beyond its own): the cost's measure.
inline uint64_t MarkTexels(const std::vector<Mark>& marks) {
  uint64_t n = 0;
  for (const Mark& m : marks) {
    n += m.rect.Area();
  }
  return n;
}

// ---------------------------------------------------------------------------------------------------------------------
// the paint draw
// ---------------------------------------------------------------------------------------------------------------------
// Whether a draw of the paint variant that carries no vehicle stencil ID is given the constants of a still car: a draw of the TRAFFIC paint (a customisable paint of a car with no ID is
// the player's or a racer's in a world the history does not hold: the game's) while the switch is on.
inline bool StillConstantsFor(uint32_t stencil_ids, bool traffic_paint, bool switch_on) { return stencil_ids == 0u && traffic_paint && switch_on; }

// The window line's words.
inline std::string Describe(uint32_t marks, uint64_t texels) {
  return std::to_string(marks) + " marks over " + std::to_string(texels) + " history texels";
}

}  // namespace mcla::gpu::car_rt_parked_paint

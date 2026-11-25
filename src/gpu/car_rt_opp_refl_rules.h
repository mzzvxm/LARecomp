#pragma once
// ===========================================================================
// MCLA GPU renderer -- ray traced reflections on the OPPONENTS' cars: the rules that need no renderer
// ===========================================================================
// The ray traced car reflection (car_rt_refl.h) shades the PLAYER's car: its primary ray finds the player's body at a texel of the history image and the cone of
// rays goes from there. The cars driving beside it are in the same top level (car_rt_scene.h, car_rt_instances_rules.h) and show in the player's paint, but their own
// paint kept the screen-space march. This is the same thing for them (car_rt_opp_refl.h is the pass, hlsl/car_rt_refl_opp.hlsli its shader).
//
// WHICH PIXELS. The pixels the paint variant (shaderpack/variants/car_selfrefl.cpp, versions 5 and 6) reads: a draw of xCarPaintCustomizable::PS_CarPaint of ANOTHER car (and, since version 7,
// RT-TRAFFIC-REFL, of the TRAFFIC paint xAmbientCarPaint::PSAmbientCarPaint: its instances carry the same bit, and car_rt_traffic_rules.h says which of those cars the pass traces and with how
// much of the budget).
// The instance of such a draw carries kMaskOtherPaint (0x40, car_rt_scene_rules.h) beside kMaskOthers (OtherMask); the pass's primary ray has that bit alone, in the
// window around the history's distance, so it finds an opponent's paint at a texel and nothing else (glass, tyres, lights, chrome, traffic's own shaders are not
// found: they keep their march, the chrome its own ray traced texel). The CARS are the ones the scene already holds: the selection by stencil ID and distance from the
// player's vehicle (CarSelector, the class's cell: High 4 cars within 20 m, Mid 2 within 14 m, none on Low), so no opponent outside those limits has a triangle in the
// top level and none of its pixels finds a hit. Where the pass looks is further limited to the screen rectangles of the cars' boxes (BoxRect): the dispatch covers
// those, not the picture.
//
// EACH PIXEL'S SURFACE. The mesh's interpolated vertex normal at the primary hit (CarRtHitNormal: the normal the paint shades with, in view space, for any instance
// of the scene) and the cone's width (mcla_gpu_car_selfrefl_rt_gloss, the paint's gloss as a number for every car: the pass reads no roughness per pixel, the draw's
// own gloss constants and the paint's normal map are NOT available to it; the paint variant applies the result with its own Fresnel and strength, below).
//
// RAYS. The same top level, the same reflection mask (the push's parts: the player's body, glass and tyres, and every other car). An opponent's own body must not
// hit itself wrongly: the self-hit rule is by instance and facing (IsSelfHit: the panel the ray left, a surface within 32 degrees of the point's normal on the primary
// ray's instance), the origin is lifted 4 mm + 2 mm a metre along the normal, and the hits on the car's OTHER instances (its wing on its trunk, its mirror on its door)
// are real. The player's car appears in the reflection with the colour the history shows where the camera sees it; where it does not (the flank facing the opponent is
// the usual case) its colour is the player's record's (below).
//
// THE COLOUR OF A HIT THE HISTORY CANNOT SHOW. For a hit on another car the instance's car record gives it (the colour the camera sees of that car, or its paint:
// OtherCarColour, unchanged). For a hit on the PLAYER's car there was no record: the player's own was the pixel's own surface. Here the player's car gets a record of
// its own in the same table (a CarRow and a CarPaintRow after the boxes' rows, List::player_row): its box's centre in view space and kCarRadius, and its paint from the
// model walk where known; the shader reads it with the same OtherCarColour. Where neither the history around the centre nor a paint gives a colour a hit on a part
// the camera sees only partly is shown in the colour of the part's visible texels in shade, and a hit on a part it does not see at all is a miss (ResolvePlayerHit).
//
// BLENDING. Not here: the image is the one the paint variant reads for any paint draw (the shaded point's place in the history, through the vehicle's own motion by
// its stencil ID), and the variant applies a texel that has data with the same Fresnel, strength and agreement rules as the player's paint (CarReflApply). A texel of
// the image this pass makes is the player's texel in every code (alpha 1 + w, -(1 + w), 1, rgb) so the variant, the denoise and the debug view 5 need no change.
//
// COST AND BUDGET. One more dispatch (rectangles of the cars' boxes, at most kMaxDispatches, merged where they touch) after the player's, before the denoise: each
// texel that holds a vehicle does one primary ray, then the cone (two rays where the outer pair finds nothing). The estimate of a car's work is its rectangle's area
// in image texels. TexelBudget (by class: High 500k, Mid 200k, Low none) is the area the pass may spend at the full cone of 4 rays; PlanDispatches admits the cars by area,
// largest first, while the sum stays within twice the budget (the first is always admitted: a car filling the picture is the case a player looks at), and chooses the rays a
// texel casts by the sum: up to the budget 4, up to twice the budget 2 (the outer pair), above that 1 (the mirror: no cone). The pass is timed as "car rt refl opp" (PassTiming),
// and CarRtOppRefl logs a window every 300 frames.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "car_dump_rules.h"
#include "car_model_rt_rules.h"
#include "car_rt_instances_rules.h"
#include "car_rt_refl_rules.h"
#include "car_rt_scene_rules.h"
#include "gpu_class.h"

namespace mcla::gpu::car_rt_opp_refl {

namespace ci = car_rt_instances;
namespace rr = car_rt_rules;
namespace rt = car_rt_refl;

// ---------------------------------------------------------------------------------------------------------------------
// the parts
// ---------------------------------------------------------------------------------------------------------------------
// The primary ray finds another car's paint, and nothing else.
inline constexpr uint32_t kPrimaryParts = rr::kMaskOtherPaint;
static_assert((kPrimaryParts & (rr::kMaskBody | rr::kMaskGlass | rr::kMaskTyres | rr::kMaskDriver | rr::kMaskOtherChrome)) == 0,
              "the opponents' primary ray never finds the player's car, nor another car's chrome (the first dispatch's)");
// Every instance of another car's paint draw has kMaskOthers too (ci::OtherMask; the rules test checks it): the cone's rays (parts 0x10) hit it like any part of another car.

// ---------------------------------------------------------------------------------------------------------------------
// the budget
// ---------------------------------------------------------------------------------------------------------------------
inline constexpr uint32_t kRaysFull = 4;     // the cone's rays
inline constexpr uint32_t kRaysReduced = 2;  // the outer pair
inline constexpr uint32_t kRaysMirror = 1;   // no cone: the mirror ray
inline constexpr uint32_t kMaxDispatches = 4;
inline constexpr int32_t kRectMargin = 2;    // texels added round a car's rectangle
inline constexpr double kAdmitFactor = 2.0;  // cars are admitted while their areas sum to at most this many budgets (the first always)
// The area (image texels) the pass may spend at the full cone. About 0.5 to 0.9 ms on a 5080 class card for a rectangle half full of car, estimated from the player's
// pass (0.25 to 0.45 ms for 215k vehicle texels): CHECK with the pass timing row "car rt refl opp".
inline constexpr uint64_t kBudgetHigh = 500000;
inline constexpr uint64_t kBudgetMid = 200000;

// The class's budget, with the debug overrides: `texels_thousands` > 0 replaces it, `cars_override` > 0 on the Low class takes the High cell's (as ci::LimitsFor does for the
// cars: asked for by hand, the High cell's numbers stand for the rest). 0: the pass is off.
inline uint64_t TexelBudget(gpu_class::GpuClass cls, int cars_override, int texels_thousands) {
  if (texels_thousands > 0) {
    return uint64_t(texels_thousands) * 1000u;
  }
  if (cls == gpu_class::GpuClass::kLow && cars_override > 0) {
    cls = gpu_class::GpuClass::kHigh;
  }
  switch (cls) {
    case gpu_class::GpuClass::kHigh: return kBudgetHigh;
    case gpu_class::GpuClass::kMid: return kBudgetMid;
    case gpu_class::GpuClass::kLow: break;
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// where the pass looks: the cars' rectangles in the image
// ---------------------------------------------------------------------------------------------------------------------
struct TexelRect {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // texels: [x0, x1) x [y0, y1)
  bool Empty() const { return x1 <= x0 || y1 <= y0; }
  int32_t Width() const { return Empty() ? 0 : x1 - x0; }
  int32_t Height() const { return Empty() ? 0 : y1 - y0; }
  uint64_t Area() const { return Empty() ? 0 : uint64_t(x1 - x0) * uint64_t(y1 - y0); }
  bool operator==(const TexelRect&) const = default;
};

inline TexelRect Union(const TexelRect& a, const TexelRect& b) {
  if (a.Empty()) return b;
  if (b.Empty()) return a;
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}
inline bool Touch(const TexelRect& a, const TexelRect& b) {
  return !a.Empty() && !b.Empty() && a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

// The rectangle of image texels a box of the frame's view space (x right, y up, z forward: the scene's) covers, `margin` texels larger and clamped to the image. The
// image is `width` x `height` texels for a `render_w` x `render_h` picture (divisor 1 or 2: texel i stands for render pixel floor((i + 0.5) divisor), rt::TexelCentre) with
// the lens tan_x, tan_y of the history (HistoryInfo). A box wholly behind the camera: empty. One that crosses the camera's plane (a car beside the camera) has no
// finite rectangle: the whole image, to be safe (the budget then lowers the rays).
inline TexelRect BoxRect(const ci::ViewBox& box, double tan_x, double tan_y, double render_w, double render_h, double divisor, int32_t width, int32_t height,
                         int32_t margin = kRectMargin) {
  if (!box.valid || width <= 0 || height <= 0 || !(tan_x > 0.0) || !(tan_y > 0.0) || !(render_w > 0.0) || !(render_h > 0.0) || !(divisor >= 1.0)) {
    return {};
  }
  double lo_x = std::numeric_limits<double>::infinity(), hi_x = -lo_x, lo_y = lo_x, hi_y = -lo_x;
  bool behind = false, front = false;
  for (int corner = 0; corner < 8; ++corner) {
    const double x = (corner & 1) ? box.max[0] : box.min[0];
    const double y = (corner & 2) ? box.max[1] : box.min[1];
    const double z = (corner & 4) ? box.max[2] : box.min[2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
      return {};
    }
    if (!(z > double(rt::kMinDepth))) {
      behind = true;
      continue;
    }
    front = true;
    const double nx = x / (z * tan_x);
    const double ny = y / (z * tan_y);
    lo_x = std::min(lo_x, nx);
    hi_x = std::max(hi_x, nx);
    lo_y = std::min(lo_y, ny);
    hi_y = std::max(hi_y, ny);
  }
  if (!front) {
    return {};
  }
  if (behind) {
    return {0, 0, width, height};
  }
  const double centre = std::floor(divisor * 0.5) + 0.5;
  const double fx0 = ((lo_x * 0.5 + 0.5) * render_w - centre) / divisor;
  const double fx1 = ((hi_x * 0.5 + 0.5) * render_w - centre) / divisor;
  const double fy0 = ((0.5 - hi_y * 0.5) * render_h - centre) / divisor;  // y up in view space, down in the image
  const double fy1 = ((0.5 - lo_y * 0.5) * render_h - centre) / divisor;
  if (!(fx1 >= -double(margin) - 1.0) || !(fx0 <= double(width) + double(margin)) || !(fy1 >= -double(margin) - 1.0) || !(fy0 <= double(height) + double(margin))) {
    return {};  // wholly off the picture (the comparisons are false for a NaN too)
  }
  auto clamp = [](double v, int32_t lo, int32_t hi) { return int32_t(std::clamp(v, double(lo), double(hi))); };
  TexelRect r;
  r.x0 = clamp(std::floor(fx0) - margin, 0, width);
  r.x1 = clamp(std::floor(fx1) + 1.0 + margin, 0, width);
  r.y0 = clamp(std::floor(fy0) - margin, 0, height);
  r.y1 = clamp(std::floor(fy1) + 1.0 + margin, 0, height);
  return r;
}

// Rectangles that overlap or touch become their union; while more than `max_out` are left the two whose union is the smallest are merged. Order is not kept.
inline std::vector<TexelRect> MergeRects(std::vector<TexelRect> rects, uint32_t max_out = kMaxDispatches) {
  std::erase_if(rects, [](const TexelRect& r) { return r.Empty(); });
  for (bool merged = true; merged;) {
    merged = false;
    for (size_t i = 0; i < rects.size() && !merged; ++i) {
      for (size_t j = i + 1; j < rects.size(); ++j) {
        if (Touch(rects[i], rects[j])) {
          rects[i] = Union(rects[i], rects[j]);
          rects.erase(rects.begin() + std::ptrdiff_t(j));
          merged = true;
          break;
        }
      }
    }
  }
  max_out = std::max<uint32_t>(max_out, 1);
  while (rects.size() > max_out) {
    size_t bi = 0, bj = 1;
    uint64_t best = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < rects.size(); ++i) {
      for (size_t j = i + 1; j < rects.size(); ++j) {
        const uint64_t area = Union(rects[i], rects[j]).Area();
        if (area < best) {
          best = area;
          bi = i;
          bj = j;
        }
      }
    }
    rects[bi] = Union(rects[bi], rects[bj]);
    rects.erase(rects.begin() + std::ptrdiff_t(bj));
  }
  return rects;
}

// The thread groups of a dispatch over `r` (the shader's group is 8 x 8; it drops the texels past the image).
inline uint32_t GroupsX(const TexelRect& r, uint32_t group = 8) { return uint32_t(r.Width() + int32_t(group) - 1) / group; }
inline uint32_t GroupsY(const TexelRect& r, uint32_t group = 8) { return uint32_t(r.Height() + int32_t(group) - 1) / group; }

// ---------------------------------------------------------------------------------------------------------------------
// the plan of a frame
// ---------------------------------------------------------------------------------------------------------------------
struct Plan {
  std::vector<TexelRect> dispatches;  // after merging, disjoint
  uint32_t cars = 0;                  // the boxes that have a rectangle in the picture
  uint32_t admitted = 0;
  uint32_t skipped = 0;               // over the budget
  uint64_t texels = 0;                // the admitted cars' rectangles' areas, as the budget counts them
  uint64_t covered = 0;               // the dispatches' areas
  uint32_t rays = kRaysFull;          // the cone's rays a texel casts (kRaysFull, kRaysReduced, kRaysMirror)
  bool operator==(const Plan&) const = default;
};

// The rays for admitted rectangles of `texels` in all against a budget: the full cone up to the budget, the outer pair up to twice it, the mirror above.
inline uint32_t RaysFor(uint64_t texels, uint64_t budget) {
  if (texels <= budget) return kRaysFull;
  if (double(texels) <= kAdmitFactor * double(budget)) return kRaysReduced;
  return kRaysMirror;
}

// `car_rects` the cars' rectangles (BoxRect of each box, any order); `budget` TexelBudget; `rays_override` 0: by the sum, else 1, 2 or 4.
inline Plan PlanDispatches(const std::vector<TexelRect>& car_rects, uint64_t budget, uint32_t rays_override = 0, uint32_t max_dispatches = kMaxDispatches) {
  Plan plan;
  if (budget == 0) {
    return plan;
  }
  std::vector<uint32_t> order;
  for (uint32_t i = 0; i < car_rects.size(); ++i) {
    if (!car_rects[i].Empty()) {
      order.push_back(i);
    }
  }
  plan.cars = uint32_t(order.size());
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return car_rects[a].Area() > car_rects[b].Area(); });
  std::vector<TexelRect> admitted;
  const double limit = kAdmitFactor * double(budget);
  for (const uint32_t i : order) {
    const uint64_t area = car_rects[i].Area();
    if (admitted.empty() || double(plan.texels + area) <= limit) {
      admitted.push_back(car_rects[i]);
      plan.texels += area;
      ++plan.admitted;
    } else {
      ++plan.skipped;
    }
  }
  plan.rays = (rays_override == kRaysFull || rays_override == kRaysReduced || rays_override == kRaysMirror) ? rays_override : RaysFor(plan.texels, budget);
  plan.dispatches = MergeRects(std::move(admitted), max_dispatches);
  for (const TexelRect& r : plan.dispatches) {
    plan.covered += r.Area();
  }
  return plan;
}

// ---------------------------------------------------------------------------------------------------------------------
// the aux image's instance code
// ---------------------------------------------------------------------------------------------------------------------
// The denoise treats two taps with the same code in the aux image's green as the same instance. The player's pass writes min(instance, 255) / 255 (the player's instances
// come first in the table, under 200), and 255 is the other cars' chrome texel's own code (no aux: the denoise copies it). An opponent's instance index is 195 and more, past 255
// with a second car, so the opponents' pass writes codes of their own in 200 .. 254, by the index modulo 55 (two panels 55 instances apart share a code; the history's distance
// and the surface's slope still tell them apart).
inline constexpr uint32_t kCodeBase = 200;
inline constexpr uint32_t kCodes = 55;
inline float EncodeInstance(uint32_t instance) { return float(kCodeBase + instance % kCodes) / 255.0f; }
// What the denoise's `int(y * 255 + 0.5)` makes of a code read back from the 8-bit unorm image.
inline int DecodeInstance(float stored) { return int(std::lround(std::clamp(stored, 0.0f, 1.0f) * 255.0f)); }
static_assert(kCodeBase + kCodes - 1 < 255, "the codes stay under 255, the chrome texel's own");

// ---------------------------------------------------------------------------------------------------------------------
// the player's car's record
// ---------------------------------------------------------------------------------------------------------------------
// The paint colour of the player's car from the walk's cars: the first player's car whose paint is known. False: none.
inline bool PlayerPaint(const std::vector<car_model_rt::CarInfo>& cars, float rgb[3]) {
  for (const car_model_rt::CarInfo& c : cars) {
    if (c.is_player && c.paint.known) {
      for (int k = 0; k < 3; ++k) {
        rgb[k] = c.paint.rgb[k];
      }
      return true;
    }
  }
  return false;
}

struct PlayerRecord {
  ci::CarRow row;
  ci::CarPaintRow paint;
};
// The record the shader's OtherCarColour reads for the player's car: the centre of its box in the view space (the car's body), kCarRadius (the colour is sampled from the history round it),
// the paint where known. No box (none of the car's meshes had bounds): radius 0, so the shader finds no colour in the history and takes the paint, or none.
inline PlayerRecord MakePlayerRecord(const ci::ViewBox& box, bool has_paint, const float rgb[3]) {
  PlayerRecord r;
  if (box.valid) {
    for (int c = 0; c < 3; ++c) {
      r.row.centre[c] = 0.5f * (box.min[c] + box.max[c]);
    }
    r.row.radius = ci::kCarRadius;
  }
  if (has_paint) {
    for (int c = 0; c < 3; ++c) {
      r.paint.rgb[c] = rgb[c];
    }
    r.paint.flags = ci::kPaintKnown;
  }
  return r;
}

// How many instances of the list carry the paint bit.
inline uint32_t CountPaintInstances(const ci::List& list) {
  uint32_t n = 0;
  for (const ci::Item& item : list.items) {
    n += (item.mask & rr::kMaskOtherPaint) != 0 ? 1u : 0u;
  }
  return n;
}
// ... of which are of the traffic paint.
inline uint32_t CountTrafficPaintInstances(const ci::List& list) {
  uint32_t n = 0;
  for (const ci::Item& item : list.items) {
    n += ((item.mask & rr::kMaskOtherPaint) != 0 && item.paint_kind == ci::PaintKind::kTraffic) ? 1u : 0u;
  }
  return n;
}

// Says which of the list's boxes (one a listed other car that has an instance, nearest first: AppendOthers) are of a car that has an instance of another car's paint draw, in
// ViewBox::paint, and which of those are a TRAFFIC car's (every paint draw of the car is the traffic paint's: ViewBox::traffic), and which of those are a PARKED car's (RT-PARKED-PAINT: a car
// built from its model data that the camera sees, whose parts carry the paint bit: ViewBox::parked). The cars' records follow the instances' rows, kCarRows each, in
// the order of the boxes (a car with no instance has a record and no box), and an instance of another car
// names its car's first row in `draw`: so box j is the j-th car with an instance. A list with no other car, or whose boxes do not match its cars, marks none.
inline void MarkPaintBoxes(ci::List* list) {
  for (ci::ViewBox& b : list->boxes) {
    b.paint = false;
    b.traffic = false;
    b.parked = false;
  }
  if (list->box_row == 0 || list->box_row < list->items.size() || list->box_row > list->rows.size()) {
    return;
  }
  const uint32_t first_record = uint32_t(list->items.size());
  const uint32_t cars = (list->box_row - first_record) / ci::kCarRows;
  std::vector<uint8_t> has_instance(cars, 0), has_paint(cars, 0), has_custom(cars, 0), has_parked(cars, 0);
  for (size_t i = 0; i < list->items.size() && i < list->rows.size(); ++i) {
    if ((list->items[i].mask & rr::kMaskOthers) == 0) {
      continue;  // the player's own instance: its `draw` is a place in the car frame's draws
    }
    const uint32_t row = list->rows[i].draw;
    if (row < first_record || (row - first_record) % ci::kCarRows != 0 || (row - first_record) / ci::kCarRows >= cars) {
      continue;
    }
    const size_t car = (row - first_record) / ci::kCarRows;
    has_instance[car] = 1;
    has_paint[car] |= (list->items[i].mask & rr::kMaskOtherPaint) != 0 ? 1 : 0;
    has_custom[car] |= list->items[i].paint_kind == ci::PaintKind::kCustom ? 1 : 0;
    has_parked[car] |= list->items[i].parked ? 1 : 0;  // RT-PARKED-PAINT: a part of a parked car that carries the paint bit
  }
  size_t box = 0;
  for (uint32_t car = 0; car < cars; ++car) {
    if (!has_instance[car]) {
      continue;
    }
    if (box < list->boxes.size()) {
      list->boxes[box].paint = has_paint[car] != 0;
      list->boxes[box].traffic = has_paint[car] != 0 && has_custom[car] == 0;
      list->boxes[box].parked = has_paint[car] != 0 && has_parked[car] != 0;
    }
    ++box;
  }
}

// Adds the player's record to the table (after the boxes' rows) when the list has instances of another car's paint, and says where it is (List::player_row), and counts those
// instances (List::other_paint_instances). `paints` the walk's cars (null: no paint known).
inline void AppendPlayerRecord(ci::List* list, const std::vector<car_model_rt::CarInfo>* paints) {
  list->other_paint_instances = CountPaintInstances(*list);
  list->traffic_paint_instances = CountTrafficPaintInstances(*list);
  list->player_row = 0;
  MarkPaintBoxes(list);
  if (list->other_paint_instances == 0) {
    return;
  }
  float rgb[3] = {};
  const bool has_paint = paints && PlayerPaint(*paints, rgb);
  const PlayerRecord record = MakePlayerRecord(list->player_box, has_paint, rgb);
  list->player_row = uint32_t(list->rows.size());
  rr::InstanceRow raw;
  static_assert(sizeof(raw) == sizeof(record.row) && sizeof(raw) == sizeof(record.paint));
  std::memcpy(&raw, &record.row, sizeof(raw));
  list->rows.push_back(raw);
  std::memcpy(&raw, &record.paint, sizeof(raw));
  list->rows.push_back(raw);
}

// ---------------------------------------------------------------------------------------------------------------------
// a hit on the player's car, from another car's paint (hlsl/car_rt_refl_opp.hlsli)
// ---------------------------------------------------------------------------------------------------------------------
// The ray hit a part of the player's car. `visible` 0..1 is how much of the hit the history shows (rt::kVisibleFullShare makes it whole), `known` the share of the player's
// record's colour that is known (OtherCarColour's return: the history's share round the centre, 1 for the paint). The colour of a part the history does not show is the
// record's where it is known (>= ci::kCarRadius's own minimum, kOtherCarMinShare 0.15), else the shown colour in shade where part of it is shown, else nothing: a miss.
enum class PlayerHit : uint8_t { kShown, kRecord, kShownInShade, kMiss };
inline constexpr float kOtherCarMinShare = 0.15f;  // as hlsl/car_rt_refl_cs.hlsl
inline PlayerHit ResolvePlayerHit(float visible, float known) {
  if (visible >= 1.0f) {
    return PlayerHit::kShown;
  }
  if (known >= kOtherCarMinShare) {
    return PlayerHit::kRecord;
  }
  return visible > 0.0f ? PlayerHit::kShownInShade : PlayerHit::kMiss;
}

// ---------------------------------------------------------------------------------------------------------------------
// the log
// ---------------------------------------------------------------------------------------------------------------------
inline std::string Describe(const Plan& plan) {
  std::string s = std::to_string(plan.admitted) + " of " + std::to_string(plan.cars) + " cars in " + std::to_string(plan.dispatches.size()) + " dispatches (" +
                  std::to_string(plan.covered) + " texels, " + std::to_string(plan.texels) + " counted), " + std::to_string(plan.rays) + " rays";
  if (plan.skipped != 0) {
    s += ", " + std::to_string(plan.skipped) + " over the budget";
  }
  return s;
}

}  // namespace mcla::gpu::car_rt_opp_refl

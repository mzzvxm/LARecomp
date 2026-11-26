#pragma once
// ===========================================================================
// MCLA GPU renderer -- ray traced reflections on the TRAFFIC and PARKED cars' paint: which of those cars the pass traces, and with how much of its budget (RT-TRAFFIC-REFL)
// ===========================================================================
// WHAT WAS MISSING. The opponents' pass (car_rt_opp_refl.h) traces the reflection of another car's PAINT from the car's real shape: its primary ray finds the paint at a pixel of the history
// image (the instances of the draws of xCarPaintCustomizable, mask kMaskOtherPaint) and the paint variant reads the answer in place of its screen-space march. The cars of the CITY's traffic and
// the parked ones are drawn with another shader, xAmbientCarPaint::PSAmbientCarPaint (the cheap paint of the `xAmbient*` family, identity 9A6BE351ED46662D): they are in the ray traced scene
// (they are other cars: the scene holds the nearest ones of every kind, car_rt_instances_rules.h CarSelector, and they show in the player's door), but their OWN paint kept the game's flat
// environment read: a car beside the player at a red light did not reflect the player's car, the street or the car behind it.
//
// WHAT CHANGED (the owner's "Yes please" of 2026-11-20; nothing of the pass's shader changed):
//   1. the variant: shaderpack/variants/car_selfrefl.cpp (version 7) gives the traffic paint's text the same apply as the customisable paint's, with the shader's own interpolants, reading the
//      host's ray traced image ALONE (no screen-space march: where the image has no answer the paint is the game's, so nothing changes on a device that cannot trace);
//   2. the mask: another car's draw of the traffic paint has kMaskOtherPaint (car_rt_instances_rules.h OtherMask, PaintKindOf), so the pass's primary ray finds a traffic car's paint at a
//      pixel as it finds an opponent's; List::traffic_paint_instances and ViewBox::traffic say which instances and boxes are traffic's;
//   3. THIS FILE: the class rule. A busy street has many more cars than a race has opponents, and the pass's texel budget (High 500k, Mid 200k: car_rt_opp_refl_rules.h) is the same pass's.
//      The opponents come first, as before, and the traffic gets a SHARE of the budget of its own, over the same rectangles (BoxRect), admitted largest first, the first always (a car beside the
//      player at a light is the case that shows it), the others while they fit the share; a car too far to show a reflection worth the rays, or too small, is left out. The rays a texel casts
//      follow the TOTAL texels as before (the full cone up to the budget, the outer pair up to twice it, the mirror above), so a crowded scene degrades the cone before it drops a car.
//
//   level   share of the budget   farthest car (its nearest face)   smallest rectangle
//   High    40 % (200k texels)    45 m                              600 texels
//   Mid     25 % (50k texels)     30 m                              600 texels
//   Low     none: the pass has no ray tracing there
//   mcla_gpu_car_rt_traffic_refl (debug): -1 the class's level (the default), 0 off (the traffic paint keeps the game's reads), 1..100 on at that percent of the budget (any class that has the pass).
//
// WHAT IT COSTS (an estimate, nothing was run; EstimateMs is the arithmetic): the pass is about 1.4 ms a million texels at the full cone on an RTX 5080 class card (car_rt_opp_refl_rules.h: "0.5
// to 0.9 ms for a rectangle half full of car" at 500k, estimated from the player's pass). A car's rectangle in the history image (half the picture's size at 4K, the picture's at 1080p: 1080 rows
// either way) is about 160 000 texels at 6 m from the camera, 40 000 at 12 m, 9 000 at 25 m and 3 000 at 45 m (a 4.5 x 1.5 m side view, a 60 degree lens), so a street's three to five traffic
// cars in view are 40 000 to 120 000 texels: +0.05 to +0.17 ms at the full cone on the High class (the share's most, 200k texels: +0.28 ms; the first car alone may exceed the share, and
// then the rays fall: 160k texels at the mirror ray is +0.1 ms), +0.02 to +0.07 ms on the Mid class (50k texels, the outer pair of rays, three to four times the cost a ray: not measured). The variant's own cost: a device with no ray traced image pays one constant read a traffic pixel (the helper returns at once: the traffic paint is the game's, bit for bit, and the
// screen-space march of the customisable paint is NOT run for it); with an image, the shaded point's self-test (one load of the history) and the image's lookup (version 6: nine taps of the
// history and of the image), about 19 loads a traffic pixel, no march. The host: one
// more rectangle per traffic car with a paint instance (microseconds), the instance list's own work unchanged (the traffic cars were in the scene already). The cars the camera does not draw
// (placed from memory or from their model data) have no pixel in the history and no paint bit: nothing is traced for them.
//
// RUN PLAN (D3D12 only until Plume's ray query; the exe is built by the main session; nothing here was run). The scenes are the SHOWCASE-RT ones (perf/scripts/showcase_rt_frame.py make writes
// ports/scripts/scenes/showcase-rt-day.scene and -night.scene: the player's car and a spawned Camaro 1 m from its door, three cameras in the gap, the world frozen from 6 s), plus a twin pair with
// a TRAFFIC car as the hero:
//   LU-8 (the lamps), the night scene as it is: the Camaro's tail lights in the player's door (camera b, from the rear, shows them): mcla_gpu_car_rt_lamps true against false, and against the
//     Camaro with the brake on (the toolkit's car verb for the pedal, or the player's own in a drive) so that the brake lamp is brighter than the tail's; the day scene shows no glow either way.
//   RT-TRAFFIC-REFL, a copy of the day scene with the Camaro replaced by a traffic car: `at 0.5 car t ambient va_gmc_yukon_05 near player 2.2,0,-1 heading 90` (a real traffic-class car 2.2 m to
//     the player's right, 1 m behind, the verb of gb-g2-street.scene), the cameras in the gap aimed at the traffic car's flank: the player's car in its door. 4K DLAA, pairs (A)
//     mcla_gpu_car_rt_traffic_refl 0 (the game's flat read), (B) the default; also `traffic on` / `parked 1` with the scene's density, and a drive through traffic (the tour) with
//     mcla_gpu_pass_timing true: "car rt refl opp" with the switch at 0 and at -1, the means over 600 frames, at 1920x1080 and 3840x2160, then --mcla_gpu_class=mid.
//   The log: "[car rt refl] opponents" lines now carry "traffic cars a pass: N with a rectangle, N admitted (texels), N too far or small, N over the share"; "[car rt] lamps" and
//     "[car-lights]" lines for the lenses.
// THE NIGHT (CHECK 1 below): the traffic paint xAmbientCarPaint may be a day shader.
//
// CHECK (nothing here was run):
//   0. WHICH SHADER A TRAFFIC CAR'S PAINT IS AT NIGHT. gb-g2-street-night.scene's header says the traffic paint (xAmbientCarPaint::PSAmbientCarPaint) is a DAY shader and that at night the same cars draw
//      through PSMultiLight (xAmbientVehicle's, which has no environment read: nothing to replace, a reflection would be an ADDED term); the night probe's C2 line (perf/out/NIGHT-PROBE.txt) logged 253
//      draws of the traffic paint in a night run. Unresolved from the code: the first run's census says which shader a traffic car at 23 h has; if it is the multi-light one, the night pair shows
//      no traffic reflection and a night twin of the variant (an added fresnel term in xAmbientVehicle__PSMultiLight, a look the owner decides) is the next step, not this one;
//   1. the traffic paint's draws go through the variant: the log's "[car selfrefl] the first draw of the paint through the variant" and the census of draws with a stencil ID (an ambient car may have
//      none: its constants block is then off and its paint is the game's); no alpha test or alpha to coverage on those draws (else they keep the game's shader);
//   2. the interpolants iTexCoord0 (world position), iTexCoord1 (normal), iTexCoord4 (view vector) of the traffic paint, read from its vertex shader, not seen at run time: the debug view 4 of
//      the variant (mcla_gpu_car_selfrefl_debug 4) is all red if they are not;
//   3. that the cars of the traffic are in the history image (the vehicle stencil IDs 1 to 23 hold them) and in the scene's top level within the class's count of cars and radius: the log's
//      "[car rt]" lines count other cars; traffic cars beyond the class's count have no triangle and no paint bit (the scene holds High 4 cars within 20 m, Mid 2 within 14 m, and the walked
//      traffic cars besides, which have no paint bit). ANSWERED (RT-NEXT-DAY, from the RT-NEXT run): only a car that WRITES a vehicle stencil ID is in the history image, in the scene's draw path
//      and under the paint variant's constants. A moving traffic car does (the race run admitted traffic cars); a PARKED one does not, and neither does the toolkit's `car <id> ambient` (a layout car
//      that stands still), which the RT-NEXT traffic scene used: 0 traffic paint instances, 0 rays. The walker (car_model_walk.cpp AddTraffic) leaves a car the main view shows to the draw path, so such a
//      visible parked car is in no top level at all. The log's census now counts the traffic paint's draws with and without an ID (car_rt_instances_rules.h NoStencilNote). Reflecting parked cars needs
//      their pixels marked another way (their boxes, tested against the depth) and the walker to place a shown car the draw path never claimed: a follow-up, not this file's rule;
//   4. the cost against the estimate, and that the share keeps a crowded street's pass under the budget (the log's frames at 4 / 2 / 1 rays);
//   5. the starter list of pipelines (tools/pipelines/pipeline_list.py): the traffic paint's new variant word has no entry, so the pipeline of its first draw is made when it is drawn (a
//      one-time hitch the first frame a traffic car is in view) unless the list is regenerated.
//
// This header includes car_rt_opp_refl_rules.h and nothing of the renderer; car_rt_traffic_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "car_rt_instances_rules.h"
#include "car_rt_opp_refl_rules.h"
#include "gpu_class.h"

namespace mcla::gpu::car_rt_traffic {

namespace ci = car_rt_instances;
namespace op = car_rt_opp_refl;

// ---------------------------------------------------------------------------------------------------------------------
// the level
// ---------------------------------------------------------------------------------------------------------------------
struct Level {
  bool on = false;
  double share = 0.0;        // of the opponents' texel budget: the traffic cars' rectangles' areas may sum to this (the first car excepted)
  float max_depth = 0.0f;    // metres from the camera: a traffic car whose nearest face is farther is not traced
  uint32_t min_texels = 0;   // a traffic car whose rectangle is smaller is not traced
  bool operator==(const Level&) const = default;
};

inline Level LevelFor(gpu_class::GpuClass) {
  Level level;
  level.on = true;
  level.share = 0.40;
  level.max_depth = 45.0f;
  level.min_texels = 600;
  return level;
}

// The class's level with the debug override: -1 keeps it, 0 turns the traffic off, 1..100 turns it on at that percent of the budget (on a class that has no level the High class's depth and
// smallest rectangle stand in, as the opponents' budget does for the Low class asked for by hand).
inline Level LevelWith(Level level, int override_percent) {
  if (override_percent < 0) {
    return level;
  }
  if (override_percent == 0) {
    level.on = false;
    return level;
  }
  if (!level.on) {
    level = LevelFor(gpu_class::GpuClass::kHigh);
  }
  level.on = true;
  level.share = double(std::min(override_percent, 100)) / 100.0;
  return level;
}

// The distance of a box's nearest face from the camera (the frame's view space has z forward): 0 for a box that holds the camera.
inline float NearDepth(const ci::ViewBox& box) { return std::max(box.min[2], 0.0f); }

// ---------------------------------------------------------------------------------------------------------------------
// the plan
// ---------------------------------------------------------------------------------------------------------------------
struct CarRect {
  op::TexelRect rect;        // BoxRect of the car's box (empty: off the picture)
  bool traffic = false;      // ViewBox::traffic
  float near_depth = 0.0f;   // NearDepth of the box
};

struct TrafficPlan {
  op::Plan plan;                     // the dispatches, the cars admitted (all kinds), the rays: as the opponents' plan
  uint32_t traffic_cars = 0;         // traffic cars with a rectangle in the picture
  uint32_t traffic_admitted = 0;
  uint32_t traffic_far = 0;          // left out: too far or too small
  uint32_t traffic_over = 0;         // left out: the traffic's share or the pass's limit
  uint64_t traffic_texels = 0;       // the admitted traffic cars' areas (counted in plan.texels too)
  std::vector<uint32_t> admitted_cars;  // RT-PARKED-PAINT: the indices (into the `cars` the plan was made from) of every car admitted, opponents first, then the traffic's; the parked cars' marks are made from these
  bool operator==(const TrafficPlan&) const = default;
};

// The plan of a frame. The opponents' cars (`traffic` false) are admitted as op::PlanDispatches does it: largest first, the first always, the others while the sum of the areas stays within twice
// the budget. The TRAFFIC cars come after them, largest first, each admitted when the level is on, it is near enough and big enough, and (the pass has nothing yet, or the sum stays within
// twice the budget and either the traffic's own sum stays within its share or this is the first traffic car). So the opponents' admission never depends on the traffic, the traffic never
// takes more than its share (the first car apart) and never takes the pass over its limit (the pass's first car apart), and the rays follow the total as before. With no traffic car this is
// op::PlanDispatches' answer (the test holds it to that on random scenes).
inline TrafficPlan PlanDispatches(const std::vector<CarRect>& cars, uint64_t budget, const Level& level, uint32_t rays_override = 0, uint32_t max_dispatches = op::kMaxDispatches) {
  TrafficPlan out;
  if (budget == 0) {
    return out;
  }
  std::vector<uint32_t> opponents, traffic;
  for (uint32_t i = 0; i < cars.size(); ++i) {
    if (cars[i].rect.Empty()) {
      continue;
    }
    (cars[i].traffic ? traffic : opponents).push_back(i);
  }
  out.traffic_cars = uint32_t(traffic.size());
  out.plan.cars = uint32_t(opponents.size() + traffic.size());
  const auto by_area = [&](uint32_t a, uint32_t b) { return cars[a].rect.Area() > cars[b].rect.Area(); };
  std::stable_sort(opponents.begin(), opponents.end(), by_area);
  std::stable_sort(traffic.begin(), traffic.end(), by_area);
  const double limit = op::kAdmitFactor * double(budget);
  const double share = level.share * double(budget);
  std::vector<op::TexelRect> admitted;
  for (const uint32_t i : opponents) {
    const uint64_t area = cars[i].rect.Area();
    if (admitted.empty() || double(out.plan.texels + area) <= limit) {
      admitted.push_back(cars[i].rect);
      out.admitted_cars.push_back(i);
      out.plan.texels += area;
      ++out.plan.admitted;
    } else {
      ++out.plan.skipped;
    }
  }
  for (const uint32_t i : traffic) {
    const uint64_t area = cars[i].rect.Area();
    if (!level.on || cars[i].near_depth > level.max_depth || area < level.min_texels) {
      ++out.traffic_far;
      continue;
    }
    const bool fits_pass = admitted.empty() || double(out.plan.texels + area) <= limit;
    const bool fits_share = out.traffic_admitted == 0 || double(out.traffic_texels + area) <= share;
    if (fits_pass && fits_share) {
      admitted.push_back(cars[i].rect);
      out.admitted_cars.push_back(i);
      out.plan.texels += area;
      out.traffic_texels += area;
      ++out.traffic_admitted;
      ++out.plan.admitted;
    } else {
      ++out.traffic_over;
      ++out.plan.skipped;
    }
  }
  out.plan.rays = (rays_override == op::kRaysFull || rays_override == op::kRaysReduced || rays_override == op::kRaysMirror) ? rays_override : op::RaysFor(out.plan.texels, budget);
  out.plan.dispatches = op::MergeRects(std::move(admitted), max_dispatches);
  for (const op::TexelRect& r : out.plan.dispatches) {
    out.plan.covered += r.Area();
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// the cost estimate
// ---------------------------------------------------------------------------------------------------------------------
// Milliseconds a million texels cost at the full cone of four rays plus the primary on an RTX 5080 class card: car_rt_opp_refl_rules.h's "0.5 to 0.9 ms at 500k", the middle of it. A cone of two rays
// and the mirror ray cost less (the primary ray and the shading are the same): kRayCostShare is the primary's share, the rest is split by the rays.
inline constexpr double kMsPerMegaTexel = 1.4;
inline constexpr double kPrimaryShare = 0.25;
inline double EstimateMs(uint64_t texels, uint32_t rays) {
  const double per_ray = (1.0 - kPrimaryShare) / double(op::kRaysFull);
  const double ray_share = kPrimaryShare + per_ray * double(std::clamp<uint32_t>(rays, 1, op::kRaysFull));
  return kMsPerMegaTexel * double(texels) / 1.0e6 * ray_share;
}

// The window line's words for the traffic's part of a plan.
inline std::string Describe(const TrafficPlan& p) {
  return std::to_string(p.traffic_admitted) + " of " + std::to_string(p.traffic_cars) + " traffic cars (" + std::to_string(p.traffic_texels) + " texels; " + std::to_string(p.traffic_far) +
         " too far or small, " + std::to_string(p.traffic_over) + " over the share)";
}

}  // namespace mcla::gpu::car_rt_traffic

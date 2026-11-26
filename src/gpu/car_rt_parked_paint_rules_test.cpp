// Check of the parked cars' own ray traced paint (car_rt_parked_paint_rules.h, RT-PARKED-PAINT): the history's alpha (a vehicle's distance, a parked car's negated one, what each reader makes of it), the rule
// that marks a texel (the stencil gives no ID, the distance is inside the car's box's range), the box's depth range, the plan of the marks over the traffic plan's admission (only the parked cars the
// opponents' pass will trace, nearest first, within the cap), the constants of a still car and the flag that goes with them (and what the paint variant makes of the history's distance with and without it),
// the paint bit, the record's radius and the box's flags of a parked car in the instance list, and that the history pass, the ray traced passes, the host, the variant and the settings hold the header's
// numbers and are wired as the header says. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_parked_paint_rules_test, then run src/gpu/larecomp_car_rt_parked_paint_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and shaderpack; by default the one this file is in)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "car_rt_parked_paint_rules.h"

namespace ci = mcla::gpu::car_rt_instances;
namespace rr = mcla::gpu::car_rt_rules;
namespace op = mcla::gpu::car_rt_opp_refl;
namespace rt = mcla::gpu::car_rt_refl;
namespace tr = mcla::gpu::car_rt_traffic;
namespace pp = mcla::gpu::car_rt_parked_paint;
namespace cs = mcla::gpu::car_selfrefl;
namespace tp = mcla::gpu::temporal;
using mcla::gpu::gpu_class::GpuClass;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool Near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

ci::ViewBox Box(float x0, float y0, float z0, float x1, float y1, float z1, bool paint, bool traffic, bool parked) {
  ci::ViewBox b;
  b.min[0] = x0;
  b.min[1] = y0;
  b.min[2] = z0;
  b.max[0] = x1;
  b.max[1] = y1;
  b.max[2] = z1;
  b.valid = true;
  b.paint = paint;
  b.traffic = traffic;
  b.parked = parked;
  return b;
}

tr::CarRect Car(int32_t x0, int32_t x1, bool traffic, float depth) {
  tr::CarRect c;
  c.rect = {x0, 0, x1, 100};
  c.traffic = traffic;
  c.near_depth = depth;
  return c;
}

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

// ---- the history's alpha ----------------------------------------------------------------------------------------------------------
void AlphaTests() {
  Check(pp::MarkedAlpha(12.5f) == -12.5f && pp::IsMarkedAlpha(pp::MarkedAlpha(12.5f)) && !pp::IsVehicleAlpha(pp::MarkedAlpha(12.5f)),
        "a mark is the negated distance: a mark is no vehicle's pixel to the readers that ask a > 0");
  Check(pp::IsVehicleAlpha(7.0f) && !pp::IsMarkedAlpha(7.0f) && !pp::IsVehicleAlpha(0.0f) && !pp::IsMarkedAlpha(0.0f), "a vehicle's alpha is positive, no data is 0 and neither");
  Check(pp::HistoryDistance(-9.25f) == 9.25f && pp::HistoryDistance(9.25f) == 9.25f && pp::HistoryDistance(0.0f) == 0.0f && rt::HistoryDistance(-3.0f) == 3.0f,
        "the ray traced passes read the distance of either kind (0: none)");
  // what the readers make of a mark: MatchWeight (the passes of the player's trace) ignores it, through HistoryDistance it is a surface
  const float marked = pp::MarkedAlpha(8.0f);
  Check(rt::MatchWeight(marked, 8.0f, 0.1f) == 0.0f && Near(rt::MatchWeight(pp::HistoryDistance(marked), 8.0f, 0.1f), 1.0) && rt::MatchWeight(pp::HistoryDistance(marked), 8.5f, 0.1f) == 0.0f,
        "a mark matches no surface for a reader that asks a > 0, and the surface at its distance for one that reads it through HistoryDistance");
}

// ---- the rule that marks a texel -----------------------------------------------------------------------------------------------------
void MarkRuleTests() {
  Check(pp::MarksTexel(0, 10.0f, 9.0f, 12.0f), "the world's texel inside the range is marked");
  Check(!pp::MarksTexel(1, 10.0f, 9.0f, 12.0f) && !pp::MarksTexel(23, 10.0f, 9.0f, 12.0f), "a vehicle's own texel (an ID 1 to 23) is left as the first draw made it");
  Check(!pp::MarksTexel(24, 10.0f, 9.0f, 12.0f) && !pp::MarksTexel(40, 10.0f, 9.0f, 12.0f), "the UI's IDs are no car's: no mark");
  Check(pp::MarksTexel(0, 9.0f, 9.0f, 12.0f) && pp::MarksTexel(0, 12.0f, 9.0f, 12.0f), "the range is closed at both ends");
  Check(!pp::MarksTexel(0, 8.999f, 9.0f, 12.0f) && !pp::MarksTexel(0, 12.001f, 9.0f, 12.0f), "a texel a hair in front of or behind the range is not marked");
  Check(!pp::MarksTexel(0, 0.0f, 0.0f, 12.0f) && !pp::MarksTexel(0, -1.0f, -2.0f, 12.0f), "no distance (the sky, a depth the lens cannot place) is never marked");
}

// ---- the box's depth range -----------------------------------------------------------------------------------------------------------
void DepthRangeTests() {
  float lo = 0.0f, hi = 0.0f;
  const ci::ViewBox box = Box(-1, 0, 8.0f, 1, 1.5f, 12.0f, true, true, true);
  Check(pp::DepthRange(box, &lo, &hi) && Near(lo, 8.0f - pp::kMarkDepthMargin) && Near(hi, 12.0f + pp::kMarkDepthMargin), "the range is the box's z with the margin at both ends");
  Check(pp::kMarkDepthMargin >= 0.16f && pp::kMarkDepthMargin <= 1.0f && lo < 8.0f && hi > 12.0f,
        "the margin is wider than the walked car's placement check (0.16 m: the walker's TRAFFIC SELF-CHECK) and no wider than a metre");
  const ci::ViewBox near_box = Box(-1, 0, -0.5f, 1, 1.5f, 3.0f, true, true, true);
  Check(pp::DepthRange(near_box, &lo, &hi) && lo == pp::kMarkMinDepth && Near(hi, 3.0f + pp::kMarkDepthMargin), "a box that holds the camera starts at the smallest distance");
  Check(!pp::DepthRange(Box(-1, 0, -5.0f, 1, 1.5f, -2.0f, true, true, true), &lo, &hi), "a box wholly behind the camera has no range");
  ci::ViewBox invalid = box;
  invalid.valid = false;
  Check(!pp::DepthRange(invalid, &lo, &hi), "a box that is not valid has no range");
  ci::ViewBox nan_box = box;
  nan_box.max[2] = std::nanf("");
  Check(!pp::DepthRange(nan_box, &lo, &hi), "a box with a coordinate that is not finite has no range");
}

// ---- the plan ------------------------------------------------------------------------------------------------------------------------
void PlanTests() {
  const tr::Level level = tr::LevelFor(GpuClass::kHigh);
  const uint64_t budget = op::kBudgetHigh;
  // five boxes with a paint instance, as the pass's own list of rectangles: an opponent, a moving traffic car, a parked car near, a parked car too far to trace, a parked car with no rectangle in the picture
  const std::vector<ci::ViewBox> boxes = {
      Box(-1, 0, 6, 1, 1.5f, 10, true, false, false),    // 0 an opponent (customisable paint)
      Box(-1, 0, 12, 1, 1.5f, 16, true, true, false),    // 1 a moving traffic car: has its own pixels
      Box(-1, 0, 8, 1, 1.5f, 12, true, true, true),      // 2 a parked car, traced
      Box(-1, 0, 60, 1, 1.5f, 64, true, true, true),     // 3 a parked car too far (the traffic level's depth is 45 m)
      Box(-1, 0, 20, 1, 1.5f, 24, true, true, true),     // 4 a parked car with an empty rectangle (off the picture)
  };
  std::vector<tr::CarRect> rects = {Car(0, 40, false, 6.0f), Car(100, 140, true, 12.0f), Car(200, 240, true, 8.0f), Car(300, 340, true, 60.0f), Car(0, 0, true, 20.0f)};
  rects[3].rect = {300, 0, 340, 100};
  rects[4].rect = {};
  const std::vector<uint32_t> rect_box = {0, 1, 2, 3, 4};
  const tr::TrafficPlan plan = tr::PlanDispatches(rects, budget, level);
  Check(plan.admitted_cars == std::vector<uint32_t>({0, 1, 2}), "the traffic plan says which cars it admitted: the opponent, then the traffic cars (a far car, a car with no rectangle: none)");
  const std::vector<pp::Mark> marks = pp::PlanMarks(boxes.data(), uint32_t(boxes.size()), rects, rect_box, plan);
  Check(marks.size() == 1, "only the parked car the opponents' pass traces is marked (not the opponent, not a moving traffic car, not a far one, not one with no rectangle)");
  if (marks.size() == 1) {
    Check(marks[0].rect == rects[2].rect && Near(marks[0].near_depth, 8.0f - pp::kMarkDepthMargin) && Near(marks[0].far_depth, 12.0f + pp::kMarkDepthMargin), "the mark is the car's rectangle and its box's range");
  }
  // the mapping from a rectangle to its box: rectangles made for the boxes that have a paint instance only, so the indices differ
  std::vector<ci::ViewBox> shuffled = boxes;
  shuffled[0].paint = false;  // a car with no paint instance has a box and no rectangle
  const std::vector<tr::CarRect> rects2 = {rects[1], rects[2]};
  const std::vector<uint32_t> map2 = {1, 2};
  const tr::TrafficPlan plan2 = tr::PlanDispatches(rects2, budget, level);
  const std::vector<pp::Mark> marks2 = pp::PlanMarks(shuffled.data(), uint32_t(shuffled.size()), rects2, map2, plan2);
  Check(marks2.size() == 1 && marks2[0].rect == rects[2].rect, "a rectangle is looked up in its own box: the parked one is found when a box with no paint comes before it");
  // a box that is not parked is never marked, a box that has no paint bit neither
  std::vector<ci::ViewBox> flags = boxes;
  flags[2].parked = false;
  Check(pp::PlanMarks(flags.data(), uint32_t(flags.size()), rects, rect_box, plan).empty(), "a car whose box is not parked has no mark");
  flags = boxes;
  flags[2].paint = false;
  Check(pp::PlanMarks(flags.data(), uint32_t(flags.size()), rects, rect_box, plan).empty(), "a car whose box has no paint instance has no mark");
  Check(pp::PlanMarks(nullptr, 0, rects, rect_box, plan).empty(), "no boxes, no marks");
  // indices that point nowhere
  tr::TrafficPlan bad = plan;
  bad.admitted_cars = {7, 2};
  Check(pp::PlanMarks(boxes.data(), uint32_t(boxes.size()), rects, rect_box, bad).size() == 1, "an admitted index past the rectangles is skipped, not read");
  const std::vector<uint32_t> short_map = {0, 1};
  Check(pp::PlanMarks(boxes.data(), uint32_t(boxes.size()), rects, short_map, plan).empty(), "a rectangle with no box in the map is skipped, not read");
  Check(pp::PlanMarks(boxes.data(), 2, rects, rect_box, plan).empty(), "a box index past the box count is skipped, not read");
  tr::TrafficPlan empty_rect = plan;
  empty_rect.admitted_cars = {4};  // a parked car's rectangle that is off the picture (empty), named by a plan made by hand
  Check(pp::PlanMarks(boxes.data(), uint32_t(boxes.size()), rects, rect_box, empty_rect).empty(), "a car whose rectangle is empty has no mark, whatever the plan says");

  // the cap and the order: six parked cars, nearest first, at most kMaxMarks
  std::vector<ci::ViewBox> street;
  std::vector<tr::CarRect> street_rects;
  std::vector<uint32_t> street_map;
  for (uint32_t i = 0; i < 6; ++i) {
    const float near_z = 30.0f - 4.0f * float(i);  // the later boxes are nearer
    street.push_back(Box(-1, 0, near_z, 1, 1.5f, near_z + 4.0f, true, true, true));
    street_rects.push_back(Car(int32_t(i) * 100, int32_t(i) * 100 + 40, true, near_z));
    street_map.push_back(i);
  }
  tr::Level wide = level;
  wide.share = 5.0;  // a share large enough for all six: the marks' own cap is what is tested
  const tr::TrafficPlan street_plan = tr::PlanDispatches(street_rects, 10 * budget, wide);
  const std::vector<pp::Mark> street_marks = pp::PlanMarks(street.data(), uint32_t(street.size()), street_rects, street_map, street_plan);
  Check(street_plan.admitted_cars.size() == 6 && street_marks.size() == pp::kMaxMarks && pp::kMaxMarks == 4, "six admitted parked cars: four marks (the High class's count)");
  bool nearest_first = street_marks.size() == pp::kMaxMarks;
  for (size_t i = 1; i < street_marks.size(); ++i) {
    nearest_first = nearest_first && street_marks[i - 1].near_depth <= street_marks[i].near_depth;
  }
  Check(nearest_first && Near(street_marks[0].near_depth, 10.0f - pp::kMarkDepthMargin), "the marks are the nearest cars, nearest first");
  Check(pp::PlanMarks(street.data(), uint32_t(street.size()), street_rects, street_map, street_plan, 2).size() == 2, "the cap is a parameter");
  // the cost's measure
  Check(pp::MarkTexels(street_marks) == 4 * 40 * 100 && pp::MarkTexels({}) == 0, "the marks' texels are the sum of the rectangles' areas");
  Check(pp::Describe(2, 9000) == "2 marks over 9000 history texels", "the window line's words");
  for (GpuClass cls : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    const tr::Level level = tr::LevelFor(cls);
    const tr::TrafficPlan plan = tr::PlanDispatches(street_rects, op::kBudgetMid, level);
    const auto marks = pp::PlanMarks(street.data(), uint32_t(street.size()), street_rects, street_map, plan);
    Check(level == tr::LevelFor(GpuClass::kHigh) && !marks.empty() && marks.size() <= plan.admitted_cars.size() && marks.size() <= pp::kMaxMarks,
          "the fixed traffic level admits paint marks on every class, subject to the dispatch and mark caps");
  }
  const tr::Level off = tr::LevelWith(tr::LevelFor(GpuClass::kLow), 0);
  const tr::TrafficPlan stopped = tr::PlanDispatches(street_rects, op::kBudgetMid, off);
  Check(pp::PlanMarks(street.data(), uint32_t(street.size()), street_rects, street_map, stopped).empty(), "traffic explicitly off produces no marks");
}

// ---- the paint draw: the constants and their flag, and the variant's reading of the distance ---------------------------------------------
void ConstantsTests() {
  Check(pp::StillConstantsFor(0, true, true), "a traffic paint draw with no vehicle ID gets the constants of a still car");
  Check(!pp::StillConstantsFor(4, true, true) && !pp::StillConstantsFor(0, false, true) && !pp::StillConstantsFor(0, true, false),
        "not a draw that carries an ID (a moving car), not a customisable paint draw, not with the switch off");
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const cs::Tuning tuning;
  const cs::CarSelfReflConstants plain = cs::MakeConstants(cs::Mode::kReflection, tuning, identity, 5, 6, 0.0625f, 9, true);
  Check(plain.c[3] == (cs::kRtImageFlag | cs::kCoatLayerFlag) && (plain.c[3] & cs::kParkedFlag) == 0, "the default constants carry no parked flag (version 9's block, bit for bit)");
  const cs::CarSelfReflConstants parked = cs::MakeConstants(cs::Mode::kReflection, tuning, identity, 5, 6, 0.0625f, 9, true, true);
  Check(parked.c[3] == (cs::kRtImageFlag | cs::kCoatLayerFlag | cs::kParkedFlag) && cs::kParkedFlag == 4u && cs::kParkedFlag != cs::kRtImageFlag && cs::kParkedFlag != cs::kCoatLayerFlag,
        "the parked flag is bit 2, beside the image flag and the coat layer's");
  Check(parked.c[2] == 9 && parked.c[0] == 5 && std::memcmp(parked.m, plain.m, sizeof(plain.m)) == 0 && parked.a[0] == plain.a[0], "the rest of the block is the same");
  const cs::CarSelfReflConstants no_image = cs::MakeConstants(cs::Mode::kReflection, tuning, identity, 5, 6, 0.0625f, cs::kNoRtImage, false, true);
  Check(no_image.c[3] == cs::kParkedFlag && (no_image.c[3] & cs::kRtImageFlag) == 0, "the flag is independent of the image's");
  // the variant's reading
  Check(cs::PaintHistoryDistance(-8.0f, cs::kParkedFlag) == 8.0f && cs::PaintHistoryDistance(8.0f, cs::kParkedFlag) == 8.0f && cs::PaintHistoryDistance(0.0f, cs::kParkedFlag) == 0.0f,
        "with the flag the distance is abs(a)");
  Check(cs::PaintHistoryDistance(-8.0f, 0) == -8.0f && cs::PaintHistoryDistance(8.0f, cs::kRtImageFlag | cs::kCoatLayerFlag) == 8.0f && cs::PaintHistoryDistance(-8.0f, cs::kRtImageFlag) == -8.0f,
        "without the flag the alpha is read as it is: a mark is no vehicle");
  // the paint's read of the image (car_rt_refl_rules.h ReadForPaint) over nine texels: a parked car's texels with and without the flag
  float wx[3], wy[3];
  rt::BSplineWeights(0.0f, wx);
  rt::BSplineWeights(0.0f, wy);
  std::array<rt::PaintTap, 9> taps;
  for (rt::PaintTap& t : taps) {
    t.history_distance = pp::MarkedAlpha(8.0f);
    t.alpha = 1.5f;  // a hit of weight 0.5 on the car's own part
    t.rgb[0] = 0.3f;
    t.rgb[1] = 0.2f;
    t.rgb[2] = 0.1f;
  }
  const rt::PaintRead without = rt::ReadForPaint(wx, wy, taps.data(), 8.0f, 0.1f);
  const rt::PaintRead with = rt::ReadForPaint(wx, wy, taps.data(), 8.0f, 0.1f, cs::kParkedFlag);
  Check(!without.use, "without the flag a parked car's texels are no surface: the image does not answer (the march / the game's paint)");
  Check(with.use && Near(with.weight, 0.5) && Near(with.rgb[0], 0.3), "with the flag they are the surface at their distance and the image answers");
  std::array<rt::PaintTap, 9> moving = taps;
  for (rt::PaintTap& t : moving) {
    t.history_distance = 8.0f;
  }
  const rt::PaintRead a = rt::ReadForPaint(wx, wy, moving.data(), 8.0f, 0.1f);
  const rt::PaintRead b = rt::ReadForPaint(wx, wy, moving.data(), 8.0f, 0.1f, cs::kParkedFlag);
  Check(a.use && b.use && Near(a.weight, b.weight) && Near(a.rgb[0], b.rgb[0]), "a moving car's texels read the same with the flag or without");
  const rt::PaintRead other_depth = rt::ReadForPaint(wx, wy, taps.data(), 9.0f, 0.1f, cs::kParkedFlag);
  Check(!other_depth.use, "with the flag the texels still have to be the surface at the paint's own distance");
}

// ---- the paint bit, the record's radius and the box's flags ------------------------------------------------------------------------------
void ListTests() {
  const tp::Mat4 view = tp::Identity();
  std::map<uint64_t, ci::MeshRef> meshes;
  for (uint64_t k = 100; k < 110; ++k) {
    meshes[k] = ci::MeshRef{uint32_t(k * 10), uint32_t(k)};
  }
  const auto find = [&](uint64_t key, ci::MeshRef* ref) {
    const auto it = meshes.find(key);
    if (it == meshes.end()) {
      return false;
    }
    *ref = it->second;
    return true;
  };
  auto draw = [&](uint32_t id, uint64_t key, double x, double z, uint64_t ps, bool parked_paint) {
    ci::OtherDraw d;
    d.stencil_id = id;
    d.mesh_key = key;
    d.ps = ps;
    d.indices = 300;
    d.model_to_world = Translation(x, 0.0, z);
    d.parked_paint = parked_paint;
    return d;
  };
  auto car = [&](uint32_t id, double distance, uint32_t draws, bool remembered, bool parked) {
    ci::OtherCar c;
    c.stencil_id = id;
    c.distance = distance;
    c.draws = draws;
    c.sum[0] = 0.0;
    c.sum[2] = -5.0 * double(draws);
    c.remembered = remembered;
    c.walked = remembered;
    c.parked = parked;
    return c;
  };
  const uint32_t parked_id = ci::CarSelector::kSlots + 0, hidden_id = ci::CarSelector::kSlots + 1;
  std::vector<ci::OtherCar> cars = {car(parked_id, 6.0, 2, true, true), car(hidden_id, 9.0, 2, true, false), car(3, 12.0, 2, false, false), car(4, 15.0, 1, false, false)};
  std::vector<ci::OtherDraw> draws = {
      draw(parked_id, 100, 0.0, -6.0, 0, true),  draw(parked_id, 101, 0.0, -6.0, 0, true),                                  // a parked car's two walked parts
      draw(hidden_id, 102, 3.0, -9.0, 0, true),  draw(hidden_id, 103, 3.0, -9.0, 0, false),                                 // a hidden walked car's parts (one flagged by mistake: no bit without the car's flag)
      draw(3, 104, -3.0, -12.0, ci::kTrafficPaintPs, false), draw(3, 105, -3.0, -12.0, 0x5555ull, false),                   // a traffic car the camera draws: the paint's own bit, and a part with none
      draw(4, 106, 6.0, -15.0, mcla::gpu::car_dump::kCarPaintPs, false),                                                    // an opponent
  };
  ci::List list;
  ci::AppendOthers(&list, draws, cars, view, /*flip_z=*/true, 640, rr::kMaxInstances, find);
  op::AppendPlayerRecord(&list, nullptr);  // counts the paint instances and says which boxes are traffic's and parked
  Check(list.items.size() == 7 && list.boxes.size() == 4 && list.records.size() == 4, "seven instances, four cars with a box and a record");
  if (list.items.size() != 7 || list.boxes.size() != 4 || list.records.size() != 4) {
    return;
  }
  const uint32_t paint = rr::kMaskOthers | rr::kMaskOtherPaint;
  Check(list.items[0].mask == paint && list.items[1].mask == paint && list.items[0].paint_kind == ci::PaintKind::kTraffic && list.items[1].paint_kind == ci::PaintKind::kTraffic &&
            list.items[0].parked && list.items[1].parked,
        "a parked car's walked parts carry the paint bit and the traffic paint kind, and say they are a parked car's");
  Check(list.items[2].mask == rr::kMaskOthers && list.items[3].mask == rr::kMaskOthers && list.items[2].paint_kind == ci::PaintKind::kNone && !list.items[2].parked && !list.items[3].parked,
        "a walked car the camera does not see has no paint bit, whatever its parts say");
  Check(list.items[4].mask == paint && list.items[4].paint_kind == ci::PaintKind::kTraffic && !list.items[4].parked && list.items[5].mask == rr::kMaskOthers && !list.items[5].parked,
        "a traffic car the camera draws keeps the paint bit of its paint draw, and is not a parked car's");
  Check(list.items[6].mask == paint && list.items[6].paint_kind == ci::PaintKind::kCustom && !list.items[6].parked, "an opponent's customisable paint is as it was");
  Check(list.records[0].parked && list.records[0].radius > 0.0f && list.records[0].radius == ci::kCarRadius,
        "a parked car's record has a positive radius: the colour of its hidden parts is the one the camera sees of it");
  Check(!list.records[1].parked && list.records[1].radius == -ci::kCarRadius, "a walked car the camera does not see keeps the negative radius (its paint or the grey)");
  Check(list.records[2].radius == ci::kCarRadius && list.records[3].radius == ci::kCarRadius && !list.records[2].parked, "a car the camera draws has the positive radius, as it had");
  Check(list.boxes[0].paint && list.boxes[0].traffic && list.boxes[0].parked, "the parked car's box has a paint instance, is a traffic car's and is parked");
  Check(!list.boxes[1].paint && !list.boxes[1].traffic && !list.boxes[1].parked, "the hidden walked car's box has no paint instance");
  Check(list.boxes[2].paint && list.boxes[2].traffic && !list.boxes[2].parked, "the drawn traffic car's box is traffic's and not parked");
  Check(list.boxes[3].paint && !list.boxes[3].traffic && !list.boxes[3].parked, "the opponent's box is not traffic's and not parked");
  Check(list.other_paint_instances == 4 && list.traffic_paint_instances == 3, "the paint instances: two of the parked car, one of the drawn traffic car's, the opponent's");
  // the helpers
  ci::OtherDraw flagged;
  flagged.parked_paint = true;
  ci::OtherCar walked_parked;
  walked_parked.remembered = true;
  walked_parked.parked = true;
  ci::OtherCar walked_hidden;
  walked_hidden.remembered = true;
  ci::OtherCar drawn_flag;
  drawn_flag.parked = true;  // (never made so; the rule needs the walked car too)
  Check(ci::ParkedPaintPart(walked_parked, flagged) && !ci::ParkedPaintPart(walked_hidden, flagged) && !ci::ParkedPaintPart(walked_parked, ci::OtherDraw{}) && !ci::ParkedPaintPart(drawn_flag, flagged),
        "the paint bit needs the car's flag, the part's flag and a car placed from model data");
  Check(ci::RecordRadius(walked_parked) == ci::kCarRadius && ci::RecordRadius(walked_hidden) == -ci::kCarRadius && ci::RecordRadius(ci::OtherCar{}) == ci::kCarRadius, "the record's radius by kind of car");
}

// ---- the sources: the shaders, the host and the settings hold the header's numbers and are wired as it says --------------------------------
void WiringTests(const std::string& src) {
  const std::string gpu = src + "/gpu/";
  const std::string history = ReadFile(gpu + "hlsl/car_history_ps.hlsl");
  const std::string common = ReadFile(gpu + "hlsl/car_rt_refl_common.hlsli");
  const std::string trace = ReadFile(gpu + "hlsl/car_rt_refl_cs.hlsl");
  const std::string opp = ReadFile(gpu + "hlsl/car_rt_refl_opp.hlsli");
  const std::string denoise = ReadFile(gpu + "hlsl/car_rt_refl_denoise_cs.hlsl");
  const std::string selfrefl = ReadFile(gpu + "car_selfrefl.cpp");
  const std::string selfrefl_h = ReadFile(gpu + "car_selfrefl.h");
  const std::string selfrefl_rules = ReadFile(gpu + "car_selfrefl_rules.h");
  const std::string refl = ReadFile(gpu + "car_rt_refl.cpp");
  const std::string refl_h = ReadFile(gpu + "car_rt_refl.h");
  const std::string opp_cpp = ReadFile(gpu + "car_rt_opp_refl.cpp");
  const std::string opp_h = ReadFile(gpu + "car_rt_opp_refl.h");
  const std::string passes = ReadFile(gpu + "host_passes.cpp");
  const std::string passes_h = ReadFile(gpu + "host_passes.h");
  const std::string scene = ReadFile(gpu + "car_rt_scene.cpp");
  const std::string instances = ReadFile(gpu + "car_rt_instances_rules.h");
  const std::string opp_rules = ReadFile(gpu + "car_rt_opp_refl_rules.h");
  const std::string traffic_rules = ReadFile(gpu + "car_rt_traffic_rules.h");
  const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string variant = ReadFile(src + "/shaderpack/variants/car_selfrefl.cpp");
  const std::string variant_h = ReadFile(src + "/shaderpack/variants/car_selfrefl.h");
  if (history.empty() || trace.empty() || selfrefl.empty() || scene.empty() || settings.empty() || variant.empty() || passes.empty()) {
    Check(false, "the sources next to the test are readable (hlsl, car_selfrefl.cpp, car_rt_scene.cpp, host_passes.cpp, settings_gpu.h, the variant)");
    return;
  }
  // the history pass: the mark mode
  Check(Has(history, "if (g_push.slots2.y != 0u) {") && Has(history, "if (id != 0u || g_push.slots.z == kNoSlot) {\n      discard;") &&
            Has(history, "if (!(t > 0.0f && t >= asfloat(g_push.slots2.z) && t <= asfloat(g_push.slots2.w))) {\n      discard;") && Has(history, "return float4(HeldColour(ReadColour(p)), -t);"),
        "the history pass's mark mode: a texel with an ID or no stencil plane, or outside the range, is discarded; the rest is the scene colour and the negated distance");
  Check(Has(history, "return float4(HeldColour(colour), LinearDistance(stored));") && Has(history, "if (id < kFirstVehicleId || id > kLastVehicleId) {") &&
            Has(history, "if (g_push.sizes.w != 0.0f) {\n      discard;") && Has(history, "return float4(0.0f, 0.0f, 0.0f, 0.0f);"),
        "the history pass's own mode is as it was: a vehicle's texel, the positive distance");
  // the readers that ask a > 0 are left alone, the ray traced passes read the distance of either kind
  Check(Has(common, "float HistoryDistance(float alpha)") && Has(common, "return abs(alpha);"), "the shaders' common header: HistoryDistance is abs");
  Check(Has(trace, "const float w = wb * MatchWeight(HistoryDistance(h.a), q.z, tolerance);") && Has(trace, "const float w = tent * MatchWeight(HistoryDistance(h.a), t, tolerance);") &&
            Has(trace, "const float w = tent * MatchWeight(HistoryDistance(h.a), car.centre.z, tolerance);"),
        "the trace's three reads of the history's distance (the hit's colour, the surface's mean, another car's seen colour) read the distance of either kind");
  Check(Has(trace, "if (own.a > 0.0 && own.a < 60000.0)"), "the player's own pass still asks a > 0: a mark is not the player's pixel");
  Check(Has(opp, "const float texelDistance = HistoryDistance(own.a);") && Has(opp, "if (!(texelDistance > 0.0 && texelDistance < 60000.0))") &&
            Has(opp, "TraceOpponentTexel(history, texel, float4(own.rgb, texelDistance), aux)"),
        "the opponents' pass finds another car's paint at a texel of either kind and traces it from the distance");
  Check(Has(denoise, "const float t = HistoryDistance(history.Load(int3(centre, 0)).a);") && Has(denoise, "const float tapT = HistoryDistance(history.Load(int3(at, 0)).a);"),
        "the denoise judges the taps by the distance of either kind");
  // the host: the history pass draws the marks before anything reads the image
  const size_t marks_at = selfrefl.find("rt_->ParkedMarks(info)");
  const size_t draw_at = selfrefl.find("passes_->DrawInto(list, history_, pipeline, &push, sizeof(push), discard_background);");
  const size_t dump_at = selfrefl.find("dump_->OnHistoryImage(list, history_, divisor, p);");
  const size_t trace_at = selfrefl.find("rt_->OnHistory(list, info);");
  Check(draw_at != std::string::npos && marks_at != std::string::npos && dump_at != std::string::npos && trace_at != std::string::npos && draw_at < marks_at && marks_at < dump_at && dump_at < trace_at,
        "the host: the marks come after the history pass and before the car dump's readback and the ray traced pass");
  Check(Has(selfrefl, "if (rt_ && summary.stencil_slot != kNoSlot) {") && Has(selfrefl, "RecordMarks(list, pipeline, push, marks);"), "the host: no marks without a stencil plane or without the ray traced half");
  Check(Has(selfrefl, "push.slots2[1] = 1u;") && Has(selfrefl, "push.slots2[2] = std::bit_cast<uint32_t>(mark.near_depth);") && Has(selfrefl, "push.slots2[3] = std::bit_cast<uint32_t>(mark.far_depth);") &&
            Has(selfrefl, "rects.emplace_back(mark.rect.x0, mark.rect.y0, mark.rect.x1, mark.rect.y1);") &&
            Has(selfrefl, "passes_->DrawRectsInto(list, history_, pipeline, rects.data(), pushes.data(), sizeof(Push), uint32_t(pushes.size()));"),
        "the host: a mark is the history pass's push with the mode and the range, scissored to the car's rectangle");
  Check(Has(selfrefl_h, "uint32_t slots2[4];") && Has(selfrefl_h, "static_assert(sizeof(Push) == 64 && sizeof(Push) == HostPasses::kPushConstantBytes);"), "the history pass's push is still the host pass layout's 64 bytes");
  Check(Has(passes, "void HostPasses::DrawRectsInto(") && Has(passes, "if (count == 0 || !rects || !pushes) {\n    return;") && Has(passes, "list->setScissors(&rects[i], 1);") &&
            Has(passes, "list->setGraphicsPushConstants(0, block + size_t(i) * push_size, 0, push_size);") && Has(passes_h, "void DrawRectsInto("),
        "the host passes draw the rectangles one after the other in one render pass");
  Check(Has(passes, "void HostPasses::DrawRectsInto(") && passes.find("clearColor", passes.find("void HostPasses::DrawRectsInto(")) == std::string::npos, "DrawRectsInto never clears the image");
  Check(Has(refl, "std::vector<car_rt_parked_paint::Mark> CarRtRefl::ParkedMarks(const HistoryInfo& info) {") && Has(refl, "if (!active_ || !ready_ || !scene_ || !info.image") && Has(refl, "return opp_.ParkedMarks(input);") &&
            Has(refl_h, "std::vector<car_rt_parked_paint::Mark> ParkedMarks(const HistoryInfo& info);"),
        "the ray traced reflection hands the history pass the marks of the opponents' pass");
  Check(Has(opp_cpp, "std::vector<car_rt_parked_paint::Mark> CarRtOppRefl::ParkedMarks(const MarkInput& in) {") && Has(opp_cpp, "!larecomp::settings::mcla_gpu_car_rt_parked_paint.Get()") &&
            Has(opp_cpp, "!larecomp::settings::mcla_gpu_car_rt_opp_refl.Get() || !larecomp::settings::mcla_gpu_car_rt_parked_paint.Get()) {") && Has(opp_cpp, "if (!level.on || budget == 0) {") && Has(opp_cpp, "marks = car_rt_parked_paint::PlanMarks(in.view->boxes, in.view->box_count, rects, rect_box, plan);") &&
            Has(opp_h, "std::vector<car_rt_parked_paint::Mark> ParkedMarks(const MarkInput& input);"),
        "the opponents' pass says which parked cars it will trace: the same level, the same budget, the same rectangles");
  Check(Has(opp_cpp, "CarRects(view, f.tan_x, f.tan_y, f.render_width, f.render_height, f.divisor, f.image_width, f.image_height, &rects, &rect_box, one_ray);") &&
            Has(opp_cpp, "CarRects(*in.view, in.tan_x, in.tan_y, in.render_width, in.render_height, in.divisor, in.image_width, in.image_height, &rects, &rect_box, one_ray);") &&
            Has(opp_cpp, "traffic::LevelWith(traffic::LevelFor(budget_class), int(larecomp::settings::mcla_gpu_car_rt_traffic_refl.Get()))"),
        "the pass and the marks are made from the same rectangles and the same class level");
  // the constants
  Check(Has(selfrefl, "const bool parked = !pause_view_ && car_rt_parked_paint::StillConstantsFor(ids, traffic_paint, larecomp::settings::mcla_gpu_car_rt_parked_paint.Get());") &&
            Has(selfrefl, "if (!parked) {\n      return;\n    }") && Has(selfrefl, "if (!parked && !pause_view_ && (motion_.valid & (1u << id)) != 0 && motion_frame_ == history_frame_) {") &&
            Has(selfrefl, "car_selfrefl::ExpBiasGain(exp_bias), rt_slot, coat_layer, parked);"),
        "the paint draw: a traffic paint draw with no ID is a still car's (no motion), carries the flag, and not in the pause view");
  // the scene: the paint bit and the car's flag
  Check(Has(scene, "const bool parked_paint_on = larecomp::settings::mcla_gpu_car_rt_parked_paint.Get();") && Has(scene, "const bool parked_paint = parked_paint_on && car.traffic && car.visible;") &&
            Has(scene, "other.parked = parked_paint;") && Has(scene, "draw.parked_paint = parked_paint;") && Has(scene, "NoteParkedCars(proof, parked_placed, parked_instances, parked_paint_cars);"),
        "the scene: a placed traffic car the main view shows is a parked car with the paint bit, while the switch is on");
  Check(Has(scene, "parked paint (RT-PARKED-PAINT), frames {}..{}: placed parked cars with the traffic paint's bit {:.2f} a frame (at most {}), of {:.2f} placed;") &&
            Has(selfrefl, "parked paint (RT-PARKED-PAINT), frame {}: mcla_gpu_car_rt_parked_paint {}; history passes with marks {} of {}"),
        "the logs: the words rtnext_post.py reads");
  // the list
  Check(Has(instances, "if (ParkedPaintPart(cars[plan[size_t(slot)]], draw)) {") && Has(instances, "item.mask |= rr::kMaskOtherPaint;") && Has(instances, "item.paint_kind = PaintKind::kTraffic;") && Has(instances, "item.parked = true;") &&
            Has(instances, "row.radius = RecordRadius(cars[index]);") && Has(instances, "return car.remembered && car.parked && draw.parked_paint;") &&
            Has(instances, "return (car.remembered && !car.parked) ? -kCarRadius : kCarRadius;"),
        "the instance list: the paint bit and the radius of a parked car");
  Check(Has(opp_rules, "list->boxes[box].parked = has_paint[car] != 0 && has_parked[car] != 0;") && Has(opp_rules, "has_parked[car] |= list->items[i].parked ? 1 : 0;") && Has(opp_rules, "b.parked = false;"),
        "the boxes: parked when a paint instance of the car is a parked car's");
  Check(Has(traffic_rules, "std::vector<uint32_t> admitted_cars;") && traffic_rules.find("out.admitted_cars.push_back(i);") != std::string::npos &&
            traffic_rules.find("out.admitted_cars.push_back(i);") != traffic_rules.rfind("out.admitted_cars.push_back(i);"),
        "the traffic plan keeps which cars it admitted: the opponents' and the traffic's");
  // the settings
  Check(Has(settings, std::string("REX_SETTING") + "(bool, mcla_gpu_car_rt_parked_paint, true,") && Has(settings, "keeps the game's paint, as before. Live.\", rex::kDebugOnly);") &&
            Has(settings, "inline auto& mcla_gpu_car_rt_parked_paint = g_gpu.mcla_gpu_car_rt_parked_paint;") && Has(meta, "{\"mcla_gpu_car_rt_parked_paint\", \"MCLA/GPU\"},"),
        "the switch: mcla_gpu_car_rt_parked_paint is a debug-only bool, on by default, in the settings header, its alias and the meta table");
  const size_t at = manifest.find("\"mcla_gpu_car_rt_parked_paint\": {");
  Check(at != std::string::npos && manifest.find("\"default\": \"true\"", at) < manifest.find("}", at) && manifest.find("rex::kDebugOnly", at) < manifest.find("}", at), "the manifest holds the switch: default true, debug only");
  // the variant
  Check(Has(variant_h, "inline constexpr uint32_t kCarSelfReflVariantVersion = 11;") && Has(variant, "const float ownDistance = ((slots.w & 4u) != 0u) ? abs(own.a) : own.a;") &&
            Has(variant, "const float texelDistance = ((flags & 4u) != 0u) ? abs(h.a) : h.a;") && Has(variant, "slots.z, slots.w);"),
        "the variant: version 10 reads the history's distance as abs(a) for a draw with the parked flag, in the self-test and in the read of the ray traced image (11 is the batch's number: the no double precision edit joined it)");
  Check(Has(selfrefl_rules, "inline constexpr uint32_t kParkedFlag = 4u;") && Has(selfrefl_rules, "(flags & kParkedFlag) != 0u ? std::fabs(alpha) : alpha"), "the constants' flag is bit 2 in the rules header too");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src;
  if (argc > 1) {
    src = argv[1];
  } else {
    std::string self = __FILE__;
    std::replace(self.begin(), self.end(), '\\', '/');
    const size_t slash = self.rfind('/');
    src = (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) + "/..";
  }
  std::replace(src.begin(), src.end(), '\\', '/');
  AlphaTests();
  MarkRuleTests();
  DepthRangeTests();
  PlanTests();
  ConstantsTests();
  ListTests();
  WiringTests(src);
  std::printf("car_rt_parked_paint_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

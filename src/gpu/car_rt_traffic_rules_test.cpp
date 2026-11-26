// Check of the traffic cars' ray traced reflection (car_rt_traffic_rules.h, RT-TRAFFIC-REFL): the traffic paint's identity and the paint bit of its instances, which boxes are a traffic car's, the
// class levels and the debug override, the plan (the opponents first and untouched, the traffic a share of the budget, the first traffic car always, none too far or too small, the rays by
// the total) on explicit scenes and on random ones against the opponents' own plan, the cost estimate, and that the variant, the host and the settings hold the header's numbers.
// A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_traffic_rules_test, then run src/gpu/larecomp_car_rt_traffic_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and shaderpack; by default the one this file is in)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "car_rt_traffic_rules.h"
#include "setting_source.h"

namespace ci = mcla::gpu::car_rt_instances;
namespace rr = mcla::gpu::car_rt_rules;
namespace op = mcla::gpu::car_rt_opp_refl;
namespace tr = mcla::gpu::car_rt_traffic;
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

// A rectangle of `area` texels (exact for a multiple of 100: 100 rows high).
op::TexelRect Rect(uint64_t area, int32_t x = 0) {
  const int32_t w = int32_t(std::max<uint64_t>(1, area / 100));
  return {x, 0, x + w, 100};
}

tr::CarRect Car(uint64_t area, bool traffic, float depth = 10.0f, int32_t x = 0) {
  tr::CarRect c;
  c.rect = Rect(area, x);
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

  // ---- the paint's identity and bit -----------------------------------------------------------------------------------------------
  {
    Check(ci::kTrafficPaintPs == 0x9A6BE351ED46662Dull && ci::kTrafficPaintPs != mcla::gpu::car_dump::kCarPaintPs && ci::kTrafficPaintPs != ci::kChromePs, "the traffic paint's identity is its own");
    Check(ci::PaintKindOf(mcla::gpu::car_dump::kCarPaintPs) == ci::PaintKind::kCustom && ci::PaintKindOf(ci::kTrafficPaintPs) == ci::PaintKind::kTraffic && ci::PaintKindOf(ci::kChromePs) == ci::PaintKind::kNone &&
              ci::PaintKindOf(0) == ci::PaintKind::kNone && ci::PaintKindOf(0x1234ull) == ci::PaintKind::kNone,
          "the paint kind: the customisable paint, the traffic's, nothing for any other shader (and 0)");
    const uint32_t traffic = ci::OtherMask(ci::kTrafficPaintPs);
    Check(traffic == (rr::kMaskOthers | rr::kMaskOtherPaint) && traffic == 0x50u, "a draw of the traffic paint has the paint bit beside the others' (0x50), as the customisable paint's does");
    Check(ci::OtherMask(mcla::gpu::car_dump::kCarPaintPs) == 0x50u && ci::OtherMask(ci::kChromePs) == (rr::kMaskOthers | rr::kMaskOtherChrome) && ci::OtherMask(0x1234ull) == rr::kMaskOthers,
          "the other masks are as they were: the paint 0x50, the chrome 0x30, any other draw 0x10");
    Check((op::kPrimaryParts & traffic) == rr::kMaskOtherPaint, "the opponents' primary ray finds a traffic car's paint by that bit");
    // the shader of the variant has the same number
    const std::string variant = ReadFile(src + "/shaderpack/variants/car_selfrefl.h");
    Check(!variant.empty() && Has(variant, "{\"xAmbientCarPaint__PSAmbientCarPaint\", 0x9A6BE351ED46662Dull}"), "the variant's second shader is this identity (shaderpack/variants/car_selfrefl.h kTrafficPaint)");
  }

  // ---- the boxes of the list -------------------------------------------------------------------------------------------------------
  {
    const tp::Mat4 view = tp::Identity();
    auto draw = [&](uint32_t id, uint64_t key, double x, double z, uint64_t ps) {
      ci::OtherDraw d;
      d.stencil_id = id;
      d.mesh_key = key;
      d.ps = ps;
      d.model_to_world = Translation(x, 0.0, z);
      d.indices = 300;
      return d;
    };
    std::vector<ci::OtherDraw> draws = {
        draw(1, 11, 0.0, 8.0, mcla::gpu::car_dump::kCarPaintPs),   // an opponent: the customisable paint
        draw(1, 12, 0.0, 8.0, ci::kChromePs),
        draw(2, 21, 4.0, 14.0, ci::kTrafficPaintPs),                // a traffic car
        draw(2, 22, 4.0, 14.0, 0x5555ull),
        draw(3, 31, -4.0, 10.0, ci::kTrafficPaintPs),               // a car with both kinds of paint draw (odd: the box is not traffic's)
        draw(3, 32, -4.0, 10.0, mcla::gpu::car_dump::kCarPaintPs),
        draw(4, 41, 8.0, 20.0, ci::kTrafficPaintPs),                // a traffic car placed from memory: no pixel in the history, no paint bit
        draw(5, 51, 12.0, 30.0, 0x5555ull),                         // a car with no paint draw at all
    };
    std::vector<ci::OtherCar> cars(5);
    for (uint32_t i = 0; i < 5; ++i) {
      cars[i].stencil_id = i + 1;
    }
    cars[3].remembered = true;
    for (const ci::OtherDraw& d : draws) {
      ci::OtherCar& c = cars[d.stencil_id - 1];
      c.Add(d.model_to_world, 8.0 + double(d.stencil_id));
    }
    auto find = [&](uint64_t key, ci::MeshRef* ref) {
      (void)key;
      ref->first_index = 0;
      ref->first_vertex = 0;
      for (int c = 0; c < 3; ++c) {
        ref->bounds_min[c] = -1.0f;
        ref->bounds_max[c] = 1.0f;
      }
      ref->has_bounds = true;
      return true;
    };
    ci::List list;
    ci::AppendOthers(&list, draws, cars, view, false, 640, 640, find, 8);
    op::AppendPlayerRecord(&list, nullptr);
    uint32_t paint_items = 0, traffic_items = 0, custom_items = 0;
    for (const ci::Item& item : list.items) {
      paint_items += (item.mask & rr::kMaskOtherPaint) != 0 ? 1u : 0u;
      traffic_items += item.paint_kind == ci::PaintKind::kTraffic ? 1u : 0u;
      custom_items += item.paint_kind == ci::PaintKind::kCustom ? 1u : 0u;
    }
    Check(list.items.size() == 8, "all eight draws are listed");
    Check(paint_items == 4 && custom_items == 2 && traffic_items == 2, "four paint instances: two of the customisable paint, two of the traffic's (the remembered car's has none)");
    Check(list.other_paint_instances == 4 && list.traffic_paint_instances == 2, "the list counts them: four paint instances, two of them the traffic's");
    Check(list.boxes.size() == 5, "one box a car with an instance");
    // boxes come in the order of the plan (nearest first): cars 1, 3 (10 m), 2, 4, 5 by `distance` 9, 11, 10, 12, 13
    bool found_opponent = false, found_traffic = false, found_both = false, found_remembered = false, found_plain = false;
    for (uint32_t b = 0; b < list.boxes.size(); ++b) {
      const ci::ViewBox& box = list.boxes[b];
      const double centre_x = 0.5 * (double(box.min[0]) + double(box.max[0]));
      if (std::fabs(centre_x - 0.0) < 0.1 && box.paint && !box.traffic) found_opponent = true;
      if (std::fabs(centre_x - 4.0) < 0.1 && box.paint && box.traffic) found_traffic = true;
      if (std::fabs(centre_x + 4.0) < 0.1 && box.paint && !box.traffic) found_both = true;
      if (std::fabs(centre_x - 8.0) < 0.1 && !box.paint && !box.traffic) found_remembered = true;
      if (std::fabs(centre_x - 12.0) < 0.1 && !box.paint && !box.traffic) found_plain = true;
    }
    Check(found_opponent, "the opponent's box has paint and is not traffic's");
    Check(found_traffic, "the traffic car's box has paint and is traffic's");
    Check(found_both, "a car with a draw of each paint is an opponent's box (the customisable paint decides)");
    Check(found_remembered && found_plain, "a car placed from memory and a car with no paint draw have no paint, so no traffic flag");
    // the marks are remade by every call and never leak
    ci::List again = list;
    for (ci::ViewBox& b : again.boxes) {
      b.paint = true;
      b.traffic = true;
    }
    op::MarkPaintBoxes(&again);
    bool same = again.boxes.size() == list.boxes.size();
    for (size_t b = 0; same && b < list.boxes.size(); ++b) {
      same = again.boxes[b].paint == list.boxes[b].paint && again.boxes[b].traffic == list.boxes[b].traffic;
    }
    Check(same, "MarkPaintBoxes clears and remakes both marks");
    // only traffic: the player's record is there too (a traffic car's reflection finds the player's car through it)
    std::vector<ci::OtherDraw> only_traffic = {draw(2, 21, 4.0, 14.0, ci::kTrafficPaintPs)};
    std::vector<ci::OtherCar> one_car(1);
    one_car[0].stencil_id = 2;
    one_car[0].Add(only_traffic[0].model_to_world, 10.0);
    ci::List solo;
    ci::AppendOthers(&solo, only_traffic, one_car, view, false, 640, 640, find, 8);
    op::AppendPlayerRecord(&solo, nullptr);
    Check(solo.other_paint_instances == 1 && solo.traffic_paint_instances == 1 && solo.player_row != 0 && solo.boxes.size() == 1 && solo.boxes[0].traffic, "a scene with only a traffic car: one traffic paint instance, the player's record added, a traffic box");
    solo.Clear();
    Check(solo.traffic_paint_instances == 0 && solo.other_paint_instances == 0, "Clear resets the traffic count");
  }

  // ---- the level --------------------------------------------------------------------------------------------------------------------
  {
    const tr::Level high = tr::LevelFor(GpuClass::kHigh), mid = tr::LevelFor(GpuClass::kMid), low = tr::LevelFor(GpuClass::kLow);
    Check(high.on && Near(high.share, 0.40) && high.max_depth == 45.0f && high.min_texels == 600, "High: on, 40 percent of the budget, 45 m, 600 texels");
    Check(mid == high && low == high, "traffic uses the same distance, share and texel minimum across classes");
    Check(Near(double(op::TexelBudget(GpuClass::kMid, -1, 0)) * mid.share, 200000.0), "the traffic share is 200k texels");
    Check(tr::LevelWith(high, -1) == high && tr::LevelWith(mid, -1) == mid && tr::LevelWith(low, -1) == low, "-1 keeps the class's level");
    Check(!tr::LevelWith(high, 0).on && !tr::LevelWith(mid, 0).on, "0 turns the traffic off");
    const tr::Level fifty = tr::LevelWith(high, 50);
    Check(fifty.on && Near(fifty.share, 0.5) && fifty.max_depth == high.max_depth && fifty.min_texels == high.min_texels, "50 is half the budget, the class's distance and size stand");
    const tr::Level forced_low = tr::LevelWith(low, 30);
    Check(forced_low.on && Near(forced_low.share, 0.3) && forced_low.max_depth == high.max_depth, "asked for by hand on a class with none: the High class's distance and size stand in");
    Check(Near(tr::LevelWith(high, 250).share, 1.0), "over 100 is held to the whole budget");
    Check(tr::LevelWith(mid, 80).max_depth == mid.max_depth, "a class that has a level keeps its own distance under an override");
    ci::ViewBox box;
    const float lo[3] = {-1, 0, 7.5f}, hi[3] = {1, 1.5f, 12.0f};
    box.Add(lo);
    box.Add(hi);
    Check(tr::NearDepth(box) == 7.5f, "the nearest face's depth is the box's minimum z");
    ci::ViewBox around;
    const float lo2[3] = {-1, 0, -2.0f}, hi2[3] = {1, 1.5f, 3.0f};
    around.Add(lo2);
    around.Add(hi2);
    Check(tr::NearDepth(around) == 0.0f, "a box that holds the camera's plane is at 0");
  }

  // ---- the plan, scene by scene ------------------------------------------------------------------------------------------------------
  {
    const uint64_t budget = 500000;
    const tr::Level high = tr::LevelFor(GpuClass::kHigh);
    {
      const tr::TrafficPlan p = tr::PlanDispatches({Car(160000, true, 6.0f)}, budget, high);
      Check(p.traffic_cars == 1 && p.traffic_admitted == 1 && p.plan.admitted == 1 && p.traffic_texels == 160000 && p.plan.rays == op::kRaysFull, "one traffic car beside the player, 160k texels (under the budget): traced, the whole cone");
    }
    {
      const tr::TrafficPlan p = tr::PlanDispatches({Car(1200000, true, 4.0f)}, budget, high);
      Check(p.traffic_admitted == 1 && p.plan.rays == op::kRaysFull, "a traffic car filling the picture (1.2M, over its share and the budget's twice) is still the first and always traced, with the full cone");
    }
    {
      // four traffic cars: the nearest is the first; the second does not fit the 200k share; the third and fourth do
      const tr::TrafficPlan p = tr::PlanDispatches({Car(9000, true, 25.0f, 0), Car(60000, true, 12.0f, 3000), Car(150000, true, 6.0f, 6000), Car(40000, true, 14.0f, 9000)}, budget, high);
      Check(p.traffic_cars == 4 && p.traffic_admitted == 3 && p.traffic_over == 1 && p.traffic_far == 0, "150k first, then 60k does not fit the share (210k > 200k), 40k and 9k do");
      Check(p.traffic_texels == 150000 + 40000 + 9000 && p.plan.texels == p.traffic_texels, "the admitted areas are summed");
      Check(p.traffic_texels <= uint64_t(high.share * double(budget)), "the traffic sum is within the share (the first car aside)");
    }
    {
      const tr::TrafficPlan far = tr::PlanDispatches({Car(5000, true, 60.0f), Car(300, true, 10.0f), Car(20000, true, 45.0f), Car(20000, true, 45.01f)}, budget, high);
      Check(far.traffic_far == 3 && far.traffic_admitted == 1, "too far (60 m, and 45.01 m: the limit is 45) and too small (300 texels) are left out; a car at exactly 45 m is traced");
    }
    {
      const tr::TrafficPlan off = tr::PlanDispatches({Car(50000, true, 5.0f), Car(60000, false, 6.0f)}, budget, tr::LevelWith(high, 0));
      Check(off.traffic_admitted == 0 && off.traffic_far == 1 && off.plan.admitted == 1 && off.plan.texels == 60000, "the traffic off: left out, the opponent as it was");
    }
    {
      // the opponents first, untouched by the traffic
      const std::vector<tr::CarRect> scene = {Car(450000, false, 5.0f, 0), Car(400000, false, 7.0f, 5000), Car(100000, true, 6.0f, 10000), Car(100000, true, 8.0f, 15000)};
      const tr::TrafficPlan p = tr::PlanDispatches(scene, budget, high);
      Check(p.plan.admitted == 3 && p.traffic_admitted == 1 && p.traffic_over == 1 && p.plan.texels == 950000,
            "two opponents (850k) and two traffic cars: the first traffic car fits the pass's limit (950k <= 1M), the second does not (1.05M)");
      Check(p.plan.rays == op::kRaysFull, "the cone keeps its samples as coverage grows");
      const tr::TrafficPlan opponents_only = tr::PlanDispatches({scene[0], scene[1]}, budget, high);
      Check(opponents_only.plan.admitted == 2 && opponents_only.plan.skipped == 0, "the opponents are admitted the same without the traffic");
    }
    {
      // the pass is full of opponents: the traffic car is over the limit even as the first traffic car
      const tr::TrafficPlan p = tr::PlanDispatches({Car(600000, false, 5.0f, 0), Car(390000, false, 6.0f, 5000), Car(30000, true, 7.0f, 10000)}, budget, high);
      Check(p.plan.admitted == 2 && p.traffic_admitted == 0 && p.traffic_over == 1, "opponents at 990k leave no room: the first traffic car is over the pass's limit (1.02M)");
    }
    {
      // no budget, no cars, cars off the picture
      Check(tr::PlanDispatches({Car(1000, true)}, 0, high).plan.dispatches.empty() && tr::PlanDispatches({}, budget, high).plan.dispatches.empty(), "no budget and no cars: nothing");
      tr::CarRect off_picture;
      off_picture.traffic = true;
      const tr::TrafficPlan p = tr::PlanDispatches({off_picture}, budget, high);
      Check(p.traffic_cars == 0 && p.plan.cars == 0 && p.plan.dispatches.empty(), "a car with no rectangle in the picture is not a car of the plan");
    }
    {
      const tr::TrafficPlan forced = tr::PlanDispatches({Car(160000, true)}, budget, high, op::kRaysMirror);
      Check(forced.plan.rays == op::kRaysMirror, "the rays can be forced");
      const tr::TrafficPlan merged = tr::PlanDispatches({Car(20000, true, 10.0f, 0), Car(20000, true, 12.0f, 100)}, budget, high);
      Check(merged.plan.dispatches.size() >= 1 && merged.plan.covered >= merged.plan.texels / 2, "the rectangles are merged by the opponents' rule");
    }
    // the Mid class: a car beside the player is far over its share and still traced (with fewer rays)
    {
      const tr::Level mid = tr::LevelFor(GpuClass::kMid);
      const uint64_t mid_budget = op::TexelBudget(GpuClass::kMid, -1, 0);
      const tr::TrafficPlan p = tr::PlanDispatches({Car(260000, true, 6.0f, 0), Car(30000, true, 14.0f, 5000), Car(30000, true, 16.0f, 9000)}, mid_budget, mid);
      Check(p.traffic_admitted == 1 && p.traffic_over == 2 && p.plan.rays == op::kRaysFull, "the first car is traced alone; the remaining cars exceed the share");
    }
  }

  // ---- the plan on random scenes, against the opponents' own -------------------------------------------------------------------------
  {
    std::mt19937 rng(20261121u);
    std::uniform_int_distribution<int> count(0, 9);
    std::uniform_int_distribution<uint64_t> area(100, 900000);
    std::uniform_real_distribution<float> depth(1.0f, 80.0f);
    std::uniform_int_distribution<int> coin(0, 1);
    bool equal_without_traffic = true, opponents_untouched = true, share_held = true, limit_held = true, counters_add = true, rays_follow = true, conditions = true, deterministic = true;
    for (int trial = 0; trial < 600; ++trial) {
      const GpuClass cls = coin(rng) ? GpuClass::kHigh : GpuClass::kMid;
      const uint64_t budget = op::TexelBudget(cls, -1, 0);
      const tr::Level level = tr::LevelFor(cls);
      std::vector<tr::CarRect> scene;
      std::vector<op::TexelRect> opponents_rects;
      const int n = count(rng);
      for (int i = 0; i < n; ++i) {
        const bool is_traffic = coin(rng) != 0;
        tr::CarRect c = Car(area(rng), is_traffic, depth(rng), i * 4000);
        scene.push_back(c);
        if (!is_traffic) opponents_rects.push_back(c.rect);
      }
      const tr::TrafficPlan p = tr::PlanDispatches(scene, budget, level);
      deterministic = deterministic && p == tr::PlanDispatches(scene, budget, level);
      // the opponents alone: the opponents' own plan
      std::vector<tr::CarRect> opponents_scene;
      for (const tr::CarRect& c : scene) if (!c.traffic) opponents_scene.push_back(c);
      const tr::TrafficPlan only = tr::PlanDispatches(opponents_scene, budget, level);
      const op::Plan old_plan = op::PlanDispatches(opponents_rects, budget);
      equal_without_traffic = equal_without_traffic && only.plan.dispatches == old_plan.dispatches && only.plan.cars == old_plan.cars && only.plan.admitted == old_plan.admitted &&
                              only.plan.skipped == old_plan.skipped && only.plan.texels == old_plan.texels && only.plan.covered == old_plan.covered && only.plan.rays == old_plan.rays;
      // adding traffic does not change which opponents are admitted
      opponents_untouched = opponents_untouched && (p.plan.admitted - p.traffic_admitted) == only.plan.admitted && (p.plan.skipped - p.traffic_over) == only.plan.skipped;
      const double limit = op::kAdmitFactor * double(budget);
      share_held = share_held && (p.traffic_admitted <= 1 || double(p.traffic_texels) <= level.share * double(budget) + 1e-9);
      limit_held = limit_held && (double(p.plan.texels) <= limit || p.plan.admitted == 1);
      counters_add = counters_add && p.traffic_admitted + p.traffic_far + p.traffic_over == p.traffic_cars && p.plan.admitted + p.plan.skipped + p.traffic_far == p.plan.cars &&
                     p.traffic_texels <= p.plan.texels;
      rays_follow = rays_follow && (p.plan.admitted == 0 || p.plan.rays == op::RaysFor(0));
      // every traffic car admitted passes the near and small rules: recount by re-planning with the car alone
      for (const tr::CarRect& c : scene) {
        if (!c.traffic) continue;
        const tr::TrafficPlan alone = tr::PlanDispatches({c}, budget, level);
        const bool eligible = c.near_depth <= level.max_depth && c.rect.Area() >= level.min_texels;
        conditions = conditions && (alone.traffic_admitted == (eligible ? 1u : 0u));
      }
    }
    Check(equal_without_traffic, "600 random scenes: with no traffic car the plan is the opponents' own (dispatches, counts, areas, rays)");
    Check(opponents_untouched, "...adding traffic cars never changes which opponents are admitted or left out");
    Check(share_held, "...the traffic cars' areas stay within the share unless the first alone is over it");
    Check(limit_held, "...the pass's sum stays within twice the budget unless one car alone is over it");
    Check(counters_add, "...the counters add up (admitted + far + over = the traffic cars; admitted + skipped + far = the cars)");
    Check(rays_follow, "...the rays are the total's (RaysFor)");
    Check(conditions, "...a traffic car alone is traced exactly when it is near enough and big enough");
    Check(deterministic, "...the same scene gives the same plan");
  }

  // ---- the estimate -------------------------------------------------------------------------------------------------------------------
  {
    Check(Near(tr::EstimateMs(500000, 4), 0.7) && Near(tr::EstimateMs(1000000, 4), 1.4), "the estimate: 0.7 ms for the opponents' budget of 500k texels at the full cone (the middle of 0.5 to 0.9)");
    Check(tr::EstimateMs(200000, 4) < 0.3 && tr::EstimateMs(200000, 4) > 0.25, "the High class's whole traffic share costs about 0.28 ms at the full cone");
    Check(tr::EstimateMs(100000, 4) > tr::EstimateMs(100000, 2) && tr::EstimateMs(100000, 2) > tr::EstimateMs(100000, 1) && tr::EstimateMs(100000, 1) > 0.0, "fewer rays cost less, but not nothing (the primary ray and the shading)");
    Check(tr::EstimateMs(0, 4) == 0.0 && tr::EstimateMs(100000, 9) == tr::EstimateMs(100000, 4), "no texels no cost; more than four rays is four");
    Check(tr::EstimateMs(40000 + 9000 + 9000, 4) < 0.1, "three traffic cars at 12 m, 25 m and 25 m cost under a tenth of a millisecond");
  }

  // ---- the host, the variant, the settings ----------------------------------------------------------------------------------------------
  {
    const std::string host = ReadFile(src + "/gpu/car_rt_opp_refl.cpp");
    const std::string host_h = ReadFile(src + "/gpu/car_rt_opp_refl.h");
    const std::string scene = ReadFile(src + "/gpu/car_rt_scene.cpp");
    const std::string scene_h = ReadFile(src + "/gpu/car_rt_scene.h");
    const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
    const std::string variant = ReadFile(src + "/shaderpack/variants/car_selfrefl.cpp");
    Check(!host.empty() && !scene.empty() && !cmake.empty() && !variant.empty(), "the sources were read (give the src folder as the first argument)");
    Check(Has(host, "traffic::LevelWith(traffic::LevelFor(gpu_class::CurrentRtClass()), int(larecomp::settings::mcla_gpu_car_rt_traffic_refl.Get()))") &&
              Has(host, "traffic::PlanDispatches(rects, budget, traffic_level,") && Has(host, "car.traffic = view.boxes[i].traffic;") && Has(host, "car.near_depth = traffic::NearDepth(view.boxes[i]);") &&
              Has(host, "const opp::Plan& plan = traffic_plan.plan;"),
          "the pass makes the class's level with the switch, plans with the traffic rule and gives it each box's traffic flag and depth");
    Check(Has(host, "if (view.other_paint_instances - traffic_paint == 0 && (!traffic_level.on || traffic_paint == 0))"), "no dispatch when the frame has no paint but traffic's and the traffic is off");
    Check(Has(scene, "view_.traffic_paint_instances = cars.traffic_paint_instances;") && Has(scene_h, "uint32_t traffic_paint_instances = 0;"), "the scene hands the pass the traffic paint count");
    Check(Has(host_h, "mcla_gpu_car_rt_traffic_refl"), "the pass's header names the switch");
    Check(Has(cmake, "larecomp_car_rt_traffic_rules_test") && Has(cmake, "car_rt_traffic_rules_test.cpp"), "the build has the test");
    Check(Has(variant, "def.pixel_shaders = {kCarPaint, kTrafficPaint};") && Has(variant, "TransformCarSelfReflTrafficHlsl(shader, hlsl, ctx)"), "the variant's second shader is the traffic paint, with its own transform");
    const setting_source::Info sw = setting_source::Find(src + "/gpu", host, "mcla_gpu_car_rt_traffic_refl");
    Check(sw.found && sw.debug_only && sw.live && sw.default_text == "-1" && sw.has_range && sw.range_lo == -1.0 && sw.range_hi == 100.0, "the switch is a live, debug-only integer, -1 by default, from -1 to 100");
  }

  // ---- RT-NEXT-DAY: the stencil census of the traffic paint -----------------------------------------------------------------------
  {
    const std::string none = ci::NoStencilNote(0, 0), all = ci::NoStencilNote(7, 0), nothing = ci::NoStencilNote(0, 9), some = ci::NoStencilNote(3, 5);
    Check(none.find("no traffic car's paint reached") != std::string::npos && all.find("every traffic car carried") != std::string::npos, "the census note says when nothing was drawn and when every car had an ID");
    Check(nothing.find("NONE carried a vehicle stencil ID") != std::string::npos && nothing.find("parked car has none") != std::string::npos && nothing.find("no draw in the ray traced scene") != std::string::npos,
          "the census note says that a parked car (no ID at all) has no pixel in the history, no draw in the scene and no read of the reflection");
    Check(some.find("some carried none") != std::string::npos && some != nothing && some != all && none != all, "the four cases have four notes");
    const std::string selfrefl = ReadFile(src + "/gpu/car_selfrefl.cpp");
    const std::string selfrefl_h = ReadFile(src + "/gpu/car_selfrefl.h");
    const std::string main_scene = ReadFile(src + "/gpu/scene.cpp");
    Check(!selfrefl.empty() && !selfrefl_h.empty() && !main_scene.empty(), "the sources were read (give the src folder as the first argument)");
    Check(Has(selfrefl, "car_rt_instances::PaintKindOf(ps_identity) == car_rt_instances::PaintKind::kTraffic") && Has(selfrefl, "traffic_no_id_ += traffic_paint ? 1u : 0u;") && Has(selfrefl, "traffic_with_id_ += traffic_paint ? 1u : 0u;") &&
              Has(selfrefl, "traffic paint draws since the start: {} with a stencil ID, {} without ({})") && Has(selfrefl, "car_rt_instances::NoStencilNote(traffic_with_id_, traffic_no_id_)"),
          "the paint variant counts the traffic paint's draws with and without a stencil ID and the census line says so");
    Check(Has(selfrefl_h, "uint64_t ps_identity, car_selfrefl::CarSelfReflConstants* out);") && Has(main_scene, "car_refl_->FillConstants(d.state, UpscalerRunning(), d.ps ? d.ps->key : 0, &block);"),
          "the scene gives the draw's pixel shader to the paint variant's constants");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

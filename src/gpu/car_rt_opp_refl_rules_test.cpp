// Check of the opponents' ray traced reflection's rules (car_rt_opp_refl_rules.h): the paint's instance bit and the primary ray's parts, the class budget and its overrides, the
// rectangle of an other car's box in the history image (against the image places of car_rt_refl_rules.h), the merging of rectangles, the plan of a frame (which cars, how many
// rays), the aux image's instance codes, the player's record in the instances' table (against the list AppendOthers makes), the decision for a hit on the player's car, and that
// hlsl/car_rt_refl_opp.hlsli, car_rt_refl_cs.hlsl, car_rt_common.hlsli, the host (car_rt_opp_refl.cpp, car_rt_refl.cpp) and the CMake file hold the header's numbers.
// A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_opp_refl_rules_test, then run src/gpu/larecomp_car_rt_opp_refl_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and shaderpack; by default the one this file is in)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "car_rt_opp_refl_rules.h"
#include "setting_source.h"

namespace ci = mcla::gpu::car_rt_instances;
namespace rr = mcla::gpu::car_rt_rules;
namespace rt = mcla::gpu::car_rt_refl;
namespace op = mcla::gpu::car_rt_opp_refl;
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

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

ci::ViewBox Box(float x0, float y0, float z0, float x1, float y1, float z1) {
  ci::ViewBox b;
  const float lo[3] = {x0, y0, z0};
  const float hi[3] = {x1, y1, z1};
  b.Add(lo);
  b.Add(hi);
  return b;
}

// A 60 degree vertical field of view at 16:9, a 3840 x 2160 picture, the history image half of it (divisor 2: 1920 x 1080).
constexpr double kTanY = 0.5773502691896257;
constexpr double kTanX = kTanY * 16.0 / 9.0;

op::TexelRect RectOf(const ci::ViewBox& box, double divisor = 2.0, double picture_w = 3840.0, double picture_h = 2160.0) {
  return op::BoxRect(box, kTanX, kTanY, picture_w, picture_h, divisor, int32_t(picture_w / divisor), int32_t(picture_h / divisor));
}

ci::OtherDraw Draw(uint32_t id, uint64_t key, double x, double y, double z, uint64_t ps) {
  ci::OtherDraw d;
  d.stencil_id = id;
  d.mesh_key = key;
  d.ps = ps;
  d.model_to_world = Translation(x, y, z);
  d.indices = 300;
  return d;
}

ci::OtherCar MakeCar(uint32_t id, double distance, uint32_t draws, double x, double z) {
  ci::OtherCar c;
  c.stencil_id = id;
  c.distance = distance;
  c.draws = draws;
  c.sum[0] = x * draws;
  c.sum[2] = z * draws;
  return c;
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

  // ---- the parts -----------------------------------------------------------------------------------------------------
  {
    Check(rr::kMaskOtherPaint == 0x40 && op::kPrimaryParts == 0x40, "the paint bit is 0x40 and the opponents' primary ray has it alone");
    const uint32_t paint = ci::OtherMask(mcla::gpu::car_dump::kCarPaintPs);
    const uint32_t chrome = ci::OtherMask(ci::kChromePs);
    const uint32_t other = ci::OtherMask(0x1234ull);
    Check(paint == (rr::kMaskOthers | rr::kMaskOtherPaint) && chrome == (rr::kMaskOthers | rr::kMaskOtherChrome) && other == rr::kMaskOthers,
          "a paint draw's instance is 0x50, a chrome draw's 0x30, any other draw's 0x10 (the others' mask stays on all three)");
    const uint32_t reflection = rt::kDefaultReflectionParts;
    const uint32_t occlusion = rr::kMaskBody | rr::kMaskTyres | rr::kMaskOthers;
    Check((paint & reflection) != 0 && (paint & occlusion) != 0 && (paint & 0xFFu) == paint, "the reflection's, the occlusion's and the check's rays still hit a paint instance");
    Check((op::kPrimaryParts & ci::OtherMask(ci::kChromePs)) == 0 && (op::kPrimaryParts & rr::kMaskBody) == 0, "the opponents' primary ray finds neither another car's chrome nor the player's body");
    Check((rt::kPrimaryTraceParts & paint) == 0 && (rt::kPrimaryParts & paint) == 0, "the player's pass's primary ray never finds another car's paint (the two dispatches share no texel)");
    Check(mcla::gpu::car_dump::kCarPaintPs == 0x439B193A3F675533ull, "the paint shader's identity");
  }

  // ---- the budget ---------------------------------------------------------------------------------------------------------
  {
    for (const auto cls : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
      Check(op::TexelBudget(cls, -1, 0) == 500000, "the default texel budget is 500k on every class");
    }
    Check(op::TexelBudget(GpuClass::kHigh, -1, 300) == 300000 && op::TexelBudget(GpuClass::kLow, -1, 100) == 100000, "the texel override is in thousands and wins");
  }

  // ---- the rectangle of a box ---------------------------------------------------------------------------------------------------
  {
    // A car 4.5 m x 1.5 m x 1.8 m, 8 m ahead, 2 m to the right: its rectangle holds the projection of every point of the box.
    const ci::ViewBox box = Box(0.0f, -1.0f, 8.0f, 1.8f, 0.5f, 12.5f);
    const op::TexelRect r = RectOf(box);
    Check(!r.Empty() && r.x0 >= 0 && r.y0 >= 0 && r.x1 <= 1920 && r.y1 <= 1080, "a car ahead has a rectangle inside the image");
    std::mt19937 rng(17);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    int inside = 0, outside = 0;
    for (int i = 0; i < 4000; ++i) {
      const float p[3] = {box.min[0] + u(rng) * (box.max[0] - box.min[0]), box.min[1] + u(rng) * (box.max[1] - box.min[1]), box.min[2] + u(rng) * (box.max[2] - box.min[2])};
      const rt::ImagePoint ip = rt::ViewToImage(p, float(kTanX), float(kTanY), 3840.0f, 2160.0f, 2.0f);
      if (!ip.inside) {
        continue;
      }
      const int tx = int(std::lround(ip.x)), ty = int(std::lround(ip.y));
      (tx >= r.x0 && tx < r.x1 && ty >= r.y0 && ty < r.y1) ? ++inside : ++outside;
    }
    Check(inside > 3000 && outside == 0, "every point of the box that the image holds is inside its rectangle (" + std::to_string(inside) + " points, " + std::to_string(outside) + " outside)");
    // The rectangle is tight: the box's projection (the points above) plus the margin and a texel's rounding, no more.
    Check(r.Width() < 340 && r.Height() < 270 && r.Area() > 20000, "the rectangle is about the box's size on the screen (" + std::to_string(r.Width()) + " x " + std::to_string(r.Height()) + ")");
    // divisor 1 on a 1920 x 1080 picture: the same car covers the same share of the image
    const op::TexelRect r1 = RectOf(box, 1.0, 1920.0, 1080.0);
    Check(std::abs(double(r1.Width()) / 1920.0 - double(r.Width()) / 1920.0) < 0.01 && std::abs(double(r1.Height()) / 1080.0 - double(r.Height()) / 1080.0) < 0.01,
          "the share of the image a car covers does not depend on the divisor");
    // Behind the camera, on the plane, off to the side, huge, degenerate.
    Check(RectOf(Box(-1, -1, -9, 1, 1, -3)).Empty(), "a box wholly behind the camera: empty");
    const op::TexelRect plane = RectOf(Box(-1, -1, -1, 1, 1, 5));
    Check(plane == op::TexelRect({0, 0, 1920, 1080}), "a box that crosses the camera's plane: the whole image (conservative)");
    Check(RectOf(Box(40, -1, 5, 42, 1, 9)).Empty(), "a box far off to the right of the picture: empty");
    const op::TexelRect clip = RectOf(Box(8, -1, 5, 11, 1, 9));
    Check(clip.Empty() || (clip.x1 == 1920 && clip.x0 > 0), "a box half off the picture is clipped at its edge, a box that begins beyond the margin is empty");
    Check(RectOf(ci::ViewBox{}).Empty(), "no box: empty");
    float nan = std::nanf("");
    Check(op::BoxRect(Box(nan, 0, 5, 1, 1, 9), kTanX, kTanY, 3840, 2160, 2, 1920, 1080).Empty(), "a box with a NaN: empty");
    Check(op::BoxRect(box, 0.0, kTanY, 3840, 2160, 2, 1920, 1080).Empty() && op::BoxRect(box, kTanX, kTanY, 3840, 2160, 0.0, 1920, 1080).Empty(), "no lens, no divisor: empty");
    // A far car is small and a near one big, centred where the centre of the picture is.
    const op::TexelRect far_car = RectOf(Box(-1.0f, -1.0f, 30.0f, 1.0f, 0.5f, 34.5f));
    const op::TexelRect near_car = RectOf(Box(-1.0f, -1.0f, 6.0f, 1.0f, 0.5f, 10.5f));
    Check(far_car.Area() < near_car.Area() / 4 && far_car.x0 > 700 && far_car.x1 < 1220, "a far car's rectangle is much smaller and round the picture's middle");
    // The up axis: a higher box is higher in the image (smaller y).
    Check(RectOf(Box(-1, 3, 10, 1, 5, 14)).y1 < RectOf(Box(-1, -3, 10, 1, -1, 14)).y0, "y up in the view space is up in the image");
  }

  // ---- merging ---------------------------------------------------------------------------------------------------------------
  {
    using R = op::TexelRect;
    std::vector<R> m = op::MergeRects({R{0, 0, 100, 100}, R{100, 50, 200, 150}});
    Check(m.size() == 1 && m[0] == R({0, 0, 200, 150}), "two rectangles that touch become their union");
    m = op::MergeRects({R{0, 0, 100, 100}, R{300, 300, 400, 400}});
    Check(m.size() == 2, "two apart stay two");
    m = op::MergeRects({R{0, 0, 10, 10}, R{20, 0, 30, 10}, R{40, 0, 50, 10}, R{60, 0, 70, 10}, R{80, 0, 90, 10}}, 4);
    Check(m.size() == 4, "five apart with at most four: the pair whose union is the smallest is merged");
    m = op::MergeRects({R{0, 0, 10, 10}, R{20, 0, 30, 10}, R{40, 0, 50, 10}}, 1);
    Check(m.size() == 1 && m[0] == R({0, 0, 50, 10}), "at most one: the union of all");
    m = op::MergeRects({R{}, R{5, 5, 5, 9}, R{0, 0, 10, 10}});
    Check(m.size() == 1 && m[0] == R({0, 0, 10, 10}), "empty rectangles are dropped");
    Check(op::MergeRects({}).empty(), "nothing to merge");
    // Chains: A touches B, B touches C, A and C do not touch: all three become one.
    m = op::MergeRects({R{0, 0, 10, 10}, R{40, 0, 50, 10}, R{10, 0, 40, 10}});
    Check(m.size() == 1 && m[0] == R({0, 0, 50, 10}), "a chain of touching rectangles becomes one whatever the order");
    Check(op::GroupsX(R{3, 3, 20, 4}) == 3 && op::GroupsY(R{3, 3, 20, 4}) == 1 && op::GroupsX(R{0, 0, 8, 8}) == 1 && op::GroupsX(R{}) == 0, "the groups of a dispatch: 8 x 8, rounded up");
  }

  // ---- the plan of a frame -----------------------------------------------------------------------------------------------------
  {
    using R = op::TexelRect;
    const uint64_t budget = 500000;
    const R small{0, 0, 400, 300};        // 120k
    const R medium{600, 0, 1100, 500};    // 250k
    const R large{0, 600, 1000, 1080};    // 480k
    const R giant{0, 0, 1920, 1080};      // 2.07M
    op::Plan p = op::PlanDispatches({small}, budget);
    Check(p.admitted == 1 && p.rays == 4 && p.dispatches.size() == 1 && p.covered == small.Area() && p.skipped == 0 && p.texels == small.Area(), "one small car: admitted, the full cone");
    p = op::PlanDispatches({small, medium}, budget);
    Check(p.admitted == 2 && p.rays == 4 && p.texels == 370000 && p.dispatches.size() == 2, "two cars within the budget: both, the full cone, two dispatches");
    p = op::PlanDispatches({small, medium, large}, budget);
    Check(p.admitted == 3 && p.texels == 850000 && p.rays == 2 && p.skipped == 0, "over the budget but within twice it: all admitted, the outer pair of rays");
    p = op::PlanDispatches({large, large, medium}, budget);
    Check(p.admitted == 2 && p.skipped == 1 && p.texels == 960000 && p.rays == 2, "two large cars fill twice the budget: the third is left out, the pair casts the outer rays");
    p = op::PlanDispatches({large, R{1100, 600, 1900, 1080}, medium, small}, budget);
    Check(p.admitted == 3 && p.skipped == 1 && p.texels <= 1000000 && p.rays == 2, "the cars come by area, largest first, while the sum is within twice the budget: the smallest is left out");
    p = op::PlanDispatches({small, giant}, budget);
    Check(p.admitted == 1 && p.skipped == 1 && p.rays == 1 && p.dispatches.size() == 1 && p.dispatches[0] == giant, "a car filling the picture is admitted (the first always is), at the mirror ray, and the small one is left out");
    p = op::PlanDispatches({giant}, budget);
    Check(p.admitted == 1 && p.rays == op::kRaysMirror, "a lone giant: admitted at the mirror");
    p = op::PlanDispatches({small}, budget, 1);
    Check(p.rays == 1 && op::PlanDispatches({giant}, budget, 4).rays == 4 && op::PlanDispatches({small}, budget, 3).rays == 4, "the rays override takes 1, 2 or 4; another number is no override");
    Check(op::PlanDispatches({small}, 0).dispatches.empty() && op::PlanDispatches({small}, 0).admitted == 0, "no budget: no pass");
    Check(op::PlanDispatches({}, budget).dispatches.empty() && op::PlanDispatches({R{}, R{9, 9, 9, 9}}, budget).cars == 0, "no cars, or only empty rectangles: nothing");
    p = op::PlanDispatches({R{0, 0, 100, 100}, R{50, 50, 150, 150}}, budget);
    Check(p.admitted == 2 && p.dispatches.size() == 1 && p.covered == R({0, 0, 150, 150}).Area() && p.texels == 20000, "two cars whose rectangles overlap: counted each, covered once (one dispatch)");
    std::vector<R> many;
    for (int i = 0; i < 8; ++i) many.push_back(R{i * 200, 0, i * 200 + 100, 100});
    p = op::PlanDispatches(many, budget);
    Check(p.dispatches.size() <= op::kMaxDispatches && p.admitted == 8, "eight cars: at most four dispatches");
    // The decision is by the sum of the admitted rectangles.
    Check(op::RaysFor(500000, 500000) == 4 && op::RaysFor(500001, 500000) == 2 && op::RaysFor(1000000, 500000) == 2 && op::RaysFor(1000001, 500000) == 1, "the rays by the sum against the budget: 4, 2 over it, 1 over twice");
    Check(op::Describe(op::PlanDispatches({small, medium}, budget)).find("2 of 2 cars") != std::string::npos, "the log's words");
  }

  // ---- the aux image's instance codes ---------------------------------------------------------------------------------------------
  {
    bool in_range = true, round_trip = true;
    for (uint32_t i = 0; i < 1100; ++i) {
      const float e = op::EncodeInstance(i);
      // the 8-bit unorm image: the shader's value goes through round(x * 255) / 255, the denoise reads int(y * 255 + 0.5)
      const int code = op::DecodeInstance(e);
      in_range = in_range && code >= 200 && code <= 254;
      round_trip = round_trip && code == int(op::kCodeBase + i % op::kCodes);
    }
    Check(in_range && round_trip, "an opponent's code is 200 .. 254 after the 8-bit image, never 255 (the chrome texel's) nor the player's (under 200)");
    Check(op::DecodeInstance(op::EncodeInstance(195)) != op::DecodeInstance(op::EncodeInstance(196)) && op::DecodeInstance(op::EncodeInstance(300)) != op::DecodeInstance(op::EncodeInstance(301)),
          "neighbouring instances have different codes");
    Check(op::DecodeInstance(op::EncodeInstance(10)) == op::DecodeInstance(op::EncodeInstance(10 + op::kCodes)), "two panels kCodes apart share a code (the history's distance still tells them apart)");
  }

  // ---- the player's record and the table ----------------------------------------------------------------------------------------------
  {
    const tp::Mat4 view = tp::Identity();
    const auto find = [&](uint64_t key, ci::MeshRef* ref) {
      *ref = ci::MeshRef{uint32_t(key) * 10, uint32_t(key) * 5};
      return true;
    };
    ci::List list;
    for (int i = 0; i < 3; ++i) {  // the player's three instances
      ci::Item item;
      item.mesh_key = 100;
      item.mask = rr::kMaskBody;
      list.items.push_back(item);
      list.rows.push_back(rr::MakeRow(0, 0, rr::kMaskBody, false, uint32_t(i)));
    }
    list.player_box.Add(std::array<float, 3>{-0.9f, 0.0f, 3.0f}.data());
    list.player_box.Add(std::array<float, 3>{0.9f, 1.4f, 7.5f}.data());
    std::vector<ci::OtherCar> cars = {MakeCar(2, 8.0, 3, 3.0, -9.0), MakeCar(3, 5.0, 2, -3.0, -6.0)};
    // car 2: a paint draw, a chrome draw, a glass of another shader; car 3: one paint draw
    std::vector<ci::OtherDraw> draws = {Draw(2, 101, 3, 0, -9, mcla::gpu::car_dump::kCarPaintPs), Draw(2, 102, 3, 0, -9, ci::kChromePs), Draw(2, 103, 3, 0, -9, 0x77ull),
                                        Draw(3, 101, -3, 0, -6, mcla::gpu::car_dump::kCarPaintPs)};
    ci::AppendOthers(&list, draws, cars, view, true, 640, rr::kMaxInstances, find);
    const size_t rows_before = list.rows.size();
    const uint32_t box_row = list.box_row;
    const size_t boxes = list.boxes.size();
    std::vector<rr::InstanceRow> rows_copy = list.rows;
    Check(list.items.size() == 3 + 4 && op::CountPaintInstances(list) == 2, "four instances of other cars, two of them paint");
    Check((list.items[3].mask & rr::kMaskOtherPaint) != 0 && (list.items[4].mask & rr::kMaskOtherPaint) == 0 && (list.items[4].mask & rr::kMaskOtherChrome) != 0 &&
              (list.items[5].mask & (rr::kMaskOtherPaint | rr::kMaskOtherChrome)) == 0 && (list.items[6].mask & rr::kMaskOtherPaint) != 0,
          "the paint bit is on the paint draws only (not the chrome's, not another shader's)");
    Check((list.rows[3].flags & 0xFFu) == list.items[3].mask, "the table's flags word carries the same mask");

    // MarkPaintBoxes: car 3 (the nearer, the first box) has a paint draw, car 2 too: both boxes are marked; a car with chrome and glass only is not
    Check(list.boxes.size() == 2 && !list.boxes[0].paint && !list.boxes[1].paint, "(the boxes are not marked until the record is made)");
    {
      ci::List marked = list;
      op::MarkPaintBoxes(&marked);
      Check(marked.boxes[0].paint && marked.boxes[1].paint, "both cars have a draw of the paint's shader: both boxes are marked");
      // car 4: chrome and another shader's draw only, beyond the others in the plan (farther: the last box)
      ci::List more;
      for (int i = 0; i < 2; ++i) {
        ci::Item item;
        item.mask = rr::kMaskBody;
        more.items.push_back(item);
        more.rows.push_back(rr::MakeRow(0, 0, rr::kMaskBody, false, uint32_t(i)));
      }
      std::vector<ci::OtherCar> cars2 = {MakeCar(4, 6.0, 2, 5.0, -6.0), MakeCar(2, 8.0, 1, 3.0, -9.0), MakeCar(7, 3.0, 1, -4.0, -3.0)};
      std::vector<ci::OtherDraw> draws2 = {Draw(4, 101, 5, 0, -6, ci::kChromePs), Draw(4, 102, 5, 0, -6, 0x77ull), Draw(2, 101, 3, 0, -9, mcla::gpu::car_dump::kCarPaintPs)};
      // car 7 is the nearest and has no draw: a record and no box
      ci::AppendOthers(&more, draws2, cars2, view, true, 640, rr::kMaxInstances, find);
      op::MarkPaintBoxes(&more);
      Check(more.boxes.size() == 2 && !more.boxes[0].paint && more.boxes[1].paint && op::CountPaintInstances(more) == 1,
            "boxes follow the cars with an instance: the car with chrome and another shader's draw is not marked, the car with a paint draw is (a car with no draw has a record and no box)");
      // a remembered car's paint draw has no paint bit
      ci::List mem;
      std::vector<ci::OtherCar> cars3 = {MakeCar(2, 8.0, 1, 3.0, -9.0)};
      cars3[0].remembered = true;
      std::vector<ci::OtherDraw> draws3 = {Draw(2, 101, 3, 0, -9, mcla::gpu::car_dump::kCarPaintPs)};
      ci::AppendOthers(&mem, draws3, cars3, view, true, 640, rr::kMaxInstances, find);
      Check(mem.items.size() == 1 && (mem.items[0].mask & rr::kMaskOtherPaint) == 0 && (mem.items[0].mask & rr::kMaskOthers) != 0 && op::CountPaintInstances(mem) == 0,
            "a car placed from memory is another car's part and has no paint bit: its instances are never a pixel the pass looks for");
      ci::List none;
      op::MarkPaintBoxes(&none);
      Check(none.boxes.empty(), "no list: nothing to mark");
    }
    // the walk's cars: the player's paint known
    std::vector<mcla::gpu::car_model_rt::CarInfo> infos(2);
    infos[0].is_player = false;
    infos[0].paint.known = true;
    infos[0].paint.rgb[0] = 0.9f;  // an opponent's paint: not the player's
    infos[1].is_player = true;
    infos[1].paint.known = true;
    infos[1].paint.rgb[0] = 0.1f;
    infos[1].paint.rgb[1] = 0.2f;
    infos[1].paint.rgb[2] = 0.3f;
    op::AppendPlayerRecord(&list, &infos);
    Check(list.other_paint_instances == 2 && list.player_row == rows_before && list.rows.size() == rows_before + 2, "the record is two rows after every row the list had, named by player_row");
    Check(list.box_row == box_row && list.boxes.size() == boxes && std::equal(rows_copy.begin(), rows_copy.end(), list.rows.begin(), [](const rr::InstanceRow& a, const rr::InstanceRow& b) {
            return std::memcmp(&a, &b, sizeof(a)) == 0;
          }),
          "no row that was there moves: the boxes' rows are where the occlusion reads them");
    // read the record as the shader does (CarRtLoadCar: row.xyz centre bits, row.w radius bits; the next row paint bits, flags bit 0)
    uint32_t words[4], pwords[4];
    std::memcpy(words, &list.rows[list.player_row], 16);
    std::memcpy(pwords, &list.rows[list.player_row + 1], 16);
    float centre[3], paint[3], radius;
    std::memcpy(centre, words, 12);
    std::memcpy(&radius, &words[3], 4);
    std::memcpy(paint, pwords, 12);
    Check(Near(centre[0], 0.0) && Near(centre[1], 0.7) && Near(centre[2], 5.25) && Near(radius, ci::kCarRadius), "the player's centre is the middle of its box, the radius the car's");
    Check(Near(paint[0], 0.1) && Near(paint[1], 0.2) && Near(paint[2], 0.3) && (pwords[3] & 1u) == 1u, "the paint is the player's (not the opponent's), marked known");
    // no paint known, no box
    ci::List bare = list;
    bare.rows.resize(rows_before);
    bare.player_box = ci::ViewBox{};
    op::AppendPlayerRecord(&bare, nullptr);
    std::memcpy(words, &bare.rows[bare.player_row], 16);
    std::memcpy(pwords, &bare.rows[bare.player_row + 1], 16);
    std::memcpy(&radius, &words[3], 4);
    Check(radius == 0.0f && (pwords[3] & 1u) == 0u, "no box and no paint: radius 0 and no paint (the shader finds no colour: the hit is shown as it is seen, or is a miss)");
    // no other car's paint: no record
    ci::List alone;
    ci::Item item;
    item.mask = rr::kMaskBody;
    alone.items.push_back(item);
    alone.rows.push_back(rr::MakeRow(0, 0, rr::kMaskBody, false, 0));
    op::AppendPlayerRecord(&alone, &infos);
    Check(alone.player_row == 0 && alone.other_paint_instances == 0 && alone.rows.size() == 1, "a list with no other car's paint gets no record (the pass does not run)");
    // a second call does not add a second record
    ci::List twice = bare;
    twice.rows.resize(rows_before);
    op::AppendPlayerRecord(&twice, &infos);
    Check(twice.rows.size() == rows_before + 2, "(one record per call)");
    Check(!op::PlayerPaint({}, paint) && op::PlayerPaint(infos, paint), "the paint of the player's car from the walk's cars, none from none");
    list.Clear();
    Check(list.other_paint_instances == 0 && list.player_row == 0 && list.rows.empty(), "Clear forgets the record");
  }

  // ---- a hit on the player's car -----------------------------------------------------------------------------------------------------
  {
    using H = op::PlayerHit;
    Check(op::ResolvePlayerHit(1.0f, 0.0f) == H::kShown && op::ResolvePlayerHit(1.0f, 1.0f) == H::kShown, "a part the history shows whole is shown");
    Check(op::ResolvePlayerHit(0.0f, 1.0f) == H::kRecord && op::ResolvePlayerHit(0.4f, 0.15f) == H::kRecord, "a part it does not show takes the player's record where that is known (at 0.15 of its taps)");
    Check(op::ResolvePlayerHit(0.4f, 0.1f) == H::kShownInShade && op::ResolvePlayerHit(0.0f, 0.0f) == H::kMiss, "no record: shown in shade where partly shown, else a miss");
  }

  // ---- the shaders, the host and the build hold the numbers ----------------------------------------------------------------------------
  {
    const std::string hlsli = ReadFile(src + "/gpu/hlsl/car_rt_refl_opp.hlsli");
    const std::string refl = ReadFile(src + "/gpu/hlsl/car_rt_refl_cs.hlsl");
    const std::string common = ReadFile(src + "/gpu/hlsl/car_rt_common.hlsli");
    const std::string host = ReadFile(src + "/gpu/car_rt_opp_refl.cpp");
    const std::string host_h = ReadFile(src + "/gpu/car_rt_opp_refl.h");
    const std::string player = ReadFile(src + "/gpu/car_rt_refl.cpp");
    const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
    const std::string shaders = ReadFile(src + "/gpu/host_shaders.h");
    const std::string scene = ReadFile(src + "/gpu/car_rt_scene.cpp");
    const std::string inst = ReadFile(src + "/gpu/car_rt_instances.cpp");
    if (hlsli.empty() || refl.empty() || common.empty() || host.empty() || player.empty() || cmake.empty() || shaders.empty()) {
      std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    } else {
      char line[160];
      std::snprintf(line, sizeof(line), "static const uint kOppCodeBase = %uu;", op::kCodeBase);
      Check(Has(hlsli, line), "the shader's code base is the header's");
      std::snprintf(line, sizeof(line), "static const uint kOppCodes = %uu;", op::kCodes);
      Check(Has(hlsli, line) && Has(hlsli, "float(kOppCodeBase + instance % kOppCodes) / 255.0"), "the shader's number of codes is the header's, and its code is the header's");
      Check(Has(common, "#define CAR_RT_MASK_OTHER_PAINT 0x40") && Has(common, "#define CAR_RT_MASK_OTHERS 0x10") && Has(common, "#define CAR_RT_MASK_OTHER_CHROME 0x20"),
            "the shader's masks are the scene's (0x10, 0x20 and 0x40)");
      Check(Has(hlsli, "first.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.z, primary)") && Has(hlsli, "const uint2 texel = id.xy + g_push.opp.xy;") &&
                Has(hlsli, "const uint row = g_push.opp.z;") && Has(hlsli, "const uint maxRays = min(g_push.opp.w, LevelRays(g_push.slots.w));") && Has(hlsli, "named.draw = row;"),
            "the primary ray takes the push's mask, the dispatch starts at opp.xy, the player's record is opp.z, the rays are opp.w");
      Check(Has(hlsli, "if (playerKnown >= kOtherCarMinShare)") && Has(hlsli, "standIn = seen[j] * g_push.tune.x;") && Has(hlsli, "fade[j] = 0.0;  // hidden, and the player's colour is not known") &&
                Has(hlsli, "needPlayer = needPlayer || (fade[k] > 0.0 && visible[k] < 1.0 && !other[k]);"),
            "a hit on the player's car is decided as ResolvePlayerHit says: the record's colour where known, the shown colour in shade where partly shown, else a miss");
      Check(Has(hlsli, "if (result.a == kNoData)") && Has(hlsli, "g_output[texel] = result;") && Has(hlsli, "g_aux[texel] = aux;"),
            "a texel with no data is not written (the player's dispatch's stands), the others are, with their aux");
      Check(Has(hlsli, "aux = float4(0.0, EncodeOppInstance(primaryInstance), saturate(nv), 1.0);") && Has(hlsli, "aux.x = EncodeHitDistance(distanceSum / max(hitCount, 1.0), reach);"),
            "the aux image's texel is the player's kind (hit distance, instance code, N.V, 1) with the opponents' instance code");
      Check(Has(hlsli, "const uint maxRays = min(g_push.opp.w, LevelRays(g_push.slots.w));") && Has(hlsli, "const float tanHalf = maxRays >= 2u ? g_push.tune.w : 0.0;") && Has(hlsli, "if (slot >= int(maxRays))"),
            "1 ray is the mirror (no cone), 2 the outer pair, at most the level's rays");
      Check(Has(refl, "#ifdef CAR_RT_REFL_OPPONENTS") && Has(refl, "#define main CarRtReflPlayerMain") && Has(refl, "#undef main") && Has(refl, "#include \"car_rt_refl_opp.hlsli\"") &&
                Has(refl, "uint4 opp;"),
            "car_rt_refl_cs.hlsl: the opponents' variant renames the player's main, takes its own, and has the push's opp (the shared lines)");
      Check(Has(refl, "TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.z, primary)") && Has(refl, "TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.y, ray)"),
            "the trace functions the opponents' entry reuses are as it expects (the push's masks)");
      Check(Has(host_h, "static_assert(sizeof(Push) == 112);") && Has(host_h, "float coat[4];") && Has(host_h, "uint32_t opp[4];") && Has(host, "push.slots[2] = opp::kPrimaryParts;") && Has(host, "push.opp[2] = view.player_row;") &&
                Has(host, "push.opp[3] = city_refl::OppRays(plan.rays, f.city->on ? cell : city_refl::Cell{});") && Has(host, "push.opp[0] = uint32_t(r.x0);") && Has(host, "std::memcpy(&push, f.base_push, kBasePushBytes);"),
            "the host's push block is the shader's (the player's 96 bytes and opp), the primary mask is the header's");
      Check(Has(host, "HostPrecache::Key(HostPrecache::kCarRtRefl, 2)") && Has(player, "HostPrecache::Key(HostPrecache::kCarRtRefl, 0)") && Has(player, "HostPrecache::Key(HostPrecache::kCarRtRefl, 1)"),
            "the pipeline keys of the three passes differ (0 trace, 1 denoise, 2 opponents)");
      Check(Has(player, "opp_.Initialize(") && Has(player, "opp_.Record(opponents)") && Has(player, "opp_.Shutdown()") && Has(player, "opp_.CensusText()") &&
                Has(player, "opponents.base_push_bytes = sizeof(push);"),
            "CarRtRefl calls the opponents' pass (initialize, record after the player's dispatch, shutdown, census)");
      Check(Has(cmake, "larecomp_gpu_host_shader_rt(car_rt_refl_opp_cs cs_6_5 SOURCE car_rt_refl_cs DEFINES CAR_RT_REFL_OPPONENTS=1)") && Has(cmake, "car_rt_opp_refl.cpp") &&
                Has(cmake, "larecomp_car_rt_opp_refl_rules_test"),
            "the build has the variant, the source and the test");
      Check(Has(shaders, "car_rt_refl_opp_cs.hlsl.dxil.h") && Has(shaders, "car_rt_refl_opp_cs.hlsl.spirv.h") && Has(shaders, "MCLA_GPU_HOST_SHADER_BLOB(car_rt_refl_opp_cs)"),
            "host_shaders.h includes the variant's two blobs and names it");
      Check(Has(scene, "view_.other_paint_instances = cars.other_paint_instances;") && Has(scene, "view_.player_row = cars.player_row;") && Has(inst, "car_rt_opp_refl::AppendPlayerRecord(&list_, paints);"),
            "the scene hands the view the paint count and the player's row; the list makes the record");
      // The switch's definition is a REX_SETTING row of src/settings/generated (the REXCVAR_DEFINE of car_rt_opp_refl.cpp before the conversion).
      const setting_source::Info opp_switch = setting_source::Find(src + "/gpu", host, "mcla_gpu_car_rt_opp_refl");
      Check(setting_source::DefaultIsTrue(opp_switch) && opp_switch.debug_only, "the switch is a debug-only bool, on by default");
    }
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

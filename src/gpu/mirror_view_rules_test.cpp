// Check of the shared rear view's rules (mirror_view_rules.h): the reflection, the lookup into the view, the parallax error, what the view has to cover, its size and memory, the redirect of a
// viewport and the HUD's crop. A program of its own: it needs no game and no ReXGlue, and exits 0 when every check holds. `--table` also prints the tables perf/MIRROR-VIEW.md quotes.
//
//   cmake --build <build dir> --target larecomp_mirror_view_rules_test      (src/gpu/CMakeLists.txt, next to larecomp_mirror_draws_test)
//
// The compile by hand: clang++ -std=c++23 mirror_view_rules_test.cpp -o mirror_view_rules_test.exe

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "mirror_view_rules.h"

using namespace mcla::gpu::mirror_view;
namespace mirror_draws = mcla::gpu::mirror_draws;

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

bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

constexpr double kPi = 3.14159265358979323846;
double Deg(double rad) { return rad * 180.0 / kPi; }
double Rad(double deg) { return deg * kPi / 180.0; }

// The ASSUMED geometry of a cockpit camera in the R34 (perf/MIRROR-VIEW.md section 5.2; car coordinates x forward, y left, z up, metres): the eye of a left hand drive car's driver, the two
// mirror glasses 1.8 m apart (the dump's pair: MIRROR-RT's separation window is 1.2 to 2.9 m) a little ahead of the eye and a little below it, vertical, facing back and towards the
// driver at the dump's toe-in (normals (0.91, 0.41) and (0.93, 0.37) in the horizontal plane). CHECK: the cockpit camera's real eye point and the glass's real height.
struct Glass {
  V3 centre;
  V3 normal;
  double width = 0.165;
  double height = 0.12;
};
const V3 kEye{0.0, 0.35, 0.0};
Glass LeftMirror() { return Glass{{0.55, 0.90, -0.15}, Normalize(V3{-0.91, -0.41, 0.0})}; }
Glass RightMirror() { return Glass{{0.55, -0.90, -0.15}, Normalize(V3{-0.93, 0.37, 0.0})}; }

Rig MakeRig(V3 eye, double hfov_deg, double vfov_deg) {
  Rig r;
  r.eye = eye;
  r.tan_half_h = std::tan(Rad(hfov_deg) * 0.5);
  r.tan_half_v = std::tan(Rad(vfov_deg) * 0.5);
  return r;
}

// The directions the lookup uses for a glass's four corners and its centre (what UvForMirrorPixel projects, before the projection).
int LookupDirections(const Rig& rig, V3 eye, const Glass& g, double distance, V3 out[5]) {
  const V3 tangent = Normalize(V3{-g.normal.y, g.normal.x, 0.0});
  const V3 up{0.0, 0.0, 1.0};
  const V3 pts[5] = {g.centre,
                     g.centre + tangent * (0.5 * g.width) + up * (0.5 * g.height),
                     g.centre - tangent * (0.5 * g.width) + up * (0.5 * g.height),
                     g.centre + tangent * (0.5 * g.width) - up * (0.5 * g.height),
                     g.centre - tangent * (0.5 * g.width) - up * (0.5 * g.height)};
  for (int i = 0; i < 5; ++i) {
    const V3 d = Normalize(pts[i] - eye);
    const V3 r = Reflect(d, g.normal);
    out[i] = (pts[i] - rig.eye) + r * distance;
  }
  return 5;
}

void TestVectors() {
  // a ray straight into a wall that faces it comes straight back
  const V3 n{0.0, 0.0, 1.0};
  const V3 back = Reflect(V3{0.0, 0.0, -1.0}, n);
  Check(Near(back.x, 0.0) && Near(back.y, 0.0) && Near(back.z, 1.0), "a ray into a facing wall comes straight back");
  // a glancing ray keeps its along-the-wall part and flips the other
  const V3 slant = Reflect(Normalize(V3{1.0, 0.0, -1.0}), n);
  Check(slant.x > 0.0 && slant.z > 0.0 && Near(slant.x, slant.z), "a 45 degree ray leaves at 45 degrees");
  // the mirrored eye: as far behind the glass as the eye is in front of it
  const V3 e = MirroredEye(V3{0.0, 0.0, 2.0}, V3{5.0, 5.0, 0.0}, n);
  Check(Near(e.x, 0.0) && Near(e.y, 0.0) && Near(e.z, -2.0), "the mirrored eye is as far behind the glass as the eye is in front");
  Check(Near(Length(Normalize(V3{3.0, 4.0, 0.0})), 1.0), "Normalize makes a unit vector");
  Check(Length(Normalize(V3{})) == 0.0, "Normalize of nothing is nothing, not a NaN");
}

void TestRig() {
  const Rig rig = MakeRig(V3{}, 90.0, 45.0);
  // the rig's right is forward x up: the car's left, for a camera that looks backwards
  const V3 cross{rig.forward.y * rig.up.z - rig.forward.z * rig.up.y, rig.forward.z * rig.up.x - rig.forward.x * rig.up.z, rig.forward.x * rig.up.y - rig.forward.y * rig.up.x};
  Check(Near(cross.x, rig.right.x) && Near(cross.y, rig.right.y) && Near(cross.z, rig.right.z), "the rig's right is forward x up");
  const Uv centre = ProjectDirection(rig, V3{-1.0, 0.0, 0.0});
  Check(centre.ok && Near(centre.u, 0.5) && Near(centre.v, 0.5), "straight back is the middle of the picture");
  const Uv edge = ProjectDirection(rig, Normalize(V3{-1.0, 1.0, 0.0}));  // 45 degrees to the car's left: the right edge of a 90 degree view
  Check(edge.ok && Near(edge.u, 1.0, 1e-9) && Near(edge.v, 0.5), "the car's left at half the field is the picture's right edge");
  const Uv down = ProjectDirection(rig, Normalize(V3{-1.0, 0.0, -std::tan(Rad(22.5))}));
  Check(down.ok && Near(down.v, 1.0, 1e-9), "below the horizon at half the vertical field is the bottom edge");
  Check(!ProjectDirection(rig, V3{1.0, 0.0, 0.0}).ok, "straight ahead is behind the rear camera");
  Check(!ProjectDirection(rig, V3{0.0, 1.0, 0.0}).ok, "straight out to the side is not in the picture");
  Check(Inside(centre) && !Inside(ProjectDirection(rig, Normalize(V3{-1.0, 2.0, 0.0}))), "Inside tells the picture's bounds");
}

void TestLookup() {
  const Glass left = LeftMirror();
  const Glass right = RightMirror();
  const Rig rig = MakeRig(kEye, 70.0, 40.0);
  // the left mirror shows what is behind and a little to the left, the right one what is behind and to the right: the lookups are inside a 70 degree view, in that order across it
  const Uv ul = UvForMirrorPixel(rig, kEye, left.centre, left.normal);
  const Uv ur = UvForMirrorPixel(rig, kEye, right.centre, right.normal);
  Check(Inside(ul) && Inside(ur), "both mirrors' middles look up inside a 70 by 40 degree view");
  Check(ul.u > ur.u, "the left mirror reads the part of the picture that is the car's left (the rig's right)");
  // for a thing exactly at the assumed distance the lookup is exact
  Check(ParallaxError(rig, kEye, left.centre, left.normal, kAssumedDistance) < 1e-6, "no error at the assumed distance (to the arccosine's own precision)");
  // a rig AT the glass has no parallax at all, whatever the distance
  const Rig at_glass = MakeRig(left.centre, 70.0, 40.0);
  Check(ParallaxError(at_glass, kEye, left.centre, left.normal, 3.0) < 1e-6, "a rig at the glass sees the reflected ray's direction exactly");
  // the error is larger for a near thing than a far one, and no larger than the angle the eye offset subtends
  const double e3 = ParallaxError(rig, kEye, left.centre, left.normal, 3.0);
  const double e10 = ParallaxError(rig, kEye, left.centre, left.normal, 10.0);
  const double e200 = ParallaxError(rig, kEye, left.centre, left.normal, 200.0);
  Check(e3 > e10, "a car 3 m behind the glass is further off than one at 10 m");
  Check(e200 < Rad(4.0) && e3 < Rad(15.0), "the error stays in single digits of degrees (far) and a dozen (a car alongside)");
  // a held view (the car 2 m further on when the glass is drawn) is a parallax of its own, and a bigger one than a fresh view
  const Rig held = MakeRig(kEye + V3{-2.0, 0.0, 0.0}, 70.0, 40.0);
  Check(ParallaxError(held, kEye, left.centre, left.normal, 10.0) > e10 * 0.5, "a held view adds to the error for near things");
  // the lookup of a point behind the rear camera is not ok (a mirror turned away)
  const Uv bad = UvForMirrorPixel(rig, kEye, left.centre, Normalize(V3{1.0, 0.0, 0.0}));
  Check(!bad.ok || !Inside(bad), "a glass turned the other way does not read the picture");
}

void TestExtent() {
  const Glass left = LeftMirror();
  const Glass right = RightMirror();
  const Rig rig = MakeRig(kEye, 70.0, 40.0);
  V3 dl[5], dr[5], all[10];
  LookupDirections(rig, kEye, left, kAssumedDistance, dl);
  LookupDirections(rig, kEye, right, kAssumedDistance, dr);
  for (int i = 0; i < 5; ++i) {
    all[i] = dl[i];
    all[5 + i] = dr[i];
  }
  const Extent one = ExtentOf(rig, dl, 5);
  const Extent both = ExtentOf(rig, all, 10);
  Check(one.ok && both.ok, "the extents are made");
  Check(both.half_h >= one.half_h && both.half_v >= one.half_v, "two mirrors need at least what one needs");
  Check(Deg(both.half_h) < 45.0 && Deg(both.half_v) < 25.0, "both mirrors fit a view of 90 by 50 degrees");
  V3 ahead[1] = {V3{1.0, 0.0, 0.0}};
  Check(!ExtentOf(rig, ahead, 1).ok, "a direction that points ahead of the rear camera is no extent");
}

void TestPlan() {
  Check(!PlanView(Class::kHigh, 0.0, 0.0, 0.0).valid(), "no consumer, no view");
  const Plan side = PlanView(Class::kMid, 40.0, 14.0, 0.0);
  Check(side.valid() && Near(side.hfov_deg, 50.0) && Near(side.vfov_deg, 24.0), "side mirrors only: their field plus 5 degrees each side");
  Check(side.width % 16 == 0 && side.height % 16 == 0 && side.width <= MaxWidth(Class::kMid), "the size is a multiple of 16 and within the class's cap");
  const double aspect_view = double(side.width) / double(side.height);
  const double aspect_fov = std::tan(Rad(side.hfov_deg) * 0.5) / std::tan(Rad(side.vfov_deg) * 0.5);
  Check(std::fabs(aspect_view - aspect_fov) / aspect_fov < 0.1, "the pixels are square to within the rounding");
  const Plan hud = PlanView(Class::kHigh, 0.0, 0.0, 100.0);
  Check(hud.valid() && Near(hud.hfov_deg, 100.0) && Near(hud.vfov_deg, StampVerticalFov(100.0, 229.0 / 44.0)), "the HUD mirror alone: its field across, the stamp's own field up");
  Check(hud.vfov_deg > 25.0 && hud.vfov_deg < 27.0, "a 100 degree stamp of 229 by 44 is about 26 degrees tall");
  Check(hud.width == MaxWidth(Class::kHigh), "a wide field on a strong card is capped, not larger than the class's width");
  const Plan both = PlanView(Class::kHigh, 40.0, 14.0, 100.0);
  Check(both.hfov_deg >= hud.hfov_deg && both.hfov_deg >= side.hfov_deg, "with both on, the view holds the larger field");
  Check(PlanView(Class::kLow, 40.0, 14.0, 0.0).every == 2 && PlanView(Class::kHigh, 40.0, 14.0, 0.0).every == 1, "a weak class draws every second frame, a strong one every frame");
  Check(PlanView(Class::kLow, 40.0, 14.0, 0.0).width < PlanView(Class::kHigh, 40.0, 14.0, 0.0).width, "a weaker class gets a smaller picture");
  const Plan wide = PlanView(Class::kHigh, 300.0, 0.0, 0.0);
  Check(Near(wide.hfov_deg, kMaxHorizontalFov), "the field never goes over the limit");
  // memory: 2 host samples of colour and depth, plus the resolved colour
  const Plan p = PlanView(Class::kMid, 40.0, 14.0, 0.0);
  const uint64_t px = uint64_t(p.width) * p.height;
  Check(HeldBytes(p, 2, 8, 4, false) == px * 2 * 12 + px * 8, "memory at 2 samples: colour and depth with the samples, the resolved colour");
  Check(HeldBytes(p, 1, 8, 4, false) == px * 12, "memory at 1 sample: no resolve copy");
  Check(HeldBytes(p, 1, 8, 4, true) == px * 12 + px * 4, "memory with a resolved depth");
  Check(HeldBytes(Plan{}, 2, 8, 4, true) == 0, "no view, no memory");
  Check(HeldBytes(p, 2, 8, 4, false) < 8ull * 1024 * 1024, "the mid class's view is under 8 MiB");
}

void TestRemap() {
  const Rect mirror{916.0, 62.0, 229.0, 44.0};
  const Rect whole = RemapToView(mirror, mirror, 512, 96);
  Check(Near(whole.x, 0.0) && Near(whole.y, 0.0) && Near(whole.w, 512.0) && Near(whole.h, 96.0), "the mirror's rectangle is the whole view");
  const Rect half = RemapToView(mirror, Rect{916.0, 62.0, 114.5, 22.0}, 512, 96);
  Check(Near(half.w, 256.0) && Near(half.h, 48.0), "a viewport of half the rectangle is half the view");
  const Rect inner = RemapToView(mirror, Rect{1030.5, 84.0, 114.5, 22.0}, 512, 96);
  Check(Near(inner.x, 256.0) && Near(inner.y, 48.0), "a viewport in the lower right keeps its place");
  const Rect over = RemapToView(mirror, Rect{800.0, 0.0, 600.0, 200.0}, 512, 96);
  Check(Near(over.x, 0.0) && Near(over.y, 0.0) && Near(over.w, 512.0) && Near(over.h, 96.0), "a viewport that reaches past the rectangle is clamped to the view");
  const Rect off = RemapToView(mirror, Rect{0.0, 0.0, 100.0, 40.0}, 512, 96);
  Check(off.w == 0.0 || off.h == 0.0, "a viewport wholly outside the rectangle has no area");
  Check(RemapToView(Rect{}, mirror, 512, 96).w == 0.0, "no rectangle, nothing");
  Check(RemapToView(mirror, mirror, 0, 96).w == 0.0, "no view, nothing");
}

void TestCrop() {
  const Plan view = PlanView(Class::kHigh, 0.0, 0.0, 100.0);
  const Crop full = HudCrop(view, 100.0, 229.0 / 44.0);
  Check(Near(full.u0, 0.0) && Near(full.u1, 1.0), "the HUD wants the view's whole width: no crop across");
  Check(Near(full.v0, 0.0, 1e-9) && Near(full.v1, 1.0, 1e-9), "a view made for the stamp alone is shown whole");
  const Crop taller = HudCrop(PlanView(Class::kHigh, 59.0, 34.0, 100.0), 100.0, 229.0 / 44.0);
  Check(taller.v1 - taller.v0 < 1.0 && Near(0.5 * (taller.v0 + taller.v1), 0.5), "a view that side mirrors made taller than the stamp is cropped in height, centred");
  const Crop narrow = HudCrop(PlanView(Class::kHigh, 40.0, 14.0, 100.0), 60.0, 229.0 / 44.0);
  Check(narrow.u1 - narrow.u0 < 1.0 && Near(0.5 * (narrow.u0 + narrow.u1), 0.5), "a narrower wish is a centred crop");
  const Crop wider = HudCrop(PlanView(Class::kHigh, 40.0, 14.0, 0.0), 150.0, 229.0 / 44.0);
  Check(Near(wider.u0, 0.0) && Near(wider.u1, 1.0), "a wish wider than the view shows all of it");
  const Crop none = HudCrop(Plan{}, 100.0, 5.0);
  Check(Near(none.u0, 0.0) && Near(none.u1, 1.0) && Near(none.v0, 0.0) && Near(none.v1, 1.0), "no view: the whole of nothing");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// JOIN2: the side mirrors' wider field, the plan that follows it, the HUD's crop of it, and the request that carries it (perf/MIRROR-VIEW.md section 14)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr double kRetailHfov = 100.0;  // the design's guess of the HUD mirror's retail field across (CHECK 11): 100 across the 229x44 stamp is 26 up
constexpr double kStampAspect = 229.0 / 44.0;

void TestSideMirrorsNumbers() {
  // The two constants are what the assumed geometry needs (both glasses' lookups at the assumed distance, centred on straight back), rounded UP to a whole degree: a change of the geometry or of
  // the constants fails here.
  const Glass left = LeftMirror(), right = RightMirror();
  const Rig rig = MakeRig(kEye, 70.0, 40.0);
  V3 dl[5], dr[5], all[10];
  LookupDirections(rig, kEye, left, kAssumedDistance, dl);
  LookupDirections(rig, kEye, right, kAssumedDistance, dr);
  for (int i = 0; i < 5; ++i) {
    all[i] = dl[i];
    all[5 + i] = dr[i];
  }
  const Extent both = ExtentOf(rig, all, 10);
  const Extent el = ExtentOf(rig, dl, 5);
  const double need_h = 2.0 * Deg(both.half_h), need_v = 2.0 * Deg(both.half_v);
  Check(kSideMirrorsHfovDeg >= need_h && kSideMirrorsHfovDeg < need_h + 1.0, "the side mirrors' field across is the extent both glasses need, rounded up to a degree (" + std::to_string(need_h) + ")");
  Check(kSideMirrorsVfovDeg >= need_v && kSideMirrorsVfovDeg < need_v + 1.0, "... and up (" + std::to_string(need_v) + ")");
  Check(Near(kSideMirrorsHfovDeg, 59.0) && Near(kSideMirrorsVfovDeg, 34.0), "the numbers the design and the report quote: 59 x 34 degrees");
  const Plan side = PlanView(Class::kMid, kSideMirrorsHfovDeg, kSideMirrorsVfovDeg, 0.0);
  Check(Near(side.hfov_deg, 69.0) && Near(side.vfov_deg, 44.0), "with the margin of 5 degrees a side: 69 x 44 degrees");
  // what the game's own HUD field cannot do: 26 degrees up reaches 13 below straight back, the left glass's lookups go to 16.9 below
  const double retail_v = StampVerticalFov(kRetailHfov, kStampAspect);
  Check(0.5 * retail_v < 13.5 && 0.5 * retail_v < Deg(el.half_v) - 3.0, "the retail HUD field reaches only about 13 degrees below straight back: short of the left glass's " + std::to_string(Deg(el.half_v)));
  Check(0.5 * side.vfov_deg >= Deg(el.half_v) + 5.0 - 1e-9 && 0.5 * side.hfov_deg >= Deg(both.half_h) + 5.0 - 1e-9, "the side mirrors' field holds both glasses with the margin");
  // every lookup of both glasses lands inside the picture of the side mirrors' field, centred on straight back
  const Rig wide = MakeRig(kEye, side.hfov_deg, side.vfov_deg);
  bool inside = true;
  for (const Glass& g : {left, right}) {
    V3 d[5];
    LookupDirections(wide, kEye, g, kAssumedDistance, d);
    for (const V3& v : d) inside = inside && Inside(ProjectDirection(wide, v));
  }
  Check(inside, "the lookups of both glasses' corners and middles land inside the 69 x 44 picture");
  bool retail_inside = true;
  const Rig retail = MakeRig(kEye, kRetailHfov, retail_v);
  for (const Glass& g : {left, right}) {
    V3 d[5];
    LookupDirections(retail, kEye, g, kAssumedDistance, d);
    for (const V3& v : d) retail_inside = retail_inside && Inside(ProjectDirection(retail, v));
  }
  Check(!retail_inside, "... while some of them fall outside the retail HUD field");
}

void TestPlanForField() {
  struct F {
    double h, v;
  };
  for (Class c : {Class::kLow, Class::kMid, Class::kHigh}) {
    for (const F f : {F{69.0, 44.0}, F{100.0, 44.0}, F{100.0, 26.0}, F{45.0, 25.0}, F{110.0, 45.0}}) {
      const Plan p = PlanForField(c, f.h, f.v);
      const std::string tag = " [class " + std::to_string(int(c)) + " " + std::to_string(f.h) + "x" + std::to_string(f.v) + "]";
      Check(p.valid() && Near(p.hfov_deg, f.h) && Near(p.vfov_deg, f.v), "the plan keeps the field it was made for" + tag);
      Check(p.width % 16 == 0 && p.width <= MaxWidth(c) && p.height % 2 == 0 && p.height >= 2, "a width of whole 16s within the class's cap, an even height" + tag);
      const double exact = double(p.width) * std::tan(Rad(f.v) * 0.5) / std::tan(Rad(f.h) * 0.5);
      Check(std::fabs(double(p.height) - exact) <= 1.0, "the height follows the field's own aspect (the camera's): within a pixel" + tag);
      Check(p.every == Cadence(c), "the class's cadence" + tag);
      Check(double(p.width) <= PixelsPerDegree(c) * f.h + 16.0, "no more pixels per degree than the class's (the cap aside)" + tag);
    }
  }
  // the numbers the report quotes
  const Plan mid = PlanForField(Class::kMid, 69.0, 44.0), high = PlanForField(Class::kHigh, 100.0, 44.0), low = PlanForField(Class::kLow, 69.0, 44.0);
  Check(mid.width == 496 && mid.height == 292, "Mid, 69 x 44: 496 x 292 (" + std::to_string(mid.width) + "x" + std::to_string(mid.height) + ")");
  Check(low.width == 352 && low.height == 206, "Low, 69 x 44: 352 x 206 (" + std::to_string(low.width) + "x" + std::to_string(low.height) + ")");
  Check(high.width == 768 && high.height == 260, "High, 100 x 44: 768 x 260 (" + std::to_string(high.width) + "x" + std::to_string(high.height) + ")");
  // the size of PlanView (the design's tables) and this one differ only by the rounding of the height: the same width, and a height within 16
  for (Class c : {Class::kLow, Class::kMid, Class::kHigh}) {
    const Plan a = PlanView(c, kSideMirrorsHfovDeg, kSideMirrorsVfovDeg, 0.0), b = PlanForField(c, a.hfov_deg, a.vfov_deg);
    Check(a.width == b.width && std::abs(int(a.height) - int(b.height)) <= 16, "the same width as the design's table, a height within 16 of it");
  }
  Check(!PlanForField(Class::kMid, 0.0, 44.0).valid() && !PlanForField(Class::kMid, 69.0, 0.0).valid() && !PlanForField(Class::kMid, -5.0, 44.0).valid(), "no field, no view");
  Check(!PlanForField(Class::kMid, 180.0, 44.0).valid() && !PlanForField(Class::kMid, 69.0, 200.0).valid() && !PlanForField(Class::kMid, std::nan(""), 44.0).valid(), "a field of 180 or more, or NaN: no view");
}

void TestSideMirrorsField() {
  for (Class c : {Class::kLow, Class::kMid, Class::kHigh}) {
    const Field alone = SideMirrorsField(c, false, 0.0, 0.0);
    Check(alone.asked() && Near(alone.hfov_deg, 69.0) && Near(alone.vfov_deg, 44.0), "the side mirrors alone: 69 x 44 degrees, whatever the retail field and the rectangle are");
    const Field alone2 = SideMirrorsField(c, false, 120.0, 2.0);
    Check(alone2.asked() && Near(alone2.hfov_deg, 69.0) && Near(alone2.vfov_deg, 44.0), "... even where a stamp could not be cut out (no stamp is drawn)");
    const Field both = SideMirrorsField(c, true, kRetailHfov, kStampAspect);
    Check(both.asked() && Near(both.hfov_deg, 100.0) && Near(both.vfov_deg, 44.0), "with the HUD stamp (retail 100 across, 26 up): 100 x 44, the larger of the two in each direction");
    const Field narrow = SideMirrorsField(c, true, 60.0, kStampAspect);
    Check(narrow.asked() && Near(narrow.hfov_deg, 69.0) && Near(narrow.vfov_deg, 44.0), "a narrower retail HUD field: the side mirrors' own field holds it");
    const Field top = SideMirrorsField(c, true, 110.0, kStampAspect);
    Check(top.asked() && Near(top.hfov_deg, 110.0), "a retail field at the view's limit (110) is held");
    Check(!SideMirrorsField(c, true, 0.0, kStampAspect).asked(), "no camera built yet (retail field 0): nothing asked, the game's own field serves both until it reports");
    Check(!SideMirrorsField(c, true, kRetailHfov, 0.0).asked(), "no rectangle: nothing asked");
    Check(!SideMirrorsField(c, true, 110.5, kStampAspect).asked() && !SideMirrorsField(c, true, 140.0, kStampAspect).asked(), "a retail field wider than the view can be is never cut down: nothing asked");
    Check(!SideMirrorsField(c, true, kRetailHfov, 2.0).asked(), "a retail picture taller than the view can be (100 across a 2:1 stamp is 62 up): nothing asked");
  }
  // whenever a field is asked, the view holds the HUD's whole retail frustum: the crop is the HUD's picture exactly
  bool held = true, exact = true;
  int asked = 0;
  uint32_t seed = 7u;
  auto rnd = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return double((seed >> 8) & 0xFFFFu) / 65535.0;
  };
  for (Class c : {Class::kLow, Class::kMid, Class::kHigh}) {
    for (double retail = 30.0; retail <= 112.0; retail += 2.5) {
      for (double aspect : {5.2045, 4.0, 3.5, 6.5}) {
        const Field f = SideMirrorsField(c, true, retail, aspect);
        if (!f.asked()) continue;
        ++asked;
        const Plan view = PlanForField(c, f.hfov_deg, f.vfov_deg);
        const Crop crop = HudCrop(view, retail, aspect);
        const double tan_hud_h = std::tan(Rad(retail) * 0.5), tan_hud_v = tan_hud_h / aspect;
        held = held && std::tan(Rad(f.hfov_deg) * 0.5) >= tan_hud_h - 1e-12 && std::tan(Rad(f.vfov_deg) * 0.5) >= tan_hud_v - 1e-12;
        const Rig view_rig = MakeRig(kEye, f.hfov_deg, f.vfov_deg);
        const Rig hud_rig = MakeRig(kEye, retail, StampVerticalFov(retail, aspect));
        for (int i = 0; i < 40; ++i) {
          const double tx = 2.0 * rnd() - 1.0, ty = 2.0 * rnd() - 1.0;
          const V3 dir = Normalize(V3{-1.0, tx * tan_hud_h, ty * tan_hud_v});
          const Uv a = ProjectDirection(view_rig, dir), b = ProjectDirection(hud_rig, dir);
          const double lu = (a.u - crop.u0) / (crop.u1 - crop.u0), lv = (a.v - crop.v0) / (crop.v1 - crop.v0);
          exact = exact && a.ok && b.ok && Near(lu, b.u, 1e-9) && Near(lv, b.v, 1e-9);
        }
      }
    }
  }
  Check(asked > 100, "the sweep asked for a field in many cases (" + std::to_string(asked) + ")");
  Check(held, "whenever a field is requested for the HUD stamp too, the view's frustum holds the HUD's retail frustum whole");
  Check(exact, "and the stamp's crop of it lands every direction of the retail frustum on the same picture coordinate the game's own camera gave (the HUD mirror looks exactly as before)");
}

void TestRequestField() {
  const auto facts = [](bool need, bool rt, bool hud, double retail, double aspect, Class c) {
    RequestFacts f;
    f.enabled = true;
    f.side_need = need;
    f.rt_side_mirrors = rt;
    f.hud_stamp = hud;
    f.retail_hfov_deg = retail;
    f.stamp_aspect = aspect;
    f.cls = c;
    return f;
  };
  // side mirrors only: the wider field and the need, redirect on
  const Request alone = RequestFor(facts(true, false, false, kRetailHfov, kStampAspect, Class::kMid));
  Check(alone.redirect && alone.side_need && alone.hfov_deg == 69.0f && alone.vfov_deg == 44.0f && alone.every == 1, "side mirrors only: redirect, need, 69 x 44, no cadence");
  const Request both = RequestFor(facts(true, false, true, kRetailHfov, kStampAspect, Class::kMid));
  Check(both.redirect && both.side_need && both.hfov_deg == 100.0f && both.vfov_deg == 44.0f, "side mirrors and the HUD stamp: 100 x 44");
  const Request unknown = RequestFor(facts(true, false, true, 0.0, kStampAspect, Class::kMid));
  Check(unknown.redirect && unknown.side_need && !unknown.has_field(), "the retail field not reported yet: the need, no field");
  const Request hud_only = RequestFor(facts(false, false, true, kRetailHfov, kStampAspect, Class::kMid));
  Check(hud_only.redirect && !hud_only.side_need && !hud_only.has_field(), "no need (the HUD mirror alone): no field, the game's own, the HUD mirror as it always was");
  // the ray traced side mirrors win: no need, no field, no side-only phase nobody reads; the HUD mirror's own phase is untouched
  const Request rt_alone = RequestFor(facts(true, true, false, kRetailHfov, kStampAspect, Class::kHigh));
  Check(rt_alone.redirect && !rt_alone.side_need && !rt_alone.has_field() && rt_alone.every == 1, "RT side mirrors win, HUD stamp off: the redirect stays, the need and the field do not (the guest asks for no phase)");
  const Request rt_hud = RequestFor(facts(true, true, true, kRetailHfov, kStampAspect, Class::kHigh));
  Check(rt_hud.redirect && !rt_hud.side_need && !rt_hud.has_field(), "RT side mirrors win, HUD stamp on: the HUD mirror from the view at the game's own field, as without a finder");
  Check(RequestFor(facts(true, false, false, kRetailHfov, kStampAspect, Class::kMid)) != RequestFor(facts(true, true, false, kRetailHfov, kStampAspect, Class::kMid)), "the RT flag changes the request");
  // exhaustive over the inputs: what a request may say
  int fields = 0;
  for (int bits = 0; bits < 128; ++bits) {
    RequestFacts f;
    f.enabled = (bits & 1) != 0;
    f.blocked = (bits & 2) != 0;
    f.side_need = (bits & 4) != 0;
    f.rt_side_mirrors = (bits & 8) != 0;
    f.hud_stamp = (bits & 16) != 0;
    f.retail_hfov_deg = (bits & 32) != 0 ? kRetailHfov : 0.0;
    f.stamp_aspect = (bits & 64) != 0 ? kStampAspect : 0.0;
    for (Class c : {Class::kLow, Class::kMid, Class::kHigh}) {
      f.cls = c;
      const Request r = RequestFor(f);
      const std::string tag = " [bits " + std::to_string(bits) + " class " + std::to_string(int(c)) + "]";
      Check(r.redirect == (f.enabled && !f.blocked), "redirect: the switch on and not blocked" + tag);
      Check(r.side_need == (r.redirect && f.side_need && !f.rt_side_mirrors), "need: a redirect, the finder's flag, and the RT mirrors not winning" + tag);
      Check(!r.has_field() || r.side_need, "a field is asked for only together with the need" + tag);
      Check(r.every == 1, "no cadence" + tag);
      if (r.has_field()) {
        ++fields;
        Check(r.hfov_deg >= kMinHorizontalFov && r.hfov_deg <= kMaxHorizontalFov && r.vfov_deg >= kMinVerticalFov && r.vfov_deg <= kMaxVerticalFov, "the field is within the view's limits" + tag);
        const Request back = UnpackRequest(Pack(r));
        Check(back == r, "and the guest reads the same field from the link" + tag);
      }
    }
  }
  Check(fields == 15, "the sweep carried a field in exactly the 15 requests that can (side only: 4 retail / rectangle combinations x 3 classes; with the stamp: only when both are known, x 3: " + std::to_string(fields) + ")");
  // published and read as the guest reads it
  PublishRequest(alone);
  Check(CurrentRequest() == alone && CurrentRequest().has_field(), "published: the guest reads redirect, need and the field");
  PublishRequest(RequestFor(facts(true, true, false, kRetailHfov, kStampAspect, Class::kMid)));
  Check(!CurrentRequest().side_need && !CurrentRequest().has_field(), "the next frame with the RT mirrors on: need and field are gone at once");
  WithdrawRequest();
  Check(CurrentRequest() == Request{}, "withdrawn");
}

// MV-J2: the view's size follows the field the camera of THIS phase was built with (the begin marker's tag), nothing else.
void TestPlanForPhase() {
  const PhaseField none;
  PhaseField req;
  req.hfov_deg = 69.0f;
  req.vfov_deg = 44.0f;
  req.requested = true;
  PhaseField own;  // the game's own camera: its field, not a request
  own.hfov_deg = 100.0f;
  own.vfov_deg = 25.8f;
  own.requested = false;
  Check(req.known() && own.known() && !none.known(), "a tag with both halves is a known field");
  // the plan: the rectangle's where the camera was the game's own (the HUD mirror as it always was, bit for bit), the field's where it was the host's
  for (Class c : {Class::kLow, Class::kMid, Class::kHigh}) {
    const Plan rect_plan = PlanForRect(c, 229.0, 44.0);
    const Plan a = PlanForPhase(c, 229.0, 44.0, none);
    Check(a.width == rect_plan.width && a.height == rect_plan.height && a.hfov_deg == 0.0 && a.vfov_deg == 0.0 && a.every == rect_plan.every, "no tag (no camera yet): PlanForRect, bit for bit");
    const Plan b = PlanForPhase(c, 229.0, 44.0, own);
    Check(b.width == rect_plan.width && b.height == rect_plan.height && b.hfov_deg == 0.0 && b.vfov_deg == 0.0 && b.every == rect_plan.every,
          "the game's own camera, known but not requested: PlanForRect, bit for bit (the HUD mirror's view is unchanged)");
    const Plan d = PlanForPhase(c, 229.0, 44.0, req);
    const Plan e = PlanForField(c, 69.0, 44.0);
    Check(d.width == e.width && d.height == e.height && Near(d.hfov_deg, 69.0) && Near(d.vfov_deg, 44.0) && d.every == e.every, "the host's field applied: the field's plan, whatever the rectangle's aspect is");
    const Plan g = PlanForPhase(c, 100.0, 100.0, req);
    Check(g.width == e.width && g.height == e.height, "... the rectangle's size does not enter");
    // the picture's pixels are the camera's aspect (no stretch): the camera is built with tan(h/2) / tan(v/2) (rear_view_shared.h SharedCamera)
    const double camera_aspect = std::tan(Rad(69.0) * 0.5) / std::tan(Rad(44.0) * 0.5);
    Check(Near(double(d.width) / double(d.height), camera_aspect, camera_aspect * 0.01), "the plan's pixel aspect is the tag's camera's");
    // a requested tag with a half missing is no field: the rectangle's plan
    PhaseField broken = req;
    broken.vfov_deg = 0.0f;
    const Plan h = PlanForPhase(c, 229.0, 44.0, broken);
    Check(h.width == rect_plan.width && h.height == rect_plan.height && h.hfov_deg == 0.0, "a requested field with a half missing: the rectangle's plan");
  }
  // THE LAG (J2): the plan of a phase is a function of its own tag, so a phase that was built before a change is sized by the old field however many frames the guest has moved on; the old rule
  // (the host's request against the guest's latest camera facts) sized it by whichever they agreed on at the moment the host looked, which a guest running ahead made the rectangle's plan: a picture
  // drawn with a 69 x 44 camera into a 229 x 44 aspect.
  for (Class c : {Class::kLow, Class::kMid, Class::kHigh}) {
    PhaseField queue[3] = {own, req, own};  // phases the guest built camera 1 (game's own), 2 (requested), 3 (game's own again) for, queued before the host handled any
    const double want_aspect[3] = {229.0 / 44.0, std::tan(Rad(69.0) * 0.5) / std::tan(Rad(44.0) * 0.5), 229.0 / 44.0};
    for (int i = 0; i < 3; ++i) {
      const Plan p = PlanForPhase(c, 229.0, 44.0, queue[i]);
      const double got = double(p.width) / double(p.height);
      Check(Near(got, want_aspect[i], want_aspect[i] * 0.02), "a queued phase is sized by its own camera's aspect, whatever the guest built after it (class " + std::to_string(int(c)) + ", phase " + std::to_string(i) + ")");
    }
  }
  // the HUD's crop of a plan with no field of its own is everything (the picture is the HUD's already)
  const Crop whole = HudCrop(PlanForRect(Class::kMid, 229.0, 44.0), 100.0, kStampAspect);
  Check(Near(whole.u0, 0.0) && Near(whole.u1, 1.0) && Near(whole.v0, 0.0) && Near(whole.v1, 1.0), "the HUD mirror's own view (PlanForRect) is shown whole");
  // the crop of the side mirrors' view for the retail HUD field: the whole width, the middle 26/44 (by tangents) of the height
  const Crop both = HudCrop(PlanForField(Class::kHigh, 100.0, 44.0), 100.0, kStampAspect);
  const double hv = std::tan(Rad(StampVerticalFov(100.0, kStampAspect)) * 0.5) / std::tan(Rad(44.0) * 0.5);
  Check(Near(both.u0, 0.0, 1e-9) && Near(both.u1, 1.0, 1e-9) && Near(both.v0, 0.5 - 0.5 * hv, 1e-9) && Near(both.v1, 0.5 + 0.5 * hv, 1e-9) && hv > 0.5 && hv < 0.7, "100 x 44 view, retail HUD 100 x 26: the whole width, the middle " + std::to_string(hv) + " of the height");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// MV1: the host view's rules (the markers, the phase, the route of a draw, the view's size for the rectangle, its life, the maps into it)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

BeginFacts GoodBegin() {
  BeginFacts f;
  f.enabled = true;
  f.rect_valid = true;
  f.plan_valid = true;
  return f;
}
DrawFacts GoodDraw() {
  DrawFacts f;
  f.color_count = 1;
  f.has_depth = true;
  f.color_format = 7;
  f.depth_format = 9;
  f.samples = 2;
  return f;
}

void TestMarkers() {
  Check(kMarkerBegin == 0x7F000010u && kMarkerEnd == 0x7F000011u, "the marker values are the interface the guest's hooks use (MIRROR-VIEW.md 9)");
  Check(IsMarker(kMarkerBegin) && IsMarker(kMarkerEnd) && !IsMarker(0x7F000001u) && !IsMarker(0x10000000u) && !IsMarker(0), "only the two values are markers");
  // the guest's word on whether the HUD mirror is up (the stamp is for it): true by default, set and read back
  Check(StampWanted(), "the stamp is wanted by default (the HUD mirror is the only reason for a phase until MV2)");
  SetStampWanted(false);
  Check(!StampWanted(), "the guest can say a phase is the side mirrors' alone");
  SetStampWanted(true);
  Check(StampWanted(), "and say it is the HUD's again");
  // they sit on the side channel of the UI messages (the top byte 0x7F) and collide with none of ultrawide.h's (0x7F000001 .. 0x7F00000A)
  Check((kMarkerBegin & 0xFF000000u) == 0x7F000000u && (kMarkerEnd & 0xFF000000u) == 0x7F000000u && kMarkerBegin > 0x7F00000Au && kMarkerEnd != kMarkerBegin, "on the side channel, clear of the UI messages");
}

// The host's per frame request to the guest (RequestFor): redirect only with the switch on and the view not blocked, the side mirrors' need only with the redirect, the cadence the SIDE MIRRORS level's (MV6)
// and only with the redirect, no field until MV4 asks for one.
void TestRequest() {
  for (int bits = 0; bits < 8; ++bits) {
    for (const uint32_t every : {1u, 2u}) {
      RequestFacts f;
      f.enabled = (bits & 1) != 0;
      f.blocked = (bits & 2) != 0;
      f.side_need = (bits & 4) != 0;
      f.every = every;
      const std::string tag = " [enabled " + std::to_string(f.enabled) + " blocked " + std::to_string(f.blocked) + " need " + std::to_string(f.side_need) + " every " + std::to_string(every) + "]";
      const Request r = RequestFor(f);
      Check(r.redirect == (f.enabled && !f.blocked), "redirect only with the switch on and the view not blocked" + tag);
      Check(r.side_need == (r.redirect && f.side_need), "the need counts only with a redirect" + tag);
      Check(r.every == (r.redirect ? every : 1u) && !r.has_field(), "the cadence is the level's, only while the phase is redirected, and no field is asked for until MV4" + tag);
      // it survives the link's packing, which is what the guest reads
      const Request back = UnpackRequest(Pack(r));
      Check(back.redirect == r.redirect && back.side_need == r.side_need && back.every == r.every && !back.has_field(), "and the guest reads the same" + tag);
    }
  }
  {
    // a cadence out of the link's range is held in it (1 to 8), never 0
    RequestFacts f;
    f.enabled = true;
    f.every = 0;
    Check(RequestFor(f).every == 1, "a cadence of 0 is every frame");
    f.every = 100;
    Check(RequestFor(f).every == kMaxEvery, "a cadence over the range is held at the top");
  }
  Check(!RequestFor(RequestFacts{}).redirect && RequestFor(RequestFacts{}) == Request{}, "all off: the same as a withdrawn request");
  // the host publishes it and withdraws it; the guest's Current follows
  PublishRequest(RequestFor(RequestFacts{true, false, true}));
  Check(CurrentRequest().redirect && CurrentRequest().side_need, "published: the guest sees redirect and need");
  PublishRequest(RequestFor(RequestFacts{false, false, true}));
  Check(!CurrentRequest().redirect && !CurrentRequest().side_need, "the next frame with the switch off: redirect and need are gone at once (a request is not kept)");
  PublishRequest(RequestFor(RequestFacts{true, false, false}));
  WithdrawRequest();
  Check(CurrentRequest() == Request{}, "withdrawn at shutdown: nothing asked");
}

void TestPhase() {
  PhaseTracker t;
  Check(t.state == PhaseState::kIdle && !t.redirecting(), "no phase at the start");
  Check(t.Begin(GoodBegin()) == PhaseState::kRedirect && t.redirecting(), "a phase with the switch on, a rectangle and a plan redirects");
  Check(t.End() && t.state == PhaseState::kIdle && t.completed == 1 && t.unbalanced == 0, "its end closes it and says the picture is whole");
  // each missing condition is a pass-through: the game's own mirror
  for (int missing = 0; missing < 4; ++missing) {
    BeginFacts f = GoodBegin();
    f.enabled = missing != 0;
    f.rect_valid = missing != 1;
    f.plan_valid = missing != 2;
    f.blocked = missing == 3;
    PhaseTracker p;
    Check(p.Begin(f) == PhaseState::kPassThrough && !p.redirecting(), "without the switch, the rectangle, the plan, or with the images blocked the phase passes through (case " + std::to_string(missing) + ")");
    Check(!p.End() && p.completed == 0 && p.unbalanced == 0 && p.redirected == 0, "the end of a pass-through phase makes no picture and is balanced");
  }
  // an end outside a phase, a begin inside one, a phase left open at the frame's start: counted, never stuck
  PhaseTracker u;
  Check(!u.End() && u.unbalanced == 1 && u.state == PhaseState::kIdle, "an end with no begin is counted and changes nothing");
  u.Begin(GoodBegin());
  Check(u.Begin(GoodBegin()) == PhaseState::kRedirect && u.unbalanced == 2 && u.phases == 2, "a begin inside a phase starts a new one and counts the lost end");
  u.FrameStart();
  Check(u.state == PhaseState::kIdle && u.unbalanced == 3, "a phase open at the frame's start is closed and counted");
  u.FrameStart();
  Check(u.unbalanced == 3, "a frame start with nothing open counts nothing");
  Check(u.redirected == 2 && u.completed == 0, "the counters tell the redirected phases and the completed ones apart");
}

void TestRoute() {
  const ViewFormats none;
  Check(RouteDraw(PhaseState::kIdle, GoodDraw(), none) == Route::kMain, "outside a phase a draw is the main target's");
  Check(RouteDraw(PhaseState::kPassThrough, GoodDraw(), none) == Route::kMain, "in a pass-through phase too: the game's own mirror");
  Check(RouteDraw(PhaseState::kRedirect, GoodDraw(), none) == Route::kView, "in a redirected phase a draw of one colour and a depth goes into the view");
  DrawFacts planes = GoodDraw();
  planes.planes_attached = true;
  planes.color_count = 4;
  Check(RouteDraw(PhaseState::kRedirect, planes, none) == Route::kDrop, "a draw that carries the G-buffer's planes cannot go into a view with one colour: dropped, never leaked into the main target");
  DrawFacts planes_one = GoodDraw();
  planes_one.planes_attached = true;  // one colour in the key and still the planes: the key's colour count is not the only thing that says it
  Check(RouteDraw(PhaseState::kRedirect, planes_one, none) == Route::kDrop, "the planes' flag alone drops a draw (the Scene never attaches them in a redirected phase, and this is the rule's own guard)");
  DrawFacts two = GoodDraw();
  two.color_count = 2;
  Check(RouteDraw(PhaseState::kRedirect, two, none) == Route::kDrop, "two colour targets: dropped");
  DrawFacts no_colour = GoodDraw();
  no_colour.color_count = 0;
  Check(RouteDraw(PhaseState::kRedirect, no_colour, none) == Route::kDrop, "a depth-only draw has no colour for the view: dropped");
  DrawFacts no_depth = GoodDraw();
  no_depth.has_depth = false;
  Check(RouteDraw(PhaseState::kRedirect, no_depth, none) == Route::kDrop, "a draw with no depth cannot go into the view's framebuffer (its pipeline has none): dropped");
  // the view's formats are the first draw's
  ViewFormats view;
  view.assigned = true;
  view.color_format = 7;
  view.depth_format = 9;
  view.samples = 2;
  Check(RouteDraw(PhaseState::kRedirect, GoodDraw(), view) == Route::kView, "a later draw of the same formats and samples goes in");
  DrawFacts other = GoodDraw();
  other.samples = 4;
  Check(RouteDraw(PhaseState::kRedirect, other, view) == Route::kDrop, "another sample count: the pipeline does not fit the framebuffer: dropped");
  other = GoodDraw();
  other.color_format = 8;
  Check(RouteDraw(PhaseState::kRedirect, other, view) == Route::kDrop, "another colour format: dropped");
  other = GoodDraw();
  other.depth_format = 10;
  Check(RouteDraw(PhaseState::kRedirect, other, view) == Route::kDrop, "another depth format: dropped");
  // not assigned: whatever the first draw names becomes the view's (no drop for want of a reference)
  other = GoodDraw();
  other.samples = 8;
  Check(RouteDraw(PhaseState::kRedirect, other, none) == Route::kView, "with no formats named yet the first draw names them");
}

void TestPlanForRect() {
  const Plan mid = PlanForRect(Class::kMid, 229.0, 44.0);
  Check(mid.valid() && mid.width == 512 && mid.height == 98, "Mid: 512 wide, the rectangle's aspect (229 by 44) gives 98 rows");
  Check(PlanForRect(Class::kLow, 229.0, 44.0).width == 384 && PlanForRect(Class::kLow, 229.0, 44.0).height == 74, "Low: 384 by 74");
  Check(PlanForRect(Class::kHigh, 229.0, 44.0).width == 768 && PlanForRect(Class::kHigh, 229.0, 44.0).height == 148, "High: 768 by 148");
  for (Class c : {Class::kLow, Class::kMid, Class::kHigh}) {
    const Plan p = PlanForRect(c, 229.0, 44.0);
    const double aspect = double(p.width) / double(p.height);
    Check(std::fabs(aspect - 229.0 / 44.0) / (229.0 / 44.0) < 0.02, "the view has the rectangle's aspect to within the rounding (a stretch would show in the stamp)");
    Check(p.height % 2 == 0 && p.width <= MaxWidth(c) && p.every == Cadence(c), "an even height, within the class's width, the class's cadence");
  }
  Check(PlanForRect(Class::kMid, 120.0, 120.0).height == 512, "a square rectangle gives a square view");
  for (double h : {44.0, 45.0, 46.0, 51.0, 77.0, 101.0}) {
    Check(PlanForRect(Class::kMid, 229.0, h).height % 2 == 0, "the height is even for a rectangle 229 by " + std::to_string(int(h)));
  }
  Check(!PlanForRect(Class::kMid, 0.0, 44.0).valid() && !PlanForRect(Class::kMid, 229.0, 0.0).valid() && !PlanForRect(Class::kMid, -5.0, 10.0).valid(), "no rectangle, no view");
  Check(PlanForRect(Class::kMid, 4000.0, 1.0).height >= 2, "a hair-thin rectangle still gives a view with rows");
  // memory: the Mid view at 2 samples with an 8 byte colour and a 4 byte depth (D32 + S8 is 5 in the table of the doc; the rules take the figure the caller gives)
  Check(HeldBytes(mid, 2, 8, 4, false) == uint64_t(mid.width) * mid.height * (2 * 12 + 8), "the held bytes of the HUD-only Mid view: samples times (colour + depth), plus the resolved colour");
  Check(HeldBytes(mid, 2, 8, 4, false) < 2ull * 1024 * 1024, "the HUD-only Mid view is under 2 MiB at 2 samples");
  Check(HeldBytes(PlanForRect(Class::kHigh, 229.0, 44.0), 8, 8, 5, false) < 16ull * 1024 * 1024, "even the High view at 8 samples (a D32 + S8 depth is 5 bytes) stays under 16 MiB");
  Check(AutoWanted(Class::kMid) && AutoWanted(Class::kHigh) && !AutoWanted(Class::kLow), "auto: the view on Mid and High, the game's mirror on Low until it is measured");
}

void TestLifetime() {
  Lifetime l;
  Check(!l.ShouldDrop(0) && !l.ShouldDrop(1000000), "nothing held, nothing to drop");
  l.held = true;
  Check(l.ShouldDrop(0), "images that nobody ever asked for (a view made and never wanted) go at once");
  l.Wanted(1000);
  Check(!l.ShouldDrop(1000) && !l.ShouldDrop(1000 + kIdleDropMs), "kept for the idle time after the last wish");
  Check(l.ShouldDrop(1000 + kIdleDropMs + 1), "dropped once the idle time has passed");
  l.Wanted(1000 + kIdleDropMs);
  Check(!l.ShouldDrop(1000 + kIdleDropMs + 1), "a new wish starts the time again");
  l.held = false;
  Check(!l.ShouldDrop(1000000), "once dropped there is nothing to drop again");
  Check(kIdleDropMs >= 2000 && kIdleDropMs <= 5000, "about 3 seconds: long enough for a mirror that flickers on and off, short enough to give the memory back");
  // the stamp: a whole picture, a rectangle up, not older than the cadence holds it
  Check(StampDue(true, 0, true, true) && StampDue(true, kMaxHeldFrames, true, true), "a fresh or a held picture is stamped");
  Check(!StampDue(true, kMaxHeldFrames + 1, true, true), "a picture older than the cadence can hold is not stamped");
  Check(!StampDue(false, 0, true, true) && !StampDue(true, 0, false, true) && !StampDue(true, 0, true, false), "no picture, no rectangle, or a view the side mirrors alone asked for: no stamp");
}

void TestMaps() {
  const mirror_draws::Rect guest{916, 62, 229, 44};
  const IRect at1 = HostRectOf(guest, 20);  // 1x
  Check(at1 == IRect{916, 62, 1145, 106}, "at the guest's own scale the host rectangle is the guest's");
  const IRect at2 = HostRectOf(guest, 40);  // 2x
  Check(at2 == IRect{1832, 124, 2290, 212}, "at 2x the edges double");
  Check(HostRectOf(mirror_draws::Rect{}, 20).empty() && HostRectOf(guest, 0).empty(), "no rectangle or no scale: empty");
  // the viewport of a draw that is the rectangle is the whole view
  const Rect whole = MapViewport(at2, Rect{1832.0, 124.0, 458.0, 88.0}, 512, 98);
  Check(Near(whole.x, 0.0) && Near(whole.y, 0.0) && Near(whole.w, 512.0) && Near(whole.h, 98.0), "the draw's viewport that is the rectangle becomes the whole view");
  // a viewport that is wider than the rectangle is NOT clamped (its NDC is taken against it); it keeps its proportion
  const Rect wider = MapViewport(at1, Rect{816.0, 62.0, 429.0, 44.0}, 512, 98);
  Check(wider.x < 0.0 && wider.w > 512.0 && Near(wider.y, 0.0), "a wider viewport keeps its shape: left of the view, wider than it");
  Check(Near(wider.w, 429.0 * 512.0 / 229.0), "in proportion to the rectangle's map");
  Check(MapViewport(IRect{}, Rect{0, 0, 10, 10}, 512, 98).w == 0.0 && MapViewport(at1, Rect{0, 0, 10, 10}, 0, 98).w == 0.0, "no rectangle or no view: nothing");
  // the scissor is cut to the view and rounded outwards
  const IRect s_whole = MapScissor(at1, IRect{916, 62, 1145, 106}, 512, 98);
  Check(s_whole == IRect{0, 0, 512, 98}, "the rectangle's scissor is the whole view");
  const IRect s_wide = MapScissor(at1, IRect{0, 0, 1280, 720}, 512, 98);
  Check(s_wide == IRect{0, 0, 512, 98}, "a scissor of the whole screen is cut to the view: a draw of the phase cannot touch a pixel outside it");
  const IRect s_in = MapScissor(at1, IRect{1030, 84, 1145, 106}, 512, 98);
  Check(s_in.left == int32_t(std::floor(114.0 * 512.0 / 229.0)) && s_in.right == 512 && s_in.top == int32_t(std::floor(22.0 * 98.0 / 44.0)) && s_in.bottom == 98, "a scissor in the lower right keeps its place, edges rounded outwards");
  Check(MapScissor(at1, IRect{0, 0, 100, 40}, 512, 98).empty(), "a scissor wholly outside the rectangle is empty after the cut");
  const IRect s_part = MapScissor(at1, IRect{916, 62, 1000, 90}, 512, 98);
  Check(s_part.right == 188 && s_part.bottom == 63, "the right and bottom edges are rounded up: 187.8 and 62.4 view pixels are 188 and 63 (a draw's pixels are never cut by the rounding)");
  Check(MapScissor(IRect{}, IRect{0, 0, 10, 10}, 512, 98).empty() && MapScissor(at1, IRect{0, 0, 10, 10}, 512, 0).empty(), "no rectangle or no view: empty");
  // a viewport and a scissor of the same draw agree: the scissor's edges are the viewport's, to within the outward rounding
  const Rect vp = MapViewport(at1, Rect{916.0, 62.0, 229.0, 44.0}, 512, 98);
  const IRect sc = MapScissor(at1, IRect{916, 62, 1145, 106}, 512, 98);
  Check(sc.left <= int32_t(std::floor(vp.x)) && sc.right >= int32_t(std::ceil(vp.x + vp.w)) - 0 && sc.top <= int32_t(std::floor(vp.y)) && sc.bottom >= int32_t(std::ceil(vp.y + vp.h)), "the scissor holds the viewport it belongs to");
}

void PrintTables() {
  const Glass left = LeftMirror();
  const Glass right = RightMirror();
  const Rig rig = MakeRig(kEye, 70.0, 40.0);
  std::printf("\nassumed geometry: eye (%.2f, %.2f, %.2f), left glass (%.2f, %.2f, %.2f), right glass (%.2f, %.2f, %.2f)\n", kEye.x, kEye.y, kEye.z, left.centre.x, left.centre.y, left.centre.z,
              right.centre.x, right.centre.y, right.centre.z);
  for (int side = 0; side < 2; ++side) {
    const Glass& g = side == 0 ? left : right;
    const V3 d = Normalize(g.centre - kEye);
    const V3 r = Reflect(d, g.normal);
    const V3 me = MirroredEye(kEye, g.centre, g.normal);
    const double yaw = Deg(std::atan2(r.y, -r.x));      // from straight back, positive to the car's left
    const double pitch = Deg(std::atan2(r.z, std::sqrt(r.x * r.x + r.y * r.y)));
    std::printf("%s mirror: eye to glass %.2f m, reflected ray %.1f deg to the %s of straight back, %.1f deg %s; mirrored eye %.2f m %s of the eye, %.2f m %s\n", side == 0 ? "left " : "right",
                Length(g.centre - kEye), std::fabs(yaw), yaw >= 0.0 ? "left" : "right", std::fabs(pitch), pitch < 0.0 ? "down" : "up", std::fabs(me.x - kEye.x),
                me.x > kEye.x ? "ahead" : "behind", std::fabs(me.y - kEye.y), me.y > kEye.y ? "to the left" : "to the right");
  }
  V3 dl[5], dr[5], all[10];
  LookupDirections(rig, kEye, left, kAssumedDistance, dl);
  LookupDirections(rig, kEye, right, kAssumedDistance, dr);
  for (int i = 0; i < 5; ++i) {
    all[i] = dl[i];
    all[5 + i] = dr[i];
  }
  const Extent el = ExtentOf(rig, dl, 5), er = ExtentOf(rig, dr, 5), eb = ExtentOf(rig, all, 10);
  std::printf("view needed (half angles from straight back, rig centred): left %.1f x %.1f, right %.1f x %.1f, both %.1f x %.1f deg\n", Deg(el.half_h), Deg(el.half_v), Deg(er.half_h),
              Deg(er.half_v), Deg(eb.half_h), Deg(eb.half_v));
  // centre the view on the middle of what the two mirrors need: not done by the game's camera (it looks straight back); the table above is what a centred view must hold.
  std::printf("\nlookup error in degrees (t0 = %.0f m, fresh view), by the distance of the thing behind the glass; and in pixels of the Mid class's view (7 px per degree)\n", kAssumedDistance);
  std::printf("%-6s", "t (m)");
  const double ts[] = {3.0, 5.0, 10.0, 20.0, 50.0, 200.0};
  for (double t : ts) {
    std::printf("%8.0f", t);
  }
  std::printf("\n");
  for (int side = 0; side < 2; ++side) {
    const Glass& g = side == 0 ? left : right;
    std::printf("%-6s", side == 0 ? "left" : "right");
    for (double t : ts) {
      std::printf("%8.1f", Deg(ParallaxError(rig, kEye, g.centre, g.normal, t)));
    }
    std::printf("\n%-6s", "  px");
    for (double t : ts) {
      std::printf("%8.0f", Deg(ParallaxError(rig, kEye, g.centre, g.normal, t)) * PixelsPerDegree(Class::kMid));
    }
    std::printf("\n");
  }
  std::printf("\nthe same for a view drawn one frame (1 m at 216 km/h) and two frames (2 m) before the glass is, left mirror\n");
  for (double behind : {0.0, 1.0, 2.0}) {
    const Rig held = MakeRig(kEye + V3{-behind, 0.0, 0.0}, 70.0, 40.0);
    std::printf("held %.0f m ", behind);
    for (double t : ts) {
      std::printf("%8.1f", Deg(ParallaxError(held, kEye, left.centre, left.normal, t)));
    }
    std::printf("\n");
  }
  const double side_h = 2.0 * Deg(eb.half_h), side_v = 2.0 * Deg(eb.half_v);
  std::printf("\nsizes by class (side mirrors need %.0f x %.0f degrees centred straight back, plus 5 degrees a side; the HUD mirror wants 100 degrees across a 229 by 44 stamp):\n", side_h, side_v);
  std::printf("%-6s %-9s %6s %6s %6s %6s %6s %12s %12s\n", "class", "consumers", "hfov", "vfov", "width", "height", "every", "MiB 1 smpl", "MiB 2 smpl");
  const struct { const char* name; Class c; } classes[] = {{"low", Class::kLow}, {"mid", Class::kMid}, {"high", Class::kHigh}};
  for (const auto& k : classes) {
    for (int mode = 0; mode < 3; ++mode) {
      const Plan p = PlanView(k.c, mode != 1 ? side_h : 0.0, mode != 1 ? side_v : 0.0, mode != 0 ? 100.0 : 0.0);
      std::printf("%-6s %-9s %6.0f %6.0f %6u %6u %6u %12.2f %12.2f\n", k.name, mode == 0 ? "side" : mode == 1 ? "hud" : "both", p.hfov_deg, p.vfov_deg, p.width, p.height, p.every,
                  double(HeldBytes(p, 1, 8, 4, false)) / (1024.0 * 1024.0), double(HeldBytes(p, 2, 8, 4, false)) / (1024.0 * 1024.0));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  TestVectors();
  TestRig();
  TestLookup();
  TestExtent();
  TestPlan();
  TestRemap();
  TestCrop();
  TestSideMirrorsNumbers();
  TestPlanForField();
  TestSideMirrorsField();
  TestRequestField();
  TestPlanForPhase();
  TestMarkers();
  TestRequest();
  TestPhase();
  TestRoute();
  TestPlanForRect();
  TestLifetime();
  TestMaps();
  if (argc > 1 && std::strcmp(argv[1], "--table") == 0) {
    PrintTables();
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

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

// The host's per frame request to the guest (RequestFor): redirect only with the switch on and the view not blocked, the side mirrors' need only with the redirect, nothing else asked for.
void TestRequest() {
  for (int bits = 0; bits < 8; ++bits) {
    RequestFacts f;
    f.enabled = (bits & 1) != 0;
    f.blocked = (bits & 2) != 0;
    f.side_need = (bits & 4) != 0;
    const std::string tag = " [enabled " + std::to_string(f.enabled) + " blocked " + std::to_string(f.blocked) + " need " + std::to_string(f.side_need) + "]";
    const Request r = RequestFor(f);
    Check(r.redirect == (f.enabled && !f.blocked), "redirect only with the switch on and the view not blocked" + tag);
    Check(r.side_need == (r.redirect && f.side_need), "the need counts only with a redirect" + tag);
    Check(r.every == 1 && !r.has_field(), "no cadence and no field asked for until MV4 and MV6" + tag);
    // it survives the link's packing, which is what the guest reads
    const Request back = UnpackRequest(Pack(r));
    Check(back.redirect == r.redirect && back.side_need == r.side_need && back.every == 1 && !back.has_field(), "and the guest reads the same" + tag);
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

// Check of the one picker of the lights (light_pick.h) and the budget table (light_budget.h): LIGHT-UNIFY U3, package LU-3.
//
// The unified picker is the legacy pick in seconds. This program holds that against the two old pickers it replaces, with the checks of the old suites ported to it (the keep bonus, the
// player first, the budget, the fades, the coast) run straight on light_pick::Picker, and then replays the same inputs through BOTH modes:
//   - a RECORDED drive: the rig's inputs (every car the lens draws showed, the camera's frustum) and the view picker's (the rig's lights, the context) of a made up drive of 20 cars that
//     cut into the view, leave through its edge, vanish in the middle of it, flicker out for a frame, switch their lights off and pass behind the camera, 1200 frames, written to the text
//     format the game's recorder writes (light_pick_record.h), read back and replayed through the legacy Tracker and ViewPicker and the unified ones at the rate the legacy code was
//     tuned at (60 guest frames a second): every pick, every fade and every place the same;
//   - a recording the game made, when a path is given (`light_pick_test <recording>`; the rig's lines and the view picker's lines, mcla_gpu_light_pick_record): the same comparison, and a
//     second one with the times the game stamped (where the frame rate is not 60 the two are SUPPOSED to differ in their fades and lingers: the count of picks that differ is printed);
//   - the same scenes at 30, 60 and 144 frames a second: a light fades, lingers, coasts and is forgotten in the same seconds (the unified picker), where the legacy one counted frames.
// Also: the budget table equals what the consumers hold (the shadow level table, the volume's grid, the rig's four lights, the replay's eight views), the lamps' numbers that are reserved
// equal volumetrics_lamps.h's, and a switch of the mode in the middle of a run changes no pick.
//
// A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//   cmake --build <build dir> --target larecomp_light_pick_test
//   <build dir>/src/gpu/larecomp_light_pick_test [<a recording>]

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "headlight_rig_rules.h"
#include "light_budget.h"
#include "light_pick.h"
#include "light_pick_record.h"
#include "local_shadow_rules.h"
#include "local_shadow_types.h"
#include "volumetrics_lamps.h"
#include "volumetrics_types.h"

namespace hr = mcla::gpu::headlight_rules;
namespace lp = mcla::gpu::light_pick;
namespace lb = mcla::gpu::light_budget;
namespace ls = mcla::gpu::local_shadow;
namespace li = mcla::gpu::light_influence;
namespace rec = mcla::gpu::light_pick::record;

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

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// ---------------------------------------------------------------------------
// the budget table
// ---------------------------------------------------------------------------

void CheckBudget() {
  // The table's constants are the consumers'.
  Check(lb::kMaxHeadlights == hr::kMaxLights, "the budget's headlights are the rig's four");
  Check(lb::kMaxShadowViews == larecomp::gpu::kMaxShadowViews && ls::kMaxViews == lb::kMaxShadowViews, "the budget's views are the replay's eight");
  Check(lb::kMaxShadowViews == mcla::gpu::volumetrics::kMaxShadowViews, "and the volume's eight");
  Check(lb::kMaxHeadlights == mcla::gpu::volumetrics::kMaxHeadlights, "and the volume's four headlights");
  Check(ls::kMaxCars == lb::kMaxShadowCars && lb::kMaxShadowCars == lb::kMaxHeadlights, "the cars that can have views are the rig's four");

  // The volume's grid rows are GridFor's.
  namespace vol = mcla::gpu::volumetrics;
  for (const lb::VolumeRow& row : lb::kVolume) {
    const vol::GridDesc grid = vol::GridFor(vol::Level(uint8_t(row.level)));
    Check(grid.lamps == row.lamps && grid.headlights == row.headlights, "the volume's row of a level is its grid's lamps and headlights");
  }
  Check(lb::VolumeRowOf(lb::Level::kOff) == nullptr, "no volume row for off");
  Check(uint8_t(lb::Level::kMedium) == uint8_t(vol::Level::kMedium) && uint8_t(lb::Level::kHigh) == uint8_t(vol::Level::kHigh) && uint8_t(lb::Level::kUltra) == uint8_t(vol::Level::kUltra) &&
            uint8_t(lb::Level::kOff) == uint8_t(vol::Level::kOff),
        "the budget's levels have the volume's numbers");

  // The three level shape: the numbers of the design's table (LIGHT-UNIFY 3.4), through the budget and through the shadow rules that read it.
  const lb::Table three(lb::kThreeLevels);
  const lb::Table two(lb::kTwoLevels);
  struct Row {
    lb::Level level;
    uint32_t cars, lamp_cars, views, tile;
  };
  const Row expect[] = {{lb::Level::kMedium, 2, 0, 2, 512}, {lb::Level::kHigh, 2, 1, 3, 512}, {lb::Level::kUltra, 3, 3, 6, 1024}};
  for (const Row& e : expect) {
    const lb::LightBudget b = lb::BudgetFor(true, e.level, 0, lb::Level::kHigh, three);
    Check(b.shadow_cars == e.cars && b.shadow_lamp_cars == e.lamp_cars && b.shadow_views == e.views && b.shadow_tile == e.tile,
          std::string("the three level table at ") + std::string(ls::LevelName(e.level)) + ": cars, lamp cars, views and the tile");
    Check(ls::MaxCars(e.level) == e.cars && ls::LampCars(e.level, 0) == e.lamp_cars && ls::MaxViews(e.level, 0) == e.views && ls::TileSize(e.level) == e.tile,
          std::string("the shadow rules read the same table at ") + std::string(ls::LevelName(e.level)));
    Check(b.headlights == 4, "headlights on: the rig's four");
    Check(b.volume_lamps == 32 && b.volume_headlights == 4, "the volume at high: 32 lamps and 4 headlights");
    Check(b.lamp_views == 0 && b.lamp_views_spare == 0 && b.lamp_tile == 0, "the street lamps' shadow views are reserved: nothing yet");
  }
  // The debug setting's views a car, off, headlights off, the volume's rows.
  Check(lb::BudgetFor(true, lb::Level::kMedium, 2, lb::Level::kOff, three).shadow_lamp_cars == 2 && lb::BudgetFor(true, lb::Level::kMedium, 2, lb::Level::kOff, three).shadow_views == 4,
        "two views a car at medium: both cars have a view for each lamp, four views");
  Check(lb::BudgetFor(true, lb::Level::kUltra, 1, lb::Level::kOff, three).shadow_lamp_cars == 0 && lb::BudgetFor(true, lb::Level::kUltra, 1, lb::Level::kOff, three).shadow_views == 3,
        "one view a car at ultra: three cars, three views");
  const lb::LightBudget off = lb::BudgetFor(false, lb::Level::kOff, 0, lb::Level::kOff, three);
  Check(off == lb::LightBudget{}, "everything off: an empty budget");
  Check(lb::BudgetFor(false, lb::Level::kHigh, 0, lb::Level::kOff, three).headlights == 0, "HEADLIGHTS off: no headlight (the shadows are their own setting)");
  Check(lb::BudgetFor(true, lb::Level::kOff, 0, lb::Level::kUltra, three).volume_lamps == 48, "the volume at ultra reads 48 lamps");
  // The two level shape (the tests' way back): high is two cars each with two views.
  Check(lb::BudgetFor(true, lb::Level::kHigh, 0, lb::Level::kOff, two).shadow_views == 4 && lb::BudgetFor(true, lb::Level::kUltra, 0, lb::Level::kOff, two).shadow_views == 6,
        "the two level shape: high 4 views, ultra 6");
  Check(lb::BudgetFor(true, lb::Level::kMedium, 0, lb::Level::kOff, two).shadow_tile == 512, "a level the table does not have takes the next one up (medium in the two level shape is high)");
  // The picker's budget follows what the consumers note.
  lp::LightPicker pickers;
  Check(pickers.budget == lb::LightBudget{}, "a new picker's budget is empty");
  pickers.NoteHeadlights(true);
  pickers.NoteShadows(lb::Level::kHigh, 0, ls::kLevelTable);
  pickers.NoteVolume(lb::Level::kMedium);
  Check(pickers.budget.headlights == 4 && pickers.budget.shadow_cars == 2 && pickers.budget.shadow_lamp_cars == 1 && pickers.budget.shadow_views == 3 && pickers.budget.volume_lamps == 12 &&
            pickers.budget.volume_headlights == 2,
        "the consumers note their levels one at a time and the budget is the one function of all three");
  pickers.NoteHeadlights(false);
  Check(pickers.budget.headlights == 0 && pickers.budget.shadow_cars == 2, "HEADLIGHTS off takes the headlights out of the budget and leaves the rest");
}

// ---------------------------------------------------------------------------
// the reserved lamp numbers
// ---------------------------------------------------------------------------

void CheckLampRow() {
  namespace vol = mcla::gpu::volumetrics;
  Check(Near(lp::kLampKeepBonus, double(vol::kKeepBonus), 1e-6), "the reserved lamp keep bonus is volumetrics_lamps.h's");
  Check(Near(lp::kLampFadeSeconds, double(vol::kFadeSeconds), 1e-6), "the reserved lamp fade seconds are volumetrics_lamps.h's");
  // The rig's numbers as seconds are the legacy tuning's at the reference rate.
  const lp::Tuning t = hr::PickTuningOf(hr::TrackerTuning{});
  const lp::Tuning h = lp::HeadlightTuning();
  Check(Near(t.fade_in_per_s, h.fade_in_per_s, 1e-6) && Near(t.linger_s, h.linger_s, 1e-12) && Near(t.coast_s, h.coast_s, 1e-12) && Near(t.drop_s, h.drop_s, 1e-12) &&
            Near(t.coast_linger_s, h.coast_linger_s, 1e-12) && Near(t.keep_bonus, h.keep_bonus, 0.0) && Near(t.half_distance, h.half_distance, 0.0) &&
            Near(t.fade_near_m, h.fade_near_m, 0.0) && Near(t.fade_far_m, h.fade_far_m, 0.0) && Near(t.coast_taper_from, h.coast_taper_from, 0.0),
        "HeadlightTuning() is the legacy TrackerTuning's numbers in seconds (0.12 a frame = 7.2 a second, 6 frames = 0.1 s, 24 = 0.4 s, 90 = 1.5 s, 30 = 0.5 s)");
  const lp::Tuning s = lp::ShadowCarTuning();
  Check(Near(s.keep_bonus, ls::kViewKeepBonus, 0.0) && Near(s.half_distance, ls::kViewHalfDistance, 0.0) && Near(s.off_screen, ls::kViewOffScreen, 0.0) && !s.player_keeps_bonus,
        "ShadowCarTuning() is the view picker's: 1.3, 25 m, 0.35, the player takes no bonus");
  Check(Near(lp::kPlayerScore, 1.0e9, 0.0) && Near(ls::ViewImportance(25.0, true, false), 0.5, 1e-12), "the importance of a car 25 m away is a half");
}

// ---------------------------------------------------------------------------
// a camera, as the rig's test builds it
// ---------------------------------------------------------------------------

struct Camera {
  double eye[3] = {};
  double forward[3] = {0, 0, 1};
  double fov_y_degrees = 50.0;
  double aspect = 16.0 / 9.0;
  double m[4][4] = {};
  li::ViewFrustum frustum;
  double tan_x = 0.0;
  double tan_y = 0.0;

  void Build() {
    double f[3] = {forward[0], forward[1], forward[2]};
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (double& v : f) v /= fl;
    const double up_world[3] = {0, 1, 0};
    double r[3] = {up_world[1] * f[2] - up_world[2] * f[1], up_world[2] * f[0] - up_world[0] * f[2], up_world[0] * f[1] - up_world[1] * f[0]};
    const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (double& v : r) v /= rl;
    const double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    tan_y = std::tan(fov_y_degrees * hr::kPi / 180.0 * 0.5);
    tan_x = tan_y * aspect;
    const double er = eye[0] * r[0] + eye[1] * r[1] + eye[2] * r[2];
    const double eu = eye[0] * u[0] + eye[1] * u[1] + eye[2] * u[2];
    const double ef = eye[0] * f[0] + eye[1] * f[1] + eye[2] * f[2];
    for (int i = 0; i < 3; ++i) {
      m[i][0] = r[i] / tan_x;
      m[i][1] = u[i] / tan_y;
      m[i][2] = f[i] * 0.001;
      m[i][3] = f[i];
    }
    m[3][0] = -er / tan_x;
    m[3][1] = -eu / tan_y;
    m[3][2] = -ef * 0.001;
    m[3][3] = -ef;
    li::MakeViewFrustum(m, eye, &frustum);
  }
};

hr::Candidate CarAt(double x, double z, double distance, bool player = false, bool on_screen = true) {
  hr::Candidate c;
  c.lamp.position[0] = x;
  c.lamp.position[1] = 0.65;
  c.lamp.position[2] = z;
  c.lamp.forward[0] = 1.0;
  c.lamp.forward[1] = 0.0;
  c.lamp.forward[2] = 0.0;
  c.player = player;
  c.camera_distance = distance;
  c.on_screen = on_screen;
  return c;
}

// An entry of a car `distance` metres off, in the view with `reach`.
lp::Entry E(uint64_t id, double distance, bool player = false, double reach = 1.0, bool observed = true) {
  lp::Entry e;
  e.id = id;
  e.player = player;
  e.observed = observed;
  e.distance = distance;
  e.view_known = true;
  e.reach = reach;
  return e;
}

// ---------------------------------------------------------------------------
// the picker itself: the old suites' checks, ported (headlight_rig_test CheckTracker, local_shadow_rules_test CheckPicking*)
// ---------------------------------------------------------------------------

void CheckFadingPick() {
  const double step = lp::kReferenceStep;
  // The importance of a car: a half at 25 m, in the view in full, 0.35 of it for a beam that lights nothing, the player's above every number, the keep bonus.
  {
    lp::Entry e = E(1, 25.0);
    Check(Near(lp::Score(lp::HeadlightTuning(), e, false), 0.5, 1e-12), "a car at 25 m in the view scores a half");
    e.reach = 0.0;
    Check(Near(lp::Score(lp::HeadlightTuning(), e, false), 0.5 * li::kReachFloor, 1e-12), "a beam that lights nothing in the picture: 0.35 of that");
    e.reach = 0.2;
    Check(Near(lp::Score(lp::HeadlightTuning(), e, false), 0.5 * li::ReachWeight(0.2), 1e-12), "a reach of 0.2: ReachWeight of it");
    e.view_known = false;
    e.on_screen = false;
    Check(Near(lp::Score(lp::HeadlightTuning(), e, false), 0.5 * 0.35, 1e-12), "without the view, a lamp off the screen scores 0.35 of it");
    e.on_screen = true;
    Check(Near(lp::Score(lp::HeadlightTuning(), e, false), 0.5, 1e-12), "without the view, a lamp on the screen: in full");
    Check(Near(lp::Score(lp::HeadlightTuning(), e, true), 0.75, 1e-12), "a car that holds a place scores 1.5 times more");
    Check(Near(lp::Score(lp::ShadowCarTuning(), e, true), 0.65, 1e-12), "a car that holds a view scores 1.3 times more");
    e.observed = false;
    e.age_s = 0.1;
    Check(Near(lp::Score(lp::HeadlightTuning(), e, false), 0.25, 1e-12), "a car not reported for 0.1 s: halved (6 frames at 60 FPS)");
    e.coast_ok = true;
    e.age_s = 0.5;
    Check(Near(lp::Score(lp::HeadlightTuning(), e, false), 0.25, 1e-12), "a car that coasts: halved every 0.5 s");
    lp::Entry p = E(2, 90.0, true);
    Check(Near(lp::Score(lp::HeadlightTuning(), p, false), 1.0e9, 0.0) && Near(lp::Score(lp::HeadlightTuning(), p, true), 1.5e9, 1.0), "the player's light scores 1e9, with the bonus in the rig's pick");
    Check(Near(lp::Score(lp::ShadowCarTuning(), p, true), 1.0e9, 0.0), "and without it in the view picker's");
    p.player_penalty = 3.0;
    Check(Near(lp::Score(lp::ShadowCarTuning(), p, false), 1.0e9 - 3.0, 1e-3), "the player's tie-break is taken off");
  }
  // The fade comes up over several frames: 0.12 a frame at 60 FPS.
  {
    lp::Picker picker;
    lp::Result r;
    const lp::Entry e = E(1, 20.0);
    picker.PickFading(step, {&e, 1}, 4, &r);
    Check(r.shown.size() == 1 && Near(r.out[0].fade, 0.12, 1e-6), "the first frame fades in by a step (0.12)");
    picker.PickFading(step, {&e, 1}, 4, &r);
    Check(Near(r.out[0].fade, 0.24, 1e-6), "the second by two");
    for (int i = 0; i < 12; ++i) picker.PickFading(step, {&e, 1}, 4, &r);
    Check(Near(r.out[0].fade, 1.0, 1e-6), "a dozen frames: complete");
    // A light the source stopped reporting fades out in its slot: about 8 frames.
    lp::Entry gone = e;
    gone.observed = false;
    int frames = 0;
    for (; frames < 40; ++frames) {
      gone.age_s = double(frames + 1) * step;
      picker.PickFading(step, {&gone, 1}, 4, &r);
      if (r.shown.empty()) {
        break;
      }
    }
    Check(frames >= 6 && frames <= 12, "a light that is gone fades out in about 8 frames (it was " + std::to_string(frames) + ")");
  }
  // At most `budget` lights, the player's first, the nearest others.
  {
    lp::Picker picker;
    lp::Result r;
    std::vector<lp::Entry> in;
    for (uint64_t frame = 1; frame <= 30; ++frame) {
      in.clear();
      in.push_back(E(100, 10.0, true));
      for (uint64_t k = 1; k <= 9; ++k) in.push_back(E(k, 20.0 + 10.0 * double(k)));
      picker.PickFading(step, in, 4, &r);
      Check(r.shown.size() <= 4, "never more than the budget");
    }
    Check(r.shown.size() == 4 && in[r.shown[0]].player, "ten cars give four lights, the player's first");
    bool nearest = true;
    for (size_t k = 1; k < r.shown.size(); ++k) nearest = nearest && in[r.shown[k]].id <= 3;
    Check(nearest, "the others are the three nearest");
    picker.PickFading(step, in, 2, &r);
    Check(r.shown.size() == 2, "a budget of two: two lights (HEADLIGHTS' budget is the table's)");
    picker.PickFading(step, in, 0, &r);
    Check(r.shown.empty(), "a budget of none: none");
  }
  // The player is first though far and another is near.
  {
    lp::Picker picker;
    lp::Result r;
    const lp::Entry in[2] = {E(1, 90.0, true), E(2, 5.0)};
    for (int i = 0; i < 3; ++i) picker.PickFading(step, in, 4, &r);
    Check(in[r.shown[0]].player, "the player first though far");
  }
  // Hysteresis: three fixed cars at 10, 12, 14 m and the holder of the last place (30 m); a challenger at 28 m is not enough (1.5), one at 8 m is.
  {
    lp::Picker picker;
    lp::Result r;
    std::vector<lp::Entry> in;
    const auto run = [&](double challenger, int frames) {
      for (int i = 0; i < frames; ++i) {
        in.clear();
        in.push_back(E(1, 10.0));
        in.push_back(E(2, 12.0));
        in.push_back(E(3, 14.0));
        in.push_back(E(4, 30.0));
        if (challenger > 0.0) in.push_back(E(5, challenger));
        picker.PickFading(step, in, 4, &r);
      }
    };
    const auto holds = [&](uint64_t id) {
      for (size_t i = 0; i < in.size(); ++i) {
        if (in[i].id == id) return r.out[i].fade > 0.5f && r.out[i].picked;
      }
      return false;
    };
    run(0.0, 20);
    Check(holds(4), "the holder has the last place");
    run(28.0, 20);
    Check(holds(4) && !holds(5), "a slightly nearer challenger does not take the place (hysteresis)");
    run(8.0, 30);
    Check(holds(5) && !holds(4), "a much nearer one does");
  }
  // A car whose beam lights nothing scores lower than one whose beam is in the picture, at the same distance; a car off the screen (no view) likewise.
  {
    lp::Picker picker;
    lp::Result r;
    std::vector<lp::Entry> in;
    for (int f = 0; f < 20; ++f) {
      in.clear();
      for (uint64_t k = 0; k < 4; ++k) in.push_back(E(10 + k, 40.0 + double(k), false, 0.0));
      in.push_back(E(99, 45.0, false, 1.0));
      picker.PickFading(step, in, 4, &r);
    }
    bool has = false;
    for (const uint32_t i : r.shown) has = has || in[i].id == 99;
    Check(has, "the car whose beam is in the picture is picked over cars whose beams are not, at about the same distance");
    for (int f = 0; f < 20; ++f) {
      in.clear();
      for (uint64_t k = 0; k < 4; ++k) {
        lp::Entry e = E(10 + k, 40.0 + double(k));
        e.view_known = false;
        e.on_screen = false;
        in.push_back(e);
      }
      lp::Entry v = E(99, 45.0);
      v.view_known = false;
      in.push_back(v);
      picker.PickFading(step, in, 4, &r);
    }
    has = false;
    for (const uint32_t i : r.shown) has = has || in[i].id == 99;
    Check(has, "without the view: the car on the screen is picked over cars off it");
  }
  // Ties: the lower id; an unseen entry's budget place is counted.
  {
    lp::Picker picker;
    lp::Result r;
    const lp::Entry in[3] = {E(7, 30.0), E(3, 30.0), E(5, 30.0)};
    picker.PickFading(step, in, 2, &r);
    Check(r.ranked.size() == 3 && in[r.ranked[0]].id == 3 && in[r.ranked[1]].id == 5 && in[r.ranked[2]].id == 7, "equal scores: the lower id first");
    Check(r.shown.size() == 2 && in[r.shown[0]].id == 3 && in[r.shown[1]].id == 5, "and the budget takes the first two");
  }
  // A stall: the step is capped, a light never fades by more than a quarter second's worth at once; a negative step is none.
  {
    lp::Picker picker;
    lp::Result r;
    const lp::Entry e = E(1, 20.0);
    picker.PickFading(5.0, {&e, 1}, 4, &r);
    Check(Near(r.out[0].fade, std::min(1.0, 7.2 * lp::kMaxStepSeconds), 1e-6), "a stall of five seconds fades in by a quarter second's worth");
    picker.Forget();
    picker.PickFading(-1.0, {&e, 1}, 4, &r);
    Check(r.out[0].fade == 0.0f && r.shown.empty(), "a step back in time fades by nothing");
  }
  // The coast: the source says a car that held a place left through the edge; it keeps its light while its beam is in the view, tapers off over the last part and goes out at coast_s.
  {
    lp::Picker picker;
    lp::Result r;
    lp::Entry e = E(1, 30.0);
    for (int i = 0; i < 20; ++i) picker.PickFading(step, {&e, 1}, 4, &r);
    Check(Near(r.out[0].fade, 1.0, 1e-6), "the car is in full before it goes");
    lp::Entry coast = e;
    coast.observed = false;
    coast.coast_ok = true;
    int shown_frames = 0;
    bool coasting_seen = false;
    for (int i = 1; i <= 120; ++i) {
      coast.age_s = double(i) * step;
      coast.reach = 0.5;
      picker.PickFading(step, {&coast, 1}, 4, &r);
      if (!r.shown.empty()) shown_frames = i;
      coasting_seen = coasting_seen || r.out[0].coasting;
    }
    Check(coasting_seen, "it is marked as coasting");
    Check(shown_frames >= 88 && shown_frames <= 92, "and goes out at 1.5 s (90 frames at 60 FPS): " + std::to_string(shown_frames));
    // No coast without the source's word, without the view, with the lights off, or when its beam has left the view.
    for (int variant = 0; variant < 4; ++variant) {
      lp::Picker p2;
      lp::Entry a = E(1, 30.0);
      for (int i = 0; i < 20; ++i) p2.PickFading(step, {&a, 1}, 4, &r);
      lp::Entry b = a;
      b.observed = false;
      b.coast_ok = variant != 0;
      b.view_known = variant != 1;
      b.lit = variant != 2;
      b.reach = variant == 3 ? 0.0 : 0.5;
      b.age_s = step;
      p2.PickFading(step, {&b, 1}, 4, &r);
      const bool coasted = r.out[0].coasting;
      Check(variant == 0 ? !coasted : (variant == 1 ? !coasted : (variant == 2 ? !coasted : !coasted)), "no coast: " + std::string(variant == 0 ? "the source did not say it left" : variant == 1 ? "no view" : variant == 2 ? "lights off" : "its beam left the view"));
    }
  }
  // The coast's weight follows the beam out of the view and the distance.
  {
    lp::Picker picker;
    lp::Result r;
    lp::Entry e = E(1, 30.0);
    for (int i = 0; i < 20; ++i) picker.PickFading(step, {&e, 1}, 4, &r);
    lp::Entry c = e;
    c.observed = false;
    c.coast_ok = true;
    c.age_s = step;
    c.reach = 0.03;
    for (int i = 0; i < 10; ++i) picker.PickFading(step, {&c, 1}, 4, &r);
    const double target = li::SmoothStep(0.01, 0.06, 0.03);
    Check(Near(r.out[0].fade, target, 1e-5), "a coasting car's weight settles at the beam's reach weight (smooth between 0.01 and 0.06): " + std::to_string(r.out[0].fade));
    c.reach = 0.5;
    c.distance = 115.0;
    for (int i = 0; i < 20; ++i) picker.PickFading(step, {&c, 1}, 4, &r);
    Check(Near(r.out[0].fade, li::DistanceFade(115.0), 1e-5), "and a far one's by the distance fade");
  }
  // Nothing pops: a pick change moves a weight by at most one step.
  {
    lp::Picker picker;
    lp::Result r;
    std::vector<lp::Entry> in;
    std::array<float, 8> last{};
    double worst = 0.0;
    for (int f = 0; f < 400; ++f) {
      in.clear();
      for (uint64_t k = 0; k < 8; ++k) {
        // cars that trade distances with a slow wave: ranks cross a lot
        const double d = 25.0 + 15.0 * std::sin(double(f) * 0.03 + double(k) * 0.9);
        in.push_back(E(k + 1, d));
      }
      picker.PickFading(step, in, 4, &r);
      for (size_t i = 0; i < in.size(); ++i) {
        worst = std::max(worst, double(std::fabs(r.out[i].fade - last[i])));
        last[i] = r.out[i].fade;
      }
    }
    Check(worst <= 0.12 + 1e-6, "the weight of every light steps by at most 0.12 a frame through hundreds of rank changes (" + std::to_string(worst) + ")");
  }
}

void CheckHeldPick() {
  lp::Picker picker(lp::ShadowCarTuning());
  lp::Result r;
  // The player's car first, then by importance, at most `take`.
  {
    std::vector<lp::Entry> in;
    in.push_back(E(10, 12.0));
    in.push_back(E(11, 8.0));
    in.push_back(E(12, 30.0, true));
    in.push_back(E(13, 50.0));
    picker.PickHeld(in, 3, &r);
    Check(r.shown.size() == 3 && in[r.shown[0]].id == 12 && in[r.shown[1]].id == 11 && in[r.shown[2]].id == 10, "the player's car, then the nearer, three at the most");
    Check(picker.Holds(12) && picker.Holds(11) && picker.Holds(10) && !picker.Holds(13) && picker.held_count() == 3, "who holds a view is kept");
    picker.PickHeld(in, 10, &r);
    Check(r.shown.size() == 4, "no more than the entries");
    picker.PickHeld(in, 0, &r);
    Check(r.shown.empty() && picker.held_count() == 0, "a take of none holds none");
    picker.Forget();
    Check(picker.state_count() == 0, "Forget() ends it");
  }
  // Hysteresis: two cars of about the same importance do not trade a view back and forth.
  {
    picker.Forget();
    int swaps = 0;
    uint64_t holder = 0;
    for (int f = 0; f < 200; ++f) {
      std::vector<lp::Entry> in;
      in.push_back(E(1, 20.0, true));
      in.push_back(E(2, 30.0 + 2.0 * std::sin(double(f) * 0.3)));
      in.push_back(E(3, 30.0 - 2.0 * std::sin(double(f) * 0.3)));
      picker.PickHeld(in, 2, &r);
      const uint64_t now = in[r.shown[1]].id;
      if (f > 0 && now != holder) ++swaps;
      holder = now;
    }
    Check(swaps <= 1, "two cars of the same importance do not trade the second view (" + std::to_string(swaps) + " swaps)");
  }
  // A much nearer car takes it.
  {
    picker.Forget();
    std::vector<lp::Entry> in = {E(1, 20.0, true), E(2, 30.0), E(3, 60.0)};
    picker.PickHeld(in, 2, &r);
    Check(in[r.shown[1]].id == 2, "the nearer of two others holds the second view");
    in[2].distance = 8.0;
    picker.PickHeld(in, 2, &r);
    Check(in[r.shown[1]].id == 3, "a much nearer car takes it");
  }
  // Two player groups (the rig never makes them): the nearer first, the keep bonus does not change it.
  {
    picker.Forget();
    lp::Entry a = E(1, 30.0, true), b = E(2, 10.0, true);
    a.player_penalty = a.distance * 1e-6;
    b.player_penalty = b.distance * 1e-6;
    std::vector<lp::Entry> in = {a, b};
    picker.PickHeld(in, 1, &r);
    Check(in[r.shown[0]].id == 2, "of two player groups the nearer comes first");
    picker.PickHeld(in, 1, &r);
    Check(in[r.shown[0]].id == 2, "and keeps it (no bonus for the player in this pick)");
  }
  // Seeding (a switch of the mode hands over who holds).
  {
    lp::Picker p2(lp::ShadowCarTuning());
    p2.Seed(5, true, 1.0f, false);
    Check(p2.Holds(5) && !p2.Holds(6) && p2.held_count() == 1, "a seeded car holds");
  }
}

// ---------------------------------------------------------------------------
// the same seconds at every frame rate
// ---------------------------------------------------------------------------

struct RateRun {
  double fade_in_s = 0.0;      // the first time the light is in full
  double gone_s = 0.0;         // after the car is no longer reported (not a coast): the first time the light is out
  double dropped_s = 0.0;      // ... and the first time the tracker has forgotten it
  double coast_out_s = 0.0;    // a car that left through the edge: after its last sighting, the first time its light is out
  bool coasting = false;
};

// One car in the view for a second (a few frames at 30 FPS), then not reported; and one at the edge. `fps`: the rate of the clock; `mode`: the picker.
RateRun RunAtRate(double fps, lp::PickMode mode, bool coast_scene) {
  RateRun run;
  hr::Tracker tracker;
  tracker.SetMode(mode);
  std::array<hr::Tracked, hr::kMaxLights> out{};
  Camera cam;
  cam.eye[1] = 1.3;
  cam.Build();
  const double dt = 1.0 / fps;
  uint64_t frame = 0;
  // In the view the lamp is at (0, 0.65, 25) pointing along +z; at the edge it is at the left edge (x = -25 tan_x) and points forward too.
  double lamp[3] = {0.0, 0.65, 25.0};
  if (coast_scene) {
    lamp[0] = -25.0 * cam.tan_x * 1.0;
  }
  const double seen_for = 2.0;
  double t = 0.0;
  bool full = false;
  bool out_seen = false;
  bool dropped = false;
  double last_seen_t = 0.0;
  for (int i = 0; i < int(5.0 * fps); ++i) {
    ++frame;
    t = double(frame) * dt;
    const bool seen = t <= seen_for;
    if (seen) {
      hr::Candidate c = CarAt(lamp[0], lamp[2], 25.0);
      c.lamp.position[0] = lamp[0];
      c.lamp.position[2] = lamp[2];
      c.lamp.forward[0] = 0.0;
      c.lamp.forward[2] = 1.0;
      c.guest = true;
      c.on = 1.0;
      tracker.Observe(c);
      last_seen_t = t;
    }
    const size_t n = tracker.EndFrame(frame, &out, &cam.frustum, t);
    const float fade = n > 0 ? out[0].fade : 0.0f;
    if (n > 0 && out[0].coasting) run.coasting = true;
    if (!full && fade >= 1.0f - 1e-6f) {
      full = true;
      run.fade_in_s = t;
    }
    if (!seen && !out_seen && n == 0) {
      out_seen = true;
      run.gone_s = t - last_seen_t;
    }
    if (!seen && !dropped && tracker.track_count() == 0) {
      dropped = true;
      run.dropped_s = t - last_seen_t;
    }
  }
  run.coast_out_s = run.gone_s;
  return run;
}

void CheckRates() {
  const double rates[] = {30.0, 60.0, 144.0};
  // 1. the weight of a new car reaches 1 in 1 / 7.2 s = 0.139 s whatever the rate (within a frame); the legacy picker's takes 8 frames, so 0.267 s at 30 FPS and 0.056 s at 144.
  // 2. a car that is gone (it vanished in the middle of the picture) has no light 1/7.2 s later; it is forgotten after 0.4 s.
  for (const double fps : rates) {
    const double dt = 1.0 / fps;
    const RateRun u = RunAtRate(fps, lp::PickMode::kUnified, false);
    const RateRun l = RunAtRate(fps, lp::PickMode::kLegacy, false);
    const std::string at = " at " + std::to_string(int(fps)) + " FPS";
    Check(Near(u.fade_in_s, 1.0 / 7.2, dt + 1e-9), "unified: a new light is in full after 0.139 s" + at + " (it took " + std::to_string(u.fade_in_s) + ")");
    Check(Near(u.dropped_s, 0.4, dt + 1e-6), "unified: a light that is gone is forgotten after 0.4 s" + at + " (it took " + std::to_string(u.dropped_s) + ")");
    Check(u.gone_s > 0.0 && u.gone_s <= 1.0 / 7.2 + 2.0 * dt, "unified: a light that is gone is out within 0.14 s" + at + " (it took " + std::to_string(u.gone_s) + ")");
    if (fps == 60.0) {
      Check(Near(l.fade_in_s, u.fade_in_s, 1e-9) && Near(l.gone_s, u.gone_s, 1e-9) && Near(l.dropped_s, u.dropped_s, 1e-9), "at 60 FPS the legacy picker and the unified one are the same");
    } else {
      Check(!Near(l.fade_in_s, 1.0 / 7.2, 0.5 * dt) || fps == 60.0, "legacy" + at + ": counts frames, so its fade is not 0.139 s (the A/B has an effect there)");
    }
  }
  // 3. a car that left through the picture's edge: its light goes on while its beam is in the view for 1.5 s, at every rate.
  for (const double fps : rates) {
    const double dt = 1.0 / fps;
    const RateRun u = RunAtRate(fps, lp::PickMode::kUnified, true);
    const std::string at = " at " + std::to_string(int(fps)) + " FPS";
    Check(u.coasting, "unified: a car that left through the edge coasts" + at);
    Check(u.gone_s >= 1.3 && Near(u.gone_s, 1.5, 0.12 + 2.0 * dt), "unified: its light is out 1.5 s after its last sighting" + at + " (it took " + std::to_string(u.gone_s) + ")");
    const RateRun l = RunAtRate(fps, lp::PickMode::kLegacy, true);
    if (fps == 60.0) {
      Check(Near(l.gone_s, u.gone_s, 1e-9), "at 60 FPS the legacy coast and the unified one end together");
    }
  }
}

// ---------------------------------------------------------------------------
// a drive: 20 cars, a camera, the rig's inputs
// ---------------------------------------------------------------------------

struct SimCar {
  double start[3] = {};
  double velocity[3] = {};   // m/s
  double forward[3] = {1, 0, 0};
  bool player = false;
  int vanish_at = -1;        // a frame after which the game stops drawing it in the middle of the picture
  int lights_off_from = -1;  // a frame range in which its lights are off
  int lights_off_to = -1;
  int flicker = 0;           // every Nth frame the game does not draw it
};

std::vector<SimCar> MakeDrive() {
  std::vector<SimCar> cars;
  const auto add = [&](double x, double z, double vx, double vz, double fx, double fz) {
    SimCar c;
    c.start[0] = x;
    c.start[1] = 0.65;
    c.start[2] = z;
    c.velocity[0] = vx;
    c.velocity[2] = vz;
    c.forward[0] = fx;
    c.forward[2] = fz;
    cars.push_back(c);
    return &cars.back();
  };
  add(0.0, 0.0, 20.0, 0.0, 1, 0)->player = true;           // the player's car
  for (int k = 0; k < 6; ++k) {                              // traffic ahead, the same way, two lanes
    add(30.0 + 22.0 * k, (k % 2 ? 3.6 : -3.6), 12.0 + 1.5 * (k % 3), 0.0, 1, 0);
  }
  for (int k = 0; k < 6; ++k) {                              // oncoming traffic: ahead in the other lanes, passes the camera and goes behind it
    add(150.0 + 45.0 * k, (k % 2 ? 8.0 : 11.5), -15.0 - (k % 2), 0.0, -1, 0);
  }
  for (int k = 0; k < 3; ++k) {                              // crossing traffic: enters and leaves the picture through its sides
    const double side = k % 2 ? 1.0 : -1.0;
    add(60.0 + 50.0 * k, side * 45.0, 0.0, -side * (8.0 + 2.0 * k), 0, -int(side));
  }
  SimCar* v1 = add(90.0, -3.6, 14.0, 0.0, 1, 0);              // vanishes in the middle of the picture
  v1->vanish_at = 400;
  SimCar* v2 = add(110.0, 3.6, 13.0, 0.0, 1, 0);
  v2->vanish_at = 700;
  SimCar* o1 = add(45.0, -7.0, 10.0, 0.0, 1, 0);              // lights off for a stretch
  o1->lights_off_from = 300;
  o1->lights_off_to = 500;
  SimCar* o2 = add(70.0, 7.0, 11.0, 0.0, 1, 0);
  o2->lights_off_from = 600;
  o2->lights_off_to = 650;
  SimCar* f1 = add(35.0, -3.6, 13.0, 0.0, 1, 0);              // the game does not draw it every seventh frame (an occlusion that flickers)
  f1->flicker = 7;
  return cars;
}

struct SimFrame {
  uint64_t frame = 0;
  double now_s = 0.0;
  Camera camera;
  std::vector<hr::Candidate> cars;
};

// The rig's inputs for frame `f` (1-based) at 60 guest frames a second: what Observe was given and the frame's view.
SimFrame MakeFrame(const std::vector<SimCar>& drive, uint64_t f, double fps) {
  SimFrame s;
  s.frame = f;
  s.now_s = double(f) / fps;
  const double t = s.now_s;
  const SimCar& player = drive[0];
  s.camera.eye[0] = player.start[0] + player.velocity[0] * t - 6.0;
  s.camera.eye[1] = 2.4;
  s.camera.eye[2] = 0.0;
  s.camera.forward[0] = 1.0;
  s.camera.forward[1] = -0.08;
  s.camera.forward[2] = 0.0;
  s.camera.Build();
  for (size_t i = 0; i < drive.size(); ++i) {
    const SimCar& car = drive[i];
    hr::Candidate c;
    for (int k = 0; k < 3; ++k) {
      c.lamp.position[k] = car.start[k] + car.velocity[k] * t;
      c.lamp.forward[k] = car.forward[k];
    }
    c.lamp.up[0] = 0.0;
    c.lamp.up[1] = 1.0;
    c.lamp.up[2] = 0.0;
    c.lamp.half_span = 0.55 + 0.01 * double(i % 5);
    c.player = car.player;
    const double d[3] = {c.lamp.position[0] - s.camera.eye[0], c.lamp.position[1] - s.camera.eye[1], c.lamp.position[2] - s.camera.eye[2]};
    c.camera_distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const double extent = li::NdcExtent(s.camera.frustum, c.lamp.position);
    c.on_screen = extent <= 1.3;
    c.guest = true;
    c.on = 1.0;
    // The game draws a car while its bounds are in the view (a little outside the lamp's own place), within 200 m.
    bool drawn = car.player || (extent < 1.18 && c.camera_distance < 200.0);
    if (car.vanish_at >= 0 && int(f) > car.vanish_at) drawn = false;
    if (car.flicker > 0 && int(f) % car.flicker == 0) drawn = false;
    if (car.lights_off_from >= 0 && int(f) >= car.lights_off_from && int(f) < car.lights_off_to) {
      c.on = 0.0;  // the game says the lights are off: the rig does not observe it at all
      drawn = false;
    }
    if (drawn) {
      s.cars.push_back(c);
    }
  }
  return s;
}

struct TrackedCompare {
  uint64_t frames = 0;
  uint64_t lit_frames = 0;
  uint64_t light_sum = 0;  // the lights of every frame added up
  uint64_t coast_frames = 0;
  uint64_t pick_changes = 0;
  uint64_t mismatched_frames = 0;
  uint64_t mismatched_counts = 0;
  double worst_fade = 0.0;
  double worst_position = 0.0;
  std::string first;
};

// Compares two outputs of EndFrame: the same lights in the same order with the same weights.
bool SameLights(const std::array<hr::Tracked, hr::kMaxLights>& a, size_t na, const std::array<hr::Tracked, hr::kMaxLights>& b, size_t nb, TrackedCompare* cmp, uint64_t frame) {
  bool same = na == nb;
  if (na != nb) {
    ++cmp->mismatched_counts;
  }
  for (size_t i = 0; i < std::min(na, nb); ++i) {
    const hr::Tracked& x = a[i];
    const hr::Tracked& y = b[i];
    const double fade = std::fabs(double(x.fade) - double(y.fade));
    double position = 0.0;
    for (int c = 0; c < 3; ++c) {
      position = std::max({position, std::fabs(x.position[c] - y.position[c]), std::fabs(x.forward[c] - y.forward[c]), std::fabs(x.up[c] - y.up[c])});
    }
    cmp->worst_fade = std::max(cmp->worst_fade, fade);
    cmp->worst_position = std::max(cmp->worst_position, position);
    const bool ok = x.key == y.key && x.player == y.player && fade <= 1e-6 && position <= 1e-9 && x.guest == y.guest && Near(x.on, y.on, 0.0) && Near(x.half_span, y.half_span, 1e-12) &&
                    x.coasting == y.coasting && Near(x.reach, y.reach, 1e-12);
    same = same && ok;
  }
  if (!same && cmp->first.empty()) {
    cmp->first = "frame " + std::to_string(frame) + ": " + std::to_string(na) + " lights against " + std::to_string(nb);
    for (size_t i = 0; i < std::min(na, nb); ++i) {
      cmp->first += "; [" + std::to_string(i) + "] key " + std::to_string(a[i].key) + "/" + std::to_string(b[i].key) + " fade " + std::to_string(a[i].fade) + "/" + std::to_string(b[i].fade) +
                    " coast " + std::to_string(a[i].coasting) + "/" + std::to_string(b[i].coasting);
    }
  }
  return same;
}

// Replays rig frames through the legacy and the unified Tracker in lock step and compares every frame. `restamp`: the time of frame f is f / 60 whatever the recording says.
TrackedCompare ReplayRig(const std::vector<rec::RigFrame>& frames, bool restamp, uint64_t* total_kinds_of_pick_changes = nullptr) {
  TrackedCompare cmp;
  hr::Tracker legacy;
  hr::Tracker unified;
  legacy.SetMode(lp::PickMode::kLegacy);
  unified.SetMode(lp::PickMode::kUnified);
  std::array<hr::Tracked, hr::kMaxLights> la{}, ua{};
  std::array<uint32_t, hr::kMaxLights> last_keys{};
  size_t last_n = 0;
  uint64_t first_frame = frames.empty() ? 0 : frames.front().frame;
  (void)first_frame;
  for (const rec::RigFrame& f : frames) {
    for (const hr::Candidate& c : f.cars) {
      legacy.Observe(c);
      unified.Observe(c);
    }
    legacy.SetBeam(f.beam_range, f.beam_outer_degrees);
    unified.SetBeam(f.beam_range, f.beam_outer_degrees);
    const li::ViewFrustum* view = f.has_view ? &f.frustum : nullptr;
    const size_t nl = legacy.EndFrame(f.frame, &la, view);
    const size_t nu = unified.EndFrame(f.frame, &ua, view, restamp ? std::numeric_limits<double>::quiet_NaN() : f.now_s);
    ++cmp.frames;
    cmp.lit_frames += nu > 0 ? 1u : 0u;
    cmp.light_sum += nu;
    cmp.coast_frames += unified.coasting_count() > 0 ? 1u : 0u;
    if (!SameLights(la, nl, ua, nu, &cmp, f.frame)) {
      ++cmp.mismatched_frames;
    }
    bool changed = nu != last_n;
    for (size_t i = 0; i < std::min(nu, last_n) && !changed; ++i) changed = ua[i].key != last_keys[i];
    cmp.pick_changes += changed ? 1u : 0u;
    last_n = nu;
    for (size_t i = 0; i < nu; ++i) last_keys[i] = ua[i].key;
  }
  if (total_kinds_of_pick_changes) *total_kinds_of_pick_changes = cmp.pick_changes;
  return cmp;
}

std::vector<rec::RigFrame> RecordDrive(const std::vector<SimCar>& drive, int frames, double fps) {
  std::vector<rec::RigFrame> out;
  for (int f = 1; f <= frames; ++f) {
    const SimFrame s = MakeFrame(drive, uint64_t(f), fps);
    rec::RigFrame r;
    r.frame = s.frame;
    r.now_s = s.now_s;
    r.has_view = true;
    r.frustum = s.camera.frustum;
    r.beam_range = hr::kRange;
    r.beam_outer_degrees = hr::kOuterDegrees;
    r.cars = s.cars;
    out.push_back(r);
  }
  return out;
}

std::string TextOf(const std::vector<rec::RigFrame>& frames) {
  std::string text;
  for (const rec::RigFrame& f : frames) text += rec::FormatRig(f);
  return text;
}

void CheckRigReplay() {
  const std::vector<SimCar> drive = MakeDrive();
  const std::vector<rec::RigFrame> live = RecordDrive(drive, 1200, 60.0);
  Check(live.size() == 1200, "a drive of 1200 frames");

  // The recording: written to the game's text format, read back, and the same again.
  const std::string text = TextOf(live);
  std::vector<rec::RigFrame> back;
  std::vector<rec::ViewFrame> no_views;
  std::string error;
  Check(rec::Parse(text, &back, &no_views, &error), "the recording is read back: " + error);
  Check(back.size() == live.size() && no_views.empty(), "all its frames, no view picker frames");
  Check(TextOf(back) == text, "and written again it is the same text (every number survives the text)");
  bool same_inputs = back.size() == live.size();
  for (size_t i = 0; same_inputs && i < back.size(); ++i) {
    same_inputs = back[i].frame == live[i].frame && back[i].now_s == live[i].now_s && back[i].cars.size() == live[i].cars.size() && back[i].has_view == live[i].has_view;
    for (size_t c = 0; same_inputs && c < back[i].cars.size(); ++c) {
      const hr::Candidate& a = back[i].cars[c];
      const hr::Candidate& b = live[i].cars[c];
      same_inputs = a.lamp.position[0] == b.lamp.position[0] && a.lamp.forward[2] == b.lamp.forward[2] && a.player == b.player && a.camera_distance == b.camera_distance &&
                    a.on_screen == b.on_screen && a.guest == b.guest && a.on == b.on && a.lamp.half_span == b.lamp.half_span;
    }
  }
  Check(same_inputs, "the cars of every frame come back bit for bit");

  uint64_t changes = 0;
  const TrackedCompare cmp = ReplayRig(back, true, &changes);
  std::printf("  the recorded drive through both pickers: %llu frames, %llu with lights, %llu with a coast, %llu pick changes; worst weight difference %.2e, worst place difference %.2e\n",
              (unsigned long long)cmp.frames, (unsigned long long)cmp.lit_frames, (unsigned long long)cmp.coast_frames, (unsigned long long)cmp.pick_changes, cmp.worst_fade, cmp.worst_position);
  Check(cmp.mismatched_frames == 0, "every frame of the drive: the same lights, keys, weights and places in the legacy and the unified picker " + cmp.first);
  Check(cmp.mismatched_counts == 0, "and the same number of lights every frame");
  Check(cmp.lit_frames > 1100, "the drive lights the road almost all the time");
  Check(cmp.coast_frames > 20, "the drive has cars that leave through the edge and coast (" + std::to_string(cmp.coast_frames) + " frames)");
  Check(cmp.pick_changes > 15, "and the pick of four of twenty cars changes often enough to mean something (" + std::to_string(cmp.pick_changes) + " changes)");
  Check(cmp.worst_fade <= 1e-6 && cmp.worst_position <= 1e-9, "the weights agree to a millionth and the places to a nanometre");

  // The times as the scene clock gives them (the same 60 FPS, a clock that started at some other time): the same picks.
  std::vector<rec::RigFrame> shifted = back;
  for (rec::RigFrame& f : shifted) f.now_s += 1234.5;
  const TrackedCompare shifted_cmp = ReplayRig(shifted, false);
  Check(shifted_cmp.mismatched_frames == 0, "with the scene clock's own times (an offset of 20 minutes) at 60 frames a second the picks are the same " + shifted_cmp.first);

  // At 144 and 30 frames a second the unified picks are by seconds: the same drive recorded at those rates and replayed with their times differs from the legacy pick in the fades (that is the
  // change), not in how often a light is picked: the counts of lit frames per second stay close.
  for (const double fps : {30.0, 144.0}) {
    const std::vector<rec::RigFrame> other = RecordDrive(drive, int(20.0 * fps), fps);
    const TrackedCompare c = ReplayRig(other, false);
    const TrackedCompare c60 = ReplayRig(RecordDrive(drive, 1200, 60.0), false);
    const double mean = double(c.light_sum) / double(c.frames), mean60 = double(c60.light_sum) / double(c60.frames);
    std::printf("  the drive at %.0f FPS with its own times: %llu of %llu frames differ from the legacy pick; %.3f lights a frame on average (%.3f at 60 FPS)\n", fps, (unsigned long long)c.mismatched_frames,
                (unsigned long long)c.frames, mean, mean60);
    Check(Near(mean, mean60, 0.35), "at " + std::to_string(int(fps)) + " FPS the drive has about as many lights as at 60 (the unified fades and lingers are seconds)");
  }
}

// ---------------------------------------------------------------------------
// the view picker's inputs of the same drive
// ---------------------------------------------------------------------------

ls::Light LightOf(const hr::Tracked& t, uint32_t index) {
  ls::Light l;
  float dir[3];
  hr::CarLamp lamp;
  for (int c = 0; c < 3; ++c) {
    lamp.forward[c] = t.forward[c];
    lamp.up[c] = t.up[c];
  }
  hr::BeamAxis(lamp, dir);
  const float pos[3] = {float(t.position[0]), float(t.position[1]), float(t.position[2])};
  float offset[3];
  hr::LampOffset(t.forward, t.up, t.half_span, offset);
  ls::MakeLight(pos, dir, hr::CosOfDegrees(hr::kOuterDegrees), hr::kRange, t.key, t.player, index, &l, offset);
  return l;
}

std::vector<rec::ViewFrame> RecordViews(const std::vector<SimCar>& drive, int frames) {
  std::vector<rec::ViewFrame> out;
  hr::Tracker tracker;  // the rig's lights of the drive
  std::array<hr::Tracked, hr::kMaxLights> tracked{};
  for (int f = 1; f <= frames; ++f) {
    const SimFrame s = MakeFrame(drive, uint64_t(f), 60.0);
    for (const hr::Candidate& c : s.cars) tracker.Observe(c);
    const size_t n = tracker.EndFrame(uint64_t(f), &tracked, &s.camera.frustum);
    rec::ViewFrame v;
    v.frame = uint64_t(f);
    for (size_t i = 0; i < n; ++i) v.lights.push_back(LightOf(tracked[i], uint32_t(i)));
    // The level changes through the drive: ultra (3 cars), high (2), medium (2), then off for a few frames.
    const int phase = (f / 300) % 4;
    v.max_cars = phase == 0 ? 3u : (phase == 1 ? 2u : (phase == 2 ? 2u : 3u));
    if (f % 301 == 0) v.max_cars = 0;
    v.context.have_reference = f % 97 != 0;
    std::memcpy(v.context.reference, s.camera.eye, sizeof(v.context.reference));
    v.context.have_screen = f % 89 != 0;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) v.context.view_projection.m[r][c] = s.camera.m[r][c];
    }
    if (f % 53 != 0) {
      v.context.frustum = s.camera.frustum;  // now and then a frame with no view: the first version's rules
    }
    out.push_back(std::move(v));
  }
  return out;
}

struct ViewCompare {
  uint64_t frames = 0, changes = 0, mismatched = 0;
  std::string first;
};

ViewCompare ReplayViews(const std::vector<rec::ViewFrame>& frames, bool through_manager_picker) {
  ViewCompare cmp;
  ls::ViewPicker legacy;
  ls::ViewPicker unified;
  lp::Picker shared(lp::ShadowCarTuning());
  legacy.SetMode(lp::PickMode::kLegacy);
  unified.SetMode(lp::PickMode::kUnified);
  if (through_manager_picker) unified.SetPicker(&shared);
  ls::Group gl[4], gu[4];
  uint32_t last_keys[4] = {};
  uint32_t last_n = 0;
  for (const rec::ViewFrame& f : frames) {
    const uint32_t nl = legacy.Pick(f.lights.data(), uint32_t(f.lights.size()), f.max_cars, f.context, gl);
    const uint32_t nu = unified.Pick(f.lights.data(), uint32_t(f.lights.size()), f.max_cars, f.context, gu);
    ++cmp.frames;
    bool same = nl == nu && legacy.held_count() == unified.held_count();
    for (uint32_t i = 0; i < std::min(nl, nu); ++i) {
      same = same && gl[i].car_key == gu[i].car_key && gl[i].is_player == gu[i].is_player && gl[i].count == gu[i].count && legacy.held_key(i) == unified.held_key(i);
    }
    if (!same) {
      ++cmp.mismatched;
      if (cmp.first.empty()) cmp.first = "frame " + std::to_string(f.frame) + ": " + std::to_string(nl) + " cars against " + std::to_string(nu);
    }
    bool changed = nu != last_n;
    for (uint32_t i = 0; i < std::min(nu, last_n) && !changed; ++i) changed = gu[i].car_key != last_keys[i];
    cmp.changes += changed ? 1u : 0u;
    last_n = nu;
    for (uint32_t i = 0; i < nu; ++i) last_keys[i] = gu[i].car_key;
    if (f.frame % 211 == 0) {  // the replay forgets who held views when the lights have been off a while
      legacy.Forget();
      unified.Forget();
    }
  }
  return cmp;
}

void CheckViewReplay() {
  const std::vector<SimCar> drive = MakeDrive();
  const std::vector<rec::ViewFrame> live = RecordViews(drive, 1200);
  std::string text;
  for (const rec::ViewFrame& v : live) text += rec::FormatView(v);
  std::vector<rec::RigFrame> no_rig;
  std::vector<rec::ViewFrame> back;
  std::string error;
  Check(rec::Parse(text, &no_rig, &back, &error), "the view picker's recording is read back: " + error);
  Check(back.size() == live.size() && no_rig.empty(), "all its frames");
  std::string again;
  for (const rec::ViewFrame& v : back) again += rec::FormatView(v);
  Check(again == text, "and written again it is the same text");
  for (const bool shared : {false, true}) {
    const ViewCompare cmp = ReplayViews(back, shared);
    std::printf("  the recorded view picks through both pickers%s: %llu frames, %llu pick changes\n", shared ? " (the unified one on the manager's picker)" : "", (unsigned long long)cmp.frames,
                (unsigned long long)cmp.changes);
    Check(cmp.mismatched == 0, std::string("every frame: the same cars hold views in the legacy and the unified picker") + (shared ? " (on a picker handed in)" : "") + " " + cmp.first);
    Check(cmp.changes > 8, "and the views change hands often enough to mean something (" + std::to_string(cmp.changes) + ")");
  }
}

// ---------------------------------------------------------------------------
// a switch of the mode in the middle of a run, and a picker the manager holds
// ---------------------------------------------------------------------------

void CheckSwitch() {
  const std::vector<SimCar> drive = MakeDrive();
  const std::vector<rec::RigFrame> frames = RecordDrive(drive, 900, 60.0);
  // Three trackers: always legacy, always unified, and one that switches every 97 frames. All three give the same lights (at 60 FPS), so a switch pops nothing.
  hr::Tracker a, b, c;
  a.SetMode(lp::PickMode::kLegacy);
  b.SetMode(lp::PickMode::kUnified);
  std::array<hr::Tracked, hr::kMaxLights> oa{}, ob{}, oc{};
  TrackedCompare cmp;
  uint64_t index = 0;
  for (const rec::RigFrame& f : frames) {
    if (index % 97 == 0) c.SetMode((index / 97) % 2 ? lp::PickMode::kLegacy : lp::PickMode::kUnified);
    for (const hr::Candidate& car : f.cars) {
      a.Observe(car);
      b.Observe(car);
      c.Observe(car);
    }
    const li::ViewFrustum* view = f.has_view ? &f.frustum : nullptr;
    const size_t na = a.EndFrame(f.frame, &oa, view);
    const size_t nb = b.EndFrame(f.frame, &ob, view);
    const size_t nc = c.EndFrame(f.frame, &oc, view);
    SameLights(oa, na, ob, nb, &cmp, f.frame);
    TrackedCompare against;
    if (!SameLights(oa, na, oc, nc, &against, f.frame)) {
      ++cmp.mismatched_frames;
      if (cmp.first.empty()) cmp.first = against.first;
    }
    ++index;
  }
  Check(cmp.mismatched_frames == 0, "a tracker that switches between the two pickers every 97 frames makes the same lights as the two that do not (nothing pops at a switch) " + cmp.first);

  // A tracker on the manager's picker: its state lives there (the budget and the pick are one object's), and the same lights come out.
  lp::LightPicker manager;
  hr::Tracker own, shared;
  shared.SetPicker(&manager.headlights);
  std::array<hr::Tracked, hr::kMaxLights> o1{}, o2{};
  TrackedCompare cmp2;
  size_t max_state = 0;
  for (const rec::RigFrame& f : frames) {
    for (const hr::Candidate& car : f.cars) {
      own.Observe(car);
      shared.Observe(car);
    }
    const li::ViewFrustum* view = f.has_view ? &f.frustum : nullptr;
    const size_t n1 = own.EndFrame(f.frame, &o1, view);
    const size_t n2 = shared.EndFrame(f.frame, &o2, view);
    if (!SameLights(o1, n1, o2, n2, &cmp2, f.frame)) ++cmp2.mismatched_frames;
    max_state = std::max(max_state, manager.headlights.state_count());
  }
  Check(cmp2.mismatched_frames == 0, "a tracker that picks with the manager's picker makes the same lights as one with its own " + cmp2.first);
  Check(max_state > 4 && max_state <= 24, "the manager's picker holds the tracks (" + std::to_string(max_state) + " at the most)");
  shared.Clear();
  Check(manager.headlights.state_count() == 0, "Clear() on the tracker forgets the manager's headlight picks");
  // A tuning handed to the tracker is the picker's (seconds): TrackerTuning's frame counts become seconds at the reference rate.
  hr::TrackerTuning tuning;
  tuning.keep_bonus = 2.5;
  tuning.drop_after = 48;
  tuning.fade_in = 0.3f;
  const lp::Tuning t = hr::PickTuningOf(tuning);
  Check(Near(t.keep_bonus, 2.5, 0.0) && Near(t.drop_s, 0.8, 1e-12) && Near(t.fade_in_per_s, 18.0, 1e-6), "a TrackerTuning's frames are seconds at 60 FPS: 48 frames 0.8 s, 0.3 a frame 18 a second");
}

// ---------------------------------------------------------------------------
// a recording from the game
// ---------------------------------------------------------------------------

void CheckRecordingFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Check(false, "the recording " + path + " can be opened");
    return;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::vector<rec::RigFrame> rig;
  std::vector<rec::ViewFrame> views;
  std::string error;
  Check(rec::Parse(buffer.str(), &rig, &views, &error), "the recording " + path + " is read: " + error);
  std::printf("  the game's recording %s: %zu rig frames, %zu view picker frames\n", path.c_str(), rig.size(), views.size());
  if (!rig.empty()) {
    const TrackedCompare cmp = ReplayRig(rig, true);
    Check(cmp.mismatched_frames == 0, "the game's recorded rig inputs: the same lights in both pickers at the rate the legacy code was tuned at " + cmp.first);
    const TrackedCompare real = ReplayRig(rig, false);
    double span = rig.back().now_s - rig.front().now_s;
    double fps = span > 0.0 ? double(rig.size() - 1) / span : 0.0;
    std::printf("  with the times the game stamped (%.1f frames a second): %llu of %llu frames differ from the legacy pick (worst weight difference %.3f); with the times at 60 a second: %llu\n", fps,
                (unsigned long long)real.mismatched_frames, (unsigned long long)real.frames, real.worst_fade, (unsigned long long)cmp.mismatched_frames);
  }
  if (!views.empty()) {
    for (const bool shared : {false, true}) {
      const ViewCompare cmp = ReplayViews(views, shared);
      Check(cmp.mismatched == 0, std::string("the game's recorded view picker inputs: the same cars hold views in both pickers") + (shared ? " (on a picker handed in)" : "") + " " + cmp.first);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckBudget();
  CheckLampRow();
  CheckFadingPick();
  CheckHeldPick();
  CheckRates();
  CheckRigReplay();
  CheckViewReplay();
  CheckSwitch();
  for (int i = 1; i < argc; ++i) {
    CheckRecordingFile(argv[i]);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

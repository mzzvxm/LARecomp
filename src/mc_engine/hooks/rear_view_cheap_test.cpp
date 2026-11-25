// Check of the rear view mirror's cheap options (rear_view_cheap.h): the camera, the city radius, the racers, the camera cut and the every-Nth-frame
// cadence. A program of its own: it needs no game and no ReXGlue, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_rear_view_cheap_test     (CMakeLists.txt, next to larecomp_effect_fade_test)
//   <build dir>/larecomp_rear_view_cheap_test
//
// The compile by hand: clang++ -std=c++23 rear_view_cheap_test.cpp -o rear_view_cheap_test.exe

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "rear_view_cheap.h"

using namespace larecomp::rearview_cheap;

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

void CheckNum(double got, double want, const std::string& what, double tol = 1e-5) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
  }
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

void TestFarPlane() {
  CheckNum(FarPlane(1500.0f, 0.0), 1500.0, "0 leaves the far plane");
  CheckNum(FarPlane(1500.0f, 250.0), 250.0, "250 pulls it in");
  CheckNum(FarPlane(200.0f, 250.0), 200.0, "never pushed out beyond the game's");
  CheckNum(FarPlane(1500.0f, 5.0), 20.0, "never under 20 m");
  CheckNum(FarPlane(10.0f, 5.0), 10.0, "a game far plane under 20 m stays (min of the two)");
  CheckNum(FarPlane(1500.0f, -50.0), 1500.0, "a negative setting is off");
  CheckNum(FarPlane(1500.0f, double(kNaN)), 1500.0, "NaN is off");
  Check(std::isnan(FarPlane(kNaN, 250.0)), "a stock value that is not a number is returned as it is");
  Check(std::isinf(FarPlane(kInf, 250.0)), "an infinite stock value is returned as it is (the register did not hold a far plane)");
  CheckNum(FarPlane(0.0f, 250.0), 0.0, "a zero stock value is left");
}

void TestFieldOfView() {
  CheckNum(FieldOfView(1.0f, 1.0), 1.0, "1.0 leaves the field of view");
  CheckNum(FieldOfView(1.0f, 0.5), 0.5, "0.5 halves it");
  CheckNum(FieldOfView(1.2f, 0.25), 0.3, "0.25 of 1.2");
  CheckNum(FieldOfView(1.0f, 0.01), 0.1, "the scale is held at 0.1");
  CheckNum(FieldOfView(1.0f, 2.0), 1.0, "never widened");
  CheckNum(FieldOfView(1.0f, 0.0), 1.0, "0 is off, not a zero angle");
  CheckNum(FieldOfView(1.0f, -1.0), 1.0, "negative is off");
  CheckNum(FieldOfView(1.0f, double(kNaN)), 1.0, "NaN is off");
  CheckNum(FieldOfView(0.0f, 0.5), 0.0, "a zero stock field is left");
}

void TestLodRadius() {
  CheckNum(LodRadius(600.0f, 0.0), 600.0, "0 leaves the radius");
  CheckNum(LodRadius(600.0f, 60.0), 60.0, "60 m cuts it");
  CheckNum(LodRadius(300.0f, 900.0), 300.0, "never raised");
  CheckNum(LodRadius(600.0f, -1.0), 600.0, "negative is off");
  CheckNum(LodRadius(600.0f, double(kNaN)), 600.0, "NaN is off");
  Check(std::isnan(LodRadius(kNaN, 60.0)), "a stock value that is not a number is returned as it is");
}

void TestCarLevel() {
  Check(CarLevel(0, 0) == 0 && CarLevel(1, 0) == 1 && CarLevel(2, 0) == 2, "0 leaves every level");
  Check(CarLevel(0, 1) == 1, "level 0 becomes 1");
  Check(CarLevel(1, 1) == 1 && CarLevel(2, 1) == 2, "1 and 2 stay at least 1");
  Check(CarLevel(0, 2) == 2 && CarLevel(1, 2) == 2 && CarLevel(2, 2) == 2, "at least 2: all coarsest");
  Check(CarLevel(0, 5) == 2, "more than 2 is the coarsest, not past it");
  Check(CarLevel(-1, 2) == -1, "a level the game marked as not drawn is left");
  Check(CarLevel(7, 2) == 7, "a level outside 0..2 (not a car's) is left");
  Check(CarLevel(2, 1) == 2, "never made finer");
}

void TestCarBudget() {
  CarBudget b;
  Check(b.Allow(0) && b.Allow(0) && b.Allow(0) && b.Allow(0), "0 is no limit");
  Check(b.drawn == 4 && b.turned_away == 0, "four drawn, none turned away");
  b.Reset();
  Check(b.Allow(3) && b.Allow(3) && b.Allow(3), "the first three are drawn");
  Check(!b.Allow(3) && !b.Allow(3), "the fourth and fifth are turned away");
  Check(b.drawn == 3 && b.turned_away == 2, "three drawn, two turned away");
  b.Reset();
  Check(b.Allow(3), "after the phase's reset the budget is whole");
  Check(b.drawn == 1 && b.turned_away == 0, "counts reset");
  Check(b.Allow(1) == false, "a lower setting turns away at once (1 already drawn)");
  b.Reset();
  Check(b.Allow(1) && !b.Allow(1), "1 draws one car");
  Check(b.Allow(-2), "a negative setting is no limit");
}

void TestEyeJumped() {
  const float a[3] = {-600.0f, 18.7f, -637.5f};
  const float near_a[3] = {-598.6f, 18.7f, -637.0f};
  const float far_a[3] = {-300.0f, 18.7f, -637.5f};
  Check(!EyeJumped(a, near_a, true), "a metre and a half in a frame is not a cut");
  Check(EyeJumped(a, far_a, true), "300 m in a frame is a cut");
  Check(EyeJumped(a, near_a, false), "no look before: the first is a cut");
  const float edge[3] = {-600.0f + 19.9f, 18.7f, -637.5f};
  Check(!EyeJumped(a, edge, true), "19.9 m is under the 20 m rule");
  const float over[3] = {-600.0f + 20.1f, 18.7f, -637.5f};
  Check(EyeJumped(a, over, true), "20.1 m is over it");
  const float diag[3] = {-600.0f + 12.0f, 18.7f + 12.0f, -637.5f + 12.0f};
  Check(EyeJumped(a, diag, true), "the distance is 3-D: 12 m on each axis is 20.8 m");
  const float nan[3] = {kNaN, 0.0f, 0.0f};
  Check(EyeJumped(a, nan, true) && EyeJumped(nan, a, true), "a look that is not a position is a cut");
  Check(!EyeJumped(a, a, true), "no move is no cut");
}

std::string Pattern(Cadence& c, int every, int frames, const std::vector<int>& cuts = {}) {
  std::string out;
  for (int f = 0; f < frames; ++f) {
    bool cut = false;
    for (int x : cuts) {
      cut = cut || x == f;
    }
    out += c.Decide(every, cut) ? 'D' : '.';
  }
  return out;
}

void TestCadence() {
  {
    Cadence c;
    Check(Pattern(c, 1, 6) == "DDDDDD", "every 1: every frame");
  }
  {
    Cadence c;
    Check(Pattern(c, 2, 8) == "D.D.D.D.", "every 2: alternate frames, the first drawn");
  }
  {
    Cadence c;
    Check(Pattern(c, 3, 9) == "D..D..D..", "every 3: one in three");
  }
  {
    Cadence c;
    Check(Pattern(c, 4, 9) == "D...D...D", "every 4");
  }
  {
    Cadence c;
    Check(Pattern(c, 2, 8, {3}) == "D.DD.D.D", "a cut on a held frame draws it, and the count starts again from it");
  }
  {
    Cadence c;
    const std::string got = Pattern(c, 2, 6, {2});
    // frame 2 is due anyway: the cut changes nothing there
    Check(got == "D.D.D.", "a cut on a frame that was due changes nothing: " + got);
    Check(c.cut_draws == 0, "not counted as a cut draw");
  }
  {
    Cadence c;
    Pattern(c, 2, 4, {1});
    Check(c.cut_draws == 1, "one cut draw counted");
  }
  {
    Cadence c;
    Pattern(c, 2, 5);
    c.Reset();
    Check(c.Decide(2, false), "after the mirror was down the first frame is drawn");
    Check(!c.Decide(2, false) && c.Decide(2, false), "and the rhythm starts again from it");
  }
  {
    Cadence c;
    Pattern(c, 3, 3);                  // D..
    Check(c.Decide(2, false), "the setting lowered mid-run: due at once");
    Check(!c.Decide(2, false), "and then every second");
  }
  {
    Cadence c;
    Pattern(c, 4, 2);                  // D.
    Check(c.Decide(1, false) && c.Decide(1, false), "back to 1: every frame");
  }
  {
    Cadence c;
    Pattern(c, 2, 10);
    Check(c.drawn == 5 && c.skipped == 5, "totals: five drawn, five held");
  }
  {
    Cadence c;
    Check(c.Decide(0, false) && c.Decide(-3, false), "an every of 0 or less is every frame");
  }
}

}  // namespace

int main() {
  TestFarPlane();
  TestFieldOfView();
  TestLodRadius();
  TestCarLevel();
  TestCarBudget();
  TestEyeJumped();
  TestCadence();
  std::printf("rear_view_cheap_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// Check of the ray tracing scene service's rules (rt_scene_rules.h): CITY-RT M2 package H3. A program of its own: no renderer, no game files, no GPU; exit 0 = passed.
//
//   build: cmake --build <dir> --target larecomp_rt_scene_rules_test, then run src/gpu/larecomp_rt_scene_rules_test
//   or by hand: clang++ -std=c++23 -O1 -I src/gpu src/gpu/rt_scene_rules_test.cpp
//
// It checks: the Want lifecycle (a want counts for its frame and the next, the latest call of a frame wins, the merge of the consumers, hit data implying the structures, the
// idle count and the one frame the release is due in, a want that ends the idle count), the top level's plan for every combination of what is wanted, whether the car was found
// and whether a camera is known, and the camera predictor on made-up drives (a straight road, a steady turn, a steady turn while accelerating, a cut, a stale pose, a new session).

#include <cmath>
#include <cstdio>

#include "rt_scene_rules.h"

using namespace mcla::gpu;
using namespace mcla::gpu::rt_scene;

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

Needs CarsOnly() {
  Needs n;
  n.cars = true;
  return n;
}
Needs CityAt(Level level, float radius = 0.0f) {
  Needs n;
  n.city = level;
  n.city_radius = radius;
  return n;
}
Needs HitAt(Level level) {
  Needs n;
  n.hit = level;
  return n;
}

// ---------------------------------------------------------------------------
// the wants
// ---------------------------------------------------------------------------

void TestNeeds() {
  CHECK(!Needs{}.any());
  CHECK(CarsOnly().any() && CityAt(Level::kHigh).any() && HitAt(Level::kHigh).any());
  // hit data needs the structures
  CHECK(Normalized(HitAt(Level::kUltra)).city == Level::kUltra);
  CHECK(Normalized(HitAt(Level::kHigh)).city == Level::kHigh);
  Needs both = CityAt(Level::kUltra);
  both.hit = Level::kHigh;
  CHECK(Normalized(both).city == Level::kUltra && Normalized(both).hit == Level::kHigh);
  // the merge: cars OR, the higher level, the larger radius
  const Needs m = Merge(Merge(CarsOnly(), CityAt(Level::kHigh, 120.0f)), Merge(HitAt(Level::kUltra), CityAt(Level::kHigh, 300.0f)));
  CHECK(m.cars && m.city == Level::kUltra && m.hit == Level::kUltra && m.city_radius == 300.0f);
  CHECK(Merge(Needs{}, Needs{}) == Needs{});
  // CITY-RT M1c: the textures a consumer of the hit data needs resident; the widest scope wins, and a want of textures without hit data is no want of textures
  Needs alpha = HitAt(Level::kHigh);
  alpha.textures = TextureScope::kAlpha;
  Needs everything = HitAt(Level::kHigh);
  everything.textures = TextureScope::kAll;
  Needs lonely = CityAt(Level::kHigh);
  lonely.textures = TextureScope::kAll;
  CHECK(Normalized(alpha).textures == TextureScope::kAlpha && Normalized(lonely).textures == TextureScope::kNone && Normalized(Needs{}).textures == TextureScope::kNone);
  CHECK(Merge(alpha, Needs{}).textures == TextureScope::kAlpha && Merge(alpha, everything).textures == TextureScope::kAll && Merge(everything, alpha).textures == TextureScope::kAll);
  CHECK(Merge(alpha, Normalized(lonely)).textures == TextureScope::kAlpha && Merge(Normalized(lonely), CarsOnly()).textures == TextureScope::kNone);
  CHECK(!(alpha == everything) && alpha == alpha && !Needs{}.any() && !(alpha == HitAt(Level::kHigh)));
  CHECK(MaxScope(TextureScope::kNone, TextureScope::kAlpha) == TextureScope::kAlpha && MaxScope(TextureScope::kAll, TextureScope::kAlpha) == TextureScope::kAll);
  {
    WantBoard board;
    board.Want(Consumer::kCitySunShadow, alpha);
    CHECK(board.Now().textures == TextureScope::kAlpha && board.Now().hit == Level::kHigh);
    board.Want(Consumer::kCityReflections, everything);
    CHECK(board.Now().textures == TextureScope::kAll);
    board.EndFrame();
    board.Want(Consumer::kCitySunShadow, alpha);  // the reflections' want counts one more frame, then the scope falls back to the shadows'
    CHECK(board.Now().textures == TextureScope::kAll);
    board.EndFrame();
    board.Want(Consumer::kCitySunShadow, alpha);
    CHECK(board.Now().textures == TextureScope::kAlpha);
  }
  CHECK(MaxLevel(Level::kHigh, Level::kOff) == Level::kHigh && MaxLevel(Level::kOff, Level::kUltra) == Level::kUltra && MaxLevel(Level::kUltra, Level::kHigh) == Level::kUltra);
  CHECK(ConsumerName(Consumer::kCityGi) == "city gi" && ConsumerName(Consumer::kCars) == "car effects");
  CHECK(ConsumerName(Consumer::kRtAo) == "rtao" && size_t(Consumer::kRtAo) == 8);
  CHECK(ConsumerName(Consumer::kVirtualBounce) == "light bounce" && size_t(Consumer::kVirtualBounce) + 1 == kConsumerCount);
}

void TestWantLifecycle() {
  WantBoard board;
  CHECK(!board.Now().any() && !board.Wants(Consumer::kCitySunShadow));
  // a want counts for the frame it is made in...
  board.Want(Consumer::kCitySunShadow, CityAt(Level::kHigh));
  CHECK(board.Now().city == Level::kHigh && board.Wants(Consumer::kCitySunShadow) && !board.Wants(Consumer::kCars));
  CHECK(board.ThisFrame().city == Level::kHigh);
  // ... and the next, then ends
  board.EndFrame();
  CHECK(board.Now().city == Level::kHigh && board.Wants(Consumer::kCitySunShadow));
  CHECK(!board.ThisFrame().any());  // nothing was said in the new frame: what the boundary hands on is empty
  board.EndFrame();
  CHECK(!board.Now().any() && !board.Wants(Consumer::kCitySunShadow));
  CHECK(board.frames_wanted(Consumer::kCitySunShadow) == 1 && board.frames_wanted(Consumer::kCars) == 0);

  // the latest call of a frame replaces the consumer's earlier one (and an empty one is "no longer")
  WantBoard b2;
  b2.Want(Consumer::kCityReflections, CityAt(Level::kUltra));
  b2.Want(Consumer::kCityReflections, CityAt(Level::kHigh));
  CHECK(b2.Now().city == Level::kHigh);
  b2.Want(Consumer::kCityReflections, Needs{});
  CHECK(!b2.Now().any());

  // two consumers: the merge, and each is still wanted when the other stops
  WantBoard b3;
  b3.Want(Consumer::kCars, CarsOnly());
  b3.Want(Consumer::kCityGi, HitAt(Level::kUltra));
  Needs now = b3.Now();
  CHECK(now.cars && now.city == Level::kUltra && now.hit == Level::kUltra);
  b3.EndFrame();
  b3.Want(Consumer::kCars, CarsOnly());  // only the cars again
  now = b3.Now();
  CHECK(now.cars && now.hit == Level::kUltra);  // the GI's want of the frame before still counts
  b3.EndFrame();
  now = b3.Now();
  CHECK(now.cars && now.hit == Level::kOff && now.city == Level::kOff);  // and now it does not
  CHECK(b3.Wants(Consumer::kCars) && !b3.Wants(Consumer::kCityGi));
  CHECK(b3.frames_wanted(Consumer::kCars) == 2 && b3.frames_wanted(Consumer::kCityGi) == 1);

  // a want out of range is ignored, not a crash
  WantBoard b4;
  b4.Want(Consumer::kCount, CarsOnly());
  CHECK(!b4.Now().any() && !b4.Wants(Consumer::kCount));
}

void TestIdleAndRelease() {
  WantBoard board;
  uint32_t due = 0;
  uint64_t due_at = 0;
  for (uint64_t f = 0; f < 2 * WantBoard::kIdleReleaseFrames; ++f) {
    board.EndFrame();
    if (board.ReleaseDue()) {
      ++due;
      due_at = f;
    }
  }
  CHECK(due == 1);                                                // once, not every frame after
  CHECK(due_at + 1 == WantBoard::kIdleReleaseFrames);             // after exactly kIdleReleaseFrames closed frames without a want
  CHECK(board.idle_frames() == WantBoard::kIdleReleaseFrames + 1);  // and the count stops there

  // a want in the last frame before the limit restarts the count
  WantBoard b2;
  for (uint32_t f = 0; f < WantBoard::kIdleReleaseFrames - 1; ++f) b2.EndFrame();
  CHECK(!b2.ReleaseDue());
  b2.Want(Consumer::kCars, CarsOnly());
  b2.EndFrame();
  CHECK(b2.idle_frames() == 0 && !b2.ReleaseDue());
  for (uint32_t f = 0; f < WantBoard::kIdleReleaseFrames; ++f) b2.EndFrame();
  CHECK(b2.ReleaseDue());

  // a want while the count is past the limit also restarts it, and the release can come due again later
  WantBoard b3;
  for (uint32_t f = 0; f < WantBoard::kIdleReleaseFrames + 20; ++f) b3.EndFrame();
  b3.Want(Consumer::kCitySunShadow, CityAt(Level::kHigh));
  b3.EndFrame();
  CHECK(b3.idle_frames() == 0);
  uint32_t due_again = 0;
  for (uint32_t f = 0; f < WantBoard::kIdleReleaseFrames + 5; ++f) {
    b3.EndFrame();
    due_again += b3.ReleaseDue() ? 1 : 0;
  }
  CHECK(due_again == 1);
}

// ---------------------------------------------------------------------------
// the plan
// ---------------------------------------------------------------------------

void TestPlan() {
  const Needs none;
  const Needs cars = CarsOnly();
  const Needs city = CityAt(Level::kHigh);
  Needs both = CarsOnly();
  both.city = Level::kUltra;
  const Needs hit_only = HitAt(Level::kHigh);  // not normalised here: the plan is given merged needs

  // nothing wanted: nothing built, whatever the frame has
  for (const bool car : {false, true}) {
    for (const bool camera : {false, true}) {
      CHECK(!PlanTlas(none, car, camera).build);
      CHECK(PlanTlas(none, car, camera).view == ViewSource::kNone);
    }
  }
  // the car was found: its exact camera, the instances by what is wanted (today's behaviour for the cars, and for the cars with the city)
  TlasPlan p = PlanTlas(cars, true, false);
  CHECK(p.build && p.cars && !p.city && p.view == ViewSource::kCarFrame);
  p = PlanTlas(both, true, false);
  CHECK(p.build && p.cars && p.city && p.view == ViewSource::kCarFrame);
  p = PlanTlas(city, true, true);
  CHECK(p.build && !p.cars && p.city && p.view == ViewSource::kCarFrame);  // the city alone, but the exact camera is there: used
  p = PlanTlas(hit_only, true, true);
  CHECK(!p.build);                                                           // hit data alone holds no structures (Normalized makes the real merge carry city)
  p = PlanTlas(Normalized(hit_only), true, true);
  CHECK(p.build && p.city);

  // THE DECOUPLING: the car is not in the frame
  p = PlanTlas(cars, false, true);
  CHECK(!p.build && p.view == ViewSource::kNone);  // the cars alone have nothing to show without the car
  p = PlanTlas(city, false, true);
  CHECK(p.build && !p.cars && p.city && p.view == ViewSource::kCamera);  // the city in the predicted camera
  p = PlanTlas(both, false, true);
  CHECK(p.build && !p.cars && p.city && p.view == ViewSource::kCamera);  // the cars asked, there is no car: the city for the others that asked
  p = PlanTlas(city, false, false);
  CHECK(!p.build && p.view == ViewSource::kNone);  // no camera: nothing (a top level in a wrong place is worse than none)
  CHECK(std::string_view(p.why).find("no camera") != std::string_view::npos);
  p = PlanTlas(Normalized(hit_only), false, true);
  CHECK(p.build && p.city && p.view == ViewSource::kCamera);
  // every plan that builds has a view and something in it
  for (const Needs& n : {cars, city, both, Normalized(hit_only)}) {
    for (const bool car : {false, true}) {
      for (const bool camera : {false, true}) {
        const TlasPlan q = PlanTlas(n, car, camera);
        CHECK(q.build == (q.view != ViewSource::kNone));
        CHECK(!q.build || q.cars || q.city);
        CHECK(q.why != nullptr && q.why[0] != 0);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// the camera
// ---------------------------------------------------------------------------

// A camera to world pose of the game's convention (rows: right, up, BACKWARD, position) looking along the heading `yaw` (about world up) from `position`.
temporal::Mat4 PoseOf(double yaw, double x, double y, double z) {
  temporal::Mat4 m = temporal::Identity();
  const double c = std::cos(yaw), s = std::sin(yaw);
  m.m[0][0] = c;  m.m[0][1] = 0;  m.m[0][2] = -s;  // right
  m.m[1][0] = 0;  m.m[1][1] = 1;  m.m[1][2] = 0;   // up
  m.m[2][0] = s;  m.m[2][1] = 0;  m.m[2][2] = c;   // backward
  m.m[3][0] = x;  m.m[3][1] = y;  m.m[3][2] = z;
  return m;
}

// The step in the camera's own frame: turn by `turn` radians about the camera's up, move `forward` metres along its heading. A constant step is a drive along an arc.
temporal::Mat4 LocalStep(double turn, double forward) {
  temporal::Mat4 d = PoseOf(turn, 0.0, 0.0, -forward);  // forward is minus z in the camera's frame
  return d;
}

double MaxAbs(const temporal::Mat4& a, const temporal::Mat4& b) { return temporal::MaxAbsDiff(a, b); }

void TestPredictorBasics() {
  CameraPredictor cp;
  CameraPrediction out;
  CHECK(!cp.known() && !cp.Predict(5, &out));
  // one pose: held
  const temporal::Mat4 p0 = PoseOf(0.3, 10, 2, -5);
  cp.Note(10, p0, false);
  CHECK(cp.known() && cp.Predict(11, &out) && !out.extrapolated && !out.cut);
  temporal::Mat4 view0;
  CHECK(temporal::Inverse(p0, &view0));
  CHECK(MaxAbs(out.view, view0) < 1e-9);
  // the view looks down -z, so the instance transform flips z
  CHECK(out.flip_z);
  // the same view with z forward (negated third column, the occlusion's view space) says not to flip
  temporal::Mat4 zforward = view0;
  for (int r = 0; r < 4; ++r) zforward.m[r][2] = -zforward.m[r][2];
  CHECK(!CameraPredictor::LooksDownMinusZ(zforward, p0));
  CHECK(CameraPredictor::LooksDownMinusZ(view0, p0));
  // an unknown future: a frame before the pose, and one beyond the age limit
  CHECK(!cp.Predict(9, &out));
  CHECK(cp.Predict(10 + CameraPredictor::kMaxAgeFrames, &out) && !cp.Predict(10 + CameraPredictor::kMaxAgeFrames + 1, &out));
  // not numbers: forgotten
  temporal::Mat4 bad = p0;
  bad.m[3][1] = std::nan("");
  cp.Note(11, bad, false);
  CHECK(!cp.known());
  CHECK(!cp.Predict(12, nullptr));
}

void TestPredictorConstantTwist() {
  // A camera that drives along an arc at a steady speed and turning rate: P(n+1) = D P(n). The prediction of frame n+1 from n and n-1 is exact.
  const temporal::Mat4 d = LocalStep(0.012, 1.4);  // 0.7 degrees and 1.4 m a frame: 84 m/s at 60 FPS, a turn of 41 degrees a second
  temporal::Mat4 pose = PoseOf(1.0, 100, 3, 200);
  CameraPredictor cp;
  double worst = 0.0;
  for (uint64_t frame = 1; frame <= 60; ++frame) {
    CameraPrediction out;
    if (frame >= 3 && cp.Predict(frame, &out)) {
      CHECK(out.extrapolated && !out.cut);
      temporal::Mat4 truth_view;
      CHECK(temporal::Inverse(pose, &truth_view));
      worst = std::max(worst, MaxAbs(out.view, truth_view));
    }
    cp.Note(frame, pose, false);
    pose = temporal::Mul(d, pose);
  }
  CHECK(worst < 1e-8);  // exact up to the arithmetic

  // The same drive with the speed changing by 2 per cent a frame (hard braking or a launch): the error of the held pose is the whole step, the extrapolation's is the change of it
  temporal::Mat4 q = PoseOf(0.2, 0, 0, 0);
  temporal::Mat4 previous = q;
  CameraPredictor accel;
  double speed = 1.0;
  double worst_extrap = 0.0, worst_hold = 0.0;
  for (uint64_t frame = 1; frame <= 40; ++frame) {
    CameraPrediction e;
    if (frame >= 3 && accel.Predict(frame, &e)) {
      temporal::Mat4 truth_view, held_view;
      CHECK(temporal::Inverse(q, &truth_view));
      CHECK(temporal::Inverse(previous, &held_view));
      worst_extrap = std::max(worst_extrap, MaxAbs(e.view, truth_view));
      worst_hold = std::max(worst_hold, MaxAbs(held_view, truth_view));
    }
    accel.Note(frame, q, false);
    previous = q;
    q = temporal::Mul(LocalStep(0.004, speed), q);
    speed *= 1.02;
  }
  CHECK(worst_extrap < 0.05);                // at most the step's change in one frame: centimetres at 1.9 m a frame growing by 2 per cent
  CHECK(worst_extrap * 10.0 < worst_hold);   // and an order of magnitude better than holding the last pose
}

void TestPredictorHoldsAndCuts() {
  CameraPredictor cp;
  CameraPrediction out;
  const temporal::Mat4 a = PoseOf(0.0, 0, 0, 0);
  const temporal::Mat4 b = PoseOf(0.0, 0, 0, -1.5);
  cp.Note(1, a, false);
  cp.Note(2, b, false);
  CHECK(cp.Predict(3, &out) && out.extrapolated);
  // the prediction is one step on: z = -3
  temporal::Mat4 expect_view;
  CHECK(temporal::Inverse(PoseOf(0.0, 0, 0, -3.0), &expect_view));
  CHECK(MaxAbs(out.view, expect_view) < 1e-9);

  // a teleport: the step is beyond what a frame moves: held, and said to be a cut
  const temporal::Mat4 far_pose = PoseOf(0.0, 1600, 0, -1.5);
  cp.Note(3, far_pose, false);
  CHECK(cp.Predict(4, &out) && !out.extrapolated && out.cut);
  temporal::Mat4 far_view;
  CHECK(temporal::Inverse(far_pose, &far_view));
  CHECK(MaxAbs(out.view, far_view) < 1e-9);
  // the frame after the cut has a step again and is predicted
  cp.Note(4, PoseOf(0.0, 1600, 0, -3.0), false);
  CHECK(cp.Predict(5, &out) && out.extrapolated && !out.cut);

  // a turn beyond the cut angle in one frame is a cut too
  CameraPredictor turn;
  turn.Note(1, PoseOf(0.0, 0, 0, 0), false);
  turn.Note(2, PoseOf(1.2, 0, 0, 0), false);
  CHECK(turn.Predict(3, &out) && out.cut && !out.extrapolated);

  // the scene's own cut flag holds the pose whatever the step
  CameraPredictor flagged;
  flagged.Note(1, a, false);
  flagged.Note(2, b, true);
  CHECK(flagged.Predict(3, &out) && out.cut && !out.extrapolated);

  // a gap in the frames (a pause between two poses): not extrapolated, held
  CameraPredictor gap;
  gap.Note(1, a, false);
  gap.Note(5, b, false);
  CHECK(gap.Predict(6, &out) && !out.extrapolated && !out.cut);
  // asking for the frame of the pose itself (the anchor has been read): the pose
  CHECK(gap.Predict(5, &out) && !out.extrapolated);
  // time going backwards (a new session): forgotten
  gap.Note(2, a, false);
  CHECK(gap.known() && gap.last_frame() == 2 && gap.Predict(3, &out) && !out.extrapolated);  // one pose again: held for frame 3
}

void TestStepSize() {
  const CameraPredictor::StepSize s = CameraPredictor::StepOf(PoseOf(0.0, 0, 0, 0), PoseOf(0.25, 3, 4, 0));
  CHECK(std::fabs(s.metres - 5.0) < 1e-12);
  CHECK(std::fabs(s.radians - 0.25) < 1e-9);
  const CameraPredictor::StepSize none = CameraPredictor::StepOf(PoseOf(1.0, 1, 2, 3), PoseOf(1.0, 1, 2, 3));
  CHECK(none.metres == 0.0 && none.radians < 1e-6);
  // opposite headings: pi
  CHECK(std::fabs(CameraPredictor::StepOf(PoseOf(0.0, 0, 0, 0), PoseOf(3.14159265358979, 0, 0, 0)).radians - 3.14159265358979) < 1e-6);
}

}  // namespace

int main() {
  TestNeeds();
  TestWantLifecycle();
  TestIdleAndRelease();
  TestPlan();
  TestPredictorBasics();
  TestPredictorConstantTwist();
  TestPredictorHoldsAndCuts();
  TestStepSize();
  std::printf("rt_scene_rules_test: %d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

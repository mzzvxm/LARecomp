// A check of the gameplay cameras' mouse look geometry (camera_orbit_core.h) that needs no game.
//   cmake --build <dir> --target larecomp_camera_orbit_test, then run it (exit 0 = passed).
//   larecomp_camera_orbit_test <cam_orbit.csv>   also replays a recording made in the game with cam_orbit_record on, checks the same rules on every
//                                                 recorded frame and prints how far from the car's middle the OLD look's "zero" sat on them.
// Or by hand: clang++ -std=c++20 -I src/mc_engine src/mc_engine/camera/camera_orbit_test.cpp -o camera_orbit_test

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "camera/camera_orbit_core.h"

namespace {

using namespace larecomp::camorbit;

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED line %d: %s\n", line, what);
  }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
bool NearV(const V3& a, const V3& b, float eps) { return Len(Sub(a, b)) <= eps; }
float HDist(const V3& a, const V3& b) { return HorizLen(Sub(a, b)); }

constexpr float kPi = 3.14159265f;

// A chase boom: car at `car` heading along `heading` (radians about Y, 0 = +Z), camera `back` behind and `up` above, look-at `ahead` in front of the
// car and `lift` above its middle -- the shape of the real booms (the view line does not pass through the car's middle).
In Chase(const V3& car, float heading, float back, float up, float ahead, float lift) {
  const V3 fwd = RotY({0, 0, 1}, heading);
  In in;
  in.pivot = car;
  in.pos = Add(car, Add(Scale(fwd, -back), V3{0, up, 0}));
  in.look = Add(car, Add(Scale(fwd, ahead), V3{0, lift, 0}));
  return in;
}

void TestIdentity() {
  Params p;
  for (float heading : {0.0f, 1.0f, -2.5f}) {
    In in = Chase({100, 5, -40}, heading, 6.0f, 1.8f, 3.0f, 0.8f);
    Out o = Apply(in, 0.0f, 0.0f, p);
    CHECK(o.mode == Mode::Orbit);
    // Bit for bit: no look means the game's camera, untouched.
    CHECK(std::memcmp(&o.pos, &in.pos, sizeof(V3)) == 0);
    CHECK(std::memcmp(&o.look, &in.look, sizeof(V3)) == 0);
  }
}

// THE BUG REPORT: the zero point has to be the car's exact middle. Half a turn puts the camera on the opposite side of the car, through its middle:
// the middle of the old and the new camera position is the car, whatever the boom's distance and wherever the look-at target sits.
void TestHalfTurnThroughTheMiddle() {
  Params p;
  for (float back : {4.5f, 6.0f, 9.0f, 14.0f}) {
    for (float ahead : {-1.0f, 0.0f, 3.0f, 8.0f}) {
      In in = Chase({12, 0.6f, 30}, 0.7f, back, 1.8f, ahead, 0.8f);
      Out o = Apply(in, kPi, 0.0f, p);
      CHECK(o.mode == Mode::Orbit);
      const V3 mid = Scale(Add(in.pos, o.pos), 0.5f);
      CHECK(HDist(mid, in.pivot) < 1.0e-3f);
      CHECK(Near(o.pos.y, in.pos.y, 1.0e-4f));  // yaw does not change the height
    }
  }
}

// Any yaw keeps the camera at the boom's own distance from the car's middle, and the look-at target turns with it (same angle, same pivot).
void TestYawKeepsDistance() {
  Params p;
  In in = Chase({-300, 2, 80}, -1.2f, 7.0f, 2.0f, 4.0f, 1.0f);
  const float d0 = Len(Sub(in.pos, in.pivot));
  const float h0 = HDist(in.pos, in.pivot);
  const float l0 = Len(Sub(in.look, in.pivot));
  const float pl0 = Len(Sub(in.look, in.pos));
  for (float yaw = -3.0f; yaw <= 3.0f; yaw += 0.25f) {
    Out o = Apply(in, yaw, 0.0f, p);
    CHECK(Near(Len(Sub(o.pos, in.pivot)), d0, 1.0e-3f));
    CHECK(Near(HDist(o.pos, in.pivot), h0, 1.0e-3f));
    CHECK(Near(Len(Sub(o.look, in.pivot)), l0, 1.0e-3f));
    CHECK(Near(Len(Sub(o.look, o.pos)), pl0, 1.0e-3f));  // the view still looks at the same thing from the same distance
    // The arm turned by exactly `yaw`.
    const V3 a = Sub(in.pos, in.pivot), b = Sub(o.pos, in.pivot);
    const float turned = std::atan2(a.x * b.z - a.z * b.x, a.x * b.x + a.z * b.z);  // signed angle from a to b about Y
    CHECK(Near(std::fabs(turned), std::fabs(yaw), 1.0e-3f));
  }
}

// The same sign as the old look: RotY(+yaw) on the arm.
void TestYawSign() {
  Params p;
  In in = Chase({0, 0, 0}, 0.0f, 6.0f, 2.0f, 3.0f, 1.0f);  // camera at (0, 2, -6)
  Out o = Apply(in, 0.5f, 0.0f, p);
  const V3 want = RotY(Sub(in.pos, in.pivot), 0.5f);
  CHECK(NearV(Sub(o.pos, in.pivot), want, 1.0e-4f));
}

// Pitch raises the camera about the car's middle, keeps its distance, never rolls (the yaw of the arm stays), and stops at the limits.
void TestPitch() {
  Params p;
  p.min_elev = 0.0f;
  p.max_elev = 1.0f;
  In in = Chase({5, 0.5f, 5}, 2.0f, 6.0f, 1.5f, 3.0f, 0.8f);
  const V3 arm0 = Sub(in.pos, in.pivot);
  const float e0 = Elevation(arm0);
  const float d0 = Len(arm0);

  Out up = Apply(in, 0.0f, 0.3f, p);
  CHECK(up.pos.y > in.pos.y);
  CHECK(Near(Elevation(Sub(up.pos, in.pivot)), e0 + 0.3f, 1.0e-3f));
  CHECK(Near(Len(Sub(up.pos, in.pivot)), d0, 1.0e-3f));
  const V3 arm1 = Sub(up.pos, in.pivot);
  CHECK(Near(std::atan2(arm1.x, arm1.z), std::atan2(arm0.x, arm0.z), 1.0e-3f));  // same heading: no roll, no yaw

  Out top = Apply(in, 0.0f, 5.0f, p);
  CHECK(Near(Elevation(Sub(top.pos, in.pivot)), 1.0f, 1.0e-3f));
  Out bottom = Apply(in, 0.0f, -5.0f, p);
  CHECK(Near(Elevation(Sub(bottom.pos, in.pivot)), 0.0f, 1.0e-3f));

  // A base camera already below the floor is not pushed up by a pitch that goes further down, and not dragged down either.
  In low = Chase({5, 0.5f, 5}, 2.0f, 6.0f, -0.5f, 3.0f, 0.8f);
  Out lo = Apply(low, 0.0f, -0.2f, p);
  CHECK(NearV(lo.pos, low.pos, 1.0e-5f));
}

// A camera mounted on the car turns where it sits: the position never moves, the look-at swings round it at the same distance.
void TestInPlace() {
  Params p;
  // The bumper: ~2 units ahead of the middle, below the inplace distance.
  In bumper = Chase({0, 0.5f, 0}, 0.3f, -2.0f, 0.3f, 20.0f, 0.3f);
  CHECK(Classify(bumper, p) == Mode::InPlace);
  const float l0 = Len(Sub(bumper.look, bumper.pos));
  for (float yaw : {-3.0f, -0.4f, 1.1f, 3.1f}) {
    Out o = Apply(bumper, yaw, 0.2f, p);
    CHECK(o.mode == Mode::InPlace);
    CHECK(std::memcmp(&o.pos, &bumper.pos, sizeof(V3)) == 0);
    CHECK(Near(Len(Sub(o.look, o.pos)), l0, 1.0e-3f));
  }
  // Positive pitch tips the view down; the limit holds.
  Out down = Apply(bumper, 0.0f, 0.3f, p);
  CHECK(down.look.y < bumper.look.y);
  Out floor = Apply(bumper, 0.0f, 10.0f, p);
  CHECK(Near(Elevation(Sub(floor.look, floor.pos)), -p.view_pitch_limit, 1.0e-3f));
  // The game's own mounted flag wins even when the camera sits far from the pivot.
  In flagged = Chase({0, 0, 0}, 0.0f, 6.0f, 2.0f, 3.0f, 1.0f);
  flagged.mounted_flag = true;
  CHECK(Classify(flagged, p) == Mode::InPlace);
}

// A pivot that is not the car (unset, far away, not finite) falls back to orbiting the look-at target rather than flinging the camera.
void TestPivotFallback() {
  Params p;
  In in = Chase({0, 0, 0}, 0.0f, 6.0f, 2.0f, 3.0f, 1.0f);
  in.pivot = {5000, 0, 5000};
  Out o = Apply(in, 1.0f, 0.0f, p);
  CHECK(o.mode == Mode::OrbitLookAt);
  CHECK(NearV(o.look, in.look, 1.0e-4f));
  CHECK(Near(Len(Sub(o.pos, in.look)), Len(Sub(in.pos, in.look)), 1.0e-3f));
  in.pivot = {NAN, 0, 0};
  CHECK(Classify(in, p) == Mode::OrbitLookAt);
  in.pos = {INFINITY, 0, 0};
  CHECK(Classify(in, p) == Mode::Skipped);
}

// A car driving a curve with the mouse held at a fixed angle: the camera stays at the same offset from the car in the car's own frame every frame.
void TestDrivingSteadyLook() {
  Params p;
  V3 car{0, 0.5f, 0};
  float heading = 0.0f;
  V3 first_local{};
  for (int f = 0; f < 600; ++f) {
    heading += 0.01f;
    car = Add(car, Scale(RotY({0, 0, 1}, heading), 0.6f));
    In in = Chase(car, heading, 6.5f, 1.9f, 3.0f, 0.8f);
    Out o = Apply(in, 1.2f, 0.1f, p);
    const V3 local = RotY(Sub(o.pos, car), -heading);
    if (f == 0) first_local = local;
    CHECK(NearV(local, first_local, 2.0e-3f));
  }
}

// What the OLD look did with the same frames: the guessed centre 7.5 along the view line misses the car's middle by the difference between the boom's
// real geometry and the guess. Kept as a test so the reason for the change stays measurable.
void TestOldCentreMissed() {
  In near = Chase({0, 0.5f, 0}, 0.0f, 5.0f, 1.6f, 3.0f, 0.8f);
  In far = Chase({0, 0.5f, 0}, 0.0f, 10.0f, 2.5f, 3.0f, 0.8f);
  const float miss_near = HDist(OldOrbitCentre(near.pos, near.look, 7.5f), near.pivot);
  const float miss_far = HDist(OldOrbitCentre(far.pos, far.look, 7.5f), far.pivot);
  std::printf("old look's centre vs the car's middle: near boom %.2f, far boom %.2f (new: 0)\n", miss_near, miss_far);
  CHECK(miss_near > 1.0f);  // in front of the car: the bonnet
  CHECK(miss_far > 1.0f);   // behind it: the boot
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Replay of a recording (cam_orbit_record): one row per camBoomCS update with the look engaged.
//   t,yaw,pitch,flag668,px,py,pz,lx,ly,lz,cx,cy,cz
// ---------------------------------------------------------------------------------------------------------------------------------------
int Replay(const char* path) {
  std::ifstream f(path);
  if (!f) {
    std::printf("cannot open %s\n", path);
    return 2;
  }
  Params p;
  std::string line;
  int rows = 0, bad = 0;
  int per_mode[4] = {0, 0, 0, 0};
  double old_miss_sum = 0.0, old_miss_max = 0.0;
  int old_n = 0;
  float worst_dist = 0.0f, worst_half = 0.0f;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == 't' || line[0] == '#') continue;
    for (char& c : line)
      if (c == ',') c = ' ';
    std::istringstream s(line);
    double t;
    float yaw, pitch;
    int flag;
    In in;
    if (!(s >> t >> yaw >> pitch >> flag >> in.pos.x >> in.pos.y >> in.pos.z >> in.look.x >> in.look.y >> in.look.z >> in.pivot.x >> in.pivot.y >> in.pivot.z)) {
      ++bad;
      continue;
    }
    in.mounted_flag = flag != 0;
    ++rows;
    Out o = Apply(in, yaw, pitch, p);
    ++per_mode[int(o.mode)];
    if (o.mode == Mode::Orbit) {
      // The rules, on a real frame: distance to the car's middle kept, half a turn goes through it.
      const float dd = std::fabs(Len(Sub(o.pos, in.pivot)) - Len(Sub(in.pos, in.pivot)));
      if (dd > worst_dist) worst_dist = dd;
      Out half = Apply(in, kPi, 0.0f, p);
      const float hm = HDist(Scale(Add(in.pos, half.pos), 0.5f), in.pivot);
      if (hm > worst_half) worst_half = hm;
      const double miss = HDist(OldOrbitCentre(in.pos, in.look, 7.5f), in.pivot);
      old_miss_sum += miss;
      if (miss > old_miss_max) old_miss_max = miss;
      ++old_n;
    } else if (o.mode == Mode::InPlace) {
      if (std::memcmp(&o.pos, &in.pos, sizeof(V3)) != 0) {
        std::printf("FAILED replay t=%.3f: an in-place camera moved\n", t);
        ++g_failed;
      }
    }
  }
  std::printf("replay %s: %d frames (%d unreadable) orbit %d, in-place %d, orbit-lookat %d, skipped %d\n", path, rows, bad, per_mode[0], per_mode[1],
              per_mode[2], per_mode[3]);
  std::printf("  new: distance to the car's middle off by at most %.4f, half turn misses the middle by at most %.4f\n", worst_dist, worst_half);
  if (old_n)
    std::printf("  old: its centre sat %.2f from the car's middle on average, %.2f at worst\n", old_miss_sum / old_n, old_miss_max);
  ++g_checks;
  if (worst_dist > 0.01f || worst_half > 0.01f) {
    ++g_failed;
    std::printf("FAILED replay: the orbit drifted off the car's middle\n");
  }
  if (rows == 0) {
    ++g_failed;
    std::printf("FAILED replay: no frames in %s\n", path);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  TestIdentity();
  TestHalfTurnThroughTheMiddle();
  TestYawKeepsDistance();
  TestYawSign();
  TestPitch();
  TestInPlace();
  TestPivotFallback();
  TestDrivingSteadyLook();
  TestOldCentreMissed();
  for (int i = 1; i < argc; ++i) {
    const int r = Replay(argv[i]);
    if (r != 0) return r;
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// Check of the title screen's road reflection plane rules (title_road_plane.h). A program of its own: it needs no ReXGlue, no guest and no game, and
// exits 0 when every check holds.
//
//   clang++ -std=c++20 -O1 -Wall -Wextra -I src/mc_engine/hooks src/mc_engine/hooks/title_road_plane_test.cpp -o title_road_plane_test
//   ./title_road_plane_test
//
// What it pins down: the probe rays (the picture's centre looks where the camera looks, the corners fan out the way the field of view says, every
// probe is a unit ray and the grid is the lower part of the picture), which hits are ground (facing up, not above the camera, finite), the plane
// the hits give (the lower quartile: roofs and cars above the road do not lift it, a few hits are not enough, walls and undersides are no ground), the
// follow (a first value and a cut are taken as they are, anything else approaches at the same speed in time at 30 and 240 FPS, never overshoots), and
// the story of the bug in numbers: a title shot 83 m up, the road at 61 m, the game's plane at 1 m.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "title_road_plane.h"

using namespace larecomp::title_plane;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what);
  }
}

void CheckNum(double got, double want, const char* what, double tol = 1e-4) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g\n", what, got, want);
  }
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// A camera at (x, y, z) looking along `forward` (unit, horizontal or not) with Y up: the rows right, up, back.
Camera MakeCamera(Vec3 pos, Vec3 forward, float fov = 50.0f) {
  Camera c;
  c.valid = true;
  c.pos = pos;
  c.fov_degrees = fov;
  const Vec3 world_up{0.0f, 1.0f, 0.0f};
  // right = forward x up
  Vec3 right{forward.y * world_up.z - forward.z * world_up.y, forward.z * world_up.x - forward.x * world_up.z,
             forward.x * world_up.y - forward.y * world_up.x};
  const float rl = Length(right);
  right = Scale(right, 1.0f / rl);
  // up = right x forward
  Vec3 up{right.y * forward.z - right.z * forward.y, right.z * forward.x - right.x * forward.z, right.x * forward.y - right.y * forward.x};
  up = Scale(up, 1.0f / Length(up));
  c.right = right;
  c.up = up;
  c.back = Scale(forward, -1.0f);
  return c;
}

Hit Ground(float y, float ny = 1.0f) {
  Hit h;
  h.pos = {10.0f, y, 20.0f};
  h.normal = {0.0f, ny, std::sqrt(std::max(0.0f, 1.0f - ny * ny))};
  return h;
}

void TestRays() {
  const Camera c = MakeCamera({100.0f, 83.0f, -40.0f}, {0.0f, 0.0f, 1.0f});
  const Ray centre = RayThrough(c, 0.5f, 0.5f);
  CheckNum(Length(centre.dir), 1.0, "a ray is a unit vector");
  CheckNum(centre.dir.z, 1.0, "the centre of the picture looks where the camera looks");
  CheckNum(centre.origin.y, 83.0, "the ray starts at the camera");
  const Ray down = RayThrough(c, 0.5f, 1.0f);
  Check(down.dir.y < 0.0f, "the bottom edge of the picture looks down");
  const float half_v = 25.0f * 3.14159265f / 180.0f;
  CheckNum(down.dir.y / down.dir.z, -std::tan(half_v), "the bottom edge is half the vertical field below the axis", 1e-4);
  const Ray top = RayThrough(c, 0.5f, 0.0f);
  CheckNum(top.dir.y / top.dir.z, std::tan(half_v), "the top edge is half the vertical field above the axis", 1e-4);
  const Ray right = RayThrough(c, 1.0f, 0.5f);
  const Ray left = RayThrough(c, 0.0f, 0.5f);
  CheckNum(right.dir.x, -left.dir.x, "the sides are symmetric");
  CheckNum(std::fabs(right.dir.x / right.dir.z), std::tan(half_v) * kAspect, "the side edge follows the aspect", 1e-4);
  // The camera's right: forward +Z, up +Y: right = forward x up = -X (RH, Y up): the picture's right side has a negative X here.
  Check(right.dir.x * c.right.x > 0.0f, "the picture's right side is on the camera's right");

  size_t unit = 0, below = 0;
  for (size_t i = 0; i < kRayCount; ++i) {
    const Ray r = ProbeRay(c, i);
    if (std::fabs(Length(r.dir) - 1.0f) < 1e-4f) ++unit;
    if (r.dir.y < 0.0f) ++below;
  }
  Check(unit == kRayCount, "every probe is a unit ray");
  CheckNum(double(kRayCount), 20.0, "5 columns x 4 rows");
  Check(below == kRayCount, "a level camera's rows are all under the middle of the picture, so every probe looks down");
  Check(ProbeRay(c, 0).dir.y > ProbeRay(c, kRayCount - 1).dir.y, "the first probe is higher in the picture than the last");

  Camera zero = c;
  zero.back = {};
  Check(!Usable(zero), "a zero row is not a camera");
  Camera nan_cam = c;
  nan_cam.pos.x = kNaN;
  Check(!Usable(nan_cam), "a NaN position is not a camera");
  Camera bad_fov = c;
  bad_fov.fov_degrees = 0.0f;
  Check(!Usable(bad_fov), "a field of view of 0 is not a camera");
  bad_fov.fov_degrees = 179.0f;
  Check(!Usable(bad_fov), "a field of view of 179 is not a camera");
  Camera invalid = c;
  invalid.valid = false;
  Check(!Usable(invalid), "an unset camera is not a camera");
  Check(Usable(c), "the camera is usable");
}

void TestGround() {
  Check(IsGround(Ground(60.0f), 83.0f), "an upward hit under the camera is ground");
  Check(!IsGround(Ground(90.0f), 83.0f), "a hit above the camera is not ground (a roof)");
  Check(IsGround(Ground(83.0f), 83.0f), "a hit level with the camera is ground (a camera on the road)");
  Check(!IsGround(Ground(60.0f, 0.5f), 83.0f), "a slope past the limit is not ground");
  Check(IsGround(Ground(60.0f, 0.8f), 83.0f), "a gentle slope is ground");
  Check(!IsGround(Ground(60.0f, -1.0f), 83.0f), "a hit facing down (an underside) is not ground");
  Hit nan = Ground(60.0f);
  nan.pos.y = kNaN;
  Check(!IsGround(nan, 83.0f), "a NaN hit is not ground");
  Hit nan_normal = Ground(60.0f);
  nan_normal.normal.y = kNaN;
  Check(!IsGround(nan_normal, 83.0f), "a NaN normal is not ground");
}

void TestPlane() {
  float plane = -1.0f;
  std::vector<Hit> none;
  Check(!PlaneFromHits(none, 83.0f, &plane), "no hits: no plane");
  std::vector<Hit> two = {Ground(61.0f), Ground(61.0f)};
  Check(!PlaneFromHits(two, 83.0f, &plane), "two hits are not enough");
  std::vector<Hit> three = {Ground(61.0f), Ground(61.0f), Ground(61.0f)};
  Check(PlaneFromHits(three, 83.0f, &plane), "three ground hits are enough");
  CheckNum(plane, 61.0, "the plane of a flat road");

  // A road at 61 with cars and a roof in the sample: the roof is above the camera and drops out; the cars lift only the top of the sorted list.
  std::vector<Hit> mixed = {Ground(61.0f), Ground(61.1f), Ground(60.9f), Ground(62.6f), Ground(62.7f), Ground(61.0f), Ground(120.0f), Ground(75.0f)};
  Check(PlaneFromHits(mixed, 83.0f, &plane), "a road with cars and a roof gives a plane");
  Check(plane >= 60.9f && plane <= 61.1f, "the lower quartile is the road, not the cars or the roof");

  // Walls and undersides are not ground.
  std::vector<Hit> walls = {Ground(61.0f, 0.1f), Ground(62.0f, 0.1f), Ground(63.0f, 0.0f), Ground(64.0f, -1.0f)};
  Check(!PlaneFromHits(walls, 83.0f, &plane), "walls and undersides give no plane");

  // The quartile of a sorted list of n: index (n-1)/4.
  std::vector<Hit> eight;
  for (int i = 0; i < 8; ++i) eight.push_back(Ground(50.0f + float(i)));
  Check(PlaneFromHits(eight, 83.0f, &plane), "eight hits");
  CheckNum(plane, 51.0, "the lower quartile of 8 is the second lowest ((8 - 1) / 4 = 1)");
  std::vector<Hit> three_levels = {Ground(70.0f), Ground(50.0f), Ground(60.0f)};
  Check(PlaneFromHits(three_levels, 83.0f, &plane), "three levels");
  CheckNum(plane, 50.0, "the lower quartile of 3 is the lowest");

  // The input is not changed by the call.
  Check(three_levels[0].pos.y == 70.0f, "the hits are left as they were");

  // A camera low on the street: the ground is level with it.
  std::vector<Hit> street = {Ground(1.6f), Ground(1.5f), Ground(1.5f)};
  Check(PlaneFromHits(street, 3.0f, &plane), "a camera 1.5 m over the road");
  CheckNum(plane, 1.5, "the plane is the road's height");
}

void TestFollow() {
  CheckNum(Follow(false, 5.0f, 61.0f, 0.016f), 61.0, "the first value is taken as it is");
  CheckNum(Follow(true, kNaN, 61.0f, 0.016f), 61.0, "a previous value that is not a number is dropped");
  CheckNum(Follow(true, 1.0f, 61.0f, 0.016f), 61.0, "a cut (60 m away) is taken as it is");
  CheckNum(Follow(true, 61.0f, 61.0f + kCutMetres, 0.016f), 61.0f + kCutMetres, "exactly the cut distance is a cut");
  const float step = Follow(true, 61.0f, 64.0f, 0.016f);
  Check(step > 61.0f && step < 64.0f, "a small move approaches and does not arrive");
  CheckNum(Follow(true, 61.0f, 64.0f, 0.0f), 61.0, "no time, no move");
  CheckNum(Follow(true, 61.0f, 64.0f, kNaN), 61.0, "a time that is not a number is no time");
  Check(Follow(true, 61.0f, 64.0f, 100.0f) < 64.0f, "a long pause is a quarter second at most: it does not jump");

  // The same speed in time at 30 and 240 FPS.
  const auto run = [](float fps, float seconds) {
    float p = 61.0f;
    const int frames = int(std::lround(fps * seconds));
    for (int i = 0; i < frames; ++i) p = Follow(true, p, 64.0f, 1.0f / fps);
    return p;
  };
  CheckNum(run(30.0f, 0.3f), run(240.0f, 0.3f), "the plane is at the same place after 0.3 s at 30 and 240 FPS", 1e-3);
  CheckNum(run(60.0f, 0.3f), run(120.0f, 0.3f), "and at 60 and 120 FPS", 1e-3);
  CheckNum(run(60.0f, kTauSeconds), 61.0 + 3.0 * (1.0 - std::exp(-1.0)), "after one follow time it has covered 1 - 1/e of the way", 0.02);

  // It never overshoots, from either side.
  float p = 61.0f;
  for (int i = 0; i < 600; ++i) {
    p = Follow(true, p, 64.0f, 0.016f);
    if (p > 64.0f + 1e-4f) {
      Check(false, "the plane overshot its target");
      break;
    }
  }
  CheckNum(p, 64.0, "and arrives", 1e-3);
  p = 64.0f;
  for (int i = 0; i < 600; ++i) {
    p = Follow(true, p, 61.0f, 0.016f);
    if (p < 61.0f - 1e-4f) {
      Check(false, "the plane undershot its target");
      break;
    }
  }
}

void TestChosen() {
  CheckNum(Chosen(true, 61.0f, 1.0f), 61.0, "a plane that is known is used");
  CheckNum(Chosen(false, 61.0f, 1.0f), 1.0, "no plane known: the game's own value");
  CheckNum(Chosen(true, kNaN, 1.0f), 1.0, "a plane that is not a number: the game's own value");
  Check(Fresh(0.0), "a pose of this instant is fresh");
  Check(Fresh(0.5), "half a second is still fresh");
  Check(!Fresh(0.51), "past half a second it is stale");
  Check(!Fresh(-1.0), "an age below zero is not a pose");
}

// The bug in numbers: a front-end shot 83 m up looking 6 degrees down at a road 61 m up (a freeway on a hill), the plane of the game at 1 m. With the
// plane at the road the mirror of a point on the road is the point itself: a puddle reflects what stands on the road, not the city below sea level.
void TestStory() {
  const float cam_y = 83.0f, road_y = 61.0f, game_plane = 1.0f;
  const Camera c = MakeCamera({2903.0f, cam_y, 22.6f}, {-0.993f, -0.106f, 0.044f});
  Check(Usable(c), "the shot's camera is usable");
  // Probe the flat road: every ray that points down meets y = road_y; the others miss.
  std::vector<Hit> hits;
  size_t down_rays = 0;
  for (size_t i = 0; i < kRayCount; ++i) {
    const Ray r = ProbeRay(c, i);
    if (r.dir.y < -1e-4f) {
      const float t = (road_y - cam_y) / r.dir.y;
      if (t <= kRayLengthMetres) {
        ++down_rays;
        Hit h;
        h.pos = Add(r.origin, Scale(r.dir, t));
        h.normal = {0.0f, 1.0f, 0.0f};
        hits.push_back(h);
      }
    }
  }
  Check(down_rays >= kMinHits, "enough rays of the grid reach the road");
  float plane = 0.0f;
  Check(PlaneFromHits(hits, cam_y, &plane), "the road gives a plane");
  CheckNum(plane, road_y, "the plane is the road's height", 1e-3);
  // A point on the road mirrors to itself about the right plane and 2 * (1 - 61) = -120 m under the road about the game's.
  const float y_point = road_y;
  CheckNum(2.0f * plane - y_point, road_y, "mirrored about the plane a road point stays on the road");
  CheckNum(2.0f * game_plane - y_point, -59.0, "mirrored about the game's plane it lands 120 m under the road");
}

}  // namespace

int main() {
  TestRays();
  TestGround();
  TestPlane();
  TestFollow();
  TestChosen();
  TestStory();
  if (g_failed != 0) {
    std::printf("%d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("title_road_plane_test: all %d checks passed\n", g_checks);
  return 0;
}

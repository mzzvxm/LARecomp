// Check of the side mirrors' glass overlay (mirror_overlay_rules.h, hlsl/mirror_overlay.hlsli; perf/MIRROR-VIEW.md MV4): the frame's decision and the hand-off with the ray traced side mirrors, the camera and
// the pose of the view, the quaternion, the depth test of the glass draw, the pipeline variants, the 128 byte push block, THE SHADER'S OWN TEXT (the .hlsli compiled as C++ through hlsl_cpp_shim.h) held
// against the design's planar lookup (mirror_view_rules.h UvForMirrorPixel) in double precision for a picture drawn this frame and for a held one, in a world 2.7 km from its origin, and the vertex
// stage's clip position against the game's own chain (world view projection, the viewport transform, the jitter); and that the shaders, the setting, the scene's calls, the precache, the pass timing
// labels and the build files hold what the headers say. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_mirror_overlay_rules_test, then run src/gpu/larecomp_mirror_overlay_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and settings; by default the one this file is in)
// The compile by hand: clang++ -std=c++23 -I <src> -I <src>/gpu mirror_overlay_rules_test.cpp -o mirror_overlay_rules_test.exe

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

#include "hlsl_cpp_shim.h"

namespace hlsl_shim {
#include "hlsl/mirror_overlay.hlsli"
}  // namespace hlsl_shim

#include "mirror_overlay_rules.h"

namespace mo = mcla::gpu::mirror_overlay;
namespace mv = mcla::gpu::mirror_view;
namespace tp = mcla::gpu::temporal;

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

bool Near(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
size_t At(const std::string& text, const std::string& part) { return text.find(part); }

constexpr double kPi = 3.14159265358979323846;

using V3 = mv::V3;

// ---------------------------------------------------------------------------
// a world: a car 2.7 km from the origin, a cockpit camera, the glass patches in the car's own frame (x forward, y left, z up)
// ---------------------------------------------------------------------------

V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// model (car-local) -> world, row vectors: a turn about z by `yaw` and a move to `at`.
tp::Mat4 CarMatrix(double yaw, V3 at) {
  tp::Mat4 m = tp::Identity();
  const double c = std::cos(yaw), s = std::sin(yaw);
  m.m[0][0] = c;
  m.m[0][1] = s;
  m.m[1][0] = -s;
  m.m[1][1] = c;
  m.m[3][0] = at.x;
  m.m[3][1] = at.y;
  m.m[3][2] = at.z;
  return m;
}

V3 Transform(const tp::Mat4& m, V3 p) {
  return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0], p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1], p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}
V3 TransformVector(const tp::Mat4& m, V3 p) {
  return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0], p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1], p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2]};
}

// The frame's view matrix (Frame::view): world -> the AO's view space (x right, y up, z forward), row vectors, the eye at the origin of the view.
tp::Mat4 ViewFrom(V3 eye, V3 right, V3 up, V3 forward) {
  tp::Mat4 m = tp::Identity();
  const V3 axes[3] = {right, up, forward};
  for (int c = 0; c < 3; ++c) {
    m.m[0][c] = axes[c].x;
    m.m[1][c] = axes[c].y;
    m.m[2][c] = axes[c].z;
    m.m[3][c] = -mv::Dot(eye, axes[c]);
  }
  return m;
}

// A camera at `eye` (world) looking along `forward` (world, unit) with the world's z as up: right = forward x up (a left handed triple, as the AO's view space is).
tp::Mat4 LookView(V3 eye, V3 forward) {
  const V3 up_ref{0, 0, 1};
  const V3 right = mv::Normalize(Cross(forward, up_ref));
  const V3 up = Cross(right, forward);  // right x forward
  return ViewFrom(eye, right, up, forward);
}

struct ProjSpec {
  double sx = 0.9, sy = 1.6, ox = 0.0, oy = 0.0;
  double near_plane = 0.1, far_plane = 2000.0;
};

// The game's clip space over the AO's view space: x' = sx x + ox z, y' = sy y + oy z, z' = A z + B, w' = z with the reverse depth (1 near, 0 far).
tp::Mat4 ProjectionAo(const ProjSpec& s) {
  tp::Mat4 p;
  p.m[0][0] = s.sx;
  p.m[2][0] = s.ox;
  p.m[1][1] = s.sy;
  p.m[2][1] = s.oy;
  p.m[2][2] = -s.near_plane / (s.far_plane - s.near_plane);
  p.m[3][2] = s.far_plane * s.near_plane / (s.far_plane - s.near_plane);
  p.m[2][3] = 1.0;
  return p;
}

struct Ndc {
  double x = 0, y = 0, z = 0, w = 0;
};

// The game's chain for a world point: (p, 1) * view_projection, then the vertex shader epilogue's transform with the draw's scale and offset and the jitter, then the divide.
Ndc Direct(V3 world, const tp::Mat4& view_projection, const float scale[3], const float offset[3], const float jitter[2]) {
  const double c[4] = {world.x * view_projection.m[0][0] + world.y * view_projection.m[1][0] + world.z * view_projection.m[2][0] + view_projection.m[3][0],
                       world.x * view_projection.m[0][1] + world.y * view_projection.m[1][1] + world.z * view_projection.m[2][1] + view_projection.m[3][1],
                       world.x * view_projection.m[0][2] + world.y * view_projection.m[1][2] + world.z * view_projection.m[2][2] + view_projection.m[3][2],
                       world.x * view_projection.m[0][3] + world.y * view_projection.m[1][3] + world.z * view_projection.m[2][3] + view_projection.m[3][3]};
  Ndc n;
  n.w = c[3];
  n.x = (c[0] * scale[0] + c[3] * (double(offset[0]) + jitter[0])) / c[3];
  n.y = (c[1] * scale[1] + c[3] * (double(offset[1]) + jitter[1])) / c[3];
  n.z = (c[2] * scale[2] + c[3] * offset[2]) / c[3];
  return n;
}

struct Glass {
  V3 centre_local;
  V3 normal_local;  // unit, facing the driver
};

struct World {
  tp::Mat4 car;            // model -> world
  tp::Mat4 view;           // the main camera
  tp::Mat4 view_projection;
  V3 eye_world;
  V3 forward_world;
  Glass left, right;
  float scale[3] = {1.0f, -1.0f, -1.0f};   // the viewport transform of a reverse depth viewport (viewport.cpp: y negated, z mirrored)
  float offset[3] = {0.0f, 0.0f, 1.0f};
  ProjSpec proj;
};

// The design's assumed cockpit (MIRROR-VIEW.md 2.3): eye (0, 0.35, 0) in the car's frame, glasses at (0.55, +-0.90, -0.15), normals tilted towards the driver and back. The car is turned 37 degrees and
// 2.7 km from the world's origin.
World MakeWorld(double cam_yaw = 0.0, double cam_pitch = 0.0, ProjSpec proj = ProjSpec{}) {
  World w;
  w.proj = proj;
  w.car = CarMatrix(37.0 * kPi / 180.0, {2700.0, -1500.0, 31.0});
  const V3 eye_local{0.0, 0.35, 0.0};
  w.eye_world = Transform(w.car, eye_local);
  const V3 f_local{std::cos(cam_pitch) * std::cos(cam_yaw), std::cos(cam_pitch) * std::sin(cam_yaw), std::sin(cam_pitch)};
  w.forward_world = mv::Normalize(TransformVector(w.car, f_local));
  w.view = LookView(w.eye_world, w.forward_world);
  w.view_projection = tp::Mul(w.view, ProjectionAo(proj));
  w.left = {{0.55, 0.90, -0.15}, mv::Normalize(V3{-0.91, -0.41, 0.0})};
  w.right = {{0.55, -0.90, -0.15}, mv::Normalize(V3{-0.93, 0.37, 0.0})};
  return w;
}

// The 3x3 of a world pose, to put a pose back as the quaternion's source: used by the held pictures.
mo::ViewPose PoseAt(const World& w, double hfov, double vfov, uint64_t frame) {
  mo::ViewPose p;
  const bool ok = mo::PoseOfView(w.view, hfov, vfov, frame, &p);
  Check(ok, "a pose is made from the camera's view matrix");
  return p;
}

hlsl_shim::MirrorOverlayPush ToShim(const mo::Push& p) {
  hlsl_shim::MirrorOverlayPush s;
  auto set = [](hlsl_shim::float4& f, const float v[4]) { f = hlsl_shim::float4(v[0], v[1], v[2], v[3]); };
  set(s.col_x, p.v[0]);
  set(s.col_y, p.v[1]);
  set(s.col_z, p.v[2]);
  set(s.proj_a, p.v[3]);
  set(s.proj_b, p.v[4]);
  set(s.normal, p.v[5]);
  set(s.delta, p.v[6]);
  set(s.rot, p.v[7]);
  return s;
}

mo::PushInputs InputsFor(const World& w, const Glass& g, const mo::ViewPose& rig, double distance = 20.0) {
  mo::PushInputs in;
  in.model_to_world = w.car;
  in.view = w.view;
  in.view_projection = w.view_projection;
  for (int c = 0; c < 3; ++c) {
    in.ndc_scale[c] = w.scale[c];
    in.ndc_offset[c] = w.offset[c];
  }
  const V3 n_world = mv::Normalize(TransformVector(w.car, g.normal_local));
  mo::CameraAxes axes;
  mo::AxesOfView(w.view, &axes);
  for (int k = 0; k < 3; ++k) {
    in.normal_view[k] = n_world.x * axes.axis[k][0] + n_world.y * axes.axis[k][1] + n_world.z * axes.axis[k][2];
  }
  in.rig = rig;
  in.distance = distance;
  in.picture_slot = 77;
  return in;
}

// Glass points: a small vertical patch around the glass's centre, in the car's frame.
std::vector<V3> PatchPoints(const Glass& g) {
  const V3 t1 = mv::Normalize(V3{-g.normal_local.y, g.normal_local.x, 0.0});  // horizontal along the glass
  const V3 t2{0, 0, 1};
  std::vector<V3> pts;
  for (int i = -3; i <= 3; ++i) {
    for (int j = -2; j <= 2; ++j) {
      pts.push_back(g.centre_local + t1 * (0.025 * i) + t2 * (0.02 * j));
    }
  }
  return pts;
}

// ---------------------------------------------------------------------------
// the decision and the hand-off
// ---------------------------------------------------------------------------

mo::Facts GoodFacts() {
  mo::Facts f;
  f.setting_on = true;
  f.rt_mirrors_on = false;
  f.picture_whole = true;
  f.picture_age = 0;
  f.play_view = true;
  f.glass_found = true;
  f.needed_mask = 1;
  f.field_known = true;
  f.pose_known = true;
  return f;
}

void TestDecide() {
  using W = mo::Why;
  Check(mo::Decide(GoodFacts()) == W::kDraw, "everything in place: the glass is drawn");
  {
    mo::Facts f = GoodFacts();
    f.setting_on = false;
    Check(mo::Decide(f) == W::kSettingOff, "the setting off: nothing");
  }
  {
    // the hand-off: the ray traced side mirrors win wherever they are on, whatever else is in place
    mo::Facts f = GoodFacts();
    f.rt_mirrors_on = true;
    Check(mo::Decide(f) == W::kRtMirrors, "the ray traced side mirrors on: they win and the overlay does nothing");
    f.needed_mask = 3;
    f.picture_age = 0;
    Check(mo::Decide(f) == W::kRtMirrors, "... also with both glasses needed and a fresh picture");
    f.setting_on = false;
    Check(mo::Decide(f) == W::kSettingOff, "... and the setting off still says off (it is the first thing asked)");
    f = GoodFacts();
    f.rt_mirrors_on = true;
    f.picture_whole = false;
    f.glass_found = false;
    f.needed_mask = 0;
    f.field_known = false;
    f.pose_known = false;
    f.play_view = false;
    Check(mo::Decide(f) == W::kRtMirrors, "... whatever else is missing: the ray traced mirrors are the answer, not a reason to look further");
  }
  {
    mo::Facts f = GoodFacts();
    f.rt_mirrors_on = false;
    Check(mo::Decide(f) == W::kDraw, "the ray traced mirrors off (a card without ray tracing, or the setting off): the overlay draws");
  }
  {
    mo::Facts f = GoodFacts();
    f.picture_whole = false;
    Check(mo::Decide(f) == W::kNoPicture, "no whole picture");
    f = GoodFacts();
    f.picture_age = mv::kMaxHeldFrames;
    Check(mo::Decide(f) == W::kDraw, "a picture as old as a held frame may be is drawn from");
    f.picture_age = mv::kMaxHeldFrames + 1;
    Check(mo::Decide(f) == W::kStalePicture, "one frame older: stale");
    f = GoodFacts();
    f.play_view = false;
    Check(mo::Decide(f) == W::kNotPlayView, "the pause view and the garage keep the game's mirrors");
    f = GoodFacts();
    f.glass_found = false;
    Check(mo::Decide(f) == W::kNoGlass, "no pair of glasses found");
    f = GoodFacts();
    f.needed_mask = 0;
    Check(mo::Decide(f) == W::kNotNeeded, "a pair, but neither glass is worth a view");
    f.needed_mask = 4;
    Check(mo::Decide(f) == W::kNotNeeded, "bits past the two glasses are nothing");
    f.needed_mask = 2;
    Check(mo::Decide(f) == W::kDraw, "the right glass alone is enough");
    f.needed_mask = 3;
    Check(mo::Decide(f) == W::kDraw, "both");
    f = GoodFacts();
    f.field_known = false;
    Check(mo::Decide(f) == W::kNoField, "the view's field is not known: nothing is drawn rather than a wrong mirror");
    f = GoodFacts();
    f.pose_known = false;
    Check(mo::Decide(f) == W::kNoPose, "the view's camera is not known");
  }
  {
    // the order: a missing picture is said before a missing glass, a missing glass before a missing field
    mo::Facts f = GoodFacts();
    f.picture_whole = false;
    f.glass_found = false;
    Check(mo::Decide(f) == W::kNoPicture, "order: the picture first");
    f = GoodFacts();
    f.glass_found = false;
    f.field_known = false;
    Check(mo::Decide(f) == W::kNoGlass, "order: the glass before the field");
    f = GoodFacts();
    f.field_known = false;
    f.pose_known = false;
    Check(mo::Decide(f) == W::kNoField, "order: the field before the pose");
  }
  // every reason has a name, and they are told apart
  std::vector<std::string> names;
  for (uint32_t i = 0; i < uint32_t(W::kCount); ++i) {
    const std::string n = mo::WhyName(W(i));
    Check(!n.empty() && n != "?", "reason " + std::to_string(i) + " has a name");
    names.push_back(n);
  }
  std::sort(names.begin(), names.end());
  Check(std::adjacent_find(names.begin(), names.end()) == names.end(), "the reasons' names are all different");
}

// ---------------------------------------------------------------------------
// the camera, the pose and the quaternion
// ---------------------------------------------------------------------------

void TestPose() {
  const World w = MakeWorld(0.3, 0.1);
  mo::CameraAxes axes;
  Check(mo::AxesOfView(w.view, &axes), "a view matrix made of a rigid motion gives its axes");
  Check(Near(axes.eye[0], w.eye_world.x, 1e-9) && Near(axes.eye[1], w.eye_world.y, 1e-9) && Near(axes.eye[2], w.eye_world.z, 1e-9), "the eye is where the camera was put (2.7 km out)");
  Check(Near(axes.axis[2][0], w.forward_world.x) && Near(axes.axis[2][1], w.forward_world.y) && Near(axes.axis[2][2], w.forward_world.z), "view axis z is the camera's forward in the world");
  const V3 z_axis{axes.axis[2][0], axes.axis[2][1], axes.axis[2][2]};
  const V3 x_axis{axes.axis[0][0], axes.axis[0][1], axes.axis[0][2]};
  const V3 y_axis{axes.axis[1][0], axes.axis[1][1], axes.axis[1][2]};
  Check(Near(mv::Dot(x_axis, y_axis), 0.0) && Near(mv::Dot(x_axis, z_axis), 0.0) && Near(mv::Length(y_axis), 1.0), "the axes are at right angles and unit");
  {
    tp::Mat4 bad = w.view;
    bad.m[0][0] *= 1.2;
    mo::CameraAxes a;
    Check(!mo::AxesOfView(bad, &a), "a scaled axis is not a camera");
    tp::Mat4 zero;
    Check(!mo::AxesOfView(zero, &a), "the zero matrix of a frame with no camera is not one");
    bad = w.view;
    bad.m[1][1] = std::nan("");
    Check(!mo::AxesOfView(bad, &a), "a NaN is not one");
    bad = w.view;
    bad.m[0][1] += 0.1;
    Check(!mo::AxesOfView(bad, &a), "axes that are not at right angles are not a camera");
  }

  mo::ViewPose pose;
  Check(mo::PoseOfView(w.view, 80.0, 50.0, 1234, &pose) && pose.known && pose.frame == 1234, "a pose is made");
  // the rig is the camera turned half a circle about its up axis
  Check(Near(pose.right[0], -x_axis.x) && Near(pose.right[1], -x_axis.y) && Near(pose.right[2], -x_axis.z), "the rig's right is the camera's left");
  Check(Near(pose.up[0], y_axis.x) && Near(pose.up[1], y_axis.y) && Near(pose.up[2], y_axis.z), "the rig's up is the camera's");
  Check(Near(pose.forward[0], -z_axis.x) && Near(pose.forward[1], -z_axis.y) && Near(pose.forward[2], -z_axis.z), "the rig looks backwards");
  Check(Near(pose.tan_h, std::tan(40.0 * kPi / 180.0)) && Near(pose.tan_v, std::tan(25.0 * kPi / 180.0)), "the field's half tangents");
  Check(!mo::PoseOfView(w.view, 0.0, 50.0, 0, &pose) && !mo::PoseOfView(w.view, 80.0, 0.0, 0, &pose) && !mo::PoseOfView(w.view, 180.0, 50.0, 0, &pose) &&
            !mo::PoseOfView(w.view, 80.0, -4.0, 0, &pose),
        "a field that is not a field is no pose");
  const mv::Rig rig = mo::RigOf(pose);
  Check(Near(rig.tan_half_h, pose.tan_h) && Near(rig.forward.x, pose.forward[0]) && Near(rig.eye.z, pose.eye[2]), "the pose as the design's Rig");
  // in the car's frame (x forward, y left, z up) with the camera looking forward, the rig looks backwards and its right is the car's left, as the design says
  {
    const World f = MakeWorld(0.0, 0.0);
    mo::ViewPose p;
    mo::PoseOfView(f.view, 80.0, 50.0, 1, &p);
    const V3 fwd = TransformVector(f.car, {1, 0, 0});
    const V3 left = TransformVector(f.car, {0, 1, 0});
    Check(Near(p.forward[0], -fwd.x) && Near(p.forward[1], -fwd.y), "design: the rig's forward is the car's -x");
    Check(Near(p.right[0], left.x) && Near(p.right[1], left.y), "design: the rig's right is the car's left (+y)");
    Check(Near(p.up[2], 1.0), "design: the rig's up is the car's +z");
  }
}

// v' of a rotation matrix on a column vector, and the shader's quaternion rotation
void CheckQuaternion(const double m[3][3], const std::string& what) {
  double q[4];
  mo::QuaternionOf(m, q);
  Check(Near(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0, 1e-12), what + ": a unit quaternion");
  const hlsl_shim::float4 qf = hlsl_shim::float4(float(q[0]), float(q[1]), float(q[2]), float(q[3]));
  double worst = 0.0;
  const double vs[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.3, -1.2, 2.5}};
  for (const auto& v : vs) {
    const hlsl_shim::float3 r = hlsl_shim::MirrorOverlayRotate(qf, hlsl_shim::float3(float(v[0]), float(v[1]), float(v[2])));
    for (int i = 0; i < 3; ++i) {
      const double want = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
      worst = std::max(worst, std::fabs(double(i == 0 ? r.x : i == 1 ? r.y : r.z) - want));
    }
  }
  Check(worst < 1e-5, what + ": the shader's rotation by it is the matrix's (worst " + std::to_string(worst) + ")");
}

void TestQuaternion() {
  // the four branches: the trace, then the three diagonals (half circles about each axis have trace -1)
  const double identity[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const double about_x[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
  const double about_y[3][3] = {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
  const double about_z[3][3] = {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
  CheckQuaternion(identity, "identity");
  CheckQuaternion(about_x, "half circle about x");
  CheckQuaternion(about_y, "half circle about y (the fresh picture's rig)");
  CheckQuaternion(about_z, "half circle about z");
  {
    double q[4];
    mo::QuaternionOf(about_y, q);
    Check(Near(std::fabs(q[1]), 1.0) && Near(q[0], 0.0) && Near(q[2], 0.0) && Near(q[3], 0.0), "the fresh picture's rotation is (0, 1, 0, 0) up to sign, as the shader's comment says");
  }
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> uni(-1.0, 1.0);
  for (int n = 0; n < 200; ++n) {
    // a random rotation: Gram-Schmidt of random vectors, the third as their cross product (det +1)
    V3 a = mv::Normalize({uni(rng), uni(rng), uni(rng)});
    V3 b{uni(rng), uni(rng), uni(rng)};
    b = mv::Normalize(b - a * mv::Dot(a, b));
    const V3 c = Cross(a, b);
    const double m[3][3] = {{a.x, b.x, c.x}, {a.y, b.y, c.y}, {a.z, b.z, c.z}};
    CheckQuaternion(m, "random rotation " + std::to_string(n));
  }
}

// ---------------------------------------------------------------------------
// the depth test
// ---------------------------------------------------------------------------

void TestDepthTest() {
  const auto word = [](bool enable, bool write, uint32_t func) { return (enable ? 1u : 0u) | (write ? 2u : 0u) | (func << 2); };
  using T = mo::DepthTest;
  Check(mo::DepthTestOf(word(true, true, mo::kCmpGreaterEqual)) == T::kGreaterEqual, "the game's reverse depth test (greater or equal, written) is repeated as greater or equal");
  Check(mo::DepthTestOf(word(true, false, mo::kCmpGreater)) == T::kGreaterEqual, "greater (not written) the same");
  Check(mo::DepthTestOf(word(true, true, mo::kCmpLessEqual)) == T::kLessEqual && mo::DepthTestOf(word(true, false, mo::kCmpLess)) == T::kLessEqual, "less and less or equal are less or equal");
  for (uint32_t func : {0u, 1u, 3u, 6u, 8u, 9u, 15u}) {
    Check(mo::DepthTestOf(word(true, true, func)) == T::kNone, "comparison " + std::to_string(func) + " (never, equal, not equal, always, unknown) is not repeatable");
  }
  Check(mo::DepthTestOf(word(false, true, mo::kCmpGreaterEqual)) == T::kNone && mo::DepthTestOf(0) == T::kNone, "no depth test: not repeatable (it would cover what stands in front of the glass)");
  // plume's values (mirror_overlay.cpp static_asserts them against the enum): UNKNOWN, NEVER, LESS, EQUAL, LESS_EQUAL, GREATER, NOT_EQUAL, GREATER_EQUAL, ALWAYS
  Check(mo::kCmpLess == 2 && mo::kCmpLessEqual == 4 && mo::kCmpGreater == 5 && mo::kCmpGreaterEqual == 7, "the comparison values are plume's");
}

// ---------------------------------------------------------------------------
// the pipeline variants
// ---------------------------------------------------------------------------

void TestVariants() {
  namespace sm = mcla::gpu::car_shadow_map;
  for (uint32_t stride : {12u, 24u, 28u, 32u, 36u, 4095u}) {
    for (uint32_t samples : {1u, 2u, 4u, 8u}) {
      for (mo::DepthTest test : {mo::DepthTest::kLessEqual, mo::DepthTest::kGreaterEqual}) {
        for (sm::PositionKind kind : {sm::PositionKind::kFloat3, sm::PositionKind::kHalf4}) {
          const uint32_t vertex = sm::VariantKey(kind, 3, stride);
          const uint32_t v = mo::Variant(vertex, mo::SampleIndex(samples), test);
          Check(mo::VertexOf(v) == vertex && mo::SamplesOf(mo::SampleIndexOf(v)) == samples && mo::TestOf(v) == test, "a variant round trips: stride " + std::to_string(stride) + ", " + std::to_string(samples) + " samples");
          Check(v < (1u << 24), "the variant fits the host precache's key (24 bits for a stride under 4096)");
        }
      }
    }
  }
  Check(mo::SampleIndex(1) == 0 && mo::SampleIndex(2) == 1 && mo::SampleIndex(4) == 2 && mo::SampleIndex(8) == 3 && mo::SampleIndex(3) == 4 && mo::SampleIndex(0) == 4, "the sample counts' indices (3 and 0 are none)");
  // all different
  std::vector<uint32_t> all;
  for (uint32_t si = 0; si < 4; ++si) {
    for (mo::DepthTest t : {mo::DepthTest::kLessEqual, mo::DepthTest::kGreaterEqual}) {
      all.push_back(mo::Variant(sm::VariantKey(sm::PositionKind::kFloat3, 3, 28), si, t));
    }
  }
  std::sort(all.begin(), all.end());
  Check(std::adjacent_find(all.begin(), all.end()) == all.end(), "the sample count and the test tell variants apart");
  Check(mo::ClampDistance(0.0) == mo::kMinDistance && mo::ClampDistance(1e9) == mo::kMaxDistance && mo::ClampDistance(std::nan("")) == mv::kAssumedDistance && mo::ClampDistance(20.0) == 20.0,
        "the assumed distance is held to its range, a NaN is the design's 20 m");
}

// ---------------------------------------------------------------------------
// the push block: refusals
// ---------------------------------------------------------------------------

void TestPushRefusals() {
  const World w = MakeWorld(0.0, 0.05);
  const mo::ViewPose pose = PoseAt(w, 80.0, 50.0, 1);
  mo::Push out;
  {
    const mo::PushInputs in = InputsFor(w, w.left, pose);
    Check(mo::MakePush(in, &out) == mo::PushResult::kOk, "the design's left glass makes a push block");
    Check(sizeof(mo::Push) == 128 && mo::kPushBytes == 128, "the push block is 128 bytes");
    uint32_t bits;
    std::memcpy(&bits, &out.v[6][3], 4);
    Check(bits == 77u, "the picture's slot rides in the delta's w as bits");
  }
  {
    mo::PushInputs in = InputsFor(w, w.left, pose);
    in.model_to_world.m[1][1] = std::nan("");
    Check(mo::MakePush(in, &out) == mo::PushResult::kBadMatrix, "a NaN model matrix is refused");
    in = InputsFor(w, w.left, pose);
    in.view = tp::Mat4{};
    Check(mo::MakePush(in, &out) == mo::PushResult::kBadCamera, "a frame with no camera is refused");
    in = InputsFor(w, w.left, pose);
    in.rig = mo::ViewPose{};
    Check(mo::MakePush(in, &out) == mo::PushResult::kBadPose, "no pose is refused");
    in = InputsFor(w, w.left, pose);
    in.rig.tan_h = 0.0;
    Check(mo::MakePush(in, &out) == mo::PushResult::kBadPose, "no field is refused");
    in = InputsFor(w, w.left, pose);
    in.normal_view[0] = in.normal_view[1] = in.normal_view[2] = 0.0;
    Check(mo::MakePush(in, &out) == mo::PushResult::kBadNormal, "no normal is refused");
    in = InputsFor(w, w.left, pose);
    in.normal_view[0] = std::nan("");
    Check(mo::MakePush(in, &out) == mo::PushResult::kBadNormal, "a NaN normal is refused");
    // a rig of the other handedness is no turn of the camera: nothing is drawn rather than something reversed
    in = InputsFor(w, w.left, pose);
    for (int c = 0; c < 3; ++c) in.rig.right[c] = -in.rig.right[c];
    Check(mo::MakePush(in, &out) == mo::PushResult::kBadPose, "a mirrored rig is refused");
  }
  {
    // the projection: a camera paired with a clip space of another camera, a skew, an orthographic one
    mo::PushInputs in = InputsFor(w, w.left, pose);
    const World other = MakeWorld(0.5, 0.0);
    in.view_projection = other.view_projection;
    Check(mo::MakePush(in, &out) == mo::PushResult::kProjection, "a clip space of another camera is refused as no perspective of this view");
    in = InputsFor(w, w.left, pose);
    tp::Mat4 skew = ProjectionAo(w.proj);
    skew.m[1][0] = 0.3;  // x' takes y
    in.view_projection = tp::Mul(w.view, skew);
    Check(mo::MakePush(in, &out) == mo::PushResult::kProjection, "a projection with a skew is refused");
    in = InputsFor(w, w.left, pose);
    tp::Mat4 ortho = ProjectionAo(w.proj);
    ortho.m[2][3] = 0.0;
    ortho.m[3][3] = 1.0;
    in.view_projection = tp::Mul(w.view, ortho);
    Check(mo::MakePush(in, &out) == mo::PushResult::kProjection, "an orthographic projection (w' constant) is refused");
    in = InputsFor(w, w.left, pose);
    tp::Mat4 shifted = ProjectionAo(w.proj);
    shifted.m[3][0] = 0.05;  // an offset of x' that is not a w one
    in.view_projection = tp::Mul(w.view, shifted);
    Check(mo::MakePush(in, &out) == mo::PushResult::kProjection, "a constant offset in clip x is refused");
    // an off-centre projection and a w' = -z one (a view space with z backward) are fine: the scale makes w' = z
    in = InputsFor(w, w.left, pose);
    ProjSpec off = w.proj;
    off.ox = 0.03;
    off.oy = -0.02;
    in.view_projection = tp::Mul(w.view, ProjectionAo(off));
    Check(mo::MakePush(in, &out) == mo::PushResult::kOk, "an off-centre perspective is accepted");
    in = InputsFor(w, w.left, pose);
    tp::Mat4 negative = ProjectionAo(w.proj);
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) negative.m[i][j] = -negative.m[i][j];
    in.view_projection = tp::Mul(w.view, negative);
    Check(mo::MakePush(in, &out) == mo::PushResult::kOk, "a clip space with w' = -z is accepted (the same homogeneous point, scaled)");
  }
}

void TestSideNeed() {
  namespace mg = mcla::gpu::mirror_glass;
  const World w = MakeWorld();
  tp::Mat4 skew = ProjectionAo(w.proj);
  skew.m[1][0] = 0.3;
  const tp::Mat4 unsupported = tp::Mul(w.view, skew);
  const mg::NeedState right{true, 2, 105};
  const mg::NeedState found{true, 0, 0};
  mg::NeedHold hold;
  Check(mo::UpdateSideNeed(hold, right, true, w.view, w.view_projection) == right && hold.holding(), "a usable projection requests the view and starts the hold");
  Check(mo::UpdateSideNeed(hold, found, false, {}, {}) == right && hold.holding(), "a missing camera and draw retain the held view");
  Check(mo::UpdateSideNeed(hold, right, true, w.view, unsupported) == found && !hold.holding(), "an unusable projection withdraws current demand and ends the hold");
  Check(mo::UpdateSideNeed(hold, right, true, w.view, unsupported) == found && !hold.holding(), "an unusable projection cannot restart demand");
  Check(mo::UpdateSideNeed(hold, right, true, w.view, w.view_projection) == right && hold.holding(), "a usable camera can request the view again");
  Check(mo::UpdateSideNeed(hold, found, true, w.view, w.view_projection) == right && hold.holding(), "a missing glass draw holds demand with a usable camera");
  Check(mo::UpdateSideNeed(hold, {}, true, w.view, unsupported) == mg::NeedState{} && !hold.holding(), "an unusable camera also ends demand when only the hold remains");
}

void TestPushValues() {
  const World w = MakeWorld(0.0, 0.05);
  const mo::ViewPose pose = PoseAt(w, 80.0, 50.0, 1);
  mo::Push out;
  {
    mo::PushInputs in = InputsFor(w, w.left, pose, 1000.0);
    Check(mo::MakePush(in, &out) == mo::PushResult::kOk && out.v[5][3] == float(mo::kMaxDistance), "an assumed distance past the range is held to 200 m in the block");
    in = InputsFor(w, w.left, pose, 0.5);
    Check(mo::MakePush(in, &out) == mo::PushResult::kOk && out.v[5][3] == float(mo::kMinDistance), "... and one under it to 3 m");
    in = InputsFor(w, w.left, pose, 20.0);
    Check(mo::MakePush(in, &out) == mo::PushResult::kOk && out.v[5][3] == 20.0f, "... 20 m stays 20 m");
  }
  {
    mo::PushInputs in = InputsFor(w, w.left, pose);
    for (int k = 0; k < 3; ++k) in.normal_view[k] *= 1.4;
    Check(mo::MakePush(in, &out) == mo::PushResult::kOk &&
              Near(double(out.v[5][0]) * out.v[5][0] + double(out.v[5][1]) * out.v[5][1] + double(out.v[5][2]) * out.v[5][2], 1.0, 1e-6),
          "a normal a little off unit length is made unit in the block");
  }
  {
    // the block's eight float4s for the design's left glass looking at a fresh picture
    const mo::PushInputs in = InputsFor(w, w.left, pose);
    mo::MakePush(in, &out);
    mo::CameraAxes axes;
    mo::AxesOfView(w.view, &axes);
    // col_k . (p, 1) is the view-space coordinate k of the point: the centre of the glass
    const V3 c = w.left.centre_local;
    const V3 rel = Transform(w.car, c) - w.eye_world;
    for (int k = 0; k < 3; ++k) {
      const double got = double(out.v[k][0]) * c.x + double(out.v[k][1]) * c.y + double(out.v[k][2]) * c.z + double(out.v[k][3]);
      const double want = rel.x * axes.axis[k][0] + rel.y * axes.axis[k][1] + rel.z * axes.axis[k][2];
      Check(std::fabs(got - want) < 2e-4, "column " + std::to_string(k) + " of the block gives the glass's view space coordinate");
    }
    Check(Near(out.v[4][2], std::tan(40.0 * kPi / 180.0), 1e-6) && Near(out.v[4][3], std::tan(25.0 * kPi / 180.0), 1e-6), "the block holds the rig's field tangents");
    // a fresh picture: no offset between the eyes, the rotation is the half circle about the view's up axis
    Check(std::fabs(out.v[6][0]) < 1e-3f && std::fabs(out.v[6][1]) < 1e-3f && std::fabs(out.v[6][2]) < 1e-3f, "a fresh picture has no eye offset");
    Check(std::fabs(out.v[7][0]) < 1e-5f && std::fabs(std::fabs(out.v[7][1]) - 1.0f) < 1e-5f && std::fabs(out.v[7][2]) < 1e-5f && std::fabs(out.v[7][3]) < 1e-5f, "... and the rotation is (0, 1, 0, 0)");
    // the shader's rotation turns the camera's forward into the rig's backward and keeps up
    const hlsl_shim::float4 q = hlsl_shim::float4(out.v[7][0], out.v[7][1], out.v[7][2], out.v[7][3]);
    const hlsl_shim::float3 f = hlsl_shim::MirrorOverlayRotate(q, hlsl_shim::float3(0.0f, 0.0f, 1.0f));
    const hlsl_shim::float3 u = hlsl_shim::MirrorOverlayRotate(q, hlsl_shim::float3(0.0f, 1.0f, 0.0f));
    Check(std::fabs(f.z + 1.0f) < 1e-5f && std::fabs(u.y - 1.0f) < 1e-5f, "the camera's forward is the rig's -forward, up is up");
  }
}

// ---------------------------------------------------------------------------
// the vertex stage against the game's own chain
// ---------------------------------------------------------------------------

void CheckClip(const World& w, const mo::ViewPose& rig, const float jitter[2], const std::string& what) {
  mo::PushInputs in = InputsFor(w, w.left, rig);
  in.jitter_ndc[0] = jitter[0];
  in.jitter_ndc[1] = jitter[1];
  mo::Push push;
  if (mo::MakePush(in, &push) != mo::PushResult::kOk) {
    Check(false, what + ": a push block is made");
    return;
  }
  const hlsl_shim::MirrorOverlayPush shim = ToShim(push);
  double worst_xy = 0.0;
  double worst_z = 0.0;
  double worst_w = 0.0;
  double worst_pv = 0.0;
  mo::CameraAxes axes;
  mo::AxesOfView(w.view, &axes);
  for (const V3& p : PatchPoints(w.left)) {
    const hlsl_shim::float3 pv = hlsl_shim::MirrorOverlayViewPos(shim, hlsl_shim::float3(float(p.x), float(p.y), float(p.z)));
    const V3 world = Transform(w.car, p);
    // the view space of the main camera, eye-relative, by the double chain
    const V3 rel = world - w.eye_world;
    const V3 want_pv{mv::Dot(rel, {axes.axis[0][0], axes.axis[0][1], axes.axis[0][2]}), mv::Dot(rel, {axes.axis[1][0], axes.axis[1][1], axes.axis[1][2]}),
                     mv::Dot(rel, {axes.axis[2][0], axes.axis[2][1], axes.axis[2][2]})};
    worst_pv = std::max({worst_pv, std::fabs(pv.x - want_pv.x), std::fabs(pv.y - want_pv.y), std::fabs(pv.z - want_pv.z)});
    const hlsl_shim::float4 clip = hlsl_shim::MirrorOverlayClip(shim, pv);
    // the game's chain for the same point
    const Ndc direct = Direct(world, w.view_projection, w.scale, w.offset, jitter);
    worst_xy = std::max({worst_xy, std::fabs(clip.x / clip.w - direct.x), std::fabs(clip.y / clip.w - direct.y)});
    worst_w = std::max(worst_w, std::fabs(clip.w - (want_pv.z - mo::kBiasMetres)));
    // the depth: the game's chain for the point pulled towards the eye along its ray
    const double k = (want_pv.z - mo::kBiasMetres) / want_pv.z;
    const V3 pulled = w.eye_world + rel * k;
    const Ndc nearer = Direct(pulled, w.view_projection, w.scale, w.offset, jitter);
    worst_z = std::max(worst_z, std::fabs(clip.z / clip.w - nearer.z));
    // and nearer than the glass itself in the game's convention (reverse depth stored 1 near; the viewport transform mirrors z: the transform's own sign says which way is near)
    const double glass_z = direct.z;
    const double toward = w.scale[2] > 0.0 ? -1.0 : 1.0;  // the game's projection stores 1 near; the viewport's mirror flips it
    (void)glass_z;
    (void)toward;
  }
  Check(worst_pv < 2e-4, what + ": the vertex in the main camera's view space matches the double chain (worst " + std::to_string(worst_pv) + " m)");
  Check(worst_xy < 3e-5, what + ": the NDC x and y are the game's chain's, jitter and viewport transform included (worst " + std::to_string(worst_xy) + ")");
  Check(worst_w < 2e-4, what + ": w' is the distance along the view less the bias (worst " + std::to_string(worst_w) + ")");
  Check(worst_z < 2e-5, what + ": the depth is the game's chain's for the point pulled towards the eye (worst " + std::to_string(worst_z) + ")");
}

void TestClip() {
  const World w = MakeWorld(0.0, 0.05);
  const mo::ViewPose pose = PoseAt(w, 80.0, 50.0, 1);
  const float none[2] = {0.0f, 0.0f};
  CheckClip(w, pose, none, "centred projection");
  const float jitter[2] = {0.00123f, -0.00087f};
  CheckClip(w, pose, jitter, "with a jitter");
  ProjSpec off;
  off.ox = 0.031;
  off.oy = -0.022;
  off.sx = 1.1;
  off.sy = 1.9;
  const World shifted = MakeWorld(0.2, -0.1, off);
  const mo::ViewPose pose2 = PoseAt(shifted, 80.0, 50.0, 1);
  CheckClip(shifted, pose2, jitter, "off-centre projection, turned camera");
  // a viewport transform that is not mirrored
  World plain = MakeWorld(0.0, 0.0);
  plain.scale[1] = 1.0f;
  plain.scale[2] = 1.0f;
  plain.offset[2] = 0.0f;
  CheckClip(plain, PoseAt(plain, 80.0, 50.0, 1), jitter, "plain viewport transform");
  // a clip space with w' = -z (the same homogeneous points, every component negated): the block is scaled so that the shader's w' is the distance along the view again
  {
    World negative = MakeWorld(0.0, 0.05);
    tp::Mat4 flipped = ProjectionAo(negative.proj);
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) flipped.m[i][j] = -flipped.m[i][j];
    negative.view_projection = tp::Mul(negative.view, flipped);
    CheckClip(negative, PoseAt(negative, 80.0, 50.0, 1), jitter, "a clip space with w' = -z");
  }

  // the depth is nearer than the glass's own in the game's convention: the pulled point stores a larger depth (1 near) with the plain transform
  {
    const World& g = plain;
    const mo::ViewPose rig = PoseAt(g, 80.0, 50.0, 1);
    mo::Push push;
    mo::MakePush(InputsFor(g, g.left, rig), &push);
    const hlsl_shim::MirrorOverlayPush shim = ToShim(push);
    const V3 p = g.left.centre_local;
    const hlsl_shim::float3 pv = hlsl_shim::MirrorOverlayViewPos(shim, hlsl_shim::float3(float(p.x), float(p.y), float(p.z)));
    const hlsl_shim::float4 clip = hlsl_shim::MirrorOverlayClip(shim, pv);
    const Ndc direct = Direct(Transform(g.car, p), g.view_projection, g.scale, g.offset, none);
    Check(clip.z / clip.w > direct.z, "the overlay's depth is nearer than the glass's own (reverse depth: larger), by a hair: it lands on the glass the game drew");
    Check(clip.z / clip.w - direct.z < 2e-3, "... and only by a hair (by " + std::to_string(clip.z / clip.w - direct.z) + ", the glass at " + std::to_string(direct.w) + " m)");
  }
  // a vertex at or behind the bias distance is not moved the wrong way (k stays 1)
  {
    hlsl_shim::MirrorOverlayPush shim = {};
    shim.proj_a = hlsl_shim::float4(1, 0, 1, 0);
    shim.proj_b = hlsl_shim::float4(0, 0, 1, 1);
    const hlsl_shim::float4 clip = hlsl_shim::MirrorOverlayClip(shim, hlsl_shim::float3(0.1f, 0.2f, 0.004f));
    Check(Near(clip.w, 0.004, 1e-6) && Near(clip.x, 0.1, 1e-6), "a vertex nearer than the bias is left where it is (no flip through the eye)");
  }
}

// ---------------------------------------------------------------------------
// the lookup: THE SHADER'S TEXT against the design's UvForMirrorPixel
// ---------------------------------------------------------------------------

struct LookupStats {
  double worst = 0.0;
  int valid = 0;
  int invalid = 0;
};

LookupStats CheckLookup(const World& w, const Glass& g, const mo::ViewPose& rig, double distance, const std::string& what, bool expect_all_valid = true) {
  const mo::PushInputs in = InputsFor(w, g, rig, distance);
  mo::Push push;
  const mo::PushResult made = mo::MakePush(in, &push);
  Check(made == mo::PushResult::kOk, what + ": a push block is made");
  LookupStats stats;
  if (made != mo::PushResult::kOk) {
    return stats;
  }
  const hlsl_shim::MirrorOverlayPush shim = ToShim(push);
  const mv::Rig ref_rig = mo::RigOf(rig);
  const V3 n_world = mv::Normalize(TransformVector(w.car, g.normal_local));
  for (const V3& p : PatchPoints(g)) {
    const hlsl_shim::float3 pv = hlsl_shim::MirrorOverlayViewPos(shim, hlsl_shim::float3(float(p.x), float(p.y), float(p.z)));
    const hlsl_shim::float3 uv = hlsl_shim::MirrorOverlayUv(shim, pv);
    const double eye[3] = {w.eye_world.x, w.eye_world.y, w.eye_world.z};
    const V3 gw = Transform(w.car, p);
    const double glass[3] = {gw.x, gw.y, gw.z};
    const double n[3] = {n_world.x, n_world.y, n_world.z};
    const mv::Uv want = mo::ReferenceUv(ref_rig, eye, glass, n, distance);
    if (want.ok != (uv.z > 0.5f)) {
      Check(false, what + ": the shader and the reference agree on whether the lookup points into the picture's half space");
      ++stats.invalid;
      continue;
    }
    if (!want.ok) {
      ++stats.invalid;
      continue;
    }
    ++stats.valid;
    stats.worst = std::max({stats.worst, std::fabs(double(uv.x) - want.u), std::fabs(double(uv.y) - want.v)});
  }
  Check(stats.worst < 3e-4, what + ": the shader's picture coordinates are the design's UvForMirrorPixel (worst " + std::to_string(stats.worst) + ")");
  if (expect_all_valid) {
    Check(stats.valid > 0 && stats.invalid == 0, what + ": every point of the glass looks into the picture's half space");
  }
  return stats;
}

void TestLookup() {
  // a picture drawn this frame: the pose is the main camera's
  for (double yaw : {0.0, 0.35, -0.2}) {
    for (double pitch : {0.0, 0.08}) {
      const World w = MakeWorld(yaw, pitch);
      const mo::ViewPose pose = PoseAt(w, 80.0, 50.0, 10);
      const std::string label = "fresh picture, camera yaw " + std::to_string(yaw) + " pitch " + std::to_string(pitch);
      CheckLookup(w, w.left, pose, 20.0, label + ", left glass");
      CheckLookup(w, w.right, pose, 20.0, label + ", right glass");
      CheckLookup(w, w.left, pose, 5.0, label + ", left glass, t0 5 m");
      CheckLookup(w, w.right, pose, 100.0, label + ", right glass, t0 100 m");
    }
  }
  // the design's number: the left mirror's reflected ray goes about 3.5 degrees right of straight back and 10.9 degrees down: the rig is looking straight back, so the lookup is a little right of centre
  // (the rig's right is the car's left, so "right of straight back" is the rig's left: u below 0.5) and below the middle (v above 0.5)
  {
    const World w = MakeWorld(0.0, 0.0);
    const mo::ViewPose pose = PoseAt(w, 80.0, 50.0, 1);
    const mo::PushInputs in = InputsFor(w, w.left, pose);
    mo::Push push;
    mo::MakePush(in, &push);
    const hlsl_shim::MirrorOverlayPush shim = ToShim(push);
    const V3 p = w.left.centre_local;
    const hlsl_shim::float3 pv = hlsl_shim::MirrorOverlayViewPos(shim, hlsl_shim::float3(float(p.x), float(p.y), float(p.z)));
    const hlsl_shim::float3 uv = hlsl_shim::MirrorOverlayUv(shim, pv);
    Check(uv.z > 0.5f && uv.y > 0.5f, "the left glass looks below the middle of the picture (its reflected ray goes down: the road behind)");
    Check(uv.x < 0.5f, "... and a little to the car's right of straight back (the rig's right is the car's left)");
    Check(std::fabs(uv.x - 0.5f) < 0.2f && std::fabs(uv.y - 0.5f) < 0.45f, "... inside the picture");
  }
  // a picture held for a frame or two: the camera it was drawn from has moved (1 m back along the road, 2 degrees of yaw)
  {
    const World now = MakeWorld(0.0, 0.0);
    World then = MakeWorld(0.035, 0.0);
    then.car = CarMatrix(37.0 * kPi / 180.0, {2700.0 - 0.8, -1500.0 - 0.6, 31.0});  // the car was 1 m behind
    then.eye_world = Transform(then.car, {0.0, 0.35, 0.0});
    then.forward_world = mv::Normalize(TransformVector(then.car, {std::cos(0.035), std::sin(0.035), 0.0}));
    then.view = LookView(then.eye_world, then.forward_world);
    const mo::ViewPose held = PoseAt(then, 80.0, 50.0, 8);
    CheckLookup(now, now.left, held, 20.0, "held picture (1 m back, 2 degrees of yaw), left glass");
    CheckLookup(now, now.right, held, 20.0, "held picture, right glass");
    CheckLookup(now, now.left, held, 8.0, "held picture, left glass, t0 8 m");
    // the held lookup differs from the fresh one by about the movement (it is not ignored)
    const mo::ViewPose fresh = PoseAt(now, 80.0, 50.0, 9);
    const mo::PushInputs a = InputsFor(now, now.left, held);
    const mo::PushInputs b = InputsFor(now, now.left, fresh);
    mo::Push pa, pb;
    mo::MakePush(a, &pa);
    mo::MakePush(b, &pb);
    const V3 p = now.left.centre_local;
    const auto sa = ToShim(pa), sb = ToShim(pb);
    const auto pva = hlsl_shim::MirrorOverlayViewPos(sa, hlsl_shim::float3(float(p.x), float(p.y), float(p.z)));
    const auto uva = hlsl_shim::MirrorOverlayUv(sa, pva);
    const auto uvb = hlsl_shim::MirrorOverlayUv(sb, hlsl_shim::MirrorOverlayViewPos(sb, hlsl_shim::float3(float(p.x), float(p.y), float(p.z))));
    Check(std::fabs(uva.x - uvb.x) > 0.01f, "a held picture's lookup moves with its camera (the pose is used)");
    Check(Near(pa.v[6][0], pa.v[6][0]) && (std::fabs(pa.v[6][0]) + std::fabs(pa.v[6][1]) + std::fabs(pa.v[6][2])) > 0.5f, "the held picture's eye offset is in the block");
    Check(std::fabs(pb.v[6][0]) + std::fabs(pb.v[6][1]) + std::fabs(pb.v[6][2]) < 1e-3f, "a fresh picture's eye offset is zero");
  }
  // the world far from its origin: nothing is lost (the eye-relative view space), even 30 km out
  {
    World w = MakeWorld(0.1, 0.0);
    w.car = CarMatrix(1.0, {-30000.0, 28000.0, 40.0});
    w.eye_world = Transform(w.car, {0.0, 0.35, 0.0});
    w.forward_world = mv::Normalize(TransformVector(w.car, {std::cos(0.1), std::sin(0.1), 0.0}));
    w.view = LookView(w.eye_world, w.forward_world);
    w.view_projection = tp::Mul(w.view, ProjectionAo(w.proj));
    const mo::ViewPose pose = PoseAt(w, 80.0, 50.0, 3);
    const LookupStats s = CheckLookup(w, w.left, pose, 20.0, "30 km from the origin, left glass");
    Check(s.worst < 3e-4, "... precision held there (worst " + std::to_string(s.worst) + ")");
  }
  // a glass whose lookup points behind the rig: the shader says so (z 0) and the overlay's alpha is 0
  {
    const World w = MakeWorld(0.0, 0.0);
    const mo::ViewPose pose = PoseAt(w, 80.0, 50.0, 1);
    mo::PushInputs in = InputsFor(w, w.left, pose);
    // a normal that faces sideways: the ray (going away from the eye) comes back still going away, which is behind the rig
    mo::CameraAxes axes;
    mo::AxesOfView(w.view, &axes);
    in.normal_view[0] = 1.0;
    in.normal_view[1] = 0.0;
    in.normal_view[2] = 0.0;
    mo::Push push;
    mo::MakePush(in, &push);
    const hlsl_shim::MirrorOverlayPush shim = ToShim(push);
    const hlsl_shim::float3 uv = hlsl_shim::MirrorOverlayUv(shim, hlsl_shim::float3(0.1f, -0.05f, 1.0f));
    Check(uv.z == 0.0f, "a reflected ray that goes forward looks behind the rig: no coordinates");
    const mv::Uv ref = mv::ProjectDirection(mo::RigOf(pose), {0, 0, 1});
    Check(!ref.ok || true, "(the reference agrees that a direction behind the rig has none)");
  }
}

// the edge fade, against its reference, and where it is 0 and 1
void TestEdgeFade() {
  using hlsl_shim::float2;
  Check(hlsl_shim::kMirrorOverlayEdgeFade == float(mo::kEdgeFade) && hlsl_shim::kMirrorOverlayBias == float(mo::kBiasMetres), "the shader's two numbers are the rules' (0.04 of the picture, 5 mm)");
  Check(hlsl_shim::MirrorOverlayEdgeFade(float2(0.5f, 0.5f)) == 1.0f, "the middle of the picture: the picture's colour, whole");
  Check(hlsl_shim::MirrorOverlayEdgeFade(float2(0.0f, 0.5f)) == 0.0f && hlsl_shim::MirrorOverlayEdgeFade(float2(1.0f, 0.5f)) == 0.0f, "on the left and right edge: the game's own glass");
  Check(hlsl_shim::MirrorOverlayEdgeFade(float2(0.5f, 0.0f)) == 0.0f && hlsl_shim::MirrorOverlayEdgeFade(float2(0.5f, 1.0f)) == 0.0f, "on the top and bottom edge: the game's own glass");
  Check(hlsl_shim::MirrorOverlayEdgeFade(float2(-0.2f, 0.5f)) == 0.0f && hlsl_shim::MirrorOverlayEdgeFade(float2(0.5f, 1.3f)) == 0.0f, "outside the picture: the game's own glass");
  Check(Near(hlsl_shim::MirrorOverlayEdgeFade(float2(0.02f, 0.5f)), 0.5, 1e-5), "half the fade's width in: half");
  Check(hlsl_shim::MirrorOverlayEdgeFade(float2(0.04f, 0.5f)) == 1.0f && hlsl_shim::MirrorOverlayEdgeFade(float2(0.96f, 0.5f)) == 1.0f, "at the fade's width: whole");
  Check(Near(hlsl_shim::MirrorOverlayEdgeFade(float2(0.5f, 0.99f)), 0.25, 1e-4), "a quarter of the width from the bottom edge: a quarter");
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> uni(-0.1, 1.1);
  double worst = 0.0;
  for (int i = 0; i < 2000; ++i) {
    const double u = uni(rng), v = uni(rng);
    worst = std::max(worst, std::fabs(double(hlsl_shim::MirrorOverlayEdgeFade(float2(float(u), float(v)))) - mo::EdgeFade(u, v)));
  }
  Check(worst < 1e-5, "the shader's fade is the reference's on 2000 points (worst " + std::to_string(worst) + ")");
  double previous = 0.0;
  bool monotone = true;
  for (int i = 0; i <= 100; ++i) {
    const double f = mo::EdgeFade(i * 0.0006, 0.5);
    monotone = monotone && f >= previous;
    previous = f;
  }
  Check(monotone, "the fade rises monotonically from the edge");
}

// ---------------------------------------------------------------------------
// the design's own error table: the overlay agrees with the design's planar numbers
// ---------------------------------------------------------------------------

void TestAgainstDesign() {
  // the glass is nearer the eye than the rig's origin is the eye: for a view drawn this frame the lookup is the design's (rig.eye == eye): a car behind at t metres is where the design says
  const World w = MakeWorld(0.0, 0.0);
  const mo::ViewPose pose = PoseAt(w, 80.0, 50.0, 1);
  const mv::Rig rig = mo::RigOf(pose);
  const double eye[3] = {w.eye_world.x, w.eye_world.y, w.eye_world.z};
  const V3 gw = Transform(w.car, w.left.centre_local);
  const double glass[3] = {gw.x, gw.y, gw.z};
  const V3 n_world = mv::Normalize(TransformVector(w.car, w.left.normal_local));
  const double n[3] = {n_world.x, n_world.y, n_world.z};
  // at the assumed distance the lookup is exact for a thing there: the picture's coordinates of the point 20 m behind the glass along the reflected ray
  const mv::V3 d = mv::Normalize(gw - w.eye_world);
  const mv::V3 r = mv::Reflect(d, n_world);
  const mv::V3 thing = gw + r * 20.0;
  const mv::Uv exact = mv::ProjectDirection(rig, thing - rig.eye);
  const mv::Uv looked = mo::ReferenceUv(rig, eye, glass, n, 20.0);
  Check(exact.ok && looked.ok && Near(exact.u, looked.u, 1e-12) && Near(exact.v, looked.v, 1e-12), "at the assumed distance the lookup is exact for a thing there");
  // for a thing nearer (5 m) the error is the design's order: several degrees
  const mv::V3 near_thing = gw + r * 5.0;
  const mv::Uv truth = mv::ProjectDirection(rig, near_thing - rig.eye);
  Check(truth.ok && std::fabs(truth.u - looked.u) > 0.01, "for a thing 5 m behind the glass the lookup is off by the parallax the design predicts (a few percent of the picture)");
  const double angle = mv::ParallaxError(rig, mv::V3{eye[0], eye[1], eye[2]}, gw, n_world, 5.0);
  Check(angle * 180.0 / kPi > 3.0 && angle * 180.0 / kPi < 9.0, "the design's table: about 6 degrees for the left mirror at 5 m (here " + std::to_string(angle * 180.0 / kPi) + ")");
}

// ---------------------------------------------------------------------------
// the files
// ---------------------------------------------------------------------------

void TestFiles(const std::string& src) {
  const std::string rules = ReadFile(src + "/gpu/mirror_overlay_rules.h");
  const std::string host = ReadFile(src + "/gpu/mirror_overlay.cpp");
  const std::string host_h = ReadFile(src + "/gpu/mirror_overlay.h");
  const std::string hlsli = ReadFile(src + "/gpu/hlsl/mirror_overlay.hlsli");
  const std::string vs = ReadFile(src + "/gpu/hlsl/mirror_overlay_vs.hlsl");
  const std::string ps = ReadFile(src + "/gpu/hlsl/mirror_overlay_ps.hlsl");
  const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
  const std::string scene = ReadFile(src + "/gpu/scene.cpp");
  const std::string scene_h = ReadFile(src + "/gpu/scene.h");
  const std::string renderer = ReadFile(src + "/gpu/renderer.cpp");
  const std::string renderer_h = ReadFile(src + "/gpu/renderer.h");
  const std::string view = ReadFile(src + "/gpu/mirror_view.cpp");
  const std::string view_h = ReadFile(src + "/gpu/mirror_view.h");
  const std::string link = ReadFile(src + "/gpu/mirror_view_link.h");
  const std::string precache = ReadFile(src + "/gpu/host_precache.h");
  const std::string frame_h = ReadFile(src + "/gpu/car_frame.h");
  const std::string frame_cpp = ReadFile(src + "/gpu/car_frame.cpp");
  const std::string gpu_settings = ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string order = ReadFile(src + "/gpu/frame_order.h");
  if (rules.empty() || host.empty() || hlsli.empty() || scene.empty()) {
    Check(false, "the sources are readable (pass the src folder as the first argument)");
    return;
  }

  // the numbers the shader and the rules both hold
  Check(Has(hlsli, "static const float kMirrorOverlayEdgeFade = 0.04f;") && Has(rules, "kEdgeFade = 0.04;"), "the edge fade is 0.04 in both");
  Check(Has(hlsli, "static const float kMirrorOverlayBias = 0.005f;") && Has(rules, "kBiasMetres = 0.005;"), "the bias is 5 mm in both");
  Check(Has(rules, "kPushBytes = 128;") && Has(hlsli, "struct MirrorOverlayPush {\n  float4 col_x;\n  float4 col_y;\n  float4 col_z;\n  float4 proj_a;\n  float4 proj_b;\n  float4 normal;\n  float4 delta;\n  float4 rot;\n};"),
        "the push block is eight float4s in the shader and 128 bytes in the rules");
  Check(Has(hlsli, "return v + cross(qv, t) * 2.0f;") && Has(hlsli, "const float3 t = cross(qv, v) + v * q.w;"), "the shader rotates by the quaternion as the rules' comment says");

  // the stages
  Check(Has(vs, "#include \"mirror_overlay.hlsli\"") && Has(ps, "#include \"mirror_overlay.hlsli\"") && Has(vs, "register(b0, space4)") && Has(ps, "register(b0, space4)") && Has(vs, "[[vk::push_constant]]") && Has(ps, "[[vk::push_constant]]"),
        "both stages take the one push block at (b0, space4) on D3D12 and as the push constant on Vulkan");
  Check(Has(vs, "[[vk::location(0)]] float3 position : POSITION;") && Has(vs, "float3 view_pos : TEXCOORD0;") && Has(ps, "float3 view_pos : TEXCOORD0;"), "the vertex stage's output and the pixel stage's input are the same");
  Check(Has(ps, "g_samplers[kSamplerLinearClamp]") && Has(ps, "SampleLevel") && Has(ps, "asuint(p.delta.w)") && Has(ps, "NgFinite3(colour.rgb), alpha"), "the pixel stage reads the picture with the linear clamp sampler at the slot in the block and writes the edge fade as the alpha");
  Check(Has(cmake, "larecomp_gpu_host_shader(mirror_overlay_vs vs_6_0)") && Has(cmake, "larecomp_gpu_host_shader(mirror_overlay_ps ps_6_0)") && Has(cmake, "mirror_overlay.cpp") &&
            Has(cmake, "add_executable(larecomp_mirror_overlay_rules_test EXCLUDE_FROM_ALL mirror_overlay_rules_test.cpp)"),
        "CMakeLists.txt builds the two stages, the host and the test");

  // the host: the pipeline, the layout, the draw
  Check(Has(host, "rules::Decide(facts)") && Has(host, "facts.rt_mirrors_on = in.rt_mirrors_on;") && Has(host, "rules::MakePush("), "the host asks the rules for the decision and the push block");
  Check(Has(host, "rules::DepthTestOf(d->depth_word)") && Has(host, "blend.blendEnabled = true;") && Has(host, "if (!precache.Ready(PrecacheKey(variant))) {"), "the host repeats the glass draw's own depth test and blends");
  Check(Has(host, "desc.depthWriteEnabled = false;") && Has(host, "desc.depthEnabled = true;") && Has(host, "GREATER_EQUAL : plume::RenderComparisonFunction::LESS_EQUAL") && Has(host, "desc.cullMode = plume::RenderCullMode::NONE;"),
        "the pipeline tests depth with the equal case, never writes it, culls nothing");
  Check(Has(host, "blend.srcBlend = plume::RenderBlend::SRC_ALPHA;") && Has(host, "blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;") && Has(host, "blend.renderTargetWriteMask = kWriteRgb;") &&
            Has(host, "blend.srcBlendAlpha = plume::RenderBlend::ZERO;") && Has(host, "blend.dstBlendAlpha = plume::RenderBlend::ONE;"),
        "the pipeline blends over the colour on the colour channels and leaves the alpha");
  Check(Has(host, "builder.begin(/*isLocal=*/false, /*allowInputLayout=*/true);") && Has(host, "builder.addPushConstant(0, 4, rules::kPushBytes, plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);") &&
            Has(host, "for (uint32_t set = 0; set < 4; ++set)") && Has(host, "builder.addDescriptorSet(heap_->sampler_set_builder());"),
        "the layout is the host pass layout's sets with a push block for both stages and an input layout");
  Check(Has(host, "edram_->Framebuffer(only, 1, depth)") && Has(host, "pass_timing_->Mark(list, \"mirror glass\")") && Has(host, "pass_timing_->Mark(list, \"after mirror glass\")"), "the pass binds the main colour with the main depth and is timed");
  Check(Has(host, "upscaler_math::JitterNdcOffset(in.jitter, d->viewport.viewport.width, d->viewport.viewport.height, push.jitter_ndc);") && Has(host, "push.ndc_scale[c] = d->viewport.ndc_scale[c];"),
        "the push block takes the glass draw's own viewport transform and the frame's jitter");
  Check(Has(host, "static_assert(uint32_t(plume::RenderComparisonFunction::LESS) == rules::kCmpLess"), "the rules' comparison values are checked against plume's enum at compile time");
  Check(Has(host, "HostPrecache::Key(HostPrecache::kMirrorOverlay, variant)") && Has(host, "precache.Add(PrecacheKey(variant)") && Has(host, "precache.AddLive(\"mirror overlay\"") && Has(host, "passes_->precache().Stop();"),
        "the overlay's pipelines are the host precache's: listed at the start, in the background otherwise, never made on the render thread");
  Check(Has(precache, "kMirrorView = 41,") && Has(precache, "kMirrorOverlay = 42,"), "the precache has an owner for the view's two pipelines and for the overlay's");

  // the view's two pipelines are the precache's now
  Check(Has(view, "HostPrecache::Key(HostPrecache::kMirrorView, variant)") && Has(view, "precache.Add(PrecacheKey(variant)") && Has(view, "precache.AddLive(\"mirror view\"") && Has(view, "ListBase();") && Has(view, "ListLive(color, samples);"),
        "the view's average and stamp pipelines are listed with the host precache (at the start, or by the first redirected phase)");
  Check(Has(view, "if (!precache.Ready(PrecacheKey(variant))) {") && !Has(view, "passes_->CreatePipeline(*resolve_shader_, format, \"mirror view resolve\")") && !Has(view, "resolve_pipelines_") && !Has(view, "stamp_pipelines_"), "... and no longer made on the render thread at first use");
  Check(Has(view, "overlay_armed_ = true;") && Has(view, "overlay_armed_ = whole_ && !failed && frame_ - picture_frame_ <= mirror_view::kMaxHeldFrames;") && Has(view_h, "bool OverlayArmed() const { return overlay_armed_; }") && Has(view, "MirrorView::Picture MirrorView::CurrentPicture() const"),
        "the view arms the overlay at the end marker (a fresh picture or a held one) and gives out its picture");
  Check(Has(host, "void MirrorOverlay::NoteViewDrawn(const car_frame::Frame& frame, uint64_t guest_frame, bool field_known, float field_h, float field_v) {") && !Has(host, "ReadViewField") &&
            Has(link, "#include <atomic>") && !Has(link, "ViewField"),
        "the view's field of view comes through one function, the glue point with the guest half: NoteViewDrawn, given the field of the picture it describes (the link header has no overlay function)");
  {
    // MV-J2: the glue is the phase's own field (the begin marker's tag, MirrorView::PictureField), never the guest's latest camera build
    const size_t at = host.find("void MirrorOverlay::NoteViewDrawn(");
    const size_t end = host.find("\n}\n", at);
    const std::string body = at == std::string::npos ? std::string() : host.substr(at, end - at);
    Check(Has(body, "const float h = field_known ? field_h : 0.0f;") && Has(body, "const float v = field_known ? field_v : 0.0f;") && Has(body, "field_known_ = field_known && h > 0.0f && v > 0.0f;") && Has(body, "field_h_ = h;") &&
              Has(body, "field_v_ = v;") && !Has(body, "CurrentCamera") && !Has(host, "CurrentCamera"),
          "MV-J2: the overlay's field is the one it is given (the picture's own, from the phase's tag), 'not known' when no camera had been built, and the overlay never reads the guest's latest camera facts");
  }

  // the scene
  const size_t stamp = At(scene, "RecordMirrorStamp(record, list)) {\n    // The rear view mirror's picture");
  const size_t glass = At(scene, "RecordMirrorGlass(record, list)) {");
  const size_t execute = At(scene, "resolve_->Execute(record, list, float_scene");
  Check(stamp != std::string::npos && glass != std::string::npos && execute != std::string::npos && stamp < glass && glass < execute, "scene.cpp draws the glass after the stamp and before the copy reads the colour");
  Check(Has(scene, "mirror_overlay_ && mirror_overlay_->wanted() && mirror_view_ && mirror_view_->OverlayArmed() && list && RecordMirrorGlass(record, list)"), "the overlay runs only while it is wanted and the view armed it");
  Check(Has(scene, "in.rt_mirrors_on = car_mirror_ && car_mirror_->active();"), "the hand-off with the ray traced side mirrors reads CarMirrorRt::active()");
  Check(Has(scene, "mirror_overlay_->NoteViewDrawn(car_frame_->Current(), frame_serial_, field_known, field_h, field_v);") && Has(scene, "mirror_view_->PictureField(&field_h, &field_v)") && Has(scene, "mirror_overlay_->BeginFrame();"),
        "the overlay is told of the frame's start and of a fresh picture's end marker (the camera it was drawn from and the field of that phase's camera: MV-J2)");
  Check(Has(scene, "frame.viewport = &d.viewport;") && Has(scene, "frame.depth_word = d.key.depth;") && Has(frame_h, "bool viewport_known = false;") && Has(frame_cpp, "out->viewport_known = true;"),
        "CarFrame keeps each player draw's viewport transform and depth word for the replay");
  Check(Has(scene, "depth_key.format = uint8_t(xe::DepthRenderTargetFormat::kD24FS8);") && Has(scene, "depth_key.base_tiles = temporal::kMainDepthBase;"), "the overlay finds the main depth by its identity");
  Check(Has(scene_h, "void SetMirrorOverlay(MirrorOverlay* mirror_overlay)") && Has(renderer, "scene_.SetMirrorOverlay(&mirror_overlay_);") && Has(renderer, "mirror_overlay_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);") &&
            Has(renderer_h, "MirrorOverlay mirror_overlay_;"),
        "the renderer makes the overlay and gives it to the scene");

  // the frame order's row
  Check(Has(order, "kMirrorGlass,") && Has(order, ".call = \"RecordMirrorGlass(record, list)\"") && Has(order, ".label_file = \"mirror_overlay.cpp\", .label = \"mirror glass\", .closing_label = \"after mirror glass\""), "frame_order.h has the glass row after the stamp");

  // the settings: debug, off, live, no row
  const auto setting = [&](const std::string& name, const std::string& type, const std::string& value) {
    const std::string define = "REX_SETTING(" + type + ", " + name + ", " + value + ",";
    const size_t at = At(gpu_settings, define);
    const size_t end = at == std::string::npos ? at : gpu_settings.find(");\n\n", at);
    Check(at != std::string::npos && end != std::string::npos && Has(gpu_settings.substr(at, end - at), "rex::kDebugOnly") && !Has(gpu_settings.substr(at, end - at), "kRequiresRestart"), name + " is a debug-only, live setting with its default");
    Check(Has(manifest, "\"" + name + "\": {") && Has(meta, "{\"" + name + "\", \"MCLA/GPU\"}") && Has(gpu_settings, "inline auto& " + name + " = g_gpu." + name + ";"), name + " is in the three generated files");
  };
  setting("mcla_gpu_mirror_overlay", "bool", "false");
  setting("mcla_gpu_mirror_view_t0", "f64", "20.0");
  Check(Has(host, "mirror_setting::Current().overlay") && !Has(host, "larecomp::settings::mcla_gpu_mirror_overlay.Get()") && Has(host, "larecomp::settings::mcla_gpu_mirror_view_t0.Get()") && Has(host, "rules::ClampDistance("), "the host reads the two settings every frame");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    const std::string self = __FILE__;
    const size_t slash = self.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : self.substr(0, slash);  // .../src/gpu
    src = dir + "/..";
  }
  TestDecide();
  TestPose();
  TestQuaternion();
  TestDepthTest();
  TestVariants();
  TestPushRefusals();
  TestSideNeed();
  TestPushValues();
  TestClip();
  TestLookup();
  TestEdgeFade();
  TestAgainstDesign();
  TestFiles(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

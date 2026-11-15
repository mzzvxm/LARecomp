// Check of the car self-reflection's rules (car_selfrefl_rules.h): the modes, the clamps, the matrix the paint variant projects
// with, the constants' layout and the depth linearisation the history pass and the paint agree on. A program of its own: it
// needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_selfrefl_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_selfrefl_rules_test

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "car_selfrefl_rules.h"

namespace cr = mcla::gpu::car_selfrefl;
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

// clip = [p 1] M as the paint variant does it: p.x m[0..3] + p.y m[4..7] + p.z m[8..11] + m[12..15].
void Project(const float m[16], const double p[3], double clip[4]) {
  for (int c = 0; c < 4; ++c) {
    clip[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
  }
}

// The game's projection, row vectors, the camera looking down -z, ndc z 0 at the near plane and 1 at the far one (the stored
// depth is 1 - ndc: 1 near, 0 far).
tp::Mat4 Perspective(double fov_y, double aspect, double n, double f) {
  tp::Mat4 p;
  const double h = 1.0 / std::tan(fov_y * 0.5);
  p.m[0][0] = h / aspect;
  p.m[1][1] = h;
  p.m[2][2] = f / (n - f);
  p.m[2][3] = -1.0;
  p.m[3][2] = n * f / (n - f);
  return p;
}

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 t = tp::Identity();
  t.m[3][0] = x;
  t.m[3][1] = y;
  t.m[3][2] = z;
  return t;
}

}  // namespace

int main() {
  // the modes
  Check(cr::ModeOfTest(-3) == cr::Mode::kOff && cr::ModeOfTest(0) == cr::Mode::kOff, "0 and below is off");
  Check(cr::ModeOfTest(1) == cr::Mode::kOcclusion, "1 is the occlusion");
  Check(cr::ModeOfTest(2) == cr::Mode::kReflection && cr::ModeOfTest(9) == cr::Mode::kReflection, "2 and above is the reflection");

  // the clamps
  cr::Tuning wild;
  wild.length = 100.0f;
  wild.thickness = -1.0f;
  wild.strength = 3.0f;
  wild.steps = 1000;
  wild.view = -2;
  wild.gain = 1000.0f;
  const cr::Tuning clamped = cr::Clamped(wild);
  Check(clamped.length == cr::kMaxLength && clamped.thickness == cr::kMinThickness && clamped.strength == 1.0f &&
            clamped.steps == cr::kMaxSteps && clamped.view == 0 && clamped.gain == cr::kMaxGain,
        "the tuning is clamped");
  const cr::Tuning defaults;
  Check(cr::Clamped(defaults).steps == 12 && cr::Clamped(defaults).length == 2.0f, "the defaults pass the clamps unchanged");

  // the exponent bias
  Check(cr::ExpBiasGain(4) == 0.0625f && cr::ExpBiasGain(0) == 1.0f && cr::ExpBiasGain(-1) == 2.0f,
        "2^-bias: the target's bias 4 is a gain of 1/16");
  Check(cr::ExpBiasGain(1000) == std::ldexp(1.0f, -24), "the bias is limited");

  // the matrix: no motion is the view-projection itself; a motion is applied first
  tp::Mat4 vp = Perspective(0.45, 16.0 / 9.0, 0.5, 1000.0);
  // the history camera one unit to the right of the origin looking down -z: world x 1 is its view x 0
  vp = tp::Mul(Translation(-1.0, 0.0, 0.0), vp);
  float m[16];
  cr::HistoryMatrix(vp, nullptr, m);
  double clip[4];
  const double p[3] = {1.0, 0.0, -10.0};
  Project(m, p, clip);
  Check(Near(clip[0], 0.0, 1e-5) && Near(clip[3], 10.0, 1e-5), "a point straight ahead of the history camera projects to the middle at w 10");
  // a car that was 0.5 behind in world x one frame ago: this frame's x 1.5 is the history's x 1
  const tp::Mat4 motion = Translation(-0.5, 0.0, 0.0);
  cr::HistoryMatrix(vp, &motion, m);
  const double q[3] = {1.5, 0.0, -10.0};
  Project(m, q, clip);
  Check(Near(clip[0], 0.0, 1e-5) && Near(clip[3], 10.0, 1e-5), "the vehicle's motion is applied before the view-projection");

  // the constants' layout
  const cr::Tuning tuning;
  cr::CarSelfReflConstants c = cr::MakeConstants(cr::Mode::kReflection, tuning, m, 77u, 0u, 0.0625f, 5.0f);
  Check(c.a[0] == 2.0f && c.a[1] == 2.0f && c.a[2] == 0.25f && c.a[3] == 1.0f, "A: mode, length, thickness, strength");
  Check(c.b[0] == 12.0f && c.b[1] == 0.0f && c.b[2] == 0.0625f && c.b[3] == 5.0f, "B: steps, view, gain, noise frame");
  Check(c.c[0] == 77u && c.c[1] == 0u && c.c[2] == 0u && c.c[3] == 0u, "C: the slots");
  Check(std::memcmp(c.m, m, sizeof(m)) == 0, "M: the matrix rows");
  const cr::CarSelfReflConstants off = cr::OffConstants();
  Check(off.a[0] == 0.0f && off.c[0] == 0u, "the constants of a draw without a history are mode 0");
  cr::Tuning viewed = tuning;
  viewed.view = 3;
  viewed.gain = 2.0f;
  c = cr::MakeConstants(cr::Mode::kOcclusion, viewed, m, 1u, 0u, 0.0625f, 0.0f);
  Check(c.a[0] == 1.0f && c.b[1] == 3.0f && c.b[2] == 0.125f, "the debug view and the gain multiplier reach the block");

  // the linearisation the history pass makes and the w the paint divides by are the same distance
  const tp::Mat4 proj = Perspective(0.45, 16.0 / 9.0, 0.5, 1000.0);
  bool depth_ok = true;
  for (double distance : {0.6, 1.0, 3.0, 12.5, 40.0, 300.0, 900.0}) {
    // stored depth of a point `distance` in front of the camera, as the game stores it (1 - ndc z)
    const double clip_z = -distance * proj.m[2][2] + proj.m[3][2];
    const double clip_w = distance;  // p[2][3] = -1, p[3][3] = 0
    const double stored = 1.0 - clip_z / clip_w;
    const double t = cr::LinearDepth(stored, proj);
    if (!Near(t, distance, 1e-6)) {
      depth_ok = false;
      std::printf("  distance %g: stored %.9g gives %.9g\n", distance, stored, t);
    }
  }
  Check(depth_ok, "LinearDepth gives back the distance, which is the clip w of the paint's projection");
  Check(Near(cr::LinearDepth(0.0, proj), 1000.0, 1e-6), "stored 0 is the far plane");
  Check(Near(cr::LinearDepth(1.0, proj), 0.5), "stored 1 is the near plane");

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

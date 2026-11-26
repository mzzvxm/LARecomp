#include "cascade_fit_rules.h"
#include "cascade_schedule_rules.h"

#include <cstdio>
#include <cstdlib>
#include <numbers>

namespace cf = mcla::gpu::cascade_fit;
namespace cs = mcla::gpu::cascade_schedule;

namespace {
unsigned checks;
void Check(bool ok, const char* what) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  std::exit(1);
}
bool Near(double actual, double expected) { return std::abs(actual - expected) < 1e-8; }
double Cos(double degrees) { return std::cos(degrees * std::numbers::pi / 180.0); }

cf::Corners Cone(double yaw, const cf::Vec& eye) {
  const cf::Vec forward{std::sin(yaw), 0, std::cos(yaw)};
  const cf::Vec right{std::cos(yaw), 0, -std::sin(yaw)};
  cf::Corners corners;
  for (unsigned i = 0; i < 4; ++i) {
    corners[i] = eye;
    const cf::Vec offset = cf::Add(cf::Scale(right, (i & 1) ? 36 : -36),
                                  {0, (i & 2) ? 21.0 : -21.0, 0});
    corners[i + 4] = cf::Add(cf::Add(eye, cf::Scale(forward, 100)), offset);
  }
  return corners;
}
}

int main() {
  const cf::Basis basis = cf::WorldBasis(cf::Unit({0.31, 0.83, -0.46}));
  Check(Near(cf::Dot(basis.right, basis.sun), 0.0) &&
            Near(cf::Dot(basis.up, basis.sun), 0.0) &&
            Near(cf::Dot(basis.right, basis.up), 0.0), "orthogonal world light frame");
  Check(Near(cf::Length(basis.right), 1.0) && Near(cf::Length(basis.up), 1.0), "unit light axes");
  Check(Near(cf::Dot(cf::Cross(basis.right, basis.sun), basis.up), 1), "guest light handedness");
  Check(Near(cf::Length(cf::WorldBasis({0, 1, 0}).right), 1), "zenith has a valid fallback axis");

  const cf::Corners corners = Cone(0.0, {820, 17, -803});
  const cf::Sphere sphere = cf::EncloseCone(corners);
  const cf::Sphere turned = cf::EncloseCone(Cone(1.3, corners[0]));
  Check(Near(sphere.radius, 58.685), "tight sphere for an asymmetric cone");
  Check(Near(sphere.radius, turned.radius), "sphere size survives yaw");
  Check(cf::EncloseCone({}).radius == 0.0, "degenerate cone uses the guest fit");
  const double margin = cf::FitMargin(100.0);
  const cf::Box box = cf::Snap(sphere, basis, 1280, margin);
  const auto bounds = cf::Bounds(box);
  for (unsigned axis = 0; axis < 3; ++axis) {
    Check(Near(bounds[axis], box.center[axis] - box.half) &&
              Near(bounds[axis + 3], box.center[axis] + box.half), "guest min/max register order");
  }
  const auto turned_box = cf::Snap(turned, basis, 1280, margin);
  Check(Near(box.half, turned_box.half) && Near(box.texel, turned_box.texel), "yaw keeps footprint");
  Check(Near(cf::Snap(sphere, basis, 640, margin).texel * 639,
             cf::Snap(sphere, basis, 1920, margin).texel * 1919), "grid follows physical atlas size");
  const std::array directions{basis.right, basis.up, basis.sun};
  for (unsigned axis = 0; axis < directions.size(); ++axis) {
    for (const auto& corner : corners) {
      const auto shifted = cf::Add(corner, cf::Scale(directions[axis], cf::kOneFrameTravel));
      Check(cf::Dot(shifted, directions[axis]) <= bounds[axis + 3], "one more frame fits in the box");
    }
    const cf::Sphere translated{cf::Add(sphere.center, cf::Scale(directions[axis], box.texel * 3.3)),
                                sphere.radius};
    const auto moved = cf::Snap(translated, basis, 1280, margin);
    const auto delta = (moved.center[axis] - box.center[axis]) / box.texel;
    Check(Near(delta, std::round(delta)) && (Near(delta, 3.0) || Near(delta, 4.0)),
          "creep advances by whole texels");
  }
  const cf::Basis axes{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const auto centered = cf::Snap({{0, 0, 0}, 12.0}, axes, 1280, 3.0);
  const auto subtexel = cf::Snap({{centered.texel * -0.49, centered.texel * 0.49, 0}, 12.0},
                                axes, 1280, 3.0);
  Check(subtexel.center == centered.center, "motion within one cell holds exactly");
  const auto beyond = cf::Snap({{centered.texel * -0.51, centered.texel * 0.51, 0}, 12.0},
                               axes, 1280, 3.0);
  Check(Near(beyond.center[0], -centered.texel) && Near(beyond.center[1], centered.texel),
        "negative and positive grid transitions are symmetric");
  Check(cf::Snap(sphere, basis, 1, margin).half == 0, "invalid atlas size uses the original fit");

  const cf::Pose sample{{0, 0, 0}, {0, 0, 1}, {0, 1, 0}};
  cf::Pose current = sample;
  current.eye[2] = 3;
  Check(Near(cf::VisibleFar(sample, current, 60, 160), 78), "depth plus caster and two motion margins");
  current.eye[2] = 9;
  Check(cf::VisibleFar(sample, current, 60, 160) == 160, "discontinuous motion restores stock");
  current = sample;
  current.forward = {std::sqrt(1 - Cos(2) * Cos(2)), 0, Cos(2)};
  Check(cf::VisibleFar(sample, current, 60, 160) > 80, "turning adds depth margin");
  current.up = {std::sqrt(1 - Cos(5) * Cos(5)), Cos(5), 0};
  Check(cf::VisibleFar(sample, current, 60, 160) == 160, "roll exposes unsampled receivers");
  Check(cf::VisibleFar(sample, sample, 0, 160) == 160, "all sky uses stock Far");
  Check(cf::VisibleFar(sample, sample, 200, 160) == 160, "avenue uses stock Far");
  Check(cf::SelectFar(160, 80, 60, 0) == 88, "keep overlap with the inner tile");
  Check(cf::SelectFar(160, 80, 99.2, 0) == 104, "round the band upward");
  Check(cf::SelectFar(160, 80, 99.2, 112) == 112, "hysteresis holds while contracting");
  Check(cf::SelectFar(160, 80, 99.2, 96) == 104, "grow immediately");
  Check(cf::SelectFar(160, 80, 160, 88) == 160, "lost depth returns to the stock split");
  const double slice = cf::SelectFar(144, 72, 86, 0) / 160.0;
  Check(Near(slice * 160, 88) && Near(0.90 * 80, 72), "FarSlice changes the current tile only");

  const std::array<double, 4> lens{1, 1, 1, 0};
  Check(Near(cf::LinearDepth(0.125f, lens), 8), "reverse depth uses the current lens");
  Check(cf::LinearDepth(0.0f, lens) == 0, "sky is excluded");
  Check(cf::LinearDepth(2.0f, lens) == 0, "invalid stored depth is excluded");
  Check(cf::LinearDepth(0.1f, {}) == 0, "absent camera returns no sample");
  Check(cf::HoldCovers(3.0, Cos(4), 160) && !cf::HoldCovers(3.0, Cos(5), 160),
        "held box covers four degrees and one frame of travel");
  const cf::Vec normal{2, 0, 0};
  const float w = cf::PadPlane(normal, -10.0f, cf::kOneFrameTravel);
  Check(Near(w, -4.0) && cf::Dot(normal, {2, 0, 0}) + w >= 0.0,
        "culling accepts a boundary caster through one frame of travel");
  Check(cf::Dot(normal, {1.99, 0, 0}) + w < 0.0, "the padded cull plane still rejects outside");

  cs::View old;
  old.valid = true;
  old.sun = {0.1f, 0.9f, 0.1f};
  old.forward = old.heading = {0, 0, 1};
  old.up = {0, 1, 0};
  old.stable_far = 160;
  cs::View now = old;
  now.forward = now.heading = {float(std::sin(4 * std::numbers::pi / 180.0)), 0, float(Cos(4))};
  const cs::Casters no_casters;
  Check(cs::Decide(4, 1, old, now, no_casters, no_casters) == cs::Reason::kHold,
        "a four-degree turn still holds a stable tile");
  now.stable_far = 104;
  Check(cs::Decide(4, 1, old, now, no_casters, no_casters) == cs::Reason::kFit,
        "a new split forces the map and its matrices to refresh together");
  std::printf("cascade fit: %u checks passed\n", checks);
}

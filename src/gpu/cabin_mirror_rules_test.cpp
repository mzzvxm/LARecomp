#include <cstdio>

#include "cabin_mirror_rules.h"
#include "mirror_glass_rules.h"
#include "mirror_overlay_rules.h"

namespace {
namespace g = mcla::gpu;
namespace cab = g::cabin_mirror;
namespace mg = g::mirror_glass;
namespace mv = g::mirror_view;
namespace tp = g::temporal;

int checks = 0, failures = 0;

void Check(bool value, const char* description) {
  ++checks;
  if (!value) {
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", description);
  }
}

bool Near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }

void Tests() {
  const cab::V3 body_min{-0.975, -0.98, -2.3}, body_max{0.975, 0.34, 2.3};
  const cab::V3 centre{0, 0.20, -0.30};
  const cab::V3 normal{-0.18, -0.14, 0.97};
  Check(cab::Score(centre, normal, body_min, body_max) > 0, "R34 golden: on centre line, 14 cm below roof");
  Check(cab::Score({0.89, -0.05, -0.40}, normal, body_min, body_max) < 0, "side glass is outboard");
  Check(cab::Score({0.10, 0.19, -0.30}, normal, body_min, body_max) < 0, "outboard badge rejected");
  Check(cab::Score({0, -0.20, -0.30}, normal, body_min, body_max) < 0, "low centre trim rejected");
  Check(cab::Score(centre, {0, -0.95, 0.31}, body_min, body_max) < 0, "downward visor rejected");
  Check(cab::Score(centre, {0.55, 0, 0.84}, body_min, body_max) < 0, "oblique patch rejected");
  Check(cab::Score(centre, {0, 0.51, 0.86}, body_min, body_max) < 0, "steep patch rejected");
  Check(cab::Score(centre, {0, 0, -1}, body_min, body_max) < 0, "forward-facing patch rejected");
  Check(cab::Score(centre, normal, {-0.45, -0.98, -1.4}, {0.45, 0.34, 1.4}) < 0, "bike body is narrow");

  constexpr uint64_t kChrome = 0x2D8CA0430FF88ED7ull;
  mg::Finder finder;
  finder.BeginFrame(1);
  const auto reader = [](auto* positions, auto* indices, auto* normals) {
    // Hand-authored 20 by 5 cm patch, 16 triangles, facing +z in its mesh.
    for (uint32_t y = 0; y < 3; ++y) {
      for (uint32_t x = 0; x < 5; ++x) {
        positions->insert(positions->end(), {0.05f * x - 0.10f, 0.025f * y - 0.025f, 0.0f});
        normals->insert(normals->end(), {0, 0, 1});
        if (x < 4 && y < 2) {
          const uint32_t i = y * 5 + x;
          indices->insert(indices->end(), {i, i + 1, i + 6, i, i + 6, i + 5});
        }
      }
    }
    return true;
  };
  using cm = g::car_mirror::PatchStats;
  Check(finder.OnDraw(true, 7, 48, kChrome, reader, true).outcome == mg::Finder::Outcome::kMeasured, "one patch measured");
  Check(finder.OnDraw(true, 7, 48, kChrome, reader, true).outcome == mg::Finder::Outcome::kHave, "mesh cache hit");
  const auto* entry = finder.cache().Find(7);
  Check(entry && entry->stats.triangles == 16 && Near(entry->stats.area, 0.01) && entry->stats.flat > 0.99,
        "R34 golden measurements");
  if (!entry) return;
  const auto& a = entry->aperture;
  Check(Near(a.right - a.left, 0.20) && Near(a.top - a.bottom, 0.05), "measured aperture has 4:1 aspect");
  Check(cab::IsCabinCandidate(entry->stats, a, g::car_mirror::kClassInterior), "an interior shader can carry the mirror");
  Check(!cab::IsCabinCandidate(entry->stats, a, g::car_mirror::kClassLight), "lamps cannot become mirrors");
  const auto shape = [&](const cm& s) { return cab::IsCabinCandidate(s, a, g::car_mirror::kClassChrome); };
  cm bad = entry->stats;
  bad.triangles = 7;
  Check(!shape(bad), "too few triangles");
  bad.triangles = 61;
  Check(!shape(bad), "too many triangles");
  bad = entry->stats;
  bad.area = 0.0039f;
  Check(!shape(bad), "tiny badge");
  bad.area = 0.0601f;
  Check(!shape(bad), "large window");
  bad = entry->stats;
  bad.flat = 0.949f;
  Check(!shape(bad), "curved trim");
  bad = entry->stats;
  bad.extent = 0.401f;
  Check(!shape(bad), "long visor");
  auto almost_square = a;
  almost_square.top += 0.08;
  Check(!cab::IsCabinCandidate(entry->stats, almost_square, g::car_mirror::kClassOther), "near-square patch rejected");

  auto body_from_world = tp::Identity();
  body_from_world.m[0][0] = body_from_world.m[2][2] = -1;
  auto model = body_from_world;
  model.m[3][1] = centre.y;
  model.m[3][2] = 0.55;
  mg::Camera camera{tp::Identity(), tp::Identity(), false};
  camera.view_projection.m[2][3] = 1;
  camera.view_projection.m[3][3] = 0;
  mg::ci::ViewBox bounds;
  bounds.valid = true;
  for (int i = 0; i < 3; ++i) {
    const double lo[3] = {body_min.x, body_min.y, body_min.z}, hi[3] = {body_max.x, body_max.y, body_max.z};
    bounds.min[i] = float(lo[i]);
    bounds.max[i] = float(hi[i]);
  }
  finder.BeginEvaluation();
  finder.Consider(13, true, 7, 48, kChrome, model, true);
  auto result = finder.Choose(1, camera, body_from_world, bounds);
  Check(result.cabin_found && !result.found && result.glass[2].draw == 13, "one patch works without a side pair");
  Check(result.need.mask == cab::kReader && result.need.widest >= 40, "visible cabin requests bit 2");
  Check(result.need.hfov_deg > 0 && result.need.vfov_deg > 0, "cabin alone fits the rear field");
  Check(Near(cab::UnpackOffset(result.cabin_offset).z, 1.10, 0.0041), "guest rig is shifted by twice the eye distance");
  Check(mg::Unpack(mg::Pack(result.need)) == result.need, "bit 2 survives the renderer to guest need word");
  mg::NeedHold hold;
  hold.Update(result.need);
  for (uint32_t i = 0; i < 20; ++i) Check(hold.Update({}).mask == cab::kReader, "cabin demand is held");
  Check(hold.Update({}).mask == 0, "cabin demand expires");
  Check(!finder.Choose(1, camera).cabin_found, "a non-cockpit caller supplies no body bounds");

  const cab::V3 eye{0.07, -0.10, -0.55}, plane{0, 0, 0}, n{0, 0, -1};
  const auto shift = cab::VirtualOffset(eye, plane, n);
  const auto virtual_eye = eye + shift;
  Check(Near(shift.z, 1.1) && Near(virtual_eye.x, eye.x) && Near(virtual_eye.y, eye.y), "reflect the eye in the plane");
  const auto tagged = cab::UnpackOffset(cab::PackOffset({-0.31, -0.04, 1.1}));
  const auto world = cab::OffsetInRearAxes(tagged, {0, 0, 1}, {0, 1, 0}, {-1, 0, 0});
  Check(Near(world.x, -1.1, 0.0041) && Near(world.y, -0.04, 0.0041) && Near(world.z, 0.31, 0.0041), "guest offset uses the rotated camera rows");
  Check(cab::PackOffset({0, 0, -1}) == 0 && cab::PackOffset({5, 0, 1}) == 0, "invalid rig shifts are refused");

  const cab::Aperture aperture{{1, 0, 0}, {0, 1, 0}, -0.10, 0.10, -0.025, 0.025};
  const auto matrix = cab::MaskProjection(aperture, plane, n, virtual_eye);
  const auto clip = [&](cab::V3 p, int c) { return p.x * matrix.m[0][c] + p.y * matrix.m[1][c] + p.z * matrix.m[2][c] + matrix.m[3][c]; };
  Check(Near(clip({-0.10, 0.025, 0}, 0), -1) && Near(clip({-0.10, 0.025, 0}, 1), 1), "aperture top-left agrees with the UV");
  Check(Near(clip({0.10, -0.025, 0}, 0), 1) && Near(clip({0.10, -0.025, 0}, 1), -1), "aperture bottom-right agrees with the UV");
  const auto glass_point = cab::V3{0.03, 0.02, 0};
  const auto pillar = virtual_eye + (glass_point - virtual_eye) * 3.0;
  Check(Near(clip(pillar, 0) / clip(pillar, 3), 0.30) && Near(clip(pillar, 1) / clip(pillar, 3), 0.80) && clip(pillar, 2) > 0,
        "part behind the glass masks the reflected ray");
  Check(clip(glass_point, 2) < 0 && clip({0, 0, 0.15}, 2) < 0, "glass and geometry in front of it are clipped");
  Check(!cab::IsBlocker(g::car_mirror::kSeeThroughShaders[1], g::car_mirror::kClassGlass, true), "window remains an opening");
  Check(cab::IsBlocker(1, g::car_mirror::kClassInterior, false), "interior trim can cover the view");
  Check(cab::IsBlocker(1, g::car_mirror::kClassPaint, true), "opaque painted pillars cover the view");
  Check(!cab::IsBlocker(1, g::car_mirror::kClassTyre, true), "rolling tyre never invalidates the cache");
  Check(!cab::IsBlocker(1, g::car_mirror::kClassOther, false), "blended details do not occlude");
  Check(cab::Refresh(false, false, 0, 0), "make first image");
  Check(cab::Refresh(true, true, 0, 0), "refresh immediately if roof geometry changes");
  Check(!cab::Refresh(true, false, 0, 5000), "stable relative pose keeps the image");
  Check(!cab::Refresh(true, false, 0.01, 499), "steady drift waits 500 ms");
  Check(cab::Refresh(true, false, 0.01, 500), "steady drift refreshes at 2 Hz");
  Check(cab::Refresh(true, false, 0.09, 0), "large pose change invalidates immediately");
}
}  // namespace

int main() {
  Tests();
  std::printf("cabin_mirror_rules: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}

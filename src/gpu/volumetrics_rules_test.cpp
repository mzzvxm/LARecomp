// Check of the volumetric light's rules (volumetrics_rules.h, volumetrics_types.h): the slice law and the 2D atlas, the phase function, the lamp's law,
// the cascade pick and the headlight shadow lookup, the reprojection of a voxel, the constants' layout and the builder; and, given the src/gpu folder,
// that the shaders' byte offsets are the header's. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_volumetrics_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_volumetrics_rules_test [<src/gpu dir>]

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "volumetrics_rules.h"

using namespace mcla::gpu::volumetrics;

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

bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

void CheckNear(double got, double want, double eps, const std::string& what) {
  ++g_checks;
  if (!Near(got, want, eps)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.6f, want %.6f\n", what.c_str(), got, want);
  }
}

void CheckSlices() {
  for (const Level level : {Level::kMedium, Level::kHigh, Level::kUltra}) {
    const GridDesc g = GridFor(level);
    const float far_m = g.far_m;
    CheckNear(SliceBoundary(g.near_m, far_m, g.nz, 0.0f), g.near_m, 1e-5, "the first boundary is the near plane");
    CheckNear(SliceBoundary(g.near_m, far_m, g.nz, float(g.nz)), far_m, far_m * 1e-5, "the last boundary is the far plane");
    float previous = 0.0f;
    for (uint32_t k = 0; k < g.nz; ++k) {
      const float c = SliceCentre(g.near_m, far_m, g.nz, k);
      const float t = SliceThickness(g.near_m, far_m, g.nz, k);
      Check(c > previous, "slice centres grow");
      Check(t > 0.0f, "slices have a thickness");
      // The coordinate of a centre is k + 0.5 and the law inverts.
      CheckNear(SliceCoord(g.near_m, far_m, g.nz, c), double(k) + 0.5, 1e-3, "SliceCoord of a centre is k + 0.5");
      previous = c;
    }
    // A slice is a constant share of its depth thicker than the one before: the ratio of thicknesses is the grid's.
    const float ratio = std::exp(LogRatio(g.near_m, far_m) / float(g.nz));
    CheckNear(SliceThickness(g.near_m, far_m, g.nz, 5) / SliceThickness(g.near_m, far_m, g.nz, 4), ratio, 1e-4, "log spaced slices");
  }
  // The survey's numbers: 0.5 m to 160 m in 64 slices grow by 9.5 percent a slice.
  const GridDesc u = GridFor(Level::kUltra);
  CheckNear(std::exp(LogRatio(u.near_m, u.far_m) / float(u.nz)), 1.0945, 1e-3, "ultra's slice growth");
}

void CheckAtlas() {
  const GridDesc g = GridFor(Level::kHigh);
  uint32_t seen = 0;
  for (uint32_t k = 0; k < g.nz; ++k) {
    uint32_t tx, ty, ax, ay;
    TileOf(g, k, &tx, &ty);
    Check(tx < g.tiles_x && ty < g.tiles_y, "the tile is inside the atlas");
    AtlasTexel(g, g.nx - 1, g.ny - 1, k, &ax, &ay);
    Check(ax < g.atlas_width() && ay < g.atlas_height(), "the last voxel of a slice is inside the atlas");
    AtlasTexel(g, 0, 0, k, &ax, &ay);
    Check(ax == tx * g.nx && ay == ty * g.ny, "the first voxel of a slice is its tile's corner");
    seen += 1;
  }
  Check(seen == g.nz, "every slice has a tile");
  // The uv stays half a texel inside the tile.
  float au, av;
  AtlasUv(g, 0.0f, 0.0f, 9, &au, &av);
  uint32_t tx, ty;
  TileOf(g, 9, &tx, &ty);
  CheckNear(au, (double(tx * g.nx) + 0.5) / g.atlas_width(), 1e-6, "uv 0 is half a texel in");
  AtlasUv(g, 1.0f, 1.0f, 9, &au, &av);
  CheckNear(au, (double((tx + 1) * g.nx) - 0.5) / g.atlas_width(), 1e-6, "uv 1 is half a texel in");
  CheckNear(av, (double((ty + 1) * g.ny) - 0.5) / g.atlas_height(), 1e-6, "uv 1 (v) is half a texel in");
}

void CheckPhase() {
  // The phase function integrates to 1 over the sphere (2 pi times the integral over cos), for isotropic, forward and backward g.
  for (const float g : {0.0f, 0.25f, 0.6f, -0.3f}) {
    double sum = 0.0;
    const int n = 20000;
    for (int i = 0; i < n; ++i) {
      const double c = -1.0 + 2.0 * (double(i) + 0.5) / n;
      sum += double(HenyeyGreenstein(float(c), g)) * (2.0 / n);
    }
    CheckNear(sum * 2.0 * double(kPi), 1.0, 2e-3, "the phase function integrates to 1");
  }
  CheckNear(HenyeyGreenstein(0.3f, 0.0f), 1.0 / (4.0 * double(kPi)), 1e-6, "g = 0 is isotropic");
  Check(HenyeyGreenstein(1.0f, 0.6f) > HenyeyGreenstein(-1.0f, 0.6f) * 10.0f, "g > 0 scatters forward");
}

void CheckLights() {
  CheckNear(SpotFactor(1.0f, std::cos(0.6f), std::cos(0.3f)), 1.0, 1e-6, "on the axis the spot is full");
  CheckNear(SpotFactor(std::cos(0.7f), std::cos(0.6f), std::cos(0.3f)), 0.0, 1e-6, "outside the cone the spot is 0");
  const float mid = SpotFactor(0.5f * (std::cos(0.6f) + std::cos(0.3f)), std::cos(0.6f), std::cos(0.3f));
  Check(mid > 0.0f && mid < 1.0f, "between the angles the spot is smooth");
  CheckNear(SpotFactor(-0.3f, -1.0f, -1.0f), 1.0, 1e-6, "cos_outer -1 is a point light");
  CheckNear(LampFalloff(25.0f, 25.0f), 0.0, 1e-9, "the lamp's light is 0 at the reach");
  CheckNear(LampFalloff(30.0f, 25.0f), 0.0, 1e-9, "and beyond it");
  Check(LampFalloff(2.0f, 25.0f) > LampFalloff(4.0f, 25.0f) * 3.5f, "inverse square near the lamp");
  Check(std::isfinite(LampFalloff(0.0f, 25.0f)) && LampFalloff(0.0f, 25.0f) < 2.0f, "no singularity at the lamp");
  // Extinction: the height layer decays and a hill above it is clear air; the noise averages out.
  VolWorld w;
  w.density[0] = 0.01f;
  w.density[1] = 10.0f;
  w.density[2] = 20.0f;
  w.density[3] = 2.0f;
  w.noise[0] = 0.5f;
  CheckNear(Extinction(w, 10.0f, 0.5f), 0.01 * 3.0, 1e-6, "at the ground the layer triples the haze");
  CheckNear(Extinction(w, 10.0f + 20.0f, 0.5f), 0.01 * (1.0 + 2.0 * std::exp(-1.0)), 1e-6, "one scale height up");
  CheckNear(Extinction(w, 1000.0f, 0.5f), 0.01, 1e-4, "far above, the clear air floor");
  CheckNear(Extinction(w, 1000.0f, 1.0f), 0.01 * 1.5, 1e-4, "the noise's high end");
  w.density[0] = 0.0f;
  CheckNear(Extinction(w, 10.0f, 0.5f), 0.0, 1e-9, "no haze, no extinction");
}

void CheckCamera() {
  VolCamera c;
  c.pos[0] = 10.0f;
  c.pos[1] = 2.0f;
  c.pos[2] = -5.0f;
  c.right[0] = 0.0f;
  c.right[2] = 1.0f;
  c.forward[0] = 1.0f;
  c.forward[2] = 0.0f;
  c.tan_x = 1.5f;
  c.tan_y = 0.8f;
  for (const float u : {0.1f, 0.5f, 0.93f}) {
    for (const float v : {0.2f, 0.5f, 0.8f}) {
      float p[3];
      ViewToWorld(c, u, v, 17.0f, p);
      float u2, v2, z2;
      Check(WorldToView(c, p, &u2, &v2, &z2), "a point in front is in view");
      CheckNear(u2, u, 1e-5, "u round trip");
      CheckNear(v2, v, 1e-5, "v round trip");
      CheckNear(z2, 17.0, 1e-4, "z round trip");
    }
  }
  const float behind[3] = {0.0f, 2.0f, -5.0f};
  float u, v, z;
  Check(!WorldToView(c, behind, &u, &v, &z), "a point behind is not");
  // The centre of the picture at depth z is straight ahead.
  float centre[3];
  ViewToWorld(c, 0.5f, 0.5f, 4.0f, centre);
  CheckNear(centre[0], 14.0, 1e-5, "the centre ray goes forward");
  CheckNear(centre[1], 2.0, 1e-5, "... at the camera's height");
}

void CheckShadows() {
  // Two nested cascade boxes along the x axis (right) and y (top): the finest holds |x + 0| < 10, the coarser |x| < 50.
  VolSun sun;
  sun.right[0] = 1.0f;
  sun.top[2] = 1.0f;
  sun.cascade_count = 4;
  const float halves[4] = {10.0f, 20.0f, 50.0f, 200.0f};
  for (int i = 0; i < 4; ++i) {
    sun.cascades[i].box[0] = 0.0f;
    sun.cascades[i].box[1] = halves[i];
    sun.cascades[i].box[2] = 0.0f;
    sun.cascades[i].box[3] = halves[i];
    // the matrix: u = x / (2 half) + 0.5 ; v = z / (2 half) + 0.5 ; depth = y * 0.01 + 0.5, w 1
    sun.cascades[i].rows[0][0] = 1.0f / (2.0f * halves[i]);
    sun.cascades[i].rows[1][2] = 0.01f;
    sun.cascades[i].rows[2][1] = 1.0f / (2.0f * halves[i]);
    sun.cascades[i].rows[3][0] = 0.5f;
    sun.cascades[i].rows[3][1] = 0.5f;
    sun.cascades[i].rows[3][2] = 0.5f;
    sun.cascades[i].rows[3][3] = 1.0f;
  }
  const float a[3] = {5.0f, 3.0f, 2.0f};
  const float b[3] = {30.0f, 3.0f, 2.0f};
  const float c[3] = {500.0f, 3.0f, 2.0f};
  const float d[3] = {100.0f, 0.0f, 0.0f};
  Check(CascadeOf(sun, a) == 0, "a point in every box is in the finest cascade (4 minus the count)");
  Check(CascadeOf(sun, b) == 2, "a point in the two coarsest boxes is in the third cascade");
  Check(CascadeOf(sun, c) == -1, "a point in no box has no cascade");
  Check(CascadeOf(sun, d) == 3, "a point in the coarsest box only is in the last cascade");
  float u, v, depth;
  CascadePoint(sun.cascades[0], a, &u, &v, &depth);
  CheckNear(u, 5.0 / 20.0 + 0.5, 1e-6, "cascade uv.x");
  CheckNear(v, 2.0 / 20.0 + 0.5, 1e-6, "cascade uv.y");
  CheckNear(depth, 3.0 * 0.01 + 0.5, 1e-6, "cascade depth");
  // The headlight's published matrix: column vectors, row major, the atlas uv and depth over w. uv = (x + 1) / 2, (z + 1) / 2, depth = y / 10, w = 1.
  VolShadowView view = {};
  view.world_to_shadow[0] = 0.5f;
  view.world_to_shadow[3] = 0.5f;
  view.world_to_shadow[4 + 2] = 0.5f;
  view.world_to_shadow[4 + 3] = 0.5f;
  view.world_to_shadow[8 + 1] = 0.1f;
  view.world_to_shadow[12 + 3] = 1.0f;
  const float p[3] = {1.0f, 4.0f, 0.0f};
  Check(ShadowViewPoint(view, p, &u, &v, &depth), "a point in front of the light");
  CheckNear(u, 1.0, 1e-6, "view uv.x");
  CheckNear(v, 0.5, 1e-6, "view uv.y");
  CheckNear(depth, 0.4, 1e-6, "view depth");
  view.world_to_shadow[12 + 3] = -1.0f;
  Check(!ShadowViewPoint(view, p, &u, &v, &depth), "a point behind the light has w <= 0");
  // The compare: shadowed when the stored value is nearer than the receiver minus the bias; a stored 1 and a receiver outside 0..1 are lit.
  Check(ShadowLit(0.5f, 0.4f, 0.0f), "the stored depth is behind the receiver: lit");
  Check(!ShadowLit(0.3f, 0.4f, 0.0f), "the stored depth is in front of the receiver: shadowed");
  Check(ShadowLit(0.3f, 0.4f, 0.2f), "the bias lifts a shadow");
  Check(ShadowLit(1.0f, 0.9f, 0.0f), "nothing drawn there: lit");
  Check(ShadowLit(0.1f, 1.2f, 0.0f), "beyond the map's far plane: lit");
  Check(ShadowLit(0.1f, -0.1f, 0.0f), "before its near plane: lit");
}

void CheckNoise() {
  for (uint32_t frame = 0; frame < 4; ++frame) {
    for (int i = 0; i < 100; ++i) {
      const float n = InterleavedGradient(float(i * 7 % 160), float(i * 13 % 90), frame);
      Check(n >= 0.0f && n < 1.0f, "interleaved gradient noise is in 0..1");
    }
  }
  double sum = 0.0;
  for (uint32_t i = 1; i <= 64; ++i) {
    const float h = Halton(i, 2);
    Check(h >= 0.0f && h < 1.0f, "Halton is in 0..1");
    sum += h;
  }
  CheckNear(sum / 64.0, 0.5, 0.02, "Halton is evenly spread");
  CheckNear(Halton(1, 2), 0.5, 1e-9, "Halton(1, 2)");
  CheckNear(Halton(3, 3), 1.0 / 9.0, 1e-6, "Halton(3, 3)");
}

void CheckDefaults() {
  CheckNear(SunStrength(-5.0f), 0.0, 1e-6, "no sun under the horizon");
  CheckNear(SunStrength(15.0f), 1.0, 1e-6, "full sun at 15 degrees");
  CheckNear(SunStrength(60.0f), 0.0, 1e-6, "none at a noon sun's 60 degrees");
  Check(SunStrength(3.0f) > 0.0f && SunStrength(3.0f) < 1.0f, "a ramp at dawn");
  CheckNear(NightFactor(-10.0f), 1.0, 1e-6, "night under -4 degrees");
  CheckNear(NightFactor(10.0f), 0.0, 1e-6, "day from 6 degrees");
  CheckNear(WetFactor(0.095f), 0.0, 1e-6, "dry");
  CheckNear(WetFactor(1.17f), 1.0, 1e-6, "rain");
  CheckNear(BaseSigma(0.0f, 0.0f), 0.0012, 1e-9, "the clear air floor");
  CheckNear(BaseSigma(1.0f, 0.0f), 0.008, 1e-9, "rain");
  CheckNear(BaseSigma(0.0f, 0.94f), 0.02 * SmoothStep(0.7f, 0.95f, 0.94f), 1e-9, "the game's foggy weather");
  Check(BaseSigma(0.0f, 0.6f) == BaseSigma(0.0f, 0.0f), "a cloudy sky's fog opacity adds no haze");
}

void CheckConstants() {
  // The byte layout the shaders read.
  Check(sizeof(VolLamp) == 64 && sizeof(VolShadowView) == 96 && sizeof(VolCascade) == 80 && sizeof(VolWorld) == 112, "member sizes");
  Check(sizeof(VolSun) == 384, "VolSun is 384 bytes");
  Check(Offsets::grid == 0 && Offsets::depth == 16 && Offsets::history == 32 && Offsets::jitter == 48, "grid, depth, history, jitter offsets");
  Check(Offsets::cam_pos == 64 && Offsets::prev_pos == 128 && Offsets::world == 192 && Offsets::sun == 304, "camera, history camera, world, sun offsets");
  Check(Offsets::counts == 688 && Offsets::shadow_atlas == 704 && Offsets::lamps == 720, "counts, shadow atlas, lamps offsets");
  Check(Offsets::headlights == 720 + 48 * 64 && Offsets::shadow_views == Offsets::headlights + 4 * 64, "headlights, shadow views offsets");
  Check(sizeof(VolGpuConstants) == Offsets::shadow_views + 4 * 96 && sizeof(VolGpuConstants) % 16 == 0, "the constants end with the shadow views");

  // The builder.
  const GridDesc g = GridFor(Level::kMedium);
  VolInputs in;
  in.valid = true;
  in.world.density[0] = 0.01f;
  in.world.haze_colour[3] = 90.0f;  // a producer's request for the far plane
  in.lamp_count = 30;                // the level reads 12
  in.headlight_count = 4;            // the level reads 2
  for (uint32_t i = 0; i < 30; ++i) {
    in.lamps[i].pos[0] = float(i);
    in.lamps[i].range = 20.0f;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    in.headlights[i].pos[0] = 100.0f + float(i);
    in.headlights[i].shadow_view = int32_t(i);
  }
  in.shadow_view_count = 2;
  in.shadow_atlas_slot = 77;
  in.shadow_atlas_size[0] = 1024.0f;
  in.sun_valid = false;
  in.sun.cascade_count = 4;
  in.sun.atlas_slot = 5;
  VolCamera cam;
  cam.pos[0] = 1.0f;
  cam.tan_x = 1.7f;
  cam.tan_y = 0.9f;
  HistoryUse history;
  history.valid = true;
  history.camera = cam;
  history.camera.pos[0] = 0.5f;
  history.far_m = 100.0f;
  history.weight = 0.9f;
  VolGpuConstants c;
  MakeGpuConstants(g, in, cam, history, 123, &c);
  Check(c.grid[0] == 96 && c.grid[1] == 54 && c.grid[2] == 32 && c.grid[3] == 8, "grid");
  CheckNear(c.depth[0], 0.5, 1e-6, "near");
  CheckNear(c.depth[1], 90.0, 1e-6, "the producer's far plane is used (it is between 30 m and the level's far)");
  CheckNear(c.depth[2], std::log(180.0), 1e-5, "ln(far / near)");
  CheckNear(c.depth[3] * c.depth[2], 1.0, 1e-6, "1 / ln");
  CheckNear(c.history[2], 0.0, 1e-9, "Medium has no history: the weight is 0 and the history is never read");
  Check(c.counts[0] == 12 && c.counts[1] == 2, "the lamps and headlights are cut to the level's counts");
  Check(c.counts[2] == 2 && c.counts[3] == 77, "shadow views and the atlas slot");
  Check(c.headlights[0].shadow_view == 0 && c.headlights[1].shadow_view == 1, "the headlights' shadow views are kept");
  Check(c.sun.cascade_count == 0 && c.sun.atlas_slot == kNoSlot, "an unshadowed sun has no cascades in the constants");
  CheckNear(c.cam_pos[3], 1.7, 1e-6, "tan_x in cam_pos.w");
  CheckNear(c.cam_right[3], 0.9, 1e-6, "tan_y in cam_right.w");
  // The far plane request below 30 m or above the level's is ignored.
  in.world.haze_colour[3] = 10.0f;
  MakeGpuConstants(g, in, cam, history, 123, &c);
  CheckNear(c.depth[1], 120.0, 1e-6, "a request under 30 m is ignored");
  in.world.haze_colour[3] = 500.0f;
  MakeGpuConstants(g, in, cam, history, 123, &c);
  CheckNear(c.depth[1], 120.0, 1e-6, "a request above the level's far is ignored");
  // A level with history takes the history's camera and far plane.
  const GridDesc high = GridFor(Level::kHigh);
  in.world.haze_colour[3] = 0.0f;
  in.sun_valid = true;
  history.weight = 0.9f;
  MakeGpuConstants(high, in, cam, history, 124, &c);
  CheckNear(c.history[0], 100.0, 1e-6, "the history's far plane");
  CheckNear(c.history[2], 0.9, 1e-6, "the history's weight");
  CheckNear(c.prev_pos[0], 0.5, 1e-6, "the history's camera");
  Check(c.sun.cascade_count == 4 && c.sun.atlas_slot == 5, "a shadowed sun keeps its cascades");
  history.valid = false;
  MakeGpuConstants(high, in, cam, history, 125, &c);
  CheckNear(c.history[2], 0.0, 1e-9, "no valid history: weight 0");
  CheckNear(c.prev_pos[0], 1.0, 1e-6, "... and the history's camera is the frame's");
  // The weight by the camera's move.
  Check(HistoryWeight(high, true, 0.0f, 0.0f) == 0.9f, "a still camera keeps 0.9");
  CheckNear(HistoryWeight(high, true, 10.0f, 0.0f), 0.5, 1e-6, "a fast camera keeps 0.5");
  Check(HistoryWeight(high, false, 0.0f, 0.0f) == 0.0f, "no history, no weight");
  Check(HistoryWeight(GridFor(Level::kMedium), true, 0.0f, 0.0f) == 0.0f, "a level without history has no weight");
  // The gate.
  VolInputs none;
  none.world.density[0] = 0.0012f;
  Check(!HasSomethingToShow(none, 0.0f, 1e-4f), "a clear noon: nothing to show");
  Check(HasSomethingToShow(none, 2.0f, 1e-4f), "a low sun in a hazy air shows");
  none.lamp_count = 1;
  Check(HasSomethingToShow(none, 0.0f, 1e-4f), "a lamp shows");
  none.world.density[0] = 0.0f;
  Check(!HasSomethingToShow(none, 2.0f, 1e-4f), "no medium, nothing shows");
}

// The headlight rig's and the shadow replay's shapes, as the branch feat/headlights has them (headlight_rig.h Headlight / HeadlightSet,
// local_shadow_types.h LocalShadowView / LocalShadowFrame): the adaptors take them by their member names.
struct TestHeadlight {
  float pos[3];
  float dir[3];
  float cos_outer;
  float cos_inner;
  float range;
  float color[3];
  uint32_t car_key;
  bool is_player;
};
struct TestHeadlightSet {
  std::array<TestHeadlight, 4> lights;
  uint32_t count = 0;
};
struct TestShadowView {
  float world_to_shadow[16];
  float atlas_rect[4];
  float depth_bias;
  float texel_size;
  uint32_t light_index;
  uint32_t valid;
};
struct TestShadowFrame {
  std::array<TestShadowView, 4> views{};
  uint32_t count = 0;
};

void CheckAdaptors() {
  TestHeadlightSet set;
  set.count = 3;
  for (uint32_t i = 0; i < 3; ++i) {
    TestHeadlight& h = set.lights[i];
    h = {};
    h.pos[0] = float(i);
    h.dir[2] = -1.0f;
    h.cos_outer = 0.8f;
    h.cos_inner = 0.95f;
    h.range = 70.0f;
    h.color[0] = 10.0f + float(i);
  }
  VolInputs in;
  FillHeadlights(set, &in);
  Check(in.headlight_count == 3, "three headlights");
  Check(in.headlights[2].pos[0] == 2.0f && in.headlights[2].colour[0] == 12.0f && in.headlights[2].cos_inner == 0.95f && in.headlights[2].range == 70.0f,
        "the headlight's fields");
  Check(in.headlights[1].shadow_view == -1, "no shadow view yet");
  TestShadowFrame frame;
  frame.count = 2;
  frame.views[0].light_index = 1;
  frame.views[0].valid = 1;
  frame.views[0].depth_bias = 0.002f;
  frame.views[0].world_to_shadow[3] = 7.0f;
  frame.views[1].light_index = 2;
  frame.views[1].valid = 0;  // a view that is not valid shadows nothing
  FillShadowViews(&frame, 9, 2048.0f, 1024.0f, &in);
  Check(in.shadow_view_count == 2 && in.shadow_atlas_slot == 9, "two views and the atlas slot");
  Check(in.headlights[1].shadow_view == 0, "the headlight of light_index 1 takes view 0");
  Check(in.headlights[2].shadow_view == -1 && in.headlights[0].shadow_view == -1, "an invalid view and no view leave the light unshadowed");
  Check(in.shadow_views[0].world_to_shadow[3] == 7.0f && in.shadow_views[0].depth_bias == 0.002f, "the view's fields");
  Check(in.shadow_atlas_size[0] == 2048.0f && in.shadow_atlas_size[1] == 1024.0f, "the atlas size");
  FillShadowViews(static_cast<const TestShadowFrame*>(nullptr), 9, 1.0f, 1.0f, &in);
  Check(in.shadow_view_count == 0 && in.shadow_atlas_slot == kNoSlot && in.headlights[1].shadow_view == -1, "no frame: every headlight is lit");
}

// --- the shaders ---------------------------------------------------------------------------------------------------------------------------

bool ReadFile(const std::string& path, std::string* text) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  *text = buffer.str();
  return true;
}

// `static const uint kO_<name> = <number>;` lines of volumetrics_common.hlsli.
std::map<std::string, uint32_t> ParseOffsets(const std::string& text) {
  std::map<std::string, uint32_t> out;
  const std::string key = "\nstatic const uint kO_";
  size_t at = 0;
  while ((at = text.find(key, at)) != std::string::npos) {
    at += key.size();
    const size_t eq = text.find('=', at);
    const size_t semi = text.find(';', at);
    if (eq == std::string::npos || semi == std::string::npos || eq > semi) {
      break;
    }
    std::string name = text.substr(at, eq - at);
    while (!name.empty() && name.back() == ' ') {
      name.pop_back();
    }
    out[name] = uint32_t(std::stoul(text.substr(eq + 1, semi - eq - 1)));
    at = semi;
  }
  return out;
}

void CheckShaders(const std::string& folder) {
  std::string common;
  if (!ReadFile(folder + "/hlsl/volumetrics_common.hlsli", &common)) {
    Check(false, "volumetrics_common.hlsli is readable in " + folder);
    return;
  }
  const std::map<std::string, uint32_t> offsets = ParseOffsets(common);
  const std::map<std::string, uint32_t> want = {
      {"grid", Offsets::grid},         {"depth", Offsets::depth},       {"history", Offsets::history},         {"jitter", Offsets::jitter},
      {"cam_pos", Offsets::cam_pos},   {"prev_pos", Offsets::prev_pos}, {"world", Offsets::world},             {"sun", Offsets::sun},
      {"cascades", Offsets::sun + offsetof(VolSun, cascades)},          {"sun_atlas", Offsets::sun + offsetof(VolSun, atlas_slot)},
      {"sun_size", Offsets::sun + offsetof(VolSun, atlas_size)},        {"counts", Offsets::counts},           {"shadow_atlas", Offsets::shadow_atlas},
      {"lamps", Offsets::lamps},       {"headlights", Offsets::headlights}, {"shadow_views", Offsets::shadow_views},
  };
  for (const auto& entry : want) {
    const auto found = offsets.find(entry.first);
    Check(found != offsets.end(), "volumetrics_common.hlsli has kO_" + entry.first);
    if (found != offsets.end()) {
      Check(found->second == entry.second, "kO_" + entry.first + " is " + std::to_string(entry.second) + " (the header's offsetof), the shader has " + std::to_string(found->second));
    }
  }
  Check(offsets.size() == want.size(), "volumetrics_common.hlsli has no offset the header does not know");
  // The members of VolSun and VolLamp the shaders read by their own constants: lamp 64 bytes, cascade 80 (box at 64), shadow view 96 (bias at 80).
  Check(offsetof(VolCascade, box) == 64, "the cascade's box is at byte 64");
  Check(offsetof(VolShadowView, depth_bias) == 80, "the shadow view's bias is at byte 80");
  Check(offsetof(VolLamp, shadow_view) == 48, "the lamp's shadow view is at byte 48");
  Check(offsetof(VolSun, top) == 16 && offsetof(VolSun, cascades) == 32, "the sun's axes and cascades");
  std::string inject;
  if (ReadFile(folder + "/hlsl/volumetrics_inject_cs.hlsl", &inject)) {
    Check(inject.find("i * 80u + 64u") != std::string::npos, "the inject reads a cascade's box at 80 * i + 64");
    Check(inject.find("uint(view_index) * 96u") != std::string::npos, "the inject reads a shadow view at 96 * i");
    Check(inject.find("kO_lamps + i * 64u") != std::string::npos && inject.find("kO_headlights + j * 64u") != std::string::npos, "the inject reads the lamps at 64 bytes each");
  } else {
    Check(false, "volumetrics_inject_cs.hlsl is readable");
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckSlices();
  CheckAtlas();
  CheckPhase();
  CheckLights();
  CheckCamera();
  CheckShadows();
  CheckNoise();
  CheckDefaults();
  CheckConstants();
  CheckAdaptors();
  if (argc > 1) {
    CheckShaders(argv[1]);
  }
  if (g_failed != 0) {
    std::printf("volumetrics_rules_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("volumetrics_rules_test: %d checks passed%s\n", g_checks, argc > 1 ? " (with the shaders)" : "");
  return 0;
}

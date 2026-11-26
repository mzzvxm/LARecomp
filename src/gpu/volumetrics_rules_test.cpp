// Check of the volumetric light's rules (volumetrics_rules.h, volumetrics_types.h): the slice law and the 2D atlas, the phase function, the lamp's law,
// the cascade pick and the headlight shadow lookup, the reprojection of a voxel, the constants' layout and the builder; and, given the src/gpu folder,
// that the shaders' byte offsets are the header's. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_volumetrics_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_volumetrics_rules_test [<src/gpu dir>]

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "volumetrics_headlights.h"
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
}

// ---------------------------------------------------------------------------
// the look: the table by weather and hour, the weights, the clamps (VOL-LOOK-TUNE)
// ---------------------------------------------------------------------------

// A column of the volume as the shaders integrate it (the inject's headlight and lamp terms and the integrate pass), for the canonical open street of the
// chain's views (local-lights-open-mid): the camera 7 m behind the player and 1.7 m up, the player's two low beams (z = 2.1 m, 0.65 m high) and the oncoming car's
// 27 m ahead of the player, 4.86 m to its left, nose toward it. x right, y up, z ahead. Returns the light the composite would add (before the soft limit): the
// scene units of the volume. The same pipeline as perf's model (the VOL-LOOK-TUNE calibration: display luminance is about 0.02 to 0.03 of these units).
struct Street {
  VolLamp heads[4];
  VolLamp lamp;
};

Street MakeStreet() {
  Street s{};
  const float dip = -2.0f * kPi / 180.0f;
  for (int i = 0; i < 4; ++i) {
    VolLamp& l = s.heads[i];
    const bool oncoming = i >= 2;
    const float side = (i % 2 == 0) ? -0.65f : 0.65f;
    l.pos[0] = (oncoming ? -4.86f : 0.0f) + side;
    l.pos[1] = 0.65f;
    l.pos[2] = oncoming ? 24.9f : 2.1f;
    l.dir[0] = 0.0f;
    l.dir[1] = std::sin(dip);
    l.dir[2] = (oncoming ? -1.0f : 1.0f) * std::cos(dip);
    l.colour[0] = 1.0f;
    l.colour[1] = 0.93f;
    l.colour[2] = 0.78f;
    l.range = 70.0f;
    l.cos_outer = std::cos(38.0f * kPi / 180.0f);
    l.cos_inner = std::cos(12.0f * kPi / 180.0f);
    l.shadow_view = -1;
    ApplyHeadlightUnits(&l);
  }
  // a street lamp of intensity 70 (the game's units: colour x intensity, the plateau's radius squared x 0.618 as the near radius, the reach sqrt(100 I)), 8 m to the
  // left of the camera's axis, 6 m high, 13 m ahead of it, pointing down with the game's 125 degree cone (full inside 37.25, gone at 62.5)
  VolLamp& l = s.lamp;
  l.pos[0] = -8.0f;
  l.pos[1] = 6.0f;
  l.pos[2] = 6.0f;
  l.dir[1] = -1.0f;
  l.colour[0] = 70.0f;
  l.colour[1] = 70.0f * 0.85f;
  l.colour[2] = 70.0f * 0.55f;
  l.range = std::sqrt(100.0f * 70.0f);
  l.cos_outer = std::cos(62.5f * kPi / 180.0f);
  l.cos_inner = std::cos(37.25f * kPi / 180.0f);
  l.pad[0] = 0.618034f * 70.0f;
  l.shadow_view = -1;
  return s;
}

// (u, v) in 0..1 of the picture (v down); the 55 degree vertical field of view at 16:9; the camera pitched down 1.4 degrees.
void StreetRay(float u, float v, float dir[3]) {
  const float tan_y = std::tan(27.5f * kPi / 180.0f);
  const float tan_x = tan_y * 16.0f / 9.0f;
  const float pitch = -1.4f * kPi / 180.0f;
  const float fwd[3] = {0.0f, std::sin(pitch), std::cos(pitch)};
  const float up[3] = {0.0f, std::cos(pitch), -std::sin(pitch)};
  const float x = (2.0f * u - 1.0f) * tan_x;
  const float y = (1.0f - 2.0f * v) * tan_y;
  const float d[3] = {x + fwd[0] + y * up[0], fwd[1] + y * up[1], fwd[2] + y * up[2]};
  const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  for (int i = 0; i < 3; ++i) {
    dir[i] = d[i] / n;
  }
}

// The light the column of pixel (u, v) gets to the far end of the High grid (the sky takes the far slice), for a look, headlights and the lamp as asked.
double StreetLight(const VolLook& look, float u, float v, bool heads, bool lamps, double* largest = nullptr) {
  const Street street = MakeStreet();
  const GridDesc g = GridFor(Level::kHigh);
  VolWorld world;
  world.density[0] = look.sigma;
  world.density[1] = 0.3f;
  world.density[2] = look.ground_h;
  world.density[3] = look.ground_k;
  world.noise[0] = 0.0f;
  float dir[3];
  StreetRay(u, v, dir);
  const float cam[3] = {0.0f, 1.7f, -7.0f};
  const float pitch = -1.4f * kPi / 180.0f;
  const float fwd[3] = {0.0f, std::sin(pitch), std::cos(pitch)};
  const float along = dir[0] * fwd[0] + dir[1] * fwd[1] + dir[2] * fwd[2];
  double s[3] = {0.0, 0.0, 0.0};
  double t = 1.0;
  float d0 = g.near_m;
  for (uint32_t k = 0; k < g.nz; ++k) {
    const float dc = SliceCentre(g.near_m, g.far_m, g.nz, k);
    const float d1 = SliceBoundary(g.near_m, g.far_m, g.nz, float(k) + 1.0f);
    const float ray_t = dc / along;
    const float p[3] = {cam[0] + dir[0] * ray_t, cam[1] + dir[1] * ray_t, cam[2] + dir[2] * ray_t};
    const float sigma = std::max(Extinction(world, p[1], 0.5f), 1e-6f);
    double rate[3] = {0.0, 0.0, 0.0};
    auto add = [&](const VolLamp& l, float g_phase, float strength, bool head) {
      const float delta[3] = {p[0] - l.pos[0], p[1] - l.pos[1], p[2] - l.pos[2]};
      const float dist = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
      if (!(dist < l.range)) {
        return;
      }
      const float travel[3] = {delta[0] / dist, delta[1] / dist, delta[2] / dist};
      const float spot = SpotFactor(travel[0] * l.dir[0] + travel[1] * l.dir[1] + travel[2] * l.dir[2], l.cos_outer, l.cos_inner);
      float shape = 1.0f;
      if (head) {
        shape = HeadlightCutoff(travel[1]) * HeadlightNearFade(dist);
      }
      const float phase = HenyeyGreenstein(-(travel[0] * dir[0] + travel[1] * dir[1] + travel[2] * dir[2]), g_phase);
      const float term = strength * spot * shape * LampFalloff(dist, l.range, l.pad[0]) * phase * sigma * kLightGain;
      for (int c = 0; c < 3; ++c) {
        rate[c] += double(l.colour[c]) * term;
      }
    };
    if (heads) {
      for (const VolLamp& l : street.heads) {
        add(l, look.g_head, look.head_strength, true);
      }
    }
    if (lamps) {
      add(street.lamp, look.g_lamp, look.lamp_strength, false);
    }
    // the integrate pass: two halves around the slice's centre
    double tz = std::exp(-double(sigma) * double(dc - d0));
    for (int c = 0; c < 3; ++c) {
      s[c] += t * (rate[c] - rate[c] * tz) / sigma;
    }
    t *= tz;
    tz = std::exp(-double(sigma) * double(d1 - dc));
    for (int c = 0; c < 3; ++c) {
      s[c] += t * (rate[c] - rate[c] * tz) / sigma;
    }
    t *= tz;
    d0 = d1;
  }
  const double m = std::max({s[0], s[1], s[2]});
  if (largest) {
    *largest = m;
  }
  return m;
}

// the pictures' places (the measured regions of VOL-LOOK-TUNE): the sky above the street, the sky above the oncoming car, the street along its beam and the glare
// right at its lamps
constexpr float kSkyTop[2] = {0.5f, 0.07f};
constexpr float kSkyMid[2] = {0.5f, 0.21f};
constexpr float kStreetFar[2] = {0.5f, 0.49f};
constexpr float kBall[2] = {0.4f, 0.49f};

void CheckLook() {
  // the weights: three weathers and three hours, each triple sums to 1, for every sun height, wetness and fog opacity
  for (float e = -50.0f; e <= 80.0f; e += 2.5f) {
    for (float wet : {0.0f, 0.3f, 0.5f, 0.8f, 1.0f}) {
      for (float opacity : {0.0f, 0.13f, 0.41f, 0.56f, 0.67f, 0.94f, 1.0f}) {
        const LookWeights w = LookWeightsFor(e, wet, opacity);
        CheckNear(w.clear + w.fog + w.rain, 1.0, 1e-5, "the weathers sum to 1");
        CheckNear(w.night + w.low + w.day, 1.0, 1e-5, "the hours sum to 1");
        Check(w.clear >= -1e-6f && w.fog >= -1e-6f && w.rain >= -1e-6f && w.night >= 0.0f && w.low >= 0.0f && w.day >= 0.0f, "no weight is negative");
      }
    }
  }
  // what the run's census saw (the fog quad's opacity): a clear night 0 to 0.13, the dusk 0.41, the foggy night 0.56, the stormy night 0.59 with the rain's wetness,
  // the clear day 0.39 to 0.67, the foggy day 0.94
  CheckNear(LookWeightsFor(-47.5f, 0.0f, 0.0f).clear, 1.0, 1e-5, "a clear night (opacity 0) is clear");
  CheckNear(LookWeightsFor(-47.5f, 0.0f, 0.13f).clear, 1.0, 1e-5, "a clear night (opacity 0.13) is clear");
  CheckNear(LookWeightsFor(-47.5f, 0.0f, 0.56f).fog, 1.0, 1e-5, "the foggy night (opacity 0.56: the old test needed 0.7) is fog");
  CheckNear(LookWeightsFor(-47.5f, 1.17f, 0.59f).rain, 1.0, 1e-5, "the stormy night is rain");
  CheckNear(LookWeightsFor(-47.5f, WetFactor(1.17f), 0.59f).fog, 0.0, 1e-5, "and the rain leaves no room for fog");
  CheckNear(LookWeightsFor(20.0f, 0.0f, 0.41f).clear, 1.0, 1e-5, "the dusk of the chain (17.6 h, 20 degrees, opacity 0.41) is clear");
  CheckNear(LookWeightsFor(25.0f, 0.0f, 0.41f).clear, 1.0, 1e-5, "and at 17.2 h (25 degrees)");
  for (float opacity : {0.39f, 0.45f, 0.48f, 0.67f}) {
    CheckNear(LookWeightsFor(45.0f, 0.0f, opacity).fog, 0.0, 1e-5, "a clear day's fog opacity (0.39 to 0.67) is no fog");
  }
  CheckNear(LookWeightsFor(40.0f, 0.0f, 0.94f).fog, 1.0, 1e-5, "the foggy day (0.94) is fog");
  CheckNear(LookWeightsFor(10.0f, 0.0f, 0.41f).low, 1.0, 1e-5, "10 degrees is the low sun");
  Check(LookWeightsFor(20.0f, 0.0f, 0.41f).low > 0.99f && LookWeightsFor(25.0f, 0.0f, 0.41f).low > 0.8f, "and 20 and 25 degrees (the chain's dusk at 17.6 h and 17.2 h) are the low sun");
  CheckNear(LookWeightsFor(-30.0f, 0.0f, 0.0f).night, 1.0, 1e-5, "under the horizon is night");
  CheckNear(LookWeightsFor(60.0f, 0.0f, 0.0f).day, 1.0, 1e-5, "60 degrees is day");
  CheckNear(LookWeightsFor(40.0f, 0.0f, 0.0f).day, 1.0, 1e-5, "and 40 degrees");

  // the table: every cell finite and in the ranges the shaders and the cvars take
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) {
      const VolLook l = LookCell(LookWeather(a), LookHour(b));
      Check(std::isfinite(l.sigma) && l.sigma > 1e-4f && l.sigma < 0.05f, "a cell's extinction is a haze's (1e-4 .. 0.05 per metre)");
      Check(l.ground_k >= 0.0f && l.ground_k <= 20.0f && l.ground_h >= 0.0f && l.ground_h <= 500.0f, "a cell's ground layer is in the cvars' ranges");
      Check(l.g_sun >= 0.0f && l.g_sun <= 0.95f && l.g_lamp >= 0.0f && l.g_lamp <= 0.95f && l.g_head >= 0.0f && l.g_head <= 0.95f, "a cell's anisotropies are below 0.95");
      Check(l.sun_strength >= 0.0f && l.lamp_strength > 0.0f && l.head_strength > 0.0f, "a cell's strengths are positive (the sun's may be 0: night, rain)");
      Check(l.extinction >= 0.0f && l.extinction <= 1.0f, "a cell's dimming is a share");
      Check(l.cap > 0.0f, "every cell has a cap: nothing is left to add without limit");
    }
  }
  const VolLook clear_night = LookCell(LookWeather::kClear, LookHour::kNight);
  const VolLook fog_night = LookCell(LookWeather::kFog, LookHour::kNight);
  const VolLook rain_night = LookCell(LookWeather::kRain, LookHour::kNight);
  Check(fog_night.sigma >= 8.0f * clear_night.sigma, "the foggy night is at least 8 times the clear night's haze (the first look gave it the clear night's)");
  Check(rain_night.sigma >= 5.0f * clear_night.sigma, "the rain's air is at least 5 times the clear night's");
  Check(clear_night.head_strength <= 0.10f, "a clear night's headlight gain is small (the oncoming beam covers the camera: any more is a veil)");
  Check(clear_night.lamp_strength >= 3.0f, "a clear night's lamps carry the halos");
  Check(rain_night.head_strength < fog_night.head_strength, "rain's headlights are weaker than fog's: its beam is brighter on wet air");
  Check(clear_night.cap <= fog_night.cap && clear_night.cap <= rain_night.cap, "the cap is lowest in clear air");
  Check(fog_night.g_head < clear_night.g_head + 0.001f, "fog's headlights scatter less forward (the beam reads as a cone from behind)");
  Check(LookCell(LookWeather::kClear, LookHour::kLow).g_sun > clear_night.g_sun, "the low sun's lobe is tighter (god rays)");
  Check(LookCell(LookWeather::kClear, LookHour::kLow).extinction <= 0.05f + 1e-6f, "the low sun's sky is not dimmed by the haze");
  Check(rain_night.sun_strength == 0.0f && LookCell(LookWeather::kRain, LookHour::kDay).sun_strength == 0.0f, "rain has no sun term");

  // the mix is continuous: sweeping the sun from -20 to 70 degrees and the wetness from 0 to 1 moves no parameter by more than 8 per cent of its range per step
  {
    VolLook previous = LookFor(-20.0f, 0.0f, 0.0f);
    for (float e = -19.5f; e <= 70.0f; e += 0.5f) {
      const VolLook now = LookFor(e, 0.0f, 0.0f);
      Check(std::fabs(now.sigma - previous.sigma) < 0.0006f && std::fabs(now.cap - previous.cap) < 0.12f && std::fabs(now.extinction - previous.extinction) < 0.02f &&
                std::fabs(now.sun_strength - previous.sun_strength) < 0.08f,
            "the look moves smoothly with the sun's height");
      previous = now;
    }
    previous = LookFor(-30.0f, 0.0f, 0.56f);
    for (float wet = 0.0f; wet <= 1.0f; wet += 0.02f) {
      const VolLook now = LookFor(-30.0f, wet, 0.56f);
      Check(std::fabs(now.sigma - previous.sigma) < 0.0004f && std::fabs(now.head_strength - previous.head_strength) < 0.02f, "the look moves smoothly with the wetness");
      previous = now;
    }
  }
  // the mixed look is between the cells it is made of
  {
    const VolLook mixed = LookFor(LookWeights{0.5f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f});
    CheckNear(mixed.sigma, 0.5 * (clear_night.sigma + fog_night.sigma), 1e-6, "half clear, half fog is the mean of the cells");
  }

  // the clamps: the soft limit
  for (float cap : {0.5f, 1.5f, 6.0f}) {
    const float knee = kSoftKnee * cap;
    CheckNear(SoftLimit(0.0f, cap), 0.0, 1e-9, "no light stays none");
    CheckNear(SoftLimit(0.5f * knee, cap), 0.5 * knee, 1e-6, "below the knee the light is as it is");
    CheckNear(SoftLimit(knee, cap), knee, 1e-6, "at the knee the light is as it is");
    // the slope at the knee is 1 on both sides (a smooth shoulder)
    const double slope = (SoftLimit(knee + 1e-3f, cap) - SoftLimit(knee, cap)) / 1e-3;
    CheckNear(slope, 1.0, 5e-3, "the shoulder starts with the slope of the line");
    float last = 0.0f;
    for (float x = 0.0f; x <= 100.0f * cap; x += cap / 50.0f) {
      const float y = SoftLimit(x, cap);
      Check(y >= last - 1e-6f, "the soft limit never falls");
      Check(y <= cap + 1e-5f && y <= x + 1e-5f, "the soft limit never reaches the cap nor adds light");
      last = y;
    }
    Check(SoftLimit(1e9f, cap) <= cap && SoftLimit(1e9f, cap) > 0.999f * cap, "a huge light gets the cap");
    Check(SoftLimit(60000.0f, cap) <= cap, "the largest value a volume can hold (60000) is limited too");
  }
  CheckNear(SoftLimit(123.0f, 0.0f), 123.0, 1e-6, "no cap, no limit");
  {
    float c[3] = {40.0f, 20.0f, 4.0f};
    SoftLimitColour(c, 2.0f);
    Check(std::max({c[0], c[1], c[2]}) <= 2.0f, "a colour's largest channel is limited to the cap");
    CheckNear(c[1] / c[0], 0.5, 1e-5, "the limit keeps the colour's hue");
    CheckNear(c[2] / c[0], 0.1, 1e-5, "all three channels");
    float d[3] = {0.1f, 0.05f, 0.01f};
    SoftLimitColour(d, 2.0f);
    CheckNear(d[0], 0.1, 1e-6, "a dim colour is left alone");
  }
  // the low beam's shape (the rig's: headlight_rig_rules.h LampScale)
  CheckNear(HeadlightCutoff(-0.5f), 1.0, 1e-9, "below the lamp the low beam is complete");
  CheckNear(HeadlightCutoff(0.10f), 1.0, 1e-9, "complete up to the cut-off's start");
  CheckNear(HeadlightCutoff(0.38f), 0.0, 1e-9, "gone at the cut-off's end");
  CheckNear(HeadlightCutoff(0.9f), 0.0, 1e-9, "no light into the sky");
  Check(HeadlightCutoff(0.24f) > 0.0f && HeadlightCutoff(0.24f) < 1.0f, "a ramp between");
  CheckNear(HeadlightNearFade(0.5f), 0.0, 1e-9, "nothing in the lamp's first 0.8 m");
  CheckNear(HeadlightNearFade(2.3f), 1.0, 1e-6, "complete from 2.3 m");

  // THE BUDGETS: what the table gives in the chain's views (the scene units of the light the composite adds; the display's luminance is about 0.02 to 0.03 of them
  // below 1: the first look's clear night added 0.7 to the sky and 3.4 along the street, which is the grey veil; its rain 4.8 and 21, the white ball).
  const VolLook night = LookFor(-47.5f, 0.0f, 0.0f);
  const VolLook foggy = LookFor(-47.5f, 0.0f, 0.56f);
  const VolLook rainy = LookFor(-47.5f, 1.0f, 0.59f);
  const double sky_clear = StreetLight(night, kSkyTop[0], kSkyTop[1], true, true);
  const double mid_clear = StreetLight(night, kSkyMid[0], kSkyMid[1], true, true);
  const double street_clear = StreetLight(night, kStreetFar[0], kStreetFar[1], true, true);
  const double ball_clear = StreetLight(night, kBall[0], kBall[1], true, true);
  Check(sky_clear <= 0.06, "clear night: the sky above the street gets at most 0.06 (a few levels of the display), not the first look's 0.7");
  Check(mid_clear <= 0.10, "clear night: the sky above the oncoming car gets at most 0.10");
  Check(street_clear >= 0.10 && street_clear <= 0.6, "clear night: the oncoming beam along the street is faint but there (0.10 to 0.6; the first look 3.4)");
  Check(ball_clear <= 1.5 && ball_clear >= street_clear, "clear night: the glare at the oncoming lamps stays under the cap and is the brightest place");
  Check(sky_clear * 4.0 <= street_clear + 1e-9, "clear night: the beam stands at least 4 times above the veil");
  // a lamp's halo: the pixel at the lamp's place on the picture (the lamp is 8 m left, 6 m up, 13 m ahead of the camera: about u 0.16, v 0.26)
  const double halo_clear = StreetLight(night, 0.16f, 0.26f, false, true);
  Check(halo_clear >= 0.08 && halo_clear <= 1.0, "clear night: a street lamp shows a subtle halo (0.08 to 1.0)");
  const double sky_fog = StreetLight(foggy, kSkyTop[0], kSkyTop[1], true, true);
  const double ball_fog = StreetLight(foggy, kBall[0], kBall[1], true, true);
  Check(sky_fog <= 0.6 && sky_fog > sky_clear, "fog: a haze over the sky, under 0.6 (the first look 0.8), thicker than the clear night's");
  Check(SoftLimit(float(ball_fog), foggy.cap) <= foggy.cap, "fog: the glare is limited by the cap");
  const double sky_rain = StreetLight(rainy, kSkyTop[0], kSkyTop[1], true, true);
  const double street_rain = StreetLight(rainy, kStreetFar[0], kStreetFar[1], true, true);
  const double ball_rain = StreetLight(rainy, kBall[0], kBall[1], true, true);
  Check(sky_rain <= 0.5, "rain: the sky gets at most 0.5 (the first look 4.8)");
  Check(street_rain >= 0.8 && street_rain <= 4.0, "rain: the beam along the street is clear (0.8 to 4; the first look 21)");
  Check(ball_rain <= 6.0, "rain: the glare at the oncoming lamps is under 6 even before the cap (the first look 43: the white ball)");
  Check(SoftLimit(float(ball_rain), rainy.cap) < rainy.cap, "rain: and the cap holds it under its limit");
  // THE SUN: a horizontal ray at 1.7 m through sunlit air (vis 1, a sun colour whose red is 1, the game's 17 h is (1.00, 0.48, 0.10)), by the cosine between the ray and the
  // sun. Facing the sun the dusk's haze adds a warm veil of about half the sun's red (0.2 to 0.9), 90 degrees to it almost nothing: the rays are a contrast between lit and
  // shaded air close to the sun. The first look (sigma 0.0023, g 0.6) gave 0.13 facing it, which a run showed as no shafts at all.
  {
    const VolLook dusk = LookFor(22.0f, 0.0f, 0.41f);
    const GridDesc g = GridFor(Level::kHigh);
    VolWorld world;
    world.density[0] = dusk.sigma;
    world.density[1] = 0.3f;
    world.density[2] = dusk.ground_h;
    world.density[3] = dusk.ground_k;
    world.noise[0] = 0.0f;
    const float sigma = Extinction(world, 1.7f, 0.5f);
    auto column = [&](float cos_view) {
      double s = 0.0;
      double t = 1.0;
      float d0 = g.near_m;
      const double rate = double(dusk.sun_strength) * HenyeyGreenstein(cos_view, dusk.g_sun) * sigma;
      for (uint32_t k = 0; k < g.nz; ++k) {
        const float d1 = SliceBoundary(g.near_m, g.far_m, g.nz, float(k) + 1.0f);
        const double tz = std::exp(-double(sigma) * double(d1 - d0));
        s += t * (rate - rate * tz) / sigma;
        t *= tz;
        d0 = d1;
      }
      return s;
    };
    const double facing = column(0.97f);
    const double side = column(0.0f);
    Check(facing >= 0.2 && facing <= 0.9, "dusk: facing the sun the sunlit haze adds 0.2 to 0.9 of the sun's red (the first look 0.13)");
    Check(side <= 0.1 && side < 0.2 * facing, "dusk: at 90 degrees to the sun it adds almost nothing");
    Check(SoftLimit(float(facing) * 50.0f, dusk.cap) <= dusk.cap, "dusk: and 50 times the sun's strength stays under the cap");
    Check(dusk.extinction <= 0.05f + 1e-6f, "dusk: the haze dims the bright sky by at most 5 per cent of its own extinction");
  }
  // the debug multipliers cannot white out the picture: a head strength of 50 (the cvar's top) is still held by the cap
  VolLook stretched = rainy;
  stretched.head_strength *= 50.0f;
  stretched.lamp_strength *= 50.0f;
  Check(SoftLimit(float(StreetLight(stretched, kBall[0], kBall[1], true, true)), rainy.cap) <= rainy.cap, "50 times the lights still stay under the cap");
}

void CheckRainScatter() {
  for (float elevation : {-47.5f, 20.0f, 60.0f}) {
    const LookWeights clear = LookWeightsFor(elevation, 0.0f, 0.0f);
    const LookWeights fog = LookWeightsFor(elevation, 0.0f, 0.94f);
    const LookWeights rain = LookWeightsFor(elevation, WetFactor(1.17f), 0.59f);
    CheckNear(LookFor(clear, true).head_strength, 0.09, 1e-6, "rain switch preserves dry clear headlight scatter");
    CheckNear(LookFor(fog, true).head_strength, 0.16, 1e-6, "rain switch preserves dry fog headlight scatter");
    CheckNear(LookFor(rain, false).head_strength, 0.12, 1e-6, "rain switch off preserves the existing rain gain");
    CheckNear(LookFor(rain, true).head_strength, 0.06, 1e-6, "rain switch on halves the rain gain");
  }
  const LookWeights mixed_clear = LookWeightsFor(-47.5f, 0.5f, 0.0f);
  const LookWeights mixed_fog = LookWeightsFor(-47.5f, 0.5f, 0.56f);
  CheckNear(LookFor(mixed_clear, true).head_strength, (0.09 + 0.06) / 2, 1e-6, "wetness blends tuned rain with clear air");
  CheckNear(LookFor(mixed_fog, true).head_strength, (0.16 + 0.06) / 2, 1e-6, "wetness blends tuned rain with fog");

  const LookWeights rain = LookWeightsFor(-47.5f, 1.0f, 0.59f);
  const VolLook original = LookFor(rain);
  const VolLook tuned = LookFor(rain, true);
  CheckNear(original.head_strength, 0.12, 1e-6, "default rain look uses the original gain");
  const double original_beam = StreetLight(original, kStreetFar[0], kStreetFar[1], true, false);
  const double tuned_beam = StreetLight(tuned, kStreetFar[0], kStreetFar[1], true, false);
  const double tuned_sky = StreetLight(tuned, kSkyTop[0], kSkyTop[1], true, false);
  CheckNear(tuned_beam, original_beam * 0.5, 1e-5, "tuned rain beam carries half the original headlight scatter");
  Check(tuned_beam > tuned_sky, "the tuned rain headlight beam stands above its sky scatter");
  CheckNear(StreetLight(tuned, 0.16f, 0.26f, false, true), StreetLight(original, 0.16f, 0.26f, false, true), 1e-6,
            "the rain switch preserves the street lamp halo in the model");
}

void CheckConstants() {
  // The byte layout the shaders read.
  Check(sizeof(VolLamp) == 64 && sizeof(VolShadowView) == 96 && sizeof(VolCascade) == 80 && sizeof(VolWorld) == 112, "member sizes");
  Check(sizeof(VolSun) == 384, "VolSun is 384 bytes");
  Check(Offsets::grid == 0 && Offsets::depth == 16 && Offsets::history == 32 && Offsets::jitter == 48, "grid, depth, history, jitter offsets");
  Check(Offsets::cam_pos == 64 && Offsets::prev_pos == 128 && Offsets::world == 192 && Offsets::sun == 304, "camera, history camera, world, sun offsets");
  Check(Offsets::counts == 688 && Offsets::shadow_atlas == 704 && Offsets::lamps == 720, "counts, shadow atlas, lamps offsets");
  Check(Offsets::headlights == 720 + 48 * 64 && Offsets::shadow_views == Offsets::headlights + 4 * 64, "headlights, shadow views offsets");
  Check(Offsets::prev_counts == Offsets::shadow_views + kMaxShadowViews * 96 && Offsets::prev_headlights == Offsets::prev_counts + 16, "the history's headlights follow the shadow views (VOL-STABLE-2: appended, so that no offset above moved)");
  Check(Offsets::lamp_changes == Offsets::prev_headlights + 4 * 64 && Offsets::retired_lamps == Offsets::lamp_changes + 16 &&
            sizeof(VolGpuConstants) == Offsets::retired_lamps + kMaxLamps * 64 && kMaxShadowViews == 8,
        "lamp change masks and retired lamps follow the previous headlights");

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
  // VOL-STABLE-2: the headlights of the volume the history holds ride with the history, and only with it
  Check(c.prev_counts[0] == 0, "a history that holds no headlight has none in the constants");
  history.headlight_count = 2;
  history.headlights[0].pos[0] = 11.0f;
  history.headlights[1].pos[0] = 22.0f;
  MakeGpuConstants(high, in, cam, history, 124, &c);
  Check(c.prev_counts[0] == 2 && c.prev_headlights[0].pos[0] == 11.0f && c.prev_headlights[1].pos[0] == 22.0f, "the history's headlights are in the constants");
  history.headlight_count = 9;
  MakeGpuConstants(high, in, cam, history, 124, &c);
  Check(c.prev_counts[0] == 4, "and cut to the level's headlights");
  MakeGpuConstants(GridFor(Level::kMedium), in, cam, history, 124, &c);
  Check(c.prev_counts[0] == 0, "a level without history carries none");
  history.headlight_count = 0;
  history.valid = false;
  MakeGpuConstants(high, in, cam, history, 125, &c);
  CheckNear(c.history[2], 0.0, 1e-9, "no valid history: weight 0");
  Check(c.prev_counts[0] == 0, "... and no history's headlights");
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
  std::array<TestShadowView, 8> views{};
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
  // A car's light has a view for each of its two headlamps: the first view that names it is shadow_view, the second shadow_view_b, a third is not read.
  {
    TestShadowFrame lamps;
    lamps.count = 5;
    const uint32_t names[5] = {2, 2, 0, 2, 0};
    for (uint32_t i = 0; i < 5; ++i) {
      lamps.views[i].light_index = names[i];
      lamps.views[i].valid = 1;
    }
    FillHeadlights(set, &in);
    FillShadowViews(&lamps, 9, 1.0f, 1.0f, &in);
    Check(in.shadow_view_count == 5 && in.headlights[2].shadow_view == 0 && in.headlights[2].shadow_view_b == 1, "light 2 reads its first two views");
    Check(in.headlights[0].shadow_view == 2 && in.headlights[0].shadow_view_b == 4, "light 0 reads views 2 and 4");
    Check(in.headlights[1].shadow_view == -1 && in.headlights[1].shadow_view_b == -1, "a light no view names has none");
    // Eight views (four cars, two lamps each) are all carried.
    TestShadowFrame eight;
    eight.count = 8;
    for (uint32_t i = 0; i < 8; ++i) {
      eight.views[i].light_index = i / 2;
      eight.views[i].valid = 1;
    }
    FillHeadlights(set, &in);
    FillShadowViews(&eight, 9, 1.0f, 1.0f, &in);
    Check(in.shadow_view_count == 8, "eight views are carried (the room is a view for each lamp of four cars)");
  }
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
      {"prev_counts", Offsets::prev_counts}, {"prev_headlights", Offsets::prev_headlights},
      {"lamp_changes", Offsets::lamp_changes}, {"retired_lamps", Offsets::retired_lamps},
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
  Check(offsetof(VolLamp, shadow_view) == 48 && offsetof(VolLamp, shadow_view_b) == 56, "the lamp's shadow views are at bytes 48 and 56 (a car's two headlamps)");
  Check(offsetof(VolSun, top) == 16 && offsetof(VolSun, cascades) == 32, "the sun's axes and cascades");
  std::string inject;
  if (ReadFile(folder + "/hlsl/volumetrics_inject_cs.hlsl", &inject)) {
    Check(inject.find("i * 80u + 64u") != std::string::npos, "the inject reads a cascade's box at 80 * i + 64");
    Check(inject.find("uint(view_index) * 96u") != std::string::npos, "the inject reads a shadow view at 96 * i");
    Check(inject.find("static const float kLightGain = " + std::to_string(int(kLightGain)) + ".0;") != std::string::npos, "the inject's light gain is the header's");
    Check(inject.find("kO_lamps + i * 64u") != std::string::npos && inject.find("kO_headlights + j * 64u") != std::string::npos, "the inject reads the lamps at 64 bytes each");
    // the low beam's shape: the shader's four numbers are the header's (and the header's are the rig's: volumetrics_inputs_test.cpp)
    auto number = [&](const std::string& text, const std::string& name, double* out) {
      const size_t at = text.find("static const float " + name + " = ");
      if (at == std::string::npos) {
        return false;
      }
      *out = std::atof(text.c_str() + at + std::string("static const float " + name + " = ").size());
      return true;
    };
    double v = 0.0;
    Check(number(inject, "kHeadCutLow", &v) && Near(v, kHeadCutLow, 1e-6), "the inject's low beam cut-off starts where the header's does");
    Check(number(inject, "kHeadCutHigh", &v) && Near(v, kHeadCutHigh, 1e-6), "and ends where the header's does");
    Check(number(inject, "kHeadNearStart", &v) && Near(v, kHeadNearStart, 1e-6), "the inject's near fade starts where the header's does");
    Check(number(inject, "kHeadNearSpan", &v) && Near(v, kHeadNearSpan, 1e-6), "and spans what the header's does");
    Check(inject.find("HeadlightShape(travel.y, dist)") != std::string::npos, "the inject applies the low beam's shape to the headlights");
  } else {
    Check(false, "volumetrics_inject_cs.hlsl is readable");
  }
  std::string apply;
  if (ReadFile(folder + "/hlsl/volumetrics_apply_ps.hlsl", &apply)) {
    const std::string knee = "static const float kSoftKnee = ";
    const size_t at = apply.find(knee);
    Check(at != std::string::npos && Near(std::atof(apply.c_str() + at + knee.size()), kSoftKnee, 1e-6), "the composite's soft knee is the header's");
    Check(apply.find("SoftLimit(max(v.rgb, 0.0), g_push.sizes.w)") != std::string::npos, "the composite limits the light by the push's cap (sizes.w)");
  } else {
    Check(false, "volumetrics_apply_ps.hlsl is readable");
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
  CheckLook();
  CheckRainScatter();
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

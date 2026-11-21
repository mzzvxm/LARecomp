// Check of the headlight shadows' lookup (light_shadow_filter.h, the twin of the lit shaders' HeadlightShadow) and of the sizes of the views (local_shadow_rules.h). A program of its own:
// it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
// The maps are made by a small ray caster that does what the replay's rasteriser does for a flat surface: the depth z/w of the surface at the centre of each texel (z/w = A (1 - n / w)),
// with the pipeline's slope scaled bias on top, into an atlas of two tiles; the views are the ones local_shadow_rules.h makes (FitView, LightClip, WorldToShadow) at the world
// coordinates the game has (about 1000: the float error of the matrices counts). The scenes are the owner's: a road seen at a grazing angle from a lamp 0.7 m up, a bollard and a sign
// post in the beam at 5 / 10 / 20 m, a kerb, a wall. The old lookup (headlight_rig_rules.h SoftShadow, the box of 3 x 3 texels over a 512 tile of 84 degrees with the slope bias 2) is run on
// the same scenes, so that the numbers of the report say what changed.
//
//   cmake --build <build dir> --target larecomp_light_shadow_filter_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_light_shadow_filter_test

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "headlight_rig_rules.h"
#include "headlight_types.h"
#include "light_shadow_filter.h"
#include "local_shadow_rules.h"

namespace ls = mcla::gpu::local_shadow;
namespace sf = larecomp::gpu::shadow_filter;
namespace hr = mcla::gpu::headlight_rules;
namespace tp = mcla::gpu::temporal;

namespace mcla::gpu {
const HeadlightSet& CurrentHeadlights() {
  static const HeadlightSet empty{};
  return empty;
}
}  // namespace mcla::gpu

namespace {

bool g_debug = false;
int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

void CheckNear(double a, double b, double tol, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(a - b) <= tol)) {
    ++g_failed;
    std::printf("FAIL: %s: %.9g vs %.9g (tol %.3g)\n", what.c_str(), a, b, tol);
  }
}

void Info(const std::string& text) { std::printf("INFO %s\n", text.c_str()); }
std::string Num(double v, int digits = 3) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
  return buf;
}

// ---------------------------------------------------------------------------
// a ray cast scene
// ---------------------------------------------------------------------------

struct Vec {
  double x = 0, y = 0, z = 0;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double Len(Vec a) { return std::sqrt(Dot(a, a)); }
Vec Unit(Vec a) { return a * (1.0 / Len(a)); }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

struct Box {
  Vec lo, hi;
};

struct Scene {
  bool ground = true;
  double ground_y = 0.0;
  std::vector<Box> boxes;
};

struct Hit {
  bool hit = false;
  double t = 1e30;
  int face = -1;  // a surface id: 0 the ground, 1 + 6 b + side for a box
};

Hit Cast(const Scene& scene, Vec o, Vec d) {
  Hit best;
  if (scene.ground && d.y < -1e-12) {
    const double t = (scene.ground_y - o.y) / d.y;
    if (t > 0) {
      best = {true, t, 0};
    }
  }
  for (size_t b = 0; b < scene.boxes.size(); ++b) {
    const Box& box = scene.boxes[b];
    double t0 = 0.0, t1 = 1e30;
    int enter = -1;
    const double lo[3] = {box.lo.x, box.lo.y, box.lo.z}, hi[3] = {box.hi.x, box.hi.y, box.hi.z};
    const double oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    bool ok = true;
    for (int a = 0; a < 3 && ok; ++a) {
      if (std::fabs(dd[a]) < 1e-14) {
        ok = oo[a] >= lo[a] && oo[a] <= hi[a];
        continue;
      }
      double ta = (lo[a] - oo[a]) / dd[a], tb = (hi[a] - oo[a]) / dd[a];
      int side = dd[a] > 0 ? 2 * a : 2 * a + 1;
      if (ta > tb) {
        std::swap(ta, tb);
      }
      if (ta > t0) {
        t0 = ta;
        enter = side;
      }
      t1 = std::min(t1, tb);
      ok = t0 <= t1;
    }
    if (ok && enter >= 0 && t0 > 0 && t0 < best.t) {
      best = {true, t0, 1 + 6 * int(b) + enter};
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// a view and its map
// ---------------------------------------------------------------------------

struct Map {
  int tile = 0, atlas_w = 0, atlas_h = 0;  // two tiles side by side: the view is the second
  std::vector<float> depth;               // atlas_w x atlas_h
  larecomp::gpu::LocalShadowView view{};
  ls::ViewSpec spec;
  double a_scale = 1.0;

  float At(int x, int y) const {
    x = std::min(std::max(x, 0), atlas_w - 1);
    y = std::min(std::max(y, 0), atlas_h - 1);
    return depth[size_t(y) * size_t(atlas_w) + size_t(x)];
  }
  sf::Quad Gather(int ix, int iy) const { return {At(ix, iy + 1), At(ix + 1, iy + 1), At(ix + 1, iy), At(ix, iy)}; }
};

// A view of fov `fov_deg` from `eye` along `dir`, `tile` texels, the scene's depths with the pipeline's slope bias `slope_bias`.
Map MakeMap(const Scene& scene, Vec eye, Vec dir, double fov_deg, int tile, double slope_bias, double near_m = 1.5, double far_m = 70.0, double bias = 2e-4) {
  Map m;
  m.tile = tile;
  m.atlas_w = 2 * tile;
  m.atlas_h = tile;
  m.depth.assign(size_t(m.atlas_w) * size_t(m.atlas_h), 1.0f);
  m.spec.eye[0] = eye.x;
  m.spec.eye[1] = eye.y;
  m.spec.eye[2] = eye.z;
  m.spec.dir[0] = dir.x;
  m.spec.dir[1] = dir.y;
  m.spec.dir[2] = dir.z;
  m.spec.fov = fov_deg * ls::kPi / 180.0;
  m.spec.near_m = near_m;
  m.spec.far_m = far_m;
  tp::Mat4 clip;
  const bool lh_ok = ls::LightClip(m.spec, /*right_handed=*/true, &clip);
  if (!lh_ok) {
    std::printf("FAIL: LightClip\n");
    std::exit(1);
  }
  float uv[4];
  ls::AtlasLayout atlas = ls::MakeAtlas(uint32_t(tile), 2);
  ls::UvRect(atlas, 1, uv);
  ls::WorldToShadow(clip, uv, 1.0, 1.0, m.view.world_to_shadow);
  ls::PublishedRect(uv, m.view.atlas_rect);
  m.view.depth_bias = float(bias);
  m.view.texel_size = 1.0f / float(tile);
  m.view.valid = 1;
  m.a_scale = far_m / (far_m - near_m);
  // The camera basis as LightClip builds it (right handed: z = -dir).
  const Vec d = Unit(dir);
  Vec up{0, 1, 0};
  if (std::fabs(d.y) > 0.98) {
    up = {0, 0, 1};
  }
  const Vec zc = d * -1.0;
  const Vec x = Unit(Cross(up, zc));
  const Vec y = Cross(zc, x);
  const double th = std::tan(m.spec.fov * 0.5);
  std::vector<double> z(size_t(tile) * size_t(tile), 2.0);
  std::vector<int> face(size_t(tile) * size_t(tile), -1);
  for (int j = 0; j < tile; ++j) {
    for (int i = 0; i < tile; ++i) {
      const double ndc_x = 2.0 * (double(i) + 0.5) / double(tile) - 1.0;
      const double ndc_y = 1.0 - 2.0 * (double(j) + 0.5) / double(tile);
      const Vec v = d + x * (ndc_x * th) + y * (ndc_y * th);  // unit depth along the axis
      const Hit h = Cast(scene, eye, v);
      if (!h.hit) {
        continue;
      }
      const double w = h.t;  // metres along the axis
      const double zz = m.a_scale * (1.0 - near_m / w);
      if (zz < 0.0 || zz > 1.0) {
        continue;
      }
      z[size_t(j) * size_t(tile) + size_t(i)] = zz;
      face[size_t(j) * size_t(tile) + size_t(i)] = h.face;
    }
  }
  const auto at = [&](int i, int j) { return z[size_t(j) * size_t(tile) + size_t(i)]; };
  const auto fc = [&](int i, int j) { return face[size_t(j) * size_t(tile) + size_t(i)]; };
  for (int j = 0; j < tile; ++j) {
    for (int i = 0; i < tile; ++i) {
      const double zz = at(i, j);
      if (zz > 1.5) {
        continue;
      }
      // The slope scaled bias: the largest change of the surface's own depth over one texel, found on the same surface.
      double sx = 0.0, sy = 0.0;
      if (i + 1 < tile && fc(i + 1, j) == fc(i, j)) {
        sx = std::fabs(at(i + 1, j) - zz);
      } else if (i > 0 && fc(i - 1, j) == fc(i, j)) {
        sx = std::fabs(zz - at(i - 1, j));
      }
      if (j + 1 < tile && fc(i, j + 1) == fc(i, j)) {
        sy = std::fabs(at(i, j + 1) - zz);
      } else if (j > 0 && fc(i, j - 1) == fc(i, j)) {
        sy = std::fabs(zz - at(i, j - 1));
      }
      m.depth[size_t(j) * size_t(m.atlas_w) + size_t(tile + i)] = float(std::min(1.0, zz + slope_bias * std::max(sx, sy)));
    }
  }
  return m;
}

float Lookup(const Map& m, Vec p, Vec normal, sf::Detail* detail = nullptr) {
  const float w[3] = {float(p.x), float(p.y), float(p.z)};
  const float n[3] = {float(normal.x), float(normal.y), float(normal.z)};
  return sf::ViewLit(m.view, w, n, [&](int ix, int iy) { return m.Gather(ix, iy); }, detail);
}

// The old lookup on the same map: the box of 3 x 3 texels with the four tap shortcut (headlight_rig_rules.h SoftShadow), the depth compared at one depth.
float OldLookup(const Map& m, Vec p) {
  larecomp::gpu::LocalShadowView v = m.view;
  float uv[2], depth;
  const float w[3] = {float(p.x), float(p.y), float(p.z)};
  if (!hr::ProjectToShadow(v, w, uv, &depth)) {
    return 1.0f;
  }
  const float tx = uv[0] / ((v.atlas_rect[2] - v.atlas_rect[0]) * v.texel_size);
  const float ty = uv[1] / ((v.atlas_rect[3] - v.atlas_rect[1]) * v.texel_size);
  return hr::SoftShadow(tx, ty, [&](int ix, int iy) { return hr::ShadowTap(m.At(ix, iy), depth, v.depth_bias); });
}

// The street: a lamp 0.7 m up at world coordinates the game has, the beam 2 degrees down, along the heading.
struct Street {
  Vec lamp{1028.6, 15.5, -568.56};
  Vec dir;
  Vec side;  // to the right of the beam on the ground
  double ground_y = 14.8;
  explicit Street(double heading_deg = 200.0) {
    const double h = heading_deg * ls::kPi / 180.0;
    dir = Unit(Vec{std::cos(h), -0.0349, std::sin(h)});
    side = Unit(Cross(dir, Vec{0, 1, 0}));
  }
  // The point on the ground `d` metres from the lamp along the beam's horizontal and `lateral` to the side.
  Vec Ground(double d, double lateral = 0.0) const {
    const Vec flat = Unit(Vec{dir.x, 0.0, dir.z});
    const Vec p = lamp + flat * d + side * lateral;
    return {p.x, ground_y, p.z};
  }
  Vec Flat() const { return Unit(Vec{dir.x, 0.0, dir.z}); }
};

Box Pole(const Street& st, double d, double lateral, double half_width, double height) {
  const Vec c = st.Ground(d, lateral);
  // axis aligned boxes in the world: a square pole is symmetric enough
  return {{c.x - half_width, st.ground_y, c.z - half_width}, {c.x + half_width, st.ground_y + height, c.z + half_width}};
}

// ---------------------------------------------------------------------------
// the plane
// ---------------------------------------------------------------------------

void CheckPlane() {
  const Street st;
  Scene scene;
  scene.ground_y = st.ground_y;
  const Map m = MakeMap(scene, st.lamp, st.dir, 72.0, 1024, 1.0);
  const double texel_world = 1.0;  // unused
  (void)texel_world;
  // A road receiver at 5, 10, 20 and 40 m: the slope of the depth over one texel along u and v from the lookup against the finite difference of the real projection (double).
  tp::Mat4 clip;
  ls::LightClip(m.spec, true, &clip);
  const auto project = [&](Vec p, double out[3]) {
    double o[4];
    for (int j = 0; j < 4; ++j) {
      o[j] = p.x * clip.m[0][j] + p.y * clip.m[1][j] + p.z * clip.m[2][j] + clip.m[3][j];
    }
    out[0] = (0.5 + 0.5 * o[0] / o[3]) * 1.0;  // tile u in [0, 1]
    out[1] = (0.5 - 0.5 * o[1] / o[3]);
    out[2] = o[2] / o[3];
  };
  const Vec normal{0, 1, 0};
  for (double dist : {5.0, 10.0, 20.0, 35.0}) {
    const Vec p = st.Ground(dist, 1.3);
    sf::Detail detail;
    Lookup(m, p, normal, &detail);
    // two points on the ground a centimetre apart, along the beam and across
    const double step = 0.01;
    const Vec pa = p + st.Flat() * step, pb = p + st.side * step;
    double s0[3], sa[3], sb[3];
    project(p, s0);
    project(pa, sa);
    project(pb, sb);
    // texels: tile uv * 1024
    const double du1 = (sa[0] - s0[0]) * 1024, dv1 = (sa[1] - s0[1]) * 1024, dz1 = sa[2] - s0[2];
    const double du2 = (sb[0] - s0[0]) * 1024, dv2 = (sb[1] - s0[1]) * 1024, dz2 = sb[2] - s0[2];
    const double det = du1 * dv2 - du2 * dv1;
    const double gu = (dz1 * dv2 - dz2 * dv1) / det;
    const double gv = (du1 * dz2 - du2 * dz1) / det;
    CheckNear(detail.slope.x, gu, std::fabs(gu) * 0.01 + 2e-7, "the road's depth slope along u at " + Num(dist, 0) + " m is the projection's");
    CheckNear(detail.slope.y, gv, std::fabs(gv) * 0.01 + 2e-7, "the road's depth slope along v at " + Num(dist, 0) + " m is the projection's");
    if (dist == 10.0) {
      Info("the road at 10 m from a lamp 0.7 m up: depth slope " + Num(gu, 5) + " / " + Num(gv, 5) + " of z/w per texel of 1024 (the old slope bias 2 pushed the map " + Num(2.0 * std::max(std::fabs(gu), std::fabs(gv)), 4) + " away)");
    }
  }
  // A wall facing the lamp: no slope along the ray, the plane is the map's own.
  {
    const Vec p = st.lamp + st.dir * 10.0;  // on the view's axis, the plane at right angles to it: its depth is the same all over
    sf::Detail detail;
    Lookup(m, p, st.dir * -1.0, &detail);
    Check(std::fabs(detail.slope.x) < 1e-6 && std::fabs(detail.slope.y) < 1e-6, "a wall that faces the lamp has no depth slope (" + Num(detail.slope.x, 8) + ", " + Num(detail.slope.y, 8) + ")");
  }
  // A plane that contains the ray (its normal is at right angles to it): no slope is believed, nothing is NaN.
  {
    const Vec p = st.Ground(10.0, 0.0);
    sf::Detail detail;
    const float v = Lookup(m, p, st.side, &detail);
    Check(std::isfinite(v) && detail.slope.x == 0.0f && detail.slope.y == 0.0f, "a surface edge-on to the ray has no slope and a finite answer");
  }
}

// ---------------------------------------------------------------------------
// no acne
// ---------------------------------------------------------------------------

struct AcneStats {
  int receivers = 0;
  int shadowed = 0;
  double worst = 1.0;
};

AcneStats RoadGrid(const Map& m, const Street& st, bool old_lookup, double d0, double d1, double lat, int n) {
  AcneStats a;
  for (int i = 0; i < n; ++i) {
    for (int k = -n; k <= n; ++k) {
      const double d = d0 + (d1 - d0) * (double(i) + 0.37) / double(n);
      const Vec p = st.Ground(d, lat * double(k) / double(n) * d / 10.0 + 0.0123 * double(i));
      sf::Detail detail;
      const float v = old_lookup ? OldLookup(m, p) : Lookup(m, p, Vec{0, 1, 0}, &detail);
      if (!old_lookup && detail.outside) {
        continue;
      }
      ++a.receivers;
      if (v < 0.999f) {
        ++a.shadowed;
      }
      a.worst = std::min(a.worst, double(v));
    }
  }
  return a;
}

void CheckNoAcne() {
  const Street st;
  Scene scene;
  scene.ground_y = st.ground_y;
  // The levels: the sizes and the fields the rules make for a car's view and a lamp's (see CheckLevelSizes), the slope bias the pipeline has and none.
  const struct {
    const char* name;
    double fov;
    int tile;
  } kViews[] = {{"medium car view 768", 72.0, 768}, {"high car view 1024", 72.0, 1024}, {"ultra lamp view 1280", 80.0, 1280}};
  for (const auto& v : kViews) {
    for (double slope_bias : {double(ls::kSlopeScaledDepthBias), 0.0}) {
      const Map m = MakeMap(scene, st.lamp, st.dir, v.fov, v.tile, slope_bias);
      const AcneStats a = RoadGrid(m, st, false, 3.0, 40.0, 14.0, 60);
      Check(a.receivers > 3000 && a.shadowed == 0, std::string("no acne on the lit road, ") + v.name + ", slope bias " + Num(slope_bias, 1) + ": " + std::to_string(a.shadowed) + " of " + std::to_string(a.receivers) + " receivers shadowed (worst " + Num(a.worst, 4) + ")");
    }
  }
  // Other headings and a lamp a little higher or lower: the same.
  for (double heading : {10.0, 95.0, 160.0, 280.0}) {
    for (double height : {0.55, 0.9}) {
      Street s2(heading);
      s2.lamp.y = s2.ground_y + height;
      Scene sc2;
      sc2.ground_y = s2.ground_y;
      const Map m = MakeMap(sc2, s2.lamp, s2.dir, 72.0, 768, ls::kSlopeScaledDepthBias);
      const AcneStats a = RoadGrid(m, s2, false, 3.0, 40.0, 12.0, 40);
      Check(a.shadowed == 0, "no acne at heading " + Num(heading, 0) + ", lamp " + Num(height, 2) + " m up: " + std::to_string(a.shadowed) + " of " + std::to_string(a.receivers));
    }
  }
  // The old lookup on the same road (the slope bias 2, the map it had): the number the report quotes.
  {
    const Map old_map = MakeMap(scene, st.lamp, st.dir, 84.0, 512, 2.0);
    const AcneStats a = RoadGrid(old_map, st, true, 3.0, 40.0, 14.0, 60);
    Info("the old lookup (512, 84 degrees, slope bias 2, a box of 3) on the lit road: " + std::to_string(a.shadowed) + " of " + std::to_string(a.receivers) + " receivers shadowed (worst " + Num(a.worst, 3) + ")");
  }
  // WITHOUT the receiver's plane, the same map with the slope bias 1 would have acne: the plane is what makes the smaller bias safe. (A lookup at one depth: the old one on the new map.)
  {
    const Map m = MakeMap(scene, st.lamp, st.dir, 72.0, 1024, ls::kSlopeScaledDepthBias);
    const AcneStats a = RoadGrid(m, st, true, 3.0, 40.0, 14.0, 60);
    Info("the old lookup on the new map (slope bias 1): " + std::to_string(a.shadowed) + " of " + std::to_string(a.receivers) + " shadowed (worst " + Num(a.worst, 3) + ") -- the plane compare is what lets the bias be small");
  }
}

// ---------------------------------------------------------------------------
// a thin pole
// ---------------------------------------------------------------------------

// The share of lit along the centre line of the pole's shadow on the road, over many places of the receiver within a texel.
struct StripeStats {
  double min_dark = 1.0;  // 1 - max lit along the centre line: the darkest the shadow gets at its WORST place (1: always fully dark)
  double mean_dark = 0.0;
  int lost = 0;           // receivers on the centre line with lit == 1
  int n = 0;
};

StripeStats StripeNew(const Map& m, const Street& st, double pole_d, double width, double k0, double k1, int n) {
  StripeStats s;
  const Vec c = st.Ground(pole_d);
  for (int i = 0; i < n; ++i) {
    const double k = k0 + (k1 - k0) * (double(i) + 0.5) / double(n);
    // the ground point on the line lamp -> pole, k times as far; a jitter across within a quarter of the stripe's width
    const double lateral = ((double(i * 37 % 101) / 100.0) - 0.5) * 0.25 * width * k;
    const Vec p = st.Ground(pole_d * k, lateral) ;
    (void)c;
    sf::Detail detail;
    const float v = Lookup(m, p, Vec{0, 1, 0}, &detail);
    if (detail.outside) {
      continue;
    }
    ++s.n;
    s.min_dark = std::min(s.min_dark, 1.0 - double(v));
    s.mean_dark += 1.0 - double(v);
    if (v >= 0.999f) {
      ++s.lost;
    }
  }
  s.mean_dark /= std::max(1, s.n);
  return s;
}

StripeStats StripeOld(const Map& m, const Street& st, double pole_d, double width, double k0, double k1, int n) {
  StripeStats s;
  for (int i = 0; i < n; ++i) {
    const double k = k0 + (k1 - k0) * (double(i) + 0.5) / double(n);
    const double lateral = ((double(i * 37 % 101) / 100.0) - 0.5) * 0.25 * width * k;
    const Vec p = st.Ground(pole_d * k, lateral);
    const float v = OldLookup(m, p);
    ++s.n;
    s.min_dark = std::min(s.min_dark, 1.0 - double(v));
    s.mean_dark += 1.0 - double(v);
    if (v >= 0.999f) {
      ++s.lost;
    }
  }
  s.mean_dark /= std::max(1, s.n);
  return s;
}

void CheckThinPole() {
  const Street st(180.0);  // the beam along -x: the post's faces are at right angles to it, its width is its width
  const struct {
    const char* name;
    double fov;
    int tile;
    double min_dark_pole10;  // the darkest the shadow must be everywhere along the centre line, a 10 cm post at 10 m
  } kLevels[] = {{"medium 768", 72.0, 768, 0.85}, {"high 1024", 72.0, 1024, 0.9}, {"ultra 1280", 80.0, 1280, 0.9}};
  for (const auto& lv : kLevels) {
    for (double pole_d : {5.0, 10.0, 20.0}) {
      Scene scene;
      scene.ground_y = st.ground_y;
      scene.boxes.push_back(Pole(st, pole_d, 0.0, 0.05, 3.0));  // a 10 cm post, 3 m tall
      const Map m = MakeMap(scene, st.lamp, st.dir, lv.fov, lv.tile, ls::kSlopeScaledDepthBias);
      // the road behind it: k = 1.35 .. 3 times as far as the post (the lamp's height over 1.35 is 0.5 m: the ray passes the post at 0.18 m up)
      const StripeStats s = StripeNew(m, st, pole_d, 0.1, 1.35, std::min(3.0, 45.0 / pole_d), 400);
      const double texel_cm = 100.0 * ls::TexelMeters(m.spec, uint32_t(lv.tile), pole_d);
      const double texels_wide = 0.10 / (texel_cm / 100.0);
      Check(s.n > 100 && s.lost == 0, std::string("a 10 cm post at ") + Num(pole_d, 0) + " m (" + Num(texels_wide, 1) + " texels wide, " + lv.name + "): its shadow is never lost on the road behind it (" + std::to_string(s.lost) + " of " + std::to_string(s.n) + " lost)");
      if (texels_wide >= 4.0) {
        Check(s.min_dark >= lv.min_dark_pole10, std::string("the 10 cm post at ") + Num(pole_d, 0) + " m is as dark as the kernel allows everywhere on its centre line (" + lv.name + "): darkest worst " + Num(s.min_dark, 3));
      } else {
        Check(s.min_dark >= 0.3, std::string("the thin post at ") + Num(pole_d, 0) + " m (" + Num(texels_wide, 1) + " texels) keeps at least 30 percent of its shadow at its worst place (" + lv.name + "): " + Num(s.min_dark, 3));
      }
      Info(std::string("post 10 cm at ") + Num(pole_d, 0) + " m, " + lv.name + ": texel " + Num(texel_cm, 2) + " cm, post " + Num(texels_wide, 1) + " texels, shadow darkness over its centre line mean " + Num(s.mean_dark, 3) + ", worst " + Num(s.min_dark, 3));
    }
  }
  // The old lookup (512 tile, 84 degrees, slope bias 2) on the same posts: how often its shadow was lost.
  for (double pole_d : {5.0, 10.0, 20.0}) {
    Scene scene;
    scene.ground_y = st.ground_y;
    scene.boxes.push_back(Pole(st, pole_d, 0.0, 0.05, 3.0));
    const Map old_map = MakeMap(scene, st.lamp, st.dir, 84.0, 512, 2.0);
    const StripeStats s = StripeOld(old_map, st, pole_d, 0.1, 1.35, std::min(3.0, 45.0 / pole_d), 400);
    const double texel_cm = 100.0 * ls::TexelMeters(old_map.spec, 512, pole_d);
    Info(std::string("OLD lookup, post 10 cm at ") + Num(pole_d, 0) + " m: texel " + Num(texel_cm, 2) + " cm, post " + Num(0.10 / (texel_cm / 100.0), 1) + " texels; shadow lost on " + std::to_string(s.lost) + " of " + std::to_string(s.n) +
         " places along its centre line, darkness mean " + Num(s.mean_dark, 3) + ", worst " + Num(s.min_dark, 3));
  }
}

// ---------------------------------------------------------------------------
// a kerb, and rounding in the map
// ---------------------------------------------------------------------------

// A kerb 15 cm high and 25 cm deep, 6 m long, across the beam at d metres: the road behind it is in its shadow for 0.27 (d + 0.25) metres. How much of that length is dark (lit < 0.5) along the
// centre of the kerb, new lookup against the old one.
void CheckKerb() {
  const Street st(180.0);
  for (double d : {5.0, 10.0, 20.0}) {
    Scene scene;
    scene.ground_y = st.ground_y;
    const Vec near_corner = st.Ground(d, -3.0), far_corner = st.Ground(d + 0.25, 3.0);
    scene.boxes.push_back({{std::min(near_corner.x, far_corner.x), st.ground_y, std::min(near_corner.z, far_corner.z)}, {std::max(near_corner.x, far_corner.x), st.ground_y + 0.15, std::max(near_corner.z, far_corner.z)}});
    const double length = 0.273 * (d + 0.25);  // metres of road in its shadow, behind the kerb's far face
    const Map m = MakeMap(scene, st.lamp, st.dir, 72.0, 768, ls::kSlopeScaledDepthBias);
    const Map old_map = MakeMap(scene, st.lamp, st.dir, 84.0, 512, 2.0);
    int dark_new = 0, dark_old = 0, n = 0;
    double first_dark_new = -1.0, first_dark_old = -1.0;
    for (int i = 0; i < 400; ++i) {
      const double behind = 0.02 + length * 0.96 * (double(i) + 0.5) / 400.0;  // metres behind the kerb's far face
      const Vec p = st.Ground(d + 0.25 + behind, 0.0123 * double(i % 7));
      sf::Detail detail;
      const float v_new = Lookup(m, p, Vec{0, 1, 0}, &detail);
      const float v_old = OldLookup(old_map, p);
      if (detail.outside) {
        continue;
      }
      ++n;
      if (v_new < 0.5f) {
        ++dark_new;
        if (first_dark_new < 0) {
          first_dark_new = behind;
        }
      }
      if (v_old < 0.5f) {
        ++dark_old;
        if (first_dark_old < 0) {
          first_dark_old = behind;
        }
      }
    }
    Info("kerb 15 cm at " + Num(d, 0) + " m: its shadow is " + Num(length, 2) + " m long; dark on " + Num(100.0 * dark_new / std::max(1, n), 1) + " percent of it with the new lookup (from " + Num(first_dark_new, 2) + " m behind), " +
         Num(100.0 * dark_old / std::max(1, n), 1) + " percent with the old (from " + Num(first_dark_old, 2) + " m)");
    Check(n > 300 && dark_new >= int(0.85 * n), "a 15 cm kerb at " + Num(d, 0) + " m: its shadow is dark along at least 85 percent of its length (" + Num(100.0 * dark_new / std::max(1, n), 1) + ")");
  }
}

// The map's depth with the rounding of a float rasteriser and of vertices transformed at world coordinates near 1000 (a few 1e-5 of z/w): the lit road stays lit and a post's shadow stays.
void CheckRoundingInMap() {
  const Street st(180.0);
  const auto noisy = [&](const Scene& scene) {
    Map m = MakeMap(scene, st.lamp, st.dir, 72.0, 768, ls::kSlopeScaledDepthBias);
    unsigned seed = 99u;
    for (float& d : m.depth) {
      if (d < 1.0f) {
        seed = seed * 1664525u + 1013904223u;
        d += (float(seed >> 8) / float(1 << 24) - 0.5f) * 6e-5f;
      }
    }
    return m;
  };
  Scene road;
  road.ground_y = st.ground_y;
  const AcneStats a = RoadGrid(noisy(road), st, false, 3.0, 40.0, 14.0, 60);
  Check(a.shadowed == 0, "a map with rounding of 3e-5 in its depths keeps the road lit: " + std::to_string(a.shadowed) + " of " + std::to_string(a.receivers) + " receivers below 0.999 (worst " + Num(a.worst, 3) + ")");
  Scene post;
  post.ground_y = st.ground_y;
  post.boxes.push_back(Pole(st, 10.0, 0.0, 0.05, 3.0));
  const StripeStats s = StripeNew(noisy(post), st, 10.0, 0.1, 1.35, 3.0, 300);
  Check(s.lost == 0, "... and the post's shadow is still there (" + std::to_string(s.lost) + " places lost)");
}

// ---------------------------------------------------------------------------
// contact hardening and edges
// ---------------------------------------------------------------------------

// The width, in metres across the beam, over which the lit share goes from 10 to 90 percent at a road point d metres from the lamp, across the lateral edge of a wall's shadow.
double EdgeWidthMeters(const Map& m, const Street& st, double d, double lateral_edge_at_wall, double wall_d, double* texel_world) {
  // The wall's lateral edge is at `lateral_edge_at_wall` at the wall's distance; on the road at d the edge is at that times d / wall_d.
  const double edge = lateral_edge_at_wall * d / wall_d;
  double at10 = 0.0, at90 = 0.0;
  bool got10 = false, got90 = false;
  const double span = 0.4 + 0.04 * d;
  for (int k = 0; k <= 4000; ++k) {
    const double lat = edge - span + 2.0 * span * double(k) / 4000.0;  // from the shadowed side (negative) to the lit side
    const float v = Lookup(m, st.Ground(d, lat), Vec{0, 1, 0});
    if (!got10 && v >= 0.1f) {
      at10 = lat;
      got10 = true;
    }
    if (!got90 && v >= 0.9f) {
      at90 = lat;
      got90 = true;
    }
  }
  *texel_world = ls::TexelMeters(m.spec, uint32_t(m.tile), d);
  return got10 && got90 ? at90 - at10 : -1.0;
}

void CheckContactHardening() {
  const Street st(180.0);  // the beam along -x, the wall across it
  Scene scene;
  scene.ground_y = st.ground_y;
  const double wall_d = 8.0;
  const double wall_edge = -1.0;  // the wall covers lateral < -1 m at the wall's distance
  {
    const Vec e1 = st.Ground(wall_d, wall_edge), e2 = st.Ground(wall_d, -30.0);
    scene.boxes.push_back({{e1.x - 0.1, st.ground_y, std::min(e1.z, e2.z)}, {e1.x + 0.1, st.ground_y + 4.0, std::max(e1.z, e2.z)}});
  }
  const Map m = MakeMap(scene, st.lamp, st.dir, 72.0, 768, ls::kSlopeScaledDepthBias);
  double texel[3];
  const double near_w = EdgeWidthMeters(m, st, 9.0, wall_edge, wall_d, &texel[0]);   // 1 m behind the wall: a contact
  const double mid_w = EdgeWidthMeters(m, st, 16.0, wall_edge, wall_d, &texel[1]);
  const double far_w = EdgeWidthMeters(m, st, 30.0, wall_edge, wall_d, &texel[2]);
  Check(near_w > 0 && mid_w > 0 && far_w > 0, "the edge of a wall's shadow has a ramp at every distance");
  const double near_t = near_w / texel[0], mid_t = mid_w / texel[1], far_t = far_w / texel[2];
  Info("the 10 to 90 percent edge of a wall's shadow on the road (768 tile): 1 m behind the wall " + Num(100 * near_w, 1) + " cm = " + Num(near_t, 2) + " texels; 8 m behind " + Num(100 * mid_w, 1) + " cm = " + Num(mid_t, 2) + " texels; 22 m behind " + Num(100 * far_w, 1) + " cm = " + Num(far_t, 2) + " texels");
  Check(near_t < 1.1, "at a contact the edge is about a texel wide (" + Num(near_t, 2) + " texels): sharp");
  Check(far_t > 1.8 && far_t < 3.2, "far from the wall the edge is the soft kernel's, 2 to 3 texels (" + Num(far_t, 2) + ")");
  Check(near_t < mid_t + 0.05 && mid_t <= far_t + 0.05, "the edge widens with the distance from what casts it (contact hardening)");
  // The same wall on the old lookup: a ramp of 3 texels of the 512 tile everywhere.
  {
    const Map old_map = MakeMap(scene, st.lamp, st.dir, 84.0, 512, 2.0);
    const auto width = [&](double d) {
      const double edge = wall_edge * d / wall_d;
      double a10 = 0, a90 = 0;
      bool g10 = false, g90 = false;
      const double span = 0.6 + 0.05 * d;
      for (int k = 0; k <= 4000; ++k) {
        const double lat = edge - span + 2.0 * span * double(k) / 4000.0;
        const float v = OldLookup(old_map, st.Ground(d, lat));
        if (!g10 && v >= 0.1f) {
          a10 = lat;
          g10 = true;
        }
        if (!g90 && v >= 0.9f) {
          a90 = lat;
          g90 = true;
        }
      }
      return g10 && g90 ? a90 - a10 : -1.0;
    };
    Info("OLD lookup, the same wall: edge " + Num(100 * width(9.0), 1) + " cm at 1 m behind it, " + Num(100 * width(16.0), 1) + " cm at 8 m, " + Num(100 * width(30.0), 1) + " cm at 22 m (the new one: " + Num(100 * near_w, 1) + " / " + Num(100 * mid_w, 1) + " / " + Num(100 * far_w, 1) + ")");
  }
}

// ---------------------------------------------------------------------------
// synthetic maps: the ramp, the order of the Gather, continuity, the fade
// ---------------------------------------------------------------------------

// A view with a known matrix over an empty atlas, and a receiver in the middle of its tile facing the lamp.
struct Fixture {
  larecomp::gpu::LocalShadowView view{};
  float world[3] = {};
  float normal[3] = {};
  float tx = 0, ty = 0;  // the receiver's place in atlas texels
  float depth = 0;
};

Fixture MakeFixture(int tile = 1024) {
  const Street st;
  Scene scene;
  const Map m = MakeMap(scene, st.lamp, st.dir, 72.0, tile, 0.0);
  Fixture f;
  f.view = m.view;
  const Vec p = st.lamp + st.dir * 12.0 + Vec{0, -0.3, 0};
  f.world[0] = float(p.x);
  f.world[1] = float(p.y);
  f.world[2] = float(p.z);
  const Vec n = st.dir * -1.0;
  f.normal[0] = float(n.x);
  f.normal[1] = float(n.y);
  f.normal[2] = float(n.z);
  float uv[2];
  hr::ProjectToShadow(f.view, f.world, uv, &f.depth);
  f.tx = uv[0] / ((f.view.atlas_rect[2] - f.view.atlas_rect[0]) * f.view.texel_size);
  f.ty = uv[1] / ((f.view.atlas_rect[3] - f.view.atlas_rect[1]) * f.view.texel_size);
  return f;
}

// Moves the receiver by (dx, dy) texels in the map (a point on the plane facing the lamp: the world shift that shifts u and v, found by the Jacobian of the view).
void Shift(const Fixture& f, double dx, double dy, float out_world[3]) {
  // lateral axes of the view: the rows of the matrix
  const float* m = f.view.world_to_shadow;
  const Vec r0{m[0], m[1], m[2]}, r1{m[4], m[5], m[6]}, r3{m[12], m[13], m[14]};
  const double w = Dot(r3, Vec{f.world[0], f.world[1], f.world[2]}) + m[15];
  const double u_here = (Dot(r0, Vec{f.world[0], f.world[1], f.world[2]}) + m[3]) / w;
  const double v_here = (Dot(r1, Vec{f.world[0], f.world[1], f.world[2]}) + m[7]) / w;
  const Vec ju = (r0 - r3 * u_here) * (1.0 / w), jv = (r1 - r3 * v_here) * (1.0 / w);
  const double texel = (f.view.atlas_rect[2] - f.view.atlas_rect[0]) * f.view.texel_size;
  // the lateral steps: ju and jv are nearly at right angles; solve a * ju + b * jv for the uv change (dx, dy) * texel in least squares
  const double a11 = Dot(ju, ju), a12 = Dot(ju, jv), a22 = Dot(jv, jv);
  const double bx = dx * texel, by = dy * texel;
  const double det = a11 * a22 - a12 * a12;
  const double a = (bx * a22 - by * a12) / det, b = (by * a11 - bx * a12) / det;
  const Vec dp = ju * a + jv * b;
  out_world[0] = float(f.world[0] + dp.x);
  out_world[1] = float(f.world[1] + dp.y);
  out_world[2] = float(f.world[2] + dp.z);
}

void CheckSynthetic() {
  const Fixture f = MakeFixture(1024);
  // The Gather's order and the footprint: four fetches at the corners of the 4 x 4 block, whose lowest texel is floor(c - 0.5) - 1.
  {
    std::vector<std::pair<int, int>> calls;
    sf::Detail detail;
    const float v = sf::ViewLit(f.view, f.world, f.normal, [&](int ix, int iy) {
      calls.push_back({ix, iy});
      return sf::Quad{f.depth + 0.1f, f.depth + 0.1f, f.depth + 0.1f, f.depth + 0.1f};
    }, &detail);
    Check(v == 1.0f && calls.size() == 4 && detail.gathers == 4, "a lit footprint costs four gathers and answers 1");
    const int ix = int(std::floor(f.tx - 0.5f)) - 1, iy = int(std::floor(f.ty - 0.5f)) - 1;
    Check(calls.size() == 4 && calls[0] == std::make_pair(ix, iy) && calls[1] == std::make_pair(ix + 2, iy) && calls[2] == std::make_pair(ix, iy + 2) && calls[3] == std::make_pair(ix + 2, iy + 2),
          "the four quads start at the footprint's first texel, two texels right, two down, both");
  }
  // Everything nearer: dark; at the receiver's own depth: lit.
  {
    const auto dark = [&](int, int) { return sf::Quad{f.depth - 0.05f, f.depth - 0.05f, f.depth - 0.05f, f.depth - 0.05f}; };
    CheckNear(sf::ViewLit(f.view, f.world, f.normal, dark), 0.0, 1e-6, "a footprint in shadow is dark");
    const auto own = [&](int, int) { return sf::Quad{f.depth, f.depth, f.depth, f.depth}; };
    CheckNear(sf::ViewLit(f.view, f.world, f.normal, own), 1.0, 1e-6, "a surface at the receiver's own depth is lit (it is the receiver)");
    const auto clear = [&](int, int) { return sf::Quad{1.0f, 1.0f, 1.0f, 1.0f}; };
    CheckNear(sf::ViewLit(f.view, f.world, f.normal, clear), 1.0, 1e-6, "a cleared map (stored 1) is lit");
  }
  // An edge between texel columns E - 1 (shadow: nearer than the receiver by a hair, a contact) and E (lit): a ramp one texel wide (r = 0.5), centred on the edge; the same edge with the blocker
  // far in front: 3 texels wide (r = 1.5). The test sweeps the receiver across it 0.01 texel at a time.
  const int E = int(std::floor(f.tx));
  for (const double gap : {2e-3, 0.3}) {
    const auto edge = [&](int ix, int iy) {
      sf::Quad q;
      float v[4];
      const int col[4] = {0, 1, 1, 0}, row[4] = {1, 1, 0, 0};
      for (int k = 0; k < 4; ++k) {
        (void)row;
        v[k] = (ix + col[k]) >= E ? 1.0f : f.depth - float(gap);
      }
      q.x = v[0];
      q.y = v[1];
      q.z = v[2];
      q.w = v[3];
      (void)iy;
      return q;
    };
    double last = -1.0, worst_step = 0.0, w10 = 0.0, w90 = 0.0;
    bool g10 = false, g90 = false;
    bool monotone = true;
    for (int k = -300; k <= 300; ++k) {
      float world[3];
      Shift(f, double(E) - double(f.tx) + 0.01 * double(k), 0.0, world);
      const float v = sf::ViewLit(f.view, world, f.normal, edge);
      if (last >= 0.0) {
        worst_step = std::max(worst_step, std::fabs(double(v) - last));
        if (g_debug && double(v) < last - 1.2e-2) {
          std::printf("BACK gap=%g k=%d last=%.5f v=%.5f\n", gap, k, last, double(v));
        }
        monotone = monotone && double(v) >= last - 1.2e-2;
      }
      if (g_debug && gap > 0.1 && k % 10 == 0) {
        std::printf("DBG k=%d v=%.5f\n", k, double(v));
      }
      last = v;
      const double x = 0.01 * double(k);
      if (!g10 && v >= 0.1f) {
        w10 = x;
        g10 = true;
      }
      if (!g90 && v >= 0.9f) {
        w90 = x;
        g90 = true;
      }
    }
    const double width = w90 - w10;
    Check(monotone, std::string("the edge is a monotone ramp (blocker gap ") + Num(gap, 4) + ")");
    Check(worst_step < 0.04, std::string("no step in the edge, 0.01 texel at a time (gap ") + Num(gap, 4) + "): the largest is " + Num(worst_step, 4));
    if (gap < 1e-2) {
      Check(width > 0.7 && width < 1.0, "a contact's edge is the nearest four texels' bilinear ramp: 0.8 texel from 10 to 90 percent (" + Num(width, 2) + ")");
    } else {
      Check(width > 2.2 && width < 2.6, "a far blocker's edge is the box of 3 texels' ramp: 2.4 texels from 10 to 90 percent (" + Num(width, 2) + ")");
    }
    Info(std::string("synthetic edge, gap ") + Num(gap, 4) + " of depth: 10 to 90 percent in " + Num(width, 2) + " texels; the largest step per 0.01 texel " + Num(worst_step, 4));
  }
  // The same edge along y: the answers are the transpose (the Gather's rows are not flipped).
  {
    const int Ey = int(std::floor(f.ty));
    const auto edge_y = [&](int ix, int iy) {
      (void)ix;
      float v[4];
      const int row[4] = {1, 1, 0, 0};
      for (int k = 0; k < 4; ++k) {
        v[k] = (iy + row[k]) >= Ey ? 1.0f : f.depth - 2e-3f;
      }
      return sf::Quad{v[0], v[1], v[2], v[3]};
    };
    bool ok = true;
    double last = -1.0;
    for (int k = -150; k <= 150; ++k) {
      float world[3];
      Shift(f, 0.0, double(Ey) - double(f.ty) + 0.01 * double(k), world);
      const float v = sf::ViewLit(f.view, world, f.normal, edge_y);
      ok = ok && double(v) >= last - 1.2e-2;
      last = v;
    }
    Check(ok, "an edge along y is a monotone ramp too (rows are not flipped)");
  }
  // A line of shadow ONE texel wide, wholly between the footprint's outer texels: the old lookup did not see it (four fetches agreed); this one reads every texel.
  {
    const int Lx = E;
    const auto line = [&](int ix, int iy) {
      (void)iy;
      float v[4];
      const int col[4] = {0, 1, 1, 0};
      for (int k = 0; k < 4; ++k) {
        v[k] = (ix + col[k]) == Lx ? f.depth - 0.002f : 1.0f;
      }
      return sf::Quad{v[0], v[1], v[2], v[3]};
    };
    double worst = 1.0;
    for (int k = -100; k <= 100; ++k) {
      float world[3];
      Shift(f, double(Lx) + 0.5 - double(f.tx) + 0.01 * double(k), 0.0, world);
      worst = std::min(worst, double(sf::ViewLit(f.view, world, f.normal, line)));
    }
    Check(worst < 0.55, "a line of shadow one texel wide is seen at its darkest: " + Num(worst, 3) + " lit at its centre (a box of 1 texel counts it whole)");
  }
  // The border fade: a map in shadow everywhere, the receiver moved toward the tile's border: dark inside, a ramp to lit over the last 5 percent of the tile.
  {
    const auto dark = [&](int, int) { return sf::Quad{0.0f, 0.0f, 0.0f, 0.0f}; };
    const double tile = 1024.0;
    const double t_uv = double(f.view.atlas_rect[2] - f.view.atlas_rect[0]) * f.view.texel_size;
    const double to_border = (double(f.view.atlas_rect[2]) - double(f.tx) * t_uv) / t_uv;  // texels to the right border
    double last = 1.0;
    bool monotone = true, reached_lit = false;
    int n_ramp = 0;
    for (int k = 0; k <= 80; ++k) {
      const double dx = to_border - 0.001 - 0.0008 * tile * double(k);  // from the border inward to 6.4 percent
      float world[3];
      Shift(f, dx, 0.0, world);
      sf::Detail detail;
      const float v = sf::ViewLit(f.view, world, f.normal, dark, &detail);
      if (detail.outside) {
        continue;
      }
      if (g_debug) {
        std::printf("FADE k=%d v=%.5f out=%d\n", k, double(v), int(detail.outside));
      }
      monotone = monotone && double(v) <= last + 4e-3;
      last = v;
      if (v > 0.001f && v < 0.999f) {
        ++n_ramp;
      }
      if (v >= 0.95f) {
        reached_lit = true;
      }
    }
    Check(reached_lit && monotone && n_ramp > 10, "the shadow fades to light toward the view's border and does not step: ramp over " + std::to_string(n_ramp) + " of 80 steps");
    float world[3];
    Shift(f, 0.0, 0.0, world);
    CheckNear(sf::ViewLit(f.view, world, f.normal, dark), 0.0, 1e-6, "in the middle of the view the shadow is whole");
  }
  // A point outside the rectangle, behind the view, beyond the depth range is lit and reads nothing.
  {
    int calls = 0;
    const auto count = [&](int, int) {
      ++calls;
      return sf::Quad{0.0f, 0.0f, 0.0f, 0.0f};
    };
    float behind[3] = {f.world[0] - 40.0f * (f.normal[0] * -1.0f), f.world[1] - 40.0f * (f.normal[1] * -1.0f), f.world[2] - 40.0f * (f.normal[2] * -1.0f)};
    const float v = sf::ViewLit(f.view, behind, f.normal, count);
    Check(v == 1.0f && calls == 0, "a point behind the lamp is lit and reads nothing");
    float far_away[3] = {f.world[0] + 200.0f * (f.normal[0] * -1.0f), f.world[1] + 200.0f * (f.normal[1] * -1.0f), f.world[2] + 200.0f * (f.normal[2] * -1.0f)};
    const float v2 = sf::ViewLit(f.view, far_away, f.normal, count);
    Check(v2 == 1.0f && calls == 0, "a point beyond the far plane is lit and reads nothing");
  }
  // Fuzz: finite answers in [0, 1] for any view, normal and map.
  {
    unsigned seed = 12345u;
    const auto rnd = [&]() {
      seed = seed * 1664525u + 1013904223u;
      return float(seed >> 8) / float(1 << 24);
    };
    bool ok = true;
    for (int i = 0; i < 20000 && ok; ++i) {
      float world[3] = {f.world[0] + (rnd() - 0.5f) * 40.0f, f.world[1] + (rnd() - 0.5f) * 10.0f, f.world[2] + (rnd() - 0.5f) * 40.0f};
      float n[3] = {rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f};
      const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (len < 1e-3f) {
        continue;
      }
      for (float& c : n) {
        c /= len;
      }
      const float a = rnd(), b = rnd(), c2 = rnd(), d2 = rnd();
      const float v = sf::ViewLit(f.view, world, n, [&](int, int) { return sf::Quad{a, b, c2, d2}; });
      ok = std::isfinite(v) && v >= 0.0f && v <= 1.0f;
    }
    Check(ok, "random points, normals and maps give a finite share in [0, 1]");
  }
}

// ---------------------------------------------------------------------------
// the sizes of the views
// ---------------------------------------------------------------------------

void CheckLevelSizes() {
  // The view a car's two lamps make and a lamp's own view, as the engine makes them from a light with the beam's 38 degree cone.
  float pos[3] = {1028.6f, 15.5f, -568.56f}, dir[3] = {-0.94f, -0.035f, -0.34f};
  ls::Light light;
  Check(ls::MakeLight(pos, dir, float(std::cos(38.0 * ls::kPi / 180.0)), 70.0f, 7u, true, 0, &light), "the beam's light is made");
  Check(std::fabs(light.shadow_half - ls::Rad(ls::kShadowConeCapDegrees)) < 1e-9, "the shadow view holds the cone up to the cap, 34 degrees, not the beam's 38");
  ls::Group groups[4];
  const uint32_t cars = ls::BuildGroups(&light, 1, groups);
  ls::ViewSpec car_view;
  Check(cars == 1 && ls::FitView(&light, groups[0], &car_view), "a car's view is fitted");
  CheckNear(car_view.fov * 180.0 / ls::kPi, 2.0 * (34.0 + ls::kFovMarginDegrees), 1e-6, "a car's view is 72 degrees wide (the old one 84)");
  // The lamp's view: the car's light with its lamps 0.55 m apart.
  float offset[3] = {0.0f, 0.0f, 0.55f};
  ls::Light with_lamps;
  ls::MakeLight(pos, dir, float(std::cos(38.0 * ls::kPi / 180.0)), 70.0f, 7u, true, 0, &with_lamps, offset);
  ls::ViewPlan plan;
  ls::PlanViews(&with_lamps, groups, 1, 1, 2, &plan);
  Check(plan.count == 2, "a car with its lamps apart has two views at the high level");
  ls::ViewSpec lamp_view;
  Check(ls::FitView(plan.views[0].lights.data(), plan.views[0].group, &lamp_view), "a lamp's view is fitted");
  Info("a lamp's view is " + Num(lamp_view.fov * 180.0 / ls::kPi, 1) + " degrees wide, a car's " + Num(car_view.fov * 180.0 / ls::kPi, 1));
  Check(lamp_view.fov * 180.0 / ls::kPi < 82.0 && lamp_view.fov * 180.0 / ls::kPi > 75.0, "a lamp's view is about 80 degrees (the old one 92)");
  // The texel in the world at 5, 10 and 20 m from the lamps, per level: the numbers the table's tile sizes are chosen for (the old: 512, 84 degrees: 1.8 / 3.5 / 7.0 cm).
  const struct {
    ls::Level level;
    const char* name;
    double max_cm_at_10;
  } kLevels[] = {{ls::Level::kMedium, "medium", 2.0}, {ls::Level::kHigh, "high", 1.7}, {ls::Level::kUltra, "ultra", 1.4}};
  for (const auto& lv : kLevels) {
    const uint32_t tile = ls::TileSize(lv.level);
    const ls::ViewSpec& spec = lv.level == ls::Level::kUltra || lv.level == ls::Level::kHigh ? lamp_view : car_view;
    const double cm5 = 100.0 * ls::TexelMeters(spec, tile, 5.0), cm10 = 100.0 * ls::TexelMeters(spec, tile, 10.0), cm20 = 100.0 * ls::TexelMeters(spec, tile, 20.0);
    Check(cm10 <= lv.max_cm_at_10, std::string(lv.name) + ": a texel is at most " + Num(lv.max_cm_at_10, 1) + " cm at 10 m (" + Num(cm10, 2) + " cm; 5 m " + Num(cm5, 2) + ", 20 m " + Num(cm20, 2) + ")");
    const ls::AtlasLayout atlas = ls::AtlasFor(lv.level, 0);
    Info(std::string(lv.name) + ": tile " + std::to_string(tile) + ", " + std::to_string(atlas.views) + " views, atlas " + std::to_string(atlas.width()) + "x" + std::to_string(atlas.height()) + ", " + Num(ls::Mib(ls::AtlasBytes(atlas)), 1) +
         " MiB; texel at 5 / 10 / 20 m: " + Num(cm5, 2) + " / " + Num(cm10, 2) + " / " + Num(cm20, 2) + " cm (old: " + Num(100.0 * 2.0 * 5.0 * std::tan(42.0 * ls::kPi / 180.0) / 512.0, 2) + " / " + Num(100.0 * 2.0 * 10.0 * std::tan(42.0 * ls::kPi / 180.0) / 512.0, 2) + " / " +
         Num(100.0 * 2.0 * 20.0 * std::tan(42.0 * ls::kPi / 180.0) / 512.0, 2) + ")");
  }
  Check(ls::TileSize(ls::Level::kMedium) == 768 && ls::TileSize(ls::Level::kHigh) == 1024 && ls::TileSize(ls::Level::kUltra) == 1280, "the levels' tiles are 768, 1024 and 1280");
  // Always sharper than the old 512 at every level; the video memory of each level.
  for (const auto& lv : kLevels) {
    Check(ls::TileSize(lv.level) >= 768, std::string(lv.name) + " is never below 768 texels");
  }
  CheckNear(ls::Mib(ls::AtlasBytes(ls::AtlasFor(ls::Level::kMedium, 0))), 9.0, 0.01, "medium holds 9 MiB (2 views of 768)");
  CheckNear(ls::Mib(ls::AtlasBytes(ls::AtlasFor(ls::Level::kHigh, 0))), 24.0, 0.01, "high holds 24 MiB (3 views of 1024)");
  CheckNear(ls::Mib(ls::AtlasBytes(ls::AtlasFor(ls::Level::kUltra, 0))), 75.0, 0.01, "ultra holds 75 MiB (6 views of 1280)");
  Check(ls::kSlopeScaledDepthBias == 1.0f && ls::kDepthBias == 2e-4f, "the map's slope bias is 1 and the lookup's constant bias 2e-4 (the plane compare replaced the margin)");
}

// ---------------------------------------------------------------------------
// alpha to mask in the atlas
// ---------------------------------------------------------------------------

// computeAlphaToMask (shader_recompiler.cpp), the 1 and 2 sample branches, for the pixel (x, y): the coverage mask of the draw's word `params` (bits 0-7 the four offsets, bit 8 on, bits 9-10 log2 samples).
uint32_t CoverageOf(uint32_t params, double alpha, int x, int y) {
  if ((params & 0x100u) == 0u) {
    return 0xFFFFFFFFu;
  }
  alpha = std::round(alpha * 65536.0) * (1.0 / 65536.0);
  const uint32_t index = (uint32_t(x & 1) << 1) | uint32_t(y & 1);
  const double offset = double((params >> (index * 2u)) & 3u);
  const uint32_t log2 = (params >> 9) & 3u;
  uint32_t coverage = 0;
  if (log2 == 1u) {
    const double step = offset * (1.0 / 8.0);
    coverage |= (alpha >= 0.5 - step) ? 2u : 0u;
    coverage |= (alpha >= 1.0 - step) ? 1u : 0u;
  } else {
    coverage = (alpha >= 1.0 - offset * (1.0 / 4.0)) ? 1u : 0u;
  }
  return coverage;
}

void CheckAlphaToMask() {
  // The game's word with MSAA 2x: offsets 3, 1, 0, 2 (0x87), alpha to mask on, 2 samples: what the colour passes carry. In the 1 sample atlas the texel's covered bit is bit 0.
  const uint32_t game = 0x87u | 0x100u | (1u << 9);
  // The game's own pattern at alpha 0.8: sample 0 is covered by the pixels with offsets 3 and 2 (thresholds 0.625 and 0.75) and not by 1 and 0 (0.875, 1.0): a 2 x 2 stipple, 1 texel in 2.
  int covered = 0;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      covered += int(CoverageOf(game, 0.8, x, y) & 1u);
    }
  }
  Check(covered == 2, "the game's word stipples a prop that is fading (alpha 0.8): " + std::to_string(covered) + " of the 4 texels of a quad cover the atlas's sample");
  const uint32_t atlas = ls::AtlasAlphaToMask(game);
  Check(atlas == 0x1AAu, "the atlas's word is offsets 2 2 2 2, alpha to mask on, one sample");
  bool no_dither = true;
  for (double alpha : {0.0, 0.2, 0.49, 0.51, 0.8, 0.97, 1.0}) {
    uint32_t first = CoverageOf(atlas, alpha, 0, 0) & 1u;
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        no_dither = no_dither && (CoverageOf(atlas, alpha, x, y) & 1u) == first;
      }
    }
  }
  Check(no_dither, "with the atlas's word every texel of a quad agrees: no stipple at any alpha");
  Check((CoverageOf(atlas, 0.49, 0, 0) & 1u) == 0u && (CoverageOf(atlas, 0.5, 0, 0) & 1u) == 1u && (CoverageOf(atlas, 1.0, 1, 1) & 1u) == 1u, "... a plain cut at alpha 0.5, and an opaque draw is whole");
  Check(ls::AtlasAlphaToMask(0u) == 0u && ls::AtlasAlphaToMask(0x87u) == 0u, "a draw without alpha to mask keeps no mask (its alpha test is the shader's own)");
  Check(ls::AtlasAlphaToMask(0x87u | 0x100u) == ls::AtlasAlphaToMask(0xAAu | 0x100u | (2u << 9)), "one word for every sample count the main view had");
}

// ---------------------------------------------------------------------------
// the edge in motion (HL-SHADOW-QUALITY-3)
// ---------------------------------------------------------------------------

// The owner (2026-11-21, watching 1080p and 4K night drives of the new exe beside the old one) saw "some really sharp shadows and some full of artifacts"; the 720p stills are clean. A map is a
// grid of point samples: where a shadow's edge falls in it is quantised to a texel, so as the lamp creeps (an oncoming car at 0.5 m/s, a car's sway) the edge does not move smoothly, it HOPS a
// texel at a time, and a texel is 1.3 to 3.8 cm at 10 to 20 m (the sizes above) = 2 to 8 pixels at 4K (a pixel is about 0.05 cm for every metre of distance) but under 1.5 pixels at 720p. The kernel
// cannot hide that (the shadow's edge is a ramp of 0.8 to 2.4 texels). This is not a failure of the lookup: a number for the report. The edge of a wall's shadow on the road is found at every
// frame (where the lit share crosses 0.5, scanned every half millimetre) while the lamp moves `step_m` a frame, against where the geometry puts it; what is left after the straight line of the
// true motion is the jitter (metres). INFO only: no limit is set here; the pictures (chain-hl-shadow-quality.ps1 -Stage take) are what a person looks at.
struct Crawl {
  double jitter_std = 0.0;   // m
  double jitter_max = 0.0;   // m
  double hop_max = 0.0;      // m: the largest change of the edge's place from one frame to the next beyond the true motion
  double texel = 0.0;        // m: one texel at the receiver's distance
  int frames = 0;
  int missing = 0;           // frames in which the edge was not found
};

Crawl EdgeInMotion(bool old_lookup, int tile, double fov, double slope_bias, double recv_d, bool lateral, double step_m, int frames) {
  const Street st(180.0);  // the beam along -x
  const double wall_d = 8.0, wall_edge = -1.0;
  Scene scene;
  scene.ground_y = st.ground_y;
  {
    const Vec e1 = st.Ground(wall_d, wall_edge), e2 = st.Ground(wall_d, -30.0);
    scene.boxes.push_back({{e1.x - 0.1, st.ground_y, std::min(e1.z, e2.z)}, {e1.x + 0.1, st.ground_y + 4.0, std::max(e1.z, e2.z)}});
  }
  Crawl out;
  out.frames = frames;
  std::vector<double> t_axis, residual;
  for (int t = 0; t < frames; ++t) {
    const double s = lateral ? step_m * double(t) : 0.0;
    const double a = lateral ? 0.0 : step_m * double(t);
    const Vec eye = st.lamp + (lateral ? st.side * s : st.Flat() * a);
    const Map m = old_lookup ? MakeMap(scene, eye, st.dir, 84.0, 512, 2.0) : MakeMap(scene, eye, st.dir, fov, tile, slope_bias);
    if (t == 0) {
      out.texel = ls::TexelMeters(m.spec, uint32_t(m.tile), recv_d);
    }
    // where the geometry puts the edge on the road at recv_d (the lateral place from the first lamp's axis): the ray from the lamp through the wall's edge
    const double e_true = lateral ? s + (wall_edge - s) * recv_d / wall_d : wall_edge * (recv_d - a) / (wall_d - a);
    const double window = 6.0 * out.texel + 0.03;
    double prev_v = -1.0, prev_lat = 0.0, cross = std::nan("");
    for (double lat = e_true - window; lat <= e_true + window; lat += 0.0005) {
      const Vec p = st.Ground(recv_d, lat);
      const float v = old_lookup ? OldLookup(m, p) : Lookup(m, p, Vec{0, 1, 0});
      if (prev_v >= 0.0 && prev_v < 0.5 && v >= 0.5) {
        cross = prev_lat + (0.5 - prev_v) / (double(v) - prev_v) * (lat - prev_lat);
        break;
      }
      prev_v = v;
      prev_lat = lat;
    }
    if (std::isnan(cross)) {
      ++out.missing;
      continue;
    }
    t_axis.push_back(double(t));
    residual.push_back(cross - e_true);
  }
  if (residual.size() < 8) {
    return out;
  }
  // the straight line of the residual (the map's own bias and a slow drift) taken out
  const double n = double(residual.size());
  double st_ = 0, sr = 0, stt = 0, str = 0;
  for (size_t i = 0; i < residual.size(); ++i) {
    st_ += t_axis[i];
    sr += residual[i];
    stt += t_axis[i] * t_axis[i];
    str += t_axis[i] * residual[i];
  }
  const double slope = (n * str - st_ * sr) / std::max(1e-12, n * stt - st_ * st_);
  const double icpt = (sr - slope * st_) / n;
  double sum2 = 0.0, prev = 0.0;
  for (size_t i = 0; i < residual.size(); ++i) {
    const double r = residual[i] - (icpt + slope * t_axis[i]);
    sum2 += r * r;
    out.jitter_max = std::max(out.jitter_max, std::fabs(r));
    if (i > 0 && t_axis[i] - t_axis[i - 1] < 1.5) {
      out.hop_max = std::max(out.hop_max, std::fabs(r - prev));
    }
    prev = r;
  }
  out.jitter_std = std::sqrt(sum2 / n);
  return out;
}

void CheckEdgeInMotion() {
  // 4K: a pixel is about 0.048 cm for every metre from the camera (a 55 degree field over 2160 rows), the camera 5 m behind the lamps: at 1080p twice that
  const auto pixel_cm = [](double recv_d) { return 0.0482 * (recv_d + 5.0); };
  const struct {
    const char* name;
    int tile;
    double fov;
  } kLevels[] = {{"medium 768", 768, 72.0}, {"high 1024", 1024, 72.0}, {"ultra 1280", 1280, 80.0}};
  for (const double recv_d : {9.0, 16.0, 30.0}) {
    const Crawl old_c = EdgeInMotion(true, 512, 84.0, 2.0, recv_d, true, 0.008, 60);
    Info("OLD (512, a box of 3), the lamp creeping 0.8 cm a frame across the beam, wall edge seen on the road " + Num(recv_d, 0) + " m out (texel " + Num(100 * old_c.texel, 2) + " cm): the edge's place jitters " + Num(100 * old_c.jitter_std, 2) +
         " cm (std), " + Num(100 * old_c.jitter_max, 2) + " cm at worst, one frame's jump " + Num(100 * old_c.hop_max, 2) + " cm = " + Num(100 * old_c.jitter_std / pixel_cm(recv_d), 1) + " px at 4K, " + Num(50 * old_c.jitter_std / pixel_cm(recv_d), 1) + " px at 1080p");
    for (const auto& lv : kLevels) {
      const Crawl c = EdgeInMotion(false, lv.tile, lv.fov, ls::kSlopeScaledDepthBias, recv_d, true, 0.008, 60);
      Info(std::string("NEW ") + lv.name + ", the same: texel " + Num(100 * c.texel, 2) + " cm = " + Num(c.texel * 100 / pixel_cm(recv_d), 1) + " px at 4K, " + Num(c.texel * 50 / pixel_cm(recv_d), 1) + " px at 1080p; the edge jitters " + Num(100 * c.jitter_std, 2) +
           " cm (std), " + Num(100 * c.jitter_max, 2) + " cm at worst, one frame's jump " + Num(100 * c.hop_max, 2) + " cm = " + Num(100 * c.jitter_std / pixel_cm(recv_d), 1) + " px (std) at 4K, " + Num(50 * c.jitter_std / pixel_cm(recv_d), 1) + " px at 1080p");
      Check(c.missing < 6, std::string("the edge is found in nearly every frame of the motion at ") + Num(recv_d, 0) + " m, " + lv.name + " (" + std::to_string(c.missing) + " missing)");
    }
  }
  // moving toward the wall (a car driving along the beam): the edge hardly moves on the road (1 mm a frame at 8 cm a frame of the lamp), so the map's quantisation is what is seen
  {
    const Crawl c = EdgeInMotion(false, 768, 72.0, ls::kSlopeScaledDepthBias, 16.0, false, 0.08, 60);
    Info("NEW medium 768, the lamp driving along the beam 8 cm a frame (4.8 m/s), edge 16 m out: jitter " + Num(100 * c.jitter_std, 2) + " cm (std), " + Num(100 * c.jitter_max, 2) + " cm at worst (texel " + Num(100 * c.texel, 2) + " cm)");
  }
}

}  // namespace

int main(int argc, char** argv) {
  g_debug = argc > 1;
  CheckPlane();
  CheckNoAcne();
  CheckThinPole();
  CheckKerb();
  CheckRoundingInMap();
  CheckContactHardening();
  CheckSynthetic();
  CheckLevelSizes();
  CheckAlphaToMask();
  CheckEdgeInMotion();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

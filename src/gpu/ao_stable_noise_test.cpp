// How steady the occlusion and the bounce light are, first version against the rules of ao_stable.h and ssgi_stable.h, on the C++ twin
// of the passes (ao_stable_twin.h): a road, a wall lit by the sun and a car, seen by a camera that stands still, at 3840x2160 (render
// scale 3, the owner's picture) and 1280x720. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when
// every check holds. About half a minute.
//
//   cmake --build <build dir> --target larecomp_ao_stable_noise_test        (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ao_stable_noise_test [--print]
//
// What it measures, per pixel of a small patch, in time over the frames the history has settled in: the occlusion's visibility, the
// picture's brightness (what the picture is multiplied by, the bounce light's factor included, with the apply's gain of the level
// DEFAULT: strength 2, limit 4.5, cap 2) and the bounce's own light; as a coefficient of variation (the standard deviation over the
// mean). A pixel that is still is steady to a code of the display (0.4% of the brightness) when the number is under 0.004.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ao_stable_twin.h"
#include "ssgi_stable.h"

using namespace mcla::gpu::ao_stable_twin;
using mcla::gpu::ao_halo::kDefaultCarReach;
using mcla::gpu::ao_halo::kSky;
using mcla::gpu::ao_halo::Params;
using mcla::gpu::ssgi_light::Rgb;
namespace ssgi_stable = mcla::gpu::ssgi_stable;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_print = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[300];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}

// ---------------------------------------------------------------------------
// the scene: the road (y = 0), a wall on the left that the sun lights, a car; the sun comes from the right
// ---------------------------------------------------------------------------

struct Box {
  double x0, y0, z0, x1, y1, z1;
  int kind;  // 0 the wall, 1 the car
};

struct Hit {
  double t = 1e30;
  int what = -2;  // -2 sky, -1 road, else the box
  double x = 0, y = 0, z = 0;
};

struct Rig {
  std::vector<Box> boxes;
  double cam_x = 0.0, cam_y = 2.0, cam_z = -7.5, pitch = -0.14;
  int width = 0, height = 0;
  double tan_x = 0.0, tan_y = 0.0;
  double fx = 0, fy = 0, fz = 0, rx = 0, ry = 0, rz = 0, ux = 0, uy = 0, uz = 0;
  double wall_light = 1.0;  // 0: the wall is in shade (the sun's light on it gone)
};

constexpr double kAmbient = 0.35;
constexpr double kSun = 1.0;

Hit Cast(const Rig& r, double px, double py) {
  const double u = px / double(r.width), v = py / double(r.height);
  const double a = (u * 2.0 - 1.0) * r.tan_x, b = (1.0 - v * 2.0) * r.tan_y;
  const double dx = r.rx * a + r.ux * b + r.fx, dy = r.ry * a + r.uy * b + r.fy, dz = r.rz * a + r.uz * b + r.fz;
  Hit h;
  if (dy < -1e-9) {
    const double t = -r.cam_y / dy;
    if (t > 0.0) {
      h.t = t;
      h.what = -1;
    }
  }
  for (size_t i = 0; i < r.boxes.size(); ++i) {
    const Box& bx = r.boxes[i];
    const double o[3] = {r.cam_x, r.cam_y, r.cam_z}, d[3] = {dx, dy, dz};
    const double lo[3] = {bx.x0, bx.y0, bx.z0}, hi[3] = {bx.x1, bx.y1, bx.z1};
    double t0 = 0.0, t1 = 1e30;
    bool miss = false;
    for (int k = 0; k < 3; ++k) {
      if (std::fabs(d[k]) < 1e-12) {
        if (o[k] < lo[k] || o[k] > hi[k]) {
          miss = true;
        }
        continue;
      }
      double p = (lo[k] - o[k]) / d[k], q = (hi[k] - o[k]) / d[k];
      if (p > q) {
        std::swap(p, q);
      }
      t0 = std::max(t0, p);
      t1 = std::min(t1, q);
    }
    if (!miss && t0 <= t1 && t0 > 0.0 && t0 < h.t) {
      h.t = t0;
      h.what = int(i);
    }
  }
  if (h.what != -2) {
    h.x = r.cam_x + dx * h.t;
    h.y = r.cam_y + dy * h.t;
    h.z = r.cam_z + dz * h.t;
  }
  return h;
}

// A hash in [-1, 1] of three integers (the surfaces' texture: a cell of a few centimetres).
double Hash(int a, int b, int c) {
  uint32_t h = uint32_t(a) * 374761393u + uint32_t(b) * 668265263u + uint32_t(c) * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return double(h & 0xFFFFFFu) / double(0xFFFFFFu) * 2.0 - 1.0;
}

// The scene colour of a hit (albedo times the light that falls on it) and the sun's term at it.
Rgb SceneColour(const Rig& r, const Hit& h, float* sun_term) {
  *sun_term = 0.0f;
  if (h.what == -2) {
    return {0.5f, 0.6f, 0.9f};
  }
  double alb[3];
  double lit = 0.0;
  if (h.what == -1) {
    const double tex = 1.0 + 0.30 * Hash(int(std::floor(h.x * 25)), int(std::floor(h.z * 25)), 1) + 0.15 * Hash(int(std::floor(h.x * 2)), int(std::floor(h.z * 2)), 2);
    alb[0] = alb[1] = alb[2] = 0.22 * tex;
    lit = h.x > -1.2 ? 0.9 : 0.0;  // the wall's shadow covers the road beside it
  } else if (r.boxes[size_t(h.what)].kind == 0) {
    const double tex = 1.0 + 0.25 * Hash(int(std::floor(h.z * 10)), int(std::floor(h.y * 10)), 3);
    alb[0] = 0.55 * tex;
    alb[1] = 0.18 * tex;
    alb[2] = 0.14 * tex;
    lit = 0.6 * r.wall_light;
  } else {
    alb[0] = alb[1] = alb[2] = 0.06;
    lit = 0.8;
  }
  *sun_term = float(kSun * lit);
  const double t = kAmbient + kSun * lit;
  return {float(alb[0] * t), float(alb[1] * t), float(alb[2] * t)};
}

void MakeRig(Rig* r, int scale) {
  r->boxes.clear();
  r->boxes.push_back({-0.95, 0.15, 0.0, 0.95, 0.75, 4.4, 1});  // the car's body
  r->boxes.push_back({-0.72, 0.75, 1.0, 0.72, 1.25, 3.1, 1});  // its cabin
  r->boxes.push_back({-4.0, 0.0, -5.0, -3.5, 4.0, 40.0, 0});   // the wall
  r->width = 1280 * scale;
  r->height = 720 * scale;
  r->tan_y = std::tan(30.0 * 3.14159265358979 / 180.0);
  r->tan_x = r->tan_y * double(r->width) / double(r->height);
  r->fx = 0.0;
  r->fy = std::sin(r->pitch);
  r->fz = std::cos(r->pitch);
  r->rx = r->fz;
  r->ry = 0.0;
  r->rz = -r->fx;
  const double rl = std::sqrt(r->rx * r->rx + r->rz * r->rz);
  r->rx /= rl;
  r->rz /= rl;
  r->ux = r->fy * r->rz - r->fz * r->ry;
  r->uy = r->fz * r->rx - r->fx * r->rz;
  r->uz = r->fx * r->ry - r->fy * r->rx;
}

bool PixelOf(const Rig& r, double x, double y, double z, int* px, int* py) {
  const double dx = x - r.cam_x, dy = y - r.cam_y, dz = z - r.cam_z;
  const double zc = dx * r.fx + dy * r.fy + dz * r.fz;
  if (zc <= 0.1) {
    return false;
  }
  const double xc = dx * r.rx + dy * r.ry + dz * r.rz, yc = dx * r.ux + dy * r.uy + dz * r.uz;
  *px = int(std::floor((xc / (zc * r.tan_x) * 0.5 + 0.5) * double(r.width)));
  *py = int(std::floor((0.5 - yc / (zc * r.tan_y) * 0.5) * double(r.height)));
  return true;
}

const Params& AoParams() {
  static Params p = [] {
    Params q;
    q.ratio_sum = true;
    q.car_reach = kDefaultCarReach;
    q.slices = 3;
    q.steps = 3;
    return q;
  }();
  return p;
}

Chains MakeChains(const Rig& rig) {
  ChainInput in;
  in.width = rig.width;
  in.height = rig.height;
  in.tan_y = float(rig.tan_y);
  in.radiance_cap = std::max(2.0f * float(kAmbient + kSun), 0.5f);
  in.depth = [&rig](int x, int y) {
    const Hit h = Cast(rig, x + 0.5, y + 0.5);
    return h.what == -2 ? kSky : float(h.t);
  };
  in.colour = [&rig](int x, int y) {
    const Hit h = Cast(rig, x + 0.5, y + 0.5);
    float sun_term = 0.0f;
    return SceneColour(rig, h, &sun_term);
  };
  in.vehicle = [&rig](int x, int y) {
    const Hit h = Cast(rig, x + 0.5, y + 0.5);
    return h.what >= 0 && rig.boxes[size_t(h.what)].kind == 1;
  };
  return BuildChains(in, AoParams());
}

struct Spot {
  const char* name;
  double x, y, z;
};
const Spot kSpots[] = {
    {"shaded road beside the lit wall, 8 m", -2.8, 0.0, 8.0},
    {"road in shade, 15 m", -2.5, 0.0, 15.0},
    {"the lit wall, low, 8 m", -3.5, 0.5, 8.0},
    {"sunlit road beside the car, 2 m", 1.5, 0.0, 2.0},
};

// ---------------------------------------------------------------------------
// the run of one spot under two sets of rules
// ---------------------------------------------------------------------------

struct Measured {
  SeriesStat final_picture;   // the picture's brightness with the history the rules give
  SeriesStat final_alone;     // the same with no history at all (a frame that stands alone)
  SeriesStat bounce;          // the light the bounce adds
  SeriesStat visibility;      // the accumulated visibility
  SeriesStat visibility_alone;// the visibility after the blur, one frame
  double early[3] = {0, 0, 0};// the error of the mean of the first 4, 8 and 12 frames' blurred visibility against the settled mean
};

Measured Measure(const Rig& rig, const Chains& chains, const Spot& spot, const Rules& rules, int atrous_passes, int frames, int patch) {
  Measured m;
  int cx = 0, cy = 0;
  PixelOf(rig, spot.x, spot.y, spot.z, &cx, &cy);
  const int border = RulesBorder(rules, atrous_passes);
  const Region reg{cx - patch / 2 - border, cy - patch / 2 - border, patch + 2 * border, patch + 2 * border};
  const Geometry geo = BuildGeometry(chains, reg);
  GiParams gp;
  gp.ao = AoParams();
  gp.far_reach = 9.0f;
  gp.far_steps = 2;
  const RawFrames raw = ComputeRaw(chains, gp, reg, frames, rules.noise_lattice);
  const std::function<ApplyPixel(int, int)> pixel_of = [&rig](int x, int y) {
    const Hit h = Cast(rig, x + 0.5, y + 0.5);
    ApplyPixel ap;
    float sun_term = 0.0f;
    const Rgb col = SceneColour(rig, h, &sun_term);
    ap.colour = mcla::gpu::ssgi_light::CapRadiance(col, std::max(2.0f * float(kAmbient + kSun), 0.5f));
    ap.sun_term = sun_term;
    return ap;
  };
  const ApplyLook look;
  const int settled = frames / 2;
  const PatchSeries with = RunPatch(geo, raw, rules, atrous_passes, cx, cy, patch, pixel_of, look, 0);
  const PatchSeries alone = RunPatch(geo, raw, rules, atrous_passes, cx, cy, patch, pixel_of, look, 1);
  m.final_picture = Stat(with.final_luma, settled, frames);
  m.final_alone = Stat(alone.final_luma, settled, frames);
  m.bounce = Stat(with.bounce_add, settled, frames);
  m.visibility = Stat(with.accumulated, settled, frames);
  m.visibility_alone = Stat(alone.denoised, settled, frames);
  // The early error: the mean of the blurred visibility over the first w frames after a start, against the mean over the settled frames.
  const int windows[3] = {4, 8, 12};
  for (int wi = 0; wi < 3; ++wi) {
    double sum = 0.0;
    int count = 0;
    for (const auto& series : with.denoised) {
      double settled_mean = 0.0;
      for (int i = settled; i < frames; ++i) {
        settled_mean += series[size_t(i)];
      }
      settled_mean /= double(frames - settled);
      for (int start = 0; start + windows[wi] <= settled; ++start) {
        double wm = 0.0;
        for (int i = 0; i < windows[wi]; ++i) {
          wm += series[size_t(start + i)];
        }
        wm /= double(windows[wi]);
        sum += (wm - settled_mean) * (wm - settled_mean);
        ++count;
      }
    }
    m.early[wi] = std::sqrt(sum / double(std::max(count, 1)));
  }
  return m;
}

// ---------------------------------------------------------------------------
// the checks
// ---------------------------------------------------------------------------

void CheckStillCamera4K() {
  Rig rig;
  MakeRig(&rig, 3);
  const Chains chains = MakeChains(rig);
  const Rules first = FirstRules();
  const Rules stable = StableRules(uint32_t(rig.height));
  const int first_passes = first.atrous;
  const int stable_passes = ssgi_stable::AtrousIterations(2, uint32_t(rig.height));
  Check(stable.radius == 3 && stable.wide == 4 && stable_passes == 4 && stable.noise_lattice == 13, "at 2160 lines the rules are the 7x7 blur, the 9x9 wide blur, 4 a-trous passes and the lattice");
  double ratio_final = 0.0, ratio_alone = 0.0, ratio_bounce = 0.0, ratio_vis = 0.0, ratio_vis_alone = 0.0;
  double early[3] = {0, 0, 0};
  int n = 0;
  for (const Spot& spot : kSpots) {
    const int frames = 40;
    const Measured a = Measure(rig, chains, spot, first, first_passes, frames, 4);
    const Measured b = Measure(rig, chains, spot, stable, stable_passes, frames, 4);
    if (g_print) {
      std::printf("%-40s 3840x2160  first version -> now\n", spot.name);
      std::printf("   the picture, history of 12 and 16:   cv %.4f -> %.4f   (frame to frame %.4f -> %.4f, mean brightness %.3f)\n", a.final_picture.cv, b.final_picture.cv,
                  a.final_picture.change, b.final_picture.change, a.final_picture.mean);
      std::printf("   the picture, a frame with no history: cv %.4f -> %.4f\n", a.final_alone.cv, b.final_alone.cv);
      std::printf("   the bounce's light, with history:     cv %.4f -> %.4f   (mean %.4f)\n", a.bounce.cv, b.bounce.cv, a.bounce.mean);
      std::printf("   the visibility, accumulated:          cv %.5f -> %.5f; one blurred frame: %.4f -> %.4f\n", a.visibility.cv, b.visibility.cv, a.visibility_alone.cv, b.visibility_alone.cv);
      std::printf("   error of the visibility's mean after 4, 8, 12 frames: %.4f %.4f %.4f -> %.4f %.4f %.4f\n", a.early[0], a.early[1], a.early[2], b.early[0], b.early[1], b.early[2]);
    }
    // Every place: steadier, and the picture's flicker under the display's code (0.4%).
    Check(b.final_picture.cv < 0.004, std::string(spot.name) + Num(": the picture on a still camera flickers by %.4f (under 0.004)", b.final_picture.cv));
    Check(b.final_picture.cv <= a.final_picture.cv, std::string(spot.name) + ": the picture is no less steady");
    Check(b.final_alone.cv <= 0.6 * a.final_alone.cv, std::string(spot.name) + Num(": a frame with no history flickers %.0f%% of what it did", 100.0 * b.final_alone.cv / std::max(a.final_alone.cv, 1e-9)));
    Check(b.visibility.cv <= 0.7 * a.visibility.cv + 1e-4, std::string(spot.name) + ": the accumulated visibility is steadier");
    const double kEarlyFrames[3] = {4.0, 8.0, 12.0};
    for (int w = 0; w < 3; ++w) {
      Check(b.early[w] <= 0.9 * a.early[w] + 2e-4, std::string(spot.name) + Num(": the mean of the first %.0f frames after a cut is nearer to the settled one", kEarlyFrames[w]));
    }
    ratio_final += b.final_picture.cv / std::max(a.final_picture.cv, 1e-9);
    ratio_alone += b.final_alone.cv / std::max(a.final_alone.cv, 1e-9);
    ratio_bounce += b.bounce.cv / std::max(a.bounce.cv, 1e-9);
    ratio_vis += b.visibility.cv / std::max(a.visibility.cv, 1e-9);
    ratio_vis_alone += b.visibility_alone.cv / std::max(a.visibility_alone.cv, 1e-9);
    for (int w = 0; w < 3; ++w) {
      early[w] += b.early[w] / std::max(a.early[w], 1e-9);
    }
    ++n;
  }
  ratio_final /= n;
  ratio_alone /= n;
  ratio_bounce /= n;
  ratio_vis /= n;
  ratio_vis_alone /= n;
  for (double& e : early) {
    e /= n;
  }
  if (g_print) {
    std::printf("mean ratio now / first version at 3840x2160: picture %.2f, picture without history %.2f, bounce %.2f, visibility %.2f (one blurred frame %.2f), early error %.2f %.2f %.2f\n",
                ratio_final, ratio_alone, ratio_bounce, ratio_vis, ratio_vis_alone, early[0], early[1], early[2]);
  }
  Check(ratio_final <= 0.3, Num("on average the picture's flicker on a still camera at 4K is %.2f of the first version's (0.3 at most)", ratio_final));
  Check(ratio_alone <= 0.4, Num("on average a frame with no history flickers %.2f of what it did at 4K (0.4 at most)", ratio_alone));
  Check(ratio_bounce <= 0.5, Num("on average the bounce's light flickers %.2f of what it did at 4K (0.5 at most)", ratio_bounce));
  Check(ratio_vis <= 0.4, Num("on average the accumulated visibility flickers %.2f of what it did at 4K (0.4 at most)", ratio_vis));
  Check(ratio_vis_alone <= 0.4, Num("on average one blurred frame of the visibility flickers %.2f of what it did at 4K (0.4 at most)", ratio_vis_alone));
  for (int w = 0; w < 3; ++w) {
    Check(early[w] <= 0.7, Num("on average the mean of the first frames after a cut is within %.2f of the old error at 4K (0.7 at most)", early[w]));
  }
}

void CheckStillCamera720() {
  // At 720 lines the blur, the wide blur and the a-trous passes are the first version's (the rules differ only in the lattice and the clip box).
  Rig rig;
  MakeRig(&rig, 1);
  const Chains chains = MakeChains(rig);
  const Rules first = FirstRules();
  const Rules stable = StableRules(uint32_t(rig.height));
  Check(stable.radius == first.radius && stable.wide == first.wide && ssgi_stable::AtrousIterations(2, uint32_t(rig.height)) == first.atrous,
        "at 720 lines the blur, the wide blur and the a-trous passes are the first version's");
  double ratio_final = 0.0, ratio_bounce = 0.0;
  int n = 0;
  for (const Spot& spot : kSpots) {
    const Measured a = Measure(rig, chains, spot, first, first.atrous, 48, 6);
    const Measured b = Measure(rig, chains, spot, stable, ssgi_stable::AtrousIterations(2, uint32_t(rig.height)), 48, 6);
    if (g_print) {
      std::printf("%-40s 1280x720  first version -> now: picture cv %.4f -> %.4f, bounce %.4f -> %.4f, visibility %.5f -> %.5f, early %.4f %.4f %.4f -> %.4f %.4f %.4f\n", spot.name,
                  a.final_picture.cv, b.final_picture.cv, a.bounce.cv, b.bounce.cv, a.visibility.cv, b.visibility.cv, a.early[0], a.early[1], a.early[2], b.early[0], b.early[1],
                  b.early[2]);
    }
    Check(b.final_picture.cv <= a.final_picture.cv * 1.05 + 1e-4, std::string(spot.name) + ": at 720 lines the picture is no less steady");
    Check(b.bounce.cv <= 0.7 * a.bounce.cv + 1e-3, std::string(spot.name) + Num(": at 720 lines the bounce's light flickers %.2f of what it did", b.bounce.cv / std::max(a.bounce.cv, 1e-9)));
    ratio_final += b.final_picture.cv / std::max(a.final_picture.cv, 1e-9);
    ratio_bounce += b.bounce.cv / std::max(a.bounce.cv, 1e-9);
    ++n;
  }
  if (g_print) {
    std::printf("mean ratio now / first version at 1280x720: picture %.2f, bounce %.2f\n", ratio_final / n, ratio_bounce / n);
  }
  Check(ratio_final / n <= 0.8, Num("on average the picture at 720 lines flickers %.2f of what it did (0.8 at most)", ratio_final / n));
}

// A light that changes in one frame: the wall goes into shade (a car went by) after frame `step`; how many frames the bounce on the road
// beside it takes to follow 50% and 90% of the way.
void CheckLightChange() {
  Rig rig;
  MakeRig(&rig, 1);
  const Chains before = MakeChains(rig);
  rig.wall_light = 0.0;
  const Chains after = MakeChains(rig);
  rig.wall_light = 1.0;
  const Spot spot = kSpots[0];
  int cx = 0, cy = 0;
  PixelOf(rig, spot.x, spot.y, spot.z, &cx, &cy);
  const int frames = 64, step = 24, patch = 6;
  int f50[2] = {-1, -1}, f90[2] = {-1, -1};
  double noise[2] = {0, 0};
  for (int which = 0; which < 2; ++which) {
    const Rules rules = which == 0 ? FirstRules() : StableRules(720);
    const int passes = which == 0 ? rules.atrous : ssgi_stable::AtrousIterations(2, 720);
    const int border = RulesBorder(rules, passes);
    const Region reg{cx - patch / 2 - border, cy - patch / 2 - border, patch + 2 * border, patch + 2 * border};
    const Geometry geo = BuildGeometry(before, reg);
    GiParams gp;
    gp.ao = AoParams();
    gp.far_reach = 9.0f;
    gp.far_steps = 2;
    RawFrames raw(size_t(frames), std::vector<Px4>(reg.Count()));
    for (int nfr = 0; nfr < frames; ++nfr) {
      gp.noise_lattice = rules.noise_lattice;
      for (int y = 0; y < reg.h; ++y) {
        for (int x = 0; x < reg.w; ++x) {
          const GiOut o = GiMain(nfr < step ? before : after, gp, reg.x0 + x, reg.y0 + y, nfr);
          raw[size_t(nfr)][size_t(y) * size_t(reg.w) + size_t(x)] = {o.visibility, o.bounce};
        }
      }
    }
    const std::function<ApplyPixel(int, int)> pixel_of = [&rig](int x, int y) {
      const Hit h = Cast(rig, x + 0.5, y + 0.5);
      ApplyPixel ap;
      float sun_term = 0.0f;
      ap.colour = mcla::gpu::ssgi_light::CapRadiance(SceneColour(rig, h, &sun_term), std::max(2.0f * float(kAmbient + kSun), 0.5f));
      ap.sun_term = sun_term;
      return ap;
    };
    const PatchSeries s = RunPatch(geo, raw, rules, passes, cx, cy, patch, pixel_of, ApplyLook{}, 0);
    std::vector<double> mean(size_t(frames), 0.0);
    for (int f = 0; f < frames; ++f) {
      for (const auto& v : s.bounce_add) {
        mean[size_t(f)] += v[size_t(f)];
      }
      mean[size_t(f)] /= double(s.bounce_add.size());
    }
    double was = 0.0, now = 0.0;
    for (int f = step - 6; f < step; ++f) {
      was += mean[size_t(f)] / 6.0;
    }
    for (int f = frames - 6; f < frames; ++f) {
      now += mean[size_t(f)] / 6.0;
    }
    for (int f = step; f < frames; ++f) {
      const double share = (was - mean[size_t(f)]) / std::max(was - now, 1e-9);
      if (f50[which] < 0 && share >= 0.5) {
        f50[which] = f - step;
      }
      if (f90[which] < 0 && share >= 0.9) {
        f90[which] = f - step;
      }
    }
    // The noise before the change: the bounce's flicker over the settled frames.
    noise[which] = Stat(s.bounce_add, 12, step).cv;
  }
  if (g_print) {
    std::printf("the light on the wall goes out (720 lines): the bounce on the road beside it follows 50%% in %d -> %d frames, 90%% in %d -> %d frames (first version -> now); its flicker before %.4f -> %.4f\n",
                f50[0], f50[1], f90[0], f90[1], noise[0], noise[1]);
  }
  Check(f50[1] >= 0 && f50[1] <= 3, Num("a light that went out is half gone from the bounce within 3 frames (got %.0f)", double(f50[1])));
  Check(f90[1] >= 0 && f90[1] <= 14, Num("a light that went out is 90%% gone from the bounce within 14 frames (got %.0f)", double(f90[1])));
  Check(noise[1] <= 0.5 * noise[0], Num("the bounce's flicker before the change is %.2f of the first version's (0.5 at most)", noise[1] / std::max(noise[0], 1e-9)));
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    }
  }
  CheckStillCamera720();
  CheckLightChange();
  CheckStillCamera4K();
  std::printf("ao_stable_noise_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

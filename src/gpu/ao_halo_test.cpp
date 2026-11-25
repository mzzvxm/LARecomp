// Check of the occlusion's reach round vehicles and of its slice sum (ao_halo.h) and of its C++ twin of the shader's main pass. A
// program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ao_halo_test          (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ao_halo_test [<src/gpu dir>]
//
// With the src/gpu folder as its argument it also checks that the shaders' and ao.cpp's constants are the header's (they repeat them).
// With --print it lists the profiles the checks are made on (the numbers ao_halo.h quotes).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ao_halo.h"
#include "setting_source.h"

using namespace mcla::gpu::ao_halo;

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
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char buf[240];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c);
  return buf;
}

// ---------------------------------------------------------------------------
// synthetic scenes: the road (y = 0) and boxes, seen from a camera behind and above, as the chase camera sees a car
// ---------------------------------------------------------------------------

struct Box {
  double x0, y0, z0, x1, y1, z1;
  bool vehicle;
};

struct World {
  std::vector<Box> boxes;
  double cam_x = 0.0, cam_y = 2.0, cam_z = -7.5;
  double pitch = -0.14;  // radians, looking down
};

struct Rig {
  World world;
  Scene scene;
  double fx, fy, fz, rx, ry, rz, ux, uy, uz;  // the camera's forward, right and up
};

// The depth of the pixel (x, y), and the index of the box it shows (-1 the road, -2 the sky).
double DepthAndHit(const Rig& r, int x, int y, int* hit) {
  const double u = (double(x) + 0.5) / double(r.scene.width);
  const double v = (double(y) + 0.5) / double(r.scene.height);
  const double a = (u * 2.0 - 1.0) * double(r.scene.tan_x);
  const double b = (1.0 - v * 2.0) * double(r.scene.tan_y);
  const double dx = r.rx * a + r.ux * b + r.fx;
  const double dy = r.ry * a + r.uy * b + r.fy;
  const double dz = r.rz * a + r.uz * b + r.fz;  // the parameter t along d is the view depth (d's component along forward is 1)
  double best = 1e30;
  int what = -2;
  if (dy < -1e-9) {
    const double t = -r.world.cam_y / dy;
    if (t > 0.0) {
      best = t;
      what = -1;
    }
  }
  for (size_t i = 0; i < r.world.boxes.size(); ++i) {
    const Box& bx = r.world.boxes[i];
    const double o[3] = {r.world.cam_x, r.world.cam_y, r.world.cam_z};
    const double d[3] = {dx, dy, dz};
    const double lo[3] = {bx.x0, bx.y0, bx.z0};
    const double hi[3] = {bx.x1, bx.y1, bx.z1};
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
    if (!miss && t0 <= t1 && t0 > 0.0 && t0 < best) {
      best = t0;
      what = int(i);
    }
  }
  if (hit) {
    *hit = what;
  }
  return what == -2 ? double(kSky) : best;
}

void MakeRig(Rig* r, const World& w) {
  r->world = w;
  r->scene.width = 1280;
  r->scene.height = 720;
  r->scene.tan_y = float(std::tan(30.0 * 3.14159265358979 / 180.0));
  r->scene.tan_x = r->scene.tan_y * 1280.0f / 720.0f;
  r->fx = 0.0;
  r->fy = std::sin(w.pitch);
  r->fz = std::cos(w.pitch);
  // right = normalize(cross(world up, forward)), up = cross(forward, right)
  r->rx = r->fz;
  r->ry = 0.0;
  r->rz = -r->fx;
  const double rl = std::sqrt(r->rx * r->rx + r->rz * r->rz);
  r->rx /= rl;
  r->rz /= rl;
  r->ux = r->fy * r->rz - r->fz * r->ry;
  r->uy = r->fz * r->rx - r->fx * r->rz;
  r->uz = r->fx * r->ry - r->fy * r->rx;
  Rig* rig = r;
  r->scene.depth = [rig](int x, int y) { return float(DepthAndHit(*rig, x, y, nullptr)); };
  r->scene.vehicle = [rig](int x, int y) {
    int hit = -2;
    DepthAndHit(*rig, x, y, &hit);
    return hit >= 0 && rig->world.boxes[size_t(hit)].vehicle;
  };
}

// The pixel the road point (x, 0, z) is seen at; false when it is off the picture or covered.
bool PixelOf(const Rig& r, double x, double y, double z, int* px, int* py, bool must_be_road) {
  const double dx = x - r.world.cam_x, dy = y - r.world.cam_y, dz = z - r.world.cam_z;
  const double zc = dx * r.fx + dy * r.fy + dz * r.fz;
  if (zc <= 0.1) {
    return false;
  }
  const double xc = dx * r.rx + dy * r.ry + dz * r.rz;
  const double yc = dx * r.ux + dy * r.uy + dz * r.uz;
  const double u = (xc / (zc * double(r.scene.tan_x)) * 0.5 + 0.5) * r.scene.width;
  const double v = (0.5 - yc / (zc * double(r.scene.tan_y)) * 0.5) * r.scene.height;
  *px = int(std::floor(u));
  *py = int(std::floor(v));
  if (*px < 0 || *py < 0 || *px >= r.scene.width || *py >= r.scene.height) {
    return false;
  }
  if (must_be_road) {
    int hit = -2;
    DepthAndHit(r, *px, *py, &hit);
    return hit == -1;
  }
  return true;
}

// The mean visibility over `frames` frames (the occlusion's own accumulation) of the pixel the point is seen at.
double MeanVisibility(const Rig& r, const Params& p, int px, int py, int frames) {
  double sum = 0.0;
  for (int f = 0; f < frames; ++f) {
    sum += double(Visibility(r.scene, p, px, py, f));
  }
  return sum / double(frames);
}

World CarWorld() {
  World w;
  // A car, 1.9 m wide and 4.4 m long: the body up to 0.75 m and a cabin set back, the rear bumper at z = 0 (the camera is at -7.5).
  w.boxes.push_back({-0.95, 0.15, 0.0, 0.95, 0.75, 4.4, true});
  w.boxes.push_back({-0.72, 0.75, 1.0, 0.72, 1.25, 3.1, true});
  return w;
}

Params Legacy() {
  Params p;
  p.ratio_sum = false;
  p.car_reach = 0.0f;
  return p;
}
Params Fixed() {
  Params p;
  p.ratio_sum = true;
  p.car_reach = kDefaultCarReach;
  return p;
}

// ---------------------------------------------------------------------------
// the checks
// ---------------------------------------------------------------------------

void CheckRules() {
  // The falloff: full out to radius * (1 - ratio), 0 at the radius, falling in between.
  Check(Falloff(0.0f, 3.0f, 0.615f) == 1.0f, "falloff 1 at the pixel");
  Check(std::fabs(Falloff(3.0f * (1.0f - 0.615f), 3.0f, 0.615f) - 1.0f) < 1e-5f, "falloff still 1 where the ramp starts");
  Check(Falloff(3.0f, 3.0f, 0.615f) == 0.0f && Falloff(10.0f, 3.0f, 0.615f) == 0.0f, "falloff 0 at the radius and past it");
  bool monotone = true;
  for (float d = 0.0f; d < 3.2f; d += 0.05f) {
    monotone = monotone && Falloff(d + 0.05f, 3.0f, 0.615f) <= Falloff(d, 3.0f, 0.615f) + 1e-6f;
  }
  Check(monotone, "the falloff never grows with the distance");

  // A car's samples: the ramp over 0.4 of the radius, so 1.2 m of 3 m, full to 0.46 m.
  const float r = 3.0f, ratio = 0.615f, share = kDefaultCarReach;
  Check(VehicleFalloff(0.4f, r, ratio, share) == 1.0f, "a car's sample 0.4 m away counts in full");
  Check(VehicleFalloff(1.2f, r, ratio, share) < 1e-5f && VehicleFalloff(2.0f, r, ratio, share) == 0.0f,
        "a car's sample 1.2 m away or more does not count");
  Check(VehicleFalloff(0.8f, r, ratio, share) > 0.0f && VehicleFalloff(0.8f, r, ratio, share) < 1.0f, "a car's sample 0.8 m away counts in part");
  // Which sample gets which weight.
  const float world = Falloff(1.5f, r, ratio);
  Check(world > 0.5f, "a wall's sample 1.5 m away counts in part (the world's falloff)");
  Check(SampleWeight(1.5f, r, ratio, share, false, false) == world, "not a vehicle: the world's falloff");
  Check(SampleWeight(1.5f, r, ratio, share, true, true) == world, "a vehicle seen from a vehicle (the body's own creases): the world's falloff");
  Check(SampleWeight(1.5f, r, ratio, share, true, false) == 0.0f, "a vehicle seen from the road: 1.5 m is past its reach");
  Check(SampleWeight(0.3f, r, ratio, share, true, false) == 1.0f, "a vehicle 0.3 m from the road: in full");
  Check(SampleWeight(1.5f, r, ratio, 0.0f, true, false) == world && SampleWeight(1.5f, r, ratio, 1.0f, true, false) == world,
        "a reach of 0 or 1 is no limit");
  Check(!CarReachLimits(0.0f) && !CarReachLimits(1.0f) && CarReachLimits(0.4f) && CarReachLimits(0.01f), "which reaches limit");
  // The reach scales with the radius (the intensity looks: 2.5, 3, 3.5, 3.75 m).
  Check(std::fabs(share * 2.5f - 1.0f) < 1e-5f && std::fabs(share * 3.75f - 1.5f) < 1e-5f, "the reach in metres at the lightest and the strongest look");

  // The vehicle bit costs one unit in the last place, whatever the depth, and reads back.
  bool bit_ok = true;
  bool ulp_ok = true;
  for (float t = 0.05f; t < 60000.0f; t *= 1.37f) {
    const float on = SetVehicleBit(t, true), off = SetVehicleBit(t, false);
    bit_ok = bit_ok && IsVehicle(on) && !IsVehicle(off) && SetVehicleBit(on, false) == off && SetVehicleBit(off, true) == on;
    ulp_ok = ulp_ok && std::fabs(on - t) <= t * 1.3e-7f && std::fabs(off - t) <= t * 1.3e-7f;
  }
  Check(bit_ok, "the vehicle bit sets, clears and reads back");
  Check(ulp_ok, "the vehicle bit changes the depth by at most one unit in the last place");
  Check(std::fabs(SetVehicleBit(kSky, true) - kSky) < 0.01f && std::fabs(SetVehicleBit(kSky, false) - kSky) < 0.01f, "the sky's depth stays the sky's");

  // The stencil: IDs 1 to 23 are vehicles (the byte holds the ID in its upper six bits), 0 and 24 up are not.
  Check(!IsVehicleStencil(0) && IsVehicleStencil(1u << 2) && IsVehicleStencil(23u << 2) && !IsVehicleStencil(24u << 2) && !IsVehicleStencil(63u << 2),
        "the stencil IDs of vehicles");
  Check(IsVehicleStencil((5u << 2) | 3u), "the stencil's low two bits (the game's other flags) do not matter");

  // The ratio: an open surface is 1 for any slices, the first version's sum over the number of slices is not.
  const float plen[3] = {0.9f, 0.5f, 0.2f};
  const float cos_n[3] = {0.95f, 0.8f, 0.4f};
  const float n[3] = {0.3f, -0.6f, 1.1f};
  float open = 0.0f;
  for (int i = 0; i < 3; ++i) {
    open += OpenSlice(plen[i], cos_n[i], n[i]);
  }
  Check(std::fabs(SliceVisibility(open, open, 3, true) - 1.0f) < 1e-6f, "an open surface is exactly 1 with the ratio");
  Check(std::fabs(SliceVisibility(open, open, 3, false) - 1.0f) > 0.05f, "an open surface is not 1 with the number of slices (the first version)");
  Check(SliceVisibility(0.0f, 0.0f, 3, true) == 0.0f && SliceVisibility(5.0f, 0.0f, 3, true) == 1.0f, "no slices of any length: no division by zero, clamped");
}

void CheckOpenRoad() {
  // Nothing on the road: the visibility is 1, to a few per cent that sampling noise leaves; it was 4 to 9% dark.
  World w;
  Rig rig;
  MakeRig(&rig, w);
  const int frames = 40;
  const double zs[] = {-3.0, 0.0, 3.0, 8.0, 15.0, 30.0};
  double worst_fixed = 1.0, worst_legacy = 1.0, sum_gain = 0.0;
  int shown = 0;
  for (double z : zs) {
    int px = 0, py = 0;
    if (!PixelOf(rig, 0.0, 0.0, z, &px, &py, true)) {
      continue;
    }
    const double f = MeanVisibility(rig, Fixed(), px, py, frames);
    const double l = MeanVisibility(rig, Legacy(), px, py, frames);
    if (g_print) {
      std::printf("open road z=%5.1f depth %5.1f: first version %.3f  now %.3f\n", z, double(rig.scene.depth(px, py)), l, f);
    }
    worst_fixed = std::min(worst_fixed, f);
    worst_legacy = std::min(worst_legacy, l);
    sum_gain += f - l;
    ++shown;
  }
  Check(shown >= 5, Num("open road: %.0f pixels measured (5 or more)", shown));
  Check(worst_fixed >= 0.955, Num("open road with the fix: the darkest point is %.3f (0.955 or more)", worst_fixed));
  Check(worst_legacy <= 0.935, Num("open road, first version: the darkest point is %.3f (it must show the 4 to 9%% dark road, 0.935 or less)", worst_legacy));
  Check(sum_gain / std::max(shown, 1) > 0.025, Num("open road: the fix lifts the mean by %.3f (0.025 or more)", sum_gain / std::max(shown, 1)));
}

void CheckCarHalo() {
  const World w = CarWorld();
  Rig rig;
  MakeRig(&rig, w);
  const int frames = 48;
  // The road behind the rear bumper, going towards the camera: the car is a wall of 0.75 m there.
  const double ds[] = {0.05, 0.2, 0.4, 0.6, 0.8, 1.0, 1.25, 1.5, 2.0};
  double legacy[9] = {}, fixed[9] = {};
  bool all_seen = true;
  for (int i = 0; i < 9; ++i) {
    int px = 0, py = 0;
    if (!PixelOf(rig, 0.0, 0.0, -ds[i], &px, &py, true)) {
      all_seen = false;
      continue;
    }
    legacy[i] = MeanVisibility(rig, Legacy(), px, py, frames);
    fixed[i] = MeanVisibility(rig, Fixed(), px, py, frames);
    if (g_print) {
      std::printf("behind the rear bumper %.2f m: first version %.3f  now %.3f\n", ds[i], legacy[i], fixed[i]);
    }
  }
  Check(all_seen, "the road behind the car is seen at every distance of the profile");
  // The halo of the first version: 19% dark at 0.6 m, 13% at 0.8 m, 10% at 1 m.
  Check(legacy[3] <= 0.84 && legacy[4] <= 0.90 && legacy[5] <= 0.92, Num("first version: 0.6 m %.3f, 0.8 m %.3f (0.84 and 0.90 or less): a halo to show", legacy[3], legacy[4]));
  // Now: close to the car it is as dark as it was (the dark line where the car meets the road), past 0.6 m it is gone.
  Check(fixed[0] <= 0.45, Num("the dark line at 5 cm stays: %.3f (0.45 or less)", fixed[0]));
  Check(fixed[1] <= 0.75, Num("20 cm from the car it is still dark: %.3f (0.75 or less)", fixed[1]));
  Check(fixed[3] >= 0.90, Num("0.6 m from the car: %.3f (0.90 or more)", fixed[3]));
  Check(fixed[5] >= 0.95 && fixed[6] >= 0.95 && fixed[7] >= 0.95, Num("1 m, 1.25 m and 1.5 m from the car: %.3f, %.3f (0.95 or more)", fixed[5], fixed[6]));
  bool never_darker = true;
  for (int i = 0; i < 9; ++i) {
    never_darker = never_darker && fixed[i] >= legacy[i] - 0.02;
  }
  Check(never_darker, "the fix is nowhere darker than the first version (within the noise of the frames)");

  // The road beside a car with the car's side seen along the screen: the point 0.6 m and 1 m to the side of its front.
  const World side = CarWorld();
  Rig rig_side;
  MakeRig(&rig_side, side);
  int px = 0, py = 0;
  if (PixelOf(rig_side, 1.55, 0.0, 2.2, &px, &py, true)) {
    const double l = MeanVisibility(rig_side, Legacy(), px, py, frames);
    const double f = MeanVisibility(rig_side, Fixed(), px, py, frames);
    if (g_print) {
      std::printf("beside the car 0.6 m: first version %.3f  now %.3f\n", l, f);
    }
    Check(f >= l - 0.02, Num("beside the car 0.6 m: now %.3f, not darker than the first version %.3f", f, l));
  }
}

void CheckWallUnchanged() {
  // A wall of a building (not a vehicle): the road at its foot is as dark as before, the fix only lifts its far tail.
  World w;
  w.boxes.push_back({2.0, 0.0, -30.0, 3.0, 8.0, 40.0, false});
  Rig rig;
  MakeRig(&rig, w);
  const int frames = 48;
  const double ds[] = {0.1, 0.25, 0.5, 1.0, 2.0, 3.0};
  for (double d : ds) {
    int px = 0, py = 0;
    if (!PixelOf(rig, 2.0 - d, 0.0, 2.0, &px, &py, true)) {
      Check(false, Num("the road %.2f m from the wall is not seen", d));
      continue;
    }
    // The same slice sum on both sides, so that only the reach of cars differs: a wall is not a vehicle.
    Params with_reach = Fixed();
    Params without = Fixed();
    without.car_reach = 0.0f;
    const double a = MeanVisibility(rig, with_reach, px, py, frames);
    const double b = MeanVisibility(rig, without, px, py, frames);
    if (g_print) {
      std::printf("wall, %.2f m from its face: no car limit %.3f, with the car limit %.3f (first version %.3f)\n", d, b, a,
                  MeanVisibility(rig, Legacy(), px, py, frames));
    }
    Check(std::fabs(a - b) < 1e-9, Num("a wall is not a vehicle: the car limit changes nothing at %.2f m", d));
    if (d <= 0.25) {
      Check(a <= 0.52, Num("the foot of a wall stays dark (%.2f m: %.3f, 0.52 or less)", d, a));
    }
  }
}

void CheckCarBodyUnchanged() {
  // A pixel that is a vehicle sees everything at the full radius: the crease between the trunk and the cabin's rear wall.
  const World w = CarWorld();
  Rig rig;
  MakeRig(&rig, w);
  int px = 0, py = 0;
  const bool seen = PixelOf(rig, 0.0, 0.75, 0.85, &px, &py, false);
  Check(seen, "the trunk's crease is on the picture");
  if (!seen) {
    return;
  }
  Check(rig.scene.vehicle(px, py), "the trunk's crease is a vehicle pixel");
  Params with_reach = Fixed();
  Params without = Fixed();
  without.car_reach = 0.0f;
  const double a = MeanVisibility(rig, with_reach, px, py, 24);
  const double b = MeanVisibility(rig, without, px, py, 24);
  if (g_print) {
    std::printf("the car's own crease: no car limit %.3f, with it %.3f\n", b, a);
  }
  Check(std::fabs(a - b) < 1e-9, "a vehicle's own pixel is not limited");
  Check(a < 0.9, Num("the crease on the car is dark (%.3f, below 0.9)", a));
}

// The text of a file, or "" when it cannot be read.
std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void CheckShaders(const std::string& gpu_dir) {
  const std::string main_ps = Slurp(gpu_dir + "/hlsl/ao_main_ps.hlsl");
  const std::string depth_ps = Slurp(gpu_dir + "/hlsl/ao_depth_ps.hlsl");
  const std::string common = Slurp(gpu_dir + "/hlsl/ao_common.hlsli");
  const std::string cpp = Slurp(gpu_dir + "/ao.cpp");
  Check(!main_ps.empty() && !depth_ps.empty() && !common.empty() && !cpp.empty(), "the shaders and ao.cpp are readable from " + gpu_dir);
  if (main_ps.empty() || depth_ps.empty() || common.empty() || cpp.empty()) {
    return;
  }
  auto has = [](const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; };
  // The limits of the reach and the smallest car radius (both passes of the main shader).
  Check(has(main_ps, Num("car_share > %.3ff && car_share < %.3ff", double(kReachOff), double(kReachFull))), "ao_main_ps.hlsl: the limits of the reach are the header's");
  Check(has(main_ps, Num("max(car_share * radius, %.2ff)", double(kMinCarRadius))), "ao_main_ps.hlsl: the smallest car radius is the header's");
  Check(has(main_ps, "max(open_sum, 1e-3f)") && kMinOpenSum == 1e-3f, "ao_main_ps.hlsl: the least sum is the header's");
  // The vehicle test is the temporal pass's too.
  Check(has(depth_ps, "kFirstVehicleId = " + std::to_string(kFirstVehicleId) + "u") && has(depth_ps, "kLastVehicleId = " + std::to_string(kLastVehicleId) + "u"),
        "ao_depth_ps.hlsl: the vehicle IDs are the header's");
  const std::string temporal = Slurp(gpu_dir + "/hlsl/ao_temporal_ps.hlsl");
  Check(has(temporal, "kFirstVehicleId = " + std::to_string(kFirstVehicleId) + "u") && has(temporal, "kLastVehicleId = " + std::to_string(kLastVehicleId) + "u"),
        "ao_temporal_ps.hlsl: the vehicle IDs are the same as the depth pass's");
  Check(has(depth_ps, ">> 2") && has(depth_ps, "StencilOf"), "ao_depth_ps.hlsl reads the stencil's upper six bits");
  // The bit: the lowest of the float.
  Check(has(common, "asuint(level0_depth) & 1u") && has(common, "(asuint(depth) & ~1u) | (vehicle ? 1u : 0u)"), "ao_common.hlsli: the vehicle bit is the lowest bit of the float");
  // The falloff of a vehicle's samples: the world's ramp over the share of the radius.
  Check(has(main_ps, "-1.0f / (falloff_ratio * car_radius), 1.0f / falloff_ratio"), "ao_main_ps.hlsl: a car's falloff is the world's ramp over the car radius");
  // The default share is the setting's (a REX_SETTING row of src/settings/generated; the REXCVAR_DEFINE of ao.cpp before the conversion).
  const setting_source::Info reach = setting_source::Find(gpu_dir, cpp, "mcla_gpu_ao_car_reach");
  Check(setting_source::DefaultIs(reach, double(kDefaultCarReach)), "mcla_gpu_ao_car_reach: the setting's default is kDefaultCarReach");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else {
      gpu_dir = argv[i];
    }
  }
  CheckRules();
  CheckOpenRoad();
  CheckCarHalo();
  CheckWallUnchanged();
  CheckCarBodyUnchanged();
  if (!gpu_dir.empty()) {
    CheckShaders(gpu_dir);
  }
  std::printf("ao_halo_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// Check of the occlusion's use of the G-buffer's normal plane (ao_gbuffer.h, hlsl/ao_gbuffer_normal.hlsli, hlsl/ao_normal_ps.hlsl, the consumers' passes). A program of its own: it
// needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ao_gbuffer_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ao_gbuffer_test [<src/gpu dir>] [--print]
//
// It checks: the camera's rows (they turn a world direction into view space as ao.cpp FillPush turns the sun's, and a plane seen through a synthetic camera has the same normal from
// the depth's neighbours as from the G-buffer), the pick of a pixel's normal (no data, a normal that faces away, a grazing one), the depth's low bits (the vehicle mark and the
// source of a half size pixel, one unit in the last place or so), the facing of a sample's surface from its normal against the depth's way on planes, the choice of the camera, the
// frame's decision and its reasons, the images and slots; then it compiles hlsl/ao_gbuffer_normal.hlsli ITSELF as C++ (through the shim of the HLSL vector types below) and
// compares each function with the twin on random inputs. Given the src/gpu folder it also reads the sources: the setting, the passes' fields, the registration of the new pass.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ao_gbuffer.h"
#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "setting_source.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: the shim of what the two files use (gbuffer_common.hlsli first, as the shaders include it)
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct float2 {
  float x = 0, y = 0;
  float2() = default;
  float2(float a, float b) : x(a), y(b) {}
};
struct float3 {
  float x = 0, y = 0, z = 0;
  float3() = default;
  float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  float4() = default;
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
inline float2 operator*(float2 a, float s) { return float2(a.x * s, a.y * s); }
inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float3 operator*(float3 a, float3 b) { return float3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float3 sqrt(float3 v) { return float3(std::sqrt(v.x), std::sqrt(v.y), std::sqrt(v.z)); }
inline float3 saturate(float3 v) { return float3(saturate(v.x), saturate(v.y), saturate(v.z)); }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float3 normalize(float3 v) { return v * (1.0f / std::sqrt(dot(v, v))); }
inline uint asuint(float f) {
  uint u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
inline float asfloat(uint u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}
#include "hlsl/gbuffer_common.hlsli"
#include "hlsl/ao_gbuffer_normal.hlsli"
}  // namespace hlsl_shim

namespace gb = mcla::gpu::gbuffer;
namespace ag = mcla::gpu::ao_gbuffer;

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
  char buf[400];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  *out = text.str();
  return true;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}

struct Lcg {
  uint32_t state = 20261120u;
  float Next() {  // 0 to 1
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  gb::Vec3 Direction() {
    const float z = Range(-1.0f, 1.0f);
    const float a = Range(0.0f, 6.2831853f);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
  }
};

// The angle between two directions in degrees (atan2 of the cross product: acos of a float dot is only good to 0.02 degrees).
double AngleDegrees(gb::Vec3 a, gb::Vec3 b) {
  const double ax = a.x, ay = a.y, az = a.z, bx = b.x, by = b.y, bz = b.z;
  const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
  return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz) * 180.0 / 3.14159265358979323846;
}

// A camera of the game's: its view inverse as the game keeps it (row vectors: row 0 the right axis in world space, row 1 up, row 2 BACK, the game's view space looks along -z).
struct Camera {
  double m[4][4] = {};
  double right[3], up[3], forward[3];
  double position[3];
};
Camera MakeCamera(Lcg& rng) {
  Camera c;
  // yaw, pitch and roll of a right handed world with y up
  const double yaw = rng.Range(0.0f, 6.2831853f), pitch = rng.Range(-0.6f, 0.6f), roll = rng.Range(-0.2f, 0.2f);
  double f[3] = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};  // looks along -z at yaw 0
  const double world_up[3] = {0, 1, 0};
  double r[3] = {f[1] * world_up[2] - f[2] * world_up[1], f[2] * world_up[0] - f[0] * world_up[2], f[0] * world_up[1] - f[1] * world_up[0]};
  double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  for (double& v : r) v /= rl;
  double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
  // roll about the forward axis
  const double cr = std::cos(roll), sr = std::sin(roll);
  double r2[3], u2[3];
  for (int i = 0; i < 3; ++i) {
    r2[i] = cr * r[i] + sr * u[i];
    u2[i] = -sr * r[i] + cr * u[i];
  }
  for (int i = 0; i < 3; ++i) {
    c.right[i] = r2[i];
    c.up[i] = u2[i];
    c.forward[i] = f[i];
    c.m[0][i] = r2[i];
    c.m[1][i] = u2[i];
    c.m[2][i] = -f[i];  // the game's view space looks along -z: row 2 is the back
    c.position[i] = rng.Range(-500.0f, 500.0f);
    c.m[3][i] = c.position[i];
  }
  c.m[3][3] = 1.0;
  return c;
}

gb::Vec3 V(const double v[3]) { return {float(v[0]), float(v[1]), float(v[2])}; }

// ---------------------------------------------------------------------------
// the camera's rows
// ---------------------------------------------------------------------------
void RowChecks() {
  Lcg rng;
  double worst_axes = 0.0, worst_sun = 0.0;
  for (int i = 0; i < 2000; ++i) {
    const Camera cam = MakeCamera(rng);
    ag::Rows rows;
    if (!ag::RowsFromViewInverse(cam.m, &rows)) {
      Check(false, "a synthetic camera's view inverse was refused");
      break;
    }
    // the camera's own axes map to the view space's axes
    const gb::Vec3 right = ag::ToView(V(cam.right), rows), up = ag::ToView(V(cam.up), rows), fwd = ag::ToView(V(cam.forward), rows);
    worst_axes = std::max({worst_axes, AngleDegrees(right, {1, 0, 0}), AngleDegrees(up, {0, 1, 0}), AngleDegrees(fwd, {0, 0, 1})});
    // ao.cpp FillPush: view[j] = sum_i dir[i] * previous_.view.m[i][j] with the game's view matrix (the rotation part is the inverse of the view inverse's: its transpose), then
    // view[2] = -view[2]. The rows must give the same.
    const gb::Vec3 dir = rng.Direction();
    double view[3] = {};
    const double d[3] = {dir.x, dir.y, dir.z};
    for (int j = 0; j < 3; ++j) {
      for (int k = 0; k < 3; ++k) {
        view[j] += d[k] * cam.m[j][k];  // view.m[k][j] = view_inverse.m[j][k] for a rotation
      }
    }
    view[2] = -view[2];
    worst_sun = std::max(worst_sun, AngleDegrees(ag::ToView(dir, rows), {float(view[0]), float(view[1]), float(view[2])}));
  }
  Check(worst_axes < 0.001, Num("the camera's right, up and forward axes map to (1,0,0), (0,1,0), (0,0,1): worst %.5f degrees", worst_axes));
  Check(worst_sun < 0.001, Num("the rows turn a world direction into view space as ao.cpp FillPush turns the sun's: worst %.5f degrees", worst_sun));

  // the rotation difference: the position is not in it, a turn is
  {
    Camera a = MakeCamera(rng);
    Camera b = a;
    for (int i = 0; i < 3; ++i) b.m[3][i] += 40.0;  // the car drove 40 m
    Check(ag::RotationDifference(a.m, b.m) == 0.0, "a camera that only moved has no rotation difference");
    // turn the camera 3 degrees about its up axis: right' = right cos + back sin ... (rows 0 and 2 mix)
    const double t = 3.0 * 3.14159265358979323846 / 180.0;
    Camera c = a;
    for (int i = 0; i < 3; ++i) {
      c.m[0][i] = a.m[0][i] * std::cos(t) + a.m[2][i] * std::sin(t);
      c.m[2][i] = -a.m[0][i] * std::sin(t) + a.m[2][i] * std::cos(t);
    }
    const double turned = ag::RotationDifference(a.m, c.m);
    Check(turned > 0.02 && turned < 0.06, Num("a turn of 3 degrees is a rotation difference of %.4f (the limit kPoseStaleDifference is %.3f)", turned, ag::kPoseStaleDifference));
    Check(std::fabs(ag::kPoseStaleDifference - std::sin(t)) < 0.003, "kPoseStaleDifference is the sine of 3 degrees");
  }

  // refused matrices
  Camera cam = MakeCamera(rng);
  ag::Rows rows;
  cam.m[1][1] = 3.0;
  Check(!ag::RowsFromViewInverse(cam.m, &rows), "a row that is not a unit axis is refused");
  cam = MakeCamera(rng);
  cam.m[0][2] = std::nan("");
  Check(!ag::RowsFromViewInverse(cam.m, &rows), "a NaN entry is refused");
}

// ---------------------------------------------------------------------------
// a plane seen through a camera: the depth's normal and the G-buffer's agree
// ---------------------------------------------------------------------------

// ao_common.hlsli AoViewPos, AoNormalFromImage's neighbour rule, on a function of the depth.
struct Lens {
  float tan_x = 0.6f, tan_y = 0.3375f;  // square pixels (the occlusion's slice directions assume them)
  float w = 640.0f, h = 360.0f;
};
gb::Vec3 ViewPos(const Lens& lens, float px, float py, float t) {
  const float u = px / lens.w, v = py / lens.h;
  return {(u * 2.0f - 1.0f) * lens.tan_x * t, (1.0f - v * 2.0f) * lens.tan_y * t, t};
}
template <typename Depth>
gb::Vec3 DepthNormal(const Lens& lens, Depth depth, int px, int py) {
  const float tc = depth(px, py);
  const gb::Vec3 pc = ViewPos(lens, px + 0.5f, py + 0.5f, tc);
  const float tl = depth(px - 1, py), tr = depth(px + 1, py), tt = depth(px, py - 1), tb = depth(px, py + 1);
  const gb::Vec3 h = std::fabs(tl - tc) < std::fabs(tr - tc) ? pc - ViewPos(lens, px - 1 + 0.5f, py + 0.5f, tl) : ViewPos(lens, px + 1 + 0.5f, py + 0.5f, tr) - pc;
  const gb::Vec3 v = std::fabs(tt - tc) < std::fabs(tb - tc) ? ViewPos(lens, px + 0.5f, py - 1 + 0.5f, tt) - pc : pc - ViewPos(lens, px + 0.5f, py + 1 + 0.5f, tb);
  const gb::Vec3 n = gb::Cross(v, h);
  const float len = gb::Length(n);
  return len > 1e-20f ? n * (1.0f / len) : gb::Vec3{0, 0, -1};
}

void PlaneChecks() {
  Lcg rng;
  const Lens lens;
  double worst = 0.0;
  int planes = 0, mismatched = 0;
  for (int i = 0; i < 400; ++i) {
    const Camera cam = MakeCamera(rng);
    ag::Rows rows;
    if (!ag::RowsFromViewInverse(cam.m, &rows)) continue;
    // a world plane in front of the camera, its normal turned to the camera
    gb::Vec3 nw = rng.Direction();
    const gb::Vec3 fwd = V(cam.forward);
    const gb::Vec3 pos = V(cam.position);
    const gb::Vec3 p0 = pos + fwd * rng.Range(5.0f, 40.0f) + V(cam.right) * rng.Range(-3.0f, 3.0f) + V(cam.up) * rng.Range(-3.0f, 3.0f);
    if (gb::Dot(nw, fwd) > 0.0f) nw = nw * -1.0f;
    gb::Vec3 nv = ag::ToView(nw, rows);              // the G-buffer's way
    const gb::Vec3 p0v = ag::ToView(p0 - pos, rows);
    const gb::Vec3 ray_here = ViewPos(lens, 300.5f, 200.5f, 1.0f);
    if (gb::Dot(nv, ray_here) > 0.0f) {              // the face the camera sees through that pixel
      nw = nw * -1.0f;
      nv = nv * -1.0f;
    }
    if (gb::Dot(nv, gb::Normalize(ray_here)) > -0.2f) continue;  // not nearly edge on to the view
    const auto depth = [&](int px, int py) {
      // the view ray of the pixel centre meets the plane
      const gb::Vec3 d = ViewPos(lens, px + 0.5f, py + 0.5f, 1.0f);
      return gb::Dot(nv, p0v) / gb::Dot(nv, d);
    };
    if (depth(300, 200) < 1.0f || depth(299, 200) < 1.0f || depth(301, 200) < 1.0f || depth(300, 199) < 1.0f || depth(300, 201) < 1.0f) continue;  // the plane meets the ray behind the camera
    ++planes;
    const gb::Vec3 from_depth = DepthNormal(lens, depth, 300, 200);
    const double angle = AngleDegrees(from_depth, nv);
    worst = std::max(worst, angle);
    if (angle > 0.05) ++mismatched;
    // the pick: the texel the shader would hold for the world normal, at that pixel
    const gb::Rt1Texel texel = gb::Rt1Pack(nw);
    const gb::Vec3 pc = ViewPos(lens, 300.5f, 200.5f, depth(300, 200));
    const ag::Pick pick = ag::PickViewNormal(gb::Rt1Load(texel), pc, rows);
    Check(pick.taken, "a plane's normal facing the camera is taken");
    if (pick.taken) {
      worst = std::max(worst, AngleDegrees(pick.normal, nv));
    }
  }
  Check(planes > 200, Num("planes tested: %.0f", planes));
  Check(mismatched == 0 && worst < 0.05,
        Num("the depth's normal of a plane through a synthetic camera and the G-buffer's world normal turned into view space agree (worst %.4f degrees, %.0f planes off)", worst, mismatched));
}

// ---------------------------------------------------------------------------
// the pick of a pixel's normal
// ---------------------------------------------------------------------------
void PickChecks() {
  Lcg rng;
  const ag::Rows rows;  // identity: view space is the world's
  const gb::Vec3 pc{1.0f, 2.0f, 20.0f};
  const gb::Vec3 to_camera = gb::Normalize(pc * -1.0f);
  // no data
  Check(!ag::PickViewNormal({0.0f, 0.0f}, pc, rows).taken, "a texel that holds no data is not taken");
  // facing the camera
  gb::Vec3 n = to_camera;
  const ag::Pick facing = ag::PickViewNormal(gb::Rt1Load(gb::Rt1Pack(n)), pc, rows);
  Check(facing.taken && AngleDegrees(facing.normal, n) < 0.01, "a normal that faces the camera is taken as it is");
  // away by more than the limit
  const gb::Vec3 away = to_camera * -1.0f;
  Check(!ag::PickViewNormal(gb::Rt1Load(gb::Rt1Pack(away)), pc, rows).taken, "a normal that faces away from the camera is not taken");
  // a grazing one just past the limit (cosine -0.2): taken, moved to the minimum cosine, unit length
  const gb::Vec3 side = gb::Normalize(gb::Cross(to_camera, {0, 1, 0}));
  gb::Vec3 grazing = gb::Normalize(to_camera * -0.2f + side * 0.9798f);
  const ag::Pick graze = ag::PickViewNormal(gb::Rt1Load(gb::Rt1Pack(grazing)), pc, rows);
  Check(graze.taken, "a normal that faces away by a little (cosine -0.2) is taken");
  if (graze.taken) {
    Check(std::fabs(gb::Length(graze.normal) - 1.0f) < 1e-5f, "... as a unit vector");
    Check(gb::Dot(graze.normal, to_camera) >= ag::kMinFacing - 1e-4f, Num("... that faces the camera again (cosine %.4f, minimum %.2f)", gb::Dot(graze.normal, to_camera), ag::kMinFacing));
  }
  // just beyond the limit
  grazing = gb::Normalize(to_camera * -0.3f + side * 0.9539f);
  Check(!ag::PickViewNormal(gb::Rt1Load(gb::Rt1Pack(grazing)), pc, rows).taken, "a normal that faces away by more than the limit (cosine -0.3) is not taken");
  // the rows are used: a world normal that faces away in view space is rejected, one that faces the camera after the turn is taken
  ag::Rows turned;  // a half turn about y: x -> -x, z -> -z
  turned.x[0] = -1.0f;
  turned.z[2] = -1.0f;
  const gb::Vec3 world_n{-to_camera.x, to_camera.y, -to_camera.z};  // turns into to_camera
  const ag::Pick turned_pick = ag::PickViewNormal(gb::Rt1Load(gb::Rt1Pack(world_n)), pc, turned);
  Check(turned_pick.taken && AngleDegrees(turned_pick.normal, to_camera) < 0.01, "the camera's rows turn the world normal before the test");
  // random directions: whatever is taken faces the camera and is unit
  int taken = 0;
  bool all_face = true, all_unit = true;
  for (int i = 0; i < 20000; ++i) {
    const gb::Vec3 w = rng.Direction();
    const gb::Vec3 at{rng.Range(-30.0f, 30.0f), rng.Range(-15.0f, 15.0f), rng.Range(2.0f, 100.0f)};
    const ag::Pick p = ag::PickViewNormal(gb::Rt1Load(gb::Rt1Pack(w)), at, rows);
    if (p.taken) {
      ++taken;
      if (gb::Dot(p.normal, gb::Normalize(at * -1.0f)) < ag::kMinFacing - 1e-4f) all_face = false;
      if (std::fabs(gb::Length(p.normal) - 1.0f) > 1e-4f) all_unit = false;
    }
  }
  Check(all_face && all_unit, "every normal that is taken faces the camera (cosine at least the minimum) and is a unit vector");
  Check(taken > 9000 && taken < 14000, Num("about half of random directions are taken (%.0f of 20000)", taken));
}

// ---------------------------------------------------------------------------
// the depth's low bits and the half size source
// ---------------------------------------------------------------------------
void DepthBitChecks() {
  Lcg rng;
  bool vehicle_kept = true, source_kept = true, round_trip = true, small = true;
  double worst_relative = 0.0;
  for (int i = 0; i < 100000; ++i) {
    const float depth = rng.Range(0.1f, 5000.0f);
    const bool vehicle = rng.Next() < 0.5f;
    const uint32_t index = uint32_t(rng.Next() * 3.999f);
    const float marked = ag::SetVehicleBit(depth, vehicle);
    const float both = ag::SetSource(marked, index);
    if (ag::IsVehicle(both) != vehicle) vehicle_kept = false;
    if (ag::SourceOf(both) != index) round_trip = false;
    // setting the vehicle bit again keeps the source
    if (ag::SourceOf(ag::SetVehicleBit(both, !vehicle)) != index) source_kept = false;
    const double relative = std::fabs(double(both) - double(depth)) / double(depth);
    worst_relative = std::max(worst_relative, relative);
    if (relative > 1e-6) small = false;
  }
  Check(vehicle_kept, "the vehicle bit survives the source bits");
  Check(round_trip, "the source index (0 to 3) round trips");
  Check(source_kept, "the source index survives a new vehicle bit");
  Check(small, Num("three low bits change a depth by under 1e-6 of it (worst %.2e)", worst_relative));
  // the HLSL's: the same bits
  bool same = true;
  for (int i = 0; i < 20000; ++i) {
    const float depth = rng.Range(0.1f, 5000.0f);
    const uint32_t index = uint32_t(rng.Next() * 3.999f);
    if (hlsl_shim::AoGbSetSource(depth, index) != ag::SetSource(depth, index) || hlsl_shim::AoGbSourceOf(ag::SetSource(depth, index)) != ag::SourceOf(ag::SetSource(depth, index))) same = false;
  }
  Check(same, "ao_gbuffer_normal.hlsli: AoGbSetSource / AoGbSourceOf give the twin's bits");

  // SourcePixel
  const ag::Pixel last{99, 49};
  Check(ag::SourcePixel({10, 7}, false, 3, last) == ag::Pixel{10, 7}, "full size: the pixel itself");
  Check(ag::SourcePixel({10, 7}, true, 0, last) == ag::Pixel{20, 14}, "half size, index 0: the block's top left");
  Check(ag::SourcePixel({10, 7}, true, 1, last) == ag::Pixel{21, 14}, "half size, index 1: top right");
  Check(ag::SourcePixel({10, 7}, true, 2, last) == ag::Pixel{20, 15}, "half size, index 2: bottom left");
  Check(ag::SourcePixel({10, 7}, true, 3, last) == ag::Pixel{21, 15}, "half size, index 3: bottom right");
  Check(ag::SourcePixel({49, 24}, true, 3, last) == ag::Pixel{99, 49}, "half size: the last block's bottom right is the last pixel");
  Check(ag::SourcePixel({49, 24}, true, 1, {98, 48}) == ag::Pixel{98, 48}, "half size: a block past an odd plane's edge is kept inside it, as the depth pass keeps it");
}

// ---------------------------------------------------------------------------
// the facing of a sample's surface
// ---------------------------------------------------------------------------
void FacingChecks() {
  Lcg rng;
  const Lens lens;
  double worst = 0.0;
  int tested = 0;
  for (int i = 0; i < 20000; ++i) {
    // the pixel's own view position and a slice: the slice plane contains the camera and the screen line through the pixel
    const float px = rng.Range(50.0f, 590.0f), py = rng.Range(50.0f, 310.0f);
    const float angle = rng.Range(0.0f, 3.1415926f);
    const gb::Vec2 screen_dir{std::cos(angle), std::sin(angle)};
    const float t_pixel = rng.Range(3.0f, 60.0f);
    const gb::Vec3 pc = ViewPos(lens, px, py, t_pixel);
    const gb::Vec3 view_vec = gb::Normalize(pc * -1.0f);
    const gb::Vec3 dir_view{screen_dir.x, -screen_dir.y, 0.0f};
    const gb::Vec3 ortho = dir_view - view_vec * gb::Dot(dir_view, view_vec);
    const gb::Vec3 axis = gb::Normalize(gb::Cross(ortho, view_vec));
    // a surface plane: a normal facing the camera-ish, through a point in the slice
    const float shift = rng.Range(2.0f, 12.0f);
    const gb::Vec3 sample_pixel{px + screen_dir.x * shift, py + screen_dir.y * shift, 0.0f};
    // the sample's view ray lies in the slice plane; the surface meets it where
    gb::Vec3 n = rng.Direction();
    const gb::Vec3 ray1 = ViewPos(lens, sample_pixel.x, sample_pixel.y, 1.0f);
    const gb::Vec3 ray2 = ViewPos(lens, sample_pixel.x + screen_dir.x, sample_pixel.y + screen_dir.y, 1.0f);
    const gb::Vec3 anchor = ray1 * rng.Range(3.0f, 60.0f);
    const float d1 = gb::Dot(n, ray1), d2 = gb::Dot(n, ray2);
    if (std::fabs(d1) < 0.1f || std::fabs(d2) < 0.1f) continue;
    const float t1 = gb::Dot(n, anchor) / d1, t2 = gb::Dot(n, anchor) / d2;
    if (t1 < 1.0f || t2 < 1.0f) continue;
    const gb::Vec3 sp = ray1 * t1, sp2 = ray2 * t2;
    if (gb::Dot(n, gb::Normalize(sp)) > 0.0f) n = n * -1.0f;  // the surface faces the camera
    const gb::Vec3 delta = sp - pc;
    const float dist = gb::Length(delta);
    if (dist < 0.2f) continue;
    const gb::Vec3 dir = delta * (1.0f / dist);
    // the tangent is the profile's direction only when the surface cuts the slice plane across: skip a surface almost parallel to it
    const gb::Vec3 in_plane = n - axis * gb::Dot(n, axis);
    if (gb::Length(in_plane) < 0.2f) continue;
    const float from_normal = ag::FacingFromNormal(n, axis, sp, dir);
    const float from_tangent = ag::FacingFromTangent(sp, sp2, axis, dir);
    ++tested;
    worst = std::max(worst, double(std::fabs(from_normal - from_tangent)));
    // the HLSL's
    const hlsl_shim::float3 hn(n.x, n.y, n.z), ha(axis.x, axis.y, axis.z), hs(sp.x, sp.y, sp.z), hd(dir.x, dir.y, dir.z);
    const float shader = hlsl_shim::AoGbFacingFromNormal(hn, ha, hs, hd);
    if (std::fabs(shader - from_normal) > 1e-5f) {
      Check(false, "ao_gbuffer_normal.hlsli AoGbFacingFromNormal differs from the twin");
      break;
    }
  }
  Check(tested > 5000, Num("surfaces tested for the facing: %.0f", tested));
  Check(worst < 2e-3, Num("the facing from the surface's normal equals the depth's way (tangent one texel on, cross product) on planes: worst difference %.5f", worst));

  // degenerate: the slice plane parallel to the surface: no profile, taken to face the pixel
  Check(ag::FacingFromNormal({0, 0, -1}, {0, 0, 1}, {0, 0, 10}, {0, 0, 1}) == 1.0f, "a surface parallel to the slice plane faces the pixel (no profile)");
  // a surface that faces away from the pixel gets 0, one that faces it gets 1
  const gb::Vec3 axis{0, 1, 0};
  // the wall's face looks along -x, its sample at x = 5 (z = 10 in front of the camera)
  Check(ag::FacingFromNormal({-1, 0, 0}, axis, {5, 0, 10}, gb::Normalize({1, 0, 0})) > 0.99f, "a wall that looks at the pixel (the pixel to its left, the sample on the wall to the pixel's right) gets the whole weight");
  Check(ag::FacingFromNormal({-1, 0, 0}, axis, {5, 0, 10}, gb::Normalize({-1, 0, 1})) == 0.0f, "a wall turned from the pixel (the sample is to the pixel's left, the wall's face looks further left) gets none");
}

// ---------------------------------------------------------------------------
// the HLSL's pick against the twin
// ---------------------------------------------------------------------------
void ShaderChecks() {
  Check(hlsl_shim::kAoGbRejectFacing == ag::kRejectFacing && hlsl_shim::kAoGbMinFacing == ag::kMinFacing, "the .hlsli's two constants are the header's");
  Lcg rng;
  int compared = 0, different = 0;
  double worst = 0.0;
  for (int i = 0; i < 100000; ++i) {
    const Camera cam = MakeCamera(rng);
    ag::Rows rows;
    if (!ag::RowsFromViewInverse(cam.m, &rows)) continue;
    const gb::Vec3 w = rng.Direction();
    const gb::Vec3 pc{rng.Range(-40.0f, 40.0f), rng.Range(-20.0f, 20.0f), rng.Range(1.0f, 200.0f)};
    // one in 12 is a texel with no data
    const gb::Rt1Texel texel = rng.Next() < 0.08f ? gb::Rt1Texel{0, 0} : gb::Rt1Pack(w);
    const gb::Vec2 load = gb::Rt1Load(texel);
    const ag::Pick twin = ag::PickViewNormal(load, pc, rows);
    const hlsl_shim::float4 got = hlsl_shim::AoGbViewNormal(hlsl_shim::float2(load.x, load.y), hlsl_shim::float3(pc.x, pc.y, pc.z), hlsl_shim::float3(rows.x[0], rows.x[1], rows.x[2]),
                                                            hlsl_shim::float3(rows.y[0], rows.y[1], rows.y[2]), hlsl_shim::float3(rows.z[0], rows.z[1], rows.z[2]));
    ++compared;
    if ((got.w > 0.5f) != twin.taken) {
      ++different;
      continue;
    }
    if (twin.taken) {
      worst = std::max(worst, AngleDegrees({got.x, got.y, got.z}, twin.normal));
    }
  }
  Check(different == 0, Num("ao_gbuffer_normal.hlsli AoGbViewNormal takes the same pixels as the twin (%.0f of %.0f differ)", different, compared));
  Check(worst < 0.001, Num("... and gives the same normal (worst %.5f degrees)", worst));
}

// ---------------------------------------------------------------------------
// the decisions
// ---------------------------------------------------------------------------
void RuleChecks() {
  // the camera's choice
  ag::CameraFacts f;
  Check(ag::ChooseCamera(f) == ag::Camera::kNone, "no camera at all: none");
  f.have_previous = true;
  Check(ag::ChooseCamera(f) == ag::Camera::kPrevious, "a last frame's camera and no bank: the last frame's");
  f.bank_present = true;
  f.bank_sane = true;
  f.bank_difference = 0.001;
  Check(ag::ChooseCamera(f) == ag::Camera::kPrevious, "an untrusted bank that equals the last frame's camera: the last frame's");
  f.bank_difference = 0.2;
  Check(ag::ChooseCamera(f) == ag::Camera::kStale, "an untrusted bank that differs by 0.2: stale (the camera may have turned, or the bank is not the camera)");
  f.bank_difference = ag::kPoseStaleDifference;
  Check(ag::ChooseCamera(f) == ag::Camera::kPrevious, "a difference at the limit is not stale");
  f.bank_trusted = true;
  f.bank_difference = 0.2;
  Check(ag::ChooseCamera(f) == ag::Camera::kVertexBank, "a trusted bank is the frame's own camera, whatever it differs by");
  f.bank_sane = false;
  Check(ag::ChooseCamera(f) == ag::Camera::kPrevious, "a trusted bank that is not a camera's matrix: the last frame's");
  f.have_previous = false;
  Check(ag::ChooseCamera(f) == ag::Camera::kNone, "... and none without a last frame's camera");

  // the frame's decision, in the order of the reasons
  ag::FrameFacts a;
  a.frame_has_normal = true;
  // The real frame (the first G-buffer run, 11-20): the sidecar is the guest's 1280 x 736 times 1.5 = 1920 x 1104, the depth target has its width, the picture (the resolve's rectangle) is 1920 x 1080.
  a.plane_width = a.target_width = a.render_width = 1920;
  a.plane_height = 1104;
  a.render_height = 1080;
  a.camera = ag::Camera::kPrevious;
  Check(ag::Decide(a) == ag::Why::kUsed, "everything there: the frame uses the plane");
  a.camera = ag::Camera::kVertexBank;
  Check(ag::Decide(a) == ag::Why::kUsed, "... with the frame's own camera too");
  auto without = [&](auto change) {
    ag::FrameFacts b = a;
    change(b);
    return ag::Decide(b);
  };
  Check(without([](ag::FrameFacts& b) { b.setting = false; }) == ag::Why::kSettingOff, "the setting off: kSettingOff");
  Check(without([](ag::FrameFacts& b) { b.gbuffer_present = false; }) == ag::Why::kNoGBuffer, "no G-buffer object: kNoGBuffer");
  Check(without([](ag::FrameFacts& b) { b.play_view = false; }) == ag::Why::kPauseView, "the pause world: kPauseView");
  Check(without([](ag::FrameFacts& b) { b.frame_has_normal = false; }) == ag::Why::kNoNormalPlane, "no published normal plane: kNoNormalPlane");
  Check(without([](ag::FrameFacts& b) { b.plane_width = 1280; b.render_width = 1280; }) == ag::Why::kSizeDiffers, "a plane of another width than the depth target's: kSizeDiffers");
  Check(without([](ag::FrameFacts& b) { b.target_width = 1280; }) == ag::Why::kSizeDiffers, "a plane made at the last scale (the depth target is another width now): kSizeDiffers");
  Check(without([](ag::FrameFacts& b) { b.plane_height = 720; }) == ag::Why::kSizeDiffers, "a plane shorter than the picture does not cover it: kSizeDiffers");
  Check(without([](ag::FrameFacts& b) { b.render_height = 1104; b.render_width = 1920; }) == ag::Why::kUsed, "a picture as big as the target is covered by the plane");
  Check(without([](ag::FrameFacts& b) { b.render_height = 1105; }) == ag::Why::kSizeDiffers, "a picture taller than the plane is not covered: kSizeDiffers");
  Check(without([](ag::FrameFacts& b) { b.render_width = 1921; }) == ag::Why::kSizeDiffers, "a picture wider than the plane is not covered: kSizeDiffers");
  Check(without([](ag::FrameFacts& b) { b.plane_height = 736; b.plane_width = b.target_width = 1280; b.render_width = 1280; b.render_height = 720; }) == ag::Why::kUsed,
        "720p: the plane is 1280 x 736 and the picture 1280 x 720 (the size every real frame has): used");
  Check(without([](ag::FrameFacts& b) { b.camera = ag::Camera::kNone; }) == ag::Why::kNoCamera, "no camera: kNoCamera");
  Check(without([](ag::FrameFacts& b) { b.camera = ag::Camera::kStale; }) == ag::Why::kCameraStale, "a stale camera: kCameraStale");
  Check(without([](ag::FrameFacts& b) { b.pipeline_ready = false; }) == ag::Why::kPipelinePending, "a pipeline still being made: kPipelinePending");
  Check(without([](ag::FrameFacts& b) { b.setting = false; b.frame_has_normal = false; }) == ag::Why::kSettingOff, "the setting is the first reason");
  for (uint32_t w = 0; w < uint32_t(ag::Why::kCount); ++w) {
    Check(std::strlen(ag::WhyName(ag::Why(w))) > 3, "every reason has a name");
  }

  // what the frame asks the G-buffer for
  Check(ag::PlanesWanted(true, true) == gb::PlaneBit(gb::kPlaneNormal), "the occlusion or the contact shadows on, the setting on: the normal plane");
  Check(ag::PlanesWanted(false, true) == 0u && ag::PlanesWanted(true, false) == 0u, "the setting off, or no effect on: nothing");
  Check(ag::BouncePlanesWanted(true, true) == gb::PlaneBit(gb::kPlaneNormal) && ag::BouncePlanesWanted(true, false) == 0u && ag::BouncePlanesWanted(false, true) == 0u,
        "the bounce light asks for the normal plane while it and the setting are on");
  // the plane the GBuffer then attaches: its plan, with the stage as it is (only a normal is written)
  const gb::FramePlan on = gb::PlanFrame(gb::Setting::kAuto, ag::PlanesWanted(true, true));
  Check(on.attached != 0u && (on.written & gb::PlaneBit(gb::kPlaneNormal)) != 0u, "the G-buffer makes the sidecar for that request (auto)");
  const gb::FramePlan off = gb::PlanFrame(gb::Setting::kAuto, ag::PlanesWanted(true, false));
  Check(off.attached == 0u, "... and none without it: no image, no draw changes");

  // the images
  const uint32_t w4 = 3840, h4 = 2160, wh = 1920, hh = 1080;
  const ag::Images none = ag::ImagesFor(false, wh, hh, w4, h4, true, true);
  Check(!none.ao && !none.full && none.bytes == 0, "no images while the frame does not use the plane");
  const ag::Images full_size = ag::ImagesFor(true, w4, h4, w4, h4, false, true);
  Check(full_size.ao && !full_size.full && full_size.bytes == uint64_t(w4) * h4 * 4, "traced at the picture's size: one image (33.2 MB at 4K)");
  const ag::Images split = ag::ImagesFor(true, wh, hh, w4, h4, true, true);
  Check(split.ao && split.full && split.bytes == uint64_t(wh) * hh * 4 + uint64_t(w4) * h4 * 4, "half size traced with the contact shadows at the picture's size: two images (8.3 + 33.2 MB at 4K)");
  const ag::Images half_only = ag::ImagesFor(true, wh, hh, w4, h4, false, true);
  Check(half_only.ao && !half_only.full && half_only.bytes == uint64_t(wh) * hh * 4, "half size without the contact shadows: the traced image alone (8.3 MB at 4K)");
  if (g_print) {
    std::printf("   normal images at 4K: one %.1f MiB, split %.1f MiB, half %.1f MiB; at 1080p one %.1f MiB\n", full_size.bytes / 1048576.0, split.bytes / 1048576.0, half_only.bytes / 1048576.0,
                ag::NormalBytes(1920, 1080) / 1048576.0);
  }

  // which image a consumer reads
  Check(ag::ContactReads(true) == ag::Which::kFull && ag::ContactReads(false) == ag::Which::kAo, "the contact shadows read the full size image when they have their own size, else the traced one");
  Check(ag::ApplyReads(true, true) == ag::Which::kFull, "the apply reads the full size image beside a half size trace with the contact shadows at the picture's size");
  Check(ag::ApplyReads(false, false) == ag::Which::kAo, "... the traced one where that is the picture's size");
  Check(ag::ApplyReads(false, true) == ag::Which::kNone, "... none where the trace is half size and nothing has a full size image: the depth's normal as before");

  // the slots
  Check(ag::PackHigh(0x1234u, 0x2AAAu) == (0x2AAAu << 16 | 0x1234u) && ag::HighSlot(ag::PackHigh(7u, 32767u)) == 32767u && ag::LowHalf(ag::PackHigh(7u, 32767u)) == 7u, "a slot in the high half of a word round trips");
  Check(ag::HighSlot(ag::PackHigh(0xFFFFu, 0u)) == 0u, "no slot in the high half is slot 0, the heap's null texture: no image");
}

// ---------------------------------------------------------------------------
// the sources (given the src/gpu folder)
// ---------------------------------------------------------------------------
void SourceChecks(const std::string& gpu) {
  std::string cpp, header, common, main_ps, denoise, atrous, temporal, contact, apply, depth, normal, cmake, shaders_h, gb_common, renderer;
  if (!Slurp(gpu + "/ao.cpp", &cpp) || !Slurp(gpu + "/ao.h", &header) || !Slurp(gpu + "/hlsl/ao_common.hlsli", &common) || !Slurp(gpu + "/hlsl/ao_main_ps.hlsl", &main_ps) ||
      !Slurp(gpu + "/hlsl/ao_denoise_ps.hlsl", &denoise) || !Slurp(gpu + "/hlsl/ao_atrous_ps.hlsl", &atrous) || !Slurp(gpu + "/hlsl/ao_temporal_ps.hlsl", &temporal) ||
      !Slurp(gpu + "/hlsl/ao_contact_ps.hlsl", &contact) || !Slurp(gpu + "/hlsl/ao_apply_ps.hlsl", &apply) || !Slurp(gpu + "/hlsl/ao_depth_ps.hlsl", &depth) ||
      !Slurp(gpu + "/hlsl/ao_normal_ps.hlsl", &normal) || !Slurp(gpu + "/CMakeLists.txt", &cmake) || !Slurp(gpu + "/host_shaders.h", &shaders_h) ||
      !Slurp(gpu + "/hlsl/gbuffer_common.hlsli", &gb_common) || !Slurp(gpu + "/renderer.cpp", &renderer)) {
    Check(false, "the files of src/gpu could not be read (" + gpu + ")");
    return;
  }
  // the setting: debug-only, live, on by default
  const setting_source::Info setting = setting_source::Find(gpu, cpp, "mcla_gpu_ao_gbuffer_normals");
  Check(setting.found && !setting_source::DefaultIsTrue(setting), "the setting mcla_gpu_ao_gbuffer_normals exists and defaults to false (2026-11-20: the G-buffer stays off by default until a feature that needs it lands; its cost)");
  Check(setting.debug_only && setting.live, "... it is debug-only and live");
  Check(Has(cpp, "larecomp::settings::mcla_gpu_ao_gbuffer_normals.Get()"), "ao.cpp reads the setting");
  // the ask
  Check(Has(cpp, "gbuffer::Consumer::kAmbientOcclusion") && Has(cpp, "gbuffer::Consumer::kBounceLight") && Has(cpp, "ao_gbuffer::PlanesWanted(") && Has(cpp, "ao_gbuffer::BouncePlanesWanted("),
        "ao.cpp asks the G-buffer for the normal plane through GBuffer::Want, for the occlusion and the bounce light");
  Check(Has(cpp, "ao_gbuffer::Decide(") && Has(cpp, "ao_gbuffer::ChooseCamera(") && Has(cpp, "ao_gbuffer::RowsFromViewInverse(") && Has(cpp, "ao_gbuffer::RotationDifference("), "ao.cpp decides the frame with the header's rules");
  Check(Has(renderer, "ao_.SetGBuffer(&gbuffer_)"), "renderer.cpp gives the occlusion the G-buffer");

  // the shared include: every consumer asks AoNormalAt, nobody else reconstructs
  Check(Has(common, "float3 AoNormalAt(") && Has(common, "#include \"gbuffer_common.hlsli\"") && Has(common, "float3 AoNormalFromImage("), "ao_common.hlsli: AoNormalAt picks the image or the depth, the old reconstruction stays");
  size_t calls = 0;
  for (const std::string* text : {&main_ps, &denoise, &atrous, &temporal, &contact}) calls += Count(*text, "AoNormalFromImage(");
  Check(calls == 0, Num("no consumer pass calls AoNormalFromImage directly any more (%.0f left)", double(calls)));
  Check(Count(main_ps, "AoNormalAt(") == 2 && Count(denoise, "AoNormalAt(") == 2 && Count(atrous, "AoNormalAt(") == 1 && Count(temporal, "AoNormalAt(") == 2 && Count(contact, "AoNormalAt(") == 2,
        "the main pass (plain and bounce), the denoise (plain and bounce), the a-trous, the accumulation (plain and bounce) and the contact march (twice) read AoNormalAt");
  // the fields the passes read the slot from, as ao.h and the header say
  Check(Has(main_ps, "uint(g_push.lin.y)"), "the main pass reads the normal slot from lin.y (a number, as the traced image's slot in lin.x)");
  Check(Has(denoise, "g_push.slots2.w") && Has(atrous, "g_push.slots2.w"), "the denoise and the a-trous read the normal slot from slots2.w");
  Check(Has(temporal, "g_push.slots.y >> 16") && Has(temporal, "g_push.slots.y & 0xFFu"), "the accumulation reads it from the high half of slots.y, the host sample from its low byte");
  Check(Has(contact, "g_push.slots.y >> 16"), "the contact march reads it from the high half of slots.y");
  Check(Has(apply, "g_push.slots.w >> 16") && Has(apply, "g_push.slots.w & 0xFFFFu"), "the apply reads it from the high half of slots.w, the depth's slot from the low half");
  Check(Count(apply, "g_push.slots.w") == Count(apply, "g_push.slots.w >> 16") + Count(apply, "g_push.slots.w & 0xFFFFu"), "the apply never reads the whole of slots.w any more");
  // the facing of the bounce light
  Check(Has(main_ps, "AoGbFacingFromNormal("), "the bounce light's facing test reads the sample's normal");
  // the depth pass: the vehicle mark's literal stays (ao_size_test), the source bits ride on the push's slots2.w
  Check(Has(depth, "AoSetVehicleBit(t, IsVehiclePixel(source))") && Has(depth, "AoGbSetSource("), "the depth pass keeps the vehicle mark and writes the source index when asked");
  // the new pass and its registration
  Check(Has(normal, "AoGbViewNormal(") && Has(normal, "AoNormalFromImage(") && Has(normal, "AoGbSourceOf("), "ao_normal_ps.hlsl: picks RT1's normal or the depth's, at the depth's source texel");
  Check(Has(cmake, "larecomp_gpu_host_shader(ao_normal_ps ps_6_0 DEFINES AO_NORMAL_MS=0)") && Has(cmake, "larecomp_gpu_host_shader(ao_normal_ms_ps ps_6_0 SOURCE ao_normal_ps DEFINES AO_NORMAL_MS=1)"),
        "CMakeLists.txt builds the normal pass for one sample and for the host's MSAA level");
  Check(Has(cmake, "add_executable(larecomp_ao_gbuffer_test EXCLUDE_FROM_ALL ao_gbuffer_test.cpp)"), "CMakeLists.txt has this test's target");
  Check(Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(ao_normal_ps)") && Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(ao_normal_ms_ps)") && Has(shaders_h, "ao_normal_ps.hlsl.dxil.h") &&
            Has(shaders_h, "ao_normal_ps.hlsl.spirv.h") && Has(shaders_h, "ao_normal_ms_ps.hlsl.dxil.h") && Has(shaders_h, "ao_normal_ms_ps.hlsl.spirv.h"),
        "host_shaders.h includes and returns both blobs");
  // the flag of the depth pass and the frame's images
  Check(Has(cpp, "kPassNormal") && Has(cpp, "kVNormal") && Has(cpp, "kVNormalMs") && Has(header, "kPassNormalMs"), "ao.cpp / ao.h have the normal pass and its two variants");
  // the render path writes the sky as a direction too: no code of the prepass needs the no-data code
  Check(Has(gb_common, "float3 GbRt1Decode(float2 rt1)") && Has(gb_common, "bool GbRt1Valid(float2 rt1)"), "gbuffer_common.hlsli still has the decode and the validity test");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--print") {
      g_print = true;
    } else {
      gpu = arg;
    }
  }
  RowChecks();
  PlaneChecks();
  PickChecks();
  DepthBitChecks();
  FacingChecks();
  ShaderChecks();
  RuleChecks();
  if (!gpu.empty()) {
    SourceChecks(gpu);
  } else {
    std::printf("(no src/gpu folder given: the source checks were left out)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

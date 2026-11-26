// Check of the player's cabin term of the probe field's apply (gi_cabin_rules.h and hlsl/gi_cabin_common.hlsli, with the wiring in hlsl/gi_apply_ps.hlsl and gi_apply.cpp): INTERIOR-CAM-FIX.
// A program of its own: it needs no renderer, no game files and no GPU, and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_gi_cabin_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/gi_cabin_test.cpp
//   run: larecomp_gi_cabin_test [-v] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check with its numbers)
//
// It checks: the IDENTITY (no rule in the frame, the setting off, no box, no camera in the car, an eye at the roofline or under the hood's height, a car with no roof, a box or an eye that is not a number:
// the rows are OffRows() and every pixel's term is exactly 1, so the apply's colour is the one it was); the INTERIOR CAMERA's weight (the box grown, the headroom's window); the share (a floor, never 0, never
// over 1, monotone in the camera's weight and in the roof); the BOX (the term is the share inside, 1 outside, a smooth blend over the edge, per axis); the SURFACES the term is for (a vehicle's part that is
// not paint, chrome, glass, emissive or no data: the driver, the dash, the wheel, the seats; never the exterior, another car, a pedestrian or the road); the ROWS and the push block's last word; and then it
// compiles the .hlsli ITSELF as C++ (through a shim of the HLSL types) and compares each of its functions with the twin on random inputs, and every #define of it with the C++ value, and reads the shader, the
// host, the settings, the debug row, the documentation and the CMake line for the wiring.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>

#include "gi_cabin_rules.h"

namespace gc = mcla::gpu::gi_cabin;
namespace gb = mcla::gpu::gbuffer;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_verbose = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  } else if (g_verbose) {
    std::printf("ok: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[400];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}
bool Near(double a, double b, double abs_tol, double rel_tol = 0.0) { return std::fabs(a - b) <= abs_tol + rel_tol * std::max(std::fabs(a), std::fabs(b)); }
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}

struct Lcg {
  uint32_t state = 20261124u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Next() { return float(U32() >> 8) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
};

// A car's world box as a coupe's: 2 m wide, 4.4 m long, 1.35 m tall from the road, at a place far from the origin (the world is thousands of metres across).
struct Car {
  double lo[3] = {-1010.0, 0.05, 502.0};
  double hi[3] = {-1008.0, 1.35, 506.4};
};
// The driver's eye: 0.4 m left of the middle, 1.1 m up, in the cabin's front half.
void CockpitEye(const Car& car, float eye[3]) {
  eye[0] = float(car.lo[0] + 0.6);
  eye[1] = 1.10f;
  eye[2] = float(car.lo[2] + 2.0);
}

bool IsOff(const gc::Rows& r) {
  const gc::Rows off = gc::OffRows();
  return std::memcmp(r.data(), off.data(), sizeof(r)) == 0;
}

// ---------------------------------------------------------------------------
// the identity
// ---------------------------------------------------------------------------
void IdentityChecks() {
  // the numbers: a change of any is a decision, made here and in the header's comment together (the shader reads the box, the share and the edge from the rows, so only the host has them)
  Check(gc::kOpenShare == 0.30f && gc::kBoxGrow == 0.10f && gc::kEdge == 0.06f && gc::kHeadMin0 == 0.0f && gc::kHeadMin1 == 0.05f && gc::kHeadMax0 == 0.55f && gc::kHeadMax1 == 0.75f,
        "NUMBERS: the floor 0.30, the box's growth 0.10 m, the edge 0.06 m, the headroom's window 0 / 0.05 / 0.55 / 0.75 m");
  const gc::Rows off = gc::OffRows();
  Check(off[3] == 1.0f && sizeof(gc::Rows) == 32 && gc::kBytes == sizeof(gc::Rows), "IDENTITY: the off rows have the share 1 (the shader's first test) and are 32 bytes");
  Lcg rng;
  bool all_one = true;
  for (int i = 0; i < 2000; ++i) {
    const float pos[3] = {rng.Range(-3000.0f, 3000.0f), rng.Range(-5.0f, 20.0f), rng.Range(-3000.0f, 3000.0f)};
    all_one = all_one && gc::Term(off, pos) == 1.0f && gc::ShareFor(off, pos, rng.U32() % 32u, rng.U32() % 8u) == 1.0f;
  }
  Check(all_one, "IDENTITY: with the off rows every pixel's term is exactly 1");

  Car car;
  float eye[3];
  CockpitEye(car, eye);
  const gc::Rows on = gc::MakeRows(true, eye, true, car.lo, car.hi, 1.0f);
  Check(!IsOff(on) && Near(on[3], gc::kOpenShare, 1e-6), Num("IDENTITY: the cockpit eye in a roofed car makes a rule: the share is %.3f", on[3]));
  Check(IsOff(gc::MakeRows(false, eye, true, car.lo, car.hi, 1.0f)), "IDENTITY: the setting off: the off rows");
  Check(IsOff(gc::MakeRows(true, eye, false, car.lo, car.hi, 1.0f)), "IDENTITY: no box known: the off rows");
  Check(IsOff(gc::MakeRows(true, eye, true, car.lo, car.hi, 0.0f)), "IDENTITY: a car with no roof (closed 0): the off rows (the cabin gets the field's light as before)");
  {
    float outside[3] = {eye[0] + 3.0f, eye[1], eye[2]};  // a chase camera: well outside
    Check(IsOff(gc::MakeRows(true, outside, true, car.lo, car.hi, 1.0f)), "IDENTITY: a camera outside the car: the off rows");
    float behind[3] = {eye[0], eye[1], float(car.lo[2]) - 6.0f};
    Check(IsOff(gc::MakeRows(true, behind, true, car.lo, car.hi, 1.0f)), "IDENTITY: a chase camera 6 m behind: the off rows");
    float roofline[3] = {eye[0], float(car.hi[1]), eye[2]};
    Check(IsOff(gc::MakeRows(true, roofline, true, car.lo, car.hi, 1.0f)), "IDENTITY: an eye at the roofline (a camera on the roof): the off rows");
    float low[3] = {eye[0], 0.45f, eye[2]};  // a bumper camera: far under the roof
    Check(IsOff(gc::MakeRows(true, low, true, car.lo, car.hi, 1.0f)), "IDENTITY: an eye 0.9 m under the roof (a bumper camera): the off rows");
  }
  {
    float nan_eye[3] = {eye[0], std::nanf(""), eye[2]};
    Check(IsOff(gc::MakeRows(true, nan_eye, true, car.lo, car.hi, 1.0f)), "IDENTITY: an eye that is not a number: the off rows");
    double bad_hi[3] = {car.hi[0], std::nan(""), car.hi[2]};
    Check(IsOff(gc::MakeRows(true, eye, true, car.lo, bad_hi, 1.0f)), "IDENTITY: a box that is not a number: the off rows");
    double inverted_hi[3] = {car.lo[0] - 1.0, car.hi[1], car.hi[2]};
    Check(IsOff(gc::MakeRows(true, eye, true, car.lo, inverted_hi, 1.0f)), "IDENTITY: an empty or inverted box: the off rows");
    double inf_lo[3] = {-std::numeric_limits<double>::infinity(), car.lo[1], car.lo[2]};
    Check(IsOff(gc::MakeRows(true, eye, true, inf_lo, car.hi, 1.0f)), "IDENTITY: an infinite box: the off rows");
  }
}

// ---------------------------------------------------------------------------
// the interior camera's weight and the share
// ---------------------------------------------------------------------------
void InteriorChecks() {
  Car car;
  float eye[3];
  CockpitEye(car, eye);
  Check(gc::InteriorWeight(eye, car.lo, car.hi) == 1.0f, "INTERIOR: the cockpit eye (0.25 m under the roof, inside the box) is an interior camera, whole");
  // the headroom's window: 0 at the roofline, 1 from kHeadMin1, 1 up to kHeadMax0, 0 from kHeadMax1; smooth between
  const auto at_headroom = [&](float h) {
    float e[3] = {eye[0], float(car.hi[1]) - h, eye[2]};
    return gc::InteriorWeight(e, car.lo, car.hi);
  };
  Check(Near(at_headroom(0.0f), 0.0, 1e-6) && at_headroom(-0.05f) == 0.0f && Near(at_headroom(gc::kHeadMin1), 1.0, 1e-6) && Near(at_headroom(gc::kHeadMax0), 1.0, 1e-6) && Near(at_headroom(gc::kHeadMax1), 0.0, 1e-6) &&
            at_headroom(0.9f) == 0.0f,
        "INTERIOR: the headroom's window is 0 at and over the roofline, 1 from 5 cm to 55 cm under it, 0 from 75 cm");
  bool rising = true, falling = true;
  float prev = 0.0f;
  for (int i = 0; i <= 50; ++i) {
    const float w = at_headroom(gc::kHeadMin1 * float(i) / 50.0f);
    rising = rising && w >= prev;
    prev = w;
  }
  prev = 1.0f;
  for (int i = 0; i <= 50; ++i) {
    const float w = at_headroom(gc::kHeadMax0 + (gc::kHeadMax1 - gc::kHeadMax0) * float(i) / 50.0f);
    falling = falling && w <= prev;
    prev = w;
  }
  Check(rising && falling, "INTERIOR: the window rises and falls without a step");
  // the box grown by kBoxGrow: an eye just outside a wall still counts, beyond it does not (any axis)
  {
    float just[3] = {float(car.lo[0]) - 0.08f, eye[1], eye[2]};
    float far[3] = {float(car.lo[0]) - 0.15f, eye[1], eye[2]};
    float end_just[3] = {eye[0], eye[1], float(car.hi[2]) + 0.08f};
    float end_far[3] = {eye[0], eye[1], float(car.hi[2]) + 0.15f};
    Check(gc::InteriorWeight(just, car.lo, car.hi) == 1.0f && gc::InteriorWeight(far, car.lo, car.hi) == 0.0f && gc::InteriorWeight(end_just, car.lo, car.hi) == 1.0f &&
              gc::InteriorWeight(end_far, car.lo, car.hi) == 0.0f,
          "INTERIOR: the eye counts as inside up to 10 cm outside the box (x and z), not beyond");
  }
  // the share
  Check(gc::CabinShare(1.0f, 1.0f) == gc::kOpenShare && gc::CabinShare(0.0f, 1.0f) == 1.0f && gc::CabinShare(1.0f, 0.0f) == 1.0f && gc::CabinShare(0.0f, 0.0f) == 1.0f, "SHARE: whole interior and roof: the floor; no interior or no roof: 1");
  bool in_range = true, monotone = true;
  float prev_share = 1.0f;
  for (int i = 0; i <= 100; ++i) {
    const float s = gc::CabinShare(float(i) / 100.0f, 1.0f);
    in_range = in_range && s >= gc::kOpenShare && s <= 1.0f;
    monotone = monotone && s <= prev_share + 1e-6f;
    prev_share = s;
  }
  prev_share = 1.0f;
  for (int i = 0; i <= 100; ++i) {
    const float s = gc::CabinShare(1.0f, float(i) / 100.0f);
    in_range = in_range && s >= gc::kOpenShare && s <= 1.0f;
    monotone = monotone && s <= prev_share + 1e-6f;
    prev_share = s;
  }
  Check(in_range && monotone, "SHARE: never under the floor (the glass's share: the cabin keeps some of the street's light), never over 1, falling with the camera's weight and with the roof");
  Check(gc::CabinShare(7.0f, 7.0f) == gc::kOpenShare && gc::CabinShare(-3.0f, 1.0f) == 1.0f && gc::CabinShare(1.0f, -3.0f) == 1.0f, "SHARE: weights outside 0..1 are held to it");
  Check(gc::kOpenShare > 0.2f && gc::kOpenShare < 0.45f, "SHARE: the glass's share of the sphere round a driver is a few tenths");
}

// ---------------------------------------------------------------------------
// the box, the edge, the surfaces
// ---------------------------------------------------------------------------
void BoxChecks() {
  Car car;
  float eye[3];
  CockpitEye(car, eye);
  const gc::Rows rows = gc::MakeRows(true, eye, true, car.lo, car.hi, 1.0f);
  const float share = rows[3];
  const float centre[3] = {float(car.lo[0] + 1.0), 0.8f, float(car.lo[2] + 2.0)};
  Check(Near(gc::Term(rows, centre), share, 1e-6), "BOX: the middle of the cabin takes the whole share");
  // outside, in every direction
  bool outside_one = true;
  const float outs[6][3] = {{float(car.lo[0]) - 0.5f, 0.8f, centre[2]}, {float(car.hi[0]) + 0.5f, 0.8f, centre[2]}, {centre[0], 0.8f, float(car.lo[2]) - 0.5f},
                            {centre[0], 0.8f, float(car.hi[2]) + 0.5f}, {centre[0], float(car.hi[1]) + 0.5f, centre[2]},        {centre[0], float(car.lo[1]) - 0.5f, centre[2]}};
  for (const auto& p : outs) outside_one = outside_one && gc::Term(rows, p) == 1.0f;
  Check(outside_one, "BOX: a point outside the box, past any of its six faces, takes the term 1 (the other cars, the road, the world)");
  // the edge: 1 at the face, the share from kEdge in, smooth and monotone between, in each axis alone
  for (int axis = 0; axis < 3; ++axis) {
    float p[3] = {centre[0], centre[1], centre[2]};
    const float lo = float(axis == 0 ? car.lo[0] : axis == 1 ? car.lo[1] : car.lo[2]);
    p[axis] = lo;
    const bool at_face = gc::Term(rows, p) == 1.0f;
    p[axis] = lo + gc::kEdge;
    const bool at_edge = Near(gc::Term(rows, p), share, 1e-5);
    float prev = 1.0f;
    bool mono = true;
    float biggest_step = 0.0f;
    for (int i = 0; i <= 60; ++i) {
      p[axis] = lo + gc::kEdge * float(i) / 60.0f;
      const float t = gc::Term(rows, p);
      mono = mono && t <= prev + 1e-7f;
      biggest_step = std::max(biggest_step, std::fabs(t - prev));
      prev = t;
    }
    Check(at_face && at_edge && mono && biggest_step < 0.05f, Num("BOX: axis %.0f: 1 at the face, the share one edge in, a smooth monotone blend between (biggest step %.4f)", axis, biggest_step));
  }
  // the top and the far faces too (the min and the max side are the same rule)
  {
    float p[3] = {centre[0], float(car.hi[1]), centre[2]};
    const bool top_face = gc::Term(rows, p) == 1.0f;
    p[1] = float(car.hi[1]) - gc::kEdge;
    Check(top_face && Near(gc::Term(rows, p), share, 1e-5), "BOX: the roof's face and the far faces blend as the near ones do");
  }
}

void SurfaceChecks() {
  // the table: only a vehicle's part that is not paint, chrome, glass, emissive or no data is the cabin's
  int cabin = 0, wrong = 0;
  for (uint32_t cls = 0; cls < 32; ++cls) {
    for (uint32_t flags = 0; flags < 8; ++flags) {
      const bool vehicle = (flags & gb::kMaterialFlagVehicle) != 0;
      const bool exterior = cls == uint32_t(gb::SurfaceClass::kNone) || cls == uint32_t(gb::SurfaceClass::kPaint) || cls == uint32_t(gb::SurfaceClass::kChrome) ||
                            cls == uint32_t(gb::SurfaceClass::kGlass) || cls == uint32_t(gb::SurfaceClass::kEmissive);
      const bool want = vehicle && !exterior;
      cabin += gc::IsCabinSurface(cls, flags) ? 1 : 0;
      wrong += gc::IsCabinSurface(cls, flags) != want ? 1 : 0;
    }
  }
  Check(wrong == 0 && cabin == 4 * 27, Num("SURFACES: over every class and flag the cabin's surfaces are the vehicle's parts that are not paint, chrome, glass, emissive or no data (%.0f of 256)", cabin));
  const uint32_t veh = gb::kMaterialFlagVehicle;
  Check(gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kSkin), veh), "SURFACES: the driver's skin (xCharacterSkin: skin and the vehicle flag) is the cabin's");
  Check(!gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kSkin), 0), "SURFACES: a pedestrian (skin, no vehicle flag) is not");
  Check(gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kWorld), veh) && gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kInterior), veh) && gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kMetal), veh),
        "SURFACES: the dash, the wheel, the seats and the trim (world, interior, metal with the vehicle flag) are");
  Check(!gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kPaint), veh) && !gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kChrome), veh) && !gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kGlass), veh),
        "SURFACES: the car's paint, chrome and glass (the hood seen through the windscreen, the mirrors, other cars) keep today's light");
  Check(!gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kWorld), 0) && !gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kRoad), 0) && !gc::IsCabinSurface(uint32_t(gb::SurfaceClass::kFoliage), 3u),
        "SURFACES: the world (no vehicle flag: a wall, the road, leaves) is not, whatever its other flags");
  // the pixel's term: the cabin's surface inside the box takes the share, everything else 1
  Car car;
  float eye[3];
  CockpitEye(car, eye);
  const gc::Rows rows = gc::MakeRows(true, eye, true, car.lo, car.hi, 1.0f);
  const float inside[3] = {float(car.lo[0] + 1.0), 0.8f, float(car.lo[2] + 2.0)};
  const float far_pos[3] = {inside[0] + 50.0f, inside[1], inside[2]};
  Check(Near(gc::ShareFor(rows, inside, uint32_t(gb::SurfaceClass::kSkin), veh), gc::kOpenShare, 1e-6), "TERM: the driver's skin inside the box takes the share");
  Check(gc::ShareFor(rows, inside, uint32_t(gb::SurfaceClass::kPaint), veh) == 1.0f && gc::ShareFor(rows, inside, uint32_t(gb::SurfaceClass::kSkin), 0) == 1.0f &&
            gc::ShareFor(rows, inside, uint32_t(gb::SurfaceClass::kGlass), veh) == 1.0f,
        "TERM: paint, glass and a pedestrian inside the box take 1");
  Check(gc::ShareFor(rows, far_pos, uint32_t(gb::SurfaceClass::kSkin), veh) == 1.0f, "TERM: the driver's class 50 m from the car (another car's driver) takes 1");
  // the rule never adds light and never takes it all
  Lcg rng;
  bool bounded = true;
  for (int i = 0; i < 5000; ++i) {
    const float p[3] = {rng.Range(float(car.lo[0]) - 1.0f, float(car.hi[0]) + 1.0f), rng.Range(-0.5f, 2.0f), rng.Range(float(car.lo[2]) - 1.0f, float(car.hi[2]) + 1.0f)};
    const float t = gc::ShareFor(rows, p, rng.U32() % 32u, rng.U32() % 8u);
    bounded = bounded && t >= gc::kOpenShare - 1e-6f && t <= 1.0f;
  }
  Check(bounded, "TERM: always between the floor and 1: the rule never adds light and never takes the cabin's bounce to 0");
}

// ---------------------------------------------------------------------------
// the rows and the push block
// ---------------------------------------------------------------------------
void LayoutChecks() {
  Car car;
  float eye[3];
  CockpitEye(car, eye);
  const gc::Rows rows = gc::MakeRows(true, eye, true, car.lo, car.hi, 1.0f);
  Check(rows[0] == float(car.lo[0]) && rows[1] == float(car.lo[1]) && rows[2] == float(car.lo[2]) && rows[4] == float(car.hi[0]) && rows[5] == float(car.hi[1]) && rows[6] == float(car.hi[2]) && rows[7] == gc::kEdge,
        "ROWS: row 0 is the box's minimum and the share, row 1 its maximum and the edge");
  Check(gc::kRows == 2 && gc::kRowBytes == 16 && gc::kBytes == 32 && gc::kOffsetBytes == 464 && gc::kOffsetBytes + gc::kBytes == 496, "ROWS: two rows of 16 bytes at byte 464 of the apply's 512 byte buffer");
  // the box's numbers keep their precision a few thousand metres from the origin (a float's)
  Check(std::fabs(double(rows[0]) - car.lo[0]) < 1e-3 && std::fabs(double(rows[6]) - car.hi[2]) < 1e-3, "ROWS: a box at 1000 m keeps a millimetre in a float");
  // the push block's last word: the flags and the view keep their bits, the slot is the upper half
  const uint32_t word = 3u | (9u << 8);
  const uint32_t packed = gc::PackMaterialSlot(word, 4242u);
  Check((packed & 0xFFFFu) == word && (packed >> 16) == 4242u, "PUSH: the flags and the view stay in the lower half, the material plane's slot is the upper half");
  Check((gc::PackMaterialSlot(word, gc::kNoMaterialSlot) >> 16) == 0xFFFFu && (gc::PackMaterialSlot(word, 0x10000u) >> 16) == 0xFFFFu && (gc::PackMaterialSlot(word, 0xFFFFFFFFu) >> 16) == 0xFFFFu,
        "PUSH: no slot, and a slot that does not fit in 16 bits, read as none");
  Check((gc::PackMaterialSlot(0xFFFFFFFFu, 7u) & 0xFFFFu) == 0xFFFFu && (gc::PackMaterialSlot(0xFFFFFFFFu, 7u) >> 16) == 7u, "PUSH: an old upper half is replaced, not or'ed");
}

// ---------------------------------------------------------------------------
// the .hlsli compiled as C++
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
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
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
#include "hlsl/gi_cabin_common.hlsli"
}  // namespace hlsl_shim

void TwinChecks() {
  Lcg rng;
  Car car;
  float eye[3];
  CockpitEye(car, eye);
  const gc::Rows on = gc::MakeRows(true, eye, true, car.lo, car.hi, 1.0f);
  const gc::Rows off = gc::OffRows();
  int bad_edge = 0, bad_share = 0, bad_surface = 0, n_inside = 0;
  for (int i = 0; i < 40000; ++i) {
    const gc::Rows& rows = (rng.U32() % 4u == 0u) ? off : on;
    const float pos[3] = {rng.Range(float(car.lo[0]) - 0.3f, float(car.hi[0]) + 0.3f), rng.Range(-0.2f, 1.6f), rng.Range(float(car.lo[2]) - 0.3f, float(car.hi[2]) + 0.3f)};
    const float e = rng.Range(0.0f, 0.2f);
    const float a = gc::EdgeWeight(pos, &on[0], &on[4], e);
    const float b = hlsl_shim::GicEdgeWeight(hlsl_shim::float3(pos[0], pos[1], pos[2]), hlsl_shim::float3(on[0], on[1], on[2]), hlsl_shim::float3(on[4], on[5], on[6]), e);
    if (!Near(a, b, 1e-6)) ++bad_edge;
    const uint32_t cls = rng.U32() % 32u, flags = rng.U32() % 8u;
    const float ts = gc::ShareFor(rows, pos, cls, flags);
    const float hs = hlsl_shim::GicShare(hlsl_shim::float4(rows[0], rows[1], rows[2], rows[3]), hlsl_shim::float4(rows[4], rows[5], rows[6], rows[7]), hlsl_shim::float3(pos[0], pos[1], pos[2]), cls, flags);
    if (!Near(ts, hs, 1e-6)) ++bad_share;
    if (ts < 1.0f) ++n_inside;
  }
  Check(bad_edge == 0 && bad_share == 0 && n_inside > 500, Num("TWIN: the .hlsli's edge weight and share equal the C++ ones on 40000 random pixels (%.0f differ, %.0f of them took a share under 1)", bad_edge + bad_share, n_inside));
  for (uint32_t cls = 0; cls < 32; ++cls)
    for (uint32_t flags = 0; flags < 8; ++flags) bad_surface += hlsl_shim::GicIsCabinSurface(cls, flags) != gc::IsCabinSurface(cls, flags) ? 1 : 0;
  Check(bad_surface == 0, "TWIN: the .hlsli's cabin surfaces equal the C++ ones over every class and flag");
  bool smooth_same = true;
  for (int i = -20; i <= 120; ++i) smooth_same = smooth_same && Near(hlsl_shim::GicSmooth(float(i) / 100.0f), gc::SmoothStep(0.0f, 1.0f, float(i) / 100.0f), 1e-7);
  Check(smooth_same, "TWIN: the smooth step is the C++ one");
}

// ---------------------------------------------------------------------------
// the text: the #defines, the shader, the host, the settings
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t At(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }

void TextChecks(const std::string& dir) {
  const std::string common = ReadFile(dir + "/hlsl/gi_cabin_common.hlsli");
  const std::string ps = ReadFile(dir + "/hlsl/gi_apply_ps.hlsl");
  const std::string cpp = ReadFile(dir + "/gi_apply.cpp");
  const std::string hdr = ReadFile(dir + "/gi_apply.h");
  const std::string renderer = ReadFile(dir + "/renderer.cpp");
  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  Check(!common.empty() && !ps.empty() && !cpp.empty() && !hdr.empty() && !renderer.empty() && !cmake.empty(), "the sources are found");
  // every number #define of the .hlsli against the C++ value
  const std::map<std::string, double> expected = {{"GIC_CLASS_NONE", double(gb::SurfaceClass::kNone)},         {"GIC_CLASS_PAINT", double(gb::SurfaceClass::kPaint)},
                                                  {"GIC_CLASS_CHROME", double(gb::SurfaceClass::kChrome)},     {"GIC_CLASS_GLASS", double(gb::SurfaceClass::kGlass)},
                                                  {"GIC_CLASS_EMISSIVE", double(gb::SurfaceClass::kEmissive)}, {"GIC_FLAG_VEHICLE", double(gb::kMaterialFlagVehicle)},
                                                  {"GIC_MATERIAL_SLOT_SHIFT", double(gc::kMaterialSlotShift)}, {"GIC_NO_MATERIAL_SLOT", double(gc::kNoMaterialSlot)}};
  int defined = 0, wrong = 0;
  std::istringstream lines(common);
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("#define GIC_", 0) != 0) continue;
    std::istringstream words(line);
    std::string define, name, value;
    words >> define >> name >> value;
    const auto it = expected.find(name);
    if (it == expected.end()) continue;
    ++defined;
    while (!value.empty() && (value.back() == 'u' || value.back() == 'U') && value.rfind("0x", 0) != 0) value.pop_back();
    if (value.rfind("0x", 0) == 0) {
      while (!value.empty() && (value.back() == 'u' || value.back() == 'U')) value.pop_back();
    }
    const double v = value.rfind("0x", 0) == 0 ? double(std::strtoull(value.c_str(), nullptr, 16)) : std::strtod(value.c_str(), nullptr);
    if (!Near(v, it->second, 1e-12)) {
      ++wrong;
      std::printf("  %s: .hlsli %.9g, C++ %.9g\n", name.c_str(), v, it->second);
    }
  }
  Check(defined == int(expected.size()) && wrong == 0, Num("the .hlsli's #defines are the G-buffer's classes and flag and the rules' slot bits: %.0f of them compared, %.0f differ", defined, wrong));
  // the limits of the rules header are not repeated in the .hlsli: the edge and the share come from the rows
  Check(!Has(common, "kOpenShare") && !Has(common, "0.3f") && Has(common, "float share = rowA.w;") && Has(common, "rowB.w"), "the .hlsli takes the share and the edge from the host's rows, with no number of its own");

  // the shader: the include, the rows after the apply's 9, the material slot in the last word, the term before the add and before the debug views
  Check(Has(ps, "#include \"gi_cabin_common.hlsli\"") && Has(ps, "return GiaRowFloats(GIA_ROWS + i);") && Has(ps, "return g_push.info.w >> GIC_MATERIAL_SLOT_SHIFT;"), "SHADER: the cabin's rows follow the apply's GIA_ROWS and the material slot is the push's last word's upper half");
  const size_t at_gate = At(ps, "if (cabinA.w < 1.0f && GiaMaterialSlot() != GIC_NO_MATERIAL_SLOT)");
  const size_t at_term = At(ps, "cabin = GicShare(cabinA, GiaCabinRow(1u), GiaReceiverPosition(pixel, t, fr), GbRt3Class(rt3), GbRt3Flags(rt3));");
  const size_t at_add = At(ps, "GiaBounceAdd(albedo, rt2.w, eNet, fr.strength, fr.fieldShare * cabin, vehicle, aoTerm, fr.ambient, fr.sunLuma, nDotL, fr.cap)");
  const size_t at_view9 = At(ps, "if (view == 9u)");
  Check(at_gate != std::string::npos && at_term != std::string::npos && at_add != std::string::npos && at_view9 != std::string::npos && at_gate < at_term && at_term < at_add && at_add < at_view9 &&
            Has(ps, "float cabin = 1.0f;") && Has(ps, "float2 rt3 = GiaLoadPlane(GiaMaterialSlot(), pixel).xy;"),
        "SHADER: a frame with no rule reads nothing (share 1: the gate), the term multiplies the field's share in the add, and the debug view 9 shows it");
  Check(Count(ps, "GicShare(") == 1, "SHADER: one place applies the term (the lookup pass, the views of the field itself and the per-pixel pass are untouched)");

  // the host
  Check(Has(cpp, "static_assert(gi_cabin::kOffsetBytes == ga::kBufferBytes && gi_cabin::kOffsetBytes + gi_cabin::kBytes <= ga::kBufferAlloc") &&
            Has(cpp, "std::memcpy(static_cast<uint8_t*>(mapped) + gi_cabin::kOffsetBytes, cabin.data(), gi_cabin::kBytes);"),
        "HOST: the rows go right after the apply's 9 in the apply's buffer, which has the room");
  Check(Has(cpp, "cabin_on_ = larecomp::settings::mcla_gpu_gi_cabin.Get();") && Has(cpp, "if (enabled_ && cabin_on_ && car_frame_ != nullptr) {") && Has(cpp, "car_frame_->Request();"),
        "HOST: the setting is read at each frame's start, and the car's frames are asked for only while the field runs and the rule is on");
  Check(Has(cpp, "if (cabin_on_ && car_frame_ != nullptr && gb.Has(gbuffer::kPlaneMaterial) &&") &&
            Has(cpp, "interior_camera::ApplyGiShield(cockpit_only, CurrentRenderedCameraRig())) {") && Has(cpp, "if (car.valid) {") &&
            Has(cpp, "cabin = gi_cabin::MakeRows(true, fr.position, car.bounds_valid, car.bounds_min, car.bounds_max, 1.0f);") && Has(cpp, "material_slot = gb.slot[gbuffer::kPlaneMaterial];"),
        "HOST: the rule needs the setting, the car's frame with its box and the material plane; the eye is the camera the apply uses");
  Check(Has(cpp, "gi_cabin::PackMaterialSlot(ga::PackInfoW(") && Has(cpp, "cabin[3] < 1.0f ? material_slot : gi_cabin::kNoMaterialSlot"), "HOST: the slot is in the push block only where a rule is");
  Check(Has(hdr, "void SetCarFrame(CarFrame* car_frame) { car_frame_ = car_frame; }") && Has(hdr, "class CarFrame;") && Has(hdr, "bool cabin_on_ = true;"), "HOST: the header has the owner and the switch");
  const size_t at_car_init = At(renderer, "car_frame_.Initialize(guest_memory_);");
  const size_t at_set = At(renderer, "gi_apply_.SetCarFrame(&car_frame_);");
  Check(at_car_init != std::string::npos && at_set != std::string::npos && at_car_init < at_set && Has(cpp, "car_frame_ = nullptr;"), "HOST: the renderer gives the apply the car's frame after it is initialised; shutdown lets go of it");
  // the per-pixel pass keeps its rows where it had them (the cabin's rows are not in the buffer it copies)
  const std::string pixel_rules = ReadFile(dir + "/gi_pixel_rules.h");
  Check(Has(pixel_rules, "inline constexpr uint32_t kRowsOffsetBytes = 464;") && Has(ReadFile(dir + "/gi_apply_rules.h"), "static_assert(kBufferBytes == 464 && kBufferBytes <= kBufferAlloc"),
        "the apply's 9 rows still end at byte 464 and the per-pixel pass's rows still start there (its buffer is its own: the cabin's rows are the apply's buffer's last two)");

  // the setting, its debug row and the documentation
  const std::string settings = ReadFile(dir + "/../settings/generated/settings_gpu.h");
  const std::string define = std::string("REX_") + "SETTING(";  // (split: tools/check_settings_exposure.py takes this text for a definition)
  const size_t at_setting = At(settings, define + "bool, mcla_gpu_gi_cabin, true,");
  const size_t at_trait = at_setting == std::string::npos ? std::string::npos : At(settings, "rex::kDebugOnly);", at_setting);
  const size_t at_next = at_setting == std::string::npos ? std::string::npos : At(settings, define, at_setting + 10);
  Check(at_setting != std::string::npos && at_trait != std::string::npos && at_trait < at_next && Has(settings, "inline auto& mcla_gpu_gi_cabin = g_gpu.mcla_gpu_gi_cabin;"), "SETTING: mcla_gpu_gi_cabin is a debug-only bool, on by default, with its alias");
  Check(Has(ReadFile(dir + "/../settings/generated/manifest.json"), "\"mcla_gpu_gi_cabin\": {") && Has(ReadFile(dir + "/../settings/generated/settings_meta.inc"), "{\"mcla_gpu_gi_cabin\", \"MCLA/GPU\"},"), "SETTING: it is in the manifest and the meta list");
  const std::string menu = ReadFile(dir + "/../mc_engine/menus/debug_menu_pages.cpp");
  const size_t at_table = At(menu, "const ItemDef kDebugPictureFixesItems[] = {");
  const size_t at_row = At(menu, "NativeOnly(Bool(\"PM_RxDbgGiCabin\", \"mcla_gpu_gi_cabin\", \"CABIN BOUNCE SHIELD: \")),");
  const size_t at_end = at_table == std::string::npos ? std::string::npos : At(menu, "};", at_table);
  Check(at_table != std::string::npos && at_row != std::string::npos && at_table < at_row && at_row < at_end, "SETTING: the debug menu's PICTURE FIXES page has the row (the A/B of the rule)");
  Check(Has(ReadFile(dir + "/../../documentation/DEBUG.md"), "`mcla_gpu_gi_cabin` (debug-only, live, on by default)"), "SETTING: documentation/DEBUG.md describes it");
  Check(Has(cmake, "\nadd_executable(larecomp_gi_cabin_test EXCLUDE_FROM_ALL gi_cabin_test.cpp)\n"), "CMake: this test is listed (not commented out)");
  // the screen-space bounce light is not touched: it gathers what is on the picture, and a roof never is
  Check(!Has(ReadFile(dir + "/hlsl/ao_apply_ps.hlsl"), "gi_cabin") && !Has(ReadFile(dir + "/hlsl/ao_main_ps.hlsl"), "gi_cabin"), "the screen-space bounce light's passes have no cabin term (the header says why)");
}

std::string DirOfThisFile() {
  std::string path = __FILE__;
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = DirOfThisFile();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-v") {
      g_verbose = true;
    } else {
      dir = a;
    }
  }
  IdentityChecks();
  InteriorChecks();
  BoxChecks();
  SurfaceChecks();
  LayoutChecks();
  TwinChecks();
  TextChecks(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

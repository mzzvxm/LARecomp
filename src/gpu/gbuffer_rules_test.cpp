// Check of the G-buffer's interface (gbuffer_types.h, gbuffer_rules.h, hlsl/gbuffer_common.hlsli, light_set.h) and of the host's pure rules. A program of
// its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_gbuffer_rules_test [<hlsl/gbuffer_common.hlsli>]
//
// It checks: the octahedral code (the 16 bit error, "no data" kept free, the fold), the albedo and the material word, the derivative normal's arithmetic,
// the constants block's layout, the light set's conversion, the draw decision, the window, the trigger and the setting's words; then it compiles the
// .hlsli ITSELF as C++ (through the shim of the HLSL vector types below) and compares each of its functions with the twin on random inputs, and compares
// every GB_* define of the .hlsli with the C++ value. Without an argument the .hlsli is found next to this file (hlsl/gbuffer_common.hlsli).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "light_set.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the file uses, in a namespace of its own so that the standard library's abs, sqrt ... are not in the way
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
#include "hlsl/gbuffer_common.hlsli"
}  // namespace hlsl_shim

namespace gb = mcla::gpu::gbuffer;

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
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[320];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}

struct Lcg {
  uint32_t state = 20261119u;
  float Next() {  // 0 to 1
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  gb::Vec3 Direction() {
    // a uniform direction on the sphere
    const float z = Range(-1.0f, 1.0f);
    const float a = Range(0.0f, 6.2831853f);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
  }
};

// The angle between two directions in degrees, in double with atan2 of the cross product's length (acos of a float dot is only good to 0.02 degrees).
double AngleDegrees(gb::Vec3 a, gb::Vec3 b) {
  const double ax = a.x, ay = a.y, az = a.z, bx = b.x, by = b.y, bz = b.z;
  const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
  const double cross_len = std::sqrt(cx * cx + cy * cy + cz * cz);
  const double dot = ax * bx + ay * by + az * bz;
  return std::atan2(cross_len, dot) * 180.0 / 3.14159265358979323846;
}

// ---------------------------------------------------------------------------
// the octahedral code
// ---------------------------------------------------------------------------
void OctChecks() {
  Lcg rng;
  double worst = 0.0;
  double worst_unit = 0.0;
  for (int i = 0; i < 300000; ++i) {
    const gb::Vec3 n = rng.Direction();
    const gb::Rt1Texel t = gb::Rt1Pack(n);
    const gb::Vec3 back = gb::Rt1Decode(t);
    worst = std::max(worst, AngleDegrees(n, back));
    worst_unit = std::max(worst_unit, std::fabs(double(gb::Length(back)) - 1.0));
    if (!gb::Rt1Valid(t)) {
      Check(false, "a random direction wrote the no data code");
      break;
    }
  }
  Check(worst < 0.004, Num("the 16 bit octahedral code is within %.5f degrees (limit 0.004) over 300000 random directions", worst));
  Check(worst_unit < 1e-5, Num("the decoded normal is unit length to %.2e", worst_unit));

  // the axes and the diagonals come back to a hair
  for (int sx = -1; sx <= 1; ++sx) {
    for (int sy = -1; sy <= 1; ++sy) {
      for (int sz = -1; sz <= 1; ++sz) {
        if (sx == 0 && sy == 0 && sz == 0) continue;
        const gb::Vec3 n{float(sx), float(sy), float(sz)};
        const gb::Rt1Texel t = gb::Rt1Pack(n);
        Check(gb::Rt1Valid(t), Num("the direction (%.0f %.0f %.0f) is a valid code", sx, sy, sz));
        Check(AngleDegrees(n, gb::Rt1Decode(t)) < 0.004, Num("the direction (%.0f %.0f %.0f) round trips", sx, sy, sz));
      }
    }
  }

  // "no data" stays free: the directions whose raw code would round to (0, 0), those near -Z leaning to (-, -), are moved by a step
  {
    const gb::Vec3 hard{-1e-5f, -1e-5f, -1.0f};
    const gb::Vec2 raw = gb::OctEncode(hard);
    Check(gb::QuantizeUnorm(raw.x, 65535) == 0 && gb::QuantizeUnorm(raw.y, 65535) == 0,
          "the test direction really would round to (0, 0) without the rule");
    const gb::Rt1Texel t = gb::Rt1Pack(hard);
    Check(gb::Rt1Valid(t) && t.x == 1 && t.y == 0, "the direction that would round to (0, 0) is written (1, 0)");
    Check(AngleDegrees(hard, gb::Rt1Decode(t)) < 0.005, "and is still within 0.005 degrees of itself");
  }
  {
    int zero_codes = 0;
    for (int i = 0; i < 400000; ++i) {
      // a swarm near the corner of the octahedron where the code is smallest
      const gb::Vec3 n{-rng.Range(0.0f, 1e-4f), -rng.Range(0.0f, 1e-4f), -1.0f};
      zero_codes += gb::Rt1Valid(gb::Rt1Pack(n)) ? 0 : 1;
    }
    Check(zero_codes == 0, "no direction near the corner of the octahedron writes the no data code");
  }
  // a non direction writes "no data"
  Check(!gb::Rt1Valid(gb::Rt1Pack({0.0f, 0.0f, 0.0f})), "the zero vector writes the no data code");
  Check(!gb::Rt1Valid(gb::Rt1Pack({std::nanf(""), 0.0f, 1.0f})), "a NaN direction writes the no data code");
  // the clear value is not a normal
  Check(!gb::Rt1Valid(gb::Rt1Texel{0, 0}), "the clear value (0, 0) is not valid");
  Check(gb::Rt1Valid(gb::Rt1Texel{1, 0}) && gb::Rt1Valid(gb::Rt1Texel{0, 1}), "(1, 0) and (0, 1) are valid");

  // the fold: a direction and its mirror image about the z plane are not the same code
  const gb::Vec3 up{0.3f, 0.5f, 0.8124f};
  const gb::Vec3 down{0.3f, 0.5f, -0.8124f};
  Check(!(gb::Rt1Pack(up) == gb::Rt1Pack(down)), "mirror directions have different codes");
  Check(AngleDegrees(down, gb::Rt1Decode(gb::Rt1Pack(down))) < 0.004, "the folded half decodes");
  // the length does not matter
  Check(gb::Rt1Pack({3.0f, 4.0f, 5.0f}) == gb::Rt1Pack({0.3f, 0.4f, 0.5f}), "the code does not depend on the length");
}

// ---------------------------------------------------------------------------
// albedo, material word, classes
// ---------------------------------------------------------------------------
void AlbedoAndMaterialChecks() {
  // the albedo: sqrt keeps the dark steps; the round trip error is under half a step of the stored value
  double worst = 0.0;
  double dark_step = 0.0;
  for (int i = 0; i <= 255; ++i) {
    const float a = float(i) / 255.0f;
    const uint32_t byte = gb::AlbedoPack(a);
    worst = std::max(worst, std::fabs(double(gb::AlbedoUnpack(byte)) - double(a)));
  }
  dark_step = double(gb::AlbedoUnpack(1)) - double(gb::AlbedoUnpack(0));
  Check(gb::AlbedoPack(0.0f) == 0 && gb::AlbedoPack(1.0f) == 255, "albedo 0 and 1 store as 0 and 255");
  Check(worst < 0.004, Num("the stored albedo is within %.5f of the albedo (a step of the byte is at most 2/255)", worst));
  Check(dark_step < 0.0001, Num("the first dark step is %.6f (gamma 2 keeps the darks fine)", dark_step));
  Check(gb::AlbedoPack(-1.0f) == 0 && gb::AlbedoPack(7.0f) == 255, "albedo is clamped");

  // the material word and RT3
  int words = 0;
  for (uint32_t c = 0; c < uint32_t(gb::SurfaceClass::kCount); ++c) {
    for (uint32_t f = 0; f < 8; ++f) {
      for (float s : {0.0f, 0.25f, 0.5f, 1.0f}) {
        for (float lit : {0.0f, 1.0f}) {
          const uint32_t w = gb::MaterialWord(s, c, f, lit);
          ++words;
          const uint32_t cf = gb::MaterialRt3ClassAndFlags(w);
          if (uint32_t(gb::ClassOfByte(cf)) != c || gb::FlagsOfByte(cf) != f || gb::MaterialRt3Smoothness(w) != uint32_t(s * 255.0f + 0.5f) ||
              gb::MaterialLitByte(w) != (lit > 0.5f ? 255u : 0u) || (w >> 24) != 0) {
            Check(false, Num("the material word of class %.0f flags %.0f does not round trip", c, f));
          }
        }
      }
    }
  }
  Check(words == 16 * 8 * 4 * 2, "every class, flag set, smoothness and lit weight was checked");
  Check(gb::MaterialRt3ClassAndFlags(gb::MaterialWord(0.5f, gb::SurfaceClass::kPaint, gb::kMaterialFlagVehicle | gb::kMaterialFlagSsr, 1.0f)) ==
            (uint32_t(gb::SurfaceClass::kPaint) | ((gb::kMaterialFlagVehicle | gb::kMaterialFlagSsr) << 5)),
        "RT3.g is the class in the low 5 bits and the flags above them");
  Check((gb::kMaterialFlagSsr << gb::kMaterialFlagsShift) == 0x2000u && (gb::kMaterialFlagVehicle << gb::kMaterialFlagsShift) == 0x8000u,
        "the flags are bits 13 to 15 of the word");
  Check(gb::kClassMask == 31 && uint32_t(gb::SurfaceClass::kCount) <= 32, "the class fits 5 bits");
  Check(gb::kClassBits + 3 == 8, "the class and the three flags fill one byte");
  // class 0 is "no data" and the cleared plane says so
  Check(gb::ClassOfByte(0) == gb::SurfaceClass::kNone && gb::FlagsOfByte(0) == 0, "a cleared RT3 is class none, no flags");
  // the planes' formats and bytes (the survey's 10 bytes a sample for the full set)
  uint32_t bytes = 0;
  for (uint32_t p = 0; p < gb::kPlaneCount; ++p) bytes += gb::BytesPerSample(gb::kPlaneFormats[p]);
  Check(bytes == 10, "the three planes are 10 bytes a sample");
  Check(gb::BytesPerSample(gb::kPlaneFormats[gb::kPlaneNormal]) == 4, "the normal plane is 4 bytes a sample");
  Check(gb::kAllPlanes == 7 && gb::PlaneBit(gb::kPlaneMaterial) == 4, "the plane bits");
}

// ---------------------------------------------------------------------------
// the derivative normal
// ---------------------------------------------------------------------------
gb::Quat RandomQuat(Lcg& rng) {
  // a random rotation: an axis and an angle
  const gb::Vec3 axis = rng.Direction();
  const float angle = rng.Range(0.0f, 6.2831853f);
  const float s = std::sin(angle * 0.5f);
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}

void DerivativeChecks() {
  Lcg rng;
  // QuatFromBasis: the rotation takes the axes to the basis
  double worst_basis = 0.0;
  for (int i = 0; i < 2000; ++i) {
    const gb::Quat q = RandomQuat(rng);
    const gb::Vec3 right = gb::QuatRotate(q, {1, 0, 0});
    const gb::Vec3 up = gb::QuatRotate(q, {0, 1, 0});
    const gb::Vec3 forward = gb::QuatRotate(q, {0, 0, 1});
    const gb::Quat r = gb::QuatFromBasis(right, up, forward);
    for (gb::Vec3 axis : {gb::Vec3{1, 0, 0}, gb::Vec3{0, 1, 0}, gb::Vec3{0, 0, 1}, gb::Vec3{0.3f, -0.5f, 0.8f}}) {
      worst_basis = std::max(worst_basis, AngleDegrees(gb::QuatRotate(q, axis), gb::QuatRotate(r, axis)));
    }
  }
  Check(worst_basis < 0.001, Num("QuatFromBasis rotates like the quaternion it came from (%.5f degrees)", worst_basis));

  // a plane at a random place and orientation, seen by a random camera: the normal from the derivatives of the reconstructed world point is the plane's
  double worst = 0.0;
  int tested = 0;
  for (int i = 0; i < 4000; ++i) {
    const float tan_x = rng.Range(0.4f, 1.4f);
    const float tan_y = tan_x * 9.0f / 16.0f;
    const float width = rng.Range(640.0f, 3840.0f);
    const float height = width * 9.0f / 16.0f;
    float ray[4];
    gb::MakeViewRay(tan_x, tan_y, width, height, ray);
    const gb::Quat cam = RandomQuat(rng);
    // the plane in view space: a unit normal facing the camera (n . forward < 0) and a distance
    gb::Vec3 n_view = rng.Direction();
    if (n_view.z > -0.15f) n_view.z = -0.15f - std::fabs(n_view.z);  // keep it from edge-on
    n_view = gb::Normalize(n_view);
    const float dist = rng.Range(2.0f, 80.0f);  // n . p = dist ... for a plane facing the camera the sign makes dist negative: use -dist below
    const float px = rng.Range(10.0f, width - 10.0f);
    const float py = rng.Range(10.0f, height - 10.0f);
    // the depth of the plane along the pixel's ray (ray direction (rx, ry, 1)): n . (rx z, ry z, z) = -dist
    auto w_at = [&](float x, float y) {
      const gb::Vec3 r{x * ray[0] + ray[1], y * ray[2] + ray[3], 1.0f};
      const float denom = gb::Dot(n_view, r);
      const float z = -dist / denom;
      return 1.0f / z;
    };
    const float denom = gb::Dot(n_view, {px * ray[0] + ray[1], py * ray[2] + ray[3], 1.0f});
    if (std::fabs(denom) < 0.2f || (-dist / denom) <= 0.0f) continue;  // the plane is not in front of the pixel, or is seen too edge-on for float32 differences
    const gb::Vec3 p00 = gb::WorldPoint(ray, cam, px, py, w_at(px, py));
    const gb::Vec3 p10 = gb::WorldPoint(ray, cam, px + 1.0f, py, w_at(px + 1.0f, py));
    const gb::Vec3 p01 = gb::WorldPoint(ray, cam, px, py + 1.0f, w_at(px, py + 1.0f));
    const gb::Vec3 n = gb::FaceNormal(p10 - p00, p01 - p00, p00 * -1.0f);
    const gb::Vec3 expected = gb::QuatRotate(cam, n_view);
    worst = std::max(worst, AngleDegrees(n, expected));
    // and it faces the camera: the direction from the surface to the camera is minus the point
    if (gb::Dot(n, p00 * -1.0f) < 0.0f) Check(false, "the derivative normal faces away from the camera");
    ++tested;
  }
  Check(tested > 1500, Num("%.0f planes were tested", tested));
  // float32 positions 2 to 80 m away: the pixel to pixel differences are 1e-3 to 1e-1, so the normal is good to about a tenth of a degree (the shader has the same limit)
  Check(worst < 0.1, Num("the normal from the derivatives is the plane's to %.5f degrees (limit 0.1)", worst));

  // the view ray constants: the top left pixel's ray edge is (-tan_x, +tan_y), the bottom right's is (+tan_x, -tan_y)
  float ray[4];
  gb::MakeViewRay(1.0f, 0.5625f, 1920.0f, 1080.0f, ray);
  Check(std::fabs(0.0f * ray[0] + ray[1] + 1.0f) < 1e-6f && std::fabs(0.0f * ray[2] + ray[3] - 0.5625f) < 1e-6f, "pixel (0, 0) is the top left of the view");
  Check(std::fabs(1920.0f * ray[0] + ray[1] - 1.0f) < 1e-5f && std::fabs(1080.0f * ray[2] + ray[3] + 0.5625f) < 1e-5f, "pixel (width, height) is the bottom right");
}

// ---------------------------------------------------------------------------
// the constants block, the light set
// ---------------------------------------------------------------------------
void LayoutChecks() {
  Check(sizeof(gb::GBufferConstants) == 48, "GBufferConstants is 48 bytes");
  Check(offsetof(gb::GBufferConstants, mode) == gb::kGBufferRowMode && offsetof(gb::GBufferConstants, view_ray) == gb::kGBufferRowViewRay &&
            offsetof(gb::GBufferConstants, camera_to_world) == gb::kGBufferRowCamera,
        "the rows of the block are at 0, 16 and 32");
  Check(alignof(gb::GBufferConstants) == 4, "no padding is expected beyond the floats'");
  gb::GBufferConstants a, b;
  Check(a == b, "two default blocks are equal");
  b.material = 1;
  Check(!(a == b), "a block that differs is not equal");
  Check(a.camera_to_world[3] == 1.0f && a.camera_to_world[0] == 0.0f, "the default camera rotation is the identity quaternion");
  Check(sizeof(gb::GBufferFrame) <= 64, "GBufferFrame is small");
  gb::GBufferFrame frame;
  Check(!frame.Has(gb::kPlaneNormal), "a default frame has no plane");
  frame.valid = gb::PlaneBit(gb::kPlaneNormal);
  Check(!frame.Has(gb::kPlaneNormal), "a plane in `valid` without a slot is not there");
  frame.slot[gb::kPlaneNormal] = 7;
  Check(frame.Has(gb::kPlaneNormal) && !frame.Has(gb::kPlaneAlbedo), "a plane with its bit and its slot is there");
}

struct FakeHeadlight {
  float pos[3];
  float dir[3];
  float cos_outer;
  float cos_inner;
  float range;
  float color[3];
  uint32_t car_key;
  bool is_player;
};
struct FakeSet {
  std::array<FakeHeadlight, 4> lights;
  uint32_t count = 0;
};

void LightSetChecks() {
  using namespace larecomp::gpu;
  FakeSet set;
  set.count = 2;
  set.lights[0] = {{1, 2, 3}, {0, 0, 1}, 0.8f, 0.9f, 40.0f, {1, 0.9f, 0.7f}, 77, true};
  set.lights[1] = {{4, 5, 6}, {1, 0, 0}, 0.7f, 0.85f, 30.0f, {0.5f, 0.5f, 0.5f}, 78, false};
  const LightSet out = LightSetFromHeadlights(set, 12);
  Check(out.count == 2 && out.frame == 12, "the set carries the count and the frame");
  Check(out.lights[0].pos[1] == 2.0f && out.lights[0].dir[2] == 1.0f && out.lights[0].cos_outer == 0.8f && out.lights[0].cos_inner == 0.9f &&
            out.lights[0].range == 40.0f && out.lights[0].color[1] == 0.9f && out.lights[0].car_key == 77,
        "a headlight's fields are the light's");
  Check(out.lights[0].kind == LightKind::kSpot && out.lights[0].source == LightSource::kHeadlight && out.lights[0].shadow_view == kNoShadowView,
        "a headlight is a spot light of source headlight with no shadow view");
  Check((out.lights[0].flags & kLightFlagPlayer) != 0 && (out.lights[1].flags & kLightFlagPlayer) == 0, "the player's lamp keeps its flag");
  Check(kMaxLights == 32 && out.lights.size() == kMaxLights, "the set holds 32 lights");
  Light point;
  Check(point.kind == LightKind::kSpot && point.cos_outer == -1.0f && point.shadow_view == kNoShadowView && point.range == 0.0f, "the default light is off");
}

// ---------------------------------------------------------------------------
// the host's rules
// ---------------------------------------------------------------------------
gb::DrawFacts GoodDraw() {
  gb::DrawFacts f;
  f.active = true;
  f.window_open = true;
  f.main_samples = 2;
  f.guest_color_count = 1;
  f.has_depth = true;
  f.rt0_blended = false;
  f.rt0_writes = true;
  f.ps_variant_before = 0;
  f.ps_variant_mask = (1u << 14) | (1u << 15);
  f.variant_id = 14;
  f.alpha_variant_id = 15;
  return f;
}

void RuleChecks() {
  using gb::Why;
  {
    const gb::DrawDecision d = gb::Decide(GoodDraw());
    Check(d.why == Why::kWrites && d.variant == 14 && d.attach(), "an opaque lit draw of the main view takes the variant");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.ps_variant_before = 1;
    const gb::DrawDecision d = gb::Decide(f);
    Check(d.why == Why::kWrites && d.variant == 15, "an alpha tested draw takes the alpha test twin");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.ps_variant_mask = (1u << 14);
    f.ps_variant_before = 1;
    Check(gb::Decide(f).why == Why::kMaskedNoVariant && gb::Decide(f).attach() == gb::kMaskedDrawsAttach && gb::Decide(f).variant == 0, "an alpha draw whose shader has no twin writes nothing (and binds the sidecar masked only when kMaskedDrawsAttach)");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.ps_variant_mask = 0;
    Check(gb::Decide(f).why == Why::kMaskedNoVariant && gb::Decide(f).attach() == gb::kMaskedDrawsAttach, "a shader with no variant writes nothing");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.rt0_blended = true;
    Check(gb::Decide(f).why == Why::kMaskedBlended && gb::Decide(f).attach() == gb::kMaskedDrawsAttach, "a blended draw writes nothing");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.rt0_writes = false;
    Check(gb::Decide(f).why == Why::kMaskedNoColor && gb::Decide(f).attach() == gb::kMaskedDrawsAttach, "a draw that writes no colour writes nothing");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.ps_variant_before = 7;
    Check(gb::Decide(f).why == Why::kMaskedOtherVariant && gb::Decide(f).attach() == gb::kMaskedDrawsAttach, "a draw with another variant writes nothing");
    Check(!gb::kMaskedDrawsAttach, "while Plume's D3D12 back end has no independent blend only the draws that write the sidecar bind it (gbuffer_rules.h kMaskedDrawsAttach)");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.active = false;
    Check(gb::Decide(f).why == Why::kOff && !gb::Decide(f).attach(), "off: nothing");
    f = GoodDraw();
    f.main_samples = 0;
    Check(gb::Decide(f).why == Why::kNotMainView && !gb::Decide(f).attach(), "not the main view: nothing");
    f = GoodDraw();
    f.main_samples = 4;
    Check(gb::Decide(f).why == Why::kPauseView && !gb::Decide(f).attach(), "the pause world (4 samples): nothing in phase 1");
    f = GoodDraw();
    f.main_samples = 1;
    Check(gb::Decide(f).why == Why::kWrites, "the play view without MSAA (1 sample) writes");
    f = GoodDraw();
    f.guest_color_count = 2;
    Check(gb::Decide(f).why == Why::kNotOneColor, "two guest colour targets: nothing");
    f = GoodDraw();
    f.has_depth = false;
    Check(gb::Decide(f).why == Why::kNoDepth, "no depth: nothing");
    f = GoodDraw();
    f.window_open = false;
    Check(gb::Decide(f).why == Why::kWindowClosed && !gb::Decide(f).attach(), "a closed window: nothing");
  }
  // the window
  {
    gb::Window w;
    w.BeginFrame();
    Check(w.GateForDraw() == gb::Window::Gate::kNeedsClear, "a frame with no clear yet asks for one on the first draw");
    w.OnClear(true);
    Check(w.GateForDraw() == gb::Window::Gate::kOpen && w.clears() == 1 && w.partial_clears() == 0, "a clear opens the window");
    w.OnClear(false);
    Check(w.GateForDraw() == gb::Window::Gate::kOpen && w.clears() == 2 && w.partial_clears() == 1, "the second clear (after the collector) keeps it open, a partial one is counted");
    w.OnTrigger();
    Check(w.GateForDraw() == gb::Window::Gate::kClosed && w.triggered(), "the trigger closes it");
    w.OnClear(true);
    Check(w.GateForDraw() == gb::Window::Gate::kClosed, "a clear after the trigger does not open it again");
    w.BeginFrame();
    Check(w.GateForDraw() == gb::Window::Gate::kNeedsClear && w.clears() == 0 && !w.triggered(), "the next frame starts over");
    w.OnInitialClear();
    Check(w.GateForDraw() == gb::Window::Gate::kOpen && w.had_clear(), "an initial clear opens it too");
  }
  // the trigger
  {
    gb::TriggerFacts t;
    t.depth_copy = true;
    t.main_depth_samples = 2;
    t.dest_address = gb::kTriggerDestination;
    Check(gb::IsTrigger(t), "the mid-scene depth copy of the play view is the trigger");
    gb::TriggerFacts u = t;
    u.main_depth_samples = 1;
    Check(gb::IsTrigger(u), "and without MSAA");
    u.main_depth_samples = 4;
    Check(!gb::IsTrigger(u), "the pause world's copy is not");
    u = t;
    u.dest_address = 0x06ACD000;
    Check(!gb::IsTrigger(u), "the end-of-scene copy is not");
    u = t;
    u.first_this_frame = false;
    Check(!gb::IsTrigger(u), "the second copy is not");
    u = t;
    u.clears_depth = true;
    Check(!gb::IsTrigger(u), "a copy that clears the depth is not");
    u = t;
    u.depth_copy = false;
    Check(!gb::IsTrigger(u), "a resolve that is not a depth copy is not");
    u = t;
    u.inside_end_tiling = true;
    Check(!gb::IsTrigger(u), "a copy inside the end of the tiling is not");
    Check(gb::kTriggerDestination == 0x078B0000u, "the trigger's destination is the AO's");
  }
  // the setting
  {
    Check(gb::SettingOfWord("off") == gb::Setting::kOff && gb::SettingOfWord("NORMALS") == gb::Setting::kNormals && gb::SettingOfWord("Full") == gb::Setting::kFull &&
              gb::SettingOfWord("auto") == gb::Setting::kAuto && gb::SettingOfWord("what") == gb::Setting::kAuto && gb::SettingOfWord("") == gb::Setting::kAuto,
          "the setting's words (a word that is none is auto)");
    Check(gb::PlanesFor(gb::Setting::kOff, 7) == 0 && gb::PlanesFor(gb::Setting::kNormals, 0) == 1 && gb::PlanesFor(gb::Setting::kFull, 0) == 7 &&
              gb::PlanesFor(gb::Setting::kAuto, 0) == 0 && gb::PlanesFor(gb::Setting::kAuto, 5) == 5 && gb::PlanesFor(gb::Setting::kAuto, 0xFF) == 7,
          "the planes each setting makes (auto is what the consumers ask for)");
    Check(gb::ViewOf(0) == gb::View::kOff && gb::ViewOf(1) == gb::View::kNormal && gb::ViewOf(6) == gb::View::kLitWeight && gb::ViewOf(7) == gb::View::kOff &&
              gb::ViewOf(-1) == gb::View::kOff,
          "the debug view's number");
  }
}

// ---------------------------------------------------------------------------
// the .hlsli against the twin
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void HlslDefinesChecks(const std::string& text) {
  std::map<std::string, long> defines;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("#define GB_", 0) != 0) continue;
    std::istringstream one(line);
    std::string define, name, value;
    one >> define >> name >> value;
    defines[name] = std::strtol(value.c_str(), nullptr, 0);
  }
  const std::map<std::string, long> want = {
      {"GB_CLASS_NONE", long(gb::SurfaceClass::kNone)},         {"GB_CLASS_WORLD", long(gb::SurfaceClass::kWorld)},
      {"GB_CLASS_ROAD", long(gb::SurfaceClass::kRoad)},         {"GB_CLASS_WET_ROAD", long(gb::SurfaceClass::kWetRoad)},
      {"GB_CLASS_SIDEWALK", long(gb::SurfaceClass::kSidewalk)}, {"GB_CLASS_TERRAIN", long(gb::SurfaceClass::kTerrain)},
      {"GB_CLASS_LOD", long(gb::SurfaceClass::kLod)},           {"GB_CLASS_FOLIAGE", long(gb::SurfaceClass::kFoliage)},
      {"GB_CLASS_PAINT", long(gb::SurfaceClass::kPaint)},       {"GB_CLASS_CHROME", long(gb::SurfaceClass::kChrome)},
      {"GB_CLASS_METAL", long(gb::SurfaceClass::kMetal)},       {"GB_CLASS_GLASS", long(gb::SurfaceClass::kGlass)},
      {"GB_CLASS_SKIN", long(gb::SurfaceClass::kSkin)},         {"GB_CLASS_EMISSIVE", long(gb::SurfaceClass::kEmissive)},
      {"GB_CLASS_INTERIOR", long(gb::SurfaceClass::kInterior)}, {"GB_CLASS_WATER", long(gb::SurfaceClass::kWater)},
      {"GB_CLASS_MASK", long(gb::kClassMask)},                  {"GB_FLAG_SSR", long(gb::kMaterialFlagSsr)},
      {"GB_FLAG_ALPHA_COVERED", long(gb::kMaterialFlagAlphaCovered)}, {"GB_FLAG_VEHICLE", long(gb::kMaterialFlagVehicle)},
      {"GB_MATERIAL_CLASS_SHIFT", long(gb::kMaterialClassShift)}, {"GB_MATERIAL_FLAGS_SHIFT", long(gb::kMaterialFlagsShift)},
      {"GB_MATERIAL_LIT_SHIFT", long(gb::kMaterialLitShift)},   {"GB_MODE_WRITE_NORMAL", long(gb::kModeWriteNormal)},
      {"GB_MODE_WRITE_ALBEDO", long(gb::kModeWriteAlbedo)},     {"GB_MODE_WRITE_MATERIAL", long(gb::kModeWriteMaterial)},
      {"GB_MODE_FLIP_BACKFACE", long(gb::kModeFlipBackface)},   {"GB_MODE_DERIVATIVE_NORMAL", long(gb::kModeDerivativeNormal)},
  };
  Check(defines.size() == want.size(), Num("the .hlsli has %.0f GB_ defines (the test knows %.0f)", double(defines.size()), double(want.size())));
  for (const auto& [name, value] : want) {
    const auto it = defines.find(name);
    Check(it != defines.end() && it->second == value, name + " is the C++ value");
  }
}

float MaxAbs(float a, float b) { return std::fabs(a - b); }

void HlslAgainstTwinChecks() {
  using namespace hlsl_shim;
  namespace g = mcla::gpu::gbuffer;
  Lcg rng;
  double worst_oct = 0.0, worst_dec = 0.0, worst_rt1 = 0.0, worst_world = 0.0, worst_face = 0.0, worst_quat = 0.0;
  int word_mismatch = 0, rt3_mismatch = 0, valid_mismatch = 0, albedo_mismatch = 0;
  for (int i = 0; i < 200000; ++i) {
    const g::Vec3 n = rng.Direction();
    const float2 e = GbOctEncode(float3(n.x, n.y, n.z));
    const g::Vec2 te = g::OctEncode(n);
    worst_oct = std::max(worst_oct, double(std::max(MaxAbs(e.x, te.x), MaxAbs(e.y, te.y))));
    const float3 d = GbOctDecode(float2(te.x, te.y));
    const g::Vec3 td = g::OctDecode(te);
    worst_dec = std::max(worst_dec, double(std::max(std::max(MaxAbs(d.x, td.x), MaxAbs(d.y, td.y)), MaxAbs(d.z, td.z))));
    const float2 r = GbRt1Encode(float3(n.x, n.y, n.z));
    const g::Vec2 tr = g::Rt1Encode(n);
    worst_rt1 = std::max(worst_rt1, double(std::max(MaxAbs(r.x, tr.x), MaxAbs(r.y, tr.y))));
    valid_mismatch += (GbRt1Valid(r) != g::Rt1Valid(g::Rt1Pack(n))) ? 1 : 0;
  }
  Check(worst_oct < 1e-5, Num("GbOctEncode equals the twin to %.2e", worst_oct));
  Check(worst_dec < 1e-5, Num("GbOctDecode equals the twin to %.2e", worst_dec));
  Check(worst_rt1 < 1e-5, Num("GbRt1Encode equals the twin to %.2e", worst_rt1));
  Check(valid_mismatch == 0, "GbRt1Valid agrees with the twin on a packed normal");
  {
    // the hard directions, in the .hlsli too
    const float2 r = GbRt1Encode(float3(-1e-5f, -1e-5f, -1.0f));
    Check(r.x == 1.0f / 65535.0f && r.y < 0.5f / 65535.0f, "GbRt1Encode moves the direction that would round to (0, 0)");
    const float2 z = GbRt1Encode(float3(0, 0, 0));
    Check(z.x == 0.0f && z.y == 0.0f && !GbRt1Valid(z), "GbRt1Encode of the zero vector is no data");
    Check(!GbRt1Valid(float2(0, 0)) && GbRt1Valid(float2(1.0f / 65535.0f, 0)), "GbRt1Valid");
  }
  for (int i = 0; i < 100000; ++i) {
    const float a = rng.Next();
    const float3 enc = GbAlbedoEncode(float3(a, a * 0.5f, 1.2f * a));
    if (MaxAbs(enc.x, g::AlbedoEncode(a)) > 1e-6f || MaxAbs(enc.y, g::AlbedoEncode(a * 0.5f)) > 1e-6f || MaxAbs(enc.z, g::AlbedoEncode(1.2f * a)) > 1e-6f) ++albedo_mismatch;
    const float3 dec = GbAlbedoDecode(float3(a, a, a));
    if (MaxAbs(dec.x, g::AlbedoDecode(a)) > 1e-6f) ++albedo_mismatch;
    const float s = rng.Next();
    const float lit = rng.Next();
    const uint32_t cls = uint32_t(rng.Range(0.0f, 31.99f));
    const uint32_t flags = uint32_t(rng.Range(0.0f, 7.99f));
    const uint32_t hw = GbMaterialWord(s, cls, flags, lit);
    const uint32_t tw = g::MaterialWord(s, cls, flags, lit);
    if (hw != tw) ++word_mismatch;
    const float2 rt3 = GbMaterialRt3(tw);
    if (MaxAbs(rt3.x, float(g::MaterialRt3Smoothness(tw)) / 255.0f) > 1e-6f || MaxAbs(rt3.y, float(g::MaterialRt3ClassAndFlags(tw)) / 255.0f) > 1e-6f ||
        GbRt3Class(rt3) != cls || GbRt3Flags(rt3) != flags || MaxAbs(GbMaterialLit(tw), float(g::MaterialLitByte(tw)) / 255.0f) > 1e-6f) {
      ++rt3_mismatch;
    }
  }
  Check(albedo_mismatch == 0, "GbAlbedoEncode and GbAlbedoDecode equal the twin");
  Check(word_mismatch == 0, "GbMaterialWord equals the twin");
  Check(rt3_mismatch == 0, "GbMaterialRt3, GbMaterialLit and the RT3 readers equal the twin and decode what was packed");
  for (int i = 0; i < 20000; ++i) {
    const g::Quat q = RandomQuat(rng);
    const g::Vec3 v = rng.Direction() * rng.Range(0.1f, 50.0f);
    const float3 hr = GbQuatRotate(float4(q.x, q.y, q.z, q.w), float3(v.x, v.y, v.z));
    const g::Vec3 tr = g::QuatRotate(q, v);
    worst_quat = std::max(worst_quat, double(std::max(std::max(MaxAbs(hr.x, tr.x), MaxAbs(hr.y, tr.y)), MaxAbs(hr.z, tr.z))));
    float ray[4];
    g::MakeViewRay(rng.Range(0.4f, 1.4f), rng.Range(0.2f, 0.9f), 1920.0f, 1080.0f, ray);
    const float px = rng.Range(0.0f, 1920.0f), py = rng.Range(0.0f, 1080.0f), w = rng.Range(0.002f, 0.5f);
    const float3 hw = GbWorldPoint(float4(ray[0], ray[1], ray[2], ray[3]), float4(q.x, q.y, q.z, q.w), px, py, w);
    const g::Vec3 tw = g::WorldPoint(ray, q, px, py, w);
    worst_world = std::max(worst_world, double(std::max(std::max(MaxAbs(hw.x, tw.x), MaxAbs(hw.y, tw.y)), MaxAbs(hw.z, tw.z)) / std::max(1.0f, g::Length(tw))));
    const g::Vec3 a = rng.Direction(), b = rng.Direction(), c = rng.Direction();
    const float3 hf = GbFaceNormal(float3(a.x, a.y, a.z), float3(b.x, b.y, b.z), float3(c.x, c.y, c.z));
    const g::Vec3 tf = g::FaceNormal(a, b, c);
    worst_face = std::max(worst_face, double(std::max(std::max(MaxAbs(hf.x, tf.x), MaxAbs(hf.y, tf.y)), MaxAbs(hf.z, tf.z))));
  }
  Check(worst_quat < 1e-3, Num("GbQuatRotate equals the twin to %.2e", worst_quat));
  Check(worst_world < 1e-4, Num("GbWorldPoint equals the twin to %.2e (relative)", worst_world));
  Check(worst_face < 1e-4, Num("GbFaceNormal equals the twin to %.2e", worst_face));
}

}  // namespace

int main(int argc, char** argv) {
  std::string hlsli = argc > 1 ? argv[1] : std::string();
  if (hlsli.empty()) {
    std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    hlsli = (slash == std::string::npos ? std::string() : here.substr(0, slash + 1)) + "hlsl/gbuffer_common.hlsli";
  }
  OctChecks();
  AlbedoAndMaterialChecks();
  DerivativeChecks();
  LayoutChecks();
  LightSetChecks();
  RuleChecks();
  const std::string text = ReadFile(hlsli);
  Check(!text.empty(), "the .hlsli is readable: " + hlsli);
  if (!text.empty()) HlslDefinesChecks(text);
  HlslAgainstTwinChecks();
  std::printf("gbuffer_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

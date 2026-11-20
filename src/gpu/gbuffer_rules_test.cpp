// Check of the G-buffer's interface (gbuffer_types.h, gbuffer_rules.h, hlsl/gbuffer_common.hlsli, light_set.h) and of the host's pure rules. A program of
// its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_gbuffer_rules_test [<hlsl/gbuffer_common.hlsli>]
//
// It checks: the octahedral code (the 16 bit error, "no data" kept free, the fold), the albedo and the material word, the derivative normal's arithmetic,
// the constants block's layout, the light set's conversion, the draw decision (what each kind of draw does to the planes, under each backend capability), the planes
// and the frame's plan, the consumers' requests, MSAA and the memory, the views that never write, the window, the trigger and the setting's words; then it compiles the
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
// The G-buffer's feature is bit 10 of the variant word's feature set (shaderpack/pack_format.h kFeatureGBuffer, variant_chain.h Word): its solo word is 1 << 11, the alpha
// test twin that + 1; the headlight's (feature 9) is 1 << 10.
constexpr uint32_t kGBufferWord = 1u << 11;
constexpr uint32_t kGBufferTwinWord = (1u << 11) | 1u;
constexpr uint32_t kHeadlightWord = 1u << 10;
constexpr uint32_t kHeadlightGBufferWord = (1u << 10) | (1u << 11);
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
  f.variant_word = kGBufferWord;
  return f;
}

void RuleChecks() {
  using gb::Why;
  using gb::PlaneMode;
  {
    const gb::DrawDecision d = gb::Decide(GoodDraw());
    Check(d.why == Why::kWrites && d.variant == kGBufferWord && d.attach() && d.writes() && d.mode == PlaneMode::kWrite, "an opaque lit draw of the main view takes the variant");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.ps_variant_before = 1;
    f.variant_word = kGBufferTwinWord;
    const gb::DrawDecision d = gb::Decide(f);
    Check(d.why == Why::kWrites && d.variant == kGBufferTwinWord, "an alpha tested draw takes the alpha test twin");
  }
  {
    // On a shader that has the headlight's stage too, a draw that has the headlight takes the composed word (Scene::GBufferKey found it with ChooseVariant).
    gb::DrawFacts f = GoodDraw();
    f.ps_variant_before = kHeadlightWord;
    f.variant_word = kHeadlightGBufferWord;
    const gb::DrawDecision d = gb::Decide(f);
    Check(d.why == Why::kWrites && d.variant == kHeadlightGBufferWord && d.attach(), "a draw with the headlight takes the headlight and G-buffer word");
    f.variant_word = 0;
    gb::DrawDecision n = gb::Decide(f);
    Check(n.why == Why::kMaskedOtherVariant && !n.attach() && n.variant == 0,
          "a draw with the headlight on a shader that has no composed word writes nothing: the headlight stays, the G-buffer does not (writers only: not carried)");
    f.masked_attach = true;
    n = gb::Decide(f);
    Check(n.why == Why::kMaskedOtherVariant && n.mode == PlaneMode::kClear && n.variant == 0, "... and with the non writers carried it clears what it covers");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.variant_word = 0;
    f.ps_variant_before = 1;
    Check(gb::Decide(f).why == Why::kMaskedNoVariant && !gb::Decide(f).attach() && gb::Decide(f).variant == 0, "an alpha draw whose shader has no twin writes nothing (not carried while the non writers do not bind the sidecar)");
    f.masked_attach = true;
    Check(gb::Decide(f).why == Why::kMaskedNoVariant && gb::Decide(f).mode == PlaneMode::kClear, "... and clears when they do");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.variant_word = 0;
    Check(gb::Decide(f).why == Why::kMaskedNoVariant && !gb::Decide(f).attach(), "a shader with no variant writes nothing, writers only: not carried");
    f.masked_attach = true;
    const gb::DrawDecision d = gb::Decide(f);
    Check(d.why == Why::kMaskedNoVariant && d.attach() && !d.writes() && d.mode == PlaneMode::kClear,
          "an opaque shader with no variant CLEARS the planes where it draws (no stale normal of a surface behind it)");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.rt0_blended = true;
    Check(gb::Decide(f).why == Why::kMaskedBlended && !gb::Decide(f).attach(), "a blended draw writes nothing");
    f.masked_attach = true;
    Check(gb::Decide(f).why == Why::kMaskedBlended && gb::Decide(f).mode == PlaneMode::kMask, "a blended draw leaves the planes as they are (write mask 0) when the non writers carry the sidecar");
    f.variant_word = 0;
    Check(gb::Decide(f).mode == PlaneMode::kMask, "blended wins over no variant: it does not define the surface");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.rt0_writes = false;
    Check(gb::Decide(f).why == Why::kMaskedNoColor && !gb::Decide(f).attach(), "a draw that writes no colour writes nothing");
    f.masked_attach = true;
    Check(gb::Decide(f).mode == PlaneMode::kMask, "a draw that writes no colour leaves the planes as they are");
  }
  {
    gb::DrawFacts f = GoodDraw();
    f.ps_variant_before = 7;
    f.variant_word = 0;
    Check(gb::Decide(f).why == Why::kMaskedOtherVariant && !gb::Decide(f).attach(), "a draw with another variant writes nothing");
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
    Check(gb::Decide(f).why == Why::kPauseView && !gb::Decide(f).attach(), "the pause world (4 samples): nothing");
    f = GoodDraw();
    f.main_samples = 1;
    Check(gb::Decide(f).why == Why::kWrites, "the play view without MSAA (1 sample) writes");
    f = GoodDraw();
    f.guest_color_count = 2;
    Check(gb::Decide(f).why == Why::kNotOneColor, "two guest colour targets: nothing");
    f = GoodDraw();
    f.window_open = false;
    Check(gb::Decide(f).why == Why::kWindowClosed && !gb::Decide(f).attach(), "a closed window: nothing");
    f.masked_attach = true;
    Check(gb::Decide(f).why == Why::kWindowClosed && !gb::Decide(f).attach(), "a closed window: nothing, whatever the policy (the consumers read the planes after the trigger)");
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
    Check(gb::ViewOf(0) == gb::View::kOff && gb::ViewOf(1) == gb::View::kNormal && gb::ViewOf(6) == gb::View::kLitWeight && gb::ViewOf(7) == gb::View::kMissing &&
              gb::ViewOf(8) == gb::View::kOff && gb::ViewOf(-1) == gb::View::kOff,
          "the debug view's number (7 is the missing pixels view, above it off)");
    Check(int(gb::View::kCount) == 8, "eight views: off and seven");
  }
}

// ---------------------------------------------------------------------------
// the planes, the frame's plan, the consumers' requests
// ---------------------------------------------------------------------------
void PlaneChecks() {
  Check(gb::IsPlanePrefix(1) && gb::IsPlanePrefix(3) && gb::IsPlanePrefix(7) && !gb::IsPlanePrefix(0) && !gb::IsPlanePrefix(2) && !gb::IsPlanePrefix(5) && !gb::IsPlanePrefix(15),
        "a stage's planes are a prefix: 1, 3 or 7 (the outputs' numbers are the planes')");
  Check(gb::kStagePlanes == gb::PlaneBit(gb::kPlaneNormal), "this phase's stage writes the normal plane alone (RT2 and RT3 are placeholders)");
  Check(gb::ColourSlotOfPlane(gb::kPlaneNormal) == 1 && gb::ColourSlotOfPlane(gb::kPlaneAlbedo) == 2 && gb::ColourSlotOfPlane(gb::kPlaneMaterial) == 3 && gb::kMaxColourSlots == 4,
        "the planes are the key's colour slots 1, 2 and 3 after the guest's colour: four slots in all (draw_state.h kColorTargetCount)");
  Check(gb::PlaneCount(0) == 0 && gb::PlaneCount(1) == 1 && gb::PlaneCount(7) == 3 && gb::PlaneCount(0xFF) == 3, "the plane count");
  // what a frame's request becomes, for the stage as it is and for a stage with all three planes
  Check(gb::WrittenPlanes(7) == 1 && gb::AttachedPlanes(7) == 1, "a full request of a stage that writes the normal: the normal is written and attached");
  Check(gb::WrittenPlanes(gb::PlaneBit(gb::kPlaneAlbedo)) == 0 && gb::AttachedPlanes(gb::PlaneBit(gb::kPlaneAlbedo)) == 0,
        "a request for a plane no stage writes makes no sidecar (nothing would hold data)");
  Check(gb::WrittenPlanes(0) == 0 && gb::AttachedPlanes(0) == 0, "no request: no sidecar");
  Check(gb::WrittenPlanes(1, 7) == 1 && gb::AttachedPlanes(1, 7) == 7, "a stage with all three planes declares three outputs: a request for the normal still attaches all three (the shader writes them)");
  Check(gb::WrittenPlanes(6, 7) == 6 && gb::AttachedPlanes(6, 7) == 7 && gb::AttachedPlanes(2, 3) == 3, "the attached set is the stage's whole prefix whatever is written");
  // channels: a draw whose RT0 write mask is the sidecar's too (one blend state) has to write all the channels the planes have
  Check(gb::ChannelMaskOf(gb::PlaneFormat::kRg16Unorm) == 3 && gb::ChannelMaskOf(gb::PlaneFormat::kRgba8Unorm) == 0xF && gb::ChannelMaskOf(gb::PlaneFormat::kRg8Unorm) == 3,
        "the formats' channels");
  Check(gb::ChannelsNeeded(1) == 3 && gb::ChannelsNeeded(3) == 0xF && gb::ChannelsNeeded(7) == 0xF && gb::ChannelsNeeded(4) == 3 && gb::ChannelsNeeded(0) == 0,
        "the channels a set of planes needs written (the albedo plane has all four)");
  // the frame's plan from the setting and the consumers
  {
    gb::Wants wants;
    Check(wants.Union() == 0, "no consumer wants anything at the start");
    gb::FramePlan plan = gb::PlanFrame(gb::Setting::kAuto, wants.Union());
    Check(plan.requested == 0 && plan.written == 0 && plan.attached == 0, "auto with no consumer: no sidecar at all (made only when a consumer asks)");
    wants.Set(gb::Consumer::kAmbientOcclusion, gb::PlaneBit(gb::kPlaneNormal));
    plan = gb::PlanFrame(gb::Setting::kAuto, wants.Union());
    Check(plan.requested == 1 && plan.written == 1 && plan.attached == 1, "the ambient occlusion asks for the normal: auto makes it");
    wants.Set(gb::Consumer::kLocalLights, 0xFF);
    Check(wants.Of(gb::Consumer::kLocalLights) == 7 && wants.Union() == 7, "a want is cut to the three planes; the union of the wants");
    plan = gb::PlanFrame(gb::Setting::kAuto, wants.Union());
    Check(plan.requested == 7 && plan.written == 1 && plan.attached == 1, "the lights ask for all three: only the normal is written while the stage writes the normal");
    plan = gb::PlanFrame(gb::Setting::kAuto, wants.Union(), 7);
    Check(plan.requested == 7 && plan.written == 7 && plan.attached == 7, "... and all three with a stage that writes them");
    wants.Set(gb::Consumer::kLocalLights, 0);
    wants.Set(gb::Consumer::kAmbientOcclusion, 0);
    Check(wants.Union() == 0 && gb::PlanFrame(gb::Setting::kAuto, wants.Union()).attached == 0, "a consumer that turns off withdraws its want: the sidecar goes");
    wants.Set(gb::Consumer::kCount, 7);
    Check(wants.Union() == 0 && wants.Of(gb::Consumer::kCount) == 0, "a want for a consumer the table does not have is ignored");
    wants.Set(gb::Consumer::kAmbientOcclusion, 1);
    plan = gb::PlanFrame(gb::Setting::kOff, wants.Union());
    Check(plan.attached == 0, "off: no sidecar whatever the consumers want");
    wants.Set(gb::Consumer::kAmbientOcclusion, 0);
    plan = gb::PlanFrame(gb::Setting::kNormals, wants.Union());
    Check(plan.written == 1 && plan.attached == 1, "normals: made with no consumer (the debug switch)");
    plan = gb::PlanFrame(gb::Setting::kFull, wants.Union());
    Check(plan.requested == 7 && plan.written == 1, "full: asks for all three, the stage writes what it writes");
    Check(std::string(gb::ConsumerName(gb::Consumer::kAmbientOcclusion)) == "ambient occlusion" && std::string(gb::ConsumerName(gb::Consumer::kCount)) == "?", "the consumers' names");
  }
}

// ---------------------------------------------------------------------------
// MSAA and memory
// ---------------------------------------------------------------------------
void MsaaChecks() {
  Check(gb::SidecarSamples(0) == 1 && gb::SidecarSamples(1) == 1 && gb::SidecarSamples(2) == 2 && gb::SidecarSamples(3) == 2 && gb::SidecarSamples(4) == 4 && gb::SidecarSamples(7) == 4 &&
            gb::SidecarSamples(8) == 8 && gb::SidecarSamples(16) == 8,
        "the sidecar's samples are the target's: 1, 2, 4 or 8");
  // the survey's table (3.6) at 4K: normal only 33.2 / 66.4 / 132.7 MB, all three planes 82.9 / 165.9 / 331.8 MB (MB = 10^6 here as there)
  constexpr uint32_t k4kW = 3840, k4kH = 2160;
  Check(gb::SidecarBytes(1, k4kW, k4kH, 1) == 33177600ull && gb::SidecarBytes(1, k4kW, k4kH, 2) == 66355200ull && gb::SidecarBytes(1, k4kW, k4kH, 4) == 132710400ull,
        "the normal plane at 4K takes 33 / 66 / 133 MB at 1x / 2x / 4x");
  Check(gb::SidecarBytes(7, k4kW, k4kH, 1) == 82944000ull && gb::SidecarBytes(7, k4kW, k4kH, 4) == 331776000ull, "all three planes at 4K take 83 MB at 1x and 332 MB at 4x");
  Check(gb::SidecarBytes(7, 1920, 1080, 1) == 20736000ull && gb::SidecarBytes(1, 1920, 1080, 2) == 16588800ull, "1080p: 20.7 MB for all three, 16.6 MB for the normal at 2x");
  Check(gb::PlaneBytes(gb::kPlaneMaterial, 1000, 1000, 2) == 4000000ull && gb::PlaneBytes(gb::kPlaneAlbedo, 1000, 1000, 1) == 4000000ull && gb::PlaneBytes(gb::kPlaneNormal, 1000, 1000, 1) == 4000000ull,
        "a plane's bytes: the albedo 4 a sample, the material 2, the normal 4");
  Check(gb::SidecarBytes(0, k4kW, k4kH, 8) == 0, "no planes, no bytes");
  Check(gb::SidecarAffordable(1, k4kW, k4kH, 8) && gb::SidecarAffordable(7, k4kW, k4kH, 4) && !gb::SidecarAffordable(7, k4kW, k4kH, 8) && gb::SidecarAffordable(7, 1920, 1080, 8),
        "the cap (512 MiB): all planes at 8x 4K are too much (663 MB), the normal alone at 8x 4K (266 MB) and all three at 8x 1080p (166 MB) are fine");
  // the pause world's 4x would be 133 MB (normal) / 332 MB (all) at 4K: not made, whatever the cap says
  gb::DrawFacts f = GoodDraw();
  f.main_samples = 4;
  Check(gb::Decide(f).why == gb::Why::kPauseView && !gb::Decide(f).attach(), "the 4x pause world never carries the sidecar");
  f.masked_attach = true;
  Check(!gb::Decide(f).attach(), "... not even with the non writers carried");
  // the 2x and 1x main views are the play view at any host level: Decide only looks at the guest's samples
  for (uint32_t guest : {1u, 2u}) {
    f = GoodDraw();
    f.main_samples = guest;
    Check(gb::Decide(f).why == gb::Why::kWrites, "the play view at the guest's 1 or 2 samples writes (the host's samples are the sidecar's own business)");
  }
}

// ---------------------------------------------------------------------------
// the backend's blend capability, the attach policy, what each draw does to the planes
// ---------------------------------------------------------------------------
void AttachChecks() {
  using gb::PlaneMode;
  using gb::Why;
  Check(gb::CapsFor(true).per_target_blend, "Vulkan has a blend state and write mask per colour target");
  Check(gb::CapsFor(false).per_target_blend == gb::kD3d12PerTargetBlend && !gb::kD3d12PerTargetBlend,
        "Plume's D3D12 back end has one until its pin has 5d60b70: flip kD3d12PerTargetBlend then (and this check)");
  Check(gb::AttachOfWord("writers") == gb::AttachSetting::kWriters && gb::AttachOfWord("ALL") == gb::AttachSetting::kAll && gb::AttachOfWord("auto") == gb::AttachSetting::kAuto &&
            gb::AttachOfWord("") == gb::AttachSetting::kAuto && gb::AttachOfWord("x") == gb::AttachSetting::kAuto,
        "the attach setting's words (a word that is none is auto)");
  Check(gb::MaskedDrawsAttach(gb::AttachSetting::kAuto, gb::CapsFor(true)) && !gb::MaskedDrawsAttach(gb::AttachSetting::kAuto, gb::CapsFor(false)),
        "auto: the non writers bind the sidecar where each target has its own blend state (Vulkan), not on D3D12 without the Plume fix");
  Check(!gb::MaskedDrawsAttach(gb::AttachSetting::kWriters, gb::CapsFor(true)) && gb::MaskedDrawsAttach(gb::AttachSetting::kAll, gb::CapsFor(false)),
        "writers: never; all: always (the build is trusted to have the Plume fix)");
  // the blend of each mode
  {
    const gb::PlaneBlend w = gb::BlendFor(PlaneMode::kWrite);
    const gb::PlaneBlend c = gb::BlendFor(PlaneMode::kClear);
    const gb::PlaneBlend m = gb::BlendFor(PlaneMode::kMask);
    Check(!w.zero && w.write_mask == 0xF, "a writer: no blend, every channel");
    Check(c.zero && c.write_mask == 0xF, "a clear: blend ZERO / ZERO, every channel");
    Check(!m.zero && m.write_mask == 0, "a mask: write mask 0");
  }
  // the backends with one blend state: the writer's RT0 mask is the sidecar's
  {
    gb::DrawFacts f = GoodDraw();
    f.per_target_blend = false;
    f.rt0_write_mask = 0xF;
    Check(gb::Decide(f).why == Why::kWrites, "one blend state, a full RT0 write mask: the normal plane is written");
    f.rt0_write_mask = 0x3;
    Check(gb::Decide(f).why == Why::kWrites, "... red and green are enough for the normal");
    f.rt0_write_mask = 0x7;
    Check(gb::Decide(f).why == Why::kWrites, "... red, green and blue too");
    f.rt0_write_mask = 0x8;
    Check(gb::Decide(f).why == Why::kMaskedNarrowMask && !gb::Decide(f).attach() && gb::Decide(f).variant == 0, "an alpha only RT0 mask cannot write the normal: not carried, the game's shader draws");
    f.rt0_write_mask = 0x1;
    Check(gb::Decide(f).why == Why::kMaskedNarrowMask, "a red only one cannot either");
    f.planes = 7;
    f.rt0_write_mask = 0x7;
    Check(gb::Decide(f).why == Why::kMaskedNarrowMask, "with the albedo plane (RGBA) RT0's mask has to be all four channels");
    f.rt0_write_mask = 0xF;
    Check(gb::Decide(f).why == Why::kWrites, "... and a full one is");
    f.per_target_blend = true;
    f.rt0_write_mask = 0x8;
    Check(gb::Decide(f).why == Why::kWrites && gb::Decide(f).attach(), "with a blend state per target the writer's own mask is used, RT0's does not matter");
    f.masked_attach = true;
    Check(gb::Decide(f).mode == PlaneMode::kWrite, "and the policy does not change a writer");
  }
  // the draws that are in the main identity and are not the play view
  {
    gb::DrawFacts f = GoodDraw();
    f.per_target_blend = true;
    f.masked_attach = true;
    f.sub_view = true;
    Check(gb::Decide(f).why == Why::kMaskedSubView && gb::Decide(f).mode == PlaneMode::kClear && gb::Decide(f).variant == 0,
          "a draw into a window of the target (the rear view mirror) never writes the planes: its pixels are not the view's, it clears what it covers");
    f.masked_attach = false;
    Check(gb::Decide(f).why == Why::kMaskedSubView && !gb::Decide(f).attach(), "(writers only: not carried: the one inexactness, a window of 229 x 44 pixels keeps the main view's data)");
    f = GoodDraw();
    f.masked_attach = true;
    f.depth_is_main = false;
    Check(gb::Decide(f).why == Why::kMaskedOtherDepth && gb::Decide(f).mode == PlaneMode::kClear, "the main colour with another depth is not the view: never writes, clears");
    f = GoodDraw();
    f.masked_attach = true;
    f.has_depth = false;
    Check(gb::Decide(f).why == Why::kMaskedNoDepth && gb::Decide(f).mode == PlaneMode::kClear, "a colour draw with no depth clears what it covers");
    f.rt0_blended = true;
    Check(gb::Decide(f).mode == PlaneMode::kMask, "... unless it blends");
    f = GoodDraw();
    f.suppressed = true;
    Check(gb::Decide(f).why == Why::kSuppressed && !gb::Decide(f).attach(), "a cutscene's frame never writes");
    f.masked_attach = true;
    Check(!gb::Decide(f).attach(), "not even the masks: the frame has no sidecar");
    f = GoodDraw();
    f.active = false;
    f.suppressed = true;
    Check(gb::Decide(f).why == Why::kOff, "off is off before anything else");
  }
  // the order of the reasons: the identity first, then what the draw does with its colour
  {
    gb::DrawFacts f = GoodDraw();
    f.masked_attach = true;
    f.main_samples = 4;
    f.rt0_blended = true;
    Check(gb::Decide(f).why == Why::kPauseView, "the pause world is told before a blended draw is");
    f = GoodDraw();
    f.masked_attach = true;
    f.rt0_blended = true;
    f.rt0_writes = false;
    Check(gb::Decide(f).why == Why::kMaskedNoColor, "a draw with no colour is told before a blended one");
    f = GoodDraw();
    f.masked_attach = true;
    f.sub_view = true;
    f.variant_word = 0;
    Check(gb::Decide(f).why == Why::kMaskedSubView, "the mirror is told before a missing variant");
  }
  Check(gb::IsMasked(Why::kMaskedBlended) && gb::IsMasked(Why::kMaskedNarrowMask) && gb::IsMasked(Why::kMaskedSubView) && !gb::IsMasked(Why::kWrites) && !gb::IsMasked(Why::kOff) &&
            !gb::IsMasked(Why::kPauseView) && !gb::IsMasked(Why::kWindowClosed),
        "the log's masked counts are the draws in the window that did not write");
}

// ---------------------------------------------------------------------------
// the views that never write: the mirror's window, the cutscene gate
// ---------------------------------------------------------------------------
void ViewMaskChecks() {
  Check(gb::IsSubViewport(229, 1280) && gb::IsSubViewport(639, 1280) && !gb::IsSubViewport(640, 1280) && !gb::IsSubViewport(1280, 1280) && !gb::IsSubViewport(1000, 1280) &&
            !gb::IsSubViewport(1, 0),
        "a viewport narrower than half the pitch is a window (the mirror's 229 of 1280), a whole view is not");
  Check(gb::IsSubViewport(458, 2560) && !gb::IsSubViewport(1280, 2560) && gb::IsSubViewport(1279, 2560), "at 2x the render scale the same shares hold (host pixels on both sides)");
  gb::CutsceneGate gate;
  Check(!gate.active() && !gate.Update(0) && !gate.Update(0), "no cutscene until the hook is called");
  Check(gate.Update(1) && gate.active(), "the cutscene camera ran: the frame is a cutscene's");
  Check(gate.Update(2) && gate.Update(3), "it runs every frame of the cutscene");
  Check(gate.Update(3) && gate.active(), "one frame without a call is still the cutscene (the hook runs on the guest thread, a frame early or late)");
  Check(!gate.Update(3) && !gate.active(), "the second quiet frame ends it");
  Check(!gate.Update(3), "and it stays ended");
  Check(gate.Update(9), "a later cutscene starts it again");
}

// ---------------------------------------------------------------------------
// the pause world is the GUEST's 4 samples, never the host's MSAA level; the frame's kind; the camera kept from the last frame's anchor (K5, K9 of the phase-1 run)
// ---------------------------------------------------------------------------
gb::CameraAxes MakeAxes(float yaw_degrees) {
  gb::CameraAxes axes;
  const float yaw = yaw_degrees * 3.14159265f / 180.0f;
  axes.right = gb::Vec3{std::cos(yaw), 0.0f, -std::sin(yaw)};
  axes.up = gb::Vec3{0.0f, 1.0f, 0.0f};
  axes.forward = gb::Vec3{std::sin(yaw), 0.0f, std::cos(yaw)};
  axes.tan_half_x = 0.7f;
  axes.tan_half_y = 0.4f;
  axes.width = 1280;
  axes.height = 720;
  return axes;
}
void PauseAndCameraChecks() {
  using gb::Why;
  // K9: the pause world is told by the guest's samples. The host's MSAA level (mcla_gpu_msaa 1, 2, 4, 8) is not an input of the decision: the play view at 2 guest samples is the
  // play view at every host level, and a sidecar at 4 host samples (what the dx-day-m4 run had) belongs to it.
  Check(gb::kPauseGuestSamples == 4 && gb::IsPauseWorld(4) && !gb::IsPauseWorld(1) && !gb::IsPauseWorld(2) && !gb::IsPauseWorld(8) && !gb::IsPauseWorld(0), "only the guest's 4 samples are the pause world");
  for (uint32_t host : {1u, 2u, 4u, 8u}) {
    gb::DrawFacts f = GoodDraw();
    f.main_samples = 2;  // the play view of the game's MSAA: the guest's samples, whatever the host does with them
    const gb::DrawDecision play = gb::Decide(f);
    Check(play.why == Why::kWrites && play.attach() && gb::SidecarSamples(host) == host, Num("the play view carries the sidecar at host MSAA %.0f (a %.0f sample sidecar)", double(host), double(gb::SidecarSamples(host))));
    f.main_samples = 4;
    f.masked_attach = true;
    Check(gb::Decide(f).why == Why::kPauseView && !gb::Decide(f).attach(), Num("the pause world carries nothing, with a host MSAA of %.0f and the non writers carried", double(host)));
  }
  for (Why why : {Why::kWrites, Why::kMaskedBlended, Why::kMaskedNoVariant, Why::kMaskedOtherVariant, Why::kMaskedNoColor, Why::kMaskedNoDepth, Why::kMaskedOtherDepth,
                  Why::kMaskedSubView, Why::kMaskedNarrowMask, Why::kNotOneColor, Why::kWindowClosed}) {
    Check(gb::IsPlayViewWhy(why), "an outcome of a play view draw is the play view's");
  }
  for (Why why : {Why::kOff, Why::kNotMainView, Why::kPauseView, Why::kSuppressed}) {
    Check(!gb::IsPlayViewWhy(why), "off, a side view, the pause world and a cutscene's frame are not the play view's");
  }
  gb::ViewTally pause;
  pause.Note(Why::kPauseView);
  pause.Note(Why::kPauseView);
  pause.Note(Why::kNotMainView);
  Check(pause.pause_world() && !pause.play_world() && !pause.mixed() && pause.pause == 2 && pause.play == 0, "a frame of pause draws (and side views) is the pause world's");
  gb::ViewTally play;
  play.Note(Why::kWrites);
  play.Note(Why::kMaskedBlended);
  play.Note(Why::kNotMainView);
  Check(play.play_world() && !play.pause_world() && !play.mixed() && play.play == 2, "a frame of play draws is the play view's");
  gb::ViewTally both = play;
  both.Note(Why::kPauseView);
  Check(both.mixed() && !both.play_world() && !both.pause_world(), "a frame with both kinds is neither (and is counted)");
  const gb::ViewTally none;
  Check(!none.play_world() && !none.pause_world() && !none.mixed(), "a frame with no main view draw is neither");

  // K5: the camera of the last frame, kept by the G-buffer itself at each anchor
  gb::CameraKeeper keeper;
  keeper.BeginFrame(100);
  Check(keeper.Previous() == nullptr, "before any anchor there is no camera");
  gb::CameraAxes a = MakeAxes(10.0f);
  keeper.OnAnchor(&a, play);
  Check(keeper.Previous() == nullptr, "the frame's own anchor comes after its draws: the camera is for the next frame");
  keeper.BeginFrame(101);
  const gb::CameraAxes* got = keeper.Previous();
  Check(got != nullptr && got->width == 1280 && got->height == 720 && std::fabs(got->right.x - a.right.x) < 1e-6f && std::fabs(got->tan_half_x - 0.7f) < 1e-6f,
        "the next frame has the last frame's camera, with no upscaler involved");
  float ray[4], rot[4];
  Check(got != nullptr && gb::MakeCameraRows(got->right, got->up, got->forward, got->tan_half_x, got->tan_half_y, 2560.0f, 1440.0f, ray, rot), "and it makes the constants' camera rows (at the draw's scale)");
  gb::CameraAxes b = MakeAxes(12.0f);
  keeper.OnAnchor(&b, play);
  keeper.BeginFrame(102);
  Check(keeper.Previous() != nullptr && std::fabs(keeper.Previous()->forward.x - b.forward.x) < 1e-6f, "every frame replaces it");
  keeper.BeginFrame(103);  // a frame with no anchor (a cutscene's: the G-buffer does not track it): the next one has a gap
  keeper.BeginFrame(104);
  Check(keeper.Previous() == nullptr, "a gap of frames (a cutscene, the G-buffer off) leaves the next frame with none");
  keeper.OnAnchor(&a, play);
  keeper.BeginFrame(105);
  Check(keeper.Previous() != nullptr, "and the frame after an anchor has one again");
  keeper.OnAnchor(&a, pause);
  keeper.BeginFrame(106);
  Check(keeper.Previous() == nullptr, "the pause world's camera is not kept (another lens): the first play frame after it has none");
  keeper.OnAnchor(&a, play);
  keeper.BeginFrame(107);
  Check(keeper.Previous() != nullptr, "the second play frame after it has one");
  keeper.OnAnchor(&a, both);
  keeper.BeginFrame(108);
  Check(keeper.Previous() == nullptr, "a frame with both kinds keeps none");
  keeper.OnAnchor(&a, play);
  keeper.BeginFrame(109);
  keeper.OnAnchor(nullptr, play);
  keeper.BeginFrame(110);
  Check(keeper.Previous() == nullptr, "an anchor whose constants gave no camera keeps none");
  keeper.OnAnchor(&a, none);
  keeper.BeginFrame(111);
  Check(keeper.Previous() == nullptr, "a frame with no main view keeps none");
  // the frame count wraps nothing in practice, but a repeated serial is not a "next" frame
  keeper.OnAnchor(&a, play);
  keeper.BeginFrame(111);
  Check(keeper.Previous() == nullptr, "the same frame again is not the next frame");
}

// ---------------------------------------------------------------------------
// what a writer's constants block holds
// ---------------------------------------------------------------------------
void ModeChecks() {
  Check(gb::ModeBits(0, false, false) == 0, "nothing written, no bits");
  Check(gb::ModeBits(1, false, false) == gb::kModeWriteNormal, "the normal plane: its bit");
  Check(gb::ModeBits(7, false, false) == (gb::kModeWriteNormal | gb::kModeWriteAlbedo | gb::kModeWriteMaterial), "all three planes: three bits");
  Check(gb::ModeBits(1, true, false) == (gb::kModeWriteNormal | gb::kModeDerivativeNormal), "a camera in the block: the permission for the derivative normal");
  Check(gb::ModeBits(1, false, true) == (gb::kModeWriteNormal | gb::kModeFlipBackface), "a two sided draw: the back face flip");
  const uint32_t word = gb::DefaultMaterialWord();
  Check(gb::ClassOfByte(gb::MaterialRt3ClassAndFlags(word)) == gb::SurfaceClass::kWorld && gb::MaterialRt3Smoothness(word) == 0 && gb::MaterialLitByte(word) == 255 &&
            gb::FlagsOfByte(gb::MaterialRt3ClassAndFlags(word)) == 0,
        "the default material: the world class, smoothness 0, lit by the forward lights");
}

// ---------------------------------------------------------------------------
// the camera rows of the block: the world point a fragment's position gives back, for a left handed and a right handed world
// ---------------------------------------------------------------------------
void CameraChecks() {
  using gb::Vec3;
  const float width = 1920.0f, height = 1080.0f;
  const float tan_y = std::tan(0.5f * 0.9f);  // a vertical field of view of 0.9 radians
  const float tan_x = tan_y * width / height;
  int bad = 0;
  int tried = 0;
  for (int handed = 0; handed < 2; ++handed) {
    for (int k = 0; k < 24; ++k) {
      const float yaw = 0.4f * float(k) - 3.0f;
      const float pitch = 0.9f * std::sin(0.7f * float(k));
      const Vec3 forward = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
      const Vec3 world_up = {0.0f, 1.0f, 0.0f};
      // left handed: right = up x forward; right handed (a camera that looks along minus z): right = forward x up. up is right x forward or forward x right.
      const Vec3 right = gb::Normalize(handed == 0 ? gb::Cross(world_up, forward) : gb::Cross(forward, world_up));
      const Vec3 up = gb::Normalize(handed == 0 ? gb::Cross(forward, right) : gb::Cross(right, forward));
      const float det = gb::Dot(right, gb::Cross(up, forward));
      Check((handed == 0 && det > 0.99f) || (handed == 1 && det < -0.99f), Num("the test's camera %d has the expected handedness (det %.3f)", double(handed), double(det)));
      float ray[4];
      float cam[4];
      Check(gb::MakeCameraRows(right, up, forward, tan_x, tan_y, width, height, ray, cam), "a camera's axes make the rows");
      const gb::Quat q{cam[0], cam[1], cam[2], cam[3]};
      Check(std::fabs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0f) < 1.0e-4f, "the rotation is a unit quaternion");
      for (int j = 0; j < 6; ++j) {
        // a point in front of the camera, by its screen position and distance
        const float sx = -0.9f + 0.36f * float(j);
        const float sy = 0.8f - 0.31f * float(j);
        const float depth = 3.0f + 17.0f * float(j + 1);
        const Vec3 p = right * (sx * tan_x * depth) + up * (sy * tan_y * depth) + forward * depth;
        const float px = (sx * 0.5f + 0.5f) * width;
        const float py = (0.5f - sy * 0.5f) * height;
        const Vec3 got = gb::WorldPoint(ray, q, px, py, 1.0f / depth);
        const Vec3 d = got - p;
        ++tried;
        if (gb::Length(d) > 2.0e-4f * depth) {
          ++bad;
          if (bad < 4) std::printf("camera %d.%d point %d: got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", handed, k, j, got.x, got.y, got.z, p.x, p.y, p.z);
        }
      }
    }
  }
  Check(bad == 0, Num("the world point of a fragment is the point it was made from, left and right handed worlds, %d points (%d wrong)", double(tried), double(bad)));
  // not a camera: a short axis, no field of view, no size, axes that are not square
  float ray[4];
  float cam[4];
  const Vec3 x = {1, 0, 0}, y = {0, 1, 0}, z = {0, 0, 1};
  Check(gb::MakeCameraRows(x, y, z, 1.0f, 0.5f, 100, 50, ray, cam), "the identity camera is one");
  Check(!gb::MakeCameraRows(x * 0.5f, y, z, 1.0f, 0.5f, 100, 50, ray, cam), "a short axis is not");
  Check(!gb::MakeCameraRows(x, y, z, 0.0f, 0.5f, 100, 50, ray, cam) && !gb::MakeCameraRows(x, y, z, 1.0f, 0.5f, 0, 50, ray, cam), "no field of view or no size is not");
  Check(!gb::MakeCameraRows(x, x, z, 1.0f, 0.5f, 100, 50, ray, cam), "axes that are not square are not");
  // the identity camera: the ray's numbers are the IF's (gbuffer_types.h: x = 2 tan(half fov x) / width, y = -tan(half fov x), z = -2 tan(half fov y) / height, w = tan(half fov y))
  gb::MakeCameraRows(x, y, z, 1.0f, 0.5f, 100, 50, ray, cam);
  Check(std::fabs(ray[0] - 0.02f) < 1e-6f && std::fabs(ray[1] + 1.0f) < 1e-6f && std::fabs(ray[2] + 0.02f) < 1e-6f && std::fabs(ray[3] - 0.5f) < 1e-6f &&
            std::fabs(cam[3] - 1.0f) < 1e-6f && std::fabs(cam[0]) < 1e-6f,
        "the identity camera: the IF's ray, no rotation");
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
  PlaneChecks();
  MsaaChecks();
  AttachChecks();
  ViewMaskChecks();
  PauseAndCameraChecks();
  ModeChecks();
  CameraChecks();
  const std::string text = ReadFile(hlsli);
  Check(!text.empty(), "the .hlsli is readable: " + hlsli);
  if (!text.empty()) HlslDefinesChecks(text);
  HlslAgainstTwinChecks();
  std::printf("gbuffer_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

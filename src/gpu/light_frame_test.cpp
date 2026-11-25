// Check of the one light description (light_frame.h, light_id.h, light_laws.h, light_convert.h, hlsl/light_laws.hlsli; perf/LIGHT-UNIFY.md package LU-1 = step U1): the layout of the
// frozen records, the GpuLight's bits and its round trip, the ids (a street lamp's id IS volumetrics_lamps.h LampKey and lamp_shadow_block.h KeyOf), the laws held to the numbers that
// exist (the rig's LampScale, the game's lamp law and cone, the volume's lamp), the conversions from today's producers and their projections into the volume's lamp, the shadow
// combine rule, and the HLSL twin compiled as C++ and compared with the C++ laws. A program of its own: no renderer, no ReXGlue, no GPU, no game; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_light_frame_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_light_frame_test [<hlsl/light_laws.hlsli>]     (found next to this file without an argument)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "headlight_rig_rules.h"
#include "headlight_types.h"
#include "light_set.h"  // the thin include: everything below comes through it
#include "local_shadow_types.h"
#include "vol_lamp_list.h"
#include "volumetrics_headlights.h"
#include "volumetrics_lamps.h"
#include "volumetrics_rules.h"
#include "volumetrics_types.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the file uses, in a namespace of its own so that the standard library's abs, sqrt ... are not in the way
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
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
#include "hlsl/light_laws.hlsli"
static_assert(sizeof(LlGpuLight) == 64, "the twin's row is the GpuLight's 64 bytes");
}  // namespace hlsl_shim

namespace ll = larecomp::gpu::light_laws;
namespace lid = larecomp::gpu::light_id;
namespace vol = mcla::gpu::volumetrics;
namespace hr = mcla::gpu::headlight_rules;
using namespace larecomp::gpu;

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
void CheckNear(double got, double want, double tol, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g (tolerance %.3g)\n", what.c_str(), got, want, tol);
  }
}
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[320];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", v);
  return buf;
}

struct Lcg {
  uint32_t state = 20261120u;
  float Next() {  // 0 to 1
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return uint32_t(Next() * float(n)) % n; }
  void Direction(float out[3]) {  // a uniform direction on the sphere
    const float z = Range(-1.0f, 1.0f);
    const float a = Range(0.0f, 6.2831853f);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    out[0] = r * std::cos(a);
    out[1] = z;
    out[2] = r * std::sin(a);
  }
};

constexpr double kDeg = 3.14159265358979323846 / 180.0;

// ---------------------------------------------------------------------------
// the layout
// ---------------------------------------------------------------------------

void LayoutChecks() {
  // the same numbers as the header's static_asserts, so that a failing build names them and a passing run says them
  Check(sizeof(Light) == 120 && alignof(Light) == 8, "Light is 120 bytes");
  Check(sizeof(GpuLight) == 64 && alignof(GpuLight) == 16, "GpuLight is 64 bytes, four float4");
  Check(sizeof(GpuLightHeader) == 16 && sizeof(GpuLightBuffer) == 16 + 64 * kMaxLights, "the GPU buffer is a 16 byte header and 64 rows");
  Check(sizeof(ShadowRef) == 12 && sizeof(CarLights) == 28, "ShadowRef 12 bytes, CarLights 28");
  Check(sizeof(LocalShadowView) == 96, "the shadow view stays 96 bytes");
  Check(sizeof(vol::VolShadowView) == sizeof(LocalShadowView) && sizeof(vol::VolLamp) == 64, "the volume's shadow view and lamp are the twins' sizes");
  Check(std::is_trivially_copyable_v<LightFrame> && std::is_trivially_copyable_v<Light> && std::is_trivially_copyable_v<GpuLight> && std::is_trivially_copyable_v<ShadowFrame>,
        "a LightFrame is plain data: it copies with memcpy");
  Check(kMaxLights == 64 && kMaxSurfaceLights == 32 && kMaxLightShadowViews == 8 && kMaxCars == 8 && kRoleCount == 3, "the limits");
  Check(offsetof(GpuLight, row1) == 16 && offsetof(GpuLight, packed0) == 48 && offsetof(GpuLight, shadow_weight) == 60, "GpuLight: rows at 0, 16, 32, the packed word at 48");
  // VolLamp is the same four rows: the volume's near radius squared is its pad[0] at 52, the first shadow view at 48 (volumetrics_inputs_test.cpp holds that too)
  Check(offsetof(vol::VolLamp, pos) == 0 && offsetof(vol::VolLamp, dir) == 16 && offsetof(vol::VolLamp, colour) == 32 && offsetof(vol::VolLamp, shadow_view) == 48 &&
            offsetof(vol::VolLamp, pad) == 52,
        "VolLamp's rows are GpuLight's: pos+range, dir+cos_outer, colour+cos_inner, then the shadow view and the near radius squared");
  // the enums' numbers are the file's (the HLSL twin and the packing read them)
  Check(uint32_t(LightKind::kSpot) == 0 && uint32_t(LightKind::kPoint) == 1, "LightKind numbers");
  Check(uint32_t(LightSource::kHeadlight) == 0 && uint32_t(LightSource::kLamp) == 1 && uint32_t(LightSource::kNeon) == 2 && uint32_t(LightSource::kPolice) == 3 &&
            uint32_t(LightSource::kMod) == 4 && uint32_t(LightSource::kOther) == 5,
        "LightSource numbers (light_set.h's)");
  Check(uint32_t(LightLaw::kHeadlight) == 0 && uint32_t(LightLaw::kGameLamp) == 1 && uint32_t(LightLaw::kOmni) == 2 && uint32_t(LightLaw::kNeon) == 3, "LightLaw numbers (kNeon is the last of the two bits: GB-NEON-TUNE)");
  Check(uint32_t(ShadowKind::kNone) == 0 && uint32_t(ShadowKind::kAtlas) == 1 && uint32_t(ShadowKind::kRtMask) == 2, "ShadowKind numbers");
  Check(uint32_t(ShadowLayer::kSingle) == 0 && uint32_t(ShadowLayer::kStatic) == 1 && uint32_t(ShadowLayer::kLive) == 2, "ShadowLayer numbers");
  Check(RoleBit(LightRole::kSurface) == kRoleMaskSurface && RoleBit(LightRole::kVolume) == kRoleMaskVolume && RoleBit(LightRole::kShadow) == kRoleMaskShadow,
        "RoleBit is the mask of a role");
  Check(kRoleMaskGameLit == (kRoleMaskVolume | kRoleMaskShadow | kRoleMaskRt) && (kRoleMaskGameLit & kRoleMaskSurface) == 0, "a game-lit light takes no surface light");
  // the defaults: a default Light is off (light_set.h's rule), a default frame is empty
  Light off;
  Check(off.kind == LightKind::kSpot && off.cos_outer == -1.0f && off.range == 0.0f && off.shadow.kind == ShadowKind::kNone && off.id == kNoLightId, "the default light is off");
  Check(ll::LightScale(off, ll::LightSurface{}) == 0.0f, "a default light lights nothing");
  LightFrame frame;
  Check(frame.count == 0 && frame.car_count == 0 && frame.shadow.count == 0 && frame.gpu.lights_slot == kNoLightSlot && !frame.shadow.atlas.valid, "a default frame is empty");
  std::printf("light_frame_test: sizeof(Light) %zu, LightFrame %zu, ShadowFrame %zu\n", sizeof(Light), sizeof(LightFrame), sizeof(ShadowFrame));
}

// ---------------------------------------------------------------------------
// GpuLight: the bits and the round trip
// ---------------------------------------------------------------------------

Light RandomLight(Lcg& rng, uint32_t law_pick) {
  Light l;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = rng.Range(-200.0f, 200.0f);
    l.color[i] = rng.Range(0.0f, 100.0f);
    l.vel[i] = rng.Range(-30.0f, 30.0f);
    l.offset[i] = rng.Range(-1.0f, 1.0f);
  }
  rng.Direction(l.dir);
  l.range = rng.Range(0.0f, 120.0f);
  l.cos_outer = rng.Next() < 0.2f ? -1.0f : rng.Range(-0.5f, 0.9f);
  l.cos_inner = std::min(1.0f, l.cos_outer + rng.Range(0.0f, 0.3f));
  l.vol_weight = rng.Next();
  l.law_a = rng.Range(1.0f, 100.0f);
  l.kind = l.cos_outer <= -0.999f ? LightKind::kPoint : LightKind::kSpot;
  l.source = LightSource(rng.Below(6));
  l.law = LightLaw(law_pick % 4);
  l.state = FadeState(rng.Below(4));
  l.roles = uint8_t(rng.Below(16));
  l.flags = rng.Below(1u << 20) | (rng.Next() < 0.5f ? 0x80000000u : 0u);
  l.car_key = rng.Below(1000);
  l.id = (uint64_t(rng.Below(1u << 30)) << 32) | rng.Below(1u << 30);
  l.importance = rng.Next();
  l.shadow.kind = ShadowKind(rng.Below(3));
  l.shadow.view_count = uint8_t(rng.Below(3));
  l.shadow.first_view = uint8_t(rng.Below(8));
  l.shadow.mask_channel = uint8_t(rng.Below(4));
  l.shadow.weight = rng.Next();
  l.shadow.reach = rng.Range(10.0f, 70.0f);
  return l;
}

void GpuLightChecks() {
  // the bits, spelt out once
  Light l;
  l.pos[0] = 1.0f;
  l.pos[1] = 2.0f;
  l.pos[2] = 3.0f;
  l.range = 40.0f;
  l.dir[0] = 0.0f;
  l.dir[1] = 0.0f;
  l.dir[2] = 1.0f;
  l.cos_outer = 0.7f;
  l.color[0] = 0.5f;
  l.color[1] = 0.6f;
  l.color[2] = 0.7f;
  l.cos_inner = 0.9f;
  l.kind = LightKind::kPoint;
  l.source = LightSource::kPolice;
  l.law = LightLaw::kOmni;
  l.roles = 0xA;
  l.flags = 0x8005u | 0x10000u;  // the flag at bit 16 is CPU side: not carried
  l.law_a = 12.5f;
  l.shadow.kind = ShadowKind::kAtlas;
  l.shadow.first_view = 5;
  l.shadow.view_count = 2;
  l.shadow.mask_channel = 3;
  l.shadow.weight = 0.25f;
  const GpuLight g = PackGpuLight(l);
  Check(g.row0[0] == 1.0f && g.row0[1] == 2.0f && g.row0[2] == 3.0f && g.row0[3] == 40.0f, "row 0: pos.xyz, range");
  Check(g.row1[2] == 1.0f && g.row1[3] == 0.7f, "row 1: dir.xyz, cos_outer");
  Check(g.row2[0] == 0.5f && g.row2[2] == 0.7f && g.row2[3] == 0.9f, "row 2: color.xyz, cos_inner");
  Check(g.packed0 == 0x8005054Du, "packed0 = kind 1 | source 3 << 2 | law 2 << 5 | roles 0xA << 7 | flags 0x8005 << 16 is 0x8005054D (got " + Hex(g.packed0) + ")");
  Check(g.law_a == 12.5f && g.shadow_weight == 0.25f, "row 3: law_a and the shadow weight");
  Check(g.packed1 == 0x03010205u, "packed1 = first_view 5 | count 2 << 8 | kind 1 << 16 | channel 3 << 24 is 0x03010205 (got " + Hex(g.packed1) + ")");
  const Light back = UnpackGpuLight(g);
  Check(back.flags == 0x8005u, "flags above 16 bits are not on the GPU");
  // the round trip over random lights: every field the GPU carries comes back, nothing else is invented
  Lcg rng;
  int bad = 0;
  for (int i = 0; i < 50000; ++i) {
    const Light a = RandomLight(rng, uint32_t(i));
    const Light b = UnpackGpuLight(PackGpuLight(a));
    const bool same = std::memcmp(a.pos, b.pos, 12) == 0 && a.range == b.range && std::memcmp(a.dir, b.dir, 12) == 0 && a.cos_outer == b.cos_outer &&
                      std::memcmp(a.color, b.color, 12) == 0 && a.cos_inner == b.cos_inner && a.kind == b.kind && a.source == b.source && a.law == b.law &&
                      a.roles == b.roles && (a.flags & kLightFlagsGpuMask) == b.flags && a.law_a == b.law_a && a.shadow.kind == b.shadow.kind &&
                      a.shadow.view_count == b.shadow.view_count && a.shadow.first_view == b.shadow.first_view && a.shadow.mask_channel == b.shadow.mask_channel &&
                      a.shadow.weight == b.shadow.weight;
    const bool defaults = b.id == kNoLightId && b.car_key == 0 && b.vol_weight == 1.0f && b.vel[0] == 0.0f && b.state == FadeState::kSteady;
    bad += (same && defaults) ? 0 : 1;
  }
  Check(bad == 0, "Light -> GpuLight -> Light keeps every carried field (50000 random lights)");
  // the buffer's two parts
  GpuLightBuffer buffer;
  Check(buffer.header.count == 0 && buffer.lights[kMaxLights - 1].packed0 == 0 && buffer.lights[0].row0[3] == 0.0f, "the buffer is zero: a row past `count` is a light of no range");
}

// ---------------------------------------------------------------------------
// the ids
// ---------------------------------------------------------------------------

// lamp_shadow_block.h KeyOf of feat/lamp-shadows (not on this tree yet): a mirror of it, so that the equality is held today and the real one replaces it when that branch lands.
uint64_t KeyOfMirror(const float pos[3]) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < 3; ++i) {
    const int64_t q = int64_t(std::floor(double(pos[i]) * 8.0));
    h ^= uint64_t(q) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  }
  return h;
}

void IdChecks() {
  Lcg rng;
  int bad_key = 0, bad_of = 0;
  for (int i = 0; i < 20000; ++i) {
    mcla::gpu::vol_lamps::GameLamp g;
    for (int k = 0; k < 3; ++k) {
      g.pos[k] = rng.Range(-3500.0f, 3500.0f);
    }
    bad_key += lid::LampId(g) == vol::LampKey(g) ? 0 : 1;
    bad_of += lid::LampId(g.pos) == KeyOfMirror(g.pos) ? 0 : 1;
  }
  Check(bad_key == 0, "a street lamp's id is volumetrics_lamps.h LampKey (20000 places)");
  Check(bad_of == 0, "and lamp_shadow_block.h KeyOf");
  // the place to an eighth of a metre (floor): the same cell, the same id; the next cell, another
  const float a[3] = {10.0f, 2.0f, -7.0f}, b[3] = {10.124f, 2.0f, -7.0f}, c[3] = {10.125f, 2.0f, -7.0f}, d[3] = {10.0f, 2.0f, -7.001f};
  Check(lid::LampId(a) == lid::LampId(b), "positions in one eighth of a metre share an id");
  Check(lid::LampId(a) != lid::LampId(c) && lid::LampId(a) != lid::LampId(d), "the next cell and a negative neighbour have their own");
  // domains and stability
  Check(lid::CarLampId(7) == lid::CarLampId(7, lid::CarLamp::kPair) && lid::CarLampId(7) == lid::CarLampId(7), "a car's id is stable");
  Check(lid::CarLampId(7, lid::CarLamp::kPair) != lid::CarLampId(7, lid::CarLamp::kLeft) && lid::CarLampId(7, lid::CarLamp::kLeft) != lid::CarLampId(7, lid::CarLamp::kRight) &&
            lid::CarLampId(7) != lid::CarLampId(8),
        "a car's pair, left and right lamps and two cars have ids of their own");
  Check(lid::ModLampId(lid::ModKey("mymod"), 3) != lid::ModLampId(lid::ModKey("mymod"), 4) && lid::ModLampId(lid::ModKey("mymod"), 3) != lid::ModLampId(lid::ModKey("other"), 3),
        "a mod's lamp id is the mod and the record");
  Check(lid::ModKey("") == 2166136261u && lid::ModKey("a") == 0xE40C292Cu, "ModKey is FNV-1a 32 (the empty string and 'a' are the published values)");
  Check(lid::PointId(0x82345678u, 3) != lid::PointId(0x82345678u, 4) && lid::PointId(0x82345678u, 3) != lid::CarLampId(0x82345678u, lid::CarLamp::kPair), "a point's id is the vehicle and the index, in a domain of its own");
  // no meeting across domains, no zero: ten thousand of each family
  std::map<uint64_t, int> seen;
  int collisions = 0, zeros = 0;
  const auto take = [&](uint64_t id) {
    collisions += seen.count(id) != 0 ? 1 : 0;
    zeros += id == kNoLightId ? 1 : 0;
    seen[id] = 1;
  };
  for (uint32_t i = 0; i < 10000; ++i) {
    take(lid::CarLampId(i, lid::CarLamp(i % 3)));
    take(lid::ModLampId(i / 100, i));
    take(lid::PointId(0x82000000u + i * 16u, i % 8));
    const float p[3] = {float(i) * 3.7f, 1.0f, float(i % 97)};
    take(lid::NeonId(p, i % 5));
    take(lid::LampId(p));
  }
  Check(collisions == 0 && zeros == 0, Num("50000 ids of five sources do not meet and are never 0 (%.0f collisions, %.0f zeros)", double(collisions), double(zeros)));
  Check(lid::Make(1, 0, 0) != kNoLightId, "Make never gives 0");
}

// ---------------------------------------------------------------------------
// the laws: held to the numbers that exist
// ---------------------------------------------------------------------------

mcla::gpu::vol_lamps::GameLamp StreetLamp(float x, float z, float intensity, float height = 8.0f) {
  mcla::gpu::vol_lamps::GameLamp l;
  l.pos[0] = x;
  l.pos[1] = height;
  l.pos[2] = z;
  l.colour[0] = 1.0f;
  l.colour[1] = 0.8f;
  l.colour[2] = 0.55f;
  l.dir[0] = 0.0f;
  l.dir[1] = -1.0f;
  l.dir[2] = 0.0f;
  l.intensity = intensity;
  l.cone_deg = 125.0f;
  l.spread_deg = -50.5f;
  l.scale = 1.0f;
  l.type = 1;
  l.on = 1;
  return l;
}

hr::Lamp RigLamp(const Light& l) {
  hr::Lamp lamp;
  for (int i = 0; i < 3; ++i) {
    lamp.position[i] = l.pos[i];
    lamp.direction[i] = l.dir[i];
    lamp.color[i] = l.color[i];
  }
  lamp.range = l.range;
  lamp.cos_outer = l.cos_outer;
  lamp.cos_inner = l.cos_inner;
  return lamp;
}

void ConstantChecks() {
  // the headlight's look is the rig's, the lamp's units are the volume's, the share is the volume's: one number each, held equal
  Check(ll::kHeadlightPeak == hr::kPeak && ll::kHeadlightFalloffDistance == hr::kFalloffDistance && ll::kSurfaceWrap == hr::kSurfaceWrap && ll::kCutLow == hr::kCutLow &&
            ll::kCutHigh == hr::kCutHigh && ll::kNearStart == hr::kNearStart && ll::kNearSpan == hr::kNearSpan,
        "the headlight law's constants are headlight_rig_rules.h's");
  Check(ll::kHeadlightPeak == vol::kHeadlightPeak && ll::kHeadlightFalloffDistance == vol::kHeadlightFalloffDistance, "and the volume's headlight units'");
  Check(ll::kReachShare == vol::kReachShare && ll::kMinReach == vol::kMinReach && ll::kMaxReach == vol::kMaxReach && ll::kLampNearShare == vol::kLampNearShare &&
            ll::kVolumeNearMin == vol::kLampNear,
        "the lamp's reach, its clamps and the near share are volumetrics_lamps.h's");
  Check(ll::kPi == vol::kPi, "pi");
}

void GameLampLawChecks() {
  // the game's term: full inside sqrt(I), I / d^2 beyond, 1 per cent at the reach sqrt(100 I) (the mod loader's "20 reaches 44.7 m")
  CheckNear(ll::GameLampTerm(55.0f, 0.0f), 1.0, 1e-9, "the game's term is 1 at the lamp");
  CheckNear(ll::GameLampTerm(55.0f, 7.0f), 1.0, 1e-9, "and inside the plateau (sqrt(55) = 7.4 m)");
  CheckNear(ll::GameLampTerm(55.0f, 14.83f), 0.25, 2e-3, "twice the plateau's radius: a quarter");
  CheckNear(ll::GameReach(20.0f), 44.72, 0.01, "a lamp of 20 reaches 44.7 m");
  CheckNear(ll::GameLampTerm(20.0f, ll::GameReach(20.0f)), 0.01, 1e-5, "at the reach the game's term is 1 per cent");
  CheckNear(ll::GameLampTerm(0.0f, 3.0f), 0.0, 1e-9, "no intensity, no light");
  CheckNear(ll::LampRange(0.3f), 8.0, 1e-6, "the range has a floor of 8 m") ;
  CheckNear(ll::LampRange(400.0f), 120.0, 1e-6, "and a ceiling of 120 m");
  CheckNear(ll::LampShadowReach(55.0f), 20.98, 0.01, "a lamp of 55 has a shadow that reaches 21 m (sqrt(8 I))");
  CheckNear(ll::LampShadowReach(100.0f), 28.28, 0.01, "100: 28 m");
  CheckNear(ll::LampShadowReach(400.0f), 30.0, 1e-6, "at most 30 m");
  CheckNear(ll::LampShadowReach(2.0f), 14.0, 1e-6, "at least 14 m");
  // the same functions as the volume's, over a sweep (they are copies until the volume's own are deleted in LU-4a)
  Lcg rng;
  int bad_term = 0, bad_reach = 0, bad_cone = 0;
  for (int i = 0; i < 20000; ++i) {
    const float intensity = rng.Range(0.0f, 150.0f);
    const float dist = rng.Range(0.0f, 120.0f);
    bad_term += ll::GameLampTerm(intensity, dist) == vol::GameLampTerm(intensity, dist) ? 0 : 1;
    bad_reach += ll::GameReach(intensity) == vol::GameReach(intensity) ? 0 : 1;
    const float cone = rng.Range(0.0f, 400.0f), spread = rng.Range(-90.0f, 90.0f);
    float co1, ci1, co2, ci2;
    ll::ConeCosines(cone, spread, &co1, &ci1);
    vol::ConeCosines(cone, spread, &co2, &ci2);
    bad_cone += (co1 == co2 && ci1 == ci2) ? 0 : 1;
  }
  Check(bad_term == 0 && bad_reach == 0 && bad_cone == 0, "GameLampTerm, GameReach and ConeCosines equal the volume's on 20000 random inputs");
  // the cone (sub_822FD2C8): half angles of the angle and of the spread; a negative spread puts the soft edge inside the angle
  float co, ci;
  ll::ConeCosines(125.0f, -50.5f, &co, &ci);
  CheckNear(std::acos(ci) / kDeg, 37.25, 1e-3, "a street lamp (125, -50.5) is full inside 37.25 degrees");
  CheckNear(std::acos(co) / kDeg, 62.5, 1e-3, "and gone at 62.5");
  ll::ConeCosines(90.0f, 0.0f, &co, &ci);
  CheckNear(std::acos(co) / kDeg - std::acos(ci) / kDeg, 0.5, 1e-3, "a spread of 0 counts as 1: a half degree each side");
}

// A lamp of the game's list as a Light, lit surfaces below it: colour x min(1/I, 1/d^2) x cone x N.L, against the game's formula written out in double.
void GameLampLightChecks() {
  const auto lamp = StreetLamp(0.0f, 0.0f, 55.0f, 8.0f);
  const Light l = LightFromGameLamp(lamp);
  Check(l.law == LightLaw::kGameLamp && l.kind == LightKind::kSpot && l.source == LightSource::kLamp && l.roles == kRoleMaskGameLit && (l.flags & kLightFlagGameLit) != 0,
        "a street lamp is a spot of law kGameLamp that the game's own shaders already light with");
  CheckNear(l.color[0], 55.0, 1e-4, "its colour carries the intensity: 1.0 x 55");
  CheckNear(l.color[1], 44.0, 1e-4, "0.8 x 55");
  CheckNear(l.color[2], 30.25, 1e-4, "0.55 x 55");
  CheckNear(l.range, std::sqrt(5500.0), 1e-3, "its range is the reach sqrt(100 I)");
  CheckNear(l.law_a, 55.0, 1e-6, "law_a is the plateau's radius squared, I");
  CheckNear(l.shadow.reach, std::sqrt(8.0 * 55.0), 1e-3, "its shadow's reach is sqrt(8 I)");
  CheckNear(l.dir[1], -1.0, 1e-6, "pointing down");
  CheckNear(std::acos(l.cos_outer) / kDeg, 62.5, 1e-3, "the cone is the game's: gone at 62.5 degrees");
  Check(l.id == lid::LampId(lamp.pos), "and its id is its place");
  // the ground below: a point at ground distance x from the pole, normal up
  const double cos_full = std::cos(37.25 * kDeg), cos_gone = std::cos(62.5 * kDeg);
  int checked = 0;
  for (double x = 0.0; x <= 40.0; x += 1.25) {
    ll::LightSurface s;
    s.position[0] = float(x);
    s.position[1] = 0.0f;
    s.position[2] = 0.0f;
    const double d = std::sqrt(x * x + 64.0);
    const double cosine = 8.0 / d;  // the angle from the axis (straight down) and N.L of an upward normal: the same
    double cone = 0.0;
    if (cosine >= cos_full) {
      cone = 1.0;
    } else if (cosine > cos_gone) {
      const double t = (cosine - cos_gone) / (cos_full - cos_gone);
      cone = t * t * (3.0 - 2.0 * t);
    }
    const double plateau = d * d <= 55.0 ? 1.0 / 55.0 : 1.0 / (d * d);
    const double want = plateau * cone * cosine;
    float term[3];
    ll::LightTerm(l, s, 1.0f, term);
    const double scale = double(ll::LightScale(l, s));
    if (std::fabs(scale - want) > 1e-5 * std::max(1.0, want * 55.0)) {
      Check(false, Num("a street lamp's term at %.2f m from the pole: %.6f, want %.6f", x, scale, want));
    }
    CheckNear(term[0], double(l.color[0]) * scale, 1e-4, "the term is colour x scale");
    ++checked;
  }
  Check(checked > 30, "the street lamp's pool was swept");
  // on the axis the term IS the game's saturate(I / d^2) times the lamp's rgb
  for (const float height : {3.0f, 6.0f, 7.4f, 8.0f, 12.0f, 30.0f}) {
    const Light on_axis = LightFromGameLamp(StreetLamp(0.0f, 0.0f, 55.0f, height));
    ll::LightSurface s;
    float term[3];
    ll::LightTerm(on_axis, s, 1.0f, term);
    CheckNear(term[0], 1.0 * ll::GameLampTerm(55.0f, height), 1e-5, Num("under the lamp at %.1f m: the game's saturate(I/d^2) x rgb", double(height)));
    CheckNear(term[1], 0.8 * ll::GameLampTerm(55.0f, height), 1e-5, "and the green channel");
  }
  // the cone, the surface term, the range cut, the shadow's visibility
  {
    ll::LightSurface s;
    s.position[0] = 14.0f;  // 8 m below, 14 m across: 60.3 degrees: inside the ramp
    s.position[1] = 0.0f;
    const float inside = ll::LightScale(l, s);
    Check(inside > 0.0f, "inside the cone's ramp (60 degrees) a little light remains");
    s.position[0] = 16.0f;  // 63.4 degrees: gone
    Check(ll::LightScale(l, s) == 0.0f, "past 62.5 degrees nothing");
    // a point 5 m from the pole, its normal turned 60 degrees toward the lamp's side: the term changes by the ratio of the N.L's (the game's saturate(N.L): no wrap)
    ll::LightSurface up;
    up.position[0] = 5.0f;
    ll::LightSurface tilted = up;
    const double to_lamp[3] = {-5.0 / std::sqrt(89.0), 8.0 / std::sqrt(89.0), 0.0};
    const float full = ll::LightScale(l, up);
    tilted.normal[0] = -float(std::sin(60.0 * kDeg));
    tilted.normal[1] = float(std::cos(60.0 * kDeg));
    tilted.normal[2] = 0.0f;
    const double n_dot_l = double(tilted.normal[0]) * to_lamp[0] + double(tilted.normal[1]) * to_lamp[1];
    CheckNear(double(ll::LightScale(l, tilted)) / double(full), n_dot_l / to_lamp[1], 1e-4, "the surface term is the game's N.L");
    ll::LightSurface facing_away;
    facing_away.position[0] = 5.0f;
    facing_away.normal[1] = -1.0f;
    Check(ll::LightScale(l, facing_away) == 0.0f, "a surface facing away from the lamp gets nothing");
    CheckNear(ll::LightScale(l, up, 0.25f), 0.25 * double(full), 1e-6, "visibility is the last factor");
  }
  {
    // the range: the term is 1 per cent of the lamp's colour at the reach and 0 at and beyond it
    Light flat = l;
    flat.cos_outer = -1.0f;  // an omni lamp: so that the far point is not cut by the cone
    flat.law = LightLaw::kOmni;
    flat.kind = LightKind::kPoint;
    // a point far to the side at the lamp's own height, the normal turned to the lamp
    ll::LightSurface far_side;
    far_side.position[0] = flat.range - 0.01f;
    far_side.position[1] = 8.0f;
    far_side.normal[0] = -1.0f;
    far_side.normal[1] = 0.0f;
    const double just_in = double(ll::LightScale(flat, far_side));
    CheckNear(just_in * double(flat.law_a), 0.01, 2e-4, "at the reach the game's term is 1 per cent of the colour (I x scale)");
    far_side.position[0] = flat.range;
    Check(ll::LightScale(flat, far_side) == 0.0f, "at the range: 0");
    far_side.position[0] = flat.range + 5.0f;
    Check(ll::LightScale(flat, far_side) == 0.0f, "beyond it: 0");
    // an omni light has no cone: behind the lamp's axis it still lights
    ll::LightSurface above;
    above.position[0] = 3.0f;
    above.position[1] = 14.0f;
    above.normal[1] = -1.0f;
    Check(ll::LightScale(flat, above) > 0.0f && ll::LightScale(l, above) == 0.0f, "an omni light lights above its axis, the spot lamp does not");
    Light no_law = flat;
    no_law.law_a = 0.0f;
    Check(ll::LightScale(no_law, far_side) == 0.0f, "a lamp with no plateau radius lights nothing");
  }
}

// GB-NEON-TUNE: the underglow's law (light_laws.h NeonScale, hlsl LlNeonScale): colour x w^2 / (d^2 + near^2) x wrap(N.L), w = saturate(1 - (d / range)^4), law_a = near^2.
void NeonLawChecks() {
  Light l;
  l.pos[1] = 0.5f;
  l.color[0] = 4.0f;
  l.law = LightLaw::kNeon;
  l.law_a = 0.64f;
  l.range = 5.0f;
  l.kind = LightKind::kPoint;
  l.cos_outer = l.cos_inner = -1.0f;
  const auto facing = [&](float d, float n_dot_l_tilt) {  // a surface d metres from the light along +x, its normal tilted by an angle whose cosine to the light is n_dot_l_tilt
    ll::LightSurface s;
    s.position[0] = d;
    s.position[1] = 0.5f;
    s.normal[0] = -n_dot_l_tilt;
    s.normal[1] = std::sqrt(std::max(0.0f, 1.0f - n_dot_l_tilt * n_dot_l_tilt));
    return s;
  };
  // golden values by hand: at 2 m facing the light: (1 - (2/5)^4)^2 / (4 + 0.64) = 0.97440^2 / 4.64
  CheckNear(ll::LightScale(l, facing(2.0f, 1.0f)), 0.974400 * 0.974400 / 4.64, 1e-6, "kNeon at 2 m facing the light: window^2 / (d^2 + near^2)");
  CheckNear(ll::LightScale(l, facing(1.0f, 1.0f)), (1.0 - std::pow(0.2, 4.0)) * (1.0 - std::pow(0.2, 4.0)) / 1.64, 1e-6, "and at 1 m");
  CheckNear(ll::LightScale(l, facing(1.0f, 0.0f)) / ll::LightScale(l, facing(1.0f, 1.0f)), ll::kNeonWrap / (1.0 + ll::kNeonWrap), 1e-6, "a surface edge-on to the light gets the wrap's share (0.3 / 1.3)");
  CheckNear(ll::LightScale(l, facing(1.0f, -ll::kNeonWrap)), 0.0, 1e-9, "a surface facing away by more than the wrap gets nothing");
  CheckNear(ll::LightScale(l, facing(1.0f, -1.0f)), 0.0, 1e-9, "and the far side of a body none");
  CheckNear(ll::LightScale(l, facing(1.0f, 0.5f), 0.25f), 0.25 * ll::LightScale(l, facing(1.0f, 0.5f)), 1e-7, "visibility is the last factor");
  // the window: 0 at the range and beyond, and flat there (no edge: the step at the reach is nothing, the slope is nothing)
  CheckNear(ll::LightScale(l, facing(5.0f, 1.0f)), 0.0, 0.0, "exactly 0 at the range");
  CheckNear(ll::LightScale(l, facing(7.0f, 1.0f)), 0.0, 0.0, "and beyond it");
  const double at_1m = ll::LightScale(l, facing(1.0f, 1.0f));
  const double just_in = ll::LightScale(l, facing(5.0f * (1.0f - 1.0e-3f), 1.0f));
  Check(just_in < 1.0e-4 * at_1m, Num("the value just inside the range is %.2e of the value at 1 m: no step at the reach", just_in / at_1m));
  double worst_step = 0.0, prev = ll::LightScale(l, facing(0.05f, 1.0f));
  bool monotone = true;
  for (int i = 1; i <= 1000; ++i) {
    const double v = ll::LightScale(l, facing(0.05f + 4.95f * float(i) / 1000.0f, 1.0f));
    monotone = monotone && v <= prev + 1e-12;
    worst_step = std::max(worst_step, std::fabs(v - prev));
    prev = v;
  }
  Check(monotone, "the term falls with the distance, everywhere out to the range");
  Check(worst_step < 0.02 * at_1m, Num("no step anywhere on the way: the largest step of 1000 along the axis is %.4f of the value at 1 m", worst_step / at_1m));
  // the slope at the range is 0 (a window of w^2 with w = 1 - (d/R)^4: both go to 0)
  const double slope = (ll::LightScale(l, facing(4.99f, 1.0f)) - ll::LightScale(l, facing(4.98f, 1.0f))) / 0.01;
  Check(std::fabs(slope) < 5.0e-3 * at_1m, Num("the slope at the range is nothing (%.2e of the value at 1 m a metre)", std::fabs(slope) / at_1m));
  // a light with no near radius, at the light itself, or past 0: nothing
  Light none = l;
  none.law_a = 0.0f;
  CheckNear(ll::LightScale(none, facing(1.0f, 1.0f)), 0.0, 0.0, "a near radius of 0 is no light (the law's parameter is missing)");
  ll::LightSurface at_light;
  at_light.position[1] = 0.5f;
  at_light.normal[1] = 1.0f;
  CheckNear(ll::LightScale(l, at_light), 0.0, 0.0, "a surface at the light's own place gets nothing (no direction)");
  // the dispatch: the law decides (the same numbers as a plain omni light are not the neon's)
  Light omni = l;
  omni.law = LightLaw::kOmni;
  omni.law_a = 3.0f;
  Check(std::fabs(ll::LightScale(omni, facing(2.0f, 1.0f)) - ll::LightScale(l, facing(2.0f, 1.0f))) > 1.0e-3, "LightScale follows the law: kNeon is not the game's plateau law");
  // the pack: law 3 survives the GpuLight's two bits and the unpack
  const GpuLight g = PackGpuLight(l);
  Check(UnpackGpuLight(g).law == LightLaw::kNeon, "a kNeon light keeps its law through the GpuLight row");
  // the street lamps' units: a street lamp (rgb x I = colour, term 1/I inside its plateau) gives its rgb inside 7.4 m of it; the neon's term for colour c is c x scale, in the same units
  Light lamp;
  lamp.pos[1] = 3.0f;
  lamp.color[0] = lamp.color[1] = lamp.color[2] = 55.0f;  // rgb (1, 1, 1) x I
  lamp.law = LightLaw::kOmni;
  lamp.law_a = 55.0f;
  lamp.range = 40.0f;
  lamp.kind = LightKind::kPoint;
  lamp.cos_outer = lamp.cos_inner = -1.0f;
  ll::LightSurface under;
  under.normal[1] = 1.0f;
  CheckNear(double(lamp.color[0]) * double(ll::LightScale(lamp, under)), 1.0, 1e-6, "a street lamp's colour x term is its rgb inside the plateau, 1.0 under it (the unit the neon's intensity is in)");
}

void HeadlightLawChecks() {
  // the rig's term, bit for bit, over random lights, surfaces and visibilities
  Lcg rng;
  int bad = 0;
  double worst = 0.0;
  int lit = 0;
  for (int i = 0; i < 100000; ++i) {
    Light l;
    for (int k = 0; k < 3; ++k) {
      l.pos[k] = rng.Range(-5.0f, 5.0f);
      l.color[k] = 1.0f;
    }
    rng.Direction(l.dir);
    l.range = rng.Range(20.0f, 80.0f);
    l.cos_outer = rng.Range(0.3f, 0.9f);
    l.cos_inner = std::min(1.0f, l.cos_outer + rng.Range(0.01f, 0.2f));
    l.law = LightLaw::kHeadlight;
    l.law_a = ll::kHeadlightFalloffDistance;
    ll::LightSurface s;
    for (int k = 0; k < 3; ++k) {
      s.position[k] = l.pos[k] + l.dir[k] * rng.Range(0.0f, 60.0f) + rng.Range(-10.0f, 10.0f);
    }
    rng.Direction(s.normal);
    const float visibility = rng.Next();
    const hr::Lamp lamp = RigLamp(l);
    hr::Surface surface;
    std::memcpy(surface.position, s.position, 12);
    std::memcpy(surface.normal, s.normal, 12);
    const float want = hr::LampScale(lamp, surface, visibility);
    const float got = ll::HeadlightScale(l, s, visibility);
    worst = std::max(worst, double(std::fabs(got - want)));
    bad += got == want ? 0 : 1;
    lit += want > 0.0f ? 1 : 0;
  }
  Check(bad == 0 && lit > 5000, Num("HeadlightScale equals headlight_rules::LampScale on 100000 random cases (%.0f differ, %.0f lit)", double(bad), double(lit)));
  CheckNear(worst, 0.0, 0.0, "and to the last bit");

  // the calibration table of headlight_rig_test.cpp CheckCalibration: the road ahead of a low beam, through a Light made by LightFromHeadlight
  mcla::gpu::Headlight h{};
  h.pos[1] = 0.75f;
  const float dip = hr::kDipDegrees * float(hr::kPi) / 180.0f;
  h.dir[0] = std::cos(dip);
  h.dir[1] = -std::sin(dip);
  h.dir[2] = 0.0f;
  h.cos_outer = hr::CosOfDegrees(hr::kOuterDegrees);
  h.cos_inner = hr::CosOfDegrees(hr::kInnerDegrees);
  h.range = hr::kRange;
  h.color[0] = h.color[1] = h.color[2] = 1.0f;
  h.car_key = 9;
  h.is_player = true;
  const Light l = LightFromHeadlight(h);
  const auto road = [&](float d) {
    ll::LightSurface s;
    s.position[0] = d;
    s.normal[1] = 1.0f;
    return ll::LightScale(l, s);
  };
  struct Want {
    float d, lo, hi;
  };
  const Want table[] = {{5.0f, 0.85f, 1.20f}, {10.0f, 0.70f, 0.95f}, {15.0f, 0.55f, 0.78f}, {20.0f, 0.40f, 0.62f},
                        {25.0f, 0.30f, 0.45f}, {30.0f, 0.20f, 0.33f}, {40.0f, 0.08f, 0.17f}, {50.0f, 0.02f, 0.08f}};
  for (const Want& w : table) {
    const float got = road(w.d);
    Check(got >= w.lo && got <= w.hi, "the road " + std::to_string(int(w.d)) + " m ahead of a low beam: " + std::to_string(got) + " in [" + std::to_string(w.lo) + ", " + std::to_string(w.hi) + "]");
  }
  CheckNear(road(hr::kRange), 0.0, 1e-9, "zero at the reach");
  // the law's parameter: a light with D = 13 m falls off twice as fast as D = 26 m (a half at 13 m)
  Light narrow = l;
  narrow.law_a = 13.0f;
  Check(road(20.0f) > ll::LightScale(narrow, [] { ll::LightSurface s; s.position[0] = 20.0f; s.normal[1] = 1.0f; return s; }()), "a smaller D makes the beam fall off sooner");
  Light default_d = l;
  default_d.law_a = 0.0f;
  CheckNear(ll::LightScale(default_d, [] { ll::LightSurface s; s.position[0] = 20.0f; s.normal[1] = 1.0f; return s; }()), double(road(20.0f)), 1e-9, "law_a 0 means the rig's D");
}

void VolumeFormChecks() {
  // the volume's lamp is a PROJECTION of the light: its term at a distance is VolLampTerm of ToVolLamp, for a lamp and for a headlight
  Lcg rng;
  int bad = 0;
  for (int i = 0; i < 2000; ++i) {
    const auto g = StreetLamp(0.0f, 0.0f, rng.Range(20.0f, 100.0f));
    Light l = LightFromGameLamp(g);
    l.vol_weight = rng.Next();
    const vol::VolLamp v = ToVolLamp<vol::VolLamp>(l);
    const float dist = rng.Range(0.1f, 130.0f);
    const float a = ll::VolumeTerm(l, dist);
    const float b = vol::VolLampTerm(v, dist);
    bad += std::fabs(a - b) <= 1e-5f * std::max(1.0f, b) ? 0 : 1;
    // and a headlight
    mcla::gpu::Headlight h{};
    h.range = rng.Range(40.0f, 80.0f);
    for (int k = 0; k < 3; ++k) {
      h.color[k] = rng.Range(0.1f, 1.0f);
    }
    h.cos_outer = 0.79f;
    h.cos_inner = 0.97f;
    Light hl = LightFromHeadlight(h);
    hl.vol_weight = rng.Next();
    const vol::VolLamp hv = ToVolLamp<vol::VolLamp>(hl);
    const float hd = rng.Range(0.1f, 90.0f);
    const float c = ll::VolumeTerm(hl, hd);
    const float d = vol::VolLampTerm(hv, hd);
    bad += std::fabs(c - d) <= 1e-5f * std::max(1.0f, d) ? 0 : 1;
  }
  Check(bad == 0, "VolumeTerm of a Light is VolLampTerm of its VolLamp (lamps and headlights, 2000 each)");
  // "the beam in the air and the pool on the road are the same light": a headlight's volume term on its axis against its lit term on a wall facing it, near the car
  mcla::gpu::Headlight h{};
  h.dir[0] = 1.0f;
  h.cos_outer = hr::CosOfDegrees(hr::kOuterDegrees);
  h.cos_inner = hr::CosOfDegrees(hr::kInnerDegrees);
  h.range = hr::kRange;
  h.color[0] = h.color[1] = h.color[2] = 1.0f;
  const Light l = LightFromHeadlight(h);
  for (const float d : {3.0f, 5.0f, 8.0f, 10.0f}) {
    ll::LightSurface wall;
    wall.position[0] = d;
    wall.normal[0] = -1.0f;
    wall.normal[1] = 0.0f;
    const double lit = double(ll::LightScale(l, wall));
    const double air = double(ll::VolumeTerm(l, d));
    // both are kPeak/(1 + (d/D)^2) times a window: the volume's quartic one and the lit shaders' squared one differ by a few per cent this near the lamp
    CheckNear(air / lit, 1.0, 0.06, Num("a headlight's volume term and its lit term agree to 6 per cent at %.0f m (ratio %.4f)", double(d), air / lit));
  }
}

// ---------------------------------------------------------------------------
// the conversions
// ---------------------------------------------------------------------------

struct HeadlightWithOffset : mcla::gpu::Headlight {
  float lamp_offset[3] = {0.0f, 0.0f, 0.0f};  // as on feat/hl-per-lamp
};

void HeadlightConversionChecks() {
  mcla::gpu::Headlight h{};
  const float pos[3] = {1, 2, 3}, dir[3] = {0, 0, 1}, color[3] = {1, 0.9f, 0.7f};
  std::memcpy(h.pos, pos, 12);
  std::memcpy(h.dir, dir, 12);
  std::memcpy(h.color, color, 12);
  h.cos_outer = 0.8f;
  h.cos_inner = 0.9f;
  h.range = 40.0f;
  h.car_key = 77;
  h.is_player = true;
  const Light l = LightFromHeadlight(h);
  Check(l.pos[1] == 2.0f && l.dir[2] == 1.0f && l.cos_outer == 0.8f && l.cos_inner == 0.9f && l.range == 40.0f && l.color[1] == 0.9f && l.car_key == 77, "a headlight's fields are the light's");
  Check(l.kind == LightKind::kSpot && l.source == LightSource::kHeadlight && l.law == LightLaw::kHeadlight && l.law_a == ll::kHeadlightFalloffDistance, "a spot of source and law headlight with the rig's D");
  Check(l.flags == kLightFlagPlayer && l.roles == kRoleMaskAll && l.shadow.kind == ShadowKind::kNone && l.id == lid::CarLampId(77), "the player flag, every role, no shadow, the car's pair id");
  h.is_player = false;
  Check(LightFromHeadlight(h).flags == 0u, "another car's lamp has no flag");
  HeadlightWithOffset ho{};
  std::memcpy(ho.pos, pos, 12);
  ho.lamp_offset[0] = 0.6f;
  ho.lamp_offset[2] = -0.1f;
  const Light lo = LightFromHeadlight(ho);
  Check(lo.offset[0] == 0.6f && lo.offset[2] == -0.1f && l.offset[0] == 0.0f, "lamp_offset (feat/hl-per-lamp) becomes the offset where the type has it");

  // into the volume's lamp: what FillHeadlights + ApplyHeadlightUnits make today, field for field
  Lcg rng;
  int bad = 0;
  for (int i = 0; i < 5000; ++i) {
    mcla::gpu::Headlight r{};
    for (int k = 0; k < 3; ++k) {
      r.pos[k] = rng.Range(-100.0f, 100.0f);
      r.color[k] = rng.Range(0.0f, 1.2f);
    }
    rng.Direction(r.dir);
    r.cos_outer = rng.Range(0.5f, 0.9f);
    r.cos_inner = rng.Range(0.91f, 0.99f);
    r.range = rng.Range(30.0f, 90.0f);
    r.car_key = rng.Below(100);
    const vol::VolLamp legacy = [&] {
      vol::VolLamp v = vol::LampFromHeadlight(r);
      vol::ApplyHeadlightUnits(&v);
      return v;
    }();
    const vol::VolLamp mine = ToVolLamp<vol::VolLamp>(LightFromHeadlight(r));
    const bool same = std::memcmp(legacy.pos, mine.pos, 12) == 0 && legacy.range == mine.range && std::memcmp(legacy.dir, mine.dir, 12) == 0 && legacy.cos_outer == mine.cos_outer &&
                      std::memcmp(legacy.colour, mine.colour, 12) == 0 && legacy.cos_inner == mine.cos_inner && legacy.shadow_view == mine.shadow_view && legacy.pad[0] == mine.pad[0];
    bad += same ? 0 : 1;
  }
  Check(bad == 0, "ToVolLamp(LightFromHeadlight(h)) is LampFromHeadlight + ApplyHeadlightUnits, bit for bit (5000 headlights)");
}

void GameLampConversionChecks() {
  Lcg rng;
  int bad = 0, points = 0, spots = 0;
  for (int i = 0; i < 20000; ++i) {
    mcla::gpu::vol_lamps::GameLamp g;
    for (int k = 0; k < 3; ++k) {
      g.pos[k] = rng.Range(-3000.0f, 3000.0f);
      g.colour[k] = rng.Range(-0.1f, 1.0f);
      g.dir[k] = rng.Next() < 0.1f ? 0.0f : rng.Range(-3.0f, 3.0f);
    }
    g.intensity = rng.Next() < 0.05f ? rng.Range(-5.0f, 0.5f) : rng.Range(5.0f, 400.0f);
    g.cone_deg = rng.Next() < 0.15f ? 360.0f : rng.Range(10.0f, 200.0f);
    g.spread_deg = rng.Next() < 0.2f ? 0.0f : rng.Range(-70.0f, 70.0f);
    g.scale = rng.Range(-0.2f, 1.2f);
    g.type = rng.Next() < 0.2f ? 0 : 1;
    const float units = rng.Next() < 0.5f ? 1.0f : rng.Range(0.5f, 3.0f);
    const vol::VolLamp legacy = vol::MakeVolLamp(g, units);
    const Light l = LightFromGameLamp(g);
    const vol::VolLamp mine = ToVolLamp<vol::VolLamp>(l, units);
    const bool same = std::memcmp(legacy.pos, mine.pos, 12) == 0 && legacy.range == mine.range && std::memcmp(legacy.dir, mine.dir, 12) == 0 && legacy.cos_outer == mine.cos_outer &&
                      std::memcmp(legacy.colour, mine.colour, 12) == 0 && legacy.cos_inner == mine.cos_inner && legacy.shadow_view == mine.shadow_view &&
                      legacy.pad[0] == mine.pad[0];
    bad += same ? 0 : 1;
    (l.kind == LightKind::kPoint ? points : spots) += 1;
    // a lamp's volume weight is applied as the picker does: after the units
    Light weighted = l;
    weighted.vol_weight = 0.37f;
    const vol::VolLamp wv = ToVolLamp<vol::VolLamp>(weighted, units);
    bad += (wv.colour[0] == legacy.colour[0] * 0.37f && wv.colour[2] == legacy.colour[2] * 0.37f) ? 0 : 1;
    // the reverse: the light a volume lamp stands for
    const Light back = LightFromVolLamp(mine, l.law, l.source);
    const bool close = std::memcmp(back.pos, l.pos, 12) == 0 && back.range == l.range && std::memcmp(back.dir, l.dir, 12) == 0 && back.cos_outer == l.cos_outer &&
                       back.cos_inner == l.cos_inner && back.kind == l.kind && back.law == l.law &&
                       (l.law_a < 2.0f || std::fabs(back.law_a - l.law_a) <= 1e-4f * l.law_a) && std::fabs(back.color[0] - l.color[0] * units) <= 1e-3f * std::max(1.0f, l.color[0] * units);
    bad += close ? 0 : 1;
  }
  Check(bad == 0, "ToVolLamp(LightFromGameLamp(g), units) is MakeVolLamp(g, units), bit for bit; the weight comes last; the reverse finds the light (20000 lamps)");
  Check(points > 1000 && spots > 10000, "the sweep had omni lamps and spots");
  // an omni list entry
  auto omni = StreetLamp(1.0f, 2.0f, 40.0f);
  omni.type = 0;
  const Light lo = LightFromGameLamp(omni);
  Check(lo.kind == LightKind::kPoint && lo.law == LightLaw::kOmni && lo.cos_outer == -1.0f && lo.cos_inner == -1.0f, "an omni lamp is a point light of law kOmni");
  // a mod's lamp
  struct ModLight {
    float pos[3], colour[3], dir[3], intensity, cone_deg, spread_deg;
    uint32_t id;
    uint8_t type, flags;
  };
  ModLight m{{5, 1, 5}, {1, 1, 1}, {0, -1, 0}, 20.0f, 100.0f, -40.0f, 12, 1, 1};
  const Light lm = LightFromModLight(m, lid::ModKey("mymod"));
  Check(lm.source == LightSource::kMod && lm.id == lid::ModLampId(lid::ModKey("mymod"), 12) && lm.id != lid::LampId(m.pos), "a mod's lamp has the mod's id");
  CheckNear(lm.range, 44.72, 0.01, "a lamp of 20 reaches 44.7 m (mod_lights.cpp)");
  Check(lm.law == LightLaw::kGameLamp && (lm.flags & kLightFlagGameLit) != 0, "and the game's law");
  // police and neon
  const float pp[3] = {0, 1, 0}, pc[3] = {1, 0, 0};
  const Light lp = MakeOmniLight(LightSource::kPolice, pp, pc, 0.5f, 6.0f, 20.0f, lid::PointId(0x82345678u, 2));
  Check(lp.law == LightLaw::kOmni && lp.kind == LightKind::kPoint && lp.color[0] == 0.5f && lp.law_a == 6.0f && lp.range == 20.0f && lp.roles == kRoleMaskAll && lp.flags == 0u,
        "a police point is an omni light the game does not light with");
}

void ShadowConversionChecks() {
  // a LocalShadowView is the volume's VolShadowView, byte for byte
  LocalShadowView v{};
  for (int k = 0; k < 16; ++k) {
    v.world_to_shadow[k] = float(k) * 1.5f - 3.0f;
  }
  v.atlas_rect[0] = 0.0f;
  v.atlas_rect[1] = 0.25f;
  v.atlas_rect[2] = 0.5f;
  v.atlas_rect[3] = 0.75f;
  v.depth_bias = 0.0007f;
  v.texel_size = 0.002f;
  v.light_index = 3;
  v.valid = 1;
  const vol::VolShadowView vv = ToVolShadowView<vol::VolShadowView>(v);
  Check(std::memcmp(&vv, &v, sizeof(v)) == 0, "ToVolShadowView is the 96 bytes of the LocalShadowView");

  // the replay's frame as a ShadowFrame: grouped by light
  larecomp::gpu::LocalShadowFrame local;
  const uint32_t light_of[4] = {1, 0, 1, 0};
  for (int i = 0; i < 4; ++i) {
    local.views[i] = v;
    local.views[i].light_index = light_of[i];
    local.views[i].valid = i == 3 ? 0u : 1u;
    local.views[i].depth_bias = float(i + 1);  // a mark of which view is which
  }
  local.count = 4;
  const uint64_t ids[2] = {lid::CarLampId(10), lid::CarLampId(20)};
  ShadowAtlasInfo atlas;
  atlas.valid = true;
  atlas.slot = 7;
  atlas.width = 2048;
  atlas.height = 1024;
  const ShadowFrame sf = ShadowFrameFromLocal(local, atlas, ids, 2);
  Check(sf.count == 4 && sf.atlas.slot == 7 && sf.atlas.width == 2048, "all four views and the atlas are carried");
  Check(sf.view_light[0] == ids[1] && sf.view_light[1] == ids[1] && sf.view_light[2] == ids[0] && sf.view_light[3] == kNoLightId, "the views are grouped by light, in order of first appearance, the one that counts for nobody last");
  Check(sf.views[0].depth_bias == 1.0f && sf.views[1].depth_bias == 3.0f && sf.views[2].depth_bias == 2.0f && sf.views[3].depth_bias == 4.0f, "the views keep their numbers");
  Check(sf.views[0].light_index == 1 && sf.views[2].light_index == 0, "LocalShadowView::light_index stays the rig's index");
  Check(sf.view_layer[0] == ShadowLayer::kSingle && sf.view_layer[2] == ShadowLayer::kSingle, "views of the replay are singles");
  // a light's refs from them
  LightFrame frame;
  frame.shadow = sf;
  frame.count = 3;
  frame.lights[0].id = ids[0];
  frame.lights[1].id = ids[1];
  frame.lights[2].id = lid::CarLampId(30);
  BindShadowRefs(&frame);
  Check(frame.lights[1].shadow.kind == ShadowKind::kAtlas && frame.lights[1].shadow.first_view == 0 && frame.lights[1].shadow.view_count == 2, "the car with two views names entries 0 and 1");
  Check(frame.lights[0].shadow.kind == ShadowKind::kAtlas && frame.lights[0].shadow.first_view == 2 && frame.lights[0].shadow.view_count == 1, "the car with one valid view names entry 2");
  Check(frame.lights[2].shadow.kind == ShadowKind::kNone && frame.lights[2].shadow.view_count == 0, "a light with no view is unshadowed");
  // and into the volume's lamp: the first view, and the second
  const vol::VolLamp two = ToVolLamp<vol::VolLamp>(frame.lights[1]);
  const vol::VolLamp one = ToVolLamp<vol::VolLamp>(frame.lights[0]);
  const vol::VolLamp none = ToVolLamp<vol::VolLamp>(frame.lights[2]);
  Check(two.shadow_view == 0 && one.shadow_view == 2 && none.shadow_view == -1, "ToVolLamp names the light's first view, -1 for none");
  // the one-view cap: a third valid view of a light is not named
  larecomp::gpu::LocalShadowFrame three;
  for (int i = 0; i < 3; ++i) {
    three.views[i] = v;
    three.views[i].light_index = 0;
    three.views[i].valid = 1;
  }
  three.count = 3;
  const ShadowFrame s3 = ShadowFrameFromLocal(three, atlas, ids, 2);
  LightFrame f3;
  f3.shadow = s3;
  f3.count = 1;
  f3.lights[0].id = ids[0];
  BindShadowRefs(&f3);
  Check(f3.lights[0].shadow.view_count == 2 && s3.view_light[2] == kNoLightId, "at most two views are a light's");
}

void CombineShadowChecks() {
  ShadowFrame sf;
  sf.count = 4;
  for (int i = 0; i < 4; ++i) {
    sf.views[i].valid = 1;
  }
  ShadowRef ref;
  ref.kind = ShadowKind::kAtlas;
  ref.first_view = 0;
  ref.view_count = 2;
  ref.weight = 1.0f;
  float vis[kMaxLightShadowViews] = {0.2f, 0.8f, 0.5f, 0.5f, 1, 1, 1, 1};
  CheckNear(ll::CombineShadow(sf, ref, vis), 0.5, 1e-6, "two views of a light are averaged");
  ref.weight = 0.5f;
  CheckNear(ll::CombineShadow(sf, ref, vis), 0.75, 1e-6, "and blended by the weight: lit = lerp(1, visibility, weight)");
  ref.weight = 0.0f;
  CheckNear(ll::CombineShadow(sf, ref, vis), 1.0, 1e-6, "a weight of 0 is no shadow");
  // layers: a static entry and its live twin are ONE view whose visibility is the product
  sf.view_layer[2] = ShadowLayer::kStatic;
  sf.view_layer[3] = ShadowLayer::kLive;
  ShadowRef lamp;
  lamp.kind = ShadowKind::kAtlas;
  lamp.first_view = 2;
  lamp.view_count = 2;
  lamp.weight = 1.0f;
  CheckNear(ll::CombineShadow(sf, lamp, vis), 0.25, 1e-6, "the layers of a view are multiplied: 0.5 x 0.5");
  // an invalid view says nothing: it is not counted
  sf.views[1].valid = 0;
  ref.weight = 1.0f;
  CheckNear(ll::CombineShadow(sf, ref, vis), 0.2, 1e-6, "an invalid view is left out of the mean");
  sf.views[0].valid = 0;
  CheckNear(ll::CombineShadow(sf, ref, vis), 1.0, 1e-6, "a light whose views are all invalid is lit");
  // an invalid static layer takes its live twin with it
  sf.views[2].valid = 0;
  CheckNear(ll::CombineShadow(sf, lamp, vis), 1.0, 1e-6, "a pair whose static entry is invalid counts for nothing");
  ShadowRef none;
  CheckNear(ll::CombineShadow(sf, none, vis), 1.0, 1e-9, "an unshadowed light is lit");
  none.kind = ShadowKind::kRtMask;
  none.view_count = 2;
  CheckNear(ll::CombineShadow(sf, none, vis), 1.0, 1e-9, "the ray traced mask is not an atlas rule");
}

void CarLightChecks() {
  CarLights car;
  Check(NoteCarPoint(&car, kPointKindHeadlight, 2.0f) && (car.bits & kCarLightHead) != 0 && car.level[kCarLevelHead] == 1.0f, "a headlight point at 2.0 is on, level 1");
  Check(NoteCarPoint(&car, kPointKindBrake, 1.0f) && (car.bits & kCarLightBrake) != 0 && car.level[kCarLevelBrake] == 0.5f, "a brake light at 1.0 is half");
  Check(NoteCarPoint(&car, kPointKindTail, 0.0f) && (car.bits & kCarLightTail) == 0 && car.level[kCarLevelTailTurn] == 0.0f, "a point at 0 is off");
  Check(NoteCarPoint(&car, kPointKindHiBeam, 0.04f) && (car.bits & kCarLightHiBeam) == 0, "and so is one at or under the off threshold");
  Check(NoteCarPoint(&car, kPointKindHiBeam, 3.0f) && (car.bits & kCarLightHiBeam) != 0 && car.level[kCarLevelHead] == 1.0f, "the level is capped at 1 and shared by the headlight family");
  Check(!NoteCarPoint(&car, 6, 2.0f) && !NoteCarPoint(&car, 2, 2.0f), "a kind the probe has not read is not noted");
  Check(kPointOffIntensity == mcla::gpu::headlight_points::kOffIntensity && kPointFullIntensity == mcla::gpu::headlight_points::kFullIntensity &&
            kPointKindHeadlight == mcla::gpu::headlight_points::kHeadlightKind,
        "the point numbers are headlight_points.h's");
  uint32_t seen = 0;
  for (const uint32_t bit : {kCarLightHead, kCarLightHiBeam, kCarLightFog, kCarLightBrake, kCarLightTail, kCarLightTurnL, kCarLightTurnR, kCarLightReverse, kCarLightPoliceA,
                             kCarLightPoliceB, kCarLightPoliceC}) {
    seen |= bit;
  }
  Check(seen == 0x7FFu, "eleven distinct bits");
}

void FrameChecks() {
  LightFrame f;
  f.count = 3;
  f.lights[0].id = 11;
  f.lights[1].id = 22;
  f.lights[2].id = 33;
  f.order[uint32_t(LightRole::kVolume)] = {};
  f.order[uint32_t(LightRole::kVolume)][0] = 2;
  f.order[uint32_t(LightRole::kVolume)][1] = 0;
  f.role_count[uint32_t(LightRole::kVolume)] = 2;
  Check(f.Find(22) == 1 && f.Find(33) == 2 && f.Find(44) == -1 && f.Find(kNoLightId) == -1, "Find looks up by id, never by 0");
  Check(f.Ranked(LightRole::kVolume, 0).id == 33 && f.Ranked(LightRole::kVolume, 1).id == 11 && f.RoleCount(LightRole::kVolume) == 2, "a role's ranking is a prefix of slots, best first");
  const LightFrame copy = f;
  Check(copy.count == 3 && copy.Find(33) == 2 && copy.Ranked(LightRole::kVolume, 0).id == 33, "a frame copies as plain data");
}

// ---------------------------------------------------------------------------
// the HLSL twin
// ---------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void HlslDefinesChecks(const std::string& text) {
  std::map<std::string, std::string> defines;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("#define LL_", 0) != 0) continue;
    std::istringstream one(line);
    std::string define, name, value;
    one >> define >> name >> value;
    defines[name] = value;
  }
  const std::map<std::string, double> ints = {
      {"LL_MAX_LIGHTS", double(kMaxLights)},
      {"LL_BUFFER_HEADER_BYTES", double(sizeof(GpuLightHeader))},
      {"LL_LIGHT_BYTES", double(sizeof(GpuLight))},
      {"LL_KIND_SPOT", double(LightKind::kSpot)},
      {"LL_KIND_POINT", double(LightKind::kPoint)},
      {"LL_SOURCE_HEADLIGHT", double(LightSource::kHeadlight)},
      {"LL_SOURCE_LAMP", double(LightSource::kLamp)},
      {"LL_SOURCE_NEON", double(LightSource::kNeon)},
      {"LL_SOURCE_POLICE", double(LightSource::kPolice)},
      {"LL_SOURCE_MOD", double(LightSource::kMod)},
      {"LL_SOURCE_OTHER", double(LightSource::kOther)},
      {"LL_LAW_HEADLIGHT", double(LightLaw::kHeadlight)},
      {"LL_LAW_GAME_LAMP", double(LightLaw::kGameLamp)},
      {"LL_LAW_OMNI", double(LightLaw::kOmni)},
      {"LL_LAW_NEON", double(LightLaw::kNeon)},
      {"LL_SHADOW_NONE", double(ShadowKind::kNone)},
      {"LL_SHADOW_ATLAS", double(ShadowKind::kAtlas)},
      {"LL_SHADOW_RT_MASK", double(ShadowKind::kRtMask)},
      {"LL_ROLE_SURFACE", double(kRoleMaskSurface)},
      {"LL_ROLE_VOLUME", double(kRoleMaskVolume)},
      {"LL_ROLE_SHADOW", double(kRoleMaskShadow)},
      {"LL_ROLE_RT", double(kRoleMaskRt)},
      {"LL_FLAG_PLAYER", double(kLightFlagPlayer)},
      {"LL_FLAG_LIGHTS_GEOMETRY", double(kLightFlagLightsGeometry)},
      {"LL_FLAG_GAME_LIT", double(kLightFlagGameLit)},
      {"LL_FLAG_RT_VISIBILITY", double(kLightFlagRtVisibility)},
      {"LL_KIND_SHIFT", double(kGpuKindShift)},
      {"LL_KIND_MASK", double(kGpuKindMask)},
      {"LL_SOURCE_SHIFT", double(kGpuSourceShift)},
      {"LL_SOURCE_MASK", double(kGpuSourceMask)},
      {"LL_LAW_SHIFT", double(kGpuLawShift)},
      {"LL_LAW_MASK", double(kGpuLawMask)},
      {"LL_ROLES_SHIFT", double(kGpuRolesShift)},
      {"LL_ROLES_MASK", double(kGpuRolesMask)},
      {"LL_FLAGS_SHIFT", double(kGpuFlagsShift)},
      {"LL_FLAGS_MASK", double(kGpuFlagsMask)},
      {"LL_FIRST_VIEW_SHIFT", double(kGpuFirstViewShift)},
      {"LL_VIEW_COUNT_SHIFT", double(kGpuViewCountShift)},
      {"LL_SHADOW_KIND_SHIFT", double(kGpuShadowKindShift)},
      {"LL_MASK_CHANNEL_SHIFT", double(kGpuMaskChannelShift)},
      {"LL_HEADLIGHT_PEAK", double(ll::kHeadlightPeak)},
      {"LL_HEADLIGHT_FALLOFF_DISTANCE", double(ll::kHeadlightFalloffDistance)},
      {"LL_SURFACE_WRAP", double(ll::kSurfaceWrap)},
      {"LL_CUT_LOW", double(ll::kCutLow)},
      {"LL_CUT_HIGH", double(ll::kCutHigh)},
      {"LL_NEAR_START", double(ll::kNearStart)},
      {"LL_NEAR_SPAN", double(ll::kNearSpan)},
      {"LL_NEON_WRAP", double(ll::kNeonWrap)},
  };
  Check(defines.size() == ints.size(), Num("the .hlsli has %.0f LL_ defines (the test knows %.0f)", double(defines.size()), double(ints.size())));
  for (const auto& [name, want] : ints) {
    const auto it = defines.find(name);
    const double got = it == defines.end() ? -1e30 : std::strtod(it->second.c_str(), nullptr);  // "2.0f" parses as 2.0
    Check(it != defines.end() && std::fabs(got - want) <= 1e-6, name + " is the C++ value");
  }
}

void HlslAgainstTwinChecks() {
  using namespace hlsl_shim;
  Lcg rng;
  int unpack_bad = 0, scale_bad = 0, lit = 0;
  double worst = 0.0;
  for (int i = 0; i < 200000; ++i) {
    Light l = RandomLight(rng, uint32_t(i));
    if (l.law == LightLaw::kHeadlight) {
      l.range = rng.Range(20.0f, 90.0f);
      l.cos_outer = rng.Range(0.3f, 0.9f);
      l.cos_inner = std::min(1.0f, l.cos_outer + 0.1f);
      l.law_a = rng.Next() < 0.2f ? 0.0f : ll::kHeadlightFalloffDistance;
    } else {
      l.range = ll::LampRange(l.law_a);
    }
    const GpuLight g = PackGpuLight(l);
    LlGpuLight hg;
    std::memcpy(&hg, &g, sizeof(g));
    const LlLight h = LlUnpack(hg);
    const Light c = UnpackGpuLight(g);
    const bool same = h.pos.x == c.pos[0] && h.pos.y == c.pos[1] && h.pos.z == c.pos[2] && h.range == c.range && h.dir.x == c.dir[0] && h.dir.z == c.dir[2] &&
                      h.cos_outer == c.cos_outer && h.color.y == c.color[1] && h.cos_inner == c.cos_inner && h.law_a == c.law_a && h.shadow_weight == c.shadow.weight &&
                      h.kind == uint32_t(c.kind) && h.source == uint32_t(c.source) && h.law == uint32_t(c.law) && h.roles == c.roles && h.flags == c.flags &&
                      h.first_view == c.shadow.first_view && h.view_count == c.shadow.view_count && h.shadow_kind == uint32_t(c.shadow.kind) &&
                      h.mask_channel == c.shadow.mask_channel && LlHasRole(h, LL_ROLE_SHADOW) == ((c.roles & kRoleMaskShadow) != 0) &&
                      LlIsGameLit(h) == ((c.flags & kLightFlagGameLit) != 0);
    unpack_bad += same ? 0 : 1;
    // a surface near the light, a normal, a visibility
    ll::LightSurface s;
    for (int k = 0; k < 3; ++k) {
      s.position[k] = c.pos[k] + c.dir[k] * rng.Range(0.0f, 1.0f) * c.range * 0.9f + rng.Range(-8.0f, 8.0f);
    }
    rng.Direction(s.normal);
    const float visibility = rng.Next();
    const float want = ll::LightScale(c, s, visibility);
    const float got = LlLightScale(h, float3(s.position[0], s.position[1], s.position[2]), float3(s.normal[0], s.normal[1], s.normal[2]), visibility);
    const float diff = std::fabs(got - want);
    worst = std::max(worst, double(diff / std::max(1e-3f, std::fabs(want))));
    scale_bad += diff <= 1e-5f * std::max(1.0f, std::fabs(want) * 100.0f) ? 0 : 1;
    lit += want > 0.0f ? 1 : 0;
    // the colour too
    const float3 term = LlLightTerm(h, float3(s.position[0], s.position[1], s.position[2]), float3(s.normal[0], s.normal[1], s.normal[2]), visibility);
    float cterm[3];
    ll::LightTerm(c, s, visibility, cterm);
    scale_bad += (std::fabs(term.x - cterm[0]) <= 1e-4f * std::max(1.0f, std::fabs(cterm[0])) && std::fabs(term.z - cterm[2]) <= 1e-4f * std::max(1.0f, std::fabs(cterm[2]))) ? 0 : 1;
  }
  Check(unpack_bad == 0, "LlUnpack of a GpuLight row is UnpackGpuLight (200000 random lights)");
  Check(scale_bad == 0 && lit > 20000, Num("LlLightScale and LlLightTerm equal the C++ laws (%.0f differ, %.0f of 200000 lit, worst relative %.2e)", double(scale_bad), double(lit), worst));
}

}  // namespace

int main(int argc, char** argv) {
  std::string hlsli = argc > 1 ? argv[1] : std::string();
  if (hlsli.empty()) {
    std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    hlsli = (slash == std::string::npos ? std::string() : here.substr(0, slash + 1)) + "hlsl/light_laws.hlsli";
  }
  LayoutChecks();
  GpuLightChecks();
  IdChecks();
  ConstantChecks();
  GameLampLawChecks();
  GameLampLightChecks();
  NeonLawChecks();
  HeadlightLawChecks();
  VolumeFormChecks();
  HeadlightConversionChecks();
  GameLampConversionChecks();
  ShadowConversionChecks();
  CombineShadowChecks();
  CarLightChecks();
  FrameChecks();
  const std::string text = ReadFile(hlsli);
  Check(!text.empty(), "the .hlsli is readable: " + hlsli);
  if (!text.empty()) HlslDefinesChecks(text);
  HlslAgainstTwinChecks();
  std::printf("light_frame_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

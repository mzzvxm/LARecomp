// Check of the deferred local light pass's rules (local_lights_rules.h, hlsl/local_light.hlsli; perf/LIGHT-UNIFY.md 6.2 and 11.2, package GB-H1): the layout of the buffer the shader reads and of
// its push block, the camera's rows and a pixel's world position (held to a projection back to the pixel), the units (the game's night light intensity and height factor) and the light one
// light adds to a pixel (held to the one laws' LightTerm and to the game's own lamp numbers: the lamp A/B's CPU half), which lights the pass takes (the frame's, the A/B's mod lamps, the test
// lamp, the budget), the scissor of a light (every pixel a light can reach is inside it), the votes that pick the units from the draws' constants, and the HLSL twin compiled as C++ and compared
// with the C++ one. With the `src` folder it also reads the sources: the shaders' CMake lines and blobs, the settings, the A/B's hook in mod_lights.cpp, the hooks of the Scene and the shader's
// bindings. A program of its own: no renderer, no ReXGlue, no GPU, no game; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_local_lights_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_local_lights_test [<the src folder>]     (found from this file's place without an argument)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "local_lights_rules.h"
#include "local_lights_sources.h"
#include "mod_light_source.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the files use, in a namespace of its own so that the standard library's abs, sqrt ... are not in the way
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
inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float3 operator*(float3 a, float3 b) { return float3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
#include "hlsl/local_light.hlsli"
}  // namespace hlsl_shim

namespace ll = larecomp::gpu::light_laws;
namespace lp = larecomp::gpu::local_lights;
namespace mls = larecomp::gpu::mod_light_source;
using namespace larecomp::gpu;

// GB-NEON-BLEED: the seam of hlsl/local_light.hlsli LpContactVisibility reads the check's own depth field.
std::function<float(int, int)> g_stored_depth;
namespace hlsl_shim {
float LpStoredDepth(int px, int py) { return g_stored_depth(px, py); }
}  // namespace hlsl_shim

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
// relative closeness with an absolute floor
void CheckRel(double got, double want, double rel, double floor_abs, const std::string& what) {
  ++g_checks;
  const double tol = std::max(floor_abs, rel * std::fabs(want));
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

struct Lcg {
  uint32_t state = 20261120u;
  float Next() {  // 0 to 1
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return uint32_t(Next() * float(n)) % n; }
  void Direction(float out[3]) {
    const float z = Range(-1.0f, 1.0f);
    const float a = Range(0.0f, 6.2831853f);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    out[0] = r * std::cos(a);
    out[1] = z;
    out[2] = r * std::sin(a);
  }
};

constexpr double kDeg = 3.14159265358979323846 / 180.0;

// A random camera: a unit frame (right, up, back by the game's view inverse: row 0 right, row 1 up, row 2 back, row 3 position) from a random forward and a roll-free up.
void RandomPose(Lcg& rng, float view_inverse[16], lp::Rows* rows) {
  float forward[3];
  rng.Direction(forward);
  if (std::fabs(forward[1]) > 0.95f) {
    forward[1] *= 0.5f;
    const float n = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (float& f : forward) f /= n;
  }
  const float world_up[3] = {0.0f, 1.0f, 0.0f};
  float right[3] = {forward[1] * world_up[2] - forward[2] * world_up[1], forward[2] * world_up[0] - forward[0] * world_up[2], forward[0] * world_up[1] - forward[1] * world_up[0]};
  const float rn = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  for (float& r : right) r /= rn;
  const float up[3] = {right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2], right[0] * forward[1] - right[1] * forward[0]};
  std::memset(view_inverse, 0, 16 * sizeof(float));
  for (int c = 0; c < 3; ++c) {
    view_inverse[c] = right[c];
    view_inverse[4 + c] = up[c];
    view_inverse[8 + c] = -forward[c];
    view_inverse[12 + c] = rng.Range(-300.0f, 300.0f);
  }
  view_inverse[15] = 1.0f;
  Check(lp::RowsFromViewInverse(view_inverse, rows), "a random pose is a camera");
}

// The projection's depth rows of a D3D style perspective (near n, far f): t = n f / (f - ndc (f - n)), ndc = 1 - stored depth. lin = {f, f - n, n f, 0}.
void LinOf(float n, float f, float lin[4]) {
  lin[0] = f;
  lin[1] = f - n;
  lin[2] = n * f;
  lin[3] = 0.0f;
}
float StoredOf(float t, float n, float f) {
  const float ndc = f * (t - n) / (t * (f - n));
  return 1.0f - ndc;
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
size_t At(const std::string& text, const std::string& what) { return text.find(what); }

// the #define NAME value pairs (integers) of a text
std::map<std::string, long long> Defines(const std::string& text) {
  std::map<std::string, long long> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    size_t i = line.find("#define ");
    if (i != 0) continue;
    std::istringstream words(line.substr(8));
    std::string name, value;
    words >> name >> value;
    if (name.empty() || value.empty() || name.find('(') != std::string::npos) continue;
    char* end = nullptr;
    const long long v = std::strtoll(value.c_str(), &end, 0);
    if (end != value.c_str() && (*end == '\0' || *end == 'f' || *end == '.')) {
      if (*end == '\0') out[name] = v;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// the layout
// ---------------------------------------------------------------------------

void LayoutChecks(const std::string& hlsli) {
  Check(sizeof(lp::PassHeader) == 128 && sizeof(lp::PassPush) == 32, "the header is 128 bytes, the push block 32");
  Check(lp::kHeaderBytes == 128 && lp::kLightsOffset == 128 && lp::kMaxPassLights == 32 && lp::kMaxPassLights == kMaxSurfaceLights, "the buffer: a 128 byte header, up to 32 lights");
  Check(lp::kShadowOffset == 128 + 64 * 32 && lp::kShadowBytes == 32 + 96 * 8 && lp::kBufferBytes == lp::kShadowOffset + lp::kShadowBytes && lp::kBufferAlloc == 3072 && lp::kBufferAlloc % 256 == 0,
        "the buffer is the header, 32 lights and the shadow view table (2976 bytes), an upload buffer of 3072");
  const std::map<std::string, long long> d = Defines(hlsli);
  const auto want = [&](const char* name, long long value) {
    const auto it = d.find(name);
    Check(it != d.end() && it->second == value, std::string("hlsl/local_light.hlsli ") + name + " = " + std::to_string(value));
  };
  want("LP_HEADER_BYTES", lp::kHeaderBytes);
  want("LP_LIGHTS_OFFSET", lp::kLightsOffset);
  want("LP_MAX_LIGHTS", lp::kMaxPassLights);
  want("LP_OFF_RIGHT", offsetof(lp::PassHeader, right));
  want("LP_OFF_UP", offsetof(lp::PassHeader, up));
  want("LP_OFF_FORWARD", offsetof(lp::PassHeader, forward));
  want("LP_OFF_POSITION", offsetof(lp::PassHeader, position));
  want("LP_OFF_LIN", offsetof(lp::PassHeader, lin));
  want("LP_OFF_SIZE", offsetof(lp::PassHeader, size));
  want("LP_OFF_OCC", offsetof(lp::PassHeader, occ));
  want("LP_OFF_COUNT", offsetof(lp::PassHeader, count));
  want("LP_OFF_EXPOSURE", offsetof(lp::PassHeader, exposure));
  want("LP_FLAG_MATERIAL", lp::kFlagMaterial);
  want("LP_FLAG_OCCLUSION", lp::kFlagOcclusion);
  want("LP_FLAG_SHADOW", lp::kFlagShadow);
  // GB-H2: the shadow view table the pass reads after the lights (the offsets of the shader's seams are these defines)
  want("LP_SHADOW_OFFSET", lp::kShadowOffset);
  want("LP_SHADOW_HEADER_BYTES", sizeof(shadow_lookup::GpuShadowHeader));
  want("LP_SHADOW_VIEWS_OFFSET", lp::kShadowOffset + sizeof(shadow_lookup::GpuShadowHeader));
  want("LP_SHADOW_VIEW_BYTES", sizeof(LocalShadowView));
  want("LP_SHADOW_OFF_COUNT", lp::kShadowOffset + offsetof(shadow_lookup::GpuShadowHeader, count));
  want("LP_SHADOW_OFF_SLOT", lp::kShadowOffset + offsetof(shadow_lookup::GpuShadowHeader, atlas_slot));
  want("LP_SHADOW_OFF_SIZE", lp::kShadowOffset + offsetof(shadow_lookup::GpuShadowHeader, atlas_width));
  want("LP_SHADOW_OFF_LAYERS", lp::kShadowOffset + offsetof(shadow_lookup::GpuShadowHeader, layers));
  Check(offsetof(shadow_lookup::GpuShadowHeader, atlas_height) == offsetof(shadow_lookup::GpuShadowHeader, atlas_width) + 4, "the atlas's height follows its width (one Load of two uints)");
  want("LP_CLASS_NONE", 0);
  want("LP_CLASS_EMISSIVE", 13);
  // GB-NEON-COST: the lights a draw may carry and the push flag of the A/B (the push block is the same two rows)
  want("LP_MAX_DRAW_LIGHTS", lp::kMaxDrawLights);
  want("LP_PUSH_LATE_REACH", lp::kPushLateReach);
  Check(lp::kMaxDrawLights == neon_light::kTubePoints && lp::kPushLateReach == 1u && lp::kCostOffMask == 3u && lp::kCostOffReachFirst == 1u && lp::kCostOffMerge == 2u,
        "a draw takes a whole neon tube (four points), the A/B's bits are the reach test (1) and the merge (2)");
  Check(Has(hlsli, "#define LP_SKY 60000.0f") && lp::kSky == 60000.0f, "the sky's distance is the AO's");
  // the rows after the header are the GpuLight's 64 bytes
  Check(lp::kLightsOffset + sizeof(GpuLight) * lp::kMaxPassLights == lp::kShadowOffset && lp::kShadowOffset % 16 == 0, "the lights' rows follow the header and the shadow view table follows them, 16 byte aligned");
  Check(LL_LIGHT_BYTES == int(sizeof(GpuLight)) && LL_BUFFER_HEADER_BYTES == int(sizeof(GpuLightHeader)), "the twin's light row is the GpuLight's size");
}

// ---------------------------------------------------------------------------
// the camera and a pixel's world position
// ---------------------------------------------------------------------------

void CameraChecks() {
  // the identity view inverse: right x, up y, back z, so forward is -z; position row 3
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
  lp::Rows rows;
  Check(lp::RowsFromViewInverse(m, &rows), "the identity pose is a camera");
  Check(rows.right[0] == 1.0f && rows.up[1] == 1.0f && rows.forward[2] == -1.0f && rows.position[0] == 5.0f && rows.position[1] == 6.0f && rows.position[2] == 7.0f,
        "row 0 is the right, row 1 the up, minus row 2 the forward, row 3 the place");
  float bad[16];
  std::memcpy(bad, m, sizeof(bad));
  bad[1] = 0.5f;  // the right is no unit vector
  Check(!lp::RowsFromViewInverse(bad, &rows), "a row that is not a unit axis is no camera");
  std::memcpy(bad, m, sizeof(bad));
  bad[13] = std::nanf("");
  Check(!lp::RowsFromViewInverse(bad, &rows), "a NaN is no camera");
  std::memcpy(bad, m, sizeof(bad));
  bad[0] = bad[5] = bad[10] = 0.0f;
  Check(!lp::RowsFromViewInverse(bad, &rows), "a zero matrix is no camera");

  // LinearDepth: the D3D style projection, round trip over the range a frame has
  Lcg rng;
  float lin[4];
  LinOf(0.1f, 10000.0f, lin);
  for (int i = 0; i < 400; ++i) {
    const float t = rng.Range(0.5f, 400.0f);
    const float stored = StoredOf(t, 0.1f, 10000.0f);
    CheckRel(lp::LinearDepth(stored, lin), t, 2.0e-3, 1.0e-3, Num("LinearDepth of a stored depth for t = %.4f", t));
  }
  Check(lp::LinearDepth(0.0f, lin) == lp::kSky, "the stored depth 0 is the sky");
  Check(lp::LinearDepth(-0.5f, lin) == lp::kSky, "a stored depth under 0 is the sky");
  const float zero_den[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  Check(lp::LinearDepth(0.5f, zero_den) == lp::kSky, "a projection with no denominator gives the sky, not a number");

  // the world position and back: a pixel, a view distance -> a world point -> the same pixel and distance
  for (int i = 0; i < 300; ++i) {
    float vi[16];
    lp::Rows r;
    RandomPose(rng, vi, &r);
    const float tan_x = rng.Range(0.3f, 1.2f);
    const float tan_y = tan_x * rng.Range(0.4f, 0.8f);
    const float w = float(rng.Below(3000) + 320);
    const float h = float(rng.Below(1600) + 180);
    const float px = float(rng.Below(uint32_t(w))) + 0.5f;
    const float py = float(rng.Below(uint32_t(h))) + 0.5f;
    const float t = rng.Range(0.5f, 150.0f);
    float p[3];
    lp::WorldPosition(r, tan_x, tan_y, px, py, w, h, t, p);
    float bx, by, bt;
    Check(lp::ProjectToPixel(r, tan_x, tan_y, w, h, p, 0.01f, &bx, &by, &bt), "a world position of a pixel is in front of the camera");
    CheckNear(bx, px, 0.06, Num("the pixel x of the world position (%.2f)", px));
    CheckNear(by, py, 0.06, Num("the pixel y of the world position (%.2f)", py));
    CheckRel(bt, t, 1.0e-4, 1.0e-3, Num("the view distance of the world position (%.3f)", t));
  }
  // the centre of the picture is straight ahead: the point at distance t is position + forward * t
  {
    float vi[16];
    lp::Rows r;
    RandomPose(rng, vi, &r);
    float p[3];
    lp::WorldPosition(r, 0.8f, 0.45f, 960.0f, 540.0f, 1920.0f, 1080.0f, 12.0f, p);
    for (int i = 0; i < 3; ++i) {
      CheckNear(p[i], r.position[i] + r.forward[i] * 12.0f, 1.0e-3, "the picture's centre is straight ahead");
    }
    // the top of the picture is up: y grows toward the camera's up
    float top[3];
    lp::WorldPosition(r, 0.8f, 0.45f, 960.0f, 0.0f, 1920.0f, 1080.0f, 10.0f, top);
    const float d[3] = {top[0] - r.position[0], top[1] - r.position[1], top[2] - r.position[2]};
    const float along_up = d[0] * r.up[0] + d[1] * r.up[1] + d[2] * r.up[2];
    CheckNear(along_up, 0.45 * 10.0, 1.0e-3, "the picture's top edge is tan_y x t above the axis");
  }
}

// ---------------------------------------------------------------------------
// the units and the light a pixel gets
// ---------------------------------------------------------------------------

Light GameLamp(const float pos[3], float intensity, bool spot) {
  const float colour[3] = {1.0f, 0.82f, 0.55f};
  const float dir[3] = {0.0f, -1.0f, 0.0f};
  return MakeGameLampLight(pos, colour, dir, intensity, spot ? 125.0f : 360.0f, spot ? -50.5f : 0.0f, 1.0f, spot ? 1 : 0, LightSource::kMod, 77);
}

void UnitsChecks() {
  const float none[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  CheckNear(lp::OcclusionFactor(none, 12.0f), 1.0, 0.0, "no OcclusionInfo: the height factor is 1");
  const float occ[4] = {2.0f, 0.5f, 0.6f, 1.0f};  // ramp from y 2 to y 4, to 0.6
  CheckNear(lp::OcclusionFactor(occ, 0.0f), 1.0, 1e-6, "below the ramp the factor is 1");
  CheckNear(lp::OcclusionFactor(occ, 3.0f), 1.0 + 0.5 * (0.6 - 1.0), 1e-6, "half way up the ramp");
  CheckNear(lp::OcclusionFactor(occ, 50.0f), 0.6, 1e-6, "above the ramp the factor is OcclusionInfo.z");

  // the game's lamp inside its plateau lights with its full colour: a surface straight below a street lamp, N.L = 1, cone = 1
  const float pos[3] = {10.0f, 7.0f, -4.0f};
  const Light lamp = GameLamp(pos, 55.0f, true);
  lp::Units u;
  const float world[3] = {10.0f, 1.0f, -4.0f};  // 6 m below: d^2 = 36 <= I 55: the plateau
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const float albedo[3] = {0.5f, 0.4f, 0.3f};
  float out[3];
  lp::PixelLight(lamp, u, world, up, albedo, 1.0f, out);
  for (int c = 0; c < 3; ++c) {
    CheckNear(out[c], albedo[c] * 1.0f * (c == 0 ? 1.0f : (c == 1 ? 0.82f : 0.55f)), 1.0e-5, "inside the plateau the lamp lights with its full colour x albedo");
  }
  // beyond the plateau the term falls as I / d^2: 20 m below, the same lamp
  const float far_world[3] = {10.0f, -13.0f, -4.0f};
  lp::PixelLight(lamp, u, far_world, up, albedo, 1.0f, out);
  CheckNear(out[0], albedo[0] * (55.0 / 400.0), 1.0e-5, "beyond the plateau the term is I / d^2 (20 m: 55 / 400)");
  // the night light intensity multiplies the game's laws, the lit weight and the gain too
  u.night = 0.4f;
  u.gain = 2.0f;
  lp::PixelLight(lamp, u, world, up, albedo, 0.5f, out);
  CheckNear(out[0], albedo[0] * 0.4 * 2.0 * 0.5, 1.0e-5, "night x gain x lit weight");
  // a surface that faces away gets nothing; a lit weight of 0 (emissive) gets nothing
  const float down[3] = {0.0f, -1.0f, 0.0f};
  lp::PixelLight(lamp, lp::Units{}, world, down, albedo, 1.0f, out);
  Check(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f, "a surface that faces away from the lamp gets nothing");
  lp::PixelLight(lamp, lp::Units{}, world, up, albedo, 0.0f, out);
  Check(out[0] == 0.0f, "a lit weight of 0 takes no light");
  // the main colour target's exposure (2^bias) multiplies
  lp::Units ue;
  ue.exposure = 16.0f;
  lp::PixelLight(lamp, ue, world, up, albedo, 1.0f, out);
  CheckNear(out[0], albedo[0] * 16.0, 1.0e-4, "the exposure of the target (2^4) multiplies the light into its units");
  // the height factor multiplies
  lp::Units uo;
  std::memcpy(uo.occ, occ, sizeof(occ));
  lp::PixelLight(lamp, uo, far_world, up, albedo, 1.0f, out);  // y = -13: below the ramp: 1
  CheckNear(out[0], albedo[0] * (55.0 / 400.0), 1.0e-5, "below the ramp the lamp's light is unchanged");
  const float high[3] = {10.0f, 40.0f, -4.0f};
  Light high_lamp = GameLamp(pos, 55.0f, false);
  high_lamp.pos[1] = 46.0f;
  lp::PixelLight(high_lamp, uo, high, up, albedo, 1.0f, out);  // y = 40, 6 m below an omni lamp: the plateau; above the ramp: 0.6
  CheckNear(out[0], albedo[0] * 0.6, 1.0e-5, "above the ramp the lamp's light is x OcclusionInfo.z");
  // a headlight does not take the night light intensity (its colour has its own) but does take the lit weight, the gain and the height factor
  Light head;
  head.law = LightLaw::kHeadlight;
  head.kind = LightKind::kSpot;
  head.source = LightSource::kHeadlight;
  head.pos[0] = 0.0f; head.pos[1] = 0.7f; head.pos[2] = 0.0f;
  head.dir[0] = 1.0f; head.dir[1] = 0.0f; head.dir[2] = 0.0f;
  head.cos_outer = float(std::cos(38.0 * kDeg));
  head.cos_inner = float(std::cos(12.0 * kDeg));
  head.range = 70.0f;
  head.law_a = ll::kHeadlightFalloffDistance;
  head.color[0] = head.color[1] = head.color[2] = 0.9f;
  const float h_world[3] = {10.0f, 0.0f, 0.0f};
  const float h_normal[3] = {-1.0f, 0.0f, 0.0f};
  lp::Units night_half;
  night_half.night = 0.5f;
  float with_night[3], without[3];
  lp::PixelLight(head, night_half, h_world, h_normal, albedo, 1.0f, with_night);
  lp::PixelLight(head, lp::Units{}, h_world, h_normal, albedo, 1.0f, without);
  CheckNear(with_night[0], without[0], 1.0e-7, "a headlight's term does not take ShaderGlobals.w");
  Check(without[0] > 0.0f, "a headlight 10 m ahead lights a wall that faces it");
  // and it is exactly the law's term x albedo
  ll::LightSurface s;
  s.position[0] = 10.0f; s.position[1] = 0.0f; s.position[2] = 0.0f;
  s.normal[0] = -1.0f; s.normal[1] = 0.0f; s.normal[2] = 0.0f;
  float term[3];
  ll::LightTerm(head, s, 1.0f, term);
  CheckNear(without[0], albedo[0] * term[0], 1.0e-7, "PixelLight is albedo x the law's LightTerm (a headlight)");
}

// ---------------------------------------------------------------------------
// the votes
// ---------------------------------------------------------------------------

void VoteChecks() {
  lp::UnitsVote votes;
  std::array<uint32_t, 4> best{};
  Check(!votes.Best(&best), "no vote, no units");
  const auto bits = [](float a, float b, float c, float d) { return std::array<uint32_t, 4>{lp::FloatToBits(a), lp::FloatToBits(b), lp::FloatToBits(c), lp::FloatToBits(d)}; };
  votes.Add(bits(0.0f, 1.0f, 1.0f, 1.0f));
  votes.Add(bits(1.0f, -50.0f, 0.02f, 0.5f));
  votes.Add(bits(1.0f, -50.0f, 0.02f, 0.5f));
  votes.Add(bits(1.0f, -50.0f, 0.02f, 0.5f));
  votes.Add(bits(0.0f, 1.0f, 1.0f, 1.0f));
  Check(votes.Best(&best) && best == bits(1.0f, -50.0f, 0.02f, 0.5f), "the value most draws carry wins");
  Check(votes.draws() == 5 && votes.distinct() == 2 && votes.overflow() == 0, "draws, distinct values, overflow");
  // a tie: the first value that came in
  lp::UnitsVote tie;
  tie.Add(bits(0.3f, 0.0f, 1.0f, 1.0f));
  tie.Add(bits(0.9f, 0.0f, 1.0f, 1.0f));
  Check(tie.Best(&best) && best == bits(0.3f, 0.0f, 1.0f, 1.0f), "a tie goes to the first value");
  // overflow: more distinct values than slots
  lp::UnitsVote many;
  for (uint32_t i = 0; i < lp::UnitsVote::kSlots + 3; ++i) {
    many.Add(bits(float(i), 0.0f, 1.0f, 1.0f));
  }
  Check(many.distinct() == lp::UnitsVote::kSlots && many.overflow() == 3, "values past the table are counted, not kept");
  many.Reset();
  Check(many.draws() == 0 && many.distinct() == 0 && !many.Best(&best), "Reset empties the table");
  // the units a winner says
  lp::Units u = lp::UnitsFromBits(bits(0.75f, -20.0f, 0.04f, 0.6f), 1.5f);
  CheckNear(u.night, 0.75, 0.0, "the night light intensity of the vote");
  Check(u.occ[3] == 1.0f && u.occ[0] == -20.0f && u.occ[1] == 0.04f && u.occ[2] == 0.6f && u.gain == 1.5f, "OcclusionInfo and the gain of the vote");
  u = lp::UnitsFromBits(bits(std::nanf(""), std::nanf(""), 0.0f, 3.0f), 1.0f);
  CheckNear(u.night, 1.0, 0.0, "a NaN night light intensity is 1");
  Check(u.occ[3] == 0.0f, "OcclusionInfo that is not a ramp (scale 0 or NaN) is not used");
  u = lp::UnitsFromBits(bits(100.0f, 0.0f, 1.0f, 1.0f), 1.0f);
  CheckNear(u.night, 1.0, 0.0, "a night light intensity far past the game's range is not believed");
  u = lp::UnitsFromBits(bits(0.5f, 0.0f, 1.0f, 1000.0f), 1.0f);
  Check(u.occ[3] == 0.0f, "an absurd OcclusionInfo.z is not believed");
}

// ---------------------------------------------------------------------------
// which lights
// ---------------------------------------------------------------------------

Light HeadlightOf(uint64_t id, float x) {
  Light l;
  l.law = LightLaw::kHeadlight;
  l.kind = LightKind::kSpot;
  l.source = LightSource::kHeadlight;
  l.pos[0] = x;
  l.pos[1] = 0.7f;
  l.dir[0] = 1.0f; l.dir[1] = 0.0f; l.dir[2] = 0.0f;
  l.cos_outer = float(std::cos(38.0 * kDeg));
  l.cos_inner = float(std::cos(12.0 * kDeg));
  l.range = 70.0f;
  l.law_a = 26.0f;
  l.color[0] = l.color[1] = l.color[2] = 0.9f;
  l.id = id;
  l.roles = kRoleMaskAll;
  return l;
}

void SelectionChecks() {
  LightFrame frame;
  frame.lights[0] = HeadlightOf(101, 0.0f);                 // a headlight: surface light, the game does not light with it
  const float p[3] = {3.0f, 6.0f, 3.0f};
  frame.lights[1] = GameLamp(p, 55.0f, true);               // a street lamp: the game's grid lights it
  frame.lights[1].id = 102;
  frame.lights[2] = HeadlightOf(103, 20.0f);
  frame.lights[2].range = 0.0f;                             // off
  frame.lights[3] = HeadlightOf(0, 30.0f);                  // no id: an unused slot
  frame.lights[4] = HeadlightOf(105, 40.0f);
  frame.lights[4].color[0] = frame.lights[4].color[1] = frame.lights[4].color[2] = 0.0f;  // no colour
  frame.lights[5] = HeadlightOf(106, 50.0f);
  frame.lights[5].roles = kRoleMaskVolume;                  // may not add surface light
  frame.lights[6] = HeadlightOf(107, 60.0f);
  frame.count = 7;
  Check(lp::TakesFrameLight(frame.lights[0]) && !lp::TakesFrameLight(frame.lights[1]) && !lp::TakesFrameLight(frame.lights[2]) && !lp::TakesFrameLight(frame.lights[3]) &&
            !lp::TakesFrameLight(frame.lights[4]) && !lp::TakesFrameLight(frame.lights[5]) && lp::TakesFrameLight(frame.lights[6]),
        "the frame's lights the pass takes: surface role, not game lit, on, with colour and an id");

  // no ranking: the slots in order
  lp::Selection s = lp::SelectLights(frame, lp::kSourceFrame, nullptr, 0);
  Check(s.count == 2 && s.from_frame == 2 && s.from_extra == 0 && s.lights[0].id == 101 && s.lights[1].id == 107, "with no ranking the pass takes the slots in order");
  // sources without the frame: nothing from it
  s = lp::SelectLights(frame, 0, nullptr, 0);
  Check(s.count == 0, "without the frame's source the frame's lights are not taken");
  // a ranking: the surface role's order, best first, up to its count
  frame.order[uint32_t(LightRole::kSurface)][0] = 6;
  frame.order[uint32_t(LightRole::kSurface)][1] = 0;
  frame.order[uint32_t(LightRole::kSurface)][2] = 1;  // a street lamp somehow in the ranking: still not taken
  frame.role_count[uint32_t(LightRole::kSurface)] = 3;
  s = lp::SelectLights(frame, lp::kSourceFrame, nullptr, 0);
  Check(s.count == 2 && s.lights[0].id == 107 && s.lights[1].id == 101, "the surface ranking's order is the pass's, and a game lit light in it is not taken");

  // extras: the A/B's lamps and the test lamp are in whatever the sources say; the budget cuts the end
  float vi[16];
  lp::Rows rows;
  Lcg rng;
  RandomPose(rng, vi, &rows);
  std::vector<Light> extras;
  extras.push_back(lp::MakeTestLamp(rows));
  s = lp::SelectLights(frame, 0, extras.data(), uint32_t(extras.size()));
  Check(s.count == 1 && s.from_extra == 1 && s.lights[0].id == lp::kTestLampId, "the handed over lights are in with no source set");
  for (int i = 0; i < 40; ++i) {
    extras.push_back(lp::MakeTestLamp(rows));
  }
  s = lp::SelectLights(frame, lp::kSourceFrame, extras.data(), uint32_t(extras.size()));
  Check(s.count == lp::kMaxPassLights && s.dropped == 2 + 41 - lp::kMaxPassLights, "the budget is 32 lights and the rest is counted");
  Check(s.from_frame == 2 && s.from_extra == lp::kMaxPassLights - 2, "the frame's lights come first, the handed over ones after");
  // GB-H2: the frame class's budget: the rig's four at every tier, the rest counted as cut (a frame is never over it today, but the headlights are the dear lights: a lookup each)
  {
    LightFrame many;
    for (uint32_t i = 0; i < 7; ++i) {
      many.lights[i] = HeadlightOf(200 + i, float(i) * 10.0f);
    }
    many.count = 7;
    const lp::Selection capped = lp::SelectLights(many, lp::kSourceFrame, nullptr, 0, 4);
    Check(capped.count == 4 && capped.from_frame == 4 && capped.cut_class[size_t(lp::SourceClass::kFrame)] == 3 && capped.lights[0].id == 200 && capped.lights[3].id == 203,
          "the frame class takes its four first lights and counts the three it cuts");
    const lp::Selection uncapped = lp::SelectLights(many, lp::kSourceFrame, nullptr, 0);
    Check(uncapped.count == 7 && uncapped.cut_class[size_t(lp::SourceClass::kFrame)] == 0, "without a cap (the default) the frame's seven lights are all taken");
    for (const lp::Tier tier : {lp::Tier::kMedium, lp::Tier::kHigh, lp::Tier::kUltra}) {
      Check(lp::BudgetOf(tier).frame == 4 && lp::CapOf(lp::BudgetOf(tier), lp::SourceClass::kFrame) == 4, std::string("the frame class's cap is the rig's four at the tier ") + lp::TierName(tier));
      const lp::Selection by_tier = lp::SelectPassLights(many, lp::kSourceFrame, nullptr, 0, lp::Candidates{}, tier);
      Check(by_tier.count == 4 && by_tier.from_class[size_t(lp::SourceClass::kFrame)] == 4 && by_tier.cut_class[size_t(lp::SourceClass::kFrame)] == 3,
            std::string("SelectPassLights applies the frame class's cap at the tier ") + lp::TierName(tier));
    }
  }

  // the test lamp
  const Light test = lp::MakeTestLamp(rows);
  for (int i = 0; i < 3; ++i) {
    const float want = rows.position[i] + rows.forward[i] * 8.0f + (i == 1 ? 4.5f : 0.0f);
    CheckNear(test.pos[i], want, 1.0e-4, "the test lamp is 8 m ahead and 4.5 m up");
  }
  Check((test.flags & kLightFlagGameLit) == 0 && (test.roles & kRoleMaskSurface) != 0 && test.law == LightLaw::kGameLamp && test.law_a == 55.0f && lp::TakesFrameLight(test),
        "the test lamp is a street lamp the pass lights with");
  CheckNear(test.range, ll::LampRange(55.0f), 1.0e-4, "the test lamp's reach is the game's sqrt(100 I)");

  // the A/B lamp: a mod's record through the manager's conversion, then allowed to add surface light
  mls::Record rec;
  rec.pos[0] = 1.0f; rec.pos[1] = 6.0f; rec.pos[2] = 2.0f;
  rec.colour[0] = 1.0f; rec.colour[1] = 0.8f; rec.colour[2] = 0.6f;
  rec.dir[1] = -1.0f;
  rec.intensity = 55.0f;
  rec.cone_deg = 125.0f;
  rec.spread_deg = -50.5f;
  rec.id = 9;
  rec.type = 1;
  const Light mod = LightFromModLight(rec, 0x1234u);
  Check((mod.flags & kLightFlagGameLit) != 0 && (mod.roles & kRoleMaskSurface) == 0 && !lp::TakesFrameLight(mod), "a mod's lamp is game lit: the frame's source never takes it");
  const Light ab = lp::AsPassLight(mod);
  Check((ab.flags & kLightFlagGameLit) == 0 && (ab.roles & kRoleMaskSurface) != 0 && lp::TakesFrameLight(ab), "the A/B's lamp is allowed to add surface light");
  Check(ab.id == mod.id && ab.law == mod.law && ab.law_a == mod.law_a && ab.range == mod.range && ab.cos_outer == mod.cos_outer && ab.color[0] == mod.color[0],
        "the A/B's lamp is the same light, nothing but the permission changed");
  // the lamp the A/B and the grid agree on: colour = rgb x I, the plateau law, inside the plateau the term is the colour
  const float below[3] = {1.0f, 0.0f, 2.0f};  // 6 m below, d^2 = 36 <= 55
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const float one[3] = {1.0f, 1.0f, 1.0f};
  float out[3];
  lp::PixelLight(ab, lp::Units{}, below, up, one, 1.0f, out);
  CheckNear(out[0], 1.0, 1.0e-5, "the A/B lamp inside its plateau: the colour (rgb x I x 1/I)");
  CheckNear(out[1], 0.8, 1.0e-5, "... in every channel");
  CheckNear(out[2], 0.6, 1.0e-5, "... in every channel");
  // the A/B modes
  Check(lp::AbOf(0) == lp::Ab::kGrid && lp::AbOf(1) == lp::Ab::kNowhere && lp::AbOf(2) == lp::Ab::kPass && lp::AbOf(7) == lp::Ab::kGrid && lp::AbOf(-1) == lp::Ab::kGrid,
        "the A/B setting's values");
  Check(!lp::AbWithholdsFromGrid(lp::Ab::kGrid) && lp::AbWithholdsFromGrid(lp::Ab::kNowhere) && lp::AbWithholdsFromGrid(lp::Ab::kPass), "the grid is built with the lamps off in the pass and control modes only");
}

// ---------------------------------------------------------------------------
// the scissor
// ---------------------------------------------------------------------------

void ScissorChecks() {
  Lcg rng;
  for (int round = 0; round < 60; ++round) {
    float vi[16];
    lp::Rows rows;
    RandomPose(rng, vi, &rows);
    const float tan_x = rng.Range(0.4f, 1.0f);
    const float tan_y = tan_x * 0.5625f;
    const uint32_t w = 1920, h = 1080;
    // a light a little way in front of the camera or anywhere near it, a spot (a headlight) or a lamp
    Light l;
    const bool headlight = (round & 1) != 0;
    if (headlight) {
      l = HeadlightOf(1, 0.0f);
    } else {
      const float pp[3] = {0.0f, 0.0f, 0.0f};
      l = GameLamp(pp, rng.Range(20.0f, 100.0f), (round & 2) != 0);
    }
    float centre[3];
    rng.Direction(centre);
    const float dist = rng.Range(3.0f, 120.0f);
    for (int i = 0; i < 3; ++i) {
      l.pos[i] = rows.position[i] + centre[i] * dist;
    }
    if (headlight) {
      rng.Direction(l.dir);
    } else if ((round & 2) != 0) {
      rng.Direction(l.dir);
    }
    const lp::PixelRect rect = lp::ScissorOfLight(l, rows, tan_x, tan_y, w, h);
    // every world point the light can reach, in front of the camera and on the picture, lies in the rectangle
    int tested = 0;
    for (int k = 0; k < 4000 && tested < 400; ++k) {
      float dir[3];
      rng.Direction(dir);
      const float rr = l.range * std::cbrt(rng.Next());
      const float q[3] = {l.pos[0] + dir[0] * rr, l.pos[1] + dir[1] * rr, l.pos[2] + dir[2] * rr};
      ll::LightSurface s;
      for (int i = 0; i < 3; ++i) {
        s.position[i] = q[i];
        s.normal[i] = -dir[i];  // facing the light
      }
      if (!(ll::LightScale(l, s, 1.0f) > 0.0f)) {
        continue;
      }
      ++tested;
      float px, py, t;
      if (!lp::ProjectToPixel(rows, tan_x, tan_y, float(w), float(h), q, lp::kScissorNear, &px, &py, &t)) {
        continue;  // behind the camera: no pixel
      }
      if (px < 0.0f || py < 0.0f || px >= float(w) || py >= float(h)) {
        continue;  // off the picture
      }
      Check(px >= float(rect.x0) && px <= float(rect.x1) && py >= float(rect.y0) && py <= float(rect.y1), Num("a lit point at pixel (%.1f, %.1f) is in the light's rectangle", px, py));
    }
    Check(rect.x0 >= 0 && rect.y0 >= 0 && rect.x1 <= int32_t(w) && rect.y1 <= int32_t(h), "the rectangle is inside the picture");
  }
  // a light behind the camera: nothing; a light the camera is inside: the whole picture; a light ahead: a smaller part
  float vi[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // looks along -z
  lp::Rows rows;
  lp::RowsFromViewInverse(vi, &rows);
  const float behind_pos[3] = {0.0f, 0.0f, 200.0f};  // +z is behind
  Light lamp = GameLamp(behind_pos, 55.0f, false);
  Check(lp::ScissorOfLight(lamp, rows, 0.8f, 0.45f, 1920, 1080).Empty(), "a light whose whole reach is behind the camera has an empty rectangle");
  const float at_camera[3] = {0.0f, 0.0f, 0.0f};
  lamp = GameLamp(at_camera, 55.0f, false);
  const lp::PixelRect full = lp::ScissorOfLight(lamp, rows, 0.8f, 0.45f, 1920, 1080);
  Check(full.x0 == 0 && full.y0 == 0 && full.x1 == 1920 && full.y1 == 1080, "a light at the camera has the whole picture");
  const float ahead[3] = {0.0f, 0.0f, -300.0f};
  lamp = GameLamp(ahead, 20.0f, false);  // reach 44.7 m
  const lp::PixelRect small = lp::ScissorOfLight(lamp, rows, 0.8f, 0.45f, 1920, 1080);
  Check(!small.Empty() && small.Area() < 1920ull * 1080ull / 4ull, "a lamp 300 m ahead covers a small part of the picture");
  Check(small.x0 < 960 && small.x1 > 960 && small.y0 < 540 && small.y1 > 540, "... round the picture's centre");
  // a narrow cone's sphere is smaller than the range's ball
  float c[3], r;
  Light spot = HeadlightOf(1, 0.0f);
  lp::BoundingSphere(spot, c, &r);
  Check(r < spot.range && r > 0.5f * spot.range, "a headlight's cone sphere is between half the range and the range");
  CheckNear(c[0], spot.dir[0] * r, 1.0e-4, "... on the axis, the apex on its surface");
  Light omni = GameLamp(ahead, 55.0f, false);
  lp::BoundingSphere(omni, c, &r);
  Check(r == omni.range && c[2] == ahead[2], "an omni light's sphere is the ball of its range");
}

// ---------------------------------------------------------------------------
// the buffer
// ---------------------------------------------------------------------------

void BufferChecks() {
  Lcg rng;
  float vi[16];
  lp::Rows rows;
  RandomPose(rng, vi, &rows);
  lp::PassInputs in;
  in.rows = rows;
  in.tan_x = 0.77f;
  in.tan_y = 0.43f;
  LinOf(0.1f, 10000.0f, in.lin);
  in.width = 2560;
  in.height = 1440;
  in.units.night = 0.9f;
  in.units.gain = 1.25f;
  in.units.exposure = 16.0f;
  in.units.occ[0] = -10.0f; in.units.occ[1] = 0.05f; in.units.occ[2] = 0.7f; in.units.occ[3] = 1.0f;
  in.flags = lp::kFlagMaterial;
  lp::Selection s;
  s.lights[0] = lp::MakeTestLamp(rows);
  s.lights[1] = HeadlightOf(5, 4.0f);
  s.count = 2;
  std::vector<uint8_t> bytes(lp::kBufferAlloc, 0xAB);
  lp::BuildBuffer(in, s, bytes.data());
  lp::PassHeader h;
  std::memcpy(&h, bytes.data(), sizeof(h));
  Check(h.count == 2 && h.flags == (lp::kFlagMaterial | lp::kFlagOcclusion), "the header says two lights, the material plane and the height factor");
  CheckNear(h.tan_x, 0.77, 1.0e-6, "tan_x");
  CheckNear(h.tan_y, 0.43, 1.0e-6, "tan_y");
  CheckNear(h.gain, 1.25, 1.0e-6, "gain");
  CheckNear(h.exposure, 16.0, 0.0, "exposure");
  CheckNear(h.night, 0.9, 1.0e-6, "night");
  bool same = true;
  for (int i = 0; i < 3; ++i) {
    same = same && h.right[i] == rows.right[i] && h.up[i] == rows.up[i] && h.forward[i] == rows.forward[i] && h.position[i] == rows.position[i];
  }
  Check(same, "the camera's axes and place");
  Check(h.size[0] == 2560.0f && h.size[1] == 1440.0f && std::fabs(h.size[2] * 2560.0f - 1.0f) < 1e-6f && std::fabs(h.size[3] * 1440.0f - 1.0f) < 1e-6f, "the size and its inverse");
  Check(h.occ[0] == -10.0f && h.occ[1] == 0.05f && h.occ[2] == 0.7f && h.occ[3] == 1.0f, "OcclusionInfo");
  for (uint32_t i = 0; i < 2; ++i) {
    const GpuLight row = PackGpuLight(s.lights[i]);
    Check(std::memcmp(bytes.data() + lp::kLightsOffset + 64 * i, &row, 64) == 0, "a light's row in the buffer is its GpuLight");
  }
  bool zero_after = true;
  for (size_t i = lp::kLightsOffset + 128; i < lp::kBufferAlloc; ++i) {
    zero_after = zero_after && bytes[i] == 0;
  }
  Check(zero_after, "the rows past the count and the padding are zero");
  // no OcclusionInfo: the flag is not set
  in.units.occ[3] = 0.0f;
  lp::BuildBuffer(in, s, bytes.data());
  std::memcpy(&h, bytes.data(), sizeof(h));
  Check((h.flags & lp::kFlagOcclusion) == 0, "without OcclusionInfo the height factor flag is off");
}

// ---------------------------------------------------------------------------
// GB-H2: the shadow view table in the buffer and the pass's pixel with a shadow
// ---------------------------------------------------------------------------

// A frame's shadow views as the replay makes them: `views` views, each a perspective view looking along +z from the origin (w = z; the unit axis in row 3 and A times it in row 2, as the lookup
// reads a view: HL-SHADOW-QUALITY), in tiles of a 2 x 2 atlas of `tile` texels.
ShadowFrame MakeShadowFrame(uint32_t views, uint32_t tile, Lcg& rng) {
  ShadowFrame f;
  f.atlas.valid = true;
  f.atlas.slot = 77;
  f.atlas.sampler = 5;
  f.atlas.width = tile * 2;
  f.atlas.height = tile * 2;
  f.atlas.tile_small = tile;
  f.atlas.tile_large = 0;
  f.count = views;
  for (uint32_t e = 0; e < views; ++e) {
    LocalShadowView& v = f.views[e];
    std::memset(&v, 0, sizeof(v));
    // out = M * (p, 1), w = z: uv = the tile's middle + 0.357 (x, y) / z (a field of 70 degrees across the tile), depth = A (1 - 5 / z) (near plane 5, far plane 50: 0 at z = 5, 0.97 at z = 40)
    const float u0 = (e % 2) * 0.5f, v0 = (e / 2) * 0.5f;
    constexpr float kA = 50.0f / 45.0f;
    v.world_to_shadow[0] = 0.25f / 0.7f; v.world_to_shadow[2] = u0 + 0.25f;
    v.world_to_shadow[5] = 0.25f / 0.7f; v.world_to_shadow[6] = v0 + 0.25f;
    v.world_to_shadow[10] = kA; v.world_to_shadow[11] = -kA * 5.0f;
    v.world_to_shadow[14] = 1.0f;
    v.atlas_rect[0] = u0; v.atlas_rect[1] = v0; v.atlas_rect[2] = u0 + 0.5f; v.atlas_rect[3] = v0 + 0.5f;
    v.depth_bias = rng.Range(0.0005f, 0.003f);
    v.texel_size = 1.0f / float(tile);
    v.light_index = e / 2;
    v.valid = 1;
    f.view_light[e] = 1000 + e / 2;
    f.view_layer[e] = ShadowLayer::kSingle;
  }
  return f;
}

void ShadowBufferChecks() {
  Lcg rng;
  float vi[16];
  lp::Rows rows;
  RandomPose(rng, vi, &rows);
  lp::PassInputs in;
  in.rows = rows;
  in.tan_x = 0.77f;
  in.tan_y = 0.43f;
  LinOf(0.1f, 10000.0f, in.lin);
  in.width = 1920;
  in.height = 1080;
  in.units.exposure = 16.0f;

  // the three levels of LIGHT SHADOWS (light_budget.h kThreeLevels): medium 2 cars with one view each, high 2 cars with the first's lamps apart (3 views), ultra 3 cars with every lamp's view (6)
  struct Level { const char* name; uint32_t cars; uint32_t lamp_cars; };
  for (const Level lv : {Level{"medium", 2, 0}, Level{"high", 2, 1}, Level{"ultra", 3, 3}}) {
    uint32_t views = 0;
    std::array<uint32_t, 4> first{}, count{};
    for (uint32_t c = 0; c < lv.cars; ++c) {
      first[c] = views;
      count[c] = c < lv.lamp_cars ? 2u : 1u;
      views += count[c];
    }
    ShadowFrame sf = MakeShadowFrame(views, 64, rng);
    lp::Selection s;
    for (uint32_t c = 0; c < 4; ++c) {
      Light l = HeadlightOf(1000 + c, float(c) * 5.0f);
      if (c < lv.cars) {
        l.shadow.kind = ShadowKind::kAtlas;
        l.shadow.first_view = uint8_t(first[c]);
        l.shadow.view_count = uint8_t(count[c]);
        l.shadow.weight = 1.0f;
      }
      s.lights[c] = l;
    }
    s.count = 4;  // the rig's four lights: the shadowed cars first, the rest unshadowed
    in.shadow = &sf;
    std::vector<uint8_t> bytes(lp::kBufferAlloc, 0xAB);
    lp::BuildBuffer(in, s, bytes.data());
    lp::PassHeader h;
    std::memcpy(&h, bytes.data(), sizeof(h));
    Check((h.flags & lp::kFlagShadow) != 0 && lp::ShadowTableNeeded(s, &sf), std::string("the table is in the buffer at the ") + lv.name + " level");
    shadow_lookup::GpuShadowBuffer want;
    shadow_lookup::BuildGpuShadowBuffer(sf, &want);
    Check(std::memcmp(bytes.data() + lp::kShadowOffset, &want, sizeof(want)) == 0, std::string("the table's bytes are BuildGpuShadowBuffer's, ") + lv.name);
    // what the shader reads at the LP_SHADOW_* offsets: the header, then each light's views (the seam's own arithmetic)
    uint32_t head[4];
    std::memcpy(head, bytes.data() + LP_SHADOW_OFF_COUNT, 16);
    Check(head[0] == views && head[1] == 77 && head[2] == sf.atlas.width && head[3] == sf.atlas.height, std::string("the header the shader loads: count, slot, width, height, ") + lv.name);
    uint32_t layers;
    std::memcpy(&layers, bytes.data() + LP_SHADOW_OFF_LAYERS, 4);
    Check(layers == 0, "every view is a single layer: the layers' bits are zero");
    bool views_same = true;
    for (uint32_t e = 0; e < views; ++e) {
      LocalShadowView got;
      std::memcpy(&got, bytes.data() + LP_SHADOW_VIEWS_OFFSET + e * LP_SHADOW_VIEW_BYTES, sizeof(got));
      views_same = views_same && std::memcmp(&got, &sf.views[e], sizeof(got)) == 0;
    }
    Check(views_same, std::string("each view is at LP_SHADOW_VIEWS_OFFSET + entry x 96, ") + lv.name);
    for (uint32_t c = 0; c < lv.cars; ++c) {
      GpuLight row;
      std::memcpy(&row, bytes.data() + lp::kLightsOffset + 64 * c, 64);
      const Light back = UnpackGpuLight(row);
      Check(back.shadow.kind == ShadowKind::kAtlas && back.shadow.first_view == first[c] && back.shadow.view_count == count[c], std::string("a shadowed car's light names its views in the row, ") + lv.name);
    }
    for (uint32_t c = lv.cars; c < 4; ++c) {
      GpuLight row;
      std::memcpy(&row, bytes.data() + lp::kLightsOffset + 64 * c, 64);
      Check(UnpackGpuLight(row).shadow.kind == ShadowKind::kNone, std::string("a car with no views is unshadowed in its row, ") + lv.name);
    }
  }

  // no table when nothing needs it: no atlas, no views, a stale reference, no light with views
  {
    ShadowFrame sf = MakeShadowFrame(2, 64, rng);
    lp::Selection s;
    s.lights[0] = HeadlightOf(1, 0.0f);
    s.lights[0].shadow.kind = ShadowKind::kAtlas;
    s.lights[0].shadow.first_view = 0;
    s.lights[0].shadow.view_count = 2;
    s.count = 1;
    Check(lp::ShadowTableNeeded(s, &sf), "a light with views in a valid atlas needs the table");
    Check(!lp::ShadowTableNeeded(s, nullptr), "no frame shadow: no table");
    ShadowFrame no_atlas = sf;
    no_atlas.atlas.valid = false;
    Check(!lp::ShadowTableNeeded(s, &no_atlas), "an atlas that is not valid: no table (the lights are lit)");
    ShadowFrame no_slot = sf;
    no_slot.atlas.slot = kNoLightSlot;
    Check(!lp::ShadowTableNeeded(s, &no_slot), "an atlas with no bindless slot: no table");
    ShadowFrame none = sf;
    none.count = 0;
    Check(!lp::ShadowTableNeeded(s, &none), "no views: no table");
    lp::Selection stale = s;
    stale.lights[0].shadow.first_view = 5;
    Check(!lp::ShadowTableNeeded(stale, &sf), "a light whose first view is past the table's count does not ask for it");
    lp::Selection plain = s;
    plain.lights[0].shadow.kind = ShadowKind::kNone;
    Check(!lp::ShadowTableNeeded(plain, &sf), "no light with views: no table");
    // the bytes of such a buffer: the flag clear and the table region zero
    in.shadow = &no_atlas;
    std::vector<uint8_t> bytes(lp::kBufferAlloc, 0xCD);
    lp::BuildBuffer(in, s, bytes.data());
    lp::PassHeader h;
    std::memcpy(&h, bytes.data(), sizeof(h));
    bool zero = true;
    for (uint32_t i = lp::kShadowOffset; i < lp::kBufferAlloc; ++i) {
      zero = zero && bytes[i] == 0;
    }
    Check((h.flags & lp::kFlagShadow) == 0 && zero, "without a table the flag is clear and its region is zero");
    in.shadow = nullptr;
  }
}

// The pass's pixel with a shadow against the lookup reading the buffer the way the shader does: the views come out of the buffer's bytes (the offsets of the seams), the atlas is a synthetic depth image.
void ShadowPixelChecks() {
  Lcg rng;
  float vi[16];
  lp::Rows rows;
  RandomPose(rng, vi, &rows);
  lp::PassInputs in;
  in.rows = rows;
  in.tan_x = 0.77f;
  in.tan_y = 0.43f;
  LinOf(0.1f, 10000.0f, in.lin);
  in.width = 1280;
  in.height = 720;
  in.units.exposure = 16.0f;
  uint64_t lit_pixels = 0, shadowed_pixels = 0, unshadowed_light = 0;
  for (int round = 0; round < 300; ++round) {
    const uint32_t views = 1 + rng.Below(3);
    ShadowFrame sf = MakeShadowFrame(views, 64, rng);
    // an atlas of random depth edges: a wall at a depth that cuts across the tiles, so that some taps are lit and some are not
    const float wall = rng.Range(0.3f, 0.7f);
    const float tilt = rng.Range(-0.4f, 0.4f);
    const auto depth_at = [&](int ix, int iy) {
      ix = std::clamp(ix, 0, int(sf.atlas.width) - 1);
      iy = std::clamp(iy, 0, int(sf.atlas.height) - 1);
      return wall + tilt * (float(ix) / float(sf.atlas.width) - 0.5f);
    };
    Light l = HeadlightOf(1000, 0.0f);
    l.pos[0] = 0.0f; l.pos[1] = 1.0f; l.pos[2] = -1.0f;
    l.dir[0] = 0.0f; l.dir[1] = -0.05f; l.dir[2] = 1.0f;
    const float len = std::sqrt(l.dir[1] * l.dir[1] + 1.0f);
    l.dir[1] /= len; l.dir[2] /= len;
    l.shadow.kind = ShadowKind::kAtlas;
    l.shadow.first_view = 0;
    l.shadow.view_count = uint8_t(views);
    l.shadow.weight = round % 5 == 0 ? rng.Range(0.2f, 1.0f) : 1.0f;
    lp::Selection s;
    s.lights[0] = l;
    s.count = 1;
    in.shadow = &sf;
    std::vector<uint8_t> bytes(lp::kBufferAlloc, 0);
    lp::BuildBuffer(in, s, bytes.data());
    lp::PassHeader h;
    std::memcpy(&h, bytes.data(), sizeof(h));
    const bool table = (h.flags & lp::kFlagShadow) != 0;
    // the table as the shader decodes it: the views and the layers out of the bytes
    ShadowFrame decoded;
    decoded.atlas = sf.atlas;
    uint32_t head[4];
    std::memcpy(head, bytes.data() + LP_SHADOW_OFF_COUNT, 16);
    uint32_t layers;
    std::memcpy(&layers, bytes.data() + LP_SHADOW_OFF_LAYERS, 4);
    decoded.atlas.slot = head[1];
    decoded.atlas.width = head[2];
    decoded.atlas.height = head[3];
    decoded.count = head[0];
    for (uint32_t e = 0; e < head[0]; ++e) {
      std::memcpy(&decoded.views[e], bytes.data() + LP_SHADOW_VIEWS_OFFSET + e * LP_SHADOW_VIEW_BYTES, sizeof(LocalShadowView));
      decoded.view_layer[e] = ShadowLayer((layers >> (2 * e)) & 3u);
    }
    ShadowRef ref;
    {
      GpuLight row;
      std::memcpy(&row, bytes.data() + lp::kLightsOffset, 64);
      ref = UnpackGpuLight(row).shadow;
    }
    for (int px = 0; px < 12; ++px) {
      float world[3] = {rng.Range(-8.0f, 8.0f), rng.Range(-2.0f, 3.0f), rng.Range(1.0f, 40.0f)};
      float n[3];
      rng.Direction(n);
      n[1] = std::fabs(n[1]);
      const float albedo[3] = {rng.Next(), rng.Next(), rng.Next()};
      // the shadow's plane takes the GEOMETRIC normal (QuadNormal), not the shading one the term takes: a direction of its own
      float sn[3];
      rng.Direction(sn);
      float with_table[3], direct[3], plain[3];
      Light from_row = l;
      from_row.shadow = ref;
      const bool a = lp::PixelLightShadowed(from_row, in.units, &decoded, table, world, n, albedo, 0.9f, depth_at, with_table, sn);
      const bool b = lp::PixelLightShadowed(l, in.units, &sf, true, world, n, albedo, 0.9f, depth_at, direct, sn);
      lp::PixelLight(l, in.units, world, n, albedo, 0.9f, plain);
      Check(a == b, "the shader's path (the table decoded from the buffer) and the frame's own views say the same about a pixel");
      bool same = true;
      for (int i = 0; i < 3; ++i) {
        same = same && with_table[i] == direct[i];
      }
      Check(same, "... and give the same light, bit for bit");
      const float visibility = shadow_lookup::LightLit(sf, l.shadow, world, sn, shadow_lookup::Mode::kSurface, depth_at);
      for (int i = 0; i < 3; ++i) {
        CheckNear(direct[i], double(plain[i]) * double(visibility), std::fabs(double(plain[i])) * 1.0e-6 + 1.0e-9, "the shadowed light is the term times the lookup's visibility (at the shadow normal)");
      }
      // no shadow normal given: the pixel's own normal is the shadow's
      float own[3];
      lp::PixelLightShadowed(l, in.units, &sf, true, world, n, albedo, 0.9f, depth_at, own);
      const float own_visibility = shadow_lookup::LightLit(sf, l.shadow, world, n, shadow_lookup::Mode::kSurface, depth_at);
      for (int i = 0; i < 3; ++i) {
        CheckNear(own[i], double(plain[i]) * double(own_visibility), std::fabs(double(plain[i])) * 1.0e-6 + 1.0e-9, "with no shadow normal the pixel's normal is the shadow's");
      }
      Check(visibility >= 0.0f && visibility <= 1.0f, "the visibility is a share");
      if (plain[0] > 0.0f || plain[1] > 0.0f || plain[2] > 0.0f) {
        ++lit_pixels;
        if (visibility < 0.999f) {
          ++shadowed_pixels;
          Check(direct[0] <= plain[0] && direct[1] <= plain[1] && direct[2] <= plain[2], "a shadow never adds light");
        } else {
          ++unshadowed_light;
        }
      }
    }
  }
  in.shadow = nullptr;
  Check(lit_pixels > 100 && shadowed_pixels > 20 && unshadowed_light > 20, "the random pixels have lit pixels in the open and in a shadow (" + std::to_string(lit_pixels) + " lit, " + std::to_string(shadowed_pixels) + " shadowed)");
  // a light with no views, or a frame with no table, is the term itself
  {
    Light l = HeadlightOf(5, 0.0f);
    l.pos[2] = -1.0f; l.dir[0] = 0.0f; l.dir[1] = 0.0f; l.dir[2] = 1.0f;
    const float world[3] = {0.0f, 0.0f, 10.0f}, n[3] = {0.0f, 0.0f, -1.0f}, albedo[3] = {1.0f, 1.0f, 1.0f};
    ShadowFrame sf;
    Lcg rng2;
    sf = MakeShadowFrame(1, 64, rng2);
    const auto black = [](int, int) { return 0.0f; };  // an atlas that shadows everything in its tiles
    float plain[3], none[3], off_table[3], full[3];
    lp::PixelLight(l, lp::Units{}, world, n, albedo, 1.0f, plain);
    Check(lp::PixelLightShadowed(l, lp::Units{}, &sf, true, world, n, albedo, 1.0f, black, none), "a light with no ShadowRef is lit whatever the atlas holds");
    Light shadowed = l;
    shadowed.shadow.kind = ShadowKind::kAtlas;
    shadowed.shadow.first_view = 0;
    shadowed.shadow.view_count = 1;
    shadowed.shadow.weight = 1.0f;
    Check(lp::PixelLightShadowed(shadowed, lp::Units{}, &sf, false, world, n, albedo, 1.0f, black, off_table) && off_table[0] == plain[0], "with no table in the buffer (the flag is clear) the light is lit");
    Check(!lp::PixelLightShadowed(shadowed, lp::Units{}, &sf, true, world, n, albedo, 1.0f, black, full), "a point in full shadow takes nothing: the pass discards it");
    Check(none[0] == plain[0] && none[1] == plain[1] && none[2] == plain[2], "the unshadowed light is exactly the term");
  }
}

// ---------------------------------------------------------------------------
// the HLSL twin
// ---------------------------------------------------------------------------

hlsl_shim::LlGpuLight ShimRow(const Light& l) {
  const GpuLight g = PackGpuLight(l);
  hlsl_shim::LlGpuLight r;
  r.row0 = hlsl_shim::float4(g.row0[0], g.row0[1], g.row0[2], g.row0[3]);
  r.row1 = hlsl_shim::float4(g.row1[0], g.row1[1], g.row1[2], g.row1[3]);
  r.row2 = hlsl_shim::float4(g.row2[0], g.row2[1], g.row2[2], g.row2[3]);
  r.packed0 = g.packed0;
  r.law_a = g.law_a;
  r.packed1 = g.packed1;
  r.shadow_weight = g.shadow_weight;
  return r;
}

// The globals the shader loads from the buffer's bytes (hlsl_shim::LpGlobals filled the way hlsl/local_light_ps.hlsl LpLoadGlobals does: the offsets are the LP_OFF_* defines).
hlsl_shim::LpGlobals ShimGlobals(const uint8_t* bytes) {
  const auto f = [&](uint32_t offset, int comp) {
    float v;
    std::memcpy(&v, bytes + offset + 4 * comp, 4);
    return v;
  };
  hlsl_shim::LpGlobals g;
  g.right = hlsl_shim::float3(f(LP_OFF_RIGHT, 0), f(LP_OFF_RIGHT, 1), f(LP_OFF_RIGHT, 2));
  g.tan_x = f(LP_OFF_RIGHT, 3);
  g.up = hlsl_shim::float3(f(LP_OFF_UP, 0), f(LP_OFF_UP, 1), f(LP_OFF_UP, 2));
  g.tan_y = f(LP_OFF_UP, 3);
  g.forward = hlsl_shim::float3(f(LP_OFF_FORWARD, 0), f(LP_OFF_FORWARD, 1), f(LP_OFF_FORWARD, 2));
  g.gain = f(LP_OFF_FORWARD, 3);
  g.position = hlsl_shim::float3(f(LP_OFF_POSITION, 0), f(LP_OFF_POSITION, 1), f(LP_OFF_POSITION, 2));
  g.night = f(LP_OFF_POSITION, 3);
  g.exposure = f(LP_OFF_EXPOSURE, 0);
  g.lin = hlsl_shim::float4(f(LP_OFF_LIN, 0), f(LP_OFF_LIN, 1), f(LP_OFF_LIN, 2), f(LP_OFF_LIN, 3));
  g.size = hlsl_shim::float4(f(LP_OFF_SIZE, 0), f(LP_OFF_SIZE, 1), f(LP_OFF_SIZE, 2), f(LP_OFF_SIZE, 3));
  g.occ = hlsl_shim::float4(f(LP_OFF_OCC, 0), f(LP_OFF_OCC, 1), f(LP_OFF_OCC, 2), f(LP_OFF_OCC, 3));
  return g;
}

void TwinChecks() {
  Lcg rng;
  for (int round = 0; round < 400; ++round) {
    float vi[16];
    lp::Rows rows;
    RandomPose(rng, vi, &rows);
    lp::PassInputs in;
    in.rows = rows;
    in.tan_x = rng.Range(0.4f, 1.1f);
    in.tan_y = in.tan_x * rng.Range(0.45f, 0.75f);
    LinOf(0.1f, 10000.0f, in.lin);
    in.width = rng.Below(2000) + 640;
    in.height = rng.Below(1200) + 360;
    in.units.night = rng.Range(0.0f, 1.0f);
    in.units.gain = rng.Range(0.5f, 2.0f);
    in.units.exposure = round % 2 == 0 ? 16.0f : rng.Range(0.5f, 20.0f);
    const bool occ_on = (round % 3) != 0;
    in.units.occ[0] = rng.Range(-20.0f, 20.0f);
    in.units.occ[1] = rng.Range(0.01f, 0.5f);
    in.units.occ[2] = rng.Range(0.4f, 1.6f);
    in.units.occ[3] = occ_on ? 1.0f : 0.0f;

    // a light of each law, random place and cone
    Light l;
    const uint32_t kind = rng.Below(3);
    if (kind == 0) {
      l = HeadlightOf(1, 0.0f);
      l.color[0] = rng.Range(0.2f, 1.5f); l.color[1] = rng.Range(0.2f, 1.5f); l.color[2] = rng.Range(0.2f, 1.5f);
    } else {
      const float pp[3] = {0.0f, 0.0f, 0.0f};
      l = GameLamp(pp, rng.Range(15.0f, 100.0f), kind == 1);
    }
    float dir[3];
    rng.Direction(dir);
    const float dist = rng.Range(0.0f, 60.0f);
    for (int i = 0; i < 3; ++i) {
      l.pos[i] = rows.position[i] + dir[i] * dist;
    }
    rng.Direction(l.dir);

    // a pixel, its depth, its surface
    const float px = float(rng.Below(in.width)) + 0.5f;
    const float py = float(rng.Below(in.height)) + 0.5f;
    const float t = rng.Range(1.0f, 80.0f);
    const float stored = StoredOf(t, 0.1f, 10000.0f);
    float n[3];
    rng.Direction(n);
    const float albedo[3] = {rng.Next(), rng.Next(), rng.Next()};
    const float lit = rng.Next() < 0.2f ? 0.0f : rng.Range(0.3f, 1.0f);

    // the buffer's bytes, then both sides from them
    lp::Selection s;
    s.lights[0] = l;
    s.count = 1;
    std::vector<uint8_t> bytes(lp::kBufferAlloc, 0);
    lp::BuildBuffer(in, s, bytes.data());
    const hlsl_shim::LpGlobals g = ShimGlobals(bytes.data());
    CheckRel(hlsl_shim::LpLinearDepth(stored, g.lin), lp::LinearDepth(stored, in.lin), 1.0e-5, 1.0e-6, "the twin's linear depth");
    const float t_twin = hlsl_shim::LpLinearDepth(stored, g.lin);
    const hlsl_shim::float3 wt = hlsl_shim::LpWorldPosition(g, px, py, t_twin);
    float wc[3];
    lp::WorldPosition(rows, in.tan_x, in.tan_y, px, py, float(in.width), float(in.height), lp::LinearDepth(stored, in.lin), wc);
    CheckRel(wt.x, wc[0], 1.0e-5, 1.0e-4, "the twin's world position x");
    CheckRel(wt.y, wc[1], 1.0e-5, 1.0e-4, "the twin's world position y");
    CheckRel(wt.z, wc[2], 1.0e-5, 1.0e-4, "the twin's world position z");
    CheckRel(hlsl_shim::LpOcclusion(g.occ, wc[1]), lp::OcclusionFactor(in.units.occ, wc[1]), 1.0e-6, 1.0e-6, "the twin's height factor");

    hlsl_shim::LlLight ul = hlsl_shim::LlUnpack(ShimRow(l));
    const hlsl_shim::float3 got = hlsl_shim::LpPixelLight(ul, g, hlsl_shim::float3(wc[0], wc[1], wc[2]), hlsl_shim::float3(n[0], n[1], n[2]),
                                                          hlsl_shim::float3(albedo[0], albedo[1], albedo[2]), lit);
    float want[3];
    lp::PixelLight(l, in.units, wc, n, albedo, lit, want);
    CheckRel(got.x, want[0], 1.0e-5, 1.0e-7, Num("the twin's light, red (round %.0f)", round));
    CheckRel(got.y, want[1], 1.0e-5, 1.0e-7, "the twin's light, green");
    CheckRel(got.z, want[2], 1.0e-5, 1.0e-7, "the twin's light, blue");
  }
}

// ---------------------------------------------------------------------------
// GB-NEON-COST: the reach test before the planes, and one draw for a car's tube
// ---------------------------------------------------------------------------
// The claims, each held here to numbers:
//   1. THE REACH TEST (hlsl LpReaches) is exact: a pixel it rejects takes nothing from the light under any of the three laws, whatever the surface; one it accepts goes the whole way as before. (The
//      pixel shader asks it before the planes: the pixels it rejects are the ones the old order read the planes of and then discarded.)
//   2. THE ORDER of the shader (reach first, or the planes first: push.info.w) gives the same pixel: a C++ transcription of the shader's main, both orders, on random pixels, surfaces and draws of one
//      to four lights, bit for bit the same sum and the same discards.
//   3. THE SUM: one draw that sums a tube's lights writes the pixels the single draws wrote and the same colour up to the rounding of the half float target (the single draws round after each add).
//   4. THE DRAWS (PlanDraws): which lights go together, the union of their rectangles holds each light's own, a tube of a chase view is one full picture draw where there were four.

// A float rounded to the nearest binary16 value (ties to even; the colour target's precision after a blend), and the size of one step at a value.
float RoundHalf(float v) {
  if (!std::isfinite(v) || v == 0.0f) {
    return v;
  }
  const double a = std::fabs(double(v));
  int e = 0;
  std::frexp(a, &e);                  // a = m 2^e, m in [0.5, 1)
  const int exp2 = std::max(e - 1, -14);  // the denormals share the smallest normal's step
  const double quantum = std::ldexp(1.0, exp2 - 10);
  double q = std::nearbyint(a / quantum) * quantum;
  if (q > 65504.0) {
    q = HUGE_VAL;
  }
  return v < 0.0f ? -float(q) : float(q);
}
double HalfStep(double v) {
  int e = 0;
  std::frexp(std::fabs(v), &e);
  return std::ldexp(1.0, std::max(e - 1, -14) - 10);
}

bool AnyPositive(const hlsl_shim::float3& v) { return v.x > 0.0f || v.y > 0.0f || v.z > 0.0f; }

// One pixel as the shader sees it: what the depth plane and the three planes hold.
struct PixelData {
  float stored = 0.0f;        // the main depth plane (0: the sky)
  bool rt1_valid = true;
  float normal[3] = {0.0f, 1.0f, 0.0f};
  float albedo[3] = {0.5f, 0.5f, 0.5f};
  bool rt2_any = true;        // RT2 has any channel over 0
  float lit = 1.0f;           // RT2.w
  uint32_t surface_class = 1;  // RT3's class (GB_CLASS_*)
  bool vehicle = false;        // RT3's vehicle flag (GB-NEON-BLEED)
  std::function<float(int, int)> field;  // the main depth plane round the pixel, for the contact term's reads (empty: the sky everywhere)
};
struct FlowResult {
  bool written = false;
  hlsl_shim::float3 sum;
  uint32_t depth_reads = 0;  // the contact term's reads of the depth plane (GB-NEON-BLEED; the pixel's own read is not counted)
};

// hlsl/local_light_ps.hlsl main, line by line, on the twin's functions: `lights` are the draw's rows (count 1 to 4), `material` is LP_FLAG_MATERIAL with RT3 bound, `reach_first` the shader's order.
// The shadow lookup is left out (nothing of the neon has views of the atlas, and the order does not touch it). GB-NEON-BLEED: RT3's vehicle flag is read with its class, and a neon light on a vehicle
// pixel takes the contact term inside its own turn of the sum (LpOneLight: after the law's term, before the shadow), its reads counted.
FlowResult ShaderFlow(const PixelData& px, const hlsl_shim::LpGlobals& g, const hlsl_shim::LlLight* lights, uint32_t count, bool material, bool reach_first, float px_x, float px_y) {
  FlowResult none;
  hlsl_shim::float3 world;
  const auto place = [&]() {
    if (!(px.stored > 0.0f)) {
      return false;
    }
    const float t = hlsl_shim::LpLinearDepth(px.stored, g.lin);
    if (!(t < LP_SKY)) {
      return false;
    }
    world = hlsl_shim::LpWorldPosition(g, px_x, px_y, t);
    return true;
  };
  if (reach_first) {
    if (!place()) {
      return none;
    }
    bool any = false;
    for (uint32_t i = 0; i < count; ++i) {
      any = any || hlsl_shim::LpReaches(lights[i].pos, lights[i].range, world);
    }
    if (!any) {
      return none;
    }
  }
  if (!px.rt1_valid) {
    return none;
  }
  if (!px.rt2_any || !(px.lit > 0.0f)) {
    return none;
  }
  bool vehicle = false;
  if (material) {
    if (px.surface_class == uint32_t(LP_CLASS_NONE) || px.surface_class == uint32_t(LP_CLASS_EMISSIVE)) {
      return none;
    }
    vehicle = px.vehicle;
  }
  if (!reach_first && !place()) {
    return none;
  }
  FlowResult r;
  const hlsl_shim::float3 n(px.normal[0], px.normal[1], px.normal[2]);
  const hlsl_shim::float3 a(px.albedo[0], px.albedo[1], px.albedo[2]);
  for (uint32_t i = 0; i < count; ++i) {
    hlsl_shim::float3 add = hlsl_shim::LpPixelLight(lights[i], g, world, n, a, px.lit);
    if (!AnyPositive(add)) {
      continue;
    }
    if (hlsl_shim::LpTakesContact(lights[i], vehicle)) {
      g_stored_depth = [&](int ix, int iy) {
        ++r.depth_reads;
        return px.field ? px.field(ix, iy) : 0.0f;
      };
      add = add * hlsl_shim::LpContactVisibility(g, world, n, lights[i].pos);
      if (!AnyPositive(add)) {
        continue;
      }
    }
    r.sum = r.sum + add;
    r.written = true;
  }
  return r;
}

// A neon tube of `points` omni lights under a car at `centre`, along `heading`, 4.5 m long: the lights the pass makes (local_lights_sources.h MakeNeonTubeLights) in the car's colour.
std::vector<Light> TubeLights(const float centre[3], const float heading[3], const float colour[3], uint32_t points, uint32_t car, lp::Tier tier) {
  neon_light::NeonCar car_data;
  car_data.object = 0x40000000u + car * 0x1000u;
  car_data.on = true;
  for (int i = 0; i < 3; ++i) {
    car_data.colour[i] = colour[i];
  }
  neon_light::Tube tube;
  tube.space = neon_light::Space::kWorld;
  tube.length = 4.5f;
  for (int i = 0; i < 3; ++i) {
    tube.a[i] = centre[i] - heading[i] * 2.25f;
    tube.b[i] = centre[i] + heading[i] * 2.25f;
  }
  std::vector<Light> out;
  lp::MakeNeonTubeLights(car_data, tube, points, tier, 1.0f, &out);
  return out;
}

void CostChecks() {
  // ---- 1 and 2 and 3: random pixels, surfaces and draws -------------------------------------------------------------------------------------------
  Lcg rng;
  uint64_t pixels = 0, rejected = 0, lit_pixels = 0, written_both = 0;
  uint64_t contact_pixels = 0, contact_reads = 0, contact_spared = 0;
  double worst_steps = 0.0, worst_rel = 0.0;
  for (int round = 0; round < 6000; ++round) {
    float vi[16];
    lp::Rows rows;
    RandomPose(rng, vi, &rows);
    lp::PassInputs in;
    in.rows = rows;
    in.tan_x = rng.Range(0.4f, 1.1f);
    in.tan_y = in.tan_x * rng.Range(0.45f, 0.75f);
    LinOf(0.1f, 10000.0f, in.lin);
    in.width = 640 + rng.Below(1900);
    in.height = 360 + rng.Below(1100);
    in.units.night = rng.Range(0.2f, 3.0f);
    in.units.gain = rng.Range(0.5f, 2.0f);
    in.units.exposure = 16.0f;
    in.units.occ[0] = rng.Range(-20.0f, 20.0f);
    in.units.occ[1] = rng.Range(0.01f, 0.5f);
    in.units.occ[2] = rng.Range(0.4f, 1.6f);
    in.units.occ[3] = (round % 3) != 0 ? 1.0f : 0.0f;

    // a pixel: its place, its depth (some are the sky), its surface (some have no normal, no albedo, no lit weight, an emissive class, no class)
    const float px = float(rng.Below(in.width)) + 0.5f;
    const float py = float(rng.Below(in.height)) + 0.5f;
    PixelData pd;
    const float t = rng.Range(0.5f, 60.0f);
    pd.stored = rng.Next() < 0.12f ? 0.0f : StoredOf(t, 0.1f, 10000.0f);
    rng.Direction(pd.normal);
    for (float& c : pd.albedo) c = rng.Next();
    pd.rt1_valid = rng.Next() > 0.08f;
    pd.rt2_any = rng.Next() > 0.08f;
    pd.lit = rng.Next() < 0.1f ? 0.0f : rng.Range(0.3f, 1.0f);
    pd.surface_class = rng.Next() < 0.15f ? (rng.Next() < 0.5f ? 0u : 13u) : 1u + rng.Below(12);
    float wc[3];
    lp::WorldPosition(rows, in.tan_x, in.tan_y, px, py, float(in.width), float(in.height), lp::LinearDepth(pd.stored, in.lin), wc);
    // GB-NEON-BLEED: some pixels are a car's (RT3's vehicle flag), with a ragged depth plane round them (a shell in front of the contact term's points, open air, the sky). A random stream of its own, so
    // that the draws above are the ones this check made before the contact term joined the shader.
    {
      Lcg vr;
      vr.state = 424242u + uint32_t(round) * 7919u;
      pd.vehicle = vr.Next() < 0.45f;
      const float base_t = t + vr.Range(-0.8f, 0.8f);
      const float amp = vr.Range(0.0f, 0.5f);
      const float k1 = vr.Range(0.005f, 0.06f), k2 = vr.Range(0.005f, 0.06f);
      pd.field = [=](int ix, int iy) -> float {
        if (((ix / 31) + (iy / 29)) % 7 == 0) {
          return 0.0f;  // the sky
        }
        const float d = base_t + amp * std::sin(float(ix) * k1 + float(iy) * k2) + (((ix / 11) + (iy / 13)) % 2 == 0 ? 0.0f : 0.3f);
        return StoredOf(std::max(d, 0.2f), 0.1f, 10000.0f);
      };
    }

    // the draw: one to four omni lights of one tube (the neon's laws and numbers) round that point, or a lamp / headlight of any law for the first claim
    const uint32_t n = 1 + rng.Below(4);
    std::vector<Light> lights;
    if ((round & 3) != 3) {
      const float heading[3] = {1.0f, 0.0f, 0.0f};
      float centre[3];
      float dir[3];
      rng.Direction(dir);
      const float reach = lp::NeonReach(round % 2 ? lp::Tier::kUltra : lp::Tier::kMedium);
      const float off = rng.Range(0.0f, 1.6f * reach);
      for (int i = 0; i < 3; ++i) centre[i] = wc[i] + dir[i] * off;
      const float colour[3] = {rng.Range(0.0f, 2.0f), rng.Range(0.0f, 2.0f), rng.Range(0.0f, 2.0f)};
      lights = TubeLights(centre, heading, colour, n, 1, lp::Tier::kHigh);
    } else {
      const uint32_t kind = rng.Below(3);
      for (uint32_t k = 0; k < n; ++k) {
        Light l;
        if (kind == 0) {
          l = HeadlightOf(1, 0.0f);
          l.color[0] = rng.Range(0.2f, 1.5f); l.color[1] = rng.Range(0.2f, 1.5f); l.color[2] = rng.Range(0.2f, 1.5f);
        } else {
          const float pp[3] = {0.0f, 0.0f, 0.0f};
          l = GameLamp(pp, rng.Range(15.0f, 100.0f), kind == 1);
        }
        float dir[3];
        rng.Direction(dir);
        const float off = rng.Range(0.0f, 1.5f) * l.range;
        for (int i = 0; i < 3; ++i) l.pos[i] = wc[i] + dir[i] * off;
        rng.Direction(l.dir);
        lights.push_back(l);
      }
    }
    lp::Selection s;
    for (const Light& l : lights) s.lights[s.count++] = l;
    std::vector<uint8_t> bytes(lp::kBufferAlloc, 0);
    lp::BuildBuffer(in, s, bytes.data());
    const hlsl_shim::LpGlobals g = ShimGlobals(bytes.data());
    std::vector<hlsl_shim::LlLight> rows_of;
    for (const Light& l : lights) rows_of.push_back(hlsl_shim::LlUnpack(ShimRow(l)));
    const uint32_t count = uint32_t(rows_of.size());
    const bool material = (round % 2) == 0;

    // 1: a light the test rejects adds nothing at the pixel's world position, under its own law
    if (pd.stored > 0.0f) {
      const hlsl_shim::float3 w(wc[0], wc[1], wc[2]);
      const hlsl_shim::float3 nrm(pd.normal[0], pd.normal[1], pd.normal[2]);
      const hlsl_shim::float3 alb(pd.albedo[0], pd.albedo[1], pd.albedo[2]);
      for (uint32_t k = 0; k < count; ++k) {
        ++pixels;
        const bool reaches = hlsl_shim::LpReaches(rows_of[k].pos, rows_of[k].range, w);
        const hlsl_shim::float3 add = hlsl_shim::LpPixelLight(rows_of[k], g, w, nrm, alb, pd.lit);
        if (!reaches) {
          ++rejected;
          Check(!AnyPositive(add) && add.x == 0.0f && add.y == 0.0f && add.z == 0.0f, Num("a light the reach test rejects adds exactly 0 (round %.0f)", round));
        }
        const float dx = rows_of[k].pos.x - w.x, dy = rows_of[k].pos.y - w.y, dz = rows_of[k].pos.z - w.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        Check(reaches == !(d2 >= rows_of[k].range * rows_of[k].range), "the reach test is the negation of the laws' own test");
      }
    }

    // 2: the two orders of the shader are the same pixel
    const FlowResult fast = ShaderFlow(pd, g, rows_of.data(), count, material, true, px, py);
    const FlowResult slow = ShaderFlow(pd, g, rows_of.data(), count, material, false, px, py);
    Check(fast.written == slow.written, Num("the early reach test keeps the pixels the planes-first order writes (round %.0f)", round));
    Check(fast.sum.x == slow.sum.x && fast.sum.y == slow.sum.y && fast.sum.z == slow.sum.z, "the early reach test gives bit for bit the same sum");
    // GB-NEON-BLEED with GB-NEON-COST: the contact term reads the depth only for a vehicle pixel a neon light of the draw really lights, in either order; a pixel the reach test rejects reads none
    Check(fast.depth_reads == slow.depth_reads, Num("the two orders make the same contact reads (round %.0f)", round));
    if (pd.stored > 0.0f) {
      bool any_reach = false;
      for (uint32_t k = 0; k < count; ++k) {
        any_reach = any_reach || hlsl_shim::LpReaches(rows_of[k].pos, rows_of[k].range, hlsl_shim::float3(wc[0], wc[1], wc[2]));
      }
      if (!any_reach) {
        Check(!fast.written && !slow.written && fast.depth_reads == 0 && slow.depth_reads == 0, "a pixel no light of the draw reaches is discarded with no contact read");
        ++contact_spared;
      }
    }
    if (fast.depth_reads != 0) {
      ++contact_pixels;
      contact_reads += fast.depth_reads;
      Check(pd.vehicle && material, "only a vehicle pixel with RT3 takes contact reads");
    }

    // 3: the single draws (one add into the half float target each, a light that adds nothing is a discarded pixel) against the one draw's sum
    if (pd.stored > 0.0f && pd.rt1_valid && pd.rt2_any && pd.lit > 0.0f && !(material && (pd.surface_class == 0u || pd.surface_class == 13u))) {
      const float base[3] = {RoundHalf(rng.Range(0.0f, 8.0f)), RoundHalf(rng.Range(0.0f, 8.0f)), RoundHalf(rng.Range(0.0f, 8.0f))};
      float old_fb[3] = {base[0], base[1], base[2]};
      bool old_written = false;
      const hlsl_shim::float3 nrm(pd.normal[0], pd.normal[1], pd.normal[2]);
      const hlsl_shim::float3 alb(pd.albedo[0], pd.albedo[1], pd.albedo[2]);
      const hlsl_shim::float3 w(wc[0], wc[1], wc[2]);
      for (uint32_t k = 0; k < count; ++k) {
        hlsl_shim::float3 add = hlsl_shim::LpPixelLight(rows_of[k], g, w, nrm, alb, pd.lit);
        if (!AnyPositive(add)) {
          continue;
        }
        if (hlsl_shim::LpTakesContact(rows_of[k], material && pd.vehicle)) {  // the single draw of the neon-bleed shader: its own contact term
          g_stored_depth = pd.field;
          add = add * hlsl_shim::LpContactVisibility(g, w, nrm, rows_of[k].pos);
          if (!AnyPositive(add)) {
            continue;
          }
        }
        old_fb[0] = RoundHalf(old_fb[0] + add.x);
        old_fb[1] = RoundHalf(old_fb[1] + add.y);
        old_fb[2] = RoundHalf(old_fb[2] + add.z);
        old_written = true;
      }
      Check(old_written == fast.written, Num("the one draw writes the pixels the single draws wrote (round %.0f)", round));
      if (fast.written) {
        ++written_both;
        const float new_fb[3] = {RoundHalf(base[0] + fast.sum.x), RoundHalf(base[1] + fast.sum.y), RoundHalf(base[2] + fast.sum.z)};
        for (int c = 0; c < 3; ++c) {
          const double top = std::max(std::fabs(double(old_fb[c])), std::fabs(double(new_fb[c])));
          const double steps = std::fabs(double(old_fb[c]) - double(new_fb[c])) / HalfStep(top);
          worst_steps = std::max(worst_steps, steps);
          if (top > 0.0) worst_rel = std::max(worst_rel, std::fabs(double(old_fb[c]) - double(new_fb[c])) / top);
          // each single add rounds to within half a step of a value that is at most the final one, and the sum rounds once: n + 1 half steps at most
          Check(steps <= 0.5 * double(count + 1) + 1.0e-9, Num("the sum differs from the single draws by %.3f half float steps (round %.0f)", steps, round));
        }
        ++lit_pixels;
      }
    }
  }
  std::printf("cost twin: %llu light-pixels, %llu rejected by the reach test (%.1f %%); %llu written pixels, the one draw's sum is at most %.2f half float steps (%.4f %%) from the single draws'\n",
              (unsigned long long)pixels, (unsigned long long)rejected, pixels ? 100.0 * double(rejected) / double(pixels) : 0.0, (unsigned long long)written_both, worst_steps, 100.0 * worst_rel);
  Check(rejected > 1000 && lit_pixels > 500, "the random draws exercise both sides of the reach test");
  std::printf("cost twin with the contact term: %llu pixels took %llu contact reads (a vehicle pixel a neon light lights); %llu pixels no light of the draw reaches made none\n", (unsigned long long)contact_pixels,
              (unsigned long long)contact_reads, (unsigned long long)contact_spared);
  Check(contact_pixels > 300 && contact_reads >= contact_pixels && contact_spared > 300, "the random draws exercise the contact term inside the sum, and the pixels the reach test spares");
  Check(worst_rel < 0.0035, "the sum is within 0.35 % of the single draws' (a few steps of the half float target: 8-bit pictures cannot show it)");

  // The boundary of the reach, to the bit: a pixel at exactly the range is not reached and takes 0 under every law; one a step inside is reached
  for (int law = 0; law < 3; ++law) {
    Light l;
    if (law == 0) {
      l = HeadlightOf(1, 0.0f);
    } else {
      const float pp[3] = {0.0f, 0.0f, 0.0f};
      l = GameLamp(pp, 50.0f, law == 2);
    }
    for (int i = 0; i < 3; ++i) l.pos[i] = 0.0f;
    l.range = 8.0f;
    l.dir[0] = 0.0f; l.dir[1] = -1.0f; l.dir[2] = 0.0f;
    const hlsl_shim::LlLight row = hlsl_shim::LlUnpack(ShimRow(l));
    lp::PassInputs in;
    in.rows = lp::Rows{};
    in.tan_x = 1.0f;
    in.tan_y = 0.5625f;
    LinOf(0.1f, 10000.0f, in.lin);
    in.width = 1920;
    in.height = 1080;
    in.units.night = 1.0f;
    in.units.gain = 1.0f;
    in.units.exposure = 16.0f;
    lp::Selection s;
    s.lights[0] = l;
    s.count = 1;
    std::vector<uint8_t> bytes(lp::kBufferAlloc, 0);
    lp::BuildBuffer(in, s, bytes.data());
    const hlsl_shim::LpGlobals g = ShimGlobals(bytes.data());
    const hlsl_shim::float3 up(0.0f, 1.0f, 0.0f);
    const hlsl_shim::float3 white(1.0f, 1.0f, 1.0f);
    const hlsl_shim::float3 at_range(0.0f, -8.0f, 0.0f);
    const hlsl_shim::float3 inside(0.0f, std::nextafter(-8.0f, 0.0f), 0.0f);
    Check(!hlsl_shim::LpReaches(row.pos, row.range, at_range), "a pixel at exactly the range is not reached");
    Check(!AnyPositive(hlsl_shim::LpPixelLight(row, g, at_range, up, white, 1.0f)), "and takes nothing under any law");
    Check(hlsl_shim::LpReaches(row.pos, row.range, inside), "a pixel a step inside the range is reached");
    // a range that is not a number: the laws go on with it, so the test must not leave the pixel out
    hlsl_shim::LlLight nan_row = row;
    nan_row.range = std::nanf("");
    Check(hlsl_shim::LpReaches(nan_row.pos, nan_row.range, at_range), "a light whose range is not a number is never left out early (the laws' own test is false for it)");
    hlsl_shim::LlLight off_row = row;
    off_row.range = 0.0f;
    Check(!hlsl_shim::LpReaches(off_row.pos, off_row.range, inside), "a light of no range reaches nothing");
  }

  // ---- 4: the draws ------------------------------------------------------------------------------------------------------------------------------
  // a chase view: the camera 6 m behind a car and 1.8 m up, looking along it; the car's tube under it, four points; the picture 1920 x 1104 as the cost run's
  {
    const uint32_t w = 1920, h = 1104;
    float vi[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.8f, 6.0f, 1};  // the camera at (0, 1.8, 6) looking along -z, the car at the origin
    lp::Rows rows;
    Check(lp::RowsFromViewInverse(vi, &rows), "the chase camera is a camera");
    const float tan_x = 0.829f, tan_y = 0.466f;  // the cost run's lens
    const float centre[3] = {0.0f, 0.25f, 0.0f};
    const float heading[3] = {0.0f, 0.0f, 1.0f};
    const float colour[3] = {1.0f, 0.1f, 0.3f};
    for (const lp::Tier tier : {lp::Tier::kMedium, lp::Tier::kHigh, lp::Tier::kUltra}) {
      const uint32_t points = tier == lp::Tier::kUltra ? 4u : (tier == lp::Tier::kHigh ? 3u : 2u);
      const std::vector<Light> tube = TubeLights(centre, heading, colour, points, 1, tier);
      lp::Selection s;
      for (const Light& l : tube) s.lights[s.count++] = l;
      Check(s.count == points && tube[0].car_key != 0 && tube[0].car_key == tube[points - 1].car_key && tube[0].source == LightSource::kNeon, "a tube's lights are the neon's, one car's");
      std::array<lp::PixelRect, lp::kMaxPassLights> rects;
      uint64_t single_pixels = 0;
      for (uint32_t i = 0; i < s.count; ++i) {
        rects[i] = lp::ScissorOfLight(s.lights[i], rows, tan_x, tan_y, w, h);
        single_pixels += rects[i].Area();
      }
      std::array<lp::PassDraw, lp::kMaxPassLights> plan;
      const uint32_t merged = lp::PlanDraws(s, rects.data(), 0, plan.data());
      Check(merged == 1 && plan[0].first == 0 && plan[0].count == points, "a car's tube is one draw");
      Check(plan[0].rect.Area() <= single_pixels, "its rectangle is the union of the lights' own: no more pixels than the single draws made together");
      if (tier == lp::Tier::kUltra) {
        Check(plan[0].rect.Area() * points == single_pixels, "at ultra every light of the chase view is a full picture draw, so the pixels drawn fall to one over the number of lights (a quarter)");
      }
      for (uint32_t i = 0; i < s.count; ++i) {
        Check(plan[0].rect.x0 <= rects[i].x0 && plan[0].rect.y0 <= rects[i].y0 && plan[0].rect.x1 >= rects[i].x1 && plan[0].rect.y1 >= rects[i].y1, "the draw's rectangle holds each light's own");
      }
      std::array<lp::PassDraw, lp::kMaxPassLights> plan_off;
      const uint32_t single = lp::PlanDraws(s, rects.data(), lp::kCostOffMerge, plan_off.data());
      Check(single == points, "with the merge switched off every light is a draw of its own");
      for (uint32_t d = 0; d < single; ++d) {
        Check(plan_off[d].first == d && plan_off[d].count == 1 && plan_off[d].rect.x0 == rects[d].x0 && plan_off[d].rect.y1 == rects[d].y1, "the old way: one light, its own rectangle");
      }
      std::printf("cost model: chase view, tier %s: %u lights = %u full-size draws of %llu pixels (%.2f MPixels) -> %u draw of %llu pixels (%.2f MPixels)\n", tier == lp::Tier::kUltra ? "ultra" : (tier == lp::Tier::kHigh ? "high" : "medium"),
                  s.count, single, (unsigned long long)rects[0].Area(), double(single_pixels) * 1.0e-6, merged, (unsigned long long)plan[0].rect.Area(), double(plan[0].rect.Area()) * 1.0e-6);
    }
  }
  // the budget's four cars of the worst scene: four tubes of four lights are four draws (the cars' keys differ), the nearest car's first
  {
    lp::Selection s;
    const float heading[3] = {0.0f, 0.0f, 1.0f};
    const float colour[3] = {0.2f, 0.8f, 1.0f};
    for (uint32_t car = 0; car < 4; ++car) {
      const float centre[3] = {float(car) * 3.0f - 4.5f, 0.25f, -float(car) * 7.0f};
      for (const Light& l : TubeLights(centre, heading, colour, 4, car + 1, lp::Tier::kUltra)) s.lights[s.count++] = l;
    }
    std::array<lp::PixelRect, lp::kMaxPassLights> rects;
    float vi[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.8f, 8.0f, 1};
    lp::Rows rows;
    lp::RowsFromViewInverse(vi, &rows);
    for (uint32_t i = 0; i < s.count; ++i) rects[i] = lp::ScissorOfLight(s.lights[i], rows, 0.829f, 0.466f, 1920, 1104);
    std::array<lp::PassDraw, lp::kMaxPassLights> plan;
    const uint32_t n = lp::PlanDraws(s, rects.data(), 0, plan.data());
    Check(n == 4, "four cars of four lights are four draws");
    for (uint32_t d = 0; d < n; ++d) {
      Check(plan[d].first == d * 4 && plan[d].count == 4, "each draw is one car's four lights, in the selection's order");
    }
    Check(lp::PlanDraws(s, rects.data(), lp::kCostOffMerge, plan.data()) == 16, "sixteen lights, sixteen draws with the merge off");
    Check(lp::PlanDraws(s, rects.data(), lp::kCostOffReachFirst, plan.data()) == 4, "the other bit (the reach test) does not change the draws");
  }
  // what does not merge: another source in the run, no car key, another car's key, a shadowed light, a run longer than a tube, a light off the picture
  {
    lp::Selection s;
    const float heading[3] = {1.0f, 0.0f, 0.0f};
    const float colour[3] = {1.0f, 1.0f, 1.0f};
    const float c0[3] = {0.0f, 0.25f, -10.0f};
    std::vector<Light> tube = TubeLights(c0, heading, colour, 4, 1, lp::Tier::kUltra);
    std::array<lp::PixelRect, lp::kMaxPassLights> rects;
    for (auto& r : rects) {
      r.x0 = 100; r.y0 = 100; r.x1 = 300; r.y1 = 200;
    }
    std::array<lp::PassDraw, lp::kMaxPassLights> plan;
    const auto plan_of = [&](const std::vector<Light>& lights) {
      lp::Selection sel;
      for (const Light& l : lights) sel.lights[sel.count++] = l;
      return lp::PlanDraws(sel, rects.data(), 0, plan.data());
    };
    Check(plan_of(tube) == 1, "(control) the tube merges");
    std::vector<Light> mixed = tube;
    mixed[2].source = LightSource::kPolice;
    Check(plan_of(mixed) == 3 && plan[0].count == 2 && plan[1].count == 1 && plan[2].count == 1, "another source in the run splits it");
    std::vector<Light> no_key = tube;
    for (Light& l : no_key) l.car_key = 0;
    Check(plan_of(no_key) == 4, "lights with no car key are draws of their own");
    std::vector<Light> two_cars = tube;
    two_cars[2].car_key ^= 0x10u;
    two_cars[3].car_key ^= 0x10u;
    Check(plan_of(two_cars) == 2 && plan[0].count == 2 && plan[1].count == 2, "another car's lights are another draw");
    std::vector<Light> shadowed = tube;
    shadowed[1].shadow.kind = ShadowKind::kAtlas;
    shadowed[1].shadow.view_count = 1;
    Check(plan_of(shadowed) == 3 && plan[0].count == 1 && plan[1].count == 1 && plan[2].count == 2, "a light with views of the atlas is a draw of its own");
    std::vector<Light> long_run = tube;
    for (int i = 0; i < 3; ++i) long_run.push_back(tube[i]);
    Check(plan_of(long_run) == 2 && plan[0].count == 4 && plan[1].count == 3, "a run of seven lights is a draw of four and a draw of three");
    // a light off the picture is not in the union and a draw with every light off the picture is left out
    rects[1] = lp::PixelRect{};
    rects[1].x1 = 0;
    Check(plan_of(tube) == 1 && plan[0].count == 4 && plan[0].rect.x0 == 100 && plan[0].rect.x1 == 300, "a light off the picture takes no part in the union, but stays a light of the draw");
    for (int i = 0; i < 4; ++i) rects[i] = lp::PixelRect{};
    Check(plan_of(tube) == 0, "a draw whose lights are all off the picture is left out");
  }
  // the union holds every point any light of the draw can light (the scissor's own property, per light, carried to the union), on random poses
  {
    Lcg r2;
    r2.state = 7771u;
    for (int round = 0; round < 80; ++round) {
      float vi[16];
      lp::Rows rows;
      RandomPose(r2, vi, &rows);
      const float tan_x = r2.Range(0.4f, 1.0f), tan_y = tan_x * 0.5625f;
      const uint32_t w = 1920, h = 1080;
      float centre[3], dir[3];
      r2.Direction(dir);
      const float dist = r2.Range(2.0f, 40.0f);
      for (int i = 0; i < 3; ++i) centre[i] = rows.position[i] + dir[i] * dist;
      float heading[3];
      r2.Direction(heading);
      heading[1] = 0.0f;
      const float hn = std::sqrt(heading[0] * heading[0] + heading[2] * heading[2]);
      if (!(hn > 0.1f)) continue;
      heading[0] /= hn; heading[2] /= hn;
      const float colour[3] = {1.0f, 0.5f, 0.2f};
      lp::Selection s;
      for (const Light& l : TubeLights(centre, heading, colour, 1 + r2.Below(4), 3, lp::Tier::kUltra)) s.lights[s.count++] = l;
      std::array<lp::PixelRect, lp::kMaxPassLights> rects;
      for (uint32_t i = 0; i < s.count; ++i) rects[i] = lp::ScissorOfLight(s.lights[i], rows, tan_x, tan_y, w, h);
      std::array<lp::PassDraw, lp::kMaxPassLights> plan;
      const uint32_t n = lp::PlanDraws(s, rects.data(), 0, plan.data());
      if (n == 0) continue;
      Check(n == 1, "one tube, one draw");
      int tested = 0;
      for (int k = 0; k < 3000 && tested < 300; ++k) {
        const uint32_t li = r2.Below(s.count);
        const Light& l = s.lights[li];
        float d3[3];
        r2.Direction(d3);
        const float rr = l.range * std::cbrt(r2.Next());
        const float q[3] = {l.pos[0] + d3[0] * rr, l.pos[1] + d3[1] * rr, l.pos[2] + d3[2] * rr};
        float qx, qy, qt;
        if (!lp::ProjectToPixel(rows, tan_x, tan_y, float(w), float(h), q, lp::kScissorNear, &qx, &qy, &qt)) continue;
        if (qx < 0.0f || qy < 0.0f || qx >= float(w) || qy >= float(h)) continue;
        ++tested;
        Check(qx >= float(plan[0].rect.x0) && qx <= float(plan[0].rect.x1) && qy >= float(plan[0].rect.y0) && qy <= float(plan[0].rect.y1), "a point a light of the draw reaches is in the draw's rectangle");
      }
    }
  }
  // what the reach test spares: a model of the cost run's chase view, a flat road under a sky and a house front 25 m to one side, 1104 lines: the share of pixels no light of the tube reaches
  {
    const uint32_t w = 1920, h = 1104;
    float vi[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.8f, 6.0f, 1};
    lp::Rows rows;
    lp::RowsFromViewInverse(vi, &rows);
    const float tan_x = 0.829f, tan_y = 0.466f;
    const float centre[3] = {0.0f, 0.25f, 0.0f};
    const float heading[3] = {0.0f, 0.0f, 1.0f};
    const float colour[3] = {1.0f, 0.1f, 0.3f};
    const std::vector<Light> tube = TubeLights(centre, heading, colour, 4, 1, lp::Tier::kUltra);
    uint64_t total = 0, sky = 0, reached = 0;
    for (uint32_t y = 0; y < h; y += 4) {
      for (uint32_t x = 0; x < w; x += 4) {
        ++total;
        // the ray of the pixel centre (the camera looks along -z; x right, y up)
        const float rx = ((float(x) + 0.5f) / float(w) * 2.0f - 1.0f) * tan_x;
        const float ry = (1.0f - (float(y) + 0.5f) / float(h) * 2.0f) * tan_y;
        float dir[3] = {rx, ry, -1.0f};
        const float dn = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (float& d : dir) d /= dn;
        // the road (y = 0) where the ray goes down; a house front at x = +25 (along the whole street) where it hits that first; else the sky
        float tt = 1.0e9f;
        if (dir[1] < -1.0e-4f) tt = -rows.position[1] / dir[1];
        if (dir[0] > 1.0e-4f) tt = std::min(tt, (25.0f - rows.position[0]) / dir[0]);
        if (!(tt < 400.0f)) {
          ++sky;
          continue;
        }
        const hlsl_shim::float3 world(rows.position[0] + dir[0] * tt, rows.position[1] + dir[1] * tt, rows.position[2] + dir[2] * tt);
        bool any = false;
        for (const Light& l : tube) any = any || hlsl_shim::LpReaches(hlsl_shim::float3(l.pos[0], l.pos[1], l.pos[2]), l.range, world);
        reached += any ? 1u : 0u;
      }
    }
    const double not_reached = 100.0 * double(total - reached) / double(total);
    std::printf("cost model: chase view, ultra: of the picture %.1f %% is sky, %.1f %% is reached by a light of the tube, %.1f %% takes no plane read with the reach test first\n", 100.0 * double(sky) / double(total),
                100.0 * double(reached) / double(total), not_reached);
    Check(not_reached > 55.0 && not_reached < 98.0, "most of a chase view's picture is not reached by the tube (the reads the early test spares), and some is (the road under and round the car)");
    (void)w;
  }
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
    ++n;
  }
  return n;
}

void SourceChecks(const std::string& src) {
  const std::string gpu = src + "/gpu/";
  const std::string cmake = ReadFile(gpu + "CMakeLists.txt");
  const std::string shaders_h = ReadFile(gpu + "host_shaders.h");
  const std::string ps = ReadFile(gpu + "hlsl/local_light_ps.hlsl");
  const std::string scene = ReadFile(gpu + "scene.cpp");
  const std::string scene_h = ReadFile(gpu + "scene.h");
  const std::string renderer = ReadFile(gpu + "renderer.cpp");
  const std::string cpp = ReadFile(gpu + "local_lights.cpp");
  const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string mod = ReadFile(src + "/mc_engine/modloader/features/mod_lights.cpp");
  const std::string manager = ReadFile(gpu + "light_manager.cpp");
  Check(!cmake.empty() && !shaders_h.empty() && !ps.empty() && !scene.empty() && !cpp.empty() && !settings.empty() && !mod.empty(), "the sources are readable: " + src);

  // the shaders: both compilations in the CMake, the four blobs and the accessor in host_shaders.h
  Check(Has(cmake, "larecomp_gpu_host_shader(local_light_ps ps_6_0 DEFINES LL_PASS_MS=0)") && Has(cmake, "larecomp_gpu_host_shader(local_light_ms_ps ps_6_0 SOURCE local_light_ps DEFINES LL_PASS_MS=1)"),
        "the CMake builds the pass's two compilations");
  Check(Has(cmake, "    local_lights.cpp\n") && Has(cmake, "add_executable(larecomp_local_lights_test EXCLUDE_FROM_ALL local_lights_test.cpp)"), "the CMake has the class and this test");
  Check(Count(shaders_h, "local_light_ps.hlsl.") == 2 && Count(shaders_h, "local_light_ms_ps.hlsl.") == 2, "host_shaders.h includes the two compilations for both back ends");
  Check(Has(shaders_h, "LocalLightPixelShader(bool multisampled)") && Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(local_light_ms_ps)") && Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(local_light_ps)"),
        "host_shaders.h hands out the blob by the host's sample count");
  // the shader's bindings are the layout's: the buffer at (t0, space4) = set 4, the push at (b0, space5), the textures at spaces 0-3 (host_pass_common.hlsli)
  Check(Has(ps, "ByteAddressBuffer g_lights : register(t0, space4);") && Has(ps, "ConstantBuffer<LocalLightPush> g_push : register(b0, space5);") && Has(ps, "[[vk::push_constant]] ConstantBuffer<LocalLightPush> g_push;"),
        "the shader's buffer is at (t0, space4) and its push block at (b0, space5)");
  Check(Has(cpp, "addByteAddressBuffer(0);  // t0, space 4") && Has(cpp, "addPushConstant(0, 5, sizeof(ll::PassPush), plume::RenderShaderStageFlag::PIXEL)") &&
            Has(cpp, "for (uint32_t set = 0; set < 4; ++set) {\n    builder.addDescriptorSet(heap_->texture_set_builder());"),
        "the layout is four texture sets, the buffer's set (space 4) and the pixel push block (space 5)");
  Check(Has(ps, "uint4 slots;") && Has(ps, "uint4 info;") && Has(ps, "#include \"local_light.hlsli\""), "the shader's push block is two uint4 and it includes the twin");
  Check(Has(cpp, "push.slots[0] = summary.sample_slot;") && Has(cpp, "push.info[2] = gb.sample0;") && Has(cpp, "push.info[0] = draw.first;") && Has(cpp, "push.info[1] = draw.count;") &&
            Has(cpp, "push.info[3] = (cost_off_ & ll::kCostOffReachFirst) != 0 ? ll::kPushLateReach : 0u;"),
        "the push fields the shader reads are filled: the draw's first light and its count, the host sample, the flags");
  // GB-NEON-COST: the pass makes its draws from the plan (a car's tube is one) and reads the A/B's setting live
  Check(Has(cpp, "ll::PlanDraws(selection, rects.data(), cost_off_, plan.data())") && Has(cpp, "st::mcla_gpu_local_lights_cost_off.Get()") && Has(cpp, "for (uint32_t d = 0; d < draws; ++d) {"),
        "the pass draws what PlanDraws plans, with the setting's bits");
  Check(Has(ps, "const uint count = clamp(g_push.info.y, 1u, uint(LP_MAX_DRAW_LIGHTS));") && Has(ps, "(g_push.info.w & uint(LP_PUSH_LATE_REACH)) == 0u") && Has(ps, "LpAnyReaches(first, count, world)") &&
            Has(ps, "bool LpAnyReaches(uint first, uint count, float3 world)") && Has(ps, "LpReaches(float3(row0.x, row0.y, row0.z), row0.w, world)"),
        "the shader takes the draw's lights from the push block (at most four), tests the reach with the twin's function, and the flag picks the order");
  {
    // the order: with the reach first, the depth and the reach come before the first plane read of main; the old order reads the planes first
    const size_t main_at = At(ps, "float4 main(float4 position : SV_Position) : SV_Target {");
    const size_t early = At(ps, "if (!LpAnyReaches(first, count, world)) {");
    const size_t planes = At(ps, "const float2 rt1 = LoadPlane(g_push.slots.y, p).xy;");
    const size_t late = At(ps, "if (!reach_first) {");
    const size_t sum_at = At(ps, "const float4 one = LpOneLight(first + i, g, flags, p, vehicle, world, normal, albedo, rt2.w);");
    Check(main_at != std::string::npos && early != std::string::npos && planes != std::string::npos && late != std::string::npos && sum_at != std::string::npos && main_at < early && early < planes && planes < late &&
              late < sum_at,
          "main: the reach test (when first) comes before the planes' reads, the position after them in the old order, and the lights' sum after both");
    Check(Has(ps, "if (one.w > 0.0f) {\n      total += one.xyz;\n      any_light = true;") && Has(ps, "if (!any_light) {\n    discard;\n  }\n  return float4(total, 0.0f);"),
          "a light that adds nothing is left out of the sum, and a pixel none of them lights is discarded (as its own draw was)");
  }

  // the settings: in all three generated files, debug only
  const char* names[] = {"mcla_gpu_local_lights", "mcla_gpu_local_lights_sources", "mcla_gpu_local_lights_ab", "mcla_gpu_local_lights_gain", "mcla_gpu_headlight_pass", "mcla_gpu_local_lights_cost_off"};
  for (const char* name : names) {
    Check(Has(settings, std::string("REX_SETTING(") ) && Has(settings, std::string(", ") + name + ", "), std::string("settings_gpu.h defines ") + name);
    Check(Has(manifest, std::string("\"") + name + "\": {"), std::string("manifest.json has ") + name);
    Check(Has(meta, std::string("{\"") + name + "\", \"MCLA/GPU\"},"), std::string("settings_meta.inc lists ") + name);
    Check(Has(settings, std::string("inline auto& ") + name + " = g_gpu." + name + ";"), std::string("settings_gpu.h has the namespace alias of ") + name);
  }
  {
    // each of the four is debug only (the REX_SETTING statement ends at its first ");")
    for (const char* name : names) {
      const size_t at = settings.find(std::string(", ") + name + ", ");
      const size_t end = settings.find(");", at);
      Check(at != std::string::npos && end != std::string::npos && settings.substr(at, end - at).find("rex::kDebugOnly") != std::string::npos, std::string(name) + " is debug only");
    }
    const size_t ab_at = settings.find(", mcla_gpu_local_lights_ab, ");
    const size_t ab_end = settings.find(");", ab_at);
    Check(ab_at != std::string::npos && settings.substr(ab_at, ab_end - ab_at).find("rex::kRestart") != std::string::npos, "the lamp A/B is a restart setting (the grid is built when the city's lights load)");
    Check(Has(settings, "bool, mcla_gpu_local_lights, false,"), "the pass is off by default");
    Check(Has(settings, "i32, mcla_gpu_local_lights_sources, 0,") && Has(settings, "i32, mcla_gpu_local_lights_ab, 0,") && Has(settings, "f64, mcla_gpu_local_lights_gain, 1.0,"), "the defaults: no source, the grid, gain 1");
    Check(Has(settings, "i32, mcla_gpu_local_lights_cost_off, 0,") && Has(settings, "rex::kDebugOnly, rex::Range{0, 3});"), "GB-NEON-COST: the cost A/B defaults to 0 (both savings on), range 0 to 3");
    Check(Has(settings, "std::string, mcla_gpu_headlight_pass, \"forward\",") && Has(settings, "rex::Allowed(\"forward\", \"pass\", \"both\")"), "the headlight route's default is the forward stage, its words are forward, pass and both");
    const size_t route_at = settings.find(", mcla_gpu_headlight_pass, ");
    const size_t route_end = settings.find(");", route_at);
    Check(route_at != std::string::npos && settings.substr(route_at, route_end - route_at).find("rex::kRestart") == std::string::npos, "the headlight route is live (no restart trait)");
  }
  Check(Has(cpp, "st::mcla_gpu_local_lights.Get()") && Has(cpp, "mcla_gpu_local_lights_sources.Get()") && Has(cpp, "mcla_gpu_local_lights_ab.Get()") && Has(cpp, "mcla_gpu_local_lights_gain.Get()") &&
            Has(cpp, "hp::ParseRoute(st::mcla_gpu_headlight_pass.Get())"),
        "the class reads the four settings and the headlight route");
  // the lamp A/B's hook in the mod loader
  Check(Has(mod, "AbWithholdsFromGrid(ab)") && Has(mod, "withhold ? 0.0f : l.intensity * mult") && Has(mod, "(!withhold && (l.flags & kOn)) ? 1 : 0") && Has(mod, "mcla_gpu_local_lights_ab.Get()"),
        "mod_lights.cpp builds the grid with the lamps off in the A/B's pass and control modes");
  Check(Has(mod, "published = MakeSet(grid, mult);") && Has(mod, "r.intensity = l.intensity * mult;"), "the published set keeps the real lamp (the pass reads it)");
  // the Scene's hooks: its BeginFrame before the G-buffer's, the vote with the AO's, the pass after the AO's apply and before the sun shafts' own
  const size_t lb = At(scene, "local_lights_->BeginFrame();");
  const size_t gb = At(scene, "gbuffer_->BeginFrame(frame_serial_);");
  Check(lb != std::string::npos && gb != std::string::npos && lb < gb, "the pass's BeginFrame runs before the G-buffer's (they read its wants)");
  const size_t ao = At(scene, "ao_->OnResolve(record, summary, list, UpscalerRunning());");
  const size_t pass = At(scene, "local_lights_->OnResolve(record, summary, list, gbuffer_published, slot_);");
  const size_t shafts = At(scene, "shafts_->OnResolve(record, summary, list, UpscalerRunning());");
  Check(ao != std::string::npos && pass != std::string::npos && shafts != std::string::npos && ao < pass && pass < shafts, "the pass runs after the AO's apply and before the sun shafts");
  Check(Has(scene, "gbuffer_published = gbuffer_->OnResolve(record, summary, list);"), "the Scene hands the pass the G-buffer's own answer for the resolve");
  Check(Has(scene, "local_lights_->OnMainDraw(banks, draw.match.color);") && Has(scene, "local_lights_->OnPresent();"), "the draws' constants and the present reach the pass");
  Check(Has(scene_h, "void SetLocalLights(LocalLights* local_lights)") && Has(renderer, "scene_.SetLocalLights(&local_lights_);") && Has(renderer, "local_lights_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);") &&
            Count(renderer, "local_lights_.Shutdown();") == 2,
        "the renderer makes, hands over and shuts down the pass (both shutdown paths)");
  // the wants that switch the G-buffer on only while it runs
  Check(Has(cpp, "gbuffer_->Want(gbuffer::Consumer::kLocalLights, planes_wanted_ ? gbuffer::ConsumerPlanes(gbuffer::Consumer::kLocalLights) : 0u);") && Has(cpp, "screen_->Want(screen::Consumer::kLocalLights, planes_wanted_ ?") &&
            Has(cpp, "manager_->Want(planes_wanted_ && (sources_ & (ll::kSourceFrame | ll::kSourceCars)) != 0);") && Has(cpp, "planes_wanted_ = enabled_ && (setting_on || (hp::PassLightsHeadlights(route) && headlight_rig::SettingOn()) || neon_active_);"),
        "the wants are the pass's while it is on and HEADLIGHTS (or its own setting) wants the planes, and 0 otherwise");
  Check(Has(manager, "(every <= 0 && flips == 0 && !wanted_)") && Has(manager, "if (Checking()) {\n      Upload();") && Has(manager, "const bool cars = every > 0 || wanted_;"),
        "the light manager runs for a consumer of its frame, polls the cars for it and uploads only for its census");
  // GB-H2: the shadow in the shader and the buffer
  Check(Has(ps, "#include \"light_shadow_lookup.hlsli\"") && Has(ps, "float LslTexel(uint slot, int ix, int iy)") && Has(ps, "LslView LslLoadView(uint entry)") && Has(ps, "uint LslLayerOf(uint entry)") &&
            Has(ps, "uint LslAtlasWidth(uint slot)") && Has(ps, "uint LslAtlasHeight(uint slot)"),
        "the shader includes the one lookup and defines its five seams");
  Check(Has(ps, "g_textures[slot].Load(int3(clamp(ix, 0, int(LpAtlasW) - 1), clamp(iy, 0, int(LpAtlasH) - 1), 0)).x") && Has(ps, "uint at = uint(LP_SHADOW_VIEWS_OFFSET) + entry * uint(LP_SHADOW_VIEW_BYTES);"),
        "the seams read the atlas through the plain bindless array (set 0) and the views at the table's offsets");
  Check(Has(ps, "(flags & uint(LP_FLAG_SHADOW)) != 0u && l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u") &&
            Has(ps, "LslLightLit(l.shadow_kind, l.first_view, l.view_count, l.shadow_weight, world.x, world.y, world.z, shadow_normal.x, shadow_normal.y, shadow_normal.z, atlas_slot, uint(LSL_MODE_SURFACE))"),
        "the shader looks a light's views up in the surface mode (the plane lookup), only for a light with views and a table");
  // HL-SHADOW-QUALITY on the pass: the lookup takes the surface's geometric normal, from the depth plane's pixel quad (the lit shaders take it from the derivatives of the world position), the
  // G-buffer's normal only where the quad is not there
  Check(Has(ps, "float3 LpShadowNormal(LpGlobals g, int2 p, float3 world, float3 fallback)") && Has(ps, "const int2 q = int2(p.x & ~1, p.y & ~1);") &&
            Has(ps, "float3 n = cross(w01 - w00, w10 - w00);") && Has(ps, "if (dot(n, g.position - world) < 0.0f)") && Has(ps, "const float3 shadow_normal = LpShadowNormal(g, p, world, normal);"),
        "the shadow normal is the depth plane's quad normal turned to the camera, the G-buffer's where the quad is not there");
  Check(Has(ps, "bool LpWorldAt(LpGlobals g, int2 p, out float3 world)") && Has(ps, "const float stored = LpStoredDepth(p.x, p.y);") && Has(ps, "return g_textures_ms[g_push.slots.x].Load(int2(px, py), g_push.info.z).r;") &&
            Has(ps, "return g_textures[g_push.slots.x].Load(int3(px, py, 0)).r;"),
        "the quad's pixels are read at the guest's sample, as the pixel's own depth is (one reader: LpStoredDepth, which the contact term uses too)");
  const size_t lit_at = At(ps, "float3 add = LpPixelLight(l, g, world, normal, albedo, lit);");
  const size_t shadow_at = At(ps, "add *= LslLightLit(");
  Check(lit_at != std::string::npos && shadow_at != std::string::npos && lit_at < shadow_at, "the lookup runs after the term is known (a pixel the light does not reach takes no tap)");
  Check(Has(cpp, "in.shadow = &frame.shadow;") && Has(cpp, "ll::ShadowTableNeeded(selection, in.shadow)"), "the pass hands the frame's shadow to the buffer");
}

// HL-SHADOW-QUALITY on the pass: the shadow lookup's surface normal is the depth plane's quad normal (QuadNormal, the twin of local_light_ps.hlsl LpShadowNormal): a unit vector perpendicular to the
// quad's two edges, turned to the camera, with the G-buffer's normal where a quad pixel is the sky and where the quad is degenerate.
void QuadNormalChecks() {
  Lcg rng;
  int facing = 0, flipped = 0;
  for (int n = 0; n < 4000; ++n) {
    float base[3] = {rng.Range(-30.0f, 30.0f), rng.Range(-3.0f, 3.0f), rng.Range(2.0f, 60.0f)};
    float u[3], v[3];
    rng.Direction(u);
    rng.Direction(v);
    const float a = rng.Range(0.01f, 0.2f), b = rng.Range(0.01f, 0.2f);
    const float w00[3] = {base[0], base[1], base[2]};
    const float w10[3] = {base[0] + a * u[0], base[1] + a * u[1], base[2] + a * u[2]};
    const float w01[3] = {base[0] + b * v[0], base[1] + b * v[1], base[2] + b * v[2]};
    const float camera[3] = {rng.Range(-5.0f, 5.0f), rng.Range(0.5f, 3.0f), rng.Range(-5.0f, 0.0f)};
    const float fallback[3] = {0.0f, 1.0f, 0.0f};
    float out[3];
    lp::QuadNormal(w00, w10, w01, true, w00, camera, fallback, out);
    const float cross_len = std::sqrt((u[1] * v[2] - u[2] * v[1]) * (u[1] * v[2] - u[2] * v[1]) + (u[2] * v[0] - u[0] * v[2]) * (u[2] * v[0] - u[0] * v[2]) + (u[0] * v[1] - u[1] * v[0]) * (u[0] * v[1] - u[1] * v[0]));
    if (cross_len < 0.05f) {
      continue;  // two nearly parallel edges: the quad is a sliver, the plane is not believed (the next check covers the degenerate case)
    }
    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    const float dx[3] = {w10[0] - w00[0], w10[1] - w00[1], w10[2] - w00[2]};
    const float dy[3] = {w01[0] - w00[0], w01[1] - w00[1], w01[2] - w00[2]};
    const float along_x = std::fabs(out[0] * dx[0] + out[1] * dx[1] + out[2] * dx[2]) / (a * len);
    const float along_y = std::fabs(out[0] * dy[0] + out[1] * dy[1] + out[2] * dy[2]) / (b * len);
    const float to_camera = out[0] * (camera[0] - w00[0]) + out[1] * (camera[1] - w00[1]) + out[2] * (camera[2] - w00[2]);
    Check(std::fabs(len - 1.0f) < 1.0e-5f && along_x < 1.0e-4f && along_y < 1.0e-4f && to_camera >= 0.0f, "the quad normal is a unit vector, perpendicular to both edges, turned to the camera");
    facing += to_camera >= 0.0f ? 1 : 0;
    float raw[3] = {dy[1] * dx[2] - dy[2] * dx[1], dy[2] * dx[0] - dy[0] * dx[2], dy[0] * dx[1] - dy[1] * dx[0]};
    flipped += raw[0] * out[0] + raw[1] * out[1] + raw[2] * out[2] < 0.0f ? 1 : 0;
  }
  Check(facing > 3000 && flipped > 500 && flipped < 3500, "the random quads face the camera, and about half needed the flip of cross(dy, dx) (" + std::to_string(flipped) + ")");
  // the floor seen from above: edges along +x and +z (z away from the camera, screen y down = +z for a floor): cross(dy, dx) = z x x = +y, the camera is above: up
  {
    const float w00[3] = {0.0f, 0.0f, 10.0f}, w10[3] = {0.1f, 0.0f, 10.0f}, w01[3] = {0.0f, 0.0f, 10.2f};
    const float camera[3] = {0.0f, 1.5f, 0.0f}, fallback[3] = {1.0f, 0.0f, 0.0f};
    float out[3];
    lp::QuadNormal(w00, w10, w01, true, w00, camera, fallback, out);
    Check(std::fabs(out[0]) < 1.0e-6f && std::fabs(out[1] - 1.0f) < 1.0e-6f && std::fabs(out[2]) < 1.0e-6f, "a floor seen from above has the normal up");
    // a camera below it: down
    const float below[3] = {0.0f, -1.5f, 0.0f};
    lp::QuadNormal(w00, w10, w01, true, w00, below, fallback, out);
    Check(std::fabs(out[1] + 1.0f) < 1.0e-6f, "... and from below the normal down (turned to the camera)");
    // a quad pixel that is the sky or off the plane: the G-buffer's normal
    lp::QuadNormal(w00, w10, w01, false, w00, camera, fallback, out);
    Check(out[0] == 1.0f && out[1] == 0.0f && out[2] == 0.0f, "a quad with a pixel that is not there gives the fallback");
    // a degenerate quad (three equal points, or collinear ones): the fallback
    const float same[3] = {0.0f, 0.0f, 10.0f}, line[3] = {0.2f, 0.0f, 10.0f};
    lp::QuadNormal(w00, same, same, true, w00, camera, fallback, out);
    Check(out[0] == 1.0f && out[1] == 0.0f && out[2] == 0.0f, "a quad that is a point gives the fallback");
    lp::QuadNormal(w00, w10, line, true, w00, camera, fallback, out);
    Check(out[0] == 1.0f && out[1] == 0.0f && out[2] == 0.0f, "a quad whose edges are collinear gives the fallback");
  }
}

// ---------------------------------------------------------------------------
// GB-NEON-BLEED: a neon light does not shine through the car that is lit (local_lights_rules.h ContactVisibility, hlsl/local_light.hlsli LpContactVisibility)
// ---------------------------------------------------------------------------

// A solid made of boxes, and the picture's depth of it for a camera: the stored depth of a pixel is that of the nearest box its ray enters (the ray's parameter along the camera's forward is the
// view distance, as WorldPosition has it), 0 (the sky) where it enters none.
struct Aabb {
  float lo[3];
  float hi[3];
};
float RayEntersBoxes(const std::vector<Aabb>& boxes, const float o[3], const float d[3]) {
  float best = 1.0e30f;
  for (const Aabb& b : boxes) {
    float t0 = 0.0f, t1 = 1.0e30f;
    bool ok = true;
    for (int k = 0; k < 3 && ok; ++k) {
      if (std::fabs(d[k]) < 1.0e-9f) {
        ok = o[k] >= b.lo[k] && o[k] <= b.hi[k];
        continue;
      }
      float a = (b.lo[k] - o[k]) / d[k];
      float c = (b.hi[k] - o[k]) / d[k];
      if (a > c) std::swap(a, c);
      t0 = std::max(t0, a);
      t1 = std::min(t1, c);
      ok = t0 <= t1;
    }
    if (ok && t0 < best) best = t0;
  }
  return best;
}

hlsl_shim::LpGlobals ShimGlobalsOf(const lp::Rows& rows, float tan_x, float tan_y, float width, float height, const float lin[4]) {
  hlsl_shim::LpGlobals g;
  g.right = hlsl_shim::float3(rows.right[0], rows.right[1], rows.right[2]);
  g.up = hlsl_shim::float3(rows.up[0], rows.up[1], rows.up[2]);
  g.forward = hlsl_shim::float3(rows.forward[0], rows.forward[1], rows.forward[2]);
  g.position = hlsl_shim::float3(rows.position[0], rows.position[1], rows.position[2]);
  g.tan_x = tan_x;
  g.tan_y = tan_y;
  g.gain = 1.0f;
  g.night = 1.0f;
  g.exposure = 1.0f;
  g.lin = hlsl_shim::float4(lin[0], lin[1], lin[2], lin[3]);
  g.size = hlsl_shim::float4(width, height, 1.0f / width, 1.0f / height);
  return g;
}

// The contact term through both twins: the C++ one with `stored_at`, the HLSL one (compiled as C++) with the seam set to the same field. Returns the C++ value and checks the two agree.
float ContactBoth(const lp::Rows& rows, float tan_x, float tan_y, float width, float height, const float lin[4], const float world[3], const float normal[3], const float light[3],
                  const std::function<float(int, int)>& stored_at, const std::string& what) {
  const float cpp = lp::ContactVisibility(rows, tan_x, tan_y, width, height, lin, world, normal, light, stored_at);
  g_stored_depth = stored_at;
  const hlsl_shim::LpGlobals g = ShimGlobalsOf(rows, tan_x, tan_y, width, height, lin);
  const float hl = hlsl_shim::LpContactVisibility(g, hlsl_shim::float3(world[0], world[1], world[2]), hlsl_shim::float3(normal[0], normal[1], normal[2]),
                                                  hlsl_shim::float3(light[0], light[1], light[2]));
  CheckNear(hl, cpp, 2.0e-4, "the HLSL twin's contact term is the C++ one: " + what);
  return cpp;
}

void ContactChecks(const std::string& src) {
  // the numbers of the shader are the C++ ones
  const std::string hlsli = ReadFile(src + "/gpu/hlsl/local_light.hlsli");
  Check(Has(hlsli, "#define LP_CONTACT_STEPS 5") && lp::kContactSteps == 5 && Has(hlsli, "#define LP_CONTACT_LENGTH 0.8f") && lp::kContactLength == 0.8f &&
            Has(hlsli, "#define LP_CONTACT_NORMAL 0.03f") && lp::kContactNormal == 0.03f && Has(hlsli, "#define LP_CONTACT_BIAS 0.04f") && lp::kContactBias == 0.04f &&
            Has(hlsli, "#define LP_CONTACT_RAMP 0.06f") && lp::kContactRamp == 0.06f && Has(hlsli, "#define LP_CONTACT_THICK 0.7f") && lp::kContactThick == 0.7f &&
            Has(hlsli, "#define LP_CONTACT_FADE 0.25f") && lp::kContactFade == 0.25f,
        "the contact term's numbers are the shader's");

  // who is asked: a neon light on a vehicle pixel, nothing else
  {
    const float at[3] = {0.0f, 0.0f, 0.0f};
    const float colour[3] = {30.0f, 0.02f, 0.01f};
    const Light neon = MakeOmniLight(LightSource::kNeon, at, colour, 3.0f, 3.0f, 7.0f, 1);
    const Light police = MakeOmniLight(LightSource::kPolice, at, colour, 3.0f, 3.0f, 7.0f, 2);
    const Light head = HeadlightOf(3, 0.0f);
    Check(lp::TakesContact(neon, true) && !lp::TakesContact(neon, false) && !lp::TakesContact(police, true) && !lp::TakesContact(head, true), "only a neon light on a vehicle pixel takes the contact term");
    hlsl_shim::LlLight hl;
    hl.source = LL_SOURCE_NEON;
    Check(hlsl_shim::LpTakesContact(hl, true) && !hlsl_shim::LpTakesContact(hl, false), "... in the shader too");
    hl.source = LL_SOURCE_POLICE;
    Check(!hlsl_shim::LpTakesContact(hl, true), "... and not a police light");
  }

  // THE OWNER'S CASE as a box. The body is a solid box (x -1..1, y 0..1, z -2..1) with a vent: a slot cut into its front (z = 1) face, 0.4 m wide (x 0.2..0.6) and 0.5 m deep. The light is on the
  // left (x -3) at the height of the slot's middle. The camera is up front and to the left.
  //   - the front face (normal +z) faces away from the light: N.L gives it 0 (the far side of a box needs nothing more);
  //   - the left face (normal -x) faces the light and is open to it: lit, the contact term leaves it alone;
  //   - the slot's right wall (x 0.6, normal -x) faces the light, N.L > 0, and the light reaches it only THROUGH the body (the slot's left wall is the body): the contact term shuts it.
  const std::vector<Aabb> body = {{{-1.0f, 0.0f, -2.0f}, {0.2f, 1.0f, 1.0f}}, {{0.6f, 0.0f, -2.0f}, {1.0f, 1.0f, 1.0f}}, {{0.2f, 0.0f, -2.0f}, {0.6f, 1.0f, 0.5f}}};
  const float width = 1920.0f, height = 1080.0f, tan_x = 0.8f, tan_y = 0.45f;
  const float near_z = 0.1f, far_z = 1000.0f;
  float lin[4];
  LinOf(near_z, far_z, lin);
  lp::Rows rows;
  {
    const float eye[3] = {-4.0f, 0.6f, 6.0f};
    const float aim[3] = {0.0f, 0.5f, 0.5f};
    float f[3] = {aim[0] - eye[0], aim[1] - eye[1], aim[2] - eye[2]};
    const float fn = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fn;
    float r[3] = {-f[2], 0.0f, f[0]};  // forward x world up
    const float rn = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    for (float& v : r) v /= rn;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    for (int i = 0; i < 3; ++i) {
      rows.right[i] = r[i];
      rows.up[i] = u[i];
      rows.forward[i] = f[i];
      rows.position[i] = eye[i];
    }
  }
  const auto stored_of_box = [&](int ix, int iy) -> float {
    const float u = (float(ix) + 0.5f) / width;
    const float v = (float(iy) + 0.5f) / height;
    const float sx = (u * 2.0f - 1.0f) * tan_x;
    const float sy = (1.0f - v * 2.0f) * tan_y;
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = rows.forward[i] + rows.right[i] * sx + rows.up[i] * sy;
    const float t = RayEntersBoxes(body, rows.position, d);
    return t > 1.0e29f ? 0.0f : StoredOf(t, near_z, far_z);
  };
  // a receiver: its pixel and whether the camera sees it there (the field's depth at the pixel is the point's own)
  const auto seen = [&](const float p[3], const std::string& what) {
    float px = 0.0f, py = 0.0f, t = 0.0f;
    const bool on = lp::ProjectToPixel(rows, tan_x, tan_y, width, height, p, 0.05f, &px, &py, &t);
    Check(on, what + ": in front of the camera");
    if (!on) return;
    const float at = lp::LinearDepth(stored_of_box(int(px), int(py)), lin);
    CheckNear(at, t, 0.03, what + ": the camera sees the point (the field's depth is the point's)");
  };

  const float light_at[3] = {-3.0f, 0.5f, 0.75f};
  const float neon_colour[3] = {30.0f, 0.02f, 0.01f};
  const float plateau = 0.06f * 7.0f * 7.0f;
  const Light neon = MakeOmniLight(LightSource::kNeon, light_at, neon_colour, plateau, plateau, 7.0f, 4);
  const lp::Units units;
  const float albedo[3] = {0.4f, 0.5f, 0.6f};
  const float p_front[3] = {-0.4f, 0.5f, 1.0f}, n_front[3] = {0.0f, 0.0f, 1.0f};
  const float p_left[3] = {-1.0f, 0.5f, 0.0f}, n_left[3] = {-1.0f, 0.0f, 0.0f};
  const float p_wall[3] = {0.6f, 0.5f, 0.75f}, n_wall[3] = {-1.0f, 0.0f, 0.0f};
  seen(p_front, "the front face");
  seen(p_left, "the left face");
  seen(p_wall, "the slot's right wall");

  float out[3];
  lp::PixelLight(neon, units, p_front, n_front, albedo, 1.0f, out);
  Check(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f, "the face that faces away from the light gets 0 from the law (N.L)");
  lp::PixelLight(neon, units, p_left, n_left, albedo, 1.0f, out);
  const float left_lit = out[0];
  Check(left_lit > 0.0f, "the face that faces the light is lit");
  lp::PixelLight(neon, units, p_wall, n_wall, albedo, 1.0f, out);
  const float wall_lit = out[0];
  Check(wall_lit > 0.0f, "the slot's wall faces the light too: the law alone lights it (this is the leak: N.L cannot see the body in between)");

  const float v_left = ContactBoth(rows, tan_x, tan_y, width, height, lin, p_left, n_left, light_at, stored_of_box, "the open left face");
  const float v_wall = ContactBoth(rows, tan_x, tan_y, width, height, lin, p_wall, n_wall, light_at, stored_of_box, "the slot's wall");
  CheckNear(v_left, 1.0, 1.0e-6, "the face that is open to the light keeps all of it");
  CheckNear(v_wall, 0.0, 1.0e-6, "the wall the light can reach only through the body gets none");
  Check(left_lit * v_left > 0.0f && wall_lit * v_wall == 0.0f, "the lit side stays lit, the vent behind the body goes dark");

  // the same wall with the light on its own side of the body (in the slot's mouth, in front of the front face): open
  const float light_in_front[3] = {0.4f, 0.5f, 2.5f};
  const float v_open = ContactBoth(rows, tan_x, tan_y, width, height, lin, p_wall, n_wall, light_in_front, stored_of_box, "a light out in front of the slot");
  CheckNear(v_open, 1.0, 1.0e-6, "a light that can see the wall keeps all of it");

  // the shell's thickness: a surface well in front of the point (another car, a wall) is not the shell the light runs behind; the sky and a surface behind the way are open
  {
    const float from_ground[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float lamp[3] = {0.5f, 0.0f, 0.0f};
    lp::Rows r;
    r.position[2] = -5.0f;  // looking along +z
    const float at_p[3] = {0.0f, 0.0f, 0.0f};
    (void)from_ground;
    // the way's steps end up at view distances 5 (the point is on the axis, 5 m ahead, the light to its right on the same plane): a field at any other distance is the point's neighbour
    const auto field_at = [&](float distance) { return std::function<float(int, int)>([=](int, int) { return StoredOf(distance, near_z, far_z); }); };
    const float open_sky = ContactBoth(r, tan_x, tan_y, width, height, lin, at_p, up, lamp, [](int, int) { return 0.0f; }, "sky everywhere");
    CheckNear(open_sky, 1.0, 1.0e-6, "the sky is open");
    const float behind = ContactBoth(r, tan_x, tan_y, width, height, lin, at_p, up, lamp, field_at(8.0f), "surface behind the way");
    CheckNear(behind, 1.0, 1.0e-6, "a surface behind the way's points is open");
    const float shell = ContactBoth(r, tan_x, tan_y, width, height, lin, at_p, up, lamp, field_at(4.75f), "a shell 25 cm in front");
    CheckNear(shell, 0.0, 1.0e-6, "a surface a few decimetres in front of the way is a shell: shut");
    const float far_wall = ContactBoth(r, tan_x, tan_y, width, height, lin, at_p, up, lamp, field_at(3.0f), "a wall 2 m in front");
    CheckNear(far_wall, 1.0, 1.0e-6, "a surface 2 m in front is another object: open");
    const float skin = ContactBoth(r, tan_x, tan_y, width, height, lin, at_p, up, lamp, field_at(4.99f), "a surface 1 cm in front");
    CheckNear(skin, 1.0, 1.0e-6, "a surface within the bias (the point's own surface) is open");
    const float light_here[3] = {0.0f, 0.0f, 0.0f};
    const float same = ContactBoth(r, tan_x, tan_y, width, height, lin, at_p, up, light_here, field_at(4.75f), "light at the pixel");
    CheckNear(same, 1.0, 1.0e-6, "a light at the pixel itself has no way to look along");
  }

  // the twins against each other on random cameras, points, lights and a ragged depth field
  Lcg rng;
  int shut = 0, open = 0, partial = 0;
  for (int round = 0; round < 600; ++round) {
    float vi[16];
    lp::Rows rr;
    RandomPose(rng, vi, &rr);
    const float tx = rng.Range(0.4f, 1.1f);
    const float ty = tx * rng.Range(0.45f, 0.75f);
    const float w = float(rng.Below(2000) + 640);
    const float h = float(rng.Below(1200) + 360);
    float lin2[4];
    LinOf(0.1f, 10000.0f, lin2);
    float world[3], n[3], lightp[3];
    rng.Direction(n);
    const float depth = rng.Range(2.0f, 30.0f);
    float jitter[3];
    rng.Direction(jitter);
    const float spread = rng.Range(0.0f, 0.5f * depth * tx);
    for (int i = 0; i < 3; ++i) world[i] = rr.position[i] + rr.forward[i] * depth + jitter[i] * spread;
    float toward[3];
    rng.Direction(toward);
    const float dist = rng.Range(0.1f, 6.0f);
    for (int i = 0; i < 3; ++i) lightp[i] = world[i] + toward[i] * dist;
    const float base = depth + rng.Range(-1.0f, 1.0f);
    const float amp = rng.Range(0.0f, 0.6f);
    const float k1 = rng.Range(0.005f, 0.05f), k2 = rng.Range(0.005f, 0.05f);
    const auto field = [=](int ix, int iy) -> float {
      const float t = base + amp * std::sin(float(ix) * k1 + float(iy) * k2) + (((ix / 23) + (iy / 17)) % 2 == 0 ? 0.0f : 0.35f);
      return round % 11 == 0 ? 0.0f : StoredOf(std::max(t, 0.2f), 0.1f, 10000.0f);
    };
    const float v = ContactBoth(rr, tx, ty, w, h, lin2, world, n, lightp, field, "random round " + std::to_string(round));
    Check(v >= 0.0f && v <= 1.0f, "the contact term is a share");
    (v < 0.001f ? shut : (v > 0.999f ? open : partial))++;
  }
  Check(shut > 20 && open > 20 && partial > 5, Num("the random rounds reach every case (%.0f shut, %.0f open, %.0f partial)", shut, open, partial));

  // the shader's wiring
  const std::string ps = ReadFile(src + "/gpu/hlsl/local_light_ps.hlsl");
  Check(Has(ps, "float LpStoredDepth(int px, int py)") && Has(ps, "g_textures[g_push.slots.x].Load(int3(px, py, 0)).r") && Has(ps, "g_textures_ms[g_push.slots.x].Load(int2(px, py), g_push.info.z).r"),
        "the shader defines the contact term's seam: the main depth plane at the guest's sample 0");
  Check(Has(ps, "vehicle = (GbRt3Flags(rt3) & uint(GB_FLAG_VEHICLE)) != 0u;") && Has(ps, "bool vehicle = false;"), "the vehicle flag is RT3's, false where the frame has no RT3");
  // (GB-NEON-COST: the lights' sum is LpOneLight; the contact term is its own step of one light, so a pixel the reach test discarded, and a light that adds nothing, take no read)
  const size_t one_at = At(ps, "float4 LpOneLight(uint index, LpGlobals g, uint flags, int2 p, bool vehicle, float3 world, float3 normal, float3 albedo, float lit) {");
  const size_t lit_at = At(ps, "float3 add = LpPixelLight(l, g, world, normal, albedo, lit);");
  const size_t contact_at = At(ps, "if (LpTakesContact(l, vehicle)) {");
  const size_t shadow_at = At(ps, "add *= LslLightLit(");
  const size_t main_of_ps = At(ps, "float4 main(float4 position : SV_Position) : SV_Target {");
  Check(Has(ps, "add *= LpContactVisibility(g, world, normal, l.pos);") && one_at < lit_at && lit_at < contact_at && contact_at < shadow_at && shadow_at < main_of_ps,
        "the contact term runs inside the light's turn, after the law's term is known (a pixel the light does not light takes no read) and before the shadow's lookup");
  Check(Has(hlsli, "float LpStoredDepth(int px, int py);") && Has(hlsli, "return vehicle && l.source == LL_SOURCE_NEON;"), "the twin declares the seam and asks neon on vehicles only");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    std::string here = __FILE__;
    for (char& c : here) {
      if (c == '\\') c = '/';
    }
    const size_t slash = here.rfind('/');
    src = slash == std::string::npos ? std::string(".") : here.substr(0, slash) + "/..";
  }
  const std::string hlsli = ReadFile(src + "/gpu/hlsl/local_light.hlsli");
  Check(!hlsli.empty(), "the HLSL twin is readable: " + src + "/gpu/hlsl/local_light.hlsli");
  LayoutChecks(hlsli);
  CameraChecks();
  UnitsChecks();
  VoteChecks();
  SelectionChecks();
  ScissorChecks();
  BufferChecks();
  ShadowBufferChecks();
  ShadowPixelChecks();
  QuadNormalChecks();
  TwinChecks();
  CostChecks();
  SourceChecks(src);
  ContactChecks(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

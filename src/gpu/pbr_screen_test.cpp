// Check of the screen passes' side of the shared physically based model (pbr_screen_rules.h, hlsl/pbr_screen_common.hlsli, pbr_screen_shim.h, pbr_screen_switch.h; PBR level 1C, perf/PBR.md). A program of its own: it needs no
// renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_pbr_screen_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_pbr_screen_test [<src/gpu dir>]     (the folder defaults to the one this file is in)
//
// It checks:
//   THE CONTRACT'S NAMES  the functions of hlsl/pbr_brdf.hlsli that the screen passes call are there with the signatures they call them by (PBR-1's header is compiled as C++ below: a change that breaks a call fails here).
//   THE DECODE            hlsl/pbr_screen_common.hlsli AND the contract compiled AS C++ (the shim of the HLSL vector types below, with pbr_screen_shim.h's operators) against the twin in pbr_screen_rules.h on random
//                         pixels: every class, the metal flag, every smoothness, every angle.
//   THE DEFINITIONS       a metal's weight is its albedo's split sum and tints; RT3's metal flag makes any class a metal; a dielectric's weight is grey; an emissive pixel reflects nothing; a pure metal takes no diffuse
//                         light; a rough dielectric seen head on takes nearly all of it; the share falls with the angle for a smooth surface; the diffuse weight is the albedo times the share for a dielectric; the Ray
//                         Reconstruction diffuse guide is BaseColor x (1 - metalness) with the floor; THE CALIBRATION: the mean diffuse share over the city's class mix, printed.
//   THE SWITCH            the setting's text: missing is ON, 0 / false / off / no (any case) OFF, anything else ON; the accessor reads the registered bool live.
//   THE SOURCES           the reflections use the shared functions and read the switch (the shader text is held here, each pass's own test holds the rest).
//
// Exit 0 = passed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gbuffer_types.h"
#include "pbr_screen_rules.h"

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
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float3 sqrt(float3 v) { return float3(std::sqrt(v.x), std::sqrt(v.y), std::sqrt(v.z)); }
inline float3 saturate(float3 v) { return float3(saturate(v.x), saturate(v.y), saturate(v.z)); }
inline float3 cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline float exp2(float v) { return std::exp2(v); }
inline float log2(float v) { return std::log2(v); }
#include "pbr_screen_shim.h"   // what the contract needs and this shim does not have: float3 arithmetic, lerp, clamp, reflect ...
#include "hlsl/gbuffer_common.hlsli"
#include "hlsl/pbr_screen_common.hlsli"
}  // namespace hlsl_shim

namespace ps = mcla::gpu::pbr_screen;
namespace pb = larecomp::gpu::pbr;
namespace gb = mcla::gpu::gbuffer;
namespace hs = hlsl_shim;

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
std::string Num(const char* format, double a = 0, double b = 0, double c = 0, double d = 0) {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer), format, a, b, c, d);
  return buffer;
}
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  out->erase(std::remove(out->begin(), out->end(), '\r'), out->end());
  return true;
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

struct Rng {
  uint32_t state = 2463534242u;
  uint32_t Next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  float F() { return float(Next() >> 8) * (1.0f / 16777216.0f); }
};

constexpr uint32_t kClassCount = 16;

// ---------------------------------------------------------------------------
// THE CONTRACT'S NAMES
// ---------------------------------------------------------------------------
void TestNames(const std::string& dir) {
  std::string header;
  const bool read = Slurp(dir + "/hlsl/pbr_brdf.hlsli", &header);
  Check(read, "hlsl/pbr_brdf.hlsli is readable from " + dir);
  if (!read) return;
  Check(Has(header, "PbrSurface PbrSurfaceOfGbuffer(float3 n, float3 albedo, float smoothness, uint cls, bool metal_flag)") && Has(header, "float PbrMetalOfScreen(uint cls, bool metal_flag)") &&
            Has(header, "float PbrRoughnessFromSmoothness(float smoothness)") && Has(header, "float3 PbrEnvSpecularWeight(PbrSurface s, float nv)") &&
            Has(header, "float3 PbrEnvDiffuseWeight(PbrSurface s, float nv)"),
        "the contract's functions the screen passes call have the signatures they call them by (PbrSurfaceOfGbuffer, PbrMetalOfScreen, PbrRoughnessFromSmoothness, PbrEnvSpecularWeight, PbrEnvDiffuseWeight)");
  std::string common;
  Check(Slurp(dir + "/hlsl/pbr_screen_common.hlsli", &common) && Has(common, "#include \"pbr_brdf.hlsli\"") && !Has(common, "Schlick") && !Has(common, "0.04f") && !Has(common, "0.02f"),
        "pbr_screen_common.hlsli takes every number of the model from the contract (no reflectance, roughness or metalness of its own)");
}

// ---------------------------------------------------------------------------
// THE DECODE
// ---------------------------------------------------------------------------
void TestDecode() {
  Rng rng;
  int bad = 0;
  int trials = 0;
  double worst = 0.0;
  for (int i = 0; i < 20000; ++i) {
    const uint32_t cls = rng.Next() % kClassCount;
    const bool flag = (rng.Next() % 6) == 0;
    const float smooth = rng.F();
    const float nv = rng.F();
    const hs::float3 albedo(rng.F(), rng.F(), rng.F());
    const bool have = (rng.Next() % 5) != 0;
    const ps::Vec3 al{albedo.x, albedo.y, albedo.z};
    ++trials;
    if (hs::PbrScreenMetalness(cls, flag) != ps::Metalness(cls, flag)) ++bad;
    const hs::float3 w = hs::PbrScreenReflectionWeight(cls, flag, smooth, albedo, have, nv);
    const ps::Vec3 tw = ps::ReflectionWeight(cls, flag, smooth, al, have, nv);
    const hs::float3 d = hs::PbrScreenDiffuseWeight(cls, flag, smooth, albedo, nv);
    const ps::Vec3 td = ps::DiffuseWeight(cls, flag, smooth, al, nv);
    const float sh = hs::PbrScreenDiffuseShare(cls, flag, smooth, nv);
    const float tsh = ps::DiffuseShare(cls, flag, smooth, nv);
    const hs::float3 g = hs::PbrScreenDiffuseGuide(cls, flag, albedo, 0.02f);
    const ps::Vec3 tg = ps::DiffuseGuide(cls, flag, al, 0.02f);
    const auto D = [](double x, double y) { return std::fabs(x - y); };
    const double e = std::max({D(w.x, tw.x), D(w.y, tw.y), D(w.z, tw.z), D(d.x, td.x), D(d.y, td.y), D(d.z, td.z), D(sh, tsh), D(g.x, tg.x), D(g.y, tg.y), D(g.z, tg.z)});
    worst = std::max(worst, e);
    if (e > 2e-6) ++bad;
  }
  Check(bad == 0, Num("the .hlsli and the contract compiled as C++ are the twin on %.0f random pixels (largest difference %.2e)", double(trials), worst));
}

// ---------------------------------------------------------------------------
// THE DEFINITIONS
// ---------------------------------------------------------------------------
void TestDefinitions() {
  using C = gb::SurfaceClass;
  const auto cls = [](C c) { return uint32_t(c); };
  // the metalness: the flag makes a metal of anything, else the contract's class default (chrome 1, metal 0.85)
  {
    bool table = true;
    for (uint32_t c = 0; c < kClassCount; ++c) {
      const float expect = c == cls(C::kChrome) ? 1.0f : (c == cls(C::kMetal) ? 0.85f : 0.0f);
      table = table && ps::Metalness(c, false) == expect && ps::Metalness(c, true) == 1.0f;
    }
    Check(table, "the metalness: chrome 1, the metal class 0.85, every other class 0; RT3's metal flag makes any class 1");
  }
  // a metal's weight is its albedo's split sum: tinted, brighter than a dielectric's
  {
    const ps::Vec3 gold{0.9f, 0.7f, 0.2f};
    const ps::Vec3 m = ps::ReflectionWeight(cls(C::kMetal), false, 0.8f, gold, true, 0.9f);
    const ps::Vec3 d = ps::ReflectionWeight(cls(C::kRoad), false, 0.8f, gold, true, 0.9f);
    const ps::Vec3 f = ps::ReflectionWeight(cls(C::kRoad), true, 0.8f, gold, true, 0.9f);
    Check(m.x > m.y && m.y > m.z && Near(d.x, d.y, 1e-6) && Near(d.y, d.z, 1e-6) && m.x > 5.0f * d.x, Num("a metal reflects tinted by its albedo (%.2f %.2f %.2f); a dielectric grey (%.3f)", m.x, m.y, m.z, d.x));
    Check(f.x > 5.0f * d.x && f.x > f.y && f.y > f.z && f.x > m.x - 0.05f, "RT3's metal flag makes a road pixel a (full) metal: tinted, many times the dielectric's weight");
    // the contract's own split sum, by the twin's functions: F0 * A + B with the multi-scatter factor
    const pb::Surface s = ps::MakeSurface(cls(C::kChrome), false, 0.8f, gold, true);
    const pb::V3 expect = pb::EnvSpecularWeight(s, 0.6f);
    const ps::Vec3 w = ps::ReflectionWeight(cls(C::kChrome), false, 0.8f, gold, true, 0.6f);
    Check(Near(w.x, std::min(expect.x, 1.0f), 1e-6) && Near(w.y, std::min(expect.y, 1.0f), 1e-6) && Near(w.z, std::min(expect.z, 1.0f), 1e-6), "a chrome pixel's weight is the contract's split sum of its albedo");
  }
  // the class's reflectance reaches the weight: water reflects about half of what a road does face on
  {
    const ps::Vec3 water = ps::ReflectionWeight(cls(C::kWater), false, 0.9f, ps::Vec3{0.5f, 0.5f, 0.5f}, true, 1.0f);
    const ps::Vec3 road = ps::ReflectionWeight(cls(C::kRoad), false, 0.9f, ps::Vec3{0.5f, 0.5f, 0.5f}, true, 1.0f);
    Check(water.x < road.x && water.x > 0.4f * road.x, Num("water (F0 0.02) reflects less than a road (0.04) face on: %.3f against %.3f", water.x, road.x));
  }
  // emissive reflects nothing
  {
    const ps::Vec3 e = ps::ReflectionWeight(cls(C::kEmissive), false, 0.9f, ps::Vec3{1, 1, 1}, true, 0.05f);
    Check(e.x == 0.0f && e.y == 0.0f && e.z == 0.0f, "an emissive pixel reflects nothing at any angle");
  }
  // the diffuse side
  {
    const ps::Vec3 a{0.5f, 0.4f, 0.3f};
    const ps::Vec3 dm = ps::DiffuseWeight(cls(C::kChrome), false, 0.5f, a, 0.8f);
    Check(dm.x == 0.0f && dm.y == 0.0f && dm.z == 0.0f && ps::DiffuseShare(cls(C::kChrome), false, 0.5f, 0.8f) == 0.0f && ps::DiffuseShare(cls(C::kWorld), true, 0.5f, 0.8f) == 0.0f, "a pure metal (chrome, or the metal flag) takes no diffuse light");
    const float grimy = ps::DiffuseShare(cls(C::kMetal), false, 0.5f, 0.8f);
    Check(grimy > 0.0f && grimy < 0.2f, Num("the metal class (0.85) keeps a little diffuse: share %.3f", grimy));
    const float rough_head_on = ps::DiffuseShare(cls(C::kWorld), false, 0.0f, 1.0f);
    Check(rough_head_on > 0.97f && rough_head_on < 1.0f, Num("a rough dielectric seen head on takes %.3f of the diffuse light", rough_head_on));
    // a smooth surface loses more of it at a grazing angle
    const float head = ps::DiffuseShare(cls(C::kRoad), false, 0.9f, 1.0f), grazing = ps::DiffuseShare(cls(C::kRoad), false, 0.9f, 0.1f);
    Check(grazing < head && grazing < 0.8f && head > 0.9f, Num("a wet road (smoothness 0.9): %.3f head on, %.3f at grazing", head, grazing));
    // the diffuse weight is the albedo times the share, per channel, for a dielectric
    const ps::Vec3 d = ps::DiffuseWeight(cls(C::kWorld), false, 0.3f, a, 0.7f);
    const float s = ps::DiffuseShare(cls(C::kWorld), false, 0.3f, 0.7f);
    Check(Near(d.x, a.x * s, 2e-6) && Near(d.y, a.y * s, 2e-6) && Near(d.z, a.z * s, 2e-6), "for a dielectric the diffuse weight is the albedo times the share (the F0 does not depend on the albedo)");
  }
  // the Ray Reconstruction diffuse guide
  {
    const ps::Vec3 a{0.6f, 0.3f, 0.01f};
    const ps::Vec3 d = ps::DiffuseGuide(cls(C::kWorld), false, a, 0.02f);
    const ps::Vec3 m = ps::DiffuseGuide(cls(C::kChrome), false, a, 0.02f);
    const ps::Vec3 f = ps::DiffuseGuide(cls(C::kWorld), true, a, 0.02f);
    const ps::Vec3 grimy = ps::DiffuseGuide(cls(C::kMetal), false, a, 0.02f);
    Check(Near(d.x, 0.6f, 1e-7) && Near(d.y, 0.3f, 1e-7) && Near(d.z, 0.02f, 1e-7), "the diffuse guide of a dielectric is its albedo, never under the floor");
    Check(m.x == 0.02f && m.y == 0.02f && m.z == 0.02f && f.x == 0.02f && f.y == 0.02f && f.z == 0.02f, "the diffuse guide of a pure metal is the floor (BaseColor x (1 - metalness))");
    Check(Near(grimy.x, 0.6f * 0.15f, 1e-6) && Near(grimy.y, 0.3f * 0.15f, 1e-6), "the metal class's diffuse guide keeps 15 per cent of the albedo");
  }
  // THE CALIBRATION: the mean diffuse share over a mix of the city's surfaces and the angles a camera sees them at. The brief: the overall brightness stays close to today's.
  {
    struct Mix {
      C c;
      float smooth;
      float weight;
    };
    const Mix mix[] = {{C::kWorld, 0.15f, 0.34f}, {C::kRoad, 0.30f, 0.18f}, {C::kWetRoad, 0.92f, 0.04f}, {C::kSidewalk, 0.25f, 0.10f}, {C::kTerrain, 0.05f, 0.05f}, {C::kLod, 0.10f, 0.08f},
                       {C::kFoliage, 0.05f, 0.04f}, {C::kPaint, 0.85f, 0.07f}, {C::kGlass, 0.9f, 0.02f}, {C::kMetal, 0.7f, 0.04f}, {C::kSkin, 0.2f, 0.01f}, {C::kInterior, 0.2f, 0.03f}};
    double total = 0.0, weight = 0.0, dielectric = 0.0, dielectric_weight = 0.0;
    for (const Mix& m : mix) {
      double share = 0.0;
      // angles 0.15 to 1 (a surface seen from the camera: most of the picture is face-on to oblique), weighted by the cosine (the area of a plane that a view cone covers falls with it)
      double wsum = 0.0;
      for (int i = 1; i <= 20; ++i) {
        const float nv = 0.05f + 0.95f * float(i) / 20.0f;
        share += double(ps::DiffuseShare(uint32_t(m.c), false, m.smooth, nv)) * nv;
        wsum += nv;
      }
      share /= wsum;
      total += share * m.weight;
      weight += m.weight;
      if (m.c != C::kMetal) {
        dielectric += share * m.weight;
        dielectric_weight += m.weight;
      }
    }
    std::printf("  CALIBRATION: the mean diffuse share over the city's class mix %.3f (the dielectrics alone %.3f; the metal class keeps 15 per cent)\n", total / weight, dielectric / dielectric_weight);
    Check(dielectric / dielectric_weight > 0.90 && dielectric / dielectric_weight < 1.0, "the dielectrics' mean diffuse share is within 10 per cent of today's bounce (1.0): no recalibration of the bounce's strength is needed");
  }
}

// ---------------------------------------------------------------------------
// THE SWITCH
// ---------------------------------------------------------------------------
void TestSwitch(const std::string& dir) {
  Check(ps::EnabledOfText("") && ps::EnabledOfText("1") && ps::EnabledOfText("true") && ps::EnabledOfText("True") && ps::EnabledOfText("on") && ps::EnabledOfText("2"), "the switch reads missing and 1 / true / on as ON");
  Check(!ps::EnabledOfText("0") && !ps::EnabledOfText("false") && !ps::EnabledOfText("FALSE") && !ps::EnabledOfText("off") && !ps::EnabledOfText("No"), "the switch reads 0 / false / off / no (any case) as OFF");
  Check(std::strcmp(ps::kSettingName, "mcla_gpu_pbr") == 0, "the setting's name is PBR-1's: mcla_gpu_pbr");
  std::string sw;
  if (Slurp(dir + "/pbr_screen_switch.h", &sw)) {
    Check(Has(sw, "return larecomp::settings::mcla_gpu_pbr.Get();"),
          "the accessor reads the shared bool live");
  } else {
    Check(false, "pbr_screen_switch.h is readable");
  }
  // PBR-1's setting is the one the accessor reads
  std::string manifest;
  if (Slurp(dir + "/../settings/generated/manifest.json", &manifest)) {
    Check(Has(manifest, "\"mcla_gpu_pbr\""), "the setting mcla_gpu_pbr exists in the settings manifest (PBR-1's: the accessor reads it by name)");
  }
}

// ---------------------------------------------------------------------------
// THE SOURCES
// ---------------------------------------------------------------------------
void TestSources(const std::string& dir) {
  std::string ssr_common, ssr_apply, ssr_pass, ssr_cpp;
  const bool ok = Slurp(dir + "/hlsl/ssr_common.hlsli", &ssr_common) && Slurp(dir + "/hlsl/ssr_apply_ps.hlsl", &ssr_apply) && Slurp(dir + "/hlsl/ssr_pass.hlsli", &ssr_pass) && Slurp(dir + "/ssr.cpp", &ssr_cpp);
  Check(ok, "the reflections' sources are readable");
  if (!ok) return;
  Check(Has(ssr_common, "#include \"pbr_screen_common.hlsli\"") && Has(ssr_pass, "bool SsrPbr()") && Has(ssr_cpp, "flags.pbr = pbr_screen::Enabled();"), "the reflections read the switch (the flags' bit 19) and take the model's functions");
  Check(Has(ssr_apply, "SsrFresnel(surface.cls, surface.metal, albedo, have_albedo, smoothness, nov, SsrPbr())"), "the resolve's weight is SsrFresnel (the model's reflection weight, or the first version's with the switch off)");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "";
  if (dir.empty()) {
    std::string self = __FILE__;
    const size_t slash = self.find_last_of("/\\");
    dir = slash == std::string::npos ? "." : self.substr(0, slash);
  }
  TestNames(dir);
  TestDecode();
  TestDefinitions();
  TestSwitch(dir);
  TestSources(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

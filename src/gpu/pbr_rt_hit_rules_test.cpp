// Check of the ray traced hit shading's physically based layer (PBR level 1, task PBR-1B): src/gpu/pbr_rt_hit_rules.h (the contract's arithmetic in float and the layer over it), its shim
// for the shader-as-C++ test (the real hlsl/pbr_brdf.hlsli compiled as C++), the switch (pbr_rt_hit_switch.h), the car hit shading's PBR branch (car_rt_material_rules.h ShadeReference with the frame's pbr flag), and the text of the
// shaders (hlsl/pbr_brdf.hlsli, hlsl/pbr_rt_hit.hlsli, hlsl/car_rt_hit_shade.hlsli, hlsl/city_hit_shade.hlsli, hlsl/rt_hit_frame.hlsli). A program of its own: no renderer, no game files, no GPU.
//
//   clang++ -std=c++23 -I src -I src/gpu src/gpu/pbr_rt_hit_rules_test.cpp -o pbr_rt_hit_rules_test
//   pbr_rt_hit_rules_test <src dir>
//
// What is checked, in order:
//   the contract     the twin's functions: energy, the limits of the fresnel, the metal that has no diffuse, the weights in [0, 1], the Blinn-Phong power's roughness (what the car classes' roughness is made of)
//   the shim         the shader's contract (hlsl/pbr_brdf.hlsli itself, compiled as C++ in the shim's float3: what city_hit_shade_rules_test.cpp compiles the library against) equals the twin on random inputs
//   the layer        the game's light units meet the contract: a light of the game's colour on a surface with no fresnel is albedo x colour x N.L exactly (the lit shaders' law), the irradiance form
//   calibration      what the owner asked for: a reflected wall and a road keep about today's brightness (the model of old against the physically based one, over random views and suns), the cars' too
//   the cars         the PBR branch of the car hit shading: the highlight keeps the old lobe's width and about its brightness, the lamp at night, detail 0 the shading without highlights, chrome a metal
//   the shaders' text  the flag, the includes, the tables the car shader keeps beside the twin's, the hosts that write the flag
//   the switch       the text the debug switch reads

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "car_rt_material_rules.h"
#include "hlsl_cpp_shim.h"

namespace hlsl_shim {
#include "hlsl/pbr_brdf.hlsli"
}  // namespace hlsl_shim

#include "pbr_brdf_rules.h"
#include "pbr_rt_hit_rules.h"
#include "rt_hit_frame.h"

namespace pr = mcla::gpu::pbr_rt;
namespace pc = mcla::gpu::pbr_rt::contract;
namespace crm = mcla::gpu::car_rt_material;
namespace rh = mcla::gpu::rt_hit;

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
#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #cond)
bool Near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }
std::string Num(const char* format, double a) {
  char text[256];
  std::snprintf(text, sizeof(text), format, a);
  return text;
}
std::string Num2(const char* format, double a, double b) {
  char text[256];
  std::snprintf(text, sizeof(text), format, a, b);
  return text;
}
std::string Num3(const char* format, double a, double b, double c) {
  char text[256];
  std::snprintf(text, sizeof(text), format, a, b, c);
  return text;
}

struct Lcg {
  uint32_t state = 20261122u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Next() { return float(U32() >> 8) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// A unit vector on the hemisphere around +y (y over `min_y`), uniform in the directions.
pr::V3 RandomUnit(Lcg& rng, float min_y = -1.0f) {
  for (;;) {
    const pr::V3 v{rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
    const float l2 = pr::Dot(v, v);
    if (l2 > 0.01f && l2 <= 1.0f) {
      const pr::V3 u = pr::Normalize(v);
      if (u.y >= min_y) return u;
    }
  }
}

// The numbers of an array the shader declares: `static const float <name>[N] = {a, b, ...};`
std::vector<float> ShaderArray(const std::string& text, const char* name) {
  std::vector<float> out;
  const size_t at = text.find(std::string(name) + "[");
  if (at == std::string::npos) return out;
  const size_t open = text.find('{', at), close = text.find('}', at);
  if (open == std::string::npos || close == std::string::npos || close < open) return out;
  std::string body = text.substr(open + 1, close - open - 1);
  for (char& c : body) {
    if (c == ',') c = ' ';
  }
  std::istringstream in(body);
  float v;
  while (in >> v) out.push_back(v);
  return out;
}
float ShaderScalar(const std::string& text, const char* name) {
  const size_t at = text.find(std::string("static const float ") + name + " =");
  if (at == std::string::npos) return -12345.0f;
  return std::strtof(text.c_str() + text.find('=', at) + 1, nullptr);
}

// ---------------------------------------------------------------------------
// the contract
// ---------------------------------------------------------------------------

void ContractChecks(Lcg& rng) {
  // the fresnel: F0 head on, 1 at the horizon, monotone between
  const pr::V3 f0{0.04f, 0.5f, 0.9f};
  const pr::V3 head_on = pc::FresnelSchlick(f0, 1.0f), horizon = pc::FresnelSchlick(f0, 0.0f);
  CHECK(Near(head_on.x, 0.04) && Near(head_on.y, 0.5) && Near(head_on.z, 0.9) && Near(horizon.x, 1.0) && Near(horizon.y, 1.0));
  float previous = 0.0f;
  bool monotone = true;
  for (int i = 100; i >= 0; --i) {
    const float f = pc::FresnelSchlick(f0, float(i) / 100.0f).x;
    monotone = monotone && f >= previous - 1e-6f;
    previous = f;
  }
  CHECK(monotone);

  // the surface: roughness at least the floor, metalness saturated
  const pc::Surface s = pc::MakeSurface({0, 1, 0}, {0.5f, 0.5f, 0.5f}, 0.0f, 1.7f, 0.04f);
  CHECK(Near(s.roughness, pc::kMinRoughness) && Near(s.metalness, 1.0) && Near(pc::MakeSurface({0, 1, 0}, {1, 1, 1}, 3.0f, -1.0f, 0.04f).roughness, 1.0) && Near(pc::MakeSurface({0, 1, 0}, {1, 1, 1}, 0.5f, -1.0f, 0.04f).metalness, 0.0));
  // F0: the dielectric's for a non-metal, the albedo for a metal, between by the metalness; the diffuse colour is what a metal does not have
  const pc::Surface dielectric = pc::MakeSurface({0, 1, 0}, {0.8f, 0.4f, 0.2f}, 0.5f, 0.0f, 0.04f), metal = pc::MakeSurface({0, 1, 0}, {0.8f, 0.4f, 0.2f}, 0.5f, 1.0f, 0.04f);
  CHECK(Near(pc::F0(dielectric).x, 0.04) && Near(pc::F0(metal).x, 0.8) && Near(pc::F0(metal).y, 0.4) && Near(pc::DiffuseColour(metal).x, 0.0) && Near(pc::DiffuseColour(dielectric).x, 0.8));

  // smoothness to roughness: the complement, floored; the Blinn-Phong power: alpha = sqrt(2 / (n + 2)), perceptual roughness = sqrt(alpha)
  CHECK(Near(pc::RoughnessFromSmoothness(0.0f), 1.0) && Near(pc::RoughnessFromSmoothness(1.0f), pc::kMinRoughness) && Near(pc::RoughnessFromSmoothness(0.25f), 0.75));
  CHECK(Near(pc::RoughnessFromBlinnPower(160.0f), 1.0 / 3.0, 1e-5) && Near(pc::RoughnessFromBlinnPower(14.0f), std::sqrt(std::sqrt(2.0 / 16.0)), 1e-5) && Near(pc::RoughnessFromBlinnPower(0.0f), 1.0));
  // the car classes' numbers are made of those two (hlsl kClassRoughness, kLampRoughness): the coat, glass, chrome and the lens have the sun's old lobe's roughness, the lamp the lamp lobe's
  CHECK(Near(crm::kClassRoughness[uint32_t(crm::Class::kPaint)], pc::RoughnessFromBlinnPower(crm::kSunGlintPower), 1e-5) && Near(crm::kClassRoughness[uint32_t(crm::Class::kChrome)], pc::RoughnessFromBlinnPower(crm::kSunGlintPower), 1e-5) &&
        Near(crm::kLampRoughness, pc::RoughnessFromBlinnPower(crm::kLampGlintPower), 1e-5));

  // energy: the hemisphere integral of the BRDF x N.L (radiance 1) is a reflectance: at most 1, and a white rough dielectric reflects most of the light
  double worst_reflectance = 0.0, rough_white = 0.0;
  for (int surface = 0; surface < 6; ++surface) {
    const float roughness = 0.45f + 0.11f * float(surface);  // not smoother: a mirror-like lobe is too narrow for a uniform sample (the integral would be noise)
    const pc::Surface white = pc::MakeSurface({0, 1, 0}, {1, 1, 1}, roughness, 0.0f, 0.04f);
    for (int view = 0; view < 6; ++view) {
      const pr::V3 v = RandomUnit(rng, 0.2f);
      double sum = 0.0;
      const int samples = 120000;
      for (int i = 0; i < samples; ++i) {
        const pr::V3 l = RandomUnit(rng, 0.0f);
        sum += pc::Direct(white, v, l, {1, 1, 1}).x * (2.0 * 3.14159265358979) / double(samples);  // the directions are uniform over the hemisphere (solid angle 2 pi)
      }
      worst_reflectance = std::max(worst_reflectance, sum);
      if (roughness > 0.7f) rough_white = std::max(rough_white, sum);
    }
  }
  Check(worst_reflectance < 1.12, Num("a white surface reflects no more than it receives (the most %.3f)", worst_reflectance));
  Check(rough_white > 0.85, Num("a white rough dielectric reflects most of the light (%.3f)", rough_white));

  // the weights of a reflection and of the diffuse light: in [0, 1], the diffuse weight no more than the diffuse colour, a metal none
  bool in_range = true, diffuse_ok = true, metal_dry = true;
  for (int i = 0; i < 4000; ++i) {
    const pc::Surface random = pc::MakeSurface({0, 1, 0}, {rng.Next(), rng.Next(), rng.Next()}, rng.Range(0.0f, 1.0f), rng.Next() < 0.3f ? 1.0f : rng.Next(), rng.Range(0.02f, 0.08f));
    const float nv = rng.Range(0.02f, 1.0f);
    const pr::V3 spec = pc::EnvSpecularWeight(random, nv), diffuse = pc::EnvDiffuseWeight(random, nv), colour = pc::DiffuseColour(random);
    // the split-sum fit is not exact: its B term is a few thousandths under 0 for a rough surface of F0 near 0 (so the diffuse weight is up to that much over the diffuse colour) and it never reaches 1
    in_range = in_range && spec.x >= -0.01f && spec.x <= 1.0f && spec.y >= -0.01f && spec.y <= 1.0f && spec.z >= -0.01f && spec.z <= 1.0f;
    diffuse_ok = diffuse_ok && diffuse.x <= colour.x * 1.01f + 1e-6f && diffuse.y <= colour.y * 1.01f + 1e-6f && diffuse.z <= colour.z * 1.01f + 1e-6f && diffuse.x >= 0.0f;
    if (random.metalness >= 1.0f) metal_dry = metal_dry && pc::EnvDiffuseWeight(random, nv).x == 0.0f;
  }
  CHECK(in_range && diffuse_ok && metal_dry);
  // a mirror-smooth metal head on reflects about its albedo; the weight grows to 1 at the horizon
  const pc::Surface chrome = pc::MakeSurface({0, 1, 0}, {0.9f, 0.9f, 0.9f}, 0.1f, 1.0f, 0.04f);
  CHECK(pc::EnvSpecularWeight(chrome, 1.0f).x > 0.8f && pc::EnvSpecularWeight(chrome, 1.0f).x < 0.92f && pc::EnvSpecularWeight(chrome, 0.05f).x > 0.88f);
  // N.L <= 0: no light; a light behind the surface gives nothing
  CHECK(pc::Direct(dielectric, {0, 1, 0}, {0, -1, 0}, {1, 1, 1}).x == 0.0f && pc::Direct(dielectric, {0, 1, 0}, {1, 0, 0}, {1, 1, 1}).x == 0.0f);
}

// ---------------------------------------------------------------------------
// the shim equals the twin
// ---------------------------------------------------------------------------

hlsl_shim::float3 S(pr::V3 v) { return hlsl_shim::float3(v.x, v.y, v.z); }
bool Same(const hlsl_shim::float3& a, const pr::V3& b, double tol = 2e-5) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }

void ShimChecks(Lcg& rng) {
  int wrong_direct = 0, wrong_weights = 0, wrong_other = 0;
  for (int i = 0; i < 20000; ++i) {
    const pr::V3 n = RandomUnit(rng), v = RandomUnit(rng), l = RandomUnit(rng), albedo{rng.Next(), rng.Next(), rng.Next()}, radiance{rng.Range(0, 4), rng.Range(0, 4), rng.Range(0, 4)};
    const float roughness = rng.Range(-0.1f, 1.1f), metalness = rng.Range(-0.1f, 1.1f), f0d = rng.Range(0.01f, 0.1f), nv = rng.Range(0.0f, 1.0f);
    const pc::Surface twin = pc::MakeSurface(n, albedo, roughness, metalness, f0d);
    const hlsl_shim::PbrSurface shim = hlsl_shim::PbrMakeSurface(S(n), S(albedo), roughness, metalness, f0d);
    if (!Near(shim.roughness, twin.roughness) || !Near(shim.metalness, twin.metalness)) ++wrong_other;
    if (!Same(hlsl_shim::PbrF0(shim), pc::F0(twin)) || !Same(hlsl_shim::PbrDiffuseColour(shim), pc::DiffuseColour(twin))) ++wrong_other;
    if (!Same(hlsl_shim::PbrDirect(shim, S(v), S(l), S(radiance)), pc::Direct(twin, v, l, radiance), 1e-4)) ++wrong_direct;
    if (!Same(hlsl_shim::PbrEnvSpecularWeight(shim, nv), pc::EnvSpecularWeight(twin, nv)) || !Same(hlsl_shim::PbrEnvDiffuseWeight(shim, nv), pc::EnvDiffuseWeight(twin, nv))) ++wrong_weights;
    if (!Near(hlsl_shim::PbrRoughnessFromSmoothness(nv), pc::RoughnessFromSmoothness(nv))) ++wrong_other;
    const float power = rng.Range(0.0f, 2000.0f);
    if (!Near(hlsl_shim::PbrRoughnessFromBlinnPower(power), pc::RoughnessFromBlinnPower(power))) ++wrong_other;
    const float cosine = rng.Range(0.0f, 1.0f);
    if (!Same(hlsl_shim::PbrFresnelSchlick(S(albedo), cosine), pc::FresnelSchlick(albedo, cosine))) ++wrong_other;
    if (!Near(hlsl_shim::PbrDGgx(nv, 0.3f * v.x * v.x + 0.01f), pc::DGgx(nv, 0.3f * v.x * v.x + 0.01f), 1e-4) || !Near(hlsl_shim::PbrVSmithGgxCorrelated(nv, cosine, 0.3f * v.y * v.y + 0.01f), pc::VSmithGgxCorrelated(nv, cosine, 0.3f * v.y * v.y + 0.01f), 1e-4)) ++wrong_other;
  }
  Check(wrong_direct == 0, Num("the shim's PbrDirect is the twin's (%.0f of 20000 differ)", wrong_direct));
  Check(wrong_weights == 0, Num("the shim's reflection and diffuse weights are the twin's (%.0f differ)", wrong_weights));
  Check(wrong_other == 0, Num("the shim's surface, F0, fresnel, GGX terms and roughness mappings are the twin's (%.0f differ)", wrong_other));
}

// ---------------------------------------------------------------------------
// the layer
// ---------------------------------------------------------------------------

void LayerChecks(Lcg& rng) {
  // a light of the game's colour straight on (the view, the light and the normal one direction: the half vector is them, so the fresnel is F0 = 0 here) is albedo x colour exactly: the lit shaders' own
  // law (albedo x colour x N.L with N.L 1), that is the layer's units; off that axis the (1 - F) split and the specular lobe are the model's
  bool lambert = true;
  for (int i = 0; i < 4000; ++i) {
    const pr::V3 n = RandomUnit(rng), v = RandomUnit(rng), l = RandomUnit(rng), albedo{rng.Next(), rng.Next(), rng.Next()}, colour{rng.Range(0, 3), rng.Range(0, 3), rng.Range(0, 3)};
    const pc::Surface s = pc::MakeSurface(n, albedo, 1.0f, 0.0f, 0.0f);
    const pr::V3 got = pr::Direct(s, n, n, colour);
    lambert = lambert && Near(got.x, albedo.x * colour.x, 1e-4) && Near(got.y, albedo.y * colour.y, 1e-4) && Near(got.z, albedo.z * colour.z, 1e-4);
    (void)v;
    (void)l;
  }
  Check(lambert, "a light of the game's colour straight on a surface with no fresnel is albedo x colour exactly (the layer's units)");

  // the irradiance form: E with N.L in it from a direction l is the light of colour E / N.L from l (N.L at least kMinNdl); under the floor it is the floor's
  bool irradiance = true, floor_ok = true;
  for (int i = 0; i < 4000; ++i) {
    const pr::V3 n = RandomUnit(rng), v = RandomUnit(rng), l = RandomUnit(rng), e{rng.Range(0, 2), rng.Range(0, 2), rng.Range(0, 2)};
    const pc::Surface s = pc::MakeSurface(n, {0.7f, 0.5f, 0.3f}, rng.Range(0.1f, 1.0f), rng.Range(0.0f, 1.0f), 0.04f);
    const float nl = pr::Dot(n, l);
    const pr::V3 got = pr::DirectIrradiance(s, v, l, e);
    if (nl >= pr::kMinNdl) {
      const pr::V3 want = pr::Direct(s, v, l, e * (1.0f / nl));
      irradiance = irradiance && Near(got.x, want.x, 1e-3) && Near(got.y, want.y, 1e-3) && Near(got.z, want.z, 1e-3);
    } else {
      // under the floor the light's N.L is divided by the floor instead of by itself: the response is the unfloored one times nl / kMinNdl, at most that, never negative, never a NaN
      floor_ok = floor_ok && std::isfinite(got.x) && got.x >= 0.0f && got.x <= pr::Direct(s, v, l, e * (1.0f / pr::kMinNdl)).x * (1.0f + 1e-4f) + 1e-6f;
    }
  }
  CHECK(irradiance);
  CHECK(floor_ok);
  CHECK(Near(pr::kMinNdl, 0.05));

  // the ambient: the diffuse weight times the light
  const pc::Surface s = pc::MakeSurface({0, 1, 0}, {0.6f, 0.5f, 0.4f}, 0.8f, 0.0f, 0.04f);
  const pr::V3 d = pr::Diffuse(s, 0.8f, {0.2f, 0.2f, 0.2f}), w = pc::EnvDiffuseWeight(s, 0.8f);
  CHECK(Near(d.x, w.x * 0.2) && Near(d.y, w.y * 0.2));
  const pr::V3 r = pr::Reflection(s, 0.8f, {2.0f, 1.0f, 0.5f}), rw = pc::EnvSpecularWeight(s, 0.8f);
  CHECK(Near(r.x, rw.x * 2.0) && Near(r.y, rw.y * 1.0) && Near(r.z, rw.z * 0.5));
}

// ---------------------------------------------------------------------------
// calibration: the physically based model against the model of old
// ---------------------------------------------------------------------------

struct Ratios {
  std::vector<double> v;
  void Add(double r) { v.push_back(r); }
  double Mean() const {
    double s = 0;
    for (double r : v) s += r;
    return v.empty() ? 0.0 : s / double(v.size());
  }
  double Percentile(double p) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[std::min(v.size() - 1, size_t(p * double(v.size())))];
  }
};

// A diffuse surface of albedo `albedo` and perceptual roughness `roughness` under a sky `ambient` and a sun of colour 1, seen from random views, the sun at random places: the old model is albedo x (ambient + sun N.L) (the
// hit shading's `light`), the physically based one the ambient through the diffuse weight and the sun through the direct term.
Ratios WallRatios(Lcg& rng, float albedo, float roughness, float ambient, float f0d) {
  Ratios out;
  for (int i = 0; i < 20000; ++i) {
    const pr::V3 n = pr::Normalize({rng.Range(-0.2f, 0.2f), 1.0f, rng.Range(-0.2f, 0.2f)}), v = RandomUnit(rng, 0.05f), l = RandomUnit(rng, 0.1f);
    const float nv = pr::Dot(n, v), nl = pr::Dot(n, l);
    if (nv < 0.15f || nl <= 0.0f) continue;
    const pc::Surface s = pc::MakeSurface(n, {albedo, albedo, albedo}, roughness, 0.0f, f0d);
    const double old_model = double(albedo) * (ambient + nl);
    const pr::V3 got = pr::Diffuse(s, nv, {ambient, ambient, ambient}) + pr::Direct(s, v, l, {1, 1, 1});
    out.Add(double(got.x) / old_model);
  }
  return out;
}

void CalibrationChecks(Lcg& rng) {
  // a wall: the brightness of today's, to a few per cent, whatever the view and the sun (the energy split (1 - F) and the sky's share are all that changes)
  Ratios wall = WallRatios(rng, 0.45f, 0.85f, 0.25f, 0.04f);
  const double wall_mean = wall.Mean(), wall_p5 = wall.Percentile(0.05), wall_p95 = wall.Percentile(0.95), wall_median = wall.Percentile(0.5);
  std::printf("calibration, wall (albedo 0.45, roughness 0.85): the physically based model over the old one: mean %.3f, median %.3f, 5%% %.3f, 95%% %.3f\n", wall_mean, wall_median, wall_p5, wall_p95);
  Check(wall_mean > 0.97 && wall_mean < 1.03 && wall_p5 > 0.95 && wall_p95 < 1.08, Num3("a reflected wall keeps today's brightness (mean %.3f, 5%% %.3f, 95%% %.3f)", wall_mean, wall_p5, wall_p95));
  Ratios bright = WallRatios(rng, 0.8f, 0.85f, 0.25f, 0.04f);
  CHECK(bright.Mean() > 0.96 && bright.Mean() < 1.03);
  // a road: dark, smoother: the median is today's brightness (the diffuse), the sun's highlight is what is added at the mirror direction (the mean is a few per cent over, the tail is the highlight)
  Ratios road = WallRatios(rng, 0.15f, 0.76f, 0.25f, 0.04f);
  const double road_median = road.Percentile(0.5), road_p5 = road.Percentile(0.05);
  std::printf("calibration, road (albedo 0.15, roughness 0.76): median %.3f, 5%% %.3f, mean %.3f\n", road_median, road_p5, road.Mean());
  Check(road_median > 0.97 && road_median < 1.08 && road_p5 > 0.93, Num2("a reflected road keeps today's brightness (median %.3f, 5%% %.3f); only the sun's highlight is added", road_median, road_p5));
  // a wet road is smoother still: its highlight is the point of the model, but away from it the diffuse is the old one: the 25th percentile
  Ratios wet = WallRatios(rng, 0.08f, 0.25f, 0.25f, 0.04f);
  const double wet_p25 = wet.Percentile(0.25);
  CHECK(wet_p25 > 0.85);
  // the ambient alone (a GI bounce is diffuse transport: every light goes through the diffuse weight): the physically based one is never brighter than the old and loses at most the specular layer's share
  bool gi_ok = true;
  double gi_worst = 1.0;
  for (int i = 0; i < 4000; ++i) {
    const float roughness = rng.Range(0.3f, 1.0f), albedo = rng.Range(0.05f, 0.9f), nv = rng.Range(0.15f, 1.0f);
    const pc::Surface s = pc::MakeSurface({0, 1, 0}, {albedo, albedo, albedo}, roughness, 0.0f, 0.04f);
    const double ratio = double(pr::Diffuse(s, nv, {1, 1, 1}).x) / albedo;
    gi_ok = gi_ok && ratio <= 1.0 + 1e-6 && ratio > 0.7;
    gi_worst = std::min(gi_worst, ratio);
  }
  Check(gi_ok, Num("the diffuse transport (GI) keeps at least 70 per cent of the old diffuse, never more (the least %.3f)", gi_worst));
}

// ---------------------------------------------------------------------------
// the cars
// ---------------------------------------------------------------------------

crm::ShadeInput CarBase(crm::Class cls) {
  crm::ShadeInput in;
  in.cls = cls;
  in.tint_a[0] = 0.8f;
  in.tint_a[1] = 0.3f;
  in.tint_a[2] = 0.1f;
  in.paint = false;
  in.light.valid = true;
  in.light.sun[0] = 1.0f;
  in.light.sun[1] = 0.9f;
  in.light.sun[2] = 0.8f;
  in.light.ambient[0] = in.light.ambient[1] = in.light.ambient[2] = 0.2f;
  in.env[0] = in.env[1] = in.env[2] = 0.0f;
  in.env_known = true;
  in.fresnel[0] = 0.05f;
  in.fresnel[1] = 0.6f;
  in.fresnel[2] = 4.0f;
  in.sun_known = true;
  in.night = 0.0f;
  in.open = 1.0f;
  in.height = -1.0f;
  in.tuning.detail = 1.0f;
  return in;
}
void SetDirections(crm::ShadeInput* in, pr::V3 normal, pr::V3 ray, pr::V3 to_sun) {
  in->normal[0] = normal.x;
  in->normal[1] = normal.y;
  in->normal[2] = normal.z;
  in->ray[0] = ray.x;
  in->ray[1] = ray.y;
  in->ray[2] = ray.z;
  in->to_sun[0] = to_sun.x;
  in->to_sun[1] = to_sun.y;
  in->to_sun[2] = to_sun.z;
}
float Luma3(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

void CarChecks(Lcg& rng) {
  // the tables: every class has a roughness in [floor, 1], chrome the only metal, the lens's body share
  for (uint32_t c = 0; c < crm::kClassCount; ++c) CHECK(crm::kClassRoughness[c] >= pc::kMinRoughness && crm::kClassRoughness[c] <= 1.0f && crm::kClassBody[c] > 0.0f && crm::kClassBody[c] <= 1.0f);
  CHECK(crm::ClassMetalness(crm::Class::kChrome) == 1.0f && crm::ClassMetalness(crm::Class::kPaint) == 0.0f && crm::ClassMetalness(crm::Class::kGlass) == 0.0f);
  CHECK(Near(crm::kClassBody[uint32_t(crm::Class::kLight)], crm::kDefaults[uint32_t(crm::Class::kLight)].diffuse_gain) && Near(crm::kClassBody[uint32_t(crm::Class::kPaint)], 1.0));
  CHECK(crm::kClassRoughness[uint32_t(crm::Class::kTyre)] > crm::kClassRoughness[uint32_t(crm::Class::kPaint)] && crm::kClassRoughness[uint32_t(crm::Class::kInterior)] > crm::kClassRoughness[uint32_t(crm::Class::kPaint)]);

  {
    auto in = CarBase(crm::Class::kPaint);
    in.tuning.pbr = true;
    in.sun_known = false;
    SetDirections(&in, {0, 1, 0}, {0, -1, 0}, {0, -1, 0});
    float fallback[3], known[3];
    crm::ShadeReference(in, fallback);
    in.sun_known = true;
    in.to_sun[1] = 1.0f;
    crm::ShadeReference(in, known);
    CHECK(Near(fallback[0], known[0]) && Near(fallback[1], known[1]) && Near(fallback[2], known[2]));
  }

  {
    // INTEGRATION (PBR + LU-8's lamp glow law): a lens the game's lamps drive glows by its lamps' level alone, so at noon too (the shader's glowNight is 1 for it); the others glow by the night factor
    auto in = CarBase(crm::Class::kPaint);
    in.tuning.pbr = true;
    in.emissive = 1.0f;
    SetDirections(&in, {0, 1, 0}, {0, -1, 0}, {0, 1, 0});
    float undriven[3], driven[3];
    crm::ShadeReference(in, undriven);
    in.glow_driven = true;
    crm::ShadeReference(in, driven);
    CHECK(driven[0] > undriven[0] + 0.05f && driven[1] > undriven[1] + 0.01f);
  }

  // the diffuse light of a reflected paint, away from the sun's highlight, no reflection (env 0): about the old brightness (the energy split (1 - F)), over random views and suns, with the detail 0 (no highlights) and 1
  for (const float detail : {0.0f, 1.0f}) {
    Ratios ratios;
    for (int i = 0; i < 6000; ++i) {
      const pr::V3 n = RandomUnit(rng, 0.2f), v = RandomUnit(rng, 0.0f), l = RandomUnit(rng, 0.2f);
      const pr::V3 ray{-v.x, -v.y, -v.z};
      if (pr::Dot(n, v) < 0.2f) continue;
      crm::ShadeInput in = CarBase(crm::Class::kPaint);
      in.tuning.detail = detail;
      SetDirections(&in, n, ray, l);
      float old_out[3], new_out[3];
      in.tuning.pbr = false;
      crm::ShadeReference(in, old_out);
      in.tuning.pbr = true;
      crm::ShadeReference(in, new_out);
      if (old_out[0] > 0.05f) ratios.Add(double(new_out[0]) / old_out[0]);
    }
    const double median = ratios.Percentile(0.5), p5 = ratios.Percentile(0.05);
    std::printf("calibration, paint (detail %.0f, no reflection): the physically based model over the old one: median %.3f, 5%% %.3f, 95%% %.3f\n", detail, median, p5, ratios.Percentile(0.95));
    Check(median > 0.92 && median < 1.05 && p5 > 0.8, Num3("a reflected car keeps about today's brightness (detail %.0f: median %.3f, 5%% %.3f)", detail, median, p5));
  }

  // the sun's highlight: the sun at the mirror direction of the view. The old model's lobe (kSunRadiance x the coat's fresnel x pow(cos, 160)) and the GGX peak at the same lobe's roughness are about as bright (within a
  // factor of 2.5 for a paint, over views from head on to 60 degrees), and a degree off the mirror direction both fall
  for (const float angle : {0.0f, 25.0f, 45.0f, 60.0f}) {
    const float a = angle * 3.14159265f / 180.0f;
    const pr::V3 normal{0, 1, 0}, view{std::sin(a), std::cos(a), 0.0f}, ray{-view.x, -view.y, -view.z}, mirror{-view.x, view.y, 0.0f};  // the light's direction for the mirror image: the view reflected
    crm::ShadeInput in = CarBase(crm::Class::kPaint);
    SetDirections(&in, normal, ray, mirror);
    float old_out[3], new_out[3], off_old[3], off_new[3];
    in.tuning.pbr = false;
    crm::ShadeReference(in, old_out);
    in.tuning.pbr = true;
    crm::ShadeReference(in, new_out);
    // the highlight alone: the same with the detail 0 (no highlight), taken away
    crm::ShadeInput flat = in;
    flat.tuning.detail = 0.0f;
    float flat_old[3], flat_new[3];
    flat.tuning.pbr = false;
    crm::ShadeReference(flat, flat_old);
    flat.tuning.pbr = true;
    crm::ShadeReference(flat, flat_new);
    const double old_peak = old_out[0] - flat_old[0], new_peak = new_out[0] - flat_new[0];
    Check(old_peak > 0.1 && new_peak > 0.1 && new_peak / old_peak > 0.4 && new_peak / old_peak < 2.5, Num3("the sun's highlight on a paint at %.0f degrees is about the old lobe's (old %.2f, new %.2f)", angle, old_peak, new_peak));
    // twelve degrees off the mirror direction: both have fallen to under half (the old lobe, pow(R.L, 160), is half at 3.8 degrees; GGX at the same roughness is the wider of the two, half at 8 degrees of the light's angle)
    const float b = (angle + 12.0f) * 3.14159265f / 180.0f;
    SetDirections(&in, normal, ray, pr::V3{-std::sin(b), std::cos(b), 0.0f});
    in.tuning.pbr = false;
    crm::ShadeReference(in, off_old);
    in.tuning.pbr = true;
    crm::ShadeReference(in, off_new);
    flat.tuning.pbr = false;
    SetDirections(&flat, normal, ray, pr::V3{-std::sin(b), std::cos(b), 0.0f});
    float flat_off_old[3], flat_off_new[3];
    crm::ShadeReference(flat, flat_off_old);
    flat.tuning.pbr = true;
    crm::ShadeReference(flat, flat_off_new);
    const double off_old_peak = off_old[0] - flat_off_old[0], off_new_peak = off_new[0] - flat_off_new[0];
    Check(off_old_peak < 0.5 * old_peak && off_new_peak < 0.5 * new_peak, Num2("twelve degrees off the mirror direction the highlight has fallen (old %.2f of %.2f ...)", off_old_peak, old_peak) + Num2(" (new %.2f of %.2f)", off_new_peak, new_peak));
  }

  // detail 0 is the shading without highlights: the physically based model gives the diffuse and the reflection alone (no sun lobe, no lamp lobe): the sun at the mirror direction does not change it
  {
    crm::ShadeInput in = CarBase(crm::Class::kPaint);
    in.tuning.detail = 0.0f;
    in.tuning.pbr = true;
    const pr::V3 normal{0, 1, 0}, ray{-0.5f, -std::sqrt(0.75f), 0.0f};
    SetDirections(&in, normal, ray, pr::V3{-0.5f, std::sqrt(0.75f), 0.0f});  // the mirror direction
    float at_mirror[3], elsewhere[3];
    crm::ShadeReference(in, at_mirror);
    // the same N.L, a different azimuth: only the highlight can tell them apart
    SetDirections(&in, normal, ray, pr::V3{0.0f, std::sqrt(0.75f), 0.5f});
    // N.L is the same (sqrt(0.75)); the diffuse weight is the view's (the same); the diffuse is the sun's colour x N.L x the weight: equal
    crm::ShadeReference(in, elsewhere);
    CHECK(Near(at_mirror[0], elsewhere[0], 1e-5) && Near(at_mirror[1], elsewhere[1], 1e-5));
  }

  // the street lamp at night: a lamp over the street (up) lights what faces up and its highlight appears at the mirror direction of up: the view that reflects to straight up shows more than one that does not
  {
    crm::ShadeInput in = CarBase(crm::Class::kPaint);
    in.night = 1.0f;
    in.tuning.pbr = true;
    in.light.sun[0] = in.light.sun[1] = in.light.sun[2] = 0.5f;
    const pr::V3 normal{0, 1, 0};
    const float a = 30.0f * 3.14159265f / 180.0f;
    // the lamp's lobe sits at the reflection of "up" in the surface's normal... for a flat surface facing up the reflected ray is up when the view is straight down: compare head on (the reflected ray is up) with a view at 60 degrees
    SetDirections(&in, normal, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    float head_on[3], oblique[3];
    crm::ShadeReference(in, head_on);
    SetDirections(&in, normal, {std::sin(a), -std::cos(a), 0.0f}, {0.0f, -1.0f, 0.0f});
    crm::ShadeReference(in, oblique);
    CHECK(head_on[0] > 0.0f && oblique[0] > 0.0f && head_on[0] > oblique[0] * 0.9f);
    // and it is the lamp, not the sun: at night the sun's light is gone, so with the lamp's share 0 (detail 0) nothing is left of the highlight
    crm::ShadeInput off = in;
    off.tuning.detail = 0.0f;
    SetDirections(&off, normal, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    float without[3];
    crm::ShadeReference(off, without);
    CHECK(head_on[0] > without[0]);
    // the lamp is a wider light than the sun's disc (power 14 against 160): the lobe's share at a view 20 degrees off the axis (the half vector 10 degrees off the normal) is large, where the sun's lobe of the same
    // surface is almost gone (GGX at roughness 0.5946: 0.68 of the peak there, 0.76 with the diffuse split; at 1/3: 0.09). The lobe is what the view adds over a view far from the axis (70 degrees: the half vector 35 degrees off, nothing of it left).
    const auto lamp_view = [&](float degrees) {
      const float t = degrees * 3.14159265f / 180.0f;
      SetDirections(&in, normal, {std::sin(t), -std::cos(t), 0.0f}, {0.0f, -1.0f, 0.0f});
      float out[3];
      crm::ShadeReference(in, out);
      return double(out[0]);
    };
    const double far_view = lamp_view(70.0f), peak = lamp_view(0.0f) - far_view, tilted_lobe = lamp_view(20.0f) - far_view;
    std::printf("the street lamp's highlight: peak %.3f, at 20 degrees %.3f (%.2f of the peak)\n", peak, tilted_lobe, tilted_lobe / std::max(peak, 1e-9));
    Check(peak > 0.02 && tilted_lobe > 0.5 * peak && tilted_lobe < 0.95 * peak, Num2("the street lamp's highlight is wide: at 20 degrees it keeps %.2f of its peak (%.3f)", tilted_lobe / std::max(peak, 1e-9), peak));
  }

  // chrome is a metal: no diffuse (the sun at a place the mirror ray does not see changes nothing but the highlight's tail), the environment's reflection is tinted by the albedo, about the old chrome's brightness head on
  {
    crm::ShadeInput in = CarBase(crm::Class::kChrome);
    in.tint_a[0] = in.tint_a[1] = in.tint_a[2] = 0.9f;
    in.env[0] = in.env[1] = in.env[2] = 1.0f;
    in.fresnel[0] = 0.7f;
    in.fresnel[1] = 1.0f;
    in.fresnel[2] = 5.0f;
    in.light.sun[0] = in.light.sun[1] = in.light.sun[2] = 0.0f;  // no sun: the reflection and the ambient alone
    in.light.ambient[0] = in.light.ambient[1] = in.light.ambient[2] = 0.0f;
    SetDirections(&in, {0, 1, 0}, {0, -1, 0}, {0, 1, 0});
    float old_out[3], new_out[3];
    in.tuning.pbr = false;
    crm::ShadeReference(in, old_out);
    in.tuning.pbr = true;
    crm::ShadeReference(in, new_out);
    Check(new_out[0] > 0.6f * old_out[0] && new_out[0] < 1.5f * old_out[0], Num2("chrome seen head on reflects about what it did (old %.3f, new %.3f)", old_out[0], new_out[0]));
    // a metal lit by the ambient alone has no diffuse: ambient 1 and no environment gives (nearly) nothing
    in.light.ambient[0] = in.light.ambient[1] = in.light.ambient[2] = 1.0f;
    in.env[0] = in.env[1] = in.env[2] = 0.0f;
    in.tuning.pbr = true;
    float ambient_only[3];
    crm::ShadeReference(in, ambient_only);
    CHECK(ambient_only[0] < 1e-6f);
    in.tuning.pbr = false;
    float old_ambient[3];
    crm::ShadeReference(in, old_ambient);
    CHECK(old_ambient[0] > 0.01f);  // the old chrome kept 0.1 of its diffuse: the difference the model makes
  }

  // the colour is never negative and never above the units' cap; the gain scales it; the glow is added (a lit lens)
  {
    crm::ShadeInput in = CarBase(crm::Class::kLight);
    in.emissive = 2.0f;
    in.night = 1.0f;
    in.tuning.pbr = true;
    SetDirections(&in, {0, 1, 0}, {0, -1, 0}, {0, 1, 0});
    float a[3], b[3];
    crm::ShadeReference(in, a);
    in.emissive = 0.0f;
    crm::ShadeReference(in, b);
    CHECK(a[0] > b[0] + 1.0f);
    in.tuning.gain = 2.0f;
    float c[3];
    crm::ShadeReference(in, c);
    CHECK(Near(c[0], 2.0 * b[0], 1e-5));
    bool ok = true;
    for (int i = 0; i < 2000; ++i) {
      crm::ShadeInput r = CarBase(crm::Class(uint8_t(1 + rng.Next() * 6.99f)));
      r.tuning.pbr = true;
      r.tuning.detail = rng.Next() < 0.5f ? 0.0f : rng.Range(0.1f, 3.0f);
      r.night = rng.Next();
      r.open = rng.Next();
      r.height = rng.Next() < 0.5f ? -1.0f : rng.Range(0.0f, 2.0f);
      r.env[0] = rng.Range(0, 3);
      r.env[1] = rng.Range(0, 3);
      r.env[2] = rng.Range(0, 3);
      const pr::V3 n = RandomUnit(rng, 0.0f), v = RandomUnit(rng, 0.0f), l = RandomUnit(rng, -1.0f);
      if (pr::Dot(n, v) < 0.05f) continue;
      SetDirections(&r, n, {-v.x, -v.y, -v.z}, l);
      float out[3];
      crm::ShadeReference(r, out);
      ok = ok && std::isfinite(out[0]) && out[0] >= 0.0f && std::isfinite(out[1]) && out[1] >= 0.0f && std::isfinite(out[2]) && out[2] >= 0.0f;
    }
    CHECK(ok);
  }
  // with the flag off ShadeReference is the model of old, to the bit: the physically based branch changes nothing else
  {
    bool same = true;
    for (int i = 0; i < 500; ++i) {
      crm::ShadeInput r = CarBase(crm::Class(uint8_t(1 + rng.Next() * 6.99f)));
      r.tuning.detail = rng.Range(0.0f, 2.0f);
      r.night = rng.Next();
      r.env[0] = rng.Range(0, 3);
      const pr::V3 n = RandomUnit(rng, 0.0f), v = RandomUnit(rng, 0.0f), l = RandomUnit(rng, -1.0f);
      SetDirections(&r, n, {-v.x, -v.y, -v.z}, l);
      float with_default[3], with_false[3];
      crm::ShadeReference(r, with_default);
      r.tuning.pbr = false;
      crm::ShadeReference(r, with_false);
      same = same && with_default[0] == with_false[0] && with_default[1] == with_false[1] && with_default[2] == with_false[2];
    }
    Check(same, "a frame built without the flag is the model of old (Tuning::pbr is false by default)");
  }
}

// ---------------------------------------------------------------------------
// the text of the shaders and the hosts
// ---------------------------------------------------------------------------

void TextChecks(const std::string& src) {
  const std::string layer = ReadFile(src + "/gpu/hlsl/pbr_rt_hit.hlsli"), car = ReadFile(src + "/gpu/hlsl/car_rt_hit_shade.hlsli"), city = ReadFile(src + "/gpu/hlsl/city_hit_shade.hlsli"),
                    frame = ReadFile(src + "/gpu/hlsl/rt_hit_frame.hlsli"), car_scene = ReadFile(src + "/gpu/car_rt_scene.cpp"), world = ReadFile(src + "/gpu/rt_world_frame.cpp"),
                    frame_h = ReadFile(src + "/gpu/rt_hit_frame.h");
  if (layer.empty() || car.empty() || city.empty() || frame.empty()) {
    std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    return;
  }
  // the layer: it includes the contract and calls it with the units' pi, takes no other BRDF of its own
  CHECK(Has(layer, "#include \"pbr_brdf.hlsli\"") && Has(layer, "return PbrDirect(s, v, l, lightColour * kPbrPi);") && Has(layer, "return PbrDirect(s, v, l, irradiance * (kPbrPi / max(dot(s.n, l), kPbrRtMinNdl)));") &&
        Has(layer, "return PbrEnvDiffuseWeight(s, nv) * light;") && Has(layer, "return PbrEnvSpecularWeight(s, nv) * radiance;") && Near(ShaderScalar(layer, "kPbrRtMinNdl"), pr::kMinNdl));
  CHECK(!Has(layer, "PbrDGgx") && !Has(layer, "PbrFresnelSchlick") && !Has(layer, "pow("));
  // the layer compiles as C++ (the library is tested that way): no scalar swizzle, no out parameter
  CHECK(!Has(layer, ".xxx") && !Has(layer, "out float"));
  // the frame's flag: the shader's number is the twin's, in the frame's rows
  CHECK(Has(frame, "#define RT_HIT_FRAME_PBR 32") && rh::kFramePbr == 32 && rh::kFramePbr != rh::kFrameShade && rh::kFramePbr != rh::kFrameShadow && rh::kFramePbr != rh::kFrameEnv && rh::kFramePbr != rh::kFrameSun &&
        rh::kFramePbr != rh::kFrameCascade);
  {
    rh::FrameInput in;
    uint32_t rows[rh::kFrameRows][4];
    rh::MakeFrameRows(in, rows);
    CHECK((rows[0][3] & rh::kFramePbr) == 0u && !in.tuning.pbr);  // by default: the model of old
    in.tuning.pbr = true;
    rh::MakeFrameRows(in, rows);
    CHECK((rows[0][3] & rh::kFramePbr) != 0u);
    const uint32_t others = rows[0][3] & ~rh::kFramePbr;
    in.tuning.pbr = false;
    rh::MakeFrameRows(in, rows);
    CHECK(rows[0][3] == others);  // the flag changes no other bit
  }
  // the city library: includes the layer, reads the flag from the frame, and keeps the old lines (the model of old is the branch without the flag)
  CHECK(Has(city, "#include \"pbr_rt_hit.hlsli\"") && Has(city, "(f.flags & RT_HIT_FRAME_PBR) != 0u") && Has(city, "PbrRtDirect(surface, toViewer, toSun, f.sun * visible)") &&
        Has(city, "PbrRtDirectIrradiance(surface, toViewer, gCityHitLampDirection, lampLight)") && Has(city, "PbrRtDiffuse(surface, nv, light)") && Has(city, "PbrRtReflection(surface, nv, env)") &&
        Has(city, "PbrRoughnessFromSmoothness(m.smoothness)"));
  CHECK(Has(city, "const float3 f0 = lerp(float3(kCityHitDielectricF0, kCityHitDielectricF0, kCityHitDielectricF0), albedo, metal);") && Has(city, "CityHitSchlick(f0.x, nv)") &&
        Has(city, "float3 result = albedo * light * (kCityHitDiffuseGain[cls] * (1.0 - metal));"));
  // the car shader: the same, and its tables are the twin's
  CHECK(Has(car, "#include \"pbr_rt_hit.hlsli\"") && Has(car, "(f.flags & RT_HIT_FRAME_PBR) != 0u") && Has(car, "PbrRtDirect(surface, toViewer, toSun, f.sun * (visible * daySun))") &&
        Has(car, "PbrRtDirectIrradiance(lampSurface, toViewer, up, f.sun * lampLight)") && Has(car, "PbrRtReflection(surface, nv, env)") && Has(car, "PbrRtDiffuse(surface, nv,"));
  CHECK(Has(car, "result = albedo * light * kClassDiffuseGain[m.cls] + env * (fresnel * kClassSpecGain[m.cls] * f.specGain) * tint * ground;") && Has(car, "kSunRadiance * sunLobe * visible * daySun"));
  const std::vector<float> body = ShaderArray(car, "kClassBody"), roughness = ShaderArray(car, "kClassRoughness");
  bool tables = body.size() == crm::kClassCount && roughness.size() == crm::kClassCount;
  for (uint32_t c = 0; tables && c < crm::kClassCount; ++c) tables = Near(body[c], crm::kClassBody[c], 1e-6) && Near(roughness[c], crm::kClassRoughness[c], 1e-6);
  Check(tables, "the car shader's class body shares and roughness are the twin's");
  CHECK(Near(ShaderScalar(car, "kLampRoughness"), crm::kLampRoughness, 1e-6) && Near(ShaderScalar(car, "kF0Min"), crm::kF0Min) && Near(ShaderScalar(car, "kF0Max"), crm::kF0Max));
  CHECK(Has(car, "m.cls == CAR_MAT_CLASS_CHROME ? 1.0 : 0.0") && Has(car, "clamp(m.fresnelMin, kF0Min, kF0Max)"));
  // the hosts that write the frame's rows set the flag from the switch (the only two writers), and the twin's frame has it
  if (!car_scene.empty() && !world.empty()) {
    CHECK(Has(car_scene, "in.tuning.pbr = pbr_rt::HitShadingEnabled();") && Has(car_scene, "#include \"pbr_rt_hit_switch.h\""));
    CHECK(Has(world, "hit.tuning.pbr = pbr_rt::HitShadingEnabled();") && Has(world, "#include \"pbr_rt_hit_switch.h\""));
  }
  CHECK(Has(frame_h, "inline constexpr uint32_t kFramePbr = 32;") && Has(frame_h, "bool pbr = false;"));
}

void SwitchChecks(const std::string& src) {
  // no such setting (empty) is ON; "0", "false", "off", "no" in any case are OFF; everything else is ON
  CHECK(pr::SwitchOn("") && pr::SwitchOn("1") && pr::SwitchOn("true") && pr::SwitchOn("True") && pr::SwitchOn("on") && pr::SwitchOn("00") && pr::SwitchOn("0 ") && pr::SwitchOn("2"));
  CHECK(!pr::SwitchOn("0") && !pr::SwitchOn("false") && !pr::SwitchOn("False") && !pr::SwitchOn("OFF") && !pr::SwitchOn("No") && !pr::SwitchOn("no"));
  CHECK(std::string(pr::kSettingName) == "mcla_gpu_pbr");
  // Both RT frame writers use the same live settings reader as the screen passes.
  const std::string text = ReadFile(src + "/gpu/pbr_rt_hit_switch.h");
  if (!text.empty()) {
    CHECK(Has(text, "return pbr_screen::Enabled();") && Has(text, "#include \"pbr_screen_switch.h\""));
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string src = argc > 1 ? argv[1] : "src";
  Lcg rng;
  ContractChecks(rng);
  ShimChecks(rng);
  LayerChecks(rng);
  CalibrationChecks(rng);
  CarChecks(rng);
  TextChecks(src);
  SwitchChecks(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// Check of the shared physically based light model (PBR level 1, perf/PBR.md section 1): hlsl/pbr_brdf.hlsli compiled as C++ and held to its twin (pbr_brdf_rules.h) on random surfaces and
// directions, the twin held to the laws of physics (the white furnace: the energy a surface gives back never exceeds what came in and a white metal gives all of it back; reciprocity; the
// roughness limits and the GGX normalisation; a metal against a dielectric; the Smith visibility's anchor), the roughness the G-buffer's byte means (from the game's Blinn-Phong power, the
// pixel shaders' exponent counted twice), the spherical light's energy normalisation and its agreement with a sampled disc, and the calibration against the first model (a Lambert surface
// lit by albedo x colour x scale): the diffuse term is the first model's up to the (1 - F) split, the highlight is what is new. A program of its own: no renderer, no GPU, no game; it exits
// 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_pbr_brdf_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_pbr_brdf_test [<the src/gpu folder>]     (the text checks of the .hlsli; found from this file's place without an argument)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "hlsl_cpp_shim.h"

namespace hlsl_shim {
#include "hlsl/pbr_brdf.hlsli"
}  // namespace hlsl_shim

#include "gbuffer_material.h"
#include "gbuffer_rules.h"
#include "light_frame.h"
#include "pbr_brdf_rules.h"

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
std::string Num(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[400];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}
void CheckNear(double got, double want, double tol, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g (tolerance %.3g)\n", what.c_str(), got, want, tol);
  }
}
void CheckRel(double got, double want, double rel, double floor_abs, const std::string& what) {
  CheckNear(got, want, std::max(floor_abs, rel * std::fabs(want)), what);
}

struct Lcg {
  uint32_t state = 20261122u;
  float Next() {
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  pb::V3 Direction() {
    const float z = Range(-1.0f, 1.0f);
    const float a = Range(0.0f, 6.2831853f);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
  }
  pb::V3 Upper() {  // a direction in the upper hemisphere (z > 0.02)
    pb::V3 d = Direction();
    d.z = std::fabs(d.z) + 0.02f;
    return pb::Normalize(d);
  }
};

hs::float3 H(pb::V3 v) { return hs::float3(v.x, v.y, v.z); }
pb::V3 T(hs::float3 v) { return {v.x, v.y, v.z}; }

constexpr double kPi = 3.14159265358979323846;

// The hemisphere integral of f(l) * N.L over l (n = +z), midpoint rule on a (theta, phi) grid.
template <class F>
double Hemisphere(F&& f, int nt = 360, int np = 360) {
  double sum = 0.0;
  const double dt = (kPi / 2.0) / nt, dp = (2.0 * kPi) / np;
  for (int i = 0; i < nt; ++i) {
    const double th = (i + 0.5) * dt;
    const double st = std::sin(th), ct = std::cos(th);
    for (int j = 0; j < np; ++j) {
      const double ph = (j + 0.5) * dp;
      const pb::V3 l{float(st * std::cos(ph)), float(st * std::sin(ph)), float(ct)};
      sum += f(l) * ct * st * dt * dp;
    }
  }
  return sum;
}

pb::V3 ViewAt(float nv) { return {std::sqrt(std::max(0.0f, 1.0f - nv * nv)), 0.0f, nv}; }

pb::Surface FlatSurface(pb::V3 albedo, float roughness, float metalness, float f0 = pb::kDefaultF0) {
  return pb::MakeSurface({0, 0, 1}, albedo, roughness, metalness, f0);
}

// ---------------------------------------------------------------------------
// 1. the numbers the HLSL text and the twin share
// ---------------------------------------------------------------------------
void NumberChecks() {
  CheckNear(kPbrPi, pb::kPi, 1e-7, "the .hlsli's pi is the twin's");
  CheckNear(kPbrMinRoughness, pb::kMinRoughness, 0, "min roughness");
  CheckNear(kPbrDefaultF0, pb::kDefaultF0, 0, "default F0");
  CheckNear(PBR_METAL_CHROME, pb::kMetalChrome, 0, "chrome's metalness");
  CheckNear(PBR_METAL_METAL, pb::kMetalMetal, 0, "metal's metalness");
  CheckNear(PBR_F0_WATER, pb::kF0Water, 0, "water's F0");
  CheckNear(PBR_F0_SKIN, pb::kF0Skin, 0, "skin's F0");
  CheckNear(PBR_RADIUS_HEADLIGHT, pb::kRadiusHeadlight, 0, "headlight radius");
  CheckNear(PBR_RADIUS_LAMP, pb::kRadiusLamp, 0, "lamp radius");
  CheckNear(PBR_RADIUS_NEON, pb::kRadiusNeon, 0, "neon radius");
  CheckNear(PBR_RADIUS_OTHER, pb::kRadiusOther, 0, "other radius");
  CheckNear(PBR_MIN_E, pb::kMinE, 0, "min E");
  // the class numbers are the G-buffer's, the source numbers the light frame's
  Check(PBR_CLASS_CHROME == int(gb::SurfaceClass::kChrome) && PBR_CLASS_METAL == int(gb::SurfaceClass::kMetal) && PBR_CLASS_SKIN == int(gb::SurfaceClass::kSkin) &&
            PBR_CLASS_WATER == int(gb::SurfaceClass::kWater),
        "the .hlsli's class numbers are gbuffer_types.h's");
  using LS = larecomp::gpu::LightSource;
  Check(PBR_SOURCE_HEADLIGHT == int(LS::kHeadlight) && PBR_SOURCE_LAMP == int(LS::kLamp) && PBR_SOURCE_NEON == int(LS::kNeon) && PBR_SOURCE_POLICE == int(LS::kPolice) &&
            PBR_SOURCE_MOD == int(LS::kMod),
        "the .hlsli's source numbers are light_frame.h's");
  // every class: the default metalness and the F0 agree between the text and the twin, and only chrome and metal are metals
  for (uint32_t c = 0; c < 16; ++c) {
    CheckNear(hs::PbrMetalOfClass(c), pb::MetalOfClass(c), 0, Num("metal of class %.0f", c));
    CheckNear(hs::PbrF0OfClass(c), pb::F0OfClass(c), 0, Num("F0 of class %.0f", c));
    const bool is_metal = c == uint32_t(gb::SurfaceClass::kChrome) || c == uint32_t(gb::SurfaceClass::kMetal);
    Check((pb::MetalOfClass(c) > 0.0f) == is_metal, Num("only chrome and metal are metals (class %.0f)", c));
    Check(pb::F0OfClass(c) > 0.0f && pb::F0OfClass(c) <= 0.04f, Num("a dielectric F0 of 0.02 to 0.04 (class %.0f)", c));
  }
  CheckNear(pb::MetalOfScreen(uint32_t(gb::SurfaceClass::kPaint), true), 1.0, 0, "the metal flag makes any class a metal");
  CheckNear(pb::MetalOfScreen(uint32_t(gb::SurfaceClass::kChrome), false), 1.0, 0, "chrome without the flag is still a metal");
  CheckNear(pb::MetalOfWord(uint32_t(gb::SurfaceClass::kWorld), 0.5f), 0.5, 1e-6, "a word's metalness byte on a dielectric class");
  CheckNear(pb::MetalOfWord(uint32_t(gb::SurfaceClass::kMetal), 0.2f), 0.85, 1e-6, "a word's low byte does not lower a metal class");
  CheckNear(hs::PbrMetalOfWord(3, 0.5f), 0.5, 1e-6, "the text: a word's byte");
  CheckNear(hs::PbrMetalOfScreen(8, true) - hs::PbrMetalOfScreen(8, false), 1.0, 0, "the text: the flag");
  // the light radii by source
  for (uint32_t s = 0; s < 6; ++s) CheckNear(hs::PbrLightRadius(s), pb::LightRadius(s), 0, Num("radius of source %.0f", s));
  Check(pb::LightRadius(2) > pb::LightRadius(1) && pb::LightRadius(1) > pb::LightRadius(0), "the neon tube is the biggest source, then the street lamp, then a headlight");
}

// ---------------------------------------------------------------------------
// 2. the roughness the byte means
// ---------------------------------------------------------------------------
void RoughnessChecks() {
  // the chain: power n -> alpha = sqrt(2 / (n + 2)) -> smoothness s = 1 - sqrt(alpha) -> roughness 1 - s = sqrt(alpha)
  const float powers[] = {0.5f, 1.0f, 2.0f, 10.0f, 30.0f, 100.0f, 300.0f, 1000.0f, 5000.0f};
  for (float n : powers) {
    const float alpha = std::sqrt(2.0f / (n + 2.0f));
    const float s = gb::SmoothnessOfPower(n);
    const float r_expected = std::max(std::sqrt(alpha), pb::kMinRoughness);
    CheckNear(pb::RoughnessFromSmoothness(s), r_expected, 1e-6, Num("1 - s of the stored smoothness is sqrt(alpha) of the power %.1f", n));
    CheckNear(pb::RoughnessFromBlinnPower(n), r_expected, 1e-6, Num("RoughnessFromBlinnPower(%.1f)", n));
    CheckNear(hs::PbrRoughnessFromBlinnPower(n), pb::RoughnessFromBlinnPower(n), 1e-6, "the text's power mapping");
    // the GGX alpha is the G-buffer's (RT3 -> alpha)
    const float r = pb::RoughnessFromSmoothness(s);
    if (r > pb::kMinRoughness + 1e-4f) CheckNear(r * r, gb::AlphaOfSmoothness(s), 1e-6, Num("alpha_ggx = r^2 is the G-buffer's AlphaOfSmoothness (power %.1f)", n));
  }
  // anchors (perf/PBR.md 1.2)
  CheckNear(pb::RoughnessFromSmoothness(gb::SmoothnessOfPower(10.0f)), 0.639, 0.002, "n 10 -> r 0.64");
  CheckNear(pb::RoughnessFromSmoothness(gb::SmoothnessOfPower(100.0f)), 0.374, 0.002, "n 100 -> r 0.37");
  CheckNear(pb::RoughnessFromSmoothness(gb::SmoothnessOfPower(1000.0f)), 0.212, 0.002, "n 1000 -> r 0.21");
  // the pixel shaders' exponent is counted twice before it is a power (gbuffer_material.h GlossPowerScale): a pixel bank's E and a vertex bank's 2 E are the same material
  for (float e : {1.0f, 4.0f, 25.0f, 120.0f}) {
    const float sp = gb::GlossSmoothness(gb::GlossBank::kPixel, e, 0.0f);
    const float sv = gb::GlossSmoothness(gb::GlossBank::kVertex, 2.0f * e, 0.0f);
    CheckNear(sp, sv, 1e-6, Num("a pixel bank exponent %.0f is a vertex bank's twice that", e));
    CheckNear(pb::RoughnessFromSmoothness(sp), pb::RoughnessFromBlinnPower(2.0f * e), 1e-5, Num("... and the roughness is that of the power 2 x %.0f", e));
  }
  // limits
  CheckNear(pb::RoughnessFromSmoothness(1.0f), pb::kMinRoughness, 0, "a mirror clamps at the minimum roughness");
  CheckNear(pb::RoughnessFromSmoothness(0.0f), 1.0, 0, "smoothness 0 is a chalk");
  CheckNear(pb::RoughnessFromSmoothness(2.0f), pb::kMinRoughness, 0, "smoothness above 1 is a mirror");
  CheckNear(pb::RoughnessFromSmoothness(-1.0f), 1.0, 0, "smoothness below 0 is a chalk");
  Check(std::isfinite(pb::RoughnessFromBlinnPower(0.0f)) && std::isfinite(pb::RoughnessFromBlinnPower(-5.0f)), "a power of 0 or below is a finite roughness");
  for (int i = 0; i <= 255; ++i) {
    const float s = float(i) / 255.0f;
    CheckNear(hs::PbrRoughnessFromSmoothness(s), pb::RoughnessFromSmoothness(s), 1e-7, "the text's smoothness mapping");
    if (i > 0) Check(pb::RoughnessFromSmoothness(s) <= pb::RoughnessFromSmoothness(float(i - 1) / 255.0f), "smoother is never rougher");
  }
}

// ---------------------------------------------------------------------------
// 3. the HLSL text against the twin
// ---------------------------------------------------------------------------
void TwinChecks() {
  Lcg rng;
  int worst = 0;
  double worst_rel = 0.0;
  auto Compare = [&](hs::float3 got, pb::V3 want, double rel, double floor_abs, const char* what, int i) {
    const double scale = std::max({std::fabs(double(want.x)), std::fabs(double(want.y)), std::fabs(double(want.z))});
    const double tol = std::max(floor_abs, rel * scale);
    const double err = std::max({std::fabs(double(got.x) - want.x), std::fabs(double(got.y) - want.y), std::fabs(double(got.z) - want.z)});
    if (scale > 0) worst_rel = std::max(worst_rel, err / scale);
    if (!(err <= tol)) {
      if (worst < 6) std::printf("FAILED: %s (case %d): got (%.6g %.6g %.6g), want (%.6g %.6g %.6g)\n", what, i, got.x, got.y, got.z, want.x, want.y, want.z);
      ++worst;
      ++g_failed;
    }
    ++g_checks;
  };
  for (int i = 0; i < 6000; ++i) {
    const pb::V3 n = rng.Direction();
    const pb::V3 v = pb::Normalize(n * rng.Range(0.0f, 1.0f) + rng.Direction());  // mostly in front of the surface
    const pb::V3 l = rng.Direction();
    const pb::V3 albedo{rng.Range(0, 1), rng.Range(0, 1), rng.Range(0, 1)};
    const float rough = rng.Range(0.0f, 1.0f), metal = rng.Next() < 0.3f ? (rng.Next() < 0.5f ? 0.0f : 1.0f) : rng.Next(), f0d = rng.Range(0.0f, 0.08f);
    const pb::V3 radiance{rng.Range(0, 5), rng.Range(0, 5), rng.Range(0, 5)};
    const pb::Surface ts = pb::MakeSurface(n, albedo, rough, metal, f0d);
    const hs::PbrSurface hsurf = hs::PbrMakeSurface(H(n), H(albedo), rough, metal, f0d);
    CheckNear(hsurf.roughness, ts.roughness, 1e-7, "MakeSurface: roughness");
    CheckNear(hsurf.metalness, ts.metalness, 1e-7, "MakeSurface: metalness");
    Compare(hs::PbrF0(hsurf), pb::F0(ts), 1e-5, 1e-7, "F0", i);
    Compare(hs::PbrDiffuseColour(hsurf), pb::DiffuseColour(ts), 1e-5, 1e-7, "diffuse colour", i);
    Compare(hs::PbrDirect(hsurf, H(v), H(l), H(radiance)), pb::Direct(ts, v, l, radiance), 2e-4, 1e-6, "PbrDirect", i);
    Compare(hs::PbrDiffuseLobe(hsurf, H(v), H(l)), pb::DiffuseLobe(ts, v, l), 1e-4, 1e-7, "diffuse lobe", i);
    Compare(hs::PbrSpecularLobe(hsurf, H(v), H(l)), pb::SpecularLobe(ts, v, l), 2e-4, 1e-6, "specular lobe", i);
    const float nv = rng.Range(0.0f, 1.0f);
    Compare(hs::PbrEnvSpecularWeight(hsurf, nv), pb::EnvSpecularWeight(ts, nv), 1e-5, 1e-7, "env specular weight", i);
    Compare(hs::PbrEnvDiffuseWeight(hsurf, nv), pb::EnvDiffuseWeight(ts, nv), 1e-5, 1e-7, "env diffuse weight", i);
    Compare(hs::PbrMultiScatter(H(albedo), rough, nv), pb::MultiScatter(albedo, rough, nv), 1e-5, 1e-7, "multi-scatter", i);
    const hs::float2 ab = hs::PbrDfgAnalytic(rough, nv);
    const pb::Ab tab = pb::DfgAnalytic(rough, nv);
    CheckNear(ab.x, tab.a, 1e-6, "DFG A");
    CheckNear(ab.y, tab.b, 1e-6, "DFG B");
    const pb::V3 to_light = l * rng.Range(0.5f, 40.0f);
    const float radius = rng.Range(0.0f, 0.6f);
    Compare(hs::PbrDirectSphere(hsurf, H(v), H(to_light), radius, H(radiance)), pb::DirectSphere(ts, v, to_light, radius, radiance), 5e-4, 1e-6, "PbrDirectSphere", i);
    const hs::PbrSphereLobes ho = hs::PbrSphereLightLobes(hsurf, H(v), H(to_light), radius);
    const pb::SphereLobes to = pb::SphereLightLobes(ts, v, to_light, radius);
    CheckNear(ho.nl, to.nl, 1e-5, "sphere lobes: centre N.L");
    Compare(ho.diffuse, to.diffuse, 1e-4, 1e-7, "sphere lobes: diffuse", i);
    Compare(ho.specular, to.specular, 5e-4, 1e-6, "sphere lobes: specular", i);
    // the scalar functions
    const float nh = rng.Next(), alpha = rng.Range(0.002f, 1.0f), nl = rng.Range(0.001f, 1.0f), nv2 = rng.Range(0.001f, 1.0f);
    CheckRel(hs::PbrDGgx(nh, alpha), pb::DGgx(nh, alpha), 1e-5, 1e-9, "D");
    CheckRel(hs::PbrVSmithGgxCorrelated(nv2, nl, alpha), pb::VSmithGgxCorrelated(nv2, nl, alpha), 1e-5, 1e-9, "V");
    Compare(hs::PbrFresnelSchlick(H(albedo), nh), pb::FresnelSchlick(albedo, nh), 1e-5, 1e-7, "Fresnel", i);
    const uint32_t cls = uint32_t(rng.Range(0.0f, 15.99f)), flag = rng.Next() < 0.5f;
    const float sm = rng.Next();
    const hs::PbrSurface g1 = hs::PbrSurfaceOfGbuffer(H(n), H(albedo), sm, cls, flag != 0);
    const pb::Surface g2 = pb::SurfaceOfGbuffer(n, albedo, sm, cls, flag != 0);
    CheckNear(g1.roughness, g2.roughness, 1e-7, "SurfaceOfGbuffer: roughness");
    CheckNear(g1.metalness, g2.metalness, 1e-7, "SurfaceOfGbuffer: metalness");
    CheckNear(g1.f0_dielectric, g2.f0_dielectric, 1e-7, "SurfaceOfGbuffer: F0");
  }
  std::printf("  the text against the twin: 6000 random surfaces, worst relative difference %.3g\n", worst_rel);
  Check(worst == 0, "the .hlsli compiled as C++ agrees with the twin on every random case");
}

// ---------------------------------------------------------------------------
// 4. physics
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 4a. the fitted directional albedo against the exact integral
// ---------------------------------------------------------------------------
// The exact E and B of the lobe (D with the height-correlated V, Schlick's F): the half vector is sampled with the GGX distribution's own CDF (tan^2 theta_h = alpha^2 u / (1 - u)), so the
// integrand D (n.h) cancels and the sum is smooth even at the minimum roughness; E = F0 1, B = F0 0 (E(F0) = F0 (E - B) + B). The twin of nothing: independent of the fit it checks.
void ExactDfg(double roughness, double nv, double* e_out, double* b_out) {
  const double alpha = roughness * roughness, a2 = alpha * alpha;
  const int nu = 1200, nphi = 192;
  const double vx = std::sqrt(std::max(0.0, 1.0 - nv * nv)), vz = nv;
  double e = 0, b = 0;
  for (int i = 0; i < nu; ++i) {
    const double u = (i + 0.5) / nu;
    const double tan2 = a2 * u / (1.0 - u);
    const double ct = 1.0 / std::sqrt(1.0 + tan2), st = std::sqrt(std::max(0.0, 1.0 - ct * ct));
    for (int j = 0; j < nphi; ++j) {
      const double ph = (j + 0.5) * (2.0 * kPi / nphi);
      const double hx = st * std::cos(ph), hy = st * std::sin(ph), hz = ct;
      const double vh = hx * vx + hz * vz;
      if (vh <= 0) continue;
      const double lz = 2.0 * vh * hz - vz;
      if (lz <= 0) continue;
      const double gv = lz * std::sqrt(nv * nv * (1 - a2) + a2), gl = nv * std::sqrt(lz * lz * (1 - a2) + a2);
      const double w = 0.5 / std::max(gv + gl, 1e-12) * lz * 4.0 * vh / std::max(ct, 1e-9);
      e += w;
      b += w * std::pow(1.0 - std::min(vh, 1.0), 5.0);
      (void)hy;
    }
  }
  *e_out = e / (double(nu) * nphi);
  *b_out = b / (double(nu) * nphi);
}

void DfgChecks() {
  // the integrator itself: alpha 1 at normal incidence is 1 - ln 2, and a mirror's E is 1
  double e = 0, b = 0;
  ExactDfg(1.0, 1.0, &e, &b);
  CheckNear(e, 1.0 - std::log(2.0), 1e-4, "the exact E at roughness 1, N.V 1 is 1 - ln 2");
  CheckNear(b, 0.0, 1e-3, "... and B is 0 at normal incidence");
  CheckNear(pb::DfgB(1.0f, 1.0f), 0.0, 1e-3, "the fitted B is 0 at normal incidence");
  // the fits against it
  double worst_e = 0, worst_b = 0, worst_a_neg = 0;
  int n = 0;
  for (int i = 0; i <= 13; ++i) {
    const double r = pb::kMinRoughness + (1.0 - pb::kMinRoughness) * double(i) / 13.0;
    for (int j = 0; j <= 15; ++j) {
      const double nv = 0.03 + 0.97 * double(j) / 15.0;
      ExactDfg(r, nv, &e, &b);
      const double fe = pb::DfgE(float(r), float(nv)), fb = pb::DfgB(float(r), float(nv));
      // below the fit's N.V floor (0.04) the fit is held at its edge value: compare there
      if (nv >= 0.04) {
        worst_e = std::max(worst_e, std::fabs(fe - e));
        worst_b = std::max(worst_b, std::fabs(fb - b));
      }
      const pb::Ab ab = pb::DfgAnalytic(float(r), float(nv));
      worst_a_neg = std::min(worst_a_neg, double(ab.a));
      CheckNear(ab.a + ab.b, fe, 1e-6, "A + B is E");
      ++n;
    }
  }
  std::printf("  the fitted E and B against the exact integral (%d points): worst error E %.4f, B %.4f\n", n, worst_e, worst_b);
  CheckNear(worst_e, 0.0, 0.040, "the fitted E is the exact directional albedo to 0.04");
  CheckNear(worst_b, 0.0, 0.025, "the fitted B is the exact Fresnel part to 0.025");
  Check(worst_a_neg >= -1e-6, "A is never negative");
  // the Karis fit this replaces is off by more for this lobe (the reason for the fit): at roughness 1, N.V 1 it says 0.45 where the integral is 0.31
  CheckNear(pb::DfgE(1.0f, 1.0f), 1.0 - std::log(2.0), 0.04, "E at roughness 1, N.V 1");
  // ranges: E in [0, 1], B in [0, E]; a smooth surface at normal incidence reflects everything, B follows the Fresnel's (1 - N.V)^5 for a mirror
  for (float r : {0.045f, 0.1f, 0.3f, 0.6f, 1.0f})
    for (float nv : {0.04f, 0.2f, 0.5f, 1.0f}) {
      const float fe = pb::DfgE(r, nv), fb = pb::DfgB(r, nv);
      Check(fe >= 0.0f && fe <= 1.0f && fb >= 0.0f && fb <= fe + 1e-6f, "E in [0, 1] and B in [0, E]");
    }
  CheckNear(pb::DfgE(pb::kMinRoughness, 1.0f), 1.0, 0.02, "a mirror at normal incidence reflects everything");
  CheckNear(pb::DfgB(pb::kMinRoughness, 0.3f), std::pow(0.7, 5.0), 0.03, "a mirror's B at N.V 0.3 is Schlick's (1 - N.V)^5");
  // the text's fits are the twin's
  Lcg rng;
  for (int i = 0; i < 500; ++i) {
    const float r = rng.Range(0.0f, 1.0f), nv = rng.Range(0.0f, 1.0f);
    CheckNear(hs::PbrDfgE(r, nv), pb::DfgE(r, nv), 2e-5, "the text's E is the twin's");
    CheckNear(hs::PbrDfgB(r, nv), pb::DfgB(r, nv), 2e-5, "the text's B is the twin's");
  }
}

void FurnaceChecks() {
  // WHITE FURNACE: radiance 1 from every direction: what comes back is the integral of f * N.L over the hemisphere.
  std::printf("  white furnace (albedo 1): total / specular share\n");
  const float roughs[] = {0.25f, 0.4f, 0.6f, 0.8f, 1.0f};
  const float nvs[] = {0.15f, 0.4f, 0.7f, 1.0f};
  double min_metal = 9, max_metal = 0, max_dielectric = 0, max_dielectric_graze = 0, min_dielectric = 9, min_ss = 9, max_ss = 0, min_gain = 9;
  for (float r : roughs) {
    for (float nv : nvs) {
      const pb::V3 v = ViewAt(nv);
      // a white metal: F0 = 1, no diffuse
      const pb::Surface metal = FlatSurface({1, 1, 1}, r, 1.0f);
      const double e_metal = Hemisphere([&](pb::V3 l) { return double(pb::SpecularLobe(metal, v, l).x); });
      const double e_single = Hemisphere([&](pb::V3 l) { return double(pb::SpecularLobe(metal, v, l).x) / double(pb::MultiScatter({1, 1, 1}, r, nv).x); });
      min_metal = std::min(min_metal, e_metal);
      max_metal = std::max(max_metal, e_metal);
      min_ss = std::min(min_ss, e_single);
      max_ss = std::max(max_ss, e_single);
      min_gain = std::min(min_gain, e_metal / e_single);
      // a white dielectric (F0 0.04): diffuse + specular
      const pb::Surface diel = FlatSurface({1, 1, 1}, r, 0.0f);
      const double e_diel = Hemisphere([&](pb::V3 l) { return double((pb::DiffuseLobe(diel, v, l) + pb::SpecularLobe(diel, v, l)).x); });
      if (nv >= 0.4f) max_dielectric = std::max(max_dielectric, e_diel);
      else max_dielectric_graze = std::max(max_dielectric_graze, e_diel);
      min_dielectric = std::min(min_dielectric, e_diel);
    }
  }
  std::printf("    white metal with the multi-scatter factor %.3f .. %.3f (single scattering alone %.3f .. %.3f); white dielectric %.3f .. %.3f (N.V >= 0.4: up to %.3f)\n", min_metal, max_metal, min_ss, max_ss, min_dielectric,
              max_dielectric_graze, max_dielectric);
  Check(max_metal <= 1.06, "a white metal never gives back more than it gets (<= 1.06 with the fit's error)");
  Check(min_metal >= 0.90, "a white metal gives back (nearly) all of it with the multi-scatter factor (>= 0.90 at roughness 0.25 .. 1)");
  Check(max_ss <= 1.01, "single scattering alone never gives back more than it gets");
  Check(min_ss < 0.85, "single scattering alone loses energy on the roughest metals (what the factor puts back)");
  Check(min_gain >= 1.0, "the multi-scatter factor never takes energy away");
  Check(max_dielectric <= 1.08, "a white dielectric conserves energy for N.V >= 0.4 to 8 per cent (the (1 - F(v.h)) split is per light: not exactly what the reflection took)");
  Check(max_dielectric_graze <= 1.35, "... and overshoots by less than 35 per cent at the grazing views of the (1 - F(v.h)) split (the split is per light, the reflection at a grazing view is larger)");
  Check(min_dielectric >= 0.88, "a white dielectric gives back most of it (>= 0.88)");
  // a black dielectric's total IS the specular albedo of the split sum
  double worst = 0;
  for (float r : roughs) {
    for (float nv : nvs) {
      const pb::V3 v = ViewAt(nv);
      pb::Surface black = FlatSurface({0, 0, 0}, r, 0.0f);
      const double e = Hemisphere([&](pb::V3 l) { return double(pb::SpecularLobe(black, v, l).x); });
      const pb::Ab ab = pb::DfgAnalytic(r, nv);
      const double want = double(pb::kDefaultF0) * ab.a + ab.b;  // without the (tiny) multi-scatter factor of F0 0.04
      const double ms = pb::MultiScatter(pb::Splat(pb::kDefaultF0), r, nv).x;
      worst = std::max(worst, std::fabs(e - want * ms));
    }
  }
  std::printf("    a black dielectric's total against F0 A + B: worst difference %.4f\n", worst);
  CheckNear(worst, 0.0, 0.025, "the split sum's F0 A + B is the black dielectric's furnace total (the fit's error)");
  // the environment weights are the furnace's too, and a mirror metal's weight is its F0
  for (float r : roughs) {
    for (float nv : nvs) {
      const pb::V3 v = ViewAt(nv);
      const pb::Surface metal = FlatSurface({1, 1, 1}, r, 1.0f);
      const double w = pb::EnvSpecularWeight(metal, nv).x;
      CheckNear(w, 1.0, 0.08, Num("a white metal's reflection weight is 1 (roughness %.2f, N.V %.2f)", r, nv));
      (void)v;
    }
  }
  const pb::Surface gold = FlatSurface({1.0f, 0.77f, 0.34f}, 0.3f, 1.0f);
  const pb::V3 wg = pb::EnvSpecularWeight(gold, 0.8f);
  Check(wg.x > wg.y && wg.y > wg.z, "a gold metal reflects its own colour (red > green > blue)");
}

void ReciprocityChecks() {
  Lcg rng;
  double worst = 0, worst_diffuse = 0;
  for (int i = 0; i < 3000; ++i) {
    const pb::V3 n = pb::Normalize(rng.Direction());
    const pb::V3 v = pb::Normalize(n * rng.Range(0.1f, 1.0f) + rng.Direction() * 0.5f), l = pb::Normalize(n * rng.Range(0.1f, 1.0f) + rng.Direction() * 0.5f);
    if (pb::Dot(n, v) <= 0.02f || pb::Dot(n, l) <= 0.02f) continue;
    const pb::Surface s = pb::MakeSurface(n, {rng.Next(), rng.Next(), rng.Next()}, rng.Range(0.05f, 1.0f), rng.Next(), rng.Range(0.02f, 0.06f));
    // the single scattering lobe is symmetric in (v, l); the multi-scatter factor depends on N.V alone, so divide it out
    const pb::V3 a = pb::SpecularLobe(s, v, l) * pb::Splat(1.0f);
    const pb::V3 b = pb::SpecularLobe(s, l, v);
    auto ms_of = [&](float nv) {
      return s.metalness > pb::kMsMetal ? pb::Splat(1.0f) + (pb::MultiScatter(pb::F0(s), s.roughness, std::max(nv, 1e-4f)) - pb::Splat(1.0f)) * pb::Saturate(s.metalness * pb::kMsRamp) : pb::Splat(1.0f);
    };
    const pb::V3 ma = ms_of(pb::Dot(n, v)), mb = ms_of(pb::Dot(n, l));
    const double sa = a.x / ma.x, sb = b.x / mb.x;
    worst = std::max(worst, std::fabs(sa - sb) / std::max(1e-6, std::fabs(sa)));
    const pb::V3 da = pb::DiffuseLobe(s, v, l), db = pb::DiffuseLobe(s, l, v);
    worst_diffuse = std::max(worst_diffuse, double(std::fabs(da.x - db.x) / std::max(1e-6f, std::fabs(da.x))));
  }
  std::printf("  reciprocity: specular (single scattering) worst relative difference %.2g, diffuse %.2g\n", worst, worst_diffuse);
  CheckNear(worst, 0.0, 1e-4, "f(v, l) = f(l, v) for the specular lobe's single scattering part");
  CheckNear(worst_diffuse, 0.0, 1e-5, "f(v, l) = f(l, v) for the diffuse lobe");
}

void LimitChecks() {
  // GGX at alpha 1 is flat (1 / pi) and normalised; at the minimum roughness the peak is 1 / (pi alpha^2)
  for (float nh : {0.0f, 0.3f, 0.7f, 1.0f}) CheckNear(pb::DGgx(nh, 1.0f), 1.0 / kPi, 1e-6, "GGX at alpha 1 is 1 / pi in every direction");
  const float amin = pb::kMinRoughness * pb::kMinRoughness;
  CheckRel(pb::DGgx(1.0f, amin), 1.0 / (kPi * double(amin) * amin), 0.05, 0, "GGX's peak at the minimum roughness is 1 / (pi alpha^2) (float cancellation: 5 per cent)");
  Check(pb::DGgx(1.0f, amin) > 1000.0f && std::isfinite(pb::DGgx(1.0f, amin)), "the minimum roughness' peak is big but finite");
  for (float alpha : {0.05f, 0.1f, 0.2f, 0.5f, 1.0f}) {
    // the normalisation: the integral of D(h) (n.h) over the hemisphere of half vectors is 1
    double sum = 0;
    const int nt = 200000;
    const double dt = (kPi / 2.0) / nt;
    for (int i = 0; i < nt; ++i) {
      const double th = (i + 0.5) * dt;
      sum += double(pb::DGgx(float(std::cos(th)), alpha)) * std::cos(th) * std::sin(th) * 2.0 * kPi * dt;
    }
    CheckNear(sum, 1.0, 0.01, Num("GGX is normalised (alpha %.2f)", alpha));
  }
  // the Smith visibility: 1/4 at normal incidence for any alpha, symmetric in (nl, nv), and finite at grazing angles
  for (float alpha : {0.002f, 0.1f, 0.5f, 1.0f}) {
    CheckNear(pb::VSmithGgxCorrelated(1.0f, 1.0f, alpha), 0.25, 1e-5, Num("V at nl = nv = 1 is 1/4 (alpha %.3f)", alpha));
    CheckRel(pb::VSmithGgxCorrelated(0.3f, 0.8f, alpha), pb::VSmithGgxCorrelated(0.8f, 0.3f, alpha), 1e-5, 0, "V is symmetric in (nl, nv)");
    Check(std::isfinite(pb::VSmithGgxCorrelated(1e-4f, 1e-4f, alpha)) && pb::VSmithGgxCorrelated(1e-4f, 1e-4f, alpha) > 0, "V is finite at a grazing angle");
  }
  // the lobe gets narrower and taller as the surface gets smoother (peak at the mirror direction)
  const pb::V3 v = ViewAt(0.7f);
  const pb::V3 mirror{-v.x, 0.0f, v.z};
  double previous = 0;
  for (float r : {1.0f, 0.8f, 0.6f, 0.4f, 0.2f, 0.1f}) {
    const pb::Surface s = FlatSurface({0.5f, 0.5f, 0.5f}, r, 0.0f);
    const double peak = pb::SpecularLobe(s, v, mirror).x;
    Check(peak > previous, Num("the specular peak grows as the surface gets smoother (roughness %.2f)", r));
    previous = peak;
    // off the mirror direction, sideways (the Fresnel and the visibility move the peak of a rough lobe along the plane of incidence, so the check is for the smoother surfaces)
    if (r <= 0.5f) {
      const pb::V3 off = pb::Normalize(mirror + pb::V3{0, std::sin(r * r * 2.0f), 0});
      Check(pb::SpecularLobe(s, v, off).x < peak, Num("off the mirror direction the lobe is lower (roughness %.2f)", r));
    }
  }
  // a rough surface's lobe is nearly flat: the value at 45 degrees off the mirror direction is within a factor 4 of the peak at roughness 1
  {
    const pb::Surface s = FlatSurface({0.5f, 0.5f, 0.5f}, 1.0f, 0.0f);
    const pb::V3 off = pb::Normalize(pb::V3{mirror.x + 0.5f, 0.0f, mirror.z});
    Check(pb::SpecularLobe(s, v, off).x * 4.0f > pb::SpecularLobe(s, v, mirror).x, "a chalk's lobe is broad");
  }
}

void MetalChecks() {
  const pb::V3 albedo{0.9f, 0.6f, 0.2f};
  const pb::Surface metal = FlatSurface(albedo, 0.4f, 1.0f);
  const pb::Surface diel = FlatSurface(albedo, 0.4f, 0.0f);
  const pb::V3 v = ViewAt(0.8f), l = pb::Normalize(pb::V3{0.3f, 0.2f, 0.9f});
  const pb::V3 dm = pb::DiffuseLobe(metal, v, l);
  Check(dm.x == 0.0f && dm.y == 0.0f && dm.z == 0.0f, "a metal has no diffuse light");
  const pb::V3 f0m = pb::F0(metal), f0d = pb::F0(diel);
  CheckNear(f0m.x, albedo.x, 1e-6, "a metal's F0 is its albedo (r)");
  CheckNear(f0m.y, albedo.y, 1e-6, "a metal's F0 is its albedo (g)");
  CheckNear(f0m.z, albedo.z, 1e-6, "a metal's F0 is its albedo (b)");
  CheckNear(f0d.x, 0.04, 1e-6, "a dielectric's F0 is 0.04");
  CheckNear(f0d.y, 0.04, 1e-6, "a dielectric's F0 is 0.04 in every channel (a dielectric highlight has no colour)");
  CheckNear(pb::F0(FlatSurface(albedo, 0.4f, 0.0f, 0.02f)).x, 0.02, 1e-6, "water's F0 is 0.02");
  const pb::V3 dd = pb::DiffuseLobe(diel, v, l);
  CheckNear(dd.x * kPi / albedo.x, 1.0, 0.25, "a dielectric's diffuse lobe is its albedo / pi up to the (1 - F) split");
  Check(dd.x < albedo.x / pb::kPi, "... and never above it");
  // the Fresnel goes to 1 at grazing incidence for every material, and is F0 at normal incidence
  for (const pb::Surface& s : {metal, diel}) {
    const pb::V3 f0 = pb::F0(s);
    const pb::V3 f_n = pb::FresnelSchlick(f0, 1.0f), f_g = pb::FresnelSchlick(f0, 0.0f);
    CheckNear(f_n.x, f0.x, 1e-6, "Fresnel at normal incidence is F0");
    CheckNear(f_g.x, 1.0, 1e-6, "Fresnel at grazing incidence is 1");
    CheckNear(f_g.z, 1.0, 1e-6, "Fresnel at grazing incidence is 1 (blue)");
    double prev = f0.x;
    for (float c = 0.9f; c >= 0.0f; c -= 0.1f) {
      const double f = pb::FresnelSchlick(f0, c).x;
      Check(f >= prev - 1e-7, "Fresnel rises towards grazing");
      prev = f;
    }
  }
  // metalness in between blends F0 and the diffuse colour linearly
  const pb::Surface half = FlatSurface(albedo, 0.4f, 0.5f);
  CheckNear(pb::F0(half).x, 0.5 * (albedo.x + 0.04), 1e-6, "half metal: F0 halfway");
  CheckNear(pb::DiffuseColour(half).x, 0.5 * albedo.x, 1e-6, "half metal: half the diffuse colour");
  // the reflection of a metal carries its colour, a dielectric's does not
  const pb::V3 wm = pb::EnvSpecularWeight(FlatSurface(albedo, 0.1f, 1.0f), 0.9f), wd = pb::EnvSpecularWeight(FlatSurface(albedo, 0.1f, 0.0f), 0.9f);
  Check(wm.x > 2.0f * wm.z, "a metal's reflection is coloured");
  Check(std::fabs(wd.x - wd.z) < 0.01f, "a dielectric's reflection is not");
  Check(wm.x > 5.0f * wd.x, "a metal reflects far more than a dielectric");
  // the diffuse weight of the bounce light: none for a metal, and the dielectric's takes what the reflection left
  const pb::V3 bm = pb::EnvDiffuseWeight(FlatSurface(albedo, 0.5f, 1.0f), 0.8f), bd = pb::EnvDiffuseWeight(FlatSurface(albedo, 0.5f, 0.0f), 0.8f);
  CheckNear(bm.x + bm.y + bm.z, 0.0, 1e-6, "the bounce light's diffuse weight of a metal is 0");
  Check(bd.x < albedo.x && bd.x > 0.8f * albedo.x, "... and a dielectric's is its albedo less what the reflection took");
}

// ---------------------------------------------------------------------------
// 5. the spherical light
// ---------------------------------------------------------------------------
void SphereChecks() {
  Lcg rng;
  // radius 0 is the point light
  for (int i = 0; i < 400; ++i) {
    const pb::V3 n = rng.Direction(), v = pb::Normalize(n + rng.Direction() * 0.6f), to_light = rng.Direction() * rng.Range(1.0f, 30.0f);
    const pb::Surface s = pb::MakeSurface(n, {rng.Next(), rng.Next(), rng.Next()}, rng.Range(0.1f, 1.0f), rng.Next(), 0.04f);
    const pb::V3 radiance{1, 1, 1};
    const pb::V3 point = pb::Direct(s, v, pb::Normalize(to_light), radiance), sphere = pb::DirectSphere(s, v, to_light, 0.0f, radiance);
    if (pb::Dot(n, to_light) > 0.05f * std::sqrt(pb::Dot(to_light, to_light)) && pb::Dot(n, v) > 0.05f) CheckRel(sphere.x, point.x, 2e-3, 1e-6, "radius 0 is the point light");
  }
  // at the mirror geometry the representative point is the centre and the highlight is the point light's times (alpha / alpha')^2
  for (float r : {0.1f, 0.2f, 0.4f}) {
    for (float radius : {0.08f, 0.2f, 0.5f}) {
      const pb::Surface s = FlatSurface({0.3f, 0.3f, 0.3f}, r, 0.0f);
      const pb::V3 v = ViewAt(0.6f);
      const pb::V3 to_light = pb::V3{-v.x, 0.0f, v.z} * 8.0f;
      const pb::SphereLobes o = pb::SphereLightLobes(s, v, to_light, radius);
      const double alpha = double(s.roughness) * s.roughness;
      const double aw = std::min(1.0, alpha + radius / 16.0);
      const double want = double(pb::SpecularLobe(s, v, pb::Normalize(to_light)).x) * std::pow(alpha / aw, 2.0) * pb::Normalize(to_light).z;
      CheckRel(o.specular.x, want, 1e-3, 0, Num("the mirror geometry's highlight is the point light's x (alpha / alpha')^2 (roughness %.2f, radius %.2f)", r, radius));
      Check(o.specular.x < pb::SpecularLobe(s, v, pb::Normalize(to_light)).x * pb::Normalize(to_light).z, "a sphere's highlight is lower than the point light's at the mirror geometry");
    }
  }
  // a sphere light is never brighter than the point light of the same strength anywhere on a smooth surface, and not unbounded at the minimum roughness
  {
    const pb::Surface s = FlatSurface({0.3f, 0.3f, 0.3f}, pb::kMinRoughness, 1.0f);
    const pb::V3 v = ViewAt(0.6f);
    const pb::V3 to_light = pb::V3{-v.x, 0.0f, v.z} * 6.0f;
    const double peak = pb::DirectSphere(s, v, to_light, 0.2f, {1, 1, 1}).x;
    const double point = pb::Direct(s, v, pb::Normalize(to_light), {1, 1, 1}).x;
    std::printf("  a mirror metal, the lamp's mirror point: point light %.4g, sphere %.4g\n", point, peak);
    Check(peak < point * 0.03, "a lamp's highlight on a mirror metal is a few hundredths of the unbounded point highlight");
    Check(std::isfinite(peak) && peak < 1.0e4, "... and finite");
  }
  // against a sampled disc: the energy a disc light of radius R at distance d gives, averaged over the disc's points, against the representative point approximation
  // (it is an approximation: Karis 2013; the tolerance says how much, and a mutation of the normalisation or the point breaks it by a factor)
  double worst_ratio_hi = 0, worst_ratio_lo = 1e9;
  int cases = 0;
  for (float r : {0.25f, 0.4f, 0.6f, 0.8f}) {
    for (float nv : {0.4f, 0.8f}) {
      for (float radius : {0.15f, 0.5f}) {
        for (float dist : {4.0f, 12.0f}) {
          for (float off : {0.0f, 0.05f, 0.12f}) {  // the light's direction off the mirror direction (radians-ish)
            const pb::Surface s = FlatSurface({0.5f, 0.5f, 0.5f}, r, 0.0f);
            const pb::V3 v = ViewAt(nv);
            const pb::V3 mir = pb::Normalize(pb::V3{-v.x + off, off * 0.5f, v.z});
            const pb::V3 centre = mir * dist;
            // the disc faces the surface point: two perpendicular axes
            const pb::V3 ax = pb::Normalize(pb::V3{-mir.y, mir.x, 0.0f}), ay = pb::V3{mir.y * ax.z - mir.z * ax.y, mir.z * ax.x - mir.x * ax.z, mir.x * ax.y - mir.y * ax.x};
            double sum = 0;
            int count = 0;
            for (int i = -12; i <= 12; ++i)
              for (int j = -12; j <= 12; ++j) {
                const float px = float(i) / 12.0f, py = float(j) / 12.0f;
                if (px * px + py * py > 1.0f) continue;
                const pb::V3 p = centre + ax * (px * radius) + ay * (py * radius);
                const pb::V3 l = pb::Normalize(p);
                sum += double(pb::SpecularLobe(s, v, l).x) * std::max(0.0f, pb::Dot(s.n, l));
                ++count;
              }
            const double reference = sum / count;
            const double approx = pb::SphereLightLobes(s, v, centre, radius).specular.x;
            if (reference > 1e-4) {
              worst_ratio_hi = std::max(worst_ratio_hi, approx / reference);
              worst_ratio_lo = std::min(worst_ratio_lo, approx / reference);
              ++cases;
            }
          }
        }
      }
    }
  }
  std::printf("  the representative point against a sampled disc (%d cases): approx / reference %.2f .. %.2f\n", cases, worst_ratio_lo, worst_ratio_hi);
  Check(cases > 30, "enough cases above the noise floor");
  Check(worst_ratio_hi < 1.8 && worst_ratio_lo > 0.5, "the representative point stays within 0.5 .. 1.8 of the sampled disc");
}

// ---------------------------------------------------------------------------
// 6. the calibration against the first model
// ---------------------------------------------------------------------------
// The first model of the deferred pass: albedo x colour x scale, scale = falloff x facing (N.L, or the headlight's wrap). The new one: the lobes at radiance = pi x colour x falloff, the diffuse
// lobe's N.L replaced by the same facing (the headlight's wrap is kept), plus the highlight.
struct Scene {
  const char* name;
  pb::V3 albedo;
  float roughness, metalness;
};
void CalibrationChecks() {
  const Scene scenes[] = {
      {"dry asphalt (albedo 0.12, r 0.75)", {0.12f, 0.12f, 0.12f}, 0.75f, 0.0f},
      {"plaster wall (albedo 0.45, r 0.85)", {0.45f, 0.42f, 0.38f}, 0.85f, 0.0f},
      {"a sidewalk (albedo 0.30, r 0.8)", {0.3f, 0.3f, 0.3f}, 0.8f, 0.0f},
      {"car paint (albedo 0.3 red, r 0.35)", {0.5f, 0.1f, 0.1f}, 0.35f, 0.0f},
      {"wet asphalt (albedo 0.08, r 0.15)", {0.08f, 0.08f, 0.08f}, 0.15f, 0.0f},
  };
  const float radius = pb::kRadiusLamp;
  std::printf("  calibration: new / old light over a street lamp's pool (dielectric, the camera 1.2 m up looking ahead)\n");
  for (const Scene& sc : scenes) {
    // the lamp 6 m up; the road a pool of points; the camera behind
    const pb::V3 cam{0, 1.2f, -15.0f}, lamp{0, 6.0f, 0.0f};
    double old_sum = 0, new_sum = 0, new_diffuse = 0, max_ratio = 0;
    int n = 0;
    for (int ix = -10; ix <= 10; ++ix)
      for (int iz = -8; iz <= 8; ++iz) {
        const pb::V3 p{float(ix) * 1.0f, 0.0f, float(iz) * 1.5f};
        const pb::V3 to_light = lamp - p, to_cam = cam - p;
        const float dist = std::sqrt(pb::Dot(to_light, to_light));
        const pb::V3 l = to_light * (1.0f / dist), v = pb::Normalize(to_cam);
        const pb::Surface s = pb::MakeSurface({0, 1, 0}, sc.albedo, sc.roughness, sc.metalness, 0.04f);
        const float falloff = 1.0f / std::max(25.0f, dist * dist);  // the game's lamp law (plateau 5 m)
        const double facing = std::max(0.0f, pb::Dot(s.n, l));
        const double old_light = double(sc.albedo.x) * falloff * facing;
        pb::Surface s_local = s;
        const pb::SphereLobes o = pb::SphereLightLobes(s_local, v, to_light, radius);
        const double radiance = double(pb::kPi) * falloff;
        const double now = double((o.diffuse * float(facing * radiance) + o.specular * float(radiance)).x);
        const double diffuse_only = double(o.diffuse.x) * facing * radiance;
        old_sum += old_light;
        new_sum += now;
        new_diffuse += diffuse_only;
        if (old_light > 1e-9) max_ratio = std::max(max_ratio, now / old_light);
        ++n;
      }
    const double ratio = new_sum / old_sum, diffuse_ratio = new_diffuse / old_sum;
    std::printf("    %-40s total %.3f, diffuse part %.3f, brightest point %.2fx\n", sc.name, ratio, diffuse_ratio, max_ratio);
    Check(diffuse_ratio > 0.90 && diffuse_ratio <= 1.0 + 1e-6, std::string("the diffuse part is the first model's less the (1 - F) split (0.90 .. 1.0): ") + sc.name);
    if (sc.roughness >= 0.7f) Check(ratio > 0.92 && ratio < 1.25, std::string("a rough surface's total stays within 0.92 .. 1.25 of the first model: ") + sc.name);
    else Check(ratio > 0.9 && ratio < 2.5, std::string("a smoother surface's total stays within 0.9 .. 2.5 of the first model (the highlight): ") + sc.name);
  }
  // with F0 = 0 and no metal the diffuse lobe at radiance pi x colour x scale IS the first model, exactly
  Lcg rng;
  for (int i = 0; i < 500; ++i) {
    const pb::V3 n = rng.Direction(), v = pb::Normalize(n + rng.Direction() * 0.5f), l = pb::Normalize(n + rng.Direction() * 0.7f);
    if (pb::Dot(n, l) <= 0.01f) continue;
    const pb::V3 albedo{rng.Next(), rng.Next(), rng.Next()};
    const pb::Surface s = pb::MakeSurface(n, albedo, 0.9f, 0.0f, 0.0f);
    const pb::V3 d = pb::DiffuseLobe(s, v, l) * (pb::kPi * pb::Dot(n, l));
    const pb::V3 h = pb::Normalize(v + l);
    const float split = 1.0f - std::pow(1.0f - pb::Saturate(pb::Dot(v, h)), 5.0f);  // F0 = 0: Schlick's F is (1 - v.h)^5 and the diffuse keeps what it leaves
    CheckRel(d.x, albedo.x * pb::Dot(n, l) * split, 1e-4, 1e-6, "F0 0, no metal: the diffuse lobe x pi x N.L is albedo x N.L x (1 - F) (the first model, less the split)");
    CheckRel(d.z, albedo.z * pb::Dot(n, l) * split, 1e-4, 1e-6, "... in blue too");
  }
}

// ---------------------------------------------------------------------------
// 7. the text
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void TextChecks(const std::string& gpu_dir) {
  const std::string text = ReadFile(gpu_dir + "/hlsl/pbr_brdf.hlsli");
  if (text.empty()) {
    Check(false, "hlsl/pbr_brdf.hlsli is readable from " + gpu_dir);
    return;
  }
  // the first version's signatures (feat/pbr-base) are fixed: PBR-1B and PBR-1C were written against them
  const char* signatures[] = {
      "PbrSurface PbrMakeSurface(float3 n, float3 albedo, float roughness, float metalness, float f0_dielectric)",
      "float PbrRoughnessFromSmoothness(float smoothness)",
      "float PbrRoughnessFromBlinnPower(float n)",
      "float3 PbrF0(PbrSurface s)",
      "float3 PbrDiffuseColour(PbrSurface s)",
      "float3 PbrFresnelSchlick(float3 f0, float cos_theta)",
      "float PbrDGgx(float nh, float alpha)",
      "float PbrVSmithGgxCorrelated(float nv, float nl, float alpha)",
      "float3 PbrDirect(PbrSurface s, float3 v, float3 l, float3 radiance)",
      "float3 PbrEnvSpecularWeight(PbrSurface s, float nv)",
      "float3 PbrEnvDiffuseWeight(PbrSurface s, float nv)",
  };
  for (const char* sig : signatures) Check(text.find(sig) != std::string::npos, std::string("the contract's signature is kept: ") + sig);
  Check(text.find("float3 n;") != std::string::npos && text.find("float roughness;") != std::string::npos && text.find("float metalness;") != std::string::npos &&
            text.find("float f0_dielectric;") != std::string::npos && text.find("float3 albedo;") != std::string::npos,
        "PbrSurface keeps its five fields");
  // the subset that C++ reads (this test compiled the file; these are the things the subset forbids), outside the comments
  std::string code;
  {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      const size_t c = line.find("//");
      code += (c == std::string::npos ? line : line.substr(0, c)) + "\n";
    }
  }
  Check(code.find("static const") == std::string::npos, "no `static const` (the macros are the constants)");
  Check(code.find(".xxx") == std::string::npos && code.find("out float") == std::string::npos, "no scalar swizzles and no out parameters");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir;
  if (argc > 1) {
    gpu_dir = argv[1];
  } else {
    std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    gpu_dir = slash == std::string::npos ? "." : here.substr(0, slash);
  }
  NumberChecks();
  RoughnessChecks();
  TwinChecks();
  DfgChecks();
  FurnaceChecks();
  ReciprocityChecks();
  LimitChecks();
  MetalChecks();
  SphereChecks();
  CalibrationChecks();
  TextChecks(gpu_dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

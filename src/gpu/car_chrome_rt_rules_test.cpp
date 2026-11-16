// Check of the rules of the other cars' ray traced chrome (car_chrome_rt_rules.h): the masks, the sky probe's rays, the texel's code and the chrome variant's
// read of it, the probes over small scenes of boxes, and that the shaders and the host hold the header's numbers. A program of its own: it needs no renderer,
// no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_chrome_rt_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_chrome_rt_rules_test [<the src folder, which holds gpu and shaderpack>]
//
// Without an argument the src folder is found from this file's own path (__FILE__), so the text checks run from perf/scripts/unit-test.ps1 as well.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "car_chrome_rt_rules.h"

namespace crt = mcla::gpu::car_chrome_rt;
namespace cc = mcla::gpu::car_chrome;
namespace cr = mcla::gpu::car_selfrefl;
namespace rt = mcla::gpu::car_rt_refl;
namespace rr = mcla::gpu::car_rt_rules;
namespace ci = mcla::gpu::car_rt_instances;

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

bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

// What a half float stores of x (11 significant bits): the image is RGBA16F.
float ToHalf(float x) {
  if (x == 0.0f) return 0.0f;
  int e = 0;
  const float m = std::frexp(x, &e);
  return std::ldexp(std::round(m * 2048.0f) / 2048.0f, e);
}

crt::Vec3 V(float x, float y, float z) { return {x, y, z}; }
float Length(const crt::Vec3& v) { return std::sqrt(crt::Dot(v, v)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---------------------------------------------------------------------------------------------------------------------
// Probes over boxes (a reference of hlsl/car_rt_refl_cs.hlsl ChromeProbe, ChromeSky, ChromeTexel: the first hit of a ray against axis aligned
// boxes, weighed by its distance; no self hit is needed here: the origin is lifted off a surface and no box is the surface itself)
// ---------------------------------------------------------------------------------------------------------------------
struct Box {
  float lo[3];
  float hi[3];
};

// The distance along the ray (origin o, unit direction d) to the first entry into the box, or -1.
float RayBox(const crt::Vec3& o, const crt::Vec3& d, const Box& b, float t_max) {
  float t0 = 0.0f, t1 = t_max;
  const float oo[3] = {o.x, o.y, o.z};
  const float dd[3] = {d.x, d.y, d.z};
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(dd[k]) < 1e-9f) {
      if (oo[k] < b.lo[k] || oo[k] > b.hi[k]) return -1.0f;
      continue;
    }
    float a = (b.lo[k] - oo[k]) / dd[k];
    float c = (b.hi[k] - oo[k]) / dd[k];
    if (a > c) std::swap(a, c);
    t0 = std::max(t0, a);
    t1 = std::min(t1, c);
    if (t0 > t1) return -1.0f;
  }
  return t0;
}

float Probe(const std::vector<Box>& boxes, const crt::Vec3& origin, const crt::Vec3& dir, float reach) {
  float best = -1.0f;
  for (const Box& b : boxes) {
    const float t = RayBox(origin, dir, b, reach);
    if (t >= 0.0f && (best < 0.0f || t < best)) best = t;
  }
  return best < 0.0f ? 0.0f : crt::ProbeFade(best, reach);
}

struct Weights {
  float reflect = 0.0f;
  float sky = 0.0f;
};

// The pass's texel for a point p with unit normal n seen from the camera at `eye`, the world's up (0, 1, 0).
Weights ChromeWeights(const std::vector<Box>& boxes, const crt::Vec3& p, const crt::Vec3& n, const crt::Vec3& eye) {
  const crt::Vec3 view = crt::Normalized(p + eye * -1.0f);  // from the camera to the point
  const float nv = std::fabs(crt::Dot(n, view));
  const crt::Vec3 origin = p + n * crt::OriginLift(Length(p + eye * -1.0f));
  Weights w;
  if (nv >= crt::kMinNv) {
    const crt::Vec3 mirror = view + n * (-2.0f * crt::Dot(view, n));
    w.reflect = Probe(boxes, origin, mirror, crt::kReflectReach);
  }
  float sum = 0.0f;
  const crt::Vec3 up = V(0.0f, 1.0f, 0.0f);
  for (int k = 0; k < crt::kSkyRays; ++k) {
    sum += Probe(boxes, origin, crt::SkyRay(n, up, k), crt::kSkyReach);
  }
  w.sky = sum / float(crt::kSkyRays);
  return w;
}

}  // namespace

int main(int argc, char** argv) {
  // ---- where the sources are ------------------------------------------------------------------------------------------
  std::string src;
  if (argc > 1) {
    src = argv[1];
  } else {
    std::string self = __FILE__;
    std::replace(self.begin(), self.end(), '\\', '/');
    const size_t slash = self.rfind('/');
    const std::string gpu = slash == std::string::npos ? std::string(".") : self.substr(0, slash);
    src = gpu + "/..";
  }
  std::replace(src.begin(), src.end(), '\\', '/');

  // ---- the numbers ------------------------------------------------------------------------------------------------------
  Check(Near(crt::kReflectReach, 0.25f) && Near(crt::kSkyReach, 0.2f) && Near(crt::kFadeFrom, 0.3f) && Near(crt::kFadeTo, 0.9f) && crt::kSkyRays == 4 &&
            Near(crt::kSkySpread, 0.6f) && Near(crt::kSkyUpMin, 0.2f) && Near(crt::kLiftBase, 0.002f) && Near(crt::kLiftPerMetre, 0.0005f) &&
            Near(crt::kMinNv, 0.03f),
        "the probes' numbers: 25 cm and 20 cm (the march's reaches), the march's fade 0.3 to 0.9, 4 sky rays on a cone of tan 0.6, a 2 mm lift, N.V 0.03");
  Check(crt::kReflectReach == cc::kReflectLength && crt::kSkyReach == cc::kSkyLength && crt::kMinNv == cc::kNormalMin,
        "the reaches and the grazing limit are the march's own (car_chrome_rules.h)");
  Check(crt::ProbeFade(0.0f, 0.25f) == 1.0f && crt::ProbeFade(0.3f * 0.25f, 0.25f) == 1.0f && crt::ProbeFade(0.9f * 0.25f, 0.25f) == 0.0f &&
            crt::ProbeFade(0.25f, 0.25f) == 0.0f,
        "a hit weighs 1 up to 0.3 of the reach and 0 from 0.9 of it");
  {
    bool monotonic = true;
    float last = 2.0f;
    for (float d = 0.0f; d <= 0.25f; d += 0.005f) {
      const float w = crt::ProbeFade(d, 0.25f);
      monotonic = monotonic && w <= last + 1e-6f && w >= 0.0f && w <= 1.0f;
      last = w;
    }
    Check(monotonic, "the fade only falls with distance, between 0 and 1");
  }
  Check(crt::ProbeFade(0.1f, 0.0f) == 0.0f, "a probe of no length finds nothing");
  Check(crt::OriginLift(0.0f) >= 0.002f && crt::OriginLift(10.0f) < 0.01f, "the lift is 2 mm and under a centimetre out to 10 m (a probe of centimetres must not start past a lip)");

  // ---- the masks ---------------------------------------------------------------------------------------------------------
  {
    const uint32_t paint = rr::kMaskOthers;
    const uint32_t chrome = ci::OtherMask(ci::kChromePs);
    const uint32_t other_ps = ci::OtherMask(0x1234567890ABCDEFull);
    Check(rr::kMaskOtherChrome == 0x20 && paint == 0x10 && chrome == 0x30 && other_ps == 0x10,
          "another car's draw has 0x10, and 0x30 where it is a draw of the chrome's shader");
    const auto hits = [](uint32_t ray, uint32_t instance) { return (ray & instance) != 0u; };
    Check(hits(rt::kDefaultReflectionParts, chrome) && hits(0x15u, chrome) && hits(0xFFu, chrome) && hits(rr::kMaskOthers, chrome),
          "every ray that hit another car still hits its chrome: the reflection's 0x17, the occlusion's 0x15, the check's 0xFF, the probes' 0x10");
    Check(hits(rt::kPrimaryTraceParts, chrome) && !hits(rt::kPrimaryTraceParts, paint) && !hits(rt::kPrimaryTraceParts, rr::kMaskGlass) &&
              !hits(rt::kPrimaryTraceParts, rr::kMaskTyres) && !hits(rt::kPrimaryTraceParts, rr::kMaskDriver) && hits(rt::kPrimaryTraceParts, rr::kMaskBody),
          "the primary ray finds the player's body and another car's chrome, and nothing else: not another car's paint, nor the player's glass, tyres, driver");
    Check(!hits(rt::kPrimaryParts, chrome) && hits(rt::kPrimaryParts, rr::kMaskBody), "with the switch off the primary ray is the player's body alone, as it was");
    Check(hits(crt::kProbeParts, paint) && hits(crt::kProbeParts, chrome) && !hits(crt::kProbeParts, rr::kMaskBody) && !hits(crt::kProbeParts, rr::kMaskTyres),
          "the chrome's probes hit every part of another car and none of the player's (they are centimetres long)");
    Check(rt::kPartOtherChrome == rr::kMaskOtherChrome && rt::kPrimaryTraceParts == 0x21u, "the pass's part and the scene's mask are the same bit; the primary mask is 0x21");
    Check(ci::kChromePs == 0x2D8CA0430FF88ED7ull, "xChrome::PS_Chrome's identity");
  }
  {
    // A list of other cars' draws: the chrome's draws get 0x30 in the item, the row and the stats; the others 0x10.
    ci::List list;
    std::vector<ci::OtherDraw> draws;
    std::vector<ci::OtherCar> cars(1);
    cars[0].stencil_id = 3;
    for (int i = 0; i < 6; ++i) {
      ci::OtherDraw d;
      d.stencil_id = 3;
      d.order = uint32_t(i);
      d.ps = (i % 3 == 0) ? ci::kChromePs : 0x1111111111111111ull + uint64_t(i);
      d.mesh_key = 100 + uint64_t(i);
      d.indices = 300;
      d.model_to_world = mcla::gpu::temporal::Identity();
      draws.push_back(d);
      cars[0].Add(d.model_to_world, 4.0);
    }
    const mcla::gpu::temporal::Mat4 view = mcla::gpu::temporal::Identity();
    ci::AppendOthers(&list, draws, cars, view, false, 100, 1024, [](uint64_t, ci::MeshRef* ref) {
      *ref = ci::MeshRef{};
      return true;
    });
    int chrome_items = 0, plain_items = 0;
    bool rows_match = list.rows.size() >= list.items.size();
    for (size_t i = 0; i < list.items.size(); ++i) {
      const uint32_t m = list.items[i].mask;
      (m == 0x30u ? chrome_items : plain_items) += (m == 0x30u || m == 0x10u) ? 1 : 0;
      rows_match = rows_match && (list.rows[i].flags & 0xFFu) == m;
    }
    Check(list.items.size() == 6 && chrome_items == 2 && plain_items == 4 && rows_match && list.stats.other_chrome == 2 && list.stats.other_instances == 6,
          "AppendOthers: 2 of 6 draws are the chrome's (mask 0x30 in the item and the row), 4 are 0x10, the stats say so");
  }

  // ---- the world's up in view space --------------------------------------------------------------------------------------
  {
    using Mat = mcla::gpu::temporal::Mat4;
    Mat identity = mcla::gpu::temporal::Identity();
    float out[3] = {9.0f, 9.0f, 9.0f};
    const double up[3] = {0.0, 1.0, 0.0};
    Check(ci::ViewVector(identity, false, up, out) && Near(out[0], 0.0) && Near(out[1], 1.0) && Near(out[2], 0.0), "an upright camera: the world's up is the view's up");
    // A camera pitched 30 degrees (row vectors, world to view): y' = y cos - z sin, z' = y sin + z cos. The translation does not matter for a direction.
    Mat pitched = mcla::gpu::temporal::Identity();
    const double c = std::cos(0.5235987755982988), s = std::sin(0.5235987755982988);
    pitched.m[1][1] = c;
    pitched.m[1][2] = s;
    pitched.m[2][1] = -s;
    pitched.m[2][2] = c;
    pitched.m[3][0] = 5.0;
    pitched.m[3][1] = -7.0;
    pitched.m[3][2] = 3.0;
    Check(ci::ViewVector(pitched, false, up, out) && Near(out[1], c) && Near(out[2], s) && Near(std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]), 1.0),
          "a pitched camera: the world's up leans out of the view's up by the pitch, unit length, the translation left out");
    float flipped[3];
    Check(ci::ViewVector(pitched, true, up, flipped) && Near(flipped[1], c) && Near(flipped[2], -s), "the game's own view (z negated): the same vector with z flipped, as ViewPoint does it");
    float point_a[3], point_b[3];
    const double pa[3] = {1.0, 2.0, 3.0}, pb[3] = {1.0, 3.0, 3.0};
    Check(ci::ViewPoint(pitched, false, pa, point_a) && ci::ViewPoint(pitched, false, pb, point_b) &&
              Near(point_b[0] - point_a[0], out[0]) && Near(point_b[1] - point_a[1], out[1]) && Near(point_b[2] - point_a[2], out[2]),
          "it is the step of one metre up through ViewPoint: the same transform");
    Mat zero = mcla::gpu::temporal::Identity();
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 3; ++k) zero.m[r][k] = 0.0;
    }
    Check(!ci::ViewVector(zero, false, up, out), "a matrix with no rotation gives no direction");
  }

  // ---- the sky rays ----------------------------------------------------------------------------------------------------------
  {
    const crt::Vec3 up = V(0.0f, 1.0f, 0.0f);
    const crt::Vec3 c_up = crt::SkyCentre(V(0.0f, 1.0f, 0.0f), up);
    Check(Near(c_up.y, 1.0f) && Near(c_up.x, 0.0f), "a normal that points up: the centre ray goes up");
    const crt::Vec3 c_side = crt::SkyCentre(V(1.0f, 0.0f, 0.0f), up);
    Check(Near(c_side.x, std::sqrt(0.5f)) && Near(c_side.y, std::sqrt(0.5f)), "a normal that points sideways: the centre ray goes up at 45 degrees");
    const crt::Vec3 c_down = crt::SkyCentre(V(0.0f, -1.0f, 0.0f), up);
    Check(Near(c_down.y, 1.0f), "a normal that points straight down: no sky to look at, the centre ray takes up");
    // The march's SkyDirection (car_chrome_rules.h) is the same function with the world's up.
    bool same = true;
    for (float nx = -1.0f; nx <= 1.0f; nx += 0.25f) {
      for (float ny = -1.0f; ny <= 1.0f; ny += 0.25f) {
        const float nz2 = 1.0f - nx * nx - ny * ny;
        if (nz2 < 0.0f) continue;
        const crt::Vec3 n = V(nx, ny, std::sqrt(nz2));
        const cc::Dir march = cc::SkyDirection(n.x, n.y, n.z);
        const crt::Vec3 mine = crt::SkyCentre(n, up);
        same = same && Near(march.x, mine.x, 1e-5) && Near(march.y, mine.y, 1e-5) && Near(march.z, mine.z, 1e-5);
      }
    }
    Check(same, "the centre ray is the march's sky direction, for every normal");
    // The rays: unit, deterministic, above the tangent plane, 120 degrees apart on the cone around the centre.
    bool unit = true, above = true;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int trial = 0; trial < 500; ++trial) {
      crt::Vec3 n = crt::Normalized(V(u(rng), u(rng), u(rng)));
      crt::Vec3 pitched_up = crt::Normalized(V(0.2f * u(rng), 1.0f, 0.5f * u(rng)));
      for (int k = 0; k < crt::kSkyRays; ++k) {
        const crt::Vec3 d = crt::SkyRay(n, pitched_up, k);
        unit = unit && std::fabs(Length(d) - 1.0f) < 1e-4f;
        above = above && crt::Dot(d, n) >= rt::kMinAbove - 1e-4f;
      }
    }
    Check(unit, "every sky ray is a unit vector");
    Check(above, "every sky ray is at least 0.03 above the surface's tangent plane, whatever the normal and the up");
    const crt::Vec3 n = V(0.0f, 1.0f, 0.0f);
    const crt::Vec3 d0 = crt::SkyRay(n, up, 0), d1 = crt::SkyRay(n, up, 1), d2 = crt::SkyRay(n, up, 2), d3 = crt::SkyRay(n, up, 3);
    Check(Near(crt::Dot(d0, d1), 1.0f / std::sqrt(1.0f + 0.36f), 1e-4) && Near(crt::Dot(d0, d2), crt::Dot(d0, d1), 1e-4) && Near(crt::Dot(d0, d3), crt::Dot(d0, d1), 1e-4),
          "the three rays of the cone are the same angle from the centre (tan 0.6)");
    Check(Near(crt::Dot(d1, d2), crt::Dot(d2, d3), 1e-4) && Near(crt::Dot(d1, d3), crt::Dot(d1, d2), 1e-4), "and equally far from each other: 120 degrees apart");
    const crt::Vec3 mean = (d1 + d2 + d3) * (1.0f / 3.0f);
    Check(Near(mean.x, d0.x * Length(mean), 1e-4) && Near(mean.z, d0.z * Length(mean), 1e-4), "their mean is the centre's direction (a fan, not a lean)");
    // A grazing surface: the cone's rays are lifted, the centre is not under the plane.
    const crt::Vec3 graze = crt::Normalized(V(1.0f, -0.99f, 0.0f));
    bool lifted = true;
    for (int k = 0; k < crt::kSkyRays; ++k) lifted = lifted && crt::Dot(crt::SkyRay(graze, up, k), graze) >= rt::kMinAbove - 1e-4f;
    Check(lifted, "a normal that leans down: the rays are turned up above the tangent plane, not left to dip into the surface");
    // The up the pass is given, not the camera's: a camera pitched 30 degrees up looks at a world whose up is not its y.
    const crt::Vec3 world_up = crt::Normalized(V(0.0f, std::cos(0.5236f), std::sin(0.5236f)));
    const crt::Vec3 horizontal = crt::Normalized(V(0.0f, std::sin(0.5236f), -std::cos(0.5236f)));  // a surface that faces the world's horizon (across the up)
    const crt::Vec3 centre = crt::SkyCentre(horizontal, world_up);
    Check(Near(crt::Dot(horizontal, world_up), 0.0f, 1e-4) && Near(crt::Dot(centre, world_up), std::sqrt(0.5f), 1e-4),
          "the centre ray follows the up it is given: a surface that faces the world's horizon looks 45 degrees up into the WORLD's sky, whatever the camera's pitch");
  }

  // ---- the texel ---------------------------------------------------------------------------------------------------------------
  {
    const crt::Texel t = crt::EncodeTexel(0.25f, 0.5f);
    const crt::Decoded back = crt::DecodeTexel(t.red, t.alpha);
    Check(back.chrome && Near(back.reflect, 0.25f) && Near(back.sky, 0.5f), "a texel encodes and decodes: the reflection weight in the alpha, the sky weight in the red");
    Check(t.red <= -1.0f && t.alpha >= 1.0f && t.red < crt::kChromeMarker, "the red is -(1 + w), under -1, whatever the weights; the alpha is 1 + w");
    const crt::Texel miss = crt::EncodeTexel(0.0f, 0.0f);
    const crt::Decoded none = crt::DecodeTexel(miss.red, miss.alpha);
    Check(miss.red == -1.0f && miss.alpha == 1.0f && none.chrome && none.reflect == 0.0f && none.sky == 0.0f,
          "no hit on either probe: alpha 1 (a miss), red -1: data that says 'nothing covers this tip'");
    // Through the half float the image stores: the step at 1 to 2 is 2^-10.
    bool half_ok = true;
    for (float w = 0.0f; w <= 1.0f; w += 1.0f / 64.0f) {
      for (float s = 0.0f; s <= 1.0f; s += 1.0f / 16.0f) {
        const crt::Texel e = crt::EncodeTexel(w, s);
        const crt::Decoded d = crt::DecodeTexel(ToHalf(e.red), ToHalf(e.alpha));
        half_ok = half_ok && d.chrome && std::fabs(d.reflect - w) < 0.002f && std::fabs(d.sky - s) < 0.002f;
      }
    }
    Check(half_ok, "stored as half floats the weights come back within 0.002");
    Check(crt::EncodeTexel(7.0f, -3.0f).alpha == 2.0f && crt::EncodeTexel(7.0f, -3.0f).red == -1.0f, "weights are clamped to 0..1");
    // The player's texels are never taken for it: a colour is not negative, whatever the alpha.
    bool players = true;
    for (float alpha : {0.0f, 0.4f, 1.0f, 1.3f, 2.0f, -1.5f}) {
      for (float red : {0.0f, 0.001f, 1.0f, 60000.0f}) players = players && !crt::DecodeTexel(red, alpha).chrome;
    }
    Check(players, "a player's texel (a red that is not negative, any alpha) is never another car's chrome");
    Check(!crt::DecodeTexel(-1.5f, 0.0f).chrome, "no data (alpha 0) is no data, whatever the red");
    // What the paint and the denoise make of it: the paint's read takes it as an ordinary hit of the reflection's weight and a colour that its
    // max(colour, 0) clamps to black; the aux code says no aux.
    const rt::Texel paint = rt::DecodeAlpha(t.alpha);
    Check(paint.kind == rt::Kind::kHit && Near(paint.weight, 0.25f) && std::max(t.red, 0.0f) == 0.0f, "the paint's read: a hit of the reflection probe's weight, colour clamped to black");
    Check(crt::kAuxInstanceCode == 1.0f && int(crt::kAuxInstanceCode * 255.0f + 0.5f) == 255, "the aux texel's instance is 255: no instance of the player's car (about 195, 255 at most) has it");
  }

  // ---- the chrome variant's read ---------------------------------------------------------------------------------------------
  {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    // 1. Without a chrome texel the read is car_rt_refl::ReadForPaint's, in its use and its weight: the player's path is unchanged.
    int compared = 0;
    bool identical = true;
    for (int trial = 0; trial < 4000; ++trial) {
      float wx[3], wy[3];
      rt::BSplineWeights(u01(rng) - 0.5f, wx);
      rt::BSplineWeights(u01(rng) - 0.5f, wy);
      crt::ChromeTap taps[9];
      rt::PaintTap paint[9];
      const float depth = 6.0f + 4.0f * u01(rng);
      for (int k = 0; k < 9; ++k) {
        const float pick = u01(rng);
        const float distance = pick < 0.15f ? 0.0f : depth + (u01(rng) - 0.5f) * 0.05f;
        const float code = u01(rng);
        const float alpha = code < 0.3f ? 0.0f : (code < 0.5f ? 1.0f : (code < 0.8f ? 1.0f + u01(rng) : -(1.0f + u01(rng))));
        taps[k] = {distance, alpha, u01(rng) * 3.0f};  // a colour: never negative
        paint[k].history_distance = distance;
        paint[k].alpha = alpha;
        paint[k].rgb[0] = paint[k].rgb[1] = paint[k].rgb[2] = taps[k].red;
      }
      const rt::PaintRead a = rt::ReadForPaint(wx, wy, paint, depth, 0.03f);
      const crt::ChromeRead b = crt::ReadForChrome(wx, wy, taps, depth, 0.03f);
      ++compared;
      identical = identical && a.use == b.use && (!a.use || Near(a.weight, b.reflect, 1e-5)) && !b.sky_known;
    }
    Check(compared == 4000 && identical, "on 4000 random footprints with no chrome texel the read answers when the paint's does, with the paint's weight, and never claims the sky");

    // 2. A footprint of another car's chrome: both weights, the 3x3 B-spline's mean.
    float wx[3], wy[3];
    rt::BSplineWeights(0.0f, wx);
    rt::BSplineWeights(0.0f, wy);
    const float depth = 8.0f;
    crt::ChromeTap taps[9];
    for (int k = 0; k < 9; ++k) {
      const crt::Texel e = crt::EncodeTexel(0.5f, 0.75f);
      taps[k] = {depth, e.alpha, e.red};
    }
    crt::ChromeRead r = crt::ReadForChrome(wx, wy, taps, depth, 0.03f);
    Check(r.use && r.sky_known && Near(r.reflect, 0.5f, 1e-5) && Near(r.sky, 0.75f, 1e-5), "a footprint of another car's chrome: the image answers both probes");
    // Only the centre texel is chrome, the rest of the footprint is the same surface with no data: under half of it (the centre's weight is 9/16 x ... of the spline).
    for (int k = 0; k < 9; ++k) taps[k].alpha = 0.0f;
    const crt::Texel e = crt::EncodeTexel(1.0f, 1.0f);
    taps[4] = {depth, e.alpha, e.red};
    r = crt::ReadForChrome(wx, wy, taps, depth, 0.03f);
    const float centre_share = wx[1] * wy[1];
    Check(centre_share > 0.5f ? (r.use && r.sky_known) : !r.use, "one chrome texel in a footprint of a surface with no data: the image answers only if that texel is at least half of it (" +
                                                                       std::to_string(centre_share) + ")");
    // Taps at another distance (the bumper behind a tube, other parts) are not the surface: they neither count nor spoil the footprint.
    for (int k = 0; k < 9; ++k) {
      const crt::Texel c = crt::EncodeTexel(0.0f, 1.0f);
      taps[k] = (k % 3 == 1) ? crt::ChromeTap{depth, c.alpha, c.red} : crt::ChromeTap{depth + 0.4f, 0.0f, 0.0f};
    }
    r = crt::ReadForChrome(wx, wy, taps, depth, 0.03f);
    Check(r.use && r.sky_known && Near(r.sky, 1.0f, 1e-5), "a tube's column of texels with the bumper's (other distance, no data) either side: the tube's own texels are the whole surface");
    // A mix: the player's texels cover most of it: the player's rule (the sky is not known).
    for (int k = 0; k < 9; ++k) {
      const crt::Texel p = crt::EncodeTexel(0.0f, 0.0f);
      taps[k] = (k == 0) ? crt::ChromeTap{depth, p.alpha, p.red} : crt::ChromeTap{depth, 1.5f, 0.2f};
    }
    r = crt::ReadForChrome(wx, wy, taps, depth, 0.03f);
    Check(r.use && !r.sky_known && Near(r.reflect, 0.5f, 1e-4), "the player's texels cover most of the footprint: the player's answer, the sky left to the march");
    // No data at all, and an image that is all misses of the player's: nothing is claimed that the older read did not.
    for (int k = 0; k < 9; ++k) taps[k] = {depth, 0.0f, 0.0f};
    r = crt::ReadForChrome(wx, wy, taps, depth, 0.03f);
    Check(!r.use, "no texel with data: the march");
    for (int k = 0; k < 9; ++k) taps[k] = {0.0f, 1.5f, 0.0f};
    r = crt::ReadForChrome(wx, wy, taps, depth, 0.03f);
    Check(!r.use, "no texel the history shows at the shaded point's distance: no footprint, the march");
  }

  // ---- the probes over boxes -----------------------------------------------------------------------------------------------------
  {
    // The tailpipe of the march's test (car_chrome_rules_test.cpp): the tube's top at y = 0, the lip of the bumper 2 cm above it, the camera 1.23 m up and 6.7 m away.
    const crt::Vec3 eye = V(0.0f, 1.23f, 0.0f);
    const crt::Vec3 top = V(0.0f, 0.0f, 6.72f);
    const crt::Vec3 up = V(0.0f, 1.0f, 0.0f);
    std::vector<Box> under;
    under.push_back({{-0.50f, 0.02f, 6.66f}, {0.50f, 0.30f, 6.80f}});
    const Weights covered = ChromeWeights(under, top, up, eye);
    Check(covered.sky > 0.99f, "the tube's top under a lip 2 cm above it: every sky ray hits, the weight is whole: " + std::to_string(covered.sky));
    const std::vector<Box> open;
    const Weights free_air = ChromeWeights(open, top, up, eye);
    Check(free_air.reflect == 0.0f && free_air.sky == 0.0f, "the same tip with nothing near it, in the open sun: both weights are exactly 0, so the chrome is the game's, bit for bit");
    Check(cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 0.3f, free_air.reflect, free_air.sky, 1.0f) == cc::ChromeColour(0.8f, 1.0f, 0.6f, 0.2f, 0.3f, 0.0f, 0.0f, 1.0f),
          "and the colour the variant makes of those weights is the game's");
    // The housing 6 cm ahead of a tube's top: the reflection goes into it (the march's test: weight above 0.5).
    std::vector<Box> housed;
    housed.push_back({{-0.30f, -0.08f, 6.76f}, {0.30f, 0.10f, 6.95f}});
    const Weights ahead = ChromeWeights(housed, V(0.0f, 0.0f, 6.70f), up, eye);
    Check(ahead.reflect > 0.99f, "the reflection of a tube looking into a housing 6 cm ahead: the mirror ray hits it, weight " + std::to_string(ahead.reflect));
    // A part 40 cm above the tip is beyond both probes: nothing, so a tip under an exhaust valance far above it stays lit.
    std::vector<Box> far_lip;
    far_lip.push_back({{-0.50f, 0.40f, 6.40f}, {0.50f, 0.60f, 7.00f}});
    const Weights far_weights = ChromeWeights(far_lip, top, up, eye);
    Check(far_weights.reflect == 0.0f && far_weights.sky == 0.0f, "a part 40 cm above the tip is beyond the probes (20 and 25 cm): nothing");
    // A part at 15 cm is in the fade's falling part: lighter than a close one, not nothing.
    std::vector<Box> mid_lip;
    mid_lip.push_back({{-0.50f, 0.15f, 6.40f}, {0.50f, 0.30f, 7.00f}});
    const Weights mid = ChromeWeights(mid_lip, top, up, eye);
    Check(mid.sky > 0.0f && mid.sky < covered.sky, "a part 15 cm above the tip weighs less than one at 2 cm: " + std::to_string(mid.sky));
    // A lip that covers only the centre of the fan: the share of the rays that hit is the weight (a thin part is a share, not all or nothing).
    std::vector<Box> narrow;
    narrow.push_back({{-0.005f, 0.02f, 6.715f}, {0.005f, 0.30f, 6.725f}});
    const Weights narrow_weights = ChromeWeights(narrow, top, up, eye);
    Check(narrow_weights.sky > 0.2f && narrow_weights.sky < 0.3f, "a small part, 1 cm across, over the tip: only the centre ray of the four hits, the weight is a quarter: " + std::to_string(narrow_weights.sky));
    // A bare surface at every slope: the rays leave it above the tangent plane and its own slab is never hit (the lift and kMinAbove do it).
    float worst = 0.0f;
    int cases = 0;
    for (const float depth : {3.0f, 6.7f, 12.0f, 25.0f}) {
      for (const float slope : {0.0f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f}) {
        std::vector<Box> plane;
        const float dz = 0.001f, span = 0.5f;
        for (float z = depth - span; z < depth + span; z += dz) {
          const float surface = slope * (z + 0.5f * dz - depth);
          plane.push_back({{-0.5f, surface - 0.2f, z}, {0.5f, surface, z + dz}});
        }
        const crt::Vec3 n = crt::Normalized(V(0.0f, 1.0f, -slope));
        const Weights w = ChromeWeights(plane, V(0.0f, 0.0f, depth), n, eye);
        ++cases;
        worst = std::max({worst, w.reflect, w.sky});
      }
    }
    Check(cases == 28 && worst < 0.05f, "a bare surface at 3 to 25 m and slopes 0 to 4: no probe hits its own surface (worst " + std::to_string(worst) + ")");
  }

  // ---- the shaders and the host hold the numbers --------------------------------------------------------------------------------
  {
    const std::string refl = ReadFile(src + "/gpu/hlsl/car_rt_refl_cs.hlsl");
    const std::string common = ReadFile(src + "/gpu/hlsl/car_rt_common.hlsli");
    const std::string host = ReadFile(src + "/gpu/car_rt_refl.cpp");
    const std::string host_h = ReadFile(src + "/gpu/car_rt_refl.h");
    const std::string variant = ReadFile(src + "/shaderpack/variants/car_chrome.cpp");
    const std::string variant_h = ReadFile(src + "/shaderpack/variants/car_chrome.h");
    const std::string scene = ReadFile(src + "/gpu/car_rt_scene.cpp");
    if (refl.empty() || common.empty() || host.empty() || variant.empty() || variant_h.empty() || scene.empty()) {
      std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    } else {
      Check(Has(refl, "static const float kChromeReflectReach = 0.25;") && Has(refl, "static const float kChromeSkyReach = 0.2;") &&
                Has(refl, "static const float kChromeFadeFrom = 0.3;") && Has(refl, "static const float kChromeFadeTo = 0.9;") &&
                Has(refl, "static const int kChromeSkyRays = 4;") && Has(refl, "static const float kChromeSkySpread = 0.6;") &&
                Has(refl, "static const float kChromeSkyUpMin = 0.2;") && Has(refl, "static const float kChromeLift = 0.002;") &&
                Has(refl, "static const float kChromeLiftPerMetre = 0.0005;") && Has(refl, "static const float kMinNv = 0.03;"),
            "the compute shader holds the header's numbers (reaches, fade, 4 sky rays on tan 0.6, the 0.2 centre floor, the lift, N.V)");
      Check(Has(refl, "TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, CAR_RT_MASK_OTHERS, ray)") && Has(refl, "TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.z, primary)"),
            "the probes take every part of another car (0x10), the primary ray the push's mask");
      Check(Has(refl, "(CarRtInstanceMask(CarRtLoadInstance(primaryInstance)) & CAR_RT_MASK_OTHER_CHROME) != 0u") && Has(refl, "aux = float4(0.0, 1.0, 0.0, 0.0);") &&
                Has(refl, "return float4(-(1.0 + sky), 0.0, 0.0, 1.0 + reflection);") && Has(refl, "if (nv >= kMinNv)") &&
                Has(refl, "float3 centre = normal + up;") && Has(refl, "centre = centreLength < kChromeSkyUpMin ? up : centre / centreLength;") &&
                Has(refl, "const float angle = kTwoPi * float(k - 1) / 3.0;") && Has(refl, "if (above < kMinAbove)"),
            "the shader writes the texel as the header says (alpha 1 + reflection, red -(1 + sky), aux 0 1 0 0), takes the chrome's bit from the instance, and builds the fan the same way");
      Check(Has(refl, "float4 up;") && Has(refl, "g_push.up.xyz"), "the push block has the world's up");
      Check(Has(refl, "1.0 - smoothstep(kChromeFadeFrom * reach, kChromeFadeTo * reach, query.CommittedRayT())"), "the probes' fade is the header's");
      Check(Has(common, "#define CAR_RT_MASK_OTHER_CHROME 0x20") && Has(common, "#define CAR_RT_MASK_OTHERS 0x10"), "the shader's masks are the scene's (0x10 and 0x20)");
      Check(Has(host, "chrome_others ? car_rt_refl::kPrimaryTraceParts : car_rt_refl::kPrimaryParts") && Has(host, "REXCVAR_GET(mcla_gpu_car_chrome_occlusion) && REXCVAR_GET(mcla_gpu_car_chrome_rt_others)") &&
                Has(host, "push.up[0] = view.up[0];"),
            "the host gives the primary ray the chrome's bit only where the chrome variant is drawn and the switch is on, and passes the world's up");
      Check(Has(host_h, "float up[4];") && Has(host_h, "static_assert(sizeof(Push) == 80);"), "the host's push block is the shader's: 80 bytes");
      Check(Has(scene, "car_rt_instances::ViewVector(frame.view, flip_z, world_up, view_.up)"), "the scene gives the view the world's up");
      Check(Has(variant, "if (t.r < -0.5)") && Has(variant, "saturate(-t.r - 1.0)") && Has(variant, "saturate(code - 1.0)") && Has(variant, "s.state = 6.0;"),
            "the chrome variant reads the texel by the header's rule (red under -0.5 is another car's chrome, the sky weight is -red - 1)");
      Check(Has(variant_h, "0x2D8CA0430FF88ED7ull") && Has(variant_h, "kCarChromeVariantVersion = 3"),
            "the chrome variant's identity is the header's kChromePs, and its version is 3");
    }
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

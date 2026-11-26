#include <algorithm>
#include <cmath>
#include <cstdio>

#include "rt_denoise_v2_rules.h"

// Compile the shader's arithmetic as C++ so the tests cover the code used by
// both backends. Only the scalar HLSL intrinsics and float3's shape need shims.
struct float3 {
  float x, y, z;
  float r, g, b;
  constexpr float3(float x, float y, float z) : x(x), y(y), z(z), r(x), g(y), b(z) {}
};

using std::abs;
using std::max;
float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
float lerp(float a, float b, float t) { return std::lerp(a, b, t); }

#include "hlsl/rt_denoise_v2_math.hlsli"

int main() {
  int checks = 0, failures = 0;
  const auto check = [&](bool ok, const char* name) {
    ++checks;
    if (!ok) {
      ++failures;
      std::fprintf(stderr, "%s\n", name);
    }
  };
  const auto near = [](float a, float b) { return std::abs(a - b) < 1.0e-4f; };
  const float3 yc = Dn2YCoCg({6.0f, -2.0f, 2.0f});
  check(near(yc.x, 1.0f) && near(yc.y, 2.0f) && near(yc.z, -3.0f), "YCoCg axes include signed radiance");
  const float3 rgb = Dn2Rgb(yc);
  check(near(rgb.r, 6.0f) && near(rgb.g, -2.0f) && near(rgb.b, 2.0f), "signed radiance round trips");
  check(near(Dn2Variance(0.25f, 1.0f, 4.0f, 0.75f), 1.1875f), "mixture includes the difference of the means");
  check(near(Dn2Variance(0.25f, 1.0f, 4.0f, 0.0f), 1.0f), "disoccluded pixel discards old variance");
  check(near(Dn2Variance(0.25f, 1.0f, 4.0f, 1.0f), 0.25f), "stable sample may keep its variance");
  check(Dn2AcceptGeometry(10.25f, 10.0f, 0.13f, 0.84f), "sloped surface accepts its footprint");
  check(!Dn2AcceptGeometry(10.30f, 10.0f, 0.13f, 0.84f), "depth rejects disocclusion");
  check(!Dn2AcceptGeometry(10.25f, 10.0f, 0.15f, 0.84f), "plane rejects a parallel layer");
  check(!Dn2AcceptGeometry(10.25f, 10.0f, 0.13f, 0.82f), "normal rejects a silhouette");
  check(near(Dn2Radius(20.0f, 1.0f, 0.0f), 20.0f), "young history uses the full footprint");
  check(near(Dn2Radius(20.0f, 16.0f, 0.0f), 9.6f), "stable history reduces the radius");
  check(near(Dn2Radius(20.0f, 16.0f, 0.8f), 16.0f), "persistent noise keeps a wide radius");
  check(near(Dn2FireflyCeiling(0.0f, 0.0f, 0.0f), 0.025f), "isolated bright samples have a finite dark-neighbour floor");
  check(near(Dn2FireflyCeiling(1.0f, 0.05f, 1.2f), 2.52f), "nearby highlights protect the peak");
  check(near(Dn2FireflyCeiling(1.0f, 0.5f, 1.2f), 3.12f), "noisy neighbourhood protects wider deviations");
  using namespace mcla::gpu::rt_denoise::v2;
  check(kHasHistory == 0x80000 && kFinal == 0x100000 && kIterationShift == 16 && kFeatures == 30 && kControl == 31, "host matches shader control words");
  check(ReflectionIterations(true, -1) == 5 && ReflectionIterations(false, -1) == 4 && ReflectionIterations(true, 2) == 2, "reflection budgets and explicit overrides");
  check(VisibilityIterations(1) == 4 && VisibilityIterations(2) == 3 && kAmbientIterations == 4 && kDiffuseIterations == 5, "per-effect budgets");
  std::printf("%d/%d denoiser checks passed\n", checks - failures, checks);
  return failures == 0 ? 0 : 1;
}

#include "spec_occlusion_rules.h"

#include <cstdio>
#include <limits>

#include "hlsl_cpp_shim.h"
#include "reflection_handoff_rules.h"

namespace hlsl_shim {
#include "hlsl/spec_occlusion.hlsli"
}  // namespace hlsl_shim

namespace {
namespace so = mcla::gpu::spec_occlusion;
namespace rh = mcla::gpu::reflection_handoff;

int checks = 0;
int failures = 0;

void Check(bool ok, const char* label) {
  ++checks;
  if (!ok && ++failures <= 12) {
    std::printf("FAIL: %s\n", label);
  }
}

void Formula() {
  Check(so::kPaintFlag == hlsl_shim::kSpecularOcclusionPaintFlag &&
            so::kPaintRoughness == hlsl_shim::kSpecularOcclusionPaintRoughness,
        "the paint constants agree");
  Check(rh::Frame{}.specular_ao == 0xFFFFu, "no descriptor until AO is published");
  for (const float ao : {1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()}) {
    Check(so::Visibility(0.0f, ao, 0.0f) == 1.0f && hlsl_shim::SpecularOcclusion(0.0f, ao, 0.0f) == 1.0f,
          "open and missing AO are neutral");
  }
  for (const float ao : {0.0f, -0.5f}) {
    Check(so::Visibility(1.0f, ao, 1.0f) == 0.0f && hlsl_shim::SpecularOcclusion(1.0f, ao, 1.0f) == 0.0f,
          "fully occluded is dark");
  }

  for (int v = 0; v <= 32; ++v) {
    const float no_v = float(v) / 32.0f;
    for (int r = 0; r <= 32; ++r) {
      const float roughness = float(r) / 32.0f;
      float previous = 0.0f;
      for (int a = 0; a <= 32; ++a) {
        const float ao = float(a) / 32.0f;
        const float cpu = so::Visibility(no_v, ao, roughness);
        const float shader = hlsl_shim::SpecularOcclusion(no_v, ao, roughness);
        const double expected = std::clamp(std::pow(double(no_v) + ao, std::pow(2.0, -16.0 * roughness - 1.0)) - (1.0 - ao), 0.0, 1.0);
        Check(std::abs(cpu - expected) < 2e-7 && std::abs(shader - expected) < 2e-7, "Frostbite formula and HLSL twin");
        Check(0.0f <= shader && shader <= 1.0f && shader >= previous, "AO is monotonic and bounded");
        previous = shader;
        if (a > 0 && r == 32) {
          Check(std::abs(shader - ao) < 3e-5f, "the rough limit approaches diffuse visibility");
        }
      }
    }
  }
  Check(so::Visibility(-1.0f, 0.6f, -1.0f) == so::Visibility(0.0f, 0.6f, 0.0f) &&
            hlsl_shim::SpecularOcclusion(-1.0f, 0.6f, -1.0f) == hlsl_shim::SpecularOcclusion(0.0f, 0.6f, 0.0f),
        "negative cosine and roughness clamp");
  Check(so::Visibility(2.0f, 0.6f, 2.0f) == so::Visibility(1.0f, 0.6f, 1.0f) &&
            hlsl_shim::SpecularOcclusion(2.0f, 0.6f, 2.0f) == hlsl_shim::SpecularOcclusion(1.0f, 0.6f, 1.0f),
        "cosine and roughness clamp at one");
}

void Ladder() {
  const float visibility = hlsl_shim::SpecularOcclusion(0.25f, 0.35f, 0.15f);
  const float stock = 4.0f;
  const float screen = 8.0f;
  const float reuse = 2.0f;
  const float rt = 6.0f;
  const auto resolved = [&](rh::Weights weights, float ao_factor, float guest) {
    return weights.screen * screen + weights.reuse * reuse + weights.rt * rt +
           stock * ao_factor * (weights.planar + weights.env) - guest;
  };
  for (float planar : {0.0f, 1.0f}) {
    const auto miss = rh::Resolve(0.0f, 0.0f, 0.0f, planar);
    Check(resolved(miss, 1.0f, stock) == 0.0f, "a neutral miss preserves the guest fallback");
    Check(resolved(miss, visibility, stock) < 0.0f, "an occluded miss darkens the existing guest fallback");
    Check(resolved(miss, visibility, 0.0f) < stock, "an additive fallback is also attenuated");
    for (const auto full : {rh::Resolve(1.0f, 0.0f, 0.0f, planar), rh::Resolve(0.0f, 1.0f, 0.0f, planar),
                            rh::Resolve(0.0f, 0.0f, 1.0f, planar)}) {
      Check(resolved(full, visibility, stock) == resolved(full, 1.0f, stock), "a full-confidence hit is independent of AO");
    }
  }
}
}  // namespace

int main() {
  Formula();
  Ladder();
  std::printf("spec_occlusion_test: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}

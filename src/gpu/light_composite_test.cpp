#include "light_composite_rules.h"

#include <array>
#include <cstdio>

namespace lc = mcla::gpu::light_composite;
namespace lg = larecomp::gpu;

namespace shader {
using std::abs;
using std::max;
float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
#include "hlsl/light_composite_common.hlsli"
}  // namespace shader

unsigned checks = 0;
unsigned failures = 0;

void Check(bool ok, const char* name) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAILED: %s\n", name);
  }
}

int main() {
  lg::Light light;
  light.source = lg::LightSource::kNeon;
  light.law = lg::LightLaw::kNeon;
  Check(lc::Smooth(light, false), "smooth neon goes into irradiance");
  Check(!lc::Smooth(light, true), "screen capture keeps its own direct light");
  light.shadow.kind = lg::ShadowKind::kAtlas;
  Check(!lc::Smooth(light, false), "atlas visibility stays full size");
  light.shadow.kind = lg::ShadowKind::kRtMask;
  Check(!lc::Smooth(light, false), "ray visibility reference stays full size");
  light.shadow.kind = lg::ShadowKind::kNone;
  light.flags = lg::kLightFlagRtVisibility;
  Check(!lc::Smooth(light, false), "assigned ray channel stays full size");
  light.flags = lg::kLightFlagGameLit;
  Check(!lc::Smooth(light, false), "game-lit subtraction stays full size");
  light.flags = 0;
  light.source = lg::LightSource::kLamp;
  light.law = lg::LightLaw::kOmni;
  Check(!lc::Smooth(light, false), "street lamps stay full size");
  light.source = lg::LightSource::kHeadlight;
  Check(!lc::Smooth(light, false), "headlight edges stay full size");
  light.source = lg::LightSource::kMod;
  Check(lc::Smooth(light, false), "unshadowed point light can be combined");
  light.law = lg::LightLaw::kGameLamp;
  Check(!lc::Smooth(light, false), "sharp lamp cone stays full size");
  Check(lc::WorldVirtual(true, false, 0.0f), "street lights bounce with no field");
  Check(!lc::WorldVirtual(true, false, 1.0f), "a standing field handles street lights");
  Check(!lc::WorldVirtual(false, true, 0.0f), "the capture handles decorative light");
  Check(lc::WorldVirtual(false, false, 1.0f), "unlisted decorative lights can bounce beside the field");

  struct Surface {
    const char* name;
    float depth, albedo, lit, ao;
    bool vehicle;
    std::array<float, 4> guides;
  };
  const Surface surfaces[] = {
      {"flat road", 3.0f, 0.12f, 1.0f, 0.9f, false, {3.0f, 3.01f, 3.03f, 3.05f}},
      {"wall", 24.0f, 0.4f, 0.7f, 0.4f, false, {24.0f, 23.94f, 23.8f, 24.1f}},
      {"kerb", 4.0f, 0.3f, 0.8f, 0.2f, false, {4.0f, 3.9f, 9.0f, -4.0f}},
      {"car flank", 2.0f, 0.65f, 0.85f, 0.68f, true, {-1.99f, 2.0f, 3.0f, -2.0f}},
  };
  for (const auto& s : surfaces) {
    constexpr float direct = 0.18f + 0.32f;
    constexpr float bounce = 0.14f;
    float e = 0.0f, sum = 0.0f;
    for (float depth : s.guides) {
      const float w = shader::LcTapWeight(s.depth, depth, s.vehicle, 0.25f);
      Check(w == lc::TapWeight(s.depth, depth, s.vehicle, 0.25f), "shader matches the depth and vehicle rule");
      // Rejected geometry carries deliberately excessive energy.
      const bool same = (depth < 0.0f) == s.vehicle && std::abs(std::abs(depth) - s.depth) <= 0.25f;
      e += (same ? direct : 20.0f) * w;
      sum += w;
    }
    const float old_draws = s.albedo * s.lit * 0.18f + s.albedo * s.lit * 0.32f +
                            s.albedo * s.lit * bounce * s.ao;
    Check(sum > 0.0f && std::abs(lc::Apply(s.albedo, s.lit, e / sum, bounce, s.ao) - old_draws) < 1.0e-6f, s.name);
  }
  Check(shader::LcTapWeight(3.0f, -3.0f, false, 1.0f) == 0.0f, "vehicle and road taps cannot bleed");
  Check(shader::LcTapWeight(3.0f, 4.0f, false, 1.0f) == 0.0f, "depth step rejects");
  Check(shader::LcTapWeight(3.0f, 0.0f, false, 1.0f) == 0.0f, "sky is not a valid guide");
  Check(shader::LcTapWeight(3.0f, 3.0f, false, 0.25f) == 0.25f, "exact depth retains bilinear weight");
  Check(lc::Apply(0.25f, 0.5f, 1.0f, 1.0f, 0.0f) == 0.125f, "AO never darkens direct light");
  Check(lc::Apply(1.0f, 0.0f, 1.0f, 1.0f, 1.0f) == 0.0f, "unlit surfaces receive no light");

  std::printf("light_composite_test: %u of %u passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

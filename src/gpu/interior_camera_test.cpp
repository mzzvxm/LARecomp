#include <cstdio>
#include <initializer_list>

#include "hlsl/cabin_shadow_rules.hlsli"
#include "hlsl/interior_camera_rules.hlsli"

namespace ic = mcla::gpu::interior_camera;
namespace cs = mcla::gpu::cabin_shadow;

int main() {
  int checks = 0;
  int failures = 0;
  const auto check = [&](bool result, const char* context) {
    ++checks;
    if (!result) {
      std::fprintf(stderr, "FAIL: %s\n", context);
      ++failures;
    }
  };

  for (const int rig : {-1, 0, 1, 2, 3}) {
    check(ic::IsCockpit(rig) == (rig == 1), "use the rendered cockpit rig");
    check(ic::ApplyGiShield(false, rig), "default preserves the existing GI shield");
    check(ic::ApplyGiShield(true, rig) == (rig == 1), "GI shield follows the rendered cockpit");
  }

  for (unsigned id = 0; id <= 23; ++id) {
    const unsigned depth = cs::CabinMarkDepth(id != 0 ? 1u : 0u, 9u, id);
    for (unsigned level = 0; level < 3; ++level) {
      check(ic::SsgiSampleLevel(cs::CabinMarkedDepth(depth), level) == (id == 9 ? 0u : level),
            "the player's receiver uses level zero; world and traffic use the requested level");
    }
  }

  constexpr float tolerance = 0.04f;
  for (unsigned guide = 0; guide < 4; ++guide) {
    for (unsigned sample = 0; sample < 4; ++sample) {
      check(ic::RtAoSampleOf(sample, guide, 9u, 9u, 0.01f, tolerance) == sample,
            "keep rotating over the same surface");
      check(ic::RtAoSampleOf(sample, guide, 9u, 9u, tolerance, tolerance) == sample,
            "include the depth tolerance boundary");
      check(ic::RtAoSampleOf(sample, guide, 9u, 9u, 0.15f, tolerance) == guide,
            "a depth discontinuity can be between parts of the same car");
      check(ic::RtAoSampleOf(sample, guide, 0u, 9u, 0.01f, tolerance) == guide,
            "a street tap cannot label its trace as player AO");
      check(ic::RtAoSampleOf(sample, guide, 9u, 0u, 0.01f, tolerance) == guide,
            "a player tap cannot label its trace as street AO");
      check(ic::RtAoSampleOf(sample, guide, 7u, 9u, 0.01f, tolerance) == guide,
            "different cars remain distinct");
    }
  }

  std::printf("%d interior camera checks, %d failures\n", checks, failures);
  return failures != 0;
}

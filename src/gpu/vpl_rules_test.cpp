#include "vpl_rules.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace v = mcla::gpu::vpl;

namespace {
int checks = 0;
int failed = 0;

void Check(bool condition, const char* name) {
  ++checks;
  if (!condition) {
    ++failed;
    std::fprintf(stderr, "FAIL %s\n", name);
  }
}

bool Near(float a, float b) { return std::abs(a - b) < 0.0001f; }

// The depth marcher in a synthetic view: a positive signed depth separates
// a point in front of a receiver from one behind it. An occluding step can
// cross zero without intersecting the actual surface on that step.
template <typename Surface>
float HitDistance(float range, Surface surface) {
  float previous = 0.0f;
  bool in_front = true;
  for (uint32_t i = 1; i <= v::kBeamSteps; ++i) {
    const float now = v::StepDistance(range, i);
    const float depth = surface(now);
    if (in_front && now >= depth) {
      float lo = previous, hi = now;
      for (int k = 0; k < 5; ++k) {
        const float mid = 0.5f * (lo + hi);
        if (mid >= surface(mid)) hi = mid;
        else lo = mid;
      }
      if (hi >= 0.06f && v::WithinDepthThickness(hi, surface(hi))) return hi;
    }
    previous = now;
    in_front = now < depth;
  }
  return -1.0f;
}

void CheckPlacement() {
  Check(v::kMaxLights == 12 && v::kBeamSteps == 28, "fixed record and march budgets");
  Check(Near(v::GroundDistance(0.35f, 1.0f, 4.0f), 0.35f), "neon footprint lies directly below the tube");
  Check(Near(v::GroundDistance(0.60f, 0.05f, 24.0f), 12.0f), "shallow beam meets the car's ground plane");
  Check(Near(v::GroundDistance(0.60f, 0.0f, 24.0f), 24.0f), "horizontal fallback is bounded by reach");
  Check(Near(v::StepDistance(28.0f, 28), 28.0f) && v::StepDistance(28.0f, 7) < 2.0f, "quadratic steps cover the full range");
  Check(std::abs(HitDistance(28.0f, [](float) { return 8.0f; }) - 8.0f) < 0.05f, "beam hits a wall");
  Check(std::abs(HitDistance(12.0f, [](float x) { return 2.0f + 0.5f * x; }) - 4.0f) < 0.05f, "beam meets sloping road");
  Check(std::abs(HitDistance(14.0f, [](float x) { return x < 6.0f ? 4.0f : 9.0f; }) - 4.0f) < 0.05f, "near tunnel wall wins");
  Check(HitDistance(20.0f, [](float x) { return x < 8.0f ? 60000.0f : 2.0f; }) < 0.0f, "off-screen foreground step is not a landing");
  Check(!v::WithinDepthThickness(5.4f, 5.0f) && !v::WithinDepthThickness(4.9f, 5.0f), "depth thickness rejects both sides of a proxy hit");
}

void CheckEnergy() {
  Check(Near(v::Energy(0.9f, 0.0f, 1.8f), 0.0f), "absorbing surface gives no energy");
  Check(Near(v::Energy(0.0f, 1.0f, 1.8f), 0.0f), "switched-off source adds nothing");
  Check(Near(v::Energy(0.8f, 0.5f, 2.0f), 0.4f), "flux is incident times albedo, footprint area and bounce fraction");
  Check(Near(v::Energy(0.8f, 2.0f, 2.0f), 0.8f), "albedo cannot create energy");
  Check(Near(v::Falloff(0.0f, 1.0f), 1.0f) && v::Falloff(1.0f, 1.0f) < 0.5f, "near field is softened");
  Check(Near(v::Falloff(9.0f, 1.0f), 0.0f) && Near(v::Falloff(16.0f, 1.0f), 0.0f), "range falls smoothly to zero and stays there");
  Check(v::ReceiverShare(3, 3, false) == 0.0f && v::ReceiverShare(7, 3, false) == 0.35f &&
        v::ReceiverShare(0, 3, false) == 1.0f && v::ReceiverShare(0, 3, true) == 0.0f, "car and cockpit shares");
}

float AfterHalfSecond(int fps) {
  float left = 1.0f;
  for (int i = 0; i < fps / 2; ++i) left *= 1.0f - v::HistoryWeight(1.0 / fps);
  return left;
}

void CheckHistory() {
  const float low = AfterHalfSecond(30);
  const float mid = AfterHalfSecond(60);
  const float high = AfterHalfSecond(144);
  Check(Near(low, mid) && Near(mid, high), "smoothing is invariant under render rate");
  Check(low < 0.05f && Near(v::HistoryWeight(0.075), 0.63212056f), "a moved light leaves less than five percent after half a second");
  Check(v::HistoryWeight(0.0) == 0.0f, "held scene does not age");
  Check(v::HistoryWeight(0.5) == 1.0f && v::HistoryWeight(-1.0) == 1.0f, "gap or rewind snaps to the new surface");
}
}  // namespace

int main(int argc, char** argv) {
  CheckPlacement();
  CheckEnergy();
  CheckHistory();
  if (argc == 2) {
    std::ifstream in(std::string(argv[1]) + "/hlsl/vpl.hlsl");
    const std::string shader{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    for (const char* text : {
        "kBeamSteps = 28u", "kBounceFraction = 0.25f", "kNearCeiling = 0.35f", "kVehicleShare = 0.35f",
        "range * f * f", "height / max(down, 0.001f)", "beam >= surface",
        "saturate(albedo) * (kBounceFraction * footprint * footprint)", "9.0f * size_sq",
        "distance_sq + size_sq", "receiver != 0u && receiver == source",
        "if (InCabin(v, world, id)) discard", "lerp(flux, previous_flux, old_weight)"}) {
      Check(shader.find(text) != std::string::npos, text);
    }
  }
  std::printf("vpl_rules_test: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}

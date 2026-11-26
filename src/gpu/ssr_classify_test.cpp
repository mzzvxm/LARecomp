#include <cstdint>
#include <cstdio>

#include "ssr_rt_rules.h"

namespace {

using uint = uint32_t;
#include "hlsl/ssr_classify_rules.hlsli"

struct State {
  float hits = 0.0f;
  float misses = 0.0f;
  uint age = 0;
  uint mode = kSsrTileMixed;

  void Add(float hit, float miss) {
    hits = SsrTileFeedback(hit, hits, age);
    misses = SsrTileFeedback(miss, misses, age);
    mode = SsrTileMode(mode, SsrTileMiss(hits, misses));
    ++age;
  }
};

int checks = 0;
int failures = 0;

void Check(bool valid, const char* name) {
  ++checks;
  if (valid) return;
  ++failures;
  std::fprintf(stderr, "FAIL %s\n", name);
}

void TestFeedback() {
  Check(SsrTileMiss(0, 0) == 0.5f, "unknown confidence");
  Check(SsrTileMiss(48, 16) == 0.25f, "miss ratio");
  Check(SsrTileFeedback(64, 1, 0) == 64, "first frame uses fresh evidence");
  Check(SsrTileFeedback(64, 0, 1) == 16, "history weight");
  for (uint age = 0; age <= 15; ++age) {
    for (uint mode = 0; mode <= 2; ++mode) {
      Check(SsrTileAge(SsrTileState(age, mode)) == age &&
          SsrTileClass(SsrTileState(age, mode)) == mode, "exact state round trip");
    }
  }
  Check(SsrTileMode(kSsrTileScreen, 0.20f) == kSsrTileScreen, "screen safe band inclusive");
  Check(SsrTileMode(kSsrTileMixed, 0.20f) == kSsrTileMixed, "screen entry needs stronger evidence");
  Check(SsrTileMode(kSsrTileScreen, 0.21f) == kSsrTileMixed, "screen exit");
  Check(SsrTileMode(kSsrTileWorld, 0.60f) == kSsrTileWorld, "world safe band inclusive");
  Check(SsrTileMode(kSsrTileMixed, 0.60f) == kSsrTileMixed, "world entry needs stronger evidence");
  Check(SsrTileMode(kSsrTileWorld, 0.59f) == kSsrTileMixed, "world exit");

  State state;
  State repeat;
  for (uint n = 0; n < 96; ++n) {
    const float miss = n < 32 ? 0.05f : n < 64 ? 0.95f : (n & 1u) ? 0.64f : 0.71f;
    state.Add(64 * (1 - miss), 64 * miss);
    repeat.Add(64 * (1 - miss), 64 * miss);
    Check(state.mode == repeat.mode && state.hits == repeat.hits && state.misses == repeat.misses,
          "deterministic feedback");
    if (n == 31) Check(state.mode == kSsrTileScreen, "converges to screen");
    if (n >= 63) Check(state.mode == kSsrTileWorld, "world stays stable across alternating confidence");
  }
  for (int n = 0; n < 24; ++n) state.Add(62, 2);
  Check(state.mode == kSsrTileScreen, "converges again after confidence recovers");
}

void TestWork() {
  Check(SsrTileRays(2, 3, 0.1f, 0.0f, 0.72f, true, false) == 2, "young extra ray");
  Check(SsrTileRays(2, 4, 0.1f, 0.024f, 0.72f, true, false) == 1, "quiet tile keeps one ray");
  Check(SsrTileRays(2, 4, 0.1f, 0.025f, 0.72f, true, false) == 2, "noisy extra ray");
  Check(SsrTileRays(2, 8, 1.0f, 0.0f, 0.92f, true, false) == 2, "mirror keeps full rate");
  Check(SsrTileRays(2, 7, 1.0f, 0.0f, 0.65f, true, false) == 1, "sparse needs stable age");
  Check(SsrTileRays(2, 8, 0.97f, 0.0f, 0.65f, true, false) == 1, "sparse needs repeated misses");
  Check(SsrTileRays(2, 8, 1.0f, 0.005f, 0.65f, true, false) == 1, "sparse needs low variance");
  Check(SsrTileRays(2, 8, 1.0f, 0.0f, 0.66f, true, false) == 1, "sparse preserves smooth surfaces");
  Check(SsrTileRays(2, 8, 1.0f, 0.0f, 0.65f, true, false) == 0, "sparse skips coherent misses");
  Check(SsrTileRays(2, 8, 1.0f, 0.0f, 0.65f, false, false) == 1, "raw and RT retain a screen attempt");
  Check(SsrTileRays(2, 8, 1.0f, 0.0f, 0.65f, true, true) == 1, "probe can recover a hit");
  for (uint phase = 0; phase < 4; ++phase) {
    Check(SsrTileProbe(4 - phase, phase), "probe phase");
    uint probes = 0;
    for (uint frame = 120; frame < 148; ++frame) probes += SsrTileProbe(frame, phase);
    Check(probes == 7, "one probe every four frames");
  }

  Check(SsrTileIterations(31, 0.88f) == 31, "mirror traversal unchanged");
  Check(SsrTileIterations(31, 0.68f) == 24, "semi-gloss traverses three quarters, rounded up");
  Check(SsrTileIterations(31, 0.67f) == 16, "gloss traverses one half, rounded up");
  Check(SsrTileKeepRt(kSsrTileWorld, 8, 0.0f, 0.70f, false), "world tiles trace");
  Check(SsrTileKeepRt(kSsrTileMixed, 3, 0.0f, 0.70f, false), "disoccluded tiles trace");
  Check(SsrTileKeepRt(kSsrTileMixed, 8, 0.025f, 0.70f, false), "noisy tiles trace");
  Check(SsrTileKeepRt(kSsrTileMixed, 8, 0.0f, 0.44f, false), "deep screen hole traces");
  Check(!SsrTileKeepRt(kSsrTileMixed, 8, 0.0f, 0.45f, false), "stable marginal hole is sampled");
  Check(SsrTileKeepRt(kSsrTileMixed, 8, 0.0f, 0.45f, true), "marginal hole respawns");
  Check(SsrTileRtProbeShare(kSsrTileScreen) == 0.25f && SsrTileRtProbeShare(kSsrTileMixed) == 0.5f,
        "stable screen and mixed probe rates");
}

void TestFixedCapacity() {
  using namespace mcla::gpu::ssr_rt;
  Check(ResolveFixed(Level::kHigh, Choice::kHigh, false).why == Why::kNoRt, "physical support required");
  Check(ResolveFixed(Level::kOff, Choice::kUltra, true).why == Why::kSsrOff, "SSR off");
  Check(ResolveFixed(Level::kHigh, Choice::kOff, true).why == Why::kSetting, "RT off");
  const Cell high = ResolveFixed(Level::kUltra, Choice::kHigh, true).cell;
  const Cell ultra = ResolveFixed(Level::kUltra, Choice::kAuto, true).cell;
  Check(CapacityFor(high, 1) == 250000 && high.reach == 150.0f &&
        high.group_mask == (GroupBit(kGroupBright) | GroupBit(kGroupGround)), "fixed High cell");
  Check(CapacityFor(ultra, 1) == 1000000 && ultra.reach == 300.0f, "fixed Ultra cell");
  Check(CapacityFor(ultra, 2) == 500000, "two-ray allocation counts rays");
}

}  // namespace

int main() {
  TestFeedback();
  TestWork();
  TestFixedCapacity();
  std::printf("SSR classification: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}

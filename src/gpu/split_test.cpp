// Check of the before/after split's pure half (split.h): which positions are a split, how many columns carry the effect, the two
// spans of a draw, the effect list. A program of its own: it needs no renderer, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_split_test      (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_split_test
//
// The compile by hand (the msvc-target clang of labuild):
//   clang++ -std=c++23 -Wall -Wextra -I src/gpu src/gpu/split_test.cpp -o split_test.exe

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include "split.h"

using namespace mcla::gpu::split;

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

void CheckU(uint32_t got, uint32_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %u, want %u\n", what.c_str(), got, want);
  }
}

void TestActive() {
  Check(!Active(kOff), "-1 (the default) is no split");
  Check(!Active(-0.0001), "a value below 0 is no split");
  Check(!Active(-1e9), "far below 0 is no split");
  Check(!Active(std::numeric_limits<double>::quiet_NaN()), "NaN is no split");
  Check(!Active(std::numeric_limits<double>::infinity()), "infinity is no split");
  Check(Active(0.0), "0 is a split: the effect nowhere");
  Check(Active(0.5), "0.5 is a split");
  Check(Active(0.999999), "just short of 1 is a split");
  Check(!Active(1.0), "1 is the effect everywhere: no work");
  Check(!Active(2.0), "above 1 is the effect everywhere");
}

void TestColumns() {
  CheckU(EffectColumns(0.5, 1920), 960, "half of 1920");
  CheckU(EffectColumns(0.25, 1920), 480, "a quarter of 1920");
  CheckU(EffectColumns(0.75, 1280), 960, "three quarters of 1280");
  CheckU(EffectColumns(1.0 / 3.0, 1920), 640, "a third of 1920");
  CheckU(EffectColumns(0.0, 1920), 0, "0 is no column");
  CheckU(EffectColumns(0.0001, 1920), 0, "a share of under half a column rounds down to none");
  CheckU(EffectColumns(0.0003, 1920), 1, "a share of over half a column rounds up to one");
  CheckU(EffectColumns(0.9999, 1920), 1920, "just short of 1 rounds up to every column");
  CheckU(EffectColumns(0.5, 1281), 641, "an odd width rounds half up");
  CheckU(EffectColumns(0.5, 1), 1, "one column at half rounds up");
  CheckU(EffectColumns(0.4, 1), 0, "one column at 0.4 rounds down");
  CheckU(EffectColumns(0.5, 0), 0, "no width, no column");
  // Off: the whole width (so a caller that forgot to test Active still draws everything).
  CheckU(EffectColumns(kOff, 1920), 1920, "off is the whole width");
  CheckU(EffectColumns(1.0, 1920), 1920, "1 is the whole width");
  CheckU(EffectColumns(std::numeric_limits<double>::quiet_NaN(), 640), 640, "NaN is the whole width");
}

void TestSpans() {
  // The two parts always add up to the width and the effect part never grows past it: every width 1..96, every position in steps of 1/96.
  bool sums = true;
  bool bounded = true;
  bool monotonic = true;
  for (uint32_t width = 1; width <= 96; ++width) {
    uint32_t last = 0;
    for (int i = 0; i <= 96; ++i) {
      const double position = std::min(double(i) / 96.0, 0.999999);
      const Spans spans = SpansOf(position, width);
      sums = sums && (spans.effect + spans.plain == width);
      bounded = bounded && spans.effect <= width;
      monotonic = monotonic && spans.effect >= last;
      last = spans.effect;
    }
  }
  Check(sums, "effect + plain is the width for every width and position");
  Check(bounded, "the effect part is never wider than the picture");
  Check(monotonic, "a split that moves right never takes columns back");
  const Spans off = SpansOf(kOff, 1920);
  CheckU(off.effect, 1920, "off: every column carries the effect");
  CheckU(off.plain, 0, "off: no plain column");
  const Spans none = SpansOf(0.0, 1920);
  CheckU(none.effect, 0, "0: no column carries the effect");
  CheckU(none.plain, 1920, "0: every column is plain");
}

// A moving split steps by at most one column for each column's worth of position: no jump in a wipe.
void TestSmooth() {
  const uint32_t width = 1920;
  uint32_t last = EffectColumns(0.0, width);
  uint32_t worst = 0;
  for (int i = 1; i <= 1920; ++i) {
    const uint32_t now = EffectColumns(double(i) / 1920.0 * 0.9999, width);
    worst = std::max(worst, now - last);
    last = now;
  }
  CheckU(worst, 1, "stepping the position by one column's share moves the edge by one column at most");
  CheckU(last, 1920, "and reaches the right edge");
}

// The occlusion is cut at the render target's columns and the dither at the letterboxed picture's: the same position puts the edge
// at the same share of the picture within half a column of each, so at the picture's scale the two agree to a pixel.
void TestAgreement() {
  bool agree = true;
  const uint32_t widths[][2] = {{1280, 1920}, {1920, 3840}, {2560, 3840}, {1706, 2560}, {640, 1920}};
  for (const auto& w : widths) {
    for (int i = 0; i <= 200; ++i) {
      const double position = std::min(double(i) / 200.0, 0.999999);
      const double a = double(EffectColumns(position, w[0])) / double(w[0]);
      const double b = double(EffectColumns(position, w[1])) / double(w[1]);
      const double slack = 0.5 / double(w[0]) + 0.5 / double(w[1]);
      agree = agree && std::fabs(a - b) <= slack + 1e-12;
    }
  }
  Check(agree, "the render target's edge and the picture's edge differ by at most half a column of each");
}

void TestEffects() {
  CheckU(EffectsOf("all"), kEffectsAll, "all");
  CheckU(EffectsOf("ao"), kEffectAo, "ao");
  CheckU(EffectsOf("dither"), kEffectDither, "dither");
  CheckU(EffectsOf(""), kEffectsAll, "an empty value counts as all");
  CheckU(EffectsOf("shadows"), kEffectsAll, "an unknown name counts as all (the cvar's allowed list keeps it out)");
  Check((EffectsOf("ao") & kEffectDither) == 0, "ao leaves the dither whole");
  Check((EffectsOf("dither") & kEffectAo) == 0, "dither leaves the occlusion whole");
  Check((EffectsOf("all") & kEffectAo) != 0 && (EffectsOf("all") & kEffectDither) != 0, "all cuts both");
}

// The dither's two draws, as present.cpp makes them: left part with the code size, right part with 0. Every column is drawn exactly
// once and the dithered ones are exactly the effect's columns.
void TestDrawParts() {
  for (const uint32_t width : {1u, 2u, 7u, 64u, 1920u}) {
    for (const double position : {0.0, 0.1, 0.5, 0.9, 0.999}) {
      const Spans spans = SpansOf(position, width);
      uint32_t drawn = 0, dithered = 0;
      for (uint32_t x = 0; x < width; ++x) {
        const bool left = x < spans.effect;           // the first draw's scissor
        const bool right = x >= spans.effect;         // the second draw's
        drawn += (left ? 1u : 0u) + (right ? 1u : 0u);
        dithered += left ? 1u : 0u;
      }
      CheckU(drawn, width, "every column of " + std::to_string(width) + " is drawn once at " + std::to_string(position));
      CheckU(dithered, spans.effect, "the dithered columns are the effect's at " + std::to_string(position));
    }
  }
}

}  // namespace

int main() {
  TestActive();
  TestColumns();
  TestSpans();
  TestSmooth();
  TestAgreement();
  TestEffects();
  TestDrawParts();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

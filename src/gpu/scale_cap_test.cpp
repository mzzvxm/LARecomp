// Check of the render scale cap's arithmetic (scale_cap.h): what a scale needs, what a card allows, the
// largest scale that fits, and which choices are lowered. A program of its own: no renderer, no ReXGlue,
// no GPU; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_scale_cap_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_scale_cap_test
//
// The expected numbers are written out here again on purpose, from the model of scale_cap.h (450 MiB +
// 166 MiB per square of the scale, a quarter of the budget kept free), so that a constant changed there alone
// fails the check.

#include <cstdint>
#include <cstdio>
#include <string>

#include "scale_cap.h"

using namespace mcla::gpu;
using namespace mcla::gpu::scale_cap;

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

void CheckNum(uint64_t got, uint64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), (unsigned long long)got,
                (unsigned long long)want);
  }
}

constexpr uint64_t MiB(uint64_t n) { return n * kMiB; }

// A window the way the renderer measures it (1280x720 is 1x): the screen's scale, the default.
RenderScaleChoice Screen(uint32_t width, uint32_t height) {
  return ChooseRenderScale(0.0, false, FsrPreset::kCustom, width, height);
}

Memory Card(uint64_t budget_mib, uint64_t dedicated_mib = 0) {
  Memory m;
  m.budget = MiB(budget_mib);
  m.dedicated = MiB(dedicated_mib);
  return m;
}

void CheckNeed() {
  // 450 + 166 s^2, in whole MiB (the table of the header's comment).
  CheckNum(NeedBytes(Scale::Whole(1)) / kMiB, 616, "need at 1x");
  CheckNum(NeedBytes(Scale{30}) / kMiB, 823, "need at 1.5x (1080p)");
  CheckNum(NeedBytes(Scale::Whole(2)) / kMiB, 1114, "need at 2x (1440p)");
  CheckNum(NeedBytes(Scale::Whole(3)) / kMiB, 1944, "need at 3x (4K)");
  // The audit's estimates at the default (0.95 / 1.2 / 1.9 GiB) are within 20% of the model.
  Check(NeedBytes(Scale::Whole(3)) > MiB(1800) && NeedBytes(Scale::Whole(3)) < MiB(2050),
        "4K is about 1.9 GiB");
  // Monotonic in the scale, over the whole grid.
  bool up = true;
  for (uint32_t n = kMinRenderScale.n; n < kMaxRenderScale.n; ++n) {
    up = up && NeedBytes(Scale{n}) < NeedBytes(Scale{n + 1});
  }
  Check(up, "need grows with every step of the grid");
}

void CheckBudget() {
  CheckNum(BudgetOf(Card(3000)), MiB(3000), "a reported budget is the budget");
  CheckNum(BudgetOf(Card(0, 2048)), MiB(2048) * 85 / 100, "no budget: 85% of the dedicated memory");
  CheckNum(BudgetOf(Card(3000, 4096)), MiB(3000), "a reported budget wins over the size");
  CheckNum(BudgetOf(Card(0, 0)), 0, "neither: nothing known");
  CheckNum(PlanBytes(Card(2000)), MiB(1500), "a quarter of the budget is left free");
}

void CheckLargest() {
  // The step the 4K window (3x) comes down to on the cards of the table in the header:
  //   budget (MiB)  plan   largest   why
  //   16000         12000  3x        fits
  //   3380 (4 GB)   2535   3x        fits (1944)
  //   2560 (3 GB)   1920   2.95x     3x needs 1944, 2.95x needs 1894
  //   1740 (2 GB)   1305   2.25x     2.25x needs 1290, 2.3x needs 1328
  //   870  (1 GB)   652    1.1x      1.1x needs 650, 1.15x needs 669
  //   430           322    1x        the floor: nothing fits
  const struct {
    uint64_t budget_mib;
    uint32_t want_n;
  } cases[] = {{16000, 60}, {3380, 60}, {2560, 59}, {1740, 45}, {870, 22}, {430, 20}};
  for (const auto& c : cases) {
    const Scale got = LargestFitting(Scale::Whole(3), PlanBytes(Card(c.budget_mib)));
    CheckNum(got.n, c.want_n, "largest scale on a " + std::to_string(c.budget_mib) + " MiB budget");
  }
  // Never above what was wanted, never below the floor, whatever the plan.
  for (const uint64_t plan_mib : {0ull, 100ull, 600ull, 1000ull, 1500ull, 3000ull, 100000ull}) {
    for (uint32_t n = 10; n <= 60; n += 5) {
      const Scale got = LargestFitting(Scale{n}, MiB(plan_mib));
      Check(got.n <= n, "never above the wanted scale");
      Check(got.n >= std::min(n, kFloor.n), "never below the game's own size, or the wanted scale");
      Check(got.n == n || NeedBytes(got) <= MiB(plan_mib) || got.n == kFloor.n,
            "what is returned fits or is the floor");
      Check(got.n == n || NeedBytes(Scale{got.n + 1}) > MiB(plan_mib),
            "and is the largest step that does");
    }
  }
  // A window below 720p keeps its own scale even on a card with no room at all.
  CheckNum(LargestFitting(Scale{15}, 0).n, 15, "0.75x stays 0.75x");
  CheckNum(LargestFitting(Scale{10}, 0).n, 10, "0.5x stays 0.5x");
}

void CheckApply() {
  // The default scale in a 4K window (3840x2160 is 3x) on a 2 GB card: lowered to 2.25x, with the scale
  // the window asked for kept for the menu.
  {
    RenderScaleChoice c = Screen(3840, 2160);
    Check(c.scale == Scale::Whole(3) && c.source == RenderScaleChoice::Source::kScreen, "4K asks 3x");
    const Report r = Apply(&c, Card(1740));
    Check(r.outcome == Outcome::kCapped, "4K on a 2 GB card is capped");
    CheckNum(c.scale.n, 45, "to 2.25x");
    Check(c.capped && c.uncapped == Scale::Whole(3), "the scale the window asked for is kept");
    Check(!c.rounded(), "a capped scale is not 'rounded'");
    CheckNum(r.wanted.n, 60, "the report has the scale before");
    CheckNum(r.scale.n, 45, "and after");
    Check(r.need_scale <= r.plan && r.need_wanted > r.plan, "the capped scale fits, the wanted did not");
    const std::string line = Line(c, r);
    Check(line.find("lowered from 3x to 2.25x") != std::string::npos, "the line says what it did: " + line);
    Check(line.find("3840x2160") != std::string::npos, "the line names the window");
    Check(line.find("1740 MiB") != std::string::npos, "the line names the budget");
    Check(line.find('\n') == std::string::npos, "the line is one line");
  }
  // 1080p (1.5x) on a 2 GB card, 1440p (2x) on a 4 GB card, 4K on a 4 GB card: all fit.
  {
    RenderScaleChoice c = Screen(1920, 1080);
    Check(Apply(&c, Card(1740)).outcome == Outcome::kFits && c.scale.n == 30 && !c.capped,
          "1080p fits a 2 GB card");
    RenderScaleChoice d = Screen(2560, 1440);
    Check(Apply(&d, Card(3380)).outcome == Outcome::kFits && d.scale.n == 40, "1440p fits a 4 GB card");
    RenderScaleChoice e = Screen(3840, 2160);
    Check(Apply(&e, Card(3380)).outcome == Outcome::kFits && e.scale.n == 60, "4K fits a 4 GB card");
    RenderScaleChoice f = Screen(3840, 2160);
    Check(Apply(&f, Card(16303)).outcome == Outcome::kFits && f.scale.n == 60, "4K fits a 16 GB card");
  }
  // A card with almost no room: the game's own size, and the line says that does not fit either.
  {
    RenderScaleChoice c = Screen(3840, 2160);
    const Report r = Apply(&c, Card(430));
    Check(r.outcome == Outcome::kCapped && c.scale == kFloor, "a 430 MiB budget: 1x, never below");
    Check(Line(c, r).find("still needs") != std::string::npos, "and says that 1x does not fit either");
  }
  // A window below 720p: nothing to lower.
  {
    RenderScaleChoice c = Screen(960, 540);
    const Report r = Apply(&c, Card(300));
    Check(r.outcome == Outcome::kFloor && c.scale.n == 15 && !c.capped, "a 960x540 window keeps 0.75x");
  }
  // A card that reports no budget: 85% of its size stands in for it.
  {
    RenderScaleChoice c = Screen(3840, 2160);
    const Report r = Apply(&c, Card(0, 2048));
    Check(r.outcome == Outcome::kCapped && r.assumed_budget, "a 2 GB card with no budget is capped");
    CheckNum(c.scale.n, 45, "to 2.25x as the reported 1740 MiB gives");
    Check(Line(c, r).find("reports no budget") != std::string::npos, "and the line says the budget is assumed");
  }
  // No verdict: shared memory, nothing known, an upscaler, no scale. Nothing is changed.
  {
    Memory shared = Card(15000);
    shared.shared = true;
    RenderScaleChoice a = Screen(3840, 2160);
    const Report ra = Apply(&a, shared);
    Check(ra.outcome == Outcome::kNotChecked && a.scale.n == 60 && !a.capped, "shared memory: not checked");
    Check(Line(a, ra).find("not checked") != std::string::npos, "and the line says so");
    RenderScaleChoice b = Screen(3840, 2160);
    Check(Apply(&b, Card(0, 0)).outcome == Outcome::kNotChecked && b.scale.n == 60, "nothing known: not checked");
    RenderScaleChoice c = ChooseUpscalerOutputScale(0.0, false, FsrPreset::kCustom, 3840, 2160);
    Check(Apply(&c, Card(1000)).outcome == Outcome::kNotChecked && c.scale.n == 60 && !c.capped,
          "an upscaler's output is not checked");
    RenderScaleChoice d = ChooseRenderScale(-1.0, false, FsrPreset::kCustom, 3840, 2160);
    Check(Apply(&d, Card(1000)).outcome == Outcome::kNotChecked && !d.capped, "an invalid scale is not checked");
  }
  // The player's own choices stay: a scale asked for, and an FSR1 preset (its scale follows the window).
  {
    RenderScaleChoice c = ChooseRenderScale(3.0, false, FsrPreset::kCustom, 1920, 1080);
    const Report r = Apply(&c, Card(1740));
    Check(r.outcome == Outcome::kKept && c.scale.n == 60 && !c.capped, "a chosen 3x is kept");
    const std::string line = Line(c, r);
    Check(line.find("mcla_gpu_resolution_scale = 3") != std::string::npos, "the warning names the setting: " + line);
    Check(line.find("2.25x") != std::string::npos, "and the scale that fits");
    RenderScaleChoice p = ChooseRenderScale(0.0, true, FsrPreset::kQuality, 3840, 2160);  // 3 / 1.5 = 2x
    Check(p.scale.n == 40, "FSR1 quality in 4K renders at 2x");
    const Report rp = Apply(&p, Card(1000));  // plan 750: 2x needs 1114
    Check(rp.outcome == Outcome::kKept && p.scale.n == 40 && !p.capped, "an FSR1 preset's scale is kept");
    Check(Line(p, rp).find("FSR1 preset quality") != std::string::npos, "and named");
    RenderScaleChoice f = ChooseRenderScale(1.0, false, FsrPreset::kCustom, 3840, 2160);
    Check(Apply(&f, Card(1000)).outcome == Outcome::kFits, "a chosen 1x fits 1000 MiB (plan 750, need 616)");
  }
  // The window changes: the same card, a new answer each time (the cache of render_scale.cpp is keyed by
  // the window's size and calls this again).
  {
    const Memory card = Card(1740);
    const uint32_t sizes[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {1920, 1080}};
    const uint32_t want_n[] = {20, 30, 40, 45, 30};
    for (int i = 0; i < 5; ++i) {
      RenderScaleChoice c = Screen(sizes[i][0], sizes[i][1]);
      Apply(&c, card);
      CheckNum(c.scale.n, want_n[i], "window " + std::to_string(sizes[i][0]) + "x" + std::to_string(sizes[i][1]));
    }
  }
}

}  // namespace

int main() {
  CheckNeed();
  CheckBudget();
  CheckLargest();
  CheckApply();
  if (g_failed != 0) {
    std::printf("scale_cap_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("scale_cap_test: %d checks passed\n", g_checks);
  return 0;
}

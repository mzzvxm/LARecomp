// Check of the render thread's CPU stopwatch accounts (cpu_timing_accounts.h, mcla_gpu_cpu_timing).
// A program of its own: it needs no renderer, no ReXGlue and no GPU, takes its clock as numbers
// written out here, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_cpu_timing_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_cpu_timing_test

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "cpu_timing_accounts.h"

using namespace mcla::gpu::cpu_timing;

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

void CheckEq(uint64_t got, uint64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), (unsigned long long)got,
                (unsigned long long)want);
  }
}

uint64_t SumSelf(const Accounts::Totals& t) {
  uint64_t sum = 0;
  for (const uint64_t v : t.self) {
    sum += v;
  }
  return sum;
}

bool Has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

constexpr Section kA = Section::kEncode;
constexpr Section kB = Section::kTouchPages;
constexpr Section kC = Section::kUploadPages;

void TestOneSection() {
  Accounts a;
  a.Enter(kA, 100);
  a.Exit(250);
  const Accounts::Totals t = a.Take(250);
  CheckEq(t.self[size_t(kA)], 150, "one section: own time");
  CheckEq(t.inclusive[size_t(kA)], 150, "one section: inclusive time");
  CheckEq(t.calls[size_t(kA)], 1, "one section: calls");
  CheckEq(t.longest[size_t(kA)], 150, "one section: longest");
  CheckEq(SumSelf(t), 150, "one section: nothing else timed");
}

void TestNesting() {
  // A runs 0..100; B inside it 20..50 and again 60..70.
  Accounts a;
  a.Enter(kA, 0);
  a.Enter(kB, 20);
  a.Exit(50);
  a.Enter(kB, 60);
  a.Exit(70);
  a.Exit(100);
  const Accounts::Totals t = a.Take(100);
  CheckEq(t.self[size_t(kB)], 40, "nesting: the inner section's own time");
  CheckEq(t.self[size_t(kA)], 60, "nesting: the outer section is not billed for the inner one");
  CheckEq(t.inclusive[size_t(kA)], 100, "nesting: the outer section's inclusive time");
  CheckEq(t.inclusive[size_t(kB)], 40, "nesting: the inner section's inclusive time");
  CheckEq(t.calls[size_t(kB)], 2, "nesting: the inner section ran twice");
  CheckEq(t.longest[size_t(kB)], 30, "nesting: the inner section's longest run");
  CheckEq(SumSelf(t), t.inclusive[size_t(kA)], "nesting: own times add up to the outermost run");
}

void TestDeepNesting() {
  // A 0..100, B 10..90, C 20..30: each own time is its span less what is inside it.
  Accounts a;
  a.Enter(kA, 0);
  a.Enter(kB, 10);
  a.Enter(kC, 20);
  a.Exit(30);
  a.Exit(90);
  a.Exit(100);
  const Accounts::Totals t = a.Take(100);
  CheckEq(t.self[size_t(kC)], 10, "three deep: innermost");
  CheckEq(t.self[size_t(kB)], 70, "three deep: middle");
  CheckEq(t.self[size_t(kA)], 20, "three deep: outermost");
  CheckEq(t.inclusive[size_t(kB)], 80, "three deep: middle inclusive");
  CheckEq(SumSelf(t), 100, "three deep: the own times add up to the whole");
}

void TestSiblingsAndGaps() {
  // Time between two sections belongs to none of them.
  Accounts a;
  a.Enter(kA, 0);
  a.Exit(10);
  a.Enter(kB, 40);
  a.Exit(45);
  const Accounts::Totals t = a.Take(50);
  CheckEq(t.self[size_t(kA)], 10, "siblings: first");
  CheckEq(t.self[size_t(kB)], 5, "siblings: second");
  CheckEq(SumSelf(t), 15, "siblings: the gap is not timed");
}

void TestRecursion() {
  // A inside A: two calls, the inclusive time of the outer run once, the own times add up.
  Accounts a;
  a.Enter(kA, 0);
  a.Enter(kA, 10);
  a.Exit(30);
  a.Exit(50);
  const Accounts::Totals t = a.Take(50);
  CheckEq(t.calls[size_t(kA)], 2, "recursion: both calls counted");
  CheckEq(t.inclusive[size_t(kA)], 50, "recursion: inclusive counted once, from the outer start");
  CheckEq(t.self[size_t(kA)], 50, "recursion: own times add up to the outer span");
  CheckEq(t.longest[size_t(kA)], 50, "recursion: longest is the outer run");
}

void TestWindows() {
  // A section open across a Take is billed up to the Take, and the rest goes to the next window.
  Accounts a;
  a.Enter(kA, 0);
  const Accounts::Totals first = a.Take(100);
  CheckEq(first.self[size_t(kA)], 100, "windows: the running section is billed up to the Take");
  CheckEq(first.calls[size_t(kA)], 0, "windows: it has not ended, so no call yet");
  a.Exit(150);
  const Accounts::Totals second = a.Take(150);
  CheckEq(second.self[size_t(kA)], 50, "windows: the rest is in the next window");
  CheckEq(second.calls[size_t(kA)], 1, "windows: the call ends in the next window");
  CheckEq(first.self[size_t(kA)] + second.self[size_t(kA)], 150, "windows: the windows add up");
  // A Take with nothing open leaves nothing behind.
  const Accounts::Totals third = a.Take(200);
  CheckEq(SumSelf(third), 0, "windows: a Take resets the totals");
  // Without a clock: the running section's time since it last resumed goes to the next window.
  a.Enter(kB, 300);
  const Accounts::Totals unclocked = a.Take();
  CheckEq(SumSelf(unclocked), 0, "windows: Take() without a clock bills nothing");
  a.Exit(320);
  CheckEq(a.Take(320).self[size_t(kB)], 20, "windows: and it is all in the next window");
}

void TestTooDeep() {
  Accounts a;
  const size_t n = Accounts::kMaxDepth + 4;
  for (size_t i = 0; i < n; ++i) {
    a.Enter(kA, i);
  }
  CheckEq(a.depth(), n, "too deep: depth counts the sections not timed");
  for (size_t i = 0; i < n; ++i) {
    a.Exit(100 + i);
  }
  CheckEq(a.depth(), 0, "too deep: all ended");
  const Accounts::Totals t = a.Take(200);
  CheckEq(t.too_deep, 4, "too deep: four sections not timed");
  CheckEq(t.calls[size_t(kA)], Accounts::kMaxDepth, "too deep: only the timed ones are calls");
  CheckEq(t.unmatched, 0, "too deep: every end found its start");
  // And the accounts work as before afterwards.
  a.Enter(kB, 0);
  a.Exit(7);
  CheckEq(a.Take(7).self[size_t(kB)], 7, "too deep: usable again");
}

void TestUnmatched() {
  Accounts a;
  a.Exit(10);
  const Accounts::Totals t = a.Take(10);
  CheckEq(t.unmatched, 1, "unmatched: an end with no start is counted");
  CheckEq(SumSelf(t), 0, "unmatched: and times nothing");
}

void TestBackwardClock() {
  // Counters that disagree between cores must not wrap an unsigned subtraction.
  Accounts a;
  a.Enter(kA, 100);
  a.Exit(90);
  const Accounts::Totals t = a.Take(90);
  CheckEq(t.self[size_t(kA)], 0, "backward clock: no wrap in own time");
  CheckEq(t.inclusive[size_t(kA)], 0, "backward clock: no wrap in inclusive time");
}

void TestNames() {
  for (size_t i = 0; i < kSectionCount; ++i) {
    const std::string name = kSections[i].name;
    Check(!name.empty(), "name of section " + std::to_string(i) + " is not empty");
    Check(name.find('|') == std::string::npos && name.find(',') == std::string::npos,
          "name '" + name + "' has no '|' or ','");
    for (size_t j = i + 1; j < kSectionCount; ++j) {
      Check(name != kSections[j].name, "name '" + name + "' is unique");
    }
  }
  CheckEq(kSectionCount, size_t(Section::kCount), "one name per section");
}

void TestFrameWindow() {
  FrameWindow window;
  const size_t encode = size_t(Section::kEncode);
  const size_t resolve = size_t(Section::kResolveCall);
  CheckEq(window.P95()[encode], 0, "empty percentile");
  for (size_t i = 0; i < kWindowFrames; ++i) {
    Accounts::Totals frame;
    // Shuffled ranks 1..600 and a sparse section, active on 5 percent of frames exactly.
    frame.self[encode] = (i * 7) % kWindowFrames + 1;
    frame.inclusive[encode] = frame.self[encode] * 2;
    frame.longest[encode] = frame.inclusive[encode];
    frame.calls[encode] = 3;
    if (i < 30) {
      frame.self[resolve] = 1000;
    }
    frame.unmatched = i == 5;
    frame.too_deep = i == 8;
    window.Add(frame);
  }
  const auto p95 = window.P95();
  CheckEq(p95[encode], 570, "p95 uses nearest rank for per-frame totals");
  CheckEq(p95[resolve], 0, "p95 counts frames in which a section did not run");
  CheckEq(window.count(), kWindowFrames, "complete window count");
  CheckEq(window.totals().self[encode], 180300, "sum of per-frame self times");
  CheckEq(window.totals().inclusive[encode], 360600, "sum of per-frame inclusive times");
  CheckEq(window.totals().calls[encode], 1800, "calls across frames");
  CheckEq(window.totals().longest[encode], 1200, "longest single call across frames");
  CheckEq(window.totals().unmatched, 1, "unmatched calls survive aggregation");
  CheckEq(window.totals().too_deep, 1, "nesting overflow survives aggregation");
  Check(Has(Summary(window.totals(), kWindowFrames, 1000, 0, 0, p95), "p95 0.5700 ms/frame"),
        "log reports the frame percentile in milliseconds");
  window.Add({});
  CheckEq(window.count(), kWindowFrames, "upper bound on stored frames");
  window.Reset();
  CheckEq(window.count(), 0, "reset opens another window");
  CheckEq(window.totals().self[encode], 0, "reset clears totals");
  Accounts::Totals next;
  next.self[encode] = 11;
  window.Add(next);
  CheckEq(window.P95()[encode], 11, "new short window does not read past initialized samples");
}

void TestSummary() {
  // 600 frames at 1 ns a tick: encode 6,000,000 own (+ 3,000,000 in touch pages inside it), holds
  // 1,200,000; the thread was busy 120,000,000 ticks (200 us a frame).
  Accounts a;
  a.Enter(Section::kEncode, 0);
  a.Enter(Section::kTouchPages, 1'000'000);
  a.Exit(4'000'000);
  a.Exit(9'000'000);
  a.Enter(Section::kHolds, 10'000'000);
  a.Exit(11'200'000);
  const Accounts::Totals t = a.Take(12'000'000);
  const std::string text = Summary(t, 600.0, 1.0, 120'000'000, 40.0);
  Check(Has(text, "cpu timing over 600 frames"), "summary: names the frames");
  Check(Has(text, "render thread busy 120.00 ms (0.200/frame)"), "summary: the busy time");
  Check(Has(text, "timed 10.20 ms (0.017/frame, 8.5% of busy)"), "summary: the timed total and its share");
  Check(Has(text, "not timed 109.80 ms (0.183/frame)"), "summary: the rest of the busy time");
  Check(Has(text, "encode self 6.00 ms (0.0100/frame, 5.0%) incl 9.00 ms (0.0150/frame) 1 calls (0.0/frame) "
                  "longest 9.000 ms"),
        "summary: the encode's line");
  Check(Has(text, "touch pages self 3.00 ms (0.0050/frame, 2.5%) incl 3.00 ms"), "summary: the inner line");
  Check(Has(text, "holds self 1.20 ms"), "summary: the third line");
  // Largest own time first.
  const size_t encode = text.find("encode self");
  const size_t touch = text.find("touch pages self");
  const size_t holds = text.find("holds self");
  Check(encode < touch && touch < holds, "summary: sections by their own time, largest first");
  Check(!Has(text, "batch record"), "summary: a section that never ran is left out");
  Check(Has(text, "timing itself about 0.0000 ms/frame (0.0 sections/frame at 40 ns each)"),
        "summary: the cost of the stopwatch itself");
  // The text is split on ' | ': one header, the stopwatch's own cost and one part per section.
  size_t parts = 1;
  for (size_t at = text.find(" | "); at != std::string::npos; at = text.find(" | ", at + 3)) {
    ++parts;
  }
  CheckEq(parts, 2 + 3, "summary: one ' | ' part per section after the header");

  // Not known on this thread: no share, no 'not timed'.
  const std::string unknown = Summary(t, 600.0, 1.0, 0, 40.0);
  Check(Has(unknown, "busy time not measured on this thread"), "summary: busy time unknown");
  Check(!Has(unknown, "not timed"), "summary: no rest without a busy time");
  Check(Has(unknown, "encode self 6.00 ms (0.0100/frame, 0.0%)"), "summary: no share without a busy time");

  // A busy time smaller than the timed total (a clock that disagrees) does not go negative.
  const std::string small = Summary(t, 600.0, 1.0, 5'000'000, 0.0);
  Check(Has(small, "not timed 0.00 ms"), "summary: the rest does not go below zero");

  // Zero frames do not divide by zero.
  const std::string none = Summary(t, 0.0, 1.0, 120'000'000, 0.0);
  Check(Has(none, "cpu timing over 0 frames"), "summary: zero frames");
  Check(Has(none, "0.000/frame"), "summary: zero frames give zero means");

  // The stopwatch's own cost: 40 ticks at 1 ns for 3 sections' calls over 600 frames.
  const std::string cost = Summary(t, 600.0, 1.0, 120'000'000, 400'000.0);
  Check(Has(cost, "timing itself about "), "summary: the cost line is there");
  Check(Has(cost, "sections/frame at 400000 ns each"), "summary: the cost per section");
}

void TestTooDeepInSummary() {
  Accounts a;
  for (size_t i = 0; i < Accounts::kMaxDepth + 1; ++i) {
    a.Enter(kA, i);
  }
  for (size_t i = 0; i < Accounts::kMaxDepth + 1; ++i) {
    a.Exit(100 + i);
  }
  const std::string text = Summary(a.Take(200), 1.0, 1.0, 1000, 0.0);
  Check(Has(text, "1 sections too deep to time, 0 ends without a start"),
        "summary: the sections too deep are said");
}

}  // namespace

int main() {
  TestOneSection();
  TestNesting();
  TestDeepNesting();
  TestSiblingsAndGaps();
  TestRecursion();
  TestWindows();
  TestTooDeep();
  TestUnmatched();
  TestBackwardClock();
  TestNames();
  TestSummary();
  TestFrameWindow();
  TestTooDeepInSummary();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

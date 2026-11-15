// Check of the capture sync's decision and books (sync_builds.h). A program of its own: it needs no
// renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_sync_builds_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_sync_builds_test

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "sync_builds.h"

using namespace mcla::gpu::sync_builds;

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

bool Has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

void TestDecide() {
  // Off: the rule decides, nothing is held.
  Check(Decide(false, true).may_skip && !Decide(false, true).held, "off, rule allows: skip, not held");
  Check(!Decide(false, false).may_skip && !Decide(false, false).held, "off, rule refuses: no skip, not held");
  // On: nothing is skipped; a draw the rule would have left out is the one counted as held.
  Check(!Decide(true, true).may_skip && Decide(true, true).held, "on, rule allows: no skip, held");
  Check(!Decide(true, false).may_skip && !Decide(true, false).held,
        "on, rule refuses: no skip, not held (it waited before too)");
}

void TestBooks() {
  Books books;
  Check(books.session().Waits() == 0 && books.session().WaitNs() == 0, "fresh books are empty");

  Check(books.Note(Kind::kGuestDraw, 40'000'000), "the first wait is logged on its own");
  books.Note(Kind::kGuestDraw, 10'000'000);
  books.Note(Kind::kHdrVariant, 5'000'000);
  const Totals window = books.Take();
  Check(window.Waits() == 3, "window: 3 waits");
  Check(window.WaitNs() == 55'000'000, "window: 55 ms in all");
  Check(window.waits[size_t(Kind::kGuestDraw)] == 2 && window.waits[size_t(Kind::kHdrVariant)] == 1 &&
            window.waits[size_t(Kind::kUpscalerMask)] == 0,
        "window: waits by kind");
  Check(window.wait_ns[size_t(Kind::kGuestDraw)] == 50'000'000, "window: guest draws' time");
  Check(window.longest_ns == 40'000'000, "window: longest wait");

  // The next window starts empty; the session goes on.
  Check(books.Take().Waits() == 0, "a second Take gives an empty window");
  books.Note(Kind::kUpscalerMask, 70'000'000);
  const Totals second = books.Take();
  Check(second.Waits() == 1 && second.longest_ns == 70'000'000, "second window: only its own wait");
  Check(books.session().Waits() == 4 && books.session().WaitNs() == 125'000'000, "session keeps all 4 waits");
  Check(books.session().longest_ns == 70'000'000, "session: longest wait over both windows");

  // A kind out of range is counted in the last bucket instead of writing outside the array.
  Books odd;
  odd.Note(Kind::kCount, 1);
  odd.Note(Kind(200), 2);
  Check(odd.session().Waits() == 2 && odd.session().waits[size_t(Kind::kCount) - 1] == 2,
        "a kind out of range lands in the last bucket");
}

void TestLoggedEach() {
  Books books;
  uint32_t logged = 0;
  for (uint32_t i = 0; i < Books::kLoggedEach + 10; ++i) {
    // Spread over the kinds and over windows: the limit is the session's.
    if (books.Note(Kind(i % uint32_t(Kind::kCount)), 1000)) {
      ++logged;
    }
    if (i % 7 == 0) {
      books.Take();
    }
  }
  Check(logged == Books::kLoggedEach, "exactly the first kLoggedEach waits of a session are logged one by one");
  Check(books.session().Waits() == Books::kLoggedEach + 10, "every wait is counted, logged or not");
}

void TestNames() {
  std::set<std::string> names;
  for (size_t i = 0; i < size_t(Kind::kCount); ++i) {
    const std::string name = KindName(Kind(i));
    Check(name != "?" && !name.empty(), "every kind has a name");
    names.insert(name);
  }
  Check(names.size() == size_t(Kind::kCount), "the kinds' names differ");
  Check(std::string(KindName(Kind::kCount)) == "?", "kCount has no name");
}

void TestFormatLine() {
  Books books;
  for (int i = 0; i < 10; ++i) books.Note(Kind::kGuestDraw, 50'000'000);
  for (int i = 0; i < 2; ++i) books.Note(Kind::kHdrVariant, 11'700'000);
  const Totals window = books.Take();
  const std::string on = FormatLine("interval", true, window, books.session());
  Check(Has(on, "[gpu] capture sync (interval): on |"), "line: prefix, why and state");
  Check(Has(on, "12 wait(s)"), "line: the count");
  Check(Has(on, "(guest draws 10, hdr variants 2)"), "line: the kinds that waited, in order");
  Check(Has(on, "523.4 ms in all"), "line: total time");
  Check(Has(on, "mean 43.6"), "line: mean");
  Check(Has(on, "longest 50.0 ms"), "line: longest");
  Check(Has(on, "session 12 wait(s), 523.4 ms"), "line: session totals");
  Check(!Has(on, "upscaler masks"), "line: a kind with no wait is not listed");

  // An empty window while the flag is on: still a line, with no kinds list and no division by zero.
  const std::string quiet = FormatLine("final", true, books.Take(), books.session());
  Check(Has(quiet, "capture sync (final): on | 0 wait(s)"), "empty window: zero waits");
  Check(!Has(quiet, "(guest") && !Has(quiet, "nan") && !Has(quiet, "inf"), "empty window: no list, no nan");
  Check(Has(quiet, "mean 0.0"), "empty window: mean 0");
  Check(Has(quiet, "session 12 wait(s)"), "empty window: the session's count stays");

  const std::string off = FormatLine("interval", false, Totals{}, books.session());
  Check(Has(off, "capture sync (interval): off |"), "off state");
}

}  // namespace

int main() {
  TestDecide();
  TestBooks();
  TestLoggedEach();
  TestNames();
  TestFormatLine();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  std::printf(g_failed == 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
  return g_failed == 0 ? 0 : 1;
}

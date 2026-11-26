// Check of the render thread's sync build lines (sync_build_log.h): the rate limit, the counts and the
// wording that perf/scripts/stall_check.py reads. A program of its own: it needs no renderer, no ReXGlue
// and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_sync_build_log_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_sync_build_log_test

#include <cstdint>
#include <cstdio>
#include <regex>
#include <string>

#include "sync_build_log.h"

using namespace mcla::gpu::sync_build_log;

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

void TestRateLimit() {
  Check(!LogThis(0), "event 0 does not exist");
  bool first_all = true;
  for (uint64_t n = 1; n <= kLoggedFirst; ++n) first_all = first_all && LogThis(n);
  Check(first_all, "the first kLoggedFirst events each get a line");
  // After the first ones only every kLoggedEvery-th: the numbers in between are silent.
  uint64_t logged = 0;
  uint64_t last = 0;
  bool every = true;
  for (uint64_t n = kLoggedFirst + 1; n <= 1000; ++n) {
    if (LogThis(n)) {
      ++logged;
      every = every && n % kLoggedEvery == 0;
      last = n;
    }
  }
  Check(every, "after the first ones only multiples of kLoggedEvery");
  Check(logged > 0 && last > 900, "the tail of a long run still gets lines");
  // 276 builds at the first night frames (NIGHT-STALL): a few dozen lines, not 276.
  uint64_t night = 0;
  for (uint64_t n = 1; n <= 276; ++n) night += LogThis(n) ? 1 : 0;
  Check(night >= kLoggedFirst && night < 60, "276 builds write fewer than 60 lines");
  // The count of a run is the largest #N the lines name: the last logged number is within kLoggedEvery of the total.
  Check(1000 - last < kLoggedEvery, "the largest #N is within kLoggedEvery of the real count");
}

void TestCounters() {
  Counters c;
  Check(c.NoteBuild(40'000'000) == 1, "the first build is #1");
  Check(c.NoteBuild(10'000'000) == 2, "the second build is #2");
  Check(c.NoteWait(7'000'000) == 1, "waits are counted on their own: #1");
  Check(c.builds == 2 && c.waits == 1, "two builds, one wait");
  Check(c.build_ns == 50'000'000 && c.wait_ns == 7'000'000, "the times add up");
  Check(c.longest_ns == 40'000'000, "the longest of both kinds");
}

KeyWords NightKey() {
  KeyWords k;
  k.vs = 0x0123456789ABCDEFull;
  k.ps = 0xFEDCBA9876543210ull;
  k.vs_variant = 0;
  k.ps_variant = 0x2A;
  k.color_count = 4;
  k.sample_count = 2;
  k.stencil = true;
  return k;
}

void TestBuildLine() {
  const std::string line = FormatBuild(12, 3456, 41'200'000, Why::kNew, NightKey(), false);
  Check(line == "[pipeline] sync build #12: guest frame 3456, 41.2 ms, new, vs 0123456789ABCDEF/0 ps "
                "FEDCBA9876543210/2A, 4 colour target(s), 2x, stencil, background build refused",
        "the build line has the documented shape: " + line);
  KeyWords plain = NightKey();
  plain.stencil = false;
  const std::string allowed = FormatBuild(1, 1, 0, Why::kClaimed, plain, true);
  Check(Has(allowed, "0.0 ms, claimed,") && Has(allowed, "no stencil") && Has(allowed, "background build allowed"),
        "claimed, no stencil, allowed: " + allowed);
  Check(Has(FormatBuild(2, 1, 1'000'000, Why::kAgain, plain, true), "1.0 ms, again,"), "again");
  Check(WhyName(Why::kNew) == std::string("new") && WhyName(Why::kClaimed) == std::string("claimed") &&
            WhyName(Why::kAgain) == std::string("again"),
        "the three reasons keep their words (stall_check.py reads them)");
  for (Background reason : {Background::kRequired, Background::kSingleSample, Background::kStencilWrite,
                            Background::kTargetHistory, Background::kNoTarget, Background::kRetryListed}) {
    Check(Has(FormatBuild(12, 3456, 41'200'000, Why::kNew, NightKey(), reason),
              std::string("background build refused (") + BackgroundName(reason) + ")"),
          "mandatory draws and listed retries retain the old log prefix and name their reason");
  }
  Check(Has(FormatBuild(1, 1, 0, Why::kNew, plain, Background::kCapture),
            "background build allowed (capture needs exact draw)"), "a capture overrides an eligible draw");
  Check(Has(FormatBuild(1, 1, 0, Why::kNew, plain, Background::kCannotQueue),
            "background build allowed (request could not be queued)"), "a queue rejection is distinct from the draw rule");
}

void TestDrawUse() {
  DrawUse use;
  Check(!use.UsedIn(0), "a key never used as a fallback is not held in guest frame zero");
  use.Note(0);
  use.Note(0);
  Check(use.UsedIn(0) && !use.UsedIn(1), "the fallback holds only through the last affected guest frame");
  use.Note(4);
  Check(use.draws == 3 && use.frames == 2 && use.first_frame == 0 && use.last_frame == 4,
        "tracks distinct affected guest frames, including gaps, independently of draw count");
}

void TestWaitLine() {
  const std::string line = FormatWait(3, 3460, 8'700'000, "list", NightKey());
  Check(line == "[pipeline] sync wait #3: guest frame 3460, 8.7 ms, list build, vs 0123456789ABCDEF/0 ps "
                "FEDCBA9876543210/2A, 4 colour target(s), 2x, stencil",
        "the wait line has the documented shape: " + line);
}

// VK-FIRST-FRAME: a wait that handed a worker the render thread's priority and/or made the pipeline here too has a line of its own, which stall_check.py's "sync wait #N" pattern
// must NOT read as one of the waits (it counts those), and a totals line with the session's.
void TestHandoverLines() {
  const std::string both = FormatHandover(2, 3460, 412'000'000, "list", true, true, NightKey());
  Check(both == "[pipeline] sync wait handover #2: guest frame 3460, 412.0 ms, list build, worker raised to normal priority, made here too, vs 0123456789ABCDEF/0 ps "
                "FEDCBA9876543210/2A, 4 colour target(s), 2x, stencil",
        "the handover line has the documented shape: " + both);
  const std::string raised_only = FormatHandover(1, 5, 31'000'000, "request", true, false, NightKey());
  Check(Has(raised_only, "worker raised to normal priority") && !Has(raised_only, "made here too"), "raised only: " + raised_only);
  const std::string here_only = FormatHandover(1, 5, 400'000'000, "list", false, true, NightKey());
  Check(Has(here_only, "worker not raised, made here too"), "made here only: " + here_only);
  const std::regex wait(R"(\[pipeline\] sync wait #(\d+): guest frame (\d+), ([0-9.]+) ms, (\w+) build, vs)");
  Check(!std::regex_search(both, wait), "stall_check's wait pattern does not read a handover line as a wait");
  Check(FormatHandoverSession(3, 1) == "[pipeline] sync wait handovers this session: 3 wait(s) raised a worker or made the pipeline here too, 1 of them made it here",
        "the handovers' totals line");
}

void TestSessionLine() {
  Counters c;
  c.NoteBuild(30'000'000);
  c.NoteBuild(12'500'000);
  c.NoteWait(5'000'000);
  const std::string line = FormatSession(c);
  Check(line == "[pipeline] sync builds this session: 2 build(s) 42.5 ms, 1 wait(s) 5.0 ms, longest 30.0 ms",
        "the session line: " + line);
  Check(Has(FormatSession(Counters{}), "0 build(s) 0.0 ms, 0 wait(s) 0.0 ms"), "an empty session still reads");
}

// The words the health checks read as an error must not be in any of the lines: a sync build is information.
// What the key's stencil words must say for a line to name the stencil: the pipeline WRITES it (an operation other than KEEP, a write mask), as the game's rule
// (Scene::MaySkipForPipeline) and pipeline_list.py's stencil_writes read it. The packing is PipelineKey's (pipelines.h): the words below are built the way BuildKey builds them.
void TestStencilWrites() {
  constexpr uint32_t keep = kStencilKeep, replace = 3;
  const auto stencil_word = [&](uint32_t front_pass, uint32_t back_pass) {
    return 1u | (8u << 1) | (keep << 5) | (front_pass << 9) | (keep << 13) | (8u << 17) | (keep << 21) | (back_pass << 25);
  };
  const auto masks_word = [&](uint32_t write_mask, uint32_t back_depth_fail) { return 0xFFu | (write_mask << 8) | (0x20u << 16) | (back_depth_fail << 24); };
  Check(StencilWrites(stencil_word(replace, replace), masks_word(0xFF, keep)), "REPLACE on a pass with a write mask writes the stencil (the lit world draws)");
  Check(!StencilWrites(stencil_word(keep, keep), masks_word(0xFF, keep)), "all KEEP only tests the stencil");
  Check(!StencilWrites(stencil_word(replace, replace), masks_word(0, keep)), "a write mask of 0 writes nothing");
  Check(!StencilWrites(0, 0), "stencil off writes nothing");
  Check(!StencilWrites(stencil_word(replace, replace) & ~1u, masks_word(0xFF, keep)), "the enable bit is what turns it on");
  Check(StencilWrites(stencil_word(keep, replace), masks_word(0xFF, keep)), "the back face's pass operation counts");
  Check(StencilWrites(stencil_word(keep, keep), masks_word(0xFF, 3)), "the back face's depth fail operation (in the masks word) counts");
  Check(StencilWrites(1u | (keep << 5) | (keep << 9) | (3u << 13), masks_word(0x01, keep)), "the front face's depth fail operation counts, whatever bits of the mask");
}

// The lines are read by perf/scripts/stall_check.py: the same patterns, copied (keep them in step with SYNC_BUILD, SYNC_WAIT and SYNC_SESSION there).
void TestStallCheckPatterns() {
  const std::regex build(R"(\[pipeline\] sync build #(\d+): guest frame (\d+), ([0-9.]+) ms, (\w+), vs ([0-9A-Fa-f]+)/([0-9A-Fa-f]+) ps ([0-9A-Fa-f]+)/([0-9A-Fa-f]+), )"
                         R"((\d+) colour target\(s\), (\d+)x, (stencil|no stencil), background build (allowed|refused))");
  const std::regex wait(R"(\[pipeline\] sync wait #(\d+): guest frame (\d+), ([0-9.]+) ms, (\w+) build, vs ([0-9A-Fa-f]+)/([0-9A-Fa-f]+) ps ([0-9A-Fa-f]+)/([0-9A-Fa-f]+))");
  const std::regex session(R"(\[pipeline\] sync builds this session: (\d+) build\(s\) ([0-9.]+) ms, (\d+) wait\(s\) ([0-9.]+) ms, longest ([0-9.]+) ms)");
  KeyWords plain = NightKey();
  plain.stencil = false;
  plain.vs_variant = 0x2000;
  for (const Why why : {Why::kNew, Why::kClaimed, Why::kAgain}) {
    for (const bool skippable : {false, true}) {
      for (const KeyWords& k : {NightKey(), plain}) {
        std::smatch m;
        const std::string line = FormatBuild(12345, 987654, 41'200'000, why, k, skippable);
        Check(std::regex_search(line, m, build) && m[1] == "12345" && m[2] == "987654" && m[3] == "41.2" && m[4] == WhyName(why) && m[11] == (k.stencil ? "stencil" : "no stencil") &&
                  m[12] == (skippable ? "allowed" : "refused"),
              "stall_check's build pattern reads: " + line);
      }
    }
  }
  for (const char* source : {"list", "request"}) {
    std::smatch m;
    const std::string line = FormatWait(7, 100, 8'700'000, source, plain);
    Check(std::regex_search(line, m, wait) && m[1] == "7" && m[4] == source && m[6] == "2000", "stall_check's wait pattern reads: " + line);
  }
  Counters c;
  c.NoteBuild(30'000'000);
  c.NoteWait(5'000'000);
  const std::string line = FormatSession(c);
  std::smatch m;
  Check(std::regex_search(line, m, session) && m[1] == "1" && m[3] == "1" && m[5] == "30.0", "stall_check's session pattern reads: " + line);
}

void TestNoErrorWords() {
  const std::string all = FormatBuild(1, 1, 1, Why::kNew, NightKey(), true) + FormatBuild(1, 1, 1, Why::kClaimed, NightKey(), false) +
                          FormatBuild(1, 1, 1, Why::kAgain, NightKey(), false) + FormatWait(1, 1, 1, "list", NightKey()) +
                          FormatWait(1, 1, 1, "request", NightKey()) + FormatSession(Counters{}) + FormatHandover(1, 1, 1, "list", true, true, NightKey()) +
                          FormatHandover(1, 1, 1, "request", false, true, NightKey()) + FormatHandoverSession(1, 1);
  for (const char* word : {"fail", "error", "crash", "stalled", "validation", "Watchdog"}) {
    Check(!Has(all, word), std::string("no \"") + word + "\" in any line");
  }
}

}  // namespace

int main() {
  TestRateLimit();
  TestCounters();
  TestBuildLine();
  TestDrawUse();
  TestWaitLine();
  TestHandoverLines();
  TestSessionLine();
  TestStencilWrites();
  TestStallCheckPatterns();
  TestNoErrorWords();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  std::printf(g_failed == 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
  return g_failed == 0 ? 0 : 1;
}

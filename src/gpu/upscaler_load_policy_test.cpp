// Check of the rules that say when the upscalers' libraries are loaded (upscaler_load_policy.h). A
// program of its own: no renderer, no Windows, no GPU; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_upscaler_load_policy_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_upscaler_load_policy_test

#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "upscaler_load_policy.h"

using namespace mcla::gpu::upscaler_load;

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

const char* LoadName(FfxLoad load) {
  return load == FfxLoad::kAtStart ? "at start" : "on first use";
}

// The whole table of DecideFfxLoad, written out: vendor x saved choice x switch.
void CheckFfxTable() {
  struct Row {
    const char* vendor_name;
    uint32_t vendor;
  };
  const Row vendors[] = {{"NVIDIA", kVendorNvidia}, {"AMD", kVendorAmd}, {"Intel", kVendorIntel},
                         {"unknown", 0}, {"Microsoft basic render", 0x1414}};
  for (const Row& v : vendors) {
    for (bool fsr_saved : {false, true}) {
      for (bool lazy : {false, true}) {
        FfxLoadInputs in;
        in.lazy_enabled = lazy;
        in.vendor = v.vendor;
        in.fsr_saved = fsr_saved;
        // At the start: the switch is off, an FSR is the saved choice, or an AMD GPU (FSR 4).
        const bool want_start = !lazy || fsr_saved || v.vendor == kVendorAmd;
        const FfxLoad got = DecideFfxLoad(in);
        Check(got == (want_start ? FfxLoad::kAtStart : FfxLoad::kOnFirstUse),
              std::string("FFX load, vendor ") + v.vendor_name + ", fsr saved " +
                  (fsr_saved ? "yes" : "no") + ", lazy " + (lazy ? "on" : "off") + ": got " +
                  LoadName(got));
      }
    }
  }
  // The cases the design names, one by one.
  Check(DecideFfxLoad({true, kVendorNvidia, false}) == FfxLoad::kOnFirstUse,
        "an RTX card with no FSR saved loads FidelityFX on first use");
  Check(DecideFfxLoad({true, kVendorNvidia, true}) == FfxLoad::kAtStart,
        "an RTX card with FSR saved loads it at the start (the scene size needs the answer)");
  Check(DecideFfxLoad({true, kVendorAmd, false}) == FfxLoad::kAtStart,
        "an AMD card asks at the start (FSR 4 can only be known from the runtime)");
  Check(DecideFfxLoad({true, kVendorIntel, false}) == FfxLoad::kOnFirstUse,
        "an Intel card never has FSR 4: first use");
  Check(DecideFfxLoad({false, kVendorNvidia, false}) == FfxLoad::kAtStart,
        "with the switch off everything loads at the start, as before");
}

void CheckLazyRuntime() {
  {
    LazyRuntime r;
    Check(r.state() == LazyRuntime::State::kIdle && r.NeverNeeded(), "a new runtime is idle and unused");
    Check(r.Listed(), "an idle runtime is listed (its answer is not known yet)");
    Check(r.Need(), "the first need starts the load");
    Check(r.state() == LazyRuntime::State::kLoading && !r.NeverNeeded(), "then it is loading");
    Check(!r.Need() && !r.Need(), "a second and third need do not start another load");
    Check(r.Listed(), "a loading runtime is listed");
    r.Ended(true);
    Check(r.state() == LazyRuntime::State::kLoaded && r.Listed(), "a successful load leaves it loaded and listed");
    Check(!r.Need(), "a loaded runtime is never started again");
    r.Ended(false);
    Check(r.state() == LazyRuntime::State::kLoaded, "a late failure report does not undo a loaded runtime");
  }
  {
    LazyRuntime r;
    r.Need();
    r.Ended(false);
    Check(r.state() == LazyRuntime::State::kFailed && !r.Listed(), "a failed load is not listed any more");
    Check(!r.Need(), "a failed runtime is not tried again in the session");
    r.Ended(true);
    Check(r.state() == LazyRuntime::State::kFailed, "a late success report does not revive a failed runtime");
  }
  {
    LazyRuntime r;
    r.Ended(true);
    Check(r.state() == LazyRuntime::State::kIdle, "an end report with no load asked for is ignored");
    r.LoadedAtStart(true);
    Check(r.state() == LazyRuntime::State::kLoaded && !r.NeverNeeded(), "loaded by the start counts as loaded");
    Check(!r.Need(), "and needs nothing");
    LazyRuntime f;
    f.LoadedAtStart(false);
    Check(f.state() == LazyRuntime::State::kFailed && !f.Listed(), "a start that failed to load is failed");
  }
}

void CheckInterest() {
  Interest i;
  Check(!i.Noted(), "no interest at first");
  Check(i.Note(), "the first note says it is the first");
  Check(i.Noted(), "interest is noted");
  Check(!i.Note(), "a second note is not the first");
  Check(i.Noted(), "and it stays noted");
  i.ClearForTest();
  Check(!i.Noted(), "cleared");

  // Many threads noting at once: exactly one of them is the first.
  Interest shared;
  std::atomic<int> firsts{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&] {
      for (int k = 0; k < 1000; ++k) {
        if (shared.Note()) {
          firsts.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& t : threads) {
    t.join();
  }
  Check(firsts.load() == 1, "of 16 threads noting 1000 times each exactly one was first");
  Check(shared.Noted(), "and it is noted");
}

void CheckWarmup() {
  // The whole table.
  Check(WarmupMayStart({false, false}), "switch off, no interest: the warm-up starts as before");
  Check(WarmupMayStart({false, true}), "switch off, interest: starts");
  Check(!WarmupMayStart({true, false}), "lazy, no interest: no warm-up (nothing of Streamline loads)");
  Check(WarmupMayStart({true, true}), "lazy, interest: the warm-up starts");

  // The sequence a session goes through, interest being the one input that changes.
  Interest interest;
  WarmupInputs in;
  in.lazy_enabled = true;
  Check(!WarmupMayStart(in), "a session starts without a warm-up");
  in.interest = interest.Noted();
  Check(!WarmupMayStart(in), "reading the interest says no");
  interest.Note();
  in.interest = interest.Noted();
  Check(WarmupMayStart(in), "after the player shows the UPSCALING row the warm-up may start");
}

}  // namespace

int main() {
  CheckFfxTable();
  CheckLazyRuntime();
  CheckInterest();
  CheckWarmup();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

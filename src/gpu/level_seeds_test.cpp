// Check of the level seeds' rules (level_seeds.h: which saved records are made ahead for a new MSAA level, the
// memory bound, the queue rule, the run's counts). A program of its own: it needs no renderer, no SDK and no GPU,
// and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_level_seeds_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_level_seeds_test
//
// The records are stand-ins for Pipelines::Record: a key with an id (the rest of the pipeline) and the sample
// count, which is all the rules read. The card sizes are real ones.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "level_seeds.h"

using namespace mcla::gpu::level_seeds;

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

constexpr uint64_t M(uint64_t mib) {
  return mib << 20;
}

struct Key {
  uint32_t id = 0;
  uint32_t sample_count = 1;
  bool operator==(const Key& o) const { return id == o.id && sample_count == o.sample_count; }
};
struct KeyHash {
  size_t operator()(const Key& k) const { return size_t(k.id) * 1000003u + k.sample_count; }
};
struct Rec {
  Key key;
  uint32_t layout = 0;
};

std::vector<Rec> List(std::initializer_list<std::pair<uint32_t, uint32_t>> id_samples) {
  std::vector<Rec> v;
  for (const auto& p : id_samples) {
    v.push_back(Rec{Key{p.first, p.second}, 0});
  }
  return v;
}

bool NoneThere(const Key&) {
  return false;
}

// ---- the key conversion ------------------------------------------------------------------------------

void TestAtLevel() {
  const Key k{7, 2};
  const Key to4 = AtLevel(k, 4);
  Check(to4.id == 7 && to4.sample_count == 4, "AtLevel changes the sample count and nothing else");
  Check(k.sample_count == 2, "AtLevel does not change its argument");
  Check(AtLevel(k, 1).sample_count == 1, "level 1 is a conversion too (MSAA off, an upscaler)");
}

// ---- the plan ----------------------------------------------------------------------------------------

void TestPlanBasics() {
  // ids 1..3 recorded at 2 samples, 4 at 4 samples (a cascade, say), 5 and 6 with one sample.
  const std::vector<Rec> list = List({{1, 2}, {2, 2}, {3, 2}, {4, 4}, {5, 1}, {6, 1}});
  PlanCounts c;
  std::vector<uint32_t> picks = PlanSeeds<Rec, KeyHash>(list, 4, 2, 100, NoneThere, c);
  // To 4: the three 2-sample records convert; the 4-sample one is already at the level; the
  // single-sample ones are no source.
  CheckNum(picks.size(), 3, "to 4: three seeds");
  Check(picks == std::vector<uint32_t>({0, 1, 2}), "to 4: the list's order");
  CheckNum(c.sources, 4, "to 4: four multisampled sources");
  CheckNum(c.single, 2, "to 4: two single-sample records");
  CheckNum(c.same_level, 1, "to 4: one source is at the level");
  CheckNum(c.picked, 3, "to 4: picked counted");

  // To 1: all four multisampled records convert (the single-sample records are not touched).
  picks = PlanSeeds<Rec, KeyHash>(list, 1, 2, 100, NoneThere, c);
  CheckNum(picks.size(), 4, "to 1: four seeds");
  CheckNum(c.same_level, 0, "to 1: none at the level");
  CheckNum(c.single, 2, "to 1: the single-sample records are counted once, not per pass");

  // A record of 1 sample is never converted upward: a draw into a plain target would never ask for it.
  const std::vector<Rec> only_single = List({{1, 1}, {2, 1}});
  picks = PlanSeeds<Rec, KeyHash>(only_single, 4, 2, 100, NoneThere, c);
  Check(picks.empty() && c.sources == 0 && c.single == 2, "single-sample records give no seeds");

  // An empty list and a zero limit.
  picks = PlanSeeds<Rec, KeyHash>(std::vector<Rec>{}, 4, 2, 100, NoneThere, c);
  Check(picks.empty(), "an empty list gives nothing");
  picks = PlanSeeds<Rec, KeyHash>(list, 4, 2, 0, NoneThere, c);
  Check(picks.empty() && c.limited == 3, "limit 0: nothing is picked, all three are counted as limited");
}

void TestPlanSkips() {
  const std::vector<Rec> list = List({{1, 2}, {2, 2}, {3, 2}, {4, 2}});
  PlanCounts c;
  // The records of 2 and 3 already have a copy at 4 (a record, a queue entry, a pipeline: `present`).
  const std::unordered_set<uint32_t> there = {2, 3};
  std::vector<uint32_t> picks = PlanSeeds<Rec, KeyHash>(
      list, 4, 2, 100, [&](const Key& k) { return k.sample_count == 4 && there.count(k.id) != 0; }, c);
  Check(picks == std::vector<uint32_t>({0, 3}), "a copy that exists is not made again");
  CheckNum(c.present, 2, "present counted");

  // `present` is asked with the key AT the level, never the source's.
  bool asked_other = false;
  PlanSeeds<Rec, KeyHash>(list, 4, 2, 100, [&](const Key& k) { asked_other |= k.sample_count != 4; return false; }, c);
  Check(!asked_other, "present is asked for keys at the new level only");
}

void TestPlanDuplicatesAndGroups() {
  // The same pipeline recorded at 2 and at 4 samples (a draw seen at two levels): one copy at 1.
  const std::vector<Rec> list = List({{1, 4}, {2, 2}, {1, 2}, {3, 4}});
  PlanCounts c;
  std::vector<uint32_t> picks = PlanSeeds<Rec, KeyHash>(list, 1, 2, 100, NoneThere, c);
  // The level in force is 2: its group comes first ({2,2} at 1, {1,2} at 2), then the 4-sample group
  // ({1,4} is the same pipeline as {1,2}: a duplicate; {3,4} is picked).
  Check(picks == std::vector<uint32_t>({1, 2, 3}), "groups: the level in force first, each in list order; duplicates once");
  CheckNum(c.duplicate, 1, "one duplicate");
  CheckNum(c.picked, 3, "three picked");

  // With no level in force (0) the plain list order is used.
  picks = PlanSeeds<Rec, KeyHash>(list, 1, 0, 100, NoneThere, c);
  Check(picks == std::vector<uint32_t>({0, 1, 3}), "from_level 0: the list's order (the 4-sample twin of {1} comes first and wins)");

  // The limit keeps the first of the order, not the first of the list.
  picks = PlanSeeds<Rec, KeyHash>(list, 1, 2, 2, NoneThere, c);
  Check(picks == std::vector<uint32_t>({1, 2}), "the limit cuts the tail of the order");
  CheckNum(c.limited, 1, "one left out by the limit");
  CheckNum(c.picked, 2, "two picked");
}

void TestPlanBig() {
  // 1,300 records at 2 samples and 800 at 4 (the starter list's shape): to 1, every record is a source;
  // the limit holds the memory bound's cap.
  std::vector<Rec> list;
  for (uint32_t i = 0; i < 1300; ++i) {
    list.push_back(Rec{Key{i, 2}, 0});
  }
  for (uint32_t i = 0; i < 800; ++i) {
    list.push_back(Rec{Key{i * 2, 4}, 0});  // 650 of them are the same pipeline as a 2-sample record
  }
  PlanCounts c;
  const std::vector<uint32_t> picks = PlanSeeds<Rec, KeyHash>(list, 1, 2, kMaxSeeds, NoneThere, c);
  CheckNum(c.sources, 2100, "big list: sources");
  CheckNum(c.duplicate, 650, "big list: the twins at 4 samples are duplicates");
  CheckNum(picks.size(), kMaxSeeds, "big list: capped at kMaxSeeds");
  CheckNum(c.picked + c.limited, 1450, "big list: picked + limited = the distinct pipelines");
}

// ---- the memory bound --------------------------------------------------------------------------------

void TestAllowedCount() {
  // 16 GB card, 5 GB in use: room for the cap.
  CheckNum(AllowedCount(M(5000), M(15000), 0), kMaxSeeds, "a roomy card: the cap");
  // The cap counts what is held.
  CheckNum(AllowedCount(M(5000), M(15000), 1000), 24, "held seeds count against the cap");
  CheckNum(AllowedCount(M(5000), M(15000), kMaxSeeds), 0, "at the cap: none");
  CheckNum(AllowedCount(M(5000), M(15000), kMaxSeeds + 5), 0, "over the cap: none");
  // 4 GB laptop card (budget 3,900 MiB): the line is 80% = 3,120 MiB. 3,000 MiB in use leaves 120
  // MiB: 120 MiB / 128 KiB = 960 pipelines.
  CheckNum(AllowedCount(M(3000), M(3900), 0), 960, "the 4 GB card with 3,000 MiB in use: 960");
  CheckNum(AllowedCount(M(3100), M(3900), 0), 160, "... with 3,100 in use: 160");
  CheckNum(AllowedCount(M(3120), M(3900), 0), 0, "exactly on the line: none");
  CheckNum(AllowedCount(M(3500), M(3900), 0), 0, "over the line: none");
  // A small card: 2 GB, 1,500 in use, budget 1,900: the line 1,520: 20 MiB = 160.
  CheckNum(AllowedCount(M(1500), M(1900), 0), 160, "the 2 GB card: a few");
  // An adapter that reports no budget: the cap alone.
  CheckNum(AllowedCount(M(9000), 0, 0), kMaxSeeds, "no budget reported: the cap");
  CheckNum(AllowedCount(0, 0, 100), kMaxSeeds - 100, "no budget: the cap less what is held");
  // The planning figure keeps the cap's memory in a plain range: 1,024 x 128 KiB = 128 MiB.
  CheckNum(uint64_t(kMaxSeeds) * kBytesEach, M(128), "the cap is 128 MiB of planning figure");
}

void TestShortOfRoom() {
  const uint64_t budget = M(3900);  // the line is 85% = 3,315 MiB
  Check(!ShortOfRoom(M(3000), budget), "3,000 of 3,900: not short");
  Check(!ShortOfRoom(M(3314), budget), "just under the line: not short");
  Check(ShortOfRoom(M(3316), budget), "just over the line: short");
  Check(ShortOfRoom(M(3900), budget), "full: short");
  Check(!ShortOfRoom(M(9999), 0), "no budget reported: never short");
  // The hysteresis: seeds start under 80 and go over 85, and the caches' own response starts at 90.
  static_assert(kStartPercent < kDropPercent, "seeds start lower than they are let go");
  static_assert(kDropPercent < 90, "the seeds go before the caches' response (vram_budget.h kHighPercent 90)");
  Check(AllowedCount(M(3200), budget, 0) == 0 && !ShortOfRoom(M(3200), budget),
        "between 80 and 85: none are made, none are let go");
}

// ---- what a level change does ------------------------------------------------------------------------

void TestDecide() {
  Check(Decide(0, 4) == Retarget::kStart, "no seeds yet: start");
  Check(Decide(4, 4) == Retarget::kKeep, "the same level again: keep");
  Check(Decide(4, 1) == Retarget::kRestart, "another level: restart");
  Check(Decide(1, 2) == Retarget::kRestart, "1 to 2: restart");
  Check(Decide(8, 2) == Retarget::kRestart, "8 to 2: restart");
}

// ---- the queue rule ----------------------------------------------------------------------------------

void TestSlots() {
  CheckNum(Slots(1), 1, "one worker: one seed at a time");
  CheckNum(Slots(2), 1, "two workers: one is kept free");
  CheckNum(Slots(3), 2, "three workers (a 4 thread PC's boot mode count): two");
  CheckNum(Slots(4), 3, "four workers (8 threads): three");
  CheckNum(Slots(6), 5, "six workers (12 threads and up): five");
  CheckNum(Slots(0), 1, "no workers is read as one");
  for (uint32_t w = 2; w <= 8; ++w) {
    Check(Slots(w) < w, "with two workers or more one stays free for urgent work");
  }
}

void TestMayStart() {
  Check(MayStart(true, false, 0, 3), "queued, no boot mode, free slot: start");
  Check(!MayStart(false, false, 0, 3), "nothing queued: no");
  Check(!MayStart(true, true, 0, 3), "boot mode: the list has every processor, no seed starts");
  Check(!MayStart(true, false, 3, 3), "all slots busy: no");
  Check(MayStart(true, false, 2, 3), "one slot free: start");
  // A model of the worker's wake rule: boot mode ends, the seeds start; a slot frees, the next starts.
  uint32_t active = 0;
  uint32_t queued = 10;
  bool boot = true;
  uint32_t started_in_boot = 0;
  for (int i = 0; i < 5; ++i) {
    if (MayStart(queued > 0, boot, active, Slots(4))) {
      ++started_in_boot;
    }
  }
  CheckNum(started_in_boot, 0, "no seed starts while boot mode is on");
  boot = false;
  uint32_t started = 0;
  while (MayStart(queued > 0, boot, active, Slots(4))) {
    ++active;
    --queued;
    ++started;
  }
  CheckNum(started, 3, "boot mode over, four workers: three seeds at once");
  --active;  // one finishes
  Check(MayStart(queued > 0, boot, active, Slots(4)), "a finished seed frees its slot");
}

// ---- the run's counts --------------------------------------------------------------------------------

using Clock = SeedBook::Clock;

void TestBookRun() {
  SeedBook book;
  const Clock::time_point t0 = Clock::now();
  Check(!book.Active() && book.Live() == 0 && book.Outstanding() == 0, "a new book is idle");
  SeedBook::Report r;
  Check(!book.TakeDone(r), "nothing to report before a run");

  const uint32_t gen = book.Begin(4, 10, t0);
  Check(gen != 0, "a generation is never 0");
  CheckNum(book.Outstanding(), 10, "ten queued");
  CheckNum(book.Live(), 10, "ten live");
  for (int i = 0; i < 6; ++i) {
    book.Finished(gen, true, t0 + std::chrono::milliseconds(100 + i));
  }
  book.Finished(gen, false, t0 + std::chrono::milliseconds(200));
  CheckNum(book.Outstanding(), 3, "seven finished, three left");
  CheckNum(book.Live(), 9, "the failed one holds nothing: nine live");
  Check(!book.TakeDone(r), "not done while three are outstanding");
  book.Taken(gen);
  book.Taken(gen);
  CheckNum(book.Live(), 7, "two taken by draws: seven live");
  book.Claimed(gen);  // a draw made one itself before a worker began it
  book.Cancelled(gen, 1);
  CheckNum(book.Outstanding(), 1, "one claimed, one cancelled: one outstanding");
  Check(!book.TakeDone(r), "not done with one outstanding");
  book.Finished(gen, true, t0 + std::chrono::milliseconds(350));
  CheckNum(book.Outstanding(), 0, "none outstanding");
  Check(book.TakeDone(r), "done");
  CheckNum(r.level, 4, "report: level");
  CheckNum(r.queued, 10, "report: queued");
  CheckNum(r.ready, 7, "report: ready");
  CheckNum(r.failed, 1, "report: failed");
  CheckNum(r.taken, 2, "report: taken");
  CheckNum(r.claimed, 1, "report: claimed");
  CheckNum(r.cancelled, 1, "report: cancelled");
  Check(r.build_ms >= 349.0 && r.build_ms <= 351.0, "report: the time is from the call to the last build");
  Check(!book.TakeDone(r), "reported once");
  CheckNum(r.ready + r.failed + r.claimed + r.cancelled, 10, "every queued seed is accounted for");
}

void TestBookStaleEvents() {
  SeedBook book;
  const Clock::time_point t0 = Clock::now();
  const uint32_t first = book.Begin(4, 5, t0);
  book.Finished(first, true, t0);
  // The level changes again: the old run ends, a new one begins.
  book.End();
  const uint32_t second = book.Begin(1, 3, t0);
  Check(second != first, "a new run has a new generation");
  // A worker finishing a seed of the first run late counts for nothing.
  book.Finished(first, true, t0);
  book.Taken(first);
  book.Claimed(first);
  book.Cancelled(first, 2);
  book.Freed(first, 2);
  book.Add(first, 100);
  CheckNum(book.Outstanding(), 3, "stale events leave the new run's three outstanding");
  SeedBook::Report snap = book.Snapshot();
  CheckNum(snap.ready + snap.failed + snap.taken + snap.claimed + snap.cancelled + snap.freed, 0,
           "stale events are not counted");
  CheckNum(snap.queued, 3, "stale Add does not add");
  // An ended run takes no events at all.
  book.End();
  book.Finished(second, true, t0);
  Check(!book.Active() && book.Live() == 0, "after End nothing counts");
  SeedBook::Report r;
  Check(!book.TakeDone(r), "an ended run is not reported as done");
}

void TestBookAdd() {
  // The same level asked for again while the run is going: more seeds join it, and it reports done once
  // for all of them.
  SeedBook book;
  const Clock::time_point t0 = Clock::now();
  const uint32_t gen = book.Begin(4, 2, t0);
  book.Finished(gen, true, t0);
  book.Finished(gen, true, t0);
  SeedBook::Report r;
  Check(book.TakeDone(r), "the first two are done");
  book.Add(gen, 3);
  Check(!book.TakeDone(r), "three more are outstanding");
  for (int i = 0; i < 3; ++i) {
    book.Finished(gen, true, t0);
  }
  Check(book.TakeDone(r), "done again after the added ones");
  CheckNum(r.queued, 5, "five queued in all");
  // A run with nothing queued reports nothing.
  SeedBook empty;
  empty.Begin(2, 0, t0);
  Check(!empty.TakeDone(r), "no seeds queued: no line");
}

void TestBookFreed() {
  SeedBook book;
  const Clock::time_point t0 = Clock::now();
  const uint32_t gen = book.Begin(2, 4, t0);
  for (int i = 0; i < 4; ++i) {
    book.Finished(gen, true, t0);
  }
  book.Taken(gen);
  CheckNum(book.Live(), 3, "three made and unused are live");
  book.Freed(gen, 3);
  CheckNum(book.Live(), 0, "freed: nothing live");
  CheckNum(book.Snapshot().freed, 3, "freed counted");
}

void TestBookThreads() {
  // The workers report from their threads while the render thread reads: the counts add up.
  SeedBook book;
  const Clock::time_point t0 = Clock::now();
  constexpr uint32_t kSeeds = 800;
  const uint32_t gen = book.Begin(4, kSeeds, t0);
  std::atomic<uint32_t> next{0};
  std::vector<std::thread> workers;
  for (int w = 0; w < 4; ++w) {
    workers.emplace_back([&] {
      for (;;) {
        const uint32_t i = next.fetch_add(1);
        if (i >= kSeeds) {
          return;
        }
        book.Finished(gen, (i % 10) != 0, Clock::now());
      }
    });
  }
  uint32_t polls = 0;
  SeedBook::Report r;
  while (!book.TakeDone(r)) {
    book.Live();
    book.Outstanding();
    ++polls;
    std::this_thread::yield();
  }
  for (std::thread& t : workers) {
    t.join();
  }
  CheckNum(r.ready, 720, "threads: ready");
  CheckNum(r.failed, 80, "threads: failed");
  CheckNum(r.queued, kSeeds, "threads: queued");
  (void)polls;
}

}  // namespace

int main() {
  TestAtLevel();
  TestPlanBasics();
  TestPlanSkips();
  TestPlanDuplicatesAndGroups();
  TestPlanBig();
  TestAllowedCount();
  TestShortOfRoom();
  TestDecide();
  TestSlots();
  TestMayStart();
  TestBookRun();
  TestBookStaleEvents();
  TestBookAdd();
  TestBookFreed();
  TestBookThreads();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

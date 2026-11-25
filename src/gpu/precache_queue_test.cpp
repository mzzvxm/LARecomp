// Check of the precache queue's rules (precache_queue.h: the order of the queue the workers make from, and the
// windows the render thread's claims put in front of the cursor). A program of its own: it needs no renderer, no
// SDK and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_precache_queue_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_precache_queue_test
//
// The records are stand-ins for Pipelines::Record: an id and the sample count, which is all the order reads.
//
// The replays at the end (a discrete model of the workers and the render thread) are NOT a measurement: they
// show that the claim sequence of STARTER-ROUND3's sr3v-boot-4 (the first claim at entry 1789, then the world's
// entries 1347 to 1390 while the cursor of three workers was at 1347) cost the old cursor 45 claims and costs
// this one a handful. The numbers of the real run are in the commit message; the model's constants (an entry
// takes 8.7 ms, a claim 14.6 ms of the render thread) are that run's.

#include <compare>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "precache_queue.h"

using namespace mcla::gpu::precache_queue;

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

struct Key {
  uint32_t id = 0;
  uint32_t sample_count = 1;
};
struct Rec {
  Key key;
};

Rec R(uint32_t id, uint32_t samples) {
  return Rec{Key{id, samples}};
}

std::vector<uint32_t> Ids(const std::vector<const Rec*>& queue) {
  std::vector<uint32_t> ids;
  for (const Rec* r : queue) {
    ids.push_back(r->key.id);
  }
  return ids;
}

using V = std::vector<uint32_t>;

struct VariantKey {
  uint32_t ps = 1;
  uint32_t vs_variant = 1;
  uint32_t ps_variant = 0;
  uint32_t sample_count = 2;
  auto operator<=>(const VariantKey&) const = default;
};

struct VariantRecord {
  VariantKey key;
  uint32_t layout = 17;
  bool operator==(const VariantRecord&) const = default;
};

void TestStartupVariants() {
  constexpr VariantRecord base = {};
  constexpr VariantRecord paint = {{1, 1, 0x40, 2}, 17};
  const auto select = [](VariantKey& key) { key.ps_variant |= 0x40; };
  const auto off = [](VariantKey&) {};
  std::vector<VariantRecord> copies;
  V sources;
  const std::set known = {base.key};
  const std::vector records = {base};

  CheckNum(MakeCopies(records, known, {2}, select, copies, sources), 1, "one missing paint variant");
  Check(copies == std::vector{paint} && sources == V{0}, "the variant changes just the shader word");
  const auto queue = Order(records, copies, sources, 0);
  Check(queue.size() == 2 && queue[0] == records.data() && queue[1] == copies.data(),
        "the worker queue contains the paint variant beside its base");

  CheckNum(MakeCopies(records, known, {2}, off, copies, sources), 0, "no variant with the setting off");
  Check(copies.empty() && sources.empty(), "the worker queue has just the base with the setting off");
  MakeCopies(records, known, {2}, [](VariantKey& key) { key.vs_variant |= 0x2000; }, copies, sources);
  Check(copies.empty(), "a pixel variant must be selected before queuing a paired vertex word");
  MakeCopies(records, known, {4}, off, copies, sources);
  Check(copies == std::vector{VariantRecord{{1, 1, 0, 4}, 17}}, "MSAA copies are queued with the variant off");

  const std::set present = {base.key, paint.key};
  const std::vector with_paint = {base, paint};
  CheckNum(MakeCopies(with_paint, present, {4, 4, 0}, select, copies, sources), 0,
           "an existing variant is not added twice");
  Check(copies == std::vector{VariantRecord{{1, 1, 0, 4}, 17}, VariantRecord{{1, 1, 0x40, 4}, 17}} &&
            sources == V{0, 0}, "the converted base and variant are each queued once");
  MakeCopies(with_paint, present, {4}, off, copies, sources);
  Check(copies == std::vector{VariantRecord{{1, 1, 0, 4}, 17}, VariantRecord{{1, 1, 0x40, 4}, 17}} &&
            sources == V{0, 1}, "recorded variants retain their MSAA copies when the setting is off");

  constexpr VariantRecord alpha = {{2, 1, 1, 4}, 19};
  constexpr VariantRecord shadow = {{0, 0, 0, 1}, 20};
  constexpr VariantRecord packed_vertex = {{3, 0x2000, 0, 1}, 21};
  constexpr VariantRecord packed_pixel = {{4, 0, 0x100, 1}, 22};
  constexpr VariantRecord single_sample = {{5, 0, 0, 1}, 23};
  const std::vector mixed = {alpha, shadow, packed_vertex, packed_pixel, single_sample};
  const std::set mixed_keys = {alpha.key, shadow.key, packed_vertex.key, packed_pixel.key, single_sample.key};
  uint32_t selected = 0;
  MakeCopies(mixed, mixed_keys, {2}, [&](VariantKey& key) {
    ++selected;
    if (key.ps == alpha.key.ps) {
      key.ps_variant |= 0x40;
      key.vs_variant |= 0x2000;
    }
  }, copies, sources);
  CheckNum(selected, 2, "only base draws with a pixel shader go to the selector");
  Check(copies == std::vector{VariantRecord{{2, 1, 1, 2}, 19}, VariantRecord{{2, 0x2001, 0x41, 4}, 19},
                             VariantRecord{{2, 0x2001, 0x41, 2}, 19}} && sources == V{0, 0, 0},
        "the paired packed words and both spec bits reach the original and startup sample counts");

  MakeCopies(std::vector{single_sample}, std::set{single_sample.key}, {4}, select, copies, sources);
  Check(copies == std::vector{VariantRecord{{5, 0, 0x40, 1}, 23}}, "a single-sample variant stays single-sample");
}

// ---- DeferTarget --------------------------------------------------------------------------------------

void TestDeferTarget() {
  CheckNum(DeferTarget(false, {}, 2), 0, "no sample counts (no list): no order change");
  CheckNum(DeferTarget(false, {2}, 2), 0, "the default MSAA start keeps the list's order");
  CheckNum(DeferTarget(false, {1}, 2), 1, "MSAA off: ordered by 1");
  CheckNum(DeferTarget(false, {4}, 2), 4, "MSAA 4x: ordered by 4");
  CheckNum(DeferTarget(false, {2, 1}, 2), 1, "an upscaler at the default level: the last count (1) orders it");
  CheckNum(DeferTarget(false, {4, 1}, 2), 1, "an upscaler at 4x: the last count orders it");
  CheckNum(DeferTarget(true, {1}, 2), 0, "the command line prebuild makes the whole list: no order change");
  CheckNum(DeferTarget(true, {4}, 2), 0, "... at any level");
}

// ---- Order --------------------------------------------------------------------------------------------

void TestOrderDefault() {
  // a (2 samples), b (4), c (1), d (4); the copies at 2 samples of b and d follow them.
  const std::vector<Rec> records = {R(1, 2), R(2, 4), R(3, 1), R(4, 4)};
  const std::vector<Rec> copies = {R(12, 2), R(14, 2)};
  const std::vector<uint32_t> source = {1, 3};
  Check(Ids(Order(records, copies, source, 0)) == V({1, 2, 12, 3, 4, 14}),
        "default order: the list's order, each copy right after its record");
  Check(Ids(Order(records, std::vector<Rec>{}, std::vector<uint32_t>{}, 0)) == V({1, 2, 3, 4}),
        "no copies: the list");
  Check(Ids(Order(std::vector<Rec>{}, std::vector<Rec>{}, std::vector<uint32_t>{}, 0)).empty(), "empty list");
  // Two copies of one record (two sample counts), and a copy of the last record.
  const std::vector<Rec> records2 = {R(1, 2), R(2, 4)};
  const std::vector<Rec> copies2 = {R(11, 1), R(12, 1), R(13, 1)};
  Check(Ids(Order(records2, copies2, V({0, 0, 1}), 0)) == V({1, 11, 12, 2, 13}),
        "two copies of a record come right after it, in their order");
}

void TestOrderDeferred() {
  // MSAA off (target 1): the copies at 1 stay in place; the entries with more than one sample (any other
  // count) go after all the others in their old order.
  const std::vector<Rec> records = {R(1, 2), R(2, 4), R(3, 1), R(4, 2)};
  const std::vector<Rec> copies = {R(11, 1), R(12, 1), R(14, 1)};
  Check(Ids(Order(records, copies, V({0, 1, 3}), 1)) == V({11, 12, 3, 14, 1, 2, 4}),
        "MSAA off: the 1-sample entries first, the 2x and 4x ones after them in their old order");
  // MSAA 4x (target 4): a 2-sample record is not an image of this start (it goes last); a 4-sample one stays.
  const std::vector<Rec> copies4 = {R(21, 4), R(24, 4)};
  Check(Ids(Order(records, copies4, V({0, 3}), 4)) == V({21, 2, 3, 24, 1, 4}),
        "MSAA 4x: the 2x entries go after the 4x and the single-sample ones");
  // The default (0): the same lists, nothing moved.
  Check(Ids(Order(records, copies4, V({0, 3}), 0)) == V({1, 21, 2, 3, 4, 24}), "no target: nothing moved");
}

void TestOrderPermutation() {
  // Any lists and targets: every entry once, the entries that stay in their relative order, the ones that go
  // after in theirs.
  std::mt19937 rng(7);
  for (int round = 0; round < 200; ++round) {
    const size_t n = rng() % 40;
    std::vector<Rec> records;
    std::vector<Rec> copies;
    std::vector<uint32_t> source;
    for (size_t i = 0; i < n; ++i) {
      records.push_back(R(uint32_t(i), 1 + (rng() % 3 == 0 ? 3 : rng() % 2)));  // 1, 2 or 4 samples
      for (uint32_t c = rng() % 3 == 0 ? 1 : 0; c > 0; --c) {
        copies.push_back(R(uint32_t(1000 + copies.size()), 1 + rng() % 4));
        source.push_back(uint32_t(i));
      }
    }
    const uint32_t target = (rng() % 4 == 0) ? 0 : (1u << (rng() % 3));  // 0, 1, 2 or 4
    const std::vector<const Rec*> q = Order(records, copies, source, target);
    CheckNum(q.size(), records.size() + copies.size(), "permutation: every record and copy once (count)");
    std::vector<int> seen(2000, 0);
    for (const Rec* r : q) {
      ++seen[r->key.id];
    }
    bool once = true;
    for (const Rec& r : records) {
      once &= seen[r.key.id] == 1;
    }
    for (const Rec& r : copies) {
      once &= seen[r.key.id] == 1;
    }
    Check(once, "permutation: each id exactly once");
    // Within the entries that stay (and within those that go after), the order of the undeferred queue holds.
    const std::vector<const Rec*> plain = Order(records, copies, source, 0);
    std::vector<uint32_t> stay, later, expect_stay, expect_later;
    for (const Rec* r : q) {
      const bool is_later = target != 0 && r->key.sample_count > 1 && r->key.sample_count != target;
      (is_later ? later : stay).push_back(r->key.id);
    }
    for (const Rec* r : plain) {
      const bool is_later = target != 0 && r->key.sample_count > 1 && r->key.sample_count != target;
      (is_later ? expect_later : expect_stay).push_back(r->key.id);
    }
    Check(stay == expect_stay && later == expect_later, "the two groups keep the list's order");
    // ... and the queue is the stayers then the others.
    std::vector<uint32_t> joined = stay;
    joined.insert(joined.end(), later.begin(), later.end());
    Check(Ids(q) == joined, "the entries that stay come first");
  }
}

// The order as StartPrecache made it before the rules moved into precache_queue.h (08dd0d61), for the comparison: the same
// lists, the same target, the entries added to the queue in place or kept for the end.
std::vector<const Rec*> OldOrder(const std::vector<Rec>& records, const std::vector<Rec>& copies,
                                 const std::vector<uint32_t>& source, bool prebuild, const std::vector<uint32_t>& samples) {
  std::vector<const Rec*> queue;
  const uint32_t target = samples.empty() ? 0u : samples.back();
  const bool defer = !prebuild && target != 0 && target != 2;
  const auto later = [&](const Rec& r) { return defer && r.key.sample_count > 1 && r.key.sample_count != target; };
  std::vector<const Rec*> after;
  size_t next_copy = 0;
  for (size_t i = 0; i < records.size(); ++i) {
    if (later(records[i])) {
      after.push_back(&records[i]);
    } else {
      queue.push_back(&records[i]);
    }
    for (; next_copy < copies.size() && source[next_copy] == i; ++next_copy) {
      if (later(copies[next_copy])) {
        after.push_back(&copies[next_copy]);
      } else {
        queue.push_back(&copies[next_copy]);
      }
    }
  }
  for (const Rec* r : after) {
    queue.push_back(r);
  }
  return queue;
}

void TestOrderIsTheOldOrder() {
  // The default start's queue, the non-default start's (MSAA off, 4x, an upscaler) and the prebuild's are what
  // they were: the same entries in the same places for any list.
  std::mt19937 rng(23);
  const std::vector<std::vector<uint32_t>> starts = {{}, {2}, {1}, {4}, {2, 1}, {4, 1}, {8}};
  for (int round = 0; round < 300; ++round) {
    const size_t n = rng() % 60;
    std::vector<Rec> records;
    std::vector<Rec> copies;
    std::vector<uint32_t> source;
    for (size_t i = 0; i < n; ++i) {
      static const uint32_t counts[] = {1, 1, 2, 2, 2, 4};
      records.push_back(R(uint32_t(i), counts[rng() % 6]));
      for (uint32_t c = rng() % 3 == 0 ? 1 + rng() % 2 : 0; c > 0; --c) {
        copies.push_back(R(uint32_t(1000 + copies.size()), counts[rng() % 6]));
        source.push_back(uint32_t(i));
      }
    }
    for (const std::vector<uint32_t>& samples : starts) {
      for (const bool prebuild : {false, true}) {
        const std::vector<const Rec*> now = Order(records, copies, source, DeferTarget(prebuild, samples, 2));
        const std::vector<const Rec*> before = OldOrder(records, copies, source, prebuild, samples);
        Check(now == before, "the queue is the order StartPrecache made before");
      }
    }
  }
}

// ---- Cursor -------------------------------------------------------------------------------------------

// The entries' states as the workers see them: 0 queued, 1 taken (built, being built or claimed).
struct States {
  explicit States(size_t n) : state(n, 0) {}
  std::vector<uint8_t> state;
  bool Queued(size_t i) const { return state[i] == 0; }
};

// Pops the next entry the way a worker does: the entry is taken at once.
bool Pop(Cursor& c, States& s, size_t& i) {
  if (!c.Next(i, [&](size_t k) { return s.Queued(k); })) {
    return false;
  }
  s.state[i] = 1;
  return true;
}

void TestCursorPlain() {
  Cursor c;
  States s(10);
  c.Reset(10);
  Check(c.HasWork(), "a list of 10 has work");
  s.state[3] = 1;  // one the render thread made (a claim) or a worker has
  s.state[4] = 1;
  V got;
  size_t i;
  while (Pop(c, s, i)) {
    got.push_back(uint32_t(i));
  }
  Check(got == V({0, 1, 2, 5, 6, 7, 8, 9}), "the cursor hands out the queued entries in order, leaving the taken");
  Check(!c.HasWork(), "no work left when Next said no");
  c.Reset(0);
  Check(!c.HasWork() && !Pop(c, s, i), "an empty queue has no work");
}

void TestFollowKeepsTheCursor() {
  Cursor c;
  States s(2539);
  c.Reset(2539);
  size_t i = 0;
  for (int k = 0; k < 1347; ++k) {
    Pop(c, s, i);  // the cursor is at 1347 (sr3v-boot-4, 3887 ms after the start)
  }
  CheckNum(c.next(), 1347, "the cursor is at 1347");
  c.Follow(1789);  // the render thread made entry 1789: a one-sample entry, far ahead of the cursor
  CheckNum(c.next(), 1347, "a claim does not move the cursor");
  CheckNum(c.window_entries(), kFollowWindow, "a claim puts a window of kFollowWindow entries after it");
  Pop(c, s, i);
  CheckNum(i, 1790, "the workers make the entry after the claim first");
  for (size_t k = 1; k < kFollowWindow; ++k) {
    Pop(c, s, i);
  }
  CheckNum(i, 1789 + kFollowWindow, "the window's last entry");
  Pop(c, s, i);
  CheckNum(i, 1347, "after the window the workers go on where the cursor was: nothing was passed over");
  Pop(c, s, i);
  CheckNum(i, 1348, "... and on");
}

void TestFollowAfterAStrayClaim() {
  // The claim that cost the old cursor 44 entries: the first claim at 1789 jumped it and parked 1347 to 1788.
  // Now the claims of the world's entries at 1347 and after get windows too, the newest first.
  Cursor c;
  States s(2539);
  c.Reset(2539);
  size_t i = 0;
  for (int k = 0; k < 1347; ++k) {
    Pop(c, s, i);
  }
  c.Follow(1789);
  c.Follow(1390);  // a claim behind the earlier one: it gets its window, and first
  Pop(c, s, i);
  CheckNum(i, 1391, "the newest claim's window comes first");
  for (size_t k = 1; k < kFollowWindow; ++k) {
    Pop(c, s, i);
  }
  CheckNum(i, 1390 + kFollowWindow, "its window is kFollowWindow entries");
  Pop(c, s, i);
  CheckNum(i, 1790, "then the earlier claim's window");
}

void TestFollowEdges() {
  Cursor c;
  c.Reset(100);
  c.Follow(99);
  CheckNum(c.window_entries(), 0, "a claim at the last entry has nothing after it");
  c.Follow(500);
  CheckNum(c.window_entries(), 0, "a claim beyond the queue changes nothing");
  c.Follow(97);
  CheckNum(c.window_entries(), 2, "a window is cut at the end of the queue");
  c.Reset(100);
  CheckNum(c.window_entries(), 0, "Reset drops the windows");
  CheckNum(c.next(), 0, "Reset puts the cursor back");
  // The windows are limited: more claims than kMaxFollowWindows keep the newest.
  c.Reset(5000);
  for (size_t k = 0; k < kMaxFollowWindows + 5; ++k) {
    c.Follow(100 * (k + 1));
  }
  CheckNum(c.windows().size(), kMaxFollowWindows, "the windows are limited");
  CheckNum(c.windows().front().begin, 100 * (kMaxFollowWindows + 5) + 1, "the newest window is kept, in front");
}

void TestNoEntryIsLost() {
  // Any claims, any pops: every entry comes out exactly once, and the queue ends (nothing parked).
  std::mt19937 rng(11);
  for (int round = 0; round < 100; ++round) {
    const size_t n = 50 + rng() % 600;
    Cursor c;
    States s(n);
    c.Reset(n);
    std::vector<int> got(n, 0);
    size_t popped = 0;
    size_t i;
    for (;;) {
      const uint32_t what = rng() % 5;
      if (what == 0) {
        c.Follow(rng() % (n + 10));  // a claim: one the render thread made (the state is its)
      } else if (what == 1) {
        const size_t k = rng() % n;
        if (s.Queued(k)) {
          s.state[k] = 1;  // the render thread made it: the worker finds it taken
          ++got[k];
          ++popped;
        }
      } else if (Pop(c, s, i)) {
        ++got[i];
        ++popped;
      } else {
        break;
      }
    }
    bool once = true;
    for (size_t k = 0; k < n; ++k) {
      once &= got[k] == 1;
    }
    Check(once && popped == n, "every entry is made once, whatever the claims");
    Check(!c.HasWork(), "the queue is done when Next says so");
  }
}

// ---- the replays --------------------------------------------------------------------------------------

// The cursor as it was before this change, for the comparison: a claim jumped the cursor to the entry after it and
// parked what it passed over until the end of the list; a claim behind the cursor, or within kFollowMinGap of it,
// changed nothing.
class JumpCursor {
 public:
  void Reset(size_t end) {
    next_ = 0;
    end_ = end;
    parked_.clear();
  }
  template <class Queued>
  bool Next(size_t& index, Queued&& queued) {
    for (;;) {
      while (next_ >= end_) {
        if (parked_.empty()) {
          return false;
        }
        next_ = parked_.front().first;
        end_ = parked_.front().second;
        parked_.pop_front();
      }
      const size_t i = next_++;
      if (queued(i)) {
        index = i;
        return true;
      }
    }
  }
  void Follow(size_t claimed) {
    const size_t next = claimed + 1;
    if (next <= next_ || next > end_ || next - next_ < 8) {
      return;
    }
    parked_.push_back({next_, claimed});
    next_ = next;
  }

 private:
  size_t next_ = 0;
  size_t end_ = 0;
  std::deque<std::pair<size_t, size_t>> parked_;
};

// The cursor with no follow at all.
class PlainCursor {
 public:
  void Reset(size_t end) {
    next_ = 0;
    end_ = end;
  }
  template <class Queued>
  bool Next(size_t& index, Queued&& queued) {
    while (next_ < end_) {
      const size_t i = next_++;
      if (queued(i)) {
        index = i;
        return true;
      }
    }
    return false;
  }
  void Follow(size_t) {}

 private:
  size_t next_ = 0;
  size_t end_ = 0;
};

struct Model {
  size_t entries = 2539;       // sr3v-boot-4: 2539 entries (2369 records and 170 copies)
  uint32_t workers = 3;        // boot mode on 4 usable processors
  double entry_ms = 8.7;       // 3 workers made 1347 entries in 3887 ms
  double claim_ms = 14.6;      // 644 ms for 44 claims
  double first_ask_ms = 3887;  // the first claim of the run
  double gap_ms = 1.0;         // the render thread's own time between two asks
};

struct Outcome {
  uint32_t claims = 0;
  uint32_t waits = 0;
};

// The workers make entries from `cursor` (each takes the next as it finishes one); the render thread asks for
// `asks` in order from first_ask_ms on: an entry nobody has begun is a claim (the render thread makes it, 14.6 ms,
// and tells the cursor), one being made is waited for, one made costs nothing.
template <class C>
Outcome Replay(C& cursor, const Model& m, const std::vector<size_t>& asks) {
  std::vector<uint8_t> state(m.entries, 0);  // 0 queued, 1 being made by a worker, 2 made
  std::vector<double> done_at(m.entries, 0.0);
  std::vector<double> free_at(m.workers, 0.0);
  cursor.Reset(m.entries);
  const auto take = [&](size_t i) { return state[i] == 0; };
  const auto advance = [&](double upto) {
    for (;;) {
      size_t w = 0;
      for (size_t k = 1; k < free_at.size(); ++k) {
        if (free_at[k] < free_at[w]) {
          w = k;
        }
      }
      if (free_at[w] > upto) {
        return;
      }
      size_t i;
      if (!cursor.Next(i, take)) {
        free_at[w] = 1e18;
        continue;
      }
      state[i] = 1;
      done_at[i] = free_at[w] + m.entry_ms;
      free_at[w] = done_at[i];
    }
  };
  Outcome out;
  double rt = m.first_ask_ms;
  for (const size_t x : asks) {
    advance(rt);
    if (state[x] == 0) {
      ++out.claims;
      state[x] = 2;
      done_at[x] = rt + m.claim_ms;
      cursor.Follow(x);
      rt += m.claim_ms;
    } else if (state[x] == 1 && done_at[x] > rt) {
      ++out.waits;
      rt = done_at[x];
    }
    rt += m.gap_ms;
  }
  return out;
}

std::vector<size_t> Range(size_t first, size_t last) {  // first..last inclusive
  std::vector<size_t> v;
  for (size_t i = first; i <= last; ++i) {
    v.push_back(i);
  }
  return v;
}

std::vector<size_t> Cat(std::vector<size_t> a, const std::vector<size_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

void TestReplayBoot4() {
  const Model m;
  // The run: the first claim is entry 1789 (the first one-sample entry after the world's block), then the render
  // thread asks the world's entries from the cursor's place (about 1347) up to 1390, the last claim.
  const std::vector<size_t> asks = Cat({1789}, Range(1347, 1390));
  JumpCursor old_cursor;
  Cursor cursor;
  PlainCursor plain;
  const Outcome before = Replay(old_cursor, m, asks);
  const Outcome after = Replay(cursor, m, asks);
  const Outcome none = Replay(plain, m, asks);
  std::printf("replay boot-4: claims before %u, after %u, with no follow %u (waits %u, %u, %u)\n", before.claims,
              after.claims, none.claims, before.waits, after.waits, none.waits);
  Check(before.claims >= 40, "the model reproduces the old cursor's claims of the run (44 seen, 45 modelled)");
  Check(after.claims <= 6, "with windows the render thread makes a handful (the first stray claim and the entries the workers had not reached)");
  Check(after.claims < before.claims && after.claims <= none.claims, "windows are better than the jump and than no follow");

  // The stray claims are two (a 1789 and a 2100), or a whole block of them (1789 to 1836).
  const Outcome two_before = [&] { JumpCursor j; return Replay(j, m, Cat({1789, 2100}, Range(1347, 1420))); }();
  const Outcome two_after = [&] { Cursor k; return Replay(k, m, Cat({1789, 2100}, Range(1347, 1420))); }();
  Check(two_after.claims <= 6 && two_after.claims < two_before.claims, "two stray claims, then the world: a handful");
  const Outcome ui_before = [&] { JumpCursor j; return Replay(j, m, Cat(Range(1789, 1836), Range(1347, 1390))); }();
  const Outcome ui_after = [&] { Cursor k; return Replay(k, m, Cat(Range(1789, 1836), Range(1347, 1390))); }();
  Check(ui_after.claims <= 6 && ui_after.claims < ui_before.claims, "the one-sample block, then the world: a handful");
}

void TestReplayKeepsTheGains() {
  // 6 workers (boot mode on 8 usable processors, 0 claims in round 3): the same asks, no claim with either cursor.
  Model m;
  m.workers = 6;
  m.entry_ms = 8.0;
  const std::vector<size_t> asks = Cat({1789}, Range(1347, 1390));
  Cursor cursor;
  JumpCursor old_cursor;
  CheckNum(Replay(cursor, m, asks).claims, 0, "6 workers: no claim with windows");
  CheckNum(Replay(old_cursor, m, asks).claims, 0, "6 workers: none before either (the gain is kept)");
  // A burst the render thread asks in the list's order from near the cursor, the design case of the jump: a
  // few claims at most (the old cursor had one).
  const Model m4;
  Cursor c4;
  JumpCursor o4;
  const std::vector<size_t> burst = Range(1347 + 250, 1347 + 609);
  const Outcome a = Replay(c4, m4, burst);
  const Outcome b = Replay(o4, m4, burst);
  std::printf("replay title burst: claims before %u, after %u\n", b.claims, a.claims);
  Check(a.claims <= 6, "a 360-entry burst in list order ahead of the cursor: a handful of claims");
}

}  // namespace

int main() {
  TestStartupVariants();
  TestDeferTarget();
  TestOrderDefault();
  TestOrderDeferred();
  TestOrderPermutation();
  TestOrderIsTheOldOrder();
  TestCursorPlain();
  TestFollowKeepsTheCursor();
  TestFollowAfterAStrayClaim();
  TestFollowEdges();
  TestNoEntryIsLost();
  TestReplayBoot4();
  TestReplayKeepsTheGains();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// Check of the video memory budget of a GPU that shares memory with the CPU (uma_budget.h). A
// program of its own: it needs no renderer, no Plume and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_uma_budget_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_uma_budget_test
//
// The inputs are what the real adapters list: a Steam Deck's RADV (the two fake heaps of Mesa's
// radv_physical_device.c, with the budget formula of radv_get_memory_budget_properties), the 2-CU iGPU
// of the test PC on Windows (Vulkan heaps and D3D12's LOCAL segment, both from the logs of
// ports\run\vk6-rad-*), a discrete RTX card, and the corners (a driver that lists only the carve-out,
// a small RAM, an unknown RAM, heaps that report no budget). The old rule is written out here again
// (LegacyBiggest) so that "a discrete card is unchanged" is a comparison and not a promise.

#include <cstdint>
#include <cstdio>
#include <string>

#include "uma_budget.h"

using namespace mcla::gpu::uma_budget;

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

bool Has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

void CheckText(const std::string& text, const std::string& part, const std::string& what) {
  ++g_checks;
  if (!Has(text, part)) {
    ++g_failed;
    std::printf("FAILED: %s: '%s' is not in '%s'\n", what.c_str(), part.c_str(), text.c_str());
  }
}

constexpr uint64_t M(uint64_t mib) {
  return mib * kMiB;
}

Heap H(uint64_t size_mib, uint64_t budget_mib, uint64_t usage_mib, bool device_local, bool host_visible) {
  Heap h;
  h.size = M(size_mib);
  h.budget = M(budget_mib);
  h.usage = M(usage_mib);
  h.device_local = device_local;
  h.host_visible = host_visible;
  return h;
}

// The rule before this file, copied from backend_vulkan.cpp as it was (c6bdd23): the biggest
// DEVICE_LOCAL heap; nothing when there is none or it has no budget.
struct Legacy {
  uint64_t used = 0;
  uint64_t budget = 0;
};
Legacy LegacyBiggest(const Heap* heaps, uint32_t count) {
  uint32_t heap = UINT32_MAX;
  uint64_t biggest = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (heaps[i].device_local && heaps[i].size > biggest) {
      heap = i;
      biggest = heaps[i].size;
    }
  }
  if (heap == UINT32_MAX || heaps[heap].budget == 0) {
    return {};
  }
  return {heaps[heap].usage, heaps[heap].budget};
}

// The response's trigger (vram_budget.cpp BeginFrame): the use at kHighPercent (90) of the budget.
bool Short(uint64_t used, uint64_t budget) {
  return budget != 0 && used * 100 >= budget * 90;
}

constexpr uint64_t kRam16 = M(16384);
constexpr uint64_t kRam32 = M(31860);  // the test PC: 31.1 GiB usable

// A Steam Deck (16 GB; BIOS carve-out 1 GiB; GTT 8 GiB [M]) on Mesa's RADV, non-unified: heap 0 is
// the host-only third (GTT side), heap 1 the device-local two thirds (radv_physical_device.c: total =
// GTT + carve-out = 9216 MiB, 2/3 = 6144 as VRAM, 1/3 = 3072 as GTT). The budgets follow
// radv_get_memory_budget_properties for a process using 800 MiB in the VRAM heap and 40 in the GTT
// heap while the system as a whole uses 2000 MiB: free = 9216 - 2000 = 7216; VRAM free =
// min(2/3 * 7216, 6144 - 800) = 4810, so its budget is 4810 + 800 = 5610; GTT free = 7216 - 4810 =
// 2406, budget 2406 + 40 = 2446.
void DeckHeaps(Heap* out) {
  out[0] = H(3072, 2446, 40, false, true);
  out[1] = H(6144, 5610, 800, true, true);
}

void CheckDiscrete() {
  // An RTX card: 16 GB of its own, a 256 MiB window of it the CPU can map, system memory as a host heap.
  Heap rtx[3] = {H(16303, 15500, 700, true, false), H(31000, 25000, 120, false, true),
                 H(256, 200, 0, true, true)};
  const Result r = Compute(rtx, 3, false, kRam32);
  CheckNum(r.budget, M(15500), "discrete: the budget is the biggest device-local heap's");
  CheckNum(r.used, M(700), "discrete: the use is that heap's");
  Check(r.rule == Rule::kBiggestHeap && r.heap_mask == 1 && r.heaps == 1, "discrete: one heap, the first");
  Check(!r.capped && !r.floored, "discrete: neither cap nor floor");
  // A discrete card with a budget below the floor is NOT raised, and one above the RAM cap is not capped.
  Heap small[2] = {H(4096, 1500, 100, true, false), H(8000, 7000, 0, false, true)};
  CheckNum(Compute(small, 2, false, kRam16).budget, M(1500), "discrete: a 1500 MiB budget is not floored");
  Heap huge[1] = {H(65536, 60000, 0, true, false)};
  CheckNum(Compute(huge, 1, false, M(16384)).budget, M(60000), "discrete: no RAM cap");

  // The old rule and the new one on discrete inputs of every shape: the same answer.
  struct Case {
    Heap heap[4];
    uint32_t count;
  } cases[] = {
      {{H(16303, 15500, 700, true, false), H(31000, 25000, 120, false, true), H(256, 200, 0, true, true)}, 3},
      {{H(8192, 7900, 10, true, false)}, 1},
      {{H(8192, 0, 10, true, false), H(8000, 7000, 0, false, true)}, 2},   // no budget reported
      {{H(8000, 7000, 0, false, true)}, 1},                                // no device-local heap
      {{H(4096, 4000, 5, true, false), H(4096, 3900, 6, true, false)}, 2},  // two of the same size: the first
      {{H(256, 200, 1, true, true), H(12288, 11000, 2000, true, false)}, 2},
      {{}, 0},
  };
  int n = 0;
  for (const Case& c : cases) {
    const Legacy old = LegacyBiggest(c.heap, c.count);
    const Result now = Compute(c.heap, c.count, false, kRam32);
    CheckNum(now.budget, old.budget, "discrete case " + std::to_string(n) + ": budget as before");
    CheckNum(now.used, old.used, "discrete case " + std::to_string(n) + ": use as before");
    ++n;
  }
  // Nothing reported when the card gives no budget.
  CheckNum(Compute(cases[2].heap, 2, false, kRam32).budget, 0, "discrete, no budget: nothing reported");
  Check(!Compute(cases[3].heap, 1, false, kRam32).reported(), "discrete, no device-local heap: nothing reported");
}

void CheckDeck() {
  Heap deck[2];
  DeckHeaps(deck);
  // What the old rule read on this Deck: the device-local heap only, two thirds of what the GPU can reach.
  const Legacy old = LegacyBiggest(deck, 2);
  CheckNum(old.budget, M(5610), "deck: the old reading is the device-local heap's budget (not the 1 GiB carve-out)");

  const Result r = Compute(deck, 2, true, kRam16);
  CheckNum(r.budget, M(8056), "deck: the budget is both heaps' (free 7216 + the process's 840)");
  CheckNum(r.used, M(840), "deck: the use is the process's in both heaps");
  Check(r.rule == Rule::kSharedHeaps && r.heaps == 2 && r.heap_mask == 3, "deck: both heaps counted");
  Check(!r.capped && !r.floored, "deck: inside the cap (12288 MiB) and above the floor");
  CheckNum(r.raw_budget, M(8056), "deck: raw budget recorded");

  // The loop DECK1-NOTES R4 feared: the response starts at 90% of the budget. A carve-out-only budget of
  // 1 GiB would trim a 1.5 GiB session (4x MSAA targets) over and over; the sum and even the old reading do not.
  Check(Short(M(1500), M(1024)), "R4: a 1 GiB budget is short at 1.5 GiB in use");
  Check(!Short(M(1500), old.budget), "R4: the old reading on RADV (5610 MiB) is not short at 1.5 GiB");
  Check(!Short(M(1500), r.budget), "R4: the shared budget is not short at 1.5 GiB");
  Check(!Short(M(840), r.budget), "deck: 840 MiB in use is not short");

  // The unified-heap drirc option: one device-local heap with the whole total.
  Heap unified[1] = {H(9216, 8056, 840, true, true)};
  const Result u = Compute(unified, 1, true, kRam16);
  CheckNum(u.budget, M(8056), "deck, unified heap: the same budget");
  CheckNum(LegacyBiggest(unified, 1).budget, M(8056), "deck, unified heap: the old rule was right there already");
  Check(u.heaps == 1 && u.heap_mask == 1, "deck, unified heap: one heap counted");

  // A 4 GiB carve-out (BIOS option): total = 4096 + 8192 = 12288, 2/3 = 8192, 1/3 = 4096; nothing in use.
  Heap big[2] = {H(4096, 4096, 0, false, true), H(8192, 8192, 0, true, true)};
  const Result b = Compute(big, 2, true, kRam16);
  CheckNum(b.budget, M(12288), "deck, 4 GiB carve-out: 12288 MiB reached");
  Check(!b.capped, "deck, 4 GiB carve-out: 12288 is exactly 75% of 16384, not capped");
  // Whatever the GTT is, the cap stops it at 75% of RAM: GTT 12 GiB -> total 13 GiB -> capped to 12288.
  Heap wide[2] = {H(4437, 4437, 0, false, true), H(8874, 8874, 0, true, true)};  // total 13311
  const Result w = Compute(wide, 2, true, kRam16);
  CheckNum(w.budget, M(12288), "deck, GTT 3/4 of RAM: capped to 75% of RAM");
  Check(w.capped && !w.floored, "deck, GTT 3/4 of RAM: marked capped");
  CheckNum(w.raw_budget, M(13311), "deck, capped: the raw sum is kept for the log");
}

void CheckCarveOutOnly() {
  // A driver that lists only the carve-out as device-local and nothing else: the old reading is the
  // carve-out; the shared rule raises it to the floor (and the response still triggers on a failed creation).
  Heap only[1] = {H(1024, 1000, 800, true, true)};
  CheckNum(LegacyBiggest(only, 1).budget, M(1000), "carve-out only: the old reading");
  const Result r = Compute(only, 1, true, kRam16);
  CheckNum(r.budget, kFloorBytes, "carve-out only: raised to the floor");
  Check(r.floored && !r.capped, "carve-out only: marked floored");
  CheckNum(r.used, M(800), "carve-out only: the use is not touched");
  Check(!Short(r.used, r.budget), "carve-out only: 800 MiB is not short against the floor");
}

void CheckIgpu() {
  // The test PC's 2-CU iGPU, Vulkan on Windows (ports\run\vk6-rad-vulkan-A1 and G1-VK-IGPU.md): heap 0
  // 5.27 GiB host-only, heap 1 10.54 GiB device-local with a budget of 10251 MiB and 768 MiB in use. The
  // host heap's own budget was not logged: 5200 MiB is a stand-in [M].
  Heap vk[2] = {H(5396, 5200, 0, false, true), H(10791, 10251, 768, true, true)};
  const Result r = Compute(vk, 2, true, kRam32);
  CheckNum(LegacyBiggest(vk, 2).budget, M(10251), "igpu vulkan: the old reading is the logged 10251 MiB");
  CheckNum(r.budget, M(15451), "igpu vulkan: both heaps (10251 + 5200)");
  CheckNum(r.used, M(768), "igpu vulkan: the use");
  Check(!r.capped && !r.floored, "igpu vulkan: inside the cap (23895 MiB of 31860 MiB RAM)");
  CheckNum(RamCap(kRam32), M(23895), "the cap of the test PC's RAM");

  // The same adapter on D3D12: LOCAL budget 15648 MiB (ports\run\vk6-rad-d3d12-A1), 639 MiB in use. One
  // pool: unchanged.
  const Result d = Segment(M(639), M(15648), kRam32);
  CheckNum(d.budget, M(15648), "igpu d3d12: LOCAL budget unchanged");
  CheckNum(d.used, M(639), "igpu d3d12: use unchanged");
  Check(d.rule == Rule::kSegment && !d.capped && !d.floored, "igpu d3d12: not capped, not floored");
  // The two APIs read about the same memory: within 3% of each other.
  Check(r.budget * 100 < d.budget * 103 && d.budget * 100 < r.budget * 103,
        "the iGPU's Vulkan and D3D12 budgets agree within 3%");
  Check(!Short(M(768), r.budget) && !Short(M(768), d.budget), "igpu: 768 MiB in use is far from short");
  // D3D12: no budget reported -> nothing.
  Check(!Segment(0, 0, kRam32).reported(), "d3d12: LOCAL budget 0 is not reported");
}

void CheckCapAndFloor() {
  // A small laptop: 8 GiB of RAM, heaps summing to 8100 MiB -> capped to 75% = 6144.
  Heap lap[2] = {H(2731, 2700, 100, false, true), H(5461, 5400, 900, true, true)};
  const Result r = Compute(lap, 2, true, M(8192));
  CheckNum(r.budget, M(6144), "8 GiB laptop: capped to 75% of RAM");
  Check(r.capped, "8 GiB laptop: marked capped");
  // 2 GiB of RAM: the cap (1536) is below the floor (2048): the cap wins.
  Heap tiny[1] = {H(1500, 1500, 0, true, true)};
  const Result t = Compute(tiny, 1, true, M(2048));
  CheckNum(t.budget, M(1536), "2 GiB machine: the floor never passes the cap");
  Check(t.floored && !t.capped, "2 GiB machine: 1500 raised to the cap-limited floor");
  Heap tiny2[1] = {H(3000, 2500, 0, true, true)};
  const Result t2 = Compute(tiny2, 1, true, M(2048));
  CheckNum(t2.budget, M(1536), "2 GiB machine: 2500 capped to 1536");
  Check(t2.capped && !t2.floored, "2 GiB machine: marked capped");
  // Unknown RAM (0): no cap, the floor still holds.
  Heap big[1] = {H(30000, 20000, 0, true, true)};
  const Result u = Compute(big, 1, true, 0);
  CheckNum(u.budget, M(20000), "unknown RAM: no cap");
  Check(!u.capped, "unknown RAM: not marked capped");
  Heap low[1] = {H(512, 500, 0, true, true)};
  CheckNum(Compute(low, 1, true, 0).budget, kFloorBytes, "unknown RAM: the floor holds");
  CheckNum(RamCap(0), UINT64_MAX, "RamCap(unknown) is no cap");
  CheckNum(Floor(0), kFloorBytes, "Floor(unknown) is the floor");

  // Sweep: for any RAM and any sum the budget is at most the cap and at least the floor, and a budget
  // already inside the two is returned as it is.
  const uint64_t rams[] = {0, M(1024), M(2048), M(4096), M(8192), M(16384), M(32768), M(65536)};
  const uint64_t sums[] = {M(1), M(500), M(1024), M(2048), M(3000), M(6000), M(9000), M(15000), M(40000)};
  for (uint64_t ram : rams) {
    for (uint64_t sum : sums) {
      Heap one[1] = {Heap{}};
      one[0].size = sum;
      one[0].budget = sum;
      one[0].device_local = true;
      const Result s = Compute(one, 1, true, ram);
      const std::string at = "ram " + std::to_string(ram / kMiB) + " MiB, sum " + std::to_string(sum / kMiB);
      Check(s.budget <= RamCap(ram), at + ": at most the cap");
      Check(s.budget >= Floor(ram), at + ": at least the floor");
      if (sum <= RamCap(ram) && sum >= Floor(ram)) {
        CheckNum(s.budget, sum, at + ": inside the limits is untouched");
      }
      CheckNum(s.raw_budget, sum, at + ": raw is the sum");
    }
  }
}

void CheckHeapSelection() {
  // A heap that reports no budget is left out; the others still count.
  Heap partial[2] = {H(3072, 0, 0, false, true), H(6144, 5000, 700, true, true)};
  const Result r = Compute(partial, 2, true, kRam16);
  CheckNum(r.budget, M(5000), "a heap with no budget is left out of the sum");
  Check(r.heaps == 1 && r.heap_mask == 2, "only heap 1 counted");
  // Every counted heap without a budget: nothing reported.
  Heap none[2] = {H(3072, 0, 0, false, true), H(6144, 0, 0, true, true)};
  Check(!Compute(none, 2, true, kRam16).reported(), "no heap reports a budget: not reported");
  Check(Compute(none, 2, true, kRam16).rule == Rule::kNone, "...and the rule says none");
  // A heap the GPU cannot reach (neither device-local nor mappable) is not counted.
  Heap hidden[3] = {H(6144, 5000, 700, true, true), H(1000, 900, 50, false, false), H(3072, 2400, 10, false, true)};
  const Result h = Compute(hidden, 3, true, kRam16);
  CheckNum(h.budget, M(7400), "a heap that is neither device-local nor host-visible is skipped");
  Check(h.heap_mask == 5, "heaps 0 and 2 counted");
  // No heaps at all.
  Check(!Compute(nullptr, 0, true, kRam16).reported(), "no heaps: not reported (shared)");
  Check(!Compute(nullptr, 0, false, kRam16).reported(), "no heaps: not reported (discrete)");
  // More heaps than Vulkan allows are ignored, not read past.
  Heap many[20];
  for (int i = 0; i < 20; ++i) {
    many[i] = H(100, 100, 1, true, true);
  }
  CheckNum(Compute(many, 20, true, 0).heaps, kMaxHeaps, "at most kMaxHeaps heaps are read");
}

void CheckShared() {
  Check(IsSharedMemory(true, false, false), "uma -> shared");
  Check(IsSharedMemory(false, true, false), "integrated type -> shared (Vulkan)");
  Check(IsSharedMemory(false, false, true), "CPU rasteriser -> shared");
  Check(IsSharedMemory(true, true, false), "uma and integrated -> shared");
  Check(!IsSharedMemory(false, false, false), "discrete -> not shared");
}

void CheckDescribe() {
  Heap deck[2];
  DeckHeaps(deck);
  const std::string d = Describe(Compute(deck, 2, true, kRam16), deck, 2, kRam16);
  CheckText(d, "shared-memory adapter", "deck text: kind");
  CheckText(d, "2 heaps counted", "deck text: how many");
  CheckText(d, "heap 0 host 2446 MiB", "deck text: the host heap");
  CheckText(d, "heap 1 device-local 5610 MiB", "deck text: the device-local heap");
  CheckText(d, "= 8056 MiB", "deck text: the sum");
  CheckText(d, "within 75% of the 16384 MiB of RAM", "deck text: inside the cap");

  Heap lap[2] = {H(2731, 2700, 100, false, true), H(5461, 5400, 900, true, true)};
  const std::string c = Describe(Compute(lap, 2, true, M(8192)), lap, 2, M(8192));
  CheckText(c, "held to 75% of the 8192 MiB of RAM = 6144 MiB", "capped text");

  Heap low[1] = {H(1024, 1000, 800, true, true)};
  const std::string f = Describe(Compute(low, 1, true, kRam16), low, 1, kRam16);
  CheckText(f, "raised to the floor, 2048 MiB", "floored text");
  CheckText(Describe(Compute(low, 1, true, 0), low, 1, 0), "the RAM is unknown, no cap", "unknown RAM text");

  Heap partial[2] = {H(3072, 0, 0, false, true), H(6144, 5000, 700, true, true)};
  CheckText(Describe(Compute(partial, 2, true, kRam16), partial, 2, kRam16),
            "heap 0 reports no budget, left out", "a heap left out is said");

  Heap rtx[2] = {H(16303, 15500, 700, true, false), H(31000, 25000, 120, false, true)};
  CheckText(Describe(Compute(rtx, 2, false, kRam32), rtx, 2, kRam32),
            "own memory: the biggest device-local heap, heap 0, 15500 MiB", "discrete text");

  CheckText(Describe(Segment(M(639), M(15648), kRam32), nullptr, 0, kRam32, "NON_LOCAL budget 0 MiB"),
            "the LOCAL segment, 15648 MiB (NON_LOCAL budget 0 MiB)", "segment text");
  CheckText(Describe(Result{}, nullptr, 0, 0), "reports none", "nothing-reported text");
}

}  // namespace

int main() {
  CheckDiscrete();
  CheckDeck();
  CheckCarveOutOnly();
  CheckIgpu();
  CheckCapAndFloor();
  CheckHeapSelection();
  CheckShared();
  CheckDescribe();
  if (g_failed != 0) {
    std::printf("uma_budget_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("uma_budget_test: %d checks passed\n", g_checks);
  return 0;
}

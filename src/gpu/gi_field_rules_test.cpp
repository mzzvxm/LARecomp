// Check of the probe field owner's rules (gi_field_rules.h and hlsl/gi_field_common.hlsli): CITY-RT M3 package G1. A program of its own: it needs no renderer, no game files and no GPU,
// and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_gi_field_rules_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/gi_field_rules_test.cpp
//   run: larecomp_gi_field_rules_test [-v] [<hlsl dir>]   (without the path the shaders are found from this file's own place: hlsl/; -v prints the tables)
//
// It checks: the memory of every (ray tracing tier, level) cell (the exact bytes of the atlases, the weight map and the state buffer, the totals against the design's, the per-pixel
// images the extended layout adds) and the order of loss (the step each amount of room comes to, the steps that wait out a refusal); the slot of a probe and the probe of a slot; the
// windows' toroidal moves against a brute force that plays the GPU's reset (after every move of a random walk, every coordinate of every window is stored in its own slot, no probe is
// cleared twice, the cleared ones are exactly those that entered); the dirty sets as the camera moves (the probes a move exposes, a cut, a relight, a world box, the order of what the
// scheduler takes); the warm-up budget; the fade (its times and its latch); the state word and the weight rule; the reset list; the bindings in the .hlsl files. Then it compiles
// hlsl/gi_field_common.hlsli as C++ (through a shim of the HLSL types) and compares every function with the twin, and the traversal that finds the nearest probe sphere along a ray
// with a brute force over the probes.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gi_field_rules.h"
#include "gi_frame.h"
#include "vram_room.h"
#include "world_clock.h"

namespace gi = mcla::gpu::gi;
namespace gf = mcla::gpu::gi_field;
namespace wc = mcla::gpu::world_clock;

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the file uses, in a namespace of its own so that the standard library's abs, sqrt ... are not in the way
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float floor(float v) { return std::floor(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline int min(int a, int b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline float asfloat(uint v) { return std::bit_cast<float>(v); }
inline int asint(uint v) { return std::bit_cast<int>(v); }

std::vector<uint> g_reset_words;
#define GIF_RESET_WORD(i) (hlsl_shim::g_reset_words[(i)])
#include "hlsl/gi_field_common.hlsli"
#undef GIF_RESET_WORD
}  // namespace hlsl_shim

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_verbose = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  } else if (g_verbose) {
    std::printf("  ok: %s\n", what.c_str());
  }
}

std::string Num(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0) {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer), fmt, a, b, c, d);
  return buffer;
}

struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  explicit Rng(uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ull + 12345) {}
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double Uniform() { return double(Next() >> 11) / double(1ull << 53); }
  double Range(double lo, double hi) { return lo + (hi - lo) * Uniform(); }
  int32_t Int(int32_t lo, int32_t hi) { return lo + int32_t(Next() % uint64_t(hi - lo + 1)); }
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ---------------------------------------------------------------------------
// the memory of the cells and the order of loss
// ---------------------------------------------------------------------------
void TestMemory() {
  const gf::Memory lean = gf::MemoryOf(gi::BudgetOf(gi::Layout::kLean), false);
  const gf::Memory standard = gf::MemoryOf(gi::BudgetOf(gi::Layout::kStandard), false);
  const gf::Memory extended = gf::MemoryOf(gi::BudgetOf(gi::Layout::kExtended), false);
  Check(standard.irradiance == 5120ull * 960 * 8 && standard.moments == 5120ull * 960 * 4 && standard.weight == 512ull * 96 && standard.state == 49152ull * 4,
        "the standard field's bytes: the two atlases, the weight map and the state buffer");
  Check(standard.Field() == 59228160ull, Num("the standard field is %.0f bytes (59,228,160: 56.5 MiB)", double(standard.Field())));
  Check(lean.Field() == 4935680ull && extended.Field() == 78970880ull, Num("the lean field is %.0f bytes and the extended one %.0f", double(lean.Field()), double(extended.Field())));
  Check(standard.irradiance + standard.moments == 3ull * 19660800ull, "the standard atlases are the design's 3 x 19.66 MB");
  Check(standard.state == 196608 && lean.state == 16384 && extended.state == 262144, "the state buffer is 4 bytes a probe");
  // against the design's totals (M3 section 6): the estimates add what G2 and G3 will allocate and stay inside the cell's cost.vram_mib
  for (const gi::Layout layout : {gi::Layout::kLean, gi::Layout::kStandard, gi::Layout::kExtended}) {
    const gi::Budget& b = gi::BudgetOf(layout);
    const gf::Memory m = gf::MemoryOf(b, layout == gi::Layout::kExtended);
    const uint64_t design = uint64_t(b.cost.vram_mib) * gf::kMiB;
    Check(m.Total() <= design && m.Total() + 8 * gf::kMiB > design, Num("%s: the total %.1f MiB is within 8 MiB under the design's %.0f", 0, double(m.Total()) / double(gf::kMiB), double(b.cost.vram_mib)) + " (" + std::string(gi::LayoutName(layout)) + ")");
    Check(m.Field() < design, std::string(gi::LayoutName(layout)) + ": what the owner allocates is under the design's total");
  }
  const gf::Memory ext_full = gf::MemoryOf(gi::BudgetOf(gi::Layout::kExtended), true);
  Check(ext_full.pixel_reserve > 60 * gf::kMiB && ext_full.Total() == 190ull * gf::kMiB, Num("the extended layout with its per-pixel images is the design's 190 MiB (per-pixel images %.1f MiB)", double(ext_full.pixel_reserve) / double(gf::kMiB)));
  Check(gf::MemoryOf(gi::BudgetOf(gi::Layout::kStandard), true).pixel_reserve == 0 && gf::MemoryOf(gi::BudgetOf(gi::Layout::kNone), true).Total() == 0, "only the extended layout has per-pixel images; none holds nothing");

  // the cells
  const auto cells = gf::Cells();
  if (g_verbose) std::printf("  cells (ray tracing tier, level -> layout: field / total MiB):\n");
  for (const gf::Cell& c : cells) {
    if (g_verbose) std::printf("    %-6s %-5s %-8s field %7.2f MiB, total %7.2f MiB\n", gi::RungName(c.rung).data(), gi::LevelName(c.level).data(), gi::LayoutName(c.layout).data(),
                               double(c.memory.Field()) / double(gf::kMiB), double(c.memory.Total()) / double(gf::kMiB));
    Check(c.layout == gi::LayoutFor(c.rung, c.level), std::string("cell ") + gi::RungName(c.rung).data() + "/" + gi::LevelName(c.level).data() + " has the layout of the table");
    if (c.layout == gi::Layout::kNone) {
      Check(c.memory.Total() == 0, "a cell with no field holds nothing");
    } else {
      Check(c.memory.Field() > 0 && c.memory.Total() >= c.memory.Field(), "a cell with a field holds at least its field");
    }
  }
  const auto find = [&](gi::Rung r, gi::Level l) {
    for (const gf::Cell& c : cells)
      if (c.rung == r && c.level == l) return c;
    return gf::Cell{};
  };
  Check(find(gi::Rung::kStrong, gi::Level::kUltra).memory.pixel_reserve > 0 && find(gi::Rung::kWeak, gi::Level::kUltra).memory.pixel_reserve == 0,
        "the per-pixel images are the strong card's ULTRA only");
  Check(find(gi::Rung::kStrong, gi::Level::kHigh).layout == gi::Layout::kStandard && find(gi::Rung::kWeak, gi::Level::kHigh).layout == gi::Layout::kLean &&
            find(gi::Rung::kNone, gi::Level::kUltra).layout == gi::Layout::kNone,
        "the cells' layouts");
}

void TestLadder() {
  using gf::Step;
  // the steps of each layout
  Check(gf::NextStep(gi::Layout::kExtended, Step::kFull) == Step::kNoPixelImages && gf::NextStep(gi::Layout::kExtended, Step::kNoPixelImages) == Step::kNoFourthCascade &&
            gf::NextStep(gi::Layout::kExtended, Step::kNoFourthCascade) == Step::kNoThirdCascade && gf::NextStep(gi::Layout::kExtended, Step::kNoThirdCascade) == Step::kNone,
        "extended: full, no pixel images, no fourth cascade, no third, none (the order of M3 section 6)");
  Check(gf::NextStep(gi::Layout::kStandard, Step::kFull) == Step::kNoThirdCascade && gf::NextStep(gi::Layout::kStandard, Step::kNoThirdCascade) == Step::kNone &&
            gf::NextStep(gi::Layout::kLean, Step::kFull) == Step::kNone && gf::NextStep(gi::Layout::kLean, Step::kNone) == Step::kNone,
        "standard: full, no third cascade, none; lean: full, none");
  // the fit of each step
  const gf::Fit ext = gf::FitAt(gi::Layout::kExtended, Step::kFull);
  Check(ext.On() && ext.budget.cascade_count == 4 && ext.pixel_images && !ext.Reduced() && ext.budget.layout == gi::Layout::kExtended, "the full extended fit: 4 cascades and the per-pixel images");
  const gf::Fit ext_np = gf::FitAt(gi::Layout::kExtended, Step::kNoPixelImages);
  Check(ext_np.budget.cascade_count == 4 && !ext_np.pixel_images && ext_np.Reduced() && ext_np.memory.pixel_reserve == 0, "without the per-pixel images: 4 cascades, none of their memory");
  const gf::Fit ext_3 = gf::FitAt(gi::Layout::kExtended, Step::kNoFourthCascade);
  Check(ext_3.budget.cascade_count == 3 && !ext_3.pixel_images && ext_3.budget.cascade[2].spacing_m == 8.0f && ext_3.budget.cascade[3].dims.Probes() == 0 &&
            gi::AtlasHeight(ext_3.budget) == 960 && ext_3.memory.Field() == gf::MemoryOf(gi::BudgetOf(gi::Layout::kStandard), false).Field(),
        "without the fourth cascade: the first three of the extended budget, the standard field's atlases");
  const gf::Fit ext_2 = gf::FitAt(gi::Layout::kExtended, Step::kNoThirdCascade);
  Check(ext_2.budget.cascade_count == 2 && gi::AtlasHeight(ext_2.budget) == 640 && ext_2.budget.cascade[1].spacing_m == 4.0f, "without the third cascade: 2 and 4 m, 64 m and 128 m across");
  const gf::Fit std_2 = gf::FitAt(gi::Layout::kStandard, Step::kNoThirdCascade);
  Check(std_2.budget.cascade_count == 2 && std_2.memory.Field() == ext_2.memory.Field(), "the standard layout without its third cascade has the same atlases");
  Check(!gf::FitAt(gi::Layout::kNone, Step::kFull).On() && !gf::FitAt(gi::Layout::kStandard, Step::kNone).On() && gf::FitAt(gi::Layout::kNone, Step::kFull).memory.Total() == 0, "none is no field");
  // the totals fall with each step, and the field's memory with each cascade
  uint64_t last = ~0ull;
  bool falling = true;
  for (Step s = Step::kFull; s != Step::kNone; s = gf::NextStep(gi::Layout::kExtended, s)) {
    const uint64_t total = gf::FitAt(gi::Layout::kExtended, s).memory.Total();
    falling = falling && total < last;
    last = total;
  }
  Check(falling, "every step down of the extended layout holds less");
  Check(gf::FitAt(gi::Layout::kExtended, Step::kNoPixelImages).memory.Total() < gf::FitAt(gi::Layout::kExtended, Step::kFull).memory.Total() - 60 * gf::kMiB,
        "dropping the per-pixel images is the big saving (the first thing lost)");

  // ChooseFit against rooms
  const auto fits_up_to = [](uint64_t room) { return [room](uint64_t bytes) { return bytes <= room; }; };
  Check(gf::ChooseFit(gi::Layout::kExtended, fits_up_to(~0ull)).step == Step::kFull, "room for everything: the full layout");
  Check(gf::ChooseFit(gi::Layout::kExtended, fits_up_to(0)).step == Step::kNone && !gf::ChooseFit(gi::Layout::kExtended, fits_up_to(0)).On(), "no room: no field");
  const uint64_t full = ext.memory.Total(), nopx = ext_np.memory.Total(), three = ext_3.memory.Total(), two = ext_2.memory.Total();
  Check(gf::ChooseFit(gi::Layout::kExtended, fits_up_to(full - 1)).step == Step::kNoPixelImages, "a little short: the per-pixel images go first");
  Check(gf::ChooseFit(gi::Layout::kExtended, fits_up_to(nopx - 1)).step == Step::kNoFourthCascade, "shorter: then the fourth cascade");
  Check(gf::ChooseFit(gi::Layout::kExtended, fits_up_to(three - 1)).step == Step::kNoThirdCascade, "shorter: then the third");
  Check(gf::ChooseFit(gi::Layout::kExtended, fits_up_to(two - 1)).step == Step::kNone, "shorter: nothing");
  Check(gf::ChooseFit(gi::Layout::kStandard, fits_up_to(gf::FitAt(gi::Layout::kStandard, Step::kFull).memory.Total() - 1)).step == Step::kNoThirdCascade, "the standard layout loses its third cascade");
  Check(gf::ChooseFit(gi::Layout::kLean, fits_up_to(gf::FitAt(gi::Layout::kLean, Step::kFull).memory.Total() - 1)).step == Step::kNone, "the lean layout has nothing to lose but itself");
  // a step that waits out a refusal is passed over
  const gf::Fit skipped = gf::ChooseFit(gi::Layout::kExtended, fits_up_to(~0ull), [](Step s) { return s == Step::kFull || s == Step::kNoPixelImages; });
  Check(skipped.step == Step::kNoFourthCascade, "steps that wait out a refusal are skipped");
  // the room test of the renderer: the use plus the extra stays under 90% of the budget (vram_room.h)
  const auto card = [](uint64_t budget_mib, uint64_t used_mib, gi::Layout layout) {
    return gf::ChooseFit(layout, [&](uint64_t bytes) { return mcla::gpu::vram_room::HasRoom(used_mib * gf::kMiB, budget_mib * gf::kMiB, bytes); });
  };
  Check(card(16000, 5000, gi::Layout::kExtended).step == Step::kFull && card(0, 0, gi::Layout::kExtended).step == Step::kFull, "a 16 GB card with 5 GB used and an adapter that reports no budget take the full layout");
  const gf::Fit six = card(5800, 5100, gi::Layout::kExtended);
  Check(six.step == Step::kNoPixelImages && six.On() && !six.pixel_images && six.budget.cascade_count == 4,
        Num("a 6 GB card (5,800 MiB budget) with 5.1 GB used keeps the four cascades and loses the per-pixel images (step %.0f)", double(uint32_t(six.step))));
  Check(card(5800, 4200, gi::Layout::kExtended).step == Step::kFull, "the same card with 4.2 GB used has room for all of it");
  if (g_verbose) {
    for (const uint64_t used : {3000ull, 4200ull, 4700ull, 5000ull, 5100ull, 5150ull, 5200ull}) {
      const gf::Fit f = card(5800, used, gi::Layout::kExtended);
      std::printf("    6 GB card (5800 MiB budget), %llu MiB used, ULTRA on a strong card: %s (%u cascades, %.1f MiB)\n", (unsigned long long)used, gf::StepName(f.step), f.budget.cascade_count,
                  double(f.memory.Total()) / double(gf::kMiB));
    }
  }
}

// ---------------------------------------------------------------------------
// the state word and the weight
// ---------------------------------------------------------------------------
void TestStateAndWeight() {
  using namespace gf;
  Check(StateUpdates(MakeState(7, false, false)) == 7 && !StateInside(MakeState(7, false, false)) && StateInside(MakeState(0, true, false)) && StateNew(MakeState(0, false, true)) &&
            StateBack(MakeState(0, false, false, 200)) == 200,
        "the state word's fields");
  Check(StateUpdates(MakeState(1000, false, false)) == 255 && StateBack(MakeState(0, false, false, 999)) == 255, "the fields saturate");
  Check(StateAfterUpdate(kStateAfterReset) == 1 && StateAfterUpdate(MakeState(255, false, false)) == 255 && StateAfterUpdate(MakeState(3, true, true, 50)) == (4u | kStateInside | (50u << kStateBackShift)),
        "an update counts, clears the new bit, keeps the rest");
  Check(kStateAfterReset == kStateNew && StateUpdates(kStateAfterReset) == 0 && !StateInside(kStateAfterReset), "a reset probe is new, never traced, not inside");
  Check(ProbeWeight(kStateAfterReset, true) == 0.0f, "a probe never traced has weight 0");
  Check(ProbeWeight(MakeState(1, false, false), true) == 0.5f && ProbeWeight(MakeState(2, false, false), true) == 1.0f && ProbeWeight(MakeState(200, false, false), true) == 1.0f,
        "the ramp: half after the first update, whole after the second");
  Check(ProbeWeight(MakeState(50, true, false), true) == 0.0f && ProbeWeight(MakeState(50, false, false), false) == 0.0f, "inside geometry and out of range: weight 0 however converged");
  Check(ProbeWeight(MakeState(1, false, true), true) == 0.5f, "a probe marked again (new) that kept its data keeps its weight");
  Check(WeightByte(0.0f) == 0 && WeightByte(1.0f) == 255 && WeightByte(0.5f) == 128, "the weight map's byte");
  const gi::Vec3 cam = {100, 5, -40};
  Check(ProbeInRange({100, 5, -40}, cam, 10) && ProbeInRange({110, 5, -40}, cam, 10) && !ProbeInRange({110.5f, 5, -40}, cam, 10) && !ProbeInRange(cam, cam, 0) && !ProbeInRange(cam, cam, -3),
        "in range: within the safe range of the camera; a safe range of 0 is no field");
  // the weights pass as a map: every probe of a window with scattered states, by slot
  const gi::Budget& b = gi::BudgetOf(gi::Layout::kStandard);
  const double camera[3] = {120.3, 11.0, -55.9};
  gf::FieldState state;
  state.Start(b, camera);
  state.Move(camera);
  Rng rng(3);
  const gi::Dims d = b.cascade[0].dims;
  std::vector<uint32_t> words(d.Probes());
  for (auto& w : words) w = MakeState(uint32_t(rng.Int(0, 4)), rng.Int(0, 9) == 0, false);
  uint32_t zero = 0, off_range = 0, bad = 0;
  const float safe = 20.0f;
  for (uint32_t i = 0; i < d.Probes(); ++i) {
    const gi::Coord c = CoordOfIndex(state.origin(0), d, i);
    const gi::Vec3 world = {float(c.x * b.cascade[0].spacing_m), float(c.y * b.cascade[0].spacing_m), float(c.z * b.cascade[0].spacing_m)};
    const bool in = ProbeInRange(world, {float(camera[0]), float(camera[1]), float(camera[2])}, safe);
    const float w = ProbeWeight(words[i], in);
    zero += w == 0.0f ? 1u : 0u;
    off_range += in ? 0u : 1u;
    // a probe whose weight is positive is traced, outside no geometry and inside the range
    if (w > 0.0f && (!in || StateInside(words[i]) || StateUpdates(words[i]) == 0)) ++bad;
  }
  Check(bad == 0 && off_range > 0 && zero > off_range, Num("a window's weights: %.0f of %.0f probes are 0 (the range cuts %.0f)", double(zero), double(d.Probes()), double(off_range)));
}

// ---------------------------------------------------------------------------
// slots and coordinates
// ---------------------------------------------------------------------------
void TestSlots() {
  Rng rng(5);
  bool ok = true, inverse = true, window = true;
  for (const gi::Layout layout : {gi::Layout::kLean, gi::Layout::kStandard}) {
    const gi::Dims d = gi::BudgetOf(layout).cascade[0].dims;
    for (int trial = 0; trial < 20; ++trial) {
      const gi::Coord origin = {rng.Int(-5000, 5000), rng.Int(-300, 300), rng.Int(-5000, 5000)};
      std::set<std::tuple<int, int, int>> seen;
      for (uint32_t i = 0; i < d.Probes(); ++i) {
        const gi::Coord c = gf::CoordOfIndex(origin, d, i);
        ok = ok && gi::Contains(origin, d, c);
        seen.insert({c.x, c.y, c.z});
        inverse = inverse && gf::IndexOfCoord(c, d) == i && gi::SlotOf(c, d) == gf::SlotOfIndex(i, d);
      }
      window = window && seen.size() == d.Probes();
    }
  }
  Check(ok && inverse && window, "the coordinate of every slot is in the window, the slot of that coordinate is the slot, and the slots cover the window once (negative origins too)");
  Check(gf::SlotOfIndex(gi::ProbeIndex({31, 15, 31}, {32, 16, 32}), {32, 16, 32}) == gi::Slot{31, 15, 31} && gf::CoordOfSlot({-1, 17, 33}, {32, 16, 32}, {31, 1, 1}) == gi::Coord{-1, 17, 33},
        "spot values");
  const gi::Budget& b = gi::BudgetOf(gi::Layout::kExtended);
  const gf::ProbeTable t = gf::TableOf(b);
  Check(t.cascades == 4 && t.Total() == 65536 && t.base[0] == 0 && t.base[1] == 16384 && t.base[3] == 49152 && gf::StateIndex(t, 2, 5) == 32773 && gf::StateBytes(t) == 262144,
        "the state buffer holds the cascades one after the other");
  const gf::ProbeTable none = gf::TableOf(gi::BudgetOf(gi::Layout::kNone));
  Check(none.Total() == 0 && gf::TableOf(gi::BudgetOf(gi::Layout::kLean)).Total() == 4096, "the table of none and of lean");
}

// ---------------------------------------------------------------------------
// the dirty set
// ---------------------------------------------------------------------------
void TestDirtySet() {
  gf::DirtySet s;
  s.Resize(200);
  Check(s.Count() == 0 && s.Size() == 200 && !s.Test(0), "a new set is clean");
  Check(s.Mark(3) && !s.Mark(3) && s.Mark(64) && s.Mark(199) && s.Count() == 3 && !s.Mark(200), "marking counts once and ignores what is out of range");
  std::vector<uint32_t> got;
  s.ForEach([&](uint32_t i) { got.push_back(i); });
  Check(got == std::vector<uint32_t>({3, 64, 199}), "iteration in order");
  Check(s.Clear(64) && !s.Clear(64) && s.Count() == 2 && !s.Test(64) && s.Test(199), "clearing");
  s.MarkAll();
  Check(s.Count() == 200, "mark all");
  s.ClearAll();
  Check(s.Count() == 0, "clear all");
}

// ---------------------------------------------------------------------------
// the windows move: the toroidal invariant against the GPU's reset
// ---------------------------------------------------------------------------
// What the textures hold, by slot: the coordinate whose data is stored there (a slot nothing wrote has none).
struct ShadowField {
  std::array<std::vector<std::array<int32_t, 3>>, gi::kMaxCascades> owner;
  std::array<std::vector<char>, gi::kMaxCascades> set;
  void Start(const gi::Budget& b) {
    for (uint32_t c = 0; c < b.cascade_count; ++c) {
      owner[c].assign(b.cascade[c].dims.Probes(), {0, 0, 0});
      set[c].assign(b.cascade[c].dims.Probes(), 0);
    }
  }
};

// Plays a report's reset list on the shadow field the way the GPU does it (every group of the list is a probe). Returns the probes cleared twice.
uint32_t ApplyReset(ShadowField& shadow, const gi::Budget& b, const gf::MoveReport& r, uint64_t* cleared) {
  const gf::ResetList list = r.Reset();
  uint32_t twice = 0;
  std::set<std::tuple<uint32_t, uint32_t>> done;
  for (uint32_t g = 0; g < list.total; ++g) {
    const gf::ResetProbe p = gf::ResetProbeOf(list, g);
    if (!p.valid) continue;
    const uint32_t index = gf::IndexOfCoord(p.coord, b.cascade[p.cascade].dims);
    if (!done.insert({p.cascade, index}).second) ++twice;
    shadow.owner[p.cascade][index] = {p.coord.x, p.coord.y, p.coord.z};
    shadow.set[p.cascade][index] = 1;
  }
  *cleared = list.total;
  return twice;
}

// After a move, every coordinate of every window must be stored where it is looked up, and be its own.
bool WindowsHoldTheirOwn(const ShadowField& shadow, const gf::FieldState& state, const gi::Budget& b) {
  for (uint32_t c = 0; c < b.cascade_count; ++c) {
    const gi::Dims& d = b.cascade[c].dims;
    const gi::Coord o = state.origin(c);
    for (int32_t z = 0; z < int32_t(d.z); ++z)
      for (int32_t y = 0; y < int32_t(d.y); ++y)
        for (int32_t x = 0; x < int32_t(d.x); ++x) {
          const gi::Coord k = {o.x + x, o.y + y, o.z + z};
          const uint32_t i = gf::IndexOfCoord(k, d);
          if (!shadow.set[c][i] || shadow.owner[c][i] != std::array<int32_t, 3>{k.x, k.y, k.z}) return false;
        }
  }
  return true;
}

void TestToroidalMoves() {
  for (const gi::Layout layout : {gi::Layout::kLean, gi::Layout::kStandard, gi::Layout::kExtended}) {
    const gi::Budget& b = gi::BudgetOf(layout);
    Rng rng(11 + uint64_t(layout));
    double camera[3] = {rng.Range(-3000, 3000), rng.Range(0, 80), rng.Range(-3000, 3000)};
    gf::FieldState state;
    ShadowField shadow;
    shadow.Start(b);
    state.Start(b, camera);
    uint64_t cleared = 0;
    gf::MoveReport r = state.Move(camera);
    uint32_t twice = ApplyReset(shadow, b, r, &cleared);
    bool fresh_ok = r.fresh && r.running && !r.cut && r.exposure_count == b.cascade_count && cleared == gi::BudgetProbes(b) && r.exposed == gi::BudgetProbes(b);
    Check(fresh_ok && twice == 0 && WindowsHoldTheirOwn(shadow, state, b), std::string(gi::LayoutName(layout)) + ": the first move exposes every probe of every window once");
    bool dirty_all = true;
    for (uint32_t c = 0; c < b.cascade_count; ++c) dirty_all = dirty_all && state.DirtyCount(c) == b.cascade[c].dims.Probes();
    Check(dirty_all, std::string(gi::LayoutName(layout)) + ": and marks them all dirty");

    // a random walk: slow drives, stops, a fast stretch, jumps of a window or more; the invariant holds after every move
    bool windows_ok = true, no_twice = true, exposed_exact = true, counts_ok = true;
    uint64_t moves_with_exposure = 0, cuts = 0, exposed_total = 0;
    double velocity[3] = {0, 0, 0};
    for (int step = 0; step < 1500; ++step) {
      if (step % 100 == 0) {
        velocity[0] = rng.Range(-60, 60) / 60.0;
        velocity[1] = rng.Range(-4, 4) / 60.0;
        velocity[2] = rng.Range(-60, 60) / 60.0;
      }
      for (int a = 0; a < 3; ++a) camera[a] += velocity[a];
      const bool jump = step % 400 == 399;
      if (jump) {
        camera[0] += rng.Range(-450, 450);
        camera[2] += rng.Range(-450, 450);
      }
      // the windows before and after, to count what entered by brute force
      std::vector<gi::Coord> old_origin(b.cascade_count);
      for (uint32_t c = 0; c < b.cascade_count; ++c) old_origin[c] = state.origin(c);
      r = state.Move(camera);
      twice = ApplyReset(shadow, b, r, &cleared);
      no_twice = no_twice && twice == 0;
      uint64_t entered = 0;
      for (uint32_t c = 0; c < b.cascade_count; ++c) {
        const gi::Dims& d = b.cascade[c].dims;
        const gi::Coord n = state.origin(c);
        for (int32_t z = 0; z < int32_t(d.z); ++z)
          for (int32_t y = 0; y < int32_t(d.y); ++y)
            for (int32_t x = 0; x < int32_t(d.x); ++x)
              if (!gi::Contains(old_origin[c], d, {n.x + x, n.y + y, n.z + z})) ++entered;
      }
      // (a cut clears whole windows though the coarse ones may overlap the old ones: counted below)
      exposed_exact = exposed_exact && (r.cut ? cleared == gi::BudgetProbes(b) : (cleared == entered && r.exposed == entered));
      exposed_total += cleared;
      moves_with_exposure += cleared > 0 ? 1 : 0;
      cuts += r.cut ? 1 : 0;
      windows_ok = windows_ok && WindowsHoldTheirOwn(shadow, state, b);
      counts_ok = counts_ok && r.exposure_count <= gf::kResetListMaxEntries;
      if (r.cut) {
        // after a cut everything was cleared
        counts_ok = counts_ok && cleared == gi::BudgetProbes(b);
      }
    }
    Check(windows_ok, std::string(gi::LayoutName(layout)) + ": after every move of a 1,500 frame random walk every window holds its own probes in their slots (toroidal addressing)");
    Check(no_twice, std::string(gi::LayoutName(layout)) + ": no probe is cleared twice in a move");
    Check(exposed_exact, std::string(gi::LayoutName(layout)) + ": the probes cleared are exactly those that entered a window");
    Check(counts_ok, std::string(gi::LayoutName(layout)) + ": a move has at most 3 boxes a cascade; a cut clears everything");
    if (g_verbose) {
      std::printf("    %s: %llu of 1500 moves exposed probes (%llu probes, %.1f a frame), %llu cuts\n", gi::LayoutName(layout).data(), (unsigned long long)moves_with_exposure,
                  (unsigned long long)exposed_total, double(exposed_total) / 1500.0, (unsigned long long)cuts);
    }
  }
}

void TestDirtyAsTheCameraMoves() {
  const gi::Budget& b = gi::BudgetOf(gi::Layout::kStandard);
  const gi::Dims d = b.cascade[0].dims;
  double camera[3] = {1000.0, 20.0, -500.0};
  gf::FieldState state;
  state.Start(b, camera);
  gf::MoveReport r = state.Move(camera);
  // trace everything (the scheduler takes the dirty probes)
  std::vector<uint32_t> out(16384);
  for (uint32_t c = 0; c < b.cascade_count; ++c) {
    const uint32_t n = state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
    Check(n == b.cascade[c].dims.Probes() && state.DirtyCount(c) == 0, "the scheduler takes every dirty probe of a cascade");
  }
  Check(state.TotalDirty() == 0 && state.DirtyShare(0) == 0.0f, "all clean");
  // steady: nothing exposed, nothing dirty
  r = state.Move(camera);
  Check(r.exposure_count == 0 && r.exposed == 0 && !r.cut && state.TotalDirty() == 0, "a camera that stays marks nothing");
  // one cell along x in the finest cascade: x slab of dims.y x dims.z
  camera[0] += 2.0;
  r = state.Move(camera);
  Check(r.moved >= 1 && state.DirtyCount(0) == d.y * d.z && r.exposed >= d.y * d.z, Num("a step of one cell in x exposes a slab of %.0f probes (16 x 32)", double(state.DirtyCount(0))));
  // the cascades of larger spacing move 4, 8 m at a time: a 2 m step must not move them (usually)
  Check(state.DirtyCount(2) == 0, "the 8 m cascade did not move for 2 m");
  // a diagonal step of one cell in x and z: dims.y x dims.z + (dims.x - 1) x dims.y (the probes the step before left dirty are not marked again: the scheduler has taken them here)
  state.TakeDirty(0, 16384, {0, 0, 0}, out.data());
  const uint32_t before = state.DirtyCount(0);
  camera[0] += 2.0;
  camera[2] += 2.0;
  r = state.Move(camera);
  const uint32_t expect = d.y * d.z + (d.x - 1) * d.y;
  Check(state.DirtyCount(0) - before == expect, Num("a diagonal step exposes %.0f probes (16 x 32 + 31 x 16 = 1008)", double(state.DirtyCount(0) - before)));
  // a step of a whole window is the whole window
  state.TakeDirty(0, 16384, {0, 0, 0}, out.data());
  camera[0] += 64.0;
  r = state.Move(camera);
  Check(state.DirtyCount(0) == d.Probes() && !r.cut, "a move of a whole window exposes the whole window (not a cut: under 200 m)");
  // the share the fade reads
  Check(std::fabs(state.DirtyShare(0) - 1.0f) < 1e-6f && state.DirtyShare(7) == 0.0f, "the dirty share");

  // the dirty probes are what the scheduler takes, nearest the camera first
  gf::FieldState s2;
  double cam2[3] = {0, 8, 0};
  s2.Start(b, cam2);
  s2.Move(cam2);
  s2.TakeDirty(0, 16384, {0, 0, 0}, out.data());
  const gi::Coord at = {5, 3, -2};
  Check(s2.MarkBox(0, {{2, -4, -9}, {9, 7, 5}}) == 7u * 11u * 14u, "a box of lattice coordinates marks the probes in it");
  const gi::Coord near_far[2] = {{9, 6, 4}, {4, 3, -2}};
  uint32_t two[4];
  Check(s2.MarkBox(0, {{100, 100, 100}, {101, 101, 101}}) == 0, "a box outside the window marks nothing");
  uint32_t n = s2.CollectDirty(0, 4, at, two);
  bool nearest = n == 4;
  uint32_t last_dist = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const gi::Coord p = gf::CoordOfIndex(s2.origin(0), d, two[i]);
    const uint32_t dist = uint32_t(std::max({std::abs(p.x - at.x), std::abs(p.y - at.y), std::abs(p.z - at.z)}));
    nearest = nearest && dist >= last_dist;
    last_dist = dist;
  }
  Check(nearest && last_dist <= 1 && s2.DirtyCount(0) == 7u * 11u * 14u, "the nearest dirty probes come first, and collecting does not clear");
  (void)near_far;
  // the one at the reference itself is first
  uint32_t one[1];
  Check(s2.CollectDirty(0, 1, at, one) == 1 && gf::CoordOfIndex(s2.origin(0), d, one[0]) == at, "the probe at the reference comes first");
  const uint32_t taken = s2.TakeDirty(0, 100, at, out.data());
  Check(taken == 100 && s2.DirtyCount(0) == 7u * 11u * 14u - 100 && s2.CollectDirty(0, 0, at, out.data()) == 0, "taking clears what was taken");
  Check(s2.NoteTraced(0, out[0]) == false && s2.NoteTraced(0, gf::IndexOfCoord({8, 6, 4}, d)) == true, "noting a probe traced clears it once");
}

void TestCutsAndWarmup() {
  const gi::Budget& b = gi::BudgetOf(gi::Layout::kStandard);
  double camera[3] = {500.0, 10.0, 500.0};
  gf::FieldState state;
  state.Start(b, camera);
  gf::MoveReport r = state.Move(camera);
  Check(r.fresh && state.BudgetScale() == gi::kCutWarmupBudgetScale && state.WarmupFramesLeft() == gi::kCutWarmupFrames, "a fresh field starts with the warm-up budget");
  uint32_t frames_at_scale = 1;
  for (int i = 0; i < 40; ++i) {
    r = state.Move(camera);
    frames_at_scale += state.BudgetScale() > 1.0f ? 1u : 0u;
  }
  Check(frames_at_scale == gi::kCutWarmupFrames && state.BudgetScale() == 1.0f, Num("the warm-up lasts %.0f frames (kCutWarmupFrames) at 4x", double(frames_at_scale)));
  std::vector<uint32_t> out(16384);
  for (uint32_t c = 0; c < b.cascade_count; ++c) state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
  // a step under 200 m is not a cut, over is
  camera[0] += 199.0;
  r = state.Move(camera);
  Check(!r.cut && r.exposed > 0 && r.exposed < gi::BudgetProbes(b), "a step of 199 m is not a cut");
  for (uint32_t c = 0; c < b.cascade_count; ++c) state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
  camera[2] += 201.0;
  r = state.Move(camera);
  Check(r.cut && r.exposed == gi::BudgetProbes(b) && state.TotalDirty() == gi::BudgetProbes(b) && state.BudgetScale() == gi::kCutWarmupBudgetScale, "a step of 201 m is a cut: everything is cleared and dirty, the warm-up starts");
  // a requested teleport without any move
  for (uint32_t c = 0; c < b.cascade_count; ++c) state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
  for (int i = 0; i < 20; ++i) state.Move(camera);
  state.RequestCut(gf::CutKind::kTeleport);
  r = state.Move(camera);
  Check(r.cut && r.moved == 0 && r.exposed == gi::BudgetProbes(b) && state.TotalDirty() == gi::BudgetProbes(b), "a requested teleport clears everything though the camera did not move");
  // a camera cut the game asked for (the integration's CutFor, CITY-RT-REVIEW M4): a shot that starts 50 m away is a cut though the step is under kCutMetres; no request is no cut
  static_assert(gf::CutFor(true) == gf::CutKind::kTeleport && gf::CutFor(false) == gf::CutKind::kNone);
  for (uint32_t c = 0; c < b.cascade_count; ++c) state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
  for (int i = 0; i < 20; ++i) state.Move(camera);
  camera[0] += 50.0;
  state.RequestCut(gf::CutFor(false));
  r = state.Move(camera);
  Check(!r.cut && !r.relight && r.exposed < gi::BudgetProbes(b), "a step of 50 m with no request from the game is not a cut");
  for (uint32_t c = 0; c < b.cascade_count; ++c) state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
  for (int i = 0; i < 20; ++i) state.Move(camera);
  camera[0] += 50.0;
  state.RequestCut(gf::CutFor(true));
  r = state.Move(camera);
  Check(r.cut && r.exposed == gi::BudgetProbes(b) && state.TotalDirty() == gi::BudgetProbes(b) && state.BudgetScale() == gi::kCutWarmupBudgetScale,
        "a camera cut the game asked for clears every probe and starts the warm-up though the step (50 m) is under kCutMetres");
  // a relight (the hour jumped) clears every probe like a cut: the old hour's light is wrong everywhere, and a kept probe would blend the new light in at the jump hysteresis (85% old after the first trace) or the slow one
  for (uint32_t c = 0; c < b.cascade_count; ++c) state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
  for (int i = 0; i < 20; ++i) state.Move(camera);
  state.RequestCut(gf::CutKind::kRelight);
  r = state.Move(camera);
  Check(!r.cut && r.relight && r.moved == 0 && r.exposed == gi::BudgetProbes(b) && r.exposure_count == b.cascade_count && state.TotalDirty() == gi::BudgetProbes(b) &&
            state.BudgetScale() == gi::kCutWarmupBudgetScale,
        "a relight (the hour jumped) clears and marks dirty every probe, moves no window, starts the warm-up");
  Check(r.Reset().total == gi::BudgetProbes(b), "the reset list of a relight names every probe (the GPU clears them: their state is new again)");
  // the cut kinds the integration hands over (CutFor): the game's cut is a teleport and wins, an hour jump is a relight, nothing is nothing
  static_assert(gf::CutFor(false, true) == gf::CutKind::kRelight && gf::CutFor(true, true) == gf::CutKind::kTeleport && gf::CutFor(false, false) == gf::CutKind::kNone &&
                gf::CutFor(false) == gf::CutKind::kNone);
  // a relight on a field that has just started is the fresh start, not a second one
  gf::FieldState young;
  young.Start(b, camera);
  young.RequestCut(gf::CutKind::kRelight);
  r = young.Move(camera);
  Check(r.fresh && !r.relight && !r.cut, "a relight asked at a fresh start is the fresh start");
  // a teleport asked after a relight wins
  for (uint32_t c = 0; c < b.cascade_count; ++c) state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
  state.RequestCut(gf::CutKind::kRelight);
  state.RequestCut(gf::CutKind::kTeleport);
  state.RequestCut(gf::CutKind::kRelight);
  r = state.Move(camera);
  Check(r.cut && !r.relight, "a teleport wins over a relight");
  // not running: nothing happens
  gf::FieldState idle;
  r = idle.Move(camera);
  Check(!r.running && r.exposure_count == 0 && idle.TotalDirty() == 0 && !idle.running(), "a field that was never started moves nothing");
  state.Stop();
  r = state.Move(camera);
  Check(!state.running() && !r.running && state.TotalDirty() == 0, "a stopped field moves nothing");
  // a restart (a new layout) is a fresh field
  const gi::Budget& other = gi::BudgetOf(gi::Layout::kExtended);
  state.Start(other, camera);
  r = state.Move(camera);
  Check(r.fresh && r.exposed == gi::BudgetProbes(other) && state.starts() == 2, "a new layout starts fresh with every probe new");
}

// ---------------------------------------------------------------------------
// the hour jump (CITY-RT-REVIEW M4): the clock's step test (world_clock.h) and the detector that pairs it with the sun's step (gi_field_rules.h RelightDetector)
// ---------------------------------------------------------------------------
namespace {

// The sun's world direction at an hour: 15 degrees an hour round a tilted axis (a stand-in for the game's own path: the tests need only that it moves with the hour).
void SunAt(double hour, double out[3]) {
  const double a = (hour - 12.0) * 15.0 * 3.14159265358979323846 / 180.0;
  const double x = std::sin(a), y = std::cos(a) * 0.9 + 0.1, z = 0.436;
  const double n = std::sqrt(x * x + y * y + z * z);
  out[0] = x / n;
  out[1] = y / n;
  out[2] = z / n;
}

// One simulated play: the hour advances `per_frame` hours a frame; at frame `set_at` it is set to `set_to` (when set_at >= 0). The clock names the jump `clock_lag` frames after the hour
// changed (the game thread is ahead of, or behind, the render thread), the sun's direction of the draws follows the hour `sun_lag` frames late. Returns the frames the detector fired at.
std::vector<uint64_t> SimulateHour(double start, double per_frame, int frames, int set_at, double set_to, int clock_lag, int sun_lag, bool sun_moves = true) {
  std::vector<double> hour(size_t(frames) + 1);
  double h = start;
  for (int f = 0; f <= frames; ++f) {
    if (f == set_at) h = set_to;
    hour[size_t(f)] = h;
    h += per_frame;
    while (h >= 24.0) h -= 24.0;
  }
  wc::HourJumpTracker tracker;
  gf::RelightDetector detector;
  std::vector<uint64_t> fired;
  for (int f = 0; f <= frames; ++f) {
    // the clock's frame of the hour: the lighting update of frame f - clock_lag saw hour[f - clock_lag]
    tracker.Observe(float(hour[size_t(std::max(f - clock_lag, 0))]));
    double sun[3];
    SunAt(sun_moves ? hour[size_t(std::max(f - sun_lag, 0))] : 12.0, sun);
    if (detector.Observe(uint64_t(f) + 1000u, tracker.jumps(), true, sun)) fired.push_back(uint64_t(f));
  }
  return fired;
}

}  // namespace

void TestHourJump() {
  // THE CLOCK: the step test
  Check(wc::HourStep(23.9f, 0.1f) > 0.19f && wc::HourStep(23.9f, 0.1f) < 0.21f && wc::HourStep(0.1f, 23.9f) < -0.19f && wc::HourStep(12.0f, 18.0f) == 6.0f, "the hour's step is the short way round the day");
  {
    wc::HourJumpTracker t;
    // normal play: the largest a frame carries (0.104 h = 100 x the day speed at the 125 ms step cap; 0.42 h = a time scale of 4 on it), over a day and a half, through midnight
    bool fired = false;
    float hour = 23.0f;
    for (int i = 0; i < 1000; ++i) {
      fired |= t.Observe(hour);
      hour += 0.104f;
      if (hour >= 24.0f) hour -= 24.0f;
    }
    wc::HourJumpTracker fast;  // (the same day cycle at a scene's time scale of 4: a fresh tracker, the start hour is no step)
    hour = 5.0f;
    for (int i = 0; i < 400; ++i) {
      fired |= fast.Observe(hour);
      hour += 0.42f;
      if (hour >= 24.0f) hour -= 24.0f;
    }
    Check(!fired && t.jumps() == 0 && fast.jumps() == 0, "normal advance (the stock rate up to 100 x the speed at the step cap, and a time scale of 4) is never a jump, midnight included");
    wc::HourJumpTracker edge;
    edge.Observe(10.0f);
    Check(!edge.Observe(10.5f) && edge.jumps() == 0 && edge.Observe(11.1f) && edge.jumps() == 1, "a step of exactly kHourJumpHours is not a jump (over it is)");
  }
  {
    wc::HourJumpTracker t;
    Check(!t.Observe(12.0f), "the first hour is not a jump");
    int fires = 0;
    for (int i = 0; i < 5; ++i) fires += t.Observe(12.0f) ? 1 : 0;
    fires += t.Observe(18.0f) ? 1 : 0;
    for (int i = 0; i < 50; ++i) fires += t.Observe(18.0f) ? 1 : 0;
    Check(fires == 1 && t.jumps() == 1, "a set (12 to 18) is one jump, and the hold after it is none");
    wc::HourJumpTracker w;
    w.Observe(23.0f);
    Check(w.Observe(1.0f) && w.jumps() == 1, "23 to 1 (2 h across midnight) is a jump");
    wc::HourJumpTracker m;
    m.Observe(23.9f);
    Check(!m.Observe(0.1f) && !m.Observe(23.9f) && m.jumps() == 0, "23.9 to 0.1 is a step of 0.2 h, not 23.8");
  }
  {
    // a run of large steps (a slider dragged, a day ramp too fast for the frame) is one jump at its start; a quiet frame re-arms it
    wc::HourJumpTracker t;
    t.Observe(0.0f);
    for (int i = 1; i <= 10; ++i) t.Observe(float(i));
    Check(t.jumps() == 1, "ten large steps in a row are one jump");
    t.Observe(10.1f);  // a quiet frame
    t.Observe(16.0f);
    Check(t.jumps() == 2, "a large step after a quiet frame is another");
    // unknown hours change nothing: the next known one is compared with the last known
    wc::HourJumpTracker u;
    u.Observe(12.0f);
    u.Observe(-1.0f);
    u.Observe(24.0f);
    u.Observe(std::nanf(""));
    Check(!u.Observe(12.1f) && u.jumps() == 0, "unknown hours are skipped (no jump from 12.0 to 12.1 across them)");
    Check(u.Observe(20.0f) && u.jumps() == 1, "... and a known one that is far from the last known is a jump");
  }
  {
    // the published path: two relaxed stores and the counter
    wc::Publish(12.0f, 0);
    const uint32_t before = wc::HourJumps();
    wc::Publish(12.05f, 0);
    wc::Publish(-1.0f, -1);
    Check(wc::HourJumps() == before && wc::Hour() == -1.0f, "Publish: a normal step and an unknown hour do not count");
    wc::Publish(19.0f, 0);
    wc::Publish(19.0f, 0);
    Check(wc::HourJumps() == before + 1 && wc::Hour() == 19.0f, "Publish: a set counts once");
  }

  // THE DETECTOR: normal play never fires
  const double kNoSun[3] = {0, 0, 0};
  {
    const std::vector<uint64_t> fired = SimulateHour(23.0, 0.104, 3000, -1, 0.0, 1, 1);
    Check(fired.empty(), "normal advance at the largest step (0.104 h a frame, 100 x the day speed), 3000 frames through midnight: never fires");
    const std::vector<uint64_t> stock = SimulateHour(6.0, 0.00833 * 0.0167, 3000, -1, 0.0, 1, 1);
    Check(stock.empty(), "the stock day cycle never fires");
  }
  // a set fires once, whatever the order and the lag of the two witnesses (the clock before the draws, after them, together)
  {
    bool all_once = true;
    for (int clock_lag = 0; clock_lag <= 4; ++clock_lag) {
      for (int sun_lag = 0; sun_lag <= 4; ++sun_lag) {
        const std::vector<uint64_t> fired = SimulateHour(10.0, 0.0, 200, 100, 18.0, clock_lag, sun_lag);
        const uint64_t expect = uint64_t(100 + std::max(clock_lag, sun_lag));
        if (fired.size() != 1 || fired[0] != expect) {
          all_once = false;
          std::printf("  clock lag %d, sun lag %d: %zu fires, first at %llu (want one at %llu)\n", clock_lag, sun_lag, fired.size(), fired.empty() ? 0ull : (unsigned long long)fired[0],
                      (unsigned long long)expect);
        }
      }
    }
    Check(all_once, "an hour set (10 to 18) fires exactly once, on the frame the second witness comes, for every lag of the clock and the draws from 0 to 4 frames");
  }
  // a large step fires once: the other direction, across midnight, and a step whose sun arrives in two halves
  {
    const std::vector<uint64_t> back = SimulateHour(18.0, 0.0, 200, 100, 6.0, 1, 0);
    const std::vector<uint64_t> wrap = SimulateHour(22.0, 0.0, 200, 100, 3.0, 0, 2);
    Check(back.size() == 1 && wrap.size() == 1, "a set backwards (18 to 6) and one across midnight (22 to 3) fire once each");
    gf::RelightDetector d;
    int fires = 0;
    double sun[3];
    SunAt(10.0, sun);
    fires += d.Observe(1, 0, true, sun) ? 1 : 0;
    SunAt(14.0, sun);  // a 60 degree step in two halves: 30 degrees each, both over the threshold
    fires += d.Observe(2, 1, true, sun) ? 1 : 0;
    SunAt(18.0, sun);
    fires += d.Observe(3, 1, true, sun) ? 1 : 0;
    for (uint64_t f = 4; f < 100; ++f) fires += d.Observe(f, 1, true, sun) ? 1 : 0;
    Check(fires == 1, "a step of the sun in two halves, one jump of the clock: one relight");
  }
  // the clock alone or the sun alone does not fire; the window is the window
  {
    const std::vector<uint64_t> still_sun = SimulateHour(10.0, 0.0, 200, 100, 18.0, 1, 1, false);
    Check(still_sun.empty(), "an hour jump that the draws' sun does not follow (a still sky) does not fire: the light did not change");
    gf::RelightDetector d;
    int fires = 0;
    double sun[3];
    for (uint64_t f = 0; f < 300; ++f) {
      SunAt(f < 100 ? 8.0 : 20.0, sun);  // the draws' light set changes at 100 (a tunnel), no hour behind it
      fires += d.Observe(f, 0, true, sun) ? 1 : 0;
    }
    Check(fires == 0, "a step of the sun with no jump of the clock (a tunnel, the garage's light) does not fire, then or later");
    fires = 0;
    gf::RelightDetector e;
    SunAt(8.0, sun);
    e.Observe(0, 0, true, sun);
    SunAt(20.0, sun);
    fires += e.Observe(100, 0, true, sun) ? 1 : 0;                                // the sun steps at 100
    fires += e.Observe(101 + gf::kRelightWindowFrames, 1, true, sun) ? 1 : 0;  // the clock one frame past the window after it
    Check(fires == 0, "the two witnesses a frame farther apart than kRelightWindowFrames do not pair");
    fires = 0;
    gf::RelightDetector g;
    SunAt(8.0, sun);
    g.Observe(0, 0, true, sun);
    SunAt(20.0, sun);
    fires += g.Observe(100, 0, true, sun) ? 1 : 0;
    fires += g.Observe(100 + gf::kRelightWindowFrames, 1, true, sun) ? 1 : 0;  // exactly at the window
    Check(fires == 1, "... and exactly kRelightWindowFrames apart do");
  }
  // the first look is the baseline, bad suns are skipped, a reset forgets
  {
    gf::RelightDetector d;
    double sun[3];
    SunAt(8.0, sun);
    Check(!d.Observe(10, 7, true, sun), "the jumps counted before the first look are not a jump");
    SunAt(20.0, sun);
    Check(!d.Observe(11, 7, true, kNoSun) && !d.Observe(12, 7, false, sun), "no sun (not given, or of no length) adds no step");
    Check(!d.Observe(13, 7, true, sun), "the sun's first step after those frames (against the last good direction) is a step, no clock jump yet: no fire");
    Check(d.Observe(14, 8, true, sun), "... and the clock's count changing a frame later pairs with it");
    gf::RelightDetector r;
    SunAt(8.0, sun);
    r.Observe(1, 0, true, sun);
    SunAt(20.0, sun);
    r.Observe(2, 1, false, sun);
    r.Reset();
    Check(!r.Observe(3, 1, true, sun) && !r.Observe(4, 2, true, sun), "a reset forgets the baselines");
  }
  // the cos is cos(3 degrees)
  Check(std::fabs(gf::kRelightSunStepCos - std::cos(3.0 * 3.14159265358979323846 / 180.0)) < 1.0e-12, "kRelightSunStepCos is cos(3 degrees)");
  // the largest step of the sun a frame of normal play makes is under it: 0.104 h * 15 degrees
  Check(0.104 * 15.0 < 3.0, "a frame of the largest normal advance moves the sun by 1.6 degrees, under the 3 degree step");
}

void TestWorldBox() {
  const gi::Budget& b = gi::BudgetOf(gi::Layout::kStandard);
  double camera[3] = {0.0, 8.0, 0.0};
  gf::FieldState state;
  state.Start(b, camera);
  state.Move(camera);
  std::vector<uint32_t> out(16384);
  for (uint32_t c = 0; c < b.cascade_count; ++c) state.TakeDirty(c, 16384, {0, 0, 0}, out.data());
  // a sector 20 x 20 m, 12 m tall at (10..30, 0..12, 4..24): the probes of the finest cascade within 6 m of it
  const double lo[3] = {10, 0, 4}, hi[3] = {30, 12, 24};
  const uint32_t marked = state.MarkWorldBox(lo, hi, 0);
  // brute force: the probes whose position is within the box expanded by the margin (3 spacings), in the window
  uint32_t expect = 0;
  const gi::Dims d = b.cascade[0].dims;
  for (uint32_t i = 0; i < d.Probes(); ++i) {
    const gi::Coord c = gf::CoordOfIndex(state.origin(0), d, i);
    const double x = c.x * 2.0, y = c.y * 2.0, z = c.z * 2.0, m = 6.0;
    // the lattice range of the expanded box: floor((lo - m) / s) .. floor((hi + m) / s)
    if (c.x >= int(std::floor((lo[0] - m) / 2.0)) && c.x <= int(std::floor((hi[0] + m) / 2.0)) && c.y >= int(std::floor((lo[1] - m) / 2.0)) && c.y <= int(std::floor((hi[1] + m) / 2.0)) &&
        c.z >= int(std::floor((lo[2] - m) / 2.0)) && c.z <= int(std::floor((hi[2] + m) / 2.0)))
      ++expect;
    (void)x;
    (void)y;
    (void)z;
  }
  Check(marked == expect && marked > 0 && state.DirtyCount(0) == marked && state.DirtyCount(1) == 0, Num("a built sector marks the probes within 3 spacings of it (%.0f)", double(marked)));
  Check(state.MarkWorldBox(lo, hi, 0) == 0, "marking the same box again marks nothing new");
  const uint32_t all = state.MarkWorldBox(lo, hi);
  Check(all > 0 && state.DirtyCount(1) > 0 && state.DirtyCount(2) > 0, "without a cascade, every cascade's probes near the box are marked");
  Check(state.MarkWorldBox(lo, hi, 1, 0.0) <= state.DirtyCount(1), "an explicit margin");
  const double far_lo[3] = {5000, 0, 5000}, far_hi[3] = {5010, 10, 5010};
  Check(state.MarkWorldBox(far_lo, far_hi) == 0, "a box far outside every window marks nothing");
}

// ---------------------------------------------------------------------------
// the fade
// ---------------------------------------------------------------------------
void TestFade() {
  gf::FadeState f;
  Check(f.fade == 0.0f && !f.ready, "a fade starts at 0");
  // not ready: stays 0
  for (int i = 0; i < 60; ++i) f.Step(1.0 / 60.0, true, 1.0f);
  Check(f.fade == 0.0f && !f.ready, "a field that is all dirty does not fade in");
  // ready: rises over kFadeInSeconds
  int frames = 0;
  while (f.fade < 1.0f && frames < 1000) {
    f.Step(1.0 / 60.0, true, 0.0f);
    ++frames;
  }
  Check(f.ready && std::fabs(double(frames) / 60.0 - double(gi::kFadeInSeconds)) < 0.03, Num("the fade rises to 1 in %.2f s (0.4)", double(frames) / 60.0));
  // the latch: 0.3 does not unset it, 0.6 does
  f.Step(1.0 / 60.0, true, 0.3f);
  const bool kept = f.ready && f.fade == 1.0f;
  f.Step(1.0 / 60.0, true, 0.6f);
  Check(kept && !f.ready && f.fade < 1.0f, "the latch: a dirty share of 0.3 keeps a ready field, 0.6 loses it");
  // fall over kFadeOutSeconds from 1
  gf::FadeState g;
  g.fade = 1.0f;
  g.ready = true;
  frames = 0;
  while (g.fade > 0.0f && frames < 1000) {
    g.Step(1.0 / 60.0, false, 0.0f);
    ++frames;
  }
  Check(!g.ready && std::fabs(double(frames) / 60.0 - double(gi::kFadeOutSeconds)) < 0.03, Num("an unusable field fades out in %.2f s (0.5)", double(frames) / 60.0));
  // a long frame never jumps more than a second's worth
  gf::FadeState h;
  h.Step(30.0, true, 0.0f);
  Check(h.fade == 1.0f, "a stalled frame is clamped to a second");
  // a reset is 0 and not ready
  h.Reset();
  Check(h.fade == 0.0f && !h.ready, "reset");
  // not usable: never ready whatever the share
  gf::FadeState u;
  for (int i = 0; i < 10; ++i) u.Step(0.1, false, 0.0f);
  Check(!u.ready && u.fade == 0.0f, "an unusable field is never ready");
}

// ---------------------------------------------------------------------------
// the reset list
// ---------------------------------------------------------------------------
void TestResetList() {
  Rng rng(21);
  bool exact = true, offsets = true;
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<gf::Exposure> ex;
    std::multiset<std::tuple<uint32_t, int, int, int>> want;
    const uint32_t count = uint32_t(rng.Int(0, 14));
    for (uint32_t i = 0; i < count; ++i) {
      gf::Exposure e;
      e.cascade = uint32_t(rng.Int(0, 3));
      e.box.lo = {rng.Int(-70, 70), rng.Int(-20, 20), rng.Int(-70, 70)};
      e.box.hi = {e.box.lo.x + rng.Int(-1, 6), e.box.lo.y + rng.Int(-1, 4), e.box.lo.z + rng.Int(-1, 6)};
      ex.push_back(e);
    }
    const gf::ResetList list = gf::MakeResetList(ex.data(), uint32_t(ex.size()));
    uint32_t kept = 0;
    for (const gf::Exposure& e : ex) {
      if (e.box.Empty()) continue;
      if (++kept > gf::kResetListMaxEntries) break;
      for (int32_t z = e.box.lo.z; z < e.box.hi.z; ++z)
        for (int32_t y = e.box.lo.y; y < e.box.hi.y; ++y)
          for (int32_t x = e.box.lo.x; x < e.box.hi.x; ++x) want.insert({e.cascade, x, y, z});
    }
    std::multiset<std::tuple<uint32_t, int, int, int>> got;
    for (uint32_t g = 0; g < list.total; ++g) {
      const gf::ResetProbe p = gf::ResetProbeOf(list, g);
      if (!p.valid) {
        exact = false;
        break;
      }
      got.insert({p.cascade, p.coord.x, p.coord.y, p.coord.z});
    }
    exact = exact && got == want && !gf::ResetProbeOf(list, list.total).valid && list.words[0] == list.entries && list.words[1] == list.total;
    offsets = offsets && list.GroupsY() * gf::kResetGroupsX >= list.total && (list.total == 0 || (list.GroupsY() - 1) * gf::kResetGroupsX < list.total);
  }
  Check(exact, "every probe group of a reset list is a probe of its boxes, once, and no other (200 random lists, empty and inverted boxes dropped)");
  Check(offsets, "the dispatch's groups cover the list");
  Check(gf::kResetListWords == 100 && gf::kResetListBytes == 400 && gf::kResetListMaxEntries == 12 && gf::kResetGroupsX * 255 >= 65536 - 256, "the list's size");
  // the biggest list a layout needs: the whole windows of 4 cascades is 65,536 groups in a 256 x 256 dispatch
  std::vector<gf::Exposure> whole;
  for (uint32_t c = 0; c < 4; ++c) whole.push_back({c, {{0, 0, 0}, {32, 16, 32}}});
  const gf::ResetList big = gf::MakeResetList(whole.data(), 4);
  Check(big.total == 65536 && big.GroupsY() == 256 && big.entries == 4, "the four windows of the extended layout are 65,536 groups: a 256 x 256 dispatch (65,535 is the limit of each dimension)");
}

// ---------------------------------------------------------------------------
// the bindings
// ---------------------------------------------------------------------------
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

void TestBindings(const std::string& dir) {
  const std::string reset = ReadFile(dir + "gi_field_reset_cs.hlsl"), weights = ReadFile(dir + "gi_field_weights_cs.hlsl"), view = ReadFile(dir + "gi_field_view_ps.hlsl"),
                    common = ReadFile(dir + "gi_field_common.hlsli");
  Check(!reset.empty() && !weights.empty() && !view.empty() && !common.empty(), "the shaders are readable: " + dir);
  const auto reg = [](const char* type, uint32_t n, uint32_t space) { return std::string("register(") + type + std::to_string(n) + ", space" + std::to_string(space) + ")"; };
  Check(Has(reset, reg("t", gf::kBindRows, 0)) && Has(reset, reg("t", gf::kBindResetList, 0)) && Has(reset, reg("u", gf::kBindIrradiance, 0)) && Has(reset, reg("u", gf::kBindMoments, 0)) &&
            Has(reset, reg("u", gf::kBindWeight, 0)) && Has(reset, reg("u", gf::kBindState, 0)),
        "the reset pass declares the compute set's registers");
  Check(Has(weights, reg("t", gf::kBindRows, 0)) && Has(weights, reg("u", gf::kBindWeight, 0)) && Has(weights, reg("u", gf::kBindState, 0)), "the weights pass declares the registers it uses");
  Check(Has(view, reg("t", 0, 0)) && Has(view, reg("s", 0, 1)) && Has(view, reg("t", gf::kViewBindRows, 2)) && Has(view, reg("t", gf::kViewBindState, 2)) &&
            Has(view, "register(b0, space" + std::to_string(gf::kViewPushSpace) + ")"),
        "the view pass declares the bindless sets, the view's set and the push block");
  Check(Has(reset, "numthreads(" + std::to_string(gf::kResetGroupSize) + ", " + std::to_string(gf::kResetGroupSize) + ", 1)") &&
            Has(weights, "numthreads(" + std::to_string(gf::kWeightsGroupSize) + ", 1, 1)"),
        "the thread groups are the rules' (10 x 10 a probe tile; 64 probes)");
  Check(Has(reset, "rgba16f") && Has(reset, "rg16f") && Has(reset, "r8") && Has(weights, "r8"), "the storage images' formats are given to SPIR-V");
  // the view push block: 6 vectors of 16 bytes
  size_t fields = 0;
  for (const char* t : {"float4 posTanX", "float4 rightTanY", "float4 upGain", "float4 forwardMax", "uint4 info", "uint4 size"}) fields += Has(view, t) ? 1u : 0u;
  Check(fields == 6 && gf::kViewPushBytes == 96, "the view's push block is 6 x 16 bytes");
  // the view numbers the shader switches on
  Check(Has(view, "kViewProbes = " + std::to_string(gf::kViewProbes) + "u") && Has(view, "kViewIrradiance = " + std::to_string(gf::kViewIrradianceSpheres) + "u") &&
            Has(view, "kViewSky = " + std::to_string(gf::kViewSkySpheres) + "u"),
        "the view's modes are the rules'");
  // the defines of the .hlsli against the rules
  std::map<std::string, double> want = {{"GIF_FORMAT_VERSION", gf::kFormatVersion},
                                        {"GIF_STATE_UPDATES_MASK", gf::kStateUpdatesMask},
                                        {"GIF_STATE_UPDATES_MAX", gf::kStateUpdatesMax},
                                        {"GIF_STATE_INSIDE", gf::kStateInside},
                                        {"GIF_STATE_NEW", gf::kStateNew},
                                        {"GIF_STATE_BACK_SHIFT", gf::kStateBackShift},
                                        {"GIF_STATE_BACK_MASK", gf::kStateBackMask},
                                        {"GIF_STATE_AFTER_RESET", gf::kStateAfterReset},
                                        {"GIF_WEIGHT_RAMP_UPDATES", gf::kWeightRampUpdates},
                                        {"GIF_RESET_HEADER_WORDS", gf::kResetHeaderWords},
                                        {"GIF_RESET_ENTRY_WORDS", gf::kResetEntryWords},
                                        {"GIF_RESET_MAX_ENTRIES", gf::kResetListMaxEntries},
                                        {"GIF_RESET_GROUPS_X", gf::kResetGroupsX},
                                        {"GIF_SPHERE_RADIUS_SPACINGS", gf::kSphereRadiusSpacings}};
  std::istringstream lines(common);
  std::string line;
  std::map<std::string, double> got;
  while (std::getline(lines, line)) {
    if (line.rfind("#define GIF_", 0) != 0) continue;
    std::istringstream words(line);
    std::string define, name, value;
    words >> define >> name >> value;
    if (value.empty() || value.rfind("(", 0) == 0) continue;
    got[name] = std::strtod(value.c_str(), nullptr);
  }
  uint32_t wrong = 0;
  for (const auto& [name, value] : want) {
    const auto it = got.find(name);
    if (it == got.end() || std::fabs(it->second - value) > 1e-6 * std::max(1.0, std::fabs(value))) {
      ++wrong;
      std::printf("  .hlsli %s: %s, C++ %.9g\n", name.c_str(), it == got.end() ? "missing" : Num("%.9g", it->second).c_str(), value);
    }
  }
  Check(wrong == 0, Num("the .hlsli's %.0f defines are the rules' (%.0f wrong)", double(want.size()), double(wrong)));
}

// ---------------------------------------------------------------------------
// the .hlsli as C++ against the twin
// ---------------------------------------------------------------------------
void TestHlsliTwin() {
  using namespace hlsl_shim;
  Rng rng(31);
  // weight and state
  bool weight_ok = true, state_ok = true;
  for (uint32_t updates : {0u, 1u, 2u, 3u, 100u, 255u}) {
    for (const bool inside : {false, true})
      for (const bool is_new : {false, true})
        for (const uint32_t back : {0u, 77u, 255u}) {
          const uint32_t s = gf::MakeState(updates, inside, is_new, back);
          state_ok = state_ok && GifStateUpdates(s) == gf::StateUpdates(s) && GifStateInside(s) == gf::StateInside(s) && GifStateNew(s) == gf::StateNew(s) && GifStateBack(s) == gf::StateBack(s);
          for (const bool in : {false, true}) weight_ok = weight_ok && std::fabs(GifProbeWeight(s, in) - gf::ProbeWeight(s, in)) < 1e-7f;
        }
  }
  bool make_ok = true;
  for (uint32_t updates : {0u, 1u, 2u, 254u, 255u, 300u})
    for (const bool inside : {false, true})
      for (const bool is_new : {false, true})
        for (const uint32_t back : {0u, 100u, 255u, 400u}) {
          const uint32_t s = gf::MakeState(updates, inside, is_new, back);
          make_ok = make_ok && GifMakeState(updates, inside, is_new, back) == s && GifStateAfterUpdate(s) == gf::StateAfterUpdate(s);
        }
  Check(state_ok && weight_ok && make_ok, "the .hlsli's state decode, make, update and weight are the twin's");
  // coordinates of slots
  bool coords_ok = true;
  for (int trial = 0; trial < 20000; ++trial) {
    const gi::Dims d = {uint32_t(rng.Int(2, 40)), uint32_t(rng.Int(2, 20)), uint32_t(rng.Int(2, 40))};
    const gi::Coord origin = {rng.Int(-9000, 9000), rng.Int(-500, 500), rng.Int(-9000, 9000)};
    const gi::Slot slot = {uint32_t(rng.Int(0, int(d.x) - 1)), uint32_t(rng.Int(0, int(d.y) - 1)), uint32_t(rng.Int(0, int(d.z) - 1))};
    const GifCoord g = GifCoordOfSlot(origin.x, origin.y, origin.z, d.x, d.y, d.z, slot.x, slot.y, slot.z);
    const gi::Coord c = gf::CoordOfSlot(origin, d, slot);
    coords_ok = coords_ok && g.x == c.x && g.y == c.y && g.z == c.z && GifWrapC(origin.x, d.x) == gi::Wrap(origin.x, d.x);
  }
  Check(coords_ok, "the .hlsli's coordinate of a slot is the twin's (20,000 random windows, negative origins included)");
  // the reset list
  bool reset_ok = true;
  for (int trial = 0; trial < 100; ++trial) {
    std::vector<gf::Exposure> ex;
    const uint32_t count = uint32_t(rng.Int(0, 12));
    for (uint32_t i = 0; i < count; ++i) {
      gf::Exposure e;
      e.cascade = uint32_t(rng.Int(0, 3));
      e.box.lo = {rng.Int(-70, 70), rng.Int(-20, 20), rng.Int(-70, 70)};
      e.box.hi = {e.box.lo.x + rng.Int(0, 9), e.box.lo.y + rng.Int(0, 5), e.box.lo.z + rng.Int(0, 9)};
      ex.push_back(e);
    }
    const gf::ResetList list = gf::MakeResetList(ex.data(), uint32_t(ex.size()));
    g_reset_words.assign(list.words.begin(), list.words.end());
    for (uint32_t g = 0; g < list.total + 5; ++g) {
      const GifResetProbe a = GifResetProbeOf(g);
      const gf::ResetProbe b = gf::ResetProbeOf(list, g);
      reset_ok = reset_ok && a.valid == b.valid && (!b.valid || (a.cascade == b.cascade && a.x == b.coord.x && a.y == b.coord.y && a.z == b.coord.z));
    }
  }
  Check(reset_ok, "the .hlsli's reset probe of a group is the twin's");
}

struct BruteHit {
  bool hit = false;
  double t = 0;
  int cx = 0, cy = 0, cz = 0;
  bool borderline = false;
};

BruteHit BruteTrace(const double o[3], const double d[3], double limit, const gi::Coord& origin, const gi::Dims& dims, double spacing, double radius) {
  BruteHit best;
  best.t = 1e30;
  for (int z = 0; z < int(dims.z); ++z)
    for (int y = 0; y < int(dims.y); ++y)
      for (int x = 0; x < int(dims.x); ++x) {
        const int cx = origin.x + x, cy = origin.y + y, cz = origin.z + z;
        const double oc[3] = {o[0] - cx * spacing, o[1] - cy * spacing, o[2] - cz * spacing};
        const double b = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
        const double c = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - radius * radius;
        const double disc = b * b - c;
        if (disc < -1e-4) continue;
        if (disc < 1e-3 && disc >= -1e-4) best.borderline = true;  // a ray that grazes a sphere: the two arithmetics may disagree
        if (disc < 0) continue;
        const double t = -b - std::sqrt(disc);
        if (t > 0 && std::fabs(t - limit) < 1e-3) best.borderline = true;
        if (t > 0 && t < limit && t < best.t) {
          best.hit = true;
          best.t = t;
          best.cx = cx;
          best.cy = cy;
          best.cz = cz;
        }
      }
  return best;
}

void TestTraversal() {
  using namespace hlsl_shim;
  Rng rng(41);
  uint32_t rays = 0, hits = 0, mismatched = 0, skipped = 0, inside_starts = 0;
  for (int trial = 0; trial < 4000; ++trial) {
    // windows of every shape that matter: small and odd, the lean and the standard
    const gi::Dims dims = trial % 4 == 0 ? gi::Dims{32, 16, 32} : trial % 4 == 1 ? gi::Dims{16, 8, 16} : gi::Dims{uint32_t(rng.Int(2, 10)), uint32_t(rng.Int(2, 6)), uint32_t(rng.Int(2, 10))};
    const double spacing = trial % 3 == 0 ? 2.0 : trial % 3 == 1 ? 3.0 : 8.0;
    const gi::Coord origin = {rng.Int(-200, 200), rng.Int(-30, 30), rng.Int(-200, 200)};
    const double radius = spacing * double(gf::kSphereRadiusSpacings);
    // an origin inside the window half of the time, else some way out
    double o[3];
    if (trial % 2 == 0) {
      o[0] = (origin.x + rng.Range(-0.5, double(dims.x) - 0.5)) * spacing;
      o[1] = (origin.y + rng.Range(-0.5, double(dims.y) - 0.5)) * spacing;
      o[2] = (origin.z + rng.Range(-0.5, double(dims.z) - 0.5)) * spacing;
      ++inside_starts;
    } else {
      o[0] = (origin.x + rng.Range(-40, double(dims.x) + 40)) * spacing;
      o[1] = (origin.y + rng.Range(-20, double(dims.y) + 20)) * spacing;
      o[2] = (origin.z + rng.Range(-40, double(dims.z) + 40)) * spacing;
    }
    double d[3] = {rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)};
    if (trial % 17 == 0) d[1] = 0.0;  // an axis-aligned slab case
    if (trial % 23 == 0) d[0] = d[2] = 0.0, d[1] = 1.0;
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-3) continue;
    for (double& v : d) v /= len;
    const double limit = rng.Range(4, 400);
    ++rays;
    const BruteHit want = BruteTrace(o, d, limit, origin, dims, spacing, radius);
    if (want.borderline) {
      ++skipped;
      continue;
    }
    const GifProbeHit got = GifTraceProbes(float(o[0]), float(o[1]), float(o[2]), float(d[0]), float(d[1]), float(d[2]), float(limit), origin.x, origin.y, origin.z, dims.x, dims.y, dims.z,
                                           float(spacing), float(radius));
    hits += want.hit ? 1u : 0u;
    const bool same = got.hit == want.hit && (!want.hit || (std::fabs(double(got.t) - want.t) < 2e-3 * std::max(1.0, want.t) && got.cx == want.cx && got.cy == want.cy && got.cz == want.cz));
    if (!same) {
      if (mismatched < 5) {
        std::printf("  trace mismatch: dims %ux%ux%u s %.0f origin (%d,%d,%d) o (%.3f %.3f %.3f) d (%.4f %.4f %.4f) limit %.1f: want %d t %.4f c (%d,%d,%d), got %d t %.4f c (%d,%d,%d)\n", dims.x,
                    dims.y, dims.z, spacing, origin.x, origin.y, origin.z, o[0], o[1], o[2], d[0], d[1], d[2], limit, want.hit, want.t, want.cx, want.cy, want.cz, got.hit, got.t, got.cx, got.cy,
                    got.cz);
      }
      ++mismatched;
    }
  }
  Check(mismatched == 0, Num("the traversal finds the nearest probe sphere as a brute force over the probes does (%.0f rays, %.0f of them hit, %.0f grazing ones left out, %.0f mismatches)", double(rays),
                             double(hits), double(skipped), double(mismatched)));
  Check(hits > 200 && inside_starts > 1000, "the rays hit often enough to mean something");
  // a ray straight along a lattice row hits the first probe's sphere at its near side
  const GifProbeHit row = GifTraceProbes(-10.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 100.0f, 0, 0, 0, 8, 4, 8, 2.0f, 0.32f);
  Check(row.hit && std::fabs(row.t - (10.0f - 0.32f)) < 1e-4f && row.cx == 0 && row.cy == 0 && row.cz == 0, "a ray along a row of probes hits the first sphere's near side");
  // a ray that starts inside a sphere does not see it, and sees the next
  const GifProbeHit inside = GifTraceProbes(0.1f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 100.0f, 0, 0, 0, 8, 4, 8, 2.0f, 0.32f);
  Check(inside.hit && inside.cx == 1 && std::fabs(inside.t - (2.0f - 0.32f - 0.1f)) < 1e-4f, "starting inside a sphere skips it");
  // a ray that misses the window, and one that ends before it
  Check(!GifTraceProbes(-10.0f, 50.0f, 0.0f, 1.0f, 0.0f, 0.0f, 100.0f, 0, 0, 0, 8, 4, 8, 2.0f, 0.32f).hit && !GifTraceProbes(-10.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 5.0f, 0, 0, 0, 8, 4, 8, 2.0f, 0.32f).hit,
        "a ray that misses the window, or ends before the first probe, hits nothing");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "-v") g_verbose = true;
    else dir = argv[i];
  }
  if (dir.empty()) {
    const std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    dir = (slash == std::string::npos ? std::string() : here.substr(0, slash + 1)) + "hlsl/";
  }
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
  TestMemory();
  TestLadder();
  TestStateAndWeight();
  TestSlots();
  TestDirtySet();
  TestToroidalMoves();
  TestDirtyAsTheCameraMoves();
  TestCutsAndWarmup();
  TestHourJump();
  TestWorldBox();
  TestFade();
  TestResetList();
  TestBindings(dir);
  TestHlsliTwin();
  TestTraversal();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

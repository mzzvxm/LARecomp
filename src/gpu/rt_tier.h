#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray tracing tier, beside the GPU class
// ===========================================================================
// gpu_class.h puts every card in Low, Mid or High by memory and speed. The ray traced effects (the car's reflections, ambient occlusion and sun ray,
// the city's shadows, later the city reflections and bounce light) need a second question: how well does THIS card trace rays? An RTX 3060 and an
// RTX 5080 are both High, and the 5080 is several times faster at it. The owner's decision (2026-11-21, perf/CITY-RT-M2.md and M3, owner answers):
// split High by ray tracing strength, so that `auto` differs between the two, with the thresholds taken from the measured cost.
//
// THE TIER (RtTier): none / weak / strong, found once when the device opens (InitializeRt, called by Device::Open after gpu_class::Initialize):
//   1. the API says what the card CAN do: car_rt_scene.h's CarRtSupported (D3D12 with DXR 1.1 and shader model 6.5; Vulkan with ray query). No: none.
//   2. a software adapter: none. A card that shares memory with the CPU (the integrated class Low): weak, whatever it is (RDNA 2 on a handheld, an
//      Arc iGPU: ray tracing hardware, a handful of units) -- the class already keeps every heavy effect off it.
//   3. the tables, below. A row says none for a card the API lets trace but that has no ray tracing units (the GTX 10 and 16 series run DXR in compute,
//      far too slowly), weak or strong for the rest, by SKU group. In this order: an exact device id row (kRtIdSkus: the SKUs whose chip's default would
//      be wrong); else the name row (kRtNameRows) and the chip row (kRtIdChips) taken together, the LOWER of the two when both exist (a laptop part has
//      its desktop sibling's name and its own chip, and is slower); else nothing: unknown.
//   4. an unknown card is weak (the safe failure: it gets the lighter levels) until the benchmark says otherwise.
//   5. the benchmark (rt_bench_rules.h, rt_bench.cpp) may move a card between weak and strong: ApplyBench. It runs once on the first start with a
//      card (cached next to larecomp.toml, again after a card or driver change), traces a few million rays through a synthetic city and compares the
//      rate with kBenchThresholds. A card the table knows moves only when the rate is clearly across the line (the band), an unknown card goes by the
//      line alone, and none never moves.
// The device id is the PCI id of the adapter (DXGI's DeviceId, Vulkan's deviceID; backend::QueryAdapterIds), which Plume's description does not carry. The
// names are the ones the APIs print: "NVIDIA GeForce RTX 3060 Ti", "AMD Radeon RX 7800 XT", "AMD Radeon RX 6800 XT (RADV NAVI21)", "Intel(R) Arc(TM) A770
// Graphics" (NormalizeName and the whole-word / series-number matching are gpu_class.h's).
//
// THE RUNG (RtRung): what the Auto table reads. It is the tier gated by the class: Low is none (nothing heavy runs on shared memory), Mid is at most
// weak, High is the tier. The classes keep their own table (kEffects); an RT effect has its own, with a column per rung.
//
// THE `auto` TABLE (kRtEffects): for each ray traced effect the value `auto` stands for at each rung, like gpu_class.h's kEffects but with the columns
// none / weak / strong. The effect's package calls
//     std::string mode = gpu_class::ResolveRt(gpu_class::RtEffect::kRtShadows, setting_text);
// as it calls Resolve for the others, and the pause row's label is AutoLabelRt. The cells are FIRST GUESSES from the cost estimates in
// the design files, the way gpu_class.h's are; the main session corrects a cell from the measured cost, and the thresholds (kBenchThresholds,
// and which SKU group is weak or strong in the tables) are set from the GFX cost pass. Weak means lighter: a lighter level where the effect has one
// (the car's occlusion without its AO rays), and the Mid budgets everywhere else (RtClass: the class an effect's own per-class budget table is read
// with: 2 cars within 10 m, not 4 within 14 m; the city shadows' 150 m, not 300 m).
//
// THE BUDGET CLASS (RtClass): an RT package that already has a budget per GpuClass (car_rt_instances_rules.h ClassLimits, car_rt_opp_refl_rules.h
// TexelBudget, city_rt_rules.h BudgetFor) reads it with RtClass(Current(), tier) instead of Current(): strong is the class, weak is at most Mid, none and
// Low are Low (no ray tracing). Exactly the old behaviour on a card that is strong (a High class card) or Mid: only a weak card of the High class moves.
//
// THE OVERRIDES (debug, live): mcla_gpu_rt_tier (auto | none | weak | strong) forces the tier. Otherwise a FORCED CLASS (mcla_gpu_class = low | mid |
// high, the test harness's switch) forces it too, so that a test that says `high` on any GPU still runs the High cells: high is strong, mid is weak,
// low is none (and a card the API says cannot trace stays none). mcla_gpu_rt_bench (auto | off | force) is the benchmark's own switch (rt_bench.cpp).
//
// THE LAUNCHER: the log has one line the launcher reads like it reads the car ray tracing line (GameLogFacts, "[gpu] car ray tracing:"):
//     [gpu] ray tracing tier: <none|weak|strong> (<why>)
// written when the device opens and again when the tier changes (the benchmark, a debug override). The word after the marker is exactly one of the three.
// The launcher shows it as "Ray tracing: Strong / Weak / None" with the sentence of LauncherText(tier), and its Auto lines for the ray traced rows
// say what Auto is at each tier (the cells below; RtAutoDescription gives the words). Until the game has run once the launcher has nothing to show.
//
// Everything that decides something is a pure function in this header (no Plume, no ReXGlue): rt_tier_test.cpp checks the tables' every row, the
// detection's order, the benchmark's decision with synthetic rates, the rungs, the cells and the labels. rt_tier.cpp adds the process's state.
//
// NEEDS FROM THE MAIN SESSION when this merges (code of other branches that this branch does not touch):
//   - feat/city-rt-m1: gpu_class.h's kRtShadows cell (off, off, high) and city_rt_rules.h's LevelFor / BudgetFor read the class; they read RtCell(kRtShadows)
//     and RtClass instead (this table's kRtShadows is the same cvar, mcla_gpu_rt_shadows; the old cell goes).
//   - CITY-RT-M2 / M3 (mcla_gpu_rt_reflections, mcla_gpu_rt_gi): their `auto` cells are this table's, their budgets read RtClass.
//   - the launcher: the marker line above and the three sentences.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "gfx_preset.h"
#include "gpu_class.h"
#include "rt_bench_rules.h"

namespace mcla::gpu::gpu_class {

enum class RtTier : uint8_t { kNone, kWeak, kStrong };

constexpr std::string_view RtTierName(RtTier t) {
  switch (t) {
    case RtTier::kNone:
      return "none";
    case RtTier::kWeak:
      return "weak";
    default:
      return "strong";
  }
}

// "none", "weak" or "strong" (exactly); false for anything else, `auto` included.
inline bool ParseRtTier(std::string_view text, RtTier* out) {
  if (text == "none") {
    *out = RtTier::kNone;
    return true;
  }
  if (text == "weak") {
    *out = RtTier::kWeak;
    return true;
  }
  if (text == "strong") {
    *out = RtTier::kStrong;
    return true;
  }
  return false;
}

constexpr RtTier MinTier(RtTier a, RtTier b) {
  return static_cast<uint8_t>(a) < static_cast<uint8_t>(b) ? a : b;
}

// --- the tables ------------------------------------------------------------------------------------------------------------------
// The tiers are rough public ray tracing speeds against an RTX 3070 as 100 (reviews' DXR averages; the design files' estimates): strong is about the
// RTX 3070 and above (RTX 2080 and up, RTX 3070 and up, RTX 4060 Ti and up, RTX 5060 Ti and up, RX 6900 and up, RX 7800 XT and up, RX 9060 XT and up), weak
// is under it (RTX 2060 to 2070 Super, RTX 3050 to 3060 Ti, RTX 4060 and under, RX 6300 to 6800 XT, RX 7600 to 7700 XT, Arc A and B series). The numbers are
// rounded, partly from memory of those sources, and a row is a judgement of the group, not a measurement of this game: SET FROM THE GFX COST PASS
// (the cost of the ray traced effects on a 3060, a 5080 and a card between), and the benchmark corrects a row that is wrong for one card.

// A name row: the first row (in table order) whose vendor matches, whose `word` is in the normalised name as a whole word, whose `not_word` is not,
// and whose model number after `series` is in lo..hi (gpu_class.h MatchRows' way) says the tier. A row with no word and no series would match every
// card of its vendor: there is none (the test checks).
struct RtNameRow {
  uint32_t vendor;
  std::string_view word;      // must be in the name as whole words; "" = none
  std::string_view not_word;  // must NOT be in the name; "" = none
  std::string_view series;    // the word before the model number ("rtx", "rx", "a"); "" = no number rule
  unsigned lo, hi;            // the model number, inclusive
  RtTier tier;
  std::string_view family;    // for the log line
};

inline constexpr RtNameRow kRtNameRows[] = {
    // --- NVIDIA ---
    // A laptop part is a step under its desktop sibling's name (a power limit of 35 to 150 W): strong only from the RTX 4080 and 5080 laptops, which are
    // an RTX 4070 Ti / 5070 Ti in a bag. Matched before the desktop rows. ("RTX 2060 Max-Q" and the like carry "max q", not "laptop".)
    {kVendorNvidia, "laptop", "", "rtx", 4080, 4099, RtTier::kStrong, "RTX 4080 and 4090 Laptop"},
    {kVendorNvidia, "laptop", "", "rtx", 5080, 5099, RtTier::kStrong, "RTX 5080 and 5090 Laptop"},
    {kVendorNvidia, "laptop", "", "rtx", 2000, 5999, RtTier::kWeak, "RTX Laptop (power limited: a step under the desktop card)"},
    {kVendorNvidia, "max q", "", "rtx", 2000, 5999, RtTier::kWeak, "RTX Max-Q (power limited)"},
    {kVendorNvidia, "mobile", "", "rtx", 2000, 5999, RtTier::kWeak, "RTX Mobile (power limited)"},
    // Desktop RTX. The x060 Ti is the RTX 3070's speed or a little over (RTX 4060 Ti, 5060 Ti), the plain x060 under it.
    {kVendorNvidia, "ti", "", "rtx", 5060, 5069, RtTier::kStrong, "RTX 5060 Ti"},
    {kVendorNvidia, "ti", "", "rtx", 4060, 4069, RtTier::kStrong, "RTX 4060 Ti"},
    {kVendorNvidia, "", "", "rtx", 5070, 5099, RtTier::kStrong, "RTX 5070 and up (Blackwell)"},
    {kVendorNvidia, "", "", "rtx", 5050, 5069, RtTier::kWeak, "RTX 5050 and 5060 (Blackwell)"},
    {kVendorNvidia, "", "", "rtx", 4070, 4099, RtTier::kStrong, "RTX 4070 and up (Ada)"},
    {kVendorNvidia, "", "", "rtx", 4050, 4069, RtTier::kWeak, "RTX 4050 and 4060 (Ada)"},
    {kVendorNvidia, "", "", "rtx", 3070, 3099, RtTier::kStrong, "RTX 3070 and up (Ampere)"},
    {kVendorNvidia, "", "", "rtx", 3050, 3069, RtTier::kWeak, "RTX 3050 to 3060 Ti (Ampere)"},
    {kVendorNvidia, "", "", "rtx", 2080, 2099, RtTier::kStrong, "RTX 2080, 2080 Super and 2080 Ti (Turing)"},
    {kVendorNvidia, "", "", "rtx", 2050, 2079, RtTier::kWeak, "RTX 2050 to 2070 Super (Turing, Ampere mobile)"},
    {kVendorNvidia, "titan rtx", "", "", 0, 0, RtTier::kStrong, "TITAN RTX (Turing)"},
    // No ray tracing units: the driver may expose DXR through the shaders (the GTX 10 and 16 series), at a speed no effect can use.
    {kVendorNvidia, "", "", "gtx", 0, 9999, RtTier::kNone, "GTX (no ray tracing units)"},
    {kVendorNvidia, "", "", "gt", 0, 99999, RtTier::kNone, "GeForce GT (no ray tracing units)"},
    {kVendorNvidia, "", "", "mx", 0, 999, RtTier::kNone, "GeForce MX (no ray tracing units)"},

    // --- AMD ---
    // RX 9000 (RDNA 4, new ray tracing units): the 9060 XT is about an RTX 4060 Ti, the 9070 and 9070 XT well over.
    {kVendorAmd, "", "", "rx", 9060, 9999, RtTier::kStrong, "RX 9060 XT and up (RDNA 4)"},
    // RX 7000 (RDNA 3): the 7800 XT is about an RTX 3070, the 7900 series over it; the 7600 and the 7700 XT under.
    {kVendorAmd, "", "", "rx", 7800, 7999, RtTier::kStrong, "RX 7800 XT and up (RDNA 3)"},
    {kVendorAmd, "", "", "rx", 7600, 7799, RtTier::kWeak, "RX 7600 to 7700 XT (RDNA 3)"},
    // RX 6000 (RDNA 2): its ray tracing is about 0.6 to 0.7 of an RTX 30's at the same class; the 6900 XT and 6950 XT reach an RTX 3070.
    {kVendorAmd, "", "", "rx", 6900, 6999, RtTier::kStrong, "RX 6900 XT and 6950 XT (RDNA 2)"},
    {kVendorAmd, "", "", "rx", 6300, 6899, RtTier::kWeak, "RX 6300 to 6800 XT (RDNA 2)"},
    // RX 5000 and older (RDNA 1, Polaris): no ray tracing units. Vega, Radeon VII, R7 and R9 likewise.
    {kVendorAmd, "", "", "rx", 0, 5999, RtTier::kNone, "RX 5000 and older (no ray tracing units)"},
    {kVendorAmd, "vega", "", "", 0, 0, RtTier::kNone, "Vega (no ray tracing units)"},
    {kVendorAmd, "radeon vii", "", "", 0, 0, RtTier::kNone, "Radeon VII (no ray tracing units)"},
    {kVendorAmd, "r9", "", "", 0, 0, RtTier::kNone, "Radeon R9 (no ray tracing units)"},
    {kVendorAmd, "r7", "", "", 0, 0, RtTier::kNone, "Radeon R7 (no ray tracing units)"},
    // Mesa's RADV prints the chip when it has no marketing name ("AMD Radeon Graphics (RADV NAVI31)", "(RADV GFX1100)"): by chip, as the id rows below.
    {kVendorAmd, "navi10", "", "", 0, 0, RtTier::kNone, "Navi 10 (RDNA 1)"},
    {kVendorAmd, "navi12", "", "", 0, 0, RtTier::kNone, "Navi 12 (RDNA 1)"},
    {kVendorAmd, "navi14", "", "", 0, 0, RtTier::kNone, "Navi 14 (RDNA 1)"},
    {kVendorAmd, "navi21", "", "", 0, 0, RtTier::kWeak, "Navi 21 (RDNA 2)"},
    {kVendorAmd, "navi22", "", "", 0, 0, RtTier::kWeak, "Navi 22 (RDNA 2)"},
    {kVendorAmd, "navi23", "", "", 0, 0, RtTier::kWeak, "Navi 23 (RDNA 2)"},
    {kVendorAmd, "navi24", "", "", 0, 0, RtTier::kWeak, "Navi 24 (RDNA 2)"},
    {kVendorAmd, "navi31", "", "", 0, 0, RtTier::kStrong, "Navi 31 (RDNA 3)"},
    {kVendorAmd, "navi32", "", "", 0, 0, RtTier::kWeak, "Navi 32 (RDNA 3)"},
    {kVendorAmd, "navi33", "", "", 0, 0, RtTier::kWeak, "Navi 33 (RDNA 3)"},
    {kVendorAmd, "navi44", "", "", 0, 0, RtTier::kStrong, "Navi 44 (RDNA 4)"},
    {kVendorAmd, "navi48", "", "", 0, 0, RtTier::kStrong, "Navi 48 (RDNA 4)"},
    {kVendorAmd, "gfx1030", "", "", 0, 0, RtTier::kWeak, "Navi 21 (RDNA 2)"},
    {kVendorAmd, "gfx1031", "", "", 0, 0, RtTier::kWeak, "Navi 22 (RDNA 2)"},
    {kVendorAmd, "gfx1032", "", "", 0, 0, RtTier::kWeak, "Navi 23 (RDNA 2)"},
    {kVendorAmd, "gfx1034", "", "", 0, 0, RtTier::kWeak, "Navi 24 (RDNA 2)"},
    {kVendorAmd, "gfx1100", "", "", 0, 0, RtTier::kStrong, "Navi 31 (RDNA 3)"},
    {kVendorAmd, "gfx1101", "", "", 0, 0, RtTier::kWeak, "Navi 32 (RDNA 3)"},
    {kVendorAmd, "gfx1102", "", "", 0, 0, RtTier::kWeak, "Navi 33 (RDNA 3)"},
    {kVendorAmd, "gfx1200", "", "", 0, 0, RtTier::kStrong, "Navi 44 (RDNA 4)"},
    {kVendorAmd, "gfx1201", "", "", 0, 0, RtTier::kStrong, "Navi 48 (RDNA 4)"},

    // --- Intel ---
    // Arc A (Alchemist): ray tracing about an RTX 3060's (the A770) down to a third of it (the A380). Arc B (Battlemage): the B580 is about an RTX 4060.
    {kVendorIntel, "arc", "", "a", 300, 799, RtTier::kWeak, "Arc A-series (Alchemist)"},
    {kVendorIntel, "arc", "", "b", 500, 599, RtTier::kWeak, "Arc B570 and B580 (Battlemage)"},
};
inline constexpr int kRtNameRowCount = static_cast<int>(sizeof(kRtNameRows) / sizeof(kRtNameRows[0]));

// A device id row: the PCI vendor and the device id range (inclusive). Two tables: kRtIdSkus, tried first, holds the SKUs whose chip's default
// below would be wrong (a desktop part of a chip that mostly serves weaker or laptop parts); kRtIdChips holds the chips, with the LOWEST tier of the
// SKUs that share a chip (the name row refines it). NVIDIA's ids are from the driver's own INF (nv_dispi.inf of the 5080's driver, 2026-11-07: every
// desktop GeForce id and name), AMD's RDNA 1 and 2 from the Windows driver INF (u0206089.inf); AMD's RDNA 3 and 4 and Intel's from memory of the public
// PCI id lists; the name rows stand alone for any card whose name is known.
// CHECK: the RDNA 3 and 4 ids (0x7440 to 0x759F) and the Intel ids (0x5690 to 0x56BF, 0xE200 to 0xE2FF) are from memory, and NVIDIA's laptop ids are only
// in the chip blocks: compare them with a card of each (the log line "device id 0x...." says what the API gave) before relying on the id path.
struct RtIdRow {
  uint32_t vendor;
  uint32_t lo, hi;
  RtTier tier;
  std::string_view family;
};

inline constexpr RtIdRow kRtIdSkus[] = {
    // NVIDIA: TU104's RTX 2080 family (the chip's other SKUs, 2060 and 2070 Super, are weak)
    {kVendorNvidia, 0x1E81, 0x1E82, RtTier::kStrong, "RTX 2080 Super and 2080"},
    {kVendorNvidia, 0x1E87, 0x1E87, RtTier::kStrong, "RTX 2080"},
    // GA104: RTX 3070 Ti and 3070 (the chip's RTX 3060 Ti and laptop parts are weak)
    {kVendorNvidia, 0x2482, 0x2482, RtTier::kStrong, "RTX 3070 Ti"},
    {kVendorNvidia, 0x2484, 0x2484, RtTier::kStrong, "RTX 3070"},
    {kVendorNvidia, 0x2488, 0x2488, RtTier::kStrong, "RTX 3070"},
    // AD106: RTX 4060 Ti (the chip's RTX 4060 and laptop parts are weak)
    {kVendorNvidia, 0x2803, 0x2803, RtTier::kStrong, "RTX 4060 Ti"},
    {kVendorNvidia, 0x2805, 0x2805, RtTier::kStrong, "RTX 4060 Ti"},
    // GB206: RTX 5060 Ti. GB205: RTX 5070.
    {kVendorNvidia, 0x2D04, 0x2D04, RtTier::kStrong, "RTX 5060 Ti"},
    {kVendorNvidia, 0x2F04, 0x2F04, RtTier::kStrong, "RTX 5070"},
    // AMD Navi 21: RX 6950 XT and 6900 XT (0x73BF is the 6800, 6800 XT and a 6900 XT too: the chip's weak default)
    {kVendorAmd, 0x73A5, 0x73A5, RtTier::kStrong, "RX 6950 XT"},
    {kVendorAmd, 0x73AF, 0x73AF, RtTier::kStrong, "RX 6900 XT"},
};
inline constexpr int kRtIdSkuRows = static_cast<int>(sizeof(kRtIdSkus) / sizeof(kRtIdSkus[0]));

inline constexpr RtIdRow kRtIdChips[] = {
    // --- NVIDIA ---
    {kVendorNvidia, 0x1E00, 0x1E3F, RtTier::kStrong, "TU102 (RTX 2080 Ti, TITAN RTX)"},
    {kVendorNvidia, 0x1E80, 0x1EFF, RtTier::kWeak, "TU104 (RTX 2060 to 2080 Super)"},
    {kVendorNvidia, 0x1F00, 0x1F7F, RtTier::kWeak, "TU106 (RTX 2060, 2060 Super, 2070)"},
    {kVendorNvidia, 0x1F80, 0x1FFF, RtTier::kNone, "TU117 (GTX 1630, 1650: no ray tracing units)"},
    {kVendorNvidia, 0x2180, 0x21FF, RtTier::kNone, "TU116 (GTX 1650 Super, 1660: no ray tracing units)"},
    {kVendorNvidia, 0x2200, 0x223F, RtTier::kStrong, "GA102 (RTX 3080, 3090)"},
    {kVendorNvidia, 0x2400, 0x243F, RtTier::kWeak, "GA103 (RTX 3060 Ti, laptop 3080 Ti)"},
    {kVendorNvidia, 0x2480, 0x24FF, RtTier::kWeak, "GA104 (RTX 3060 Ti to 3070 Ti, laptops)"},
    {kVendorNvidia, 0x2500, 0x25FF, RtTier::kWeak, "GA106 and GA107 (RTX 3050 and 3060, laptops)"},
    {kVendorNvidia, 0x2680, 0x26FF, RtTier::kStrong, "AD102 (RTX 4090, 4070 Ti Super)"},
    {kVendorNvidia, 0x2700, 0x277F, RtTier::kStrong, "AD103 (RTX 4080, 4070)"},
    {kVendorNvidia, 0x2780, 0x27FF, RtTier::kStrong, "AD104 (RTX 4070 Ti, 4070 Super, 4060 Ti 16 GB)"},
    {kVendorNvidia, 0x2800, 0x28FF, RtTier::kWeak, "AD106 and AD107 (RTX 4060 Ti, 4060, laptops)"},
    {kVendorNvidia, 0x2B80, 0x2BFF, RtTier::kStrong, "GB202 (RTX 5090)"},
    {kVendorNvidia, 0x2C00, 0x2C7F, RtTier::kStrong, "GB203 (RTX 5080, 5070 Ti, 5070)"},
    {kVendorNvidia, 0x2D00, 0x2DFF, RtTier::kWeak, "GB206 and GB207 (RTX 5060 Ti, 5060, 5050)"},
    {kVendorNvidia, 0x2F00, 0x2FFF, RtTier::kWeak, "GB205 (RTX 5070, 5060)"},
    // --- AMD ---
    {kVendorAmd, 0x7310, 0x731F, RtTier::kNone, "Navi 10 (RX 5600, 5700: RDNA 1)"},
    {kVendorAmd, 0x7340, 0x734F, RtTier::kNone, "Navi 14 (RX 5300, 5500: RDNA 1)"},
    {kVendorAmd, 0x7360, 0x736F, RtTier::kNone, "Navi 12 (RDNA 1)"},
    {kVendorAmd, 0x73A0, 0x73BF, RtTier::kWeak, "Navi 21 (RX 6800 to 6950 XT: RDNA 2)"},
    {kVendorAmd, 0x73C0, 0x73DF, RtTier::kWeak, "Navi 22 (RX 6700 series: RDNA 2)"},
    {kVendorAmd, 0x73E0, 0x73FF, RtTier::kWeak, "Navi 23 (RX 6600 series: RDNA 2)"},
    {kVendorAmd, 0x7420, 0x743F, RtTier::kWeak, "Navi 24 (RX 6300 to 6500 XT: RDNA 2)"},
    {kVendorAmd, 0x7440, 0x745F, RtTier::kStrong, "Navi 31 (RX 7900 series: RDNA 3)"},
    {kVendorAmd, 0x7470, 0x747F, RtTier::kWeak, "Navi 32 (RX 7700 XT, 7800 XT: RDNA 3)"},
    {kVendorAmd, 0x7480, 0x748F, RtTier::kWeak, "Navi 33 (RX 7600: RDNA 3)"},
    {kVendorAmd, 0x7550, 0x755F, RtTier::kStrong, "Navi 48 (RX 9070 series: RDNA 4)"},
    {kVendorAmd, 0x7590, 0x759F, RtTier::kStrong, "Navi 44 (RX 9060 XT: RDNA 4)"},
    // --- Intel ---
    {kVendorIntel, 0x5690, 0x56BF, RtTier::kWeak, "DG2 (Arc A-series: Alchemist)"},
    {kVendorIntel, 0xE200, 0xE2FF, RtTier::kWeak, "BMG (Arc B-series: Battlemage)"},
};
inline constexpr int kRtIdChipRows = static_cast<int>(sizeof(kRtIdChips) / sizeof(kRtIdChips[0]));

// What a table found.
struct RtMatch {
  int row = -1;       // index into the table; -1 = no row applies
  RtTier tier = RtTier::kWeak;
  std::string text;   // what matched, for the log line ("RTX 3060", "device id 0x2487")
};

// The first name row that fits this adapter.
inline RtMatch MatchRtName(uint32_t vendor, std::string_view name) {
  RtMatch best;
  if (name.empty()) {
    return best;
  }
  const std::string norm = NormalizeName(name);
  for (int i = 0; i < kRtNameRowCount; ++i) {
    const RtNameRow& row = kRtNameRows[i];
    if (row.vendor != vendor) {
      continue;
    }
    if (!row.word.empty() && !speed_detail::HasWord(norm, row.word)) {
      continue;
    }
    if (!row.not_word.empty() && speed_detail::HasWord(norm, row.not_word)) {
      continue;
    }
    std::string text;
    if (!row.series.empty()) {
      unsigned number = 0;
      if (!speed_detail::SeriesNumber(norm, row.series, &number) || number < row.lo || number > row.hi) {
        continue;
      }
      text = std::string(row.series) + (row.series.size() > 1 ? " " : "") + std::to_string(number);
    } else if (!row.word.empty()) {
      text = std::string(row.word);
    } else {
      continue;  // no word and no series: a row that matches nothing on purpose
    }
    best.row = i;
    best.tier = row.tier;
    best.text = Upper(text);
    return best;
  }
  return best;
}

inline std::string HexId(uint32_t id) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%04X", static_cast<unsigned>(id));
  return text;
}

inline RtMatch MatchRtIdRows(const RtIdRow* rows, int count, uint32_t vendor, uint32_t device_id) {
  RtMatch best;
  if (device_id == 0) {
    return best;
  }
  for (int i = 0; i < count; ++i) {
    const RtIdRow& row = rows[i];
    if (row.vendor == vendor && device_id >= row.lo && device_id <= row.hi) {
      best.row = i;
      best.tier = row.tier;
      best.text = "device id " + HexId(device_id);
      return best;
    }
  }
  return best;
}

inline RtMatch MatchRtSku(uint32_t vendor, uint32_t device_id) {
  return MatchRtIdRows(kRtIdSkus, kRtIdSkuRows, vendor, device_id);
}
inline RtMatch MatchRtChip(uint32_t vendor, uint32_t device_id) {
  return MatchRtIdRows(kRtIdChips, kRtIdChipRows, vendor, device_id);
}

// --- the detection ---------------------------------------------------------------------------------------------------------------

// What the tier rule looks at, as plain values (rt_tier.cpp fills it from the device).
struct RtFacts {
  bool api_rt = false;           // the renderer can trace rays on this device (CarRtSupported)
  std::string_view api_reason;   // why not, when it cannot (CarRtUnsupportedReason), for the log
  bool software = false;
  bool uma = false;
  bool integrated = false;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;        // 0 when the API did not say
  std::string_view name;
};

enum class RtSource : uint8_t {
  kApi,          // the API cannot trace
  kAdapterType,  // a software adapter or a card that shares memory
  kDeviceId,     // an exact device id row
  kName,         // a name row (and, when there was one, the chip row agreed or was the same tier)
  kChip,         // a chip row alone
  kUnknown,      // no row: weak until the benchmark says
};

struct RtVerdict {
  RtTier tier = RtTier::kNone;
  bool known = true;  // a rule or a row decided (false: unknown, kWeak by default)
  RtSource source = RtSource::kApi;
  std::string why;    // one phrase for the log line
};

inline RtVerdict ClassifyRt(const RtFacts& f) {
  if (!f.api_rt) {
    const std::string reason = f.api_reason.empty() ? std::string("the graphics API reports no ray tracing here") : std::string(f.api_reason);
    return {RtTier::kNone, true, RtSource::kApi, "by API: " + reason};
  }
  if (f.software) {
    return {RtTier::kNone, true, RtSource::kAdapterType, "by adapter type: a software or virtual adapter"};
  }
  if (f.uma || f.integrated) {
    return {RtTier::kWeak, true, RtSource::kAdapterType,
            "by adapter type: shares memory with the CPU (an integrated GPU or an APU): ray tracing units, few of them"};
  }
  if (const RtMatch sku = MatchRtSku(f.vendor_id, f.device_id); sku.row >= 0) {
    const RtIdRow& row = kRtIdSkus[sku.row];
    return {sku.tier, true, RtSource::kDeviceId, "by device id: " + sku.text + " -> " + std::string(RtTierName(sku.tier)) + " (" + std::string(row.family) + ")"};
  }
  const RtMatch name = MatchRtName(f.vendor_id, f.name);
  const RtMatch chip = MatchRtChip(f.vendor_id, f.device_id);
  if (name.row >= 0 && chip.row >= 0) {
    const RtTier tier = MinTier(name.tier, chip.tier);
    const RtNameRow& nr = kRtNameRows[name.row];
    const RtIdRow& cr = kRtIdChips[chip.row];
    std::string why = "by name: " + name.text + " -> " + std::string(RtTierName(name.tier)) + " (" + std::string(nr.family) + "); by chip: " + chip.text +
                      " -> " + std::string(RtTierName(chip.tier)) + " (" + std::string(cr.family) + ")";
    if (name.tier != chip.tier) {
      why += "; the lower stands: " + std::string(RtTierName(tier));
    }
    return {tier, true, RtSource::kName, why};
  }
  if (name.row >= 0) {
    return {name.tier, true, RtSource::kName,
            "by name: " + name.text + " -> " + std::string(RtTierName(name.tier)) + " (" + std::string(kRtNameRows[name.row].family) + ")"};
  }
  if (chip.row >= 0) {
    return {chip.tier, true, RtSource::kChip,
            "by chip: " + chip.text + " -> " + std::string(RtTierName(chip.tier)) + " (" + std::string(kRtIdChips[chip.row].family) + ")"};
  }
  return {RtTier::kWeak, false, RtSource::kUnknown, "no table row for this card: weak until the benchmark says otherwise"};
}

// --- the benchmark's decision ----------------------------------------------------------------------------------------------------

struct BenchVerdict {
  RtTier tier = RtTier::kWeak;
  bool moved = false;  // the benchmark changed the tier the table gave
  std::string why;
};

// The tier after the benchmark. `table_tier` and `table_known` are ClassifyRt's. A result that cannot be used (the benchmark did not run, failed,
// or traced nothing) changes nothing. None never moves: the API or the table says there is nothing to trace with. A card the table does not
// know goes by the line alone (at or over it: strong, else weak). A card the table knows moves only when the rate is clearly across the line, by
// the band: weak to strong at line x (1 + band) or more, strong to weak under line x (1 - band); in between, the table's row stands.
inline BenchVerdict ApplyBench(RtTier table_tier, bool table_known, const rt_bench::Result& bench,
                               const rt_bench::Thresholds& th = rt_bench::kBenchThresholds) {
  BenchVerdict v;
  v.tier = table_tier;
  if (!bench.usable()) {
    v.why = "no usable benchmark result (" + std::string(rt_bench::OutcomeName(bench.outcome)) + "): the table's tier stands";
    return v;
  }
  char rate[48];
  std::snprintf(rate, sizeof(rate), "%.0f M rays/s", bench.mrays_per_s);
  const double line = th.strong_mrays;
  const double over = line * (1.0 + th.band);
  const double under = line * (1.0 - th.band);
  if (table_tier == RtTier::kNone) {
    v.why = std::string(rate) + ": no ray tracing by the API or the table, a benchmark does not change that";
    return v;
  }
  if (!table_known) {
    v.tier = bench.mrays_per_s >= line ? RtTier::kStrong : RtTier::kWeak;
    v.moved = v.tier != table_tier;
    v.why = std::string(rate) + ": the table has no row for this card, the line (" + std::to_string(static_cast<long long>(line)) + ") decides: " +
            std::string(RtTierName(v.tier));
    return v;
  }
  if (table_tier == RtTier::kWeak && bench.mrays_per_s >= over) {
    v.tier = RtTier::kStrong;
    v.moved = true;
    v.why = std::string(rate) + ": over the line's band (" + std::to_string(static_cast<long long>(over)) + "), faster than the table's weak row says";
    return v;
  }
  if (table_tier == RtTier::kStrong && bench.mrays_per_s < under) {
    v.tier = RtTier::kWeak;
    v.moved = true;
    v.why = std::string(rate) + ": under the line's band (" + std::to_string(static_cast<long long>(under)) + "), slower than the table's strong row says";
    return v;
  }
  v.why = std::string(rate) + ": inside the line's band or agreeing with the table, the table's tier stands";
  return v;
}

// --- the rung and the budget class -----------------------------------------------------------------------------------------------

// The tier the Auto table reads: gated by the class. Low (shared memory, software) is none, Mid is at most weak, High is the tier.
constexpr RtTier RtRung(GpuClass c, RtTier t) {
  switch (c) {
    case GpuClass::kLow:
      return RtTier::kNone;
    case GpuClass::kMid:
      return MinTier(t, RtTier::kWeak);
    default:
      return t;
  }
}

// The class a ray tracing package's own per-class budget table (car_rt_instances_rules.h ClassLimits, car_rt_opp_refl_rules.h TexelBudget,
// city_rt_rules.h BudgetFor) is read with: strong is the class's own (High stays High), weak is at most Mid, none is Low (the table's "no ray
// tracing" entry).
constexpr GpuClass RtClass(GpuClass c, RtTier t) {
  switch (RtRung(c, t)) {
    case RtTier::kNone:
      return GpuClass::kLow;
    case RtTier::kWeak:
      return c == GpuClass::kHigh ? GpuClass::kMid : c;
    default:
      return c;
  }
}

// The preset the class and the ray tracing rung recommend (the "Recommended for this GPU" tag; gfx_preset.h kClassToPreset is the same rule as the settings-ui expressions): a strong
// card ENHANCED RT ULTRA, a card at the weak rung ENHANCED RT, else the High class ENHANCED ULTRA, the Mid class ENHANCED and anything else (Low: shared memory, software, the
// Steam Deck) VANILLA. The first start is VANILLA whatever this says.
constexpr gfx_preset::Preset RecommendedPreset(GpuClass c, RtTier t) {
  switch (RtRung(c, t)) {
    case RtTier::kStrong:
      return gfx_preset::Preset::kEnhancedRtUltra;
    case RtTier::kWeak:
      return gfx_preset::Preset::kEnhancedRt;
    default:
      break;
  }
  switch (c) {
    case GpuClass::kHigh:
      return gfx_preset::Preset::kEnhancedUltra;
    case GpuClass::kMid:
      return gfx_preset::Preset::kEnhanced;
    default:
      return gfx_preset::Preset::kVanilla;
  }
}

// The tier a FORCED class stands for in a test (mcla_gpu_class): what the class's cells were meant to show. A card the API says cannot trace stays none.
constexpr RtTier TierForForcedClass(GpuClass forced, bool api_rt) {
  if (!api_rt) {
    return RtTier::kNone;
  }
  switch (forced) {
    case GpuClass::kLow:
      return RtTier::kNone;
    case GpuClass::kMid:
      return RtTier::kWeak;
    default:
      return RtTier::kStrong;
  }
}

// --- the `auto` table of the ray traced effects ----------------------------------------------------------------------------------

// One entry per ray traced effect that has an `auto` value. Add an entry here, in kRtEffects (same order) and in rt_tier_test.cpp.
enum class RtEffect : uint8_t {
  kCarSelfReflRt,  // mcla_gpu_car_selfrefl_rt   auto, off            (car_rt_refl.h: the car's reflection of itself, traced)
  kCarRtAo,        // mcla_gpu_ao_rt             auto, off, ao, contact (car_rt_ao.h: the car's occlusion rays and its sun ray)
  kRtShadows,      // mcla_gpu_rt_shadows        auto, off, high, ultra (city_rt_rules.h, feat/city-rt-m1: the sun's shadows from the city's triangles)
  kRtReflections,  // mcla_gpu_rt_reflections    auto, off, high, ultra (CITY-RT-M2 R1 and R8: the city in the car's reflections; the pause row RT CITY REFLECTIONS, city_refl_setting.h)
  kRtGi,           // mcla_gpu_rt_gi             auto, off, high, ultra (CITY-RT-M3, not built yet: the probe field's bounce light)
  kRtAo,           // mcla_gpu_ao                auto, off, high, ultra, rt (RTAO: the world's ambient occlusion)
};
inline constexpr int kRtEffectCount = 6;

struct RtEffectInfo {
  std::string_view cvar;
  std::string_view none, weak, strong;  // what `auto` stands for at each rung
};

// Legacy Auto for explicit RT selectors resolves to Off at every rung.
// World AO's legacy Auto uses its raster class cell.
inline constexpr RtEffectInfo kRtEffects[kRtEffectCount] = {
    {"mcla_gpu_car_selfrefl_rt", "off", "off", "off"},
    {"mcla_gpu_ao_rt", "off", "off", "off"},
    {"mcla_gpu_rt_shadows", "off", "off", "off"},
    {"mcla_gpu_rt_reflections", "off", "off", "off"},
    {"mcla_gpu_rt_gi", "off", "off", "off"},
    {"mcla_gpu_ao", "", "", ""},
};

constexpr const RtEffectInfo& RtInfo(RtEffect effect) { return kRtEffects[static_cast<int>(effect)]; }
constexpr std::string_view RtCvarName(RtEffect effect) { return RtInfo(effect).cvar; }

// What `auto` stands for at a rung (the tier already gated by the class: RtRung).
constexpr std::string_view RtCell(RtEffect effect, RtTier rung) {
  const RtEffectInfo& info = RtInfo(effect);
  switch (rung) {
    case RtTier::kNone:
      return info.none;
    case RtTier::kWeak:
      return info.weak;
    default:
      return info.strong;
  }
}

// `auto` -> the cell for this class and tier; anything else comes back unchanged.
inline std::string ResolveRtFor(GpuClass c, RtTier t, RtEffect effect, std::string_view user_value) {
  if (user_value == "auto") {
    return std::string(RtCell(effect, RtRung(c, t)));
  }
  return std::string(user_value);
}

// "AUTO (HIGH)": the pause row's label for `auto`, the cell in capitals (a cell that is no word of a menu, such as "both", is shown as it is).
inline std::string AutoLabelRtFor(GpuClass c, RtTier t, RtEffect effect) {
  return AutoLabelOf(RtCell(effect, RtRung(c, t)));
}

// The suffix a ray traced pause row shows where it cannot run (CITY-RT-M2 section 12): " (NO RT GPU)" where the card does not trace (tier none), " (GPU TOO
// SLOW)" where it does but the class is Low (shared memory), else nothing. The row is grey with it.
constexpr std::string_view RtRowSuffix(GpuClass c, RtTier t) {
  if (t == RtTier::kNone) {
    return " (NO RT GPU)";
  }
  return c == GpuClass::kLow ? std::string_view(" (GPU TOO SLOW)") : std::string_view();
}

// --- what the launcher and the log show --------------------------------------------------------------------------------------------

// The marker the launcher reads (GameLogFacts): "[gpu] ray tracing tier: strong (...)".
inline constexpr std::string_view kRtTierLogMarker = "[gpu] ray tracing tier:";

// The capitalised word the launcher shows: "Ray tracing: Strong".
constexpr std::string_view LauncherWord(RtTier t) {
  switch (t) {
    case RtTier::kNone:
      return "None";
    case RtTier::kWeak:
      return "Weak";
    default:
      return "Strong";
  }
}

// The sentence under it. Plain words, no setting names.
constexpr std::string_view LauncherText(RtTier t) {
  switch (t) {
    case RtTier::kNone:
      return "This graphics card does not trace rays, so the effects that use ray tracing keep their regular versions.";
    case RtTier::kWeak:
      return "This graphics card traces rays at a moderate speed: the ray traced effects run at lighter levels, and some stay off.";
    default:
      return "This graphics card traces rays quickly: the ray traced effects run at their full levels.";
  }
}

// What Auto is for one ray traced row at each tier, for the launcher's line under the row (the launcher cannot tell which tier this PC is until the game
// has run): "Auto is Off without ray tracing, Contact on a weak card and Both on a strong one." The cells' own words, capitalised.
inline std::string RtAutoDescription(RtEffect effect) {
  const auto cap = [](std::string_view cell) {
    std::string s(cell);
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') {
      s[0] = static_cast<char>(s[0] - 'a' + 'A');
    }
    return s;
  };
  const RtEffectInfo& info = RtInfo(effect);
  return "Auto is " + cap(info.none) + " without ray tracing, " + cap(info.weak) + " on a weak ray tracing card and " + cap(info.strong) + " on a strong one.";
}

// The log line the launcher reads, after the marker: "<tier> (<why>)".
inline std::string RtTierLogLine(RtTier tier, std::string_view why) {
  return std::string(kRtTierLogMarker) + " " + std::string(RtTierName(tier)) + " (" + std::string(why) + ")";
}

// --- this process's tier (rt_tier.cpp) ---------------------------------------------------------------------------------------------

// What the device says about ray tracing, beyond the description and the capabilities: filled by Device::Open.
struct RtProbe {
  bool api_rt = false;       // CarRtSupported: the renderer can trace rays on this device
  std::string api_reason;    // CarRtUnsupportedReason, when it cannot
  uint32_t device_id = 0;    // the PCI device id (backend::QueryAdapterIds); 0 when unknown
  bool vulkan = false;
  uint64_t driver_version = 0;
};

}  // namespace mcla::gpu::gpu_class

namespace plume {
struct RenderDeviceDescription;
struct RenderDeviceCapabilities;
}  // namespace plume

namespace mcla::gpu::gpu_class {

// Finds the tier from the device (the table), logs the "[gpu] ray tracing tier: ..." line and the values Auto comes to. Device::Open calls it right
// after Initialize. The benchmark's result arrives later (NoteRtBench). mcla_gpu_rt_tier overrides it from then on, live.
void InitializeRt(const plume::RenderDeviceDescription& description, const plume::RenderDeviceCapabilities& caps, const RtProbe& probe);

// The benchmark ran (or its cached result was read): applies ApplyBench to the table's tier, logs the change. Any thread, once or twice.
void NoteRtBench(const rt_bench::Result& result);

// The facts the benchmark's cache is keyed by, and whether a benchmark makes sense on this device at all (a card that can trace, is not integrated or
// software, and whose class is not Low). Valid after InitializeRt.
rt_bench::KeyFacts RtBenchKey();
bool RtBenchWanted();
std::string MachineRtReason(bool& benchmarked);

// The tier in force: the override if there is one (mcla_gpu_rt_tier, else a forced class), else the table's with the benchmark's say. none before
// InitializeRt. Any thread, cheap.
RtTier CurrentRtTier();

// RtRung and RtClass of Current() and CurrentRtTier(); none / Low while the class is not known (Known()). Any thread.
RtTier CurrentRtRung();
GpuClass CurrentRtClass();

// ResolveRtFor / AutoLabelRtFor for the process's class and tier. Any thread.
std::string ResolveRt(RtEffect effect, std::string_view user_value);
std::string AutoLabelRt(RtEffect effect);

}  // namespace mcla::gpu::gpu_class

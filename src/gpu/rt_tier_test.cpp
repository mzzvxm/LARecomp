// Check of the ray tracing tier (rt_tier.h): the tables' every row, the detection's order, the benchmark's decision with synthetic rates, the rungs, the
// `auto` table's cells and the words the launcher shows. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check
// holds.
//
//   cmake --build <build dir> --target larecomp_rt_tier_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_rt_tier_test
//
// The expected values are written out here again on purpose, as gpu_class_test.cpp does: a cell or a row changed in rt_tier.h alone fails the check, and
// changing one on purpose means changing it here too. The tiers of the SKU groups are first guesses (SET FROM THE GFX COST PASS); the owner's own example
// (an RTX 3060 and an RTX 5080 are both High, and `auto` must differ between them) is checked in CheckOwnerExample.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "rt_tier.h"

using namespace mcla::gpu::gpu_class;
namespace rb = mcla::gpu::rt_bench;

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

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
  }
}

const char* T(RtTier t) { return RtTierName(t) == "none" ? "none" : RtTierName(t) == "weak" ? "weak" : "strong"; }

// --- the names ------------------------------------------------------------------------------------------------------------------

struct NameCase {
  uint32_t vendor;
  const char* name;
  bool matched;  // a name row applies
  RtTier tier;
};

const NameCase kNames[] = {
    // NVIDIA Blackwell
    {kVendorNvidia, "NVIDIA GeForce RTX 5090", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 5080", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 5070 Ti", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 5070", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 5060 Ti", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 5060", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 5050", true, RtTier::kWeak},
    // Ada
    {kVendorNvidia, "NVIDIA GeForce RTX 4090", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 4080 SUPER", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 4070 Ti SUPER", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 4070", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 4060 Ti", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 4060", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 4050", true, RtTier::kWeak},
    // Ampere
    {kVendorNvidia, "NVIDIA GeForce RTX 3090 Ti", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 3080", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 3070 Ti", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 3070", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 3060 Ti", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 3060", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 3050", true, RtTier::kWeak},
    // Turing
    {kVendorNvidia, "NVIDIA GeForce RTX 2080 Ti", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 2080 SUPER", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 2070 SUPER", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 2060", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 2050", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA TITAN RTX", true, RtTier::kStrong},
    // laptops: a step under the desktop name, except the top two
    {kVendorNvidia, "NVIDIA GeForce RTX 5090 Laptop GPU", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 5080 Laptop GPU", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 4090 Laptop GPU", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 4080 Laptop GPU", true, RtTier::kStrong},
    {kVendorNvidia, "NVIDIA GeForce RTX 4070 Laptop GPU", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 3080 Ti Laptop GPU", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 2060 with Max-Q Design", true, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 3070 Mobile", true, RtTier::kWeak},
    // no ray tracing units
    {kVendorNvidia, "NVIDIA GeForce GTX 1080 Ti", true, RtTier::kNone},
    {kVendorNvidia, "NVIDIA GeForce GTX 1660 SUPER", true, RtTier::kNone},
    {kVendorNvidia, "NVIDIA GeForce GTX 1650", true, RtTier::kNone},
    {kVendorNvidia, "NVIDIA GeForce GT 1030", true, RtTier::kNone},
    {kVendorNvidia, "NVIDIA GeForce MX450", true, RtTier::kNone},
    // not in the table: workstation cards, a number the rows do not reach
    {kVendorNvidia, "NVIDIA RTX A4000", false, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA RTX 6000 Ada Generation", false, RtTier::kWeak},
    {kVendorNvidia, "NVIDIA GeForce RTX 6090", false, RtTier::kWeak},
    // AMD
    {kVendorAmd, "AMD Radeon RX 9070 XT", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon RX 9060 XT", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon RX 7900 XTX", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon RX 7800 XT", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon RX 7700 XT", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon RX 7600", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon RX 6950 XT", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon RX 6900 XT", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon RX 6800 XT", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon RX 6700 XT", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon RX 6850M XT", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon RX 6600", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon RX 6500 XT", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon RX 6300", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon RX 5700 XT", true, RtTier::kNone},
    {kVendorAmd, "Radeon RX 580 Series", true, RtTier::kNone},
    {kVendorAmd, "AMD Radeon RX 590", true, RtTier::kNone},
    {kVendorAmd, "AMD Radeon RX Vega 64", true, RtTier::kNone},
    {kVendorAmd, "AMD Radeon VII", true, RtTier::kNone},
    {kVendorAmd, "AMD Radeon R9 290", true, RtTier::kNone},
    {kVendorAmd, "AMD Radeon R7 260X", true, RtTier::kNone},
    // Mesa's RADV: the marketing name first, else the chip
    {kVendorAmd, "AMD Radeon RX 7800 XT (RADV NAVI32)", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI31)", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1100)", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI32)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1101)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI33)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1102)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI44)", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1200)", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI48)", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1201)", true, RtTier::kStrong},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI21)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1030)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI22)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1031)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI23)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1032)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI24)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV GFX1034)", true, RtTier::kWeak},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI10)", true, RtTier::kNone},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI12)", true, RtTier::kNone},
    {kVendorAmd, "AMD Radeon Graphics (RADV NAVI14)", true, RtTier::kNone},
    {kVendorAmd, "AMD Radeon Graphics", false, RtTier::kWeak},
    // Intel
    {kVendorIntel, "Intel(R) Arc(TM) A770 Graphics", true, RtTier::kWeak},
    {kVendorIntel, "Intel(R) Arc(TM) A750 Graphics", true, RtTier::kWeak},
    {kVendorIntel, "Intel(R) Arc(TM) A580 Graphics", true, RtTier::kWeak},
    {kVendorIntel, "Intel(R) Arc(TM) A380 Graphics", true, RtTier::kWeak},
    {kVendorIntel, "Intel(R) Arc(TM) B580 Graphics", true, RtTier::kWeak},
    {kVendorIntel, "Intel(R) Arc(TM) B570 Graphics", true, RtTier::kWeak},
    {kVendorIntel, "Intel(R) Arc(TM) Graphics", false, RtTier::kWeak},
    {kVendorIntel, "Intel(R) Arc(TM) Pro A60 Graphics", false, RtTier::kWeak},
    // a name of another vendor's family never matches
    {kVendorAmd, "NVIDIA GeForce RTX 3060", false, RtTier::kWeak},
    {kVendorNvidia, "AMD Radeon RX 7800 XT", false, RtTier::kWeak},
    {kVendorIntel, "AMD Radeon RX 7800 XT", false, RtTier::kWeak},
    {kVendorNvidia, "Some Future GPU", false, RtTier::kWeak},
    {kVendorNvidia, "", false, RtTier::kWeak},
};

void CheckNames(std::set<int>* hit_rows) {
  for (const NameCase& c : kNames) {
    const RtMatch m = MatchRtName(c.vendor, c.name);
    const std::string what = std::string("name '") + c.name + "'";
    Check((m.row >= 0) == c.matched, what + ": " + (c.matched ? "a row applies" : "no row applies"));
    if (c.matched) {
      Check(m.row >= 0 && m.tier == c.tier, what + ": tier " + T(c.tier) + " (got " + T(m.tier) + ", row " + std::to_string(m.row) + ")");
      if (m.row >= 0) {
        hit_rows->insert(m.row);
      }
    }
  }
}

// --- the device ids ---------------------------------------------------------------------------------------------------------------

struct IdCase {
  uint32_t vendor;
  uint32_t id;
  bool sku;     // an exact SKU row applies
  bool chip;    // a chip row applies
  RtTier tier;  // the tier of the row (the SKU's when there is one, else the chip's)
  const char* what;
};

const IdCase kIds[] = {
    // NVIDIA: the ids and names of the driver INF
    {kVendorNvidia, 0x1E02, false, true, RtTier::kStrong, "TITAN RTX (TU102)"},
    {kVendorNvidia, 0x1E04, false, true, RtTier::kStrong, "RTX 2080 Ti (TU102)"},
    {kVendorNvidia, 0x1E81, true, true, RtTier::kStrong, "RTX 2080 SUPER (TU104 SKU)"},
    {kVendorNvidia, 0x1E82, true, true, RtTier::kStrong, "RTX 2080 (TU104 SKU)"},
    {kVendorNvidia, 0x1E84, false, true, RtTier::kWeak, "RTX 2070 SUPER (TU104)"},
    {kVendorNvidia, 0x1E87, true, true, RtTier::kStrong, "RTX 2080 (TU104 SKU)"},
    {kVendorNvidia, 0x1E89, false, true, RtTier::kWeak, "RTX 2060 (TU104)"},
    {kVendorNvidia, 0x1EC7, false, true, RtTier::kWeak, "RTX 2070 SUPER (TU104)"},
    {kVendorNvidia, 0x1F02, false, true, RtTier::kWeak, "RTX 2070 (TU106)"},
    {kVendorNvidia, 0x1F08, false, true, RtTier::kWeak, "RTX 2060 (TU106)"},
    {kVendorNvidia, 0x1F47, false, true, RtTier::kWeak, "RTX 2060 SUPER (TU106)"},
    {kVendorNvidia, 0x1F82, false, true, RtTier::kNone, "GTX 1650 (TU117)"},
    {kVendorNvidia, 0x1F83, false, true, RtTier::kNone, "GTX 1630 (TU117)"},
    {kVendorNvidia, 0x2182, false, true, RtTier::kNone, "GTX 1660 Ti (TU116)"},
    {kVendorNvidia, 0x21C4, false, true, RtTier::kNone, "GTX 1660 SUPER (TU116)"},
    {kVendorNvidia, 0x2204, false, true, RtTier::kStrong, "RTX 3090 (GA102)"},
    {kVendorNvidia, 0x2208, false, true, RtTier::kStrong, "RTX 3080 Ti (GA102)"},
    {kVendorNvidia, 0x2414, false, true, RtTier::kWeak, "RTX 3060 Ti (GA103)"},
    {kVendorNvidia, 0x2482, true, true, RtTier::kStrong, "RTX 3070 Ti (GA104 SKU)"},
    {kVendorNvidia, 0x2484, true, true, RtTier::kStrong, "RTX 3070 (GA104 SKU)"},
    {kVendorNvidia, 0x2486, false, true, RtTier::kWeak, "RTX 3060 Ti (GA104)"},
    {kVendorNvidia, 0x2487, false, true, RtTier::kWeak, "RTX 3060 (GA104)"},
    {kVendorNvidia, 0x2488, true, true, RtTier::kStrong, "RTX 3070 (GA104 SKU)"},
    {kVendorNvidia, 0x24C9, false, true, RtTier::kWeak, "RTX 3060 Ti (GA104)"},
    {kVendorNvidia, 0x2503, false, true, RtTier::kWeak, "RTX 3060 (GA106)"},
    {kVendorNvidia, 0x2544, false, true, RtTier::kWeak, "RTX 3060 (GA106)"},
    {kVendorNvidia, 0x2582, false, true, RtTier::kWeak, "RTX 3050 (GA107)"},
    {kVendorNvidia, 0x2684, false, true, RtTier::kStrong, "RTX 4090 (AD102)"},
    {kVendorNvidia, 0x2689, false, true, RtTier::kStrong, "RTX 4070 Ti SUPER (AD102)"},
    {kVendorNvidia, 0x2704, false, true, RtTier::kStrong, "RTX 4080 (AD103)"},
    {kVendorNvidia, 0x2709, false, true, RtTier::kStrong, "RTX 4070 (AD103)"},
    {kVendorNvidia, 0x2782, false, true, RtTier::kStrong, "RTX 4070 Ti (AD104)"},
    {kVendorNvidia, 0x2786, false, true, RtTier::kStrong, "RTX 4070 (AD104)"},
    {kVendorNvidia, 0x2788, false, true, RtTier::kStrong, "RTX 4060 Ti (AD104)"},
    {kVendorNvidia, 0x2803, true, true, RtTier::kStrong, "RTX 4060 Ti (AD106 SKU)"},
    {kVendorNvidia, 0x2805, true, true, RtTier::kStrong, "RTX 4060 Ti (AD106 SKU)"},
    {kVendorNvidia, 0x2808, false, true, RtTier::kWeak, "RTX 4060 (AD106)"},
    {kVendorNvidia, 0x2882, false, true, RtTier::kWeak, "RTX 4060 (AD107)"},
    {kVendorNvidia, 0x2B85, false, true, RtTier::kStrong, "RTX 5090 (GB202)"},
    {kVendorNvidia, 0x2C02, false, true, RtTier::kStrong, "RTX 5080 (GB203)"},
    {kVendorNvidia, 0x2C05, false, true, RtTier::kStrong, "RTX 5070 Ti (GB203)"},
    {kVendorNvidia, 0x2C09, false, true, RtTier::kStrong, "RTX 5070 (GB203)"},
    {kVendorNvidia, 0x2D04, true, true, RtTier::kStrong, "RTX 5060 Ti (GB206 SKU)"},
    {kVendorNvidia, 0x2D05, false, true, RtTier::kWeak, "RTX 5060 (GB206)"},
    {kVendorNvidia, 0x2D83, false, true, RtTier::kWeak, "RTX 5050 (GB207)"},
    {kVendorNvidia, 0x2F04, true, true, RtTier::kStrong, "RTX 5070 (GB205 SKU)"},
    {kVendorNvidia, 0x2F06, false, true, RtTier::kWeak, "RTX 5060 (GB205)"},
    // AMD: RDNA 1 and 2 from the Windows driver INF, RDNA 3 and 4 from memory (CHECK)
    {kVendorAmd, 0x731F, false, true, RtTier::kNone, "RX 5700 XT (Navi 10)"},
    {kVendorAmd, 0x7340, false, true, RtTier::kNone, "RX 5500 XT (Navi 14)"},
    {kVendorAmd, 0x7360, false, true, RtTier::kNone, "Radeon Pro 5600M (Navi 12)"},
    {kVendorAmd, 0x73A5, true, true, RtTier::kStrong, "RX 6950 XT (Navi 21 SKU)"},
    {kVendorAmd, 0x73AF, true, true, RtTier::kStrong, "RX 6900 XT (Navi 21 SKU)"},
    {kVendorAmd, 0x73BF, false, true, RtTier::kWeak, "RX 6800 / 6800 XT (Navi 21)"},
    {kVendorAmd, 0x73DF, false, true, RtTier::kWeak, "RX 6700 XT (Navi 22)"},
    {kVendorAmd, 0x73EF, false, true, RtTier::kWeak, "RX 6650 XT (Navi 23)"},
    {kVendorAmd, 0x73FF, false, true, RtTier::kWeak, "RX 6600 (Navi 23)"},
    {kVendorAmd, 0x7424, false, true, RtTier::kWeak, "RX 6300 (Navi 24)"},
    {kVendorAmd, 0x743F, false, true, RtTier::kWeak, "RX 6500 XT (Navi 24)"},
    {kVendorAmd, 0x744C, false, true, RtTier::kStrong, "RX 7900 XTX (Navi 31)"},
    {kVendorAmd, 0x747E, false, true, RtTier::kWeak, "RX 7800 XT (Navi 32)"},
    {kVendorAmd, 0x7480, false, true, RtTier::kWeak, "RX 7600 (Navi 33)"},
    {kVendorAmd, 0x7550, false, true, RtTier::kStrong, "RX 9070 XT (Navi 48)"},
    {kVendorAmd, 0x7590, false, true, RtTier::kStrong, "RX 9060 XT (Navi 44)"},
    // Intel
    {kVendorIntel, 0x56A0, false, true, RtTier::kWeak, "Arc A770 (DG2)"},
    {kVendorIntel, 0x56A5, false, true, RtTier::kWeak, "Arc A380 (DG2)"},
    {kVendorIntel, 0xE20B, false, true, RtTier::kWeak, "Arc B580 (BMG)"},
    {kVendorIntel, 0xE20C, false, true, RtTier::kWeak, "Arc B570 (BMG)"},
    // an id of another vendor, or none
    {kVendorAmd, 0x2C02, false, false, RtTier::kWeak, "an NVIDIA id with the AMD vendor"},
    {kVendorNvidia, 0x73DF, false, false, RtTier::kWeak, "an AMD id with the NVIDIA vendor"},
    {kVendorNvidia, 0, false, false, RtTier::kWeak, "no id"},
    {kVendorNvidia, 0x0001, false, false, RtTier::kWeak, "an id no row holds"},
};

void CheckIds(std::set<int>* sku_rows, std::set<int>* chip_rows) {
  for (const IdCase& c : kIds) {
    const RtMatch sku = MatchRtSku(c.vendor, c.id);
    const RtMatch chip = MatchRtChip(c.vendor, c.id);
    const std::string what = std::string("id ") + HexId(c.id) + " (" + c.what + ")";
    Check((sku.row >= 0) == c.sku, what + ": " + (c.sku ? "an exact SKU row applies" : "no exact SKU row applies"));
    Check((chip.row >= 0) == c.chip, what + ": " + (c.chip ? "a chip row applies" : "no chip row applies"));
    if (c.sku && sku.row >= 0) {
      Check(sku.tier == c.tier, what + ": SKU tier " + T(c.tier));
      sku_rows->insert(sku.row);
    }
    if (c.chip && chip.row >= 0) {
      if (!c.sku) {
        Check(chip.tier == c.tier, what + ": chip tier " + T(c.tier) + " (got " + T(chip.tier) + ")");
      }
      chip_rows->insert(chip.row);
    }
  }
}

// --- the tables' own sense ----------------------------------------------------------------------------------------------------------

void CheckTablesAreSound(const std::set<int>& name_hit, const std::set<int>& sku_hit, const std::set<int>& chip_hit) {
  for (int i = 0; i < kRtNameRowCount; ++i) {
    const RtNameRow& row = kRtNameRows[i];
    const std::string what = "name row " + std::to_string(i) + " (" + std::string(row.family) + ")";
    Check(!row.word.empty() || !row.series.empty(), what + ": has a word or a series, or it would match every card of its vendor");
    Check(row.series.empty() || row.lo <= row.hi, what + ": lo <= hi");
    Check(!row.family.empty(), what + ": has a family text for the log");
    Check(row.vendor == kVendorNvidia || row.vendor == kVendorAmd || row.vendor == kVendorIntel, what + ": a known vendor");
    Check(name_hit.count(i) == 1, what + ": the test has an example that reaches it");
  }
  for (int i = 0; i < kRtIdSkuRows; ++i) {
    const RtIdRow& row = kRtIdSkus[i];
    const std::string what = "SKU row " + std::to_string(i) + " (" + std::string(row.family) + ")";
    Check(row.lo <= row.hi, what + ": lo <= hi");
    Check(sku_hit.count(i) == 1, what + ": the test has an example that reaches it");
    // An exception exists only where its chip's default differs: else it says nothing.
    const RtMatch chip = MatchRtChip(row.vendor, row.lo);
    Check(chip.row >= 0 && chip.tier != row.tier, what + ": its chip row has another tier (else the SKU row is not needed)");
    Check(chip.row >= 0 && MatchRtChip(row.vendor, row.hi).row == chip.row, what + ": the whole range is in one chip row");
  }
  for (int i = 0; i < kRtIdChipRows; ++i) {
    const RtIdRow& row = kRtIdChips[i];
    const std::string what = "chip row " + std::to_string(i) + " (" + std::string(row.family) + ")";
    Check(row.lo <= row.hi, what + ": lo <= hi");
    Check(chip_hit.count(i) == 1, what + ": the test has an example that reaches it");
    for (int j = i + 1; j < kRtIdChipRows; ++j) {
      const RtIdRow& other = kRtIdChips[j];
      Check(row.vendor != other.vendor || row.hi < other.lo || other.hi < row.lo, what + ": does not overlap chip row " + std::to_string(j));
    }
  }
}

// --- the detection --------------------------------------------------------------------------------------------------------------------

RtFacts Facts(uint32_t vendor, const char* name, uint32_t id = 0) {
  RtFacts f;
  f.api_rt = true;
  f.vendor_id = vendor;
  f.name = name;
  f.device_id = id;
  return f;
}

void CheckDetection() {
  // The API first: the best card with no ray tracing is none, and says why.
  {
    RtFacts f = Facts(kVendorNvidia, "NVIDIA GeForce RTX 5090", 0x2B85);
    f.api_rt = false;
    f.api_reason = "the graphics card does not offer DirectX Raytracing tier 1.1 (inline ray queries)";
    const RtVerdict v = ClassifyRt(f);
    Check(v.tier == RtTier::kNone && v.known && v.source == RtSource::kApi, "no API ray tracing: none, by API");
    Check(v.why.find("DirectX Raytracing tier 1.1") != std::string::npos, "the API's reason is in the line");
    f.api_reason = "";
    Check(ClassifyRt(f).why.find("no ray tracing") != std::string::npos, "an API with no reason still says why in words");
  }
  // A software adapter is none; a card that shares memory is weak, whatever it is.
  {
    RtFacts f = Facts(kVendorNvidia, "Microsoft Basic Render Driver");
    f.software = true;
    const RtVerdict v = ClassifyRt(f);
    Check(v.tier == RtTier::kNone && v.source == RtSource::kAdapterType, "a software adapter: none");
    RtFacts g = Facts(kVendorAmd, "AMD Radeon Graphics (RADV VANGOGH)");  // the Steam Deck
    g.uma = true;
    const RtVerdict w = ClassifyRt(g);
    Check(w.tier == RtTier::kWeak && w.known && w.source == RtSource::kAdapterType, "a UMA card (the Deck's RDNA 2): weak, known");
    RtFacts h = Facts(kVendorAmd, "AMD Radeon RX 7900 XTX");
    h.integrated = true;  // the type alone
    Check(ClassifyRt(h).tier == RtTier::kWeak, "an integrated type: weak whatever its name");
  }
  // The exact SKU row first.
  {
    const RtVerdict v = ClassifyRt(Facts(kVendorNvidia, "NVIDIA GeForce RTX 3060", 0x2484));
    Check(v.tier == RtTier::kStrong && v.source == RtSource::kDeviceId && v.why.find("by device id") == 0, "an exact SKU row beats the name");
  }
  // The name and the chip together, the lower of the two.
  {
    const RtVerdict laptop = ClassifyRt(Facts(kVendorNvidia, "NVIDIA GeForce RTX 3070", 0x249D));  // a laptop 3070: the desktop name, GA104's id
    Check(laptop.tier == RtTier::kWeak && laptop.source == RtSource::kName, "a laptop part with the desktop name: the chip's weak stands");
    Check(laptop.why.find("the lower stands: weak") != std::string::npos, "...and the line says the lower stood");
    const RtVerdict agree = ClassifyRt(Facts(kVendorNvidia, "NVIDIA GeForce RTX 5080", 0x2C02));
    Check(agree.tier == RtTier::kStrong && agree.source == RtSource::kName && agree.why.find("the lower stands") == std::string::npos,
          "name and chip agree: no 'lower stands' in the line");
    const RtVerdict none = ClassifyRt(Facts(kVendorNvidia, "NVIDIA GeForce GTX 1650", 0x2C02));
    Check(none.tier == RtTier::kNone, "a name that says none and a chip that says strong: none (the lower)");
    const RtVerdict name_only = ClassifyRt(Facts(kVendorNvidia, "NVIDIA GeForce RTX 4090"));
    Check(name_only.tier == RtTier::kStrong && name_only.source == RtSource::kName && name_only.known, "a name alone");
    const RtVerdict chip_only = ClassifyRt(Facts(kVendorNvidia, "NVIDIA Some New Card", 0x2C02));
    Check(chip_only.tier == RtTier::kStrong && chip_only.source == RtSource::kChip && chip_only.known, "a chip alone");
  }
  // Unknown: weak, not known (the benchmark decides).
  {
    const RtVerdict v = ClassifyRt(Facts(kVendorNvidia, "NVIDIA Some New Card"));
    Check(v.tier == RtTier::kWeak && !v.known && v.source == RtSource::kUnknown, "an unknown card: weak and not known");
    Check(v.why.find("benchmark") != std::string::npos, "...and the line says the benchmark decides");
  }
}

// The owner's example (2026-11-21): an RTX 3060 and an RTX 5080 are both High, and `auto` must differ between them.
void CheckOwnerExample() {
  const RtVerdict r3060 = ClassifyRt(Facts(kVendorNvidia, "NVIDIA GeForce RTX 3060", 0x2487));
  const RtVerdict r5080 = ClassifyRt(Facts(kVendorNvidia, "NVIDIA GeForce RTX 5080", 0x2C02));
  Check(r3060.tier == RtTier::kWeak, "owner's example: the RTX 3060 is weak");
  Check(r5080.tier == RtTier::kStrong, "owner's example: the RTX 5080 is strong");
  // Both are the High class by memory (12 GiB, 16 GiB); the Auto cells for the effects that differ do differ.
  Check(ResolveRtFor(GpuClass::kHigh, r3060.tier, RtEffect::kRtGi, "auto") != ResolveRtFor(GpuClass::kHigh, r5080.tier, RtEffect::kRtGi, "auto"),
        "owner's example: auto of the city bounce light differs between the two");
  Check(ResolveRtFor(GpuClass::kHigh, r3060.tier, RtEffect::kCarRtAo, "auto") != ResolveRtFor(GpuClass::kHigh, r5080.tier, RtEffect::kCarRtAo, "auto"),
        "owner's example: auto of the car's ray traced occlusion differs between the two");
  Check(RtClass(GpuClass::kHigh, r3060.tier) == GpuClass::kMid && RtClass(GpuClass::kHigh, r5080.tier) == GpuClass::kHigh,
        "owner's example: the budget class differs between the two (Mid budgets for the 3060)");
}

// --- the benchmark's decision ---------------------------------------------------------------------------------------------------------

rb::Result Rate(double mrays, rb::Outcome outcome = rb::Outcome::kOk) {
  rb::Result r;
  r.outcome = outcome;
  r.mrays_per_s = mrays;
  r.best_ms = 1.0;
  r.hit_share = 0.6;
  r.runs = 3;
  return r;
}

void CheckBenchDecision() {
  const rb::Thresholds th = {1000.0, 0.5};  // the test's own line: from 1500 up a weak card moves up, under 500 a strong one moves down
  // nothing usable: nothing moves
  for (const rb::Outcome o : {rb::Outcome::kNotRun, rb::Outcome::kFailed, rb::Outcome::kInvalid}) {
    for (const RtTier t : {RtTier::kNone, RtTier::kWeak, RtTier::kStrong}) {
      rb::Result r = Rate(99999.0, o);
      const BenchVerdict v = ApplyBench(t, true, r, th);
      Check(v.tier == t && !v.moved, std::string("an unusable benchmark (") + std::string(rb::OutcomeName(o)) + ") moves nothing: " + T(t));
    }
  }
  Check(!Rate(0.0).usable(), "a rate of 0 is not usable");
  // none never moves
  {
    const BenchVerdict v = ApplyBench(RtTier::kNone, true, Rate(99999.0), th);
    Check(v.tier == RtTier::kNone && !v.moved, "none stays none whatever the rate");
    const BenchVerdict u = ApplyBench(RtTier::kNone, false, Rate(99999.0), th);
    Check(u.tier == RtTier::kNone && !u.moved, "none stays none even when the table did not know it");
  }
  // an unknown card goes by the line alone
  {
    Check(ApplyBench(RtTier::kWeak, false, Rate(1500.0), th).tier == RtTier::kStrong, "unknown card over the line: strong");
    Check(ApplyBench(RtTier::kWeak, false, Rate(1000.0), th).tier == RtTier::kStrong, "unknown card on the line: strong");
    Check(ApplyBench(RtTier::kWeak, false, Rate(999.0), th).tier == RtTier::kWeak, "unknown card under the line: weak");
    const BenchVerdict up = ApplyBench(RtTier::kWeak, false, Rate(1500.0), th);
    Check(up.moved, "unknown card measured strong: moved from the default weak");
    Check(!ApplyBench(RtTier::kWeak, false, Rate(500.0), th).moved, "unknown card measured weak: not moved");
  }
  // a known weak card moves up only over the band
  {
    Check(ApplyBench(RtTier::kWeak, true, Rate(1499.0), th).tier == RtTier::kWeak, "known weak, 1499 (just under the band's top): stays weak");
    const BenchVerdict v = ApplyBench(RtTier::kWeak, true, Rate(1500.0), th);
    Check(v.tier == RtTier::kStrong && v.moved, "known weak, 1500 (the band's top): strong");
    Check(ApplyBench(RtTier::kWeak, true, Rate(1000.0), th).tier == RtTier::kWeak, "known weak, on the line: stays weak (the band holds the table's row)");
    Check(ApplyBench(RtTier::kWeak, true, Rate(10.0), th).tier == RtTier::kWeak, "known weak, very slow: stays weak (no lower than weak)");
  }
  // a known strong card moves down only under the band
  {
    Check(ApplyBench(RtTier::kStrong, true, Rate(500.0), th).tier == RtTier::kStrong, "known strong, 500 (the band's bottom): stays strong");
    const BenchVerdict v = ApplyBench(RtTier::kStrong, true, Rate(499.0), th);
    Check(v.tier == RtTier::kWeak && v.moved, "known strong, 499 (under the band): weak");
    Check(ApplyBench(RtTier::kStrong, true, Rate(1000.0), th).tier == RtTier::kStrong, "known strong, on the line: stays strong");
    Check(ApplyBench(RtTier::kStrong, true, Rate(99999.0), th).tier == RtTier::kStrong, "known strong, very fast: stays strong");
    // a slow outcome (the warm-up's rate) counts like any usable one
    Check(ApplyBench(RtTier::kStrong, true, Rate(20.0, rb::Outcome::kSlow), th).tier == RtTier::kWeak, "known strong measured slow (DXR in compute?): weak");
  }
  // the band is two sided and exclusive of the table's row: two measurements of one card a few percent apart do not flip it
  {
    const RtTier a = ApplyBench(RtTier::kWeak, true, Rate(1100.0), th).tier;
    const RtTier b = ApplyBench(RtTier::kWeak, true, Rate(1250.0), th).tier;
    Check(a == b, "two nearby rates of a known card give one tier");
  }
  // the whole chain with synthetic timings and the default thresholds: a few fast runs, and the slowest ones that the warm-up alone measures
  {
    rb::Measurement m;
    m.warmup_ms = 3.0;
    m.run_ms = {2.0, 1.5, 1.8};
    m.rays = rb::kRaysPerRun;
    m.hits = rb::kRaysPerRun * 6 / 10;
    const rb::Result r = rb::Summarize(m);  // the best run, 1.5 ms: 8,388,608 / 1.5 ms = 5592 M rays/s
    Check(r.usable() && r.mrays_per_s > 5500.0 && r.mrays_per_s < 5700.0, "synthetic runs: the fastest run's rate");
    Check(ApplyBench(RtTier::kStrong, true, r).tier == RtTier::kStrong, "default thresholds: a strong card at 5592 stays strong");
    Check(ApplyBench(RtTier::kWeak, true, r).tier == RtTier::kWeak, "default thresholds: a weak card at 5592 stays weak (the band holds it)");
    Check(ApplyBench(RtTier::kWeak, false, r).tier == RtTier::kStrong, "default thresholds: an unknown card at 5592 is strong");
    rb::Measurement slow;
    slow.warmup_ms = 40.0;
    slow.rays = rb::kRaysPerDispatch;
    slow.hits = rb::kRaysPerDispatch / 2;
    const rb::Result s = rb::Summarize(slow);
    Check(s.outcome == rb::Outcome::kSlow && s.usable(), "a warm-up alone is a slow measurement and usable");
    Check(ApplyBench(RtTier::kStrong, true, s).tier == RtTier::kWeak, "default thresholds: 26 M rays/s moves a strong row to weak");
  }
}

// --- an integrated GPU at its defaults ---------------------------------------------------------------------------------------------------

// VERIFY-PIPE-IGPU (2026-11-24). The test PC's iGPU (the 9800X3D's 2-CU RDNA 2, "AMD Radeon(TM) Graphics", device id 0x13C0) can trace rays (ray queries yes) and the logs of the
// 11-23 runs said `class: low`, `ray tracing tier: weak (by adapter type: shares memory ...)` and every ray traced `auto` off. A verification row read the benchmark's own
// `rt bench: not run` line as "ray tracing ran" and failed; the game was right. This holds the whole chain, from the adapter's facts to the cells, so that the day a link
// turns a ray traced effect on for a card that shares memory (a tier row that says strong for an integrated chip, a class rule that lets one past Low) THIS fails, not a log reading.
// The one exception to "Low" is kIntegratedMidTable (Strix Halo): it is Mid on purpose and gets the weak cells; the test below says so.
void CheckIntegratedGpuDefaults() {
  struct IntegratedCase {
    const char* what;
    uint32_t vendor;
    const char* name;
    uint32_t device_id;
    bool uma;
    bool integrated;
  };
  const IntegratedCase cases[] = {
      {"the test PC's iGPU on D3D12 (UMA)", kVendorAmd, "AMD Radeon(TM) Graphics", 0x13C0, true, false},
      {"the same on Vulkan (the integrated type and host visible local memory)", kVendorAmd, "AMD Radeon(TM) Graphics", 0x13C0, true, true},
      {"the same on Vulkan where only the device type says integrated", kVendorAmd, "AMD Radeon(TM) Graphics", 0x13C0, false, true},
      {"the same, the driver gave no device id", kVendorAmd, "AMD Radeon(TM) Graphics", 0, true, false},
      {"the Steam Deck on RADV", kVendorAmd, "AMD Radeon Graphics (RADV VANGOGH)", 0, true, true},
      {"an Intel UHD iGPU", kVendorIntel, "Intel(R) UHD Graphics 770", 0, true, false},
      {"an Intel Arc iGPU (ray tracing units, a handful)", kVendorIntel, "Intel(R) Arc(TM) Graphics", 0, true, false},
  };
  for (const IntegratedCase& c : cases) {
    const std::string what = c.what;
    AdapterFacts adapter;
    adapter.uma = c.uma;
    adapter.integrated = c.integrated;
    adapter.dedicated_bytes = 512ull << 20;
    adapter.raytracing = true;
    adapter.name = c.name;
    adapter.vendor_id = c.vendor;
    const Verdict cls = Classify(adapter);
    Check(cls.cls == GpuClass::kLow, what + ": the class is Low");

    RtFacts facts = Facts(c.vendor, c.name, c.device_id);
    facts.uma = c.uma;
    facts.integrated = c.integrated;
    const RtVerdict tier = ClassifyRt(facts);
    Check(tier.tier == RtTier::kWeak && tier.source == RtSource::kAdapterType, what + ": the tier is weak, by adapter type (the chip has ray tracing units, a handful)");

    // The class gates the tier: Low is the rung none, and the budget class is Low (no ray tracing) whatever the tier says, a forced strong one included.
    for (const RtTier t : {RtTier::kNone, RtTier::kWeak, RtTier::kStrong}) {
      Check(RtRung(cls.cls, t) == RtTier::kNone, what + ": the rung is none at tier " + T(t));
      Check(RtClass(cls.cls, t) == GpuClass::kLow, what + ": the budget class is Low at tier " + T(t));
      CheckEq(std::string(RtRowSuffix(cls.cls, t)), t == RtTier::kNone ? " (NO RT GPU)" : " (GPU TOO SLOW)", what + ": the pause rows are grey at tier " + T(t));
    }
    // Every ray traced effect: `auto` is off and the row says so (`both`, `contact`, `high`, `on` anywhere here is an effect running on shared memory).
    for (int i = 0; i < kRtEffectCount; ++i) {
      const RtEffect effect = static_cast<RtEffect>(i);
      const std::string name = std::string(RtCvarName(effect)) + ": " + what;
      CheckEq(ResolveRtFor(cls.cls, tier.tier, effect, "auto"), "off", name + ": auto is off");
      CheckEq(AutoLabelRtFor(cls.cls, tier.tier, effect), "AUTO (OFF)", name + ": the pause row's label is AUTO (OFF)");
      CheckEq(ResolveRtFor(cls.cls, RtTier::kStrong, effect, "auto"), "off", name + ": auto is off even when a test forces the tier strong");
    }
  }
  // The named exception, so that nobody reads the loop above as "an integrated GPU is never Mid": Strix Halo (kIntegratedMidTable) is about an RTX 4060 and takes the weak cells.
  {
    AdapterFacts halo;
    halo.uma = true;
    halo.dedicated_bytes = 512ull << 20;
    halo.raytracing = true;
    halo.name = "AMD Radeon(TM) 8060S Graphics";
    halo.vendor_id = kVendorAmd;
    Check(Classify(halo).cls == GpuClass::kMid, "Strix Halo's Radeon 8060S: the class is Mid (kIntegratedMidTable)");
    RtFacts facts = Facts(kVendorAmd, "AMD Radeon(TM) 8060S Graphics", 0);
    facts.uma = true;
    CheckEq(std::string(RtTierName(ClassifyRt(facts).tier)), "weak", "Strix Halo's Radeon 8060S: the tier is weak");
    CheckEq(std::string(RtTierName(RtRung(GpuClass::kMid, RtTier::kWeak))), "weak", "Strix Halo's Radeon 8060S: the rung is weak (the lighter cells run)");
  }
}

// --- the rungs, the budget class and the forced class ----------------------------------------------------------------------------------

void CheckRungs() {
  struct Row {
    GpuClass cls;
    RtTier tier;
    RtTier rung;
    GpuClass budget;
  };
  const Row rows[] = {
      {GpuClass::kHigh, RtTier::kStrong, RtTier::kStrong, GpuClass::kHigh}, {GpuClass::kHigh, RtTier::kWeak, RtTier::kWeak, GpuClass::kMid},
      {GpuClass::kHigh, RtTier::kNone, RtTier::kNone, GpuClass::kLow},      {GpuClass::kMid, RtTier::kStrong, RtTier::kWeak, GpuClass::kMid},
      {GpuClass::kMid, RtTier::kWeak, RtTier::kWeak, GpuClass::kMid},       {GpuClass::kMid, RtTier::kNone, RtTier::kNone, GpuClass::kLow},
      {GpuClass::kLow, RtTier::kStrong, RtTier::kNone, GpuClass::kLow},     {GpuClass::kLow, RtTier::kWeak, RtTier::kNone, GpuClass::kLow},
      {GpuClass::kLow, RtTier::kNone, RtTier::kNone, GpuClass::kLow},
  };
  for (const Row& r : rows) {
    const std::string what = std::string(ClassName(r.cls)) + " class, " + T(r.tier) + " tier";
    Check(RtRung(r.cls, r.tier) == r.rung, what + ": rung " + T(r.rung));
    Check(RtClass(r.cls, r.tier) == r.budget, what + ": budget class " + std::string(ClassName(r.budget)));
  }
  // A forced class in a test: high is strong, mid weak, low none; a card without API ray tracing stays none.
  Check(TierForForcedClass(GpuClass::kHigh, true) == RtTier::kStrong, "forced class high: strong");
  Check(TierForForcedClass(GpuClass::kMid, true) == RtTier::kWeak, "forced class mid: weak");
  Check(TierForForcedClass(GpuClass::kLow, true) == RtTier::kNone, "forced class low: none");
  for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    Check(TierForForcedClass(c, false) == RtTier::kNone, std::string("forced class ") + std::string(ClassName(c)) + " without API ray tracing: none");
  }
  // ...and the forced class's cells are the class's own: high gives strong's, mid gives weak's.
  Check(RtClass(GpuClass::kHigh, TierForForcedClass(GpuClass::kHigh, true)) == GpuClass::kHigh, "forced high keeps the High budgets");
  Check(RtClass(GpuClass::kMid, TierForForcedClass(GpuClass::kMid, true)) == GpuClass::kMid, "forced mid keeps the Mid budgets");
  // the names
  RtTier t;
  Check(ParseRtTier("none", &t) && t == RtTier::kNone, "parse none");
  Check(ParseRtTier("weak", &t) && t == RtTier::kWeak, "parse weak");
  Check(ParseRtTier("strong", &t) && t == RtTier::kStrong, "parse strong");
  for (const char* bad : {"auto", "", "Strong", "high", "low", "none "}) {
    Check(!ParseRtTier(bad, &t), std::string("'") + bad + "' is not a tier");
  }
  CheckEq(std::string(RtTierName(RtTier::kNone)), "none", "name none");
  CheckEq(std::string(RtTierName(RtTier::kWeak)), "weak", "name weak");
  CheckEq(std::string(RtTierName(RtTier::kStrong)), "strong", "name strong");
}

// --- the `auto` table --------------------------------------------------------------------------------------------------------------------

struct CellRow {
  RtEffect effect;
  const char* cvar;
  const char* none;
  const char* weak;
  const char* strong;
};

// In the order of the RtEffect enum.
const CellRow kCells[] = {
    // The car's own reflection traced: every card that traces.
    {RtEffect::kCarSelfReflRt, "mcla_gpu_car_selfrefl_rt", "off", "on", "on"},
    // The car's occlusion rays and sun ray: a weak card keeps the sun ray alone, the strong has both.
    {RtEffect::kCarRtAo, "mcla_gpu_ao_rt", "off", "contact", "both"},
    // The city's sun shadows: high where the card traces (a weak card with the Mid budget).
    {RtEffect::kRtShadows, "mcla_gpu_rt_shadows", "off", "high", "high"},
    // The city in the reflections: strong cards only (CITY-RT M2 R1: the look is a strong card's; a weak card runs it by hand).
    {RtEffect::kRtReflections, "mcla_gpu_rt_reflections", "off", "off", "high"},
    // The probe field's bounce light: strong cards only.
    {RtEffect::kRtGi, "mcla_gpu_rt_gi", "off", "off", "high"},
};

const char* const kOwnChoices[] = {"off", "on", "high", "ultra", "ao", "contact", "both", "", "Auto", "AUTO", "game", "auto " /* a space: not auto */};

void CheckCells() {
  Check(sizeof(kCells) / sizeof(kCells[0]) == static_cast<std::size_t>(kRtEffectCount), "the test lists every ray traced effect");
  Check(sizeof(kRtEffects) / sizeof(kRtEffects[0]) == static_cast<std::size_t>(kRtEffectCount), "kRtEffects has an entry for every effect");
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const RtTier tiers[] = {RtTier::kNone, RtTier::kWeak, RtTier::kStrong};
  for (int i = 0; i < kRtEffectCount; ++i) {
    const CellRow& row = kCells[i];
    const std::string name = row.cvar;
    Check(static_cast<int>(row.effect) == i, name + ": the test is in the order of the enum");
    CheckEq(std::string(RtCvarName(row.effect)), name, name + ": cvar name");
    CheckEq(std::string(RtCell(row.effect, RtTier::kNone)), row.none, name + ": cell at none");
    CheckEq(std::string(RtCell(row.effect, RtTier::kWeak)), row.weak, name + ": cell at weak");
    CheckEq(std::string(RtCell(row.effect, RtTier::kStrong)), row.strong, name + ": cell at strong");
    // The rule a table's cells keep: a card with less ray tracing never gets MORE (none is off).
    CheckEq(std::string(row.none), "off", name + ": none is off");

    for (const GpuClass c : classes) {
      for (const RtTier t : tiers) {
        const RtTier rung = RtRung(c, t);
        const std::string want = std::string(RtCell(row.effect, rung));
        const std::string what = name + " on " + std::string(ClassName(c)) + ", " + T(t);
        CheckEq(ResolveRtFor(c, t, row.effect, "auto"), want, what + ": Resolve(auto)");
        CheckEq(AutoLabelRtFor(c, t, row.effect), AutoLabelOf(want), what + ": AutoLabel");
        for (const char* own : kOwnChoices) {
          CheckEq(ResolveRtFor(c, t, row.effect, own), own, what + ": Resolve('" + own + "') is unchanged");
        }
      }
    }
    // The Low class and the none tier are off, Mid's strong card gets the weak cell, High follows its tier.
    CheckEq(ResolveRtFor(GpuClass::kLow, RtTier::kStrong, row.effect, "auto"), row.none, name + ": the Low class is off even with a strong tier");
    CheckEq(ResolveRtFor(GpuClass::kHigh, RtTier::kNone, row.effect, "auto"), row.none, name + ": no ray tracing is off");
    CheckEq(ResolveRtFor(GpuClass::kMid, RtTier::kStrong, row.effect, "auto"), row.weak, name + ": a Mid class card with a strong tier gets the weak cell");
    CheckEq(ResolveRtFor(GpuClass::kHigh, RtTier::kWeak, row.effect, "auto"), row.weak, name + ": High with a weak tier gets the weak cell");
    CheckEq(ResolveRtFor(GpuClass::kHigh, RtTier::kStrong, row.effect, "auto"), row.strong, name + ": High with a strong tier gets the strong cell");
  }
  // The labels as the pause rows show them.
  CheckEq(AutoLabelRtFor(GpuClass::kHigh, RtTier::kStrong, RtEffect::kRtGi), "AUTO (HIGH)", "label: RT bounce light strong");
  CheckEq(AutoLabelRtFor(GpuClass::kHigh, RtTier::kWeak, RtEffect::kRtGi), "AUTO (OFF)", "label: RT bounce light weak");
  CheckEq(AutoLabelRtFor(GpuClass::kHigh, RtTier::kStrong, RtEffect::kCarRtAo), "AUTO (BOTH)", "label: car occlusion strong");
  CheckEq(AutoLabelRtFor(GpuClass::kHigh, RtTier::kWeak, RtEffect::kCarRtAo), "AUTO (CONTACT)", "label: car occlusion weak");
  // A cell is never a word the setting would refuse as a player's own choice, but the ones that are not in the cvar's list are the table's words for the
  // effect's own reading: "both" (car_rt_ao_rules.h reads any word that is not off, ao or contact as the default, which is both terms).
  // The rungs are the same everywhere the tier is read: the car effects' cells at the strong rung equal the old (pre-tier) behaviour, which was `on` / `both`.
  CheckEq(std::string(RtCell(RtEffect::kCarSelfReflRt, RtTier::kStrong)), "on", "strong keeps the car self-reflection on, as before the tier");
}

// --- the words the launcher and the log show --------------------------------------------------------------------------------------------------

void CheckLauncher() {
  // The grey rows' suffix: no ray tracing first, then the Low class.
  for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    CheckEq(std::string(RtRowSuffix(c, RtTier::kNone)), " (NO RT GPU)", std::string("row suffix, ") + std::string(ClassName(c)) + " class, no ray tracing");
  }
  CheckEq(std::string(RtRowSuffix(GpuClass::kLow, RtTier::kWeak)), " (GPU TOO SLOW)", "row suffix, Low class with ray tracing units");
  CheckEq(std::string(RtRowSuffix(GpuClass::kLow, RtTier::kStrong)), " (GPU TOO SLOW)", "row suffix, Low class, strong tier (a forced test)");
  for (const GpuClass c : {GpuClass::kMid, GpuClass::kHigh}) {
    for (const RtTier t : {RtTier::kWeak, RtTier::kStrong}) {
      CheckEq(std::string(RtRowSuffix(c, t)), "", std::string("row suffix, ") + std::string(ClassName(c)) + " class, " + T(t) + " tier: none");
    }
  }
  CheckEq(std::string(LauncherWord(RtTier::kNone)), "None", "launcher word none");
  CheckEq(std::string(LauncherWord(RtTier::kWeak)), "Weak", "launcher word weak");
  CheckEq(std::string(LauncherWord(RtTier::kStrong)), "Strong", "launcher word strong");
  std::set<std::string> sentences;
  for (const RtTier t : {RtTier::kNone, RtTier::kWeak, RtTier::kStrong}) {
    const std::string text(LauncherText(t));
    Check(!text.empty() && text.back() == '.', std::string("launcher sentence ends with a full stop: ") + T(t));
    sentences.insert(text);
    // Nothing in the graphics settings is called Low (owner, 2026-11-14), and a sentence names no setting's cvar.
    Check(text.find("Low") == std::string::npos && text.find(" low ") == std::string::npos && text.find("mcla_") == std::string::npos,
          std::string("launcher sentence has no setting name: ") + T(t));
  }
  Check(sentences.size() == 3, "the three launcher sentences differ");
  CheckEq(RtAutoDescription(RtEffect::kRtGi), "Auto is Off without ray tracing, Off on a weak ray tracing card and High on a strong one.",
          "launcher line: RT bounce light");
  CheckEq(RtAutoDescription(RtEffect::kCarRtAo), "Auto is Off without ray tracing, Contact on a weak ray tracing card and Both on a strong one.",
          "launcher line: car occlusion");
  CheckEq(RtAutoDescription(RtEffect::kRtShadows), "Auto is Off without ray tracing, High on a weak ray tracing card and High on a strong one.",
          "launcher line: RT shadows");
  // The marker line: the word right after it is the tier, so the launcher reads it like the car ray tracing line.
  CheckEq(std::string(kRtTierLogMarker), "[gpu] ray tracing tier:", "the log marker the launcher reads");
  CheckEq(RtTierLogLine(RtTier::kStrong, "by name: RTX 5080 -> strong"), "[gpu] ray tracing tier: strong (by name: RTX 5080 -> strong)", "the log line");
  CheckEq(RtTierLogLine(RtTier::kNone, "by API: x"), "[gpu] ray tracing tier: none (by API: x)", "the log line, none");
  for (const RtTier t : {RtTier::kNone, RtTier::kWeak, RtTier::kStrong}) {
    const std::string line = RtTierLogLine(t, "x");
    const std::string rest = line.substr(kRtTierLogMarker.size() + 1);
    Check(rest.compare(0, RtTierName(t).size(), RtTierName(t)) == 0, std::string("the word after the marker is the tier: ") + T(t));
  }
}

}  // namespace

int main() {
  std::set<int> name_hit, sku_hit, chip_hit;
  CheckNames(&name_hit);
  CheckIds(&sku_hit, &chip_hit);
  CheckTablesAreSound(name_hit, sku_hit, chip_hit);
  CheckDetection();
  CheckOwnerExample();
  CheckBenchDecision();
  CheckIntegratedGpuDefaults();
  CheckRungs();
  CheckCells();
  CheckLauncher();
  std::printf("rt_tier_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

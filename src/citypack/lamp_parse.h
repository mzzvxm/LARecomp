#pragma once
// ===========================================================================
// LARecomp city pack -- reading lights.xlm (the file lights; lamp_table.h says what it holds and what it does not)
// ===========================================================================
// resources/city/sc/lights.xlm is an RSC5 resource of type 2018 (LZX, 2 MiB of virtual segment, no physical one). Read from the player's own file (DIRECTOR-ATLAS-NOTES 1.3,
// checked again byte by byte for this reader; all fields big endian, pointers the resource's own):
//
//   root +0x50   the light list: u32 pointer to the array, u16 count (15,174), u16 capacity (the same)
//   the array    count lights of 96 bytes back to back (the first at 0x80):
//                 +0 position xyz, +16 colour rgb (0..1), +32 direction xyz, +48 (NaN fill), +64 intensity, +68 cone angle (degrees, 360 omni), +72 spread (degrees),
//                 +76 and +80 the cone's falloff terms (0 and 1 in the file: the game computes them at load with sub_822FD2C8), +84 (0xCDCDCDCD), +88 type (0 omni, 1 spot),
//                 +89 on (1 in all 15,174), +90 and +91 (0)
//   the rest     the runtime list at +0x60 is empty in the file (the game appends the street lamps to it at load) and a spare block of 64 records follows the array
//
// A light that is not plausible (a position that is not a number or beyond 100 km, a colour outside -0.001..100, an intensity outside 0..1e5, a cone outside 0..360.5,
// a type or an on byte above 1) is left out and counted: garbage must end in a count, not in a table. `listed` is what the file says; `kept` what the table gets.
// ===========================================================================

#include <string>
#include <vector>

#include "citypack/lamp_table.h"
#include "citypack/rsc_image.h"

namespace larecomp::citypack {

inline constexpr const char* kLightsPath = "resources/city/sc/lights.xlm";
inline constexpr uint32_t kLightsRootList = 0x50;   // the root's light list
inline constexpr uint32_t kLightRecordBytes = 96;
inline constexpr uint32_t kMaxFileLights = 200000;  // what a decode believes (the game's textures hold 32,767)

struct LampParseStats {
  uint32_t listed = 0;
  uint32_t kept = 0;
  uint32_t skipped = 0;      // not plausible
  uint32_t spots = 0;        // of the kept
  uint32_t omni = 0;
  uint32_t off = 0;          // kept lights whose on byte is clear
};

// What a census of the file adds to the table's own stats: the golden comparison's numbers.
struct LampCensus {
  LampParseStats parse;
  LampStats table;
  // intensity buckets: 0, (0,10), [10,15), [15,20), [20,30), [30,60), [60,100), 100 and more
  static constexpr size_t kBuckets = 8;
  std::array<uint32_t, kBuckets> by_intensity{};
  uint32_t most_common_intensity_count = 0;  // how many lights share the commonest intensity (20 in the game's file)
  float most_common_intensity = 0.0f;
  bool present = false;     // the archive had a lights.xlm
  bool from_mods = false;   // ... and it was the mod archive's
};

// Reads the light list of a decoded lights.xlm. False (with `error`) for a resource that has no list there (not a light file); `out` then is empty. A light with an index
// above `kMaxFileLights` is never read.
bool ParseLightsResource(const RscImage& image, std::vector<Lamp>* out, LampParseStats* stats, std::string* error);

// What the player's own lights.xlm is (read byte by byte for this file, 2026-11-21; another game build or a mod's file gives other numbers, so these are the tool's golden check and a
// unit test's reference, never a limit): the lights per exact intensity (the eight commonest), the greatest intensity, the bounds, and the 64 m grid over them.
struct GoldenIntensity {
  float value;
  uint32_t count;
};
inline constexpr GoldenIntensity kGoldenLampIntensities[] = {{20.0f, 6913}, {10.0f, 1307}, {15.0f, 1093}, {0.0f, 987}, {12.0f, 978}, {40.0f, 688}, {60.0f, 434}, {30.0f, 353}};
inline constexpr float kGoldenLampMaxIntensity = 470.0f;
inline constexpr float kGoldenLampBoundsMin[3] = {-3467.95f, -30.96f, -2323.02f};
inline constexpr float kGoldenLampBoundsMax[3] = {2756.59f, 377.38f, 1963.19f};
inline constexpr uint32_t kGoldenLampGridX = 99, kGoldenLampGridZ = 68;  // cells of 64 m
inline constexpr uint32_t kGoldenLampCellsUsed = 3514;
inline constexpr uint32_t kGoldenLampCellMax = 38;                       // the most lights in one cell

struct LampGolden {
  bool ok = true;
  std::string report;  // one line a number, "equal" or "DIFFERENT"
};
// The golden comparison of a file's table (built with the default 64 m cell) and census.
LampGolden CheckLampGolden(const LampTable& table, const LampCensus& census);

// The census of a parsed file (every light of `lamps` is a file light).
LampCensus CensusOfLamps(const std::vector<Lamp>& lamps, const LampParseStats& parse);

// The text the tool prints: counts, the golden comparison, the bounds, the intensity buckets.
std::string LampCensusReport(const LampCensus& census);

}  // namespace larecomp::citypack

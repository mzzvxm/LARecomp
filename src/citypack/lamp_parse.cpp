#include "citypack/lamp_parse.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "citypack/bytes.h"

namespace larecomp::citypack {

namespace {

bool Plausible(const Lamp& l, uint8_t type, uint8_t on) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(l.pos[i]) || std::fabs(l.pos[i]) > 1.0e5f) return false;
    if (!std::isfinite(l.colour[i]) || l.colour[i] < -0.001f || l.colour[i] > 100.0f) return false;
    if (!std::isfinite(l.dir[i]) || std::fabs(l.dir[i]) > 1.0e3f) return false;
  }
  return std::isfinite(l.intensity) && l.intensity >= 0.0f && l.intensity < 1.0e5f && std::isfinite(l.cone_deg) && l.cone_deg >= 0.0f && l.cone_deg <= 360.5f &&
         std::isfinite(l.spread_deg) && std::fabs(l.spread_deg) < 1.0e3f && type <= 1 && on <= 1;
}

}  // namespace

bool ParseLightsResource(const RscImage& image, std::vector<Lamp>* out, LampParseStats* stats, std::string* error) {
  out->clear();
  LampParseStats local;
  LampParseStats& st = stats ? *stats : local;
  st = LampParseStats{};
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  constexpr uint32_t kRoot = 0x50000000u;
  RscCursor c(image);
  const uint32_t array = c.U32(kRoot + kLightsRootList);
  const uint32_t count = c.U16(kRoot + kLightsRootList + 4);
  if (c.bad() || !c.IsPointer(array)) return fail("lights.xlm has no light list at its root +0x50");
  if (count == 0 || count > kMaxFileLights) return fail("lights.xlm's light count is out of range");
  const uint8_t* lights = c.Span(array, uint64_t(count) * kLightRecordBytes);
  if (lights == nullptr) return fail("lights.xlm's light list runs past the resource");
  st.listed = count;
  out->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* p = lights + size_t(i) * kLightRecordBytes;
    Lamp l;
    for (int a = 0; a < 3; ++a) {
      l.pos[a] = LoadBEFloat(p + 4 * a);
      l.colour[a] = LoadBEFloat(p + 16 + 4 * a);
      l.dir[a] = LoadBEFloat(p + 32 + 4 * a);
    }
    l.intensity = LoadBEFloat(p + 64);
    l.cone_deg = LoadBEFloat(p + 68);
    l.spread_deg = LoadBEFloat(p + 72);
    const uint8_t type = p[88], on = p[89];
    if (!Plausible(l, type, on)) {
      ++st.skipped;
      continue;
    }
    l.spot = type;
    l.on = on;
    l.source = LampSource::kFile;
    l.file_index = i;  // the file's own index, kept for the lights that are left out too: it is what the Director's atlas calls `i`
    (type ? st.spots : st.omni) += 1;
    if (!on) ++st.off;
    out->push_back(l);
  }
  st.kept = uint32_t(out->size());
  if (out->empty()) return fail("lights.xlm holds no plausible light");
  return true;
}

LampCensus CensusOfLamps(const std::vector<Lamp>& lamps, const LampParseStats& parse) {
  LampCensus c;
  c.parse = parse;
  c.present = true;
  c.table = LampTable::Build(lamps).Stats();
  std::map<float, uint32_t> by_value;
  for (const Lamp& l : lamps) {
    const float i = l.intensity;
    size_t bucket = 0;
    if (i <= 0.0f) bucket = 0;
    else if (i < 10.0f) bucket = 1;
    else if (i < 15.0f) bucket = 2;
    else if (i < 20.0f) bucket = 3;
    else if (i < 30.0f) bucket = 4;
    else if (i < 60.0f) bucket = 5;
    else if (i < 100.0f) bucket = 6;
    else bucket = 7;
    ++c.by_intensity[bucket];
    ++by_value[i];
  }
  for (const auto& [value, n] : by_value) {
    if (n > c.most_common_intensity_count) {
      c.most_common_intensity_count = n;
      c.most_common_intensity = value;
    }
  }
  return c;
}

LampGolden CheckLampGolden(const LampTable& table, const LampCensus& census) {
  LampGolden g;
  std::ostringstream out;
  const auto line = [&](const std::string& what, bool equal, const std::string& detail) {
    out << "  " << what << ": " << detail << (equal ? ", equal" : ", DIFFERENT") << "\n";
    g.ok = g.ok && equal;
  };
  line("lights kept", census.parse.kept == kGoldenLamps && census.parse.skipped == 0, std::to_string(census.parse.kept) + " of " + std::to_string(census.parse.listed) + ", the survey's " + std::to_string(kGoldenLamps));
  line("omni and spot", census.parse.omni == kGoldenLampOmni && census.parse.spots == kGoldenLampSpots,
       std::to_string(census.parse.omni) + " and " + std::to_string(census.parse.spots) + ", the survey's " + std::to_string(kGoldenLampOmni) + " and " + std::to_string(kGoldenLampSpots));
  std::map<float, uint32_t> by_value;
  for (const Lamp& l : table.lamps()) ++by_value[l.intensity];
  bool intensities = true;
  std::string detail;
  for (const GoldenIntensity& gi : kGoldenLampIntensities) {
    const uint32_t have = by_value.count(gi.value) ? by_value[gi.value] : 0;
    intensities = intensities && have == gi.count;
    detail += std::to_string(int(gi.value)) + ":" + std::to_string(have) + (have == gi.count ? "" : "!=" + std::to_string(gi.count)) + " ";
  }
  line("lights at each of the eight commonest intensities", intensities, detail);
  line("greatest intensity", census.table.intensity_max == kGoldenLampMaxIntensity, std::to_string(census.table.intensity_max));
  bool bounds = true;
  for (int a = 0; a < 3; ++a) {
    bounds = bounds && std::fabs(census.table.bounds_min[a] - kGoldenLampBoundsMin[a]) < 0.01f && std::fabs(census.table.bounds_max[a] - kGoldenLampBoundsMax[a]) < 0.01f;
  }
  line("bounds", bounds, "x " + std::to_string(census.table.bounds_min[0]) + " .. " + std::to_string(census.table.bounds_max[0]) + ", z " + std::to_string(census.table.bounds_min[2]) + " .. " +
                             std::to_string(census.table.bounds_max[2]));
  uint32_t used = 0, most = 0;
  for (uint32_t cz = 0; cz < table.grid().nz; ++cz) {
    for (uint32_t cx = 0; cx < table.grid().nx; ++cx) {
      uint32_t first = 0, last = 0;
      table.CellRange(cx, cz, &first, &last);
      used += last > first ? 1 : 0;
      most = std::max(most, last - first);
    }
  }
  line("the 64 m grid", table.grid().nx == kGoldenLampGridX && table.grid().nz == kGoldenLampGridZ && used == kGoldenLampCellsUsed && most == kGoldenLampCellMax,
       std::to_string(table.grid().nx) + " x " + std::to_string(table.grid().nz) + " cells, " + std::to_string(used) + " used, at most " + std::to_string(most) + " lights in one");
  g.report = out.str();
  return g;
}

std::string LampCensusReport(const LampCensus& c) {
  std::ostringstream out;
  const auto golden = [&](const char* what, uint64_t have, uint64_t want) {
    out << "  " << what << ": " << have << " (the survey's " << want << (have == want ? ", equal" : ", DIFFERENT") << ")\n";
  };
  out << "lamps (lights.xlm)";
  if (!c.present) {
    out << ": not in the archive\n";
    return out.str();
  }
  out << (c.from_mods ? " [the mod archive's]" : "") << "\n";
  golden("lights listed", c.parse.listed, kGoldenLamps);
  golden("kept", c.parse.kept, kGoldenLamps);
  golden("spots", c.parse.spots, kGoldenLampSpots);
  golden("omni", c.parse.omni, kGoldenLampOmni);
  out << "  skipped as not plausible " << c.parse.skipped << ", with the on byte clear " << c.parse.off << "\n";
  out << "  bounds x " << c.table.bounds_min[0] << " .. " << c.table.bounds_max[0] << ", y " << c.table.bounds_min[1] << " .. " << c.table.bounds_max[1] << ", z "
      << c.table.bounds_min[2] << " .. " << c.table.bounds_max[2] << "; the greatest intensity " << c.table.intensity_max << "; the commonest, " << c.most_common_intensity << ", has "
      << c.most_common_intensity_count << " lights\n";
  static const char* kNames[LampCensus::kBuckets] = {"0", "under 10", "10 to 15", "15 to 20", "20 to 30", "30 to 60", "60 to 100", "100 and more"};
  out << "  intensity:";
  for (size_t b = 0; b < LampCensus::kBuckets; ++b) out << " " << kNames[b] << "=" << c.by_intensity[b];
  out << "\n";
  return out.str();
}

}  // namespace larecomp::citypack

#include "tournament_schedule.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

namespace tournament {
namespace {

// The city's regular circuit and sprint races (city "sc", race map at the city
// config +52), keyed like the game keys them (race+148). Story missions,
// cutscene races and test tracks are left out. Names are ours: the game's own
// race titles live in the race scripts, which the menu never loads.
struct Race {
  uint32_t key;
  const char* name;
};
constexpr Race kRaces[] = {
    {0xEEE614C1, "Hollywood Circuit 1"},  // hw_circuit_jp_02_eas
    {0x68F83536, "Hollywood Circuit 2"},  // hw_circuit_gs_01_eas
    {0xA09AECD7, "Hollywood Circuit 3"},  // hw_circuit_ds_02a_ea
    {0x7399DF69, "Hollywood Circuit 4"},  // hw_circuit_mb_04a_ea
    {0x84AE0825, "Hollywood Circuit 5"},  // hw_circuit_mb_01_eas
    {0x080696C3, "Hollywood Circuit 6"},  // hw_circuit_jj_01c_ea
    {0x5C99702A, "Hollywood Circuit 7"},  // hw_circuit_jp_03_eas
    {0x5C4964ED, "Hollywood Sprint 1"},  // hw_ordered_jp_04_eas
    {0x8F92A380, "Hollywood Sprint 2"},  // hw_ordered_ds_03_eas
    {0x1FD126C9, "Hollywood Sprint 3"},  // hw_ordered_mb_01_eas
    {0x85682D6F, "Hollywood Sprint 4"},  // hw_ordered_gs_01_eas
    {0xE68BB43F, "Hollywood Sprint 5"},  // hw_ordered_mb_05_eas
    {0xF1F22102, "Hollywood Sprint 6"},  // hw_ordered_jj_01_eas
    {0x3E00A90D, "Hollywood Sprint 7"},  // hw_ordered_dh_02c_ea
    {0x47E4D393, "Hollywood Sprint 8"},  // hw_ordered_mb_03a_ea
    {0x5BD63C08, "Hollywood Sprint 9"},  // hw_ordered_ds_02_eas
    {0x8159070D, "Hollywood Sprint 10"},  // hw_ordered_ds_04_eas
    {0xB1C81AD8, "Hollywood Sprint 11"},  // hw_ordered_mb_07b_ea
    {0x7EC6302A, "Hollywood Sprint 12"},  // hw_ordered_jp_11_eas
    {0x10AF0885, "Hollywood Sprint 13"},  // hw_ordered_mb_02_eas
    {0x9C5C6B56, "Hollywood Sprint 14"},  // hw_ordered_jp_10_eas
    {0xFBDACC8E, "Hollywood Sprint 15"},  // hw_ordered_mg_01_eas
    {0xE296567A, "Beaches Circuit 1"},  // be_circuit_mg_02_easy
    {0x4479FC0E, "Beaches Circuit 2"},  // be_circuit_jj_02_easy
    {0x96DCD362, "Beaches Circuit 3"},  // be_circuit_mg_01b_easy
    {0x83BC2714, "Beaches Sprint 1"},  // be_ordered_mb_01_easy
    {0xB57C8A94, "Beaches Sprint 2"},  // be_ordered_mb_04_easy
    {0x94E979C0, "Beaches Sprint 3"},  // be_ordered_ds_02_easy
    {0xCF0C96A3, "Beaches Sprint 4"},  // be_ordered_jj_03_easy
    {0x5FD48F97, "Beaches Sprint 5"},  // be_ordered_ds_01_easy
    {0x2AB5F4FD, "Beaches Sprint 6"},  // be_ordered_mb_06_easy
    {0x95016A49, "Beaches Sprint 7"},  // be_ordered_mg_02_easy
    {0x1077A7AF, "Downtown Circuit 1"},  // dt_circuit_as_02_easy
    {0xD28A140D, "Downtown Circuit 2"},  // dt_circuit_jp_01_easy
    {0xED2DE13C, "Downtown Circuit 3"},  // dt_circuit_as_01_easy
    {0xC0D25D0E, "Downtown Circuit 4"},  // dt_circuit_mg_01_easy
    {0xB40D96E9, "Downtown Circuit 5"},  // dt_circuit_mb_01_easy
    {0x037C5460, "Downtown Sprint 1"},  // dt_ordered_mb_04_easy
    {0xF4235270, "Downtown Sprint 2"},  // dt_ordered_ds_01_easy
    {0xE81B9D9F, "Downtown Sprint 3"},  // dt_ordered_mb_02_easy
    {0xB8D53DEB, "Downtown Sprint 4"},  // dt_ordered_jp_01_easy
    {0xF54D3802, "Downtown Sprint 5"},  // dt_ordered_mb_01_easy
    {0x17867B5C, "Downtown Sprint 6"},  // dt_ordered_jp_02_easy
    {0x12DAF31D, "Downtown Sprint 7"},  // dt_ordered_mb_03_easy
    {0x667E255D, "Downtown Sprint 8"},  // dt_ordered_ts_01_easy
    {0x8558368E, "Hills Circuit 1"},  // hl_circuit_ds_01_easy
    {0x11280218, "Hills Circuit 2"},  // hl_circuit_jj_02_easy
    {0x236FA6A7, "Hills Circuit 3"},  // hl_circuit_jj_01_easy
    {0xA099BFCE, "Hills Sprint 1"},  // hl_ordered_ts_01_easy
    {0x278EDA1F, "Hills Sprint 2"},  // hl_ordered_jj_03_easy
    {0xADAE59F7, "Hills Sprint 3"},  // hl_ordered_ts_04_easy
    {0x82CE68CE, "Hills Sprint 4"},  // hl_ordered_mb_03_easy
    {0xB72EF965, "Hills Sprint 5"},  // hl_ordered_jj_01_easy
    {0xB42E66F7, "Hills Sprint 6"},  // hl_ordered_ts_03_easy
    {0x89D29240, "Hills Sprint 7"},  // hl_ordered_ts_02_easy
    {0xD6C32C20, "Hills Sprint 8"},  // hl_ordered_ts_05_easy
    {0x53972E4E, "South Central Circuit 1"},  // sc_circuit_cb_01_
    {0xD1D74704, "South Central Circuit 2"},  // sc_circuit_jj_01b
    {0xB635F38A, "South Central Circuit 3"},  // sc_circuit_cb_02_
    {0xB4827272, "South Central Circuit 4"},  // sc_circuit_jp_01_
    {0x1095C03B, "South Central Circuit 5"},  // sc_circuit_ds_01_
    {0xB7A9D1A9, "South Central Circuit 6"},  // sc_circuit_cb_03b
    {0xFC13917A, "South Central Circuit 7"},  // sc_circuit_dh_01_
    {0x15D4F8BD, "South Central Sprint 1"},  // sc_ordered_mb_01_
    {0xD876A499, "South Central Sprint 2"},  // sc_ordered_cb_02_
    {0xF2D1A4A7, "South Central Sprint 3"},  // sc_ordered_jp_02_
    {0x3341B396, "South Central Sprint 4"},  // sc_ordered_mb_03_
    {0x038ED431, "South Central Sprint 5"},  // sc_ordered_mb_02_
    {0xE48B881B, "South Central Sprint 6"},  // sc_ordered_jp_01_
    {0xBEF6C261, "South Central Sprint 7"},  // sc_ordered_dh_11_
    {0xB728C811, "South Central Sprint 8"},  // sc_ordered_dh_02_
    {0xED4B3425, "South Central Sprint 9"},  // sc_ordered_dh_01_
    {0xA36A9243, "South Central Sprint 10"},  // sc_ordered_ds_01b
    {0x5841FD96, "South Central Sprint 11"},  // sc_ordered_mb_04_
    {0xAEE3B787, "South Central Sprint 12"},  // sc_ordered_dh_03_
    {0x0194422C, "South Central Sprint 13"},  // sc_ordered_jp_03_
};
constexpr uint32_t kRaceCount = sizeof(kRaces) / sizeof(kRaces[0]);

// Leaderboard slots the game accepts as a tournament table (sub_822D0438).
constexpr uint32_t kSlotCount = 27 + 31;
uint32_t SlotId(uint32_t i) {
  i %= kSlotCount;
  return i < 27 ? 13 + i : 351 + (i - 27);
}
bool ValidSlot(uint32_t id) { return (id >= 13 && id < 40) || (id >= 351 && id < 382); }

// Weeks start on Monday 00:00 UTC; 1970-01-05 was the first Monday.
constexpr int64_t kWeek = 7 * 24 * 3600;
constexpr int64_t kFirstMonday = 4 * 24 * 3600;

// Consecutive weeks step through the races 29 at a time (29 and the race
// count share no factor), so the district changes from one week to the next
// and every race comes up once per cycle.
constexpr uint32_t kRaceStride = 29;

void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool ParseU32(const std::string& s, uint32_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, 0);
  if (!end || *end) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

// race = a race key (0x...), or a race name from the table above.
bool ParseRace(const std::string& s, Definition& d) {
  for (const Race& r : kRaces) {
    if (s == r.name) {
      d.race_key = r.key;
      d.name = r.name;
      return true;
    }
  }
  uint32_t key = 0;
  if (!ParseU32(s, key) || key == 0) return false;
  d.race_key = key;
  for (const Race& r : kRaces)
    if (r.key == key) d.name = r.name;
  return true;
}

void ApplyOverrides(const std::filesystem::path& file, Definition& d) {
  std::ifstream in(file);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = Trim(line.substr(0, eq));
    const std::string v = Trim(line.substr(eq + 1));
    uint32_t u = 0;
    if (k == "race") {
      ParseRace(v, d);
    } else if (k == "slot") {
      if (ParseU32(v, u) && ValidSlot(u)) d.table_id = u;
    } else if (k == "restriction_type") {
      if (ParseU32(v, u) && u <= 2) d.restriction_type = u;
    } else if (k == "restriction_data") {
      if (ParseU32(v, u)) d.restriction_data = u;
    } else if (k == "name") {
      if (!v.empty()) d.name = v;
    }
  }
}

}  // namespace

Definition Current(int64_t now, const std::filesystem::path& dir) {
  Definition d;
  const int64_t since = std::max<int64_t>(now - kFirstMonday, 0);
  d.week = static_cast<uint32_t>(since / kWeek);
  d.ends = kFirstMonday + (static_cast<int64_t>(d.week) + 1) * kWeek;
  const Race& race = kRaces[(static_cast<uint64_t>(d.week) * kRaceStride) % kRaceCount];
  d.race_key = race.key;
  d.name = race.name;
  d.table_id = SlotId(d.week);
  if (!dir.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(dir / "tournament.txt", ec)) ApplyOverrides(dir / "tournament.txt", d);
  }
  return d;
}

std::vector<uint8_t> EncodeGuest(const Definition& d) {
  std::vector<uint8_t> b(kDefinitionSize, 0);
  StoreBe32(b.data() + 0, d.version);
  StoreBe32(b.data() + 4, d.table_id);
  StoreBe32(b.data() + 8, d.restriction_type);
  StoreBe32(b.data() + 12, d.restriction_data);
  StoreBe32(b.data() + 16, d.race_key);
  // One "en:" entry: the menu matches its language's two-letter prefix and
  // otherwise shows the first entry past the prefix, so every language gets it.
  const std::string text = "en:" + d.name;
  std::memcpy(b.data() + 20, text.data(), std::min<size_t>(text.size(), 255));
  return b;
}

}  // namespace tournament

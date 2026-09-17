#include <rex/ppc/context.h>

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "discord_rpc/discord_rpc.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/runtime.h>

#include "mc_engine/hooks.h"
#include "mc_engine/logging.h"
#include "mc_engine/pause_menu.h"

namespace {

uint8_t* GetMembase() {
  return rex::Runtime::instance()->virtual_membase();
}

uint32_t ReadGuestBE32(uint32_t guest_addr) {
  auto base = GetMembase();
  auto p = base + guest_addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t ReadGuestBE16(uint32_t guest_addr) {
  auto base = GetMembase();
  auto p = base + guest_addr;
  return static_cast<uint16_t>((uint32_t(p[0]) << 8) | uint32_t(p[1]));
}

// The guest heap and the image both live above 0x82000000; anything below is
// either a null or a small integer that got mistaken for a pointer.
bool IsGuestPtr(uint32_t ea) {
  return ea >= 0x82000000u && ea < 0xC0000000u;
}

// LocalOptions global: 0x8288E920
// +0x540: current vehicle profile index (uint32)
// +0x550: start of vehicle profile array
// Each profile: 8176 (0x1FF0) bytes
//   +0x1FAD: TypeName  (32-byte C string, internal ID)
//   +0x1FCD: ProfileName (34-byte C string, display name)
constexpr uint32_t kLocalOptions = 0x8288E920;
constexpr uint32_t kProfileIndexOff = 0x540;
constexpr uint32_t kProfileArrayOff = 0x550;
constexpr uint32_t kProfileSize = 0x1FF0;
constexpr uint32_t kProfileNameOff = 0x1FCD;
constexpr uint32_t kTypeNameOff = 0x1FAD;

const char* RaceSubtypeLabel(const char* state_name) {
  if (std::strcmp(state_name, "Racing") == 0) return nullptr;
  if (std::strcmp(state_name, "Racing_Local") == 0) return RpcTr(RpcStr::RaceStreet);
  if (std::strcmp(state_name, "Racing_Mission") == 0) return RpcTr(RpcStr::RaceMission);
  if (std::strcmp(state_name, "Racing_Series") == 0) return RpcTr(RpcStr::RaceSeries);
  if (std::strcmp(state_name, "Racing_Tournament") == 0) return RpcTr(RpcStr::RaceTournament);
  if (std::strcmp(state_name, "Racing_Wager") == 0) return RpcTr(RpcStr::RaceWager);
  if (std::strcmp(state_name, "Racing_TimeTrial") == 0) return RpcTr(RpcStr::RaceTimeTrial);
  if (std::strcmp(state_name, "Racing_Delivery") == 0) return RpcTr(RpcStr::RaceDelivery);
  if (std::strcmp(state_name, "Racing_Payback") == 0) return RpcTr(RpcStr::RacePayback);
  if (std::strcmp(state_name, "Racing_RedLight") == 0) return RpcTr(RpcStr::RaceRedLight);
  if (std::strcmp(state_name, "Racing_Freeway") == 0) return RpcTr(RpcStr::RaceFreeway);
  if (std::strcmp(state_name, "Racing_BeatMeThere") == 0) return RpcTr(RpcStr::RaceBeatMeThere);
  if (std::strcmp(state_name, "Racing_Online") == 0) return RpcTr(RpcStr::RaceOnline);
  return nullptr;
}

void GetCurrentVehicleName(char* out, size_t out_size) {
  auto base = GetMembase();
  uint32_t index = ReadGuestBE32(kLocalOptions + kProfileIndexOff);
  uint32_t profile = kLocalOptions + kProfileArrayOff + kProfileSize * index;

  const char* profile_name =
      reinterpret_cast<const char*>(base + profile + kProfileNameOff);
  if (profile_name[0] != '\0') {
    std::snprintf(out, out_size, "%s", profile_name);
    return;
  }

  const char* type_name =
      reinterpret_cast<const char*>(base + profile + kTypeNameOff);
  if (type_name[0] != '\0' && std::strcmp(type_name, "blank") != 0) {
    std::snprintf(out, out_size, "%s", type_name);
    return;
  }

  out[0] = '\0';
}

// District index (from Hook_CaptureDistrict) -> display name. nullptr for
// Unknown/unseen so the caller can omit the suffix.
const char* DistrictName(int idx) {
  switch (idx) {
    case 0: return "Hollywood";
    case 1: return "Beaches";
    case 2: return "Hills";
    case 3: return "Downtown";
    case 4: return "South Central";
    default: return nullptr;  // 5 = Unknown, -1 = not seen yet
  }
}

// ---------------------------------------------------------------------------
// Live race identity + series/tournament standings
// ---------------------------------------------------------------------------
// The state name only ever gave the race *kind* ("Racing_Series"). The race the
// player is actually running, and how they stand in a multi-race event, are
// reachable from the live race object, so the presence can name both.
//
// The road is the one the game's own race-info HUD walks (guest sub_82640AE0,
// "GetRaceInfo"), replicated here as plain guest-memory reads -- every step is a
// pointer chase or a RAGE hash-map probe, so nothing has to call into the guest:
//
//   race          = *0x8286EBF8                 the live race (0 outside one)
//   race+8        = race index into the city tables
//   race+3088     = racer count
//   race+3100     = race type enum   (Circuit / Ordered / TimeTrial / ...)
//   race+3104     = race subtype     (3 = series, 4 = tournament)
//   city          = mcConfig::LookupCity()      guest sub_8238EC00
//   city+36       = char*[] of localization keys, one per race index
//   *(city+36)[i] = "game/Race/Downtown/Ordered/dt_ordered_mb_03"
//
// That key resolves through the string table (mgr = *0x8286D7FC, display map at
// mgr+16) to the name the game itself shows -- in whatever language the game is
// running, which is exactly what the presence wants.
//
// The standings do not live in the race object: the game keeps them in the
// Scaleform results movie, written per race by sub_826CAD48 (series) and
// sub_826CAF18 (tournament). Racer slot 0 is the player -- guest sub_826CA6D8
// ranks the player by comparing nRacerPoints_0 against every other slot.
//   Series_RESULTS:      nCurrentRace, nRacesToWin, nRacerWins_0
//   Tournament_RESULTS:  nCurrentRaceIndex (0-based), nTotalRaces, nRacerPoints_0

constexpr uint32_t kCurrentRace = 0x8286EBF8;  // mcRace* (live race, else null)
constexpr uint32_t kCityNameVar = 0x82879330;  // char*  (city being played)
constexpr uint32_t kCityArray = 0x828CCE98;    // mcCityConfig[] base
constexpr uint32_t kCityCount = 0x828CCE9C;    // u16 city count
constexpr uint32_t kCityStride = 80;
constexpr uint32_t kStringTableMgr = 0x8286D7FC;
constexpr uint32_t kUiRoot = 0x8286D804;
constexpr uint32_t kRaceTypeNames = 0x827E930C;  // char*[] indexed by race type

// The race object is also poisoned with 0xFFFFFFF8 between races; the guest's
// own race-over handlers test for exactly that, so we do too.
constexpr uint32_t kRacePoison = 0xFFFFFFF8;

std::string GuestStr(uint32_t ea, size_t maxlen = 160) {
  if (!IsGuestPtr(ea)) return {};
  const char* p = reinterpret_cast<const char*>(GetMembase() + ea);
  size_t n = 0;
  while (n < maxlen && p[n]) ++n;
  return std::string(p, n);
}

// RAGE string hash, guest sub_821C9790. Lowercases A-Z, folds '\' to '/'. Keys
// both the string table and the UI element registry.
uint32_t RageHash(const char* s) {
  uint32_t h = 0;
  for (; *s; ++s) {
    uint8_t c = static_cast<uint8_t>(*s);
    if (c >= 'A' && c <= 'Z')
      c += 32;
    else if (c == '\\')
      c = '/';
    const uint32_t m = 1025u * (static_cast<uint32_t>(c) + h);
    h = (m >> 6) ^ m;
  }
  const uint32_t m2 = 9u * h;
  return 32769u * ((m2 >> 11) ^ m2);
}

// RAGE hash map probe, guest sub_826BDDB0: map+0 = bucket array, map+4 = u16
// bucket count, entry = {key, value, next}. Returns the *value* address (what
// the guest function hands back), or 0.
uint32_t HashMapFind(uint32_t map, uint32_t key) {
  const uint16_t buckets = ReadGuestBE16(map + 4);
  const uint32_t bucket_array = ReadGuestBE32(map + 0);
  if (!buckets || !IsGuestPtr(bucket_array)) return 0;
  uint32_t entry = ReadGuestBE32(bucket_array + 4u * (key % buckets));
  while (IsGuestPtr(entry)) {
    if (ReadGuestBE32(entry + 0) == key) return entry + 4;
    entry = ReadGuestBE32(entry + 8);
  }
  return 0;
}

// mcConfig::LookupCity (guest sub_8238EC00): linear scan of the city array for
// the one whose name matches the city global, case-insensitively.
uint32_t LookupCity() {
  const uint16_t count = ReadGuestBE16(kCityCount);
  const uint32_t array = ReadGuestBE32(kCityArray);
  const std::string want = GuestStr(ReadGuestBE32(kCityNameVar), 64);
  if (!count || !IsGuestPtr(array) || want.empty()) return 0;
  for (uint16_t i = 0; i < count; ++i) {
    const uint32_t entry = array + kCityStride * i;
    const std::string name = GuestStr(ReadGuestBE32(entry + 0), 64);
    if (name.size() != want.size()) continue;
    size_t k = 0;
    for (; k < name.size(); ++k) {
      char a = name[k], b = want[k];
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
      if (a != b) break;
    }
    if (k == name.size()) return entry;
  }
  return 0;
}

// txtStringTable::Get (guest sub_825F2770 -> sub_82605E38). The string table
// carries its OWN map type, not the one sub_826BDDB0 walks:
//   map+0 = u32 bucket count   map+4 = bucket array   map+8 = entry count
//   node  = {hash, value, next}
// and the value is a 24-byte string entry whose text pointer sits at +8.
//
// Worth spelling out, because the near miss costs an evening: mgr+16 is a
// second map, and it is NOT this one. That one caches the *expanded* display
// buffer (sub_822184E8 allocates it empty, sub_822180B0 fills it in only once
// the game itself asks for the key), so probing it for a key the UI has not
// rendered yet hands back an empty string rather than the name. The table below
// is the loaded data itself and is populated for every key in the file.
uint32_t StringTableText(uint32_t hash) {
  const uint32_t mgr = ReadGuestBE32(kStringTableMgr);
  if (!IsGuestPtr(mgr)) return 0;
  const uint32_t map = ReadGuestBE32(mgr + 4);
  if (!IsGuestPtr(map)) return 0;
  const uint32_t buckets = ReadGuestBE32(map + 0);
  const uint32_t bucket_array = ReadGuestBE32(map + 4);
  if (!buckets || !IsGuestPtr(bucket_array)) return 0;

  uint32_t node = ReadGuestBE32(bucket_array + 4u * (hash % buckets));
  for (int guard = 0; IsGuestPtr(node) && guard < 4096; ++guard) {
    if (ReadGuestBE32(node + 0) == hash) {
      const uint32_t entry = ReadGuestBE32(node + 4);
      return IsGuestPtr(entry) ? ReadGuestBE32(entry + 8) : 0;
    }
    node = ReadGuestBE32(node + 8);
  }
  return 0;
}

// Localized text for a string-table key. The .strtbl holds UTF-16, but the
// loader narrows it to 8 bits on the way in (sub_825F2200's a5==1 path), which
// is why every consumer downstream -- sub_82217F00, sub_822180B0 -- walks it a
// byte at a time. Latin-1 out to UTF-8, so accented text survives the trip to
// Discord. Empty when the key is absent: the guest's own miss path renders
// "!!hash!!", which is debug furniture we would rather not broadcast.
std::string LocalizedString(const char* key) {
  if (!key || !*key) return {};
  const uint32_t text = StringTableText(RageHash(key));
  if (!IsGuestPtr(text)) return {};

  const uint8_t* p = GetMembase() + text;
  std::string out;
  for (size_t i = 0; i < 160 && p[i]; ++i) {
    const uint8_t c = p[i];
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

// mcUIManager::FindElement (guest sub_821FA230): hash the element name, look it
// up in the registry map, and index the element table with the stored slot.
uint32_t FindUiElement(const char* name) {
  const uint32_t root = ReadGuestBE32(kUiRoot);
  if (!IsGuestPtr(root)) return 0;
  const uint32_t ui = ReadGuestBE32(root + 52);
  if (!IsGuestPtr(ui)) return 0;
  const uint32_t reg = ui + 4;
  const uint32_t value = HashMapFind(reg + 88, RageHash(name));
  if (!value) return 0;
  const int32_t slot = static_cast<int32_t>(ReadGuestBE32(value));
  const uint32_t table = ReadGuestBE32(reg + 76);
  if (slot < 0 || !IsGuestPtr(table)) return 0;
  return ReadGuestBE32(table + 4u * static_cast<uint32_t>(slot));
}

// mcVariant lookup on a movie (guest sub_8268DA78) without the parent walk:
// element+28 heads a singly linked list of {type, value, next, char* name}.
// Returns false when the movie does not carry the variable (yet).
bool UiVarInt(uint32_t element, const char* name, int32_t* out) {
  if (!IsGuestPtr(element)) return false;
  uint32_t node = ReadGuestBE32(element + 28);
  for (int guard = 0; IsGuestPtr(node) && guard < 512; ++guard) {
    if (GuestStr(ReadGuestBE32(node + 12), 64) == name) {
      *out = static_cast<int32_t>(ReadGuestBE32(node + 4));
      return true;
    }
    node = ReadGuestBE32(node + 8);
  }
  return false;
}

// What the presence knows about the race in progress. Rebuilt by RpcOnRaceTick.
struct RaceInfo {
  bool valid = false;
  int index = -1;    // race index into the city tables, for the diagnostic log
  std::string key;   // string-table key, kept so a lookup miss is legible
  std::string name;  // localized race name, e.g. "FIGUEROA AND GRAND"
  std::string type;  // localized race type, e.g. "CIRCUIT RACE"
  int subtype = 0;   // race+3104: 3 = series, 4 = tournament
  int series_wins = 0;
  int series_target = 0;
  int series_race = 0;
  int tour_race = 0;   // 1-based
  int tour_total = 0;
  int tour_points = 0;
};

RaceInfo g_race;

// Read the live race object. Everything is best-effort: a field we cannot reach
// simply stays empty and the presence falls back to the old generic wording.
RaceInfo ReadRaceInfo() {
  RaceInfo info;
  if (!rex::Runtime::instance() || !GetMembase()) return info;

  const uint32_t race = ReadGuestBE32(kCurrentRace);
  if (!IsGuestPtr(race) || race == kRacePoison) return info;

  const int32_t index = static_cast<int32_t>(ReadGuestBE32(race + 8));
  info.index = index;
  info.subtype = static_cast<int32_t>(ReadGuestBE32(race + 3104));

  const uint32_t type_index = ReadGuestBE32(race + 3100);
  if (type_index < 32) {
    const std::string type_name = GuestStr(ReadGuestBE32(kRaceTypeNames + 4 * type_index), 64);
    if (!type_name.empty() && type_name != "Unknown")
      info.type = LocalizedString(type_name.c_str());
  }

  if (index >= 0) {
    const uint32_t city = LookupCity();
    if (city) {
      const uint32_t keys = ReadGuestBE32(city + 36);
      if (IsGuestPtr(keys)) {
        info.key = GuestStr(ReadGuestBE32(keys + 4u * uint32_t(index)));
        if (!info.key.empty()) info.name = LocalizedString(info.key.c_str());
      }
    }
  }

  // Standings. Gated on the subtype so a stale results movie left over from a
  // previous event cannot leak its numbers into an unrelated race.
  if (info.subtype == 3) {
    const uint32_t movie = FindUiElement("Series_RESULTS");
    int32_t v = 0;
    if (UiVarInt(movie, "nRacerWins_0", &v)) info.series_wins = v;
    if (UiVarInt(movie, "nRacesToWin", &v)) info.series_target = v;
    if (UiVarInt(movie, "nCurrentRace", &v)) info.series_race = v;
  } else if (info.subtype == 4) {
    const uint32_t movie = FindUiElement("Tournament_RESULTS");
    int32_t v = 0;
    if (UiVarInt(movie, "nCurrentRaceIndex", &v)) info.tour_race = v + 1;
    if (UiVarInt(movie, "nTotalRaces", &v)) info.tour_total = v;
    if (UiVarInt(movie, "nRacerPoints_0", &v)) info.tour_points = v;
  }

  info.valid = true;
  return info;
}

// The standings line, or empty when this race is not part of an event.
std::string RaceProgressText(const RaceInfo& info) {
  char buf[96];
  if (info.subtype == 4 && info.tour_total > 0) {
    std::snprintf(buf, sizeof(buf), RpcTr(RpcStr::TournamentProgressFmt),
                  info.tour_race, info.tour_total, info.tour_points);
    return buf;
  }
  if (info.subtype == 3) {
    if (info.series_target > 0) {
      std::snprintf(buf, sizeof(buf), RpcTr(RpcStr::SeriesProgressFmt),
                    info.series_wins, info.series_target);
      return buf;
    }
    if (info.series_race > 0) {
      std::snprintf(buf, sizeof(buf), RpcTr(RpcStr::SeriesRaceFmt), info.series_race);
      return buf;
    }
  }
  return {};
}

// Current meaningful activity + last district, so a district change (while
// driving) can rebuild the presence without re-reading the state object.
LarecompDiscordState g_activity = LarecompDiscordState::Unknown;
int g_district = -1;
char g_race_details[128] = {};

// Rebuild the Free Roam / Race presence with the current district suffix.
void ApplyAreaPresence() {
  const char* dist = DistrictName(g_district);
  if (g_activity == LarecompDiscordState::FreeRoam) {
    char state_text[96];
    if (dist) {
      std::snprintf(state_text, sizeof(state_text), RpcTr(RpcStr::FreeRoamAreaFmt), dist);
    } else {
      std::snprintf(state_text, sizeof(state_text), "%s", RpcTr(RpcStr::FreeRoamState));
    }
    char vehicle[64];
    GetCurrentVehicleName(vehicle, sizeof(vehicle));
    if (vehicle[0] != '\0') {
      char details[128];
      std::snprintf(details, sizeof(details), RpcTr(RpcStr::DrivingFmt), vehicle);
      LARECOMP_Discord_SetStateText(LarecompDiscordState::FreeRoam, details, state_text);
    } else {
      LARECOMP_Discord_SetStateText(LarecompDiscordState::FreeRoam,
                                    RpcTr(RpcStr::FreeRoamGeneric), state_text);
    }
  } else if (g_activity == LarecompDiscordState::Race) {
    // Top line names the race itself once we have read it; the subtype label
    // captured at state activation is the fallback for the frames before that
    // and for races whose key is missing from the string table.
    char details[160];
    if (!g_race.name.empty()) {
      std::snprintf(details, sizeof(details), RpcTr(RpcStr::RacingFmt), g_race.name.c_str());
    } else if (g_race_details[0]) {
      std::snprintf(details, sizeof(details), "%s", g_race_details);
    } else {
      std::snprintf(details, sizeof(details), "%s", RpcTr(RpcStr::RaceGeneric));
    }

    // Bottom line: race type, event standings, district -- whichever we have.
    std::string state_text = g_race.type.empty() ? RpcTr(RpcStr::RaceStateGeneric) : g_race.type;
    const std::string progress = RaceProgressText(g_race);
    if (!progress.empty()) state_text += " \xC2\xB7 " + progress;
    if (dist) state_text += std::string(" \xC2\xB7 ") + dist;

    LARECOMP_Discord_SetStateText(LarecompDiscordState::Race, details, state_text);
  }
}

}  // namespace
#endif

// Hook at 0x8268DDD8 inside sub_8268DD70 (core vhsmState activation).
// r31 = state object guest address being activated.
// Reads the state name from guest memory (+20) and updates Discord RPC.
void RpcHook_StateActivate(PPCRegister& r31) {
#ifndef REXGLUE_HAS_XEO3_TARGET
  auto base = GetMembase();
  uint32_t state_guest_addr = static_cast<uint32_t>(r31.u64);

  uint32_t name_ptr = ReadGuestBE32(state_guest_addr + 20);
  if (!name_ptr) return;

  const char* name = reinterpret_cast<const char*>(base + name_ptr);

  // Shared signal: the garage carbon entry has no command of its own, and
  // activation is the only thing that fires when a menu item is picked.
  CarbonOnStateActivate(name, state_guest_addr);

  if (std::strncmp(name, "Cruising", 8) == 0) {
    g_activity = LarecompDiscordState::FreeRoam;
    ApplyAreaPresence();
  } else if (std::strncmp(name, "Racing", 6) == 0) {
    g_activity = LarecompDiscordState::Race;
    const char* subtype = RaceSubtypeLabel(name);
    if (subtype) {
      std::snprintf(g_race_details, sizeof(g_race_details), RpcTr(RpcStr::RacingFmt), subtype);
    } else {
      g_race_details[0] = '\0';
    }
    ApplyAreaPresence();
  } else if (std::strcmp(name, "Garage") == 0) {
    g_activity = LarecompDiscordState::Garage;
    char vehicle[64];
    GetCurrentVehicleName(vehicle, sizeof(vehicle));
    if (vehicle[0] != '\0') {
      char state_text[128];
      std::snprintf(state_text, sizeof(state_text), RpcTr(RpcStr::CustomizingFmt), vehicle);
      LARECOMP_Discord_SetStateText(LarecompDiscordState::Garage, RpcTr(RpcStr::InGarage), state_text);
    } else {
      LARECOMP_Discord_SetGarage();
    }
  } else if (std::strcmp(name, "Loading") == 0) {
    g_activity = LarecompDiscordState::Loading;
    LARECOMP_Discord_SetLoading();
  } else if (std::strcmp(name, "Intro") == 0) {
    g_activity = LarecompDiscordState::MainMenu;
    LARECOMP_Discord_SetMainMenu();
  }

  // Leaving the race drops what we read about it, so a finished race's name
  // cannot follow the player back through the menus. Keyed on the activity, not
  // on the state name: plenty of states activate mid-race and none of them mean
  // the race ended.
  if (g_activity != LarecompDiscordState::Race) g_race = RaceInfo{};
#endif
}

// Called from the district hook (Hook_CaptureDistrict). Updates the area live
// while driving: only rebuilds when the district actually changes and only when
// the current activity is Free Roam or a Race.
void RpcOnDistrictChanged(int district_idx) {
#ifndef REXGLUE_HAS_XEO3_TARGET
  if (district_idx == g_district) {
    return;
  }
  g_district = district_idx;
  if (g_activity == LarecompDiscordState::FreeRoam ||
      g_activity == LarecompDiscordState::Race) {
    ApplyAreaPresence();
  }
#endif
}

// Per-frame from Patch_DeltaTimePre. The race name is known only once the race
// object exists (a few frames after the Racing state activates) and the series /
// tournament standings move between races, so both have to be re-read rather
// than latched at activation. Sampled at 1 Hz: the reads are a handful of
// pointer chases plus two hash probes, and LARECOMP_Discord_SetStateText already
// drops an update whose text has not changed, so Discord only sees real moves.
void RpcOnRaceTick() {
#ifndef REXGLUE_HAS_XEO3_TARGET
  if (g_activity != LarecompDiscordState::Race) return;

  static int countdown = 0;
  if (--countdown > 0) return;
  countdown = 60;

  const RaceInfo info = ReadRaceInfo();
  if (!info.valid) return;

  if (info.name == g_race.name && info.type == g_race.type &&
      info.subtype == g_race.subtype && info.series_wins == g_race.series_wins &&
      info.series_target == g_race.series_target && info.series_race == g_race.series_race &&
      info.tour_race == g_race.tour_race && info.tour_total == g_race.tour_total &&
      info.tour_points == g_race.tour_points) {
    return;
  }

  // One line per race, so a miss is diagnosable from the log alone: an empty
  // key means the city table did not resolve, a key with an empty name means
  // the string table did not have it.
  if (info.index != g_race.index) {
    MC_INFO("[rpc-race] idx={} subtype={} key='{}' name='{}' type='{}'",
            info.index, info.subtype, info.key, info.name, info.type);
  }

  g_race = info;
  ApplyAreaPresence();
#endif
}

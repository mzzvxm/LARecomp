#include <rex/ppc/context.h>

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "discord_rpc/discord_rpc.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/runtime.h>

#include "mc_engine/hooks.h"
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
    char state_text[96];
    if (dist) {
      std::snprintf(state_text, sizeof(state_text), RpcTr(RpcStr::RaceAreaFmt), dist);
    } else {
      std::snprintf(state_text, sizeof(state_text), "%s", RpcTr(RpcStr::RaceStateGeneric));
    }
    LARECOMP_Discord_SetStateText(LarecompDiscordState::Race,
                                  g_race_details[0] ? g_race_details : RpcTr(RpcStr::RaceGeneric),
                                  state_text);
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

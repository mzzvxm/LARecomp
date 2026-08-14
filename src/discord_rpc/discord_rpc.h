#pragma once

#include <cstdint>
#include <string_view>

enum class LarecompDiscordState : uint32_t {
  Boot = 0,
  Loading,
  MainMenu,
  Garage,
  FreeRoam,
  Race,
  Pause,
  Credits,
  Unknown,
};

void LARECOMP_Discord_Init();
void LARECOMP_Discord_Shutdown();

void LARECOMP_Discord_SetState(LarecompDiscordState state);
void LARECOMP_Discord_SetStateText(LarecompDiscordState state,
                                   std::string_view details,
                                   std::string_view state_text);

void LARECOMP_Discord_SetLoading();
void LARECOMP_Discord_SetMainMenu();
void LARECOMP_Discord_SetGarage();
void LARECOMP_Discord_SetFreeRoam();
void LARECOMP_Discord_SetRace();
void LARECOMP_Discord_SetPause();
void LARECOMP_Discord_SetCredits();

// -----------------------------------------------------------------------------
// i18n for Rich Presence text. Discord does NOT translate presence -- it shows
// the literal string to everyone -- so we localize here. Language follows the
// cvar larecomp_discord_rpc_language ("auto" = follow the game's user_language).
// Add a language by appending a column to the table in discord_rpc.cpp.
// -----------------------------------------------------------------------------
enum class RpcStr {
  DrivingFmt,        // "Dirigindo: %s"
  FreeRoamGeneric,   // details fallback in free roam
  FreeRoamState,     // state text, no district
  FreeRoamAreaFmt,   // state text with district, "%s"
  RacingFmt,         // "Em Corrida: %s"
  RaceStateGeneric,  // race state text, no district
  RaceAreaFmt,       // race state text with district, "%s"
  RaceGeneric,       // race details fallback
  CustomizingFmt,    // "Customizando: %s"
  InGarage,
  // Race subtypes
  RaceStreet, RaceMission, RaceSeries, RaceTournament, RaceWager, RaceTimeTrial,
  RaceDelivery, RacePayback, RaceRedLight, RaceFreeway, RaceBeatMeThere, RaceOnline,
  // Per-state details (top line)
  DetBoot, DetLoading, DetMainMenu, DetGarage, DetFreeRoam, DetRace, DetPause, DetCredits, DetDefault,
  // Per-state text (bottom line)
  TxtBoot, TxtLoading, TxtMainMenu, TxtGarage, TxtFreeRoam, TxtRace, TxtPause, TxtCredits, TxtDefault,
  Count
};

// Localized string for the current RPC language.
const char* RpcTr(RpcStr key);

// Called (from the district hook) with the player's current district index.
// Updates the Rich Presence area live while in Free Roam / a Race. Cheap: it
// only rebuilds when the district actually changes.
void RpcOnDistrictChanged(int district_idx);

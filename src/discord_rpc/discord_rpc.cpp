#ifndef REXGLUE_HAS_XEO3_TARGET
#include "discord_rpc/discord_rpc.h"

#include <atomic>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include "discord_rpc/rpc_client.h"

// -----------------------------------------------------------------------------
// Notes
// -----------------------------------------------------------------------------
//
// The IPC client lives in rpc_client.cpp, vendored from the ReXGlue SDK so this
// builds against the stock SDK (which has no rex/discord_rpc.h). Everything
// below is the game-side mapping: which state the game is in, and how it reads
// in each language.
//
// -----------------------------------------------------------------------------

REXCVAR_DEFINE_BOOL(larecomp_discord_rpc, true, "Discord",
                    "Enable Discord Rich Presence.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(larecomp_discord_rpc_log_state, true, "Discord",
                    "Log Discord RPC state changes with printf.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(larecomp_discord_large_image, "mcla_logo", "Discord",
                      "Discord RPC large image key.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(larecomp_discord_rpc_language, "auto", "Discord",
                      "Rich Presence language. 'auto' follows the game (user_language); or force "
                      "en / pt / es. Discord itself does not translate presence text.")
    .allowed({"auto", "en", "pt", "es"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Localization table. Columns: EN, PT, ES (fallback EN). Add a language by
// appending a column here + a case in the resolver + a value in RpcStr use.
const char* RpcTr(RpcStr key) {
  enum { EN = 0, PT = 1, ES = 2, LANGS = 3 };
  static const char* const T[static_cast<int>(RpcStr::Count)][LANGS] = {
      /* DrivingFmt       */ {"Driving: %s", "Dirigindo: %s", "Conduciendo: %s"},
      /* FreeRoamGeneric  */ {"Cruising Los Angeles", "Passeando por Los Angeles", "Paseando por Los Angeles"},
      /* FreeRoamState    */ {"Cruise / Free Roam", "Cruise / Free Roam", "Cruise / Free Roam"},
      /* FreeRoamAreaFmt  */ {"Free Roam \xC2\xB7 %s", "Free Roam \xC2\xB7 %s", "Free Roam \xC2\xB7 %s"},
      /* RacingFmt        */ {"Racing: %s", "Em Corrida: %s", "En Carrera: %s"},
      /* RaceStateGeneric */ {"In a race", "Corrida ativa", "Carrera activa"},
      /* RaceAreaFmt      */ {"Racing \xC2\xB7 %s", "Corrida ativa \xC2\xB7 %s", "Carrera activa \xC2\xB7 %s"},
      /* RaceGeneric      */ {"Racing", "Em corrida", "En carrera"},
      /* CustomizingFmt   */ {"Customizing: %s", "Customizando: %s", "Personalizando: %s"},
      /* InGarage         */ {"In the garage", "Na garagem", "En el garaje"},
      /* RaceStreet       */ {"Street Race", "Corrida de Rua", "Carrera Callejera"},
      /* RaceMission      */ {"Mission Race", "Corrida de Miss\xC3\xA3o", "Carrera de Misi\xC3\xB3n"},
      /* RaceSeries       */ {"Series", "S\xC3\xA9rie", "Serie"},
      /* RaceTournament   */ {"Tournament", "Torneio", "Torneo"},
      /* RaceWager        */ {"Wager", "Aposta", "Apuesta"},
      /* RaceTimeTrial    */ {"Time Trial", "Contra-Rel\xC3\xB3gio", "Contrarreloj"},
      /* RaceDelivery     */ {"Delivery", "Entrega", "Entrega"},
      /* RacePayback      */ {"Payback", "Payback", "Payback"},
      /* RaceRedLight     */ {"Red Light", "Sinal Vermelho", "Sem\xC3\xA1""foro en Rojo"},
      /* RaceFreeway      */ {"Freeway", "Rodovia", "Autopista"},
      /* RaceBeatMeThere  */ {"Beat Me There", "Chega Primeiro", "Llega Primero"},
      /* RaceOnline       */ {"Online", "Online", "Online"},
      /* DetBoot          */ {"Starting up", "Inicializando", "Iniciando"},
      /* DetLoading       */ {"Loading", "Carregando", "Cargando"},
      /* DetMainMenu      */ {"In the main menu", "No menu principal", "En el men\xC3\xBA principal"},
      /* DetGarage        */ {"In the garage", "Na garagem", "En el garaje"},
      /* DetFreeRoam      */ {"Cruising Los Angeles", "Passeando por Los Angeles", "Paseando por Los Angeles"},
      /* DetRace          */ {"In a race", "Em corrida", "En carrera"},
      /* DetPause         */ {"Paused", "Pausado", "En pausa"},
      /* DetCredits       */ {"Watching the credits", "Vendo os cr\xC3\xA9""ditos", "Viendo los cr\xC3\xA9""ditos"},
      /* DetDefault       */ {"Playing", "Jogando", "Jugando"},
      /* TxtBoot          */ {"Opening LA Recomp", "Abrindo LA Recomp", "Abriendo LA Recomp"},
      /* TxtLoading       */ {"Loading session", "Carregando sess\xC3\xA3o", "Cargando sesi\xC3\xB3n"},
      /* TxtMainMenu      */ {"Choosing a game mode", "Escolhendo modo de jogo", "Eligiendo modo de juego"},
      /* TxtGarage        */ {"Customizing the car", "Customizando o carro", "Personalizando el coche"},
      /* TxtFreeRoam      */ {"Cruise / Free Roam", "Cruise / Free Roam", "Cruise / Free Roam"},
      /* TxtRace          */ {"Race in progress", "Corrida ativa", "Carrera activa"},
      /* TxtPause         */ {"Pause menu", "Menu de pausa", "Men\xC3\xBA de pausa"},
      /* TxtCredits       */ {"Credits", "Credits", "Cr\xC3\xA9""ditos"},
      /* TxtDefault       */ {"LA Recomp", "LA Recomp", "LA Recomp"},
  };

  int lang = EN;
  std::string sel = REXCVAR_GET(larecomp_discord_rpc_language);
  if (sel == "pt") {
    lang = PT;
  } else if (sel == "es") {
    lang = ES;
  } else if (sel == "en") {
    lang = EN;
  } else {
    // auto: follow the game language. Read through the registry rather than
    // REXCVAR_GET because user_language is a uint32_t on the stock SDK and a
    // std::string on our fork; GetFlagByName gives text either way, so accept
    // the name as well as the raw Xbox language id (5 = Spanish, 9 =
    // Portuguese). Anything not translated yet falls back to English.
    const std::string game = rex::cvar::GetFlagByName("user_language");
    if (game == "Portuguese" || game == "9") lang = PT;
    else if (game == "Spanish" || game == "5") lang = ES;
    else lang = EN;
  }

  int k = static_cast<int>(key);
  if (k < 0 || k >= static_cast<int>(RpcStr::Count)) return "";
  return T[k][lang];
}

namespace {

std::mutex g_rpc_mutex;
std::atomic_bool g_rpc_started{false};

LarecompDiscordState g_current_state = LarecompDiscordState::Unknown;
std::string g_current_details;
std::string g_current_state_text;

constexpr const char* kDiscordApplicationId = "1503923771264729309";

const char* DetailsForState(LarecompDiscordState state) {
  switch (state) {
    case LarecompDiscordState::Boot: return RpcTr(RpcStr::DetBoot);
    case LarecompDiscordState::Loading: return RpcTr(RpcStr::DetLoading);
    case LarecompDiscordState::MainMenu: return RpcTr(RpcStr::DetMainMenu);
    case LarecompDiscordState::Garage: return RpcTr(RpcStr::DetGarage);
    case LarecompDiscordState::FreeRoam: return RpcTr(RpcStr::DetFreeRoam);
    case LarecompDiscordState::Race: return RpcTr(RpcStr::DetRace);
    case LarecompDiscordState::Pause: return RpcTr(RpcStr::DetPause);
    case LarecompDiscordState::Credits: return RpcTr(RpcStr::DetCredits);
    default: return RpcTr(RpcStr::DetDefault);
  }
}

const char* StateTextForState(LarecompDiscordState state) {
  switch (state) {
    case LarecompDiscordState::Boot: return RpcTr(RpcStr::TxtBoot);
    case LarecompDiscordState::Loading: return RpcTr(RpcStr::TxtLoading);
    case LarecompDiscordState::MainMenu: return RpcTr(RpcStr::TxtMainMenu);
    case LarecompDiscordState::Garage: return RpcTr(RpcStr::TxtGarage);
    case LarecompDiscordState::FreeRoam: return RpcTr(RpcStr::TxtFreeRoam);
    case LarecompDiscordState::Race: return RpcTr(RpcStr::TxtRace);
    case LarecompDiscordState::Pause: return RpcTr(RpcStr::TxtPause);
    case LarecompDiscordState::Credits: return RpcTr(RpcStr::TxtCredits);
    default: return RpcTr(RpcStr::TxtDefault);
  }
}

void LogStateChange(const std::string& details, const std::string& state_text) {
  if (!REXCVAR_GET(larecomp_discord_rpc_log_state)) {
    return;
  }

  // Deliberately avoids ReX logging category dependencies.
  // Swap this to REXLOG_INFO if you prefer.
  std::printf("[LARECOMP DISCORD RPC] details='%s' state='%s'\n",
              details.c_str(),
              state_text.c_str());
}

}  // namespace

void LARECOMP_Discord_Init() {
  if (!REXCVAR_GET(larecomp_discord_rpc)) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_rpc_mutex);

  if (g_rpc_started.load()) {
    return;
  }

  larecomp::discord_rpc::Presence rpc;

  rpc.details_ = "";
  rpc.state_ = "";
  rpc.large_image_key_ = REXCVAR_GET(larecomp_discord_large_image);
  rpc.large_image_text_ = "LARecomp";

  larecomp::discord_rpc::Start(kDiscordApplicationId, rpc);

  g_rpc_started.store(true);
  g_current_state = LarecompDiscordState::Boot;
  g_current_details = rpc.details_;
  g_current_state_text = rpc.state_;

  LogStateChange(g_current_details, g_current_state_text);
}

void LARECOMP_Discord_Shutdown() {
  if (!g_rpc_started.load()) {
    return;
  }

  // If your SDK copy does not expose Stop(), comment this line.
  larecomp::discord_rpc::Stop();

  g_rpc_started.store(false);
}

void LARECOMP_Discord_SetStateText(LarecompDiscordState state,
                                   std::string_view details,
                                   std::string_view state_text) {
  if (!REXCVAR_GET(larecomp_discord_rpc)) {
    return;
  }

  if (!g_rpc_started.load()) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_rpc_mutex);

  const std::string new_details(details);
  const std::string new_state_text(state_text);

  // Prevent hook spam from hammering Discord if a hook runs many times.
  if (g_current_state == state &&
      g_current_details == new_details &&
      g_current_state_text == new_state_text) {
    return;
  }

  g_current_state = state;
  g_current_details = new_details;
  g_current_state_text = new_state_text;

  larecomp::discord_rpc::SetDetails(g_current_details);
  larecomp::discord_rpc::SetState(g_current_state_text);

  LogStateChange(g_current_details, g_current_state_text);
}

void LARECOMP_Discord_SetState(LarecompDiscordState state) {
  LARECOMP_Discord_SetStateText(
      state,
      DetailsForState(state),
      StateTextForState(state));
}

void LARECOMP_Discord_SetLoading() {
  LARECOMP_Discord_SetState(LarecompDiscordState::Loading);
}

void LARECOMP_Discord_SetMainMenu() {
  LARECOMP_Discord_SetState(LarecompDiscordState::MainMenu);
}

void LARECOMP_Discord_SetGarage() {
  LARECOMP_Discord_SetState(LarecompDiscordState::Garage);
}

void LARECOMP_Discord_SetFreeRoam() {
  LARECOMP_Discord_SetState(LarecompDiscordState::FreeRoam);
}

void LARECOMP_Discord_SetRace() {
  LARECOMP_Discord_SetState(LarecompDiscordState::Race);
}

void LARECOMP_Discord_SetPause() {
  LARECOMP_Discord_SetState(LarecompDiscordState::Pause);
}

void LARECOMP_Discord_SetCredits() {
  LARECOMP_Discord_SetState(LarecompDiscordState::Credits);
}

#endif // REXGLUE_HAS_XEO3_TARGET

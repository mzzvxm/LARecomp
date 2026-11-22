#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class DiscordSettings final : public rex::SettingOwner {
 public:
  DiscordSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, larecomp_discord_activity, true, "Enable Discord Rich Presence activity", rex::kDebugOnly);  // was inside a #if in the .cpp: check

  REX_SETTING(std::string, larecomp_discord_large_image, "mcla_logo", "Discord RPC large image key.", rex::kDebugOnly);

  REX_SETTING(bool, larecomp_discord_rpc, true, "Enable Discord Rich Presence. Live: off clears the presence at once, on shows the game's current state at once.");

  REX_SETTING(std::string, larecomp_discord_rpc_language, "auto", "Rich Presence language. 'auto' follows the game (user_language); or force "
                      "en / pt / es. Discord itself does not translate presence text.", rex::Allowed("auto", "en", "pt", "es"));

  REX_SETTING(bool, larecomp_discord_rpc_log_state, false, "Log Discord RPC state changes with printf.", rex::kDebugOnly);

};

inline DiscordSettings& g_discord = larecomp::Leak<DiscordSettings>();

inline auto& larecomp_discord_activity = g_discord.larecomp_discord_activity;
inline auto& larecomp_discord_large_image = g_discord.larecomp_discord_large_image;
inline auto& larecomp_discord_rpc = g_discord.larecomp_discord_rpc;
inline auto& larecomp_discord_rpc_language = g_discord.larecomp_discord_rpc_language;
inline auto& larecomp_discord_rpc_log_state = g_discord.larecomp_discord_rpc_log_state;

}  // namespace larecomp::settings

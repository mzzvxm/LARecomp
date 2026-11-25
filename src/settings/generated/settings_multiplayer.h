#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class MultiplayerSettings final : public rex::SettingOwner {
 public:
  MultiplayerSettings() : SettingOwner("larecomp") {}

  REX_SETTING(std::string, join_server, "", "Launch argument, never saved: join a server by itself once the city is up. "
                      "host[:port] = Cruise on it, host[:port],race = Race on it, "
                      "host[:port],room:<gamertag> = the room that player hosts. Switches online on "
                      "for this run, on that server (the same as mcla_online_enable and "
                      "mcla_online_server on the command line). The result is in the log and in "
                      "logs/join_server.txt.", rex::kDebugOnly);

  REX_SETTING(bool, leaderboards_diag, false, "Log every leaderboard read and write in detail.", rex::kDebugOnly);

  REX_SETTING(bool, leaderboards_enabled, true, "Leaderboards: serve the Xbox LIVE stats boards (race wins, battle modes, "
                    "race times) from our own backend, shared through the Rate My Ride "
                    "server. Off = stock behaviour (empty boards).");

  REX_SETTING(bool, online_diag, false, "Log online session content masks and the valid-vehicle count at the join "
                    "content check. Used to confirm whether the host's session properties "
                    "transported. Verbose; leave off for normal play.", rex::kDebugOnly);

  REX_SETTING(bool, online_ignore_content_check, true, "Ignore the session DLC/content compatibility check when joining or hosting "
                    "online. The check is symmetric and fails on any mask difference, so it "
                    "wrongly demands DLC (South Central) even when nobody owns any. Turn off to "
                    "restore the stock behaviour.", rex::kDebugOnly);

  REX_SETTING(std::string, online_master_server, "26.24.254.19:31003", "Server list for the server browser (F6): host or host:port of a "
                      "larecomp_master_server (port 31003 if omitted). Blank (empty or only spaces) "
                      "= no list: only server_browser_servers and the current server. Deleting "
                      "the setting brings the default back.");

  REX_SETTING(bool, online_race_prop_guard, true, "Skip the physics update of a prop whose owner is broken instead of "
                    "crashing. Starting an online race re-syncs every race prop.", rex::kDebugOnly);

  REX_SETTING(std::string, online_race_prop_watch, "", "Diagnostic: guest address (hex) of a prop inst. Its owner's inst word "
                      "is polled every frame and every change is logged. Empty = off.", rex::kDebugOnly);

  REX_SETTING(bool, rmr_diag, false, "Log every Rate My Ride request and record in detail.", rex::kDebugOnly);

  REX_SETTING(bool, rmr_enabled, true, "Rate My Ride: serve the mode from our own backend instead of Rockstar's "
                    "dead server. Off = stock behaviour (\"server not available\").");

  REX_SETTING(std::string, rmr_server, "", "Rate My Ride server as host[:port]. Empty = the online relay host "
                      "(mcla_online_server) when online is enabled, otherwise a local store "
                      "shared by the profiles on this PC. \"local\" forces the local store.");

  REX_SETTING(std::string, server_browser_pending, "", "Set by the server browser when a room needs another server: the room to "
                      "offer right after the restart (address|vip|label). Cleared once used.", rex::kDebugOnly);

  REX_SETTING(std::string, server_browser_servers, "", "Your own servers for the server browser, comma-separated host:port.");

  REX_SETTING(bool, server_browser_show_icons, true, "Server browser: show each server's icon.");

};

inline MultiplayerSettings& g_multiplayer = larecomp::Leak<MultiplayerSettings>();

inline auto& join_server = g_multiplayer.join_server;
inline auto& leaderboards_diag = g_multiplayer.leaderboards_diag;
inline auto& leaderboards_enabled = g_multiplayer.leaderboards_enabled;
inline auto& online_diag = g_multiplayer.online_diag;
inline auto& online_ignore_content_check = g_multiplayer.online_ignore_content_check;
inline auto& online_master_server = g_multiplayer.online_master_server;
inline auto& online_race_prop_guard = g_multiplayer.online_race_prop_guard;
inline auto& online_race_prop_watch = g_multiplayer.online_race_prop_watch;
inline auto& rmr_diag = g_multiplayer.rmr_diag;
inline auto& rmr_enabled = g_multiplayer.rmr_enabled;
inline auto& rmr_server = g_multiplayer.rmr_server;
inline auto& server_browser_pending = g_multiplayer.server_browser_pending;
inline auto& server_browser_servers = g_multiplayer.server_browser_servers;
inline auto& server_browser_show_icons = g_multiplayer.server_browser_show_icons;

}  // namespace larecomp::settings

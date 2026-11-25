#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class OnlineSettings final : public rex::SettingOwner {
 public:
  OnlineSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, net_lift_big_session_limits, false, "Sessions of more than 8 players keep damage, debris, world props and the "
    "default session settings that MCLA turns off above 8. Applies to the next "
    "session you host.");

  REX_SETTING(i32, net_max_players, 31, "Largest MaxPlayers the host menu offers (2..31). 16 is the game's own limit; above 16 "
    "the menu lists 2, 4, ... and the odd value itself as the last entry.", rex::kRequiresRestart);

};

inline OnlineSettings& g_online = larecomp::Leak<OnlineSettings>();

inline auto& net_lift_big_session_limits = g_online.net_lift_big_session_limits;
inline auto& net_max_players = g_online.net_max_players;

}  // namespace larecomp::settings

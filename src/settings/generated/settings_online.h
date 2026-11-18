#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class OnlineSettings final : public rex::SettingOwner {
 public:
  OnlineSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, net_lift_big_session_limits, false, "Sessions of more than 8 players keep damage, debris, world props and the "
    "default session settings that MCLA turns off above 8. Applies to the next "
    "session you host.");

};

inline OnlineSettings g_online;

inline auto& net_lift_big_session_limits = g_online.net_lift_big_session_limits;

}  // namespace larecomp::settings

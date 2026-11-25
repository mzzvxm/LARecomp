#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class CarbonSettings final : public rex::SettingOwner {
 public:
  CarbonSettings() : SettingOwner("larecomp") {}

  // Menu instrumentation. Off by default because it is chatty, but it is what
  // mapped the garage UI in the first place: the state tree, the field dump that
  // found state+12, the activate/command stream. Keep it — turning it on again
  // beats rediscovering any of it.
  REX_SETTING(bool, carbon_menu_diag, false, "Log the garage menu state tree, per-state field dumps and the "
    "activate/command stream while the paint screen is open.", rex::kDebugOnly);

};

inline CarbonSettings& g_carbon = larecomp::Leak<CarbonSettings>();

inline auto& carbon_menu_diag = g_carbon.carbon_menu_diag;

}  // namespace larecomp::settings

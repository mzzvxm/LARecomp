#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class TexturesSettings final : public rex::SettingOwner {
 public:
  TexturesSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, texture_mods, true, "Use the texture mods (<mods>/<Mod>/textures_replace/). False shows the game's own textures; "
    "changing it re-indexes the mods at the start of the next frame.");

  REX_SETTING(bool, texture_reload, false, "Set to true to reload the texture mods (<mods>/<Mod>/textures_replace/) at the start of the "
    "next frame, the same as RELOAD TEXTURE MODS in the pause menu. It clears itself afterwards.", rex::kDebugOnly);

};

inline TexturesSettings& g_textures = larecomp::Leak<TexturesSettings>();

inline auto& texture_mods = g_textures.texture_mods;
inline auto& texture_reload = g_textures.texture_reload;

}  // namespace larecomp::settings

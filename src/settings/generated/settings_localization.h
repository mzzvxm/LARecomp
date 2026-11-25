#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class LocalizationSettings final : public rex::SettingOwner {
 public:
  LocalizationSettings() : SettingOwner("larecomp") {}

  REX_SETTING(std::string, language, "auto", "UI text language. The disc has five full translations in its own "
    "ui/mc4stringtable*.strtbl files: English, Spanish, French, German and Italian. "
    "The game picks its language from the console's user language, which here is "
    "the SDK's user_language setting (English unless it says otherwise); at start-up "
    "LARecomp sets that from this choice (en, es, fr, de, it), so the game's own "
    "language code, its achievement list and the Discord status all follow it. "
    "The setting itself is not saved to larecomp.toml, only this one. auto leaves "
    "user_language as it is.\n"
    "Switching in-game reloads the text table at once and tells the game its new "
    "language, but screens already drawn keep their old text until they are "
    "reopened (the pause menu itself too: the game resolves a row's label once when "
    "the row is built), and the few things the game reads only at start follow after "
    "a restart.\n"
    "pt (Portuguese) works only with a mod that supplies the text: the disc's string "
    "tables carry an empty Portuguese block, and on the disc it starts where the "
    "Japanese block does, so it would show Japanese text. A mod can fill it with its "
    "own ui/mc4stringtable*.strtbl files (modloader/<mod>/files/ui/): every string "
    "table the game loads from ui/ (mc4stringtable, _0001 to _0003, _tms) needs a "
    "Portuguese block with text. Until then pt is used as auto, with a line in the "
    "log, and the setting stays in larecomp.toml for when the mod is installed. pt "
    "leaves user_language alone: only the text changes, the game's own language "
    "code stays what auto gives.\n"
    "Not offered: Japanese (its UI movies, jp_*.xsf, are not on the disc, and asking "
    "for them is a hard crash).", rex::Allowed("auto", "en", "es", "fr", "de", "it", "pt"));

};

inline LocalizationSettings& g_localization = larecomp::Leak<LocalizationSettings>();

inline auto& language = g_localization.language;

}  // namespace larecomp::settings

#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class SystemStringReplacementSettings final : public rex::SettingOwner {
 public:
  SystemStringReplacementSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, string_dump_enabled, false, "Capture encountered strings to TOML for translation/modding", rex::kDebugOnly);  // was inside a #if in the .cpp: check

  REX_SETTING(std::string, string_folder, "", "Override the strings folder (empty = <game data>/strings)", rex::kDebugOnly, rex::kRestart);  // was inside a #if in the .cpp: check

  REX_SETTING(std::string, string_language, "", "Language code for string replacements (e.g. pt_BR, es_ES)", rex::kDebugOnly);  // was inside a #if in the .cpp: check

  REX_SETTING(bool, string_replace_enabled, false, "Inject replacement strings from disk when available", rex::kDebugOnly);  // was inside a #if in the .cpp: check

};

inline SystemStringReplacementSettings g_system_string_replacement;

inline auto& string_dump_enabled = g_system_string_replacement.string_dump_enabled;
inline auto& string_folder = g_system_string_replacement.string_folder;
inline auto& string_language = g_system_string_replacement.string_language;
inline auto& string_replace_enabled = g_system_string_replacement.string_replace_enabled;

}  // namespace larecomp::settings

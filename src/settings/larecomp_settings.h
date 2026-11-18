#pragma once
//
// LARecomp's settings on rexglue 0.11 (REX_SETTING owners, scoped names, TOML tables).
//
// DESIGN (the owner's rule D4: the names players have in larecomp.toml keep working)
//   * Every LARecomp setting is a member of an owner whose scope is "larecomp" and that is named exactly like the
//     old cvar, so mcla_gpu_ao is `larecomp.mcla_gpu_ao`. The generated owners are in settings/generated/ (written
//     by merge-tools/wpl/cvar_convert.py from the old REXCVAR_DEFINE_* lines), the few hand-written ones in
//     larecomp_app_settings.h.
//   * RegisterAll() registers them in LarecompApp::OnConfigure, BEFORE the config file is read, and adds an alias
//     `mcla_gpu_ao` -> `larecomp.mcla_gpu_ao` for each, plus the aliases of the SDK names that moved
//     (sdk_names.inc: vsync -> gpu.vsync ...). So a flat line in larecomp.toml, --mcla_gpu_ao=high on the command
//     line (the launcher's More game options) and REX_<..> variables all reach the right setting. Each use of an
//     old name adds one notice line to the log (the SDK's alias rule); nothing else changes for the player.
//   * The file stays FLAT. LARecomp's own writer (config_save.cpp, profiles::EditConfig) keeps writing and updating
//     `name = value` lines, with FlatName() as the key, so a downgrade or the launcher still reads the file.
//     The SDK's own tables ([gpu], [ui], ...) are read as usual.
//   * Where a value, not only a name, changed meaning (the renderer choice, input_backend, resolution_scale) the
//     migration is MigrateLegacyConfig(): it reads the file once, before the SDK does, and sets the new
//     settings at the title layer, which the file's own new-style values still override.
//   * LARecomp's start-up defaults (the old SetFlag calls) are written with SetDefault(): the TITLE layer, so a
//     value from the file, the environment or the command line always wins and the old FlagWasConfigured /
//     NoteStartupCvar bookkeeping is not needed: "did the player choose this" is `layer >= kFile`.
//
// Use sites: larecomp::settings::<name>.Get() (rewritten from REXCVAR_GET by cvar_convert.py), the by-name calls
// through larecomp::cvar::* (the old rex::cvar names, same shapes) and GetAs<T>("<sdk name>") for SDK settings.
//
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "settings/larecomp_app_settings.h"
#include "settings/generated/settings_all.h"

namespace larecomp::settings {

// Registers every owner (generated and hand-written) and all aliases. Call once from OnConfigure.
// CHECK: SettingsRegistry::Register(owner) before the file layer is read applies values that already wait for
// these names (the command line does not: it comes after OnConfigure) - read register order in app.cpp.
[[nodiscard]] rex::Result<void> RegisterAll(rex::ISettingsRegistry& registry);
void UnregisterAll();

// Reads the player's larecomp.toml once (before the SDK does) for the keys whose VALUE meaning changed and sets the
// new settings at the title layer. `file` is app.config_file. Safe when the file is missing or unreadable.
void MigrateLegacyConfig(rex::ISettingsRegistry& registry, const std::filesystem::path& file);

// A setting by a flat LARecomp name, a flat old SDK name from sdk_names.inc, or a full name. Null when unknown.
rex::SettingBase* Find(std::string_view name);

// The old rex::cvar shapes. "" for an unknown name.
std::string Get(std::string_view name);
// Runtime layer (a menu, the console). False for an unknown name or a value the setting refuses.
bool Set(std::string_view name, std::string_view value);
// LARecomp's own default for this run (title layer). A value from the file, the environment or the command line
// is kept. False for an unknown name or a value the setting refuses.
bool SetDefault(std::string_view name, std::string_view value);

// The key the config file uses: the flat name for larecomp.<x>, a full name otherwise.
std::string FlatName(const rex::SettingBase& setting);
// The old category string ("MCLA/GPU") the settings panel groups by; empty for a setting that has none.
std::string_view Category(std::string_view flat_name);
// Every LARecomp setting (not the SDK's), sorted by name.
std::vector<rex::SettingBase*> AllLarecompSettings();

// A setting of the SDK or LARecomp read by its scoped name (REXCVAR_GET of an SDK-owned name is rewritten to this).
// Not for hot paths: it looks the setting up each time.
template <typename T>
T GetAs(std::string_view scoped_name);

// Converts the text of a setting to T (the same rules as the registry).
template <typename T>
T ParseAs(const std::string& text) {
  T value{};
  rex::detail::ParseSetting(text, value);
  return value;
}

template <typename T>
T GetAs(std::string_view scoped_name) {
  rex::SettingBase* setting = Find(scoped_name);
  return setting ? ParseAs<T>(setting->GetText()) : T{};
}

}  // namespace larecomp::settings

// ---------------------------------------------------------------------------------------------------------------
// larecomp::cvar - the old rex::cvar API for the few files that walk the registry (the settings panel, the pause
// menu, the config writer, the debug menu). Names and shapes as in the old SDK's rex/cvar.h, so those files change
// by namespace only; the entries are built from the SettingBase on demand.
namespace larecomp::cvar {

enum class FlagType { Boolean, Int32, Int64, Uint32, Uint64, Double, String, Command };
enum class Lifecycle { kInitOnly, kHotReload, kRequiresRestart };
// kTitle (LARecomp's own default) reads as kDefault: it is not the player's choice. kLock reads as kRuntime.
enum class Source { kDefault, kConfig, kEnvironment, kCommandLine, kRuntime };

struct Constraints {
  std::optional<double> min;
  std::optional<double> max;
  std::vector<std::string> allowed_values;
  bool HasRangeConstraint() const { return min.has_value() || max.has_value(); }
  bool HasAllowedValues() const { return !allowed_values.empty(); }
};

struct FlagEntry {
  std::string name;  // the FLAT name for a LARecomp setting
  FlagType type = FlagType::Boolean;
  std::string category;
  std::string description;
  std::function<std::string()> getter;
  Lifecycle lifecycle = Lifecycle::kHotReload;
  Constraints constraints;
  std::string default_value;
  bool is_debug_only = false;
  Source source = Source::kDefault;
};

std::string GetFlagByName(std::string_view name);
bool SetFlagByName(std::string_view name, std::string_view value);
// Valid until the next call on the same thread.
const FlagEntry* GetFlagInfo(std::string_view name);
Source GetFlagSource(std::string_view name);
std::vector<std::string> ListFlags();
std::vector<std::string> ListModifiedFlags();
bool HasNonDefaultValue(std::string_view name);
void ResetToDefault(std::string_view name);
bool InvokeCommand(std::string_view name, std::string_view args);
// Called with (flat name, new text) after each change. One list per name; Unregister drops them.
using ChangeCallback = std::function<void(std::string_view name, std::string_view new_value)>;
void RegisterChangeCallback(std::string_view name, ChangeCallback callback);
void UnregisterChangeCallbacks(std::string_view name);
template <typename T>
T Query(std::string_view name) {
  return larecomp::settings::ParseAs<T>(GetFlagByName(name));
}

}  // namespace larecomp::cvar

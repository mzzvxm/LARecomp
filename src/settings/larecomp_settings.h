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
//     migration is ApplyPlayerConfig(): it reads the file once, before the SDK does, and sets the new
//     settings at the title layer, which the file's own new-style values still override.
//   * LARecomp's start-up defaults (the old SetFlag calls) are written with SetDefault(): the TITLE layer, so a
//     value from the file, the environment or the command line always wins and the old FlagWasConfigured /
//     NoteStartupCvar bookkeeping is not needed: "did the player choose this" is `layer >= kFile`.
//
//   * The file is read TWICE, on purpose. The SDK reads it after OnConfigure (App::Init: settings.Load(file, kFile)) and
//     that Load stops at the first value a setting refuses (a "low" for an ambient-occlusion setting that is now
//     off/high/ultra, a number out of range): the keys after it are lost, the whole file is copied to
//     <file>.unreadable.bak and the start goes on with defaults. A 0.10 file with one stale word must not lose its
//     other lines, so ApplyPlayerConfig reads it first in OnConfigure, key by key, and keeps every valid value at the
//     file layer (the SDK's own Load then repeats the same values: harmless, and it adds the "old name" notices).
//     CHECK (SDK): Load should skip a refused value and go on; then the second read and the .bak notice go away.
//   * The environment: the SDK reads REX_<SCOPE>_<NAME> (REX_LARECOMP_DEBUG_MENU). The 0.10 names were REX_<NAME>
//     (REX_DEBUG_MENU, REX_MCLA_GPU_AO): ApplyLegacyEnvironment keeps them working for the LARecomp settings.
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

#include <rex/console_command.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "settings/larecomp_app_settings.h"
#include "settings/generated/settings_all.h"

namespace larecomp::settings {

// A console command (or any owner declared next to the code that uses it) hands itself over here from its constructor;
// RegisterAll registers it with the rest. cvar_convert.py emits one owner per REXCVAR_DEFINE_COMMAND*.
void RegisterExtraOwner(rex::SettingOwner& owner);

// Registers every owner (generated and hand-written) and all aliases. Call once from OnConfigure. The order in
// App::Init is OnConfigure, command line, the SDK's file Load, environment: every later layer finds the names and their
// aliases registered, and a layer always beats a lower one whatever the order, so nothing here depends on it.
[[nodiscard]] rex::Result<void> RegisterAll(rex::ISettingsRegistry& registry);
void UnregisterAll();
// REX_<FLAT NAME> (the 0.10 spelling) for the LARecomp settings, at the environment layer. OnConfigure calls it after
// RegisterAll (the SDK applies REX_LARECOMP_<NAME> by itself later).
void ApplyLegacyEnvironment();

// Reads the player's larecomp.toml before the SDK does (OnConfigure):
//   * the keys whose VALUE meaning changed (renderer, resolution_scale, input_backend) set the new settings at the
//     TITLE layer, which a key of the new kind in the same file still overrides;
//   * every other key is applied at the FILE layer one by one: a refused value is logged and skipped, it does not
//     cost the rest of the file (see the header comment).
// `file` is app.config_file. Safe when the file is missing or unreadable. Returns the number of refused values.
// Call it AFTER ApplyStartupDefaults: both write the title layer, the last write wins.
int ApplyPlayerConfig(rex::ISettingsRegistry& registry, const std::filesystem::path& file);

// A setting by a flat LARecomp name, a flat old SDK name from sdk_names.inc, or a full name. Null when unknown.
rex::SettingBase* Find(std::string_view name);

// The old rex::cvar shapes. "" for an unknown name. Two old names are computed, not looked up: input_backend ("sdl" or
// "xinput", the first of hid.drivers) so the pad code that asks which backend the SDK runs keeps its test.
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
// The panel's category for a setting: the REXCVAR_DEFINE one, or for the SDK's settings one derived from the scope
// ("gpu.d3d12.bindless" -> "GPU/D3D12"). Empty for a LARecomp setting without one.
std::string CategoryOf(const rex::SettingBase& setting, std::string_view flat_name);
// Every LARecomp setting (not the SDK's), sorted by name.
std::vector<rex::SettingBase*> AllLarecompSettings();
// Every registered setting, LARecomp's and the SDK's, sorted by full name.
std::vector<rex::SettingBase*> AllSettings();

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
// kStartupOnly and kUntilLaunch both read as kInitOnly: the game is running when anything asks.
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
  // Type Command only (a ConsoleCommand): runs it with the argument text.
  std::function<void(std::string_view)> command_callback;
};

std::string GetFlagByName(std::string_view name);
bool SetFlagByName(std::string_view name, std::string_view value);
// Valid until the next call on the same thread.
const FlagEntry* GetFlagInfo(std::string_view name);
Source GetFlagSource(std::string_view name);
// Every setting of the program (LARecomp's and the SDK's), by the name the file uses.
std::vector<std::string> ListFlags();
// The registry as the settings panel walks it (the 0.10 rex::cvar::GetRegistry): one entry per setting and per console
// command, grouped by category, with `source` brought up to date on every call. Entries are built once and rebuilt when
// settings are added; the reference is valid until the next call from another thread. UI thread.
std::vector<FlagEntry>& GetRegistry();
// Tags a setting the SDK owns as debug-only for the settings panel and the config writer (the 0.10
// MarkSdkDebugCvars poked entry.is_debug_only; the SDK's settings are built with their traits now). By flat or full name;
// may be called before the setting exists.
void MarkDebugOnly(std::string_view name);
// Puts a value back at the layer it came from (a trailer scene let go of a setting). The registry only raises a layer, so
// the setting goes back to its default first and the value is stored again at `source`'s layer: its change callbacks run
// twice. False for an unknown or frozen setting.
bool RestoreValue(std::string_view name, std::string_view value, Source source);
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

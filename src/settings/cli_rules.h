#pragma once
//
// LARecomp's command line on rexglue 0.11: which option goes to the SDK, which to LARecomp, and what happens to an option
// no loaded module can take. Pure (no SDK, no OS): settings/cli_rules_test.cpp compiles this header alone. The app side is
// LarecompApp::PrepareCommandLine (called by main before Init) and LarecompApp::ApplyCommandLineSettings (OnConfigure).
//
// WHY. The SDK stops the start on a command line it cannot use (rex::app::detail::ParseCommandLine, and App::ReportUnknownSettings
// after OnInit). Seen in the first v0.11 boot (2026-11-18): `--d3d12_adapter=0` with LARecomp's own renderer (gpu.driver =
// larecomp) ended in "unknown setting 'gpu.d3d12.adapter' from CommandLine" and an error dialog, because gpu.d3d12.* is
// registered only by the d3d12 driver module (as are gpu.xenos.*, gpu.vulkan.*, hid.sdl.*, ui.sdl.*), and that module is not
// loaded then. The same parse also stops the start on
//   - an option that is not a setting name (no dot, not an alias): `--mcla_fps=60`, a typo of a flat name, `--help`;
//   - a value a setting refuses (`--vsync=maybe`, a bare `--d3d12_adapter`);
//   - LARecomp's own relaunch flag `--larecomp_restart_wait_handle=<h>` (mc_engine/restart.cpp; Linux: `..._pid=`), which no
//     setting has: RESTART GAME in the pause menu would have started a game that dies in its parse.
// The launcher's "More options" box, old shortcuts and scripts pass such flags, and a player must always get into the game.
//
// POLICY.
//   * The SDK keeps what is its own: the declared actions (--build_shader_packs, --precache_pipelines) and its seven path
//     flags (kSdkFlags, mirrored from app/command_line.cpp: re-check on every SDK bump).
//   * Everything else is a SETTING option. main takes them out of the list it gives the SDK and OnConfigure applies them at the
//     same layer the SDK would (SettingLayer::kCommandLine, which beats the file and the environment whatever the order).
//   * An option LARecomp cannot use costs ONE log line (`[cli] ...`, WARN, written in OnInit once the log file is open) and the
//     start goes on:
//       kUnloaded  the setting belongs to a driver this run does not load (kDriverScopes, judged by what gpu.driver, hid.drivers,
//                  ui.driver say after the command line, the environment, larecomp.toml and the defaults are in);
//       kUnknown   not a setting name of this version (a flat name that is not an alias: the fork launcher's `--mcla_fps`, a typo);
//       kBadUse    a negated non-bool, a changed-meaning flag without a usable value;
//       a value a registered setting refuses (decided by the registry, reported by the app).
//   * The flags whose VALUE meaning changed in 0.11 keep working like their larecomp.toml keys (ApplyPlayerConfig):
//     `--resolution_scale=N` sets gpu.resolution_scale_x and _y, `--input_backend=sdl|xinput` sets hid.drivers.
//   * The environment needs nothing: the SDK applies REX_<SCOPE>_<NAME> only to settings that are registered, so a variable
//     for an unloaded driver is already ignored without a word. The file (larecomp.toml) only gets the SDK's own "unknown
//     setting ... from Config" warning for such a key, never an error.
//
// WHAT THIS CANNOT DO (it needs a change in the SDK's App): a dotted name under a driver scope that IS
// loaded but that the driver does not know (`--gpu.d3d12.adaptr=0` with d3d12 loaded) is passed to the SDK like any dotted name
// that is not registered yet (overlay binds, bindings register after OnConfigure), and the SDK's check after OnInit still fails the
// start for it. Closing that needs App::ReportUnknownSettings to warn instead of fail (a few lines in the SDK's App).
//
#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace larecomp::cli {

// One option of the command line: the shape of rex::CLIOption, kept apart so that this header needs no SDK.
struct Option {
  std::string name;                 // without the dashes, "no-" kept
  std::optional<std::string> value;  // --name=value or --name value
};

// "--name" or "--name=value", the way the option was typed.
constexpr std::string Spell(const Option& option) {
  return "--" + option.name + (option.value ? "=" + *option.value : std::string());
}

// ---------------------------------------------------------------------------------------------------------------
// Names. The SDK's grammar (app/command_line.cpp IsNamePart / IsSettingName): a dotted full name, or a flat alias.
constexpr bool IsNamePart(std::string_view part) {
  if (part.empty() || !(part.front() >= 'a' && part.front() <= 'z')) return false;
  for (const char c : part) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

constexpr bool IsSettingName(std::string_view name) {
  if (name.find('.') == std::string_view::npos) return false;
  size_t begin = 0;
  for (;;) {
    size_t end = name.find('.', begin);
    if (end == std::string_view::npos) end = name.size();
    if (!IsNamePart(name.substr(begin, end - begin))) return false;
    if (end == name.size()) return true;
    begin = end + 1;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Routing.
// The options the SDK's parser takes itself, by name (app/command_line.cpp kFlags). They need a value there.
inline constexpr std::string_view kSdkFlags[] = {"modules",          "game_data_root", "user_data_root", "update_data_root",
                                                 "cache_root",       "metadata_root",  "config_file"};
// LARecomp's own flags: read from the process's command line elsewhere (mc_engine/restart.cpp), no setting. Dropped silently.
inline constexpr std::string_view kInternalFlags[] = {"larecomp_restart_wait_handle", "larecomp_restart_wait_pid"};

// GX: dispatched before the title app is constructed, so the SDK never parses this action.
inline constexpr std::string_view kDumpSettingsUi = "dump_settings_ui";

enum class Route { kSdk, kInternal, kSetting };

constexpr bool Contains(std::span<const std::string_view> names, std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

constexpr Route RouteOf(std::string_view name, std::span<const std::string_view> actions) {
  if (Contains(actions, name) || Contains(kSdkFlags, name)) return Route::kSdk;
  if (Contains(kInternalFlags, name)) return Route::kInternal;
  return Route::kSetting;
}

struct Split {
  std::vector<std::string> sdk;      // tokens for rex::CLIArgList::Parse: the actions, the SDK's flags, the positionals
  std::vector<Option> settings;      // for ApplyCommandLineSettings, in the order typed
  std::vector<std::string> notes;    // options dropped here, one line each (no "[cli]" prefix)
};

constexpr Split SplitCommandLine(std::span<const Option> options, std::span<const std::string> positionals,
                              std::span<const std::string_view> actions) {
  Split out;
  for (const Option& option : options) {
    switch (RouteOf(option.name, actions)) {
      case Route::kSdk:
        if (!Contains(actions, option.name) && !option.value) {
          // The SDK would stop the start for a path flag with no value ("unknown option").
          out.notes.push_back(Spell(option) + " ignored: it needs a value (" + Spell({option.name, "<value>"}) + ")");
          break;
        }
        out.sdk.push_back(Spell(option));
        break;
      case Route::kInternal:
        break;
      case Route::kSetting:
        out.settings.push_back(option);
        break;
    }
  }
  for (const std::string& positional : positionals) out.sdk.push_back(positional);
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// The drivers this run loads, and the settings of the drivers that are not.
// The values of the four selector settings as the SDK will see them once every layer is in.
struct Selection {
  std::string gpu;   // gpu.driver: d3d12, vulkan, null, any, none, or LARecomp's "larecomp"
  std::string apu;   // apu.driver
  std::string hid;   // hid.drivers, a comma list
  std::string ui;    // ui.driver
  bool windows = true;  // "any" is d3d12 on Windows and vulkan elsewhere (GraphicsSystem::GetDriverName)
};

constexpr bool IsSelectorSetting(std::string_view full_name) {
  return full_name == "gpu.driver" || full_name == "apu.driver" || full_name == "hid.drivers" || full_name == "ui.driver";
}

// What the SDK will see for a selector: the command line wins, then the environment, then what the registry holds now
// (the defaults, larecomp.toml's values and ApplyPlayerConfig's conversions).
constexpr std::string EffectiveValue(const std::optional<std::string>& from_command_line,
                                  const std::optional<std::string>& from_environment, const std::string& current) {
  if (from_command_line) return *from_command_line;
  if (from_environment) return *from_environment;
  return current;
}

// The value of the last --name=value of that name on the command line (the SDK applies them in order, the last one wins).
constexpr std::optional<std::string> LastValueOf(std::span<const Option> options, std::string_view name) {
  std::optional<std::string> found;
  for (const Option& option : options) {
    if (option.name == name && option.value) found = *option.value;
  }
  return found;
}

enum class Family { kGpu, kHid, kUi };

struct DriverScope {
  std::string_view prefix;   // the owner's scope with its dot
  Family family;
  std::string_view drivers;  // the drivers of that family that register this scope, comma separated
  std::string_view what;     // for the log line
};

// The scopes that driver modules register (read from the SDK 0.11 sources: gpu/xenos/xenos_graphics_driver_base.cpp,
// d3d12_xenos_graphics_driver.cpp, vulkan_xenos_graphics_driver.cpp, hid/sdl3/sdl3_input_driver.cpp, ui/sdl3/sdl3_ui_driver.cpp).
// A scope that is not here is treated as loaded: its names go to the SDK, as before.
inline constexpr DriverScope kDriverScopes[] = {
    {"gpu.xenos.", Family::kGpu, "d3d12,vulkan,null", "the Xenia graphics drivers (d3d12, vulkan)"},
    {"gpu.d3d12.", Family::kGpu, "d3d12", "the d3d12 graphics driver"},
    {"gpu.vulkan.", Family::kGpu, "vulkan", "the vulkan graphics driver"},
    {"hid.sdl.", Family::kHid, "sdl3", "the sdl3 input driver"},
    {"ui.sdl.", Family::kUi, "sdl3", "the sdl3 window driver"},
};

constexpr std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// Whether `name` is in the comma list (blanks around a name ignored; "none" and empty names are never in it).
constexpr bool ListHas(std::string_view list, std::string_view name) {
  while (!list.empty()) {
    const size_t comma = list.find(',');
    const std::string_view item = Trim(list.substr(0, comma));
    if (!item.empty() && item != "none" && item == name) return true;
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return false;
}

// The graphics driver by name, "any" resolved the way the SDK does.
constexpr std::string GpuDriver(const Selection& selection) {
  const std::string_view gpu = Trim(selection.gpu);
  if (gpu == "any") return selection.windows ? "d3d12" : "vulkan";
  return std::string(gpu);
}

constexpr bool ScopeLoaded(const DriverScope& scope, const Selection& selection) {
  switch (scope.family) {
    case Family::kGpu: {
      const std::string gpu = GpuDriver(selection);
      return ListHas(scope.drivers, gpu);
    }
    case Family::kHid: {
      std::string_view list = selection.hid;
      while (!list.empty()) {
        const size_t comma = list.find(',');
        if (ListHas(scope.drivers, Trim(list.substr(0, comma)))) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
      }
      return false;
    }
    case Family::kUi:
      return ListHas(scope.drivers, Trim(selection.ui));
  }
  return true;
}

// nullopt when `full_name` is not in a driver scope, or its driver is loaded; else why it cannot apply.
constexpr std::optional<std::string> WhyUnloaded(std::string_view full_name, const Selection& selection) {
  for (const DriverScope& scope : kDriverScopes) {
    if (!full_name.starts_with(scope.prefix)) continue;
    if (ScopeLoaded(scope, selection)) return std::nullopt;
    std::string selector, value;
    switch (scope.family) {
      case Family::kGpu: selector = "gpu.driver"; value = selection.gpu; break;
      case Family::kHid: selector = "hid.drivers"; value = selection.hid; break;
      case Family::kUi: selector = "ui.driver"; value = selection.ui; break;
    }
    if (Trim(value).empty()) value = "none";
    return std::string(full_name) + " is a setting of " + std::string(scope.what) + ", which this run does not load (" +
           selector + " = " + value + ")";
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------------
// One setting option.
enum class Verdict {
  kApply,     // set `set` at the command line layer (the registry decides about the value)
  kUnknown,   // not a setting of this version
  kUnloaded,  // a setting of a driver this run does not load
  kBadUse,    // a negated non-bool, a changed-meaning flag with no usable value
};

struct Assignment {
  std::string name;   // a full name, or the flat / old name the player typed (the registry resolves aliases)
  std::string value;
};

struct Decision {
  Verdict verdict = Verdict::kApply;
  std::vector<Assignment> set;  // kApply
  std::string note;             // every other verdict: one line saying what was ignored and why
};

// What the registry can tell, handed in so that the rules stay pure. A `Lookup` is any type with these three members (the app
// passes a small struct over the registry; the test, plain functions), so Decide can also be evaluated at compile time:
//   bool is_alias(std::string_view name) const;
//       the registry's alias table (the SDK's and LARecomp's): an old or flat name that stands for a setting;
//   std::string full_name(std::string_view name) const;
//       the full name LARecomp's own tables give a flat name (larecomp.<x>, sdk_names.inc), registered or not; "" when they have none;
//   std::optional<bool> registered_is_bool(std::string_view full_name) const;
//       whether the setting of that full name is a bool; nullopt when nothing registered it (yet).
template <typename Lookup>
constexpr Decision Decide(const Option& option, const Selection& selection, const Lookup& lookup) {
  Decision decision;
  const std::string spelled = Spell(option);
  const auto ignore = [&](Verdict verdict, const std::string& why) {
    decision.verdict = verdict;
    decision.note = spelled + " ignored: " + why;
    return decision;
  };

  std::string_view name = option.name;
  const bool negated = name.starts_with("no-");
  if (negated) name.remove_prefix(3);

  // Names whose VALUE meaning changed in 0.11: converted like larecomp.toml's keys (settings/larecomp_settings.cpp
  // ApplyPlayerConfig), not looked up as settings.
  if (!negated && name == "resolution_scale") {
    if (!option.value || Trim(*option.value).empty()) return ignore(Verdict::kBadUse, "it needs a number (--resolution_scale=2)");
    const std::string value(Trim(*option.value));
    decision.set = {{"gpu.resolution_scale_x", value}, {"gpu.resolution_scale_y", value}};
    return decision;
  }
  if (!negated && name == "input_backend") {
    const std::string_view value = option.value ? Trim(*option.value) : std::string_view();
    if (value == "sdl") {
      decision.set = {{"hid.drivers", "sdl3"}};
    } else if (value == "xinput") {
      decision.set = {{"hid.drivers", "xinput"}};
    } else {
      return ignore(Verdict::kBadUse, "the input backend is sdl or xinput (hid.drivers names the drivers)");
    }
    return decision;
  }

  // A name the SDK's parser would refuse outright: neither a dotted name nor an alias.
  const bool alias = IsNamePart(name) && lookup.is_alias(name);
  if (!IsSettingName(name) && !alias) return ignore(Verdict::kUnknown, "not a setting of this version");

  // The full name, when LARecomp's tables or the name itself give one (an alias only the SDK knows gives none).
  std::string full = lookup.full_name(name);
  if (full.empty() && IsSettingName(name)) full = std::string(name);
  if (!full.empty()) {
    if (const auto why = WhyUnloaded(full, selection)) return ignore(Verdict::kUnloaded, *why);
    if (negated) {
      if (const auto is_bool = lookup.registered_is_bool(full); is_bool && !*is_bool) {
        return ignore(Verdict::kBadUse, full + " is not a bool, so --no- cannot turn it off");
      }
    }
  }
  // The SDK's own rule: --no-name is "false" whatever follows; --name alone is "true".
  decision.set = {{std::string(name), negated ? std::string("false") : option.value.value_or("true")}};
  return decision;
}

// ---------------------------------------------------------------------------------------------------------------
// "Did you mean": the closest candidate to a name nothing knows, or "" when none is near.
constexpr size_t EditDistance(std::string_view a, std::string_view b) {
  std::vector<size_t> previous(b.size() + 1), current(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j) previous[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    current[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      const size_t substitute = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitute});
    }
    std::swap(previous, current);
  }
  return previous[b.size()];
}

constexpr std::string ClosestName(std::string_view name, std::span<const std::string> candidates) {
  const size_t limit = std::clamp<size_t>(name.size() / 4, 1, 3);
  std::string best;
  size_t best_distance = limit + 1;
  for (const std::string& candidate : candidates) {
    if (candidate.empty()) continue;
    const size_t distance = EditDistance(name, candidate);
    if (distance < best_distance) {
      best_distance = distance;
      best = candidate;
    }
  }
  return best;
}

}  // namespace larecomp::cli

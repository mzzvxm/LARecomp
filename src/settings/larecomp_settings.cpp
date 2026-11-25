#include "settings/larecomp_settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include <rex/console_command.h>
#include <rex/logging/log_category.h>
#include <rex/platform/process.h>
#include <toml++/toml.hpp>

#include "gpu/gfx_preset.h"
#include "gpu/denoiser_setting.h"
#include "settings/immortal.h"
#include "settings/watch_table.h"

namespace larecomp::settings {

namespace {

struct SdkName {
  const char* old_name;
  const char* new_name;
};
constexpr SdkName kSdkNames[] = {
#include "settings/sdk_names.inc"
};

// Names of settings that 0.10 had and 0.11 removed (the FSR / CAS present effects belonged to the Xenia presenter,
// D4 of UPSTREAM-0.11-PLAN.md; mcla_gpu and mcla_native_gfx_nocp were LARecomp's renderer switches, gpu.driver chooses the
// renderer now and cvar_convert.py's DROPPED makes no setting of them). A name like this in an old larecomp.toml, on the
// command line or in the environment is not an error and not an "unknown setting": the alias to "" makes the registry
// ignore the value and note "<name> was removed and is ignored" in the log. The two renderer keys are still READ, from the
// raw file, by ApplyPlayerConfig (below) to set gpu.driver for an old file, so that they need no setting to be read.
constexpr const char* kRemovedNames[] = {
    "present_effect",
    "present_fsr_quality_mode",
    "present_cas_additional_sharpness",
    "present_fsr_sharpness_reduction",
    "present_fsr_max_upsampling_passes",
    "host_present_from_non_ui_thread",
    "mcla_gpu",
    "mcla_native_gfx_nocp",
};

// Keys of the file ApplyPlayerConfig converts itself: their VALUE meaning changed, so applying the text to a setting of
// the same name (there is none) would only add an "unknown setting" warning.
constexpr const char* kConvertedKeys[] = {"resolution_scale", "input_backend"};

struct CategoryRow {
  const char* name;
  const char* category;
};
constexpr CategoryRow kCategories[] = {
#include "settings/generated/settings_meta.inc"
    {"cpu_limit_lp", "MCLA/Performance"},
    {"render_wait_mode", "MCLA/Performance"},
    {"mcla_gpu_vk_present_pacing", "MCLA/Performance"},
};

struct State {
  std::mutex mutex;
  rex::ISettingsRegistry* registry = nullptr;
  std::vector<rex::SettingOwner*> owners;
  std::unordered_map<std::string, rex::SettingBase*> flat;           // mcla_gpu_ao -> larecomp.mcla_gpu_ao
  std::unordered_map<std::string, std::string> sdk_by_old_name;      // vsync -> gpu.vsync
  std::unordered_map<std::string, std::string> old_name_by_sdk;      // gpu.vsync -> vsync
  std::unordered_map<std::string, std::string> category;
};

// Never destroyed (immortal.h): a thread still running at exit (a guest thread the SDK left running, most likely, through the game's hooks)
// still called Find while the exe's static destruction ran, and the table it read was freed under it (an access violation 2 ms after
// "Process exiting normally", 2026-11-21). UnregisterAll empties what points into the SDK; a late reader then finds nothing, in memory
// that is still there.
State& S() {
  static larecomp::Immortal<State> s;
  return *s;
}

constexpr std::string_view kScope = "larecomp.";

}  // namespace

namespace {
std::vector<rex::SettingOwner*>& Extra() {
  static std::vector<rex::SettingOwner*>& extra = larecomp::Leak<std::vector<rex::SettingOwner*>>();
  return extra;
}
}  // namespace

void RegisterExtraOwner(rex::SettingOwner& owner) { Extra().push_back(&owner); }

rex::Result<void> RegisterAll(rex::ISettingsRegistry& registry) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.registry = &registry;
  s.owners.clear();
  s.owners.push_back(&g_app);
  for (rex::SettingOwner* owner : GeneratedOwners()) {
    s.owners.push_back(owner);
  }
  for (rex::SettingOwner* owner : Extra()) {
    s.owners.push_back(owner);
  }
  for (const SdkName& n : kSdkNames) {
    s.sdk_by_old_name[n.old_name] = n.new_name;
    s.old_name_by_sdk[n.new_name] = n.old_name;
  }
  for (const CategoryRow& c : kCategories) {
    s.category[c.name] = c.category;
  }
  for (rex::SettingOwner* owner : s.owners) {
    if (auto registered = registry.Register(*owner); !registered) {
      return registered;
    }
    for (rex::SettingBase* setting : owner->GetOwnedSettings()) {
      const std::string& full = setting->GetName();
      std::string flat = full.substr(kScope.size());
      if (s.sdk_by_old_name.count(flat) != 0) {
        // A LARecomp setting named like an SDK setting that moved: the flat name would be ambiguous.
        REXLOG_ERROR("settings: '{}' is both a LARecomp setting and an old SDK name; the LARecomp one is used", flat);
        s.sdk_by_old_name.erase(flat);
      }
      s.flat[flat] = setting;
      registry.AddAlias(flat, full);
    }
  }
  for (const auto& [old_name, new_name] : s.sdk_by_old_name) {
    registry.AddAlias(old_name, new_name);
  }
  for (const char* removed : kRemovedNames) {
    registry.AddAlias(removed, "");
  }
  return rex::Ok();
}

// REX_<FLAT NAME> (0.10) for the LARecomp settings, at the environment layer. The SDK reads REX_LARECOMP_<NAME> by itself.
void ApplyLegacyEnvironment() {
  State& s = S();
  std::vector<std::pair<std::string, rex::SettingBase*>> all;
  {
    std::lock_guard lock(s.mutex);  // a change callback may call Find: SetText runs without it
    all.assign(s.flat.begin(), s.flat.end());
  }
  for (const auto& [flat, setting] : all) {
    std::string var = "REX_";
    for (char c : flat) var += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (const auto value = rex::platform::GetEnvironmentValue(var)) {
      if (auto set = setting->SetText(rex::SettingLayer::kEnvironment, *value); !set) {
        REXLOG_WARN("settings: {}={} (environment) was refused: {}", var, *value, set.error().GetDescription());
      }
    }
  }
}

void UnregisterAll() {
  // First: the change callbacks, so that none runs into LARecomp code that is being torn down, and a call in progress on another thread
  // is waited for here (a guard's release waits for it). Not under the State mutex: RegisterChangeCallback takes the callbacks' mutex and
  // then FlatName takes this one.
  larecomp::cvar::ReleaseChangeCallbacks();
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (s.registry) {
    for (rex::SettingOwner* owner : s.owners) {
      s.registry->Unregister(*owner);
    }
  }
  s.registry = nullptr;
  s.flat.clear();
}

rex::SettingBase* Find(std::string_view name) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (const auto it = s.flat.find(std::string(name)); it != s.flat.end()) {
    return it->second;
  }
  if (!s.registry) {
    return nullptr;
  }
  std::string full(name);
  if (const auto it = s.sdk_by_old_name.find(full); it != s.sdk_by_old_name.end()) {
    full = it->second;
  }
  auto found = s.registry->Find(full);
  return found ? *found : nullptr;
}

std::string ResolveName(std::string_view name) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (const auto it = s.flat.find(std::string(name)); it != s.flat.end()) {
    return it->second->GetName();
  }
  if (const auto it = s.sdk_by_old_name.find(std::string(name)); it != s.sdk_by_old_name.end()) {
    return it->second;
  }
  return {};
}

std::string Get(std::string_view name) {
  if (name == "input_backend") {
    // The first of hid.drivers ("sdl3,xinput" -> sdl): what the 0.10 cvar said for a single driver.
    const std::string drivers = Get("hid.drivers");
    const std::string first = drivers.substr(0, drivers.find(','));
    return first == "sdl3" ? "sdl" : first;
  }
  if (name == "resolution_scale") {
    // One 0.10 name, two settings: the XENIA renderer's RENDER SCALE row (upscaling_menu.cpp) still reads and writes it.
    return Get("gpu.resolution_scale_x");
  }
  rex::SettingBase* setting = Find(name);
  return setting ? setting->GetText() : std::string();
}

bool Set(std::string_view name, std::string_view value) {
  if (name == "resolution_scale") {
    rex::SettingBase* x = Find("gpu.resolution_scale_x");
    rex::SettingBase* y = Find("gpu.resolution_scale_y");
    return x && y && x->SetText(rex::SettingLayer::kRuntime, value) && y->SetText(rex::SettingLayer::kRuntime, value);
  }
  rex::SettingBase* setting = Find(name);
  // Straight to the setting: the registry's by-name path would add an "old name used" notice per call.
  return setting && setting->SetText(rex::SettingLayer::kRuntime, value);
}

bool SetDefault(std::string_view name, std::string_view value) {
  State& s = S();
  rex::SettingBase* setting = Find(name);
  if (setting && s.registry) {
    // The registry keeps a value a higher layer holds and does not call that an error.
    return static_cast<bool>(s.registry->SetText(rex::SettingLayer::kTitle, setting->GetName(), value));
  }
  if (s.registry) {
    // A setting of a driver that is not loaded yet (gpu.d3d12.*): the value waits for its owner.
    std::string full(name);
    if (const auto it = s.sdk_by_old_name.find(full); it != s.sdk_by_old_name.end()) {
      full = it->second;
    }
    return static_cast<bool>(s.registry->SetText(rex::SettingLayer::kTitle, full, value));
  }
  return false;
}

std::string FlatName(const rex::SettingBase& setting) {
  State& s = S();
  const std::string& full = setting.GetName();
  if (full.compare(0, kScope.size(), kScope) == 0) {
    return full.substr(kScope.size());
  }
  std::lock_guard lock(s.mutex);
  if (const auto it = s.old_name_by_sdk.find(full); it != s.old_name_by_sdk.end()) {
    return it->second;
  }
  return full;
}

std::string_view Category(std::string_view flat_name) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  const auto it = s.category.find(std::string(flat_name));
  return it == s.category.end() ? std::string_view() : std::string_view(it->second);
}

// "gpu.d3d12.bindless" -> "GPU/D3D12": the settings panel groups the SDK's settings by scope, as the 0.10 cvar categories
// did. A LARecomp setting has the category its REXCVAR_DEFINE gave it (settings_meta.inc).
std::string CategoryOf(const rex::SettingBase& setting, std::string_view flat) {
  if (const std::string_view known = Category(flat); !known.empty()) {
    return std::string(known);
  }
  const std::string& scope = setting.GetOwner().GetScope();
  if (scope == "larecomp") {
    return {};
  }
  std::string out;
  size_t start = 0;
  while (start <= scope.size()) {
    const size_t dot = scope.find('.', start);
    std::string part = scope.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (part == "gpu" || part == "ui" || part == "hid" || part == "apu" || part == "xam" || part == "d3d12") {
      for (char& c : part) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    } else if (!part.empty()) {
      part[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
    }
    if (!out.empty()) out += '/';
    out += part;
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return out;
}

std::vector<rex::SettingBase*> AllLarecompSettings() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<rex::SettingBase*> all;
  for (const auto& [flat, setting] : s.flat) {
    all.push_back(setting);
  }
  std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return a->GetName() < b->GetName(); });
  return all;
}

std::vector<rex::SettingBase*> AllSettings() {
  State& s = S();
  rex::ISettingsRegistry* registry;
  {
    std::lock_guard lock(s.mutex);
    registry = s.registry;
  }
  return registry ? registry->Enumerate() : std::vector<rex::SettingBase*>{};
}

// -----------------------------------------------------------------------------------------------------------
// The player's larecomp.toml, before the SDK reads it (see larecomp_settings.h).
namespace {

// Same flattening as the SDK's loader (settings/toml.cpp CollectLeaves): nested tables become dotted names. An array is
// refused by the SDK; here it is skipped with a line in the log.
void CollectLeaves(const toml::table& table, const std::string& prefix, std::vector<std::pair<std::string, std::string>>& out) {
  for (auto&& [key, node] : table) {
    std::string name = prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
    if (auto* child = node.as_table()) {
      CollectLeaves(*child, name, out);
    } else if (node.is_array()) {
      REXLOG_WARN("larecomp.toml: '{}' is an array; no setting takes one, ignored", name);
    } else if (auto* b = node.as_boolean()) {
      out.emplace_back(name, b->get() ? "true" : "false");
    } else if (auto* i = node.as_integer()) {
      out.emplace_back(name, std::to_string(i->get()));
    } else if (auto* f = node.as_floating_point()) {
      out.emplace_back(name, rex::detail::RenderSetting(f->get()));
    } else if (auto* str = node.as_string()) {
      out.emplace_back(name, std::string(str->get()));
    }
  }
}

}  // namespace

int ApplyPlayerConfig(rex::ISettingsRegistry& registry, const std::filesystem::path& file) {
  std::error_code ec;
  if (file.empty() || !std::filesystem::exists(file, ec)) {
    return 0;
  }
  // Read through the path object (a non-ASCII folder name opens), as the SDK's loader does.
  std::string content;
  {
    std::ifstream in(file, std::ios::binary);
    if (!in) return 0;
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  toml::table table;
  try {
    table = toml::parse(content, file.string());
  } catch (const toml::parse_error&) {
    return 0;  // the SDK reports an unreadable file itself (and keeps the defaults)
  }
  const auto text_of = [&](const char* key) -> std::optional<std::string> {
    const toml::node* node = table.get(key);
    if (!node) return std::nullopt;
    if (auto* b = node->as_boolean()) return std::string(b->get() ? "true" : "false");
    if (auto* i = node->as_integer()) return std::to_string(i->get());
    if (auto* f = node->as_floating_point()) return rex::detail::RenderSetting(f->get());
    if (auto* str = node->as_string()) return std::string(str->get());
    return std::nullopt;
  };
  const auto put_title = [&](std::string_view name, const std::string& value) {
    (void)registry.SetText(rex::SettingLayer::kTitle, name, value);
  };

  // --- values whose meaning changed ---------------------------------------------------------------
  // The renderer. The old file chose XENIA with mcla_native_gfx_nocp = false or mcla_gpu = false (the pause
  // menu wrote the four cvars together); the new choice is gpu.driver. A gpu.driver line of the new kind in the
  // same file is a higher layer and wins. Neither old key is a setting any more (kRemovedNames): they are read here
  // from the file's own text, and the loop below hands them to the registry, which ignores them with a note.
  const auto nocp = text_of("mcla_native_gfx_nocp");
  const auto plume = text_of("mcla_gpu");
  if ((nocp && *nocp == "false") || (plume && *plume == "false")) {
    const auto api = text_of("graphics_backend");
    put_title("gpu.driver", (api && (*api == "d3d12" || *api == "vulkan")) ? *api : "any");
  }
  // One old number, two settings.
  if (const auto scale = text_of("resolution_scale")) {
    put_title("gpu.resolution_scale_x", *scale);
    put_title("gpu.resolution_scale_y", *scale);
  }
  // input_backend was "xinput" or "sdl"; hid.drivers is a list of driver names. The SDK registers "sdl3" and "xinput"
  // (src/hid/sdl3 and src/hid/xinput, modules rexhid-sdl3 and rexhid-xinput); its default is "sdl3" on every platform,
  // as the old default "sdl" was.
  if (const auto backend = text_of("input_backend")) {
    if (*backend == "sdl") put_title("hid.drivers", "sdl3");
    else if (*backend == "xinput") put_title("hid.drivers", "xinput");
  }

  // --- everything else, key by key ----------------------------------------------------------------
  std::vector<std::pair<std::string, std::string>> leaves;
  CollectLeaves(table, {}, leaves);
  // The old player row saved auto/rr/ours. Seed the new bool below the file layer so an explicit
  // toggle wins. The renderer only treats the chooser as an override at the environment layer or
  // above, including when the SDK reads the same legacy line again after OnConfigure.
  namespace dn = mcla::gpu::denoiser;
  const auto& chooser = mcla_gpu_denoiser;
  for (const auto& [name, text] : leaves) {
    if (name == dn::kCvarName || name == chooser.GetName()) {
      if (const auto legacy = dn::LegacyVendorPreference(text)) {
        put_title(mcla_gpu_vendor_denoiser.GetName(), *legacy ? "true" : "false");
      }
    }
  }
  int refused = 0;
  mcla::gpu::gfx_preset::FileFacts preset_facts;  // what the file says about the graphics presets (gpu/gfx_preset.h DecideInitial)
  for (const auto& [name, text] : leaves) {
    preset_facts.NoteKey(name);
    if (std::any_of(std::begin(kConvertedKeys), std::end(kConvertedKeys), [&](const char* k) { return name == k; })) {
      continue;
    }
    rex::Result<void> result = rex::Ok();
    if (rex::SettingBase* setting = Find(name)) {
      // Straight to the setting: the registry's by-name path would add an "old name used" notice for each of them,
      // and the SDK's own Load adds those once more.
      result = setting->SetText(rex::SettingLayer::kFile, text);
    } else {
      // Not registered yet (a driver that loads later) or not a setting: the registry keeps it for its owner.
      result = registry.SetText(rex::SettingLayer::kFile, name, text);
    }
    if (!result) {
      ++refused;
      REXLOG_WARN("larecomp.toml: {} = {} was not used ({}); the default stays", name, text, result.error().GetDescription());
    }
  }
  // A file with a covered value and no preset line starts on CUSTOM. Otherwise the setting defaults to VANILLA.
  // The title-layer default stays below the file and command line layers and is never written back.
  if (mcla::gpu::gfx_preset::DecideInitial(preset_facts) == mcla::gpu::gfx_preset::Preset::kCustom) {
    put_title("larecomp.mcla_gpu_preset", std::string(mcla::gpu::gfx_preset::Id(mcla::gpu::gfx_preset::Preset::kCustom)));
  }
  if (refused > 0) {
    REXLOG_WARN("larecomp.toml: {} value(s) refused; every other line was applied. The SDK will report the file as not "
                "used and keep {}.unreadable.bak: that is the same refusal, not a second problem",
                refused, file.filename().string());
  }
  return refused;
}

}  // namespace larecomp::settings

// -----------------------------------------------------------------------------------------------------------
namespace larecomp::cvar {

namespace {

namespace ls = larecomp::settings;

FlagType TypeOf(rex::SettingType t) {
  switch (t) {
    case rex::SettingType::kBool: return FlagType::Boolean;
    case rex::SettingType::kI32: return FlagType::Int32;
    case rex::SettingType::kI64: return FlagType::Int64;
    case rex::SettingType::kU32: return FlagType::Uint32;
    case rex::SettingType::kU64: return FlagType::Uint64;
    case rex::SettingType::kF64: return FlagType::Double;
    default: return FlagType::String;
  }
}

Source SourceOf(rex::SettingLayer layer) {
  switch (layer) {
    case rex::SettingLayer::kFile: return Source::kConfig;
    case rex::SettingLayer::kEnvironment: return Source::kEnvironment;
    case rex::SettingLayer::kCommandLine: return Source::kCommandLine;
    case rex::SettingLayer::kRuntime:
    case rex::SettingLayer::kLock: return Source::kRuntime;
    default: return Source::kDefault;  // kDefault and kTitle (LARecomp's own default)
  }
}

std::mutex g_marks_mutex;
std::set<std::string>& DebugMarks() {
  static larecomp::Immortal<std::set<std::string>> marks;  // read through Build (the settings panel's rows, the config save): not destroyed at exit either
  return *marks;
}

bool IsMarkedDebug(const std::string& full, const std::string& flat) {
  std::lock_guard lock(g_marks_mutex);
  return DebugMarks().count(full) != 0 || DebugMarks().count(flat) != 0;
}

FlagEntry Build(rex::SettingBase& s) {
  FlagEntry e;
  e.name = ls::FlatName(s);
  e.type = TypeOf(s.GetType());
  e.category = ls::CategoryOf(s, e.name);
  e.description = s.GetHelp();
  rex::SettingBase* p = &s;
  e.getter = [p] { return p->GetText(); };
  const rex::SettingConstraints& c = s.GetConstraints();
  // kInitOnly closes before OnInit and kLockedInGame when the title launches: either way it is read-only to anything
  // that runs afterwards (the settings panel, the pause menu).
  e.lifecycle = c.mode == rex::SettingMode::kRequiresRestart ? Lifecycle::kRequiresRestart
                : (c.mode == rex::SettingMode::kInitOnly || c.mode == rex::SettingMode::kLockedInGame) ? Lifecycle::kInitOnly
                                                                                                         : Lifecycle::kHotReload;
  e.constraints.min = c.min;
  e.constraints.max = c.max;
  e.constraints.allowed_values = c.allowed;
  e.default_value = s.GetDefaultText();
  e.is_debug_only = c.debug_only || IsMarkedDebug(s.GetName(), e.name);
  e.source = SourceOf(s.GetLayer());
  return e;
}

FlagEntry BuildCommand(rex::ConsoleCommand& command) {
  FlagEntry e;
  const std::string& full = command.GetName();
  e.name = full.compare(0, 9, "larecomp.") == 0 ? full.substr(9) : full;
  e.type = FlagType::Command;
  e.description = command.GetHelp();
  e.getter = [] { return std::string(); };
  e.category = "MCLA/Commands";
  rex::ConsoleCommand* p = &command;
  e.command_callback = [p](std::string_view args) { (void)p->Run(args); };
  return e;
}

std::mutex g_cb_mutex;
// The guards of the change callbacks, under g_cb_mutex. A guard released after its setting is gone (the SDK's settings are destroyed with their
// systems before the exe's static destruction) only drops its reference, because the SDK detaches the watches of a setting when the setting
// is destroyed (settings/watch_table.h): that is what the SDK's settings thread model gave back, and larecomp_watch_table_test checks it. The
// table itself is never destroyed (immortal.h, like every static of the exe that a thread the SDK left running can still reach: EXIT-HARD): a
// straggler calling RegisterChangeCallback or UnregisterChangeCallbacks while the exe's static destruction runs (mcla_fast_exit off) must not
// meet a destroyed map. Nothing is lost by it: UnregisterAll takes every guard out and releases it in OnShutdown, and RegisterChangeCallback
// adds none after that (the settings are not found any more). A guard is never released under g_cb_mutex: its release waits for a call in
// progress on another thread, and that call may ask for this mutex.
larecomp::settings::WatchTable<rex::ScopeGuard>& Guards() {
  static larecomp::settings::WatchTable<rex::ScopeGuard>& g = larecomp::Leak<larecomp::settings::WatchTable<rex::ScopeGuard>>();
  return g;
}

template <typename T>
void Watch(rex::SettingBase& s, const std::string& flat, const ChangeCallback& cb);

template <>
void Watch<std::string>(rex::SettingBase& s, const std::string& flat, const ChangeCallback& cb) {
  rex::SettingBase* base = &s;
  // frame_step_cap is not a Setting<std::string> (settings/validated_setting.h): the cast below would be undefined for it.
  if (auto* validated = dynamic_cast<ls::ValidatedString*>(&s)) {
    Guards().Add(flat, validated->OnChange([flat, cb, base](const std::string&) { cb(flat, base->GetText()); }));
    return;
  }
  auto& typed = static_cast<rex::Setting<std::string>&>(s);
  Guards().Add(flat, typed.OnChange([flat, cb, base](const std::string&) { cb(flat, base->GetText()); }));
}

template <typename T>
void Watch(rex::SettingBase& s, const std::string& flat, const ChangeCallback& cb) {
  auto& typed = static_cast<rex::Setting<T>&>(s);
  rex::SettingBase* base = &s;
  Guards().Add(flat, typed.OnChange([flat, cb, base](const T&) { cb(flat, base->GetText()); }));
}

}  // namespace

std::string GetFlagByName(std::string_view name) { return ls::Get(name); }

bool SetFlagByName(std::string_view name, std::string_view value) { return ls::Set(name, value); }

const FlagEntry* GetFlagInfo(std::string_view name) {
  rex::SettingBase* s = ls::Find(name);
  if (!s) return nullptr;
  thread_local FlagEntry entry;
  entry = Build(*s);
  return &entry;
}

Source GetFlagSource(std::string_view name) {
  rex::SettingBase* s = ls::Find(name);
  return s ? SourceOf(s->GetLayer()) : Source::kDefault;
}

std::vector<std::string> ListFlags() {
  std::vector<std::string> names;
  for (rex::SettingBase* s : ls::AllSettings()) names.push_back(ls::FlatName(*s));
  return names;
}

std::vector<FlagEntry>& GetRegistry() {
  static std::mutex mutex;
  static std::vector<FlagEntry>& entries = larecomp::Leak<std::vector<FlagEntry>>();
  static std::vector<rex::SettingBase*>& settings_of = larecomp::Leak<std::vector<rex::SettingBase*>>();  // entries[i] for i < size(), for the refresh
  static size_t built_for = static_cast<size_t>(-1);
  std::lock_guard lock(mutex);
  rex::ISettingsRegistry* registry = &rex::GetSettingsRegistry();
  const std::vector<rex::SettingBase*> now = registry->Enumerate();
  const std::vector<rex::ConsoleCommand*> commands = registry->EnumerateCommands();
  if (built_for != now.size() + commands.size()) {
    entries.clear();
    settings_of.clear();
    std::vector<std::pair<FlagEntry, rex::SettingBase*>> built;
    for (rex::SettingBase* s : now) built.emplace_back(Build(*s), s);
    for (rex::ConsoleCommand* c : commands) built.emplace_back(BuildCommand(*c), nullptr);
    // Grouped by category, then by name: the order the 0.10 registry walk gave the panel.
    std::stable_sort(built.begin(), built.end(), [](const auto& a, const auto& b) {
      return a.first.category != b.first.category ? a.first.category < b.first.category : a.first.name < b.first.name;
    });
    for (auto& [entry, setting] : built) {
      entries.push_back(std::move(entry));
      settings_of.push_back(setting);
    }
    built_for = now.size() + commands.size();
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    if (settings_of[i]) entries[i].source = SourceOf(settings_of[i]->GetLayer());
  }
  return entries;
}

void MarkDebugOnly(std::string_view name) {
  rex::SettingBase* s = ls::Find(name);
  std::lock_guard lock(g_marks_mutex);
  DebugMarks().insert(s ? s->GetName() : std::string(name));
}

bool RestoreValue(std::string_view name, std::string_view value, Source source) {
  rex::SettingBase* s = ls::Find(name);
  if (!s) return false;
  // A layer only rises, so the setting goes back to its default first (layer kDefault) and the value is stored again at
  // the layer it came from. A default that came from LARecomp's own start-up code (the title layer, reported as kDefault)
  // is restored at the title layer. One batch: both writes store at once, and each function watching the setting is called
  // once, with the restored value, instead of once with the default and once with the value.
  const rex::SettingsBatch batch;
  if (!s->Reset()) return false;
  if (source == Source::kDefault && value == s->GetDefaultText()) return true;
  rex::SettingLayer layer = rex::SettingLayer::kTitle;
  switch (source) {
    case Source::kConfig: layer = rex::SettingLayer::kFile; break;
    case Source::kEnvironment: layer = rex::SettingLayer::kEnvironment; break;
    case Source::kCommandLine: layer = rex::SettingLayer::kCommandLine; break;
    case Source::kRuntime: layer = rex::SettingLayer::kRuntime; break;
    case Source::kDefault: break;
  }
  return static_cast<bool>(s->SetText(layer, value));
}

bool HasNonDefaultValue(std::string_view name) {
  rex::SettingBase* s = ls::Find(name);
  return s && s->GetText() != s->GetDefaultText();
}

std::vector<std::string> ListModifiedFlags() {
  std::vector<std::string> names;
  for (rex::SettingBase* s : ls::AllSettings()) {
    if (s->GetText() != s->GetDefaultText()) names.push_back(ls::FlatName(*s));
  }
  return names;
}

void ResetToDefault(std::string_view name) {
  if (rex::SettingBase* s = ls::Find(name)) (void)s->Reset();
}

bool InvokeCommand(std::string_view name, std::string_view args) {
  // The three commands (gpu_trace_next, warp, menu_cam_dump) are rex::ConsoleCommand members of owners cvar_convert.py
  // emits next to their code; the registry runs one by its full name.
  const std::string full = std::string("larecomp.") + std::string(name);
  return static_cast<bool>(rex::GetSettingsRegistry().Run(full, args));
}

bool RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
  rex::SettingBase* s = ls::Find(name);
  if (!s) {
    // The old rex::cvar let a namespace-scope initializer add a callback (the REXCVAR_DEFINE next to it had registered the cvar at
    // static initialization). Here the settings exist from RegisterAll (OnConfigure) on: asked for earlier, the callback finds nothing
    // and is dropped. That was the trailer toolkit's "mcla_scene set at run time never starts" (scene_want.h): say so in the log
    // instead of dropping it in silence, and let the caller fall back.
    REXLOG_WARN("settings: no change callback for '{}': no such setting is registered (asked for before RegisterAll, or a driver that is not loaded)", name);
    return false;
  }
  std::lock_guard lock(g_cb_mutex);
  const std::string flat = ls::FlatName(*s);
  switch (s->GetType()) {
    case rex::SettingType::kBool: Watch<bool>(*s, flat, callback); break;
    case rex::SettingType::kI32: Watch<rex::i32>(*s, flat, callback); break;
    case rex::SettingType::kI64: Watch<rex::i64>(*s, flat, callback); break;
    case rex::SettingType::kU32: Watch<rex::u32>(*s, flat, callback); break;
    case rex::SettingType::kU64: Watch<rex::u64>(*s, flat, callback); break;
    case rex::SettingType::kF64: Watch<rex::f64>(*s, flat, callback); break;
    case rex::SettingType::kString: Watch<std::string>(*s, flat, callback); break;
    case rex::SettingType::kPath: Watch<std::filesystem::path>(*s, flat, callback); break;
  }
  return true;
}

void UnregisterChangeCallbacks(std::string_view name) {
  rex::SettingBase* s = ls::Find(name);
  if (!s) return;
  std::vector<rex::ScopeGuard> released;
  {
    std::lock_guard lock(g_cb_mutex);
    released = Guards().Take(ls::FlatName(*s));
  }
  // The guards go here, after the unlock: each waits for a call of its function that runs on another thread.
}

void ReleaseChangeCallbacks() {
  std::vector<rex::ScopeGuard> released;
  {
    std::lock_guard lock(g_cb_mutex);
    released = Guards().TakeAll();
  }
}

}  // namespace larecomp::cvar

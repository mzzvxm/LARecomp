#include "settings/larecomp_settings.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

#include <rex/logging/log_category.h>
#include <toml++/toml.hpp>

namespace larecomp::settings {

namespace {

struct SdkName {
  const char* old_name;
  const char* new_name;
};
constexpr SdkName kSdkNames[] = {
#include "settings/sdk_names.inc"
};

struct CategoryRow {
  const char* name;
  const char* category;
};
constexpr CategoryRow kCategories[] = {
#include "settings/generated/settings_meta.inc"
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

State& S() {
  static State s;
  return s;
}

constexpr std::string_view kScope = "larecomp.";

}  // namespace

rex::Result<void> RegisterAll(rex::ISettingsRegistry& registry) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.registry = &registry;
  s.owners.clear();
  s.owners.push_back(&g_app);
  for (rex::SettingOwner* owner : GeneratedOwners()) {
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
  return rex::Ok();
}

void UnregisterAll() {
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

std::string Get(std::string_view name) {
  rex::SettingBase* setting = Find(name);
  return setting ? setting->GetText() : std::string();
}

bool Set(std::string_view name, std::string_view value) {
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

// -----------------------------------------------------------------------------------------------------------
// Values whose meaning changed. Runs from OnConfigure, before the SDK reads the same file.
void MigrateLegacyConfig(rex::ISettingsRegistry& registry, const std::filesystem::path& file) {
  std::error_code ec;
  if (file.empty() || !std::filesystem::exists(file, ec)) {
    return;
  }
  toml::table table;
  try {
    table = toml::parse_file(file.string());
  } catch (const toml::parse_error&) {
    return;  // the SDK reports an unreadable file itself (and keeps the defaults)
  }
  const auto text_of = [&](const char* key) -> std::optional<std::string> {
    const toml::node* node = table.get(key);
    if (!node) return std::nullopt;
    if (auto* b = node->as_boolean()) return b->get() ? "true" : "false";
    if (auto* i = node->as_integer()) return std::to_string(i->get());
    if (auto* f = node->as_floating_point()) return std::to_string(f->get());
    if (auto* str = node->as_string()) return std::string(str->get());
    return std::nullopt;
  };
  const auto put = [&](std::string_view name, const std::string& value) {
    (void)registry.SetText(rex::SettingLayer::kTitle, name, value);
  };

  // The renderer. The old file chose XENIA with mcla_native_gfx_nocp = false or mcla_gpu = false (the pause
  // menu wrote the four cvars together); the new choice is gpu.driver. A gpu.driver line of the new kind in the
  // same file is a higher layer and wins.
  const auto nocp = text_of("mcla_native_gfx_nocp");
  const auto plume = text_of("mcla_gpu");
  if ((nocp && *nocp == "false") || (plume && *plume == "false")) {
    const auto api = text_of("graphics_backend");
    put("gpu.driver", (api && (*api == "d3d12" || *api == "vulkan")) ? *api : "any");
  }
  // One old number, two settings.
  if (const auto scale = text_of("resolution_scale")) {
    put("gpu.resolution_scale_x", *scale);
    put("gpu.resolution_scale_y", *scale);
  }
  // input_backend was "xinput" or "sdl"; the SDK's hid.drivers is a list of driver names.
  if (const auto backend = text_of("input_backend")) {
    if (*backend == "sdl") put("hid.drivers", "sdl3");
    else if (*backend == "xinput") put("hid.drivers", "xinput");
    // CHECK: the default driver list of hid.drivers on Windows and its names (rexhid-sdl3, rexhid-xinput).
  }
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

FlagEntry Build(rex::SettingBase& s) {
  FlagEntry e;
  e.name = ls::FlatName(s);
  e.type = TypeOf(s.GetType());
  e.category = std::string(ls::Category(e.name));
  e.description = s.GetHelp();
  rex::SettingBase* p = &s;
  e.getter = [p] { return p->GetText(); };
  const rex::SettingConstraints& c = s.GetConstraints();
  e.lifecycle = c.life == rex::SettingLife::kRestart ? Lifecycle::kRequiresRestart
                : c.life == rex::SettingLife::kStartupOnly ? Lifecycle::kInitOnly
                                                           : Lifecycle::kHotReload;
  e.constraints.min = c.min;
  e.constraints.max = c.max;
  e.constraints.allowed_values = c.allowed;
  e.default_value = s.GetDefaultText();
  e.is_debug_only = c.debug_only;
  e.source = SourceOf(s.GetLayer());
  return e;
}

std::mutex g_cb_mutex;
std::map<std::string, std::vector<rex::ScopeGuard>>& Guards() {
  static std::map<std::string, std::vector<rex::ScopeGuard>> g;
  return g;
}

template <typename T>
void Watch(rex::SettingBase& s, const std::string& flat, const ChangeCallback& cb) {
  auto& typed = static_cast<rex::Setting<T>&>(s);
  rex::SettingBase* base = &s;
  Guards()[flat].push_back(typed.OnChange([flat, cb, base](const T&) { cb(flat, base->GetText()); }));
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
  for (rex::SettingBase* s : ls::AllLarecompSettings()) names.push_back(ls::FlatName(*s));
  return names;
}

bool HasNonDefaultValue(std::string_view name) {
  rex::SettingBase* s = ls::Find(name);
  return s && s->GetText() != s->GetDefaultText();
}

std::vector<std::string> ListModifiedFlags() {
  std::vector<std::string> names;
  for (rex::SettingBase* s : ls::AllLarecompSettings()) {
    if (s->GetText() != s->GetDefaultText()) names.push_back(ls::FlatName(*s));
  }
  return names;
}

void ResetToDefault(std::string_view name) {
  if (rex::SettingBase* s = ls::Find(name)) (void)s->Reset();
}

bool InvokeCommand(std::string_view name, std::string_view args) {
  // CHECK: the three commands (gpu_trace_next, warp, menu_cam_dump) become rex::ConsoleCommand by hand
  // (rex/console_command.h); ISettingsRegistry::Run(name, args) runs one by its full name.
  (void)name;
  (void)args;
  return false;
}

void RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
  rex::SettingBase* s = ls::Find(name);
  if (!s) return;
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
}

void UnregisterChangeCallbacks(std::string_view name) {
  rex::SettingBase* s = ls::Find(name);
  if (!s) return;
  std::lock_guard lock(g_cb_mutex);
  Guards().erase(ls::FlatName(*s));
}

}  // namespace larecomp::cvar

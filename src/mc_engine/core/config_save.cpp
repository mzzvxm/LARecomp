#include "config_save.h"
#include "settings/immortal.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "profile/larecomp_profile_store.h"

namespace larecomp {

namespace {

namespace pf = larecomp::profiles;

bool IsDigits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(),
                                   [](unsigned char c) { return std::isdigit(c) != 0; });
}

// A TOML integer or float, bare: [+-] digits without a leading zero, then an
// optional .digits and an optional e[+-]digits; or inf and nan.
bool IsTomlNumber(std::string_view v) {
  if (!v.empty() && (v[0] == '-' || v[0] == '+')) {
    v.remove_prefix(1);
  }
  if (v == "inf" || v == "nan") {
    return true;
  }
  std::string_view mantissa = v;
  const size_t e = v.find_first_of("eE");
  if (e != std::string_view::npos) {
    mantissa = v.substr(0, e);
    std::string_view exponent = v.substr(e + 1);
    if (!exponent.empty() && (exponent[0] == '-' || exponent[0] == '+')) {
      exponent.remove_prefix(1);
    }
    if (!IsDigits(exponent)) {
      return false;
    }
  }
  const size_t dot = mantissa.find('.');
  if (dot != std::string_view::npos) {
    if (!IsDigits(mantissa.substr(dot + 1))) {
      return false;
    }
    mantissa = mantissa.substr(0, dot);
  }
  return IsDigits(mantissa) && (mantissa == "0" || mantissa[0] != '0');
}

// TOML integers are 64-bit signed.
bool IsTomlInteger(std::string_view v) {
  int64_t parsed = 0;
  const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed);
  return ec == std::errc() && end == v.data() + v.size() && IsTomlNumber(v);
}

// The parser rejects a string that is not UTF-8, and the file with it.
bool IsValidUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t more = 0;
    uint32_t code = 0;
    if ((c & 0xE0) == 0xC0) {
      more = 1;
      code = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      more = 2;
      code = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      more = 3;
      code = c & 0x07;
    } else {
      return false;
    }
    if (i + more >= s.size()) {
      return false;
    }
    for (size_t k = 1; k <= more; ++k) {
      const unsigned char next = static_cast<unsigned char>(s[i + k]);
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      code = (code << 6) | (next & 0x3F);
    }
    const uint32_t shortest = more == 1 ? 0x80 : (more == 2 ? 0x800 : 0x10000);
    if (code < shortest || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
      return false;
    }
    i += more + 1;
  }
  return true;
}

}  // namespace

std::optional<std::string> CvarTomlValue(std::string_view name, std::string_view value) {
  const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(name);
  bool bare = false;
  if (info) {
    switch (info->type) {
      case larecomp::cvar::FlagType::String:
        break;
      case larecomp::cvar::FlagType::Boolean:
        bare = value == "true" || value == "false";
        if (!bare) {
          return std::nullopt;
        }
        break;
      case larecomp::cvar::FlagType::Double:
        bare = IsTomlNumber(value);
        if (!bare) {
          return std::nullopt;
        }
        break;
      case larecomp::cvar::FlagType::Command:
        return std::nullopt;
      case larecomp::cvar::FlagType::Int32:
      case larecomp::cvar::FlagType::Int64:
      case larecomp::cvar::FlagType::Uint32:
      case larecomp::cvar::FlagType::Uint64:
        bare = IsTomlInteger(value);
        if (!bare) {
          return std::nullopt;
        }
        break;
    }
  } else {
    // Not registered here, so its type is unknown. A pause-menu row can still
    // hold a value for it (graphics_backend on an SDK without that cvar): a bare
    // word such as `graphics_backend = vulkan` is not TOML and made the whole
    // file unreadable.
    bare = value == "true" || value == "false" || IsTomlNumber(value);
  }
  if (bare) {
    return std::string(value);
  }
  if (!IsValidUtf8(value)) {
    return std::nullopt;
  }
  return pf::QuoteTomlString(value);
}

namespace {

using Edits = std::vector<pf::ConfigEdit>;

// The value each cvar had right after LARecomp's startup code set it, as the
// cvar's own getter prints it, so it compares with a later read of the same getter.
std::mutex g_startup_mutex;
std::unordered_map<std::string, std::string>& StartupValues() {
  static std::unordered_map<std::string, std::string>& values = larecomp::Leak<std::unordered_map<std::string, std::string>>();
  return values;
}

// The cvars a save never writes (NoteSessionOnlyCvar).
std::unordered_set<std::string>& SessionOnly() {
  static std::unordered_set<std::string>& names = larecomp::Leak<std::unordered_set<std::string>>();
  return names;
}

bool IsSessionOnly(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_startup_mutex);
  return SessionOnly().count(name) != 0;
}

bool AtStartupValue(const std::string& name, const std::string& value) {
  std::lock_guard<std::mutex> lock(g_startup_mutex);
  const auto it = StartupValues().find(name);
  return it != StartupValues().end() && it->second == value;
}

// The cvars a scene holds right now (SetCvarForScene): the value each had before the scene took it and where that value came from.
struct SceneHold {
  std::string value;
  larecomp::cvar::Source source = larecomp::cvar::Source::kDefault;
};
std::mutex g_scene_mutex;
std::unordered_map<std::string, SceneHold>& SceneHolds() {
  static std::unordered_map<std::string, SceneHold>& holds = larecomp::Leak<std::unordered_map<std::string, SceneHold>>();
  return holds;
}

std::unordered_map<std::string, SceneHold> SceneHoldsNow() {
  std::lock_guard<std::mutex> lock(g_scene_mutex);
  return SceneHolds();
}

// Adds the edit for `name`, or replaces the one already there.
void Put(Edits& edits, const std::string& name, std::optional<std::string> value,
         bool update_only) {
  if (!value) {
    REXLOG_WARN("Config: {} not saved, its value cannot be written as TOML", name);
    return;
  }
  for (pf::ConfigEdit& edit : edits) {
    if (edit.key == name) {
      edit.value = std::move(value);
      edit.update_only = update_only;
      return;
    }
  }
  edits.push_back({name, std::move(value), update_only});
}

bool Write(const std::filesystem::path& path, const Edits& edits) {
  if (path.empty()) {
    REXLOG_ERROR("Config: no path to save to");
    return false;
  }
  if (edits.empty()) {
    REXLOG_DEBUG("Config: nothing was changed, {} left alone", pf::Utf8FromPath(path));
    return true;
  }
  if (!pf::EditConfig(path, edits)) {
    REXLOG_ERROR("Config: could not write {}; the file is as it was", pf::Utf8FromPath(path));
    return false;
  }
  REXLOG_INFO("Saved config to {}", pf::Utf8FromPath(path));
  return true;
}

}  // namespace

void NoteSessionOnlyCvar(std::string_view name) {
  std::lock_guard<std::mutex> lock(g_startup_mutex);
  SessionOnly().insert(std::string(name));
}

void NoteStartupCvar(std::string_view name) {
  std::string value = larecomp::cvar::GetFlagByName(name);
  std::lock_guard<std::mutex> lock(g_startup_mutex);
  StartupValues()[std::string(name)] = std::move(value);
}

bool IsAtStartupValue(std::string_view name) {
  return AtStartupValue(std::string(name), larecomp::cvar::GetFlagByName(name));
}

bool SetCvarForScene(std::string_view name, std::string_view value) {
  const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(name);
  if (!info) {
    return false;
  }
  const std::string key(name);
  // The hold goes in before the value changes, so a save on another thread never sees the scene's value unheld.
  bool first = false;
  {
    SceneHold before{larecomp::cvar::GetFlagByName(name), info->source};
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    first = SceneHolds().try_emplace(key, std::move(before)).second;  // a second `set` of the same cvar keeps the first hold
  }
  if (!larecomp::cvar::SetFlagByName(name, value)) {
    if (first) {
      std::lock_guard<std::mutex> lock(g_scene_mutex);
      SceneHolds().erase(key);
    }
    return false;
  }
  return true;
}

bool RestoreCvarForScene(std::string_view name) {
  const std::string key(name);
  SceneHold hold;
  {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    const auto it = SceneHolds().find(key);
    if (it == SceneHolds().end()) {
      return false;
    }
    hold = it->second;
  }
  // Still held while it is put back, so a save in between writes the old value, not the scene's. The value goes back at the
  // layer it came from (RestoreValue: the registry only raises a layer, so it is reset first and stored again; the 0.10 code
  // wrote entry.source of the registry's own vector, which no longer exists).
  const bool ok = larecomp::cvar::RestoreValue(name, hold.value, hold.source);
  std::lock_guard<std::mutex> lock(g_scene_mutex);
  SceneHolds().erase(key);
  return ok;
}

bool SaveCvarsToConfig(const std::filesystem::path& path, const HeldCvars& held) {
  Edits edits;
  // Only cvars whose last writer was SetFlagByName: a menu, the settings panel,
  // the console. A value that came from the command line, the environment or the
  // file itself is not a change, and the file keeps its line for the last one.
  // The same for a value LARecomp's startup code gave (NoteStartupCvar) and that
  // nobody has changed since: it is not the player's choice. A line the file has
  // already still follows the value, so a cvar the player changed, saved and then
  // put back is not left at the changed value.
  // A cvar tagged .debug_only() that was changed while the game ran is not written at all, and
  // its line in the file, if any, is not touched: debug settings last one session, and a
  // tester's hand-written line keeps its value. That covers the DEBUG menu, the settings panel
  // and the console with no per-cvar call. (`held`, below, is not affected.)
  // A cvar a scene holds (the trailer toolkit's `set` key) is judged by what it was before the scene took it: the scene's value is not
  // the player's choice, so it is never written, and a value the player had changed earlier is written as the player left it.
  const std::unordered_map<std::string, SceneHold> scene_holds = SceneHoldsNow();
  for (const std::string& name : larecomp::cvar::ListFlags()) {
    const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(name);
    if (!info) {
      continue;
    }
    const auto hold = scene_holds.find(name);
    CvarSaveFacts facts;
    facts.is_command = info->type == larecomp::cvar::FlagType::Command;
    facts.session_only = IsSessionOnly(name);
    facts.changed_this_session = info->source == larecomp::cvar::Source::kRuntime;
    facts.debug_only = info->is_debug_only;
    facts.held_by_scene = hold != scene_holds.end();
    facts.changed_before_scene = facts.held_by_scene && hold->second.source == larecomp::cvar::Source::kRuntime;
    const CvarSaveAction action = DecideCvarSave(facts);
    if (action == CvarSaveAction::kSkip) {
      continue;
    }
    if (action == CvarSaveAction::kPutDefault) {
      // Not the live value, whatever set it: only a line the file has is put back to the default.
      Put(edits, name, CvarTomlValue(name, info->default_value), true);
      continue;
    }
    const std::string value =
        action == CvarSaveAction::kWriteBeforeScene ? hold->second.value : info->getter();
    // Save an explicit OFF even if this is the first write of the new toggle. A legacy "rr"
    // line can still supply the startup value until the file contains the new key.
    const bool unchanged = name != "mcla_gpu_vendor_denoiser" &&
                           (value == info->default_value || AtStartupValue(name, value));
    Put(edits, name, CvarTomlValue(name, value), unchanged);
  }
  for (const auto& [name, value] : held) {
    Put(edits, name, CvarTomlValue(name, value), false);
  }
  return Write(path, edits);
}

bool SaveHeldCvarsToConfig(const std::filesystem::path& path, const HeldCvars& held) {
  Edits edits;
  for (const auto& [name, value] : held) {
    Put(edits, name, CvarTomlValue(name, value), false);
  }
  return Write(path, edits);
}

}  // namespace larecomp

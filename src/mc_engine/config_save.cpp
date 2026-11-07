#include "config_save.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>

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
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
  bool bare = false;
  if (info) {
    switch (info->type) {
      case rex::cvar::FlagType::String:
        break;
      case rex::cvar::FlagType::Boolean:
        bare = value == "true" || value == "false";
        if (!bare) {
          return std::nullopt;
        }
        break;
      case rex::cvar::FlagType::Double:
        bare = IsTomlNumber(value);
        if (!bare) {
          return std::nullopt;
        }
        break;
      case rex::cvar::FlagType::Command:
        return std::nullopt;
      case rex::cvar::FlagType::Int32:
      case rex::cvar::FlagType::Int64:
      case rex::cvar::FlagType::Uint32:
      case rex::cvar::FlagType::Uint64:
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
  static std::unordered_map<std::string, std::string> values;
  return values;
}

// The cvars a save never writes (NoteSessionOnlyCvar).
std::unordered_set<std::string>& SessionOnly() {
  static std::unordered_set<std::string> names;
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
  std::string value = rex::cvar::GetFlagByName(name);
  std::lock_guard<std::mutex> lock(g_startup_mutex);
  StartupValues()[std::string(name)] = std::move(value);
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
  for (const std::string& name : rex::cvar::ListFlags()) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
    if (!info || info->type == rex::cvar::FlagType::Command) {
      continue;
    }
    if (IsSessionOnly(name)) {
      // Not the live value, whatever set it: only a line the file has is put back to the default.
      Put(edits, name, CvarTomlValue(name, info->default_value), true);
      continue;
    }
    if (info->source != rex::cvar::Source::kRuntime) {
      continue;
    }
    const std::string value = info->getter();
    const bool unchanged = value == info->default_value || AtStartupValue(name, value);
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

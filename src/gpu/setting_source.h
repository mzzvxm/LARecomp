// What the source checks of ao_halo_test, ao_paint_test and ao_size_test read of a setting: its default, whether it is debug-only, whether it is
// live (a change takes effect without a restart) and its range. The tests link nothing of the renderer, so they read the C++ text. A setting is
// written in one of two forms and Find reads both:
//   - rexglue 0.10 (main): REXCVAR_DEFINE_<TYPE>(name, default, "category", "help").debug_only().range(lo, hi).lifecycle(...kHotReload); in a .cpp
//   - rexglue 0.11: a row REX_SETTING(type, name, default, "help", rex::kDebugOnly, rex::Range{lo, hi}) in src/settings/generated/settings_<group>.h
//     (live: neither rex::kRestart nor rex::kStartupOnly among its traits)
// A header only, standard library only. setting_source_test.cpp checks it.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace setting_source {

struct Info {
  bool found = false;
  std::string default_text;  // as written: "0.4", "true", "\"auto\""
  bool debug_only = false;
  bool live = false;
  bool has_range = false;
  double range_lo = 0.0;
  double range_hi = 0.0;
};

namespace detail {

inline std::string Trim(const std::string& s) {
  const size_t first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return std::string();
  }
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// The index after the string literal that starts at text[at] (a quote), its escapes honoured.
inline size_t SkipString(const std::string& text, size_t at) {
  for (++at; at < text.size(); ++at) {
    if (text[at] == '\\') {
      ++at;
    } else if (text[at] == '"') {
      return at + 1;
    }
  }
  return text.size();
}

// The arguments of the call whose opening bracket ('(', '{' or '[') is text[open], split at the commas of its own level and trimmed.
// Strings and comments are skipped over (a help text holds commas, brackets and semicolons). *end is the index after the closing bracket.
// Empty when the call is not closed.
inline std::vector<std::string> CallArguments(const std::string& text, size_t open, size_t* end) {
  std::vector<std::string> args;
  int depth = 0;
  size_t start = open + 1;
  for (size_t i = open; i < text.size();) {
    const char c = text[i];
    if (c == '"') {
      i = SkipString(text, i);
      continue;
    }
    if (c == '/' && i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '*')) {
      const size_t stop = text[i + 1] == '/' ? text.find('\n', i) : text.find("*/", i + 2);
      if (stop == std::string::npos) {
        break;
      }
      i = stop + (text[i + 1] == '/' ? 1 : 2);
      continue;
    }
    if (c == '(' || c == '{' || c == '[') {
      ++depth;
    } else if (c == ')' || c == '}' || c == ']') {
      if (--depth == 0) {
        args.push_back(Trim(text.substr(start, i - start)));
        *end = i + 1;
        return args;
      }
    } else if (c == ',' && depth == 1) {
      args.push_back(Trim(text.substr(start, i - start)));
      start = i + 1;
    }
    ++i;
  }
  return {};
}

inline bool IsIdentifierChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The numbers of "lo, hi" inside a range's brackets or call.
inline void TakeRange(Info& info, const std::string& text, size_t open) {
  size_t end = 0;
  const std::vector<std::string> numbers = CallArguments(text, open, &end);
  if (numbers.size() == 2) {
    info.has_range = true;
    info.range_lo = std::strtod(numbers[0].c_str(), nullptr);
    info.range_hi = std::strtod(numbers[1].c_str(), nullptr);
  }
}

}  // namespace detail

// The 0.11 form: the REX_SETTING row of `name` in the text of a generated header.
inline Info FromRow(const std::string& text, const std::string& name) {
  Info info;
  const std::string macro = std::string("REX_") + "SETTING(";  // in two pieces: tools/check_settings_exposure.py would take the literal for a row
  for (size_t at = text.find(macro); at != std::string::npos; at = text.find(macro, at + 1)) {
    if (at > 0 && detail::IsIdentifierChar(text[at - 1])) {
      continue;
    }
    size_t end = 0;
    const std::vector<std::string> args = detail::CallArguments(text, at + macro.size() - 1, &end);
    if (args.size() < 3 || args[1] != name) {
      continue;
    }
    info.found = true;
    info.default_text = args[2];
    info.live = true;
    for (size_t i = 3; i < args.size(); ++i) {
      if (args[i] == "rex::kDebugOnly") {
        info.debug_only = true;
      } else if (args[i] == "rex::kRestart" || args[i] == "rex::kStartupOnly") {
        info.live = false;
      } else if (args[i].rfind("rex::Range{", 0) == 0) {
        detail::TakeRange(info, args[i], args[i].find('{'));
      }
    }
    return info;
  }
  return info;
}

// The 0.10 form: the REXCVAR_DEFINE_<TYPE> call of `name` and the calls chained to it, in the text of a .cpp.
inline Info FromDefine(const std::string& text, const std::string& name) {
  Info info;
  const std::string macro = std::string("REXCVAR_") + "DEFINE_";  // likewise
  for (size_t at = text.find(macro); at != std::string::npos; at = text.find(macro, at + 1)) {
    size_t open = at + macro.size();
    while (open < text.size() && detail::IsIdentifierChar(text[open])) {
      ++open;
    }
    if (open >= text.size() || text[open] != '(') {
      continue;  // the name of the macro family in a comment, not a call
    }
    size_t end = 0;
    const std::vector<std::string> args = detail::CallArguments(text, open, &end);
    if (args.size() < 2 || args[0] != name) {
      continue;
    }
    info.found = true;
    info.default_text = args[1];
    // .debug_only().range(lo, hi).lifecycle(rex::cvar::Lifecycle::kHotReload); in any order, up to the semicolon
    for (size_t i = end;;) {
      i = text.find_first_not_of(" \t\r\n", i);
      if (i == std::string::npos || text[i] != '.') {
        break;
      }
      size_t call = i + 1;
      while (call < text.size() && detail::IsIdentifierChar(text[call])) {
        ++call;
      }
      const std::string method = text.substr(i + 1, call - i - 1);
      if (call >= text.size() || text[call] != '(') {
        break;
      }
      size_t call_end = 0;
      const std::vector<std::string> chained = detail::CallArguments(text, call, &call_end);
      if (call_end == 0) {
        break;
      }
      if (method == "debug_only") {
        info.debug_only = true;
      } else if (method == "range") {
        detail::TakeRange(info, text, call);
      } else if (method == "lifecycle" && !chained.empty() && chained[0].find("kHotReload") != std::string::npos) {
        info.live = true;
      }
      i = call_end;
    }
    return info;
  }
  return info;
}

// The setting `name` as the tree under `gpu_dir` (the src/gpu folder) defines it: a row of src/settings/generated first, else the
// REXCVAR_DEFINE call in `cpp` (the text of the .cpp that held it before the conversion).
inline Info Find(const std::string& gpu_dir, const std::string& cpp, const std::string& name) {
  const std::filesystem::path generated = std::filesystem::path(gpu_dir) / ".." / "settings" / "generated";
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(generated, ec)) {
    const std::string file = entry.path().filename().string();
    if (file.rfind("settings_", 0) != 0 || entry.path().extension() != ".h") {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const Info info = FromRow(ss.str(), name);
    if (info.found) {
      return info;
    }
  }
  return FromDefine(cpp, name);
}

// The default as a number (a float setting's default is compared to the header's float constant, so to six digits).
inline bool DefaultIs(const Info& info, double value) {
  if (!info.found || info.default_text.empty()) {
    return false;
  }
  char* stop = nullptr;
  const double written = std::strtod(info.default_text.c_str(), &stop);
  return stop != info.default_text.c_str() && *stop == '\0' && std::fabs(written - value) <= 1e-6 * std::fmax(1.0, std::fabs(value));
}

inline bool DefaultIsTrue(const Info& info) { return info.found && info.default_text == "true"; }

}  // namespace setting_source

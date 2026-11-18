#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - ReXGlue Recompiled Project
//
// LARecomp's logging set-up on rexglue 0.11 (see spdlog_console.h): the SDK's logger does the writing, this file sets
// LARecomp's defaults on its log.* settings and finds the log file. No spdlog here: the SDK uses spdlog inside rexbase, so
// a copy linked into the exe would be a second registry whose sinks never see the SDK's lines.
#include "spdlog_console.h"

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/platform/file.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

namespace {

// The 0.10 file layout, which the analysis scripts and the support requests are used to:
//   [2026-11-13 14:05:09.123] [info] [mc] [t1234] text
// (the SDK's own default is "%Y-%m-%d %H:%M:%S.%e %Q %-10n %j %v"). One pattern serves the console, the debugger and the
// file in 0.11; the 0.10 console layout (coloured level, no time) is gone with the separate pattern. A player who wants
// another sets log.pattern.
constexpr const char* kLinePattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [t%t] %v";

// TitleApp("larecomp", ...): the name of the log file when log.file_name is empty.
constexpr const char* kAppName = "larecomp";

// The SDK's file naming (src/base/log_file.cpp RunNumber): "<name>_NNN.log", the digits ending at the first '.'.
int RunNumber(const std::filesystem::path& file, const std::string& prefix) {
  if (file.extension() != ".log") return 0;
  const std::string stem = file.stem().string();
  if (stem.compare(0, prefix.size(), prefix) != 0) return 0;
  std::string_view digits(stem);
  digits.remove_prefix(prefix.size());
  digits = digits.substr(0, digits.find('.'));
  int number = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
  return (error == std::errc() && end == digits.data() + digits.size() && !digits.empty()) ? number : 0;
}

// app.log_root ("{app.root}/logs" unless set), resolved. Not reachable through a getter: the setting belongs to the App.
std::filesystem::path LogRoot() {
  if (auto found = rex::GetSettingsRegistry().Find("app.log_root");
      found && *found != nullptr && (*found)->GetType() == rex::SettingType::kPath) {
    return static_cast<rex::Setting<std::filesystem::path>&>(**found).Get();
  }
  return rex::platform::GetExecutableFolder() / "logs";
}

}  // namespace

void InitLarecompLogging() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;

  // By name through the registry: a setting that is not registered yet (the category settings appear when their module
  // connects) keeps the value for its owner, so nothing here depends on the order of start-up.
  rex::SettingsRegistry& settings = rex::GetSettingsRegistry();
  const auto set = [&settings](rex::SettingLayer layer, const char* name, std::string_view text) {
    (void)settings.SetText(layer, name, text);
  };

  // LARECOMP_LOG_FILE named the log file itself (a harness points each run at its own). log.file_name is the same thing
  // when it is an absolute path: the SDK then writes that file with no run number. The environment layer is what the
  // variable always was: above larecomp.toml.
  if (const char* custom = std::getenv("LARECOMP_LOG_FILE"); custom && *custom) {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::absolute(custom, ec);
    if (ec) {
      path = custom;
    } else if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path(), ec);
    }
    set(rex::SettingLayer::kEnvironment, "log.file_name", path.string());
  }

  // REX_LOG_LEVEL is the SDK's own variable for log.level; 0.10 also took "warning" and "err".
  if (const char* env_level = std::getenv("REX_LOG_LEVEL"); env_level && *env_level) {
    std::string level = env_level;
    for (char& c : level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (level == "warning") {
      set(rex::SettingLayer::kEnvironment, "log.level", "warn");
    } else if (level == "err") {
      set(rex::SettingLayer::kEnvironment, "log.level", "error");
    }
  }

  // The 0.10 defaults: debug for everything (core, kernel, mc, larecomp.app ...) except cpu and gpu at info, and the crash
  // handler's category at trace. log.flush_level (warn) and log.flush_interval (1 s) are the SDK's defaults and were
  // LARecomp's flush_on(warn) / flush_every(1 s).
  set(rex::SettingLayer::kTitle, "log.level", "debug");
  set(rex::SettingLayer::kTitle, "log.pattern", kLinePattern);
  set(rex::SettingLayer::kTitle, "log.categories.cpu", "info");
  set(rex::SettingLayer::kTitle, "log.categories.gpu", "info");
  set(rex::SettingLayer::kTitle, "log.categories.larecomp_crash", "trace");
}

void ShutdownLarecompLogging() {
  rex::log::Flush();
}

std::filesystem::path LarecompLogFile() {
  const std::string named = rex::GetLogger().file_name.Get();
  if (!named.empty()) {
    const std::filesystem::path path(named);  // as the SDK reads it (src/base/logger.cpp OpenFile)
    if (path.is_absolute()) {
      return path;
    }
  }
  const std::string prefix = (named.empty() ? std::string(kAppName) : named) + "_";
  const std::filesystem::path folder = LogRoot();
  std::filesystem::path newest;
  int highest = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (const int number = RunNumber(entry.path(), prefix); number > highest) {
      highest = number;
      newest = entry.path();
    }
  }
  return newest;
}

const char* LarecompLogFilePath() {
  thread_local std::string text;
  text = LarecompLogFile().string();
  return text.c_str();
}

std::filesystem::path LarecompLogFolder() {
  const std::filesystem::path file = LarecompLogFile();
  if (!file.empty() && file.has_parent_path()) {
    return file.parent_path();
  }
  return LogRoot();
}

#endif // REXGLUE_HAS_XEO3_TARGET

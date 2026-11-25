// A check that needs no game: LARecomp's log categories are added to the process logger before their first line.
//   cmake --build <build dir> --target larecomp_log_category_test, then run <build dir>/larecomp_log_category_test (exit 0 = passed).
// It links rex::base (rexbase.dll next to it or on the PATH) and the SDK's operator new and delete forwarders, like the other checks of the
// SDK's own settings and logger (larecomp_watch_table_test), and compiles spdlog_console.cpp itself.
//
// The case of 2026-11-22: every run ended with
//   [warning] [core] log category larecomp_crash is not registered
//   [info] [larecomp_crash] Process exiting normally (exit/return from main)
// and larecomp_app and mc opened the same way. The categories were added in LarecompApp::OnConfigure through
// AppInterfaces::Get<ILogger>(), which is null there (the SDK publishes its interface factory after the step that runs OnConfigure), so
// AddCategories never ran: a category the logger has not been given opens with that warning, writes at log.level, and has no
// log.categories.<name> setting (larecomp_crash's "trace" had nothing to land on).
//
// What it proves:
//   * THE CONTROL: a category nobody adds opens with the warning, once (if it did not, the check below would prove nothing);
//   * THE FIX: after InitLarecompLogging, the first line of larecomp_app, larecomp_crash and mc carries no warning, and the same for the
//     very last line of a run (the crash handler's "Process exiting normally" is the first line of its category);
//   * THE SETTING: each of the three has its log.categories.<name> setting, and larecomp_crash's title-layer "trace" landed on it;
//   * TWICE: InitLarecompLogging a second time changes nothing (the app constructor may run it again in a restart path), and a second
//     AddLarecompLogCategories is refused by the SDK (the error comes back, nothing is added twice).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "larecomp_log.h"
#include "mc_engine/core/logging.h"
#include "spdlog_console.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

// A category that no code adds: what larecomp_crash was.
inline constinit rex::log::LogCategory g_never_added{"larecomp_test_never_added", "a category that nobody adds"};

// The lines the capture sink holds since `after`, as "category: text".
std::vector<std::string> LinesSince(const rex::log::LogCaptureSink& sink, uint64_t after) {
  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(after, messages);
  std::vector<std::string> lines;
  lines.reserve(messages.size());
  for (const rex::log::CapturedMessage& m : messages) lines.push_back(m.category + ": " + m.text);
  return lines;
}

bool AnyHas(const std::vector<std::string>& lines, const std::string& needle) {
  for (const std::string& line : lines) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool HasSetting(const char* name) {
  auto found = rex::GetSettingsRegistry().Find(name);
  return found && *found != nullptr;
}

}  // namespace

int main() {
  rex::log::LogCaptureSink sink;
  rex::ScopeGuard attached = rex::GetLogger().AddSink(sink);

  // THE CONTROL.
  uint64_t mark = sink.GetLatestSequence();
  REXLOG_CAT_INFO(g_never_added, "its first line");
  REXLOG_CAT_INFO(g_never_added, "its second line");
  std::vector<std::string> lines = LinesSince(sink, mark);
  CHECK(AnyHas(lines, "log category larecomp_test_never_added is not registered"));
  int warnings = 0;
  for (const std::string& line : lines) {
    if (line.find("is not registered") != std::string::npos) ++warnings;
  }
  CHECK(warnings == 1);  // once: the second line finds the slot

  // THE FIX.
  InitLarecompLogging();
  mark = sink.GetLatestSequence();
  LARECOMP_APP_INFO("the first line of larecomp_app");
  MC_INFO("the first line of mc");
  LARECOMP_CRASH_INFO("Process exiting normally (exit/return from main)");  // the crash handler's only line in a good run
  lines = LinesSince(sink, mark);
  CHECK(!AnyHas(lines, "is not registered"));
  CHECK(AnyHas(lines, "larecomp_app: the first line of larecomp_app"));
  CHECK(AnyHas(lines, "mc: the first line of mc"));
  CHECK(AnyHas(lines, "larecomp_crash: Process exiting normally (exit/return from main)"));

  // THE SETTING.
  CHECK(HasSetting("log.categories.larecomp_app"));
  CHECK(HasSetting("log.categories.larecomp_crash"));
  CHECK(HasSetting("log.categories.mc"));
  CHECK(larecomp::log::Crash.IsEnabled(rex::log::Level::kTrace));  // InitLarecompLogging's title-layer "trace"

  // TWICE.
  InitLarecompLogging();
  const auto again = AddLarecompLogCategories();
  CHECK(!again);
  mark = sink.GetLatestSequence();
  LARECOMP_CRASH_INFO("after the second call");
  lines = LinesSince(sink, mark);
  CHECK(!AnyHas(lines, "is not registered"));
  CHECK(AnyHas(lines, "larecomp_crash: after the second call"));

  attached.Reset();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

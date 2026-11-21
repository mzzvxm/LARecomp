#pragma once
//
// What a crash does when nobody is there to read a message box: the test harness's switch (the pure half of it; the handlers are
// in crash_handler.cpp and LarecompApp::GetCrashOptions).
//
// A crashed game writes its crash report to the log and then, for a player, shows a modal box ("<app> crashed. The log file has the
// details.", rexglue's crash reporter, CrashReporterOptions::show_dialog) and stays alive until it is closed. A test run has nobody to close it
// (2026-11-20 and 2026-11-21 two runs sat on the box until someone saw it, and every run queued behind them waited too).
// LARECOMP_NO_CRASH_DIALOG=1 in the environment makes the crash skip the box and end the process right after the report is
// written and the log is flushed. Players never set it: the box stays the default.
//
// Environment, not a setting: it has to be in force before the settings load (a crash during start-up included), the harness
// sets it for every run, and a setting would need a place in the menus and the settings review for something no player wants.

#include <cstdlib>
#include <string_view>

namespace larecomp::crash {

// The variable's name.
inline constexpr const char* kNoDialogEnv = "LARECOMP_NO_CRASH_DIALOG";

// The exit status of a process that ended itself after a crash with the box switched off (an access violation or another
// exception the unhandled-exception filter sees). 0xC4A50001 reads "CRASh 1" and is no Windows exception code, so a harness
// tells it from a crash that went on to Windows Error Reporting (the exception code itself, 0xC0000005 ...) and from the 3 that abort(),
// std::terminate and a failed CRT check end with (those paths write the report and end the process without a box either way).
inline constexpr unsigned kNoDialogExitCode = 0xC4A50001u;

inline bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// The variable's value says yes: "1", "true", "yes" or "on" in any case. Unset, empty, "0", "false" and anything else say no (a
// stray value must never silence the box of a player's game).
inline bool NoDialogRequested(const char* value) {
    if (!value) return false;
    const std::string_view v(value);
    return v == "1" || EqualsNoCase(v, "true") || EqualsNoCase(v, "yes") || EqualsNoCase(v, "on");
}

inline bool NoDialogFromEnvironment() {
    return NoDialogRequested(std::getenv(kNoDialogEnv));
}

}  // namespace larecomp::crash

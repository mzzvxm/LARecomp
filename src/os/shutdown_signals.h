#pragma once
// ===========================================================================
// SIGTERM, SIGINT and SIGHUP end the game the way its window's close button does (Linux)
// ===========================================================================
// Who sends them: Steam (on Linux and in the Steam Deck's game mode) stops a native game with SIGTERM and follows with SIGKILL when it is
// still there after a while; a terminal's Ctrl+C is SIGINT; closing the terminal the game was started from is SIGHUP. All three should
// land in the normal quit (the saves finish, the devices are given back, the exit order of the app's Shutdown), not in the default
// action, which ends the process on the spot.
//
// The SDK's app (rex::app::App::Init) already turns SIGINT and SIGTERM into rex::app::RequestShutdown(0). What it does not do, and this
// does:
//   * SIGHUP (the default action would end the game in the middle of a save),
//   * a way out of a quit that hangs: a signal that comes later than kRepeatWindow after the first ends the process at once,
//   * a line in the log that says which signal came (the SDK's handler is silent: a quit that began from outside looked like nothing).
// The SDK's handlers stay in place under these: every signal is passed on to the action that was installed before (a signal in the window
// of a repeat is only passed on), so the quit does not depend on this code alone.
//
// A signal handler may do almost nothing. Here it does three async-signal-safe things: reads the monotonic clock, writes one 16-byte record
// to a pipe (a write of that size is atomic) and calls the previous action. A helper thread of its own reads the pipe and runs the two
// callbacks, where locks, the log and RequestShutdown are all fine.
//
// Used by larecomp_app.cpp (OnInit). Linux only: elsewhere Install returns false and nothing changes (Windows keeps the SDK's console
// handler, which already turns Ctrl+C, Ctrl+Break and closing the console into the same shutdown request).
// Check: os/shutdown_signals_test.cpp (cmake --build <dir> --target larecomp_shutdown_signals_test; exit 0 = passed).
// ===========================================================================

#include <chrono>
#include <functional>

namespace larecomp::os {

// What a signal means once the first one has come.
enum class ShutdownSignalStep {
  kQuit,    // the first: ask for the normal quit
  kIgnore,  // within kRepeatWindow of the first: the same request again (see below)
  kForce,   // a later one: the quit is taking too long for whoever sends it, or they mean it: end the process now
};

// The rule that tells the three apart, without any signal in it (so it can be tested alone).
//
// Why a window: `timeout` and some launchers send the signal to the process and again to its whole process group, a few microseconds apart,
// and one Ctrl+C reaches a script and the game it started. Those are one request. A person who presses Ctrl+C a second time because the
// quit is slow does it later than a quarter of a second.
class ShutdownSignalPolicy {
 public:
  static constexpr std::chrono::milliseconds kRepeatWindow{250};

  // `at`: when the signal arrived, on any monotonic clock (the same one for every call).
  ShutdownSignalStep OnSignal(std::chrono::nanoseconds at);

 private:
  bool first_seen_ = false;
  std::chrono::nanoseconds first_at_{};
};

struct ShutdownSignalActions {
  // The first signal. Runs on the helper thread, never inside the signal handler. `signal` is the number (SIGTERM, ...).
  std::function<void(int signal)> quit;
  // A later one. Runs on the helper thread. The app writes its log and ends the process here (larecomp::EndProcessNow).
  std::function<void(int signal)> force;
};

// "SIGTERM", "SIGINT" and "SIGHUP" for the three this file handles, "signal N" for any other.
const char* ShutdownSignalName(int signal);

// Linux: installs the handlers for SIGTERM and SIGINT, and for SIGHUP unless it was ignored when this ran (a game started with nohup must
// stay running after its terminal closes), and starts the helper thread. False on another OS, or when it is already installed. The thread
// and the handlers stay until the process ends: the app never removes them (a second Ctrl+C has to work during the shutdown too).
bool InstallShutdownSignals(ShutdownSignalActions actions);

// For the check only: stops the helper thread, puts the previous actions back and forgets the state. No signal may be on its way.
void RemoveShutdownSignals();

}  // namespace larecomp::os

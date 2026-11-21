// Standalone check of the termination signals (os/shutdown_signals.h): the rule that tells the first signal from a repeat and from a later
// one (every OS), and on Linux the real thing: signals raised in this process reach the helper thread's callbacks (not the handler), the
// previous action is called, a later signal forces, SIGHUP is handled and stays ignored when it was ignored, a callback that throws does not
// end the helper thread, and Remove puts the old actions back. No game, no SDK.
//
//   cmake --build <dir> --target larecomp_shutdown_signals_test, then run it (exit 0 = passed).
//   Or by hand:  clang++ -std=c++23 -I src src/os/shutdown_signals_test.cpp src/os/shutdown_signals.cpp -pthread
//
// What it cannot say: that Steam's own SIGTERM and a terminal's Ctrl+C reach a running game (needs the game: the Linux boot check sends the
// signal with `timeout --signal=TERM`; see perf\scripts\linux-catchup\lxc-boot.sh), and that the handler is async-signal-safe (read from
// the code: a clock read, one write, the previous action).

#include "os/shutdown_signals.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <signal.h>
#endif

namespace os = larecomp::os;
using Step = os::ShutdownSignalStep;
using namespace std::chrono_literals;

namespace {

int g_failed = 0;
int g_checked = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    ++g_checked;                                                   \
    if (!(cond)) {                                                 \
      ++g_failed;                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
    }                                                              \
  } while (0)

void TestPolicy() {
  using std::chrono::nanoseconds;
  const auto at = [](std::chrono::milliseconds ms) { return std::chrono::duration_cast<nanoseconds>(ms); };

  os::ShutdownSignalPolicy policy;
  CHECK(policy.OnSignal(at(1000ms)) == Step::kQuit);
  // The copies a tool sends (the process and again its group), a few microseconds apart: one request.
  CHECK(policy.OnSignal(at(1000ms) + 5us) == Step::kIgnore);
  CHECK(policy.OnSignal(at(1000ms) + 2ms) == Step::kIgnore);
  CHECK(policy.OnSignal(at(1000ms) + 249ms) == Step::kIgnore);
  // A person's second Ctrl+C, and every one after it.
  CHECK(policy.OnSignal(at(1000ms) + 250ms) == Step::kForce);
  CHECK(policy.OnSignal(at(2500ms)) == Step::kForce);
  CHECK(policy.OnSignal(at(2500ms) + 1us) == Step::kForce);

  // The window runs from the first signal: copies that keep coming do not keep it open.
  os::ShutdownSignalPolicy trickle;
  CHECK(trickle.OnSignal(at(0ms)) == Step::kQuit);
  CHECK(trickle.OnSignal(at(200ms)) == Step::kIgnore);
  CHECK(trickle.OnSignal(at(400ms)) == Step::kForce);

  // A fresh policy starts over.
  os::ShutdownSignalPolicy fresh;
  CHECK(fresh.OnSignal(at(9000ms)) == Step::kQuit);

  CHECK(std::string(os::ShutdownSignalName(15)).size() > 0);  // never null
#if defined(SIGTERM)
  CHECK(std::string(os::ShutdownSignalName(SIGTERM)) == "SIGTERM");
  CHECK(std::string(os::ShutdownSignalName(SIGINT)) == "SIGINT");
#endif
#if defined(__linux__)
  CHECK(std::string(os::ShutdownSignalName(SIGHUP)) == "SIGHUP");
  CHECK(std::string(os::ShutdownSignalName(SIGUSR1)) == "signal " + std::to_string(SIGUSR1));
#endif
}

#if defined(__linux__)

// What the callbacks saw, and a way to wait for it.
struct Seen {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<int> quits;
  std::vector<int> forces;
  std::vector<std::thread::id> threads;

  os::ShutdownSignalActions Actions(bool quit_throws = false) {
    os::ShutdownSignalActions actions;
    actions.quit = [this, quit_throws](int signal) {
      {
        std::lock_guard lock(mutex);
        quits.push_back(signal);
        threads.push_back(std::this_thread::get_id());
      }
      changed.notify_all();
      if (quit_throws) throw 42;
    };
    actions.force = [this](int signal) {
      {
        std::lock_guard lock(mutex);
        forces.push_back(signal);
        threads.push_back(std::this_thread::get_id());
      }
      changed.notify_all();
    };
    return actions;
  }

  bool WaitFor(size_t quit_count, size_t force_count, std::chrono::milliseconds limit = 5000ms) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, limit, [&] { return quits.size() >= quit_count && forces.size() >= force_count; });
  }

  // True when nothing more comes in `quiet` (for "nothing happened").
  bool Unchanged(size_t quit_count, size_t force_count, std::chrono::milliseconds quiet = 150ms) {
    std::this_thread::sleep_for(quiet);
    std::lock_guard lock(mutex);
    return quits.size() == quit_count && forces.size() == force_count;
  }
};

std::atomic<int> g_previous_calls{0};
void PreviousHandler(int) { g_previous_calls.fetch_add(1); }

bool SameAction(const struct sigaction& a, const struct sigaction& b) {
  return a.sa_handler == b.sa_handler && ((a.sa_flags & SA_SIGINFO) == (b.sa_flags & SA_SIGINFO));
}

void SetHandler(int signal, void (*handler)(int)) {
  struct sigaction action{};
  action.sa_handler = handler;
  sigemptyset(&action.sa_mask);
  sigaction(signal, &action, nullptr);
}

struct sigaction ActionOf(int signal) {
  struct sigaction action{};
  sigaction(signal, nullptr, &action);
  return action;
}

void TestSignals() {
  const std::thread::id main_thread = std::this_thread::get_id();

  // --- the whole sequence of a quit that has to be forced ---
  {
    SetHandler(SIGTERM, PreviousHandler);  // stands for the SDK's forwarder
    SetHandler(SIGINT, SIG_DFL);
    SetHandler(SIGHUP, SIG_DFL);
    const struct sigaction before_term = ActionOf(SIGTERM);
    g_previous_calls = 0;

    Seen seen;
    CHECK(os::InstallShutdownSignals(seen.Actions()));
    CHECK(!os::InstallShutdownSignals(seen.Actions()));  // once
    CHECK(!SameAction(ActionOf(SIGTERM), before_term));   // ours is in
    CHECK(ActionOf(SIGINT).sa_flags & SA_SIGINFO);        // so is the one for SIGINT

    const auto t0 = std::chrono::steady_clock::now();
    raise(SIGTERM);
    CHECK(g_previous_calls.load() == 1);  // the handler passed it on, before raise() returned
    raise(SIGTERM);                       // the same request again, microseconds later
    const auto spread = std::chrono::steady_clock::now() - t0;
    CHECK(seen.WaitFor(1, 0));
    CHECK(g_previous_calls.load() == 2);  // a repeat is still passed on
    if (spread < 100ms) {
      CHECK(seen.Unchanged(1, 0));  // ...and is not a second quit and not a force
    } else {
      std::printf("  note: two raises took %lld ms on this machine; the repeat window was not tested\n",
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(spread).count()));
    }
    {
      std::lock_guard lock(seen.mutex);
      CHECK(seen.quits.size() == 1 && seen.quits[0] == SIGTERM);
      CHECK(!seen.threads.empty() && seen.threads[0] != main_thread);  // not inside the handler, not on the raising thread
    }

    std::this_thread::sleep_for(os::ShutdownSignalPolicy::kRepeatWindow + 100ms);
    raise(SIGINT);  // a later one: force (SIGINT's previous action was the default: it must not end this process)
    CHECK(seen.WaitFor(1, 1));
    raise(SIGHUP);  // SIGHUP is handled too (also not the default action)
    CHECK(seen.WaitFor(1, 2));
    {
      std::lock_guard lock(seen.mutex);
      CHECK(seen.forces.size() == 2 && seen.forces[0] == SIGINT && seen.forces[1] == SIGHUP);
      CHECK(seen.quits.size() == 1);
    }

    os::RemoveShutdownSignals();
    CHECK(SameAction(ActionOf(SIGTERM), before_term));  // the previous action is back
    SetHandler(SIGTERM, SIG_DFL);
  }

  // --- SIGHUP first: the quit, with its number ---
  {
    SetHandler(SIGHUP, SIG_DFL);
    Seen seen;
    CHECK(os::InstallShutdownSignals(seen.Actions()));
    raise(SIGHUP);
    CHECK(seen.WaitFor(1, 0));
    {
      std::lock_guard lock(seen.mutex);
      CHECK(seen.quits.size() == 1 && seen.quits[0] == SIGHUP);
    }
    os::RemoveShutdownSignals();
  }

  // --- a SIGHUP that was ignored stays ignored (nohup); the other two are handled ---
  {
    SetHandler(SIGHUP, SIG_IGN);
    SetHandler(SIGINT, SIG_DFL);
    Seen seen;
    CHECK(os::InstallShutdownSignals(seen.Actions()));
    CHECK(ActionOf(SIGHUP).sa_handler == SIG_IGN);
    raise(SIGHUP);  // would end this process if the action had been replaced and then restored wrongly
    CHECK(seen.Unchanged(0, 0));
    raise(SIGINT);
    CHECK(seen.WaitFor(1, 0));
    os::RemoveShutdownSignals();
    CHECK(ActionOf(SIGHUP).sa_handler == SIG_IGN);
    SetHandler(SIGHUP, SIG_DFL);
  }

  // --- a callback that throws does not end the helper thread ---
  {
    Seen seen;
    CHECK(os::InstallShutdownSignals(seen.Actions(/*quit_throws=*/true)));
    raise(SIGTERM);
    CHECK(seen.WaitFor(1, 0));
    std::this_thread::sleep_for(os::ShutdownSignalPolicy::kRepeatWindow + 100ms);
    raise(SIGINT);
    CHECK(seen.WaitFor(1, 1));  // the thread still runs
    os::RemoveShutdownSignals();
  }

  // --- Remove with nothing installed is fine; Install works again afterwards ---
  {
    os::RemoveShutdownSignals();
    Seen seen;
    CHECK(os::InstallShutdownSignals(seen.Actions()));
    raise(SIGTERM);
    CHECK(seen.WaitFor(1, 0));
    os::RemoveShutdownSignals();
  }
}

#else

void TestSignals() {
  // Not Linux: nothing is installed, and says so.
  os::ShutdownSignalActions actions;
  CHECK(!os::InstallShutdownSignals(std::move(actions)));
  os::RemoveShutdownSignals();
}

#endif

}  // namespace

int main() {
  TestPolicy();
  TestSignals();
  std::printf("shutdown signals: %d checks, %d failed\n", g_checked, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// SIGTERM, SIGINT and SIGHUP end the game the way its window's close button does. See shutdown_signals.h.

#include "os/shutdown_signals.h"

#include <csignal>
#include <cstdio>
#include <utility>

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <thread>
#endif

namespace larecomp::os {

ShutdownSignalStep ShutdownSignalPolicy::OnSignal(std::chrono::nanoseconds at) {
  if (!first_seen_) {
    first_seen_ = true;
    first_at_ = at;
    return ShutdownSignalStep::kQuit;
  }
  // Measured from the first signal, not from the last: a train of copies of one request must not keep the window open for ever.
  if (at - first_at_ < kRepeatWindow) {
    return ShutdownSignalStep::kIgnore;
  }
  return ShutdownSignalStep::kForce;
}

const char* ShutdownSignalName(int signal) {
#if defined(SIGTERM)
  if (signal == SIGTERM) return "SIGTERM";
#endif
#if defined(SIGINT)
  if (signal == SIGINT) return "SIGINT";
#endif
#if defined(SIGHUP)
  if (signal == SIGHUP) return "SIGHUP";
#endif
  static thread_local char text[32];
  std::snprintf(text, sizeof(text), "signal %d", signal);
  return text;
}

#if defined(__linux__)

namespace {

constexpr int kSignals[] = {SIGTERM, SIGINT, SIGHUP};
constexpr int kSignalCount = 3;

int IndexOf(int signal) {
  for (int i = 0; i < kSignalCount; ++i) {
    if (kSignals[i] == signal) return i;
  }
  return -1;
}

// What the handler writes and the helper thread reads: a 16-byte record, so one write is atomic (PIPE_BUF is 4096).
struct Record {
  std::int32_t signal;  // 0: stop (RemoveShutdownSignals)
  std::int32_t reserved;
  std::int64_t at_ns;   // CLOCK_MONOTONIC
};
static_assert(sizeof(Record) == 16);

struct State {
  int pipe_read = -1;
  int pipe_write = -1;  // non-blocking: a full pipe drops the record instead of stopping the handler
  struct sigaction previous[kSignalCount] = {};
  bool installed[kSignalCount] = {};
  ShutdownSignalActions actions;
  ShutdownSignalPolicy policy;  // only the helper thread uses it
  std::thread thread;
};

std::atomic<State*> g_state{nullptr};

std::int64_t NowNs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);  // async-signal-safe
  return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
}

// The signal handler: async-signal-safe things only (clock_gettime, write, errno, the previous action, which is itself a handler).
void OnSignal(int signal, siginfo_t* info, void* context) {
  const int saved_errno = errno;
  State* state = g_state.load(std::memory_order_acquire);
  const int index = IndexOf(signal);
  if (state != nullptr && index >= 0) {
    const Record record{signal, 0, NowNs()};
    ssize_t written;
    do {
      written = write(state->pipe_write, &record, sizeof(record));
    } while (written < 0 && errno == EINTR);
    // On to whatever was there before (the SDK's forwarder to its own signal thread, for SIGINT and SIGTERM). SIG_DFL and SIG_IGN are not
    // called: the first would end the process before the quit began, the second means nobody wanted it.
    const struct sigaction& previous = state->previous[index];
    if (previous.sa_flags & SA_SIGINFO) {
      if (previous.sa_sigaction != nullptr) previous.sa_sigaction(signal, info, context);
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
      previous.sa_handler(signal);
    }
  }
  errno = saved_errno;
}

void RunHelper(State* state) {
  pthread_setname_np(pthread_self(), "lar-signals");  // 11 characters: Linux keeps 15
  // The signals are for the other threads: this one only reads the pipe and must never be stopped inside the callbacks by a handler.
  sigset_t set;
  sigemptyset(&set);
  for (const int signal : kSignals) sigaddset(&set, signal);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  for (;;) {
    Record record{};
    const ssize_t got = read(state->pipe_read, &record, sizeof(record));
    if (got < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (got != static_cast<ssize_t>(sizeof(record))) return;  // 0: the write end is closed
    if (record.signal == 0) return;
    const ShutdownSignalStep step = state->policy.OnSignal(std::chrono::nanoseconds(record.at_ns));
    const std::function<void(int)>* action = nullptr;
    if (step == ShutdownSignalStep::kQuit) {
      action = &state->actions.quit;
    } else if (step == ShutdownSignalStep::kForce) {
      action = &state->actions.force;
    }
    if (action != nullptr && *action) {
      try {
        (*action)(record.signal);
      } catch (...) {
        // a callback that throws must not end the thread that a later signal needs
      }
    }
  }
}

}  // namespace

bool InstallShutdownSignals(ShutdownSignalActions actions) {
  if (g_state.load(std::memory_order_acquire) != nullptr) return false;

  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) return false;
  for (const int fd : fds) fcntl(fd, F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);

  auto* state = new State();
  state->pipe_read = fds[0];
  state->pipe_write = fds[1];
  state->actions = std::move(actions);
  // Read the old actions before the new ones go in, so the handler never sees an empty `previous` of a signal it already owns.
  for (int i = 0; i < kSignalCount; ++i) {
    struct sigaction old{};
    sigaction(kSignals[i], nullptr, &old);
    state->previous[i] = old;
    // A terminal that closes must not stop a game that was started to outlive it (nohup): SIGHUP stays ignored.
    state->installed[i] = !(kSignals[i] == SIGHUP && old.sa_handler == SIG_IGN && !(old.sa_flags & SA_SIGINFO));
  }
  try {
    state->thread = std::thread(RunHelper, state);
  } catch (...) {
    close(fds[0]);
    close(fds[1]);
    delete state;
    return false;
  }
  g_state.store(state, std::memory_order_release);
  for (int i = 0; i < kSignalCount; ++i) {
    if (!state->installed[i]) continue;
    struct sigaction action{};
    action.sa_sigaction = OnSignal;
    sigemptyset(&action.sa_mask);
    // SA_RESTART: a system call another thread was in goes on after the handler instead of failing with EINTR. SA_ONSTACK: threads that
    // have a signal stack (the SDK gives them one) run it there.
    action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigaction(kSignals[i], &action, nullptr);
  }
  return true;
}

void RemoveShutdownSignals() {
  State* state = g_state.load(std::memory_order_acquire);
  if (state == nullptr) return;
  for (int i = 0; i < kSignalCount; ++i) {
    if (state->installed[i]) sigaction(kSignals[i], &state->previous[i], nullptr);
  }
  g_state.store(nullptr, std::memory_order_release);
  const Record stop{0, 0, 0};
  ssize_t written;
  do {
    written = write(state->pipe_write, &stop, sizeof(stop));
  } while (written < 0 && errno == EINTR);
  if (state->thread.joinable()) state->thread.join();
  close(state->pipe_read);
  close(state->pipe_write);
  delete state;
}

#else  // not Linux

bool InstallShutdownSignals(ShutdownSignalActions) { return false; }

void RemoveShutdownSignals() {}

#endif

}  // namespace larecomp::os

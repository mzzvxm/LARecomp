// A check that needs no game: the never-destroyed state of settings/immortal.h. Build:
// cmake --build <build dir> --target larecomp_immortal_test, then run it; exit 0 = passed.
//
// The case of 2026-11-21 (run neon1-ab-vk-g2): the exe's settings table was a function-local `static State s`, destroyed by the exe's static
// destruction while a guest thread the SDK left running (TerminateTitle never force-kills a straggler) still called larecomp::settings::Find
// through a hook: an access violation 2 ms after "Process exiting normally", reading a hash bucket through the destroyed table's null
// bucket vector. The order is reproduced here in a child process (the exit has to be real): the child makes a table in miniature (a
// mutex, a name map, a flag its destructor sets BEFORE the map goes, the way ~unordered_map empties the bucket vector), starts a straggler
// thread that looks a name up in a loop, and returns from main. The destructor then keeps the main thread inside the exit for up to two
// seconds, as the real exe's static destruction does (74 ms in that log), and the straggler either meets the destroyed table, which ends
// the process with status 77, or never does and the process ends with 0.
//   plain      a function-local static table: the straggler meets it (the control: the test sees the bug's order happen)
//   immortal   the same table behind Immortal: its destructor never runs, the straggler never meets a destroyed table
// The table says "destroyed" through a flag instead of really reading freed memory, so the control is a clean status, not an access violation.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "settings/immortal.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using larecomp::Immortal;

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

constexpr int kMetDestroyed = 77;  // the straggler met the destroyed table
constexpr int kWatchdog = 99;      // the child ran out of time (a bug in the test, not in the code under test)

// The process ends at once, from any thread: TerminateProcess on Windows (ExitProcess from a second thread would wait for the exit already
// in progress), _exit elsewhere.
[[noreturn]] void EndNow(int status) {
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(status));
  for (;;) Sleep(1000);
#else
  _exit(status);
#endif
}

// larecomp::settings::State in miniature.
struct Table {
  std::mutex mutex;
  std::unordered_map<std::string, int> flat;
  std::atomic<bool> destroyed{false};

  Table() { flat["gpu.driver"] = 1; }
  ~Table() {
    destroyed.store(true, std::memory_order_release);  // ~unordered_map frees the buckets: from here a lookup is a read of freed memory
    // The rest of the exe's static destruction: the main thread is still inside exit() for a while, the straggler keeps running.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // larecomp::settings::Find: lock, look the name up. A lookup that arrives after the destructor began is the crash.
  bool Find(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    if (destroyed.load(std::memory_order_acquire)) EndNow(kMetDestroyed);
    return flat.find(name) != flat.end();
  }
};

Table& PlainTable() {
  static Table t;  // what larecomp_settings.cpp's S() was
  return t;
}

Table& ImmortalTable() {
  static Immortal<Table> t;  // what it is now
  return *t;
}

// The child: make the table, start the straggler, return from main (the exit begins).
int RunChild(bool immortal) {
  Table& table = immortal ? ImmortalTable() : PlainTable();
  std::atomic<bool>* started = new std::atomic<bool>(false);  // read by the straggler after main's frame is gone: not on the stack
  std::thread([&table, started] {
    started->store(true, std::memory_order_release);
    for (;;) table.Find("gpu.driver");
  }).detach();
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    EndNow(kWatchdog);
  }).detach();
  while (!started->load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return 0;  // exit(0): the static destructors run now, with the straggler calling Find
}

// The parent: run this executable again as the child and take its exit status.
int Spawn(const char* self, const char* mode) {
#if defined(_WIN32)
  return static_cast<int>(_spawnlp(_P_WAIT, self, self, mode, nullptr));
#else
  const pid_t pid = fork();
  if (pid == 0) {
    execl(self, self, mode, static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Immortal on its own: made once, same object every time, the destructor of what it holds never runs (a local one goes out of scope).
int g_made = 0;
int g_destroyed = 0;
struct Probe {
  int value;
  explicit Probe(int v = 5) : value(v) { ++g_made; }
  ~Probe() { ++g_destroyed; }
};

Probe& SiteA() {
  static Immortal<Probe> p(11);
  return *p;
}

void CheckImmortalAlone() {
  {
    Immortal<Probe> local(3);
    CHECK(g_made == 1);
    CHECK(local->value == 3);
    CHECK((*local).value == 3);
  }
  CHECK(g_destroyed == 0);  // out of scope, and the Probe is still there
  Probe& a = SiteA();
  Probe& b = SiteA();
  CHECK(&a == &b);
  CHECK(a.value == 11);
  CHECK(g_made == 2);  // one per site, on first use only
  static_assert(std::is_trivially_destructible_v<Immortal<Probe>>, "nothing runs at exit for the holder either");
  static_assert(!std::is_trivially_destructible_v<Probe>);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "plain") == 0) return RunChild(false);
  if (argc >= 2 && std::strcmp(argv[1], "immortal") == 0) return RunChild(true);

  CheckImmortalAlone();
  const int plain = Spawn(argv[0], "plain");
  const int immortal = Spawn(argv[0], "immortal");
  std::printf("child statuses: plain %d, immortal %d\n", plain, immortal);
  CHECK(plain == kMetDestroyed);  // the control: with a plain static the order of the crash happens (if not, the test proves nothing)
  CHECK(immortal == 0);           // the fix: the straggler never meets a destroyed table, the process ends normally

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

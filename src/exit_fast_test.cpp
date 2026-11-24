// A check that needs no game: the fast exit of exit_fast.h. Build: cmake --build <build dir> --target larecomp_exit_fast_test, then run it;
// exit 0 = passed.
//
// What it proves, each in a child process (the exit has to be real):
//   * THE RACE (EXIT-HARD, the class of the 2026-11-21 crash): a function-local static whose destructor marks its table destroyed, and a
//     straggler thread that keeps using the table, as a guest thread does while the C runtime's exit runs the exe's static destructors.
//       legacy   mcla_fast_exit off: Finish returns, main returns, the destructor runs, the straggler meets the destroyed table (status 77: the
//                control, the test sees the bug's order happen; if it does not, the test proves nothing)
//       fast     mcla_fast_exit on: Finish ends the process, no destructor runs, the straggler never meets anything (status 0)
//   * THE CODE: the exit status is the code given (42), with the process ended by Finish.
//   * THE ORDER: the flush hooks run in the order they were added, a hook that throws does not stop the next one, then the note hook (the
//     "Process exiting normally" line) runs, then the process ends.
//   * THE OFF SWITCH STILL FLUSHES (EXIT-REVIEW): with mcla_fast_exit off Finish returns the code and main returns, but the flush hooks have run:
//     the exe's statics are immortal in both modes, so a writer that finished its file in its destructor needs its hook there too.
//   * A WRITE QUEUED JUST BEFORE THE EXIT LANDS: a writer thread that writes what it is given every 3 s (a log's flush interval) is given five
//     lines and the process exits at once. With its flush hook registered all five are in the file; without the hook none is (the control: the
//     queue is lost when the process ends without running the destructors, which is what the hooks are for).
//   * STDIO: a FILE* written with fprintf and never flushed is complete after Finish (fflush(nullptr)); after EndProcessNow alone it is not.
//   * THE WATCHDOG (EXIT-SMALL M5): EndAfterHang, what the 5 s close watchdog calls when the shutdown hung. The hooks that have not run do run (on a helper
//     thread; the hook the hung thread is inside is neither run again nor waited for), the "before" and "after" lines come in order, the note hook does not
//     run, and the status is kWatchdogExitCode (not 0). When the helper itself is stuck (a hook that never returns) the process still ends within the bound.
//   * THE TABLE: AddFlush keeps a function once, refuses a null hook and the 33rd one, RunFlushes runs each hook once.
// The straggler's table says "destroyed" through a flag instead of reading freed memory, so the control is a clean status, not an access violation.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exit_fast.h"

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

// ---- the straggler's table (larecomp::settings::State in miniature, as immortal_test.cpp has it)
struct Table {
  std::mutex mutex;
  std::unordered_map<std::string, int> flat;
  std::atomic<bool> destroyed{false};

  Table() { flat["gpu.driver"] = 1; }
  ~Table() {
    destroyed.store(true, std::memory_order_release);
    // The rest of the exe's static destruction: the main thread stays inside exit() for a while, the straggler keeps running.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  bool Find(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    if (destroyed.load(std::memory_order_acquire)) larecomp::EndProcessNow(kMetDestroyed);
    return flat.find(name) != flat.end();
  }
};

Table& PlainTable() {
  static Table t;  // an ordinary function-local static: destroyed by the C runtime's exit
  return t;
}

void StartWatchdog() {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    larecomp::EndProcessNow(kWatchdog);
  }).detach();
}

// ---- the writer of the queued-write checks: a thread that writes the queue every 3 s, a destructor that would write the rest
struct QueuedWriter {
  std::mutex mutex;
  std::vector<std::string> queue;
  std::string path;

  void Push(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push_back(line);
  }
  void Drain() {
    std::vector<std::string> take;
    {
      std::lock_guard<std::mutex> lock(mutex);
      take.swap(queue);
    }
    if (take.empty()) return;
    std::ofstream out(path, std::ios::app | std::ios::binary);
    for (const std::string& line : take) out << line << "\n";
    out.flush();
  }
};

QueuedWriter* g_writer = nullptr;  // never freed: the hook is a plain function pointer
void DrainHook() { if (g_writer) g_writer->Drain(); }

// ---- the order check's hooks write into a file named by g_order_path
std::string g_order_path;
void AppendOrder(const char* word) {
  std::ofstream out(g_order_path, std::ios::app | std::ios::binary);
  out << word << "\n";
}
void OrderFlush1() { AppendOrder("flush1"); }
void OrderThrows() { AppendOrder("throws"); throw std::runtime_error("a hook that fails"); }
void OrderFlush3() { AppendOrder("flush3"); }
void OrderNote() { AppendOrder("note"); }
void OrderHangs() {  // a hook that never returns (a full disk, a scanner, a lock a stuck thread holds)
  AppendOrder("hang-start");
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
void OrderBefore() { AppendOrder("before"); }
void OrderAfter(const larecomp::exitfast::Report& report) { AppendOrder(report.ran == 1 ? "after ran=1" : "after ran=other"); }

// ---- the children
int RunRace(bool fast, const char* flag_path) {
  Table& table = PlainTable();
  std::atomic<bool>* started = new std::atomic<bool>(false);  // read by the straggler after main's frame is gone: not on the stack
  std::thread([&table, started] {
    started->store(true, std::memory_order_release);
    for (;;) table.Find("gpu.driver");
  }).detach();
  StartWatchdog();
  while (!started->load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  larecomp::exitfast::Arm(fast);
  (void)flag_path;
  return larecomp::exitfast::Finish(0);  // fast: the process ends here. legacy: 0 comes back, main returns, the destructors run
}

int RunCode() {
  StartWatchdog();
  larecomp::exitfast::Arm(true);
  return larecomp::exitfast::Finish(42);
}

int RunOrder(const char* path) {
  g_order_path = path;
  StartWatchdog();
  larecomp::exitfast::AddFlush("one", &OrderFlush1);
  larecomp::exitfast::AddFlush("throws", &OrderThrows);
  larecomp::exitfast::AddFlush("three", &OrderFlush3);
  larecomp::exitfast::SetNote(&OrderNote);
  larecomp::exitfast::Arm(true);
  return larecomp::exitfast::Finish(0);
}

// The shutdown hangs inside the second hook (on the exiting thread); the watchdog (this thread, 300 ms later) ends the process.
int RunHangOtherThread(const char* path) {
  g_order_path = path;
  StartWatchdog();
  larecomp::exitfast::AddFlush("one", &OrderFlush1);
  larecomp::exitfast::AddFlush("hangs", &OrderHangs);
  larecomp::exitfast::AddFlush("three", &OrderFlush3);
  larecomp::exitfast::SetNote(&OrderNote);
  larecomp::exitfast::Arm(true);
  std::thread([] { larecomp::exitfast::EndNow(0); }).detach();  // flush1, then stuck in the hook that hangs
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  larecomp::exitfast::EndAfterHang(larecomp::exitfast::kWatchdogExitCode, 2000, &OrderBefore, &OrderAfter);
}

// No other thread: the helper itself runs into the hook that never returns, and the watcher gives up after its bound.
int RunHangInHelper(const char* path) {
  g_order_path = path;
  StartWatchdog();
  larecomp::exitfast::AddFlush("hangs", &OrderHangs);
  larecomp::exitfast::AddFlush("three", &OrderFlush3);
  larecomp::exitfast::SetNote(&OrderNote);
  larecomp::exitfast::EndAfterHang(larecomp::exitfast::kWatchdogExitCode, 300, &OrderBefore, &OrderAfter);
}

int RunLegacyFlush(const char* path) {
  g_order_path = path;
  StartWatchdog();
  larecomp::exitfast::AddFlush("one", &OrderFlush1);
  larecomp::exitfast::SetNote(&OrderNote);  // the note is the app's atexit handler on this path: Finish does not run it
  larecomp::exitfast::Arm(false);
  return larecomp::exitfast::Finish(5);  // off: the code comes back, after the hook
}

// The off switch with the app's own note call (LarecompApp::FinishProcess): the note runs once, before the code comes back, however often it is asked.
int RunLegacyNote(const char* path) {
  g_order_path = path;
  StartWatchdog();
  larecomp::exitfast::AddFlush("one", &OrderFlush1);
  larecomp::exitfast::SetNote(&OrderNote);
  larecomp::exitfast::Arm(false);
  const int code = larecomp::exitfast::Finish(6);
  larecomp::exitfast::RunNote();
  larecomp::exitfast::RunNote();
  return code;
}

int RunQueued(bool with_hook, const char* path) {
  StartWatchdog();
  g_writer = new QueuedWriter();
  g_writer->path = path;
  std::thread([] {  // the writer's own thread: it would write the queue 3 s from now
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      g_writer->Drain();
    }
  }).detach();
  for (int i = 1; i <= 5; ++i) g_writer->Push("line " + std::to_string(i));
  if (with_hook) larecomp::exitfast::AddFlush("queued writer", &DrainHook);
  larecomp::exitfast::Arm(true);
  return larecomp::exitfast::Finish(0);  // right now: 5 lines queued, nothing written yet
}

int RunStdio(bool through_finish, const char* path) {
  StartWatchdog();
  std::FILE* file = std::fopen(path, "wb");
  if (!file) return 3;
  std::fprintf(file, "the last line of a stdio stream\n");  // buffered: nothing of it is in the file yet
  larecomp::exitfast::Arm(true);
  if (through_finish) return larecomp::exitfast::Finish(0);
  larecomp::EndProcessNow(0);  // what the exit paths did before: no flush
}

// ---- the parent
int Spawn(const char* self, const char* mode, const std::string& path) {
  const std::string quoted = "\"" + path + "\"";
#if defined(_WIN32)
  return static_cast<int>(_spawnlp(_P_WAIT, self, self, mode, quoted.c_str(), nullptr));
#else
  const pid_t pid = fork();
  if (pid == 0) {
    execl(self, self, mode, path.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string Strip(std::string text) {
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

template <int N>
void Filler() {}  // a distinct function for each N

template <int... Is>
int FillTable(std::integer_sequence<int, Is...>) {
  int accepted = 0;
  ((accepted += larecomp::exitfast::AddFlush("filler", &Filler<Is>) ? 1 : 0), ...);
  return accepted;
}

void CheckTable() {
  using namespace larecomp::exitfast;
  static int ran_a = 0, ran_b = 0, ran_c = 0;
  static std::string order;
  auto a = [] { ++ran_a; order += "a"; };
  auto b = [] { ++ran_b; order += "b"; throw 1; };
  auto c = [] { ++ran_c; order += "c"; };
  CHECK(!AddFlush("null", nullptr));
  CHECK(AddFlush("a", +a));
  CHECK(AddFlush("a again", +a));  // the same function: kept once, reported as added
  CHECK(AddFlush("b", +b));
  CHECK(AddFlush("c", +c));
  Report r = RunFlushes();
  CHECK(r.ran == 2);    // a and c
  CHECK(r.threw == 1);  // b
  CHECK(order == "abc");  // in the order added; the throwing one did not stop c
  CHECK(ran_a == 1 && ran_b == 1 && ran_c == 1);
  Report again = RunFlushes();
  CHECK(again.ran == 0 && again.threw == 0);  // once each
  CHECK(ran_a == 1 && ran_b == 1 && ran_c == 1);
  CHECK(r.ms >= 0);
  // the table holds 32: three are in, 29 more fit, the next ones are refused
  CHECK(FillTable(std::make_integer_sequence<int, 40>{}) == 29);
  CHECK(Armed());         // the default is on
  Arm(false);
  CHECK(!Armed());
  CHECK(Finish(7) == 7);  // off: the code comes back and the process goes on
  Arm(true);
  CHECK(Armed());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "legacy") == 0) return RunRace(false, argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "fast") == 0) return RunRace(true, argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "code") == 0) return RunCode();
  if (argc >= 3 && std::strcmp(argv[1], "order") == 0) return RunOrder(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "legacy-flush") == 0) return RunLegacyFlush(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "legacy-note") == 0) return RunLegacyNote(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "hang-other") == 0) return RunHangOtherThread(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "hang-helper") == 0) return RunHangInHelper(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "queued-hook") == 0) return RunQueued(true, argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "queued-nohook") == 0) return RunQueued(false, argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "stdio-finish") == 0) return RunStdio(true, argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "stdio-raw") == 0) return RunStdio(false, argv[2]);

  CheckTable();

  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / ("larecomp_exit_fast_test_" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
  std::filesystem::create_directories(dir, ec);
  auto file = [&](const char* name) {
    const std::string p = (dir / name).string();
    std::filesystem::remove(p, ec);
    return p;
  };

  // the race
  const int legacy = Spawn(argv[0], "legacy", file("legacy.txt"));
  const int fast = Spawn(argv[0], "fast", file("fast.txt"));
  std::printf("race: legacy %d, fast %d\n", legacy, fast);
  CHECK(legacy == kMetDestroyed);  // the control: the straggler meets the destroyed table after a return from main
  CHECK(fast == 0);                // the fix: the process ends before any destructor can run

  // the code
  const int code = Spawn(argv[0], "code", file("code.txt"));
  std::printf("exit code: %d\n", code);
  CHECK(code == 42);

  // the order
  const std::string order_path = file("order.txt");
  const int order_status = Spawn(argv[0], "order", order_path);
  CHECK(order_status == 0);
  CHECK(Strip(ReadAll(order_path)) == "flush1\nthrows\nflush3\nnote\n");

  // the off switch: the hook runs, the note does not (the atexit handler of the app is the note there), the code comes back
  const std::string legacy_flush_path = file("legacy-flush.txt");
  const int legacy_flush_status = Spawn(argv[0], "legacy-flush", legacy_flush_path);
  std::printf("off switch: status %d, text '%s'\n", legacy_flush_status, Strip(ReadAll(legacy_flush_path)).c_str());
  CHECK(legacy_flush_status == 5);
  CHECK(Strip(ReadAll(legacy_flush_path)) == "flush1\n");

  // the off switch with RunNote (the app's path since 11-24): the note once, after the hook, and the code comes back
  const std::string legacy_note_path = file("legacy-note.txt");
  const int legacy_note_status = Spawn(argv[0], "legacy-note", legacy_note_path);
  std::printf("off switch with the note: status %d, text '%s'\n", legacy_note_status, Strip(ReadAll(legacy_note_path)).c_str());
  CHECK(legacy_note_status == 6);
  CHECK(Strip(ReadAll(legacy_note_path)) == "flush1\nnote\n");

  // the watchdog: the hook the hung thread is inside is skipped, the ones behind it run, the status is the watchdog's own
  CHECK(larecomp::exitfast::kWatchdogExitCode != 0 && larecomp::exitfast::kWatchdogExitCode != 3 && larecomp::exitfast::kWatchdogExitCode != kMetDestroyed &&
        larecomp::exitfast::kWatchdogExitCode != kWatchdog);
  const std::string hang_other_path = file("hang-other.txt");
  const auto hang_t0 = std::chrono::steady_clock::now();
  const int hang_other_status = Spawn(argv[0], "hang-other", hang_other_path);
  const auto hang_other_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - hang_t0).count();
  std::printf("watchdog (a hung hook on another thread): status %d in %lld ms, text '%s'\n", hang_other_status, static_cast<long long>(hang_other_ms),
              Strip(ReadAll(hang_other_path)).c_str());
  CHECK(hang_other_status == larecomp::exitfast::kWatchdogExitCode);
  // flush1 ran on the exiting thread; hang-start is the hook it is stuck in; the helper then runs before, the hook behind the hung one (flush3) and after,
  // in that order; no "flush1" a second time, no second "hang-start", and no "note" (the note says "exiting normally")
  CHECK(Strip(ReadAll(hang_other_path)) == "flush1\nhang-start\nbefore\nflush3\nafter ran=1\n");
  CHECK(hang_other_ms < 6000);  // the process did not wait for the hung hook
  const std::string hang_helper_path = file("hang-helper.txt");
  const auto hang_t1 = std::chrono::steady_clock::now();
  const int hang_helper_status = Spawn(argv[0], "hang-helper", hang_helper_path);
  const auto hang_helper_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - hang_t1).count();
  std::printf("watchdog (the helper stuck in a hook): status %d in %lld ms, text '%s'\n", hang_helper_status, static_cast<long long>(hang_helper_ms),
              Strip(ReadAll(hang_helper_path)).c_str());
  CHECK(hang_helper_status == larecomp::exitfast::kWatchdogExitCode);  // the bound held: not the test's own 10 s watchdog (status 99)
  CHECK(Strip(ReadAll(hang_helper_path)) == "before\nhang-start\n");  // the hooks behind the stuck one and the "after" line never ran
  CHECK(hang_helper_ms < 6000);

  // a write queued just before the exit
  const std::string hook_path = file("queued-hook.txt");
  const std::string nohook_path = file("queued-nohook.txt");
  const int hook_status = Spawn(argv[0], "queued-hook", hook_path);
  const int nohook_status = Spawn(argv[0], "queued-nohook", nohook_path);
  const std::string hook_text = Strip(ReadAll(hook_path));
  const std::string nohook_text = Strip(ReadAll(nohook_path));
  std::printf("queued write: with the hook status %d, %zu bytes; without status %d, %zu bytes\n", hook_status, hook_text.size(), nohook_status, nohook_text.size());
  CHECK(hook_status == 0);
  CHECK(hook_text == "line 1\nline 2\nline 3\nline 4\nline 5\n");  // landed, complete and in order
  CHECK(nohook_status == 0);
  CHECK(nohook_text.empty());  // the control: with no hook the queue dies with the process

  // stdio
  const std::string finish_path = file("stdio-finish.txt");
  const std::string raw_path = file("stdio-raw.txt");
  const int finish_status = Spawn(argv[0], "stdio-finish", finish_path);
  const int raw_status = Spawn(argv[0], "stdio-raw", raw_path);
  std::printf("stdio: through Finish status %d, %zu bytes; raw status %d, %zu bytes\n", finish_status, ReadAll(finish_path).size(), raw_status, ReadAll(raw_path).size());
  CHECK(finish_status == 0);
  CHECK(Strip(ReadAll(finish_path)) == "the last line of a stdio stream\n");
  CHECK(raw_status == 0);
  CHECK(ReadAll(raw_path).empty());  // the control: EndProcessNow alone loses what the stream had buffered

  std::filesystem::remove_all(dir, ec);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// A small program that crashes on purpose, to check the Linux crash reporter (crash_report_linux.h)
// without the game, the SDK or a GPU. Linux only; it needs nothing but the compiler:
//
//   clang++ -std=c++20 -O1 -g -I src src/crash_report_demo.cpp -o crash_report_demo -pthread
//   ./crash_report_demo MODE [log file]        (ports/scripts/linux/crash-test.sh runs every mode)
//
// The "log" is a buffered stdio file, like the game's log sink: the line written just before the
// crash must be in the file BEFORE the crash report (the report asks the helper thread to flush it).
//
// MODE:
//   segv            write to address 0x10 on the main thread (has the signal stack)
//   segv-thread     the same on a worker thread (no signal stack of its own)
//   stack           endless recursion on a worker thread that has a signal stack
//   abort           abort()
//   terminate       an uncaught exception: std::terminate, then abort
//   illegal         ud2
//   fpe             integer division by zero
//   kill-segv       kill(getpid(), SIGSEGV): a signal that is not a CPU fault
//   chain-consumed  a stand-in for the runtime's handler consumes a write-watch fault; nothing may be
//                   reported and the program ends with exit status 0
//   chain-declined  the same, then a fault it declines: reported once, killed by SIGSEGV
//   chain-twice     two threads fault at once: one report, no mixed lines
//
// Expected, for every mode but chain-consumed: a report in the log and on stderr that starts with
// "!!! CRASH DETECTED !!!" (or "!!! ABORT DETECTED !!!" for abort and terminate), the exit status of
// the signal (128 + 11, 6, 4 or 8 in a shell) and, with `ulimit -c unlimited`, a core file.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <exception>
#include <stdexcept>

#include "crash_report_linux.h"

namespace lx = larecomp::crash::lx;

namespace {

FILE* g_log = nullptr;

void FlushDemoLog() {
  if (g_log) fflush(g_log);
}

void Say(const char* text) {
  fprintf(stdout, "%s\n", text);
  fflush(stdout);
}

// ---- the faults ------------------------------------------------------------------------------------

[[gnu::noinline]] void WriteNull() {
  volatile int* p = reinterpret_cast<volatile int*>(0x10);
  *p = 1;
}

// The recursion has no end on purpose.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Winfinite-recursion"
#endif
[[gnu::noinline]] int Recurse(int depth) {
  volatile char pad[2048];
  pad[0] = static_cast<char>(depth);
  return Recurse(depth + 1) + pad[0];
}

[[gnu::noinline]] void Divide() {
  volatile int zero = 0;
  volatile int r = 7 / zero;
  (void)r;
}

alignas(16) char g_thread_stack[128 * 1024];

void* NullThread(void*) {
  WriteNull();
  return nullptr;
}

void* StackThread(void*) {
  lx::EnableAltStack(g_thread_stack, sizeof(g_thread_stack));
  Recurse(0);
  return nullptr;
}

void* ThrowThread(void*) {
  throw std::runtime_error("demo: uncaught exception on a worker thread");
}

// ---- a stand-in for the runtime's handler ------------------------------------------------------------

char* g_watched = nullptr;
long g_page = 0;
int g_consumed = 0;

// What the runtime's ExceptionHandlerCallback does with write watches: lift the protection and
// return so that the instruction runs again. A fault it does not know it declines, which is where
// crash_handler.cpp's rex::arch::ExceptionHandler callback runs and calls ReportUnconsumed.
void FakeRuntimeHandler(int sig, siginfo_t* info, void*) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(info->si_addr);
  const uintptr_t w = reinterpret_cast<uintptr_t>(g_watched);
  if (a >= w && a < w + static_cast<uintptr_t>(g_page)) {
    mprotect(g_watched, static_cast<size_t>(g_page), PROT_READ | PROT_WRITE);
    ++g_consumed;
    return;
  }
  lx::Fault f;
  f.signo = sig;
  f.has_addr = true;
  f.addr = a;
  f.op = 2;
  lx::ReportUnconsumed(f);
  // returns: the instruction faults again and the default action ends the process
}

void InstallFakeRuntime() {
  g_page = sysconf(_SC_PAGESIZE);
  g_watched = static_cast<char*>(
      mmap(nullptr, static_cast<size_t>(g_page), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = &FakeRuntimeHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;  // like the runtime's: no SA_ONSTACK, empty mask
  sigaction(SIGSEGV, &sa, nullptr);
}

void* TwinThread(void*) {
  struct timespec ts = {0, 20 * 1000 * 1000};
  nanosleep(&ts, nullptr);
  WriteNull();
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "";
  const char* log = argc > 2 ? argv[2] : "crash_report_demo.log";
  if (!*mode) {
    fprintf(stderr, "usage: %s MODE [log file]   (modes are listed in the file's header)\n", argv[0]);
    return 2;
  }

  g_log = fopen(log, "a");
  if (g_log) fprintf(g_log, "[demo] started, mode %s\n", mode);  // buffered, like a log sink

  char log_abs[1024];
  if (!realpath(log, log_abs)) log_abs[0] = 0;
  struct tm tmv;
  time_t now = time(nullptr);
  localtime_r(&now, &tmv);
  lx::InstallEarly(log_abs, static_cast<int>(tmv.tm_gmtoff), &FlushDemoLog);
  std::set_terminate([] {
    lx::ReportAbortLike("std::terminate", "demo uncaught exception");
    abort();
  });

  if (strcmp(mode, "chain-consumed") == 0 || strcmp(mode, "chain-declined") == 0 ||
      strcmp(mode, "chain-twice") == 0) {
    InstallFakeRuntime();  // the "runtime" installs its handler after InstallEarly, as in the game
  }
  lx::InstallLate();  // and ours goes on top after Runtime::Setup

  if (g_log) fprintf(g_log, "[demo] the last line before the crash (must come first in the file)\n");

  pthread_t th;
  if (strcmp(mode, "segv") == 0) {
    Say("writing to address 0x10 on the main thread");
    WriteNull();
  } else if (strcmp(mode, "segv-thread") == 0) {
    Say("writing to address 0x10 on a worker thread");
    pthread_create(&th, nullptr, &NullThread, nullptr);
    pthread_join(th, nullptr);
  } else if (strcmp(mode, "stack") == 0) {
    Say("recursing until the worker thread's stack is gone (it has a signal stack)");
    pthread_create(&th, nullptr, &StackThread, nullptr);
    pthread_join(th, nullptr);
  } else if (strcmp(mode, "abort") == 0) {
    Say("abort()");
    abort();
  } else if (strcmp(mode, "terminate") == 0) {
    Say("an uncaught exception on a worker thread");
    pthread_create(&th, nullptr, &ThrowThread, nullptr);
    pthread_join(th, nullptr);
  } else if (strcmp(mode, "illegal") == 0) {
    Say("ud2");
    __builtin_trap();
  } else if (strcmp(mode, "fpe") == 0) {
    Say("integer division by zero");
    Divide();
  } else if (strcmp(mode, "kill-segv") == 0) {
    Say("kill(getpid(), SIGSEGV)");
    kill(getpid(), SIGSEGV);
    sleep(5);
    Say("still running after SIGSEGV: the default action was not restored");
    return 3;
  } else if (strcmp(mode, "chain-consumed") == 0 || strcmp(mode, "chain-declined") == 0) {
    Say("touching a protected page the stand-in runtime lifts the protection of");
    volatile char c = g_watched[100];
    (void)c;
    if (g_consumed != 1) {
      Say("FAIL: the fault was not consumed exactly once");
      return 4;
    }
    mprotect(g_watched, static_cast<size_t>(g_page), PROT_NONE);
    g_watched[200] = 1;
    if (g_consumed != 2) {
      Say("FAIL: the second watch fault was not consumed");
      return 4;
    }
    Say("two faults consumed by the runtime stand-in, none reported");
    if (strcmp(mode, "chain-declined") == 0) {
      Say("now a fault it declines");
      WriteNull();
    }
    if (g_log) fflush(g_log);
    return 0;
  } else if (strcmp(mode, "chain-twice") == 0) {
    Say("two threads write to address 0x10 about together");
    pthread_t a, b;
    pthread_create(&a, nullptr, &TwinThread, nullptr);
    pthread_create(&b, nullptr, &TwinThread, nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
  } else {
    fprintf(stderr, "unknown mode '%s'\n", mode);
    return 2;
  }
  Say("FAIL: the program did not crash");
  return 3;
}

#pragma once
// The Linux crash reporter's signal machinery (used by crash_handler.cpp; the standalone check
// src/crash_report_demo.cpp includes it too). Linux only.
//
// What it does, in the order a crash goes through it:
//
//   * InstallEarly(): the first thing the game does (the app constructor). A signal stack for the
//     calling thread, sigaction for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT, a pipe for safe
//     memory reads, a helper thread that can flush the log sink, and one backtrace() call so that
//     libgcc is loaded before a crash needs it.
//   * InstallLate(): after Runtime::Setup. The runtime installs its own SIGSEGV and SIGILL
//     handlers (write watches and MMIO need them) and, through the SDK's seh_initialize, SIGBUS and
//     SIGFPE; those replace the early ones. InstallLate puts ours back on top and remembers the
//     runtime's action as `prev`:
//       - SIGSEGV and SIGILL: FaultWrapper runs on the signal stack, calls the runtime's handler
//         and returns. Nothing is reported here: a fault the runtime handles must stay invisible.
//         crash_handler.cpp also registers a rex::platform::ExceptionHandler AFTER the runtime's own
//         (the runtime calls them in the order installed and stops at the first that says
//         "handled"), so it is reached only when every one of them declined, and then calls
//         ReportUnconsumed(). On rexglue 0.11 the runtime then forwards the fault to the action
//         under it (FatalSignalHandler here) or, with none, reports, restores SIG_DFL and
//         re-raises (ForwardSignal); 0.10's handler did neither, so the instruction ran again and
//         faulted again forever. The report is written once either way (State::reported).
//       - SIGBUS, SIGFPE, SIGABRT (and any signal before InstallLate): FatalSignalHandler reports.
//   * The report (Report) is async-signal-safe: no malloc, no locks, no stdio, no iostreams. It
//     asks the helper thread to flush the log sink and waits for it at most 300 ms (the crashing
//     thread may hold the sink's lock; a report that hangs anyway ends the process after 15 s with
//     SIGALRM), then writes the lines with write(2) to the log file (opened
//     again by path, O_APPEND, the same file the sink appends to) and to stderr, and fsync()s.
//   * Last, the default action is restored and the signal raised again, so the exit status is the
//     normal "killed by SIGSEGV" and a core dump is still written.
//
// The lines match the Windows handler's (crash_handler.cpp): the same headlines and the same
// Portuguese words, in the log's own line format, so the log tools and triage scripts find them.
//
// CHECK: written on Windows without a Linux compiler or glibc headers: see the report of agent
// LX-CRASH for what was and was not checked.

#if !defined(__linux__)
#error "crash_report_linux.h is for Linux only"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "crash_report_util.h"

namespace larecomp::crash::lx {

constexpr size_t kAltStackSize = 256 * 1024;
constexpr int kMaxFrames = 48;     // as many as the Windows handler prints
constexpr int kFrameSlots = kMaxFrames + 24;  // room for the reporter's own frames at the top
constexpr int kMaxSignal = 65;

// What a report is about.
struct Fault {
  int signo = 0;
  bool has_si_code = false;
  int si_code = 0;
  bool has_addr = false;
  uint64_t addr = 0;       // the address the access went to
  int op = 0;              // 0 unknown, 1 read, 2 write, 3 instruction fetch
  bool has_pc = false;
  uint64_t pc = 0;         // the instruction that faulted
  bool has_regs = false;
  uint64_t gpr[16] = {};   // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
  uint64_t rflags = 0;
  bool has_err = false;
  uint64_t err = 0;        // the CPU's page-fault error code
  bool is_abort = false;   // "ABORT DETECTED" (abort(), std::terminate) instead of "CRASH DETECTED"
  const char* reason = nullptr;
  const char* detail = nullptr;
};

struct State {
  char log_path[1024];
  char exe_path[512];
  int utc_offset;
  void (*flush_logs)();
  bool (*skip_signal)(int sig);  // true: the runtime owns this fault right now, never report it
  int probe_pipe[2];
  sem_t flush_req;
  bool flush_thread_ok;
  volatile int flush_done;
  volatile int reporting;
  volatile int reported;
  struct sigaction prev[kMaxSignal];
  alignas(16) char altstack[kAltStackSize];
  // Working memory of a report (a report can run on the signal stack: keep it off the stack).
  void* frames[kFrameSlots];
  uint64_t addrs[kMaxFrames + 2];
  uint64_t lookup[kMaxFrames + 2];
  Region regions[kMaxFrames + 2];
  SymHit hits[kMaxFrames + 2];
  SymHit tmp_hits[kMaxFrames + 2];
  uint64_t modoff[kMaxFrames + 2];
  bool have_modoff[kMaxFrames + 2];
  ElfInfo elf;
  char maps_chunk[8192];
  unsigned char elf_scratch[65536];
  char file_prefix[128];
  char err_prefix[64];
  char body[1024];
  char line[1400];
};

inline State g_state;  // constant-initialised: no constructor runs before main
inline State& S() { return g_state; }

// What FaultWrapper knows about the signal it is running, for the ExceptionHandler callback that
// runs inside it on the same thread.
struct Stash {
  int sig;
  int depth;
  void* uctx;
  siginfo_t info;
};
inline thread_local Stash t_stash;

// ---- small wrappers ------------------------------------------------------------------------------

inline int Gettid() { return static_cast<int>(syscall(SYS_gettid)); }

inline void WriteAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
}

inline long ReadFd(void* ctx, void* buf, size_t n) {
  const int fd = *static_cast<int*>(ctx);
  for (;;) {
    ssize_t r = read(fd, buf, n);
    if (r < 0 && errno == EINTR) continue;
    return static_cast<long>(r);
  }
}

inline bool PreadAll(void* ctx, uint64_t off, void* buf, size_t n) {
  const int fd = *static_cast<int*>(ctx);
  char* p = static_cast<char*>(buf);
  while (n > 0) {
    ssize_t r = pread(fd, p, n, static_cast<off_t>(off));
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
    off += static_cast<uint64_t>(r);
  }
  return true;
}

// Copies up to n (<= 64) bytes of the process's memory at p without risking a fault: write(2) to a
// pipe answers EFAULT for an unreadable buffer where a plain read would signal.
inline size_t ProbeRead(const void* p, void* out, size_t n) {
  State& s = S();
  if (s.probe_pipe[1] < 0 || n == 0 || n > 64) return 0;
  ssize_t w = write(s.probe_pipe[1], p, n);
  if (w <= 0) return 0;
  ssize_t r = read(s.probe_pipe[0], out, static_cast<size_t>(w));
  return r > 0 ? static_cast<size_t>(r) : 0;
}

inline void SleepMs(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

inline void DefaultAndRaise(int sig) {
  struct sigaction dfl;
  __builtin_memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
  raise(sig);  // pending while the handler runs, delivered with the default action when it returns
}

// ---- names ---------------------------------------------------------------------------------------

inline const char* SignalName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
  }
}

inline const char* SiCodeName(int sig, int code) {
  if (code <= 0) {
    switch (code) {
      case 0: return "SI_USER: sent by kill(), not a CPU fault";
      case -6: return "SI_TKILL: sent by raise(), abort() or tgkill()";
      case -1: return "SI_QUEUE: sent by sigqueue()";
      default: return "sent by a signal call, not a CPU fault";
    }
  }
  switch (sig) {
    case SIGSEGV:
      if (code == 1) return "SEGV_MAPERR: address not mapped";
      if (code == 2) return "SEGV_ACCERR: mapped, access not allowed";
      break;
    case SIGBUS:
      if (code == 1) return "BUS_ADRALN: misaligned address";
      if (code == 2) return "BUS_ADRERR: no such physical address";
      if (code == 3) return "BUS_OBJERR: object-specific hardware error";
      break;
    case SIGILL:
      if (code == 1) return "ILL_ILLOPC: illegal opcode";
      if (code == 2) return "ILL_ILLOPN: illegal operand";
      break;
    case SIGFPE:
      if (code == 1) return "FPE_INTDIV: integer divide by zero";
      if (code == 2) return "FPE_INTOVF: integer overflow";
      if (code == 3) return "FPE_FLTDIV: float divide by zero";
      break;
    default:
      break;
  }
  return "";
}

// ---- the report ----------------------------------------------------------------------------------

inline void EmitBody(int fd_log, int fd_err, const Text& body) {
  State& s = S();
  if (fd_log >= 0) {
    Text l(s.line, sizeof(s.line) - 1);
    l.Str(s.file_prefix);
    l.Chars(body.data(), body.size());
    l.Char('\n');
    WriteAll(fd_log, l.data(), l.size());
  }
  if (fd_err >= 0) {
    Text l(s.line, sizeof(s.line) - 1);
    l.Str(s.err_prefix);
    l.Chars(body.data(), body.size());
    l.Char('\n');
    WriteAll(fd_err, l.data(), l.size());
  }
}

// "  <label> 0x<addr>  module+0x<off>  symbol+0x<off>" as the Windows handler's LogAddr prints it.
inline void FormatAddr(Text& b, const char* label, uint64_t addr, const Region& reg,
                       bool have_off, uint64_t modoff, const SymHit& hit) {
  b.Str("  ");
  b.Str(label);
  b.Str(" 0x");
  b.Hex(addr, 16);
  b.Str("  ");
  const char* mod = "?";
  if (reg.found && reg.path[0] != 0) mod = BaseName(reg.path);
  b.Str(mod);
  b.Str("+0x");
  b.Hex(have_off ? modoff : addr, 1);
  if (hit.found) {
    b.Str("  ");
    b.Str(hit.name);
    b.Str("+0x");
    b.Hex(have_off && modoff >= hit.value ? modoff - hit.value : 0, 1);
  } else {
    b.Str("  <no symbol>");
  }
}

inline void LogFlushRequest() {
  State& s = S();
  if (!s.flush_thread_ok || !s.flush_logs) return;
  __atomic_store_n(&s.flush_done, 0, __ATOMIC_SEQ_CST);
  sem_post(&s.flush_req);
  for (int i = 0; i < 60; ++i) {  // 300 ms at most: the crashing thread may hold the sink's lock
    if (__atomic_load_n(&s.flush_done, __ATOMIC_ACQUIRE)) return;
    SleepMs(5);
  }
}

// Resolves every address in s.addrs[0..n) (s.lookup is what is looked up: a return address minus
// one) to its mapping, its offset in the module and, where the file has a symbol table, a name.
inline void Symbolise(size_t n) {
  State& s = S();
  for (size_t i = 0; i < n; ++i) {
    s.hits[i] = SymHit{};
    s.have_modoff[i] = false;
    s.modoff[i] = 0;
  }
  int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (maps < 0) {
    for (size_t i = 0; i < n; ++i) s.regions[i] = Region{};
    return;
  }
  FindRegions(&ReadFd, &maps, s.lookup, s.regions, n, s.maps_chunk, sizeof(s.maps_chunk));
  close(maps);

  bool done[kMaxFrames + 2] = {};
  for (size_t i = 0; i < n; ++i) {
    if (done[i] || !s.regions[i].found || s.regions[i].path[0] != '/') continue;
    // Every address of the same file in one lookup (one pass over its symbol table).
    int fd = open(s.regions[i].path, O_RDONLY | O_CLOEXEC);
    size_t idx[kMaxFrames + 2];
    uint64_t vaddr[kMaxFrames + 2];
    size_t count = 0;
    if (fd >= 0) {
      ElfIo io;
      io.read = &PreadAll;
      io.ctx = &fd;
      if (ElfOpen(io, &s.elf, s.elf_scratch, sizeof(s.elf_scratch))) {
        for (size_t j = i; j < n; ++j) {
          if (done[j] || !s.regions[j].found || !CStrEq(s.regions[j].path, s.regions[i].path)) {
            continue;
          }
          done[j] = true;
          const uint64_t file_off = s.lookup[j] - s.regions[j].start + s.regions[j].file_offset;
          uint64_t va;
          if (s.elf.VaddrOfFileOffset(file_off, &va)) {
            // The printed offset is of the frame's own address, not of the lookup address.
            s.modoff[j] = va + (s.addrs[j] - s.lookup[j]);
            s.have_modoff[j] = true;
            idx[count] = j;
            vaddr[count] = va;
            ++count;
          }
        }
        if (count > 0) {
          ElfLookup(io, s.elf, vaddr, s.tmp_hits, count, s.elf_scratch, sizeof(s.elf_scratch));
          for (size_t k = 0; k < count; ++k) s.hits[idx[k]] = s.tmp_hits[k];
        }
      }
      close(fd);
    }
    done[i] = true;
  }
}

inline void WriteRegisters(int fd_log, int fd_err, const Fault& f) {
  State& s = S();
  static const char* const kNames[16] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
                                         "R8 ", "R9 ", "R10", "R11", "R12", "R13", "R14", "R15"};
  Text b(s.body, sizeof(s.body));
  b.Str("Registers:");
  EmitBody(fd_log, fd_err, b);
  for (int row = 0; row < 4; ++row) {
    b.Clear();
    b.Str(" ");
    for (int col = 0; col < 4; ++col) {
      const int i = row * 4 + col;
      b.Str(" ");
      b.Str(kNames[i]);
      b.Str(" 0x");
      b.Hex(f.gpr[i], 16);
    }
    EmitBody(fd_log, fd_err, b);
  }
  b.Clear();
  b.Str("  RIP 0x");
  b.Hex(f.pc, 16);
  b.Str("  EFLAGS 0x");
  b.Hex(f.rflags, 1);
  if (f.has_err) {
    b.Str("  ERR 0x");
    b.Hex(f.err, 1);
  }
  EmitBody(fd_log, fd_err, b);
}

// Writes one report. Returns false when another thread is already writing one.
inline bool Report(const Fault& f) {
  State& s = S();
  if (__atomic_exchange_n(&s.reporting, 1, __ATOMIC_ACQ_REL) != 0) return false;
  const int saved_errno = errno;
  // Writing to a stderr whose pipe is closed would raise SIGPIPE and end the process after the first
  // line: block it for the thread while the report is written (the write then fails with EPIPE).
  sigset_t block_pipe, old_mask;
  sigemptyset(&block_pipe);
  sigaddset(&block_pipe, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &block_pipe, &old_mask);
  // A report that hangs (a lock held by a thread that is gone, a stuck read) must end the process
  // rather than freeze it: SIGALRM's default action does. The handler's own mask may block it.
  sigset_t unblock_alarm;
  sigemptyset(&unblock_alarm);
  sigaddset(&unblock_alarm, SIGALRM);
  pthread_sigmask(SIG_UNBLOCK, &unblock_alarm, nullptr);
  alarm(15);

  // 1. The stack, before anything else disturbs it.
  const int got = backtrace(s.frames, kFrameSlots);
  int nframes = got > 0 ? got : 0;
  int first = 0;
  bool pc_in_frames = false;
  if (f.has_pc) {
    for (int i = 0; i < nframes; ++i) {
      if (reinterpret_cast<uint64_t>(s.frames[i]) == f.pc) {
        first = i;  // the frames above it are the handlers; the fault is the one under them
        pc_in_frames = true;
        break;
      }
    }
    if (!pc_in_frames && f.has_regs) {
      // No usable backtrace (libgcc missing, or it stopped in the handler): walk the frame
      // pointers from the register state. Code built without frame pointers ends the walk early.
      nframes = 0;
      s.frames[nframes++] = reinterpret_cast<void*>(f.pc);
      uint64_t fp = f.gpr[5];
      const uint64_t sp = f.gpr[4];
      while (nframes < kMaxFrames) {
        if ((fp & 7) != 0 || fp < sp || fp - sp > (uint64_t(16) << 20)) break;
        uint64_t pair[2];
        if (ProbeRead(reinterpret_cast<const void*>(fp), pair, 16) != 16) break;
        if (pair[1] < 4096) break;
        s.frames[nframes++] = reinterpret_cast<void*>(pair[1]);
        if (pair[0] <= fp) break;
        fp = pair[0];
      }
      first = 0;
      pc_in_frames = true;
    }
  }

  // 2. Ask for the log sink to be flushed (what the process logged before is on disk first).
  LogFlushRequest();

  // 3. Resolve. Entry 0 is the fault pc when there is one, then the frames.
  size_t n = 0;
  size_t frame_index_base = 0;
  if (f.has_pc && !pc_in_frames) {
    s.addrs[n] = f.pc;
    s.lookup[n] = f.pc;
    ++n;
    frame_index_base = 1;
  }
  const int last = nframes < first + kMaxFrames ? nframes : first + kMaxFrames;
  for (int i = first; i < last && n < static_cast<size_t>(kMaxFrames + 2); ++i) {
    const uint64_t a = reinterpret_cast<uint64_t>(s.frames[i]);
    s.addrs[n] = a;
    // A return address points after the call: look the caller up one byte earlier. The frame the
    // fault happened in is exact.
    s.lookup[n] = (pc_in_frames && i == first && f.has_pc) ? a : (a > 0 ? a - 1 : a);
    ++n;
  }
  Symbolise(n);

  // 4. Write.
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  const int tid = Gettid();
  {
    Text p(s.file_prefix, sizeof(s.file_prefix) - 1);
    AppendTimestamp(p, static_cast<int64_t>(now.tv_sec), static_cast<int>(now.tv_nsec / 1000000),
                    s.utc_offset);
    p.Str(" [error] [larecomp.crash] [t");
    p.Dec(tid);
    p.Str("] ");
    s.file_prefix[p.size()] = 0;
    Text e(s.err_prefix, sizeof(s.err_prefix) - 1);
    e.Str("[error] [larecomp.crash] [t");
    e.Dec(tid);
    e.Str("] ");
    s.err_prefix[e.size()] = 0;
  }
  int fd_log = -1;
  if (s.log_path[0] != 0) fd_log = open(s.log_path, O_WRONLY | O_APPEND | O_CLOEXEC);
  const int fd_err = 2;

  Text b(s.body, sizeof(s.body));
  const char* headline = f.is_abort ? "!!! ABORT DETECTED !!!" : "!!! CRASH DETECTED !!!";
  b.Str(headline);
  if (f.is_abort) {
    b.Str(" (");
    b.Str(f.reason ? f.reason : "abort()");
    b.Str(")");
  }
  EmitBody(fd_log, fd_err, b);

  if (f.is_abort) {
    if (f.detail && *f.detail) {
      b.Clear();
      b.Str("Detalhe: ");
      b.Str(f.detail);
      EmitBody(fd_log, fd_err, b);
    }
  } else {
    b.Clear();
    b.Str("Codigo: 0x");
    b.Hex(static_cast<uint64_t>(f.signo), 8);
    b.Str(" no endereco 0x");
    b.Hex(f.has_pc ? f.pc : f.addr, 16);
    EmitBody(fd_log, fd_err, b);

    if (f.has_addr && (f.signo == SIGSEGV || f.signo == SIGBUS)) {
      b.Clear();
      b.Str("Access violation: ");
      b.Str(f.op == 1 ? "LEITURA" : f.op == 2 ? "ESCRITA" : f.op == 3 ? "EXEC(DEP)" : "?");
      b.Str(" em 0x");
      b.Hex(f.addr, 16);
      if (f.addr < 0x10000) b.Str("  (null page)");
      EmitBody(fd_log, fd_err, b);
    }

    b.Clear();
    b.Str("Faulting instruction:");
    EmitBody(fd_log, fd_err, b);
    if (f.has_pc) {
      b.Clear();
      // Entry 0 is the pc itself when it was not part of the backtrace, else the first frame.
      FormatAddr(b, "PC", f.pc, s.regions[0], s.have_modoff[0], s.modoff[0], s.hits[0]);
      EmitBody(fd_log, fd_err, b);
    } else {
      b.Clear();
      b.Str("  PC <unknown>");
      EmitBody(fd_log, fd_err, b);
    }
  }

  b.Clear();
  b.Str("Stack trace:");
  EmitBody(fd_log, fd_err, b);
  bool any_mangled = false;
  if (n == 0) {
    b.Clear();
    b.Str("  <no stack: backtrace() returned nothing>");
    EmitBody(fd_log, fd_err, b);
  }
  for (size_t i = frame_index_base; i < n; ++i) {
    char lbl[8];
    Text lt(lbl, sizeof(lbl) - 1);
    lt.Char('#');
    lt.Dec(static_cast<int64_t>(i - frame_index_base), 2, '0');
    lbl[lt.size()] = 0;
    b.Clear();
    FormatAddr(b, lbl, s.addrs[i], s.regions[i], s.have_modoff[i], s.modoff[i], s.hits[i]);
    EmitBody(fd_log, fd_err, b);
    if (s.hits[i].found && s.hits[i].name[0] == '_' && s.hits[i].name[1] == 'Z') any_mangled = true;
  }
  if (any_mangled) {
    b.Clear();
    b.Str("  (C++ names are not demangled here: c++filt reads them)");
    EmitBody(fd_log, fd_err, b);
  }

  // Extras the Windows handler has no equivalent of.
  if (f.signo != 0) {
    b.Clear();
    b.Str("Signal: ");
    b.Str(SignalName(f.signo));
    b.Str(" (");
    b.Dec(f.signo);
    b.Str(")");
    if (f.has_si_code) {
      b.Str(", si_code ");
      b.Dec(f.si_code);
      const char* name = SiCodeName(f.signo, f.si_code);
      if (*name) {
        b.Str(" ");
        b.Str(name);
      }
    }
    EmitBody(fd_log, fd_err, b);
  }
  {
    char tname[32] = {};
    prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(tname), 0, 0, 0);
    tname[sizeof(tname) - 1] = 0;
    b.Clear();
    b.Str("Thread: t");
    b.Dec(tid);
    b.Str(" \"");
    b.Str(tname);
    b.Str("\"  process ");
    b.Dec(static_cast<int64_t>(getpid()));
    EmitBody(fd_log, fd_err, b);
    if (s.exe_path[0] != 0) {
      b.Clear();
      b.Str("Executable: ");
      b.Str(s.exe_path);
      EmitBody(fd_log, fd_err, b);
    }
  }
  if (f.has_pc && !f.is_abort) {
    unsigned char code[16];
    const size_t got_bytes = ProbeRead(reinterpret_cast<const void*>(f.pc), code, sizeof(code));
    b.Clear();
    b.Str("Instruction bytes at PC:");
    if (got_bytes == 0) {
      b.Str(" <not readable>");
    } else {
      for (size_t i = 0; i < got_bytes; ++i) {
        b.Str(" ");
        b.Hex(code[i], 2);
      }
    }
    EmitBody(fd_log, fd_err, b);
  }
  if (f.has_regs && !f.is_abort) WriteRegisters(fd_log, fd_err, f);

  if (fd_log >= 0) {
    fsync(fd_log);
    close(fd_log);
  }
  __atomic_store_n(&s.reported, 1, __ATOMIC_RELEASE);
  alarm(0);
  pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  errno = saved_errno;
  return true;
}

// ---- Fault from a signal -------------------------------------------------------------------------

inline Fault FaultFromSignal(int sig, const siginfo_t* info, const void* uctx) {
  Fault f;
  f.signo = sig;
  if (info) {
    f.has_si_code = true;
    f.si_code = info->si_code;
    if (info->si_code > 0 && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE)) {
      f.has_addr = sig == SIGSEGV || sig == SIGBUS;
      f.addr = reinterpret_cast<uint64_t>(info->si_addr);
    }
  }
  if (sig == SIGABRT) {
    f.is_abort = true;
    f.reason = "abort()";
    return f;  // the register state is inside raise(): the backtrace says it better
  }
#if defined(__x86_64__)
  if (uctx) {
    const ucontext_t* uc = static_cast<const ucontext_t*>(uctx);
    const greg_t* r = uc->uc_mcontext.gregs;
    f.has_pc = true;
    f.pc = static_cast<uint64_t>(r[REG_RIP]);
    f.has_regs = true;
    static const int kOrder[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                   REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
                                   REG_R12, REG_R13, REG_R14, REG_R15};
    for (int i = 0; i < 16; ++i) f.gpr[i] = static_cast<uint64_t>(r[kOrder[i]]);
    f.rflags = static_cast<uint64_t>(r[REG_EFL]);
    f.has_err = true;
    f.err = static_cast<uint64_t>(r[REG_ERR]);
    if (sig == SIGSEGV || sig == SIGBUS) {
      f.op = (f.err & 0x10) ? 3 : (f.err & 0x2) ? 2 : 1;  // X86_PF_INSTR, X86_PF_WRITE
    }
  }
#elif defined(__aarch64__)
  if (uctx) {
    const ucontext_t* uc = static_cast<const ucontext_t*>(uctx);
    f.has_pc = true;
    f.pc = static_cast<uint64_t>(uc->uc_mcontext.pc);
  }
#else
  (void)uctx;
#endif
  return f;
}

// ---- the handlers --------------------------------------------------------------------------------

// Another thread is already writing the report: stay out of its way until the process is gone.
inline void WaitForOtherReport() {
  for (int i = 0; i < 100; ++i) SleepMs(50);
}

// SIGBUS, SIGFPE, SIGABRT, and every signal before InstallLate.
inline void FatalSignalHandler(int sig, siginfo_t* info, void* uctx) {
  const int saved_errno = errno;
  State& s = S();
  if (s.skip_signal && s.skip_signal(sig)) {
    // The runtime's own handling is active on this thread: it wants the signal. Hand it over.
    const struct sigaction& p = s.prev[sig];
    if ((p.sa_flags & SA_SIGINFO) && p.sa_sigaction) {
      p.sa_sigaction(sig, info, uctx);
      errno = saved_errno;
      return;
    }
    if (!(p.sa_flags & SA_SIGINFO) && p.sa_handler != SIG_DFL && p.sa_handler != SIG_IGN) {
      p.sa_handler(sig);
      errno = saved_errno;
      return;
    }
  }
  if (__atomic_load_n(&s.reported, __ATOMIC_ACQUIRE) == 0) {
    Fault f = FaultFromSignal(sig, info, uctx);
    if (!Report(f)) WaitForOtherReport();
  }
  DefaultAndRaise(sig);
  errno = saved_errno;
}

// SIGSEGV and SIGILL after InstallLate: runs the runtime's handler on the signal stack and returns.
inline void FaultWrapper(int sig, siginfo_t* info, void* uctx) {
  const int saved_errno = errno;
  State& s = S();
  if (__atomic_load_n(&s.reporting, __ATOMIC_ACQUIRE) != 0) {
    // A crash report is being written by another thread: this fault is not worth another one.
    WaitForOtherReport();
  }
  Stash& st = t_stash;
  ++st.depth;
  st.sig = sig;
  st.uctx = uctx;
  if (info) __builtin_memcpy(&st.info, info, sizeof(siginfo_t));
  const struct sigaction& p = s.prev[sig];
  bool called = false;
  if (p.sa_flags & SA_SIGINFO) {
    if (p.sa_sigaction) {
      p.sa_sigaction(sig, info, uctx);
      called = true;
    }
  } else if (p.sa_handler != SIG_DFL && p.sa_handler != SIG_IGN) {
    p.sa_handler(sig);
    called = true;
  }
  --st.depth;
  if (!called) {
    // No runtime handler under ours: nothing can consume this fault.
    if (__atomic_load_n(&s.reported, __ATOMIC_ACQUIRE) == 0) {
      Fault f = FaultFromSignal(sig, info, uctx);
      if (!Report(f)) WaitForOtherReport();
    }
    DefaultAndRaise(sig);
  }
  errno = saved_errno;
}

// Reached when every handler of the runtime declined a SIGSEGV or SIGILL (crash_handler.cpp's
// rex::platform::ExceptionHandler callback). `from_runtime` is what the runtime's Exception said, used
// when this thread is not inside FaultWrapper. The caller returns false afterwards: the runtime
// forwards the fault to FatalSignalHandler (the report is written already) and the default action
// ends the process.
inline void ReportUnconsumed(const Fault& from_runtime) {
  State& s = S();
  const Stash& st = t_stash;
  Fault f = from_runtime;
  int sig = from_runtime.signo;
  if (st.depth > 0) {
    f = FaultFromSignal(st.sig, &st.info, st.uctx);
    sig = st.sig;
    // The runtime knew the operation and address even for faults the kernel gave no si_addr for.
    if (!f.has_addr && from_runtime.has_addr && st.info.si_code > 0) {
      f.has_addr = true;
      f.addr = from_runtime.addr;
    }
  }
  if (__atomic_load_n(&s.reported, __ATOMIC_ACQUIRE) == 0) {
    if (!Report(f)) WaitForOtherReport();
  }
  if (sig != 0) DefaultAndRaise(sig);
}

// For std::terminate and the like (not a signal): the stack of the calling thread.
inline void ReportAbortLike(const char* reason, const char* detail) {
  Fault f;
  f.is_abort = true;
  f.reason = reason;
  f.detail = detail;
  Report(f);
}

// ---- installation --------------------------------------------------------------------------------

inline void* FlushMain(void*) {
  State& s = S();
  for (;;) {
    if (sem_wait(&s.flush_req) != 0) {
      if (errno == EINTR) continue;
      return nullptr;
    }
    if (s.flush_logs) s.flush_logs();
    __atomic_store_n(&s.flush_done, 1, __ATOMIC_RELEASE);
  }
}

// A signal stack for the calling thread (each thread has its own). The installing thread gets the
// static one in InstallEarly; a thread that wants one passes its own memory.
inline bool EnableAltStack(void* memory, size_t size) {
  stack_t ss;
  __builtin_memset(&ss, 0, sizeof(ss));
  ss.ss_sp = memory;
  ss.ss_size = size;
  ss.ss_flags = 0;
  return sigaltstack(&ss, nullptr) == 0;
}

inline void SetHandler(int sig, void (*fn)(int, siginfo_t*, void*), bool block_all) {
  State& s = S();
  struct sigaction sa;
  __builtin_memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = fn;
  if (block_all) {
    sigfillset(&sa.sa_mask);
  } else {
    sigemptyset(&sa.sa_mask);  // the runtime's own handler runs with the same (empty) mask
  }
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(sig, &sa, &s.prev[sig]);
}

// The local time's offset from UTC in seconds. Taken once at start: localtime_r is not
// async-signal-safe, so a report adds this to clock_gettime instead of converting itself.
inline int LocalUtcOffsetSeconds() {
  time_t now = time(nullptr);
  struct tm local;
  __builtin_memset(&local, 0, sizeof(local));
  localtime_r(&now, &local);
  return static_cast<int>(local.tm_gmtoff);
}

// `log_path`: the file the log sink appends to (absolute; empty or null: stderr only).
// `flush`: flushes the log sink; runs on a helper thread, never in a signal handler.
inline void InstallEarly(const char* log_path, int utc_offset_seconds, void (*flush)()) {
  State& s = S();
  s.utc_offset = utc_offset_seconds;
  s.flush_logs = flush;
  if (log_path) CopyZ(s.log_path, sizeof(s.log_path), log_path, CStrLen(log_path));
  {
    ssize_t n = readlink("/proc/self/exe", s.exe_path, sizeof(s.exe_path) - 1);
    s.exe_path[n > 0 ? n : 0] = 0;
  }
  s.probe_pipe[0] = s.probe_pipe[1] = -1;
  if (pipe2(s.probe_pipe, O_NONBLOCK | O_CLOEXEC) != 0) s.probe_pipe[0] = s.probe_pipe[1] = -1;

  // libgcc is loaded by the first backtrace(): do that now, not in a signal handler.
  {
    void* warm[8];
    backtrace(warm, 8);
  }

  if (flush && sem_init(&s.flush_req, 0, 0) == 0) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &attr, &FlushMain, nullptr) == 0) s.flush_thread_ok = true;
    pthread_attr_destroy(&attr);
  }

  EnableAltStack(s.altstack, sizeof(s.altstack));
  static const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  for (int sig : kSignals) SetHandler(sig, &FatalSignalHandler, true);
}

// After Runtime::Setup: put ours on top of the runtime's (see the top of this file).
inline void InstallLate() {
  static const int kRuntimeOwned[] = {SIGSEGV, SIGILL};
  static const int kSdkOwned[] = {SIGBUS, SIGFPE};
  for (int sig : kRuntimeOwned) {
    struct sigaction cur;
    sigaction(sig, nullptr, &cur);
    if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == &FaultWrapper) continue;
    SetHandler(sig, &FaultWrapper, false);
  }
  for (int sig : kSdkOwned) {
    struct sigaction cur;
    sigaction(sig, nullptr, &cur);
    if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == &FatalSignalHandler) continue;
    SetHandler(sig, &FatalSignalHandler, true);
  }
}

}  // namespace larecomp::crash::lx

#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "crash_handler.h"
#include "larecomp_log.h"

#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::mutex g_sym_mtx;  // dbghelp is single-threaded; serialize handler entry

// Resolve one host address to "module+off  symbol+off (file:line)" and log it.
// Recompiled guest functions appear in the PDB as rex_sub_82XXXXXX, so a symbol
// like that is a direct pointer to a GUEST address to open in the IDB.
void LogAddr(const char* label, DWORD64 a) {
    HANDLE proc = GetCurrentProcess();

    char modname[64] = "?";
    DWORD64 modbase = 0;
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(a), &mod) &&
        mod) {
        modbase = reinterpret_cast<DWORD64>(mod);
        char full[MAX_PATH];
        if (GetModuleFileNameA(mod, full, MAX_PATH)) {
            const char* b = std::strrchr(full, '\\');
            std::snprintf(modname, sizeof(modname), "%s", b ? b + 1 : full);
        }
    }

    char symbuf[sizeof(SYMBOL_INFO) + 512] = {};
    auto* si = reinterpret_cast<SYMBOL_INFO*>(symbuf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 512;
    DWORD64 sdisp = 0;
    const char* sym = nullptr;
    if (SymFromAddr(proc, a, &sdisp, si)) sym = si->Name;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    bool haveline = SymGetLineFromAddr64(proc, a, &ldisp, &line) != FALSE;

    if (sym && haveline) {
        LARECOMP_CRASH_ERROR("  {} 0x{:016X}  {}+0x{:X}  {}+0x{:X}  ({}:{})", label, a, modname,
                             a - modbase, sym, sdisp, line.FileName, line.LineNumber);
    } else if (sym) {
        LARECOMP_CRASH_ERROR("  {} 0x{:016X}  {}+0x{:X}  {}+0x{:X}", label, a, modname, a - modbase,
                             sym, sdisp);
    } else {
        LARECOMP_CRASH_ERROR("  {} 0x{:016X}  {}+0x{:X}  <no symbol>", label, a, modname,
                             a - modbase);
    }
}

}  // namespace

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::lock_guard<std::mutex> lock(g_sym_mtx);

    const auto* er = ep->ExceptionRecord;
    LARECOMP_CRASH_ERROR("!!! CRASH DETECTED !!!");
    LARECOMP_CRASH_ERROR("Codigo: 0x{:08X} no endereco 0x{:016X}",
                         static_cast<unsigned>(er->ExceptionCode),
                         reinterpret_cast<std::uintptr_t>(er->ExceptionAddress));

    // Access violations carry the operation (read/write/exec) and target address.
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = er->ExceptionInformation[0] == 0   ? "LEITURA"
                         : er->ExceptionInformation[0] == 1 ? "ESCRITA"
                         : er->ExceptionInformation[0] == 8 ? "EXEC(DEP)"
                                                            : "?";
        LARECOMP_CRASH_ERROR("Access violation: {} em 0x{:016X}", op,
                             static_cast<std::uintptr_t>(er->ExceptionInformation[1]));
    }

    // Faulting instruction pointer, resolved.
    LARECOMP_CRASH_ERROR("Faulting instruction:");
    LogAddr("PC", reinterpret_cast<DWORD64>(er->ExceptionAddress));

    // Walk the call stack from the crash context (not the handler's own stack).
    LARECOMP_CRASH_ERROR("Stack trace:");
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
#if defined(_M_X64) || defined(__x86_64__)
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
    sf.AddrPC.Offset = ctx.Eip;
    sf.AddrFrame.Offset = ctx.Ebp;
    sf.AddrStack.Offset = ctx.Esp;
    const DWORD machine = IMAGE_FILE_MACHINE_I386;
#endif
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Mode = AddrModeFlat;

    HANDLE proc = GetCurrentProcess();
    HANDLE thr = GetCurrentThread();
    for (int frame = 0; frame < 48; ++frame) {
        if (!StackWalk64(machine, proc, thr, &sf, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)) {
            break;
        }
        if (sf.AddrPC.Offset == 0) break;
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "#%02d", frame);
        LogAddr(lbl, sf.AddrPC.Offset);
    }

    spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) {
        if (logger) logger->flush();
    });

    return EXCEPTION_CONTINUE_SEARCH;
}

namespace {

void FlushLogs() {
    spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) {
        if (logger) logger->flush();
    });
}

// Stack trace for deaths that carry no EXCEPTION_POINTERS: std::terminate,
// abort(), a CRT invalid parameter, a pure virtual call. SetUnhandledException
// Filter never sees these - the process is gone before any exception is raised,
// which is what "it crashed and left no log" looks like.
void LogCurrentStack(const char* reason, const char* detail = nullptr) {
    std::lock_guard<std::mutex> lock(g_sym_mtx);
    LARECOMP_CRASH_ERROR("!!! ABORT DETECTED !!! ({})", reason);
    if (detail && *detail) {
        LARECOMP_CRASH_ERROR("Detalhe: {}", detail);
    }
    LARECOMP_CRASH_ERROR("Stack trace:");
    void* frames[48] = {};
    const USHORT captured = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < captured; ++i) {
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "#%02d", int(i));
        LogAddr(lbl, reinterpret_cast<DWORD64>(frames[i]));
    }
    FlushLogs();
}

void OnTerminate() {
    std::string detail;
    // An uncaught C++ exception is the usual reason terminate runs; recover its
    // message by rethrowing inside the handler, which is legal here.
    if (std::current_exception()) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            detail = std::string("uncaught std::exception: ") + e.what();
        } catch (...) {
            detail = "uncaught non-std exception";
        }
    }
    LogCurrentStack("std::terminate", detail.c_str());
    // Exit without running the default terminate handler, which would abort
    // into Windows Error Reporting and can hang on a machine with no debugger.
    std::_Exit(3);
}

void OnAbortSignal(int) {
    LogCurrentStack("abort()");
    std::_Exit(3);
}

void OnInvalidParameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                        unsigned int line, uintptr_t) {
    char detail[512];
    std::snprintf(detail, sizeof(detail), "%ls in %ls (%ls:%u)",
                  expression ? expression : L"<no expression>", function ? function : L"<unknown>",
                  file ? file : L"<no file>", line);
    LogCurrentStack("CRT invalid parameter", detail);
    std::_Exit(3);
}

void OnPureCall() {
    LogCurrentStack("pure virtual call");
    std::_Exit(3);
}

}  // namespace

void InstallCrashLogger() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    SetUnhandledExceptionFilter(&CrashHandler);

    // The paths above kill the process without ever raising an exception, so
    // without these the log just stops mid-line with no reason recorded.
    std::set_terminate(&OnTerminate);
    std::signal(SIGABRT, &OnAbortSignal);
    // Keep abort() on the signal path instead of letting the CRT fast-fail
    // straight into Windows Error Reporting, and drop the modal abort message.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(&OnInvalidParameter);
    _set_purecall_handler(&OnPureCall);

    // Separates "the process crashed" from "the process exited on purpose":
    // std::_Exit (used by the shutdown paths) skips this, a normal exit does
    // not, and a crash reaches neither.
    std::atexit([]() {
        LARECOMP_CRASH_INFO("Process exiting normally (exit/return from main)");
        FlushLogs();
    });
}

std::filesystem::path ExeDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);

    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buffer).parent_path();
    }

    return std::filesystem::current_path();
}

#else

#if defined(__linux__)
// The Linux crash logger. The machinery (signal handlers on a signal stack, the async-signal-safe
// report, the ELF symbol lookup) is in crash_report_linux.h and crash_report_util.h, which need
// nothing of the game or the SDK; this file is the glue: where the log is, how the runtime's own
// fault handling is chained with, std::terminate, the exit marker and the crash test.
//
// How it chains with the runtime. The runtime handles SIGSEGV and SIGILL itself (write watches on
// guest memory, MMIO): rex::arch::ExceptionHandler keeps a list of callbacks and calls them in the
// order they were installed until one returns true. Its signal handler never chains and never
// restores the default action, so a fault no callback takes would run again and fault again for
// ever. InstallCrashLoggerAfterRuntime() therefore (1) registers OnUnconsumedFault AFTER the
// runtime's own callback, so it is reached only for a fault that nothing consumed and then writes
// the report, restores the default action and returns false (the instruction faults once more and
// the process dies with the normal status and core dump), and (2) puts a wrapper on SIGSEGV and
// SIGILL with SA_ONSTACK that calls the runtime's handler first (what makes the signal stack count)
// and takes SIGBUS and SIGFPE, which the SDK's seh_initialize had taken from the early handlers.
// A fault the runtime handles never reaches the report.

#include <spdlog/spdlog.h>

#include <rex/cvar.h>
#include <rex/exception_handler.h>
#include <rex/platform/seh.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "crash_report_linux.h"
#include "mc_engine/debug_menu.h"
#include "spdlog_console.h"

extern uint8_t* g_guest_mem;  // main.cpp; set once the runtime's memory exists

REXCVAR_DEFINE_STRING(
    mcla_test_crash, "", "MCLA/Debug",
    "Crash test for the Linux crash logger: MODE or MODE:SECONDS (5 seconds by default), a few "
    "seconds after the game has started. segv writes to address 0x10 on a worker thread; guest "
    "reads the guest's null page (the runtime logs 'Unhandled guest access violation' first, as for "
    "a real guest crash); stack runs a worker thread out of stack (it has a signal stack); abort "
    "calls abort(); terminate throws an exception nobody catches; illegal runs an illegal "
    "instruction; fpe divides by zero. The log must end with '!!! CRASH DETECTED !!!' (or '!!! "
    "ABORT DETECTED !!!') and a stack trace, and the game dies with the signal's status. Only works "
    "with the debug menu unlocked. Linux only.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();

namespace {

namespace lx = larecomp::crash::lx;

void FlushLogs() {
    spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) {
        if (logger) logger->flush();
    });
}

// A signal handler must never take the log sink's lock (the crashing thread may hold it), so the
// flush runs on the reporter's helper thread, which the handler waits for at most 300 ms.
void FlushLogsOnHelperThread() {
    FlushLogs();
}

// The SDK's seh_initialize handlers for SIGBUS and SIGFPE belong to the code that asked for SEH
// protection (nothing in the game does today): leave a fault inside such a region to it.
bool SehRegionActive(int) {
    return rex::platform::seh_active();
}

// Registered after the runtime's own callbacks (see the top of the section): reached only when
// every one of them declined this fault.
bool OnUnconsumedFault(rex::arch::Exception* ex, void*) {
    lx::Fault f;
    switch (ex->code()) {
        case rex::arch::Exception::Code::kAccessViolation:
            f.signo = SIGSEGV;
            f.has_addr = true;
            f.addr = ex->fault_address();
            switch (ex->access_violation_operation()) {
                case rex::arch::Exception::AccessViolationOperation::kRead:
                    f.op = 1;
                    break;
                case rex::arch::Exception::AccessViolationOperation::kWrite:
                    f.op = 2;
                    break;
                default:
                    break;
            }
            break;
        case rex::arch::Exception::Code::kIllegalInstruction:
            f.signo = SIGILL;
            break;
        default:
            return false;
    }
#if REX_ARCH_AMD64
    if (const rex::arch::HostThreadContext* c = ex->thread_context()) {
        f.has_pc = true;
        f.pc = c->rip;
        f.has_regs = true;
        for (int i = 0; i < 16; ++i) f.gpr[i] = c->int_registers[i];
        f.rflags = c->eflags;
    }
#endif
    lx::ReportUnconsumed(f);
    return false;  // nobody took it: the instruction runs again and the default action ends the process
}

// Stack trace for the deaths that carry no signal of their own: std::terminate.
void OnTerminate() {
    char detail[512] = {};
    // An uncaught C++ exception is the usual reason terminate runs; recover its message by
    // rethrowing inside the handler, which is legal here.
    if (std::current_exception()) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            std::snprintf(detail, sizeof(detail), "uncaught std::exception: %s", e.what());
        } catch (...) {
            std::snprintf(detail, sizeof(detail), "uncaught non-std exception");
        }
    }
    lx::ReportAbortLike("std::terminate", detail);
    // abort() raises SIGABRT; the report is written already, the handler only ends the process
    // with the default action (status 134, core dump).
    std::abort();
}

// ---- the crash test (mcla_test_crash) -------------------------------------------------------------

alignas(16) char g_test_altstack[128 * 1024];

[[gnu::noinline]] void TestWriteNull() {
    volatile int* p = reinterpret_cast<volatile int*>(0x10);
    *p = 1;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion"
#endif
[[gnu::noinline]] int TestRecurse(int depth) {
    volatile char pad[2048];
    pad[0] = static_cast<char>(depth);
    return TestRecurse(depth + 1) + pad[0];
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

[[gnu::noinline]] void TestDivide() {
    volatile int zero = 0;
    volatile int r = 7 / zero;
    (void)r;
}

void RunCrashTest(const std::string& mode) {
    if (mode == "segv") {
        TestWriteNull();
    } else if (mode == "guest") {
        if (!g_guest_mem) {
            LARECOMP_CRASH_ERROR("[crash-test] no guest memory yet");
            return;
        }
        volatile uint8_t* p = g_guest_mem + 0x0C;
        const uint8_t v = *p;
        LARECOMP_CRASH_ERROR("[crash-test] guest address 0xC was readable (value {}): protect_zero is off?",
                             static_cast<unsigned>(v));
    } else if (mode == "stack") {
        lx::EnableAltStack(g_test_altstack, sizeof(g_test_altstack));
        TestRecurse(0);
    } else if (mode == "abort") {
        std::abort();
    } else if (mode == "terminate") {
        throw std::runtime_error("mcla_test_crash=terminate");
    } else if (mode == "illegal") {
        __builtin_trap();
    } else if (mode == "fpe") {
        TestDivide();
    } else {
        LARECOMP_CRASH_ERROR("[crash-test] unknown mode '{}' (segv, guest, stack, abort, terminate, illegal, fpe)",
                             mode);
    }
}

void ArmCrashTest() {
    std::string spec = REXCVAR_GET(mcla_test_crash);
    if (spec.empty()) return;
    // The same gate as everything hidden: the answer was fixed at start, nothing unlocks a running game.
    if (!larecomp::DebugMenuUnlocked()) {
        LARECOMP_CRASH_WARN("[crash-test] mcla_test_crash='{}' ignored: the debug menu is locked (--debug_menu)",
                            spec);
        return;
    }
    std::string mode = spec;
    int seconds = 5;
    if (const auto colon = spec.find(':'); colon != std::string::npos) {
        mode = spec.substr(0, colon);
        seconds = std::atoi(spec.c_str() + colon + 1);
        if (seconds < 0) seconds = 0;
    }
    LARECOMP_CRASH_WARN("[crash-test] mcla_test_crash: '{}' in {} s on a worker thread; the game will die", mode,
                        seconds);
    std::thread([mode, seconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        RunCrashTest(mode);
    }).detach();
}

}  // namespace

void InstallCrashLogger() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    // Where the log sink appends, as an absolute path: the report opens the file again by name.
    std::string log_path;
    if (const char* name = LarecompLogFilePath(); name && *name) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(name, ec);
        log_path = ec ? std::string(name) : absolute.string();
    }
    lx::InstallEarly(log_path.c_str(), lx::LocalUtcOffsetSeconds(), &FlushLogsOnHelperThread);
    lx::S().skip_signal = &SehRegionActive;

    // The paths that kill the process without a signal of their own reaching the handlers above.
    std::set_terminate(&OnTerminate);

    // Separates "the process crashed" from "the process exited on purpose": std::_Exit (used by
    // the shutdown paths) skips this, a normal exit does not, and a crash reaches neither.
    std::atexit([]() {
        LARECOMP_CRASH_INFO("Process exiting normally (exit/return from main)");
        FlushLogs();
    });
}

void InstallCrashLoggerAfterRuntime() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    // After the runtime's own callback (MMIOHandler::Install, in Memory::Initialize, which
    // Runtime::Setup has run by the time OnPostSetup calls this): called in the order installed.
    rex::arch::ExceptionHandler::Install(&OnUnconsumedFault, nullptr);
    lx::InstallLate();
    LARECOMP_CRASH_INFO("Linux crash logger active (log {})", LarecompLogFilePath());

    ArmCrashTest();
}

#else  // another POSIX system: no crash logger

void InstallCrashLogger() {}
void InstallCrashLoggerAfterRuntime() {}

#endif  // __linux__

std::filesystem::path ExeDir() {
    try {
        return std::filesystem::canonical("/proc/self/exe").parent_path();
    } catch (...) {
        return std::filesystem::current_path();
    }
}

#endif

#endif // REXGLUE_HAS_XEO3_TARGET

#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "crash_handler.h"
#include "crash_options.h"
#include "larecomp_log.h"
#include "process_exit.h"

#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

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

// Hands what the logger holds to every sink, so the file has the last lines. rexglue 0.11: the SDK's logger is the only
// registry (spdlog is private to the SDK), and rex::log::Flush() is its call for a crash handler.
void FlushLogs() { rex::log::Flush(); }

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

    // std::format (the SDK's log macros) takes a C string by pointer; an array argument is the one spelling left to doubt.
    const char* const mod_name = modname;

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
        LARECOMP_CRASH_ERROR("  {} 0x{:016X}  {}+0x{:X}  {}+0x{:X}  ({}:{})", label, a, mod_name,
                             a - modbase, sym, sdisp, line.FileName, line.LineNumber);
    } else if (sym) {
        LARECOMP_CRASH_ERROR("  {} 0x{:016X}  {}+0x{:X}  {}+0x{:X}", label, a, mod_name, a - modbase,
                             sym, sdisp);
    } else {
        LARECOMP_CRASH_ERROR("  {} 0x{:016X}  {}+0x{:X}  <no symbol>", label, a, mod_name,
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

    FlushLogs();

    // The test harness's switch (crash_options.h): the SDK's reporter has written its report and skipped its box (LarecompApp::
    // GetCrashOptions) and has called this filter last, so nothing is left to write. End the process here with a status of its
    // own instead of going on to Windows Error Reporting (its box, its minutes-long dump), so a test run sees the end at once.
    if (larecomp::crash::NoDialogFromEnvironment()) {
        larecomp::EndProcessNow(static_cast<int>(larecomp::crash::kNoDialogExitCode));
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

namespace {

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
    // Not std::_Exit: that still ends in ExitProcess, whose DLL detach destroys a
    // live graphics driver (process_exit.h), a second failure behind this one.
    larecomp::EndProcessNow(3);
}

void OnAbortSignal(int) {
    LogCurrentStack("abort()");
    larecomp::EndProcessNow(3);
}

void OnInvalidParameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                        unsigned int line, uintptr_t) {
    char detail[512];
    std::snprintf(detail, sizeof(detail), "%ls in %ls (%ls:%u)",
                  expression ? expression : L"<no expression>", function ? function : L"<unknown>",
                  file ? file : L"<no file>", line);
    LogCurrentStack("CRT invalid parameter", detail);
    larecomp::EndProcessNow(3);
}

void OnPureCall() {
    LogCurrentStack("pure virtual call");
    larecomp::EndProcessNow(3);
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
    // CHECK (rexglue 0.11): rex::Init then calls std::signal(SIGABRT, AbortSignal) (src/base/crash_win.cpp,
    // InstallPlatform), which replaces this handler and does not call it (it keeps ours only to put it back at
    // uninstall). So an abort() writes the SDK's "SIGABRT (abort, ...)" report and the CRT ends the process
    // (status 3), not LogCurrentStack's "ABORT DETECTED" block with symbols. The unhandled-exception filter
    // is not affected: the SDK's filter calls ours (its g_previous_filter). If the symbols are missed, set
    // OnAbortSignal again in OnInit; decide from a run with an abort() (no test mode on Windows).
    std::signal(SIGABRT, &OnAbortSignal);
    // Keep abort() on the signal path instead of letting the CRT fast-fail
    // straight into Windows Error Reporting, and drop the modal abort message.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(&OnInvalidParameter);
    _set_purecall_handler(&OnPureCall);

    // Separates "the process crashed" from "the process exited on purpose":
    // EndProcessNow (the shutdown watchdog and the crash paths above) skips this,
    // a normal exit does not, and an unhandled exception reaches neither.
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
// How it chains with the runtime (rexglue 0.11). The runtime handles SIGSEGV and SIGILL itself (write
// watches on guest memory, MMIO): rex::platform::ExceptionHandler keeps a list of callbacks and calls them
// in the order they were installed until one returns true. A fault that no callback takes is forwarded (ForwardSignal, src/base/exception_handler_posix.cpp) to the action that was installed
// before the runtime's own, which here is our early FatalSignalHandler (crash_report_linux.h). With no such
// handler (SIG_DFL or SIG_IGN), or when the same fault comes back too often, the SDK writes its own report,
// restores the default action and re-raises. So the 0.10 trap is gone: there the handler never chained and
// never restored the default action, and a fault nothing took ran again and faulted again for ever.
//
// The SDK's own crash reporter (rex::Init, src/base/crash_posix.cpp) puts its handler only on a signal that
// still has the default action, and InstallCrashLogger() runs in the app constructor, before rex::Init:
// on Linux our handlers already hold SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT, so the SDK's reporter
// leaves all five to us and a crash writes one report, ours. (On Windows it is the other way round for
// SIGABRT: see InstallCrashLogger.) CHECK: not run on Linux yet: a deliberate crash of each kind
// (mcla_test_crash) should give one report and the signal's normal exit status.
//
// InstallCrashLoggerAfterRuntime() therefore (1) registers OnUnconsumedFault AFTER the runtime's own
// callback (the SDK adds that one when TitleRuntime::Load makes the guest memory, so the call belongs in
// OnTitleLaunch, not OnInit: from OnInit the logger's callback came first and EVERY fault, a GPU write
// watch included, was reported as a crash and ended the process; the 2026-11-21 Linux boot check died
// that way on the first write to a watched guest page, and the guard in the function refuses an early
// call), so it is reached only for a fault that nothing consumed; it writes the report (with the
// operation and address the runtime's Exception carries), restores the default action and returns false,
// and the runtime then forwards the fault to our early handler, which finds the report written and ends
// the process with the normal status and core dump. CHECK: that forward also reaches FatalSignalHandler with
// the signal's own ucontext, so this callback may be redundant now; it stays until a Linux run says so.
// And (2) puts a wrapper on SIGSEGV and SIGILL with SA_ONSTACK that calls the runtime's handler first (what
// makes the signal stack count) and takes SIGBUS and SIGFPE, which the SDK's seh_initialize had taken from
// the early handlers. A fault the runtime handles never reaches the report.

#include "settings/larecomp_settings.h"
#include <rex/platform/exception_handler.h>
#include <rex/ppc/seh.h>

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
#include "mc_engine/menus/debug_menu.h"
#include "spdlog_console.h"

extern uint8_t* g_guest_mem;  // main.cpp; set once the runtime's memory exists

namespace {

namespace lx = larecomp::crash::lx;

void FlushLogs() { rex::log::Flush(); }

// A signal handler must never take the log sink's lock (the crashing thread may hold it), so the
// flush runs on the reporter's helper thread, which the handler waits for at most 300 ms.
void FlushLogsOnHelperThread() {
    FlushLogs();
}

// The SDK's seh_initialize handlers for SIGBUS and SIGFPE belong to the code that asked for SEH
// protection (nothing in the game does today): leave a fault inside such a region to it.
bool SehRegionActive(int) {
    return rex::platform::GetSEHThreadState().top != nullptr;  // an open REX_SEH_TRY scope on this thread (POSIX)
}

// Registered after the runtime's own callbacks (see the top of the section): reached only when
// every one of them declined this fault.
bool OnUnconsumedFault(rex::platform::Exception* ex, void*) {
    lx::Fault f;
    switch (ex->GetCode()) {
        case rex::platform::Exception::Code::kAccessViolation:
            f.signo = SIGSEGV;
            f.has_addr = true;
            f.addr = ex->GetFaultAddress();
            switch (ex->GetAccessViolationOperation()) {
                case rex::platform::Exception::AccessViolationOperation::kRead:
                    f.op = 1;
                    break;
                case rex::platform::Exception::AccessViolationOperation::kWrite:
                    f.op = 2;
                    break;
                default:
                    break;
            }
            break;
        case rex::platform::Exception::Code::kIllegalInstruction:
            f.signo = SIGILL;
            break;
        default:
            return false;
    }
#if REX_ARCH_AMD64
    if (const rex::platform::HostThreadContext* c = ex->GetThreadContext()) {
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
    // abort() raises SIGABRT; the report is written already, the handler (ours: the SDK's reporter leaves
    // SIGABRT to it, see the top of this section) only ends the process with the default action
    // (status 134, core dump).
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
    std::string spec = larecomp::settings::mcla_test_crash.Get();
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

    // Where the log sink appends, as an absolute path: the report opens the file again by name. The SDK opens its file in
    // App::Init, after this runs, so only an absolute log.file_name (LARECOMP_LOG_FILE) names it for certain here;
    // otherwise the early reports go to stderr and NoteCrashLogFile() (OnInit) sets the path.
    std::string log_path;
    if (const std::filesystem::path named(rex::GetLogger().file_name.Get()); named.is_absolute()) {
        log_path = named.string();
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

// OnInit: the SDK has opened this run's log file by now, so LarecompLogFile() names it (the reports append to it by name). Kept apart
// from InstallCrashLoggerAfterRuntime() so that a crash while TitleRuntime::Load runs still reaches the file.
void NoteCrashLogFile() {
    const std::filesystem::path log_file = LarecompLogFile();
    if (!log_file.empty()) {
        std::snprintf(lx::S().log_path, sizeof(lx::S().log_path), "%s", log_file.string().c_str());
    }
}

void InstallCrashLoggerAfterRuntime() {
    static bool installed = false;
    if (installed) return;

    // The runtime's own callback (GuestAddressSpace::Initialize, which TitleRuntime::Load calls) has to be on
    // the ExceptionHandler list already: the list is called in the order installed and this callback ends the
    // process for every fault it is given. g_guest_mem is set by OnTitleLaunch once that memory exists.
    if (!g_guest_mem) {
        LARECOMP_CRASH_ERROR("[crash] the Linux crash logger's second half was asked for before the guest memory exists: "
                             "not installed (its callback would end the game at the first guest write watch)");
        return;
    }
    installed = true;

    // After the runtime's own callback: called in the order installed.
    rex::platform::ExceptionHandler::Install(&OnUnconsumedFault, nullptr);
    lx::InstallLate();
    NoteCrashLogFile();  // in case OnInit's call found no file yet
    LARECOMP_CRASH_INFO("Linux crash logger active (log {})", LarecompLogFile().string());

    ArmCrashTest();
}

#else  // another POSIX system: no crash logger

void InstallCrashLogger() {}
void NoteCrashLogFile() {}
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

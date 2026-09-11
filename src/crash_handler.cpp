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

void InstallCrashLogger() {}

std::filesystem::path ExeDir() {
    try {
        return std::filesystem::canonical("/proc/self/exe").parent_path();
    } catch (...) {
        return std::filesystem::current_path();
    }
}

#endif

#endif // REXGLUE_HAS_XEO3_TARGET

#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "crash_handler.h"
#include "mc_engine/core/logging.h"

#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <spdlog/spdlog.h>

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    MC_ERROR("!!! CRASH DETECTED !!!");
    MC_ERROR("Codigo: 0x{:08X} no endereco 0x{:016X}",
                         static_cast<unsigned>(ep->ExceptionRecord->ExceptionCode),
                         reinterpret_cast<std::uintptr_t>(
                             ep->ExceptionRecord->ExceptionAddress));

    spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) {
        if (logger) logger->flush();
    });

    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLogger() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    SetUnhandledExceptionFilter(&CrashHandler);
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

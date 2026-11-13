#pragma once

#include <filesystem>

void InstallCrashLogger();

// Linux: the second half of the crash logger, for after Runtime::Setup (call it from OnPostSetup).
// The runtime puts its own SIGSEGV and SIGILL handling in place during Setup, and the crash logger
// has to sit behind it for the faults it handles and in front of it for the rest (the long note in
// crash_handler.cpp). Nothing to do on Windows, where the unhandled exception filter is that place.
#if defined(_WIN32)
inline void InstallCrashLoggerAfterRuntime() {}
#else
void InstallCrashLoggerAfterRuntime();
#endif

std::filesystem::path ExeDir();

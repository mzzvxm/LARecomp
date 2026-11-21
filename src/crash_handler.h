#pragma once

#include <filesystem>

void InstallCrashLogger();

// Linux: the second half of the crash logger, for after TitleRuntime::Load made the guest memory (call it from OnTitleLaunch,
// never earlier: it refuses to install while g_guest_mem is not set).
// The runtime puts its own SIGSEGV and SIGILL handling in place while Load makes the guest memory, and the crash logger
// has to sit behind it for the faults it handles and in front of it for the rest (the long note in
// crash_handler.cpp). Nothing to do on Windows, where the unhandled exception filter is that place.
#if defined(_WIN32)
inline void InstallCrashLoggerAfterRuntime() {}
inline void NoteCrashLogFile() {}
#else
void InstallCrashLoggerAfterRuntime();
// Linux: tells the crash reports which file the SDK's log sink writes (call it from OnInit, once the SDK has opened it).
void NoteCrashLogFile();
#endif

std::filesystem::path ExeDir();

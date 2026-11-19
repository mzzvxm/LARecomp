// RESTART GAME: see restart.h. The Windows hand-over is as in mcla-recomp's src/mcla_app.cpp.
#include "restart.h"

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#include "logging.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>

extern char** environ;
#endif

namespace mcla {

#if defined(_WIN32)
namespace {

// A handle to the old process on the new one's command line: a handle, not a process id, which could
// be reused. Read straight from the command line, not as an option, so it is never saved to the
// settings file.
constexpr std::wstring_view kRestartFlag = L"--larecomp_restart_wait_handle=";

// One argument quoted as CommandLineToArgvW reads it back.
std::wstring QuoteArgument(std::wstring_view arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(arg);
  }
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    // Backslashes are literal unless they precede a quote.
    out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  return out + L'"';
}

}  // namespace

void WaitForRestartedGame() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return;
  }
  HANDLE old_game = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::wstring_view(argv[i]).starts_with(kRestartFlag)) {
      old_game = reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(std::wcstoull(argv[i] + kRestartFlag.size(), nullptr, 16)));
    }
  }
  LocalFree(argv);
  if (!old_game) {
    return;
  }
  const DWORD waited = WaitForSingleObject(old_game, 30'000);
  CloseHandle(old_game);
  if (waited == WAIT_TIMEOUT) {
    MessageBoxW(nullptr,
                L"The game that was restarting is still running, so this one is not starting. "
                L"Close it, then start the game again.",
                L"LARecomp", MB_OK | MB_ICONWARNING);
    ExitProcess(1);
  }
}

bool RelaunchGame() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return false;
  }
  std::wstring command;
  for (int i = 0; i < argc; ++i) {
    if (i > 0 && std::wstring_view(argv[i]).starts_with(kRestartFlag)) {
      continue;
    }
    command += (command.empty() ? L"" : L" ") + QuoteArgument(argv[i]);
  }
  LocalFree(argv);

  HANDLE self = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                       SYNCHRONIZE, TRUE, 0)) {
    MC_ERROR("[restart] no handle for the new game to wait on (error {})", GetLastError());
    return false;
  }
  wchar_t handle_text[32];
  std::swprintf(handle_text, 32, L"%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(self)));
  command += L" " + std::wstring(kRestartFlag) + handle_text;

  SIZE_T list_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
  std::vector<uint8_t> list_storage(list_size);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
  std::wstring exe(32768, L'\0');
  exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const bool started =
      !exe.empty() && InitializeProcThreadAttributeList(list, 1, 0, &list_size) &&
      UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof(self),
                                nullptr, nullptr) &&
      (startup.lpAttributeList = list,
       CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo,
                      &process));
  const DWORD error = GetLastError();
  if (startup.lpAttributeList) {
    DeleteProcThreadAttributeList(list);
  }
  CloseHandle(self);
  if (!started) {
    MC_ERROR("[restart] could not start the game again (error {})", error);
    return false;
  }
  MC_INFO("[restart] started again as process {}", process.dwProcessId);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}
#else
// POSIX (Linux): the same hand-over with a process id in place of the handle, which the new game
// polls until the old one is gone. The command line is read back from /proc/self/cmdline.
// CHECK: not run on Linux (spawn, the wait for the old game, and under an AppImage).
namespace {

constexpr std::string_view kRestartFlag = "--larecomp_restart_wait_pid=";

// This process's arguments (argv[0] first), from /proc/self/cmdline: its arguments, each ended by a
// NUL byte.
std::vector<std::string> OwnArguments() {
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> args;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\0', start);
    args.push_back(text.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return args;
}

// The file to run again. An AppImage mounts itself for as long as it runs, so the executable's own
// path (/proc/self/exe) is gone once this game has closed; the AppImage file (APPIMAGE) is not.
std::string OwnExecutable() {
  if (const char* image = std::getenv("APPIMAGE"); image && *image) {
    return image;
  }
  char path[4096];
  const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
  return length > 0 ? std::string(path, size_t(length)) : std::string();
}

// Whether the process still runs. One that has ended but not been collected by its parent yet (a
// zombie) does not count: it holds nothing the new game needs.
bool ProcessRunning(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!in) {
    // No /proc to ask (or no such process): signal 0 only asks whether the process exists.
    return kill(pid, 0) == 0 || errno == EPERM;
  }
  std::getline(in, line);
  // "pid (name) state ...": the name may hold spaces and brackets, so the state follows the last ")".
  const size_t close = line.rfind(')');
  if (close == std::string::npos || close + 2 >= line.size()) {
    return true;
  }
  const char state = line[close + 2];
  return state != 'Z' && state != 'X';
}

}  // namespace

void WaitForRestartedGame() {
  pid_t old_game = 0;
  for (const std::string& arg : OwnArguments()) {
    if (std::string_view(arg).starts_with(kRestartFlag)) {
      old_game = pid_t(std::strtol(arg.c_str() + kRestartFlag.size(), nullptr, 10));
    }
  }
  if (old_game <= 0) {
    return;
  }
  for (int waited_ms = 0; ProcessRunning(old_game); waited_ms += 50) {
    if (waited_ms >= 30'000) {
      std::fputs("LARecomp: the game that was restarting is still running, so this one is not "
                 "starting. Close it, then start the game again.\n",
                 stderr);
      std::_Exit(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

bool RelaunchGame() {
  const std::string exe = OwnExecutable();
  if (exe.empty()) {
    MC_ERROR("[restart] the game's own file is not known, so it cannot be started again");
    return false;
  }
  std::vector<std::string> command{exe};
  const std::vector<std::string> args = OwnArguments();
  for (size_t i = 1; i < args.size(); ++i) {
    if (!std::string_view(args[i]).starts_with(kRestartFlag)) {
      command.push_back(args[i]);
    }
  }
  command.push_back(std::string(kRestartFlag) + std::to_string(getpid()));
  std::vector<char*> argv;
  for (std::string& arg : command) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  pid_t child = 0;
  const int error = posix_spawn(&child, exe.c_str(), nullptr, nullptr, argv.data(), environ);
  if (error != 0) {
    MC_ERROR("[restart] could not start the game again (error {})", error);
    return false;
  }
  MC_INFO("[restart] started again as process {}", int(child));
  return true;
}
#endif

}  // namespace mcla

// RESTART GAME: see restart.h. The Windows hand-over is as in mcla-recomp's src/mcla_app.cpp.
#include "restart.h"

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#include "logging.h"
#include "os/appimage_env.h"

#if defined(_WIN32)
#include "restart_handoff_win.h"

#include <memory>
#include <shellapi.h>
#else
#include "restart_handoff_linux.h"

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

extern char** environ;
#endif

namespace mcla {

#if defined(_WIN32)
namespace {

using restart::HandoffStatus;
using restart::kRestartFlag;

[[noreturn]] void RejectRestart(const std::wstring& message) {
  MessageBoxW(nullptr, message.c_str(), L"LARecomp", MB_OK | MB_ICONWARNING);
  ExitProcess(1);
}

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
    const DWORD error = GetLastError();
    RejectRestart(L"Windows could not read the game's command line (error " +
                  std::to_wstring(error) + L"). Close any running game, then start it again.");
  }
  const auto result = restart::WaitForHandoff({argv, static_cast<size_t>(argc)}, 30'000);
  LocalFree(argv);

  // CHECK: verify Windows startup and parent exit ordering via native Restart Game.
  switch (result.status) {
    case HandoffStatus::Ready:
      return;
    case HandoffStatus::MalformedToken:
      RejectRestart(L"The restart command has an invalid handoff token. "
                    L"Close any running game, then start the game normally.");
    case HandoffStatus::DuplicateToken:
      RejectRestart(L"The restart command has more than one handoff token. "
                    L"Close any running game, then start the game normally.");
    case HandoffStatus::TimedOut:
      RejectRestart(L"The game that was restarting is still running, so this one is not starting. "
                    L"Close it, then start the game again.");
    case HandoffStatus::WaitFailed:
      RejectRestart(L"Windows could not wait for the previous game to exit (error " +
                    std::to_wstring(result.detail) + L"). Close it, then start the game again.");
    case HandoffStatus::UnexpectedWait:
      RejectRestart(L"Windows returned an unexpected result while waiting for the previous game "
                    L"to exit (result " + std::to_wstring(result.detail) +
                    L"). Close it, then start the game again.");
  }
  RejectRestart(L"The restart handoff could not be verified. "
                L"Close any running game, then start the game again.");
}

bool RelaunchGame() {
  const auto spawn_failed = [](DWORD error) {
    MC_ERROR("[restart] could not start the game again (error {})", error);
    return false;
  };
  int argc = 0;
  std::unique_ptr<LPWSTR, decltype(&LocalFree)> argv(
      CommandLineToArgvW(GetCommandLineW(), &argc), &LocalFree);
  if (!argv) {
    return spawn_failed(GetLastError());
  }
  std::wstring command;
  for (int i = 0; i < argc; ++i) {
    const std::wstring_view argument(argv.get()[i]);
    if (i > 0 && argument.starts_with(kRestartFlag)) {
      continue;
    }
    command += (command.empty() ? L"" : L" ") + QuoteArgument(argument);
  }
  argv.reset();

  HANDLE self = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                       SYNCHRONIZE, TRUE, 0)) {
    const DWORD error = GetLastError();
    MC_ERROR("[restart] no handle for the new game to wait on (error {})", error);
    return false;
  }
  const std::unique_ptr<void, decltype(&CloseHandle)> self_handle(self, &CloseHandle);
  wchar_t handle_text[32];
  std::swprintf(handle_text, 32, L"%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(self)));
  command += L" " + std::wstring(kRestartFlag) + handle_text;

  SIZE_T list_size = 0;
  // A null-list probe is expected to fail and return the required buffer size.
  InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
  const DWORD sizing_error = GetLastError();
  if (sizing_error != ERROR_INSUFFICIENT_BUFFER || list_size == 0) {
    return spawn_failed(sizing_error);
  }
  std::wstring exe(32768, L'\0');
  const DWORD exe_length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
  if (exe_length == 0 || exe_length >= exe.size()) {
    return spawn_failed(GetLastError());
  }
  exe.resize(exe_length);

  std::vector<uint8_t> list_storage(list_size);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &list_size)) {
    return spawn_failed(GetLastError());
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = list;
  PROCESS_INFORMATION process{};
  // CHECK: verify native update failure and retry in the disposable Windows restart rehearsal.
  const bool started =
      UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof(self),
                                nullptr, nullptr) &&
      CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                     EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
  const DWORD error = started ? ERROR_SUCCESS : GetLastError();
  // An initialized list must be deleted even if its update failed.
  DeleteProcThreadAttributeList(list);
  if (!started) {
    return spawn_failed(error);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  MC_INFO("[restart] started again as process {}", process.dwProcessId);
  return true;
}
#else
namespace {

namespace handoff = restart::linux_handoff;

handoff::ProcRead ReadProcFile(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {{}, errno};
  }
  handoff::ProcRead result;
  char buffer[4096];
  for (;;) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count == 0) {
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.error = errno;
      break;
    }
    result.text.append(buffer, static_cast<std::size_t>(count));
  }
  close(fd);
  return result;
}

std::optional<std::vector<std::string>> OwnArguments() {
  const auto cmdline = ReadProcFile("/proc/self/cmdline");
  return cmdline.error == 0 ? handoff::DecodeArguments(cmdline.text) : std::nullopt;
}

handoff::ProcessResult ProbeProcess(handoff::ProcessIdentity expected) {
  if (expected.pid == getpid()) {
    return {handoff::ProcessState::IdentityMismatch};
  }
  return handoff::InspectProcess(
      expected, ReadProcFile("/proc/" + std::to_string(expected.pid) + "/stat"),
      [](int pid) { return kill(pid, 0) == 0 ? 0 : errno; });
}

[[noreturn]] void RejectRestart(const char* message) {
  std::fputs(message, stderr);
  std::fflush(stderr);
  std::_Exit(1);
}

// The file to run again: the game itself. When the game is an AppImage (its own file is inside its own
// $APPDIR) the mount goes away when this game closes, so the executable's own path (/proc/self/exe) is
// gone by then and the AppImage file is what to start. A game the launcher started is not that: the
// launcher's AppImage runtime left its APPIMAGE in the environment, which names the LAUNCHER, so it is
// not read here (os/appimage_env.h; the app removes those variables at start as well).
std::string OwnExecutable() {
  if (std::string image = larecomp::os::OwnAppImageFile(); !image.empty()) {
    return image;
  }
  char path[4096];
  const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (length <= 0) {
    return {};
  }
  std::string exe(path, size_t(length));
  // The kernel adds " (deleted)" to the path of a file that was replaced while it ran (the launcher's
  // update): the game to start again is whatever stands at that path now.
  constexpr std::string_view kDeleted = " (deleted)";
  if (std::string_view(exe).ends_with(kDeleted)) {
    exe.resize(exe.size() - kDeleted.size());
  }
  return exe;
}

}  // namespace

void WaitForRestartedGame() {
  const auto args = OwnArguments();
  if (!args) {
    RejectRestart("LARecomp: could not read the game's command line. "
                  "Close any running game, then start it again.\n");
  }
  const auto result = handoff::WaitForHandoff(
      *args, ProbeProcess, std::chrono::steady_clock::now,
      [](std::chrono::steady_clock::duration delay) { std::this_thread::sleep_for(delay); });
  // CHECK: capture native Linux and AppImage argv and prior exit ordering through Restart Game.
  if (result.CanStart()) {
    return;
  }
  switch (result.status) {
    case handoff::HandoffStatus::MalformedToken:
      RejectRestart("LARecomp: the restart command has an invalid handoff token. "
                    "Close any running game, then start the game normally.\n");
    case handoff::HandoffStatus::DuplicateToken:
      RejectRestart("LARecomp: the restart command has more than one handoff token. "
                    "Close any running game, then start the game normally.\n");
    case handoff::HandoffStatus::IdentityMismatch:
      RejectRestart("LARecomp: the previous game's process identity did not match the restart "
                    "token. Close any running game, then start the game normally.\n");
    case handoff::HandoffStatus::TimedOut:
      RejectRestart("LARecomp: the game that was restarting is still running, so this one is not "
                    "starting. Close it, then start the game again.\n");
    case handoff::HandoffStatus::CheckFailed:
      std::fprintf(stderr, "LARecomp: could not verify the previous game's process (error %d).\n",
                   result.error);
      break;
    case handoff::HandoffStatus::NoHandoff:
    case handoff::HandoffStatus::Exited:
      break;
  }
  RejectRestart("LARecomp: close any running game, then start the game normally.\n");
}

bool RelaunchGame() {
  const std::string exe = OwnExecutable();
  if (exe.empty()) {
    MC_ERROR("[restart] the game's own file is not known, so it cannot be started again");
    return false;
  }
  const auto args = OwnArguments();
  if (!args) {
    MC_ERROR("[restart] could not read the game's command line");
    return false;
  }
  const auto self_stat = ReadProcFile("/proc/self/stat");
  const auto self = self_stat.error == 0 ? handoff::ParseProcessStat(self_stat.text) : std::nullopt;
  if (!self || self->identity.pid != getpid()) {
    MC_ERROR("[restart] could not verify the game's own process identity");
    return false;
  }

  // CHECK: exercise posix_spawn with the native ELF and the game's own AppImage runtime.
  const auto spawned =
      handoff::SpawnReplacement(exe, *args, self->identity, [](const char* path, char* const* argv) {
        pid_t child = 0;
        const int error = posix_spawn(&child, path, nullptr, nullptr, argv, environ);
        return handoff::SpawnResult{child, error};
      });
  if (!spawned) {
    MC_ERROR("[restart] could not start the game again (error {})", spawned.error);
    return false;
  }
  MC_INFO("[restart] started again as process {}", spawned.pid);
  return true;
}
#endif

}  // namespace mcla

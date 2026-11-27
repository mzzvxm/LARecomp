#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace mcla::restart {

// RelaunchGame passes an inherited process handle, whose lifetime is independent of PID reuse.
inline constexpr std::wstring_view kRestartFlag = L"--larecomp_restart_wait_handle=";

enum class HandoffStatus {
  Ready,
  MalformedToken,
  DuplicateToken,
  TimedOut,
  WaitFailed,
  UnexpectedWait,
};

struct HandoffResult {
  HandoffStatus status;
  DWORD detail = 0;
};

inline HandoffResult WaitForHandoff(std::span<const wchar_t* const> args, DWORD timeout_ms) {
  constexpr auto kBareFlag = kRestartFlag.substr(0, kRestartFlag.size() - 1);
  std::wstring_view token;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (!args[i]) {
      continue;
    }
    const std::wstring_view arg(args[i]);
    if (arg != kBareFlag && !arg.starts_with(kRestartFlag)) {
      continue;
    }
    if (!token.empty()) {
      return {HandoffStatus::DuplicateToken};
    }
    token = arg;
  }
  if (token.empty()) {
    return {HandoffStatus::Ready};
  }

  // RelaunchGame emits plain hexadecimal digits. Consume every digit without truncation.
  std::uintptr_t value = 0;
  if (token.starts_with(kRestartFlag)) {
    for (wchar_t c : token.substr(kRestartFlag.size())) {
      if (c >= L'A' && c <= L'F') {
        c += L'a' - L'A';
      }
      const std::size_t digit = std::wstring_view(L"0123456789abcdef").find(c);
      if (digit == std::wstring_view::npos || value > (UINTPTR_MAX - digit) / 16) {
        return {HandoffStatus::MalformedToken};
      }
      value = value * 16 + digit;
    }
  }
  if (value == 0 || value == UINTPTR_MAX) {
    return {HandoffStatus::MalformedToken};
  }

  // Consume the inherited handle without changing its access rights. DuplicateHandle returns an
  // error for a stale token even with strict handle checking, which can raise on a direct wait.
  const HANDLE self = GetCurrentProcess();
  HANDLE old_game = nullptr;
  if (!DuplicateHandle(self, reinterpret_cast<HANDLE>(value), self, &old_game, 0, FALSE,
                       DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) {
    return {HandoffStatus::WaitFailed, GetLastError()};
  }
  const DWORD waited = WaitForSingleObject(old_game, timeout_ms);
  const DWORD error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
  CloseHandle(old_game);
  if (waited == WAIT_OBJECT_0) {
    return {HandoffStatus::Ready};
  }
  if (waited == WAIT_TIMEOUT) {
    return {HandoffStatus::TimedOut};
  }
  if (waited == WAIT_FAILED) {
    return {HandoffStatus::WaitFailed, error};
  }
  return {HandoffStatus::UnexpectedWait, waited};
}

}  // namespace mcla::restart

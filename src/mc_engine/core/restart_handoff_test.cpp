#include "restart_handoff_win.h"

#include <cstdio>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <string>
#include <thread>

namespace {

using mcla::restart::HandoffResult;
using mcla::restart::HandoffStatus;
using mcla::restart::WaitForHandoff;
using mcla::restart::kRestartFlag;

constexpr wchar_t kExe[] = L"C:\\Test Profile\\larecomp.exe";
int checks = 0;
int failed = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    ++checks;                                                                   \
    if (!(condition)) {                                                         \
      ++failed;                                                                 \
      std::printf("FAIL %d: %s\n", __LINE__, #condition);                        \
    }                                                                           \
  } while (0)

HandoffResult TestHandoff(std::initializer_list<const wchar_t*> args) {
  return WaitForHandoff({args.begin(), args.size()}, 0);
}

std::wstring Token(HANDLE handle) {
  wchar_t digits[2 * sizeof(std::uintptr_t) + 1];
  std::swprintf(digits, std::size(digits), L"%llx",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle)));
  return std::wstring(kRestartFlag) + digits;
}

void CheckTokens() {
  CHECK(TestHandoff({kExe}).status == HandoffStatus::Ready);
  CHECK(TestHandoff({kExe, L"--user_data_root=C:\\Test Profile", L"--kernel.user_name=Test Driver",
                     L"--larecomp_restart_wait_handle_extra=1"})
            .status == HandoffStatus::Ready);
  CHECK(TestHandoff({L"--larecomp_restart_wait_handle=bad"}).status == HandoffStatus::Ready);
  CHECK(TestHandoff({kExe, L"--larecomp_restart_wait_handle"}).status ==
        HandoffStatus::MalformedToken);

  const auto overflow = L"1" + std::wstring(2 * sizeof(std::uintptr_t) - 1, L'0') + L"1";
  const std::wstring invalid_handle(2 * sizeof(std::uintptr_t), L'f');
  const std::wstring_view invalid_suffixes[] = {
      L"", L"0", L"+24", L"-24", L"0x24", L" 24", L"24 ", L"24junk", L"24=8", L"\uff12\uff14",
      overflow, invalid_handle,
  };
  for (const auto suffix : invalid_suffixes) {
    const std::wstring arg = std::wstring(kRestartFlag) + std::wstring(suffix);
    CHECK(TestHandoff({kExe, arg.c_str()}).status == HandoffStatus::MalformedToken);
  }

  CHECK(TestHandoff({kExe, L"--larecomp_restart_wait_handle=24",
                     L"--larecomp_restart_wait_handle=24"})
            .status == HandoffStatus::DuplicateToken);
  CHECK(TestHandoff({kExe, L"--larecomp_restart_wait_handle=24",
                     L"--larecomp_restart_wait_handle"})
            .status == HandoffStatus::DuplicateToken);
  CHECK(TestHandoff({kExe, L"--larecomp_restart_wait_handle=",
                     L"--larecomp_restart_wait_handle=24"})
            .status == HandoffStatus::DuplicateToken);
}

void CheckWait(HANDLE handle, HandoffStatus expected, DWORD expected_detail = 0) {
  CHECK(handle != nullptr);
  if (!handle) {
    return;
  }
  const std::wstring token = Token(handle);
  const HANDLE self = GetCurrentProcess();
  DWORD handles_before = 0;
  CHECK(GetProcessHandleCount(self, &handles_before));
  const auto result = TestHandoff({kExe, L"--kernel.user_name=Test Driver", token.c_str()});
  CHECK(result.status == expected);
  CHECK(result.detail == expected_detail);
  DWORD handles_after = 0;
  CHECK(GetProcessHandleCount(self, &handles_after));
  CHECK(handles_before == handles_after + 1);
  HANDLE probe = nullptr;
  const bool still_open =
      DuplicateHandle(self, handle, self, &probe, 0, FALSE, DUPLICATE_SAME_ACCESS) != 0;
  CHECK(!still_open);
  if (still_open) {
    CloseHandle(probe);
    CloseHandle(handle);
  }
}

}  // namespace

int main() {
  CheckTokens();

  // Events give deterministic signaled and unsignaled waits with a zero test timeout.
  CheckWait(CreateEventW(nullptr, TRUE, TRUE, nullptr), HandoffStatus::Ready);
  CheckWait(CreateEventW(nullptr, TRUE, FALSE, nullptr), HandoffStatus::TimedOut);

  const HANDLE self = GetCurrentProcess();
  HANDLE synchronize_only = nullptr;
  CHECK(DuplicateHandle(self, self, self, &synchronize_only, SYNCHRONIZE, TRUE, 0));
  CheckWait(synchronize_only, HandoffStatus::TimedOut);

  // This process handle is valid and can be closed, but Windows denies the wait.
  HANDLE no_synchronize = nullptr;
  CHECK(DuplicateHandle(self, self, self, &no_synchronize, PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, 0));
  CheckWait(no_synchronize, HandoffStatus::WaitFailed, ERROR_ACCESS_DENIED);

  const HANDLE stale = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  CHECK(stale != nullptr);
  if (stale) {
    const std::wstring token = Token(stale);
    CHECK(CloseHandle(stale));
    const auto result = TestHandoff({kExe, token.c_str()});
    CHECK(result.status == HandoffStatus::WaitFailed);
    CHECK(result.detail == ERROR_INVALID_HANDLE);
  }

  const HANDLE mutex = CreateMutexW(nullptr, FALSE, nullptr);
  CHECK(mutex != nullptr);
  if (mutex) {
    DWORD acquired = WAIT_FAILED;
    std::thread owner([&] { acquired = WaitForSingleObject(mutex, 0); });
    owner.join();
    CHECK(acquired == WAIT_OBJECT_0);
    CheckWait(mutex, HandoffStatus::UnexpectedWait, WAIT_ABANDONED);
  }

  std::printf("%d checks, %d failed\n", checks, failed);
  return failed != 0;
}

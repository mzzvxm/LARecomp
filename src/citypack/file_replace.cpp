#include "citypack/file_replace.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace larecomp::citypack {

namespace fs = std::filesystem;

namespace {

uint32_t ProcessId() {
#if defined(_WIN32)
  return uint32_t(GetCurrentProcessId());
#else
  return uint32_t(getpid());
#endif
}

// One real try to move the file over the target (replacing it).
MoveError RealTryMove(const fs::path& from, const fs::path& to, std::string* why) {
#if defined(_WIN32)
  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) return MoveError::kNone;
  const DWORD error = GetLastError();
  if (why) *why = std::system_category().message(int(error)) + " (error " + std::to_string(unsigned(error)) + ")";
  return (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) ? MoveError::kBusy : MoveError::kOther;
#else
  std::error_code ec;
  fs::rename(from, to, ec);  // replaces an existing file atomically
  if (!ec) return MoveError::kNone;
  if (why) *why = ec.message();
  return (ec == std::errc::device_or_resource_busy || ec == std::errc::text_file_busy) ? MoveError::kBusy : MoveError::kOther;
#endif
}

void RealWait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

bool IsTempNameOf(const std::string& name, const std::string& base) {
  constexpr size_t kSuffix = 4;  // ".tmp"
  if (base.empty() || name.size() < base.size() + kSuffix) return false;
  if (name.compare(0, base.size(), base) != 0) return false;
  if (name.compare(name.size() - kSuffix, kSuffix, ".tmp") != 0) return false;
  return name.size() == base.size() + kSuffix || name[base.size()] == '.';
}

int SweepStaleTemps(const fs::path& target, std::chrono::seconds older_than) {
  const fs::path folder = target.has_parent_path() ? target.parent_path() : fs::path(".");
  const std::string base = target.filename().string();
  std::error_code ec;
  int removed = 0;
  const auto now = fs::file_time_type::clock::now();
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || entry_ec) continue;
    if (!IsTempNameOf(it->path().filename().string(), base)) continue;
    const auto written = fs::last_write_time(it->path(), entry_ec);
    if (entry_ec) continue;
    if (now - written < older_than) continue;
    std::error_code rm;
    if (fs::remove(it->path(), rm) && !rm) ++removed;
  }
  return removed;
}

fs::path TempPathFor(const fs::path& target) {
  static std::atomic<uint32_t> counter{0};
  (void)SweepStaleTemps(target, std::chrono::hours(2));
  const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  fs::path temp = target;  // += keeps the wide characters of a Windows path (string() would go through the ANSI code page)
  temp += "." + std::to_string(ProcessId()) + "." + std::to_string(n) + ".tmp";
  return temp;
}

bool ShouldRetryMove(MoveError error, int attempts_made, int max_attempts) { return error == MoveError::kBusy && attempts_made < max_attempts; }

bool SameFileContent(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  const uintmax_t size_a = fs::file_size(a, ec);
  if (ec) return false;
  const uintmax_t size_b = fs::file_size(b, ec);
  if (ec || size_a != size_b) return false;
  std::ifstream in_a(a, std::ios::binary);
  std::ifstream in_b(b, std::ios::binary);
  if (!in_a || !in_b) return false;
  constexpr size_t kChunk = size_t(1) << 20;
  std::vector<char> buffer_a(kChunk);
  std::vector<char> buffer_b(kChunk);
  uintmax_t left = size_a;
  while (left != 0) {
    const size_t chunk = size_t(std::min<uintmax_t>(left, kChunk));
    in_a.read(buffer_a.data(), std::streamsize(chunk));
    in_b.read(buffer_b.data(), std::streamsize(chunk));
    if (size_t(in_a.gcount()) != chunk || size_t(in_b.gcount()) != chunk) return false;
    if (!std::equal(buffer_a.begin(), buffer_a.begin() + std::ptrdiff_t(chunk), buffer_b.begin())) return false;
    left -= chunk;
  }
  return true;
}

MoveResult MoveIntoPlace(const fs::path& temp, const fs::path& target, std::string* why, const MoveOptions& options, const MoveHooks& hooks, int* attempts) {
  const int max_attempts = std::max(1, options.max_attempts);
  std::string last;
  int made = 0;
  for (;;) {
    ++made;
    last.clear();
    const MoveError error = hooks.try_move ? hooks.try_move(temp, target, &last) : RealTryMove(temp, target, &last);
    if (error == MoveError::kNone) {
      if (attempts) *attempts = made;
      return MoveResult::kMoved;
    }
    if (!ShouldRetryMove(error, made, max_attempts)) break;
    const int ms = std::max(0, options.retry_ms) * made;
    if (hooks.wait) {
      hooks.wait(ms);
    } else {
      RealWait(ms);
    }
  }
  if (attempts) *attempts = made;
  // The target stayed out of reach. When it already holds these exact bytes, another process wrote it first and nothing is left to do.
  const bool identical = SameFileContent(temp, target);
  std::error_code ec;
  fs::remove(temp, ec);
  if (identical) return MoveResult::kAlreadyThere;
  if (why) *why = last.empty() ? std::string("the file in place is open elsewhere and holds other bytes") : last + "; the file in place holds other bytes";
  return MoveResult::kFailed;
}

}  // namespace larecomp::citypack

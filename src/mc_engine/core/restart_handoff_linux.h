#pragma once

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::restart::linux_handoff {

// The spawning game sends PID:start_ticks using its starttime from /proc/self/stat.
inline constexpr std::string_view kRestartFlag = "--larecomp_restart_wait_pid=";
inline constexpr auto kBareFlag = kRestartFlag.substr(0, kRestartFlag.size() - 1);

inline bool IsRestartArgument(std::string_view arg) {
  return arg == kBareFlag || arg.starts_with(kRestartFlag);
}

// /proc/self/cmdline contains one NUL per argument, including empty arguments.
inline std::optional<std::vector<std::string>> DecodeArguments(std::string_view bytes) {
  if (bytes.empty() || bytes.back() != '\0') {
    return std::nullopt;
  }
  std::vector<std::string> args;
  while (!bytes.empty()) {
    const auto end = bytes.find('\0');
    args.emplace_back(bytes.substr(0, end));
    bytes.remove_prefix(end + 1);
  }
  return args;
}

struct ProcessIdentity {
  int pid;
  std::uint64_t start_ticks;
};

inline bool ReadDecimal(std::string_view text, auto& value) {
  if (text.empty()) {
    return false;
  }
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value, 10);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

inline bool ReadPositiveDecimal(std::string_view text, auto& value) {
  return ReadDecimal(text, value) && value > 0;
}

struct ProcessStat {
  ProcessIdentity identity;
  char state;
  unsigned threads;
};

inline std::optional<ProcessStat> ParseProcessStat(std::string_view text) {
  // The parenthesized comm field can contain spaces, newlines and parentheses.
  const auto open = text.find(" (");
  const auto close = text.rfind(')');
  ProcessStat stat{};
  if (open == text.npos || close == text.npos || close < open + 2 ||
      !ReadPositiveDecimal(text.substr(0, open), stat.identity.pid)) {
    return std::nullopt;
  }
  text.remove_prefix(close + 1);
  if (text.size() < 3 || text[0] != ' ' || text[2] != ' ' ||
      std::string_view("RSDZTtXxKWPI").find(text[1]) == std::string_view::npos) {
    return std::nullopt;
  }
  stat.state = text[1];
  text.remove_prefix(3);
  // Field 3 was state. Read num_threads (20) on the way to starttime (22).
  for (int field = 4; field < 22; ++field) {
    const auto end = text.find(' ');
    if (end == text.npos || end == 0 ||
        (field == 20 && !ReadDecimal(text.substr(0, end), stat.threads))) {
      return std::nullopt;
    }
    text.remove_prefix(end + 1);
  }
  const auto end = text.find(' ');
  if (end == text.npos || !ReadPositiveDecimal(text.substr(0, end), stat.identity.start_ticks)) {
    return std::nullopt;
  }
  return stat;
}

struct ProcRead {
  std::string text;
  int error = 0;
};

enum class ProcessState { Running, Exited, IdentityMismatch, CheckFailed };

struct ProcessResult {
  ProcessState state;
  int error = 0;
};

inline ProcessResult InspectProcess(ProcessIdentity expected, const ProcRead& observation,
                                    auto signal_zero) {
  if (observation.error == ENOENT || observation.error == ESRCH) {
    // A missing procfs entry can also mean hidepid or a missing mount. Confirm absence in the
    // PID namespace. Success or EPERM from signal zero means we have no verified identity.
    const int error = signal_zero(expected.pid);
    return error == ESRCH ? ProcessResult{ProcessState::Exited}
                          : ProcessResult{ProcessState::CheckFailed,
                                          error == 0 ? observation.error : error};
  }
  if (observation.error != 0) {
    return {ProcessState::CheckFailed, observation.error};
  }
  const auto stat = ParseProcessStat(observation.text);
  if (!stat) {
    return {ProcessState::CheckFailed, EINVAL};
  }
  if (stat->identity.pid != expected.pid || stat->identity.start_ticks != expected.start_ticks) {
    return {ProcessState::IdentityMismatch};
  }
  const char state = stat->state;
  const bool dead = state == 'Z' || state == 'X' || state == 'x';
  // A zombie thread-group leader can still have live sibling threads.
  return {dead && stat->threads <= 1 ? ProcessState::Exited : ProcessState::Running};
}

enum class HandoffStatus {
  NoHandoff,
  Exited,
  MalformedToken,
  DuplicateToken,
  IdentityMismatch,
  CheckFailed,
  TimedOut,
};

struct HandoffResult {
  HandoffStatus status;
  int error = 0;

  bool CanStart() const {
    return status == HandoffStatus::NoHandoff || status == HandoffStatus::Exited;
  }
};

inline HandoffResult WaitForHandoff(std::span<const std::string> args, auto probe, auto now,
                                   auto sleep) {
  std::string_view token;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (IsRestartArgument(arg)) {
      if (!token.empty()) {
        return {HandoffStatus::DuplicateToken};
      }
      token = arg;
    }
  }
  if (token.empty()) {
    return {HandoffStatus::NoHandoff};
  }
  if (!token.starts_with(kRestartFlag)) {
    return {HandoffStatus::MalformedToken};
  }
  token.remove_prefix(kRestartFlag.size());
  const auto separator = token.find(':');
  ProcessIdentity expected{};
  if (separator == token.npos || !ReadPositiveDecimal(token.substr(0, separator), expected.pid) ||
      !ReadPositiveDecimal(token.substr(separator + 1), expected.start_ticks)) {
    return {HandoffStatus::MalformedToken};
  }

  const auto deadline = now() + std::chrono::seconds(30);
  constexpr auto interval = std::chrono::milliseconds(50);
  while (now() < deadline) {
    const ProcessResult result = probe(expected);
    const auto observed = now();
    if (observed >= deadline) {
      break;
    }
    switch (result.state) {
      case ProcessState::Exited:
        return {HandoffStatus::Exited};
      case ProcessState::IdentityMismatch:
        return {HandoffStatus::IdentityMismatch};
      case ProcessState::CheckFailed:
        return {HandoffStatus::CheckFailed, result.error};
      case ProcessState::Running:
        sleep(std::min(deadline - observed,
                       std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval)));
        break;
    }
  }
  return {HandoffStatus::TimedOut};
}

struct SpawnResult {
  int pid;
  int error;

  explicit operator bool() const { return error == 0 && pid > 0; }
};

inline SpawnResult SpawnReplacement(const std::string& exe, std::span<const std::string> args,
                                    ProcessIdentity self, auto spawn) {
  std::vector<std::string> command{exe};
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (!IsRestartArgument(args[i])) {
      command.push_back(args[i]);
    }
  }
  command.push_back(std::string(kRestartFlag) + std::to_string(self.pid) + ":" +
                    std::to_string(self.start_ticks));

  std::vector<char*> argv;
  for (std::string& arg : command) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return spawn(exe.c_str(), argv.data());
}

}  // namespace mcla::restart::linux_handoff

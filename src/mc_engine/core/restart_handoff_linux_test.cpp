#include "restart_handoff_linux.h"

#include <cstdio>
#include <initializer_list>
#include <limits>

namespace {

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;
namespace handoff = mcla::restart::linux_handoff;
using handoff::HandoffStatus;
using handoff::ProcessResult;
using handoff::ProcessState;

constexpr char kExe[] = "/candidate/larecomp";
constexpr handoff::ProcessIdentity kPrevious{412, 85015};
constexpr char kToken[] = "--larecomp_restart_wait_pid=412:85015";

int checks = 0;
int failed = 0;

#define CHECK(condition)                                  \
  do {                                                   \
    ++checks;                                            \
    if (!(condition)) {                                  \
      ++failed;                                          \
      std::printf("FAIL %d: %s\n", __LINE__, #condition); \
    }                                                    \
  } while (0)

std::string Stat(char state, int pid = kPrevious.pid,
                 std::uint64_t start_ticks = kPrevious.start_ticks, unsigned threads = 1) {
  return std::to_string(pid) + " (name) with (spaces\nand a newline) " + state +
         " 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 " + std::to_string(threads) +
         " 21 " + std::to_string(start_ticks) + " 23 24\n";
}

struct WaitFixture {
  using Clock = std::chrono::steady_clock;
  std::vector<ProcessResult> states{{ProcessState::Exited}};
  Clock::time_point current{};
  Clock::duration probe_cost{};
  Clock::duration oversleep{};
  std::size_t probes = 0;
  std::size_t sleeps = 0;
  handoff::ProcessIdentity observed{};

  handoff::HandoffResult Run(std::initializer_list<std::string> args) {
    return handoff::WaitForHandoff(
        {args.begin(), args.size()},
        [&](auto identity) {
          observed = identity;
          current += probe_cost;
          return states[std::min(probes++, states.size() - 1)];
        },
        [&] { return current; },
        [&](auto delay) {
          ++sleeps;
          current += delay + oversleep;
        });
  }
};

void CheckArguments() {
  const auto args =
      handoff::DecodeArguments("/a path/larecomp\0--user_data_root=/profile space\0\0one\ntwo\0"sv);
  CHECK(args.has_value());
  if (args) {
    CHECK((*args == std::vector<std::string>{
                        "/a path/larecomp", "--user_data_root=/profile space", "", "one\ntwo"}));
  }
  CHECK(!handoff::DecodeArguments(""sv));
  CHECK(!handoff::DecodeArguments("/candidate/larecomp"sv));
  CHECK(!handoff::DecodeArguments("/candidate/larecomp\0--larecomp_restart_wait_pid=412"sv));

  WaitFixture wait;
  const auto ordinary =
      wait.Run({kExe, "--user_data_root=/profile space", "--show_debug_menu=true",
                "--larecomp_restart_wait_pid_extra=1", "contains --larecomp_restart_wait_pid=0"});
  CHECK(ordinary.status == HandoffStatus::NoHandoff);
  CHECK(ordinary.CanStart());
  CHECK(wait.Run({kToken}).status == HandoffStatus::NoHandoff);
  CHECK(wait.probes == 0);

  const auto bare = wait.Run({kExe, std::string(handoff::kBareFlag), "412:85015"});
  CHECK(bare.status == HandoffStatus::MalformedToken);
  CHECK(!bare.CanStart());
  const std::string too_large_pid = std::to_string(std::numeric_limits<int>::max()) + "0:85015";
  for (const std::string_view suffix :
       {"", "0", "412", "0:85015", "412:0", ":85015", "412:", "+412:85015", "-412:85015",
        " 412:85015", "412:+85015", "412:-85015", "412:85015junk", "412junk:85015",
        "412:85015 ", "412:85015:1", "412:18446744073709551616"}) {
    const auto result = wait.Run({kExe, std::string(handoff::kRestartFlag) + std::string(suffix)});
    CHECK(result.status == HandoffStatus::MalformedToken);
    CHECK(!result.CanStart());
  }
  CHECK(wait.Run({kExe, std::string(handoff::kRestartFlag) + too_large_pid}).status ==
        HandoffStatus::MalformedToken);
  for (const auto args : {std::initializer_list<std::string>{kExe, kToken, kToken},
                          {kExe, kToken, "--larecomp_restart_wait_pid=0"},
                          {kExe, "--larecomp_restart_wait_pid", kToken}}) {
    const auto result = wait.Run(args);
    CHECK(result.status == HandoffStatus::DuplicateToken);
    CHECK(!result.CanStart());
  }
  CHECK(wait.probes == 0);
}

void CheckProcessStatus() {
  const auto stat = handoff::ParseProcessStat(Stat('Z'));
  CHECK(stat.has_value());
  if (stat) {
    CHECK(stat->identity.pid == kPrevious.pid);
    CHECK(stat->identity.start_ticks == kPrevious.start_ticks);
    CHECK(stat->state == 'Z');
  }

  int signals = 0;
  int signal_result = ESRCH;
  const auto signal_zero = [&](int pid) {
    ++signals;
    CHECK(pid == kPrevious.pid);
    return signal_result;
  };
  for (const char state : "ZXSRTtIDP"sv) {
    const auto result = handoff::InspectProcess(kPrevious, {Stat(state)}, signal_zero);
    CHECK(result.state == (state == 'Z' || state == 'X' ? ProcessState::Exited
                                                       : ProcessState::Running));
  }
  CHECK(handoff::InspectProcess(kPrevious, {Stat('Z', 412, 85015, 2)}, signal_zero).state ==
        ProcessState::Running);
  CHECK(handoff::InspectProcess(kPrevious, {Stat('Z', kPrevious.pid, 85016)}, signal_zero).state ==
        ProcessState::IdentityMismatch);
  CHECK(handoff::InspectProcess(kPrevious, {Stat('Z', kPrevious.pid + 1)}, signal_zero).state ==
        ProcessState::IdentityMismatch);
  for (const std::string& text :
       {std::string{}, "412 (broken) Z 1 2 "s, Stat('?'), Stat('Z', 412, 0)}) {
    const auto result = handoff::InspectProcess(kPrevious, {text}, signal_zero);
    CHECK(result.state == ProcessState::CheckFailed);
    CHECK(result.error == EINVAL);
  }
  for (const int error : {EACCES, EIO}) {
    const auto result = handoff::InspectProcess(kPrevious, {Stat('Z'), error}, signal_zero);
    CHECK(result.state == ProcessState::CheckFailed);
    CHECK(result.error == error);
  }
  CHECK(signals == 0);
  for (const int error : {ENOENT, ESRCH}) {
    CHECK(handoff::InspectProcess(kPrevious, {{}, error}, signal_zero).state == ProcessState::Exited);
  }
  for (const int error : {0, EPERM, EINVAL}) {
    signal_result = error;
    const auto result = handoff::InspectProcess(kPrevious, {{}, ENOENT}, signal_zero);
    CHECK(result.state == ProcessState::CheckFailed);
    CHECK(result.error == (error == 0 ? ENOENT : error));
  }
  CHECK(signals == 5);
}

void CheckAdmissionAndDeadline() {
  WaitFixture success{{{ProcessState::Running}, {ProcessState::Exited}}};
  const auto exited = success.Run({kExe, kToken});
  CHECK(exited.status == HandoffStatus::Exited);
  CHECK(exited.CanStart());
  CHECK(success.observed.pid == kPrevious.pid);
  CHECK(success.observed.start_ticks == kPrevious.start_ticks);
  CHECK(success.probes == 2 && success.sleeps == 1);
  CHECK(success.current.time_since_epoch() == 50ms);

  WaitFixture reused{{{ProcessState::Running}, {ProcessState::IdentityMismatch}}};
  const auto mismatch = reused.Run({kExe, kToken});
  CHECK(mismatch.status == HandoffStatus::IdentityMismatch);
  CHECK(!mismatch.CanStart());
  CHECK(reused.probes == 2 && reused.sleeps == 1);

  WaitFixture inaccessible{{{ProcessState::CheckFailed, EACCES}}};
  const auto rejected = inaccessible.Run({kExe, kToken});
  CHECK(rejected.status == HandoffStatus::CheckFailed);
  CHECK(!rejected.CanStart());
  CHECK(rejected.error == EACCES);
  CHECK(inaccessible.probes == 1 && inaccessible.sleeps == 0);

  WaitFixture running{{{ProcessState::Running}}};
  const auto timeout = running.Run({kExe, kToken});
  CHECK(timeout.status == HandoffStatus::TimedOut);
  CHECK(!timeout.CanStart());
  CHECK(running.probes == 600 && running.sleeps == 600);
  CHECK(running.current.time_since_epoch() == 30s);

  for (const auto delay : {30s - 1ms, 30s + 0ms, 30s + 1ms}) {
    WaitFixture last_probe;
    last_probe.probe_cost = delay;
    const auto result = last_probe.Run({kExe, kToken});
    CHECK(result.CanStart() == (delay < 30s));
    CHECK(last_probe.probes == 1 && last_probe.sleeps == 0);
  }
  WaitFixture overslept{{{ProcessState::Running}, {ProcessState::Exited}}};
  overslept.oversleep = 30s;
  CHECK(overslept.Run({kExe, kToken}).status == HandoffStatus::TimedOut);
  CHECK(overslept.probes == 1);
}

void CheckSpawn() {
  const std::string image = "/test folder/game.AppImage";
  const std::vector<std::string> args{
      "/tmp/mount/usr/bin/larecomp", "--user_data_root=/profile space", "",
      "--larecomp_restart_wait_pid=400:73102", "--show_debug_menu=true", "end-of-arguments"};
  const std::vector<std::string> expected{
      image, "--user_data_root=/profile space", "", "--show_debug_menu=true", "end-of-arguments",
      kToken};
  int attempts = 0;
  for (const int error : {0, ENOENT, EACCES}) {
    const auto result = handoff::SpawnReplacement(image, args, kPrevious,
                                                   [&](const char* exe, char* const* argv) {
      ++attempts;
      CHECK(exe && exe == image);
      CHECK(argv);
      if (argv) {
        std::vector<std::string> actual;
        for (std::size_t i = 0; i <= expected.size() && argv[i]; ++i) {
          actual.emplace_back(argv[i]);
        }
        CHECK(actual == expected);
      }
      return handoff::SpawnResult{913, error};
    });
    CHECK(static_cast<bool>(result) == (error == 0));
    CHECK(result.error == error);
    CHECK(result.pid == 913);
  }
  CHECK(attempts == 3);
}

}  // namespace

int main() {
  CheckArguments();
  CheckProcessStatus();
  CheckAdmissionAndDeadline();
  CheckSpawn();
  std::printf("%d checks, %d failed\n", checks, failed);
  return failed != 0;
}

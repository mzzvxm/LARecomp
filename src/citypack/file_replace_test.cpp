// A check that needs no game, no archive, no renderer and no SDK: how the city pack's files are written when parallel processes share the cache folder
// (file_replace.h). Build: cmake --build <dir> --target larecomp_file_replace_test (src/citypack/CMakeLists.txt); run: larecomp_file_replace_test [<scratch folder>]
// (exit 0 = passed).
//
// The case of 2026-11-22 (integ-ab-vk-off2): two runs of a test batch built the city hit data at the same time into one cache. Both wrote "<name>.tmp", one
// moved the other's file away, and the second one's rename failed: "the texture file could not be moved into place", the city effects stayed off that session.
//
// What it proves:
//   * THE NAMES: the temporary names are unique per call and per thread (a hundred calls on eight threads give 800 names), in the target's own folder, and
//     IsTempNameOf knows them (and the old "<name>.tmp") and nothing else; SweepStaleTemps removes the old ones of that target only;
//   * THE DECISION (hooked tries, no real file system waiting): a first try that works moves once; a busy file is tried again with growing waits until it
//     works; a file that stays busy with the SAME bytes in place is a success (kAlreadyThere, the temporary file removed, the old file untouched); with other bytes it
//     is a failure that leaves the old file alone and removes the temporary one; an error that is not "busy" is not tried again;
//   * THE FILES: a move over nothing, over an old file; on Windows an old file that another handle holds open without delete sharing (what a reader's ifstream
//     does): other bytes fail, the same bytes pass, and a handle that is let go after 120 ms is waited for;
//   * THE RACE: eight threads write the same bytes to one target through their own temporary files, twenty rounds, and every one ends kMoved or kAlreadyThere with the
//     right bytes in place and no temporary file left.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
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
#endif

#include "citypack/file_replace.h"

using namespace larecomp::citypack;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

void Write(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), std::streamsize(bytes.size()));
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Bytes that differ from one `seed` to the next and cross the 1 MiB chunk of SameFileContent.
std::string Bytes(size_t size, uint32_t seed) {
  std::string out(size, '\0');
  uint32_t x = seed * 2654435761u + 1;
  for (char& c : out) {
    x = x * 1664525u + 1013904223u;
    c = char(x >> 24);
  }
  return out;
}

size_t CountTemps(const fs::path& folder) {
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(folder)) {
    if (e.path().extension() == ".tmp") ++n;
  }
  return n;
}

void TestNames(const fs::path& dir) {
  CHECK(IsTempNameOf("tex.bin.tmp", "tex.bin"));
  CHECK(IsTempNameOf("tex.bin.1234.7.tmp", "tex.bin"));
  CHECK(!IsTempNameOf("tex.bin", "tex.bin"));
  CHECK(!IsTempNameOf("tex.bin.tmpx", "tex.bin"));
  CHECK(!IsTempNameOf("tex.bin.body", "tex.bin"));
  CHECK(!IsTempNameOf("tex.binx.tmp", "tex.bin"));  // another file's temporary name
  CHECK(!IsTempNameOf("other.tmp", "tex.bin"));
  CHECK(!IsTempNameOf(".tmp", ""));
  CHECK(!IsTempNameOf("tex.bin.tm", "tex.bin"));

  const fs::path target = dir / "names" / "tex.bin";
  fs::create_directories(target.parent_path());
  std::set<std::string> seen;
  const fs::path first = TempPathFor(target);
  CHECK(first.parent_path() == target.parent_path());
  CHECK(first != target);
  CHECK(IsTempNameOf(first.filename().string(), "tex.bin"));
  seen.insert(first.string());
  CHECK(TempPathFor(target) != first);

  std::vector<std::vector<std::string>> per_thread(8);
  std::vector<std::thread> pool;
  for (size_t t = 0; t < per_thread.size(); ++t) {
    pool.emplace_back([&, t] {
      for (int i = 0; i < 100; ++i) per_thread[t].push_back(TempPathFor(target).string());
    });
  }
  for (std::thread& t : pool) t.join();
  size_t total = 0;
  for (const auto& names : per_thread) {
    for (const std::string& name : names) {
      seen.insert(name);
      ++total;
    }
  }
  CHECK(total == 800);
  CHECK(seen.size() == 801);  // the first one and the 800: none twice
}

void TestSweep(const fs::path& dir) {
  const fs::path folder = dir / "sweep";
  fs::create_directories(folder);
  const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(3);
  const char* old_names[] = {"x.bin.tmp", "x.bin.123.4.tmp", "x.bin.9.9.tmp"};
  for (const char* name : old_names) {
    Write(folder / name, "old");
    fs::last_write_time(folder / name, old_time);
  }
  Write(folder / "x.bin.999.1.tmp", "fresh");  // a live writer's
  Write(folder / "x.bin", "the file");
  fs::last_write_time(folder / "x.bin", old_time);
  Write(folder / "x.bin.body", "body");
  fs::last_write_time(folder / "x.bin.body", old_time);
  Write(folder / "y.bin.1.1.tmp", "another target's");
  fs::last_write_time(folder / "y.bin.1.1.tmp", old_time);

  const int removed = SweepStaleTemps(folder / "x.bin", std::chrono::hours(2));
  CHECK(removed == 3);
  for (const char* name : old_names) CHECK(!fs::exists(folder / name));
  CHECK(fs::exists(folder / "x.bin.999.1.tmp"));
  CHECK(fs::exists(folder / "x.bin"));
  CHECK(fs::exists(folder / "x.bin.body"));
  CHECK(fs::exists(folder / "y.bin.1.1.tmp"));
  CHECK(SweepStaleTemps(folder / "x.bin", std::chrono::hours(2)) == 0);
  CHECK(SweepStaleTemps(dir / "no-such-folder" / "x.bin", std::chrono::hours(2)) == 0);  // no folder: nothing, no throw
}

void TestSame(const fs::path& dir) {
  const fs::path folder = dir / "same";
  fs::create_directories(folder);
  const std::string a = Bytes((size_t(3) << 20) + 17, 1);
  std::string b = a;
  Write(folder / "a", a);
  Write(folder / "b", b);
  CHECK(SameFileContent(folder / "a", folder / "b"));
  b.back() = char(b.back() ^ 1);  // the last byte, past two whole chunks
  Write(folder / "b", b);
  CHECK(!SameFileContent(folder / "a", folder / "b"));
  b = a;
  b[size_t(1) << 20] = char(b[size_t(1) << 20] ^ 0x40);  // the first byte of the second chunk
  Write(folder / "b", b);
  CHECK(!SameFileContent(folder / "a", folder / "b"));
  Write(folder / "b", a.substr(0, a.size() - 1));  // shorter
  CHECK(!SameFileContent(folder / "a", folder / "b"));
  CHECK(!SameFileContent(folder / "a", folder / "missing"));
  CHECK(!SameFileContent(folder / "missing", folder / "a"));
  Write(folder / "e1", "");
  Write(folder / "e2", "");
  CHECK(SameFileContent(folder / "e1", folder / "e2"));  // two empty files are the same
}

void TestRetryRule() {
  CHECK(ShouldRetryMove(MoveError::kBusy, 1, 6));
  CHECK(ShouldRetryMove(MoveError::kBusy, 5, 6));
  CHECK(!ShouldRetryMove(MoveError::kBusy, 6, 6));  // no attempt left
  CHECK(!ShouldRetryMove(MoveError::kBusy, 1, 1));
  CHECK(!ShouldRetryMove(MoveError::kOther, 1, 6));
  CHECK(!ShouldRetryMove(MoveError::kNone, 1, 6));
}

// The decision, with the tries made up and the waits recorded instead of slept.
void TestDecision(const fs::path& dir) {
  const fs::path folder = dir / "decision";
  fs::create_directories(folder);
  const std::string mine = Bytes(4096, 7);
  const std::string theirs = Bytes(4096, 8);
  const fs::path target = folder / "t.bin";
  const auto fresh_temp = [&] {
    const fs::path temp = TempPathFor(target);
    Write(temp, mine);
    return temp;
  };
  std::vector<int> waits;
  MoveHooks hooks;
  hooks.wait = [&](int ms) { waits.push_back(ms); };
  MoveOptions options;  // 6 attempts, 40 ms

  // works at once
  {
    hooks.try_move = [](const fs::path&, const fs::path&, std::string*) { return MoveError::kNone; };
    waits.clear();
    int attempts = 0;
    std::string why;
    const fs::path temp = fresh_temp();
    CHECK(MoveIntoPlace(temp, target, &why, options, hooks, &attempts) == MoveResult::kMoved);
    CHECK(attempts == 1 && waits.empty());
    fs::remove(temp);
  }
  // busy twice, then it works: three tries, the waits grow
  {
    int calls = 0;
    hooks.try_move = [&](const fs::path&, const fs::path&, std::string* why) {
      if (why) *why = "busy";
      return ++calls <= 2 ? MoveError::kBusy : MoveError::kNone;
    };
    waits.clear();
    int attempts = 0;
    const fs::path temp = fresh_temp();
    CHECK(MoveIntoPlace(temp, target, nullptr, options, hooks, &attempts) == MoveResult::kMoved);
    CHECK(attempts == 3);
    CHECK((waits == std::vector<int>{40, 80}));
    fs::remove(temp);
  }
  // busy for ever, the same bytes in place: done, the temporary file removed, the file in place untouched
  {
    Write(target, mine);
    const auto stamp = fs::last_write_time(target);
    hooks.try_move = [](const fs::path&, const fs::path&, std::string* why) {
      if (why) *why = "access denied";
      return MoveError::kBusy;
    };
    waits.clear();
    int attempts = 0;
    std::string why = "unchanged";
    const fs::path temp = fresh_temp();
    CHECK(MoveIntoPlace(temp, target, &why, options, hooks, &attempts) == MoveResult::kAlreadyThere);
    CHECK(attempts == options.max_attempts);
    CHECK(waits.size() == size_t(options.max_attempts - 1));
    CHECK(!fs::exists(temp));
    CHECK(Read(target) == mine && fs::last_write_time(target) == stamp);
    CHECK(why == "unchanged");  // no reason is given for a success
  }
  // busy for ever, other bytes in place: a failure with a reason; the file in place untouched, the temporary file removed
  {
    Write(target, theirs);
    waits.clear();
    int attempts = 0;
    std::string why;
    const fs::path temp = fresh_temp();
    CHECK(MoveIntoPlace(temp, target, &why, options, hooks, &attempts) == MoveResult::kFailed);
    CHECK(attempts == options.max_attempts);
    CHECK(!fs::exists(temp));
    CHECK(Read(target) == theirs);
    CHECK(why.find("access denied") != std::string::npos && why.find("other bytes") != std::string::npos);
  }
  // busy for ever and nothing in place: a failure
  {
    fs::remove(target);
    const fs::path temp = fresh_temp();
    CHECK(MoveIntoPlace(temp, target, nullptr, options, hooks) == MoveResult::kFailed);
    CHECK(!fs::exists(temp) && !fs::exists(target));
  }
  // an error that is not "busy" is not tried again
  {
    hooks.try_move = [](const fs::path&, const fs::path&, std::string* why) {
      if (why) *why = "the disk is gone";
      return MoveError::kOther;
    };
    waits.clear();
    int attempts = 0;
    std::string why;
    Write(target, theirs);
    fs::path temp = fresh_temp();
    CHECK(MoveIntoPlace(temp, target, &why, options, hooks, &attempts) == MoveResult::kFailed);
    CHECK(attempts == 1 && waits.empty());
    CHECK(why.find("the disk is gone") != std::string::npos);
    CHECK(!fs::exists(temp) && Read(target) == theirs);
    Write(target, mine);  // the same bytes already in place: still done (the job is, whatever the error was)
    temp = fresh_temp();
    CHECK(MoveIntoPlace(temp, target, nullptr, options, hooks, &attempts) == MoveResult::kAlreadyThere);
    CHECK(attempts == 1 && !fs::exists(temp));
  }
  // one attempt only (max_attempts 1) and the waits of a zero retry_ms
  {
    hooks.try_move = [](const fs::path&, const fs::path&, std::string*) { return MoveError::kBusy; };
    waits.clear();
    int attempts = 0;
    MoveOptions once;
    once.max_attempts = 1;
    Write(target, theirs);
    const fs::path temp = fresh_temp();
    CHECK(MoveIntoPlace(temp, target, nullptr, once, hooks, &attempts) == MoveResult::kFailed);
    CHECK(attempts == 1 && waits.empty());
    MoveOptions none;
    none.max_attempts = 0;  // clamped to one
    const fs::path temp2 = fresh_temp();
    CHECK(MoveIntoPlace(temp2, target, nullptr, none, hooks, &attempts) == MoveResult::kFailed);
    CHECK(attempts == 1);
  }
  CHECK(CountTemps(folder) == 0);
}

#if defined(_WIN32)
// A handle that holds the file the way a reader's ifstream does: read and write sharing, no delete sharing.
HANDLE HoldOpen(const fs::path& path) {
  return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
#endif

void TestFiles(const fs::path& dir) {
  const fs::path folder = dir / "files";
  fs::create_directories(folder);
  const std::string mine = Bytes(300000, 21);
  const std::string theirs = Bytes(300000, 22);
  const fs::path target = folder / "tex.bin";

  // over nothing
  fs::path temp = TempPathFor(target);
  Write(temp, mine);
  std::string why;
  CHECK(MoveIntoPlace(temp, target, &why) == MoveResult::kMoved);
  CHECK(!fs::exists(temp) && Read(target) == mine);
  // over an old file
  temp = TempPathFor(target);
  Write(temp, theirs);
  CHECK(MoveIntoPlace(temp, target, &why) == MoveResult::kMoved);
  CHECK(!fs::exists(temp) && Read(target) == theirs);

#if defined(_WIN32)
  MoveOptions quick;
  quick.max_attempts = 3;
  quick.retry_ms = 1;
  // held open, other bytes: the move cannot happen, the old file stays
  {
    HANDLE held = HoldOpen(target);
    CHECK(held != INVALID_HANDLE_VALUE);
    temp = TempPathFor(target);
    Write(temp, mine);
    int attempts = 0;
    CHECK(MoveIntoPlace(temp, target, &why, quick, {}, &attempts) == MoveResult::kFailed);
    CHECK(attempts == 3);
    CHECK(!why.empty());
    CHECK(!fs::exists(temp));
    CloseHandle(held);
    CHECK(Read(target) == theirs);
  }
  // held open, the same bytes: another process wrote it first
  {
    HANDLE held = HoldOpen(target);
    CHECK(held != INVALID_HANDLE_VALUE);
    temp = TempPathFor(target);
    Write(temp, theirs);
    CHECK(MoveIntoPlace(temp, target, &why, quick) == MoveResult::kAlreadyThere);
    CHECK(!fs::exists(temp));
    CloseHandle(held);
    CHECK(Read(target) == theirs);
  }
  // held open and let go after 120 ms: waited for, then moved
  {
    HANDLE held = HoldOpen(target);
    CHECK(held != INVALID_HANDLE_VALUE);
    std::thread release([held] {
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      CloseHandle(held);
    });
    temp = TempPathFor(target);
    Write(temp, mine);
    int attempts = 0;
    MoveOptions patient;  // 6 tries, 40 ms growing: up to 600 ms
    CHECK(MoveIntoPlace(temp, target, &why, patient, {}, &attempts) == MoveResult::kMoved);
    CHECK(attempts >= 2);
    release.join();
    CHECK(!fs::exists(temp) && Read(target) == mine);
  }
#endif
  CHECK(CountTemps(folder) == 0);
}

void TestRace(const fs::path& dir) {
  const fs::path folder = dir / "race";
  fs::create_directories(folder);
  const fs::path target = folder / "mcla_city_hit_tex.bin";
  const std::string bytes = Bytes((size_t(2) << 20) + 5, 99);
  std::atomic<int> moved{0};
  std::atomic<int> already{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < 8; ++t) {
    pool.emplace_back([&] {
      for (int round = 0; round < 20; ++round) {
        const fs::path temp = TempPathFor(target);
        Write(temp, bytes);
        std::string why;
        switch (MoveIntoPlace(temp, target, &why)) {
          case MoveResult::kMoved: ++moved; break;
          case MoveResult::kAlreadyThere: ++already; break;
          case MoveResult::kFailed:
            ++failed;
            std::printf("  race: a move failed: %s\n", why.c_str());
            break;
        }
      }
    });
  }
  for (std::thread& t : pool) t.join();
  CHECK(failed == 0);
  CHECK(moved + already == 160);
  CHECK(moved >= 1);
  CHECK(Read(target) == bytes);
  CHECK(CountTemps(folder) == 0);
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "larecomp_file_replace_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  TestNames(dir);
  TestSweep(dir);
  TestSame(dir);
  TestRetryRule();
  TestDecision(dir);
  TestFiles(dir);
  TestRace(dir);
  fs::remove_all(dir, ec);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

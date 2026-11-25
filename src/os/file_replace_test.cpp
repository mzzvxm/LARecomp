// A check that needs no game, no archive, no renderer and no SDK: how the caches, packs and lists that parallel processes share are written (src/os/file_replace.h), and
// the readers' side of two of them (gpu/cache_file_check.h, native_gfx/d3d12/library_file.h). Build: cmake --build <dir> --target larecomp_file_replace_test
// (CMakeLists.txt); run: larecomp_file_replace_test [<scratch folder>] (exit 0 = passed).
//
// The case of 2026-11-22 (integ-ab-vk-off2): two runs of a test batch built the city hit data at the same time into one cache. Both wrote "<name>.tmp", one
// moved the other's file away, and the second one's rename failed: "the texture file could not be moved into place", the city effects stayed off that session. The
// pipeline caches, the shader packs and their stamps, the Direct3D 12 library and the mod loader's template cache had the same pattern (CACHE-WRITE-RACE-2).
//
// What it proves:
//   * THE NAMES: the temporary names are unique per call and per thread (a hundred calls on eight threads give 800 names), in the target's own folder, and
//     IsTempNameOf knows them (and the old "<name>.tmp") and nothing else; SweepStaleTemps removes the old ones of that target only; UniqueTempPath (the write probes'
//     name) makes no file and sweeps nothing;
//   * THE DECISION (hooked tries, no real file system waiting): a first try that works moves once; a busy file is tried again with growing waits until it
//     works; a file that stays busy with the SAME bytes in place is a success (kAlreadyThere, the temporary file removed, the old file untouched); with other bytes it
//     is a failure that leaves the old file alone and removes the temporary one; an error that is not "busy" is not tried again;
//   * THE FILES: a move over nothing, over an old file; on Windows an old file that another handle holds open without delete sharing (what a reader's ifstream
//     does): other bytes fail, the same bytes pass, and a handle that is let go after 120 ms is waited for;
//   * WriteFileReplacing (what the pipeline caches, the stamps, the shader packs and the template cache use): folders made, an empty file, no temporary file left,
//     a failure with a reason that leaves the old file alone;
//   * THE RACE: eight threads write the same bytes to one target through their own temporary files, twenty rounds, and every one ends kMoved or kAlreadyThere with the
//     right bytes in place and no temporary file left; six writers with DIFFERENT whole files (two games with different pipeline records) leave one of the whole
//     files, never a mix;
//   * A READER DURING THE REPLACE: a reader that opens and reads the file over and over while three writers replace it sees only whole files (each one checks itself:
//     magic, length, hash), never a missing file, never half of one;
//   * THE READERS' CHECKS: the record count of a pipeline cache is believed only when the file can hold that many records; the Direct3D 12 library file is SEALED
//     (header, size and hash match), LEGACY (no header) or DAMAGED (cut short, a flipped byte in the library or in the header, a header of another file, a byte too many).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

#include "gpu/cache_file_check.h"
#include "native_gfx/d3d12/library_file.h"
#include "os/file_replace.h"

using namespace larecomp::os;
namespace fs = std::filesystem;
namespace lf = mcla::native_gfx::library_file;

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

// A file that checks itself: magic, a version number, the length of the body, the body, and a hash of all that. A reader that gets half of one, or a mix of two, fails
// CheckVersioned; that is what the real formats' readers do with their own magic, sizes and hashes.
uint64_t Fnv(const std::string& bytes, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= uint8_t(bytes[i]);
    h *= 1099511628211ull;
  }
  return h;
}

std::string MakeVersioned(uint32_t id, size_t body_size) {
  std::string out("VERS", 4);
  out.append(reinterpret_cast<const char*>(&id), 4);
  const uint64_t n = body_size;
  out.append(reinterpret_cast<const char*>(&n), 8);
  out += Bytes(body_size, id);
  const uint64_t h = Fnv(out, out.size());
  out.append(reinterpret_cast<const char*>(&h), 8);
  return out;
}

// The id of a whole versioned file, 0 for anything else (ids start at 1).
uint32_t CheckVersioned(const std::string& bytes) {
  if (bytes.size() < 24 || bytes.compare(0, 4, "VERS") != 0) return 0;
  uint32_t id = 0;
  uint64_t n = 0;
  uint64_t h = 0;
  std::memcpy(&id, bytes.data() + 4, 4);
  std::memcpy(&n, bytes.data() + 8, 8);
  if (n != bytes.size() - 24) return 0;
  std::memcpy(&h, bytes.data() + bytes.size() - 8, 8);
  return h == Fnv(bytes, bytes.size() - 8) ? id : 0;
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

void TestUniqueTempPath(const fs::path& dir) {
  const fs::path folder = dir / "unique";
  fs::create_directories(folder);
  const fs::path target = folder / ".larecomp_write_test";
  const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(3);
  Write(folder / ".larecomp_write_test.1.1.tmp", "old");
  fs::last_write_time(folder / ".larecomp_write_test.1.1.tmp", old_time);
  const fs::path a = UniqueTempPath(target);
  const fs::path b = UniqueTempPath(target);
  CHECK(a != b);
  CHECK(a.parent_path() == folder);
  CHECK(IsTempNameOf(a.filename().string(), ".larecomp_write_test"));
  CHECK(!fs::exists(a) && !fs::exists(b));                         // no file is made
  CHECK(fs::exists(folder / ".larecomp_write_test.1.1.tmp"));      // and nothing is swept
  (void)TempPathFor(target);
  CHECK(!fs::exists(folder / ".larecomp_write_test.1.1.tmp"));     // TempPathFor sweeps the old one
}

void TestWriteFileReplacing(const fs::path& dir) {
  const fs::path folder = dir / "write";
  fs::create_directories(folder);
  const fs::path target = folder / "sub" / "deeper" / "list.bin";  // the folders are made
  const std::string a = Bytes(100000, 31);
  const std::string b = Bytes(70000, 32);
  std::string why;
  CHECK(WriteFileReplacing(target, a.data(), a.size(), &why) == MoveResult::kMoved);
  CHECK(Read(target) == a);
  CHECK(WriteFileReplacing(target, b.data(), b.size(), &why) == MoveResult::kMoved);  // over an old file
  CHECK(Read(target) == b);
  CHECK(Succeeded(WriteFileReplacing(target, b.data(), b.size(), &why)) && Read(target) == b);  // the same bytes again
  CHECK(WriteFileReplacing(target, nullptr, 0, &why) == MoveResult::kMoved);                  // an empty file
  CHECK(fs::exists(target) && fs::file_size(target) == 0);
  CHECK(CountTemps(target.parent_path()) == 0);

  // a folder that cannot be made (a file is in the way): a failure with a reason, the file in the way untouched, no temporary file
  Write(folder / "blocker", "x");
  why.clear();
  CHECK(WriteFileReplacing(folder / "blocker" / "list.bin", a.data(), a.size(), &why) == MoveResult::kFailed);
  CHECK(!why.empty());
  CHECK(Read(folder / "blocker") == "x");
  CHECK(CountTemps(folder) == 0);

  // a file that stays busy: other bytes in place are left alone, the same bytes are a success; the temporary file is removed either way
  MoveHooks busy;
  busy.try_move = [](const fs::path&, const fs::path&, std::string* reason) {
    if (reason) *reason = "access denied";
    return MoveError::kBusy;
  };
  busy.wait = [](int) {};
  Write(target, a);
  why.clear();
  CHECK(WriteFileReplacing(target, b.data(), b.size(), &why, {}, busy) == MoveResult::kFailed);
  CHECK(why.find("access denied") != std::string::npos);
  CHECK(Read(target) == a);
  CHECK(WriteFileReplacing(target, a.data(), a.size(), &why, {}, busy) == MoveResult::kAlreadyThere);
  CHECK(Read(target) == a);
  CHECK(CountTemps(target.parent_path()) == 0);
}

// Six writers with different whole files (two games with different pipeline records, say) replace one target: whoever is last wins, and the file is whole.
void TestRaceDifferentFiles(const fs::path& dir) {
  const fs::path folder = dir / "race_different";
  fs::create_directories(folder);
  const fs::path target = folder / "pipelines.cache";
  std::atomic<int> failed{0};
  std::vector<std::thread> pool;
  for (uint32_t t = 1; t <= 6; ++t) {
    pool.emplace_back([&, t] {
      for (uint32_t round = 0; round < 25; ++round) {
        const std::string bytes = MakeVersioned(t * 1000 + round, 30000 + size_t(t) * 777);
        std::string why;
        if (!Succeeded(WriteFileReplacing(target, bytes.data(), bytes.size(), &why))) {
          ++failed;
          std::printf("  race: a write failed: %s\n", why.c_str());
        }
      }
    });
  }
  for (std::thread& t : pool) t.join();
  CHECK(failed == 0);
  CHECK(CheckVersioned(Read(target)) != 0);  // one writer's whole file, not a mix
  CHECK(CountTemps(folder) == 0);
}

// A reader that opens and reads the file over and over while writers replace it: a whole file every time (on Windows the reader's open handle makes a replace wait
// or lose: the old file stays; nothing is half written or missing).
void TestReaderDuringReplace(const fs::path& dir) {
  const fs::path folder = dir / "reader";
  fs::create_directories(folder);
  const fs::path target = folder / "shared.pack";
  const std::string first = MakeVersioned(1, 50000);
  CHECK(Succeeded(WriteFileReplacing(target, first.data(), first.size(), nullptr)));

  std::atomic<bool> stop{false};
  std::atomic<int> whole{0};
  std::atomic<int> torn{0};
  std::atomic<int> gaps{0};
  std::thread reader([&] {
    while (!stop.load()) {
      std::ifstream in(target, std::ios::binary);
      if (!in) {
        ++gaps;
        continue;
      }
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      if (CheckVersioned(bytes) == 0) {
        ++torn;
      } else {
        ++whole;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  });
  std::atomic<int> in_place{0};
  std::atomic<int> lost{0};
  std::vector<std::thread> writers;
  for (uint32_t t = 0; t < 3; ++t) {
    writers.emplace_back([&, t] {
      for (uint32_t round = 0; round < 30; ++round) {
        const std::string bytes = MakeVersioned(100 + t * 100 + round, 40000 + size_t(round) * 500);
        if (Succeeded(WriteFileReplacing(target, bytes.data(), bytes.size(), nullptr))) {
          ++in_place;
        } else {
          ++lost;  // the file stayed busy through every try and held other bytes: the old file is still there
        }
      }
    });
  }
  for (std::thread& t : writers) t.join();
  stop = true;
  reader.join();
  CHECK(torn == 0);
  CHECK(gaps == 0);
  CHECK(whole > 0);
  CHECK(in_place >= 1);
  CHECK(in_place + lost == 90);
  CHECK(CheckVersioned(Read(target)) != 0);
  CHECK(CountTemps(folder) == 0);
  std::printf("  reader: %d whole reads, %d writes in place, %d lost to a busy file\n", whole.load(), in_place.load(), lost.load());
}

std::string Sealed(const std::string& blob) {
  std::string out(lf::kHeaderBytes, '\0');
  out += blob;
  lf::FillHeader(reinterpret_cast<uint8_t*>(out.data()), reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
  return out;
}

lf::Opened OpenText(const std::string& file) { return lf::Open(reinterpret_cast<const uint8_t*>(file.data()), file.size()); }

// The Direct3D 12 library file's readers' check: sealed, legacy or damaged.
void TestLibraryFile() {
  const std::string blob = Bytes(100003, 41);  // not a multiple of 8: the hash's tail
  const std::string sealed = Sealed(blob);
  CHECK(sealed.size() == blob.size() + lf::kHeaderBytes);
  {
    const lf::Opened o = OpenText(sealed);
    CHECK(o.kind == lf::Kind::kSealed);
    CHECK(o.blob_size == blob.size());
    CHECK(o.blob == reinterpret_cast<const uint8_t*>(sealed.data()) + lf::kHeaderBytes);
    CHECK(o.blob && o.blob_size == blob.size() && std::memcmp(o.blob, blob.data(), blob.size()) == 0);
  }
  // an older build's file has no header: the whole file is the library, as it always was
  {
    const lf::Opened o = OpenText(blob);
    CHECK(o.kind == lf::Kind::kLegacy);
    CHECK(o.blob == reinterpret_cast<const uint8_t*>(blob.data()) && o.blob_size == blob.size());
  }
  CHECK(OpenText("abc").kind == lf::Kind::kLegacy);
  CHECK(OpenText("").kind == lf::Kind::kLegacy);

  // damaged: a file that was cut short (every length from the whole file down to nothing but the magic)
  CHECK(OpenText(sealed.substr(0, sealed.size() - 1)).kind == lf::Kind::kDamaged);
  CHECK(OpenText(sealed.substr(0, sealed.size() / 2)).kind == lf::Kind::kDamaged);
  CHECK(OpenText(sealed.substr(0, lf::kHeaderBytes)).kind == lf::Kind::kDamaged);       // the header only
  CHECK(OpenText(sealed.substr(0, lf::kHeaderBytes - 1)).kind == lf::Kind::kDamaged);   // a header cut short
  CHECK(OpenText(sealed.substr(0, 10)).kind == lf::Kind::kDamaged);
  CHECK(OpenText(sealed.substr(0, 8)).kind == lf::Kind::kDamaged);                      // the magic only
  CHECK(OpenText(sealed + "x").kind == lf::Kind::kDamaged);                             // a byte too many
  // damaged: one flipped byte in the library (first, middle, last), in the stored size, in the stored hash
  for (const size_t at : {lf::kHeaderBytes, lf::kHeaderBytes + blob.size() / 2, sealed.size() - 1, size_t(8), size_t(16), size_t(23)}) {
    std::string bad = sealed;
    bad[at] = char(bad[at] ^ 0x10);
    CHECK(OpenText(bad).kind == lf::Kind::kDamaged);
  }
  // damaged: the header of another file of the same length (the size matches, the hash does not)
  {
    const std::string other = Sealed(Bytes(100003, 42));
    CHECK(OpenText(other.substr(0, lf::kHeaderBytes) + blob).kind == lf::Kind::kDamaged);
    CHECK(OpenText(other).kind == lf::Kind::kSealed);
  }
  // an empty library is never written, and a header over none is not one
  CHECK(OpenText(Sealed("")).kind == lf::Kind::kDamaged);
  // every single flipped byte of a short library is noticed, whatever its length mod 8
  for (size_t n = 1; n <= 40; ++n) {
    const std::string small = Bytes(n, uint32_t(n));
    const std::string file = Sealed(small);
    CHECK(OpenText(file).kind == lf::Kind::kSealed);
    for (size_t i = 0; i < file.size(); ++i) {
      std::string bad = file;
      bad[i] = char(bad[i] ^ 0x01);
      // a flipped byte of the magic itself makes a file without a header: the older builds' way (the driver's own checks), the one case the header cannot catch
      CHECK(OpenText(bad).kind == (i < 8 ? lf::Kind::kLegacy : lf::Kind::kDamaged));
    }
  }
}

// The pipeline cache's record count is believed only when the file can hold that many records.
void TestRecordsFit() {
  using mcla::gpu::cache_file::RecordsFit;
  static_assert(RecordsFit(2, 10, 25));
  CHECK(RecordsFit(0, 100, 0));
  CHECK(RecordsFit(3, 100, 300));
  CHECK(RecordsFit(3, 100, 399));
  CHECK(!RecordsFit(4, 100, 399));
  CHECK(!RecordsFit(1, 100, 99));
  CHECK(!RecordsFit(0xFFFFFFFFull, 100, 5000));        // a damaged header
  CHECK(!RecordsFit(~uint64_t(0), 100, ~uint64_t(0)));  // no overflow
  CHECK(!RecordsFit(1, 0, 100));                        // a record of no size is no record
  CHECK(RecordsFit(0xFFFFFFFFull, 1, 0xFFFFFFFFull));
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
  TestUniqueTempPath(dir);
  TestWriteFileReplacing(dir);
  TestRaceDifferentFiles(dir);
  TestReaderDuringReplace(dir);
  TestLibraryFile();
  TestRecordsFit();
  fs::remove_all(dir, ec);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

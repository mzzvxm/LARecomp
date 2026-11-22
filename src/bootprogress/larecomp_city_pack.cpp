#include "larecomp_city_pack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "settings/immortal.h"
#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "larecomp_log.h"
#include "spdlog_console.h"  // LarecompLogFolder()
#include "larecomp_version.h"
#include "bootprogress/city_pack_rules.h"
#include "citypack/builder.h"
#include "citypack/hit_data.h"
#include "mc_engine/modloader/xcompress.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

namespace larecomp::citypackbuild {
namespace {

namespace cp = larecomp::citypack;
using Clock = std::chrono::steady_clock;

constexpr const char* kSourceName = "xarchive_cache.rpf";
constexpr const char* kModsName = "xarchive_mods.rpf";
constexpr const char* kLogName = "city_pack_build.txt";
constexpr const char* kHitLogName = "city_hit_build.txt";
constexpr const char* kHitCheckLogName = "city_hit_check.txt";

std::string PathUtf8(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

// The request both ways of building make: the same files, the same builder and stamp, so what the launcher's step writes is current for the game and the other way round.
// The caller picks the threads and the force.
cp::BuildRequest MakeRequest(const std::filesystem::path& game_root, const std::filesystem::path& folder) {
  cp::BuildRequest request;
  request.source_rpf = game_root / kSourceName;
  request.mods_rpf = game_root / kModsName;  // the mod loader's overlay: only its city files count (cp::CityOverlayHash)
  request.out_dir = folder;
  request.lzx = [](const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst, size_t dst_len) { return mc::modloader::LzxDecompress(src, src_len, dst, dst_len); };
  request.app_version = LARECOMP_VERSION_LABEL;
  return request;
}

cp::BuildResult BuildGuarded(const cp::BuildRequest& request, cp::ProgressSink* sink) {
  cp::BuildResult result;
  try {
    result = cp::BuildCityPack(request, sink);
  } catch (const std::exception& e) {
    result.ok = false;
    result.error = std::string("the city pack build stopped unexpectedly: ") + e.what();
  } catch (...) {
    result.ok = false;
    result.error = "the city pack build stopped unexpectedly";
  }
  return result;
}

// The hit data's request for the same files: the default rules and zstd level and no GB-F2 rows, which is what citypack_tool build-hit gives with no options, so a file the tool made is
// current for the game and the other way round (the stamp has the rules and the rows' hash; the app version is information only).
cp::HitDataRequest MakeHitRequest(const cp::BuildRequest& base) {
  cp::HitDataRequest request;
  request.base = base;
  return request;
}

cp::HitDataResult BuildHitGuarded(const cp::HitDataRequest& request, cp::ProgressSink* sink) {
  cp::HitDataResult result;
  try {
    result = cp::BuildHitData(request, sink);
  } catch (const std::exception& e) {
    result.ok = false;
    result.error = std::string("the city hit data build stopped unexpectedly: ") + e.what();
  } catch (...) {
    result.ok = false;
    result.error = "the city hit data build stopped unexpectedly";
  }
  return result;
}

// The in-game build's workers run below the game's own threads: on Windows a thread of a lower priority only runs on a core the game's threads leave free. Background mode lowers
// the thread's CPU, disk and memory priorities together (the game's own streaming keeps the disk while the build reads 2 GB of archive); where it is refused the CPU priority alone goes down.
void LowerThreadPriority() {
#ifdef _WIN32
  if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  }
#else
  // Linux: the thread's own nice value (setpriority with a thread id affects that thread only).
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

// logs/city_pack_build.txt: every line of a build, the way logs/shader_build.txt has them (it is what a support request needs).
class FileLog {
 public:
  explicit FileLog(const char* name = kLogName) : started_(Clock::now()) {
    const std::filesystem::path file = LarecompLogFolder() / name;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    file_.open(file, std::ios::out | std::ios::trunc);
  }
  void Line(const std::string& text) {
    if (!file_) return;
    const double seconds = std::chrono::duration<double>(Clock::now() - started_).count();
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "[%7.1f s] ", seconds);
    file_ << stamp << text << '\n';
    file_.flush();
  }

 private:
  std::ofstream file_;
  Clock::time_point started_;
};

void Emit(const std::string& line) {
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);  // a pipe is fully buffered; the reader wants every line now
}

// The build's progress: to the file always; to the game's log (the lines the player's support request needs); to stdout as @citypack lines when a launcher reads them. `cancel` is polled
// between sectors (the game's shutdown sets it).
class BuildSink final : public cp::ProgressSink {
 public:
  BuildSink(FileLog* file, bool emit, const std::atomic<bool>* cancel, const char* tag = "[city pack]") : file_(file), emit_(emit), cancel_(cancel), tag_(tag) {}

  void BeginPhase(const std::string& name, int total) override {
    total_ = total;
    done_ = 0;
    last_emit_ = Clock::time_point{};
    file_->Line(total > 0 ? "phase: " + name + " (" + std::to_string(total) + " steps)" : "phase: " + name);
    if (emit_) Emit(LinePhase(total, name));
  }
  void Step(const std::string& item) override {
    ++done_;
    if (!emit_) return;
    const Clock::time_point now = Clock::now();
    if (done_ == total_ || now - last_emit_ >= std::chrono::milliseconds(200)) {
      last_emit_ = now;
      Emit(LineStep(done_, total_, item));
    }
  }
  void Log(cp::LogLevel level, const std::string& text) override {
    const int code = level == cp::LogLevel::kError ? 2 : level == cp::LogLevel::kWarning ? 1 : 0;
    file_->Line(std::string(code == 2 ? "error: " : code == 1 ? "warning: " : "") + text);
    if (code == 2) {
      LARECOMP_APP_ERROR("{} {}", tag_, text);
    } else if (code == 1) {
      LARECOMP_APP_WARN("{} {}", tag_, text);
    }
    if (emit_) Emit(LineLog(code, text));
  }
  bool Cancelled() const override { return cancel_ != nullptr && cancel_->load(std::memory_order_relaxed); }

 private:
  FileLog* file_;
  bool emit_;
  const std::atomic<bool>* cancel_;
  const char* tag_;
  int total_ = 0;
  int done_ = 0;
  Clock::time_point last_emit_{};
};

// "built the hit data: 439 sectors, ..." for a finished build (city_pack_rules.h HitBuiltText).
std::string BuiltHitText(const cp::HitDataResult& result) {
  const cp::HitCensus& c = result.hit.census;
  return HitBuiltText(c.sectors, c.hi_runs + c.lod_runs, c.materials, c.textures, result.hit.hit_bytes, result.texture_bytes, result.seconds);
}

#if defined(LARECOMP_HAS_GPU)

// The game's own background build and the state the renderer is told. One per kind (the city pack, the hit data) and process (the renderer's hooks reach them through Instance() and
// HitInstance()). The hit data's build is the same machinery with another builder: same thread budget and priority, one build a session, a failure final until the next start.
class Background {
 public:
  enum class Kind : uint8_t { kPack, kHit };
  explicit Background(Kind kind) : kind_(kind) {}

  void Configure(std::filesystem::path game_root, std::filesystem::path folder) {
    std::lock_guard lock(mutex_);
    game_root_ = std::move(game_root);
    folder_ = std::move(folder);
  }

  // Stat only (and the mod archive's table): which state the pack (or the hit data) is in for the game files now. Render thread.
  mcla::gpu::city_rt::PackState State(std::string* why) {
    const bool hit = kind_ == Kind::kHit;
    if (building_.load(std::memory_order_acquire)) {
      if (why) *why = "a background build is running";
      return mcla::gpu::city_rt::PackState::kBuilding;
    }
    const cp::BuildRequest request = MakeRequest(game_root_, folder_);
    std::error_code ec;
    Facts facts;
    facts.failed = failed_.load(std::memory_order_acquire);
    facts.source_exists = std::filesystem::exists(request.source_rpf, ec);
    // "exists": a file or the stamp is there (a first start has neither; anything else is out of date or damaged).
    facts.pack_exists = hit ? std::filesystem::exists(folder_ / cp::kHitFileName, ec) || std::filesystem::exists(folder_ / cp::kHitStampFileName, ec)
                            : std::filesystem::exists(folder_ / cp::kPackFileName, ec) || std::filesystem::exists(folder_ / cp::kStampFileName, ec);
    cp::StampCheck check;
    if (facts.source_exists) check = hit ? cp::CheckHitData(MakeHitRequest(request)) : cp::CheckCityPack(request);
    facts.current = check.current;
    const mcla::gpu::city_rt::PackState state = StateFor(facts);
    if (why) {
      switch (state) {
        case mcla::gpu::city_rt::PackState::kCurrent:
          why->clear();
          break;
        case mcla::gpu::city_rt::PackState::kMissing:
          *why = hit ? "no hit data yet" : "no pack yet";
          break;
        case mcla::gpu::city_rt::PackState::kNoSource:
          *why = PathUtf8(request.source_rpf) + " is not there";
          break;
        case mcla::gpu::city_rt::PackState::kFailed:
          *why = std::string("the build of this session failed: see the ") + (hit ? "[city hit]" : "[city pack]") + " lines";
          break;
        default:
          *why = check.why;
          break;
      }
    }
    return state;
  }

  // Starts the build, once. Render thread.
  void Request() {
    std::lock_guard lock(mutex_);
    if (!ShouldStartBackgroundBuild(mcla::gpu::city_rt::PackState::kMissing, started_)) return;
    started_ = true;
    building_.store(true, std::memory_order_release);
    thread_ = std::thread([this] {
      if (kind_ == Kind::kHit) {
        RunHit();
      } else {
        Run();
      }
    });
  }

  void Stop() {
    std::thread thread;
    {
      std::lock_guard lock(mutex_);
      stop_.store(true, std::memory_order_relaxed);
      thread = std::move(thread_);
    }
    if (thread.joinable()) thread.join();
  }

 private:
  void Run() {
    FileLog file;
    BuildSink sink(&file, /*emit=*/false, &stop_);
    cp::BuildRequest request = MakeRequest(game_root_, folder_);
    request.threads = BackgroundWorkers(std::thread::hardware_concurrency());
    request.worker_start = &LowerThreadPriority;
    const cp::StampCheck why = cp::CheckCityPack(request);
    LARECOMP_APP_INFO("[city pack] building the city pack in the background for the ray traced city shadows ({}): {} worker thread(s) in background mode (below the game's own threads for the CPU and the disk), about 17 seconds of one "
                      "core in all; the game's own shadows stand until it is done; {} -> {}",
                      why.current ? "its stamp is current" : why.why, request.threads, PathUtf8(request.source_rpf), PathUtf8(folder_));
    file.Line("background build into " + PathUtf8(folder_) + " from " + PathUtf8(request.source_rpf) + ": " + (why.current ? "current" : why.why));
    const cp::BuildResult result = BuildGuarded(request, &sink);
    if (result.ok && result.up_to_date) {
      LARECOMP_APP_INFO("[city pack] the city pack in {} is current", PathUtf8(folder_));
    } else if (result.ok) {
      const std::string text = BuiltText(result.census.sectors, result.census.hi_kept_triangles, result.census.lod_kept_triangles, result.pack_bytes, result.seconds);
      LARECOMP_APP_INFO("[city pack] {} (into {}): the ray traced city shadows start by themselves", text, PathUtf8(folder_));
      file.Line("result: ok: " + text);
    } else if (result.cancelled) {
      LARECOMP_APP_INFO("[city pack] the build was cancelled (the game is closing); the next start builds it again");
      file.Line("result: cancelled");
    } else {
      failed_.store(true, std::memory_order_release);
      LARECOMP_APP_WARN("[city pack] the build failed: {}; ray traced city shadows stay off this session (the game's own shadows stand) and the next start tries again", result.error);
      file.Line("result: failed: " + result.error);
    }
    building_.store(false, std::memory_order_release);
  }

  // The hit data (the textures, then the hit file, over the pack that is in place). This thread's own priority goes down too: it writes the files and joins the workers, and with the texture set
  // (about the size of the texture file again in memory at the peak) it is the heavier of the two builds. CHECK: the build's time and memory at a quarter of the threads on a slow PC.
  void RunHit() {
    LowerThreadPriority();
    FileLog file(kHitLogName);
    BuildSink sink(&file, /*emit=*/false, &stop_, "[city hit]");
    cp::HitDataRequest request = MakeHitRequest(MakeRequest(game_root_, folder_));
    request.base.threads = BackgroundWorkers(std::thread::hardware_concurrency());
    request.base.worker_start = &LowerThreadPriority;
    const cp::StampCheck why = cp::CheckHitData(request);
    LARECOMP_APP_INFO("[city hit] building the city hit data (the city's surfaces and pictures, for the ray traced effects that read them) in the background ({}): {} worker thread(s) in background mode "
                      "(below the game's own threads for the CPU and the disk); those effects wait for it and the structures and shadows go on without it; {} -> {}",
                      why.current ? "its stamp is current" : why.why, request.base.threads, PathUtf8(request.base.source_rpf), PathUtf8(folder_));
    file.Line("background build into " + PathUtf8(folder_) + " from " + PathUtf8(request.base.source_rpf) + ": " + (why.current ? "current" : why.why));
    const cp::HitDataResult result = BuildHitGuarded(request, &sink);
    if (result.ok && result.up_to_date) {
      LARECOMP_APP_INFO("[city hit] the city hit data in {} is current", PathUtf8(folder_));
    } else if (result.ok) {
      const std::string text = BuiltHitText(result);
      LARECOMP_APP_INFO("[city hit] {} (into {}): the effects that read it start by themselves", text, PathUtf8(folder_));
      file.Line("result: ok: " + text);
    } else if (result.cancelled) {
      LARECOMP_APP_INFO("[city hit] the build was cancelled (the game is closing); the next start builds it again");
      file.Line("result: cancelled");
    } else {
      failed_.store(true, std::memory_order_release);
      LARECOMP_APP_WARN("[city hit] the build failed: {}; the effects that read the city's surfaces stay off this session and the next start tries again", result.error);
      file.Line("result: failed: " + result.error);
    }
    building_.store(false, std::memory_order_release);
  }

  const Kind kind_;
  std::mutex mutex_;
  std::thread thread_;
  bool started_ = false;
  std::atomic<bool> building_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> stop_{false};
  std::filesystem::path game_root_;
  std::filesystem::path folder_;
};

// Never destroyed (EXIT-HARD): the exit's static destruction must not run a Background (its thread member would end the process in std::terminate while joinable, and the
// hooks above can still call into it); Stop() above is what ends its thread at shutdown.
Background& Instance() {
  static Background& instance = larecomp::Leak<Background>(Background::Kind::kPack);
  return instance;
}

Background& HitInstance() {
  static Background& instance = larecomp::Leak<Background>(Background::Kind::kHit);
  return instance;
}

#endif  // LARECOMP_HAS_GPU

}  // namespace

void InstallRendererHooks(const std::filesystem::path& game_root) {
#if defined(LARECOMP_HAS_GPU)
  if (!mcla::gpu::Enabled()) return;
  Instance().Configure(game_root, mcla::gpu::CityPackFolder());
  HitInstance().Configure(game_root, mcla::gpu::CityPackFolder());
  mcla::gpu::CityPackHooks hooks;
  hooks.state = [](std::string* why) { return Instance().State(why); };
  hooks.request_build = [] { Instance().Request(); };
  // The hit data: the renderer's hit scene asks only while a consumer wants it, with the pack open (gpu/city_hit_scene.h SetFileProbe).
  hooks.hit_state = [](std::string* why) { return HitInstance().State(why); };
  hooks.request_hit_build = [] { HitInstance().Request(); };
  mcla::gpu::SetCityPackHooks(std::move(hooks));
#else
  (void)game_root;
#endif
}

void StopBackgroundBuild() {
#if defined(LARECOMP_HAS_GPU)
  Instance().Stop();
  HitInstance().Stop();
#endif
}

#if defined(LARECOMP_HAS_GPU)
namespace {

// The last thing every command line does: the result to the file log and the game's log, the @citypack|result line, the exit code.
int FinishCommand(FileLog& file, const char* tag, Status status, const std::string& text) {
  file.Line(std::string("result: ") + std::string(StatusName(status)) + ": " + text);
  if (ExitCodeFor(status) == 0) {
    LARECOMP_APP_INFO("{} {}: {}", tag, StatusName(status), text);
  } else {
    LARECOMP_APP_WARN("{} {}: {}", tag, StatusName(status), text);
  }
  Emit(LineResult(status, text));
  return ExitCodeFor(status);
}

}  // namespace
#endif

// CHECK: written without a build of larecomp (the library and the rules have unit tests; this file is only syntax checked). Run it: `larecomp.exe --build_city_pack` in the install's game
// folder with the stdout piped; expect the folder, phase, step and result lines, exit 0 (1.7 s on 12 threads), logs/city_pack_build.txt, and then a second run that prints "up to date" at
// once; a normal start with RT SHADOWS on must log "[city rt] the city pack is open" and no [city pack] build.
int RunCommandLine(const std::filesystem::path& game_root) {
#if defined(LARECOMP_HAS_GPU)
  const std::filesystem::path folder = mcla::gpu::CityPackFolder();
  FileLog file;
  BuildSink sink(&file, /*emit=*/true, nullptr);
  Emit(LineFolder(PathUtf8(folder)));
  file.Line("command line: building the city pack into " + PathUtf8(folder) + " from " + PathUtf8(game_root));

  const auto finish = [&](Status status, const std::string& text) { return FinishCommand(file, "[city pack]", status, text); };

  cp::BuildRequest request = MakeRequest(game_root, folder);
  std::error_code ec;
  if (!std::filesystem::exists(request.source_rpf, ec)) {
    return finish(Status::kNoArchive, PathUtf8(request.source_rpf) + " is not there: the game folder is not the one with the game files. The game builds the pack itself when ray traced "
                                                                      "shadows are on and the archive is found.");
  }
  request.force = larecomp::settings::build_city_pack_force.Get();
  request.threads = uint32_t(std::max(0, larecomp::settings::shader_pack_threads.Get()));  // 0: every thread but one

  const cp::BuildResult result = BuildGuarded(request, &sink);
  if (result.ok && result.up_to_date) {
    return finish(Status::kUpToDate, "The city pack in " + PathUtf8(folder) + " is up to date (its stamp matches the game files).");
  }
  if (result.ok) {
    return finish(Status::kOk, BuiltText(result.census.sectors, result.census.hi_kept_triangles, result.census.lod_kept_triangles, result.pack_bytes, result.seconds) + " (into " +
                                   PathUtf8(folder) + ")");
  }
  if (result.cancelled) return finish(Status::kCancelled, "The build was cancelled.");
  return finish(Status::kFailed, result.error + " The game builds the pack itself when ray traced shadows are on.");
#else
  (void)game_root;
  Emit(LineResult(Status::kNotNeeded, "This build has no Plume renderer, so it needs no city pack."));
  return ExitCodeFor(Status::kNotNeeded);
#endif
}

// CHECK: written without a build of larecomp. Run it in the install's game folder with the stdout piped, after --build_city_pack: `larecomp.exe --build_city_hit`; expect the folder line, the phases
// "city textures", "city texture files", "far level of detail" and "city hit sectors" with their steps, "@citypack|result|ok|built the hit data: ...", exit 0, logs/city_hit_build.txt, the two files
// and mcla_city_hit.stamp in <shader pack folder>/city; then a second run that prints "up to date" at once (the stamp), and `--check_city_hit` (exit 0, "up to date"), with
// `--check_city_hit_deep=true` the three "checking ..." phases and exit 0. Also: delete the texture file, `--check_city_hit` must say "not current" (exit 1) and the build must build both again.
int RunBuildHitCommandLine(const std::filesystem::path& game_root) {
#if defined(LARECOMP_HAS_GPU)
  const std::filesystem::path folder = mcla::gpu::CityPackFolder();
  FileLog file(kHitLogName);
  BuildSink sink(&file, /*emit=*/true, nullptr, "[city hit]");
  Emit(LineFolder(PathUtf8(folder)));
  file.Line("command line: building the city hit data into " + PathUtf8(folder) + " from " + PathUtf8(game_root));
  const auto finish = [&](Status status, const std::string& text) { return FinishCommand(file, "[city hit]", status, text); };

  cp::BuildRequest base = MakeRequest(game_root, folder);
  std::error_code ec;
  if (!std::filesystem::exists(base.source_rpf, ec)) {
    return finish(Status::kNoArchive, PathUtf8(base.source_rpf) + " is not there: the game folder is not the one with the game files. The game builds the hit data itself when an effect that "
                                                                  "reads it is on and the archive is found.");
  }
  base.force = larecomp::settings::build_city_hit_force.Get();
  base.threads = uint32_t(std::max(0, larecomp::settings::shader_pack_threads.Get()));  // 0: every thread but one

  const cp::HitDataResult result = BuildHitGuarded(MakeHitRequest(base), &sink);
  if (result.ok && result.up_to_date) return finish(Status::kUpToDate, HitCurrentText(PathUtf8(folder)));
  if (result.ok) return finish(Status::kOk, BuiltHitText(result) + " (into " + PathUtf8(folder) + ")");
  if (result.cancelled) return finish(Status::kCancelled, "The build was cancelled.");
  if (result.no_archive) return finish(Status::kNoArchive, result.error);
  if (result.no_pack) return finish(Status::kNoPack, result.error + " (--build_city_pack builds it, and so does the game when ray traced shadows are on.)");
  return finish(Status::kFailed, result.error + " The game builds the hit data itself when an effect that reads it is on.");
#else
  (void)game_root;
  Emit(LineResult(Status::kNotNeeded, "This build has no Plume renderer, so it needs no city hit data."));
  return ExitCodeFor(Status::kNotNeeded);
#endif
}

// CHECK: written without a build of larecomp (see RunBuildHitCommandLine for the run to make).
int RunCheckHitCommandLine(const std::filesystem::path& game_root, bool deep) {
#if defined(LARECOMP_HAS_GPU)
  const std::filesystem::path folder = mcla::gpu::CityPackFolder();
  FileLog file(kHitCheckLogName);
  BuildSink sink(&file, /*emit=*/true, nullptr, "[city hit]");
  Emit(LineFolder(PathUtf8(folder)));
  file.Line(std::string("command line: ") + (deep ? "reading back" : "checking") + " the city hit data in " + PathUtf8(folder) + " for " + PathUtf8(game_root));
  const auto finish = [&](Status status, const std::string& text) { return FinishCommand(file, "[city hit]", status, text); };

  const cp::BuildRequest base = MakeRequest(game_root, folder);
  std::error_code ec;
  if (!std::filesystem::exists(base.source_rpf, ec)) {
    return finish(Status::kNoArchive, PathUtf8(base.source_rpf) + " is not there: the game folder is not the one with the game files.");
  }
  // The stat check first, deep or not: files that are not for these game files and this city pack are not worth reading back.
  cp::StampCheck stamp;
  try {
    stamp = cp::CheckHitData(MakeHitRequest(base));
  } catch (const std::exception& e) {
    return finish(Status::kFailed, std::string("the check stopped unexpectedly: ") + e.what());
  }
  if (!stamp.current) return finish(Status::kNotCurrent, stamp.why);
  if (!deep) return finish(Status::kUpToDate, HitCurrentText(PathUtf8(folder)));

  cp::HitDataCheck check;
  try {
    check = cp::DeepCheckHitData(folder, cp::HitDataCheckParts{}, &sink);
  } catch (const std::exception& e) {
    return finish(Status::kFailed, std::string("the check stopped unexpectedly: ") + e.what());
  }
  for (const std::string& problem : check.problems) sink.Log(cp::LogLevel::kWarning, problem);
  if (check.unreadable) return finish(Status::kFailed, check.error);
  const std::string text = DeepCheckText(check.pack_problems, check.hit_problems, check.texture_problems, check.pictures, check.textures_skipped,
                                         check.pack_seconds + check.hit_seconds + check.texture_seconds);
  return finish(check.ok ? Status::kUpToDate : Status::kFailed, text);
#else
  (void)game_root;
  (void)deep;
  Emit(LineResult(Status::kNotNeeded, "This build has no Plume renderer, so it needs no city hit data."));
  return ExitCodeFor(Status::kNotNeeded);
#endif
}

}  // namespace larecomp::citypackbuild

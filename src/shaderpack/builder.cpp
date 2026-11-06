// LARecomp shader pack builder -- the orchestration (see builder.h).
//
// Replaces native/tools/p-pack.ps1 and the three Python builders it runs (build_shader_pack.py v2,
// build_shader_pack_v3.py, build_shader_pack_v8.py, by mzzvxm) with one pass:
//   read the RPF's Shaders folder and the image -> containers (key, vertex elements, flags, texture
//   mask) -> per key, on a pool: translate, check the HLSL, compile the variants of the formats
//   asked, check the blobs and the vertex position -> write the pack in key order -> HDR step ->
//   stamp.

#include "shaderpack/builder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#ifdef _WIN32
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

#include "native_gfx/shader_identity.h"
#include "shaderpack/contract_checks.h"
#include "shaderpack/hash.h"
#include "shaderpack/hlsl_source.h"
#include "shaderpack/image_scan.h"
#include "shaderpack/pack_writer.h"
#include "shaderpack/rpf_shaders.h"

namespace larecomp::shaderpack {

namespace {

using Clock = std::chrono::steady_clock;

// The retail Complete Edition: 2056 distinct shaders (a different count is a warning only).
constexpr size_t kExpectedShaderCount = 2056;
// About what one active dxc compiler instance costs (GUESS: measure; the automatic thread count
// leaves this much free memory per worker).
constexpr uint64_t kBytesPerWorker = 300ull * 1024 * 1024;
constexpr unsigned kMaxAutoThreads = 16;

double SecondsSince(Clock::time_point since) {
  return std::chrono::duration<double>(Clock::now() - since).count();
}

std::string PathText(const std::filesystem::path& path) {
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) text += '\n';
    text += lines[i];
  }
  return text;
}

class NullSink : public ProgressSink {
 public:
  void BeginPhase(const std::string&, int) override {}
  void Step(const std::string&) override {}
  void Log(LogLevel, const std::string&) override {}
  bool Cancelled() const override { return false; }
};

// Points the shaderpack log at the sink for the length of a build (the readers log through log.h).
class ScopedLogRedirect {
 public:
  explicit ScopedLogRedirect(ProgressSink* sink) : active_(sink != nullptr) {
    if (active_) {
      previous_ = SetLogCallback(
          [sink](LogLevel level, const std::string& text) { sink->Log(level, text); });
    }
  }
  ~ScopedLogRedirect() {
    if (active_) SetLogCallback(std::move(previous_));
  }
  ScopedLogRedirect(const ScopedLogRedirect&) = delete;
  ScopedLogRedirect& operator=(const ScopedLogRedirect&) = delete;

 private:
  bool active_ = false;
  LogCallback previous_;
};

uint64_t AvailablePhysicalBytes() {
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) return status.ullAvailPhys;
  return 0;
#else
  const long pages = sysconf(_SC_AVPHYS_PAGES);
  const long page_size = sysconf(_SC_PAGESIZE);
  return pages > 0 && page_size > 0 ? uint64_t(pages) * uint64_t(page_size) : 0;
#endif
}

unsigned ChooseThreadCount(unsigned requested, size_t jobs) {
  unsigned threads = requested;
  if (threads == 0) {
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 4;
    const uint64_t available = AvailablePhysicalBytes();
    const uint64_t by_memory = available != 0 ? std::max<uint64_t>(1, available / kBytesPerWorker) : hardware;
    threads = unsigned(std::min<uint64_t>({hardware, by_memory, kMaxAutoThreads}));
  }
  threads = std::min<size_t>(threads, jobs);
  return std::max(1u, threads);
}

void LowerThreadPriority() {
#ifdef _WIN32
  // The overlay, the log and the render thread keep their cores.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

// ---------------------------------------------------------------------------
// One shader: translate, check, compile, prove
// ---------------------------------------------------------------------------

struct JobContext {
  const ContainerSet* set = nullptr;
  uint32_t compile_formats = 0;  // formats to compile
  uint32_t reuse_formats = 0;    // formats to take from `reuse` when the entry has the blob
  const PackFile* reuse = nullptr;
  uint32_t position_stride = 1;
};

struct JobOutput {
  Status status = Status::kOk;
  bool retryable = false;  // a compiler failure that may have been transient (memory)
  std::string label;       // "name [key]"
  std::string message;
  uint32_t spec_mask = 0;
  bool rounds_alpha = false;
  uint32_t compiled = 0;
  uint32_t reused = 0;
  std::array<std::vector<uint8_t>, kBlobSlots> blobs;
};

void RunJob(DxcWorker& dxc, size_t index, const JobContext& context, JobOutput& out) {
  out = JobOutput{};
  const ShaderSource& source = context.set->shaders[index];
  out.label = std::format("{} [{}]", source.name, Hex64(source.key));
  const auto fail = [&](Status status, bool retryable, std::string message) {
    out.status = status;
    out.retryable = retryable;
    out.message = std::move(message);
  };

  // 1. Translate.
  TranslatedShader translated;
  std::string error;
  if (!TranslateContainer(source.container, &translated, &error)) {
    fail(Status::kTranslateFailed, false, out.label + ": " + error);
    return;
  }
  if (translated.stage != source.stage) {
    fail(Status::kTranslateFailed, false,
         out.label + ": the translator and the container disagree on the shader's stage");
    return;
  }
  if (translated.spec_mask > 0xFF) {
    fail(Status::kTranslateFailed, false,
         std::format("{}: spec mask {:#x} does not fit the pack's byte", out.label, translated.spec_mask));
    return;
  }
  out.spec_mask = translated.spec_mask;

  // 2. The HLSL text and the control flow, against the contract.
  Problems problems;
  CheckControlFlow(out.label, translated.control_flow, &problems);
  HlslCheckInput text;
  text.name = out.label;
  text.hlsl = translated.hlsl;
  text.stage = source.stage;
  text.spec_mask = translated.spec_mask;
  text.ps_outputs = (source.flags >> kFlagPixelOutputsShift) & kFlagPixelOutputsMask;
  CheckHlslText(text, &problems, &out.rounds_alpha);
  if (!problems.empty()) {
    fail(Status::kContractFailed, false, JoinLines(problems));
    return;
  }

  // 3. The blobs: variant 0, and variant 1 with the whole spec mask when there is one.
  const int variants = translated.spec_mask != 0 ? 2 : 1;
  const PackFile::Entry* old_entry = context.reuse ? context.reuse->Find(source.key) : nullptr;
  std::array<bool, kBlobSlots> compiled_here{};
  for (int variant = 0; variant < variants; ++variant) {
    for (const bool spirv : {false, true}) {
      const uint32_t format = spirv ? kSpirv : kDxil;
      if (((context.compile_formats | context.reuse_formats) & format) == 0) continue;
      const int slot = BlobIndexOf(spirv, variant);

      // A format the existing pack has: its blob is what a compile would give (a pure function of
      // the same inputs), provided it is intact.
      if ((context.reuse_formats & format) != 0 && old_entry) {
        const std::span<const uint8_t> old = context.reuse->BlobBytes(old_entry->blobs[slot]);
        if (!old.empty() && mcla::native_gfx::Fnv1a64(old.data(), old.size()) == old_entry->blobs[slot].fnv) {
          out.blobs[slot].assign(old.begin(), old.end());
          ++out.reused;
          continue;
        }
      }

      CompileRequest request;
      request.hlsl = translated.hlsl;
      request.stage = source.stage;
      request.spec_constants = variant != 0 ? translated.spec_mask : 0;
      request.name = out.label;
      const DxcStatus status = spirv ? dxc.CompileSpirv(request, &out.blobs[slot])
                                     : dxc.CompileDxil(request, &out.blobs[slot]);
      if (!status.ok()) {
        fail(status.error == DxcError::kNotSigned ? Status::kDxilNotSigned : Status::kCompileFailed,
             status.error == DxcError::kCompileFailed, status.message);
        return;
      }
      compiled_here[slot] = true;
      ++out.compiled;
    }
  }

  // 4. The blobs against the contract, and the position of the vertex shaders (only blobs made
  // here: the ones taken from the existing pack were checked when they were built).
  const bool writes_oc0 = source.stage == ShaderStage::kPixel && (text.ps_outputs & kPixelOutputColor0) != 0;
  const auto compiled_blob = [&](bool spirv, int variant) -> std::span<const uint8_t> {
    const int slot = BlobIndexOf(spirv, variant);
    return compiled_here[slot] ? std::span<const uint8_t>(out.blobs[slot]) : std::span<const uint8_t>();
  };
  const bool prove_dxil_position =
      context.position_stride <= 1 || (index % context.position_stride) == 0;
  for (int variant = 0; variant < variants; ++variant) {
    BlobContractInput blob_input;
    blob_input.name = out.label;
    blob_input.stage = source.stage;
    blob_input.variant = variant;
    blob_input.writes_oc0 = writes_oc0;
    blob_input.dxil = compiled_blob(false, variant);
    blob_input.spirv = compiled_blob(true, variant);
    CheckBlobContract(blob_input, &problems);
    if (source.stage == ShaderStage::kVertex) {
      if (!blob_input.spirv.empty()) CheckSpirvPosition(out.label, variant, blob_input.spirv, &problems);
      if (!blob_input.dxil.empty() && prove_dxil_position) {
        CheckDxilPosition(dxc, out.label, variant, blob_input.dxil, &problems);
      }
    }
  }
  if (!problems.empty()) fail(Status::kContractFailed, false, JoinLines(problems));
}

// A job that throws (std::bad_alloc when memory runs out) is a failure of that job, retryable.
void RunJobGuarded(DxcWorker& dxc, size_t index, const JobContext& context, JobOutput& out) {
  try {
    RunJob(dxc, index, context, out);
  } catch (const std::exception& exception) {
    out.status = Status::kCompileFailed;
    out.retryable = true;
    out.message = std::format("{}: {}", context.set->shaders[index].name, exception.what());
  } catch (...) {
    out.status = Status::kCompileFailed;
    out.retryable = true;
    out.message = context.set->shaders[index].name + ": unknown exception";
  }
}

struct JobsOutcome {
  Status status = Status::kOk;
  std::string message;
  std::string first_shader;
  std::vector<std::string> problems;
};

// Runs every job on `threads` workers. The outputs are indexed by shader (key order), so what the
// caller builds from them never depends on which worker finished first.
JobsOutcome RunAllJobs(const JobContext& context, const std::shared_ptr<DxcRuntime>& runtime,
                       unsigned threads, ProgressSink& sink, std::vector<JobOutput>& outputs) {
  JobsOutcome outcome;
  const size_t count = context.set->shaders.size();
  outputs.assign(count, JobOutput{});

  // 0: not run, 1: done, 2: failed.
  std::unique_ptr<std::atomic<uint8_t>[]> state(new std::atomic<uint8_t>[count]());
  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};
  std::atomic<unsigned> running{threads};
  std::mutex setup_mutex;
  std::string setup_error;

  const auto work = [&]() {
    LowerThreadPriority();
    DxcStatus status;
    std::unique_ptr<DxcWorker> worker = runtime->CreateWorker(&status);
    if (!worker) {
      {
        std::lock_guard<std::mutex> lock(setup_mutex);
        if (setup_error.empty()) setup_error = status.message;
      }
      stop.store(true);
      running.fetch_sub(1);
      return;
    }
    for (;;) {
      if (stop.load(std::memory_order_relaxed)) break;
      const size_t index = next.fetch_add(1);
      if (index >= count) break;
      RunJobGuarded(*worker, index, context, outputs[index]);
      const bool ok = outputs[index].status == Status::kOk;
      state[index].store(ok ? 1 : 2, std::memory_order_release);
      if (!ok) stop.store(true);
    }
    worker.reset();
    running.fetch_sub(1);
  };

  sink.BeginPhase("Compiling shaders", int(count));
  std::vector<char> reported(count, 0);
  size_t reported_low = 0;
  const auto report = [&]() {
    const size_t high = std::min(count, next.load());
    for (size_t i = reported_low; i < high; ++i) {
      if (!reported[i] && state[i].load(std::memory_order_acquire) != 0) {
        reported[i] = 1;
        sink.Step(context.set->shaders[i].name);
      }
    }
    while (reported_low < count && reported[reported_low]) ++reported_low;
  };

  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
  bool cancelled = false;
  while (running.load() != 0) {
    if (!cancelled && sink.Cancelled()) {
      cancelled = true;
      stop.store(true);
    }
    report();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  for (std::thread& thread : pool) thread.join();
  report();

  if (cancelled) {
    outcome.status = Status::kCancelled;
    outcome.message = "The shader build was cancelled.";
    return outcome;
  }
  if (!setup_error.empty()) {
    outcome.status = Status::kCompileFailed;
    outcome.message = "The shader compiler could not start a worker: " + setup_error;
    return outcome;
  }

  std::vector<size_t> failed;
  for (size_t i = 0; i < count; ++i) {
    if (state[i].load() == 2) failed.push_back(i);
  }
  if (failed.empty()) return outcome;

  // A failure that is not the compiler's own (a contract check, the translator) is final. A compiler
  // failure may be a transient one (memory): the shaders that did not finish run again on one
  // thread, once.
  const auto final_failure = [&](size_t index) {
    outcome.status = outputs[index].status;
    outcome.message = outputs[index].message;
    outcome.first_shader = outputs[index].label;
    for (const size_t other : failed) {
      if (other != index && outcome.problems.size() < 10) outcome.problems.push_back(outputs[other].message);
    }
  };
  for (const size_t index : failed) {
    if (!outputs[index].retryable) {
      final_failure(index);
      return outcome;
    }
  }
  SHADERPACK_LOG_WARN("{} shader(s) failed in the compiler (first: {}); trying the rest on one thread once",
                      failed.size(), outputs[failed.front()].label);
  DxcStatus status;
  std::unique_ptr<DxcWorker> worker = runtime->CreateWorker(&status);
  if (!worker) {
    final_failure(failed.front());
    return outcome;
  }
  for (size_t i = 0; i < count; ++i) {
    if (state[i].load() == 1) continue;
    if (sink.Cancelled()) {
      outcome.status = Status::kCancelled;
      outcome.message = "The shader build was cancelled.";
      return outcome;
    }
    RunJobGuarded(*worker, i, context, outputs[i]);
    if (outputs[i].status != Status::kOk) {
      failed.assign(1, i);
      final_failure(i);
      return outcome;
    }
    state[i].store(1);
    sink.Step(context.set->shaders[i].name);
  }
  return outcome;
}

}  // namespace

// ---------------------------------------------------------------------------

const char* StatusName(Status status) {
  switch (status) {
    case Status::kOk:
      return "ok";
    case Status::kUpToDate:
      return "up to date";
    case Status::kCancelled:
      return "cancelled";
    case Status::kBadRequest:
      return "bad request";
    case Status::kMissingInput:
      return "missing game files";
    case Status::kMissingDxc:
      return "missing shader compiler";
    case Status::kDxilNotSigned:
      return "DXIL not signed";
    case Status::kTranslateFailed:
      return "translation failed";
    case Status::kCompileFailed:
      return "compile failed";
    case Status::kContractFailed:
      return "contract check failed";
    case Status::kIoError:
      return "file error";
  }
  return "unknown";
}

BuildResult BuildShaderPacks(const BuildRequest& request) {
  const Clock::time_point total_start = Clock::now();
  BuildResult result;
  NullSink null_sink;
  ProgressSink& sink = request.sink ? *request.sink : null_sink;
  ScopedLogRedirect log_redirect(request.sink);

  const auto finish = [&](Status status, std::string message, std::string shader = std::string()) -> BuildResult {
    result.status = status;
    result.message = std::move(message);
    result.first_shader = std::move(shader);
    result.seconds_total = SecondsSince(total_start);
    return result;
  };

  // ---- the request
  const uint32_t wanted = request.formats & (kDxil | kSpirv);
  if (wanted == 0) return finish(Status::kBadRequest, "No shader format was asked for.");
#ifndef _WIN32
  if ((wanted & kDxil) != 0) return finish(Status::kBadRequest, "DXIL shaders can only be built on Windows.");
#endif
  if (request.image.data == nullptr || request.image.size == 0) {
    return finish(Status::kBadRequest, "The game's image is not available to read the shaders from.");
  }
  if (request.game_root.empty() || request.out_dir.empty()) {
    return finish(Status::kBadRequest, "The game folder and the output folder have to be given.");
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(request.out_dir, ec);
    if (ec) {
      return finish(Status::kIoError, std::format("Cannot create the shader folder {}: {}",
                                                  PathText(request.out_dir), ec.message()));
    }
  }
  const std::filesystem::path pack_path = request.out_dir / kPackFileName;
  const std::filesystem::path pack_temporary = request.out_dir / (std::string(kPackFileName) + ".tmp");
  const std::filesystem::path stamp_path = request.out_dir / kStampFileName;
  {
    std::error_code ec;
    std::filesystem::remove(pack_temporary, ec);  // left by a build that was killed
  }

  // ---- what is there
  const GameFilesQuick quick{request.game_root, request.dxc_dir};
  const StampCheck check = CheckStamp(request.out_dir, wanted, quick, request.hdr_step);
  if (!request.force && check.main_ok && check.hdr_ok) {
    result.formats_built = check.stamped_formats;
    result.message = "The shader packs are up to date.";
    result.status = Status::kUpToDate;
    result.seconds_total = SecondsSince(total_start);
    return result;
  }
  if (request.force) {
    SHADERPACK_LOG_INFO("building the shader packs: forced");
  } else {
    SHADERPACK_LOG_INFO("building the shader packs: {}", check.why.empty() ? std::string("no valid stamp") : check.why);
  }

  // The pack on disk is the right one for the inputs and the compiler (what may differ: its formats,
  // the HDR pack, the game files' times).
  bool existing_ok = !request.force && check.inputs_match;
  // Nothing of the main pack needs redoing.
  bool main_stays = existing_ok && check.formats_ok;
  // The RPF is read unless the main pack stays and its game files are known to be the same.
  const bool read_rpf = !(main_stays && !check.content_check_needed);

  // ---- 1. the game files
  const Clock::time_point read_start = Clock::now();
  RpfShaders rpf;
  if (read_rpf) {
    bool phase_started = false;
    const RpfProgress progress = [&](size_t, size_t total, const std::string& path) {
      if (!phase_started) {
        sink.BeginPhase("Reading game files", int(total));
        phase_started = true;
      }
      if (!path.empty()) sink.Step(path);
      return !sink.Cancelled();
    };
    const RpfShadersResult read = ReadRpfShaders(request.game_root, rpf, progress);
    if (!read.ok()) {
      return finish(read.code == RpfShadersCode::kCancelled ? Status::kCancelled : Status::kMissingInput,
                    read.code == RpfShadersCode::kCancelled
                        ? std::string("The shader build was cancelled.")
                        : "The game files are incomplete: " + read.message);
    }
  }
  if (sink.Cancelled()) return finish(Status::kCancelled, "The shader build was cancelled.");
  ImageScan image = ScanImage(request.image.data, request.image.size, request.image.base_address);
  if (image.containers.empty()) {
    return finish(Status::kMissingInput,
                  "The game's executable holds no shader containers (the game files are not the ones "
                  "this version expects).");
  }

  ContainerSet set;
  {
    std::string error;
    bool cancelled = false;
    if (!GatherContainers(read_rpf ? &rpf : nullptr, &image, &set, &error, &cancelled)) {
      return finish(cancelled ? Status::kCancelled : Status::kTranslateFailed,
                    "A shader container could not be read: " + error);
    }
  }
  rpf = RpfShaders{};
  image = ImageScan{};
  if (read_rpf && set.rpf_containers == 0) {
    return finish(Status::kMissingInput, "xarchive_cache.rpf holds no shader containers.");
  }
  result.containers_seen = set.rpf_containers + set.image_containers;
  result.shaders = set.shaders.size();
  result.vertex_shaders = set.vertex_shaders;
  result.pixel_shaders = set.pixel_shaders;
  if (read_rpf && set.shaders.size() != kExpectedShaderCount) {
    SHADERPACK_LOG_WARN("{} distinct shaders, the retail game has {}; carrying on, the shader contract does "
                        "not depend on the count",
                        set.shaders.size(), kExpectedShaderCount);
  }
  SHADERPACK_LOG_INFO("{} containers ({} in the RPF, {} in the image), {} distinct shaders ({} vertex, {} pixel)",
                      result.containers_seen, set.rpf_containers, set.image_containers, set.shaders.size(),
                      set.vertex_shaders, set.pixel_shaders);

  // Only a time of the game files moved: the same containers mean the same packs (a copied game folder).
  if (existing_ok && check.content_check_needed) {
    uint64_t stamped_rpf = 0;
    uint64_t stamped_image = 0;
    const bool same = check.stamp.GetHex("rpf_shaders_xxh3", &stamped_rpf) && stamped_rpf == set.rpf_fingerprint &&
                      check.stamp.GetHex("xex_containers_xxh3", &stamped_image) &&
                      stamped_image == set.image_fingerprint;
    if (!same) {
      SHADERPACK_LOG_INFO("the game files are not the ones the packs were built from");
      existing_ok = false;
      main_stays = false;
    }
  }
  result.seconds_read = SecondsSince(read_start);

  // What the main pack will carry, and what of it is taken from the pack that is there.
  uint32_t reuse_formats = 0;
  if (existing_ok && !main_stays) reuse_formats = check.stamped_formats & (kDxil | kSpirv);
  uint32_t pack_formats = main_stays ? (check.stamped_formats & (kDxil | kSpirv)) : (wanted | reuse_formats);

  // The HDR variant step runs after a main build and when only the HDR pack is stale.
  const bool run_hdr = request.hdr_step != nullptr && (!main_stays || !check.hdr_ok);
  result.hdr_only = main_stays && run_hdr;

  // ---- 2. the compiler (only when something is to be compiled)
  std::shared_ptr<DxcRuntime> runtime;
  const auto load_compiler = [&]() -> bool {
    DxcRuntime::LoadOptions options;
    options.need_dxil = (pack_formats & kDxil) != 0;
    DxcStatus status;
    runtime = DxcRuntime::Load(request.dxc_dir, options, &status);
    if (!runtime) {
      result.status = status.error == DxcError::kNotSigned ? Status::kDxilNotSigned : Status::kMissingDxc;
      result.message = std::format("The shader compiler could not be loaded from {}: {}",
                                   PathText(request.dxc_dir), status.message);
      return false;
    }
    SHADERPACK_LOG_INFO("shader compiler {} {}", PathText(runtime->compiler_path()),
                        runtime->version().empty() ? std::string("(version unknown)") : runtime->version());
    if (!runtime->notes().empty()) SHADERPACK_LOG_WARN("{}", runtime->notes());
    return true;
  };

  // The stamp that goes with the packs, finished at the end.
  Stamp stamp;
  uint64_t pack_xxh3 = 0;

  // ---- 3. the main pack
  if (!main_stays) {
    // The pack that is there, read before anything is written (the new one replaces it).
    PackFile old_pack;
    if (reuse_formats != 0) {
      std::string why;
      if (!old_pack.Open(pack_path, &why) || (old_pack.formats() & reuse_formats) != reuse_formats) {
        SHADERPACK_LOG_WARN("the existing pack cannot be reused ({}); compiling every format", why);
        reuse_formats = 0;
        pack_formats = wanted;
      }
    }
    if (!load_compiler()) {
      result.seconds_total = SecondsSince(total_start);
      return result;
    }

    const Clock::time_point compile_start = Clock::now();
    JobContext context;
    context.set = &set;
    context.compile_formats = pack_formats & ~reuse_formats;
    context.reuse_formats = reuse_formats;
    context.reuse = reuse_formats != 0 ? &old_pack : nullptr;
    context.position_stride = std::max(1u, request.position_proof_stride);

    result.threads = ChooseThreadCount(request.threads, set.shaders.size());
    SHADERPACK_LOG_INFO("compiling {} shaders on {} threads ({}{})", set.shaders.size(), result.threads,
                        (context.compile_formats & kDxil) != 0 ? "DXIL " : "",
                        (context.compile_formats & kSpirv) != 0 ? "SPIR-V" : "");
    std::vector<JobOutput> outputs;
    const JobsOutcome outcome = RunAllJobs(context, runtime, result.threads, sink, outputs);
    if (outcome.status != Status::kOk) {
      result.problems = outcome.problems;
      return finish(outcome.status,
                    outcome.status == Status::kCancelled
                        ? outcome.message
                        : "The shaders could not be built from the game files: " + outcome.message,
                    outcome.first_shader);
    }
    uint32_t rounding_sources = 0;
    for (const JobOutput& output : outputs) {
      result.blobs_compiled += output.compiled;
      result.blobs_reused += output.reused;
      if (output.rounds_alpha) ++rounding_sources;
    }
    result.seconds_compile = SecondsSince(compile_start);
    SHADERPACK_LOG_INFO("compiled {} blobs, {} taken from the existing pack; {} sources round the alpha before "
                        "alpha to coverage",
                        result.blobs_compiled, result.blobs_reused, rounding_sources);

    // ---- the pack, in key order
    const Clock::time_point write_start = Clock::now();
    sink.BeginPhase("Writing the shader pack", 0);
    std::vector<PackEntryInput> entries(set.shaders.size());
    for (size_t i = 0; i < set.shaders.size(); ++i) {
      const ShaderSource& source = set.shaders[i];
      PackEntryInput& entry = entries[i];
      entry.key = source.key;
      entry.stage = source.stage;
      entry.spec_mask = outputs[i].spec_mask;
      entry.flags = source.flags;
      entry.texture_fetch_mask = source.texture_fetch_mask;
      entry.vertex_elements = std::span<const uint32_t>(source.vertex_elements);
      for (int slot = 0; slot < kBlobSlots; ++slot) entry.blobs[slot] = std::span<const uint8_t>(outputs[i].blobs[slot]);
    }
    const PackWriteResult written = WritePackFile(pack_temporary, entries, pack_formats);
    if (!written.ok) return finish(Status::kIoError, "The shader pack could not be written: " + written.error);
    outputs.clear();
    entries.clear();

    // What the renderer's Validate and VerifyBlob would say about it, before it is put in place.
    ValidateOptions validate_options;
    validate_options.formats = pack_formats;
    const ValidateResult validated = ValidatePack(pack_temporary, validate_options);
    if (!validated.ok) {
      std::error_code ec;
      std::filesystem::remove(pack_temporary, ec);
      return finish(Status::kContractFailed,
                    "The shader pack that was built does not pass the renderer's own checks: " +
                        JoinLines(validated.problems));
    }
    if (validated.xxh3 != written.xxh3) {
      std::error_code ec;
      std::filesystem::remove(pack_temporary, ec);
      return finish(Status::kIoError, "The shader pack that was written does not read back as it was written.");
    }

    // The stamp goes first: while the pack is replaced there is none to vouch for the old one.
    {
      std::error_code ec;
      std::filesystem::remove(stamp_path, ec);
    }
    std::string error;
    if (!CommitFile(pack_temporary, pack_path, &error)) return finish(Status::kIoError, error);
    pack_xxh3 = written.xxh3;
    result.pack_bytes = written.bytes;
    result.pack_xxh3 = written.xxh3;
    result.formats_built = pack_formats;

    // ---- the stamp's lines
    StampInputs inputs;
    inputs.formats = pack_formats;
    inputs.xex = StatFile(request.game_root / "default.xex");
    inputs.rpf = StatFile(request.game_root / "xarchive_cache.rpf");
    inputs.rpf_fingerprint = set.rpf_fingerprint;
    inputs.image_fingerprint = set.image_fingerprint;
    inputs.dxc_compiler = StatFile(request.dxc_dir / DxcCompilerFileName());
    if (inputs.dxc_compiler.exists) HashFile(request.dxc_dir / DxcCompilerFileName(), &inputs.dxc_compiler_xxh3);
    if (!DxcDilFileName().empty()) {
      inputs.dxc_dil = StatFile(request.dxc_dir / DxcDilFileName());
      if (inputs.dxc_dil.exists) HashFile(request.dxc_dir / DxcDilFileName(), &inputs.dxc_dil_xxh3);
    }
    inputs.dxc_version = runtime->version();
    inputs.pack_size = written.bytes;
    inputs.pack_xxh3 = written.xxh3;
    inputs.app_version = request.app_version;
    stamp = MakeMainStamp(inputs);
    result.seconds_write = SecondsSince(write_start);
    SHADERPACK_LOG_INFO("wrote {} ({} bytes, xxh3 {}, {} shaders)", PathText(pack_path), written.bytes,
                        Hex64(written.xxh3), result.shaders);
  } else {
    // The main pack stays; the stamp is the one that was there (its game file lines refreshed below).
    stamp = check.stamp;
    result.formats_built = pack_formats;
    uint64_t size = 0;
    stamp.GetU64("pack_size", &size);
    result.pack_bytes = size;
    stamp.GetHex("pack_xxh3", &pack_xxh3);
    result.pack_xxh3 = pack_xxh3;
    if (check.content_check_needed) {
      const FileStat xex = StatFile(request.game_root / "default.xex");
      const FileStat rpf_file = StatFile(request.game_root / "xarchive_cache.rpf");
      stamp.Set("xex_size", xex.size);
      stamp.Set("xex_mtime", std::to_string(xex.mtime_us));
      stamp.Set("rpf_size", rpf_file.size);
      stamp.Set("rpf_mtime", std::to_string(rpf_file.mtime_us));
    }
  }

  // ===========================================================================================
  // HDR VARIANT STEP (written separately: hdr_variant.{h,cpp}; see HdrStep in builder.h).
  // Called once, here: after the main pack is in place and its stamp lines are known, before the
  // stamp is written. `set` holds the containers read above (only the image's when the main pack
  // stayed), `runtime` the loaded compiler. The lines it returns go into the stamp with the three
  // the builder owns (kStampHdrFormats, kStampHdrMainPackXxh3, kStampHdrPackSize). A step that
  // fails leaves the main pack as it is and the stamp without HDR lines.
  // ===========================================================================================
  stamp.RemovePrefix(kStampHdrPrefix);
  if (run_hdr) {
    const Clock::time_point hdr_start = Clock::now();
    {
      // The old stamp no longer vouches for the HDR pack that is about to be replaced.
      std::error_code ec;
      std::filesystem::remove(stamp_path, ec);
    }
    if (!runtime && !load_compiler()) {
      // The main pack is fine without it: HDR stays off, and the next start tries again.
      result.hdr_message = result.message;
      result.status = Status::kOk;
      result.message.clear();
    } else {
      HdrStepInput input;
      input.containers = &set;
      input.dxc = runtime;
      input.out_dir = request.out_dir;
      input.formats = pack_formats;
      input.threads = ChooseThreadCount(request.threads, set.shaders.size());
      input.sink = &sink;
      const HdrStepResult hdr = request.hdr_step->Build(input);
      result.hdr_built = hdr.built;
      result.hdr_message = hdr.message;
      if (hdr.built) {
        for (const StampLine& line : hdr.stamp_lines) stamp.Set(line.key, line.value);
        stamp.Set(kStampHdrFormats, uint64_t(pack_formats));
        stamp.SetHex(kStampHdrMainPackXxh3, pack_xxh3);
        stamp.Set(kStampHdrPackSize, StatFile(request.out_dir / kHdrPackFileName).size);
      } else {
        SHADERPACK_LOG_WARN("the HDR variant pack was not built: {}", hdr.message);
      }
    }
    result.seconds_hdr = SecondsSince(hdr_start);
  } else if (main_stays && check.hdr_ok && request.hdr_step) {
    // Still current: keep its lines as they were.
    for (const StampLine& line : check.stamp.lines()) {
      if (line.key.starts_with(kStampHdrPrefix)) stamp.Set(line.key, line.value);
    }
  }

  // ---- the stamp, last
  {
    std::string error;
    if (!stamp.Write(stamp_path, &error)) {
      return finish(Status::kIoError, "The shader stamp could not be written: " + error);
    }
  }
  if (main_stays && !run_hdr) {
    result.message = "The shader packs are up to date (only the game files' times had changed).";
    result.status = Status::kUpToDate;
    result.seconds_total = SecondsSince(total_start);
    return result;
  }
  return finish(Status::kOk,
                std::format("Built {} shaders in {:.1f} s.", result.shaders, SecondsSince(total_start)));
}

}  // namespace larecomp::shaderpack

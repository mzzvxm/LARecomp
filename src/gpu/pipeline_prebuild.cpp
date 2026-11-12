// MCLA GPU renderer -- the pipeline prebuild. See pipeline_prebuild.h.

#include "pipeline_prebuild.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "edram.h"
#include "frame.h"
#include "gpu.h"
#include "layout.h"
#include "pipelines.h"
#include "shaders.h"
#include "temporal_backend.h"

REXCVAR_DECLARE(bool, mcla_gpu_pipeline_cache);

namespace mcla::gpu {

namespace {

constexpr const char* kStampMagic = "mcla pipeline prebuild 1";

// What the game's renderer builds on, in the order it makes it (and unmakes it, reversed).
// Held by pointer so that a driver that never answers can be left as it is (kStalled).
struct Parts {
  Device device;
  FrameRing frames;
  BindlessHeap heap;
  GuestLayout layout;
  ShaderStore shaders;
  Pipelines pipelines;
};

std::string Utf8(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

// A file as the stamp knows it: its size and its time of last write (a pack that is built again
// or an exe that is replaced changes it), or "none".
std::string FileIdentity(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return "none";
  }
  const auto time = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return std::format("{}:?", size);
  }
  return std::format("{}:{}", size, static_cast<long long>(time.time_since_epoch().count()));
}

std::string OneLine(std::string text) {
  for (char& c : text) {
    if (c == '\r' || c == '\n' || c == '\t') {
      c = ' ';
    }
  }
  return text;
}

std::string JoinCounts(const std::vector<uint32_t>& counts) {
  std::string text;
  for (const uint32_t count : counts) {
    text += (text.empty() ? "" : ",") + std::to_string(count);
  }
  return text;
}

// What a prebuild is made for. Every line is part of the key; the stamp file is this text plus
// comment lines (starting with '#') that nobody compares.
std::string StampText(Parts& parts, const Pipelines::SetupList& list,
                      const std::vector<uint32_t>& samples) {
  const plume::RenderDeviceDescription& d = parts.device.description();
  std::string text;
  const auto line = [&text](const std::string& s) { text += s + "\n"; };
  line(kStampMagic);
  line(std::format("backend={}", BackendName()));
  line("adapter=" + OneLine(d.name));
  line(std::format("vendor={}", static_cast<int>(d.vendor)));
  line(std::format("driver={:#x}", d.driverVersion));
  line("exe=" + FileIdentity(rex::filesystem::GetExecutablePath()));
  line("pack=" + FileIdentity(parts.shaders.path()));
  line("hdr_pack=" + (parts.shaders.has_hdr_variants() ? FileIdentity(parts.shaders.hdr_path())
                                                       : std::string("none")));
  line(std::format("key_size={}", sizeof(PipelineKey)));
  line("samples=" + JoinCounts(samples));
  line(std::format("list={}+{}:{:016x}", list.records, list.copies, list.hash));
  return text;
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// The first key whose line differs: what to tell the player (or the log) changed.
std::string WhatChanged(const std::string& have, const std::string& expected) {
  if (have.empty()) {
    return "no stamp yet";
  }
  size_t at = 0;
  size_t have_at = 0;
  while (at < expected.size()) {
    const size_t end = expected.find('\n', at);
    const std::string want = expected.substr(at, end - at);
    const size_t have_end = have.find('\n', have_at);
    const std::string got = have_at < have.size() ? have.substr(have_at, have_end - have_at)
                                                  : std::string();
    if (want != got) {
      const size_t eq = want.find('=');
      return eq == std::string::npos ? std::string("an older stamp format")
                                     : want.substr(0, eq) + " changed (" + got + " -> " + want + ")";
    }
    if (end == std::string::npos || have_end == std::string::npos) {
      break;
    }
    at = end + 1;
    have_at = have_end + 1;
  }
  return "the stamp differs";
}

void WriteStamp(const std::filesystem::path& path, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(text.data(), std::streamsize(text.size()));
    if (!file) {
      REXLOG_ERROR("[gpu] pipelines: the stamp {} could not be written", Utf8(temp));
      return;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    REXLOG_ERROR("[gpu] pipelines: the stamp {} could not replace the old one: {}", Utf8(path),
                 ec.message());
  }
}

PrebuildResult Fail(PrebuildResult result, PrebuildStatus status, std::string text) {
  result.status = status;
  result.text = std::move(text);
  return result;
}

}  // namespace

const char* PrebuildStatusName(PrebuildStatus status) {
  switch (status) {
    case PrebuildStatus::kBuilt: return "ok";
    case PrebuildStatus::kUpToDate: return "up to date";
    case PrebuildStatus::kNotNeeded: return "not needed";
    case PrebuildStatus::kNoList: return "no list";
    case PrebuildStatus::kNoPack: return "no shader pack";
    case PrebuildStatus::kNoDevice: return "no device";
    case PrebuildStatus::kBadOption: return "bad option";
    case PrebuildStatus::kStalled: return "stalled";
    case PrebuildStatus::kFailed: return "failed";
    case PrebuildStatus::kNotReady: return "not ready";
  }
  return "?";
}

// CHECK: written without a build of larecomp or a run (clang -fsyntax-only is clean on this file,
// pipelines.cpp, edram.cpp, renderer.cpp and the app side). To verify, in order:
//  - it links and runs (Pipelines::PrecacheForSetup, this file, larecomp_pipeline_build.cpp);
//  - a device with no window on both APIs (D3D12 and `--precache_pipelines_backend=vulkan`): the
//    log shows the device line, "guest shader layout created" and "command line prebuild:
//    precaching N pipelines on T threads", then "precache done in ..." and "N records ... written";
//  - the driver's disk cache really keeps what was made here: precache time of the GAME's first
//    start after this run (log "precache done in X ms", with a warm driver cache) against the
//    same start without this step; compare the pending counts in the scene intervals ("skipped
//    [pipeline pending ...]"), which should be near zero. A driver that flushes its cache only at
//    device or process end would be covered by the orderly teardown here (the parts are
//    destroyed before the process ends), but an NVIDIA/AMD cache size limit smaller than the
//    list's compiled size would evict (check the driver's cache folder grows by about the size of
//    the list's pipelines);
//  - a Vulkan run with DLSS saved as the upscaler goes through Streamline's device hook
//    (Device::Open, dlss::BeforeDevice): untried without a swap chain or a window;
//  - the second run says "up to date" within about a second, and a driver update (or --precache_
//    pipelines_force) builds again;
//  - the sample counts: the log line "the start draws the multisampled images at N sample(s)" says 2
//    on the defaults (mcla_gpu_msaa 2, no upscaler), the stamp's samples= line the same; with
//    mcla_gpu_msaa = 4 or 8, "off", or mcla_gpu_upscaler = fsr31 the copies follow (4 / 8 / 1 / 2,1);
//    the game's first start at that level then logs few pending draws (no "skipped [pipeline
//    pending" burst in the first minute) against a start of the same game without this step (a
//    cold driver cache, mcla_gpu_msaa 4: the main view's pipelines were recorded at 2).
PrebuildResult PrebuildPipelines(const PrebuildOptions& options, const PrebuildEvents& events) {
  PrebuildResult result;
  const auto log = [&events](int level, const std::string& text) {
    if (events.log) {
      events.log(level, text);
    }
  };

  if (!Enabled()) {
    return Fail(result, PrebuildStatus::kNotNeeded,
                "The Plume renderer is not in use (mcla_gpu is off), so there are no pipelines to "
                "prepare.");
  }
  if (!REXCVAR_GET(mcla_gpu_pipeline_cache)) {
    return Fail(result, PrebuildStatus::kNotNeeded,
                "The pipeline cache is turned off (mcla_gpu_pipeline_cache), so the game keeps no "
                "pipeline list and there is nothing to prepare.");
  }

  // The backend: the game's own choice, or the one asked for here.
  Backend forced = kDefaultBackend;
  const Backend* forced_ptr = nullptr;
  if (!options.backend.empty() && options.backend != "auto") {
    if (!ParseBackend(options.backend, forced)) {
      return Fail(result, PrebuildStatus::kBadOption,
                  "precache_pipelines_backend is \"" + options.backend +
                      "\"; it has to be auto, d3d12 or vulkan.");
    }
    if (!BackendCompiled(forced)) {
      return Fail(result, PrebuildStatus::kBadOption,
                  std::string("This build has no ") + BackendName(forced) + " renderer.");
    }
    forced_ptr = &forced;
  }

  auto holder = std::make_unique<Parts>();
  Parts& parts = *holder;
  const Device::Options device_options = PlannedDeviceOptions(forced_ptr);
  if (!parts.device.Initialize(device_options)) {
    return Fail(result, PrebuildStatus::kNoDevice,
                std::string("No graphics device could be made for ") +
                    BackendName(device_options.backend) + " (the log has the reason).");
  }
  result.backend = BackendName();
  result.adapter = parts.device.description().name;
  if (parts.device.fell_back()) {
    if (forced_ptr) {
      return Fail(result, PrebuildStatus::kNoDevice,
                  std::string(BackendName(*forced_ptr)) + " could not be started on this PC (the log has "
                  "the reason).");
    }
    // The game falls back the same way, so its pipelines are the fallback's.
    log(1, std::string(BackendName(device_options.backend)) + " could not be started; preparing " +
               BackendName() + ", which the game will run on");
  }
  if (events.device) {
    events.device(result.backend, result.adapter,
                  std::format("{:#x}", parts.device.description().driverVersion));
  }

  // The pieces the guest's pipelines are made from, as the renderer's Initialize makes them.
  if (!parts.frames.Initialize(parts.device) || !parts.heap.Initialize(parts.device, parts.frames) ||
      !parts.layout.Initialize(parts.device, parts.heap) || !parts.shaders.Initialize(parts.device)) {
    return Fail(result, PrebuildStatus::kNoDevice,
                "The graphics device could not make the objects the pipelines need (the log has the "
                "reason).");
  }
  // The pack the game would attach. The app checked that its stamp is current.
  if (!parts.shaders.AttachFirst(ShaderPackSearchFolders())) {
    return Fail(result, PrebuildStatus::kNoPack,
                "There is no shader pack to make the pipelines from. Build the shader packs first "
                "(larecomp.exe --build_shader_packs).");
  }
  // The HDR variants too when the pack has them: the records that name them are kept and made,
  // and the file written below does not lose them (the game only reads them while HDR is on).
  {
    std::string why;
    if (!parts.shaders.LoadHdrVariants(&why)) {
      log(0, "no HDR variant pack is used: " + why);
    }
  }
  parts.pipelines.Initialize(parts.device, parts.layout, parts.shaders);

  std::string expected;
  std::filesystem::path stamp_path;
  Pipelines::SetupOptions setup;
  setup.threads = options.threads;
  setup.level_samples = Pipelines::StartSampleCounts(parts.device);
  log(0, "the start draws the multisampled images at " + JoinCounts(setup.level_samples) +
             " sample(s): copies of the listed pipelines are made for those counts too");
  setup.wanted = [&](const Pipelines::SetupList& list) {
    expected = StampText(parts, list, setup.level_samples);
    stamp_path = parts.pipelines.cache_path();
    stamp_path.replace_extension(".stamp");
    if (options.force) {
      log(0, "building regardless of the stamp (precache_pipelines_force)");
      return true;
    }
    const std::string have = ReadAll(stamp_path);
    if (have.starts_with(expected)) {
      return false;
    }
    log(0, "the pipelines need building: " + WhatChanged(have, expected));
    return true;
  };
  using Clock = std::chrono::steady_clock;
  bool phase_sent = false;
  Clock::time_point last_step{};
  setup.progress = [&](uint32_t done, uint32_t total) {
    if (!phase_sent) {
      phase_sent = true;
      if (events.phase) {
        events.phase(total, "Preparing the graphics pipelines");
      }
    }
    const Clock::time_point now = Clock::now();
    if (events.step && (done >= total || now - last_step >= std::chrono::milliseconds(100))) {
      last_step = now;
      events.step(done, total);
    }
  };

  const Pipelines::SetupResult run = parts.pipelines.PrecacheForSetup(parts.shaders, setup);
  result.pipelines = uint32_t(run.list.records);
  result.copies = uint32_t(run.list.copies);
  result.failed = run.failed;
  result.seconds = run.seconds;
  switch (run.outcome) {
    case Pipelines::SetupOutcome::kNotReady:
      return Fail(result, PrebuildStatus::kNotReady, "The pipelines could not be started.");
    case Pipelines::SetupOutcome::kDisabled:
      return Fail(result, PrebuildStatus::kNotNeeded,
                  "The pipeline cache is turned off, so there is nothing to prepare.");
    case Pipelines::SetupOutcome::kEmpty:
      return Fail(result, PrebuildStatus::kNoList,
                  "No pipelines are listed yet (the starter list is missing and the game has not "
                  "recorded any), so there is nothing to prepare.");
    case Pipelines::SetupOutcome::kNotWanted:
      return Fail(result, PrebuildStatus::kUpToDate,
                  std::format("The {} pipelines are already prepared for {} ({}).",
                              run.list.records + run.list.copies, result.adapter, result.backend));
    case Pipelines::SetupOutcome::kStalled:
      // The driver never answered: its worker threads are still inside it. Nothing is destroyed
      // (the caller ends the process).
      (void)holder.release();
      return Fail(result, PrebuildStatus::kStalled,
                  "The graphics driver stopped answering while the pipelines were prepared. The "
                  "game prepares them itself when it starts.");
    case Pipelines::SetupOutcome::kBuilt:
      break;
  }
  if (run.ready == 0) {
    return Fail(result, PrebuildStatus::kFailed,
                std::format("None of the {} pipelines could be made (the log has the reasons).",
                            run.list.records + run.list.copies));
  }
  // Last, like the shader packs' stamp: what the stamp vouches for is in place.
  WriteStamp(stamp_path, expected + std::format("# ready={} failed={} seconds={:.1f}\n", run.ready,
                                                 run.failed, run.seconds));
  std::string text = std::format("Prepared {} pipelines for {} ({}) in {:.0f} s.", run.ready,
                                 result.adapter, result.backend, run.seconds);
  if (run.list.copies != 0) {
    text += std::format(" {} of them are copies at the MSAA level the game starts with.",
                        run.list.copies);
  }
  if (run.failed != 0) {
    text += std::format(" {} could not be made; the game logs them too.", run.failed);
  }
  result.status = PrebuildStatus::kBuilt;
  result.text = std::move(text);
  return result;
}

}  // namespace mcla::gpu

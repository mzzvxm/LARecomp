#include "larecomp_pipeline_build.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "larecomp_log.h"
#include "bootprogress/larecomp_shader_build.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#include "gpu/pipeline_prebuild.h"
#endif

REXCVAR_DEFINE_BOOL(
    precache_pipelines, false, "MCLA/GPU",
    "Prepares the graphics pipelines the game compiles while it starts, and closes, without "
    "starting the game: no window, no graphics output. The launcher runs this at the end of its "
    "setup (after the shader packs are built), so that the first start has no shaders to compile "
    "and no pipeline pending. It fills the graphics driver's own cache for this PC; it does not "
    "have to be repeated until the driver, the game files or LARecomp change (the next run "
    "notices and does nothing when everything is current). Progress is printed to the console. "
    "Exit code 0 when the pipelines are prepared or need nothing.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();

REXCVAR_DEFINE_STRING(
    precache_pipelines_backend, "", "MCLA/GPU",
    "With precache_pipelines: which graphics API to prepare. Empty (or auto) is the one the game "
    "runs on (graphics_backend). d3d12 or vulkan prepares that one; one API per run.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();

REXCVAR_DEFINE_BOOL(
    precache_pipelines_force, false, "MCLA/GPU",
    "With precache_pipelines: prepare even when the last run says the pipelines are current. For "
    "after the graphics driver's shader cache was cleared (Windows' Disk Cleanup does that).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();

REXCVAR_DEFINE_INT32(
    precache_pipelines_threads, 0, "MCLA/GPU",
    "With precache_pipelines: how many threads compile the pipelines. 0 uses every processor but "
    "one. A lower number leaves the PC more usable meanwhile, and the preparation takes longer.")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();

namespace larecomp::pipelinebuild {
namespace {

// The line format is in the header. Flushed at every line: a pipe is fully buffered and the
// reader wants each now.
void Emit(const std::string& line) {
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// One line of free text: no line breaks, not too long.
std::string Field(std::string text, size_t max = 600) {
  for (char& c : text) {
    if (c == '\r' || c == '\n' || c == '\t' || c == '|') c = ' ';
  }
  if (text.size() > max) text = text.substr(0, max - 3) + "...";
  return text;
}

}  // namespace

bool CommandLineRequested() {
  return REXCVAR_GET(precache_pipelines);
}

// CHECK: written without a build of larecomp. Run it: `larecomp.exe --precache_pipelines` in the
// install's game folder after --build_shader_packs, with the stdout piped; expect the device,
// phase, step and result lines, exit 0, and then a normal start of the game whose log shows the
// precache done in a fraction of the first run's time and (scene intervals) no "skipped [pipeline
// pending" draws. A second run must print "up to date" within about a second.
int RunCommandLine(const std::filesystem::path& game_root) {
#if defined(LARECOMP_HAS_GPU)
  const auto finish = [](int code, const char* status, const std::string& text) {
    if (code == 0) {
      LARECOMP_APP_INFO("[pipelines] {}: {}", status, text);
    } else {
      LARECOMP_APP_WARN("[pipelines] {}: {}", status, text);
    }
    Emit(std::string("@pipelines|result|") + status + "|" + Field(text));
    return code;
  };

  if (!mcla::gpu::Enabled()) {
    return finish(0, "not needed",
                  "The Plume renderer is not in use (mcla_gpu is off), so there are no pipelines to "
                  "prepare.");
  }
  // Pipelines are made from the packs the game will use. Packs the game would rebuild first (the
  // stamp says they are stale) would make these pipelines useless: build the packs first.
  if (!larecomp::shaderbuild::PacksCurrent(game_root)) {
    return finish(1, "packs not current",
                  "The shader packs are out of date. Build them first (larecomp.exe "
                  "--build_shader_packs); the game prepares the pipelines itself until then.");
  }

  mcla::gpu::PrebuildOptions options;
  options.backend = REXCVAR_GET(precache_pipelines_backend);
  std::transform(options.backend.begin(), options.backend.end(), options.backend.begin(),
                 [](unsigned char ch) { return char(std::tolower(ch)); });
  options.force = REXCVAR_GET(precache_pipelines_force);
  options.threads = uint32_t(std::max(0, REXCVAR_GET(precache_pipelines_threads)));

  mcla::gpu::PrebuildEvents events;
  events.device = [](const std::string& backend, const std::string& adapter,
                     const std::string& driver) {
    Emit("@pipelines|device|" + Field(backend) + "|" + Field(adapter) + "|" + Field(driver));
  };
  events.phase = [](uint32_t steps, const std::string& name) {
    Emit("@pipelines|phase|" + std::to_string(steps) + "|" + Field(name));
  };
  events.step = [](uint32_t done, uint32_t total) {
    Emit("@pipelines|step|" + std::to_string(done) + "|" + std::to_string(total) + "|");
  };
  events.log = [](int level, const std::string& text) {
    if (level >= 2) {
      LARECOMP_APP_WARN("[pipelines] {}", text);
    } else {
      LARECOMP_APP_INFO("[pipelines] {}", text);
    }
    Emit(std::string("@pipelines|log|") + (level >= 2 ? "error" : level == 1 ? "warn" : "info") + "|" +
         Field(text));
  };

  const mcla::gpu::PrebuildResult result = mcla::gpu::PrebuildPipelines(options, events);
  return finish(mcla::gpu::PrebuildSucceeded(result.status) ? 0 : 1,
                mcla::gpu::PrebuildStatusName(result.status), result.text);
#else
  (void)game_root;
  Emit("@pipelines|result|not needed|This build has no Plume renderer, so it has no pipelines to "
       "prepare.");
  return 0;
#endif
}

}  // namespace larecomp::pipelinebuild

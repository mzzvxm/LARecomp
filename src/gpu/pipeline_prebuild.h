#pragma once
// ===========================================================================
// MCLA GPU renderer -- the pipeline prebuild (larecomp.exe --precache_pipelines)
// ===========================================================================
// The game precaches its pipelines at every start from a list (the starter list that ships in
// <exe>/assets plus what earlier sessions recorded, pipelines.h) on worker threads: on a first
// start with a cold driver that is minutes of compiling while the title already runs, and the
// draws that ask before a pipeline is ready are skipped ("pipeline pending"). This does the same
// work ahead of time, for the launcher's setup, in a process that has no window, no guest and no
// render thread:
//
//   Device (the one Startup would open) -> FrameRing -> BindlessHeap -> GuestLayout ->
//   ShaderStore (the pack the game attaches, and its HDR variants) -> Pipelines::PrecacheForSetup
//
// Plume needs no window to make a device: D3D12 never asks for one, and Vulkan checks a queue's
// present support only when a swap chain is made. The pipelines are made by the same code the
// game's workers run (Pipelines::BuildPrecached), all processors but one, at normal priority; each
// is dropped as soon as it exists, because what the player gains is the driver's own cache (D3D12
// and Vulkan drivers keep compiled pipelines on disk, keyed by shader, state and driver version;
// Plume passes no VkPipelineCache and no D3D12 pipeline library, so that is the only cache there
// is). At the end the record file the game reads at its next start is written, as the game does at
// its exit.
//
// Skipping. A stamp next to the cache file (cache/mcla_gpu_pipelines_<api>.stamp) says what the
// last prebuild was made for: this exe, the graphics API, the adapter and its driver version, the
// shader packs, and the list of pipelines. A start whose stamp is that same text does nothing but
// open the device and read the list (about a second). A new driver, a new pack, a new exe or a
// list that grew (the game recorded pipelines of its own) builds again; the driver answers the old
// ones from its cache, so it is quick. The stamp cannot know that the player cleared the driver's
// shader cache (Windows' Disk Cleanup does); precache_pipelines_force builds regardless.
//
// Not covered: the renderer's own pipelines (resolve conversions, decode, host_precache.h): they
// need the resolve and texture components, which need guest memory. One builder thread makes them
// at the game's start (a few dozen).
// ===========================================================================

#include <cstdint>
#include <functional>
#include <string>

namespace mcla::gpu {

struct PrebuildOptions {
  // "" or "auto": the backend graphics_backend would give the game (with its "auto" rules);
  // "d3d12" or "vulkan": that one. One backend per process: a launcher that wants both runs the
  // program twice.
  std::string backend;
  bool force = false;   // build even when the stamp says the cache is complete
  uint32_t threads = 0; // 0: all usable processors but one (at most 16)
};

// All called on the calling thread. May be empty.
struct PrebuildEvents {
  // Once the device exists: "D3D12" or "Vulkan", the adapter's name, its driver version in hex.
  std::function<void(const std::string& backend, const std::string& adapter,
                     const std::string& driver)>
      device;
  // A phase begins: how many steps it has (0 = unknown) and its name.
  std::function<void(uint32_t steps, const std::string& name)> phase;
  // Progress in the phase, about ten times a second at most.
  std::function<void(uint32_t done, uint32_t total)> step;
  // The prebuild's own words (why it builds or skips, what it found): level 0 info, 1 warn, 2 error.
  std::function<void(int level, const std::string& text)> log;
};

enum class PrebuildStatus : uint8_t {
  kBuilt,       // ok: the pipelines were made and the cache files written
  kUpToDate,    // ok: the stamp says it was done for this driver, pack and exe
  kNotNeeded,   // ok: the Plume renderer is off, or the pipeline cache is (nothing to prepare)
  kNoList,      // ok: nothing is listed (no starter list, no cache file): nothing to make
  kNoPack,      // failed: no shader pack to make the pipelines from
  kNoDevice,    // failed: no graphics device could be made (for the asked backend)
  kBadOption,   // failed: precache_pipelines_backend is not auto, d3d12 or vulkan, or not built in
  kStalled,     // failed: no pipeline finished for three minutes (the process must end at once)
  kFailed,      // failed: not one pipeline of the list could be made
  kNotReady,    // failed: the pipelines could not be started (a bug)
};
// The status as a line of the output: "ok", "up to date", ...
const char* PrebuildStatusName(PrebuildStatus status);
// True for the statuses that are fine for the launcher (exit code 0).
inline bool PrebuildSucceeded(PrebuildStatus status) {
  return status == PrebuildStatus::kBuilt || status == PrebuildStatus::kUpToDate ||
         status == PrebuildStatus::kNotNeeded || status == PrebuildStatus::kNoList;
}

struct PrebuildResult {
  PrebuildStatus status = PrebuildStatus::kNotReady;
  std::string text;          // plain English, one line
  std::string backend;       // "D3D12" / "Vulkan", empty before the device existed
  std::string adapter;
  uint32_t pipelines = 0;    // the list's size
  uint32_t failed = 0;       // of them, not made (their shader is missing or the driver refused)
  double seconds = 0;        // the build, not the skip
};

// Call it from the app's SetupEnvironment, where nothing else has started: the config is read
// (Enabled() decides the backend from it) and the shader packs are in place and current (the app
// checks the stamp; this attaches the pack the game would). Blocks until done; on kStalled it
// returns with threads of the driver still running and everything leaked on purpose, so the caller
// must end the process without running destructors that wait for them (std::_Exit).
PrebuildResult PrebuildPipelines(const PrebuildOptions& options, const PrebuildEvents& events);

}  // namespace mcla::gpu

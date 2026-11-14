// MCLA GPU renderer -- the host MSAA level changes while the game runs. See msaa_level.h.

#include "msaa_level.h"

#include <algorithm>
#include <atomic>
#include <format>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "edram.h"
#include "gpu.h"
#include "pipelines.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_msaa_with_upscaler, false, "MCLA/GPU",
    "While FSR or DLSS draws the scene, keep the MSAA level of mcla_gpu_msaa (2, 4 or 8 samples) "
    "instead of 1 sample. The upscaler then gets a scene whose edges and alpha tested foliage are "
    "already smoothed by the sample coverage, and has less aliasing to average over its jitter; it "
    "costs the multisampled scene at the render size. For A/B. Live (read at start too).")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] msaa:";

std::atomic<bool> g_switching{false};

double MillisSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

bool MsaaKeptUnderUpscaler() {
  return REXCVAR_GET(mcla_gpu_msaa_with_upscaler);
}

bool MsaaSwitching() {
  return g_switching.load(std::memory_order_relaxed);
}

void MsaaLevel::Initialize(EdramTargets& edram, Pipelines& pipelines) {
  edram_ = &edram;
  pipelines_ = &pipelines;
}

// CHECK (nothing here was built or run; D3D12 and Vulkan, debug layer / validation on):
//  1. OFF at start against the game's 2 sample layout over 1 host sample, the same parked frame
//     (a build where OFF was the game's own switch, such as test/live-next f15c6f5, against one
//     with this code): the picture (edges, grass and fences, the sun's glare), the draws a frame,
//     the GPU time per pass (the main pass label reads "1280 2x ..." with 1 host sample) and the
//     resolves a frame (two tile resolves of each surface where the game's own OFF had one). The
//     game's tile layout and the targets it makes differ with its MSAA mode (sub_822E4CC8 makes its
//     surfaces with it, sub_822E5ED8 hands it to BeginTiledRendering, sub_822E6408 picks an
//     argument of sub_8241E428 by it); none of that was decoded here, so any other difference the
//     A/B shows is to be looked for there.
//  2. The switch in game (cvar cycle mcla_gpu_msaa off -> 2 -> 4 -> 8 -> off -> 2, 25 s apart, then
//     4 s apart once the pipelines exist): "live change" lines with the switch's milliseconds (a
//     few ms is the targets and their clears), no hitch beyond it in the frame times, no draw left
//     out for more than a frame or two ("pending" in the pipeline statistics), the picture N frames
//     after equal to a start at the new level, and the same for the pause menu and the map (their
//     guest 4x targets take the level too: the "game view" and "pause view" lines of edram.cpp).
//  3. Level 1's pipelines: PrebuildForLevel(1) after a start at 2x (known, present, queued), the
//     switch with no pending draws, and a start with OFF: the first frames make their pipelines on
//     the render thread if the starter list has no 1 sample copies (the number in the precache
//     statistics against the 2x start).
//  4. An upscaler: a start with one on (level 1 from the first target, the "runs from the start"
//     line of edram.cpp, no switch line), turned on in game over 2x/4x/8x (the level goes to 1 once
//     the path has settled: "live change: Nx -> 1x (an upscaler runs)"), turned off (the player's
//     level returns within a few frames: the gap between the path switch line and this one), a
//     DLSS that fails or cannot run a mode (the level returns), and nothing changing while a path
//     change is pending or a render scale change is being made.
//  5. Video memory at level 1 equal to the old OFF's, at 8x as before; old and new targets live
//     together for the frames in flight.
//  6. Weak hardware: the two tile resolves of a 2 sample layout over 1 host sample on the iGPU.
uint32_t MsaaLevel::Wanted(bool upscaling) {
  if (upscaling && !MsaaKeptUnderUpscaler()) {
    return 1;  // the upscaler smooths the picture; the setting is kept for when it stops
  }
  const EdramTargets::MsaaSetting setting = EdramTargets::ReadMsaaSetting();
  if (setting.invalid && invalid_logged_ != setting.text) {
    invalid_logged_ = setting.text;
    REXLOG_ERROR("{} mcla_gpu_msaa = '{}' is not off, 2, 4 or 8; the console's 2x is used", kPrefix,
                 setting.text);
  }
  // An invalid value reads as 2, what the start used for it.
  const uint32_t level = edram_->SupportedMsaaLevel(setting.level);
  if (level != setting.level && clamp_logged_ != setting.text) {
    clamp_logged_ = setting.text;
    REXLOG_ERROR("{} MSAA {}x asked, {}x is the most this device can do for the screen formats",
                 kPrefix, setting.level, level);
  }
  return level;
}

void MsaaLevel::Rest() {
  wanted_ = 0;
  staged_ = false;
  retry_after_ = Clock::time_point{};
  retry_delay_ = kRetry;
  failure_logged_ = false;
  g_switching.store(false, std::memory_order_relaxed);
}

void MsaaLevel::BeginFrame(plume::RenderCommandList* list, bool upscaling, bool settled,
                           bool moved) {
  if (!edram_ || !pipelines_ || !list) {
    return;
  }
  const uint32_t wanted = Wanted(upscaling);
  const uint32_t level = edram_->msaa_level();
  // The upscaler's own change of path stood still before it was made (TemporalSystem::UpdatePath),
  // so the level it asks for needs no settling of its own: that is for a row stepped through values.
  const bool path_driven = upscaling != last_upscaling_;
  last_upscaling_ = upscaling;
  if (wanted == level) {
    Rest();
    return;
  }
  const Clock::time_point now = Clock::now();
  if (wanted != wanted_) {
    wanted_ = wanted;
    wanted_since_ = path_driven ? now - kSettle : now;
    staged_ = false;
    retry_after_ = Clock::time_point{};
    retry_delay_ = kRetry;
    failure_logged_ = false;
  }
  if (now - wanted_since_ < kSettle || now < retry_after_) {
    return;
  }
  g_switching.store(true, std::memory_order_relaxed);
  const char* why = upscaling ? "an upscaler runs" : "mcla_gpu_msaa";
  if (!staged_) {
    staged_ = true;
    staged_at_ = now;
    const Pipelines::LevelPrebuild b = pipelines_->PrebuildForLevel(wanted);
    staged_total_ = b.queued;
    REXLOG_INFO("{} live change: {}x -> {}x ({}): {} pipelines of draws into the multisampled "
                "images are known, {} of them have the new sample count already, {} are being "
                "made in the background ({} not finished); the switch is made at the first frame "
                "boundary after they are ready and the upscaler path has settled (at most {} s "
                "for the pipelines) while {}x goes on",
                kPrefix, level, wanted, why, b.known, b.present, b.queued, b.waiting,
                kMaxWait.count(), level);
  }
  if (moved || !settled) {
    // The targets were remade this frame (the path or the render scale: one remake a frame), or a
    // path change is waiting or being made and will remake them: the level follows it.
    return;
  }
  const uint32_t waiting = pipelines_->PrebuildWaiting();
  const bool timed_out = now - staged_at_ >= kMaxWait;
  if (waiting != 0 && !timed_out) {
    return;
  }
  const Clock::time_point began = Clock::now();
  if (!edram_->SetMsaaLevel(wanted, list)) {
    // Asked again after a while, less often each time: a level the card cannot hold keeps failing;
    // another value (a smaller level) is asked at once (wanted_ changes).
    retry_delay_ = failure_logged_ ? std::min(retry_delay_ * 2, kRetryMax) : kRetry;
    retry_after_ = began + retry_delay_;
    if (!failure_logged_) {
      failure_logged_ = true;
      REXLOG_WARN("{} the change to {}x was not made: the targets could not be made with {} samples "
                  "(video memory is most likely short): the game stays at {}x, and the change is "
                  "asked again after {} seconds, then less often",
                  kPrefix, wanted, wanted, level, retry_delay_.count());
    }
    g_switching.store(false, std::memory_order_relaxed);
    return;
  }
  const double switch_ms = MillisSince(began);
  REXLOG_INFO("{} live change: {}x -> {}x ({}), switched in {:.2f} ms ({:.0f} ms after the "
              "setting stood still, {:.0f} ms after the pipelines were asked for){}",
              kPrefix, level, wanted, why, switch_ms,
              std::chrono::duration<double, std::milli>(began - wanted_since_).count(),
              std::chrono::duration<double, std::milli>(began - staged_at_).count(),
              waiting != 0
                  ? std::format("; {} of {} pipelines for the new sample count were not made yet and "
                                "are made as draws ask for them",
                                waiting, staged_total_)
                  : std::string());
  Rest();
}

}  // namespace mcla::gpu

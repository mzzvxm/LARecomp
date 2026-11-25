// MCLA GPU renderer -- the host MSAA level changes while the game runs. See msaa_level.h.

#include "msaa_level.h"

#include <algorithm>
#include <atomic>
#include <format>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "edram.h"
#include "gpu.h"
#include "pipelines.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] msaa:";

std::atomic<bool> g_switching{false};

double MillisSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

bool MsaaKeptUnderUpscaler() {
  return larecomp::settings::mcla_gpu_msaa_with_upscaler.Get();
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
  if (msaa_plan::UpscalerTakesLevel(upscaling, MsaaKeptUnderUpscaler())) {
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

void MsaaLevel::BeginFrame(plume::RenderCommandList* list, bool upscaling, bool settled,
                           bool moved) {
  if (!edram_ || !pipelines_ || !list) {
    return;
  }
  msaa_plan::Frame frame;
  frame.wanted = Wanted(upscaling);
  frame.level = edram_->msaa_level();
  frame.upscaling = upscaling;
  frame.settled = settled;
  frame.moved = moved;
  frame.now = Clock::now();
  const uint32_t wanted = frame.wanted;
  const uint32_t level = frame.level;
  const char* why = upscaling ? "an upscaler runs" : "mcla_gpu_msaa";
  // Whether the change is made now, and what is asked of the pipelines on the way, is the plan's
  // (msaa_plan.h): the upscaler's own path change needs no settling, a row stepped through values does.
  const msaa_plan::Decision decision = planner_.Plan(
      frame,
      [&] {
        const Pipelines::LevelPrebuild b = pipelines_->PrebuildForLevel(wanted, level);
        staged_total_ = b.queued;
        REXLOG_INFO("{} live change: {}x -> {}x ({}): {} pipelines of draws into the multisampled "
                    "images are known, {} of them have the new sample count already, {} are being "
                    "made in the background ({} not finished); the switch is made at the first frame "
                    "boundary after they are ready and the upscaler path has settled (at most {} s "
                    "for the pipelines) while {}x goes on{}",
                    kPrefix, level, wanted, why, b.known, b.present, b.queued, b.waiting,
                    msaa_plan::kMaxWait.count(), level,
                    b.seed_sources != 0
                        ? std::format("; {} more are made from the saved lists for the views not drawn yet, "
                                      "at low priority and not waited for ({} listed pipelines of "
                                      "multisampled images, {} have the new count already, {} left out by "
                                      "the limit)",
                                      b.seeds, b.seed_sources, b.seed_present, b.seed_limited)
                        : std::string());
      },
      [&] { return pipelines_->PrebuildWaiting(); });
  g_switching.store(planner_.switching(), std::memory_order_relaxed);
  if (decision.verdict == msaa_plan::Verdict::kRest) {
    pipelines_->NoteLevelInForce(level);  // seeds for a level that was asked for and taken back go
    return;
  }
  if (decision.verdict != msaa_plan::Verdict::kApply) {
    // Waiting for the setting to stand still or for a retry, held by a path change or a remake of the
    // targets (one remake a frame), or waiting for the pipelines: the level follows.
    return;
  }
  const Clock::time_point began = Clock::now();
  const bool made = edram_->SetMsaaLevel(wanted, list);
  const bool first_failure = planner_.Applied(began, made);
  g_switching.store(planner_.switching(), std::memory_order_relaxed);
  if (!made) {
    // Asked again after a while, less often each time: a level the card cannot hold keeps failing;
    // another value (a smaller level) is asked at once (the plan sees another wanted level).
    if (first_failure) {
      REXLOG_WARN("{} the change to {}x was not made: the targets could not be made with {} samples "
                  "(video memory is most likely short): the game stays at {}x, and the change is "
                  "asked again after {} seconds, then less often",
                  kPrefix, wanted, wanted, level, planner_.retry_delay().count());
    }
    return;
  }
  const double switch_ms = MillisSince(began);
  REXLOG_INFO("{} live change: {}x -> {}x ({}), switched in {:.2f} ms ({:.0f} ms after the "
              "setting stood still, {:.0f} ms after the pipelines were asked for){}",
              kPrefix, level, wanted, why, switch_ms,
              std::chrono::duration<double, std::milli>(began - planner_.wanted_since()).count(),
              std::chrono::duration<double, std::milli>(began - planner_.staged_at()).count(),
              decision.waiting != 0
                  ? std::format("; {} of {} pipelines for the new sample count were not made yet and "
                                "are made as draws ask for them",
                                decision.waiting, staged_total_)
                  : std::string());
}

}  // namespace mcla::gpu

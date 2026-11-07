// MCLA GPU renderer -- the host MSAA level changes while the game runs. See msaa_level.h.

#include "msaa_level.h"

#include <algorithm>
#include <atomic>
#include <format>

#include <rex/logging.h>

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

bool MsaaSwitching() {
  return g_switching.load(std::memory_order_relaxed);
}

void MsaaLevel::Initialize(EdramTargets& edram, Pipelines& pipelines) {
  edram_ = &edram;
  pipelines_ = &pipelines;
}

// CHECK: the switch in game (cvar cycle mcla_gpu_msaa 4 -> 8 -> 2 -> 4, 25 s apart, then 4 s apart once
// the pipelines exist): "live change" lines with the switch's milliseconds (a few ms is the targets
// and their clears), no hitch beyond it in the frame times, no draw left out for more than a frame or
// two ("pending" in the pipeline statistics), the picture N frames after equal to a start at the
// new level (the 4x and 8x edges), the pause menu and the map (their guest 4x targets take the new
// level too, at 2x with fewer samples than the guest's), video memory at 8x (old and new targets
// live together for the frames in flight), D3D12 debug layer and Vulkan validation clean in the
// switch frame, and no switch while an upscaler runs (the hold line), then the saved level applies
// the frame after it is off.
uint32_t MsaaLevel::Wanted() {
  // A game that started without MSAA (off, or held off for an upscaler) has no multisampled target
  // the level could apply to, and the game's own switch is read once: the setting waits for the
  // next start. Before the game has made its renderer nothing is known.
  if (GetGuestMsaa() != GuestMsaa::kOn) {
    return 0;
  }
  const EdramTargets::MsaaSetting setting = EdramTargets::ReadMsaaSetting();
  if (setting.off) {
    return 0;  // the game's own switch: a restart (the menu row says so)
  }
  if (setting.invalid) {
    if (invalid_logged_ != setting.text) {
      invalid_logged_ = setting.text;
      REXLOG_ERROR("{} mcla_gpu_msaa = '{}' is not off, 2, 4 or 8; the level in use ({}x) stays",
                   kPrefix, setting.text, edram_->msaa_level());
    }
    return 0;
  }
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
  held_logged_ = false;
  retry_after_ = Clock::time_point{};
  retry_delay_ = kRetry;
  failure_logged_ = false;
  g_switching.store(false, std::memory_order_relaxed);
}

void MsaaLevel::BeginFrame(plume::RenderCommandList* list, bool hold, bool moved) {
  if (!edram_ || !pipelines_ || !list) {
    return;
  }
  const uint32_t wanted = Wanted();
  const uint32_t level = edram_->msaa_level();
  if (wanted == 0 || wanted == level) {
    Rest();
    return;
  }
  if (hold) {
    // The setting is kept; it applies the frame after the upscaler is off.
    if (!held_logged_) {
      held_logged_ = true;
      REXLOG_INFO("{} MSAA {}x is chosen and {}x runs: the change waits while an upscaler runs or "
                  "is being switched to",
                  kPrefix, wanted, level);
    }
    wanted_ = 0;
    staged_ = false;
    g_switching.store(false, std::memory_order_relaxed);
    return;
  }
  held_logged_ = false;
  const Clock::time_point now = Clock::now();
  if (wanted != wanted_) {
    wanted_ = wanted;
    wanted_since_ = now;
    staged_ = false;
    retry_after_ = Clock::time_point{};
    retry_delay_ = kRetry;
    failure_logged_ = false;
  }
  if (now - wanted_since_ < kSettle || now < retry_after_) {
    return;
  }
  g_switching.store(true, std::memory_order_relaxed);
  if (!staged_) {
    staged_ = true;
    staged_at_ = now;
    const Pipelines::LevelPrebuild b = pipelines_->PrebuildForLevel(wanted);
    staged_total_ = b.queued;
    REXLOG_INFO("{} live change: {}x -> {}x: {} pipelines of draws into the multisampled images are "
                "known, {} of them have the new sample count already, {} are being made in the "
                "background ({} not finished); the switch is made at the first frame boundary "
                "after they are ready (at most {} s) while {}x goes on",
                kPrefix, level, wanted, b.known, b.present, b.queued, b.waiting, kMaxWait.count(),
                level);
  }
  if (moved) {
    return;  // the targets were remade this frame (the path or the render scale): one remake a frame
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
  REXLOG_INFO("{} live change: {}x -> {}x (mcla_gpu_msaa), switched in {:.2f} ms ({:.0f} ms after the "
              "setting stood still, {:.0f} ms after the pipelines were asked for){}",
              kPrefix, level, wanted, switch_ms,
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

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the host MSAA level changes while the game runs
// ===========================================================================
// mcla_gpu_msaa 2, 4 or 8 sets the samples the host gives the images of the screen the game
// multisamples (EdramTargets::HostSamplesAt), the pause menu's and the map's world view among
// them: the game draws that one with 4 samples and the host gives it the same samples as the game
// view (1 while the game runs without MSAA, which is not this class's to change). The game's own
// MSAA stays the 2 samples it made its renderer with. Off against on is the game's start-up
// switch (hooks/graphics.cpp Patch_DisableMSAA, read once) and stays a restart: this changes the
// level among 2, 4 and 8 only, and only in a game that started with MSAA (GuestMsaa::kOn; one
// without has no multisampled target the level could apply to).
//
// The steps, each frame at the boundary before the first draw (Scene::BeginFrame, after the
// temporal system's own path change):
//   1. the setting stands at another level for kSettle (a row stepped through 2X, 4X, 8X asks for
//      nothing in between);
//   2. Pipelines::PrebuildForLevel hands the builder threads a copy of every pipeline the game has
//      drawn into those targets at the sample count the new level gives it; the old level keeps
//      drawing meanwhile (make before break), and the pause menu's MSAA row says (WAIT);
//   3. when the copies are made (or kMaxWait has passed: the rest are made as draws ask for them,
//      the usual background build that leaves a draw out for a few frames), EdramTargets::
//      SetMsaaLevel makes the targets again with the new samples in one all-or-nothing step, the
//      same as a render scale change (SetScales); a failure (video memory: 8x at a large render
//      scale) keeps the old level and is asked again after 3 s, 6 s, ... up to 30 s;
//   4. the log says from, to and how long the switch frame took.
//
// Not while an upscaler runs or is being switched to (TemporalSystem::PathBusy): the upscaler
// holds the anti-aliasing the picture gets (FXAA and SMAA are held off the same way,
// TemporalSystem::HoldsAaOff), its vectors and depth are read from the targets the level sets,
// and the path switch remakes those targets itself. The pause menu greys the row then, so the
// setting changes only through the F4 panel or larecomp.toml; it is kept, and applies the frame
// after the upscaler is off. The frame a path or render scale change remade the targets is
// skipped too (one remake a frame).
//
// Render thread.
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <string>

#include <plume_render_interface.h>

namespace mcla::gpu {

class EdramTargets;
class Pipelines;

class MsaaLevel {
 public:
  void Initialize(EdramTargets& edram, Pipelines& pipelines);

  // The first list of a frame is open (nothing recorded, outside a render pass). `hold`: an
  // upscaler runs or is being switched to; `moved`: the targets were remade this frame already.
  void BeginFrame(plume::RenderCommandList* list, bool hold, bool moved);

 private:
  using Clock = std::chrono::steady_clock;

  // The level the settings ask for now, within what the device can do; 0 when they ask for none
  // that this running game can take (off, a value that is none of 2, 4 and 8, a game that started
  // without MSAA).
  uint32_t Wanted();
  // Nothing is asked: forgets what was staged.
  void Rest();

  // The setting stands this long before anything is made for it.
  static constexpr std::chrono::milliseconds kSettle{400};
  // The copies of the pipelines are waited for this long; after it the level changes anyway.
  static constexpr std::chrono::seconds kMaxWait{20};
  // A level the card cannot hold is asked again after kRetry, twice as long each time.
  static constexpr std::chrono::seconds kRetry{3};
  static constexpr std::chrono::seconds kRetryMax{30};

  EdramTargets* edram_ = nullptr;
  Pipelines* pipelines_ = nullptr;

  uint32_t wanted_ = 0;  // the level being staged (0: none)
  Clock::time_point wanted_since_{};
  bool staged_ = false;  // its pipelines were asked for
  Clock::time_point staged_at_{};
  uint32_t staged_total_ = 0;  // the copies asked for (queued by the call)
  Clock::time_point retry_after_{};
  std::chrono::seconds retry_delay_ = kRetry;
  bool failure_logged_ = false;
  bool held_logged_ = false;
  std::string clamp_logged_;    // the setting a "asked, used" line was logged for
  std::string invalid_logged_;  // the value an "is not 2, 4 or 8" line was logged for
};

}  // namespace mcla::gpu

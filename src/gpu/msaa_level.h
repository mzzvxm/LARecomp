#pragma once
// ===========================================================================
// MCLA GPU renderer -- the host MSAA level changes while the game runs
// ===========================================================================
// The game always draws its own 2 sample layout (hooks/graphics.cpp MsaaOff: the native renderer
// never turns the game's MSAA switch off). What the host gives the images of the screen the game
// multisamples is the MSAA level (EdramTargets::HostSamplesAt): 1, 2, 4 or 8 samples, the game's
// pause menu and map world view (drawn by the game with 4) among them. mcla_gpu_msaa off is level
// 1, 2X, 4X and 8X are levels 2, 4 and 8, and an upscaler that runs sets the level to 1 whatever
// the setting says (it smooths the picture itself, MSAA under it only costs time); the player's
// level comes back when the upscaler stops. Every change among the four is made while the game
// runs, at a frame boundary, and the pause menu's MSAA row says (APPLYING) while one is prepared.
//
// The steps, each frame at the boundary before the first draw (Scene::BeginFrame, after the
// temporal system's own path change):
//   1. the level the settings and the upscaler ask for (Wanted) stands at another value than the
//      one in force for kSettle (a row stepped through 2X, 4X, 8X asks for nothing in between;
//      a level the upscaler asks for does not wait, its path change has settled already);
//   2. Pipelines::PrebuildForLevel hands the builder threads a copy of every pipeline the game has
//      drawn into those targets at the sample count the new level gives it (1 included); the old
//      level keeps drawing meanwhile (make before break);
//   3. when the copies are made (or kMaxWait has passed: the rest are made as draws ask for them,
//      the usual background build that leaves a draw out for a few frames) and the upscaler path
//      has settled, EdramTargets::SetMsaaLevel makes the targets again with the new samples in one
//      all-or-nothing step, the same as a render scale change (SetScales); a failure (video
//      memory: 8x at a large render scale) keeps the old level and is asked again after 3 s, 6 s,
//      ... up to 30 s;
//   4. the log says from, to, why and how long the switch frame took.
//
// The upscaler. Steps 1 and 2 begin as soon as the settings steer to an upscaler that can run (or
// away from one), so the copies are being made while its context is; step 3 waits until the path
// has settled (TemporalSystem::PathSettled): the path switch remakes the scene's targets itself,
// and one remake a frame is all that is made (`moved`). An upscaler turned on therefore runs for
// a few frames over the player's level (the upscaler reads the game's resolved products, so host
// samples above 1 do not matter to it: it ran that way in a game started with MSAA), and an
// upscaler turned off draws for a few frames at level 1 before the player's level returns. The
// pause menu greys the MSAA row while an upscaler runs (upscaling_menu.cpp).
//
// mcla_gpu_msaa_with_upscaler (debug, for A/B) lets the player's level stand under an upscaler: the
// scene then reaches it with the coverage of its edges and of the alpha to mask foliage averaged by
// the game's resolve (the guest's resolve is the MSAA resolve), so the jitter has less aliasing to
// average out. It costs the multisampled scene at the render size.
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

  // The first list of a frame is open (nothing recorded, outside a render pass). `upscaling`: the
  // path the settings steer to is an upscaler that runs (TemporalSystem::UpscalerNext), which
  // makes the level 1; `settled`: no path or render scale change is waiting or being made
  // (TemporalSystem::PathSettled), so the targets may change; `moved`: the targets were remade
  // this frame already.
  void BeginFrame(plume::RenderCommandList* list, bool upscaling, bool settled, bool moved);

 private:
  using Clock = std::chrono::steady_clock;

  // The level the settings and the upscaler ask for now, within what the device can do: 1, 2, 4
  // or 8, never 0. A value that is none of off, 2, 4 and 8 is the console's 2.
  uint32_t Wanted(bool upscaling);
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

  bool last_upscaling_ = false;  // the `upscaling` of the last call: a change of it is the path's
  uint32_t wanted_ = 0;  // the level being staged (0: none)
  Clock::time_point wanted_since_{};
  bool staged_ = false;  // its pipelines were asked for
  Clock::time_point staged_at_{};
  uint32_t staged_total_ = 0;  // the copies asked for (queued by the call)
  Clock::time_point retry_after_{};
  std::chrono::seconds retry_delay_ = kRetry;
  bool failure_logged_ = false;
  std::string clamp_logged_;    // the setting a "asked, used" line was logged for
  std::string invalid_logged_;  // the value an "is not off, 2, 4 or 8" line was logged for
};

}  // namespace mcla::gpu

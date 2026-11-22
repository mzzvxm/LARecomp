#pragma once
// ===========================================================================
// MCLA GPU renderer -- the game's clock and sky, handed to the renderer (CITY-RT package FRAME)
// ===========================================================================
// The ray tracing world frame (rt_world_frame.h) needs the world's hour (the lamps' on hours and the night curve) and the sky's index (the weather): the renderer has no read of the guest's
// lighting manager, so the time of day hook (src/mc_engine/gameplay/time_of_day.cpp Hook_TimeOfDay, which runs once a frame before the lighting update) publishes them here, the way it
// already publishes the garage's light (menu_world.h SetGarageLighting): two relaxed stores, no guest access from the render thread. The values are the manager's at the START of the
// frame's lighting update (a scene's or a menu shot's hour that the hook pins is written after: the render thread sees it one frame later, a few seconds of the day's clock at most).
// An unset clock (no hook yet, the XEO3 build) reads -1 and -1: the consumers then fall back to the world's sun colour.
//
// THE HOUR'S JUMPS (CITY-RT-REVIEW M4): the same call also tells a jump of the hour from its normal advance, for the probe field of the bounce light (gi_field_rules.h RelightDetector), which
// must be relit when the light of the whole world changes at once. The hour is not set at one place: the Director's scene and a menu shot pin it in the hook (TimeOfDay_SceneHour,
// TimeOfDay_RequestHour), the settings' hold and the real clock pin it there too, and the game's own mission starts, skips and garage changes move it inside the lighting update. Every one of
// them leaves the same trace at the next call: the hour of this frame's start is far from the last frame's. So the step is tested here, where all of them pass, and counted (HourJumps).
//
//   The largest step normal play makes in one frame: the game's stock rate is at most 0.00833 h/s (time_of_day.cpp: the midday rate, 2 minutes an hour), the day speed setting goes to 100
//   (settings_world.h Range{0, 100}) = 0.833 h/s, and one frame's step of the game's clock is capped at 125 ms by default (frame_timing.cpp kStepCapCeilingMs; the setting's own ceiling is
//   1000 ms) = 0.104 h (6.2 minutes) a frame; a scene's time scale of 4 raises that to 0.42 h. A scene's own hour ramp (`at T hour a..b over s`) steps (b - a) / s * (the frame's seconds):
//   a whole day in 5 s at 60 FPS is 0.08 h. kHourJumpHours is 0.5 h (about 7.5 degrees of the sun): above all of those, under every change a person makes on purpose to move the light
//   (the menu's own presets, time_of_day.cpp, are 6.0, 16.75, 17.4 and 23.5: the nearest two differ by 0.65 h). Under it the field follows the change by its own hysteresis, as it does the
//   day's advance.
//
//   One jump counts once: after a step over the threshold the next one counts only after a frame whose step is under it (a run of large steps, a slider dragged, a day ramp faster than
//   a 10 FPS frame can follow, is one jump at its start).
// Header only; no guest, no renderer.
// ===========================================================================

#include <atomic>
#include <cstdint>

namespace mcla::gpu::world_clock {

// The step of the hour, in hours, that is a jump (see above).
inline constexpr float kHourJumpHours = 0.5f;

// The shortest way round the 24 hour circle from `from` to `to`, in hours: -12 < step <= 12 (23.9 to 0.1 is +0.2, not -23.8).
constexpr float HourStep(float from, float to) {
  float step = to - from;
  while (step > 12.0f) step -= 24.0f;
  while (step <= -12.0f) step += 24.0f;
  return step;
}

// The pure step test: one hour a frame in, a jump or not out. Not thread safe: one thread (the hook's) feeds it.
class HourJumpTracker {
 public:
  // `hour` is the world's hour at the start of a frame's lighting update; anything outside 0..24 is unknown and changes nothing (the last known hour stays the one the next is compared with).
  // True when this hour is a jump from the last known one.
  bool Observe(float hour) {
    if (!(hour >= 0.0f && hour < 24.0f)) {
      return false;
    }
    bool jump = false;
    if (have_) {
      float step = HourStep(last_, hour);
      step = step < 0.0f ? -step : step;
      if (step > kHourJumpHours) {
        jump = armed_;
        armed_ = false;
      } else {
        armed_ = true;
      }
    }
    last_ = hour;
    have_ = true;
    if (jump) {
      ++jumps_;
    }
    return jump;
  }
  // The jumps seen so far (wraps; readers compare for a change).
  uint32_t jumps() const { return jumps_; }
  void Reset() { *this = HourJumpTracker{}; }

 private:
  float last_ = 0.0f;
  bool have_ = false;
  bool armed_ = true;
  uint32_t jumps_ = 0;
};

inline std::atomic<float> g_hour{-1.0f};
inline std::atomic<int32_t> g_weather{-1};
inline std::atomic<uint32_t> g_hour_jumps{0};
inline HourJumpTracker g_hour_tracker;  // the hook's thread only

// The hook's: the world's hour in 0..24 (anything else is stored as unknown) and the weather index (0 nice, 1 cloudy, 2 stormy, 3 foggy; anything else unknown).
inline void Publish(float hour, int32_t weather) {
  const float known = hour >= 0.0f && hour < 24.0f ? hour : -1.0f;
  g_hour.store(known, std::memory_order_relaxed);
  g_weather.store(weather >= 0 && weather < 4 ? weather : -1, std::memory_order_relaxed);
  if (g_hour_tracker.Observe(known)) {
    g_hour_jumps.store(g_hour_tracker.jumps(), std::memory_order_relaxed);
  }
}
inline float Hour() { return g_hour.load(std::memory_order_relaxed); }
inline int32_t Weather() { return g_weather.load(std::memory_order_relaxed); }
// The jumps of the hour so far: a reader keeps the number it last saw and takes a change as a jump (GiIntegration).
inline uint32_t HourJumps() { return g_hour_jumps.load(std::memory_order_relaxed); }

}  // namespace mcla::gpu::world_clock

#pragma once
// ===========================================================================
// MCLA GPU renderer -- the game's clock and sky, handed to the renderer (CITY-RT package FRAME)
// ===========================================================================
// The ray tracing world frame (rt_world_frame.h) needs the world's hour (the lamps' on hours and the night curve) and the sky's index (the weather): the renderer has no read of the guest's
// lighting manager, so the time of day hook (src/mc_engine/gameplay/time_of_day.cpp Hook_TimeOfDay, which runs once a frame before the lighting update) publishes them here, the way it
// already publishes the garage's light (menu_world.h SetGarageLighting): two relaxed stores, no guest access from the render thread. The values are the manager's at the START of the
// frame's lighting update (a scene's or a menu shot's hour that the hook pins is written after: the render thread sees it one frame later, a few seconds of the day's clock at most).
// An unset clock (no hook yet, the XEO3 build) reads -1 and -1: the consumers then fall back to the world's sun colour.
// Header only; no guest, no renderer.
// ===========================================================================

#include <atomic>
#include <cstdint>

namespace mcla::gpu::world_clock {

inline std::atomic<float> g_hour{-1.0f};
inline std::atomic<int32_t> g_weather{-1};

// The hook's: the world's hour in 0..24 (anything else is stored as unknown) and the weather index (0 nice, 1 cloudy, 2 stormy, 3 foggy; anything else unknown).
inline void Publish(float hour, int32_t weather) {
  g_hour.store(hour >= 0.0f && hour < 24.0f ? hour : -1.0f, std::memory_order_relaxed);
  g_weather.store(weather >= 0 && weather < 4 ? weather : -1, std::memory_order_relaxed);
}
inline float Hour() { return g_hour.load(std::memory_order_relaxed); }
inline int32_t Weather() { return g_weather.load(std::memory_order_relaxed); }

}  // namespace mcla::gpu::world_clock

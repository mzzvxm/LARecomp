#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — what is left of the app bring-up with no emulated GPU.
// See nocp_app.h, and nocp_driver.h for the driver that replaced the rest.

#include "nocp_app.h"

#include <atomic>

#include <rex/chrono.h>

#include "nocp_driver.h"

namespace mcla::native_gfx::nocp {

bool WantNoCommandProcessor() { return DriverSelected(); }

namespace {
std::atomic<const char*> g_last_hook{nullptr};
std::atomic<uint64_t> g_hook_calls{0};
std::atomic<uint64_t> g_frame_ends{0};
std::atomic<uint64_t> g_swaps{0};

// Last swap-to-swap interval, written by the guest thread at the swap and read
// by the overlay on the UI thread. Two separate relaxed atomics can tear by one
// frame, which a once-per-host-frame readout cannot show.
std::atomic<double> g_frame_time_ms{0.0};
std::atomic<double> g_fps{0.0};
std::atomic<uint64_t> g_timed_frames{0};
}  // namespace

void NoteHook(const char* name) {
  g_last_hook.store(name, std::memory_order_relaxed);
  // A plain increment, not a locked one: this runs at every hooked call, a draw
  // among them, and the count only says that hooks are being called (the stall
  // line in guest_gpu.cpp). Two threads at once can lose a count.
  g_hook_calls.store(g_hook_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

const char* LastHookName() {
  const char* n = g_last_hook.load(std::memory_order_relaxed);
  return n ? n : "(none)";
}

uint64_t HookCallCount() { return g_hook_calls.load(std::memory_order_relaxed); }

void NoteFrameEnd() { g_frame_ends.fetch_add(1, std::memory_order_relaxed); }

void NoteSwapCall() {
  g_swaps.fetch_add(1, std::memory_order_relaxed);
  NotifyGuestSwap();

  // The frame timing the F3 "Debug Frames" overlay reads. The swap is the frame boundary and this hook is on it.
  static uint64_t last_tick = 0;
  const uint64_t freq = rex::Clock::GetHostTickFrequency();
  const uint64_t now = rex::Clock::GetHostTickCount();
  if (last_tick && now > last_tick && freq) {
    const uint64_t delta = now - last_tick;
    g_frame_time_ms.store(double(delta) * 1000.0 / double(freq), std::memory_order_relaxed);
    g_fps.store(double(freq) / double(delta), std::memory_order_relaxed);
    g_timed_frames.fetch_add(1, std::memory_order_relaxed);
  }
  last_tick = now;
}

rex::gpu::FrameStats GuestFrameStats() {
  rex::gpu::FrameStats stats;
  stats.frame_time_ms = g_frame_time_ms.load(std::memory_order_relaxed);
  stats.fps = g_fps.load(std::memory_order_relaxed);
  stats.frame_count = g_timed_frames.load(std::memory_order_relaxed);
  return stats;
}

uint64_t FrameEndCount() { return g_frame_ends.load(std::memory_order_relaxed); }
uint64_t SwapCount() { return g_swaps.load(std::memory_order_relaxed); }

}  // namespace mcla::native_gfx::nocp

#endif  // REXGLUE_HAS_XEO3_TARGET

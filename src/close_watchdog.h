#pragma once
//
// The close watchdog's decision (larecomp_app.cpp FinishWindowCloseWork): when a close that has not finished is ended as hung.
//
// WHY IT IS NOT A FIXED 5 s ANY MORE (VK-SLOW-CLOSE, 2026-11-22). Of 556 Vulkan runs of 2026-11-20 to 11-22, 9 logged "[quit] the process was
// still running 5 s after the window was asked to close". Two were not slow at all (a crash in the renderer's teardown, fixed in d42cc65e and
// daebb115, and an old start failure with no renderer). The other seven were the renderer's teardown: 4.4 to 4.7 s between the cache file
// write and "renderer shut down" where 0.1 s is normal, and the 5 s clock (which counts the whole close) ran out 0.3 s before the title's own
// termination finished. The time is the graphics driver's: the renderer logs its slow stages since 12fdfe64, and of the 12 runs of builds
// that have that line 10 name "the device (the driver's own teardown)" (vkDestroyDevice / vkDestroyInstance, or the D3D12 device's release;
// 273 to 1763 ms, all six Vulkan ones that logged a stage), none a stage of ours (the seven long runs are from builds before it: the stage is
// inferred from the one gap, cache file written to renderer shut down). It happens with another game running on the machine (slow closes:
// 12 of 229 Vulkan closes with a second game, 1 of 296 alone; D3D12 8 of 630 against 4 of 1031). Nothing of ours waits there: the call is
// one driver call that returns by itself, and the process was not hung when the watchdog ended it, only slow.
//
// So the watchdog asks "has the close made no progress for 5 s?" instead of "have 5 s passed?". The renderer's shutdown publishes where it is
// (gpu.h ReadShutdownProgress: a stage name, whether the stage is one call into the driver, and how long ago it last moved). A close whose
// renderer is still moving, or is inside the driver's own teardown call, is given time; a close that is stuck anywhere else ends as before:
//   * the renderer never started its shutdown (the stuck thing is before it, or there is no renderer: Xenia): 5 s after the close began, as before;
//   * the renderer's shutdown is in a stage that is not a driver call (a drain that waits for a GPU that never answers): 5 s after that stage began;
//   * the renderer is inside a driver call (the final teardown): 8 s after that call began, because it returns by itself and the slowest seen is 4.7 s;
//   * the renderer's shutdown has finished: 5 s after it finished (the rest of a normal close takes 0.4 s);
//   * and whatever the renderer does, 12 s after the close began (the cap: a watchdog that can be moved out for ever is no watchdog).
// The first line it logs keeps its words ("the process was still running N s after the window was asked to close", N = the seconds it really
// waited) and now says where the renderer's shutdown was.
//
// No SDK, no renderer and no threads in this header: close_watchdog_test.cpp decides, with timelines of the logged runs, what it ends and what it leaves.

#include <cstdint>
#include <cstdio>
#include <string>

namespace larecomp::closewatch {

// A close with no progress for this long is hung (the old fixed limit, and still the first moment the watchdog may end anything).
inline constexpr int64_t kQuietMs = 5000;
// The same, while the renderer is inside one call into the graphics driver (its final teardown): the call returns by itself.
inline constexpr int64_t kDriverCallQuietMs = 8000;
// The close is ended after this whatever else is true.
inline constexpr int64_t kCapMs = 12000;
// The watcher looks this often.
inline constexpr int64_t kPollMs = 100;

struct Look {
  int64_t elapsed_ms = 0;       // since the watchdog was armed (the window was asked to close)
  int64_t quiet_ms = 0;         // since the renderer's shutdown last changed stage or ended; elapsed_ms while it has not started
  bool in_driver_call = false;  // the renderer's shutdown is inside one call into the graphics driver
};

// Makes a Look from what the renderer reports: `started` false (no shutdown yet, no renderer) counts the quiet time from the arming.
inline Look MakeLook(int64_t elapsed_ms, bool started, int64_t renderer_quiet_ms, bool in_driver_call) {
  Look look;
  look.elapsed_ms = elapsed_ms;
  look.quiet_ms = started ? (renderer_quiet_ms < elapsed_ms ? renderer_quiet_ms : elapsed_ms) : elapsed_ms;
  look.in_driver_call = started && in_driver_call;
  return look;
}

// The quiet time that ends the close.
inline constexpr int64_t QuietLimitMs(bool in_driver_call) { return in_driver_call ? kDriverCallQuietMs : kQuietMs; }

// True when the close is to be ended as hung.
inline constexpr bool ShouldEnd(const Look& look) {
  if (look.elapsed_ms >= kCapMs) return true;
  if (look.elapsed_ms < kQuietMs) return false;  // nothing is ended before the old limit
  return look.quiet_ms >= QuietLimitMs(look.in_driver_call);
}

// What the renderer's shutdown was doing when the watchdog ended the close (gpu.h ShutdownProgress, copied: this header has no SDK).
struct RendererState {
  bool started = false;
  bool finished = false;
  const char* stage = nullptr;  // static text, or null
  bool in_driver_call = false;
  int64_t quiet_ms = 0;
};

// The words of the watchdog's first log line, without the "[quit] " tag. The beginning is what the checkers read (gb_neon_report.py QUIT_SLOW_RX,
// chain-quit1.ps1: "the process was still running <N> s"), N = the whole seconds the close really took before it was ended, so it stays 5 when the
// renderer had nothing to do with it and says 8 or 12 when the watchdog waited for a driver call. What follows says where the renderer's shutdown was.
inline std::string EndText(int64_t elapsed_ms, const RendererState& renderer) {
  char seconds[32];
  std::snprintf(seconds, sizeof(seconds), "%.1f", double(renderer.quiet_ms) / 1000.0);
  std::string text = "the process was still running " + std::to_string(elapsed_ms / 1000) + " s after the window was asked to close; exiting";
  if (!renderer.started) {
    text += " (the renderer's shutdown had not begun)";
  } else if (renderer.finished) {
    text += std::string(" (the renderer's shutdown had finished ") + seconds + " s before)";
  } else {
    text += std::string(" (the renderer's shutdown was in '") + (renderer.stage ? renderer.stage : "its first stage") + "', " +
            (renderer.in_driver_call ? "one call into the graphics driver, which returns by itself" : "a stage of ours") + ", for " + seconds + " s)";
  }
  return text;
}

}  // namespace larecomp::closewatch

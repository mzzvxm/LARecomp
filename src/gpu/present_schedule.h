#pragma once
// ===========================================================================
// MCLA GPU renderer -- scheduled presents (gpu.h SetNextPresentTarget)
// ===========================================================================
// The frame pacer (mc_engine/hooks/frame_pacing.cpp) gives each guest frame
// the time it is due on screen. The render thread presents it then -- after
// the frame's GPU work is submitted and after the guest was released
// (Renderer::ReleaseGuest), from its own loop between the next frame's records,
// so no guest thread waits on the hold -- and reads back from the
// presentation's frame statistics how late each frame really reached the
// screen, which the pacer turns into the latency it plans with.
//
// A frame that is ready only after its time goes out at once, late. The frames
// after it then keep that lateness instead of returning to their times at
// once: returning would show the next frame's step in less time than it was
// simulated with, a jump ahead right after the stall. The carried lateness
// fades by a fraction of a millisecond a frame, which shows the next frames a
// little faster than they were simulated -- about 1.5% at 100 FPS, for half a
// second -- instead.
//
// Times are QPC ticks (QueryPerformanceCounter), the clock DXGI's frame
// statistics report in. Outside Windows nothing is held (WaitUntilQpc returns
// at once) and no lateness is reported.
//
// Threading: WaitUntilQpc and PresentSchedule on the render thread; the
// reports (gpu.h TakePresentReports) are read by any one thread.
// ===========================================================================

#include <cstdint>
#include <string>

namespace mcla::gpu {

int64_t QpcNow();
int64_t QpcFrequency();

// Returns once QpcNow() >= target: a high-resolution waitable timer to a little
// before, then a spin for the rest.
void WaitUntilQpc(int64_t target);

class PresentSchedule {
 public:
  // A scheduled guest frame, submitted and ready at `ready`, planned for
  // `target`: when to present it -- its target plus the lateness carried from
  // late frames before it -- or 0 for at once (late even so, or a time too far
  // away to be a frame's).
  int64_t Due(int64_t target, int64_t ready);
  // After the swap chain's present of a scheduled guest frame: its present
  // count (GetLastPresentCount; 0 without one), its plan, the time it was held
  // to (`due`, 0 = none) and the time it was ready (submitted, before any hold).
  void NotePresented(uint32_t present_count, int64_t target, int64_t due, int64_t ready);
  // The presentation's frame statistics: the newest present on screen and the
  // QPC time it went out. Reports that frame's lateness when it was scheduled.
  void NoteOnScreen(uint32_t on_screen_present, int64_t on_screen_qpc);
  // One line for the renderer's stats: what the presents came to since the
  // last call. Empty when nothing was scheduled.
  std::string TakeSummary();

 private:
  struct Entry {
    uint32_t present_count = 0;
    int64_t due = 0;
  };
  static constexpr uint32_t kEntries = 64;
  Entry entries_[kEntries] = {};
  uint32_t last_on_screen_ = 0;
  int64_t flip_delay_ = -1;  // learned present-to-screen delay of a frame on time; -1 = none yet
  int64_t carry_ = 0;        // lateness carried from late frames
  int64_t last_target_ = 0;

  struct Stats {
    uint64_t frames = 0;
    uint64_t held = 0;
    int64_t hold_total = 0;
    int64_t hold_worst = 0;
    uint64_t late = 0;           // ready after their time
    int64_t late_worst = 0;
    int64_t carry_worst = 0;
    uint64_t on_screen = 0;      // frames the statistics named
    uint64_t on_screen_late = 0;  // on screen more than half a millisecond past the hold
    int64_t on_screen_worst = 0;
  };
  Stats stats_;
};

// A report for gpu.h TakePresentReports (render thread; dropped when the
// reader has fallen 256 behind).
void PushPresentReport(int64_t target, int64_t late, bool on_screen);

}  // namespace mcla::gpu

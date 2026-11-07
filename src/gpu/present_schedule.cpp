// MCLA GPU renderer -- scheduled presents. See present_schedule.h.

#include "present_schedule.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "gpu.h"

namespace mcla::gpu {

namespace {

// gpu.h TakePresentReports: one producer (the render thread), one consumer.
constexpr uint32_t kReports = 256;
PresentReport g_reports[kReports];
std::atomic<uint32_t> g_report_head{0};  // next to write
std::atomic<uint32_t> g_report_tail{0};  // next to read

}  // namespace

int64_t QpcNow() {
#if defined(_WIN32)
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
#else
  return 0;
#endif
}

int64_t QpcFrequency() {
#if defined(_WIN32)
  static const int64_t frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  return frequency;
#else
  return 1;
#endif
}

void WaitUntilQpc(int64_t target) {
#if defined(_WIN32)
  const int64_t frequency = QpcFrequency();
  int64_t now = QpcNow();
  if (now >= target) {
    return;
  }
  // A high-resolution timer (Windows 10 1803 and later) wakes a fraction of a
  // millisecond late; it is set to wake that much early, and a spin covers the
  // rest. How early follows the lateness seen (a slowly decaying maximum).
  static thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  static thread_local double late_us = 300.0;
  const int64_t slack = static_cast<int64_t>(std::clamp(late_us + 100.0, 150.0, 1000.0) *
                                             static_cast<double>(frequency) / 1e6);
  if (timer && target - now > slack + frequency / 10000) {
    const int64_t wake = target - slack;
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>((wake - now) * 10000000 / frequency);  // 100 ns, relative
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
        WaitForSingleObject(timer, 200) == WAIT_OBJECT_0) {
      const int64_t woke = QpcNow();
      const double late = woke > wake ? static_cast<double>(woke - wake) * 1e6 /
                                            static_cast<double>(frequency)
                                      : 0.0;
      late_us = std::max(late, late_us * 0.98);
    }
  }
  while (QpcNow() < target) {
    YieldProcessor();
  }
#else
  (void)target;
#endif
}

int64_t PresentSchedule::Due(int64_t target, int64_t ready) {
  const int64_t frequency = QpcFrequency();
  const int64_t step = last_target_ != 0 ? target - last_target_ : 0;
  last_target_ = target;
  ++stats_.frames;
  // The carried lateness fades: 0.15 ms a frame.
  carry_ = std::max<int64_t>(0, carry_ - frequency * 15 / 100000);
  const int64_t due = target + carry_;
  if (ready >= due) {
    // Late even so: at once, and the frames after this one keep its lateness --
    // up to 8 ms, and to 60% of a step, past which holding the next frames
    // would hold the guest up too.
    const int64_t late = ready - target;
    int64_t ceiling = frequency * 8 / 1000;
    if (step > 0) ceiling = std::min(ceiling, step * 6 / 10);
    carry_ = std::clamp<int64_t>(late, 0, ceiling);
    ++stats_.late;
    stats_.late_worst = std::max(stats_.late_worst, ready - due);
    stats_.carry_worst = std::max(stats_.carry_worst, carry_);
    return 0;
  }
  if (due - ready > frequency / 10) {
    return 0;  // not a frame's time
  }
  ++stats_.held;
  stats_.hold_total += due - ready;
  stats_.hold_worst = std::max(stats_.hold_worst, due - ready);
  return due;
}

void PresentSchedule::NotePresented(uint32_t present_count, int64_t target, int64_t due,
                                    int64_t ready) {
  Entry& e = entries_[present_count % kEntries];
  e.present_count = present_count;
  e.due = due != 0 ? due : ready;
  // Every frame reports how its readiness compared with its plan -- negative
  // when it was early and held -- with or without frame statistics (Vulkan has
  // none, and DXGI has none while the desktop composes the window).
  PushPresentReport(target, ready - target, /*on_screen=*/false);
}

void PresentSchedule::NoteOnScreen(uint32_t on_screen_present, int64_t on_screen_qpc) {
  if (on_screen_present == 0 || on_screen_present == last_on_screen_) {
    return;
  }
  last_on_screen_ = on_screen_present;
  const Entry& e = entries_[on_screen_present % kEntries];
  if (e.present_count != on_screen_present || e.due == 0) {
    return;
  }
  // A frame presented on time still reaches the screen a little after the
  // present (the flip). That delay is learned -- the least seen, allowed to
  // creep back up -- and only what goes past it is lateness: the GPU had not
  // finished the frame when it was presented. Bounded, so a run of genuinely
  // late frames cannot pass for the flip.
  const int64_t sample = on_screen_qpc - e.due;
  const int64_t ceiling = QpcFrequency() * 3 / 2000;  // 1.5 ms
  if (flip_delay_ < 0) {
    flip_delay_ = ceiling;
  }
  flip_delay_ = std::min(flip_delay_ + QpcFrequency() / 500000, sample);  // creeps 2 us a frame
  flip_delay_ = std::clamp<int64_t>(flip_delay_, 0, ceiling);
  const int64_t late = sample - flip_delay_;
  ++stats_.on_screen;
  if (late > QpcFrequency() / 2000) {
    ++stats_.on_screen_late;
    stats_.on_screen_worst = std::max(stats_.on_screen_worst, late);
  }
  PushPresentReport(e.due, late, /*on_screen=*/true);
}

std::string PresentSchedule::TakeSummary() {
  if (stats_.frames == 0) {
    return {};
  }
  const double ms = static_cast<double>(QpcFrequency()) / 1000.0;
  const Stats& s = stats_;
  const std::string line = std::format(
      "schedule: {} frames, {} held (mean {:.2f}, worst {:.2f} ms), {} late at the present "
      "({:.2f}%, worst {:.2f} ms past its time, up to {:.2f} ms carried after) | on screen: {} named "
      "by the frame statistics, {} more than 0.5 ms past their time (worst {:.2f} ms), flip {:.2f} ms",
      s.frames, s.held, s.held ? static_cast<double>(s.hold_total) / ms / s.held : 0.0,
      static_cast<double>(s.hold_worst) / ms, s.late, 100.0 * s.late / s.frames,
      static_cast<double>(s.late_worst) / ms, static_cast<double>(s.carry_worst) / ms, s.on_screen,
      s.on_screen_late, static_cast<double>(s.on_screen_worst) / ms,
      flip_delay_ >= 0 ? static_cast<double>(flip_delay_) / ms : 0.0);
  stats_ = {};
  return line;
}

void PushPresentReport(int64_t target, int64_t late, bool on_screen) {
  const uint32_t head = g_report_head.load(std::memory_order_relaxed);
  if (head - g_report_tail.load(std::memory_order_acquire) >= kReports) {
    return;  // the reader stopped reading (pacing off): nothing is waiting for these
  }
  g_reports[head % kReports] = PresentReport{target, late, on_screen};
  g_report_head.store(head + 1, std::memory_order_release);
}

uint32_t TakePresentReports(PresentReport* out, uint32_t capacity) {
  const uint32_t tail = g_report_tail.load(std::memory_order_relaxed);
  const uint32_t head = g_report_head.load(std::memory_order_acquire);
  const uint32_t n = std::min(head - tail, capacity);
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = g_reports[(tail + i) % kReports];
  }
  g_report_tail.store(tail + n, std::memory_order_release);
  return n;
}

}  // namespace mcla::gpu

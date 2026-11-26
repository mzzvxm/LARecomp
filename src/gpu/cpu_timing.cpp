#include "cpu_timing.h"

#include "cpu_timing_accounts.h"

#include <rex/logging/log_category.h>

#include "cycles.h"
#include "settings/generated/settings_gpu.h"
#include "thread_load.h"

namespace mcla::gpu::cpu_timing {
namespace detail {

struct State {
  explicit State(ThreadLoad& thread_load) : load(thread_load) {}

  Accounts accounts;
  FrameWindow window;
  ThreadLoad& load;
  uint64_t busy_start = 0;
  double overhead_ticks = 0;
};
thread_local State* g_state = nullptr;

// Subtract recorded GPU waits. This is elapsed host time and still includes unscheduled time.
uint64_t Clock(const State& state) {
  return cycles::Now() - state.load.gpu_wait_total();
}

bool Begin(Section section) {
  if (State* state = g_state) {
    state->accounts.Enter(section, Clock(*state));
    return true;
  }
  return false;
}

void End() {
  if (State* state = g_state) {
    state->accounts.Exit(Clock(*state));
  }
}

}  // namespace detail
namespace {

void ReadSetting() {
  const bool want = larecomp::settings::mcla_gpu_cpu_timing.Get();
  if (want == (detail::g_state != nullptr)) {
    return;
  }
  if (!want) {
    EndSession();
    return;
  }

  auto* state = new detail::State(CurrentThreadLoad());
  detail::g_state = state;
  detail::g_on.store(true, std::memory_order_relaxed);
  constexpr int kRuns = 4096;
  const uint64_t start = cycles::Now();
  for (int i = 0; i < kRuns; ++i) {
    const Scope scope(Section::kEncode);
  }
  state->overhead_ticks = double(cycles::Now() - start) / kRuns;
  state->accounts.Take();
  state->busy_start = state->load.busy_clock();
}

}  // namespace

void BeginSession() { ReadSetting(); }

void FinishFrame(uint64_t guest_frames) {
  if (detail::State* state = detail::g_state) {
    // CHECK: verify full windows and live toggle alignment in a run with internal flushes and FG.
    state->window.Add(state->accounts.Take());
    if (guest_frames % kWindowFrames == 0) {
      const uint64_t now = state->load.busy_clock();
      const uint64_t busy = now > state->busy_start ? now - state->busy_start : 0;
      if (state->window.count() == kWindowFrames) {
        REXLOG_INFO("[gpu] scene (interval): {}",
                    Summary(state->window.totals(), kWindowFrames, cycles::NsPerCycle(), busy,
                            state->overhead_ticks, state->window.P95()));
      }
      state->window.Reset();
      state->busy_start = state->load.busy_clock();
    }
  }
  ReadSetting();
}

void EndSession() {
  detail::g_on.store(false, std::memory_order_relaxed);
  delete detail::g_state;
  detail::g_state = nullptr;
}

}  // namespace mcla::gpu::cpu_timing

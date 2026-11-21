#pragma once
//
// The guest's D3D fence poll (Patch_FenceSpinThrottle in frame_timing.cpp, the hook inside sub_82412F98's 32-NOP loop): how often a poll
// gives up its time slice. Pure (no OS calls), so the rule has a check that needs no game: fence_spin_test.cpp.
//
// Every poll pauses the core (Windows YieldProcessor, Linux the x86 PAUSE) and every kFencePollsPerYield-th poll also hands the rest of its
// time slice to another thread that wants the core (Windows SwitchToThread, Linux sched_yield). The guest reaches the hook about ten million
// times a second while the GPU works, so the count lives in a thread_local of the caller.
//

#include <cstdint>

namespace mcla::hooks {

inline constexpr uint32_t kFencePollsPerYield = 16;

// One poll of a thread whose polls so far since the last yield are `count`. True when this poll also yields the time slice; the count then
// starts again. (The Windows hook had this as `if (++s_spin_count >= 16) { ...; s_spin_count = 0; }`.)
inline bool FencePollYieldDue(uint32_t& count) {
  if (++count < kFencePollsPerYield) {
    return false;
  }
  count = 0;
  return true;
}

}  // namespace mcla::hooks

#pragma once
//
// The pure part of the Race Line rate cap (hooks/race_line.cpp): the clock the cap counts and one planner's schedule. Nothing here
// knows the game or the SDK (the standard library, and capture_core.h's tick constant), so race_line_test.cpp compiles it alone.
//
// The cap lets a planner's job run 30 times a second (race_line_max_hz) of the GAME's time. Time passes in the game by what its
// update's step says: the measured frame time in normal play (the steady clock is the same thing to within the hitch clamp), but
// exactly the fixed step per update whenever one is set (a take, or mcla_fixed_step_fps alone for the dev tests), whatever the
// frame took in wall time. A cap on the steady clock under a fixed step gives a different rate: at a step of 1/30 s and 240 wall
// frames a second the game runs 8 times faster than the wall clock, so "30 a second" was one job per 8 updates.
//
#include <cstdint>

#include "../cinematic/capture_core.h"  // kGameTickSeconds

namespace race_line {

// The cap's clock in nanoseconds: `updates` (the updates the game has run) times `step_ticks` (the fixed step in the game's ticks)
// under a fixed step, else the steady clock `steady_ns`. The two are different clocks: a change from one to the other is a step of
// this clock, which RateSchedule::Due answers by starting its schedule again.
inline int64_t CapClockNs(uint64_t step_ticks, uint64_t updates, int64_t steady_ns) {
    if (step_ticks == 0) return steady_ns;
    return static_cast<int64_t>(static_cast<double>(updates) * static_cast<double>(step_ticks) * (mc::cinematic::capture::kGameTickSeconds * 1e9));
}

// One planner's schedule on that clock (the caller holds the lock that guards it).
struct RateSchedule {
    int64_t next_due_ns = 0;
    int64_t last_now_ns = 0;  // the clock at the last call: a clock that stepped back (a take ended, a fixed step was switched off or on) restarts the schedule

    // The planner's first call: it runs, and the next one is due an interval later.
    void Start(int64_t now_ns, int64_t interval_ns) {
        next_due_ns = now_ns + interval_ns;
        last_now_ns = now_ns;
    }

    // Every later call: true when the job runs now. `same_layout`: the planner's layout is the one the last job saw (a changed layout
    // always runs the job).
    bool Due(bool same_layout, int64_t now_ns, int64_t interval_ns) {
        const bool clock_back = now_ns < last_now_ns;
        last_now_ns = now_ns;
        if (same_layout && !clock_back && now_ns < next_due_ns) return false;
        // keep the long-run rate: the next slot follows the last one, unless the planner fell far behind (or the layout or the clock forced this run)
        next_due_ns = (!same_layout || clock_back || now_ns - next_due_ns > interval_ns) ? now_ns + interval_ns : next_due_ns + interval_ns;
        return true;
    }
};

}  // namespace race_line

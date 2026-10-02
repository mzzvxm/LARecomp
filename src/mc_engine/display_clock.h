#pragma once

// The display's own refresh clock: when each vblank of the monitor the game
// window is on happens, on the QPC timeline.
//
// frame_pacing's display-locked mode schedules every frame onto one of these
// refreshes. The presenter paints on the same vblanks -- its UI tick waits on
// IDXGIOutput::WaitForVBlank of the window's monitor too -- so a refresh index
// here is a refresh a frame can actually be shown at.

#include <cstdint>

namespace mc::display_clock {

struct Grid {
    bool valid = false;
    int64_t anchor_index = 0;   // a refresh, counted since the clock started
    double anchor_qpc = 0.0;    // when it happened
    double period_qpc = 0.0;    // one refresh, in QPC ticks
    double qpc_frequency = 0.0;

    double Time(int64_t index) const {
        return anchor_qpc + double(index - anchor_index) * period_qpc;
    }
    // The first refresh at or after `qpc`.
    int64_t IndexAtOrAfter(double qpc) const;
    double RefreshHz() const { return period_qpc > 0.0 ? qpc_frequency / period_qpc : 0.0; }
};

// The game window, whose monitor the clock follows. Set once the window
// exists; does not start anything.
void SetWindow(void* hwnd);

// Starts the clock thread the first time it is called. Cheap afterwards.
void EnsureRunning();

// The current grid; .valid is false until the clock has locked on, and again
// whenever it loses the output (minimized, monitor change in progress).
Grid Get();

// The refresh rate the window's monitor is set to (its fastest, under
// variable refresh). 0 when unknown.
double MonitorRefreshHz();

uint64_t QpcNow();

}  // namespace mc::display_clock

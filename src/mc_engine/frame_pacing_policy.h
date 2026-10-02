#pragma once

// Frame pacing policy: which time step the game simulates for a frame, given the
// frame time the clock just measured. Pure arithmetic with no runtime
// dependencies, so it can be replayed offline against recorded frame times.
//
// Why the measured time is the wrong step to simulate
// ---------------------------------------------------
// sub_821BDA90 reads the timebase at the start of each main-thread frame. That
// thread cannot start frame N+1 until the render thread has finished frame N-1
// (sub_823057E8 waits on it), and the render thread cannot finish a frame until
// D3DDevice_Swap's BlockOnFence sees the command processor retire the frame
// before it -- which, with vsync on, happens at a vblank. So the start-to-start
// time of a frame is the on-screen time of a frame roughly three swaps older.
//
// When every frame takes one vblank that does not matter. When one frame misses
// a vblank, the screen holds a frame for two vblanks, and three frames later the
// clock reports the long frame and the simulation takes a double step. The
// world stops for a frame and then lurches ahead: at 200 km/h that is up to a
// metre, which is the "car goes back and comes forward" stutter.
//
// What this does instead
// ----------------------
// - While the game holds its target rate (an integer number of vblanks, or the
//   fps limiter's period), every frame advances by exactly that period. The
//   ±1-2 ms scheduling noise in the measurement never reaches the simulation.
// - When it does not, the step follows a smoothed frame time rather than each
//   frame's own measurement, so the pipeline delay is spread out instead of
//   landing as a spike.
// - Simulation time is kept on wall time: what the steps leave unpaid is
//   tracked, up to about one step of it is carried silently (a frame of extra
//   latency nobody can see), and anything beyond that is paid back
//   proportionally, a few frames at a time.
// - A hitch (streaming stall, alt-tab) froze the screen for as long as it
//   lasted, so it passes through untouched and stays out of the average.

#include <algorithm>
#include <cmath>

namespace mc::pacing {

struct Policy {
    // Smoothing of the measured frame time: ~10 frames.
    static constexpr double kEmaGain = 0.1;
    // How close the smoothed frame time has to be to a period for the step to
    // snap to it exactly. Kept below kMaxCatchUp, so a snapped step that is a
    // little off the real rate can still be paid back instead of running the
    // debt into its cap.
    static constexpr double kSnap = 0.03;
    // Unpaid time carried without any correction, in steps.
    static constexpr double kDeadband = 1.25;
    // Beyond the deadband, this fraction of the excess is paid back per frame
    // (a time constant of ~5 frames), so a sudden change of frame rate is
    // followed within a few frames instead of running the debt into its cap...
    static constexpr double kCatchUpGain = 0.2;
    // ...but no single step grows or shrinks by more than this fraction.
    static constexpr double kMaxCatchUp = 0.5;
    // A frame this many times the smoothed frame time is a hitch.
    static constexpr double kHitch = 2.5;
    // Frames passed through untouched while the average warms up. The first
    // ones after a load are anything but typical (the very first is usually
    // the hitch clamp), and an average seeded from them runs the simulation
    // well ahead of the clock.
    static constexpr int kWarmup = 16;

    double ema = 0.0;    // smoothed frame time
    double owed = 0.0;   // time the simulation is behind the wall clock
    int warm = 0;
    double seed[kWarmup] = {};

    // All values in the same unit (the caller uses guest timebase ticks).
    // measured:  what the clock measured for this frame, already clamped.
    // limiter:   the fps limiter's period, 0 without one. Frames the limiter
    //            holds start exactly one period apart, so the step snaps to it
    //            -- but only to one period: a frame that misses its deadline
    //            restarts the schedule, it does not wait for the next one.
    // vblank:    one guest vblank when vsync paces the swaps, 0 otherwise.
    //            Swaps then land on whole vblanks, so the step snaps to whole
    //            multiples of it.
    // max_owed:  debt beyond this is dropped instead of paid back.
    // Returns the step to simulate.
    double Next(double measured, double limiter, double vblank, double max_owed) {
        if (!(measured > 0.0)) return measured;
        if (warm < kWarmup) {
            // Seed the average with the median of the warm-up frames, so one
            // long frame cannot set it.
            seed[warm++] = measured;
            if (warm == kWarmup) {
                double sorted[kWarmup];
                std::copy(seed, seed + kWarmup, sorted);
                std::nth_element(sorted, sorted + kWarmup / 2, sorted + kWarmup);
                ema = sorted[kWarmup / 2];
            }
            owed = 0.0;
            return measured;
        }
        if (measured > kHitch * std::max(ema, std::max(limiter, vblank))) return measured;

        ema += kEmaGain * (measured - ema);

        double step = ema;
        if (limiter > 0.0 && std::fabs(ema - limiter) <= kSnap * limiter) {
            step = limiter;
        } else if (vblank > 0.0) {
            const double k = std::max(1.0, std::floor(ema / vblank + 0.5));
            if (std::fabs(ema - k * vblank) <= kSnap * k * vblank) step = k * vblank;
        }

        owed += measured;
        double dt = step;
        const double rest = owed - dt;
        const double band = kDeadband * step;
        const double excess = rest > band ? rest - band : (rest < -band ? rest + band : 0.0);
        dt += std::max(-kMaxCatchUp * step, std::min(kMaxCatchUp * step, kCatchUpGain * excess));
        owed -= dt;
        owed = std::max(-max_owed, std::min(max_owed, owed));
        return dt;
    }

    void Reset() { *this = Policy{}; }
};

}  // namespace mc::pacing

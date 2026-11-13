#pragma once
//
// The neon underglow's pulse and the console's 30 Hz clock it is held on (hooks/neon_pulse.cpp). Pure
// math: no ReXGlue, no guest memory; neon_pulse_test.cpp checks it alone.
//
// sub_82353E38 (the car effect object's update) ends with, for the exterior neon (the car config's
// ExtNeonFlashRate, a signed byte at config + 396) and the interior one (+ 397):
//
//     pulse = (sin(10 * rate * T) + 1) * 0.5          // 10.0 at 0x82003A8C, 0.5 at 0x82003A24, 1.0 at 0x82001D14
//
// stored at this + 404 (exterior) and this + 408 (interior). T is the float at 0x827D7518, the engine
// timer's game clock. Every operation of that tail is single precision (fmuls, fmuls, a sine taken as a
// double and rounded with frsp, fadds, fmuls) and is written out the same way here, in the same order.
//
// A flash rate is radians per second, so a high one (19 in the garage test car) is a wave of about 30 Hz.
// The console's update ran 30 times a second, and a 30 Hz wave sampled 30 times a second keeps almost the
// same phase from one update to the next: the player saw a slow throb (a flash cycle every 3 to 4 s), not
// a flicker. At 60 FPS the same wave is sampled twice as often and shows as about 11 blinks a second, and
// uncapped as a strobe. HeldClock gives the pulse the clock as the console's updates saw it, so every
// frame rate shows the console's wave.

#include <cmath>
#include <cstdint>

namespace mcla::neon {

constexpr float kRadiansPerRateUnit = 10.0f;  // 0x82003A8C: rad/s for a flash rate of 1
constexpr float kPulseHalf = 0.5f;            // 0x82003A24

// The console's updates a second.
constexpr double kConsoleHz = 30.0;

// How far into a 30 Hz step the clock is counted as already in the next one, in steps. The game clock
// is a float sum, so a clock that advances in whole steps (the engine's fixed step of 1/60 s when
// real_frame_delta is off, a 30 FPS cap that lands exactly on 1/30 s) is a hair under or over every
// boundary, and a plain floor would pick the step before or after at random from one update to the next.
// An eighth of a step (4 ms) puts the boundary where no whole-step clock lands (whole multiples of 1/30,
// 1/60 and 1/120 s sit at least an eighth of a step away from it) and is far above the float noise of a
// session of several hours (0.03 step at 3 h), and a 4 ms shift of a grid that has no meaning of its own
// cannot be seen.
constexpr double kGridGuard = 0.125;

// The game clock as the console's updates saw it: the start of the 30 Hz step the clock is in. The clock is
// the game's own, so a pause (the clock stands still) and the time scale (the clock runs slower or faster)
// act on it as before; only its resolution changes. A clock that is not a finite number is left alone.
inline float HeldClock(float clock_seconds) {
    const double t = static_cast<double>(clock_seconds);
    if (!std::isfinite(t)) return clock_seconds;
    return static_cast<float>(std::floor(t * kConsoleHz + kGridGuard) / kConsoleHz);
}

// The sine the game takes, a float in and a float out (the guest's own routine in the game, std::sin here).
inline float StdSine(float x) { return static_cast<float>(std::sin(static_cast<double>(x))); }

// The game's own arithmetic, one rounding to float after each operation. `sine` takes the argument as a float
// and returns the sine rounded to a float.
template <typename SineFn>
inline float PulseOf(int8_t rate, float clock_seconds, SineFn&& sine) {
    const float a = static_cast<float>(rate) * clock_seconds;  // fmuls f10,f11,f0
    const float arg = a * kRadiansPerRateUnit;                 // fmuls f1,f10,f30
    const float s = sine(arg);                                 // bl sin; frsp
    return (s + 1.0f) * kPulseHalf;                            // fadds, fmuls
}

inline float PulseOf(int8_t rate, float clock_seconds) { return PulseOf(rate, clock_seconds, StdSine); }

// The pulse the console showed at this clock: the same pulse at the held clock.
template <typename SineFn>
inline float HeldPulseOf(int8_t rate, float clock_seconds, SineFn&& sine) {
    return PulseOf(rate, HeldClock(clock_seconds), sine);
}

// Flashes a second at a flash rate: the pulse repeats every 2 pi / (10 * rate) seconds.
inline double HzOf(int8_t rate) {
    const double r = rate < 0 ? -static_cast<double>(rate) : static_cast<double>(rate);
    return r * static_cast<double>(kRadiansPerRateUnit) / 6.283185307179586;
}

}  // namespace mcla::neon

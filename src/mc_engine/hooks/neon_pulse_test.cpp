// Check of the neon pulse's 30 Hz hold (neon_pulse.h). A program of its own: it needs no ReXGlue, no guest
// and no game, and exits 0 when every check holds.
//
//   clang++ -std=c++20 -O1 -Wall -Wextra -I src/mc_engine/hooks src/mc_engine/hooks/neon_pulse_test.cpp -o neon_pulse_test
//   ./neon_pulse_test
//
// What it pins down: the formula the game's sub_82353E38 uses (range, the steady rate 0, the period, the
// argument the sine gets), the held clock (the start of the 30 Hz step the clock is in, a pause and a time
// scale acting on it as on the clock itself, no jitter for a clock that advances in whole steps), and the
// point of it all: sampled at 30, 60, 120, 144 and 240 FPS and at an irregular uncapped rate, the pulse takes
// the same sequence of values, so the neon shows the same throb at every frame rate; without the hold the
// same measurement gives a strobe at 60 FPS and above.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "neon_pulse.h"

using namespace mcla::neon;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, double a = 0.0, double b = 0.0) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s (%.9g, %.9g)\n", what, a, b);
    }
}

constexpr double kPi = 3.14159265358979323846;

// The 30 Hz step number a held clock is in.
long CellOf(float held) { return std::lround(static_cast<double>(held) * kConsoleHz); }

// A clock for a frame rate: the frame times, as the game clock sum the engine keeps (an exact double sum
// rounded to a float, as timer_precision stores it). `jitter` makes the frame times irregular (an uncapped
// run's 4 to 8 ms), the same series for every run.
struct Clock {
    double fps;
    bool jitter;
    double sum = 0.0;
    uint32_t lcg = 12345u;
    float Next() {
        double dt = 1.0 / fps;
        if (jitter) {
            lcg = lcg * 1664525u + 1013904223u;
            dt = 0.004 + 0.004 * (static_cast<double>(lcg >> 8) / 16777216.0);
        }
        sum += dt;
        return static_cast<float>(sum);
    }
};

// The pulse value the exterior neon takes in each 30 Hz step (values[step]), for `seconds` of game clock at a frame
// rate. Returns false when a step was skipped or went back, or when the step was not seen by any update (the
// first update of a clock that starts at 0 may already be in step 1 at 30 FPS: step 0 is then not seen, which is
// fine; every later step must be).
bool HeldSequence(double fps, bool jitter, int8_t rate, double seconds, std::vector<float>& values, long& last_cell) {
    Clock clock{fps, jitter};
    values.clear();
    long prev = -1;
    bool in_order = true;
    while (clock.sum < seconds) {
        const float held = HeldClock(clock.Next());
        const long cell = CellOf(held);
        if (cell == prev) continue;
        if (prev >= 0 && cell != prev + 1) in_order = false;  // a step skipped, or back
        if (static_cast<size_t>(cell) >= values.size()) values.resize(static_cast<size_t>(cell) + 1, std::nanf(""));
        values[static_cast<size_t>(cell)] = PulseOf(rate, held);
        prev = cell;
    }
    last_cell = prev;
    return in_order;
}

// What a screen recording sees: the latest pulse the game stored, looked at 60 times a second for
// `seconds`, as neon_measure.py reads the clip. Rising crossings of 0.5 with a hysteresis of a twentieth of
// the range, a second.
double VisibleCyclesPerSecond(double fps, bool jitter, int8_t rate, bool held, double seconds, double start = 100.0) {
    Clock clock{fps, jitter};
    clock.sum = start;
    double next_look = start;
    float pulse = 0.0f;
    std::vector<float> seen;
    const double end = start + seconds;
    while (next_look < end) {
        const float t = clock.Next();
        pulse = held ? PulseOf(rate, HeldClock(t)) : PulseOf(rate, t);
        // every look that falls before the next frame sees this pulse
        while (next_look < clock.sum && next_look < end) {
            seen.push_back(pulse);
            next_look += 1.0 / 60.0;
        }
    }
    float lo = seen[0], hi = seen[0];
    for (float v : seen) {
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    const float h = 0.05f * (hi - lo);
    int n = 0;
    bool high = seen[0] > 0.5f;
    for (size_t i = 1; i < seen.size(); ++i) {
        if (high && seen[i] < 0.5f - h) {
            high = false;
        } else if (!high && seen[i] > 0.5f + h) {
            high = true;
            ++n;
        }
    }
    return n / seconds;
}

}  // namespace

int main() {
    // ---- the formula (as sub_82353E38 computes it) ----
    Check(PulseOf(0, 123.456f) == 0.5f, "a steady neon (rate 0) is 0.5 at any clock");
    Check(PulseOf(19, 0.0f) == 0.5f, "the pulse is 0.5 at clock 0");
    {
        float lo = 1.0f, hi = 0.0f;
        for (int rate : {1, 3, 5, 19, -19, 64, 127, -128}) {
            for (int i = 0; i < 5000; ++i) {
                const float p = PulseOf(static_cast<int8_t>(rate), 0.0137f * static_cast<float>(i));
                lo = p < lo ? p : lo;
                hi = p > hi ? p : hi;
            }
        }
        Check(lo >= 0.0f && hi <= 1.0f, "the pulse stays within 0 and 1", lo, hi);
        Check(lo < 0.01f && hi > 0.99f, "and reaches both ends", lo, hi);
    }
    // rate 1: arg = 10 T, a quarter of the way through a cycle at T = pi / 20, three quarters at 3 pi / 20
    Check(std::fabs(PulseOf(1, static_cast<float>(kPi / 20.0)) - 1.0f) < 1e-5f, "rate 1 peaks at T = pi / 20");
    Check(std::fabs(PulseOf(1, static_cast<float>(3.0 * kPi / 20.0))) < 1e-5f, "rate 1 is at its lowest at T = 3 pi / 20");
    Check(std::fabs(HzOf(1) - 10.0 / (2.0 * kPi)) < 1e-12, "a flash rate of 1 is 1.59 Hz");
    Check(std::fabs(HzOf(19) - 30.2394) < 1e-3, "a flash rate of 19 is a 30.24 Hz wave", HzOf(19), 30.2394);
    Check(HzOf(-19) == HzOf(19), "the sign of the rate does not change the wave's rate");
    {
        // the sine gets float(float(rate * T) * 10), and the pulse is (s + 1) * 0.5 of what it returns
        float got = -1.0f;
        const float t = 12.3456f;
        const float p = PulseOf(5, t, [&](float x) {
            got = x;
            return 0.25f;
        });
        const float a = 5.0f * t;
        Check(got == a * 10.0f, "the sine's argument is (rate * T) * 10, rounded to float after each step", got, a * 10.0f);
        Check(p == 0.625f, "the pulse is (sine + 1) * 0.5 of the sine the guest gives", p, 0.625);
        float got_held = -1.0f;
        HeldPulseOf(5, t, [&](float x) {
            got_held = x;
            return 0.0f;
        });
        const float held = HeldClock(t);
        Check(got_held == (5.0f * held) * 10.0f, "the held pulse feeds the sine the held clock", got_held, (5.0f * held) * 10.0f);
    }

    // ---- the held clock ----
    Check(HeldClock(0.0f) == 0.0f, "the held clock at 0 is 0");
    for (long k = 0; k <= 200000; k += 7) {
        const float exact = static_cast<float>(static_cast<double>(k) / kConsoleHz);
        if (HeldClock(exact) != exact) {
            Check(false, "a clock on a step boundary is held on that step", static_cast<double>(k), 0.0);
            break;
        }
    }
    {
        bool ok = true;
        for (long k = 0; k < 20000; ++k) {
            const double step = static_cast<double>(k) / kConsoleHz;
            const float early = static_cast<float>(step + 0.80 / kConsoleHz);   // 0.8 into the step: still the step
            const float late = static_cast<float>(step + 0.95 / kConsoleHz);    // 0.95 into the step: the guard has put it in the next
            ok = ok && HeldClock(early) == static_cast<float>(step) && HeldClock(late) == static_cast<float>(static_cast<double>(k + 1) / kConsoleHz);
        }
        Check(ok, "inside a step the held clock is the step's start (the guard rolls over in its last eighth)");
    }
    {
        bool ok = true;
        float prev = HeldClock(0.0f);
        for (int i = 1; i < 2000000; ++i) {
            const float t = 0.0005f * static_cast<float>(i);
            const float h = HeldClock(t);
            ok = ok && h >= prev && h <= t + 0.2 / kConsoleHz && h > t - 1.2 / kConsoleHz;
            prev = h;
        }
        Check(ok, "the held clock never goes back and stays within a step of the clock");
    }
    {
        const float nan = std::nanf("");
        const float inf = INFINITY;
        Check(std::isnan(HeldClock(nan)), "a NaN clock is left alone");
        Check(HeldClock(inf) == inf && HeldClock(-inf) == -inf, "an infinite clock is left alone");
        // ten hours of play: the float clock steps by 4 ms, and the held clock still only moves forward in 30 Hz steps
        bool ok = true;
        float prev = HeldClock(36000.0f);
        for (int i = 1; i < 100000; ++i) {
            const float t = 36000.0f + 0.0007f * static_cast<float>(i);
            const float h = HeldClock(t);
            ok = ok && h >= prev && std::fabs(static_cast<double>(h) - static_cast<double>(t)) < 2.0 / kConsoleHz;
            prev = h;
        }
        Check(ok, "ten hours into a session the held clock still only moves forward, within a step");
    }

    // ---- a clock that advances in whole steps does not jitter ----
    {
        for (double fps : {30.0, 60.0, 120.0}) {
            // the engine's exact sum rounded to float (timer_precision), 20000 steps: 11 minutes at 30
            Clock clock{fps, false};
            long prev = -1;
            long repeats_or_back = 0;
            long steps = 0;
            const long updates = static_cast<long>(fps * 600.0);
            for (long i = 0; i < updates; ++i) {
                const long cell = CellOf(HeldClock(clock.Next()));
                if (cell < prev) ++repeats_or_back;                       // a step back
                if (fps == 30.0 && i > 0 && cell != prev + 1) ++repeats_or_back;  // a step repeated or skipped at 30 FPS
                if (cell != prev) ++steps;
                prev = cell;
            }
            Check(repeats_or_back == 0, "a clock that advances in whole steps never jitters or goes back", fps, static_cast<double>(repeats_or_back));
            Check(std::labs(steps - static_cast<long>(600.0 * kConsoleHz)) <= 1, "and takes 30 steps a second", fps, static_cast<double>(steps));
        }
        // a clock that is a plain float sum (the engine's own float sum before timer_precision) is itself a hair
        // fast or slow (a float step of 1/30 s is rounded to the clock's grid: 0.02% at 3 minutes), so once in a few
        // thousand updates it repeats or skips a step: that is the clock's error, not the hold's. The hold must still
        // never go back and stay within 1% of the updates being irregular; a plain floor is printed next to it.
        float t = 0.0f;
        const float dt = static_cast<float>(1.0 / 30.0);
        long prev = -1, back = 0, irregular = 0;
        for (int i = 0; i < 5400; ++i) {
            t += dt;
            const long cell = CellOf(HeldClock(t));
            if (cell < prev) ++back;
            if (i > 0 && cell != prev + 1) ++irregular;
            prev = cell;
        }
        Check(back == 0 && irregular <= 54, "a plain float sum of 1/30 s steps never goes back, and repeats or skips under 1% of the steps", static_cast<double>(back), static_cast<double>(irregular));
        t = 0.0f;
        prev = -1;
        long plain_irregular = 0;
        for (int i = 0; i < 5400; ++i) {
            t += dt;
            const long cell = std::lround(std::floor(static_cast<double>(t) * kConsoleHz));
            if (i > 0 && cell != prev + 1) ++plain_irregular;
            prev = cell;
        }
        std::printf("info: a plain float sum of 1/30 s steps, 5400 updates, steps repeated or skipped: held %ld, plain floor %ld\n", irregular, plain_irregular);
    }

    // ---- pause and time scale act on the held clock as on the clock ----
    {
        const float paused = 123.4567f;
        Check(HeldClock(paused) == HeldClock(paused), "a paused clock holds the same value");
        Check(PulseOf(7, HeldClock(paused)) == PulseOf(7, HeldClock(paused)), "and the same pulse");
        for (double scale : {0.25, 0.5, 1.0, 2.0}) {
            // a clock that runs at `scale` times the wall clock, drawn at 144 FPS for 20 s: 30 * scale steps per wall second
            double clock = 50.0;
            long prev = -1, steps = 0;
            const long frames = static_cast<long>(144.0 * 20.0);
            for (long i = 0; i < frames; ++i) {
                clock += scale / 144.0;
                const long cell = CellOf(HeldClock(static_cast<float>(clock)));
                if (cell != prev) ++steps;
                prev = cell;
            }
            Check(std::fabs(static_cast<double>(steps) / 20.0 - 30.0 * scale) < 0.2, "the 30 Hz steps follow the time scale", static_cast<double>(steps) / 20.0, 30.0 * scale);
        }
    }

    // ---- the point: every frame rate shows the console's sequence ----
    {
        struct Rate {
            double fps;
            bool jitter;
            const char* name;
        };
        const Rate rates[] = {{30.0, false, "30"},  {60.0, false, "60"},   {120.0, false, "120"},
                              {144.0, false, "144"}, {240.0, false, "240"}, {0.0, true, "uncapped (4 to 8 ms)"}};
        for (int rate : {1, 3, 5, 19, -19, 100}) {
            std::vector<float> reference;
            long ref_last = 0;
            const bool ref_ok = HeldSequence(30.0, false, static_cast<int8_t>(rate), 60.0, reference, ref_last);
            Check(ref_ok && ref_last >= 1790, "the 30 FPS sequence takes every 30 Hz step in order", static_cast<double>(rate), static_cast<double>(ref_last));
            for (const Rate& r : rates) {
                std::vector<float> seq;
                long last = 0;
                const bool ok = HeldSequence(r.fps, r.jitter, static_cast<int8_t>(rate), 60.0, seq, last);
                Check(ok, "every frame rate takes every 30 Hz step in order", r.fps, static_cast<double>(rate));
                const size_t n = seq.size() < reference.size() ? seq.size() : reference.size();
                bool same = n >= 1790;
                for (size_t i = 1; i < n && same; ++i) same = seq[i] == reference[i];  // step 0 only exists at 60 FPS and up
                Check(same, "and the pulse takes the same values as at 30 FPS, in the same order", r.fps, static_cast<double>(rate));
                if (!same) std::printf("      (rate %d, %s FPS)\n", rate, r.name);
            }
        }
    }

    // ---- what a screen recording sees (neon_measure.py's measure): flash rate 19, the garage test car ----
    {
        const double ref = VisibleCyclesPerSecond(30.0, false, 19, true, 40.0);
        std::printf("info: visible cycles a second at flash rate 19, 40 s: held 30 FPS %.3f", ref);
        Check(ref > 0.15 && ref < 0.45, "at 30 FPS the held pulse is a slow throb (about 0.3 visible cycles a second)", ref, 0.3);
        for (double fps : {60.0, 120.0, 144.0, 240.0}) {
            const double c = VisibleCyclesPerSecond(fps, false, 19, true, 40.0);
            std::printf(", %g FPS %.3f", fps, c);
            Check(std::fabs(c - ref) <= 0.1, "at any frame rate the held pulse shows the same slow throb", fps, c);
        }
        const double cu = VisibleCyclesPerSecond(0.0, true, 19, true, 40.0);
        std::printf(", uncapped %.3f\n", cu);
        Check(std::fabs(cu - ref) <= 0.1, "also at an irregular uncapped rate", cu, ref);

        // without the hold: the bug
        const double raw30 = VisibleCyclesPerSecond(30.0, false, 19, false, 40.0);
        const double raw60 = VisibleCyclesPerSecond(60.0, false, 19, false, 40.0);
        const double raw240 = VisibleCyclesPerSecond(240.0, false, 19, false, 40.0);
        std::printf("info: without the hold: 30 FPS %.3f, 60 FPS %.3f, 240 FPS %.3f visible cycles a second\n", raw30, raw60, raw240);
        Check(raw30 > 0.15 && raw30 < 0.45, "without the hold 30 FPS is the console's throb too", raw30, 0.3);
        Check(raw60 > 3.0, "without the hold 60 FPS strobes (the measure does see it)", raw60, 3.0);
        Check(raw240 > 3.0, "without the hold 240 FPS strobes", raw240, 3.0);
    }

    // a slow flash rate is still a smooth pulse, only held on the console's steps: 8 Hz (rate 5) keeps its shape
    {
        const double fast = VisibleCyclesPerSecond(240.0, false, 5, true, 20.0);
        const double console = VisibleCyclesPerSecond(30.0, false, 5, true, 20.0);
        std::printf("info: rate 5 (8.0 Hz wave): held 30 FPS %.2f, held 240 FPS %.2f visible cycles a second\n", console, fast);
        Check(std::fabs(fast - console) <= 0.5, "a slow flash rate shows the same pulse at 240 FPS as at 30", fast, console);
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

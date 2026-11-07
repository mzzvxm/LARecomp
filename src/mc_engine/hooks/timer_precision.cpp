// Clocks the engine keeps in a float or in whole milliseconds, kept true over a long
// session and at a high frame rate.
//
// The engine timer (sub_821BDA90, the object at 0x827D7500) adds the frame time to two
// float clocks every frame: [+0x14] (all frames) and [+0x18] (frames the game is
// running). A float holds about seven digits, so the sum is rounded to its own step on
// every frame: 0.24 ms near one hour, 3.9 ms near ten hours. A frame shorter than that
// step is added as a whole step or not at all, so the clock runs a few percent fast or
// slow, and past about 36 hours at 240 FPS (72 hours at 60) it stops. The readers use
// them as "now" for animation phases, effect and cooldown stamps and UI timeouts.
//
// The same shape is the audio engine's clock (sub_821E63F0, the float at 0x8286CF34):
// it adds [+0x60] of the timer every frame and hands the audio engine the result in
// milliseconds.
//
// Each of these keeps a double running sum next to the float and stores the sum rounded
// to a float. At normal session lengths nothing the readers see changes (the old clock
// is within one step of a float of the sum for the first hour or so), but the error stops
// growing: the clock keeps time, within one step of a float, for as long as the game runs.
//
// Separately, the clock the race timer, the time trial and capture-the-flag run on
// (sub_82256220, the whole milliseconds at [+0xEA4] of the object returned by
// sub_82256058) adds (int)(dt * 1000) each frame, which drops the fraction of every
// frame: 16 ms for a 16.67 ms frame at 60 FPS (4% slow), 6 ms for 6.94 ms at 144 (14%),
// 2 ms for 2.78 ms at 360 (28%). The fraction is carried to the next frame instead, so
// the clock counts real time at any frame rate.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <cmath>
#include <cstdint>

#include "hooks.h"
#include "hooks_internal.h"

namespace {

// A clock the guest advances as `clock = (float)(clock + inc)` once a frame. Remembers the
// exact sum next to the float and, after the guest's own add, puts back the sum rounded
// to a float.
struct FloatClock {
    double sum = 0.0;         // the exact running total
    float published = 0.0f;   // what this left in the guest's clock last time
    bool valid = false;       // `sum` is the total of the guest's clock

    // `guest` is what the guest's add just stored, `inc` what it added. Returns the value
    // to store instead.
    float Advance(float guest, float inc) {
        if (!std::isfinite(guest) || !std::isfinite(inc)) {
            valid = false;
            return guest;
        }
        // The guest read back `published` and added `inc`, so it stored exactly this. If it
        // did not, something else wrote the clock (the engine's reset at start-up, a load):
        // start again from what is there.
        if (valid && guest != published + inc) valid = false;
        if (valid) {
            sum += static_cast<double>(inc);
        } else {
            sum = static_cast<double>(guest);
            valid = true;
        }
        published = static_cast<float>(sum);
        return published;
    }
};

FloatClock g_total_time;   // timer [+0x14]
FloatClock g_game_time;    // timer [+0x18]
FloatClock g_audio_time;   // the audio engine's clock

void AdvanceTimerField(PPCRegister& r3, PPCRegister& f0, FloatClock& clock, uint32_t field) {
    uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t addr = static_cast<uint32_t>(r3.u64) + field;
    const float guest = ReadGuestF32(base, addr);
    const float out = clock.Advance(guest, static_cast<float>(f0.f64));
    if (out != guest) WriteGuestF32(base, addr, out);
}

}  // namespace

// After each `stfs fN,20(r3)` / `stfs fN,24(r3)` in sub_821BDA90 (four of each, one per
// path through the update): f0 is the frame time the path just added.
void MCLATimeAccumTotal(PPCRegister& r3, PPCRegister& f0) {
    AdvanceTimerField(r3, f0, g_total_time, 0x14);
}

void MCLATimeAccumGame(PPCRegister& r3, PPCRegister& f0) {
    AdvanceTimerField(r3, f0, g_game_time, 0x18);
}

// After `stfs f0,-12492(r9)` in sub_821E63F0: r9 is the high half of the clock's address,
// f0 the new sum (which the next instructions turn into milliseconds, so it is replaced
// too), r29 the engine timer, whose [+0x60] was just added.
void MCLAAudioClockAccum(PPCRegister& r9, PPCRegister& r29, PPCRegister& f0) {
    uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t clock_addr = static_cast<uint32_t>(r9.u64) + static_cast<uint32_t>(-12492);
    const float inc = ReadGuestF32(base, static_cast<uint32_t>(r29.u64) + 0x60);
    const float guest = static_cast<float>(f0.f64);
    const float out = g_audio_time.Advance(guest, inc);
    if (out != guest) {
        WriteGuestF32(base, clock_addr, out);
        f0.f64 = static_cast<double>(out);
    }
}

// Before `fctiwz f13,f0` in sub_82256220: f0 is the frame time in milliseconds, which the
// conversion would cut to a whole number. Hands it the whole part of this frame's time and
// what was left over by the frames before, and keeps the rest for the next one. Only
// reached on frames the clock runs.
void MCLAGameClockCarry(PPCRegister& f0) {
    static double carry = 0.0;
    const double ms = f0.f64 + carry;
    if (!std::isfinite(ms) || std::fabs(ms) > 1.0e9) {
        carry = 0.0;
        return;
    }
    const double whole = std::trunc(ms);
    carry = ms - whole;
    f0.f64 = whole;
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void MCLATimeAccumTotal(PPCRegister& r3, PPCRegister& f0) {}
void MCLATimeAccumGame(PPCRegister& r3, PPCRegister& f0) {}
void MCLAAudioClockAccum(PPCRegister& r9, PPCRegister& r29, PPCRegister& f0) {}
void MCLAGameClockCarry(PPCRegister& f0) {}
#endif  // REXGLUE_HAS_XEO3_TARGET

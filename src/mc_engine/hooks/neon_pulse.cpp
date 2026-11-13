// The neon underglow's pulse, held on the console's 30 Hz updates.
//
// What the game does (feature-parity/NEON-PULSE.md): a car's customisation holds ExtNeonFlashRate (a signed
// byte at config + 396) and IntNeonFlashRate (+ 397). The car effect object (this; the config is at
// this[20] + 132) refreshes the pulse once an update in sub_82353E38: this[404] = (sin(10 * rate * T) + 1)
// * 0.5, this[408] the same with the interior rate, T being the engine timer's game clock, the float at
// 0x827D7518. The underglow lights, the car's shaders and the driver's neon light all read those two
// floats and nothing else of the pulse.
//
// What was wrong: the rate is in radians a second, and a high one (19 in the garage test car) is a wave
// of about 30 Hz. On the console, updating 30 times a second, each update sampled that wave at almost
// the same phase, so the neon throbbed slowly (a cycle every 3 to 4 s; measured at a 30 FPS cap: 0.3
// visible cycles a second). At 60 FPS the same wave shows as about 11 blinks a second and uncapped as a
// strobe (NEON.txt: 11.2 and 12.4 on/off cycles a second at 60 FPS and uncapped, 0.30 at 30 FPS).
//
// The fix: the pulse is worked out again from the game clock held on the console's 30 Hz steps
// (neon_pulse.h, HeldClock), so every frame rate shows the console's wave. It is the game's own clock, so
// a pause and the time scale act on it as before, and the arithmetic is the game's own too: the sine is
// the guest's sub_823DBBC8, called with the same float argument. Only the two stored pulses change.
// Everything else the update does, and everything that reads the pulse (the intensities, the colours, the
// lights), is the game's. A car whose flash rates are both 0 (a steady half) is left alone, so a car
// without a neon flash costs three loads and a compare.
//
// This is not a clock fix: T keeps real time at any frame rate (frame_timing, timer_precision). It is a
// sampling fix, the neon's own: the same idea as the checkpoint sweep, which gives a script above 30 FPS
// what it would have seen at 30.
//
// mcla_fix_neon_30hz turns it off (the FIXES page of the debug menu), for a tester who wants the pulse
// at the display's rate.
//
// CHECK: the addresses are read from the generated code (native\tree-k1\generated, sub_82353E38 and
// sub_823DBBC8), not seen in a running game; the feature-parity probe measured the game's pulse at up to
// 0.7 from this formula at the same clock (the probe read the clock after the update, which may not be
// the clock the update read); this file calls the guest's own sine so that does not matter for the
// arithmetic, and the check chain (perf\scripts\chain-neonfix.ps1) reads the visible result.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <atomic>
#include <cstdint>

#include "hooks_internal.h"
#include "larecomp_log.h"
#include "neon_pulse.h"

REXCVAR_DEFINE_BOOL(mcla_fix_neon_30hz, true, "MCLA/Patches",
    "Fix: the neon underglow's pulse follows the console's 30 Hz clock, not every frame. A fast flash "
    "rate is a wave of about 30 Hz; the console sampled it 30 times a second and showed a slow throb, "
    "and at 60 FPS or more the same wave blinks about 11 times a second (a strobe). On: every frame "
    "rate shows the console's throb. Off: the pulse is worked out at every frame.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_82353E38);  // the car effect object's update (the pulse is its last step)
REX_EXTERN(rex_sub_823DBBC8);         // the game's sine: f1 in (a double), f1 out

namespace {

constexpr uint32_t kThis_Record = 20;     // this + 20: the vehicle record
constexpr uint32_t kRecord_Config = 132;  // record + 132: the car's config (mcCarConfig)
constexpr uint32_t kCfg_ExtNeonRate = 396;
constexpr uint32_t kCfg_IntNeonRate = 397;
constexpr uint32_t kThis_ExtPulse = 404;
constexpr uint32_t kThis_IntPulse = 408;

std::atomic<bool> g_logged{false};

// The guest's sine of a float, rounded to a float as the game rounds it (frsp). It runs in a context of
// its own, so nothing of the caller's registers is touched.
float GuestSine(PPCContext& ctx, uint8_t* base, float x) {
    rex::CallFrame frame(ctx);
    PPCContext& call = frame;
    call.f1.f64 = static_cast<double>(x);
    rex_sub_823DBBC8(call, base);
    return static_cast<float>(call.f1.f64);
}

}  // namespace

// Runs the game's update, then works the two pulses out again at the held clock.
extern "C" REX_FUNC(rex_sub_82353E38) {
    const uint32_t self = ctx.r3.u32;
    __imp__rex_sub_82353E38(ctx, base);

    if (self < 0x10000u) return;
    const uint32_t record = ReadGuestU32(base, self + kThis_Record);
    if (record < 0x10000u) return;
    const uint32_t cfg = ReadGuestU32(base, record + kRecord_Config);
    if (cfg < 0x10000u) return;

    const int8_t rate_ext = static_cast<int8_t>(base[cfg + kCfg_ExtNeonRate]);
    const int8_t rate_int = static_cast<int8_t>(base[cfg + kCfg_IntNeonRate]);
    // A steady neon (rate 0) is 0.5 at any clock: nothing to hold.
    if (rate_ext == 0 && rate_int == 0) return;

    const bool fix_on = REXCVAR_GET(mcla_fix_neon_30hz);
    if (!g_logged.exchange(true, std::memory_order_relaxed)) {
        LARECOMP_APP_INFO("[NeonPulse] first car with a neon flash: flash rate {} exterior, {} interior (a wave of "
                          "{:.1f} Hz and {:.1f} Hz); the pulse is held on the console's 30 Hz clock: {}",
                          static_cast<int>(rate_ext), static_cast<int>(rate_int), mcla::neon::HzOf(rate_ext),
                          mcla::neon::HzOf(rate_int), fix_on ? "yes" : "no (mcla_fix_neon_30hz is off)");
    }
    if (!fix_on) return;

    // The clock the update just read (the same float, 0x827D7518). CHECK: that nothing advances it between
    // the update's read and this one; both run on the thread that updates the car, right after each other.
    const float clock = ReadGuestF32(base, kGuestAccumB);
    auto sine = [&](float x) { return GuestSine(ctx, base, x); };
    if (rate_ext != 0) WriteGuestF32(base, self + kThis_ExtPulse, mcla::neon::HeldPulseOf(rate_ext, clock, sine));
    if (rate_int != 0) WriteGuestF32(base, self + kThis_IntPulse, mcla::neon::HeldPulseOf(rate_int, clock, sine));
}
#endif  // REXGLUE_HAS_XEO3_TARGET
